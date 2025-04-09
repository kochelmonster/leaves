#ifndef _LEAVES_DELETER_HPP
#define _LEAVES_DELETER_HPP

#include "_node.hpp"

namespace leaves {

template <typename Transition>
struct _Deleter {
  typedef _Deleter<Transition> Deleter;
  using Traits = typename Transition::Traits;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using block_ptr = typename Transition::block_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  Transition* back;

  _Deleter(Transition* back_) : back(back_) {}

  template <typename T>
  offset_t resolve(T ptr) {
    return back->cursor->storage.resolve(ptr);
  }

  block_ptr resolve(offset_t offset) {
    return back->cursor->storage.resolve(offset);
  }

  block_ptr alloc(uint16_t size) { return back->cursor->storage.alloc(size); }
  block_ptr alloc_slot(uint16_t size) {
    return back->cursor->storage.alloc_slot(size);
  }

  void free(block_ptr& block) { back->cursor->storage.free(block); }

  void exec() {
    assert(back->success());
    assert(back->is_leaf());
    if (back->is_root()) {
      // remove the last node
      back->cursor->set_root(offset_t());
      free(back->block);
      back->pop();
      return;
    }

    Transition& parent = back->parent();
    parent.pop();
    assert(parent.is_trie());

    if (parent.trie->count() > 2)
      reduce_array(parent);
    else
      combine(parent);
    free(back->block);
  }

  void reduce_array(Transition& parent) {
    trie_ptr otrie = parent.trie;

    parent.trie = alloc(otrie->size() - sizeof(offset_e));
    parent.trie->create_remove(
        *otrie, back->prefix ? parent.branch_key : TrieNode::NONE);
    parent.cmp = Transition::NOT_FOUND;
    parent.replace(resolve(parent.trie));

    free(otrie);
  }

  void combine(Transition& parent) {
    trie_ptr otrie = parent.trie;
    offset_e* link = parent.link();
    offset_e* begin = otrie->array();
    offset_e* child_left = link == begin ? begin + 1 : begin;

    uint8_t buffer[256];
    uint8_t len = parent.trie->_compressed_len;
    memcpy(buffer, parent.trie->compressed(), len);
    if (child_left->type() == TRIE) {
      trie_ptr child = resolve(*child_left);
      memcpy(buffer + len, child->compressed(), child->_compressed_len);
      len += child->_compressed_len;
      parent.trie = alloc(TrieNode::size(len, child->count()));
      parent.trie->create(*child, Slice(buffer, len));
      free(child);
      parent.replace(resolve(parent.trie));
    } else {
      leaf_ptr child = resolve(*child_left);
      memcpy(buffer + len, child->data, child->key_size);
      len += child->key_size;
      parent.leaf = alloc(LeafNode::size(len, child->vsize()));
      parent.leaf->key_size = len;
      parent.leaf->value_size = child->value_size;
      memcpy(parent.leaf->data, buffer, len);
      memcpy(parent.leaf->vdata(), child->vdata(), child->vsize());
      free(child);
      parent.replace(resolve(parent.leaf));
    }
    free(otrie);
    
  }
};
}  // namespace leaves
#endif  // _LEAVES_DELETER_HPP