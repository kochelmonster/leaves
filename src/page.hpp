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


struct node_p {
  union {
    struct {
      uint16_t type : 3;
      uint16_t offset : 13;
    };
    uint16_t val;
  };
};

/*
 A Page has two data types:
 Nodes
 and an Index lit
*/

struct Page {
  static const int MIN_SPACE =
      3 * sizeof(Trie) + 3 * sizeof(node_t) + sizeof(Value) + sizeof(stored_ptr);
  static const int MIN_COUNT = 5;
  
  uint16_t size;

  // ie for index entry
  uint16_t ie_count;
  uint16_t ie_free_head;
  uint16_t ie_free_count;

  char data[PAGE_SIZE - 5 * sizeof(uint16_t)];
  node_p root;

  Page* init() {
    size = 0;
    ie_count = 0;
    ie_free_count = 0;
    ie_free_head = 0;
    ie_count = 0;
    root.offset = 0;
    return this;
  }

  const node_p* get_ie(node_t index) const { return (&root - index); }
  node_p* get_ie(node_t index){ return (&root - index); }
  const Node* get_node(const node_p* pie) const { return (Node*)&data[pie->offset]; }
  Node* get_node(const node_p* pie) { return (Node*)&data[pie->offset]; }
  const Node* get_node(node_t index) const { return get_node(get_ie(index)); }
  Node* get_node(node_t index) { return get_node(get_ie(index)); }
  node_t copy_node(Page* dest, node_t id) const {
    return NodeHandler::HANDLERS[get_ie(id)->type]->copy_node(dest, this, id);
  }

  void grow(uint16_t offset, int delta);
  node_t alloc(uint16_t space, NodeType type);
  void free(node_t index, uint16_t size);
  bool reserve(int space, uint16_t links);
  void add(const Page* child) { child->copy_node(this, 0); }
  node_t find_split_node();
  void split(Storage& storage);
  stored_ptr write_page(Storage& storage);
  void free_page(Storage& storage);
};


#pragma pack(0)

}  // namespace leaves

#endif  // _LEAVES_PAGE_HPP