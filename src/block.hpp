// declarations for memory blocks
#ifndef _LEAVES_BLOCKS_HPP
#define _LEAVES_BLOCKS_HPP

#include <leaves.hpp>
#include <stdexcept>

#include "pool.hpp"

#ifdef HEADER_ONLY
#define INLINE inline
#else
#define INLINE
#endif

namespace leaves {

#pragma pack(1)

union Node;

enum BlockType { kTrieBlock = 0, kValueBlock, kBigValueBlock };

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


enum NodeType { kNull, kUpperTrie, kLowerTrie, kArray, kValue, kLink, kString };

typedef uint16_t ssize_t;

// the standard node size
const ssize_t NODE_SIZE = 32;

// the minimum node size (LinkNode)
const ssize_t MIN_NODE_SIZE = sizeof(offset_ptr);


// A pointer inside a TrieNode block.
struct node_ptr {
  union {
    struct {
      // type of the node
      uint16_t type : 3;

      // offset form
      int16_t offset : 13;
    };
    uint16_t val;
  };
  
  node_ptr() : val(0) {}
  node_ptr(const void* p, NodeType type_) : type(type_) {
    offset = ((const char*)p - (const char*)this);
  }

  bool is_valid() const { return offset != 0; }

  Node* resolve() const {
    return is_valid() ? (Node*)((char*)this + offset) : NULL;
  }

  void set(const void* p, NodeType type_) {
    type = type_;
    offset = ((const char*)p - (const char*)this);
  }

  operator Node*() { return resolve(); }

  Node* operator->() { return resolve(); }
  Node* operator->() const { return resolve(); }

  node_ptr& operator=(const node_ptr& src) {
    type = src.type;
    offset = src.offset ? ((const char*)src.resolve() - (const char*)this) : 0;
    return *this;
  }
};


struct TrieBlock : public BlockMeta {
  static const size_t SIZE = 4 * K;
  static const int POOL_ID = get_pool(SIZE);

  offset_ptr value;  // pointer to Value block
  ssize_t used;      // used bytes of data
  node_ptr root;

  static const size_t DATA_SIZE =
      SIZE - sizeof(BlockMeta) - sizeof(value) - sizeof(used) - sizeof(root);

  /* the maximum additional node size need to insert a new node
   * in the structure (without the rest_key) */
  static const size_t RESERVE_SIZE = 2 * NODE_SIZE;
  static const size_t MAX_SPACE = DATA_SIZE - RESERVE_SIZE;

  char data[DATA_SIZE];

  void init() {
    assert(size == SIZE);
    assert(offset > 0);
    value = 0;
    type = kTrieBlock;
    used = 0;
    root.val = 0;
  }

  bool needs_split(ssize_t needed) const { return used + needed >= MAX_SPACE; }
  ssize_t free_space() const { return DATA_SIZE - RESERVE_SIZE - used; }

  // allocs size bytes and returns node_ptr of the allocated area
  Node* alloc(ssize_t size_ = NODE_SIZE) {
    assert(used + size_ <= DATA_SIZE);
    ssize_t allocated = used;
    used += size_;
    memset(&data[allocated], 0, size_);
    return (Node*)&data[allocated];
  }
};

/* A block to save the small data of a TrieBlock.

   At the end of the block a reverse growing index table
   pointing to the data block is stored.
 */

struct ValueBlock : public BlockMeta {
  struct IndexEntry {
    uint32_t offset;
    uint32_t size;
  };

  uint32_t data_size;
  uint16_t index_count;
  static const size_t OVERHEAD =
      sizeof(BlockMeta) + sizeof(index_count) + sizeof(data_size);
  static const size_t INITAL_OVERHEAD = OVERHEAD + sizeof(IndexEntry);

  char data[0];

  IndexEntry& entry(uint16_t index) {
    return *(((IndexEntry*)((char*)this + size)) - index);
  }

  void init() {
    data_size = 0;
    index_count = 0;
  }

  Slice get_value(uint16_t index) {
    IndexEntry& entry_ = entry(index);
    return Slice(&data[entry_.offset], entry_.size);
  }

  uint16_t set_init_value(const Slice& value) {
    assert(size - INITAL_OVERHEAD > value.size());
    assert(index_count == 0);
    assert(data_size == 0);
    index_count++;
    IndexEntry& entry_ = entry(index_count);
    entry_.offset = data_size;
    entry_.size = value.size();
    data_size += entry_.size;
    memcpy(data + entry_.offset, value.data(), entry_.size);
    return index_count;
  }

  size_t calc_copy_size(uint16_t index, const Slice& value) {
    if (!index)
      return data_size + OVERHEAD * (index_count + 1) * sizeof(IndexEntry) +
             value.size();

    auto entry_ = entry(index);
    return data_size + OVERHEAD * (index_count) * sizeof(IndexEntry) +
           value.size() - entry_.size;
  }

  uint16_t copy_block(ValueBlock* src, uint16_t index, const Slice& value) {
    index_count = src->index_count;

    memcpy(&entry(index_count), &src->entry(index_count),
           sizeof(IndexEntry) * index_count);

    if (!index) {
      // Add new value
      data_size = src->data_size;
      memcpy(data, src->data, data_size);
      IndexEntry& entry_ = entry(++index_count);
      entry_.size = value.size();
      entry_.offset = data_size;
      memcpy(data + entry_.offset, value.data(), value.size());
      data_size += entry_.size;
      return index_count;
    }

    // Replace Value

    IndexEntry& entry_ = entry(index);
    data_size = src->data_size;
    
    // copy before index
    memcpy(data, src->data, entry_.offset);

    // copy value
    memcpy(data + entry_.offset, value.data(), value.size());

    // copy after index
    memcpy(data + entry_.offset + value.size(),
           src->data + entry_.offset + entry_.size,
           data_size - entry_.offset - entry_.size);

    // correct the entries
    int delta = value.size() - entry_.size;
    data_size += delta;
    entry_.size = value.size();

    for(int i = index+1 ; i <= index_count; i++) {
      entry(i).offset += delta;
    }
    return index;
  }
};

// A data block for very big Values
struct BigValueBlock : public BlockMeta {
  uint32_t data_size;
  static const size_t OVERHEAD = sizeof(BlockMeta) + sizeof(data_size);
  char data[0];
};

// typedef BlockMeta* block_ptr;

struct block_ptr {
  BlockMeta* ptr;

  TrieBlock* trie() { return static_cast<TrieBlock*>(ptr); }
  BigValueBlock* bval() { return static_cast<BigValueBlock*>(ptr); }
  ValueBlock* val() { return static_cast<ValueBlock*>(ptr); }

  const TrieBlock* trie() const { return static_cast<const TrieBlock*>(ptr); }
  BlockMeta* operator->() { return ptr; }
  const BlockMeta* operator->() const { return ptr; }

  void reset() { ptr = NULL; }

  operator BlockMeta*() { return ptr; }
  block_ptr& operator=(const block_ptr& src) {
    ptr = src.ptr;
    return *this;
  }

  bool is_valid() const { return ptr != NULL; }
};

#pragma pack(0)

}  // namespace leaves

#endif  // _LEAVES_BLOCKS_HPP
