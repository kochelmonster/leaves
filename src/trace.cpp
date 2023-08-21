#include <trace.hpp>

namespace leaves {

void Trace::find(const Slice& key) {
  rest_key = key;
  current_key.clear();
  stack.clear();

  Page* start = storage.start;
  location_p &root = start->header.root;
  location_p root_pos = location_p::b(0, start->offset(&root));
  stack.push_back(Transition(start->header.root, root_pos));
  while (handler()->find(*this));
}

void Trace::set_value(const Slice& value) {
  writing = true;
  handler()->insert(*this, value);
  while (handler()->find(*this));
}

Slice Trace::get_value() const {
  if (valid()) {
    Location loc = back();
    assert(loc.loc.type == kEndLeaf);
    return Slice(loc.node->endleaf.data, loc.node->endleaf.size);
  }
  return Slice();
}


void Trace::commit() {
  storage.flush();
  writing = false;
}

} // namespace leaves