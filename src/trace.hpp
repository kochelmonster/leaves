#ifndef _LARCH_LEAVES_TRACE_HPP
#define _LARCH_LEAVES_TRACE_HPP

#include "storage.hpp"
#include "node.hpp"

namespace leaves {

typedef offset_ptr* (*move_func_t)(Transition& transition, string& current_key);


struct Trace {
  typedef std::vector<Transition> stack_type;

  Trace(Storage& storage, offset_ptr* root_=NULL);
  bool valid() const { return rest_key.empty() && stack.size() && stack.back().valid(); }
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void set_value(const Slice& value);
  Slice get_value() const;
  void remove();

  any_ptr allocate(size_t size);
  void free(any_ptr ptr);

  any_ptr ipop_value();
  void iinsert(any_ptr val_ptr);
  void ifind();
  void imove_end(move_func_t move);
  void imove(move_func_t move, move_func_t move_end);
  void update();

  stack_type stack;
  Storage& storage;
  offset_ptr* root;
  ISlice rest_key;
  string current_key;
  uint64_t version;
};

inline Slice Trace::get_value() const {
  ValueData* node(stack.back().value);
  assert(node->type == kValue);
  return Slice(node->value, node->size);
}

inline any_ptr Trace::allocate(size_t size) {
  return storage.allocate(size);
}

inline void Trace::free(any_ptr ptr) {
  return storage.free(ptr);
}

inline void Trace::update() {
  while(version != *storage.version) {
    // restore trace after another cursor changed the trie.
    find(current_key);
  }
}

inline void Trace::iinsert(any_ptr val_ptr) {
  stack.back().insert(rest_key, val_ptr);
  ifind();
}

} // namespace leaves

#endif // _LARCH_LEAVES_TRACE_HPP
