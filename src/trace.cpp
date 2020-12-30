#include "trace.hpp"


namespace larch_leaves {

void Trace::find(const Slice& key) {
  // Lock lock(storage.read_lock());
  rest_key = key;
  current_key.clear();
  stack.clear();
  ifind(Transition(&storage.start, &storage));
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

void Trace::next() {
  if (stack.empty())
    throw NoValidPosition();

  segment_ptr *next;
  while(stack.size()) {
    next = stack.back().next(current_key);
    if (next)
      break;
    stack.pop_back();
  }

  while(next) {
    stack.push_back(Transition(next, &storage));
    next = stack.back().first(current_key);
  }
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
