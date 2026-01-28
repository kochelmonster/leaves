#ifndef _LEAVES_MERGER_HPP
#define _LEAVES_MERGER_HPP

#include "../core/_bits.hpp"
#include "../core/_node.hpp"

namespace leaves {

/**
 * @brief Recursive COW merger
 *
 * Recursively merges a source tree into a destination tree with copy-on-write
 * semantics. Only modified paths are copied; unchanged subtrees are shared.
 */
template <typename DBDst, typename DBSrc, typename MergePolicy>
struct _Merger {
  // Use CursorTraits from DB which has node type definitions
  using DstTraits = typename DBDst::CursorTraits;
  using SrcTraits = typename DBSrc::CursorTraits;
  using PageHeader = typename DstTraits::PageHeader;
  using TrieNode = _TrieNode<DstTraits>;
  using LeafNode = _LeafNode<DstTraits>;
  using SrcTrieNode = _TrieNode<SrcTraits>;
  using SrcLeafNode = _LeafNode<SrcTraits>;
  using page_ptr = typename DstTraits::ptr;
  using trie_ptr = typename DstTraits::template Pointer<TrieNode>;
  using leaf_ptr = typename DstTraits::template Pointer<LeafNode, LEAF>;
  using offset_e = typename DstTraits::offset_e;
  using src_offset_e = typename SrcTraits::offset_e;
  using src_trie_ptr = typename SrcTraits::template Pointer<SrcTrieNode>;
  using src_leaf_ptr = typename SrcTraits::template Pointer<SrcLeafNode, LEAF>;

  DBDst& db_dst;
  offset_e* dst_root;
  DBSrc& db_src;
  src_offset_e* src_root;
  MergePolicy& handler;
  std::string key_buf;  // Reusable buffer for building keys (avoids heap allocs)
  offset_e children_buf[257];  // Reusable buffer for collecting children (avoids stack allocs)

  /**
   * @param db_dest  Destination database for allocating and resolving nodes
   * @param dest     Pointer to the root offset of the destination tree
   * @param db_src   Source database for resolving source nodes
   * @param src      Pointer to the root offset of the source tree
   * @param h        Merge policy handler
   */
  _Merger(DBDst& db_dest, offset_e* dest, DBSrc& db_src_, src_offset_e* src,
          MergePolicy& h)
      : db_dst(db_dest),
        dst_root(dest),
        db_src(db_src_),
        src_root(src),
        handler(h) {}

  /**
   * Merge the entire source tree into the destination tree.
   * Returns new root offset if anything was merged, or null offset if nothing
   * changed.
   */
  offset_e exec() {
    if (!*src_root) {
      return offset_e();  // Nothing to merge
    }

    // Pre-allocate key buffer to avoid repeated heap allocations
    key_buf.clear();
    key_buf.reserve(256);

    // Recursively merge source into destination
    offset_e new_root = merge_trees(*dst_root, *src_root);
    *dst_root = new_root;
    return new_root;
  }

 private:
  // =========================================================================
  // Node allocation helpers (following _Inserter patterns)
  // =========================================================================

  page_ptr alloc(uint16_t size) { return db_dst.alloc(size); }

  // Allocate node with PageHeader prefix, return pointer to node
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    page_ptr page = alloc(node_size);
    return page + sizeof(PageHeader);
  }

  // Free node by computing PageHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    static_assert(
        !std::is_same_v<NodePtr, page_ptr>,
        "free_node must be called with node pointers, not page pointers");
    page_ptr page = node - sizeof(PageHeader);
    db_dst.free(page);
  }

  template <typename T>
  offset_e resolve(T ptr) {
    return db_dst.resolve(ptr);
  }

  template <typename T, NodeTypes nt = TRIE>
  typename DstTraits::template Pointer<T, nt> resolve_dst(const offset_e* off) {
    return db_dst.template resolve<T>(off);
  }

  template <typename T, NodeTypes nt = TRIE>
  typename SrcTraits::template Pointer<T, nt> resolve_src(const src_offset_e* off) {
    return db_src.template resolve<T>(off);
  }

  // =========================================================================
  // Main merge dispatch
  // =========================================================================

  /**
   * Recursively merge source subtree into destination subtree.
   * @param dst_offset  Destination node offset (may be empty)
   * @param src_offset  Source node offset (must be valid)
   * @return New offset for merged subtree
   * @note Uses key_buf member for accumulated key prefix
   */
  offset_e merge_trees(offset_e dst_offset, src_offset_e src_offset) {
    // Empty destination - copy entire source subtree
    if (!dst_offset) {
      return copy_subtree(src_offset);
    }

    // Dispatch based on source node type
    if (src_offset.type() == LEAF) {
      return merge_src_leaf(dst_offset, src_offset);
    } else {
      return merge_src_trie(dst_offset, src_offset);
    }
  }

  // =========================================================================
  // Source is LEAF
  // =========================================================================

  offset_e merge_src_leaf(offset_e dst_offset, src_offset_e src_offset) {
    auto src_leaf = resolve_src<SrcLeafNode, LEAF>(&src_offset);
    Slice src_key = src_leaf->key();
    Slice src_value = get_src_value(src_leaf);

    if (dst_offset.type() == LEAF) {
      return merge_leaf_into_leaf(dst_offset, src_key, src_value);
    } else {
      return merge_leaf_into_trie(dst_offset, src_key, src_value);
    }
  }

  /**
   * Merge source leaf into destination leaf.
   *
   * Case 1: Keys match exactly (overwrite check)
   *   BEFORE:                      AFTER (if overwrite):
   *     DST: [leaf:"abc"=v1]         [leaf:"abc"=v2]
   *     SRC: [leaf:"abc"=v2]
   *
   * Case 2: Keys diverge at position N
   *   BEFORE:                      AFTER:
   *     DST: [leaf:"abc"=v1]         [trie prefix="a"]
   *     SRC: [leaf:"axyz"=v2]              /     \
   *                                     'b'     'x'
   *                                      |       |
   *                               [leaf:"c"=v1] [leaf:"yz"=v2]
   */
  offset_e merge_leaf_into_leaf(offset_e dst_offset, const Slice& src_key,
                                const Slice& src_value) {
    auto dst_leaf = resolve_dst<LeafNode, LEAF>(&dst_offset);
    Slice dst_key = dst_leaf->key();

    size_t common = common_prefix_len(dst_key, src_key);

    // Keys match completely - check overwrite policy
    if (common == dst_key.size() && common == src_key.size()) {
      size_t saved = key_buf.size();
      key_buf.append(src_key.data(), src_key.size());
      Slice dst_value = dst_leaf->value();
      bool do_overwrite = handler.check_overwrite(key_buf, dst_value, src_value);
      key_buf.resize(saved);
      if (do_overwrite) {
        offset_e new_offset = create_leaf(src_key, src_value);
        free_leaf(dst_leaf);
        return new_offset;
      }
      return dst_offset;  // Keep destination unchanged
    }

    // Keys diverge at 'common' - create a trie to split them
    Slice prefix(dst_key.data(), common);
    int dst_branch =
        (common < dst_key.size()) ? (uint8_t)dst_key[common] : TrieNode::NONE;
    int src_branch =
        (common < src_key.size()) ? (uint8_t)src_key[common] : TrieNode::NONE;

    // Create new trie with two branches
    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(common, dst_branch, src_branch));
    auto idxs = new_trie->create(prefix, dst_branch, src_branch);

    // Create reduced leaves for both branches
    Slice dst_suffix(dst_key.data() + common, dst_key.size() - common);
    Slice src_suffix(src_key.data() + common, src_key.size() - common);

    leaf_ptr new_dst_leaf = create_leaf_node(dst_suffix, dst_leaf->value());
    leaf_ptr new_src_leaf = create_leaf_node(src_suffix, src_value);

    new_trie->array()[idxs.first] = resolve(new_dst_leaf);
    new_trie->array()[idxs.second] = resolve(new_src_leaf);

    free_leaf(dst_leaf);
    return resolve(new_trie);
  }

  /**
   * Merge source leaf into destination trie.
   *
   * Case 1: Source key matches trie prefix, branch exists (recurse)
   *   BEFORE:                      AFTER:
   *     DST: [trie prefix="ab"]       [trie prefix="ab"]  (COW copy)
   *              |  'c'                    |  'c'
   *           [child]                   [merged_child]
   *     SRC: [leaf:"abcdef"=v]
   *
   * Case 2: Source key matches trie prefix, branch doesn't exist (add)
   *   BEFORE:                      AFTER:
   *     DST: [trie prefix="ab"]       [trie prefix="ab"]
   *              |  'c'                  /  'c'   \  'x'
   *           [child]              [child]    [leaf:"yz"=v]
   *     SRC: [leaf:"abxyz"=v]
   *
   * Case 3: Source key diverges within trie prefix (split)
   *   BEFORE:                      AFTER:
   *     DST: [trie prefix="abcd"]     [trie prefix="ab"]
   *              |  'x'                  /  'c'   \  'x'
   *           [child]         [trie prefix="d"]  [leaf:"yz"=v]
   *     SRC: [leaf:"abxyz"=v]        |  'x'
   *                               [child]
   */
  offset_e merge_leaf_into_trie(offset_e dst_offset, const Slice& src_key,
                                const Slice& src_value) {
    auto dst_trie = resolve_dst<TrieNode>(&dst_offset);
    Slice dst_prefix((const char*)dst_trie->compressed(), dst_trie->len());

    size_t common = common_prefix_len(dst_prefix, src_key);

    if (common < dst_prefix.size()) {
      // Source key diverges within trie's prefix - need to split trie
      return split_trie_for_leaf(dst_trie, dst_offset, common, src_key,
                                 src_value);
    }

    // common == dst_prefix.size(): source key matches trie prefix
    Slice remaining(src_key.data() + common, src_key.size() - common);
    int branch = remaining.empty() ? TrieNode::NONE : (uint8_t)remaining[0];

    // Key for the child (skip the branch byte if present)
    Slice child_key(remaining.data() + (remaining.empty() ? 0 : 1),
                    remaining.empty() ? 0 : remaining.size() - 1);

    // Save key_buf, append prefix + branch for child calls
    size_t saved = key_buf.size();
    key_buf.append(dst_prefix.data(), dst_prefix.size());
    if (!remaining.empty()) {
      key_buf += remaining[0];
    }

    if (dst_trie->isset(branch)) {
      // Branch exists - recurse into child with leaf key/value
      offset_e* child_offset_ptr = dst_trie->offset(branch);
      offset_e new_child =
          merge_leaf_into_subtree(*child_offset_ptr, child_key, src_value);

      if (new_child == *child_offset_ptr) {
        return dst_offset;  // No change
      }

      // COW: create new trie with updated child
      return copy_trie_with_updated_branch(dst_trie, branch, new_child);
    } else {
      // Branch doesn't exist - add new branch with leaf
      leaf_ptr new_leaf = create_leaf_node(child_key, src_value);

      trie_ptr new_trie =
          alloc_node<trie_ptr>(dst_trie->increment_size(branch));
      uint16_t idx = new_trie->create(*dst_trie, branch);
      new_trie->array()[idx] = resolve(new_leaf);

      free_trie(dst_trie);
      return resolve(new_trie);
    }
  }

  /**
   * Helper: merge a leaf (by key+value) into an existing subtree.
   * Precondition: dst_offset is valid (caller checked isset())
   */
  offset_e merge_leaf_into_subtree(offset_e dst_offset, const Slice& key,
                                   const Slice& value) {
    assert(dst_offset && "caller must check isset() before calling");

    if (dst_offset.type() == LEAF) {
      return merge_leaf_into_leaf(dst_offset, key, value);
    } else {
      return merge_leaf_into_trie(dst_offset, key, value);
    }
  }

  /**
   * Helper: merge a src trie (with prefix skipped) into dst subtree.
   * Used when dst prefix is a prefix of src prefix.
   */
  offset_e merge_subtree_into_dst(offset_e dst_offset,
                                   src_trie_ptr src_trie,
                                   src_offset_e src_offset,
                                   size_t skip_prefix) {
    Slice old_prefix((const char*)src_trie->compressed(), src_trie->len());
    Slice remaining(old_prefix.data() + skip_prefix, old_prefix.size() - skip_prefix);

    if (remaining.empty()) {
      // No remaining prefix - merge src children directly
      if (dst_offset.type() == LEAF) {
        // Merge src trie (with empty prefix) into dst leaf
        return merge_trie_into_leaf(dst_offset, src_trie, src_offset,
                                    Slice());
      } else {
        // Merge children
        auto dst_trie = resolve_dst<TrieNode>(&dst_offset);
        return merge_trie_children(dst_trie, dst_offset, src_trie, src_offset);
      }
    }

    // There's still remaining prefix - recurse with reduced src prefix
    if (dst_offset.type() == LEAF) {
      return merge_trie_into_leaf(dst_offset, src_trie, src_offset,
                                  remaining);
    } else {
      return merge_trie_into_trie(dst_offset, src_trie, src_offset,
                                  remaining);
    }
  }

  // =========================================================================
  // Source is TRIE
  // =========================================================================

  offset_e merge_src_trie(offset_e dst_offset, src_offset_e src_offset) {
    auto src_trie = resolve_src<SrcTrieNode>(&src_offset);
    Slice src_prefix((const char*)src_trie->compressed(), src_trie->len());

    if (dst_offset.type() == LEAF) {
      return merge_trie_into_leaf(dst_offset, src_trie, src_offset, src_prefix);
    } else {
      return merge_trie_into_trie(dst_offset, src_trie, src_offset, src_prefix);
    }
  }

  /**
   * Merge source trie into destination leaf.
   *
   * Case 1: Prefixes diverge at position N
   *   BEFORE:                        AFTER:
   *     DST: [leaf:"abcX"=v1]          [trie prefix="abc"]
   *     SRC: [trie prefix="abcY"]           /  'X'   \  'Y'
   *               |  ...           [leaf:""=v1]  [trie prefix=""]
   *            [children]                             |  ...
   *                                               [children]
   *
   * Case 2: Dst leaf key is prefix of src trie prefix (TODO)
   *   BEFORE:                        AFTER:
   *     DST: [leaf:"ab"=v1]            Need to merge leaf value into
   *     SRC: [trie prefix="abcd"]      trie's NONE branch
   *
   * Case 3: Src trie prefix is prefix of dst leaf key (TODO)
   *   BEFORE:                        AFTER:
   *     DST: [leaf:"abcd"=v1]          Need to insert leaf into
   *     SRC: [trie prefix="ab"]        appropriate branch of trie
   */
  offset_e merge_trie_into_leaf(offset_e dst_offset,
                                src_trie_ptr src_trie,
                                src_offset_e src_offset, const Slice& src_prefix) {
    auto dst_leaf = resolve_dst<LeafNode, LEAF>(&dst_offset);
    Slice dst_key = dst_leaf->key();

    size_t common = common_prefix_len(dst_key, src_prefix);

    if (common < src_prefix.size() && common < dst_key.size()) {
      // Case 1: Prefixes diverge - create split trie
      Slice prefix(dst_key.data(), common);
      int dst_branch = (uint8_t)dst_key[common];
      int src_branch = (uint8_t)src_prefix[common];

      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(common, dst_branch, src_branch));
      auto idxs = new_trie->create(prefix, dst_branch, src_branch);

      // Reduced dst leaf
      Slice dst_suffix(dst_key.data() + common, dst_key.size() - common);
      leaf_ptr new_dst_leaf = create_leaf_node(dst_suffix, dst_leaf->value());
      new_trie->array()[idxs.first] = resolve(new_dst_leaf);

      // Copy src subtrie with adjusted prefix
      size_t saved = key_buf.size();
      key_buf.append(prefix.data(), prefix.size());
      key_buf += (char)src_branch;
      offset_e src_child = copy_subtree_with_prefix_adjust(src_trie, src_offset,
                                                           common);
      key_buf.resize(saved);
      new_trie->array()[idxs.second] = src_child;

      free_leaf(dst_leaf);
      return resolve(new_trie);
    }

    if (common == dst_key.size() && common == src_prefix.size()) {
      // Case 2a: Dst leaf key equals src trie prefix exactly
      //   DST: [leaf:"abc"=v1]  →  [trie prefix="abc"]
      //   SRC: [trie prefix="abc"]    / NONE \ 'd' \ 'e'
      //           / 'd' \ 'e'    [leaf:""=v1] [copy] [copy]
      //        [s1]   [s2]
      //
      // Dst leaf becomes NONE branch, copy src children
      size_t saved = key_buf.size();
      key_buf.append(dst_key.data(), common);

      clear_children_buf();
      uint16_t branch_count = 0;

      // Add dst leaf as NONE branch
      if (src_trie->isset(TrieNode::NONE)) {
        // Src also has NONE - merge
        src_offset_e* src_child = src_trie->offset(TrieNode::NONE);
        offset_e dst_none_leaf = create_leaf(Slice(), dst_leaf->value());
        children_buf[0] = merge_trees(dst_none_leaf, *src_child);
      } else {
        leaf_ptr new_dst_leaf = create_leaf_node(Slice(), dst_leaf->value());
        children_buf[0] = resolve(new_dst_leaf);
      }
      if (children_buf[0]) branch_count++;

      // Copy remaining src children using bitmap iteration (skip NONE since handled above)
      int start = src_trie->has_none() ? src_trie->next(SrcTrieNode::NONE) : src_trie->first();
      for (int i = start; i != SrcTrieNode::OUT_OF_RANGE; i = src_trie->next(i)) {
        src_offset_e* child = src_trie->offset(i);
        key_buf += (char)i;
        children_buf[i + 1] = copy_subtree(*child);
        key_buf.resize(saved + common);  // Restore to just the prefix
        if (children_buf[i + 1]) branch_count++;
      }
      key_buf.resize(saved);

      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(common, branch_count));
      new_trie->create(Slice(dst_key.data(), common), children_buf + 1);

      free_leaf(dst_leaf);
      return resolve(new_trie);
    }

    if (common == dst_key.size()) {
      // Case 2b: Dst leaf key is prefix of src trie prefix (common < src_prefix.size())
      //   DST: [leaf:"ab"=v1]  →  [trie prefix="ab"]
      //   SRC: [trie prefix="abcd"]    / NONE \ 'c'
      //             |  ...        [leaf:""=v1] [trie prefix="d"]
      //          [children]                       |  ...
      //                                       [children]
      //
      // Create new trie with dst leaf as NONE branch, src trie (reduced) as 'c' branch
      int src_branch = (uint8_t)src_prefix[common];

      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(common, TrieNode::NONE, src_branch));
      auto idxs = new_trie->create(Slice(dst_key.data(), common),
                                   TrieNode::NONE, src_branch);

      // Dst leaf becomes empty-key leaf at NONE branch
      leaf_ptr new_dst_leaf = create_leaf_node(Slice(), dst_leaf->value());
      new_trie->array()[idxs.first] = resolve(new_dst_leaf);

      // Copy src subtrie with adjusted prefix
      size_t saved = key_buf.size();
      key_buf.append(dst_key.data(), common);
      key_buf += (char)src_branch;
      offset_e src_child = copy_subtree_with_prefix_adjust(src_trie, src_offset,
                                                           common);
      key_buf.resize(saved);
      new_trie->array()[idxs.second] = src_child;

      free_leaf(dst_leaf);
      return resolve(new_trie);
    }

    // Case 3: Src trie prefix is prefix of dst leaf key (common == src_prefix.size())
    //   DST: [leaf:"abcd"=v1]  →  Copy src trie, merge leaf into branch 'c'
    //   SRC: [trie prefix="ab"]
    //             |  ...
    //          [children]
    //
    // Copy src trie, then merge remaining dst leaf ("cd") into appropriate branch
    size_t saved = key_buf.size();
    key_buf.append(src_prefix.data(), src_prefix.size());
    Slice dst_remaining(dst_key.data() + common, dst_key.size() - common);
    int dst_branch = dst_remaining.empty() ? TrieNode::NONE : (uint8_t)dst_remaining[0];

    // Build new children array by copying src and merging dst leaf
    clear_children_buf();
    uint16_t branch_count = 0;

    for (int i = src_trie->first(); i != SrcTrieNode::OUT_OF_RANGE; i = src_trie->next(i)) {
      src_offset_e* child = src_trie->offset(i);
      size_t prefix_len = key_buf.size();
      if (i != TrieNode::NONE) key_buf += (char)i;

      if (i == dst_branch) {
        // Merge dst leaf into this branch
        Slice leaf_key = dst_remaining;
        offset_e dst_leaf_offset = create_leaf(leaf_key, dst_leaf->value());
        children_buf[i + 1] = merge_trees(dst_leaf_offset, *child);
      } else {
        children_buf[i + 1] = copy_subtree(*child);
      }
      key_buf.resize(prefix_len);
      if (children_buf[i + 1]) branch_count++;
    }

    // If dst_branch wasn't in src_trie, add the dst leaf as new branch
    if (!src_trie->isset(dst_branch)) {
      Slice leaf_key = dst_remaining;
      offset_e leaf_off = create_leaf(leaf_key, dst_leaf->value());
      children_buf[dst_branch + 1] = leaf_off;
      branch_count++;
    }
    key_buf.resize(saved);

    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(src_prefix.size(), branch_count));
    new_trie->create(src_prefix, children_buf + 1);

    free_leaf(dst_leaf);
    return resolve(new_trie);
  }

  /**
   * Merge source trie into destination trie.
   *
   * Case 1: Prefixes diverge at position N (neither is prefix of other)
   *   BEFORE:                        AFTER:
   *     DST: [trie prefix="abcX"]       [trie prefix="abc"]
   *               |  ...                   /  'X'   \  'Y'
   *            [dst_kids]      [trie prefix=""]  [trie prefix=""]
   *     SRC: [trie prefix="abcY"]       |  ...        |  ...
   *               |  ...            [dst_kids]   [src_kids]
   *            [src_kids]
   *
   * Case 2: Prefixes match exactly
   *   BEFORE:                        AFTER:
   *     DST: [trie prefix="abc"]       [trie prefix="abc"]  (merged)
   *           / 'x' \ 'y'                / 'x' \ 'y' \ 'z'
   *        [d1]   [d2]               [m1]   [m2]   [s3]
   *     SRC: [trie prefix="abc"]
   *           / 'y' \ 'z'
   *        [s2]   [s3]
   *     (Branch 'y' recurses, 'x' kept, 'z' copied from src)
   *
   * Case 3: Dst prefix is prefix of src prefix (TODO)
   *   BEFORE:                        AFTER:
   *     DST: [trie prefix="ab"]        Need to recurse into dst
   *     SRC: [trie prefix="abcd"]      branch matching src_prefix[2]
   *
   * Case 4: Src prefix is prefix of dst prefix (TODO)
   *   BEFORE:                        AFTER:
   *     DST: [trie prefix="abcd"]      Need to insert dst as child
   *     SRC: [trie prefix="ab"]        of copied src trie
   */
  offset_e merge_trie_into_trie(offset_e dst_offset,
                                src_trie_ptr src_trie,
                                src_offset_e src_offset, const Slice& src_prefix) {
    auto dst_trie = resolve_dst<TrieNode>(&dst_offset);
    Slice dst_prefix((const char*)dst_trie->compressed(), dst_trie->len());

    size_t common = common_prefix_len(dst_prefix, src_prefix);

    if (common < dst_prefix.size() && common < src_prefix.size()) {
      // Prefixes diverge - create split trie
      Slice prefix(dst_prefix.data(), common);
      int dst_branch = (uint8_t)dst_prefix[common];
      int src_branch = (uint8_t)src_prefix[common];

      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(common, dst_branch, src_branch));
      auto idxs = new_trie->create(prefix, dst_branch, src_branch);

      // Reduced dst trie
      offset_e dst_child = copy_trie_with_reduced_prefix(dst_trie, common);
      new_trie->array()[idxs.first] = dst_child;

      // Copy src subtrie with adjusted prefix
      size_t saved = key_buf.size();
      key_buf.append(prefix.data(), prefix.size());
      key_buf += (char)src_branch;
      offset_e src_child = copy_subtree_with_prefix_adjust(src_trie, src_offset,
                                                           common);
      key_buf.resize(saved);
      new_trie->array()[idxs.second] = src_child;

      free_trie(dst_trie);
      return resolve(new_trie);
    }

    if (common == dst_prefix.size() && common == src_prefix.size()) {
      // Case 2: Prefixes match exactly - merge children
      size_t saved = key_buf.size();
      key_buf.append(dst_prefix.data(), dst_prefix.size());
      offset_e result = merge_trie_children(dst_trie, dst_offset, src_trie, src_offset);
      key_buf.resize(saved);
      return result;
    }

    if (common == dst_prefix.size()) {
      // Case 3: Dst prefix is prefix of src prefix
      //   DST: [trie prefix="ab"]   →  [trie prefix="ab"]  (COW)
      //           |  'x'                  /  'x'   \  'c'
      //        [child]              [child]    [src reduced]
      //   SRC: [trie prefix="abcd"]
      //
      // Recurse into dst branch 'c' (or add if doesn't exist)
      int src_branch = (uint8_t)src_prefix[common];
      size_t saved = key_buf.size();
      key_buf.append(dst_prefix.data(), dst_prefix.size());
      key_buf += (char)src_branch;

      if (dst_trie->isset(src_branch)) {
        // Branch exists - recurse with reduced src prefix
        offset_e* child_ptr = dst_trie->offset(src_branch);
        offset_e new_child = merge_subtree_into_dst(*child_ptr, src_trie, src_offset,
                                                     common);
        key_buf.resize(saved);

        if (new_child == *child_ptr) {
          return dst_offset;  // No change
        }
        return copy_trie_with_updated_branch(dst_trie, src_branch, new_child);
      } else {
        // Branch doesn't exist - add src subtree as new branch
        offset_e src_child = copy_subtree_with_prefix_adjust(src_trie, src_offset,
                                                              common);
        key_buf.resize(saved);
        trie_ptr new_trie =
            alloc_node<trie_ptr>(dst_trie->increment_size(src_branch));
        uint16_t idx = new_trie->create(*dst_trie, src_branch);
        new_trie->array()[idx] = src_child;

        free_trie(dst_trie);
        return resolve(new_trie);
      }
    }

    // Case 4: Src prefix is prefix of dst prefix (common == src_prefix.size())
    //   DST: [trie prefix="abcd"]  →  [trie prefix="ab"]  (from src)
    //           |  ...                   /  ...   \  'c'
    //        [dst_kids]            [src_kids]  [dst reduced]
    //   SRC: [trie prefix="ab"]
    //           |  ...
    //        [src_kids]
    //
    // Copy src children, merge dst (with reduced prefix) into appropriate branch
    size_t saved = key_buf.size();
    key_buf.append(src_prefix.data(), src_prefix.size());
    int dst_branch = (uint8_t)dst_prefix[common];

    clear_children_buf();
    uint16_t branch_count = 0;

    for (int i = src_trie->first(); i != SrcTrieNode::OUT_OF_RANGE; i = src_trie->next(i)) {
      src_offset_e* child = src_trie->offset(i);
      size_t prefix_len = key_buf.size();
      if (i != TrieNode::NONE) key_buf += (char)i;

      if (i == dst_branch) {
        // Merge reduced dst into this src branch
        offset_e dst_reduced = copy_trie_with_reduced_prefix(dst_trie, common);
        children_buf[i + 1] = merge_trees(dst_reduced, *child);
      } else {
        children_buf[i + 1] = copy_subtree(*child);
      }
      key_buf.resize(prefix_len);
      if (children_buf[i + 1]) branch_count++;
    }

    // If dst_branch wasn't in src_trie, add dst (reduced) as new branch
    if (!src_trie->isset(dst_branch)) {
      offset_e dst_reduced = copy_trie_with_reduced_prefix(dst_trie, common);
      children_buf[dst_branch + 1] = dst_reduced;
      branch_count++;
    }
    key_buf.resize(saved);

    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(src_prefix.size(), branch_count));
    new_trie->create(src_prefix, children_buf + 1);

    free_trie(dst_trie);
    return resolve(new_trie);
  }

  /**
   * Merge children of two tries with matching prefixes.
   *
   * Both tries have identical prefix. Merge branch-by-branch:
   *
   *   DST:  [trie]               SRC:  [trie]
   *        / NONE \ 'a' \ 'b'          / NONE \ 'b' \ 'c'
   *      [d0]   [d1]   [d2]          [s0]   [s2']  [s3]
   *
   *   RESULT: [new_trie]
   *          / NONE \ 'a' \ 'b'    \ 'c'
   *    merge(d0,s0) [d1] merge(d2,s2') [s3]
   *
   * - NONE: both have → recurse merge
   * - 'a': only dst has → keep dst child
   * - 'b': both have → recurse merge
   * - 'c': only src has → copy src child
   */
  offset_e merge_trie_children(trie_ptr dst_trie, offset_e dst_offset,
                               src_trie_ptr src_trie,
                               src_offset_e src_offset) {
    // Collect all children from both tries
    clear_children_buf();
    bool changed = false;

    // First, copy all dst children using bitmap iteration
    for (int i = dst_trie->first(); i != TrieNode::OUT_OF_RANGE; i = dst_trie->next(i)) {
      children_buf[i + 1] = *dst_trie->offset(i);
    }

    // Then, merge src children using bitmap iteration
    size_t saved = key_buf.size();
    for (int i = src_trie->first(); i != SrcTrieNode::OUT_OF_RANGE; i = src_trie->next(i)) {
      src_offset_e* src_child = src_trie->offset(i);
      if (i != TrieNode::NONE) key_buf += (char)i;

      if (children_buf[i + 1]) {
        // Both have this branch - recurse
        offset_e merged = merge_trees(children_buf[i + 1], *src_child);
        if (merged != children_buf[i + 1]) {
          children_buf[i + 1] = merged;
          changed = true;
        }
      } else {
        // Only src has this branch - copy
        children_buf[i + 1] = copy_subtree(*src_child);
        changed = true;
      }
      key_buf.resize(saved);
    }

    if (!changed) {
      return dst_offset;
    }

    // Create new trie with merged children
    Slice prefix((const char*)dst_trie->compressed(), dst_trie->len());

    // Count branches (dst_count + new src branches - could track incrementally but count() is fast)
    uint16_t branch_count = 0;
    for (int i = 0; i < 257; i++) {
      if (children_buf[i]) branch_count++;
    }

    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
    new_trie->create(prefix, children_buf + 1);  // +1 because NONE is at index 0

    free_trie(dst_trie);
    return resolve(new_trie);
  }

  // =========================================================================
  // Copy operations
  // =========================================================================

  /**
   * Copy/migrate entire source subtree to destination.
   *
   * Deep copies a subtree from source DB to destination DB.
   * Applies MergePolicy to each leaf to check if it should be copied.
   *
   *   SRC:  [trie prefix="abc"]    →    DST:  [trie prefix="abc"]  (new)
   *            / 'x' \ 'y'                      / 'x' \ 'y'
   *         [leaf] [trie]                  [leaf'] [trie']  (all new)
   *                  |                              |
   *               [leaf]                         [leaf']
   */
  offset_e copy_subtree(src_offset_e src_offset) {
    if (!src_offset) return offset_e();

    if (src_offset.type() == LEAF) {
      auto src_leaf = resolve_src<SrcLeafNode, LEAF>(&src_offset);
      Slice key = src_leaf->key();
      Slice value = get_src_value(src_leaf);

      size_t saved = key_buf.size();
      key_buf.append(key.data(), key.size());
      bool should_copy = handler.check_overwrite(key_buf, Slice(), value);
      key_buf.resize(saved);
      if (!should_copy) {
        return offset_e();  // Policy says don't copy
      }

      return create_leaf(key, value);
    } else {
      auto src_trie = resolve_src<SrcTrieNode>(&src_offset);
      return copy_trie(src_trie, src_offset);
    }
  }

  /**
   * Copy a source trie and all its children to destination.
   */
  offset_e copy_trie(src_trie_ptr src_trie, src_offset_e src_offset) {
    Slice prefix((const char*)src_trie->compressed(), src_trie->len());
    size_t saved = key_buf.size();
    key_buf.append(prefix.data(), prefix.size());

    // Collect and copy all children using bitmap iteration
    clear_children_buf();
    uint16_t branch_count = 0;

    for (int i = src_trie->first(); i != SrcTrieNode::OUT_OF_RANGE; i = src_trie->next(i)) {
      src_offset_e* child = src_trie->offset(i);
      size_t prefix_len = key_buf.size();
      if (i != TrieNode::NONE) key_buf += (char)i;

      children_buf[i + 1] = copy_subtree(*child);
      key_buf.resize(prefix_len);
      if (children_buf[i + 1]) branch_count++;
    }
    key_buf.resize(saved);

    if (branch_count == 0) return offset_e();

    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
    new_trie->create(prefix, children_buf + 1);
    return resolve(new_trie);
  }

  offset_e copy_subtree_with_prefix_adjust(
      src_trie_ptr src_trie, src_offset_e src_offset,
      size_t skip_prefix) {
    Slice old_prefix((const char*)src_trie->compressed(), src_trie->len());
    Slice new_prefix_slice(old_prefix.data() + skip_prefix,
                           old_prefix.size() - skip_prefix);

    // Copy children using bitmap iteration
    clear_children_buf();
    uint16_t branch_count = 0;
    size_t saved = key_buf.size();

    for (int i = src_trie->first(); i != SrcTrieNode::OUT_OF_RANGE; i = src_trie->next(i)) {
      src_offset_e* child = src_trie->offset(i);
      if (i != TrieNode::NONE) key_buf += (char)i;

      children_buf[i + 1] = copy_subtree(*child);
      key_buf.resize(saved);
      if (children_buf[i + 1]) branch_count++;
    }

    if (branch_count == 0) return offset_e();

    trie_ptr new_trie = alloc_node<trie_ptr>(
        TrieNode::size(new_prefix_slice.size(), branch_count));
    new_trie->create(new_prefix_slice, children_buf + 1);
    return resolve(new_trie);
  }

  offset_e copy_trie_with_reduced_prefix(trie_ptr trie, size_t skip_prefix) {
    Slice old_prefix((const char*)trie->compressed(), trie->len());
    Slice new_prefix(old_prefix.data() + skip_prefix,
                     old_prefix.size() - skip_prefix);

    trie_ptr new_trie =
        alloc_node<trie_ptr>(trie->changed_len(new_prefix.size()));
    new_trie->create(*trie, new_prefix);
    return resolve(new_trie);
  }

  offset_e copy_trie_with_updated_branch(trie_ptr old_trie, int branch,
                                         offset_e new_child) {
    // Create copy of trie with same structure but updated child
    trie_ptr new_trie = alloc_node<trie_ptr>(old_trie->size());
    Slice prefix((const char*)old_trie->compressed(), old_trie->len());

    // Copy structure
    memcpy((char*)new_trie, (const char*)old_trie, old_trie->size());

    // Update the specific branch
    int idx = new_trie->array_index(branch);
    assert(idx >= 0);
    new_trie->array()[idx] = new_child;

    free_trie(old_trie);
    return resolve(new_trie);
  }

  // =========================================================================
  // Split operations
  // =========================================================================

  /**
   * Split a destination trie to accommodate a source leaf that diverges
   * within the trie's prefix.
   *
   *   BEFORE:                        AFTER:
   *     DST: [trie prefix="abcd"]       [trie prefix="ab"]  (new parent)
   *              |  'x'                    /  'c'   \  'd'
   *           [child]         [trie prefix="d"]  [leaf:"ef"=v]  (new)
   *     SRC: [leaf:"abdef"=v]        |  'x'
   *                               [child]
   *
   *   split_pos=2: common prefix is "ab", dst continues with 'c',
   *   src continues with 'd'. Original trie's prefix is reduced.
   */
  offset_e split_trie_for_leaf(trie_ptr dst_trie, offset_e dst_offset,
                               size_t split_pos, const Slice& src_key,
                               const Slice& src_value) {
    Slice dst_prefix((const char*)dst_trie->compressed(), dst_trie->len());

    // Split prefix
    Slice common_prefix(dst_prefix.data(), split_pos);
    int dst_branch = (uint8_t)dst_prefix[split_pos];
    int src_branch = (split_pos < src_key.size()) ? (uint8_t)src_key[split_pos]
                                                  : TrieNode::NONE;

    // Create new parent trie
    trie_ptr new_parent =
        alloc_node<trie_ptr>(TrieNode::size(split_pos, dst_branch, src_branch));
    auto idxs = new_parent->create(common_prefix, dst_branch, src_branch);

    // Create reduced dst trie
    offset_e dst_child = copy_trie_with_reduced_prefix(dst_trie, split_pos);
    new_parent->array()[idxs.first] = dst_child;

    // Create new leaf for src
    Slice leaf_key(src_key.data() + split_pos, src_key.size() - split_pos);
    leaf_ptr new_leaf = create_leaf_node(leaf_key, src_value);
    new_parent->array()[idxs.second] = resolve(new_leaf);

    free_trie(dst_trie);
    return resolve(new_parent);
  }

  // =========================================================================
  // Leaf operations
  // =========================================================================

  offset_e create_leaf(const Slice& key, const Slice& value) {
    leaf_ptr leaf = create_leaf_node(key, value);
    return resolve(leaf);
  }

  leaf_ptr create_leaf_node(const Slice& key, const Slice& value) {
    leaf_ptr leaf =
        alloc_node<leaf_ptr>(LeafNode::size(key.size(), value.size()));
    leaf->set(key, value.size());
    memcpy(leaf->vdata(), value.data(), value.size());
    return leaf;
  }

  void free_leaf(leaf_ptr& leaf) {
    if (leaf->is_big()) handler.free_big(leaf);
    free_node(leaf);
  }

  void free_trie(trie_ptr& trie) { free_node(trie); }

  // =========================================================================
  // Helper functions
  // =========================================================================

  void clear_children_buf() {
    memset(children_buf, 0, sizeof(children_buf));
  }

  size_t common_prefix_len(const Slice& a, const Slice& b) {
    size_t len = std::min(a.size(), b.size());
    for (size_t i = 0; i < len; i++) {
      if (a[i] != b[i]) return i;
    }
    return len;
  }

  Slice get_src_value(src_leaf_ptr src_leaf) {
    if (src_leaf->is_big()) {
      return handler.migrate_big_value(*src_leaf, db_src);
    }
    return src_leaf->value();
  }
};

}  // namespace leaves

#endif  // _LEAVES_MERGER_HPP
