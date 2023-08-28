#include <trace.hpp>

namespace leaves {


void Trace::refresh() {
  std::string key = current_key;
  key.append(rest_key.data(), rest_key.size());
  find(key);
}

void Trace::find(const Slice& key) {
  rest_key = key;
  current_key.clear();
  stack.clear();

  Page* start = storage.page(0, writing);
  location_p &root = start->header.root;
  location_p root_pos = location_p::b(0, start->offset(&root), 0);
  stack.push_back(Transition(start->header.root, root_pos));
  while (handler()->find(*this));
}

void Trace::set_value(const Slice& value) {
  writing = true;
  handler()->insert(*this, value);
  
  if (storage.rearrange_pages()) {
    refresh();
  }
  else {
    while (handler()->find(*this));
  }
}

Slice Trace::get_value() const {
  if (valid()) {
    Location loc = back();
    assert(loc.loc.type == kEndLeaf);
    if (writing && loc.node->endleaf.size > BIG_VALUE) {
      loc.page = storage.page(loc.loc.page, false);
      loc.node = loc.page->node(0);
    }

    return Slice(loc.node->endleaf.data, loc.node->endleaf.size);
  }
  return Slice();
}


void Trace::remove() {
  
}

void Trace::commit() {
  storage.flush();
  writing = false;
}

void Trace::first() {

}

void Trace::last() {

}

void Trace::next() {

}

void Trace::prev() {

}


} // namespace leaves