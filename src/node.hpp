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
};


struct ISlice : public Slice {
  ISlice(): Slice() {}
  ISlice(const Slice& src) : Slice(src.data(), src.size()) { }

  Slice& iadvance(size_t size) {
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

#define MAX_COMPRESSED_LEN  (MAX_POOL_SIZE - sizeof(CompressedData))

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
    if (cmp == 0 && size_ == size) {
      key.iadvance(size);
      current_key.append(keys, size);
    }
    return cmp;
  }

  void fill(Storage& storage, any_ptr next_, const Slice& key) {
    storage.set_value(next, next_);
    storage.set_value(size, (uint8_t)key.size());
    storage.memcpy(keys, key.data(), key.size());
  }

  static any_ptr build(Trace* trace, any_ptr next, const Slice& key);
};

#pragma pack(0)


struct NodeHandler;
struct TrieData;


struct Transition {
  Transition(offset_ptr* pptr, Trace* trace)
    : node_ptr(pptr), value(node_ptr->resolve().value), trace(trace), cmp(0) {  }

  void set(any_ptr ptr) {
    *node_ptr = ptr;
    node = ptr.node;
  }

  bool valid() const;
  offset_ptr* find(ISlice& key, string& current_key);
  offset_ptr* next(string& current_key);
  offset_ptr* prev(string& current_key);
  offset_ptr* first(string& current_key);
  offset_ptr* last(string& current_key);
  int advance(ISlice& key);
  void insert(ISlice& key, any_ptr val_ptr, string& current_key);
  bool remove();
  Slice get_value() const;
  NodeHandler* handler() const { return handlers[node->type]; }

  template <typename type1, typename type2>
  void set_value(type1& dest, type2 src);
  void memcpy(void* dest, const void* src, size_t size);
  void memmove(void* dest, const void* src, size_t size);
  void memset(void* dest, char val, size_t size);

  offset_ptr* node_ptr;
  offset_ptr* second_ptr;

  union {
    Node* node;
    ValueData *value;
    CompressedData *compressed;
    TrieData *upper;
  };
  TrieData *lower;
  Trace* trace;
  char key;
  char cmp;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual offset_ptr* find(Transition& self, ISlice& key, string& current_key) = 0;
  virtual offset_ptr* next(Transition& self, string& current_key) = 0;
  virtual offset_ptr* first(Transition& self, string& current_key) = 0;
  virtual offset_ptr* prev(Transition& self, string& current_key) = 0;
  virtual offset_ptr* last(Transition& self, string& current_key) = 0;
  virtual void insert(Transition& self, ISlice& key, any_ptr val_ptr, string& current_key) = 0;
  virtual bool remove(Transition& self) = 0;
  virtual int advance(Transition& self, ISlice& key) = 0;
  virtual bool valid() const { return false; }
  virtual Slice get_value(const Transition& self) const { return Slice(); }
};


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


inline int Transition::advance(ISlice& key) {
  return handler()->advance(*this, key);
}

inline void Transition::insert(ISlice& key, any_ptr val_ptr, string& current_key) {
  handler()->insert(*this, key, val_ptr, current_key);
}

inline bool Transition::valid() const {
  return handler()->valid();
}

inline Slice Transition::get_value() const {
  return handler()->get_value(*this);
}

inline bool Transition::remove() {
  return handler()->remove(*this);
}

} // namespace leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
