#include "trace.hpp"


namespace leaves {

void Trace::find(const Slice& key) {
  rest_key = key;

  int same = 0; // index of same
  for(stack_type::iterator i = stack.begin(); i != stack.end(); i++) {
    int add = i->advance(rest_key);
    if (add < 0) {
      stack.erase(++i, stack.end());
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
  stack.back().insert(rest_key, value, ifind_next());
  (*storage.version)++;
  ifind();
}

void Trace::remove() {
  if (!valid())
    throw NoValidPosition();

  sanitize();
  while(stack.size() && stack.back().remove())
    stack.pop_back();

  if (!root->next)
    root->next = root;

  stack.clear();
  current_key.clear();
  (*storage.version)++;
}

TrieNavigation* Trace::ifind_next() {
  for(stack_type::reverse_iterator i = stack.rbegin(); i != stack.rend(); i++) {
    TrieNavigation* next = i->next();
    if (next)
      return next;
  }
  return root;
}

void Trace::ifind() {
  if (stack.empty())
    stack.push_back(Transition(&root->next, &storage));

  version = *storage.version;

  offset_ptr *next;
  while(true) {
    next = stack.back().find(rest_key);
    if (!next)
      break;
    stack.push_back(Transition(next, &storage));
  }

  Transition& active(stack.back());
  if (active.node->type == kLeaf && active.cmp == 0 && rest_key.empty())
    set_cursor(active.leaf);
  else
    set_cursor(root);
}

} // namespace leaves
