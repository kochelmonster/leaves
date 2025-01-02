#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>
#include <vector>

#include "memory.hpp"

namespace leaves {

struct Transition {
  block_ptr branch;   // the branch block referenced in this transition
  block_ptr leaf;     // the final leaf block if any
  uint16_t prefix;    // count of equal chars in stringnode
  uint16_t suffix;    // count of equal chars in keyvaluenode

  union {
    TrieBranch::Index tindex;
    char index;       // bit index or index of arraynode or trie_node -2 means value is used
  };

  bsize_t olink;      // the offset inside block that points to the output link
  const Leaf* found_leaf;
  bool success;

  // position inside the key
  bsize_t keypos;

  offset_ptr* plink() { return branch->plink(olink); }
  void reset() { branch.reset(); leaf.reset(); success = false; }
  bool follow_link(Trace& trace, const offset_ptr* link);
};

struct Stack {
  typedef std::vector<Transition> stack_v;
  stack_v data;
  size_t size;

  Stack();

  void push(block_ptr block);

  Transition& front() { return data[0]; }
  Transition& back() { return data[size - 1]; }
  Transition& parent() { assert(size > 1); return data[size - 2]; }
  const Transition& back() const { return data[size - 1]; }
  void clear(int size_ = 0) {
    for (int i = size_; i < size; i++) {
      data[i].reset();
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
  void set_value(const Slice& value);
  Slice get_value();
  void remove();
  void commit();
  void rollback();

  void advance_key(size_t size) {
    current_key.append(rest_key.data(), size);
    rest_key.iadvance(size);
  }

  void push(const offset_ptr& ptr) {
    stack.push(storage.get_block(ptr));
  }

  void _keep_stack();
  void _find();
  void _make_writable();

  void _prepare_trie();
  void _add_key_to_trie();
    
  void _update();
  void _split_block();
  
  DBMemory& storage;

  // registration id in storage.shared
  int cursor_id;

  offset_ptr root;
  Stack stack;
  Slice rest_key;
  std::string current_key;
  bool transaction_active;
};




}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP
