#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>
#include <vector>

#include "memory.hpp"

namespace leaves {

struct Transition {
  static const int NOT_SAME = 2;   // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  block_ptr branch;  // the branch block referenced in this transition
  block_ptr leaf;    // the final leaf block if any
  uint16_t prefix;   // count of equal chars in stringnode
  uint16_t suffix;   // count of equal chars in keyvaluenode
  uint8_t branch_key;
  bsize_t olink;  // the offset inside block that points to the output link

  const Compressed* compressed;
  union {
    const ArrayBranch* array;
    const TrieBranch* trie;
  };
  const Leaf* found_leaf;

  // 1: the key to find is bigger than the found node
  // 0: the key is found
  // -1: the key to find is smalled than the found node
  // NOT_SAME: it is not equal but not known if -1 or 1
  int cmp;

  // position inside the key
  bsize_t keypos;

  bool success() const { return found_leaf && cmp == 0; }
  offset_ptr* plink() { return branch->plink(olink); }
  void reset() {
    branch.reset();
    leaf.reset();
  }
  template <typename caller>
  bool follow_link(Trace& trace, const offset_ptr* link, caller c);
  };

struct Stack {
  typedef std::vector<Transition> stack_v;
  stack_v data;
  size_t size;

  Stack();

  void push(block_ptr block, bsize_t keypos = 0);

  Transition& front() { return data[0]; }
  Transition& back() { return data[size - 1]; }
  Transition& parent() {
    assert(size > 1);
    return data[size - 2];
  }
  const Transition& back() const { return data[size - 1]; }
  void clear(int size_ = 0) {
    for (int i = size_; i < size; i++) {
      data[i].reset();
    }
    size = size_;
  }
};

// A very simple implementation of strimg
struct KeyString {
  static const size_t MAX_SIZE = 512;

  const char* data() const { return _data; }
  bsize_t size() const { return _size; }

  void resize(bsize_t size) { _size = size; }
  char& back() { return _data[_size - 1]; }
  void pop_back() { _size--; }
  void push_back(char b) { _data[_size++] = b; }
  void clear() { _size = 0; }
  void append(const char* data, size_t size) {
    memcpy(&_data[_size], data, size);
    _size += size;
  }
  char operator[](bsize_t idx) { return _data[idx]; }
  operator Slice() const { return Slice(_data, _size); }
  template <typename T>
  bool operator==(T& other) const {
    return _size == other.size() && memcmp(_data, other.data(), _size) == 0;
  }

  KeyString() : _size(0) {}

  bsize_t _size;
  char _data[MAX_SIZE];

  friend std::ostream& operator<<(std::ostream& os,
                                  const leaves::KeyString& ks) {
    // Define how to print the object here
    char data[leaves::KeyString::MAX_SIZE];
    memcpy(data, ks.data(), ks.size());
    data[ks.size()] = 0;
    os << data;
    return os;
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

  /* Helpers */

  void advance_key(size_t size) {
    current_key.append(rest_key.data(), size);
    rest_key.iadvance(size);
  }

  void push(const offset_ptr& ptr) {
    stack.push(storage.get_block(ptr), current_key.size());
  }

  void pop() {
    assert(stack.size > 0);
    current_key.resize(stack.back().keypos);
    stack.size--;
  }

  void changed_branch_key() {
    // used in move operations
    Transition& back = stack.back();
    bsize_t bpos = back.keypos + back.prefix;
    if (current_key.size() == bpos) {
      current_key.push_back(back.branch_key);
    } else {
      assert(current_key.size() == bpos + 1);
      current_key.back() = back.branch_key;
    }
  }

  bool _keep_stack();
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
  // std::string current_key;
  KeyString current_key;
  bool transaction_active;
};

template <typename caller>
inline bool Transition::follow_link(Trace& trace, const offset_ptr* link,
                                    caller c) {
  if (!link) return false;
  assert(link->data);
  assert(branch->data + BranchBlock::MAX_SPACE > (const uint8_t*)link);
  BranchBlock* block = branch;

  olink = (const uint8_t*)link - block->data;
  if (link->pool_id() == LEAF_BLOCK) {
    leaf = trace.storage.get_block(block->leaves);
    c(trace, leaf.leaf()->leaf(*link));
    return false;
  }

  assert(link->pool_id() < POOL_COUNT);
  trace.stack.back().cmp = 0;
  trace.push(*link);
  return true;
}

}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP
