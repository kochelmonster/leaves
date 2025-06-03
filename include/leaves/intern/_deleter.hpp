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
    return back->cursor->_db->resolve(ptr);
  }

  block_ptr resolve(offset_t offset) {
    return back->cursor->_db->resolve(offset);
  }

  block_ptr alloc(uint16_t size) { return back->cursor->_db->alloc(size); }

  void free(block_ptr& block) { back->cursor->_db->free(block); }

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
    block_ptr block = back->block;
    uint16_t prefix = back->prefix;
    parent.pop();
    assert(parent.is_trie());
    if (parent.trie()->count() > 2)
      reduce_array(parent, prefix);
    else
      combine(parent);
    free(block);
  }

  void reduce_array(Transition& parent, uint16_t prefix) {
    trie_ptr otrie = parent.trie();
    parent.trie() = alloc(otrie->size() - sizeof(offset_e));
    parent.trie()->create_remove(*otrie,
                                 prefix ? parent.branch_key : TrieNode::NONE);
    parent.replace(resolve(parent.trie()));
    free(otrie);
    parent.cmp = -1;
    if (!prefix) parent.prefix = 0;  // NONE Key -> to the first child
    parent.cursor->next();
  }

  void combine(Transition& parent) {
    trie_ptr otrie = parent.trie();
    offset_e* link = parent.link();
    offset_e* begin = otrie->array();
    bool go_next = link != begin;
    offset_e* child_left = go_next ? begin : begin + 1;

    uint8_t buffer[256];
    uint8_t len = parent.trie()->len();
    memcpy(buffer, parent.trie()->compressed(), len);
    if (child_left->type() == TRIE) {
      trie_ptr child = resolve(*child_left);
      memcpy(buffer + len, child->compressed(), child->len());
      len += child->len();
      parent.trie() = alloc(TrieNode::size(len, child->count()));
      parent.trie()->create(*child, Slice(buffer, len));
      free(child);
      parent.replace(resolve(parent.trie()));
      parent.cmp = Transition::NOT_FOUND;
      if (go_next)
        parent.cursor->next();
      else {
        parent.resize_key(parent.keypos);
        parent.first();
      }
    } else {
      leaf_ptr child = resolve(*child_left);
      memcpy(buffer + len, child->data, child->key_size);
      len += child->key_size;
      parent.leaf() = alloc(LeafNode::size(len, child->vsize()));
      parent.leaf()->key_size = len;
      parent.leaf()->value_size = child->value_size;
      memcpy(parent.leaf()->data, buffer, len);
      memcpy(parent.leaf()->vdata(), child->vdata(), child->vsize());
      free(child);
      parent.replace(resolve(parent.leaf()));
      parent.leaf_step();
      if (go_next) parent.cursor->next();
    }
    free(otrie);
  }
};
}  // namespace leaves
#endif  // _LEAVES_DELETER_HPP