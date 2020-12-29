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
  Transition(segment_ptr* pptr, Storage* storage):
    node_ptr(pptr), storage(storage) {}

  void* resolve(segment_ptr ptr) { return ptr.resolve(storage); }

  bool valid() const;
  segment_ptr* find(Slice& key);
  segment_ptr* insert(Slice& key, const Slice& value);
  bool remove(std::string& key, bool last);
  void append_to(std::string& key);
  Slice get_value() const;
  NodeHandler* handler() const { return handlers[node_ptr->type]; }

  segment_ptr* node_ptr;
  segment_ptr* second_ptr;
  Storage* storage;
  char value;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual segment_ptr* find(Transition& self, Slice& key) = 0;
  virtual segment_ptr* insert(Transition& self, Slice& key, const Slice& value) = 0;
  virtual bool remove(Transition& self, std::string& key, bool last) = 0;
  virtual segment_ptr* first(Transition& self) { return self.node_ptr; }
  virtual segment_ptr* last(Transition& self) { return self.node_ptr; }
  virtual void append_to(Transition& self, std::string& key) {};
  virtual bool valid() const { return false; }
  virtual Slice get_value(const Transition& self) const { return Slice(); }
};


inline segment_ptr* Transition::find(Slice& key) {
  return handler()->find(*this, key);
}

inline segment_ptr* Transition::insert(Slice& key, const Slice& value) {
  return handler()->insert(*this, key, value);
}

inline void Transition::append_to(std::string& key) {
  return handler()->append_to(*this, key);
}

inline bool Transition::valid() const {
  return handler()->valid();
}

inline Slice Transition::get_value() const {
  return handler()->get_value(*this);
}

inline bool Transition::remove(std::string& key, bool last) {
  return handler()->remove(*this, key, last);
}


} // namespace larch_leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
