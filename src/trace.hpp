#ifndef _LARCH_LEAVES_TRACE_HPP
#define _LARCH_LEAVES_TRACE_HPP

#include "storage.hpp"
#include "node.hpp"


namespace leaves {

struct Trace;

struct Stack {
  typedef std::vector<Transition> stack_type;

  Stack(Trace& trace);
  bool empty() const;
  Transition* begin();
  Transition* end();
  Transition& push_back(offset_ptr* next);
  Transition& pop_back();
  Transition& back();
  const Transition& back() const;
  Transition& back(int index);
  void erase(Transition* iter);
  void grow();
  void clear();
  Transition& restart();

  Trace& trace;
  offset_ptr null;
  size_t pos;
  Transition* top;
  stack_type data;
};

struct Trace {
  Trace(Storage& storage, offset_ptr* root_=NULL);
  bool valid() const { return rest_key.empty() && stack.back().node->type == kValue; }
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void set_value(const Slice& value);
  Slice get_value() const;
  void remove();
  Transition& restart();

  any_ptr allocate(size_t size);
  void free(any_ptr ptr);

  any_ptr ipop_value();
  void iinsert(any_ptr val_ptr);
  void ifind();
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
  version = storage.header->version;
}

inline void Trace::iinsert(any_ptr val_ptr) {
  stack.back().insert(rest_key, val_ptr);
  ifind();
}

inline Transition& Trace::restart() {
  if (stack.empty())
    stack.push_back(root);
  return stack.restart();
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

inline Transition& Stack::pop_back() {
  assert(pos>1);
  pos--;
  return *--top;
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

inline void Stack::clear() {
  pos = 1;
  top = &data[0];
}

inline Transition& Stack::restart() {
  pos = 2;
  top = &data[1];
  return *top;
}

inline void Transition::child_find(offset_ptr* child, ISlice& key, KeyString& current_key) {
  trace->stack.push_back(child).find(key, current_key);
}

inline void Transition::parent_next(KeyString& current_key) {
  trace->stack.pop_back().next(current_key);
}

inline void Transition::parent_prev(KeyString& current_key) {
  trace->stack.pop_back().prev(current_key);
}

inline void Transition::child_first(offset_ptr* child, KeyString& current_key) {
  trace->stack.push_back(child).first(current_key);
}

inline void Transition::child_last(offset_ptr* child, KeyString& current_key) {
  trace->stack.push_back(child).last(current_key);
}



} // namespace leaves

#endif // _LARCH_LEAVES_TRACE_HPP
