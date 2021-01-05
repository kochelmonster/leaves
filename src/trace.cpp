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
    same += add;
  }
  ifind();
}

void Trace::set_value(const Slice& value) {
  if (stack.empty())
    throw NoValidPosition();

  sanitize();
  Transition& transition(stack.back());
  transition.insert(rest_key, value, current_key);
  (*storage.version)++;
  ifind();
}

void Trace::remove() {
  if (!valid())
    throw NoValidPosition();

  sanitize();
  bool last = true;
  while(stack.size() && stack.back().remove(last)) {
    stack.pop_back();
    last = false;
  }
  stack.clear();
  current_key.clear();
  (*storage.version)++;
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
  stack.push_back(Transition(storage.start, &storage));
  while(true) {
      offset_ptr *next = move(stack.back(), current_key);
      if (!next)
        break;
      stack.push_back(Transition(next, &storage));
  }
  version = *storage.version;
}

void Trace::imove(move_func_t move, move_func_t move_end) {
  if (stack.empty())
    throw NoValidPosition();

  sanitize();
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
      stack.push_back(Transition(next, &storage));
      next = move_end(stack.back(), current_key);
    }
  version = *storage.version;
}

void Trace::ifind() {
  if (stack.empty())
    stack.push_back(Transition(storage.start, &storage));

  offset_ptr *next;
  while(true) {
    Transition& active(stack.back());
    next = active.find(rest_key, current_key);
    if (!next)
      break;
    stack.push_back(Transition(next, &storage));
  }
  version = *storage.version;
}

} // namespace leaves
