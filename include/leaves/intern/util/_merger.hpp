#ifndef _LEAVES_MERGER_HPP
#define _LEAVES_MERGER_HPP

#include "../core/_bits.hpp"
#include "../core/_node.hpp"
#include "../db/_cursor.hpp"

namespace leaves {

// _NodeIterator is now defined in _cursor.hpp

/**
 * @brief Result of migrate_big_value
 *
 * Contains the value data to store and whether it should be marked as big.
 * This allows policies to convert big values to small values during migration.
 */
struct MigratedValue {
  Slice data;   // Value data (BigValue struct if is_big, actual data if not)
  bool is_big;  // Whether destination leaf should be marked as big
};

/**
 * @brief Standard MergePolicy with default big value migration
 *
 * Provides a default implementation for migrate_big_value that allocates
 * space in the destination BigMemory and copies the value data.
 * Policies can inherit from this or implement their own migrate_big_value.
 */
struct StandardMergePolicy {
  // Always overwrite destination with source
  bool may_overwrite(const std::string& key, const Slice& dst, const Slice& src,
                     bool dst_is_big, bool src_is_big) {
    return true;
  }

  // Always add new leaves from source
  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return true;
  }

  // Always recurse into source tries
  bool may_add_trie(const std::string& key) { return true; }

  // Called after a trie node is created/merged during the merge process.
  // Override to compute the trie's hash based on its children's hashes.
  // |trie| is the newly created trie node, |db| is the destination database.
  // Children's hashes are already computed (either copied or computed earlier).
  template <typename TriePtr, typename DB>
  void after_trie_merged(TriePtr& trie, DB* db) {}

  // Free big value from destination BigMemory
  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {
    _BigValue* bvalue = (_BigValue*)leaf->vdata();
    dst_cursor.get_bigmemory().free(bvalue);
  }

  // Storage for the _BigValue returned by migrate_big_value
  mutable _BigValue _big_value_storage;

  /**
   * @brief Migrate a big value from source to destination
   *
   * Reads the big value data from the source, allocates space in the
   * destination BigMemory, copies the data, and returns a MigratedValue
   * containing the new _BigValue struct.
   *
   * Since _BigValue uses fixed little-endian types, no endian conversion
   * is needed between source and destination — the layout is always the same.
   *
   * @param leaf Source leaf containing the big value reference
   * @param src_cursor Source cursor for resolving the big value data
   * @param dst_cursor Destination cursor for allocating new big value
   * @return MigratedValue with data pointing to _BigValue and is_big=true
   */
  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor,
                                  DstCursor& dst_cursor) {
    // Source BigValue uses fixed little-endian layout
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData {
      char data;
    };
    auto src_data =
        src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

    // Allocate in destination BigMemory and copy data
    _BigValue* dst_bvalue = &_big_value_storage;
    dst_cursor.get_bigmemory().alloc(value_size, dst_bvalue);
    offset_t dst_offset(dst_bvalue->chunk_offset);
    auto dst_data =
        dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    optimized_memcpy((char*)dst_data, (char*)src_data, value_size);

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }
};

/**
 * @brief Merger for combining two tries
 *
 * Merges source trie into destination trie, using a MergePolicy to control
 * which nodes are copied, overwritten, or filtered. The policy's may_add_leaf,
 * may_add_trie, and may_overwrite methods determine merge behavior.
 *
 * Big values are migrated via the policy's migrate_big_value method, which
 * can copy them to the destination's BigMemory or convert them to small values.
 */
template <typename CursorDst, typename CursorSrc, typename MergePolicy>
struct _Merger {
  typedef _Merger<CursorDst, CursorSrc, MergePolicy> Merger;
  using Traits = typename CursorDst::Traits;
  using SrcTraits = typename CursorSrc::Traits;
  using Transition = typename CursorDst::Transition;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using page_ptr = typename Transition::page_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;
  using SrcTransition = typename CursorSrc::Transition;
  using SrcTrieNode = typename SrcTransition::TrieNode;
  using SrcLeafNode = typename SrcTransition::LeafNode;
  using src_offset_e = typename SrcTransition::offset_e;
  using BigMemory = typename CursorDst::BigMemory;
  using BigValue = typename BigMemory::BigValue;

  CursorDst& dst_cursor;
  CursorSrc& src_cursor;
  MergePolicy& handler;
  std::string current_key;

  _Merger(CursorDst& dest, CursorSrc& src, MergePolicy& handler)
      : dst_cursor(dest), src_cursor(src), handler(handler) {}

  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    return dst_cursor._db->template alloc_node<NodePtr>(node_size);
  }

  // Free node by computing PageHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    using PageHeader = typename Traits::PageHeader;
    static_assert(
        !std::is_same_v<NodePtr, page_ptr>,
        "free_node must be called with node pointers, not page pointers");

    if constexpr (NodePtr::type == LEAF) {
      if (node->is_big()) handler.free_big(node, dst_cursor);
    }

    page_ptr page((char*)node - sizeof(PageHeader));
    dst_cursor._db->free(page);
  }

  leaf_ptr fill_leaf(const Slice& key, SrcLeafNode& src_leaf) {
    Slice src_value;
    bool is_big = false;
    if (src_leaf.is_big()) {
      // migrate_big_value reads from src, allocates in dst, returns
      // MigratedValue
      auto migrated =
          handler.migrate_big_value(src_leaf, src_cursor, dst_cursor);
      src_value = migrated.data;
      is_big = migrated.is_big;
    } else {
      src_value = src_leaf.value();
    }

    uint16_t vsize = src_value.size();
    leaf_ptr leaf = alloc_node<leaf_ptr>(LeafNode::size(key.size(), vsize));
    leaf->set(key, vsize);
    if (is_big) {
      leaf->set_big();
    }
    memcpy(leaf->vdata(), src_value.data(), vsize);

    // Copy hash from source leaf - content hashes are identical
    if constexpr (sizeof(leaf->hash) > 0) {
      memcpy(leaf->hash, src_leaf.hash, sizeof(leaf->hash));
    }

    return leaf;
  }

  template <typename T>
  typename SrcTraits::template Pointer<T> resolve_src(
      const src_offset_e* offset_ptr) {
    return src_cursor._db->template resolve<T>(offset_ptr);
  }

  template <typename T>
  typename Traits::template Pointer<T> resolve_dst(const offset_e* offset_ptr) {
    return dst_cursor._db->template resolve<T>(offset_ptr);
  }

  template <typename T>
  offset_t resolve_offset(T ptr) {
    return dst_cursor._db->resolve(ptr);
  }

  void exec() {
    current_key = src_cursor.current_key;
    merge_node();
  }

  void merge_node() {
    if (!src_cursor.stack.size) return;
    size_t size = current_key.size();
    auto src = src_cursor.stack.back();
    if (src.is_leaf()) {
      auto& leaf = src.leaf();
      auto key = leaf->key();
      current_key.append((const char*)key.data(), key.size());
    } else {
      auto& trie = src.trie();
      current_key.append((const char*)trie->compressed(), trie->len());
    }

    dst_cursor.find(current_key);

    // Handle empty destination - deep copy entire source tree
    if (!dst_cursor.stack.size) {
      // Get root from source - it's the first element in the stack.
      // Undo the prefix append above — selective_deep_copy_trie will
      // re-append it when it resolves the root node.
      current_key.resize(size);
      selective_deep_copy_subtree(src_cursor.stack.front().offset,
                                  dst_cursor._root);
      return;
    }

    assert(dst_cursor.current_key.size() <= current_key.size());
    assert(dst_cursor.current_key ==
           current_key.substr(0, dst_cursor.current_key.size()));

    auto& dst = dst_cursor.stack.back();
    if (dst.is_trie()) {
      merge_trie_node(dst, src);
    } else {
      if (!dst.leaf()->key_size && src.is_trie() && !dst.is_root()) {
        // dst is a none branch leaf and src is a trie -> we want to merge the
        // trie
        dst_cursor.pop();
        merge_trie_node(dst_cursor.stack.back(), src);
      } else
        merge_leaf_node(dst, src);
    }
    current_key.resize(size);
    src_cursor.pop();
  }

  void merge_leaf_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src) {
    auto& dst_leaf = dst.leaf();
    assert(dst.prefix <= dst_leaf->key_size);

    // Check for exact key match: src is leaf and both keys end at split point
    if (src.is_leaf() && dst.cmp == 0) {
      auto& src_leaf = src.leaf();
      // Exact match — check may_overwrite before touching anything
      if (!handler.may_overwrite(current_key, dst_leaf->value(),
                                 src_leaf->value(), dst_leaf->is_big(),
                                 src_leaf->is_big())) {
        return;  // Keep dst unchanged
      }
      // Overwrite: create new leaf with src key/value, replace dst
      leaf_ptr new_leaf = fill_leaf(src_leaf->key(), *src_leaf);
      *dst.offset = resolve_offset(new_leaf);
      free_node(dst_leaf);
      return;
    }

    // Not an exact match — need to create reduced copy for split
    leaf_ptr new_leaf = dst_leaf;
    if (dst.prefix) {
      new_leaf = _Inserter(&dst, 0).copy_reduced_leaf(dst.prefix, dst_leaf);
      free_node(dst_leaf);
    }

    resolve_divergence(dst, src,
                       new_leaf->key_size ? new_leaf->data[0] : TrieNode::NONE,
                       resolve_offset(new_leaf));
  }

  void merge_trie_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src) {
    auto& dst_trie = dst.trie();
    assert(dst.prefix <= dst_trie->len());
    uint8_t suffix_len = dst_trie->len() - dst.prefix;

    if (suffix_len == 0) {
      merge_into_trie(dst, src);
    } else {
      trie_ptr new_trie =
          alloc_node<trie_ptr>(dst_trie->changed_len(suffix_len));
      new_trie->create(*dst_trie,
                       Slice(&dst_trie->compressed()[dst.prefix], suffix_len));
      free_node(dst_trie);
      assert(new_trie->len() > 0);
      resolve_divergence(dst, src, new_trie->compressed()[0],
                         resolve_offset(new_trie));
    }
  }

  void resolve_divergence(typename CursorDst::Transition& dst,
                          typename CursorSrc::Transition& src, int key1,
                          offset_t child1) {
    uint16_t split_pos = dst.keypos + dst.prefix;
    assert(split_pos >= src.keypos);
    uint16_t src_split_pos = split_pos - src.keypos;
    assert(src_split_pos < 256);

    if (src.is_leaf()) {
      int key = split_pos < current_key.size() ? current_key[split_pos]
                                               : TrieNode::NONE;
      auto& src_leaf = src.leaf();
      assert(src_leaf->key_size >= src_split_pos);
      uint8_t suffix_len = src_leaf->key_size - src_split_pos;

      Slice src_value(src_leaf->value());
      if (!handler.may_add_leaf(current_key, src_value, src_leaf->is_big()))
        return;  // rejected

      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
      auto idxs = new_trie->create(
          Slice(current_key.data() + dst.keypos, dst.prefix), key1, key);
      new_trie->array()[idxs.first] = child1;
      dst.trie() = new_trie;
      dst.link_idx = idxs.second;
      dst.update_trie_offset();

      leaf_ptr new_leaf = fill_leaf(
          Slice(&src_leaf->data[src_split_pos], suffix_len), *src_leaf);
      *dst.link() = resolve_offset(new_leaf);

      // Compute hash for the new trie - both children have valid hashes
      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    if (split_pos < current_key.size()) {
      assert(src.is_trie());
      int key = current_key[split_pos];

      // A new trie with two branches for the old dst and the new src
      // But first, selectively copy the src side — if nothing survives
      // may_add, we skip the split entirely.

      // src is a trie — selectively copy children and build a suffix trie
      // with only survivors
      auto& src_trie = src.trie();
      assert(src_trie->len() >= src_split_pos);
      uint8_t suffix_len = src_trie->len() - src_split_pos;
      Slice suffix_prefix((const char*)&src_trie->compressed()[src_split_pos],
                          suffix_len);

      // Append suffix to current_key so may_add_trie/may_add_leaf see correct keys
      size_t saved_key_len = current_key.size();
      current_key.append((const char*)suffix_prefix.data(), suffix_len);

      // Early-out: let the policy reject the entire subtree by prefix
      if (!handler.may_add_trie(current_key)) {
        current_key.resize(saved_key_len);
        return;
      }

      // Collect surviving children into a flat offset array
      // +1 offset so offsets_buf[-1] (NONE) is valid
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int surviving = 0;
      for (int k = src_trie->first(); k != SrcTrieNode::OUT_OF_RANGE;
           k = src_trie->next(k)) {
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_trie->offset(k), &child_offset)) {
          offsets_buf[k] = child_offset;
          surviving++;
        }
      }

      current_key.resize(saved_key_len);

      if (!surviving) {
        return;  // nothing from source survived may_add
      }

      // Build suffix trie from survivors
      trie_ptr suffix_trie =
          alloc_node<trie_ptr>(TrieNode::size(suffix_prefix.size(), surviving));
      suffix_trie->create(suffix_prefix, offsets_buf);

      // Compute hash for suffix_trie - children have valid hashes from deep copy
      handler.after_trie_merged(suffix_trie, dst_cursor._db);

      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
      auto idxs = new_trie->create(
          Slice(current_key.data() + dst.keypos, dst.prefix), key1, key);
      new_trie->array()[idxs.first] = child1;
      dst.trie() = new_trie;
      dst.link_idx = idxs.second;
      dst.update_trie_offset();
      *dst.link() = resolve_offset(suffix_trie);

      // Compute hash for new_trie - child1 and suffix_trie have valid hashes
      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    assert(src.is_trie());
    assert(split_pos == current_key.size());
    // the whole prefix of the src == dst.prefix

    auto src_trie = src.trie();
    if (src_trie->isset(key1)) {
      // Src trie has a branch matching the dst child's key.
      // We need to merge that branch recursively, and selectively
      // copy all other src branches.

      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 1;  // key1 branch

      offsets_buf[key1] = child1;

      // Selectively deep copy all other src branches
      for (int k = src_trie->first(); k != SrcTrieNode::OUT_OF_RANGE;
           k = src_trie->next(k)) {
        if (k == key1) continue;
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_trie->offset(k), &child_offset)) {
          offsets_buf[k] = child_offset;
          branch_count++;
        }
      }

      Slice prefix((const char*)src_trie->compressed(), src_trie->len());
      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
      new_trie->create(prefix, offsets_buf);

      dst.trie() = new_trie;
      dst.update_trie_offset();

      // Recursively merge the shared branch (skip if src offset is incomplete)
      if (*src_trie->offset(key1) != 0) {
        src_cursor.push(src_trie->offset(key1));
        dst_cursor.stack.clear();
        merge_node();
      }

      // Compute hash for new_trie - all children now have valid hashes
      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    // Src trie does not have key1 — add key1 as a new branch in src's
    // structure, and selectively copy src's existing branches.
    {
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 1;  // key1 branch

      offsets_buf[key1] = child1;

      for (int k = src_trie->first(); k != SrcTrieNode::OUT_OF_RANGE;
           k = src_trie->next(k)) {
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_trie->offset(k), &child_offset)) {
          offsets_buf[k] = child_offset;
          branch_count++;
        }
      }

      Slice prefix((const char*)src_trie->compressed(), src_trie->len());
      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
      new_trie->create(prefix, offsets_buf);

      dst.trie() = new_trie;
      dst.update_trie_offset();

      // Compute hash for new_trie - all children have valid hashes
      handler.after_trie_merged(new_trie, dst_cursor._db);
    }
  }

  void merge_into_trie(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src) {
    using SrcTrieNode = typename CursorSrc::Transition::TrieNode;
    using DstTrieNode = typename CursorDst::Transition::TrieNode;
    assert(dst.is_trie());
    assert(dst.prefix == dst.trie()->len());
    assert(current_key.size() >= dst_cursor.current_key.size());
    assert(current_key.size() < dst_cursor.current_key.size() + 256);
    uint8_t suffix_len = current_key.size() - dst_cursor.current_key.size();
    auto dst_trie = dst.trie();

    if (src.is_leaf()) return merge_leaf_into_trie(dst, src, suffix_len);

    auto& src_trie = src.trie();
    if (suffix_len) {
      // src prefix is longer -> selectively copy children, then build
      // a correctly-sized suffix trie from survivors only.

      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int surviving = 0;
      for (int k = src_trie->first(); k != SrcTrieNode::OUT_OF_RANGE;
           k = src_trie->next(k)) {
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_trie->offset(k), &child_offset)) {
          offsets_buf[k] = child_offset;
          surviving++;
        }
      }

      if (!surviving) return;  // nothing from source survived may_add

      Slice suffix_prefix(
          (const char*)&src_trie->compressed()[src_trie->len() - suffix_len],
          suffix_len);
      trie_ptr suffix_trie =
          alloc_node<trie_ptr>(TrieNode::size(suffix_prefix.size(), surviving));
      suffix_trie->create(suffix_prefix, offsets_buf);

      // Compute hash for suffix_trie - children have valid hashes from deep copy
      handler.after_trie_merged(suffix_trie, dst_cursor._db);

      uint16_t loffset;
      trie_ptr new_trie =
          expand_trie_with_branch(dst_trie, suffix_len, &loffset);

      dst.trie() = new_trie;
      dst.link_idx = loffset;
      *dst.link() = resolve_offset(suffix_trie);
      dst.update_trie_offset();

      // Compute hash for new_trie - all children have valid hashes
      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    // Merge children of both tries into a flat offset array.
    // Shared branches are merged recursively after building the new trie.

    // +1 offset so offsets_buf[-1] (NONE) is valid
    offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
    offset_e* offsets_buf = &offsets_raw[1];

    struct SharedBranch {
      int key;
      src_offset_e* src_off;  // non-const: push() takes mutable pointer
      offset_e dst_off;
    };
    SharedBranch shared[257];
    int shared_count = 0;
    int branch_count = 0;

    // Copy all dst branches (they all survive)
    for (int k = dst_trie->first(); k != DstTrieNode::OUT_OF_RANGE;
         k = dst_trie->next(k)) {
      offsets_buf[k] = *dst_trie->offset(k);
      branch_count++;
    }

    // Process src branches: record shared for later merge, selectively copy
    // src-only
    for (int k = src_trie->first(); k != SrcTrieNode::OUT_OF_RANGE;
         k = src_trie->next(k)) {
      if (dst_trie->isset(k)) {
        // Shared branch — merge recursively later (skip incomplete src)
        if (*src_trie->offset(k) != 0) {
          shared[shared_count++] = {
              k, const_cast<src_offset_e*>(src_trie->offset(k)),
              *dst_trie->offset(k)};
        }
      } else {
        // Src-only — selectively deep copy
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_trie->offset(k), &child_offset)) {
          offsets_buf[k] = child_offset;
          branch_count++;
        }
      }
    }

    // Build merged trie
    Slice prefix((const char*)dst_trie->compressed(), dst_trie->len());
    trie_ptr new_trie =
        alloc_node<trie_ptr>(DstTrieNode::size(prefix.size(), branch_count));
    new_trie->create(prefix, offsets_buf);

    dst.trie() = new_trie;
    dst.update_trie_offset();

    // Recursively merge shared branches
    for (int si = 0; si < shared_count; si++) {
      int k = shared[si].key;
      *new_trie->offset(k) = shared[si].dst_off;

      src_cursor.push(shared[si].src_off);
      dst_cursor.stack.clear();
      merge_node();
    }

    // Compute hash for new_trie - all children now have valid hashes
    handler.after_trie_merged(new_trie, dst_cursor._db);

    free_node(dst_trie);
  }

  void merge_leaf_into_trie(typename CursorDst::Transition& dst,
                            typename CursorSrc::Transition& src,
                            int suffix_len) {
    // src is a leaf -> insert into dst trie (if may_add allows)
    auto& src_leaf = src.leaf();

    if (!handler.may_add_leaf(current_key, src_leaf->value(),
                              src_leaf->is_big()))
      return;  // rejected

    uint16_t loffset;
    trie_ptr new_trie =
        expand_trie_with_branch(dst.trie(), suffix_len, &loffset);

    assert(src_leaf->key_size >= suffix_len);
    uint8_t split_pos = src_leaf->key_size - suffix_len;
    leaf_ptr new_leaf =
        fill_leaf(Slice(&src_leaf->data[split_pos], suffix_len), *src_leaf);

    dst.trie() = new_trie;
    dst.link_idx = loffset;
    *dst.link() = resolve_offset(new_leaf);
    dst.update_trie_offset();

    // Compute hash for new_trie - all children have valid hashes
    handler.after_trie_merged(new_trie, dst_cursor._db);
  }

  typename CursorDst::Transition::trie_ptr expand_trie_with_branch(
      typename CursorDst::Transition::trie_ptr& dst_trie, int suffix_len,
      uint16_t* loffset) {
    int branch_key = suffix_len ? current_key[dst_cursor.current_key.size()]
                                : TrieNode::NONE;
    assert(!(branch_key == TrieNode::NONE ? dst_trie->has_none()
                                          : dst_trie->isset(branch_key)));
    // otherwise find would have walked down

    trie_ptr new_trie =
        alloc_node<trie_ptr>(dst_trie->increment_size(branch_key));
    *loffset = new_trie->create(*dst_trie, branch_key);
    free_node(dst_trie);
    return new_trie;
  }

  // ── Selective (filtered) deep copy ────────────────────────────────────
  // These variants consult handler.may_add() for every leaf and propagate
  // rejections upward: if a whole sub-trie becomes empty, the branch is
  // dropped from its parent.  Returns true iff at least one leaf survived.

  /**
   * @brief Selectively deep copy an entire subtree, filtering leaves via
   * may_add.
   * @param src_offset Source offset to copy from
   * @param parent_link Pointer to parent's offset slot to set (zeroed when
   * rejected)
   * @return true if at least one leaf was copied into the destination
   */
  bool selective_deep_copy_subtree(const src_offset_e* src_offset,
                                   offset_e* parent_link) {
    if (*src_offset == 0) {
      *parent_link = offset_e();
      return false;
    }

    if (src_offset->type() == LEAF) {
      return selective_deep_copy_leaf(src_offset, parent_link);
    } else {
      return selective_deep_copy_trie(src_offset, parent_link);
    }
  }

  /**
   * @brief Selectively deep copy a single leaf, consulting may_add.
   *
   * Reconstructs the full key from current_key + the leaf's own suffix,
   * calls handler.may_add_leaf(), and only copies the leaf if allowed.
   */
  bool selective_deep_copy_leaf(const src_offset_e* src_offset,
                                offset_e* parent_link) {
    auto src_leaf = resolve_src<SrcLeafNode>(src_offset);
    size_t saved = current_key.size();
    current_key.append((const char*)src_leaf->data, src_leaf->key_size);

    bool accepted = handler.may_add_leaf(current_key, src_leaf->value(),
                                         src_leaf->is_big());
    current_key.resize(saved);

    if (!accepted) {
      *parent_link = offset_e();
      return false;
    }

    leaf_ptr new_leaf = fill_leaf(Slice(src_leaf->key()), *src_leaf);
    *parent_link = resolve_offset(new_leaf);
    return true;
  }

  /**
   * @brief Selectively deep copy a trie and its subtree, dropping rejected
   * leaves.
   *
   * Two-pass approach:
   *  1. Recurse into every child, accumulate offsets for survivors.
   *  2. Build a correctly-sized trie from survivors only.
   * Returns false (and writes a zero offset) when no child survives.
   */
  bool selective_deep_copy_trie(const src_offset_e* src_offset,
                                offset_e* parent_link) {
    auto src_trie = resolve_src<SrcTrieNode>(src_offset);

    // Append this trie's compressed prefix to current_key for descendants
    size_t saved = current_key.size();
    current_key.append((const char*)src_trie->compressed(), src_trie->len());

    // Early-out: let the policy reject the entire subtree by prefix
    if (!handler.may_add_trie(current_key)) {
      current_key.resize(saved);
      *parent_link = offset_e();
      return false;
    }

    // ── Pass 1: recurse children, collect survivors ──────────────────
    // offsets_buf is indexed as: NONE → index (-1+1)=0, byte 0 → 1, …, 255 →
    // 256 We offset the pointer so that offsets_buf[NONE] (i.e. [-1]) works.
    offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};  // all zero-initialised
    offset_e* offsets_buf = &offsets_raw[1];  // now [-1] is valid

    int surviving = 0;
    auto src_array = src_trie->array();
    for (int i = 0, k = src_trie->first(), scount = src_trie->count();
         i < scount; i++, k = src_trie->next(k)) {
      offset_e child_offset;
      if (selective_deep_copy_subtree(&src_array[i], &child_offset)) {
        offsets_buf[k] = child_offset;
        surviving++;
      }
    }

    current_key.resize(saved);

    if (surviving == 0) {
      *parent_link = offset_e();
      return false;
    }

    // ── Pass 2: build destination trie with only survivors ───────────
    Slice prefix((const char*)src_trie->compressed(), src_trie->len());
    uint16_t new_size = TrieNode::size(prefix.size(), surviving);
    trie_ptr dst_trie = alloc_node<trie_ptr>(new_size);
    dst_trie->create(prefix, offsets_buf);

    // Copy hash from source trie if all children survived unchanged.
    // If any child was filtered, compute the hash.
    if constexpr (sizeof(dst_trie->hash) > 0) {
      if (surviving == src_trie->count()) {
        memcpy(dst_trie->hash, src_trie->hash, sizeof(dst_trie->hash));
      } else {
        handler.after_trie_merged(dst_trie, dst_cursor._db);
      }
    }

    *parent_link = resolve_offset(dst_trie);
    return true;
  }
};

}  // namespace leaves

#endif  // _LEAVES_MERGER_HPP
