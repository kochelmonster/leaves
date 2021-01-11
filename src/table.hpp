#ifndef _LARCH_LEAVES_TABLE_HPP
#define _LARCH_LEAVES_TABLE_HPP

#include "node.hpp"

#define MIN_BURST_ITEMS 4

namespace leaves {

struct Transition;
struct ISlice;
struct Slice;

#pragma pack(4)


struct DataItem {
  offset_ptr value;
  uint16_t key_size;
  char key_data[];

  void set(const Slice& key, any_ptr val_ptr) {
    key_size = key.size();
    value = val_ptr;
    memcpy(key_data, key.data(), key_size);
  }
  void set(const DataItem* src, int offset) {
    key_size = src->key_size - offset;
    value = src->value;
    memcpy(key_data, src->key_data + offset, key_size);
  }
  int compare(const Slice& other) { return Slice(key_data, key_size).compare(other); }
  static size_t calc_size(size_t keys_size) { return sizeof(DataItem) + keys_size; }
};

/*
  The Table layout is
  +--------------------------------+
  | item_index[0]                  |
  +--------------------------------+
  | item_index[1]                  |
  +--------------------------------+
  |                                |
  |       free_space               |
  |                                |
  +--------------------------------+
  | DataItem[1]                    |
  +--------------------------------+
  | DataItem[0]                    \
  +--------------------------------+

key_fragments grow from top to bottom
node_pts grow from bottom to top
*/

struct TableData : public Node {
  uint16_t count;
  uint16_t data_top;
  uint16_t ptrs[];

  void insert(Transition& self, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self, int index);
  int advance(const Slice& key, int index);

  offset_ptr* find(Transition& self, ISlice& key, string& current_key);
  offset_ptr* ifind(Transition& self, const Slice& key);
  offset_ptr* next(Transition& self, string& current_key);
  offset_ptr* prev(Transition& self, string& current_key);
  offset_ptr* first(Transition& self, string& current_key);
  offset_ptr* last(Transition& self, string& current_key);

  void split(Transition& self, const Slice& key, any_ptr val_ptr);
  void trie_split(Transition& self, int split_pos);
  bool remove(Transition& self);
  offset_ptr* fill(Transition& self, string& current_key);
  void cut(Transition& self, string& current_key);
  int compare_item(int index, const Slice& other);

  DataItem* get_item(uint16_t index);
  DataItem* prepare_item(int index, size_t key_size);
  bool copy_to_split(Transition& self, TableData* dest, int start, int end, int offset);
  void insert_to_trie(Transition& self, const Slice& key, any_ptr next);

  static TableData* alloc(Trace* trace);
  static any_ptr build(Trace* trace, any_ptr val_ptr, const Slice& key);
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


inline DataItem* TableData::get_item(uint16_t index) {
  return (DataItem*)((char*)this+ptrs[index]);
}

inline DataItem* TableData::prepare_item(int index, size_t key_size) {
  data_top -= DataItem::calc_size(key_size);
  ptrs[index] = data_top;
  return get_item(index);
}

inline offset_ptr* TableData::fill(Transition& self, string& current_key) {
  DataItem* item = get_item(self.index);
  current_key.append(item->key_data, item->key_size);
  return &item->value;
}

inline void TableData::cut(Transition& self, string& current_key) {
  if (self.cmp == 0) {
    DataItem* item = get_item(self.index);
    current_key.resize(current_key.size()-item->key_size);
  }
}

inline offset_ptr* TableData::next(Transition& self, string& current_key) {
  cut(self, current_key);
  if (self.cmp > 0) {
    self.cmp = 0;
    return fill(self, current_key);
  }

  self.cmp = 0;
  return ++self.index < count ? fill(self, current_key) : NULL;
}

inline offset_ptr* TableData::prev(Transition& self, string& current_key) {
  cut(self, current_key);
  if (self.cmp < 0) {
    self.cmp = 0;
    return fill(self, current_key);
  }
  self.cmp = 0;
  return (--self.index >= 0) ? fill(self, current_key) : NULL;
}

inline offset_ptr* TableData::first(Transition& self, string& current_key) {
  self.index = 0;
  self.cmp = 0;
  return fill(self, current_key);
}

inline offset_ptr* TableData::last(Transition& self, string& current_key) {
  self.index = count - 1;
  self.cmp = 0;
  return fill(self, current_key);
}


inline int TableData::advance(const Slice& key, int index) {
  DataItem* item = get_item(index);
  return item->compare(key) ? -1 : item->key_size;
}

extern Table table_handler;

} // namespace leaves
#endif // _LARCH_LEAVES_TABLE_HPP
