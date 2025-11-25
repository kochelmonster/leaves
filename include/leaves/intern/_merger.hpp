#ifndef _LEAVES_MERGER_HPP
#define _LEAVES_MERGER_HPP

#include "_bits.hpp"
#include "_cursor.hpp"
#include "_node.hpp"

namespace leaves {

// _NodeIterator is now defined in _cursor.hpp

/**
 * @brief Merger for combining two tries
 *
 * Merges trie A into trie B,
 */
template <typename CursorDst, typename CursorSrc, typename Handler>
struct _Merger {
  typedef _Merger<CursorDst, CursorSrc, Handler> Merger;
  using Traits = typename CursorDst::Traits;
  using Transition = typename CursorDst::Transition;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using block_ptr = typename Transition::block_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  CursorDst& dst_cursor;
  CursorSrc& src_cursor;
  Handler& handler;
  std::string current_key;

  _Merger(CursorDst& dest, CursorSrc& src, Handler& handler)
      : dst_cursor(dest), src_cursor(src), handler(handler) {}

  // Helper methods for memory management
  block_ptr alloc(uint16_t size) { return dst_cursor._db->alloc(size); }

  leaf_ptr fill_leaf(const Slice& key, const Slice& src_value) {
    leaf_ptr leaf = alloc(LeafNode::size(key.size(), src_value.size()));
    auto bv = leaf->set(key, src_value.size());
    if (bv) {
      bv->offset = dst_cursor._db->alloc_big(bv->size()).offset();
      block_ptr ptr = resolve_dst(bv->offset);
      memcpy((char*)ptr, src_value.data(), src_value.size());
    } else
      memcpy(leaf->vdata(), src_value.data(), src_value.size());

    return leaf;
  }

  template <typename T>
  void free(T& block) {
    if constexpr (T::type == LEAF) {
      if (block->is_big()) {
        auto bv = block->big();
        dst_cursor._db->free_big(bv->offset, bv->size());
      }
    }
    dst_cursor._db->free(block);
  }

  block_ptr resolve_src(offset_t offset) {
    return src_cursor._db->resolve(offset);
  }

  block_ptr resolve_dst(offset_t offset) {
    return dst_cursor._db->resolve(offset);
  }

  template <typename T>
  offset_t resolve_offset(T ptr) {
    return dst_cursor._db->resolve(ptr);
  }

  void exec() {
    // put root in the stack
    if (src_cursor._prepare_move()) return;
    current_key.clear();

    // now start merge recursively down the source trie
    merge_node();
    dst_cursor.stack.clear();  // reset dst_cursor
  }

  void merge_node() {
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
      auto root_offset = src_cursor.stack.front().offset;
      auto new_root = deep_copy_subtree(root_offset);
      Traits::set_root(dst_cursor._txn, new_root);
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
    free(dst_leaf);
    split_trie(dst, src,
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
      trie_ptr new_trie = alloc(TrieNode::size(suffix_len, dst_trie->count()));
      new_trie->create(*dst_trie,
                       Slice(&dst_trie->compressed()[dst.prefix], suffix_len));
      free(dst_trie);
      assert(new_trie->len() > 0);
      split_trie(dst, src, new_trie->compressed()[0], resolve_offset(new_trie));
    }
  }

  void split_trie(typename CursorDst::Transition& dst,
                  typename CursorSrc::Transition& src, int key1,
                  offset_t child1) {
    uint16_t split_pos = dst.keypos + dst.prefix;
    assert(split_pos > src.keypos);
    uint16_t src_split_pos = split_pos - src.keypos;
    assert(src_split_pos < 256);

    int key = split_pos < current_key.size() ? current_key[split_pos]
                                             : TrieNode::NONE;

    if (key != TrieNode::NONE || src.is_leaf()) {
      if (key == TrieNode::NONE && key1 == TrieNode::NONE) {
        assert(src.is_leaf());
        assert(child1.type() == LEAF);
        if (handler.overwrite(current_key, dst, src)) {
          assert(key1 == TrieNode::NONE);
          leaf_ptr old_leaf = resolve_dst(child1);
          auto src_slice = src.leaf()->value(*src_cursor._db);
          _Inserter(&dst, src_slice.size()).change_leaf();
          auto& new_leaf = dst.leaf();
          memcpy(new_leaf->vdata(), src_slice.data(), src_slice.size());
          free(old_leaf);
        }
        return;
      }

      // A new trie with two branches for the old dst and the new src
      trie_ptr new_trie = alloc(TrieNode::size(src_split_pos, 2));
      auto loffset =
          new_trie->create(Slice(current_key.data() + dst.keypos, dst.prefix),
                           key1, child1, key);
      dst.replace(resolve_offset(new_trie));
      dst.block = new_trie;
      dst.link_offset = loffset;

      if (src.is_leaf()) {
        auto& src_leaf = src.leaf();
        assert(src_leaf->key_size >= src_split_pos);
        uint8_t suffix_len = src_leaf->key_size - src_split_pos;
        leaf_ptr new_leaf =
            fill_leaf(Slice(&src_leaf->data[src_split_pos], suffix_len),
                      src_leaf->value(*src_cursor._db));
        *dst.link() = resolve_offset(new_leaf);
        return;
      }
      auto& src_trie = src.trie();
      assert(src_trie->len() >= src_split_pos);
      uint8_t suffix_len = src_trie->len() - src_split_pos;

      trie_ptr suffix_trie =
          alloc(TrieNode::size(suffix_len, src_trie->count()));
      suffix_trie->create(
          *src_trie, Slice(&src_trie->compressed()[src_split_pos], suffix_len));
      *dst.link() = resolve_offset(suffix_trie);

      auto dst_offset = suffix_trie->array();
      auto src_offset = src_trie->array();
      for (int i = 0, scount = src_trie->count(); i < scount; i++) {
        dst_offset[i] = deep_copy_subtree(src_offset[i]);
      }
      return;
    }

    assert(src.is_trie());
    assert(split_pos == current_key.size());
    // the whole prefix of the src == dst.prefix

    auto src_trie = src.trie();
    if (src_trie->isset(key1)) {
      // copy src trie to dst and move down the branch src shares with dst
      trie_ptr new_trie = alloc(src_trie->size());
      copy(*new_trie, *src_trie);
      dst.replace(resolve_offset(new_trie));

      auto dst_p = new_trie->offset(key1);
      auto dst_offset = new_trie->array();
      auto src_offset = src_trie->array();
      for (int i = 0, count = new_trie->count(); i < count; i++) {
        if (&dst_offset[i] == dst_p) {
          dst_offset[i] = child1;
          src_cursor.push(src_offset[i]);
          dst_cursor.stack.clear();
          merge_node();
          continue;
        }
        dst_offset[i] = deep_copy_subtree(src_offset[i]);
      }
      return;
    }

    trie_ptr new_trie =
        alloc(TrieNode::size(src_trie->len(), src_trie->count() + 1));
    dst.replace(resolve_offset(new_trie));
    dst.block = new_trie;
    dst.link_offset = new_trie->create(*src_trie, key1);
    *dst.link() = child1;
    for (int key = new_trie->first(); key != TrieNode::OUT_OF_RANGE;
         key = new_trie->next(key)) {
      if (key == key1) continue;
      *new_trie->offset(key) = deep_copy_subtree(*src_trie->offset(key));
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
      trie_ptr new_trie = clone_plus_one(dst_trie, suffix_len, &loffset);
      trie_ptr suffix_trie =
          inserter.alloc(DstTrieNode::size(suffix_len, src_trie->count()));
      suffix_trie->create(
          *src_trie,
          Slice(&src_trie->compressed()[src_trie->len() - suffix_len],
                suffix_len));

      dst.replace(resolve_offset(new_trie));
      dst.block = new_trie;
      dst.link_offset = loffset;
      *dst.link() = resolve_offset(suffix_trie);

      auto dst_offset = suffix_trie->array();
      auto src_offset = src_trie->array();
      for (int i = 0, scount = src_trie->count(); i < scount; i++) {
        dst_offset[i] = deep_copy_subtree(src_offset[i]);
      }
      return;
    }

    // Merge the children of both tries
    trie_ptr new_trie = alloc(DstTrieNode::size(
        dst_trie->len(), dst_trie->count() + src_trie->count()));

    // merge src_trie into dst_trie
    dst.replace(resolve_offset(new_trie));
    dst.block = new_trie;

    const typename DstTrieNode::offset_e* dst_poffset[257];
    const typename SrcTrieNode::offset_e* src_poffset[257];
    new_trie->create(*dst_trie, *src_trie, dst_poffset, src_poffset);
    auto array = new_trie->array();
    for (int i = 0, count = new_trie->count(); i < count; i++) {
      if (dst_poffset[i] && src_poffset[i]) {
        // walk down and merge - both tries have this branch
        array[i] = *dst_poffset[i];
        src_cursor.push(*src_poffset[i]);
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
      array[i] = deep_copy_subtree(*src_poffset[i]);
    }

    free(dst_trie);
  }

  void merge_leaf_into_trie(typename CursorDst::Transition& dst,
                            typename CursorSrc::Transition& src,
                            int suffix_len) {
    // src is a leaf -> insert into dst trie
    uint16_t loffset;
    trie_ptr new_trie = clone_plus_one(dst.trie(), suffix_len, &loffset);

    auto& src_leaf = src.leaf();

    assert(src_leaf->key_size >= suffix_len);
    uint8_t split_pos = src_leaf->key_size - suffix_len;
    leaf_ptr new_leaf = fill_leaf(Slice(&src_leaf->data[split_pos], suffix_len),
                                  src_leaf->value(*src_cursor._db));

    dst.replace(resolve_offset(new_trie));
    dst.block = new_trie;
    dst.link_offset = loffset;
    *dst.link() = resolve_offset(new_leaf);
  }

  typename CursorDst::Transition::trie_ptr clone_plus_one(
      typename CursorDst::Transition::trie_ptr& dst_trie, int suffix_len,
      uint16_t* loffset) {
    int branch_key = suffix_len ? current_key[dst_cursor.current_key.size()]
                                : TrieNode::NONE;
    assert(!(branch_key == TrieNode::NONE ? dst_trie->has_none()
                                          : dst_trie->isset(branch_key)));
    // otherwise find would have walked down

    trie_ptr new_trie =
        alloc(TrieNode::size(dst_trie->len(), dst_trie->count() + 1));
    *loffset = new_trie->create(*dst_trie, branch_key);
    free(dst_trie);
    return new_trie;
  }

  /**
   * @brief Deep copy entire subtree from source to destination
   */
  offset_t deep_copy_subtree(offset_t src_offset) {
    if (src_offset.type() == LEAF) return deep_copy_leaf(src_offset);
    return deep_copy_trie(src_offset);
  }

  /**
   * @brief Deep copy a leaf node from source to destination
   */
  offset_t deep_copy_leaf(offset_t src_offset) {
    auto src_leaf = (leaf_ptr)resolve_src(src_offset);
    Slice src_value = src_leaf->value(*src_cursor._db);
    leaf_ptr new_leaf = fill_leaf(src_leaf->key(), src_value);
    return resolve_offset(new_leaf);
  }

  /**
   * @brief Deep copy a trie node and its subtree from source to destination
   */
  offset_t deep_copy_trie(offset_t src_offset) {
    auto src_trie = (trie_ptr)resolve_src(src_offset);
    uint16_t trie_size = src_trie->size();
    trie_ptr dst_trie = alloc(trie_size);
    copy(*dst_trie, *src_trie);

    // Recursively copy all children and update offsets
    auto dst_array = dst_trie->array();
    auto src_array = src_trie->array();
    for (int i = 0, count = dst_trie->count(); i < count; i++) {
      dst_array[i] = deep_copy_subtree(src_array[i]);
    }
    return resolve_offset(dst_trie);
  }
};

}  // namespace leaves

#endif  // _LEAVES_MERGER_HPP
