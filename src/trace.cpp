#include "trace.hpp"


namespace leaves {


offset_ptr* ifirst(Transition& transition, string& current_key) {
  return transition.first(current_key);
}

offset_ptr* ilast(Transition& transition, string& current_key) {
  return transition.last(current_key);
}

offset_ptr* inext(Transition& transition, string& current_key) {
  return transition.next(current_key);
}

offset_ptr* iprev(Transition& transition, string& current_key) {
  return transition.prev(current_key);
}


Trace::Trace(Storage& storage, offset_ptr* root_)
      : storage(storage), root(root_), version(*storage.version) {
  current_key.reserve(1024);
  stack.reserve(1024);
  if (!root_)
    root = storage.root;
}


void Trace::find(const Slice& key) {
  rest_key = key;

  int same = 0; // index of same
  for(stack_type::iterator i = stack.begin(); i != stack.end(); i++) {
    int add = i->advance(rest_key);
    if (add < 0) {
      stack.erase(++i, stack.end());
      current_key.resize(same);
      break;
    }
    else
      rest_key.iadvance(add);
    same += add;
  }
  ifind();
}

void Trace::set_value(const Slice& value) {
  if (stack.empty())
    throw NoValidPosition();

  update();
  iinsert(ValueData::build(this, value));
  (*storage.version)++;
  ifind();
}

void Trace::remove() {
  if (!valid())
    throw NoValidPosition();

  update();

  storage.free(ipop_value());
  current_key.clear();

  (*storage.version)++;
}

any_ptr Trace::ipop_value() {
  Transition &end(stack.back());
  ValueData* result = end.value;
  assert(result->type == kValue);
  *end.node_ptr = result->next;
  stack.pop_back();
  while(stack.size() && stack.back().remove())
    stack.pop_back();
  stack.clear();
  if (!*root)
    *root = storage.null;
  return result;
}

void Trace::first() {
  imove_end(ifirst);
}

void Trace::last() {
  imove_end(ilast);
}

void Trace::next() {
  imove(inext, ifirst);
}

void Trace::prev() {
  imove(iprev, ilast);
}

void Trace::imove_end(move_func_t move) {
  rest_key = Slice();
  current_key.clear();
  stack.clear();
  stack.push_back(Transition(root, this));
  while(true) {
      offset_ptr *next = move(stack.back(), current_key);
      if (!next)
        break;
      stack.push_back(Transition(next, this));
  }
  version = *storage.version;
}

void Trace::imove(move_func_t move, move_func_t move_end) {
  if (stack.empty())
    throw NoValidPosition();

  update();
  rest_key = Slice();
  offset_ptr *next;
  while(stack.size()) {
    next = move(stack.back(), current_key);
    if (next)
      break;
    stack.pop_back();
  }

  if (next && next != stack.back().node_ptr)  // see node.cpp Value::prev
    while(next) {
      stack.push_back(Transition(next, this));
      next = move_end(stack.back(), current_key);
    }
  version = *storage.version;
}

void Trace::ifind() {
  if (stack.empty())
    stack.push_back(Transition(root, this));

  offset_ptr *next;
  while(true) {
    next = stack.back().find(rest_key, current_key);
    if (!next)
      break;
    stack.push_back(Transition(next, this));
  }
  version = *storage.version;
}

} // namespace leaves
