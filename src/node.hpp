// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/leaves.hpp"
#include "storage.hpp"

using std::string;

namespace leaves {

struct Transition;

#pragma pack(2)
struct NodeBase {
  uint8_t flags;
  uint8_t reserved;
};
#pragma pack(0)

#pragma pack(2)
struct Leaf {
  segment_ptr value;
  segment_ptr prev;
  segment_ptr next;
  uint16_t size;
  char key;
};
#pragma pack(0)




/*
null?   0

trie    1
bittrie 2

compress 3
Leaf     4
value    5



4*4*4*4

clz(next_power(size-1))

2   2+2*6   16   8*2     16*1      / 2  0    1..16   0..15   16
5   2+5*6   32   8*4     16*2      / 2  1    17..32  16..31  32
10  2+10*6  62   8*8     16*4      / 2  2
16  16*6    96   8*12    16*6(7)   / 2  3



clz()

*/

#pragma pack(2)
struct ValueData {
  segment_ptr next;
  uint32_t size;
  char value[];

  size_t size_of() {
    return size + sizeof(ValueData);
  }

  static resolved_ptr build(Storage* storage, segment_ptr next, const Slice& value) {
    resolved_ptr result(storage->allocate(value.size()+sizeof(ValueData)));
    result.value->size = value.size();
    memcpy(result.value->value, value.data(), value.size());
    result.value->next = next;
    return result;
  }
};
#pragma pack(0)

#define MAX_COMPRESSED_LEN (MAX_POOL_SIZE - sizeof(CompressedData))


#pragma pack(1)
struct CompressedData {
  segment_ptr next;
  unsigned char size;
  char keys[];

  size_t size_of(size_t size) {
    return size+sizeof(CompressedData);
  }

  void free(Transition& self);

  void fill(segment_ptr next_, const Slice& key) {
    next = next_;
    size = (unsigned char)key.size();
    memcpy(keys, key.data(), key.size());
  }

  static resolved_ptr build(Storage* storage, const resolved_ptr& next, const Slice& key) {
    if (key.empty())
      return next;

    if (key.size() > MAX_COMPRESSED_LEN) {
      // divide key in multiple compressed
      Slice first(key.data(), MAX_COMPRESSED_LEN);
      Slice second(key.advance(MAX_COMPRESSED_LEN));
      return build(storage, build(storage, next, second), first);
    }

    resolved_ptr result(storage->allocate(key.size()+sizeof(CompressedData)).type(kCompressed));
    result.compressed->fill(next.me, key);
    return result;
  }

};
#pragma pack(0)



struct ISlice : public Slice {
  ISlice(): Slice() {}
  ISlice(const Slice& src) : Slice(src.data(), src.size()) { }

  Slice& iadvance(size_t size) {
    _data += size;
    _size -= size;
    return *this;
  }
};


struct NodeHandler;
struct TrieData;


struct Transition {
  Transition(segment_ptr* pptr, Storage* storage)
    : node_ptr(pptr), value(node_ptr->resolve(storage).value), storage(storage), cmp(0) {  }

  Transition(resolved_ptr* pptr, Storage* storage)
    : node_ptr(&pptr->me), value(pptr->value), storage(storage), cmp(0) {  }

  void set(segment_ptr ptr) {
    *node_ptr = ptr;
    value = ptr.resolve(storage).value;
  }

  void set(const resolved_ptr& ptr) {
    *node_ptr = ptr.me;
    value = ptr.value;
  }

  resolved_ptr resolve(segment_ptr ptr) {
    if (ptr.segment_id == node_ptr->segment_id) {
      // we don't need storage
      return resolved_ptr(ptr, (void*)((size_t)value + ((ptr.delta - node_ptr->delta) << 3)));
    }
    return ptr.resolve(storage);
  }

  bool valid() const;
  segment_ptr* find(ISlice& key, string& current_key);
  segment_ptr* next(string& current_key);
  segment_ptr* prev(string& current_key);
  segment_ptr* first(string& current_key);
  segment_ptr* last(string& current_key);
  int advance(ISlice& key);
  void insert(ISlice& key, const Slice& value, string& current_key);
  bool remove(bool last);
  Slice get_value() const;
  NodeHandler* handler() const { return handlers[node_ptr->type]; }

  segment_ptr* node_ptr;
  segment_ptr* second_ptr;

  union {
    NodeBase *node;
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
  virtual segment_ptr* find(Transition& self, ISlice& key, string& current_key) = 0;
  virtual segment_ptr* next(Transition& self, string& current_key) = 0;
  virtual segment_ptr* first(Transition& self, string& current_key) = 0;
  virtual segment_ptr* prev(Transition& self, string& current_key) = 0;
  virtual segment_ptr* last(Transition& self, string& current_key) = 0;
  virtual void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) = 0;
  virtual bool remove(Transition& self, bool last) = 0;
  virtual int advance(Transition& self, ISlice& key) = 0;
  virtual bool valid() const { return false; }
  virtual Slice get_value(const Transition& self) const { return Slice(); }
};


inline segment_ptr* Transition::find(ISlice& key, string& current_key) {
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


inline void CompressedData::free(Transition& self) {
  self.storage->free(resolved_ptr(*self.node_ptr, this), size_of(size));
}


} // namespace leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
