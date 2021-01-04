#include "node.hpp"
#include "port.hpp"

namespace leaves {

struct Transition;
struct ISlice;
struct Slice;


#pragma pack(2)
struct TrieData {
  uint16_t bits;
  segment_ptr children[];

  size_t size_of(size_t count);
  int index_of(int bit);
  int index_of_moved(int moved_bit);
  bool full(size_t count);
  segment_ptr* find(int bit);
  segment_ptr* next(uint8_t& bit);
  segment_ptr* first(uint8_t& bit);
  segment_ptr* prev(uint8_t& bit);
  segment_ptr* last(uint8_t& bit);
  void add(int bit, segment_ptr next);
  resolved_ptr insert(Transition& self, segment_ptr* to_me, segment_ptr next, int bit);
  bool remove(Transition& self, segment_ptr* to_me, TrieData** dest, int bit);

  static resolved_ptr create(Storage* storage, segment_ptr next, int bit);
  static resolved_ptr build(Storage* storage, segment_ptr next, char key);
};
#pragma pack(0)


struct Trie : public NodeHandler {
  segment_ptr* find(Transition& self, ISlice& key, string& current_key);
  segment_ptr* ifind(Transition& self, char key);
  segment_ptr* next(Transition& self, string& current_key);
  segment_ptr* first(Transition& self, string& current_key);
  segment_ptr* prev(Transition& self, string& current_key);
  segment_ptr* last(Transition& self, string& current_key);
  int advance(Transition& self, ISlice& key);
  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key);
  bool remove(Transition& self, bool last);
};


inline size_t TrieData::size_of(size_t count) {
  return count * sizeof(segment_ptr) + sizeof(TrieData);
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

inline segment_ptr* TrieData::find(int bit) {
  int moved_bit = 1 << bit;
  return (bits & moved_bit) ? &children[index_of_moved(moved_bit)] : NULL;
}

inline segment_ptr* TrieData::next(uint8_t& bit) {
  uint16_t nbits = bits & (0xFFFF << (bit+1));
  if (nbits) {
    bit = ctz(nbits);
    return &children[index_of(bit)];
  }
  return NULL;
}

inline segment_ptr* TrieData::first(uint8_t& bit) {
  bit = ctz(bits);
  return &children[index_of(bit)];
}

inline segment_ptr* TrieData::prev(uint8_t& bit) {
  if (bit) {
    uint16_t nbits = bits & (0xFFFF >> (16-bit));
    if (nbits) {
      bit = 15 - (clz(nbits) & 0xf);
      return &children[index_of(bit)];
    }
  }
  return NULL;
}

inline segment_ptr* TrieData::last(uint8_t& bit) {
  bit = 15 - (clz(bits) & 0xf);
  return &children[index_of(bit)];
}

} // namespace leaves
