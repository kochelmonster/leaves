// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/leaves.hpp"
#include "storage.hpp"

using std::string;

namespace leaves {

enum NodeTypes {
  kValue = 0,
  kNull,
  kCompressed,
  kTrie,
};

struct Transition;


#pragma pack(2)
struct ValueData {
  segment_ptr prev;
  segment_ptr next;
  uint32_t size;
  char value[];
};
#pragma pack(0)



struct NodeHandler;
struct TrieData;
struct CompressedData;


struct Transition {
  Transition(segment_ptr* pptr, Storage* storage):
    node_ptr(pptr), value(NULL), storage(storage), cmp(0) {}

  void* resolve(segment_ptr ptr) {
    // use only if node_ptr has been resolved
    assert(value != NULL);
    if (ptr.segment_id == node_ptr->segment_id) {
      // we don't need storage
      return (void*)((size_t)value - node_ptr->delta + ptr.delta);
    }
    return ptr.resolve(storage);
  }

  bool valid() const;
  segment_ptr* find(Slice& key, string& current_key);
  segment_ptr* next(string& current_key);
  segment_ptr* prev(string& current_key);
  segment_ptr* first(string& current_key);
  segment_ptr* last(string& current_key);
  int advance(Slice& key);

  segment_ptr* insert(Slice& key, const Slice& value, string& current_key);
  bool remove(bool last);
  Slice get_value() const;
  NodeHandler* handler() const { return handlers[node_ptr->type]; }

  segment_ptr* node_ptr;
  segment_ptr* second_ptr;

  union {
    ValueData *value;
    CompressedData *compressed;
    TrieData *upper;
  };
  TrieData *lower;
  Storage* storage;
  char key;
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
  virtual int advance(Transition& self, Slice& key) = 0;
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


inline int Transition::advance(Slice& key) {
  return handler()->advance(*this, key);
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

} // namespace leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
