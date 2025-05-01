#ifndef _LEAVES_INSERTER_HPP
#define _LEAVES_INSERTER_HPP

#include "./_node.hpp"

namespace leaves {

template <typename Transition>
struct _Inserter {
  typedef _Inserter<Transition> Inserter;
  using Traits = typename Transition::Traits;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using block_ptr = typename Transition::block_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  const Slice& value;
  Transition* back;

  _Inserter(Transition* back_, const Slice& value_)
      : value(value_), back(back_) {}

  template <typename T>
  offset_t resolve(T ptr) {
    return back->cursor->_db->resolve(ptr);
  }

  block_ptr resolve(offset_t offset) {
    return back->cursor->_db->resolve(offset);
  }

  block_ptr alloc(uint16_t size) { return back->cursor->_db->alloc(size); }

  block_ptr alloc_big(size_t size) {
    auto db = back->cursor->_db;
    auto slice = db->alloc_big(size);
    return db->resolve(slice.offset);
  }

  void free_big(leaf_ptr& leaf) {
    if (leaf->is_big()) {
      auto bv = leaf->big();
      back->cursor->_db->free_big(bv->offset, bv->size());
    }
  }

  void free(block_ptr& block) { back->cursor->_db->free(block); }

  void exec() {
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // insert the very first value
  void first_exec() {
    const Slice& bkey = back->key();
    back->leaf() = fill_leaf(bkey);
    back->offset = resolve(back->leaf());
    back->cmp = 0;
    back->prefix = bkey.size();
    back->advance_key(back->prefix);
    back->cursor->set_root(back->offset);
  }

  bool split_compressed() {
    if (back->is_trie() && back->prefix == back->trie()->_compressed_len)
      return false;  // no split

    /*
    Operation:

      Before:
        parent -> [abcd] -> children

      Insert: [abef]

      After:
        parent -> [ab] -> [cd] -> children
                       -> [ef] -> table with new value
    */

    assert(back->prefix < back->trie()->_compressed_len);

    trie_ptr otrie = back->trie();

    // copy the original trie node with second part of compressed
    // to a new page
    uint8_t prefix_len = otrie->_compressed_len - back->prefix;
    trie_ptr child_trie = alloc(TrieNode::size(prefix_len, otrie->count()));
    child_trie->create(*otrie,
                       Slice(&otrie->compressed()[back->prefix], prefix_len));

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int key =
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE;
    back->trie() = alloc(TrieNode::size(back->prefix, 2));
    back->link_offset = back->trie()->create(
        Slice(otrie->compressed(), back->prefix),
        otrie->compressed()[back->prefix], resolve(child_trie), key);
    free(otrie);
    back->replace(resolve(back->trie()));
    create_leaf();
    return true;
  }

  void create_leaf() {
    assert(back->key().size() < 255);
    const Slice& bkey = back->key();
    leaf_ptr leaf = fill_leaf(bkey);
    Transition& bottom = back->push(resolve(leaf));
    if (leaf->is_big()) bottom.big_value = resolve(leaf->big()->offset);
    bottom.cmp = 0;
    bottom.prefix = bkey.size();
    bottom.advance_key(bottom.prefix);
    *back->link() = bottom.offset;
  }

  leaf_ptr fill_leaf(const Slice& key) {
    leaf_ptr leaf = alloc(LeafNode::size(key, value));
    auto bv = leaf->set(key, value, back->cursor->big_key);
    if (bv) {
      const Slice& bkey = back->cursor->big_key;
      block_ptr ptr = alloc_big(bv->size());
      memcpy((char*)ptr, bkey.data(), bkey.size());
      memcpy((char*)ptr + bkey.size(), value.data(), value.size());
      bv->offset = resolve(ptr);
    }
    return leaf;
  }

  const uint16_t MAX_SIZE = TrieNode::MAX_SIZE;

  void add_to_array() {
    trie_ptr otrie = back->trie();
    back->trie() = alloc(std::min(
        (uint16_t)(otrie->size() + 2 * sizeof(offset_e)), (uint16_t)MAX_SIZE));
    back->link_offset = back->trie()->create(
        *otrie, back->key() ? back->branch_key : TrieNode::NONE);

    free(otrie);
    back->replace(resolve(back->trie()));
    back->cmp = 0;
    create_leaf();
  }

  // change the value of leaf
  void change_leaf() {
    assert(back->is_leaf());
    leaf_ptr oleaf = back->leaf();

    if (back->cmp == 0) {
      assert(back->prefix == back->leaf()->key_size);
      assert(back->key().empty());
      back->leaf() = fill_leaf(oleaf->key());
      free_big(oleaf);
      free(oleaf);
      back->replace(resolve(back->leaf()));
      return;
    }
    leaf_ptr copy =
        alloc(LeafNode::size(oleaf->key_size - back->prefix, value.size()));
    copy->key_size = oleaf->key_size - back->prefix;
    copy->value_size = oleaf->value_size;
    memcpy(copy->data, oleaf->data + back->prefix,
           copy->key_size + copy->vsize());

    int bkey = !copy->key_size ? TrieNode::NONE : copy->data[0];

    back->trie() = alloc(TrieNode::size(back->prefix, 2));
    back->link_offset = back->trie()->create(
        Slice(oleaf->data, back->prefix), bkey, resolve(copy),
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE);

    free(oleaf);
    back->replace(resolve(back->trie()));
    create_leaf();
  }
};

}  // namespace leaves
#endif  // _LEAVES_INSERTER_HPP