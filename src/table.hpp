#ifndef _LARCH_LEAVES_TABLE_HPP
#define _LARCH_LEAVES_TABLE_HPP

#include "node.hpp"

namespace leaves {

struct Transition;
struct ISlice;
struct Slice;

/*
  The Table layout is
  +--------------------------------+
  | key_fragment1 (16bytes)        |
  +--------------------------------+
  | key_fragment2 (16bytes)        |
  +--------------------------------+
  |                                |
  |       free_space               |
  |                                |
  +--------------------------------+
  | node_ptr for fragmen2 (16bytes)|
  +--------------------------------+
  | node_ptr for fragmen1 (16bytes)|
  +--------------------------------+

key_fragments grow from top to bottom
node_pts grow from bottom to top
*/

#pragma pack(8)
struct TableData : public Node {
  uint16_t padding;
  uint16_t count;
  uint16_t bottom;
  union Item {
    offset_ptr ptr;
    char fragment[16];
  } data[];

  void insert(Transition& self, ISlice& key, any_ptr val_ptr);
  bool remove(Transition& self, int index);
  int find(ISlice& key, int* index);
  int find(ISlice& key, int* index, int bottom);
  int advance(ISlice& key, int index);

  offset_ptr* next(Transition& self);
  offset_ptr* prev(Transition& self);
  offset_ptr* first(Transition& self);
  offset_ptr* last(Transition& self);

  offset_ptr* get_ptr(int index) { return &data[bottom-index].ptr; }

  void insert_item(Transition& self, ISlice& key, any_ptr val_ptr, int index);
  void split(Transition& self, ISlice& key, any_ptr val_ptr);
  void trie_split(Transition& self, int split_pos, ISlice& key, any_ptr val_ptr);
  bool remove(Transition& self);

  static size_t calc_size(uint16_t count) { return (2*count+1)*sizeof(Item); }
  static any_ptr build(Trace* trace, any_ptr next1, any_ptr next2);
  static size_t min_size(size_t size);
};
#pragma pack(0)


struct Table : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key, string& current_key);
  offset_ptr* next(Transition& self, string& current_key);
  offset_ptr* first(Transition& self, string& current_key);
  offset_ptr* prev(Transition& self, string& current_key);
  offset_ptr* last(Transition& self, string& current_key);
  int advance(Transition& self, ISlice& key);
  void insert(Transition& self, ISlice& key, any_ptr val_ptr);
  bool remove(Transition& self);
};


inline offset_ptr* TableData::next(Transition& self) {
  if (self.cmp < 0) {
    self.cmp = 0;
    return &data[bottom-self.index].ptr;
  }
  self.cmp = 0;
  return ++self.index < count ? &data[bottom-self.index].ptr : NULL;
}

inline offset_ptr* TableData::prev(Transition& self) {
  if (self.cmp > 0) {
    self.cmp = 0;
    return &data[bottom-self.index].ptr;
  }
  self.cmp = 0;
  return --self.index >= 0 ? &data[bottom-self.index].ptr : NULL;
}

inline offset_ptr* TableData::first(Transition& self) {
  self.index = 0;
  self.cmp = 0;
  return &data[bottom].ptr;
}

inline offset_ptr* TableData::last(Transition& self) {
  self.index = count - 1;
  self.cmp = 0;
  return &data[bottom-count+1].ptr;
}

inline int TableData::find(ISlice& key, int* pivot) {
  return find(key, pivot, count-1);
}

inline int TableData::advance(ISlice& key, int index) {
  if (!memcmp(&data[index], key.data(), std::min(sizeof(Item), key.size()))) {
    return 0;
  }
  return -1;
}

inline size_t TableData::min_size(size_t size) {
  return std::min(size, sizeof(Item));
}

} // namespace leaves
#endif // _LARCH_LEAVES_TABLE_HPP
