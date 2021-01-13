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

struct KeyString {
  KeyString() : _size(0) {}

  Slice slice() const { return Slice(_data, _size); }
  size_t size() const { return _size; }
  void push_back(char c) { _data[_size++] = c; }
  void pop_back() { _size--; }
  void resize(size_t size) { _size = size; }

  void append(const char* data, size_t size) {
    memcpy(_data+_size, data, size);
    _size += size;
  }


  char _data[MAX_KEY_SIZE];
  uint16_t _size;
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

  int find(ISlice& key) {
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
  Transition& init(offset_ptr* pptr);
  bool valid() const;
  void find(ISlice& key, KeyString& current_key);
  offset_ptr* next(KeyString& current_key);
  offset_ptr* prev(KeyString& current_key);
  offset_ptr* first(KeyString& current_key);
  offset_ptr* last(KeyString& current_key);

  void find_next(offset_ptr* next, ISlice& key, KeyString& current_key);

  int advance(const Slice& key);
  void insert(const Slice& key, any_ptr val_ptr);
  bool remove();
  NodeHandler* handler() const { return handlers[node->type]; }

  TransitionData lower; // for lower trie

  Trace* trace;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual void find(Transition& self, ISlice& key, KeyString& current_key) = 0;
  virtual offset_ptr* next(Transition& self, KeyString& current_key) = 0;
  virtual offset_ptr* first(Transition& self, KeyString& current_key) = 0;
  virtual offset_ptr* prev(Transition& self, KeyString& current_key) = 0;
  virtual offset_ptr* last(Transition& self, KeyString& current_key) = 0;
  virtual void insert(Transition& self, const Slice& key, any_ptr val_ptr) = 0;
  virtual bool remove(Transition& self) = 0;
  virtual int advance(Transition& self, const Slice& key) = 0;
  virtual bool valid() const { return false; }
  virtual void report(offset_ptr* node, Stats& stats) {}
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
  node = NULL;
  return false;
}

inline Transition& Transition::init(offset_ptr* pptr) {
  node_ptr = pptr;
  node = node_ptr->resolve().node;
  return *this;
}

inline void Transition::find(ISlice& key, KeyString& current_key) {
  handler()->find(*this, key, current_key);
}

inline offset_ptr* Transition::next(KeyString& current_key) {
  return handler()->next(*this, current_key);
}

inline offset_ptr* Transition::first(KeyString& current_key) {
  return handler()->first(*this, current_key);
}

inline offset_ptr* Transition::prev(KeyString& current_key) {
  return handler()->prev(*this, current_key);
}

inline offset_ptr* Transition::last(KeyString& current_key) {
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
