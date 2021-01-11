#ifndef _LARCH_LEAVES_TRIE_HPP
#define _LARCH_LEAVES_TRIE_HPP

#include "node.hpp"
#include "port.hpp"

namespace leaves {

struct Transition;
struct ISlice;
struct Slice;


#pragma pack(2)
struct TrieData : public Node {
  uint16_t bits;
  offset_ptr children[2];  // or more

  size_t size_of(size_t count);
  int index_of(int bit);
  int index_of_moved(int moved_bit);
  bool full(size_t count);
  offset_ptr* find(int bit);
  offset_ptr* next(uint8_t& bit);
  offset_ptr* first(uint8_t& bit);
  offset_ptr* prev(uint8_t& bit);
  offset_ptr* last(uint8_t& bit);
  void add(int bit, any_ptr next);
  any_ptr insert(Transition& self, any_ptr next, int bit);
  bool remove(Transition& self, TrieData** dest, offset_ptr *link, int bit);
  void copy_to(any_ptr dest, size_t count);

  static any_ptr create(Trace* trace, any_ptr next, int bit);
  static any_ptr build(Trace* trace, any_ptr next, char key);
};
#pragma pack(0)


struct Trie : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key, string& current_key);
  offset_ptr* ifind(Transition& self, char key);
  offset_ptr* next(Transition& self, string& current_key);
  offset_ptr* first(Transition& self, string& current_key);
  offset_ptr* prev(Transition& self, string& current_key);
  offset_ptr* last(Transition& self, string& current_key);
  int advance(Transition& self, const Slice& key);
  void insert(Transition& self, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self);
  bool one_branch(Transition& self);
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

inline offset_ptr* TrieData::next(uint8_t& bit) {
  uint16_t nbits = bits & (0xFFFF << (bit+1));
  if (nbits) {
    bit = ctz(nbits);
    return &children[index_of(bit)];
  }
  return NULL;
}

inline offset_ptr* TrieData::first(uint8_t& bit) {
  bit = ctz(bits);
  return &children[index_of(bit)];
}

inline offset_ptr* TrieData::prev(uint8_t& bit) {
  if (bit) {
    uint16_t nbits = bits & (0xFFFF >> (16-bit));
    if (nbits) {
      bit = 15 - (clz(nbits) & 0xf);
      return &children[index_of(bit)];
    }
  }
  return NULL;
}

inline offset_ptr* TrieData::last(uint8_t& bit) {
  bit = 15 - (clz(bits) & 0xf);
  return &children[index_of(bit)];
}

namespace bit {
inline uint8_t upper(uint8_t value) {
  return value >> 4;
}

inline uint8_t lower(uint8_t value) {
  return (value & 0x0F);
}
} // namespace bit

extern Trie trie_handler;

} // namespace leaves
#endif // _LARCH_LEAVES_TRIE_HPP
