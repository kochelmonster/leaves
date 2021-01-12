// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/leaves.hpp"
#include "storage.hpp"

using std::string;

namespace leaves {

struct Transition;
struct Trace;

enum NodeTypes {
  kValue = 0,
  kNull,
  kCompressed,
  kTrie,
  kTable,
};


struct ISlice : public Slice {
  ISlice(): Slice() {}
  ISlice(const Slice& src) : Slice(src) { }
  ISlice(const char *data, size_t size) : Slice(data, size) { }

  Slice& iadvance(size_t size) {
    size = std::min(size, _size);
    _data += size;
    _size -= size;
    return *this;
  }
};


#pragma pack(2)

struct ValueData : public Node {
  offset_ptr next;
  uint32_t size;
  char value[];

  size_t size_of() {
    return size + sizeof(ValueData);
  }

  static any_ptr build(Trace* trace, const Slice& value);
};

inline char sign(int x) {
  return (x>0)-(x<0);
}

#define MAX_COMPRESSED_LEN  255

struct CompressedData : public Node {
  offset_ptr next;
  uint8_t size;
  char keys[];

  size_t size_of(size_t size) {
    return size+sizeof(CompressedData);
  }

  int find(ISlice& key, string& current_key) {
    size_t size_ = std::min(key.size(), (size_t)size);
    int cmp = sign(memcmp(key.data(), keys, size));
    if (cmp == 0) {
      return sign(size_-size);
    }
    return cmp;
  }

  void fill(any_ptr next_, const Slice& key) {
    next = next_;
    size = (uint8_t)key.size();
    memcpy(keys, key.data(), key.size());
  }

  void eat_child(Transition& self);
  static any_ptr build(Trace* trace, any_ptr next, const Slice& key);
};

#pragma pack(0)


struct NodeHandler;
struct TrieData;
struct TableData;

struct TransitionData {
  void set(any_ptr ptr);
  bool set(offset_ptr* pptr);

  offset_ptr* node_ptr;
  union {
    Node* node;
    ValueData *value;
    CompressedData *compressed;
    TrieData *trie;
    TableData *table;
  };
  char key;
  char cmp;
  int16_t index;
};

struct Transition : public TransitionData {
  void init(offset_ptr* pptr);
  bool valid() const;
  offset_ptr* find(ISlice& key, string& current_key);
  offset_ptr* next(string& current_key);
  offset_ptr* prev(string& current_key);
  offset_ptr* first(string& current_key);
  offset_ptr* last(string& current_key);
  int advance(const Slice& key);
  void insert(const Slice& key, any_ptr val_ptr);
  bool remove();
  NodeHandler* handler() const { return handlers[node->type]; }

  TransitionData lower; // for lower trie

  Trace* trace;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual offset_ptr* find(Transition& self, ISlice& key, string& current_key) = 0;
  virtual offset_ptr* next(Transition& self, string& current_key) = 0;
  virtual offset_ptr* first(Transition& self, string& current_key) = 0;
  virtual offset_ptr* prev(Transition& self, string& current_key) = 0;
  virtual offset_ptr* last(Transition& self, string& current_key) = 0;
  virtual void insert(Transition& self, const Slice& key, any_ptr val_ptr) = 0;
  virtual bool remove(Transition& self) = 0;
  virtual int advance(Transition& self, const Slice& key) = 0;
  virtual bool valid() const { return false; }
};


inline void TransitionData::set(any_ptr ptr) {
  if (ptr.as_int) {
    *node_ptr = ptr;
    node = ptr.node;
  }
}

inline bool TransitionData::set(offset_ptr* pptr) {
  node_ptr = pptr;
  if (pptr) {
    node = node_ptr->resolve().node;
    return true;
  }
  return false;
}

inline void Transition::init(offset_ptr* pptr) {
  node_ptr = pptr;
  node = node_ptr->resolve().node;
}

inline offset_ptr* Transition::find(ISlice& key, string& current_key) {
  return handler()->find(*this, key, current_key);
}

inline offset_ptr* Transition::next(string& current_key) {
  return handler()->next(*this, current_key);
}

inline offset_ptr* Transition::first(string& current_key) {
  return handler()->first(*this, current_key);
}

inline offset_ptr* Transition::prev(string& current_key) {
  return handler()->prev(*this, current_key);
}

inline offset_ptr* Transition::last(string& current_key) {
  return handler()->last(*this, current_key);
}


inline int Transition::advance(const Slice& key) {
  return handler()->advance(*this, key);
}

inline void Transition::insert(const Slice& key, any_ptr val_ptr) {
  handler()->insert(*this, key, val_ptr);
}

inline bool Transition::valid() const {
  return handler()->valid();
}

inline bool Transition::remove() {
  return handler()->remove(*this);
}

} // namespace leaves
#endif // _LARCH_LEAVES_NODE_HPP
