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
  if (! stack.size())
    throw NoValidPosition();

  // Lock lock(storage.write_lock());
  sanitize();
  Transition transition(stack.back());
  segment_ptr *next = transition.insert(rest_key, value);
  (*storage.version)++;
  stack.pop_back();
  ifind(Transition(next, &storage));
}

void Trace::remove() {
  if (!valid())
    throw NoValidPosition();

  sanitize();
  bool last = true;
  while(stack.size() && stack.back().remove(current_key, last)) {
    stack.pop_back();
    last = false;
  }

  (*storage.version)++;
}


void Trace::ifind(Transition transition) {
  segment_ptr *next(transition.node_ptr);
  while(true) {
    stack.push_back(transition);
    Transition& active(stack.back());
    next = active.find(rest_key);
    if (!next)
      break;
    active.append_to(current_key);
    transition = Transition(next, &storage);
  }
  version = *storage.version;
}

} // namespace larch_leaves
