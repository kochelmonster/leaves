#include "node.hpp"
#include "port.hpp"

namespace leaves {

struct Transition;
struct ISlice;
struct Slice;


#pragma pack(2)
struct TrieData : public Node {
  uint16_t bits;
  offset_ptr children[];

  size_t size_of(size_t count);
  int index_of(int bit);
  int index_of_moved(int moved_bit);
  bool full(size_t count);
  offset_ptr* find(int bit);
  any_ptr next(uint8_t bit);
  any_ptr first();
  void add(int bit, any_ptr next);
  any_ptr insert(Transition& self, any_ptr next, int bit);
  bool remove(Transition& self, TrieData** dest, offset_ptr *link, int bit);
  void copy_to(any_ptr dest, size_t count);

  static any_ptr create(Storage* storage, any_ptr next, int bit);
  static any_ptr build(Storage* storage, any_ptr next, char key);
};
#pragma pack(0)


struct Trie : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key);
  offset_ptr* ifind(Transition& self, char key);
  TrieNavigation* next(Transition& self);
  TrieNavigation* first(any_ptr node);
  int advance(Transition& self, ISlice& key);
  void insert(Transition& self, ISlice& key, const Slice& value, TrieNavigation* next_leaf);
  bool remove(Transition& self);
};


inline size_t TrieData::size_of(size_t count) {
  return count * sizeof(offset_ptr) + sizeof(TrieData);
}

inline int TrieData::index_of(int bit) {
  return index_of_moved(1<<bit);
}

inline int TrieData::index_of_moved(int moved_bit) {
  return popcount(bits & (moved_bit-1));
}

inline bool TrieData::full(size_t count) {
  switch(count) {
    case 2:
    case 5:
    case 10:
      return true;
  }
  return false;
}

inline offset_ptr* TrieData::find(int bit) {
  int moved_bit = 1 << bit;
  return (bits & moved_bit) ? &children[index_of_moved(moved_bit)] : NULL;
}

inline any_ptr TrieData::next(uint8_t bit) {
  uint16_t nbits = bits & (0xFFFF << (bit+1));
  if (nbits) {
    bit = ctz(nbits);
    return children[index_of(bit)].resolve();
  }
  return NULL;
}

inline any_ptr TrieData::first() {
  return children[0].resolve();
}

} // namespace leaves
