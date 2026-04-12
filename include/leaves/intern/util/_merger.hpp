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
  // Always overwrite destination with source.
  // Thread safety: may be called concurrently during parallel merge.
  bool may_overwrite(const std::string& key, const Slice& dst, const Slice& src,
                     bool dst_is_big, bool src_is_big) {
    return true;
  }

  // Always add new leaves from source.
  // Thread safety: may be called concurrently during parallel merge.
  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return true;
  }

  // Always recurse into source tries.
  // Thread safety: may be called concurrently during parallel merge.
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

  // Whether to descend into a src subtree during move-merge.
  // Returns true by default (descend into everything).
  // _ContextMergePolicy overrides to skip snapshot nodes.
  template <typename PagePtr>
  bool should_descend_src(PagePtr) { return true; }

  // Called when a src node is orphaned during move-merge (not adopted as-is).
  // Default: no-op. _ContextMergePolicy overrides to free writer-allocated nodes.
  template <typename NodePtr>
  void free_src(NodePtr&) {}

  // Storage for the _BigValue returned by migrate_big_value
  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor,
                                  DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData {
      char data;
    };
    auto src_data =
        src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

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
 * @brief Common base for _Merger and _MoveMerger
 *
 * Holds type aliases, cursor/policy references, and shared helper methods
 * (alloc, free, resolve, fill_leaf, expand_trie_with_branch).
 */
template <typename CursorDst, typename CursorSrc, typename MergePolicy>
struct _MergerBase {
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

  static constexpr bool _has_may_add_leaf = !std::is_same_v<decltype(&MergePolicy::may_add_leaf), decltype(&StandardMergePolicy::may_add_leaf)>;
  static constexpr bool _has_may_add_trie = !std::is_same_v<decltype(&MergePolicy::may_add_trie), decltype(&StandardMergePolicy::may_add_trie)>;
  static constexpr bool _has_should_descend_src = !std::is_same_v<decltype(&MergePolicy::template should_descend_src<page_ptr>), decltype(&StandardMergePolicy::template should_descend_src<page_ptr>)>;

  CursorDst& dst_cursor;
  CursorSrc& src_cursor;
  MergePolicy& handler;

  _MergerBase(CursorDst& dest, CursorSrc& src, MergePolicy& handler)
      : dst_cursor(dest), src_cursor(src), handler(handler) {}

  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    return dst_cursor.template alloc_node<NodePtr>(node_size);
  }

  // Free node by computing PageHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    using PageHeader = typename Traits::PageHeader;
    static_assert(
        !std::is_same_v<NodePtr, page_ptr>,
        "free_node must be called with node pointers, not page pointers");

    if constexpr (NodePtr::type == LEAF) {
      if (node->is_big()) {
        handler.free_big(node, dst_cursor);
      }
    }

    page_ptr page = node - sizeof(PageHeader);
    dst_cursor.free(page);
  }

  leaf_ptr fill_leaf(const Slice& key, SrcLeafNode& src_leaf) {
    Slice src_value;
    bool is_big = false;
    if (src_leaf.is_big()) {
      MigratedValue migrated =
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

  trie_ptr expand_trie_with_branch(
      trie_ptr& dst_trie, int suffix_len,
      uint16_t* loffset, std::string& current_key) {
    int branch_key = suffix_len ? current_key[dst_cursor.current_key.size()]
                                : TrieNode::NONE;
    assert(!(branch_key == TrieNode::NONE ? dst_trie->has_none()
                                          : dst_trie->isset(branch_key)));

    trie_ptr new_trie =
        alloc_node<trie_ptr>(dst_trie->increment_size(branch_key));
    *loffset = new_trie->create(*dst_trie, branch_key);
    free_node(dst_trie);
    return new_trie;
  }
};

/**
 * @brief Merger for combining two tries (deep-copy semantics)
 *
 * Merges source trie into destination trie, using a MergePolicy to control
 * which nodes are copied, overwritten, or filtered. The policy's may_add_leaf,
 * may_add_trie, and may_overwrite methods determine merge behavior.
 *
 * Big values are migrated via the policy's migrate_big_value method, which
 * can copy them to the destination's BigMemory or convert them to small values.
 */
template <typename CursorDst, typename CursorSrc, typename MergePolicy>
struct _Merger : _MergerBase<CursorDst, CursorSrc, MergePolicy> {
  using Base = _MergerBase<CursorDst, CursorSrc, MergePolicy>;
  using typename Base::Traits;
  using typename Base::SrcTraits;
  using typename Base::Transition;
  using typename Base::TrieNode;
  using typename Base::LeafNode;
  using typename Base::page_ptr;
  using typename Base::trie_ptr;
  using typename Base::leaf_ptr;
  using typename Base::offset_e;
  using typename Base::SrcTransition;
  using typename Base::SrcTrieNode;
  using typename Base::SrcLeafNode;
  using typename Base::src_offset_e;
  using typename Base::BigMemory;
  using Base::_has_may_add_leaf;
  using Base::_has_may_add_trie;
  using Base::_has_should_descend_src;
  using Base::dst_cursor;
  using Base::src_cursor;
  using Base::handler;
  using Base::alloc_node;
  using Base::free_node;
  using Base::fill_leaf;
  using Base::resolve_src;
  using Base::resolve_dst;
  using Base::resolve_offset;
  using Base::expand_trie_with_branch;

  _Merger(CursorDst& dest, CursorSrc& src, MergePolicy& handler)
      : Base(dest, src, handler) {}

  void exec() {
    // Pre-init BigMemory to avoid lazy-init race
    dst_cursor.get_bigmemory();
    std::string current_key = src_cursor.current_key;
    current_key.reserve(255);
    merge_node(current_key);
  }

  void merge_node(std::string& current_key) {
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
                                  dst_cursor._root, current_key);
      return;
    }

    assert(dst_cursor.current_key.size() <= current_key.size());
    assert(dst_cursor.current_key ==
           current_key.substr(0, dst_cursor.current_key.size()));

    auto& dst = dst_cursor.stack.back();
    if (dst.is_trie()) {
      merge_trie_node(dst, src, current_key);
    } else {
      if (!dst.leaf()->key_size && src.is_trie() && !dst.is_root()) {
        // dst is a none branch leaf and src is a trie -> we want to merge the
        // trie
        dst_cursor.pop();
        merge_trie_node(dst_cursor.stack.back(), src, current_key);
      } else
        merge_leaf_node(dst, src, current_key);
    }
    current_key.resize(size);
    src_cursor.pop();
  }

  void merge_leaf_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src,
                       std::string& current_key) {
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
                       resolve_offset(new_leaf), current_key);
  }

  void merge_trie_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src,
                       std::string& current_key) {
    auto& dst_trie = dst.trie();
    assert(dst.prefix <= dst_trie->len());
    uint8_t suffix_len = dst_trie->len() - dst.prefix;

    if (suffix_len == 0) {
      merge_into_trie(dst, src, current_key);
    } else {
      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(dst_trie->changed_len(suffix_len));
      new_trie->create(*dst_trie,
                       Slice(&dst_trie->compressed()[dst.prefix], suffix_len));
      free_node(dst_trie);
      assert(new_trie->len() > 0);
      resolve_divergence(dst, src, new_trie->compressed()[0],
                         resolve_offset(new_trie), current_key);
    }
  }

  void resolve_divergence(typename CursorDst::Transition& dst,
                          typename CursorSrc::Transition& src, int key1,
                          offset_t child1, std::string& current_key) {
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
          this->template alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
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

      size_t saved_key_len = current_key.size();
      if constexpr (_has_may_add_trie) {
        // current_key already has the full src prefix (appended by merge_node),
        // so it's the correct key for may_add_trie without further appending.
        if (!handler.may_add_trie(current_key)) {
          return;
        }
      }

      // Collect surviving children into a flat offset array
      // +1 offset so offsets_buf[-1] (NONE) is valid
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int surviving = 0;
      uint8_t upper = 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          surviving++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      current_key.resize(saved_key_len);

      if (!surviving) {
        return;  // nothing from source survived may_add
      }

      // Build suffix trie from survivors
      trie_ptr suffix_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(suffix_prefix.size(), surviving));
      suffix_trie->create(suffix_prefix, offsets_buf, upper);

      // Compute hash for suffix_trie - children have valid hashes from deep copy
      handler.after_trie_merged(suffix_trie, dst_cursor._db);

      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
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

      // Selectively deep copy all other src branches, count survivors inline
      uint8_t upper = (key1 != TrieNode::NONE) ? (1u << TrieNode::ubit(key1)) : 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        if (k == key1) return;
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          branch_count++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      Slice prefix((const char*)src_trie->compressed(), src_trie->len());
      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
      new_trie->create(prefix, offsets_buf, upper);

      dst.trie() = new_trie;
      dst.update_trie_offset();

      // Recursively merge the shared branch (skip if src offset is incomplete)
      if (*src_trie->offset(key1) != 0) {
        src_cursor.current_key.resize(current_key.size());
        src_cursor.push(src_trie->offset(key1));
        dst_cursor.stack.clear();
        merge_node(current_key);
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

      uint8_t upper = (key1 != TrieNode::NONE) ? (1u << TrieNode::ubit(key1)) : 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          branch_count++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      Slice prefix((const char*)src_trie->compressed(), src_trie->len());
      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
      new_trie->create(prefix, offsets_buf, upper);

      dst.trie() = new_trie;
      dst.update_trie_offset();

      // Compute hash for new_trie - all children have valid hashes
      handler.after_trie_merged(new_trie, dst_cursor._db);
    }
  }

  void merge_into_trie(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src,
                       std::string& current_key) {
    using SrcTrieNode = typename CursorSrc::Transition::TrieNode;
    using DstTrieNode = typename CursorDst::Transition::TrieNode;
    assert(dst.is_trie());
    assert(dst.prefix == dst.trie()->len());
    assert(current_key.size() >= dst_cursor.current_key.size());
    assert(current_key.size() < dst_cursor.current_key.size() + 256);
    uint8_t suffix_len = current_key.size() - dst_cursor.current_key.size();
    auto dst_trie = dst.trie();

    if (src.is_leaf()) return merge_leaf_into_trie(dst, src, suffix_len, current_key);

    auto& src_trie = src.trie();
    if (suffix_len) {
      // src prefix is longer -> selectively copy children, then build
      // a correctly-sized suffix trie from survivors only.

      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int surviving = 0;
      uint8_t upper = 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          surviving++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      if (!surviving) return;  // nothing from source survived may_add

      Slice suffix_prefix(
          (const char*)&src_trie->compressed()[src_trie->len() - suffix_len],
          suffix_len);
      trie_ptr suffix_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(suffix_prefix.size(), surviving));
      suffix_trie->create(suffix_prefix, offsets_buf, upper);

      uint16_t loffset;
      trie_ptr new_trie =
          expand_trie_with_branch(dst_trie, suffix_len, &loffset, current_key);

      dst.trie() = new_trie;
      dst.link_idx = loffset;
      *dst.link() = resolve_offset(suffix_trie);
      dst.update_trie_offset();

      // Compute hash for suffix_trie - children have valid hashes from deep copy
      handler.after_trie_merged(suffix_trie, dst_cursor._db);

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
    dst_trie->for_each_branch([&](int k, auto* off) {
      offsets_buf[k] = *off;
      branch_count++;
    });

    // Process src branches: record shared for later merge, selectively copy
    // src-only. Count src-only survivors inline to avoid a second bitmap walk.
    uint8_t upper = dst_trie->_upper;
    src_trie->for_each_branch([&](int k, auto* src_off) {
      if (dst_trie->isset(k)) {
        // Shared branch — merge recursively later (skip incomplete src)
        if (*src_off != 0) {
          shared[shared_count++] = {
              k, const_cast<src_offset_e*>(src_off),
              *dst_trie->offset(k)};
        }
      } else {
        // Src-only — selectively deep copy
        offset_e child_offset;
        if (selective_deep_copy_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          branch_count++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      }
    });

    // Build merged trie
    Slice prefix((const char*)dst_trie->compressed(), dst_trie->len());
    trie_ptr new_trie =
        this->template alloc_node<trie_ptr>(DstTrieNode::size(prefix.size(), branch_count));
    new_trie->create(prefix, offsets_buf, upper);

    dst.trie() = new_trie;
    dst.update_trie_offset();

    // Recursively merge shared branches.
    for (int si = 0; si < shared_count; si++) {
      int k = shared[si].key;
      *new_trie->offset(k) = shared[si].dst_off;

      src_cursor.current_key.resize(current_key.size());
      src_cursor.push(shared[si].src_off);
      dst_cursor.stack.clear();
      merge_node(current_key);
    }

    // Compute hash for new_trie - all children now have valid hashes
    handler.after_trie_merged(new_trie, dst_cursor._db);

    free_node(dst_trie);
  }

  void merge_leaf_into_trie(typename CursorDst::Transition& dst,
                            typename CursorSrc::Transition& src,
                            int suffix_len, std::string& current_key) {
    // src is a leaf -> insert into dst trie (if may_add allows)
    auto& src_leaf = src.leaf();

    if (!handler.may_add_leaf(current_key, src_leaf->value(),
                              src_leaf->is_big()))
      return;  // rejected

    uint16_t loffset;
    trie_ptr new_trie =
        expand_trie_with_branch(dst.trie(), suffix_len, &loffset, current_key);

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
                                   offset_e* parent_link,
                                   std::string& current_key) {
    if (*src_offset == 0) {
      *parent_link = offset_e();
      return false;
    }

    if (src_offset->type() == LEAF) {
      return selective_deep_copy_leaf(src_offset, parent_link, current_key);
    } else {
      return selective_deep_copy_trie(src_offset, parent_link, current_key);
    }
  }

  /**
   * @brief Selectively deep copy a single leaf, consulting may_add.
   *
   * Reconstructs the full key from current_key + the leaf's own suffix,
   * calls handler.may_add_leaf(), and only copies the leaf if allowed.
   */
  bool selective_deep_copy_leaf(const src_offset_e* src_offset,
                                offset_e* parent_link,
                                std::string& current_key) {
    auto src_leaf = this->template resolve_src<SrcLeafNode>(src_offset);

    if constexpr (_has_may_add_leaf) {
      size_t saved = current_key.size();
      current_key.append((const char*)src_leaf->data, src_leaf->key_size);

      bool accepted = handler.may_add_leaf(current_key, src_leaf->value(),
                                             src_leaf->is_big());
      current_key.resize(saved);

      if (!accepted) {
        *parent_link = offset_e();
        return false;
      }
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
                                offset_e* parent_link,
                                std::string& current_key) {
    auto src_trie = this->template resolve_src<SrcTrieNode>(src_offset);

    size_t saved = current_key.size();

    // Append compressed prefix when may_add_leaf or may_add_trie needs
    // the full reconstructed key for filtering decisions.
    if constexpr (_has_may_add_trie || _has_may_add_leaf) {
      current_key.append((const char*)src_trie->compressed(), src_trie->len());
    }

    if constexpr (_has_may_add_trie) {
      // Early-out: let the policy reject the entire subtree by prefix
      if (!handler.may_add_trie(current_key)) {
        current_key.resize(saved);
        *parent_link = offset_e();
        return false;
      }
    }

    // ── Pass 1: recurse children, collect survivors ──────────────────
    offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
    offset_e* offsets_buf = &offsets_raw[1];

    int surviving = 0;
    uint8_t upper = 0;
    src_trie->for_each_branch([&](int k, auto* src_off) {
      offset_e child_offset;
      if (selective_deep_copy_subtree(src_off, &child_offset, current_key)) {
        offsets_buf[k] = child_offset;
        surviving++;
        if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
      }
    });

    current_key.resize(saved);

    if (surviving == 0) {
      *parent_link = offset_e();
      return false;
    }

    // ── Pass 2: build destination trie with only survivors ───────────
    Slice prefix((const char*)src_trie->compressed(), src_trie->len());
    uint16_t new_size = TrieNode::size(prefix.size(), surviving);
    trie_ptr dst_trie = this->template alloc_node<trie_ptr>(new_size);
    dst_trie->create(prefix, offsets_buf, upper);

    *parent_link = resolve_offset(dst_trie);
    return true;
  }
};

/**
 * @brief Move-merger for same-storage merges (zero-copy adopt semantics)
 *
 * Unlike _Merger which deep-copies src into dst, _MoveMerger directly links
 * (adopts) writer-allocated src nodes into the dst trie.  Src nodes that are
 * replaced during merge are freed inline via handler.free_src().
 *
 * should_descend_src controls which src subtrees to process:
 * - Returns false for snapshot nodes (txn_id <= base) — already in committed.
 * - Returns true for writer-allocated nodes — these are adopted or freed.
 *
 * Requires CursorDst and CursorSrc to share the same storage (same-process).
 */
template <typename CursorDst, typename CursorSrc, typename MergePolicy>
struct _MoveMerger : _MergerBase<CursorDst, CursorSrc, MergePolicy> {
  using Base = _MergerBase<CursorDst, CursorSrc, MergePolicy>;
  using typename Base::Traits;
  using typename Base::SrcTraits;
  using typename Base::Transition;
  using typename Base::TrieNode;
  using typename Base::LeafNode;
  using typename Base::page_ptr;
  using typename Base::trie_ptr;
  using typename Base::leaf_ptr;
  using typename Base::offset_e;
  using typename Base::SrcTransition;
  using typename Base::SrcTrieNode;
  using typename Base::SrcLeafNode;
  using typename Base::src_offset_e;
  using typename Base::BigMemory;
  using Base::_has_may_add_leaf;
  using Base::_has_may_add_trie;
  using Base::_has_should_descend_src;
  using Base::dst_cursor;
  using Base::src_cursor;
  using Base::handler;
  using Base::alloc_node;
  using Base::free_node;
  using Base::fill_leaf;
  using Base::resolve_src;
  using Base::resolve_dst;
  using Base::resolve_offset;
  using Base::expand_trie_with_branch;

  _MoveMerger(CursorDst& dest, CursorSrc& src, MergePolicy& handler)
      : Base(dest, src, handler) {}

  // Free a src node that was orphaned (not adopted into dst).
  template <typename NodePtr>
  void free_src_node(NodePtr& node) {
    handler.free_src(node);
  }

  void exec() {
    std::string current_key = src_cursor.current_key;
    current_key.reserve(255);
    merge_node(current_key);
  }

  void merge_node(std::string& current_key) {
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

    // Handle empty destination — adopt entire source tree
    if (!dst_cursor.stack.size) {
      current_key.resize(size);
      adopt_subtree(src_cursor.stack.front().offset,
                    dst_cursor._root, current_key);
      return;
    }

    assert(dst_cursor.current_key.size() <= current_key.size());
    assert(dst_cursor.current_key ==
           current_key.substr(0, dst_cursor.current_key.size()));

    auto& dst = dst_cursor.stack.back();
    if (dst.is_trie()) {
      merge_trie_node(dst, src, current_key);
    } else {
      if (!dst.leaf()->key_size && src.is_trie() && !dst.is_root()) {
        dst_cursor.pop();
        merge_trie_node(dst_cursor.stack.back(), src, current_key);
      } else
        merge_leaf_node(dst, src, current_key);
    }
    current_key.resize(size);
    src_cursor.pop();
  }

  void merge_leaf_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src,
                       std::string& current_key) {
    auto& dst_leaf = dst.leaf();
    assert(dst.prefix <= dst_leaf->key_size);

    if (src.is_leaf() && dst.cmp == 0) {
      auto& src_leaf = src.leaf();
      if (!handler.may_overwrite(current_key, dst_leaf->value(),
                                 src_leaf->value(), dst_leaf->is_big(),
                                 src_leaf->is_big())) {
        // Rejected — src leaf not adopted, free it
        free_src_node(src_leaf);
        return;
      }
      // Adopt src leaf directly (it's already in storage)
      *dst.offset = src_cursor._db->resolve(src_leaf);
      free_node(dst_leaf);
      return;
    }

    // Not an exact match — need to split
    leaf_ptr new_leaf = dst_leaf;
    if (dst.prefix) {
      new_leaf = _Inserter(&dst, 0).copy_reduced_leaf(dst.prefix, dst_leaf);
      free_node(dst_leaf);
    }

    resolve_divergence(dst, src,
                       new_leaf->key_size ? new_leaf->data[0] : TrieNode::NONE,
                       resolve_offset(new_leaf), current_key);
  }

  void merge_trie_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src,
                       std::string& current_key) {
    auto& dst_trie = dst.trie();
    assert(dst.prefix <= dst_trie->len());
    uint8_t suffix_len = dst_trie->len() - dst.prefix;

    if (suffix_len == 0) {
      merge_into_trie(dst, src, current_key);
    } else {
      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(dst_trie->changed_len(suffix_len));
      new_trie->create(*dst_trie,
                       Slice(&dst_trie->compressed()[dst.prefix], suffix_len));
      free_node(dst_trie);
      assert(new_trie->len() > 0);
      resolve_divergence(dst, src, new_trie->compressed()[0],
                         resolve_offset(new_trie), current_key);
    }
  }

  void resolve_divergence(typename CursorDst::Transition& dst,
                          typename CursorSrc::Transition& src, int key1,
                          offset_t child1, std::string& current_key) {
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
      if (!handler.may_add_leaf(current_key, src_value, src_leaf->is_big())) {
        free_src_node(src_leaf);
        return;
      }

      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
      auto idxs = new_trie->create(
          Slice(current_key.data() + dst.keypos, dst.prefix), key1, key);
      new_trie->array()[idxs.first] = child1;
      dst.trie() = new_trie;
      dst.link_idx = idxs.second;
      dst.update_trie_offset();

      // Src leaf needs a reduced copy if split_pos is not at 0
      if (src_split_pos > 0) {
        leaf_ptr new_leaf = fill_leaf(
            Slice(&src_leaf->data[src_split_pos], suffix_len), *src_leaf);
        *dst.link() = resolve_offset(new_leaf);
        free_src_node(src_leaf);
      } else {
        // Adopt entire src leaf
        *dst.link() = src_cursor._db->resolve(src_leaf);
      }

      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    if (split_pos < current_key.size()) {
      assert(src.is_trie());
      int key = current_key[split_pos];

      auto& src_trie = src.trie();
      assert(src_trie->len() >= src_split_pos);
      uint8_t suffix_len = src_trie->len() - src_split_pos;
      Slice suffix_prefix((const char*)&src_trie->compressed()[src_split_pos],
                          suffix_len);

      size_t saved_key_len = current_key.size();
      if constexpr (_has_may_add_trie) {
        if (!handler.may_add_trie(current_key)) {
          free_src_subtree(src_trie);
          return;
        }
      }

      // Collect surviving children — adopt directly
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int surviving = 0;
      uint8_t upper = 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        offset_e child_offset;
        if (adopt_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          surviving++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      current_key.resize(saved_key_len);

      if (!surviving) {
        free_src_node(src_trie);
        return;
      }

      trie_ptr suffix_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(suffix_prefix.size(), surviving));
      suffix_trie->create(suffix_prefix, offsets_buf, upper);
      // Original src trie node is replaced by new suffix_trie — free it
      free_src_node(src_trie);

      handler.after_trie_merged(suffix_trie, dst_cursor._db);

      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
      auto idxs = new_trie->create(
          Slice(current_key.data() + dst.keypos, dst.prefix), key1, key);
      new_trie->array()[idxs.first] = child1;
      dst.trie() = new_trie;
      dst.link_idx = idxs.second;
      dst.update_trie_offset();
      *dst.link() = resolve_offset(suffix_trie);

      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    assert(src.is_trie());
    assert(split_pos == current_key.size());

    auto src_trie = src.trie();
    if (src_trie->isset(key1)) {
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 1;

      offsets_buf[key1] = child1;

      uint8_t upper = (key1 != TrieNode::NONE) ? (1u << TrieNode::ubit(key1)) : 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        if (k == key1) return;
        offset_e child_offset;
        if (adopt_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          branch_count++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      Slice prefix((const char*)src_trie->compressed(), src_trie->len());
      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
      new_trie->create(prefix, offsets_buf, upper);

      dst.trie() = new_trie;
      dst.update_trie_offset();
      // Free the src trie node — its children were adopted or recursed
      free_src_node(src_trie);

      if (*src_trie->offset(key1) != 0) {
        src_cursor.current_key.resize(current_key.size());
        src_cursor.push(src_trie->offset(key1));
        dst_cursor.stack.clear();
        merge_node(current_key);
      }

      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    {
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 1;

      offsets_buf[key1] = child1;

      uint8_t upper = (key1 != TrieNode::NONE) ? (1u << TrieNode::ubit(key1)) : 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        offset_e child_offset;
        if (adopt_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          branch_count++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      Slice prefix((const char*)src_trie->compressed(), src_trie->len());
      trie_ptr new_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(prefix.size(), branch_count));
      new_trie->create(prefix, offsets_buf, upper);

      dst.trie() = new_trie;
      dst.update_trie_offset();
      free_src_node(src_trie);

      handler.after_trie_merged(new_trie, dst_cursor._db);
    }
  }

  void merge_into_trie(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src,
                       std::string& current_key) {
    using SrcTrieNode = typename CursorSrc::Transition::TrieNode;
    using DstTrieNode = typename CursorDst::Transition::TrieNode;
    assert(dst.is_trie());
    assert(dst.prefix == dst.trie()->len());
    assert(current_key.size() >= dst_cursor.current_key.size());
    assert(current_key.size() < dst_cursor.current_key.size() + 256);
    uint8_t suffix_len = current_key.size() - dst_cursor.current_key.size();
    auto dst_trie = dst.trie();

    if (src.is_leaf()) return merge_leaf_into_trie(dst, src, suffix_len, current_key);

    auto& src_trie = src.trie();
    if (suffix_len) {
      offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
      offset_e* offsets_buf = &offsets_raw[1];
      int surviving = 0;
      uint8_t upper = 0;
      src_trie->for_each_branch([&](int k, auto* src_off) {
        offset_e child_offset;
        if (adopt_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          surviving++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      });

      if (!surviving) {
        free_src_node(src_trie);
        return;
      }

      Slice suffix_prefix(
          (const char*)&src_trie->compressed()[src_trie->len() - suffix_len],
          suffix_len);
      trie_ptr suffix_trie =
          this->template alloc_node<trie_ptr>(TrieNode::size(suffix_prefix.size(), surviving));
      suffix_trie->create(suffix_prefix, offsets_buf, upper);
      free_src_node(src_trie);

      uint16_t loffset;
      trie_ptr new_trie =
          expand_trie_with_branch(dst_trie, suffix_len, &loffset, current_key);

      dst.trie() = new_trie;
      dst.link_idx = loffset;
      *dst.link() = resolve_offset(suffix_trie);
      dst.update_trie_offset();

      handler.after_trie_merged(suffix_trie, dst_cursor._db);
      handler.after_trie_merged(new_trie, dst_cursor._db);
      return;
    }

    // Merge children of both tries
    offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
    offset_e* offsets_buf = &offsets_raw[1];

    struct SharedBranch {
      int key;
      src_offset_e* src_off;
      offset_e dst_off;
    };
    SharedBranch shared[257];
    int shared_count = 0;
    int branch_count = 0;

    dst_trie->for_each_branch([&](int k, auto* off) {
      offsets_buf[k] = *off;
      branch_count++;
    });

    uint8_t upper = dst_trie->_upper;
    src_trie->for_each_branch([&](int k, auto* src_off) {
      if (dst_trie->isset(k)) {
        if (*src_off != 0) {
          shared[shared_count++] = {
              k, const_cast<src_offset_e*>(src_off),
              *dst_trie->offset(k)};
        }
      } else {
        offset_e child_offset;
        if (adopt_subtree(src_off, &child_offset, current_key)) {
          offsets_buf[k] = child_offset;
          branch_count++;
          if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        }
      }
    });

    Slice prefix((const char*)dst_trie->compressed(), dst_trie->len());
    trie_ptr new_trie =
        this->template alloc_node<trie_ptr>(DstTrieNode::size(prefix.size(), branch_count));
    new_trie->create(prefix, offsets_buf, upper);

    dst.trie() = new_trie;
    dst.update_trie_offset();
    // Free src trie node — children adopted or queued for recursive merge
    free_src_node(src_trie);

    for (int si = 0; si < shared_count; si++) {
      int k = shared[si].key;
      *new_trie->offset(k) = shared[si].dst_off;

      src_cursor.current_key.resize(current_key.size());
      src_cursor.push(shared[si].src_off);
      dst_cursor.stack.clear();
      merge_node(current_key);
    }

    handler.after_trie_merged(new_trie, dst_cursor._db);
    free_node(dst_trie);
  }

  void merge_leaf_into_trie(typename CursorDst::Transition& dst,
                            typename CursorSrc::Transition& src,
                            int suffix_len, std::string& current_key) {
    auto& src_leaf = src.leaf();

    if (!handler.may_add_leaf(current_key, src_leaf->value(),
                              src_leaf->is_big())) {
      free_src_node(src_leaf);
      return;
    }

    uint16_t loffset;
    trie_ptr new_trie =
        expand_trie_with_branch(dst.trie(), suffix_len, &loffset, current_key);

    dst.trie() = new_trie;
    dst.link_idx = loffset;

    // Adopt src leaf — if suffix_len matches, link directly
    assert(src_leaf->key_size >= suffix_len);
    uint8_t split_pos = src_leaf->key_size - suffix_len;
    if (split_pos > 0) {
      // Need a reduced leaf copy (key was split)
      leaf_ptr new_leaf =
          fill_leaf(Slice(&src_leaf->data[split_pos], suffix_len), *src_leaf);
      *dst.link() = resolve_offset(new_leaf);
      free_src_node(src_leaf);
    } else {
      // Adopt directly
      *dst.link() = src_cursor._db->resolve(src_leaf);
    }
    dst.update_trie_offset();

    handler.after_trie_merged(new_trie, dst_cursor._db);
  }

  // ── Adopt subtree (move semantics) ────────────────────────────────

  /**
   * @brief Adopt a src subtree into dst, using should_descend_src filtering.
   *
   * For nodes where should_descend_src returns false: they belong to the
   * snapshot and are already in committed — skip entirely (don't adopt, don't free).
   *
   * For writer-allocated nodes: adopt directly (link offset into parent_link).
   * If a trie node's children need partial handling, recurse and build a new
   * trie from the mix of adopted and skipped children.
   */
  bool adopt_subtree(const src_offset_e* src_offset,
                     offset_e* parent_link,
                     std::string& current_key) {
    if (*src_offset == 0) {
      *parent_link = offset_e();
      return false;
    }

    if constexpr (_has_should_descend_src) {
      // Resolve page header to check txn_id
      using PageHeader = typename SrcTraits::PageHeader;
      auto page = src_cursor._db->template resolve<PageHeader>(src_offset);
      if (!handler.should_descend_src(page)) {
        // Snapshot node — skip entirely (already in committed)
        *parent_link = offset_e();
        return false;
      }
    }

    if (src_offset->type() == LEAF) {
      return adopt_leaf(src_offset, parent_link, current_key);
    } else {
      return adopt_trie(src_offset, parent_link, current_key);
    }
  }

  bool adopt_leaf(const src_offset_e* src_offset,
                  offset_e* parent_link,
                  std::string& current_key) {
    auto src_leaf = this->template resolve_src<SrcLeafNode>(src_offset);

    if constexpr (_has_may_add_leaf) {
      size_t saved = current_key.size();
      current_key.append((const char*)src_leaf->data, src_leaf->key_size);
      bool accepted = handler.may_add_leaf(current_key, src_leaf->value(),
                                           src_leaf->is_big());
      current_key.resize(saved);
      if (!accepted) {
        free_src_node(src_leaf);
        *parent_link = offset_e();
        return false;
      }
    }

    // Adopt: link src leaf directly into dst
    *parent_link = *src_offset;
    return true;
  }

  bool adopt_trie(const src_offset_e* src_offset,
                  offset_e* parent_link,
                  std::string& current_key) {
    auto src_trie = this->template resolve_src<SrcTrieNode>(src_offset);

    size_t saved = current_key.size();
    if constexpr (_has_may_add_trie || _has_may_add_leaf) {
      current_key.append((const char*)src_trie->compressed(), src_trie->len());
    }

    if constexpr (_has_may_add_trie) {
      if (!handler.may_add_trie(current_key)) {
        current_key.resize(saved);
        free_src_subtree(src_trie);
        *parent_link = offset_e();
        return false;
      }
    }

    // Check if ALL children can be adopted as-is (fast path)
    // If should_descend_src is default (always true), all children are
    // writer-allocated, so the whole trie can be adopted directly.
    if constexpr (!_has_should_descend_src && !_has_may_add_leaf && !_has_may_add_trie) {
      current_key.resize(saved);
      *parent_link = *src_offset;
      return true;
    }

    // Need to check each child individually
    offset_e offsets_raw[TrieNode::MAX_BRANCH_COUNT] = {};
    offset_e* offsets_buf = &offsets_raw[1];

    int surviving = 0;
    uint8_t upper = 0;
    bool all_adopted_in_place = true;

    src_trie->for_each_branch([&](int k, auto* src_off) {
      offset_e child_offset;
      if (adopt_subtree(src_off, &child_offset, current_key)) {
        offsets_buf[k] = child_offset;
        surviving++;
        if (k != SrcTrieNode::NONE) upper |= (1u << TrieNode::ubit(k));
        // Check if child was adopted in-place (same offset)
        if (child_offset != *src_off) all_adopted_in_place = false;
      } else {
        all_adopted_in_place = false;
      }
    });

    current_key.resize(saved);

    if (surviving == 0) {
      free_src_node(src_trie);
      *parent_link = offset_e();
      return false;
    }

    // If all children survived and were adopted in-place, adopt the whole trie
    if (all_adopted_in_place && surviving == (int)src_trie->count()) {
      *parent_link = *src_offset;
      return true;
    }

    // Build new trie from survivors, free old src trie node
    Slice prefix((const char*)src_trie->compressed(), src_trie->len());
    uint16_t new_size = TrieNode::size(prefix.size(), surviving);
    trie_ptr dst_trie = this->template alloc_node<trie_ptr>(new_size);
    dst_trie->create(prefix, offsets_buf, upper);
    free_src_node(src_trie);

    *parent_link = resolve_offset(dst_trie);
    return true;
  }

  // Recursively free an entire src subtree (when may_add rejects it).
  template <typename SrcNodePtr>
  void free_src_subtree(SrcNodePtr& node) {
    // For a trie, recurse into children first
    if constexpr (std::is_same_v<std::decay_t<SrcNodePtr>,
                                 typename SrcTransition::trie_ptr>) {
      node->for_each_branch([&](int k, auto* src_off) {
        if (*src_off == 0) return;
        if (src_off->type() == LEAF) {
          auto child = this->template resolve_src<SrcLeafNode>(src_off);
          free_src_node(child);
        } else {
          auto child = this->template resolve_src<SrcTrieNode>(src_off);
          free_src_subtree(child);
        }
      });
    }
    free_src_node(node);
  }
};

}  // namespace leaves

#endif  // _LEAVES_MERGER_HPP
