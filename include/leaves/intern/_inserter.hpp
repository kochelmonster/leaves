#ifndef _LEAVES_INSERTER_HPP
#define _LEAVES_INSERTER_HPP

#include <cstring>

#include "_node.hpp"

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

  Transition* back;
  const size_t value_size;

  _Inserter(Transition* back_, size_t size) : back(back_), value_size(size) {}

  template <typename T>
  offset_t resolve(T ptr) {
    return back->cursor->_db->resolve(ptr);
  }

  block_ptr resolve(const offset_t* offset_ptr) {
    return back->cursor->_db->resolve(offset_ptr);
  }

  block_ptr alloc(uint16_t size) { return back->cursor->alloc(size); }

  // Allocate node with BlockHeader prefix, return pointer to node
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using BlockHeader = typename Traits::BlockHeader;
    block_ptr block = alloc(sizeof(BlockHeader) + node_size);
    return NodePtr((char*)block + sizeof(BlockHeader));
  }

  // Free node by computing BlockHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    using BlockHeader = typename Traits::BlockHeader;
    static_assert(
        !std::is_same_v<NodePtr, block_ptr>,
        "free_node must be called with node pointers, not block pointers");
    block_ptr block((char*)node - sizeof(BlockHeader));
    free(block);
  }

  void free(block_ptr block) { back->cursor->_db->free(block); }

  void exec() {
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // insert the very first value
  void first_exec() {
    Transition* front = back;

    const Slice& bkey = back->key();
    if (bkey.size() > 255) {
      trie_ptr trie = alloc_node<trie_ptr>(TrieNode::size(255, 1));
      assert(trie);  // must always succeed
      back->offset = resolve(trie);
      back->set_root(back->offset);
      back->trie() = trie;
      fill_bigkey(*back);
      create_leaf();
      return;
    }

    assert(bkey.size() <= 255);
    back->leaf() = fill_leaf(bkey);
    assert(back->leaf());  // must always succeed
    back->offset = resolve(back->leaf());
    back->cmp = 0;
    back->prefix = bkey.size();
    back->advance_key(back->prefix);
    if (front == back)
      back->set_root(back->offset);
    else
      *back->parent().link() = back->offset;
  }

  bool split_compressed() {
    if (back->is_trie() && back->prefix == back->trie()->len())
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

    assert(back->prefix < back->trie()->len());

    trie_ptr otrie = back->trie();
    assert(otrie->count() < otrie->MAX_BRANCH_COUNT);

    // copy the original trie node with second part of compressed
    // to a new slot
    uint8_t suffix_len = otrie->len() - back->prefix;
    trie_ptr child_trie =
        alloc_node<trie_ptr>(TrieNode::size(suffix_len, otrie->count()));
    child_trie->create(*otrie,
                       Slice(&otrie->compressed()[back->prefix], suffix_len));

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int key =
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE;
    trie_ptr trie = alloc_node<trie_ptr>(TrieNode::size(back->prefix, 2));

    back->trie() = trie;
    back->link_offset = back->trie()->create(
        Slice(otrie->compressed(), back->prefix),
        otrie->compressed()[back->prefix], resolve(child_trie), key);
    free_node(otrie);
    back->replace(resolve(back->trie()));
    create_leaf();
    return true;
  }

  void fill_bigkey(Transition& trans) {
    assert(trans.key().size() > 255);
    Slice prefix = trans.key().slice(255);
    trans.branch_key = trans.key()[prefix.size()];
    trans.link_offset = trans.trie()->create(prefix, trans.branch_key);
    trans.prefix = prefix.size();
    trans.cmp = 0;
    trans.advance_key(trans.prefix);
  }

  void create_bigkey() {
    Slice& key = back->key();
    while (key.size() > 255) {
      trie_ptr trie = alloc(TrieNode::size(255, 1));
      Transition& bottom = back->push(resolve(trie));
      fill_bigkey(bottom);
      *back->link() = bottom.offset;
      back = &bottom;
    }
  }

  void create_leaf() {
    create_bigkey();
    const Slice& bkey = back->key();
    leaf_ptr leaf = fill_leaf(bkey);
    Transition& bottom = back->push(resolve(leaf));
    bottom.cmp = 0;
    bottom.prefix = bkey.size();
    bottom.advance_key(bottom.prefix);
    *back->link() = bottom.offset;
  }

  leaf_ptr fill_leaf(const Slice& key) {
    leaf_ptr leaf =
        alloc_node<leaf_ptr>(LeafNode::size(key.size(), value_size));
    leaf->set(key, value_size);
    return leaf;
  }

  const uint16_t MAX_SIZE = TrieNode::MAX_SIZE;

  void add_to_array() {
    trie_ptr otrie = back->trie();
    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(back->prefix, otrie->count() + 1));

    back->trie() = new_trie;
    back->link_offset = new_trie->create(
        *otrie, back->key() ? back->branch_key : TrieNode::NONE);

    free_node(otrie);
    back->replace(resolve(new_trie));
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
      free_node(oleaf);
      back->replace(resolve(back->leaf()));
      return;
    }

    // replace the leaf with a trie node!

    // first: copy the leaf node and cut of the new rest key by prefix
    // if it is a big leaf just the reference to the big value is copied
    assert(back->prefix <= oleaf->key_size);

    leaf_ptr copy = copy_reduced_leaf(back->prefix, oleaf);

    int bkey = !copy->key_size ? TrieNode::NONE : copy->data[0];

    trie_ptr new_trie = alloc_node<trie_ptr>(TrieNode::size(back->prefix, 2));
    back->trie() = new_trie;
    back->link_offset = new_trie->create(
        Slice(oleaf->data, back->prefix), bkey, resolve(copy),
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE);

    free_node(oleaf);
    back->replace(resolve(back->trie()));
    create_leaf();
  }

  leaf_ptr copy_reduced_leaf(uint8_t split_pos, leaf_ptr& oleaf) {
    leaf_ptr copy = alloc_node<leaf_ptr>(
        LeafNode::size(oleaf->key_size - split_pos, oleaf->vsize()));
    if (copy) {
      copy->key_size = oleaf->key_size - split_pos;
      copy->value_size = oleaf->value_size;
      memcpy(copy->data, oleaf->data + split_pos,
             copy->key_size + copy->vsize());
    }
    return copy;
  }
};

}  // namespace leaves
#endif  // _LEAVES_INSERTER_HPP