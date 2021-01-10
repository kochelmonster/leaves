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
    char bytes[16];
    struct {
      uint8_t size;
      char fragment[15];
    };
  } data[];

  void insert(Transition& self, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self, int index);
  int advance(const Slice& key, int index);

  offset_ptr* find(Transition& self, ISlice& key, string& current_key);
  offset_ptr* ifind(Transition& self, const Slice& key);
  offset_ptr* next(Transition& self, string& current_key);
  offset_ptr* prev(Transition& self, string& current_key);
  offset_ptr* first(Transition& self, string& current_key);
  offset_ptr* last(Transition& self, string& current_key);

  offset_ptr* get_ptr(int index) { return &data[bottom-index].ptr; }

  void split(Transition& self, const Slice& key, any_ptr val_ptr);
  void trie_split(Transition& self, int split_pos, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self);
  offset_ptr* result_of_move(Transition& self, string& current_key);
  void cut(Transition& self, string& current_key);
  int compare_item(int index, const Slice& other);

  static size_t calc_size(uint16_t count) { return (2*count+1)*sizeof(Item); }
  static any_ptr build(Trace* trace, any_ptr val_ptr, const Slice& key);
  static size_t min_size(size_t size);
};
#pragma pack(0)


struct Table : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key, string& current_key);
  offset_ptr* next(Transition& self, string& current_key);
  offset_ptr* first(Transition& self, string& current_key);
  offset_ptr* prev(Transition& self, string& current_key);
  offset_ptr* last(Transition& self, string& current_key);
  int advance(Transition& self, const Slice& key);
  void insert(Transition& self, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self);
};

inline int TableData::compare_item(int index, const Slice& other) {
  Item& item(data[index]);
  size_t size = std::min(other.size(), (size_t)item.size);
  int cmp;

  (cmp = memcmp(item.fragment, other.data(), size)) || (cmp=item.size-min_size(other.size()));
  cmp = sign(cmp);

  if (cmp == 0 && other.size() > sizeof(Item::fragment)) {
    CompressedData* next = data[bottom-index].ptr.resolve().compressed;
    if (next->type == kCompressedLeaf) {
      return Slice(next->keys, next->size).compare(other.advance(sizeof(Item::fragment)));
    }
    assert(next->type == kValue);
    return -1;
  }

  return cmp;
}

inline offset_ptr* TableData::result_of_move(Transition& self, string& current_key) {
  current_key.append(data[self.index].fragment, data[self.index].size);
  return &data[bottom-self.index].ptr;
}

inline void TableData::cut(Transition& self, string& current_key) {
  if (self.cmp == 0)
    current_key.resize(current_key.size()-data[self.index].size);
}

inline offset_ptr* TableData::next(Transition& self, string& current_key) {
  cut(self, current_key);
  if (self.cmp < 0) {
    self.cmp = 0;
    return result_of_move(self, current_key);
  }
  self.cmp = 0;
  return ++self.index < count ? result_of_move(self, current_key) : NULL;
}

inline offset_ptr* TableData::prev(Transition& self, string& current_key) {
  cut(self, current_key);
  if (self.cmp > 0) {
    self.cmp = 0;
    return result_of_move(self, current_key);
  }
  self.cmp = 0;
  return (--self.index >= 0) ? result_of_move(self, current_key) : NULL;
}

inline offset_ptr* TableData::first(Transition& self, string& current_key) {
  self.index = 0;
  self.cmp = 0;
  return result_of_move(self, current_key);
}

inline offset_ptr* TableData::last(Transition& self, string& current_key) {
  self.index = count - 1;
  self.cmp = 0;
  return result_of_move(self, current_key);
}

inline size_t TableData::min_size(size_t size) {
  return std::min(size, sizeof(Item::fragment));
}

inline int TableData::advance(const Slice& key, int index) {
  return compare_item(index, key) ? -1 : data[index].size;
}

} // namespace leaves
#endif // _LARCH_LEAVES_TABLE_HPP
