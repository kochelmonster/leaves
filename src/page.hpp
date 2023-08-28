#ifndef _LEAVES_PAGE_HPP
#define _LEAVES_PAGE_HPP

#include <cstdint>

#include "node.hpp"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define SPLIT_SIZE (PAGE_SIZE / 4)

#define PAGE_ROUND_UP(x) ((((size_t)(x)) + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1)))

namespace leaves {

#pragma pack(1)

struct Storage;

struct StorageHeader {
  char signature[sizeof(SIGNATURE)];
  uint16_t version;
  uint64_t transaction_id;
  size_t freed_head;
  size_t free_block;
  location_p root;
};

struct Page {
  union {
    uint64_t next_storage;  // for the storage page pool
    Page* next_mem;         // for tmp mem pool
    StorageHeader header;
    struct {
      union {
        node_p end;  // the type of end is the type of root
        node_p root;
      };
      char content[PAGE_SIZE - sizeof(node_p)];
    };
  };
  Page() {}

  const Node* node(uint16_t pos) const { return (const Node*)&content[pos]; }

  Node* node(uint16_t pos) { return (Node*)&content[pos]; }

  const Node* node(node_p pos) const { return node(pos.offset); }

  Node* node(node_p pos) { return node(pos.offset); }

  node_p* link(uint16_t pos) { return (node_p*)&content[pos]; }

  uint16_t offset(const void* p) const { return ((char*)p - &content[0]); }

  /*
    Find the node where to split the page
    link is the offset to the link of the split node.
    i.e. offset(&Compressed.child)
  */
  size_t find_split_link(node_p npos, SplitCandidate& candidate) {
    return NodeHandler::HANDLERS[npos.type]->find_split_link(this, npos, candidate);
  }
};

struct SplitCandidate {
  node_p* _link;
  size_t _size;

  SplitCandidate() : _link(NULL), _size(0) {}

  bool set_link(node_p* link, size_t size) {
    if (link->offset > 0 && _size < size) {
      if (_size < SPLIT_SIZE) {
        _size = size;
        _link = link;
      } else if (link->type >= kUpperTrie && _link->type < kUpperTrie &&
                 link->offset >= PAGE_SIZE / 2) {
        // prefer split at trie nodes before others
        _size = size;
        _link = link;
      }

      return _link->type >= kUpperTrie && _size >= SPLIT_SIZE;
    }
    return false;
  }
};

struct WritablePage : public Page {
  /* Writeable Page has an overflow for inserting new nodes beyond
     the page boarder. The overflow will never be more than PAGE_SIZE;
   */
  char overflow[PAGE_SIZE];
  bool too_small;
  uint16_t backed_end;
  
  /*
    Allocate n bytes on the current page, moves nodes
    if there is not enough space.
  */
  uint16_t alloc(size_t size);
  node_p alloc(size_t size, uint16_t type) {
    uint16_t offset = alloc(size);
    if (!offset) root.type = type;
    return node_p::b(offset, type);
  }

  /*
  Scales the size oi a node by delta, adjust all node_p pointers.
  */
  void scale_node(size_t start, int delta);

  size_t free() const {
    return sizeof(content) > end.offset ? sizeof(content) - end.offset : 0;
  }

  bool split(Storage& storage);
  bool merge(Storage& storage);

  void adjust_pointers(node_p npos, size_t start, int delta) {
    NodeHandler::HANDLERS[npos.type]->adjust_pointers(this, npos, start, delta);
  }

  node_p move_node(node_p* node, WritablePage* dest) {
    return NodeHandler::HANDLERS[node->type]->move_node(this, node, dest);
  }

  node_p merge_node(Storage& storage, node_p npos) {
    return NodeHandler::HANDLERS[npos.type]->merge_node(storage, this, npos);
  }
};
#pragma pack(0)

}  // namespace leaves

#endif  // _LEAVES_PAGE_HPP