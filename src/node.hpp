// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/larch/leaves.hpp"
#include "storage.hpp"

namespace larch_leaves {

enum NodeTypes {
  kValue = 0,
  kNull,
  kCompressed,
  kTrie,
};

struct NodeHandler;

struct Transition {
  Transition() {}

  Transition(segment_ptr* pptr, Storage* storage):
    node_ptr(pptr), storage(storage), handler(handlers[pptr->type]) {}

  void* resolve(segment_ptr ptr) {
    return ptr.resolve(storage);
  }

  segment_ptr* find(Slice& key);
  segment_ptr* insert(Slice& key, Slice& value);

  segment_ptr* node_ptr;
  segment_ptr* second_ptr;
  Storage *storage;
  NodeHandler* handler;
  char value;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual segment_ptr* find(Transition& self, Slice& key) = 0;
  virtual segment_ptr* insert(Transition& self, Slice& key, Slice& value) = 0;
  virtual segment_ptr* first(Transition& self) { return self.node_ptr; }
  virtual segment_ptr* last(Transition& self) { return self.node_ptr; }
};


inline segment_ptr* Transition::find(Slice& key) {
  return handler->find(*this, key);
}

inline segment_ptr* Transition::insert(Slice& key, Slice& value) {
  return handler->insert(*this, key, value);
}


} // namespace larch_leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
