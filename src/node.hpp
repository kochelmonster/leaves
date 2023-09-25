#ifndef _LEAVES_NODE_HPP
#define _LEAVES_NODE_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <leaves.hpp>

#include "port.hpp"

#ifdef TESTING
#include <sstream>
#endif

namespace leaves {

enum NodeType {
  kNull,
  kValue,
  kLink,
  kHeapLink,
  kCompressed,
  kUpperTrie,
  kLowerTrie,
  kEndType
};

struct MemoryViewBase {
  char* start;
};

typedef uint16_t node_t;
struct stored_ptr {
  static const int PAGE_POOL = 8;
  union {
    struct {
      uint64_t size : 12;  // size of page
      uint64_t offset : 52;
    };
    uint64_t val;
  };

  stored_ptr() : val(0) {}
  stored_ptr(const stored_ptr& src) : val(src.val) {}

  template <typename T>
  T* get(MemoryViewBase* memview) const {
    return (T*)(memview->start + offset);
  }

  int pool_id() const {
    if (size > 20) {
      size_t exact_size = size - 20;
      int high_bit = 32 - clz((uint64_t)size | 1);
      if ((1 << high_bit) < size) high_bit++;
      return std::max(high_bit, 4) - 4;
    }
    return size + 8;
  }
};

struct ValueBlock;
struct Trace;
struct Page;
struct Storage;

#pragma pack(1)

/*
!!!Caution: all node_p links to other nodes on the same page are
offsets from the original node ot the child node.
With this technique whole pages just can be copied to
ot  her pages in different positions.
*/

struct Trie {
  uint16_t bits;
  node_t children[];

  // minimum size is Pointer size
  size_t struct_size(int delta=0) const {
    int ec = count() + delta;
    ec += ec & 1;
    return std::max(sizeof(Trie) + ec * sizeof(node_t), sizeof(Page*));
  }

  size_t count() const { return popcount(bits); }

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
};

struct Compressed {
  uint8_t size;
  node_t child;
  char key[];

  // minimum size is Pointer size
  size_t struct_size() const {
    return std::max(sizeof(Compressed) + size, sizeof(Page*));
  }

  static inline uint16_t calc_size(uint8_t size) {
    return size ? std::max(sizeof(Compressed) + size, sizeof(Page*)) : 0;
  }
};

struct Value {
  stored_ptr value;
  node_t child;  // if non zero link to the next node
};

struct Node {
  union {
    Page* pointer;
    stored_ptr link;
    Trie trie;
    Compressed compressed;
    Value value;
  };
};

#pragma pack(0)

struct NodeHandler {
  /*
  Find the next node. Returns true to go on or false
  */
  virtual bool find(Trace& trace) = 0;

  /*
  Returns true if key is at a valid position.
  */
  virtual bool valid(const Trace& trace) const { return false; }
  virtual void insert(Trace& trace, const Slice& value) = 0;
  virtual node_t copy_node(Page* dest, const Page* src, node_t id) = 0;
  virtual uint16_t get_size(const Node* node) = 0;

#if 0
  virtual Node* next(Trace& trace) = 0;
  virtual Node* first(Trace& trace) = 0;
  virtual Node* prev(Trace& trace) = 0;
  virtual Node* last(Trace& trace) = 0;

  
  virtual bool remove(Transition& self, bool last) = 0;
  //virtual segment_ptr* last(Transition& self) { return self.node_ptr; }
  
  virtual Slice get_value(const Transition& self) const { return Slice(); }
#endif

  static NodeHandler* HANDLERS[kEndType];
};

#ifdef TESTING

struct TestPoints {
  static std::stringstream tp_output;

  static void testpoint(const char* str) {
    tp_output << "TESTPOINT: " << str << std::endl;
  }
};

inline std::stringstream TestPoints::tp_output;

#define TESTPOINT(x) TestPoints::testpoint(#x)
#else
#define TESTPOINT(x)
#endif

}  // namespace leaves

#endif  // _LEAVES_NODE_HPP