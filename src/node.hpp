// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/leaves.hpp"
#include "storage.hpp"

using std::string;

namespace leaves {

struct Transition;

enum NodeTypes {
  kNull,
  kCompressed,
  kTrie,
  kLeaf,
  kValue,
};


struct ISlice : public Slice {
  size_t offset;

  ISlice(): Slice() {}
  ISlice(const Slice& src) : Slice(src.data(), src.size()), offset(0) { }
  ISlice(const ISlice& src) : Slice(src.data(), src.size()), offset(src.offset) { }

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

struct TrieNavigation : public Node {
  offset_ptr next;        // pointer to next in trie
  offset_ptr next_leaf;   // pointer to next leaf (linked list)
  offset_ptr prev_leaf;   // pointer to next prev (linked list)

  void init() {
    type = kNull;
    next = next_leaf = prev_leaf = this;
  }
};


inline char sign(int x) {
  return (x>0)-(x<0);
}


#pragma pack(2)

struct ValueData : public Node {
  uint32_t size;
  char value[];

  size_t size_of() {
    return size + sizeof(ValueData);
  }

  static any_ptr build(Storage* storage, const Slice& value) {
    any_ptr result(storage->allocate(value.size()+sizeof(ValueData)).type(kValue));
    result.value->size = value.size();
    memcpy(result.value->value, value.data(), value.size());
    return result;
  }
};


struct LeafData : public TrieNavigation {
  offset_ptr value;
  uint16_t size;
  char chars[];

  void fill(any_ptr value_, const Slice& key) {
    value = value_;
    size = key.size();
    memcpy(chars, key.data(), key.size());
  }

  any_ptr transform(Storage* storage, const ISlice& rest) {
    return this;
  }

  Slice get_value() const {
    ValueData* node = value.resolve().value;
    return Slice(node->value, node->size);
  }

  static any_ptr build(
        Storage* storage, const ISlice& key, const Slice& value, TrieNavigation* next_) {
    Slice complete(key.complete());
    assert(complete.size() <= 0xFFFF);
    any_ptr result(storage->allocate(complete.size()+sizeof(LeafData)).type(kLeaf));
    result.leaf->fill(ValueData::build(storage, value), complete);

    TrieNavigation* prev = next_->prev_leaf.resolve().navigation;
    result.leaf->next = offset_ptr();
    result.leaf->next_leaf = next_;
    result.leaf->prev_leaf = prev;
    prev->next_leaf = result;
    next_->prev_leaf = result;
    return result;
  }
};


#define MAX_COMPRESSED_LEN  (MAX_POOL_SIZE - sizeof(CompressedData))

struct CompressedData : public Node {
  offset_ptr next;
  unsigned char size;
  char chars[];

  size_t size_of(size_t size) {
    return size+sizeof(CompressedData);
  }

  void fill(any_ptr next_, const Slice& key) {
    next = next_;
    size = key.size();
    memcpy(chars, key.data(), key.size());
  }

  any_ptr transform(Storage* storage, const ISlice& rest) {
    any_ptr result = build(storage, next, rest);
    storage->free(this);
    return result;
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

  offset_ptr* find(ISlice& key);
  TrieNavigation* next();
  int advance(ISlice& key);
  void insert(ISlice& key, const Slice& value, TrieNavigation* next_leaf);
  bool remove(bool end_node);
  NodeHandler* handler() const { return handlers[node->type]; }

  offset_ptr* node_ptr;
  offset_ptr* second_ptr;

  union {
    Node* node;
    ValueData *value;
    CompressedData *compressed;
    TrieData *upper;
    LeafData *leaf;
    TrieNavigation *navigation;
  };
  TrieData *lower;
  Storage* storage;
  char key;
  char cmp;
  static NodeHandler* handlers[];
};


struct NodeHandler {
  virtual offset_ptr* find(Transition& self, ISlice& key) = 0;
  virtual TrieNavigation* next(Transition& self) = 0;
  virtual TrieNavigation* first(any_ptr node) = 0;
  virtual void insert(
    Transition& self, ISlice& key, const Slice& value, TrieNavigation* next_leaf) = 0;
  virtual bool remove(Transition& self, bool end_node) = 0;
  virtual int advance(Transition& self, ISlice& key) = 0;
  TrieNavigation* rfirst(any_ptr next) {
    assert(next.as_int);
    return Transition::handlers[next.node->type]->first(next);
  }
};


inline offset_ptr* Transition::find(ISlice& key) {
  return handler()->find(*this, key);
}

inline TrieNavigation* Transition::next() {
  return handler()->next(*this);
}

inline int Transition::advance(ISlice& key) {
  return handler()->advance(*this, key);
}

inline void Transition::insert(ISlice& key, const Slice& value, TrieNavigation* next_leaf) {
  handler()->insert(*this, key, value, next_leaf);
}

inline bool Transition::remove(bool end_node) {
  return handler()->remove(*this, end_node);
}

} // namespace leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
