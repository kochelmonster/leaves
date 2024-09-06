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

struct Transition;
struct BlockSplitter;
typedef uint16_t ssize_t;
union Node;

#pragma pack(1)

// A trie node that represents the upper or lower 4bits of a char
struct BitTrie {
  static BitTrie& cast(Transition& trans);
  static const BitTrie& cast(const Transition& trans);
  static const NodeType type = kBitTrie;

  size_t count() const { return popcount(bits); }

  // returns the relative offset of the child with given index
  static ssize_t offset(int index) {
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
    children[idx].val = 0;
    return idx;
  }

  // bit mask of the childrens bit value
  uint16_t bits;
  node_ptr children[7];
};

struct Trie {
  static const int CHILDREN = 16;
  node_ptr children[CHILDREN];

  static Trie& cast(Transition& trans);
  static const Trie& cast(const Transition& trans);
  static const NodeType type = kTrie;

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
  uint16_t size;
  char key[];

  static ssize_t offset() { return offsetof(Compressed, child); }
  static ssize_t calc_alloc_size(size_t size) {
    return (sizeof(Compressed) + size + 7) & ~7;
  }
};

struct Value {
  static const size_t SMALL_SIZE = 1024;
  node_ptr child;
  size_t size;
  union {
    offset_ptr link;
    char data[1];
  };

  static ssize_t calc_alloc_size(size_t size) {
    ssize_t result = sizeof(Value);
    if (size <= SMALL_SIZE)
      result += std::max(size, sizeof(link)) - sizeof(link);
    return (result + 7) & ~7;
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
const is_valid_t is_valid[] = {
    is_valid_null, is_valid_null,       is_valid_null, is_valid_value,
    is_valid_null, is_valid_null};

typedef ssize_t (*get_size_t)(const Transition& trans);
ssize_t get_size_null(const Transition& trans);
ssize_t get_size_bit_trie(const Transition& trans);
ssize_t get_size_trie(const Transition& trans);
ssize_t get_size_value(const Transition& trans);
ssize_t get_size_link(const Transition& trans);
ssize_t get_size_compressed(const Transition& trans);

const get_size_t get_size[] = {
    get_size_null, get_size_bit_trie, get_size_trie,      get_size_value,
    get_size_link, get_size_compressed};

typedef bool (*find_t)(Trace& cursor, Transition& trans);
bool find_null(Trace& cursor, Transition& trans);
bool find_bit_trie(Trace& cursor, Transition& trans);
bool find_trie(Trace& cursor, Transition& trans);
bool find_value(Trace& cursor, Transition& trans);
bool find_link(Trace& cursor, Transition& trans);
bool find_compressed(Trace& cursor, Transition& trans);
const find_t find[] = {find_null, find_bit_trie,   find_trie,      find_value,
                       find_link, find_compressed};

typedef bool (*advance_t)(Trace& cursor, const Transition& trans);
bool advance_null(Trace& cursor, const Transition& trans);
bool advance_trie(Trace& cursor, const Transition& trans);
bool advance_value(Trace& cursor, const Transition& trans);
bool advance_link(Trace& cursor, const Transition& trans);
bool advance_compressed(Trace& cursor, const Transition& trans);
const advance_t advance[] = {
    advance_null, advance_trie,       advance_trie,      advance_value,
    advance_link, advance_compressed};

typedef void (*set_value_t)(Trace& cursor, const Slice& value);
void set_value_null(Trace& cursor, const Slice& value);
void set_value_bit_trie(Trace& cursor, const Slice& value);
void set_value_trie(Trace& cursor, const Slice& value);
void set_value_value(Trace& cursor, const Slice& value);
void set_value_link(Trace& cursor, const Slice& value);
void set_value_compressed(Trace& cursor, const Slice& value);
const set_value_t set_value[] = {
    set_value_null, set_value_bit_trie,   set_value_trie,      set_value_value,
    set_value_link, set_value_compressed};

/* finds a branch with at least 4K size and moves it to a new page */
typedef ssize_t (*find_splitpoint_t)(BlockSplitter& bs, Transition& trans);
ssize_t find_splitpoint_null(BlockSplitter& bs, Transition& trans);
ssize_t find_splitpoint_bit_trie(BlockSplitter& bs, Transition& trans);
ssize_t find_splitpoint_trie(BlockSplitter& bs, Transition& trans);
ssize_t find_splitpoint_value(BlockSplitter& bs, Transition& trans);
ssize_t find_splitpoint_link(BlockSplitter& bs, Transition& trans);
ssize_t find_splitpoint_compressed(BlockSplitter& bs, Transition& trans);
const find_splitpoint_t find_splitpoint[] = {
    find_splitpoint_null,      find_splitpoint_bit_trie, find_splitpoint_trie,
    find_splitpoint_value,     find_splitpoint_link,
    find_splitpoint_compressed};

/* moves a node and its children to a new block */
typedef node_ptr (*move_t)(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
node_ptr move_null(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
node_ptr move_bit_trie(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
node_ptr move_trie(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
node_ptr move_value(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
node_ptr move_link(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
node_ptr move_compressed(TrieBlock* src, node_ptr psrc, TrieBlock* dest);
const move_t move[] = {move_null, move_bit_trie, move_trie,      move_value,
                       move_link, move_compressed};

inline char sign(int x) { return (x > 0) - (x < 0); }

/* adds a value an updates the current stack.
   returns true if the values was replaced.
 */
bool add_value(Trace& cursor, const Slice& value);

}  // namespace leaves

#endif  // _LEAVES_NODE_HPP