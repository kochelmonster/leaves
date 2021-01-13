#ifndef _LARCH_LEAVES_TRACE_HPP
#define _LARCH_LEAVES_TRACE_HPP

#include "storage.hpp"
#include "node.hpp"


namespace leaves {

typedef offset_ptr* (*move_func_t)(Transition& transition, KeyString& current_key);

struct Trace;

struct Stack {
  typedef std::vector<Transition> stack_type;

  Stack(Trace& trace);
  bool empty() const;
  size_t size() const;
  Transition* begin();
  Transition* end();
  Transition& push_back(offset_ptr* next);
  void pop_back();
  Transition& back();
  const Transition& back() const;
  Transition& back(int index);
  void erase(Transition* iter);
  void grow();
  void clear();

  Trace& trace;
  offset_ptr null;
  size_t pos;
  Transition* top;
  stack_type data;
};

struct Trace {
  Trace(Storage& storage, offset_ptr* root_=NULL);
  bool valid() const { return rest_key.empty() && stack.back().valid(); }
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

  Storage& storage;
  Stack stack;
  offset_ptr* root;
  ISlice rest_key;
  KeyString current_key;
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
  while(version != storage.header->version) {
    // restore trace after another cursor changed the trie.
    find(current_key.slice());
  }
}

inline void Trace::iinsert(any_ptr val_ptr) {
  stack.back().insert(rest_key, val_ptr);
  ifind();
}

/* Stack
---------------------------------------------------------------------
*/

inline Stack::Stack(Trace& trace_) : trace(trace_), pos(1) {
  grow();
  null = &trace.storage.header->null;
  data[0].init(&null);
  top = &data[0];
}

inline Transition& Stack::push_back(offset_ptr* next) {
  if (pos++ >= data.size())
    grow();
  return (++top)->init(next);
}

inline void Stack::pop_back() {
  assert(pos>1);
  top--;
  pos--;
}

inline Transition& Stack::back() {
  assert(pos >= 1);
  return *top;
}

inline const Transition& Stack::back() const {
  assert(pos >= 1);
  return *top;
}

inline Transition& Stack::back(int index) {
  assert(pos-index >= 1);
  return *(top-index);
}

inline void Stack::grow() {
  size_t old_size = data.size();
  data.resize(data.size()+128);
  for(size_t i = old_size; i < data.size(); i++) {
    data[i].trace = &trace;
  }
  top = &data[pos-1];
}

inline void Stack::erase(Transition* iter) {
  pos = (iter - &data[0]);
  top = &data[pos-1];
};

inline Transition* Stack::begin() {
  return &data[1];
}

inline Transition* Stack::end() {
  return top+1;
}

inline bool Stack::empty() const {
  return pos <= 1;
}

inline size_t Stack::size() const {
  return pos > 1;
}

inline void Stack::clear() {
  pos = 1;
  top = &data[0];
}


inline void Transition::find_next(offset_ptr* next, ISlice& key, KeyString& current_key) {
  trace->stack.push_back(next).find(key, current_key);
}


} // namespace leaves

#endif // _LARCH_LEAVES_TRACE_HPP
