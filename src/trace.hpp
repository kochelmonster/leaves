#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>
#include <vector>

#include "node.hpp"
#include "storage.hpp"

namespace leaves {

const size_t MAX_KEY_SIZE = 1024;
enum TrieState { Upper, Lower };

struct Transition {
  // offset of the trie block this transition works with
  offset_ptr offset;

  // the pointer offset points to
  BlockUnion* block;

  // pointer to a Node union: node = &trie_block.data[pnode->offset]
  Node* node;

  // pointer to a node_ptr to node: pnode = &trie_block.data[onode]
  node_ptr* pnode;

  // offset to pnode
  ssize_t onode;

  // position inside the key
  ssize_t keypos;

  // upper or lower bits of char
  TrieState trie_state;

  // the index of the current branch
  int index;

  Transition(offset_ptr offset_ = 0, ssize_t onode_ = 0, ssize_t keypos_ = 0);

  // fill node, pnode, offset
  Transition& resolve(Trace& cursor);
  Transition& clear(Trace& cursor);
  Transition derive(ssize_t donode = 0, ssize_t keypos_ = 0) const;

  void to_writable(BlockUnion* block);

  bool advance(Trace& cursor);
  bool find(Trace& cursor);
  Slice get_value(Trace& cursor) const;
  void set_value(Trace& cursor, const Slice& value);
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

  Transition& back() { return stack.back(); }

  void make_stack_writable();

  struct alloc_ptr {
    node_ptr ptr;
    bool need_refresh;  // if true alloc changed the current block
  };

  // allocates size bytes on the current block for an object of type.
  alloc_ptr alloc(ssize_t size, NodeType type);
  Node* resolve(node_ptr node);

  Storage& storage;

  // registration id in storage.shared
  int cursor_id;

  typedef std::vector<Transition> stack_v;

  offset_ptr root;
  stack_v stack;
  Slice rest_key;
  std::string current_key;

  bool transaction_active;
};

struct BlockSplitter {
  BlockSplitter(Trace& cursor_) : is_finished(false), cursor(cursor_) {}

  size_t find_splitpoint(Transition& trans);
  offset_ptr split_block(Transition& block_root);

  bool is_finished;
  Transition splitpoint;
  Trace& cursor;
};

}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP
