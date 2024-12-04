#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>
#include <vector>

#include "memory.hpp"
#include "node.hpp"

namespace leaves {

struct Transition {
  // the pointer offset points to
  block_ptr block;

  // pointer to a node_ptr to node: pnode = &trie_block.data[onode]
  node_ptr* pnode;

  // position inside the key
  size_t keypos;

  // the index of the current branch
  int index;

  void rebase(block_ptr dest) {
    pnode = (node_ptr*)(((char*)pnode-(char*)block.ptr)+(char*)dest.ptr);
    block = dest;
  }
};

struct Stack {
  typedef std::vector<Transition> stack_v;
  stack_v data;
  size_t size;

  Stack();

  void push(block_ptr block, node_ptr* pnode, size_t keypos, int index);

  Transition& front() { return data[0]; }
  Transition& back() { return data[size - 1]; }
  const Transition& back() const { return data[size - 1]; }
  void clear(int size_ = 0) {
    for (int i = size_; i < size; i++) {
      data[i].block.reset();
    }
    size = size_;
  }
};

// A cursor to
struct Trace {
  Trace(DBMemory& storage_);
  ~Trace();

  // return true if the cursor is on a valid position
  bool is_valid() const;
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void set_value(Slice value);
  Slice get_value();
  void remove();
  void commit();
  void rollback();

  void _advance_stack();
  void _find();
  void _make_stack_writable();
  void _prepare_trie();
  void _add_key_to_trie();
  node_ptr* _add_string(block_ptr block, node_ptr* pnode);
  void _update();
  void _split_block();
  void _clear_value();
  
  DBMemory& storage;

  // registration id in storage.shared
  int cursor_id;

  offset_ptr root;
  Stack stack;
  Slice rest_key;
  std::string current_key;
  bool transaction_active;
  int last_root;
  block_ptr current_value;
  block_ptr current_big_value;

  uint32_t _debug_stat_page_splits;
};

}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP
