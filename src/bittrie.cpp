#include "bittrie.h"

namespace larch_leaves {

inline size_t common_prefix(const char* s1, const char *s2, size_t size) {
  for(size_t i = 0; i < size && *s1 == *s2; i++, s1++, s2++);
  return i;
}

// round to the next 16 byte
inline size_t node_size(size_t s) {
  return ((s+15)/16) * 16;
}
struct TempNode {
  Node* node; // points to data or theinserted note
  nodetype_t node;
  char data[514]; // the biggest node



}
/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/


// links do not implement these methods
struct LinkMixin {
  virtual void next(NodeRef& rnode, HandlerContext& context) {
      assert(0);
    }
  virtual void prev(NodeRef& rnode, HandlerContext& context)  {
      assert(0);
    }
    
  virtual void pop(NodeRef& rnode, HandlerContext& context) {
      assert(0);
    }
};


struct LeafMixin {
  void next(NodeRef& rnode, HandlerContext& context) {
      parent_next(rnode, context);
    }
    
  void prev(Node* rnode, PageRef& page, HandlerContext& context) {
      parent_prev(rnode, context);
    }
  
};

// a leaf node with data in Leaf memory
struct Leaf {
  offset_t pointer;
};


struct LeafHandler : public NodeHandler, public LeafMixin {
  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
    }
    
  void first(NodeRef& rnode, HandlerContext& context) {
      //add_to_trace((Leaf*)node, page, context);
    }
    
  void last(NodeRef& rnode, HandlerContext& context) {
      //add_to_trace((Leaf*)node, page, context);
    }
    
  void pop(NodeRef& rnode, HandlerContext& context) {
    }
};

static LeafHandler leaf;
// a leaf node with key and values <= 256 bytes
// the leag is saved in the page
struct PageLeaf {
  boost::uint8_t key_size;
  boost::uint8_t value_size;
  char data[];
  
  size_t size() const {
      return node_size(sizeof(key_size)+sizeof(value_size)+key_size+value_size);
    }
};



struct PageLeafHandler : public NodeHandler, public LeafMixin {
  void add_to_trace(NodeRef& rnode, PageLeaf *n, HandlerContext& context) {
      context.trace.nodes.push_back(rnode);
      context.trace.key.append(n->data, n->key_size);
    }

  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      PageLeaf *n = (PageLeaf*)rnode.get_node();
      if (key.size() == n->key_size)
          && memcmp(key.data(), n->data, n->key_size) == 0) {
        add_to_trace(rnode, n, context);
      }
    }
  
  void first(NodeRef& rnode, HandlerContext& context) {
      add_to_trace(rnode, (PageLeaf*)rnode.get_node(), context);
    }
    
  void last(NodeRef& rnode, HandlerContext& context) {
      add_to_trace(rnode, (PageLeaf*)rnode.get_node(), context);
    }
  
  void pop(NodeRef& rnode, HandlerContext& context) {
      PageLeaf *n = (PageLeave*)rnode.get_node();
      context.trace.nodes.pop_back();
      context.trace.key.resize(key.size()-n->key_size);
    }
    
  void add(const Slice& key, const Slice& value,
           NodeRef& rnode, HandlerContext& context) {
      PageLeaf *n = (PageLeave*)rnode.get_node();
      
      PageManipulator pm(context);      
      size_t prefix = common_prefix(key.data(), n->data, 
                                    std::min(key.size(), n->key_size));
      Slice mykey(n->data+prefix, n->key_size - prefix);
      Slice myvalue(n->data+key_size, n->value_size);
      Slice restkey = key.advance(prefix);
      TempLeaf child1(mykey, myvalue), child2(rest_key, value);
      TempTrie trie;
      int index1 = trie.add(mykey), index2 = trie.add(rest_key);
      
      switch(prefix) {
        case 0:
          pm.change(rnode.id, trie);
          break;
        
        case 1: {
            TempTrie parent_trie;
            int index = parent_trie.add(n->data[0]);
            pm.change(rnode.id, parent_trie);
            pm.add(trie);
            parent_trie.set_child(index, trie);
            break;
          }
            
        default: {
            TempCompressed compressed(n->data, prefix);
            pm.change(rnode.id, compressed);
            pm.add(trie)
            compressed.link(trie);
          }
      }
                  
      pm.add(child1);
      trie.set_child(index1);
      pm.add(child2);
      trie.set_child(index2);
    }
};

static PageLeafHandler pageleaf;
// links to another page
struct Link {
  pageid_t page;
};

struct LinkHandler : public NodeHandler, public LinkMixin {
  NodeRef link_node(NodeRef& rnode, HandlerContext& context) {
      Link *l = (Link*)node.get_node();
      PageRef next_page(context.map.get_page(l->page));
      return NodeRef(next_page, 0);
    }

  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      link_node(rnode, context).find(key, context);
    }
    
    void first(NodeRef& rnode, HandlerContext& context) {
      link_node(rnode, context).first(context);
    }
    
    void last(NodeRef& rnode, HandlerContext& context) {
      link_node(rnode, context).last(context);
    }
};

static LinkHandler link;
// Node with the complete alphabet 
// children count is > 56
struct Node64 {
  nodeid_t children[64];
};

struct Node256Handler : public NodeHandler {
  virtual void add_to_trace(int key, NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.push_back(rnode);
      context.trace.key.append((char)key);
    }
  
  void pop(NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.pop_back();
      context.trace.key.pop_back();
  }
  
  bool _prev(NodeRef& rnode, HandlerContext& context) {
      trieindex_t *index = (trieindex_t*)&context.trace.key.back());
      Node64 *n = (Node64*)rnode.get_node();
      while(*index > 0) {
        (*index)--;
        nodeid_t child = n->children[*index];
        if (child) {
          NodeRef(rnode.page, child).last();
          return false;
        }
      }
      return true;
    }

  nodeid_t _next(NodeRef& rnode, HandlerContext& context) {
      trieindex_t *index = (trieindex_t*)&trace.key.back();
      Node64 *n = (Node64*)rnode.get_node();
      while(*index < 0xff) {
        (*index)++;
        nodeid_t child = n->children[*index];
        if (child) 
          return child;
      }
      return 0;
    }

  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      if (key.empty())
        // this node type has no empty
        return;

      Node256 *n = (Node256*)node.get_node();
      trieindex_t index = key.trie_index();
      nodeid_t child = n->children[index];
      if (child) {
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, child).find(key.advance(1), context);
      }
    }

  void next(NodeRef& rnode, HandlerContext& context) {
      nodeid_t child = _next(node, page, context);
      if (child)
        NodeRef(rnode.page, child).first();
      else
        parent_next(rnode, context);
    }
    
  void prev(NodeRef& rnode, HandlerContext& context) {
      if (_prev(rnode, context)) 
        parent_prev(rnode, context);
    }

  void first(NodeRef& rnode, HandlerContext& context) {
      Node256 *n = (Node256*)node;
      for(int index = 0; index <= 0xff; i++) {
        nodeid_t child = n->children[index];
        if (child) {
          add_to_trace(index, rnode, context);
          NodeRef(rnode.page, child).first();
          return;
        }
      }
      assert(0);
    }
    
  void last(NodeRef& rnode, HandlerContext& context) {
      Node256 *n = (Node256*)rnode.get_node();
      for(int index = 0xff; index >= 0; i--) {
        nodeid_t child = n->children[index];
        if (child) {
          add_to_trace(index, rnode, context);
          NodeRef(rnode.page, child).last(context);
          return;
        }
      }
      assert(0);
    }
};

static Node256Handler node256;


// Node with 257 values (256 for each byte value + 1 for end)
struct Node257 {
  nodeid_t children[256];
  nodeid_t end;
};

struct Node257Handler : public Node256Handler {
  virtual void add_to_trace(int key, NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.push_back(rnode);
      if (key >=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }
    
  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      if (key.empty()) {
        Node257 *n = (Node257*)rnode.get_node();
        if (n->end) { 
          add_to_trace(-1, rnode, context);
          NodeRef(rnode.page, n->end).first(context);
        }
        return;
      }
      Node256Handler::find(key, node, page, context);
    }
  
  void next(NodeRef& rnode, HandlerContext& context) {
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        nodeid_t child = _next(rnode, context); // child must be != NULL
        NodeRef(rnode.page, child).first();
        return;
      }
      Node256Handler::next(rnode, context);
    }
    
  void prev(NodeRef& rnode, HandlerContext& context) {
      if (context.trace.last_was_empty) {
        parent_prev(rnode, context);
        return;
      }
    
      if (_prev(rnode, page, context)) {
        Node257 *n = (Node257*)node.get_node();
        if (n->end) {
          context.trace.key.pop_back();
          context.trace.last_was_empty = true;
          NodeRef(rnode.page, n->end).last(context);
        }
        else {
          parent_prev(rnode, context);
      }
    }
  }

  void first(NodeRef& rnode, HandlerContext& context) {
      Node257 *n = (Node257*)rnode.get_node();
      if (n->end) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, n->end).first();
      }
      else {
        Node256Handler::first(node, page, context);
      }
    }
  
  void pop(Node* node, HandlerContext& context) {
      context.trace.nodes.pop_back();
      if (context.trace.last_was_empty)
        context.trace.last_was_empty = false
      else
        context.trace.key.pop_back();
  }
};

static Node257Handler node257;


    
// A Node containing a string part equal to all descendants
struct Compressed {
  nodeid_t child;
  boost::uint8_t size;
  char data[]
};



struct CompressHandler : public NodeHandler, public LeafMixin {
  void add_to_trace(Compressed* c, NodeRef& rnode, HandlerContext& context) {
            context.trace.nodes.push_back(rnode);
      context.trace.key.append(c->data, c->size);
    }

  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      Compressed *n = (Compressed*)rnode.gt_node();
      add_to_trace(n, rnode, context);
      if (key.size() < n->size)
        return;
        
      if (memcmp(key.data(), n->data, n->size) == 0) {
        NodeRef(rnode.page, n->child).find(key.advance(n->size), context);
      }
    }
    
  void first(NodeRef& rnode, HandlerContext& context) {
      add_to_trace((Compressed*)rnode.get_node(), rnode, context);
    }
    
  void last(NodeRef& rnode, HandlerContext& context) {
      add_to_trace((Compressed*)rnode.get_node(), rnode, context);
    }
    
  void pop(NodeRef& rnode, HandlerContext& context) {
      Compressed *n = (Compressed*)rnode.get_node();
      context.trace.nodes.pop_back();
      context.trace.key.resize(key.size()-n->size);
    }
};

static CompressedHandler compressed;
// Node with a range
#if UINTPTR_MAX == 0xffffffff

// maybe there is no intrenic

#ifdef __GNUC__
#  define popcount __builtin_popcountl
#endif



typedef unsigned long word_t;
#  elif UINTPTR_MAX == 0xffffffffffffffff
#    define popcount __builtin_popcountll
#  endif




#ifdef _MSC_VER
#  include <intrin.h>

#ifdef _M_X64
#pragma intrinsic(_BitScanForward64)
#define popcount __popcnt64
typedef boost::uint64_t word_t;
#else
#pragma intrinsic(_BitScanForward)
#define popcount __popcnt
typedef boost::uint32_t word_t;
#endif //_M_X64
#endif


#endif
#if UINTPTR_MAX == 0xffffffffffffffff


#ifdef __GNUC__
#include <stdint.h>
#  if UINTPTR_MAX == 0xffffffff
#    define popcount __builtin_popcountl
typedef unsigned long word_t;
#  elif UINTPTR_MAX == 0xffffffffffffffff
#    define popcount __builtin_popcountll
#  endif




#ifdef _MSC_VER
#  include <intrin.h>

#ifdef _M_X64
#pragma intrinsic(_BitScanForward64)
#define popcount __popcnt64
typedef boost::uint64_t word_t;
#else
#pragma intrinsic(_BitScanForward)
#define popcount __popcnt
typedef boost::uint32_t word_t;
#endif //_M_X64
#endif



#endif

inline int popcount64(boost::uint64_t b) {
  b = (b & 0x5555555555555555LU) + (b >> 1 & 0x5555555555555555LU);
  b = (b & 0x3333333333333333LU) + (b >> 2 & 0x3333333333333333LU);
  b = b + (b >> 4) & 0x0F0F0F0F0F0F0F0FLU;
  b = b + (b >> 8);
  b = b + (b >> 16);
  b = b + (b >> 32) & 0x0000007F;
  return (int) b;
}




struct BitNode32 {
  boost::uint32_t bits[1];
  nodeid_t end;
  nodeid_t children[];
  
  int get_child_index(trieindex_t index) {
      if (bits[0] & (1 << index)) {
        boost::uint32_t m = ~0;
        m >>= (31-index)
        return popcount32(bits[0] & m);
      }
      return -1;
    }
};

struct BitNode64 {
  union {
    boost::uint32_t bits[2];
    boost::uint64_t lbits[1];
  };
  nodeid_t end;
  nodeid_t children[];
};

struct BitNode128 {
  union {
    boost::uint32_t bits[4];
    boost::uint64_t lbits[2];
  };
  nodeid_t end;
  nodeid_t children[];
};

struct BitNode192 {
  boost::uint32_t bits[6];
  nodeid_t end;
  nodeid_t children[];
};

struct BitNode256 {
  union {
    boost::uint32_t bits[8];
    boost::uint64_t lbits[4];
  };
  nodeid_t end;
  nodeid_t children[];
};

template<typename node_type, size_t offset, size_t handler_id>
struct BitNodeHandler : public NodeHandler {
  void add_to_trace(int key, NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.push_back(rnode);
      if (key >=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }

  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      node_type *n = (node_type*)rnode.get_node();
      if (key.empty()) {
        if (n->end) {
          add_to_trace(-1, n, page, context);
          NodeRef(page, n->end.ptr, n->end.type).first(context);
        }
        return;
      }  
          
      trieindex_t index = key.trie_index();
      if (offset <= index && index < offset+node_type::range) {
        int child_index = n->get_child_index(index-offset);
        if (child_index >= 0) {
          add_to_trace(index, rnode, context);
          nodeid_t child = n->children[child_index];
          NodeRef(rnode.page, child).find(key.advance(1), context);
        }
      }
    }
    
  void next(NodeRef& rnode, HandlerContext& context) {
      node_type *n = (node_type*)rnode.get_node();
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        NodeRef(rnode.page, n->children[0]).first();
        return;
      }
      
      trieindex_t *index = (trieindex_t*)&trace.key.back();
      int child_index = n->get_child_index(*index-offset) + 1;
      *index = n->get_next_bit(*index-offset) + offset;
      
      if (child_index < n->count()) {
        NodeRef(rnode.page, n->children[child_index]).first();
        return
      }
      
      parent_next(rnode, context);
    }
    
  void prev(NodeRef& rnode, HandlerContext& context) {
      if (context.trace.last_was_empty) {
        parent_prev(rnode, context);
        return;
      }
      
      node_type *n = (node_type*)rnode.get_node();
      trieindex_t *index = (trieindex_t*)&trace.key.back();
      int child_index = n->get_child_index(*index-offset) - 1;
      *index = n->get_prev_bit(*index-offset) + offset;
      if (child_index >= 0) {
        NodeRef(rnode.page, n->children[child_index]).last();
        return
      }
    
      if (n->end) {
        context.trace.key.pop_back();
        context.trace.last_was_empty = true;
        NodeRef(rnode.page, n->end).last(context);
        return;
      }
  
      parent_prev(rnode context);
    }
    
  void first(NodeRef& rnode, HandlerContext& context) {
      node_type *n = (node_type*)rnode.get_node();
      if (n->end) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, n->end).first();
      }
      else {
        trieindex_t index = n->get_first_bit() + offset;
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, n->children[0]).first();
      }
    }
    
  void last(Node* node, PageRef& page, HandlerContext& context) {
      trieindex_t index = n->get_last_bit() + offset;
      bsize_t last = n->count() - 1;
      add_to_trace(index, rnode, context);
      NodeRef(rnode.page, n->children[last]).first();
    }
    
  void pop(Node* node, HandlerContext& context) {
      context.trace.nodes.pop_back();
      if (context.trace.last_was_empty)
        context.trace.last_was_empty = false
      else
        context.trace.key.pop_back();
    }
};

NodeHandler* NodeHandler::handlers[200] = {
  &leaf;
  &pageleaf,
  &link,
  &node256,
  &node257,
  &compressed,
};
} // namespace larch_leaves 
