#include "trace.hpp"


namespace larch_leaves {


segment_ptr* ifirst(Transition& transition, string& current_key) {
  return transition.first(current_key);
}

segment_ptr* ilast(Transition& transition, string& current_key) {
  return transition.last(current_key);
}

segment_ptr* inext(Transition& transition, string& current_key) {
  return transition.next(current_key);
}

segment_ptr* iprev(Transition& transition, string& current_key) {
  return transition.prev(current_key);
}


void Trace::find(const Slice& key) {
  // Lock lock(storage.read_lock());
  rest_key = key;
  current_key.clear();
  stack.clear();
  ifind(Transition(storage.start, &storage));
}

void Trace::set_value(const Slice& value) {
  if (stack.empty())
    throw NoValidPosition();

  // Lock lock(storage.write_lock());
  sanitize();
  Transition transition(stack.back());
  segment_ptr *next = transition.insert(rest_key, value, current_key);
  (*storage.version)++;
  stack.pop_back();
  ifind(Transition(next, &storage));
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
      segment_ptr *next = move(stack.back(), current_key);
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
  segment_ptr *next;
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


void Trace::ifind(Transition transition) {
  segment_ptr *next(transition.node_ptr);
  while(true) {
    stack.push_back(transition);
    Transition& active(stack.back());
    next = active.find(rest_key, current_key);
    if (!next)
      break;
    transition = Transition(next, &storage);
  }
  version = *storage.version;
}

} // namespace larch_leaves
