// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/leaves.hpp"
#include "storage.hpp"

using std::string;

namespace leaves {

struct Transition;
struct Trace;

enum NodeTypes {
  kValue = 0,
  kNull,
  kCompressed,
  kTrie,
  kTable,
  kNodeCount
};


struct ISlice : public Slice {
  ISlice(): Slice() {}
  ISlice(const Slice& src) : Slice(src) { }
  ISlice(const char *data, size_t size) : Slice(data, size) { }

  Slice& iadvance(size_t size) {
    size = std::min(size, _size);
    _data += size;
    _size -= size;
    return *this;
  }

  void clear() {
    _size = 0;
  }
};

struct KeyString {
  KeyString() : _size(0) {}

  Slice slice() const { return Slice(_data, _size); }
  size_t size() const { return _size; }
  void push_back(char c) { _data[_size++] = c; }
  void pop_back() { _size--; }
  void resize(size_t size) { _size = size; }

  void append(const char* data, size_t size) {
    memcpy(_data+_size, data, size);
    _size += size;
  }


  char _data[MAX_KEY_SIZE];
  uint16_t _size;
};


#pragma pack(2)

struct ValueData : public Node {
  offset_ptr child;
  uint32_t size;
  char value[];

  size_t size_of() {
    return size + sizeof(ValueData);
  }

  void find(Transition& self, ISlice& key, KeyString& current_key);
  void next(Transition& self, KeyString& current_key);
  void first(Transition& self, KeyString& current_key);
  void last(Transition& self, KeyString& current_key);
  void prev(Transition& self, KeyString& current_key);
  void insert(Transition& self, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self);
  int advance(Transition& self, const Slice& key);
  void report(Stats& stats, size_t depth);

  static any_ptr build(Trace* trace, const Slice& value);
};

inline char sign(int x) {
  return (x>0)-(x<0);
}

#define MAX_COMPRESSED_LEN  255

struct CompressedData : public Node {
  offset_ptr child;
  uint8_t size;
  char keys[];

  size_t size_of(size_t size) {
    return size+sizeof(CompressedData);
  }

  int compare(ISlice& key) {
    size_t size_ = std::min(key.size(), (size_t)size);
    int cmp = sign(memcmp(key.data(), keys, size));
    if (cmp == 0) {
      return sign(size_-size);
    }
    return cmp;
  }

  void find(Transition& self, ISlice& key, KeyString& current_key);
  void next(Transition& self, KeyString& current_key);
  void first(Transition& self, KeyString& current_key);
  void last(Transition& self, KeyString& current_key);
  void prev(Transition& self, KeyString& current_key);
  void insert(Transition& self, const Slice& key, any_ptr val_ptr);
  bool remove(Transition& self);
  int advance(Transition& self, const Slice& key);
  void report(Stats& stats, size_t depth);

  offset_ptr* move(Transition& self, KeyString& current_key, bool do_it);

  void fill(any_ptr next_, const Slice& key) {
    child = next_;
    size = (uint8_t)key.size();
    memcpy(keys, key.data(), key.size());
  }

  void eat_child(Transition& self);
  static any_ptr build(Trace* trace, any_ptr next, const Slice& key);
};

#pragma pack(0)

struct TrieData;
struct TableData;

struct TransitionData {
  void set(any_ptr ptr);
  bool set(offset_ptr* pptr);

  union {
    Node* node;
    ValueData *value;
    CompressedData *compressed;
    TrieData *trie;
    TableData *table;
  };
  offset_ptr* node_ptr;
  char key;
  char cmp;
};

typedef void (*find_f)(Transition& self, ISlice& key, KeyString& current_key);
typedef void (*move_f)(Transition& self, KeyString& current_key);
typedef void (*insert_f)(Transition& self, const Slice& key, any_ptr val_ptr);
typedef bool (*remove_f)(Transition& self);
typedef int (*advance_f)(Transition& self, const Slice& key);
typedef void (*report_f)(any_ptr node, Stats& stats, size_t depth);


struct Transition : public TransitionData {
  Transition& init(offset_ptr* pptr);
  void find(ISlice& key, KeyString& current_key);
  void next(KeyString& current_key);
  void prev(KeyString& current_key);
  void first(KeyString& current_key);
  void last(KeyString& current_key);

  void child_find(offset_ptr* child, ISlice& key, KeyString& current_key);
  void child_first(offset_ptr* child, KeyString& current_key);
  void child_last(offset_ptr* child, KeyString& current_key);
  void parent_next(KeyString& current_key);
  void parent_prev(KeyString& current_key);

  int advance(const Slice& key);
  void insert(const Slice& key, any_ptr val_ptr);
  bool remove();
  static void report(any_ptr node, Stats& stats, size_t depth);

  TransitionData lower; // for lower trie
  int16_t index;
  Trace* trace;
  static find_f find_handlers[kNodeCount];
  static move_f next_handlers[kNodeCount];
  static move_f first_handlers[kNodeCount];
  static move_f prev_handlers[kNodeCount];
  static move_f last_handlers[kNodeCount];
  static insert_f insert_handlers[kNodeCount];
  static remove_f remove_handlers[kNodeCount];
  static advance_f advance_handlers[kNodeCount];
  static report_f report_handlers[kNodeCount];
};


inline void TransitionData::set(any_ptr ptr) {
  if (ptr.as_int) {
    *node_ptr = ptr;
    node = ptr.node;
  }
}

inline bool TransitionData::set(offset_ptr* pptr) {
  node_ptr = pptr;
  if (pptr) {
    node = node_ptr->resolve().node;
    return true;
  }
  node = NULL;
  return false;
}

inline Transition& Transition::init(offset_ptr* pptr) {
  node_ptr = pptr;
  node = node_ptr->resolve().node;
  return *this;
}

inline void Transition::find(ISlice& key, KeyString& current_key) {
  find_handlers[node->type](*this, key, current_key);
}

inline void Transition::next(KeyString& current_key) {
  next_handlers[node->type](*this, current_key);
}

inline void Transition::first(KeyString& current_key) {
  first_handlers[node->type](*this, current_key);
}

inline void Transition::prev(KeyString& current_key) {
  prev_handlers[node->type](*this, current_key);
}

inline void Transition::last(KeyString& current_key) {
  last_handlers[node->type](*this, current_key);
}

inline int Transition::advance(const Slice& key) {
  return advance_handlers[node->type](*this, key);
}

inline void Transition::insert(const Slice& key, any_ptr val_ptr) {
  insert_handlers[node->type](*this, key, val_ptr);
}

inline bool Transition::remove() {
  return remove_handlers[node->type](*this);
}

inline void Transition::report(any_ptr node_, Stats& stats, size_t depth) {
  report_handlers[node_.node->type](node_, stats, depth);
}


inline void CompressedData::find(Transition& self, ISlice& key, KeyString& current_key) {
  if (!(self.cmp = compare(key))) {
    key.iadvance(size);
    current_key.append(keys, size);
      self.child_find(&child, key, current_key);
  }
}

inline void CompressedData::next(Transition& self, KeyString& current_key) {
  offset_ptr* result = move(self, current_key, self.cmp < 0);
  if (result)
    self.child_first(result, current_key);
  else
    self.parent_next(current_key);
}

inline void CompressedData::first(Transition& self, KeyString& current_key) {
  self.cmp = -1;
  next(self, current_key);
}

inline void CompressedData::prev(Transition& self, KeyString& current_key) {
  offset_ptr* result = move(self, current_key, self.cmp > 0);
  if (result)
    self.child_last(result, current_key);
  else
    self.parent_prev(current_key);
}

inline void CompressedData::last(Transition& self, KeyString& current_key) {
  self.cmp = 1;
  self.prev(current_key);
}

inline int CompressedData::advance(Transition& self, const Slice& key) {
  CompressedData* node = self.compressed;
  if (node->size <= key.size() && !memcmp(node->keys, key.data(), node->size)) {
    self.cmp = 0;
    return node->size;
  }
  return -1;
}

inline void CompressedData::report(Stats& stats, size_t depth) {
  Transition::report(child, stats, depth+1);
  stats.compressed_nodes++;
}


inline void ValueData::find(Transition& self, ISlice& key, KeyString& current_key) {
  if (key.size()) {
    self.cmp = 1;
    if (child)
      self.child_find(&child, key, current_key);
  }
  else
    self.cmp = 0;
}

inline void ValueData::next(Transition& self, KeyString& current_key) {
  if (self.cmp == 0) {
    self.cmp = 1;
    if (child) {
      self.child_first(&child, current_key);
      return;
    }
  }
  self.parent_next(current_key);
}

inline void ValueData::first(Transition& self, KeyString& current_key) {
  self.cmp = 0;
}

inline void ValueData::prev(Transition& self, KeyString& current_key) {
  if (self.cmp == 1) {
    self.cmp = 0;
    return;
  }
  self.parent_prev(current_key);
}

inline void ValueData::last(Transition& self, KeyString& current_key) {
  if (child) {
    self.cmp = 1;
    self.child_last(&child, current_key);
    return;
  }
  self.cmp = 0;
}

inline int ValueData::advance(Transition& self, const Slice& key) {
  if (key.empty()) {
    self.cmp = 0;
    return -1;
  }
  return 0;
}

inline void ValueData::report(Stats& stats, size_t depth) {
  if (child) {
    stats.intermediate_nodes++;
    Transition::report(child, stats, depth+1);
  }
  else {
    stats.end_nodes++;
    stats.max_depth = std::max(stats.max_depth, depth);
  }
}

} // namespace leaves
#endif // _LARCH_LEAVES_NODE_HPP
