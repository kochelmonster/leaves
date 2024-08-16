#ifndef _LEAVES_NODE_HPP
#define _LEAVES_NODE_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <leaves.hpp>

#include "memory.hpp"
#include "port.hpp"

#ifdef TESTING
#include <sstream>
#endif

namespace leaves {

enum NodeType {
  kNull,
  kBitTrie,
  kTrie,
  kValue,
  kLink,
  kTableLink,
  kCompressed,
  kEndType
};

struct Transition;
typedef uint16_t ssize_t;
union Node;

// A pointer inside a trie block.
struct node_ptr {
  union {
    struct {
      // offset within the data attribute
      uint16_t offset : 13;

      // type of the node
      uint16_t type : 3;
    };
    uint16_t val;
  };

  node_ptr(uint16_t val_) : val(val) {}
  node_ptr(uint16_t offset_, uint8_t type_) : offset(offset), type(type_) {}

  const node_ptr& operator=(node_ptr& src) {
    val = src.val;
    return *this;
  }

  uint16_t point() const { return offset << 3; }
};

#pragma pack(1)

// A trie node that represents the upper or lower 4bits of a char
struct BitTrie {
  static BitTrie& cast(Transition& trans);
  static const BitTrie& cast(const Transition& trans);
  static ssize_t size(int count) {
    if (count <= (8 - sizeof(bits)) / sizeof(node_ptr)) return 8;
    if (count <= (16 - sizeof(bits)) / sizeof(node_ptr)) return 16;
    return 24;
  }

  size_t count() const { return popcount(bits); }

  // returns the relative offset of the child with given index
  ssize_t offset(int index) const {
    return sizeof(bits) + sizeof(node_ptr) * index;
  }

  // returns the index of the found child or -1
  int find(char key) const {
    int bit = 1 << key;
    if (!(bits & bit)) return -1;

    return popcount(bits & (bit - 1));
  }

  int index(int bit) const {
    uint16_t bitval = (1 << bit);
    return bits & bitval ? popcount(bits & (bitval - 1)) : -1;
  }

  int next(int bit) const {
    uint16_t nbits = bits & (0xFFFF << (bit + 1));
    if (nbits) {
      return ctz(nbits);
    }
    return -1;
  }

  int first() const { return ctz(bits); }

  int prev(int bit) const {
    if (bit) {
      uint16_t nbits = bits & (0xFFFF >> (16 - bit));
      if (nbits) return 15 - (clz(nbits) & 0xf);
    }
    return -1;
  }

  int last() const { return 15 - (clz(bits) & 0xf); }

  int add(int bit) {
    assert(!(bits & 1 << bit));
    bits |= 1 << bit;
    int idx = index(bit);
    for (int i = popcount(bits) - 1; i > idx; i--) {
      children[i] = children[i - 1];
    }
    return idx;
  }

  // bit mask of the childrens bit value
  uint16_t bits;
  node_ptr children[];
};

struct Trie {
  static const int CHILDREN = 16;
  node_ptr children[CHILDREN];

  static Trie& cast(Transition& trans);
  static const Trie& cast(const Transition& trans);

  // returns the index of the found child or -1
  int find(char bit) const {
    if (!children[bit].val) return -1;
    return bit;
  }

  int next(int bit) const {
    for (; bit < CHILDREN; bit++) {
      if (children[bit].val) break;
    }
    return bit;
  }

  int add(int bit) { return bit; }

  static ssize_t offset(int index) { return sizeof(node_ptr) * index; }
  size_t count() const { return CHILDREN; }
};

// compressed trie node
struct Compressed {
  node_ptr child;
  uint8_t size;
  char key[];
  static ssize_t offset() { return offsetof(Compressed, child); }
};

struct Value {
  static const size_t SMALL_SIZE = 1024;
  node_ptr child;
  size_t size;
  union {
    offset_ptr link;
    char data[1];
  };

  static ssize_t calc_struct_size(ssize_t size_) {
    return std::max(sizeof(node_ptr) + sizeof(size) + size_, sizeof(Value));
  }

  static ssize_t offset() { return offsetof(Value, child); }
};

union Node {
  offset_ptr link;
  BitTrie bit_trie;
  Trie trie;
  Compressed compressed;
  Value value;
};

#pragma pack(0)

struct TrieBlock;
struct Trace;
struct Transition;

typedef ssize_t (*is_valid_t)(const Transition& trans);
ssize_t is_valid_null(const Transition& trans);
ssize_t is_valid_value(const Transition& trans);
ssize_t is_valid_table_link(const Transition& trans);
const is_valid_t is_valid[] = {
    is_valid_null, is_valid_null,       is_valid_null, is_valid_value,
    is_valid_null, is_valid_table_link, is_valid_null};

typedef ssize_t (*get_size_t)(const Transition& trans);
ssize_t get_size_null(const Transition& trans);
ssize_t get_size_bit_trie(const Transition& trans);
ssize_t get_size_trie(const Transition& trans);
ssize_t get_size_value(const Transition& trans);
ssize_t get_size_link(const Transition& trans);
ssize_t get_size_compressed(const Transition& trans);

const get_size_t get_size[] = {
    get_size_null, get_size_bit_trie, get_size_trie,      get_size_value,
    get_size_link, get_size_link,     get_size_compressed};

typedef bool (*find_t)(Trace& cursor, Transition& trans);
bool find_null(Trace& cursor, Transition& trans);
bool find_bit_trie(Trace& cursor, Transition& trans);
bool find_trie(Trace& cursor, Transition& trans);
bool find_value(Trace& cursor, Transition& trans);
bool find_link(Trace& cursor, Transition& trans);
bool find_table_link(Trace& cursor, Transition& trans);
bool find_compressed(Trace& cursor, Transition& trans);
const find_t find[] = {find_null, find_bit_trie,   find_trie,      find_value,
                       find_link, find_table_link, find_compressed};

typedef Slice (*get_value_t)(Trace& cursor, const Transition& trans);
Slice get_value_null(Trace& cursor, const Transition& trans);
Slice get_value_value(Trace& cursor, const Transition& trans);
Slice get_value_table_value(Trace& cursor, const Transition& trans);
const get_value_t get_value[] = {
    get_value_null, get_value_null,        get_value_null, get_value_value,
    get_value_null, get_value_table_value, get_value_null};

typedef bool (*advance_t)(Trace& cursor, const Transition& trans);
bool advance_null(Trace& cursor, const Transition& trans);
bool advance_trie(Trace& cursor, const Transition& trans);
bool advance_value(Trace& cursor, const Transition& trans);
bool advance_link(Trace& cursor, const Transition& trans);
bool advance_table_link(Trace& cursor, const Transition& trans);
bool advance_compressed(Trace& cursor, const Transition& trans);
const advance_t advance[] = {
    advance_null, advance_trie,       advance_trie,      advance_value,
    advance_link, advance_table_link, advance_compressed};

#if 0
typedef void (*set_value_t)(Trace& cursor, Transition& trans,
                            const Slice& value);
void set_value_null(Trace& cursor, Transition& trans, const Slice& value);
void set_value_bit_trie(Trace& cursor, Transition& trans, const Slice& value);
void set_value_trie(Trace& cursor, Transition& trans, const Slice& value);
void set_value_value(Trace& cursor, Transition& trans, const Slice& value);
void set_value_link(Trace& cursor, Transition& trans, const Slice& value);
void set_value_table_link(Trace& cursor, Transition& trans, const Slice& value);
void set_value_compressed(Trace& cursor, Transition& trans, const Slice& value);
const set_value_t set_value[] = {
    set_value_null, set_value_bit_trie,   set_value_trie,      set_value_value,
    set_value_link, set_value_table_link, set_value_compressed};
#endif

/* writes the deep sizes of all children in the data attribute of the sizes
 block. */
typedef ssize_t (*mark_deep_size_t)(Trace& cursor, const Transition& trans,
                                    TrieBlock& sizes);
ssize_t mark_deep_size_null(Trace& cursor, const Transition& trans,
                            TrieBlock& sizes);
ssize_t mark_deep_size_trie(Trace& cursor, const Transition& trans,
                            TrieBlock& sizes);
ssize_t mark_deep_size_bit_trie(Trace& cursor, const Transition& trans,
                                TrieBlock& sizes);
ssize_t mark_deep_size_value(Trace& cursor, const Transition& trans,
                             TrieBlock& sizes);
ssize_t mark_deep_size_link(Trace& cursor, const Transition& trans,
                            TrieBlock& sizes);
ssize_t mark_deep_size_compressed(Trace& cursor, const Transition& trans,
                                  TrieBlock& sizes);
const mark_deep_size_t mark_deep_size[] = {
    mark_deep_size_null,      mark_deep_size_bit_trie, mark_deep_size_trie,
    mark_deep_size_value,     mark_deep_size_link,     mark_deep_size_link,
    mark_deep_size_compressed};

inline char sign(int x) { return (x > 0) - (x < 0); }

void create_table(Trace& cursor, Transition& trans, ssize_t onode,
                  const Slice& value);
void create_value(Trace& cursor, Transition& trans, ssize_t onode,
                  const Slice& value);

}  // namespace leaves

#endif  // _LEAVES_NODE_HPP