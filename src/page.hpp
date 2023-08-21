#ifndef _LEAVES_PAGE_HPP
#define _LEAVES_PAGE_HPP

#include <cstdint>
#include "node.hpp"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define PAGE_ROUND_UP(x) ( (((size_t)(x)) + PAGE_SIZE-1)  & (~(PAGE_SIZE-1)) ) 

namespace leaves {

#pragma pack(1)

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
    uint64_t next_storage; // for the storage page pool
    Page* next_mem;      // for tmp mem pool
    StorageHeader header;
    struct {
      char content[PAGE_SIZE-sizeof(node_p)];
      node_p end;  // the type of end is the type of root
    };
  };
  Page() {}

  const Node* node(uint16_t pos) const {
      return (const Node*)&content[pos];
    }
  
  Node* node(uint16_t pos) {
      return (Node*)&content[pos];
    }

  const Node* node(node_p pos) const {
      return node(pos.offset);
    }
  
  Node* node(node_p pos) {
      return node(pos.offset);
    }

  node_p offset(const void* p) const {
      return node_p::b((char*)p - &content[0]);
    }


  /*
    Allocate n bytes on the current page, moves nodes 
    if there is not enough space.
  */
  uint16_t alloc(Trace& trace, size_t size);
  node_p alloc(Trace& trace, size_t size, uint16_t type) {
    return node_p::b(alloc(trace, size), type);
  }
    
  /*
  Scales the size oi a node by delta, adjust all node_p pointers.
  */
  void scale_node(Trace& trace, size_t start, int delta);

  size_t free() const { return sizeof(content) - end.offset; }

  void adjust_pointers(node_p npos, size_t start, int delta);

  /*
  Move a possible node to a child page and frees space on the
  current page.
  */
 bool move_node(Trace& trace, node_p* pnpos);
};
#pragma pack(0)

} // namespace leaves

#endif // _LEAVES_PAGE_HPP