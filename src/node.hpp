// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/leaves.hpp"
#include "storage.hpp"

using std::string;

namespace leaves {

struct Transition;

enum NodeTypes {
  kValue = 0,
  kNull,
  kCompressed,
  kTrie,
};


struct ISlice : public Slice {
  size_t offset;

  ISlice(): Slice() {}
  ISlice(const Slice& src) : Slice(src.data(), src.size()), offset(0) { }

  Slice& iadvance(size_t size) {
    offset += size;
    _data += size;
    _size -= size;
    return *this;
  }

  Slice complete() const {
    return Slice(_data-offset, _size+offset);
  }
};


#pragma pack(2)

struct Leaf : public Node {
  uint16_t size;
  offset_ptr value;
  offset_ptr prev;
  offset_ptr next;
  char key;
};

struct ValueData : public Node {
  offset_ptr next;
  uint32_t size;
  char value[];

  size_t size_of() {
    return size + sizeof(ValueData);
  }

  static any_ptr build(Storage* storage, const offset_ptr& next, const Slice& value) {
    any_ptr result(storage->allocate(value.size()+sizeof(ValueData)).type(kValue));
    result.value->size = value.size();
    memcpy(result.value->value, value.data(), value.size());
    result.value->next = next;
    return result;
  }
};

inline char sign(int x) {
  return (x>0)-(x<0);
}

#define MAX_COMPRESSED_LEN  (MAX_POOL_SIZE - sizeof(CompressedData))

struct CompressedData : public Node {
  offset_ptr next;
  unsigned char size;
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

  void fill(any_ptr next_, const Slice& key) {
    next = next_;
    size = (unsigned char)key.size();
    memcpy(keys, key.data(), key.size());
  }

  static any_ptr build(Storage* storage, any_ptr next, const Slice& key) {
    if (key.empty())
      return next;

    if (key.size() > MAX_COMPRESSED_LEN) {
      // divide key in multiple compressed
      Slice first(key.data(), MAX_COMPRESSED_LEN);
      Slice second(key.advance(MAX_COMPRESSED_LEN));
      return build(storage, build(storage, next, second), first);
    }

    any_ptr result(storage->allocate(key.size()+sizeof(CompressedData)).type(kCompressed));
    result.compressed->fill(next, key);
    return result;
  }

};


#pragma pack(0)


struct NodeHandler;
struct TrieData;


struct Transition {
  Transition(offset_ptr* pptr, Storage* storage)
    : node_ptr(pptr), value(node_ptr->resolve().value), storage(storage), cmp(0) {  }

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
  void insert(ISlice& key, const Slice& value, string& current_key);
  bool remove(bool last);
  Slice get_value() const;
  NodeHandler* handler() const { return handlers[node->type]; }

  offset_ptr* node_ptr;
  offset_ptr* second_ptr;

  union {
    Node* node;
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
  virtual offset_ptr* find(Transition& self, ISlice& key, string& current_key) = 0;
  virtual offset_ptr* next(Transition& self, string& current_key) = 0;
  virtual offset_ptr* first(Transition& self, string& current_key) = 0;
  virtual offset_ptr* prev(Transition& self, string& current_key) = 0;
  virtual offset_ptr* last(Transition& self, string& current_key) = 0;
  virtual void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) = 0;
  virtual bool remove(Transition& self, bool last) = 0;
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

inline void Transition::insert(ISlice& key, const Slice& value, string& current_key) {
  handler()->insert(*this, key, value, current_key);
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
