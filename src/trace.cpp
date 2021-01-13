#include "trace.hpp"


namespace leaves {


Trace::Trace(Storage& storage, offset_ptr* root_)
      : storage(storage), stack(*this), root(root_), version(storage.header->version) {
  if (!root_)
    root = &storage.header->root;
}


void Trace::find(const Slice& key) {
#ifndef PURE_TRIE
  if (key.size() > storage.max_key_size)
    throw WrongValue("keysize too big");
#endif

  rest_key = key;

  update();

  int same = 0; // index of same
  for(Transition* i = stack.begin(); i != stack.end(); i++) {
    int add = i->advance(rest_key);
    if (add < 0) {
      stack.erase(++i);
      current_key.resize(same);
      ifind();
      return;
    }
    else
      rest_key.iadvance(add);
    same += add;
  }
  if (stack.empty())
    stack.push_back(root);
  ifind();
}

void Trace::set_value(const Slice& value) {
  update();
  if (stack.empty())
    throw NoValidPosition();

  iinsert(ValueData::build(this, value));
}

void Trace::remove() {
  if (!valid())
    throw NoValidPosition();

  update();
  storage.free(ipop_value());
  current_key.resize(0);
}

any_ptr Trace::ipop_value() {
  Transition& end(stack.back());
  ValueData* result = end.value;
  assert(result->type == kValue);
  *end.node_ptr = result->child;
  stack.pop_back();
  while(!stack.empty() && stack.back().remove())
    stack.pop_back();
  stack.clear();
  if (!*root)
    *root = &storage.header->null;
  return result;
}

void Trace::first() {
  version = storage.header->version;
  rest_key.clear();
  current_key.resize(0);
  restart().first(current_key);
}

void Trace::last() {
  version = storage.header->version;
  rest_key.clear();
  current_key.resize(0);
  restart().last(current_key);
}

void Trace::next() {
  update();
  rest_key.clear();
  stack.back().next(current_key);
}

void Trace::prev() {
  update();
  rest_key.clear();
  stack.back().prev(current_key);
}

void Trace::ifind() {
  stack.back().find(rest_key, current_key);
}

} // namespace leaves
