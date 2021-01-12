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
  offset_ptr* find(TransitionData& self);
  offset_ptr* next(TransitionData& self);
  offset_ptr* first(TransitionData& self);
  offset_ptr* prev(TransitionData& self);
  offset_ptr* last(TransitionData& self);
  void add(int bit, any_ptr next, size_t count);
  void insert(TransitionData& self, Trace* trace, any_ptr next);
  bool remove(TransitionData& self, Trace* trace);
  bool one_branch(TransitionData& self);
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
  char to_char(Transition& self);
};

inline char Trie::to_char(Transition& self) {
  return (self.key << 4) | self.lower.key;
}

inline bool Trie::one_branch(Transition& self) {
  return self.lower.trie->one_branch(self.lower) && self.trie->one_branch(self);
}

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

inline bool TrieData::one_branch(TransitionData& self) {
  if (popcount(bits) == 1) {
    self.key = ctz(bits);
    return true;
  }
  return false;
}

inline offset_ptr* TrieData::find(TransitionData& self) {
  int moved_bit = 1 << self.key;
  self.cmp = !(bits & moved_bit);
  return self.cmp ? NULL : &children[self.index=index_of_moved(moved_bit)];
}

inline offset_ptr* TrieData::next(TransitionData& self) {
  uint16_t nbits = bits & (0xFFFF << (self.key+1));
  if (nbits) {
    self.key = ctz(nbits);
    if (self.cmp) {
      self.index = index_of(self.key);
      self.cmp = 0;
    }
    else
      self.index++;
    return &children[self.index];
  }
  return NULL;
}

inline offset_ptr* TrieData::first(TransitionData& self) {
  self.key = ctz(bits);
  self.cmp = 0;
  self.index = 0;
  return &children[0];
}

inline offset_ptr* TrieData::prev(TransitionData& self) {
  if (self.key) {
    uint16_t nbits = bits & (0xFFFF >> (16-self.key));
    if (nbits) {
      self.key = 15 - (clz(nbits) & 0xf);
      if (self.cmp) {
        self.index = index_of(self.key);
        self.cmp = 0;
      }
      else
        self.index--;
      return &children[self.index];
    }
  }
  return NULL;
}

inline offset_ptr* TrieData::last(TransitionData& self) {
  self.key = 15 - (clz(bits) & 0xf);
  self.cmp = 0;
  self.index = popcount(bits)-1;
  return &children[self.index];
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
