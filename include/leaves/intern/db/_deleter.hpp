#ifndef _LEAVES_DELETER_HPP
#define _LEAVES_DELETER_HPP

#include <cstring>

#include "../core/_node.hpp"

namespace leaves {

template <typename Cursor>
struct _Deleter {
  typedef _Deleter<Cursor> Deleter;
  using Transition = typename Cursor::Transition;
  using Traits = typename Transition::Traits;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using page_ptr = typename Transition::page_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  Cursor& cursor;
  Transition* back;

  _Deleter(Cursor& cursor) : cursor(cursor), back(&cursor.stack.back()) {}

  template <typename T>
  typename Traits::template Pointer<T> resolve(const offset_t* offset_ptr) {
    return cursor._db->template resolve<T>(offset_ptr);
  }

  template <typename T>
  offset_t resolve(T ptr) {
    return cursor._db->resolve(ptr);
  }

  page_ptr alloc(uint16_t size) { return cursor._db->alloc(size); }

  // Allocate node with PageHeader prefix, return pointer to node
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc(node_size);
    return page + sizeof(PageHeader);
  }

  // Free node by computing PageHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    using PageHeader = typename Traits::PageHeader;
    static_assert(
        !std::is_same_v<NodePtr, page_ptr>,
        "free_node must be called with node pointers, not page pointers");
    page_ptr page = node - sizeof(PageHeader);
    cursor._db->free(page);
  }

  void free(page_ptr page) { cursor._db->free(page); }

  void exec() {
    assert(back->success());
    assert(back->is_leaf());
    remove_node(back);
  }

  void remove_node(Transition* trans) {
    if (trans->is_root()) {
      // remove the last node
      *trans->offset = offset_t();
      free_node(trans->node);
      trans->pop();
      return;
    }

    Transition& parent = trans->parent();
    // Save the node pointer before pop invalidates trans
    auto node = trans->node;  
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
    free_node(node);
  }

  void reduce_array(Transition& parent, uint16_t prefix) {
    trie_ptr otrie = parent.trie();
    // Calculate proper size: same prefix length, one less branch
    parent.trie() =
        alloc_node<trie_ptr>(TrieNode::size(otrie->len(), otrie->count() - 1));
    parent.trie()->create_remove(*otrie,
                                 prefix ? parent.branch_key : TrieNode::NONE);
    parent.update_trie_offset();
    free_node(otrie);
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

    uint8_t len = parent.trie()->len();
    uint8_t buffer[256];  // to hold the compressed key
    memcpy(buffer, parent.trie()->compressed(), len);
    if (child_remaining->type() == TRIE) {
      trie_ptr child = resolve<TrieNode>(child_remaining);
      if (len + child->len() > 255) {
        // the compressed part is too big -> keep the parent
        return reduce_array(parent, prefix);
      }

      memcpy(buffer + len, child->compressed(), child->len());
      len += child->len();
      parent.trie() = alloc_node<trie_ptr>(TrieNode::size(len, child->count()));
      parent.trie()->create(*child, Slice(buffer, len));
      parent.update_trie_offset();
      // replace trie! the type is important
      parent.link_idx = parent.trie()->array_index(parent.branch_key);
      assert(parent.link_idx < parent.trie()->count());

      free_node(child);
    } else {
      leaf_ptr child = resolve<LeafNode>(child_remaining);
      if (len + child->key_size > 255) {
        // the compressed part is too big -> keep the parent
        return reduce_array(parent, prefix);
      }

      memcpy(buffer + len, child->data, child->key_size);
      len += child->key_size;
      parent.leaf() = alloc_node<leaf_ptr>(LeafNode::size(len, child->vsize()));
      parent.leaf()->key_size = len;
      parent.leaf()->value_size = child->value_size;
      memcpy(parent.leaf()->data, buffer, len);
      memcpy(parent.leaf()->vdata(), child->vdata(), child->vsize());
      parent.update_leaf_offset();
      free_node(child);
    }

    free_node(otrie);
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