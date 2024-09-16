// declarations for memory blocks
#ifndef _LEAVES_BLOCKS_HPP
#define _LEAVES_BLOCKS_HPP

#include <leaves.hpp>
#include <stdexcept>

#ifdef HEADER_ONLY
#define INLINE inline
#else
#define INLINE
#endif

namespace leaves {

typedef uint64_t offset_ptr;
typedef uint64_t tid_t;

const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;

#pragma pack(1)

union Node;

enum BlockType { kTrieBlock, kValueBlock };

/* the metadata of a every block */
struct BlockMeta {
  // the blocks offset (==id)
  offset_ptr offset;

  // the transaction id the block was created;
  tid_t txn_id;

  // if the block is in a free list the free block
  offset_ptr next_free;
  // the transaction id the block was freed
  tid_t free_txn_id;

  struct {
    // the block type
    uint64_t type : 2;

    // the block size
    uint64_t size : 62;
  };
};

typedef uint16_t ssize_t;

enum NodeType { kNull, kBitTrie, kTrie, kValue, kLink, kCompressed, kEndType };

// A pointer inside a trie block.
struct node_ptr {
  union {
    struct {
      /* mini offset within the data attribute
         the real offset is moffset << 3
       */
      ssize_t moffset : 13;

      // type of the node
      ssize_t type : 3;
    };
    ssize_t val;
  };

  node_ptr(ssize_t val_ = 0) : val(val_) {}
  node_ptr(ssize_t moffset_, NodeType type_) : moffset(moffset_), type(type_) {}

  node_ptr& operator=(const node_ptr& src) {
    val = src.val;
    return *this;
  }

  ssize_t offset() const { return moffset << 3; }
};

struct TrieBlock : public BlockMeta {
  static const size_t SIZE = 16*K;
  static const size_t SPLIT = SIZE / 16;

  // count of used space in data array
  ssize_t used;

  static const size_t DATA_SIZE = SIZE - sizeof(BlockMeta) - sizeof(used);

  // memory of trie nodes
  char data[DATA_SIZE];

  void init() {
    assert(offset);
    size = SIZE;
    type = kTrieBlock;
    used = 8;  // multiple of 8
    *resolve_ptr(0) = node_ptr();
  }

  Node* resolve(node_ptr ptr) { return (Node*)&data[ptr.offset()]; }

  const Node* resolve(node_ptr ptr) const {
    return (const Node*)&data[ptr.offset()];
  }

  node_ptr* resolve_ptr(ssize_t onode) const { return (node_ptr*)&data[onode]; }

  // allocs size bytes and returns node_ptr of the allocated area
  node_ptr alloc(ssize_t size, NodeType type) {
    assert(type != kNull);
    assert((size & 7) == 0);

    if (size + used <= DATA_SIZE) {
      ssize_t allocated = used;
      used += size;
      memset(&data[allocated], 0, size);
      return node_ptr(allocated >> 3, type);
    }
    return node_ptr(0, kNull);
  }
};


// A block for a big value
struct ValueBlock : public BlockMeta {
  static const size_t OVERHEAD = sizeof(BlockMeta);
  char data[0];
};

#pragma pack(0)

union BlockUnion {
  BlockMeta meta;
  TrieBlock trie;
  ValueBlock value;
};

}  // namespace leaves

#endif  // _LEAVES_BLOCKS_HPP
