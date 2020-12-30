// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/larch/leaves.hpp"
#include "storage.hpp"

using std::string;

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
    node_ptr(pptr), storage(storage), cmp(0) {}

  void* resolve(segment_ptr ptr) { return ptr.resolve(storage); }

  bool valid() const;
  segment_ptr* find(Slice& key, string& current_key);
  segment_ptr* next(string& current_key);
  segment_ptr* prev(string& current_key);
  segment_ptr* first(string& current_key);
  segment_ptr* last(string& current_key);

  segment_ptr* insert(Slice& key, const Slice& value, string& current_key);
  bool remove(bool last);
  Slice get_value() const;
  NodeHandler* handler() const { return handlers[node_ptr->type]; }

  segment_ptr* node_ptr;
  segment_ptr* second_ptr;
  Storage* storage;
  char value;
  char cmp;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual segment_ptr* find(Transition& self, Slice& key, string& current_key) = 0;
  virtual segment_ptr* next(Transition& self, string& current_key) = 0;
  virtual segment_ptr* first(Transition& self, string& current_key) = 0;
  virtual segment_ptr* prev(Transition& self, string& current_key) = 0;
  virtual segment_ptr* last(Transition& self, string& current_key) = 0;
  virtual segment_ptr* insert(
    Transition& self, Slice& key, const Slice& value, string& current_key) = 0;
  virtual bool remove(Transition& self, bool last) = 0;
  //virtual segment_ptr* last(Transition& self) { return self.node_ptr; }
  virtual bool valid() const { return false; }
  virtual Slice get_value(const Transition& self) const { return Slice(); }
};


inline segment_ptr* Transition::find(Slice& key, string& current_key) {
  return handler()->find(*this, key, current_key);
}

inline segment_ptr* Transition::next(string& current_key) {
  return handler()->next(*this, current_key);
}

inline segment_ptr* Transition::first(string& current_key) {
  return handler()->first(*this, current_key);
}

inline segment_ptr* Transition::prev(string& current_key) {
  return handler()->prev(*this, current_key);
}

inline segment_ptr* Transition::last(string& current_key) {
  return handler()->last(*this, current_key);
}

inline segment_ptr* Transition::insert(Slice& key, const Slice& value, string& current_key) {
  return handler()->insert(*this, key, value, current_key);
}

inline bool Transition::valid() const {
  return handler()->valid();
}

inline Slice Transition::get_value() const {
  return handler()->get_value(*this);
}

inline bool Transition::remove(bool last) {
  return handler()->remove(*this, last);
}


} // namespace larch_leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
