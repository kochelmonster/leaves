#ifndef _LEAVES_NODE_HPP
#define _LEAVES_NODE_HPP

#include <cassert>
#include <cstdint>
#include <leaves.hpp>

#include "port.hpp"

namespace leaves {

enum NodeTypes {
  kNull = 0,
  kEndLeaf,
  kMiddleLeaf,
  kLink,
  kCompressed,
  kUpperTrie,
  kLowerTrie,
  kNTEnd
};

/*
  An abstract pointer to a Node within a Page
*/

struct node_p {
  uint16_t type : 3;
  uint16_t offset : 13;

  static node_p b(uint16_t offset_ = 0, uint16_t type_ = 0);
  static node_p null;
  node_p replace(uint16_t type_) const { return b(offset, type_); }
};

struct location_p {
  union {
    node_p node;
    struct {
      uint64_t type : 3;
      uint64_t offset : 13;
      uint64_t page : 48;
    };
  };

  static location_p b(uint64_t page_ = 0);
  static location_p b(uint64_t page_, node_p node_);
  static location_p b(uint64_t page_, uint16_t offset, uint16_t type);
  location_p replace(node_p n) const { return location_p::b(page, n); }
  location_p replace(uint16_t offset, uint16_t type = 0) const {
    return location_p::b(page, offset, type);
  }
};

struct Trace;
struct Page;
struct WritablePage;
struct SplitCandidate;
struct Storage;

#pragma pack(1)

struct Trie {
  uint16_t bits;
  node_p children[];

  size_t struct_size() const { return sizeof(Trie) + count() * sizeof(node_p); }

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

  node_p* add(int bit) {
    assert(!(bits & 1 << bit));
    bits |= 1 << bit;
    int idx = index(bit);
    for (int i = popcount(bits) - 1; i > idx; i--) {
      children[i] = children[i - 1];
    }
    return &children[idx];
  }
};

struct Link {
  location_p loc;
};

struct Compressed {
  uint8_t size;
  node_p child;
  char key[];

  size_t struct_size() const { return sizeof(Compressed) + size; }
};

struct EndLeaf {
  uint32_t size;
  char data[];

  size_t struct_size() const { return sizeof(EndLeaf) + size; }
};

#define BIG_VALUE (PAGE_SIZE - sizeof(EndLeaf) - sizeof(node_p))

struct MiddleLeaf {
  node_p leaf;
  node_p child;
};

struct Node {
  union {
    Trie trie;
    Link link;
    Compressed compressed;
    EndLeaf endleaf;
    MiddleLeaf middleleaf;
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
  virtual void adjust_pointers(WritablePage* page, node_p npos, size_t start,
                               int delta) = 0;
  virtual node_p move_node(WritablePage* src, node_p* npos,
                           WritablePage* dest) = 0;
  virtual size_t find_split_link(Page* page, node_p npos, SplitCandidate& candidate) = 0;
  virtual node_p merge_node(Storage& storage, WritablePage* page,
                            node_p npos) = 0;

#if 0
  virtual Node* next(Trace& trace) = 0;
  virtual Node* first(Trace& trace) = 0;
  virtual Node* prev(Trace& trace) = 0;
  virtual Node* last(Trace& trace) = 0;

  
  virtual bool remove(Transition& self, bool last) = 0;
  //virtual segment_ptr* last(Transition& self) { return self.node_ptr; }
  
  virtual Slice get_value(const Transition& self) const { return Slice(); }
#endif

  static NodeHandler* HANDLERS[kNTEnd];
};

}  // namespace leaves

#endif  // _LEAVES_NODE_HPP