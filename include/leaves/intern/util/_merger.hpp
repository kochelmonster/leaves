#ifndef _LEAVES_MERGER_HPP
#define _LEAVES_MERGER_HPP

#include "../core/_bits.hpp"
#include "../db/_cursor.hpp"
#include "../core/_node.hpp"

namespace leaves {

// _NodeIterator is now defined in _cursor.hpp

/**
 * @brief Merger for combining two tries
 *
 * Merges trie A into trie B,
 */
template <typename CursorDst, typename CursorSrc, typename OverwriteHandler>
struct _Merger {
  typedef _Merger<CursorDst, CursorSrc, OverwriteHandler> Merger;
  using Traits = typename CursorDst::Traits;
  using SrcTraits = typename CursorSrc::Traits;
  using Transition = typename CursorDst::Transition;
  using SrcTransition = typename CursorSrc::Transition;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using SrcTrieNode = typename SrcTransition::TrieNode;
  using page_ptr = typename Transition::page_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;
  using src_offset_e = typename SrcTransition::offset_e;
  using SrcLeafNode = typename CursorSrc::Transition::LeafNode;
  using BigMemory = typename CursorDst::BigMemory;
  using BigValue = typename BigMemory::BigValue;

  CursorDst& dst_cursor;
  CursorSrc& src_cursor;
  OverwriteHandler& handler;
  std::string current_key;

  _Merger(CursorDst& dest, CursorSrc& src, OverwriteHandler& handler)
      : dst_cursor(dest), src_cursor(src), handler(handler) {}

  // Helper methods for memory management
  page_ptr alloc(uint16_t size) { return dst_cursor.alloc(size); }

  // Allocate node with PageHeader prefix, return pointer to node
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc(sizeof(PageHeader) + node_size);
    return NodePtr((char*)page + sizeof(PageHeader));
  }

  // Free node by computing PageHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    using PageHeader = typename Traits::PageHeader;
    static_assert(
        !std::is_same_v<NodePtr, page_ptr>,
        "free_node must be called with node pointers, not page pointers");

    if constexpr (NodePtr::type == LEAF) {
      if (node->is_big()) handler.free_big(node);
    }

    page_ptr page((char*)node - sizeof(PageHeader));
    dst_cursor._db->free(page);
  }

  leaf_ptr fill_leaf(const Slice& key, SrcLeafNode& src_leaf) {
    Slice src_value;
    if (src_leaf.is_big()) {
      src_value = handler.migrate_big_value(src_leaf, *src_cursor._db);
    } else {
      src_value = src_leaf.value();
    }

    uint64_t vsize = src_value.size();
    uint16_t msize =
        BigMemory::template modify_size<LeafNode>(key.size(), vsize);
    leaf_ptr leaf = alloc_node<leaf_ptr>(LeafNode::size(key.size(), msize));
    leaf->set(key, src_value.size());
    if (msize != vsize) {
      leaf->set_big();
      BigValue* bvalue = (BigValue*)leaf->vdata();
      dst_cursor.get_bigmemory().alloc(vsize, bvalue);
      optimized_memcpy((char*)bvalue->data(dst_cursor._db), src_value.data(), vsize);
    } else
      memcpy(leaf->vdata(), src_value.data(), src_value.size());

    return leaf;
  }

  template <typename T>
  typename SrcTraits::template Pointer<T> resolve_src(const src_offset_e* offset_ptr) {
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
      // Get root from source - it's the first element in the stack
      auto new_root = deep_copy_subtree(src_cursor.stack.front().offset);
      *dst_cursor._root = new_root;
      return;
    }

    auto dst_key = dst_cursor.current_key;
    auto src_key = current_key;

    assert(dst_key.size() <= src_key.size());
    assert(dst_key == src_key.substr(0, dst_key.size()));

    auto& dst = dst_cursor.stack.back();
    if (dst.is_trie()) {
      merge_trie_node(dst, src);
    } else {
      merge_leaf_node(dst, src);
    }
    current_key.resize(size);
    src_cursor.pop();
  }

  void merge_leaf_node(typename CursorDst::Transition& dst,
                       typename CursorSrc::Transition& src) {
    auto& dst_leaf = dst.leaf();
    // check if dst_leaf must split
    assert(dst.prefix <= dst_leaf->key_size);
    leaf_ptr new_leaf =
        _Inserter(&dst, 0).copy_reduced_leaf(dst.prefix, dst_leaf);
    free_node(dst_leaf);
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
      resolve_divergence(dst, src, new_trie->compressed()[0], resolve_offset(new_trie));
    }
  }

  void resolve_divergence(typename CursorDst::Transition& dst,
                          typename CursorSrc::Transition& src, int key1,
                          offset_t child1) {
    uint16_t split_pos = dst.keypos + dst.prefix;
    assert(split_pos >= src.keypos);
    uint16_t src_split_pos = split_pos - src.keypos;
    assert(src_split_pos < 256);

    int key = split_pos < current_key.size() ? current_key[split_pos]
                                             : TrieNode::NONE;

    if (key != TrieNode::NONE || src.is_leaf()) {
      if (key == TrieNode::NONE && key1 == TrieNode::NONE) {
        assert(src.is_leaf());
        assert(child1.type() == LEAF);
        auto dst_slice = dst.leaf()->value();
        auto src_slice = src.leaf()->value();
        if (handler.check_overwrite(current_key, dst_slice, src_slice)) {
          assert(key1 == TrieNode::NONE);
          leaf_ptr old_leaf = resolve_dst<LeafNode>(&child1);
          _Inserter(&dst, src_slice.size()).change_leaf();
          auto& new_leaf = dst.leaf();
          memcpy(new_leaf->vdata(), src_slice.data(), src_slice.size());
          free_node(old_leaf);
        }
        return;
      }

      // A new trie with two branches for the old dst and the new src
      trie_ptr new_trie =
          alloc_node<trie_ptr>(TrieNode::size(src_split_pos, key1, key));
      auto idxs =
          new_trie->create(Slice(current_key.data() + dst.keypos, dst.prefix),
                           key1, key);
      new_trie->array()[idxs.first] = child1;
      dst.trie() = new_trie;
      dst.link_idx = idxs.second;
      dst.update_trie_offset();

      if (src.is_leaf()) {
        auto& src_leaf = src.leaf();
        assert(src_leaf->key_size >= src_split_pos);
        uint8_t suffix_len = src_leaf->key_size - src_split_pos;
        leaf_ptr new_leaf = fill_leaf(
            Slice(&src_leaf->data[src_split_pos], suffix_len), *src_leaf);
        *dst.link() = resolve_offset(new_leaf);
        return;
      }
      auto& src_trie = src.trie();
      assert(src_trie->len() >= src_split_pos);
      uint8_t suffix_len = src_trie->len() - src_split_pos;

      trie_ptr suffix_trie =
          alloc_node<trie_ptr>(src_trie->changed_len(suffix_len));
      suffix_trie->create(
          *src_trie, Slice(&src_trie->compressed()[src_split_pos], suffix_len));
      *dst.link() = resolve_offset(suffix_trie);

      auto dst_offset = suffix_trie->array();
      auto src_offset = src_trie->array();
      for (int i = 0, scount = src_trie->count(); i < scount; i++) {
        dst_offset[i] = deep_copy_subtree(&src_offset[i]);
      }
      return;
    }

    assert(src.is_trie());
    assert(split_pos == current_key.size());
    // the whole prefix of the src == dst.prefix

    auto src_trie = src.trie();
    if (src_trie->isset(key1)) {
      // copy src trie to dst and move down the branch src shares with dst
      trie_ptr new_trie = alloc_node<trie_ptr>(src_trie->size());
      copy(*new_trie, *src_trie);
      dst.trie() = new_trie;
      dst.update_trie_offset();

      auto dst_p = new_trie->offset(key1);
      auto dst_offset = new_trie->array();
      auto src_offset = src_trie->array();
      for (int i = 0, count = new_trie->count(); i < count; i++) {
        if (&dst_offset[i] == dst_p) {
          dst_offset[i] = child1;
          src_cursor.push(&src_offset[i]);
          dst_cursor.stack.clear();
          merge_node();
          continue;
        }
        dst_offset[i] = deep_copy_subtree(&src_offset[i]);
      }
      return;
    }

    trie_ptr new_trie = alloc(src_trie->increment_size(key1));
    dst.trie() = new_trie;
    dst.link_idx = new_trie->create(*src_trie, key1);
    dst.update_trie_offset();
    *dst.link() = child1;
    for (int key = new_trie->first(); key != TrieNode::OUT_OF_RANGE;
         key = new_trie->next(key)) {
      if (key == key1) continue;
      *new_trie->offset(key) = deep_copy_subtree(src_trie->offset(key));
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

    _Inserter inserter(&dst, 0);
    if (src.is_leaf()) return merge_leaf_into_trie(dst, src, suffix_len);

    auto& src_trie = src.trie();
    if (suffix_len) {
      // src prefix is longer -> split src_trie and insert the suffix part
      uint16_t loffset;
      trie_ptr new_trie = expand_trie_with_branch(dst_trie, suffix_len, &loffset);
      trie_ptr suffix_trie = inserter.alloc(src_trie->changed_len(suffix_len));
      suffix_trie->create(
          *src_trie,
          Slice(&src_trie->compressed()[src_trie->len() - suffix_len],
                suffix_len));

      dst.trie() = new_trie;
      dst.link_idx = loffset;
      *dst.link() = resolve_offset(suffix_trie);
      dst.update_trie_offset();

      auto dst_offset = suffix_trie->array();
      auto src_offset = src_trie->array();
      for (int i = 0, scount = src_trie->count(); i < scount; i++) {
        dst_offset[i] = deep_copy_subtree(&src_offset[i]);
      }
      return;
    }

    // Merge the children of both tries
    trie_ptr new_trie = alloc_node<trie_ptr>(DstTrieNode::size(
        dst_trie->len(), dst_trie->count() + src_trie->count()));

    // merge src_trie into dst_trie
    dst.trie() = new_trie;
    dst.update_trie_offset();

    const typename DstTrieNode::offset_e* dst_poffset[257];
    const typename SrcTrieNode::offset_e* src_poffset[257];
    new_trie->create(*dst_trie, *src_trie, dst_poffset, src_poffset);
    auto array = new_trie->array();
    for (int i = 0, count = new_trie->count(); i < count; i++) {
      if (dst_poffset[i] && src_poffset[i]) {
        // walk down and merge - both tries have this branch
        array[i] = *dst_poffset[i];
        src_cursor.push(const_cast<src_offset_e*>(src_poffset[i]));
        dst_cursor.stack.clear();
        merge_node();
        continue;
      }
      if (dst_poffset[i]) {
        assert(!src_poffset[i]);
        array[i] = *dst_poffset[i];
        continue;
      }
      assert(src_poffset[i]);
      assert(!dst_poffset[i]);
      array[i] = deep_copy_subtree(src_poffset[i]);
    }

    free_node(dst_trie);
  }

  void merge_leaf_into_trie(typename CursorDst::Transition& dst,
                            typename CursorSrc::Transition& src,
                            int suffix_len) {
    // src is a leaf -> insert into dst trie
    uint16_t loffset;
    trie_ptr new_trie = expand_trie_with_branch(dst.trie(), suffix_len, &loffset);

    auto& src_leaf = src.leaf();

    assert(src_leaf->key_size >= suffix_len);
    uint8_t split_pos = src_leaf->key_size - suffix_len;
    leaf_ptr new_leaf =
        fill_leaf(Slice(&src_leaf->data[split_pos], suffix_len), *src_leaf);

    dst.trie() = new_trie;
    dst.link_idx = loffset;
    *dst.link() = resolve_offset(new_leaf);
    dst.update_trie_offset();
  }

  typename CursorDst::Transition::trie_ptr expand_trie_with_branch(
      typename CursorDst::Transition::trie_ptr& dst_trie, int suffix_len,
      uint16_t* loffset) {
    int branch_key = suffix_len ? current_key[dst_cursor.current_key.size()]
                                : TrieNode::NONE;
    assert(!(branch_key == TrieNode::NONE ? dst_trie->has_none()
                                          : dst_trie->isset(branch_key)));
    // otherwise find would have walked down

    trie_ptr new_trie = alloc(dst_trie->increment_size(branch_key));
    *loffset = new_trie->create(*dst_trie, branch_key);
    free_node(dst_trie);
    return new_trie;
  }

  /**
   * @brief Deep copy entire subtree from source to destination
   */
  offset_t deep_copy_subtree(const src_offset_e* src_offset) {
    if (src_offset->type() == LEAF) return deep_copy_leaf(src_offset);
    return deep_copy_trie(src_offset);
  }

  /**
   * @brief Deep copy a leaf node from source to destination
   */
  offset_t deep_copy_leaf(const src_offset_e* src_offset) {
    auto src_leaf = resolve_src<SrcLeafNode>(src_offset);
    leaf_ptr new_leaf = fill_leaf(Slice(src_leaf->key()), *src_leaf);
    return resolve_offset(new_leaf);
  }

  /**
   * @brief Deep copy a trie node and its subtree from source to destination
   */
  offset_t deep_copy_trie(const src_offset_e* src_offset) {
    auto src_trie = resolve_src<SrcTrieNode>(src_offset);
    uint16_t trie_size = src_trie->size();
    trie_ptr dst_trie = alloc_node<trie_ptr>(trie_size);
    memcpy((char*)dst_trie, (char*)src_trie, src_trie->array_start());

    // Recursively copy all children and update offsets
    auto dst_array = dst_trie->array();
    auto src_array = src_trie->array();
    for (int i = 0, count = dst_trie->count(); i < count; i++) {
      dst_array[i] = deep_copy_subtree(&src_array[i]);
    }
    return resolve_offset(dst_trie);
  }
};

}  // namespace leaves

#endif  // _LEAVES_MERGER_HPP
