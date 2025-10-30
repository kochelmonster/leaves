#ifndef _LEAVES_DELETER_HPP
#define _LEAVES_DELETER_HPP

#include <cstring>

#include "_node.hpp"

namespace leaves {

template <typename Cursor>
struct _Deleter {
  typedef _Deleter<Cursor> Deleter;
  using Transition = typename Cursor::Transition;
  using Traits = typename Transition::Traits;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using block_ptr = typename Transition::block_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  Cursor& cursor;
  Transition* back;

  _Deleter(Cursor& cursor) : cursor(cursor), back(&cursor.stack.back()) {}

  void dec_branch_count(Transition& parent, int count) {
    // Decrement for each character position this node covers (prefix + branch)
    uint16_t start_pos = parent.keypos;
    uint16_t end_pos =
        std::min((uint16_t)(start_pos + parent.prefix + 1),
                 (uint16_t)(sizeof(cursor._txn->branch_count) /
                            sizeof(cursor._txn->branch_count[0])));
    for (uint16_t pos = start_pos; pos < end_pos; pos++) {
      cursor._txn->branch_count[pos] -= count;
    }
  }

  template <typename T>
  offset_t resolve(T ptr) {
    return cursor._db->resolve(ptr);
  }

  block_ptr resolve(offset_t offset) { return cursor._db->resolve(offset); }

  block_ptr alloc(uint16_t size) { return cursor._db->alloc(size); }

  void free(block_ptr& block) { cursor._db->free(block); }

  void exec() {
    assert(back->success());
    assert(back->is_leaf());
    remove_node(back);
  }

  void remove_node(Transition* trans) {
    if (trans->is_root()) {
      // remove the last node
      trans->set_root(offset_t());
      free(trans->block);
      trans->pop();
      return;
    }

    Transition& parent = trans->parent();
    block_ptr block = trans->block;
    uint16_t prefix = trans->prefix;
    parent.pop();  // remove trans from stack
    assert(parent.is_trie());
    switch (parent.trie()->count()) {
      case 0:
        assert(false);
        break;  // should never happen
      case 1:
        remove_node(&parent);
        break;  // recursively remove the only child
      case 2:
        combine(parent, prefix);
        break;
      default:
        reduce_array(parent, prefix);
        break;
    }
    free(block);
  }

  void reduce_array(Transition& parent, uint16_t prefix) {
    trie_ptr otrie = parent.trie();
    int key = prefix ? parent.branch_key : TrieNode::NONE;
    // Calculate proper size: same prefix length, one less branch
    parent.trie() = alloc(TrieNode::size(otrie->len(), otrie->count() - 1));
    parent.trie()->create_remove(*otrie, key);
    parent.replace(resolve(parent.trie()));
    dec_branch_count(parent, 1);  // Removing 1 branch
    free(otrie);
    parent.cmp = -1;
    if (!prefix) parent.prefix = 0;  // NONE Key -> to the first child
    cursor.next();
  }

  // Combine the parent with the only child
  void combine(Transition& parent, uint16_t prefix) {
    trie_ptr otrie = parent.trie();
    offset_e* link = parent.link();
    offset_e* begin = otrie->array();
    bool go_next = link != begin;
    offset_e* child_remaining = go_next ? begin : begin + 1;
    // go_next == true means the remaining child is before otrie
    // for positioning the cursor we have to move next

    // Decrement branch count for both branches being removed/combined
    dec_branch_count(parent, 2);  // Removing 2 branches

    uint8_t len = parent.trie()->len();
    uint8_t buffer[256];  // to hold the compressed key
    memcpy(buffer, parent.trie()->compressed(), len);
    if (child_remaining->type() == TRIE) {
      trie_ptr child = resolve(*child_remaining);
      if (len + child->len() > 255) {
        // the compressed part is too big -> keep the parent
        return reduce_array(parent, prefix);
      }

      memcpy(buffer + len, child->compressed(), child->len());
      len += child->len();
      parent.trie() = alloc(TrieNode::size(len, child->count()));
      parent.trie()->create(*child, Slice(buffer, len));
      parent.replace(resolve(parent.trie()));
      // replace trie! the type is important
      parent.link_offset = (char*)(&parent.trie()->array()[parent.branch_key]) -
                           (char*)parent.block;

      free(child);
    } else {
      leaf_ptr child = resolve(*child_remaining);
      if (len + child->key_size > 255) {
        // the compressed part is too big -> keep the parent
        return reduce_array(parent, prefix);
      }

      memcpy(buffer + len, child->data, child->key_size);
      len += child->key_size;
      parent.leaf() = alloc(LeafNode::size(len, child->vsize()));
      parent.leaf()->key_size = len;
      parent.leaf()->value_size = child->value_size;
      memcpy(parent.leaf()->data, buffer, len);
      memcpy(parent.leaf()->vdata(), child->vdata(), child->vsize());
      // replace leaf! the type is important
      parent.replace(resolve(parent.leaf()));
      free(child);
    }

    free(otrie);
    if (go_next)
      cursor.next();
    else {
      parent.resize_key(parent.keypos);
      parent.first();
    }
  }
};
}  // namespace leaves
#endif  // _LEAVES_DELETER_HPP