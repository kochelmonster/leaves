#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>

#include "node.hpp"
#include "storage.hpp"

namespace leaves {

enum TrieState { Upper, Lower };

struct Transition {
  // offset of the trie block this transition works with
  offset_ptr offset;

  // the pointer offset points to
  BlockUnion* block;

  // pointer to a Node union: node = &trie_block.data[pnode->offset]
  Node* node;

  // pointer to a node_ptr to node: pnode = &trie_block.data[offset]
  node_ptr* pnode;

  // offset to pnode
  ssize_t onode;

  // position inside the key
  ssize_t keypos;

  // upper or lower bits of char
  TrieState trie_state;

  // the index of the current branch
  int index;

  // true the key was found
  bool found;

  Transition(offset_ptr offset_ = 0, ssize_t onode_ = 0, ssize_t keypos_ = 0);

  // fill node, pnode, offset
  void resolve(Trace& cursor);
  void to_writable(BlockUnion* block);

  bool advance(Trace& cursor);
  bool find(Trace& cursor);
  Slice get_value(Trace& cursor) const;
  void set_value(Trace& cursor, const Slice& value);
  ssize_t mark_deep_size(Trace& cursor, TrieBlock& sizes);
};

// A cursor to
struct Trace {
  Trace(Storage& storage_);
  ~Trace();

  // return true if the cursor is on a valid position
  bool isvalid() const;
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  bool set_value(const Slice& value);
  Slice get_value() const;
  void remove();
  void commit();
  void rollback();

  block_ptr get_block(offset_ptr offset) const {
    return storage.get_block(offset);
  }

  /* allocates size bytes for a new node from the current block, add a new
     Transition to the stack. returns the new transition.

     size: the size to allocate
     dnode: dnode is the offset delta to the last child
     type: the nodes type of the new transition

     returns the new transition of the new node

     This function will always succeed. If necessary, it creates new blocks and
     rearrange the memory.
   */
  Transition& alloc(ssize_t size, ssize_t dnode, NodeType type);

  /* A sub function of alloc: 
     Does the same as alloc but returns NULL if the block does not
     provide enough free space.
   */
  Transition* alloc_in_block(ssize_t size, ssize_t dnode, NodeType type);

  /* A sub function of alloc: 
     Moves the node in the last transition to a new block and ensures the new
     block has at least 2048 byte free.
     the function corrects the stack by inserting a link node.
   */
  void move_last_node();


  Storage& storage;

  // registration id in storage.shared
  int cursor_id;

  offset_ptr root;
  std::vector<Transition> stack;
  Slice rest_key;
  std::string current_key;

  // the trie state of the stack.back
  TrieState trie_state;

  bool transaction_active;
};

}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP
