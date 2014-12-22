#include "bittrie.h"

namespace larch_leaves {

inline size_t common_prefix(const char* s1, const char *s2, size_t size) {
  size_t i;
  size = std::min(size, 0xff);
  for(i = 0; i < size && *s1 == *s2; i++, s1++, s2++);
  return i;
}

// round to the next 16 byte
inline size_t node_size(size_t s) {
  return ((s+15)/16) * 16;
}
struct TempNode {
  Node* node; // points to data or theinserted note
  nodetype_t node;
  nodeid_t id; 
  char data[514]; // the biggest node



}
class PageManipulator {
 public:
  NodeRef& current() {
    }
  
  NodeRef& parrend() {
    }
  
  void add(TempNode& node, const Slice& key_add) {
   }

  void pop() {
    }

  // grows the current node by size bytes
  void grow_by(size_t size) {
    }

};
/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/



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
  size_t size;
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
      size_t prefix_size = common_prefix(key.data(), n->data, 
                                         std::min(key.size(), n->key_size));
      Slice mykey(n->data+prefix_size, n->key_size-prefix_size);
      Slice myvalue(n->data+key_size, n->value_size);
      TempLeaf me(mykey, myvalue);
      TempTrie trie;
      int index = trie.add(mykey);
      
      switch(prefix_size) {
        case 0:
          pm.change(rnode.id, trie);
          break;
        
        case 1: {
            TempTrie parent_trie;
            pm.change(rnode.id, parent_trie);
            pm.add(trie, key.slice(1));
            BitTrie* pn = (BitTrie*)pm.parent.get_node();
            pn->add(key.trie_index(), trie.id);
            break;
          }
            
        default: {
            TempCompressed compressed(key.data(), prefix_size);
            pm.change(rnode.id, compressed);
            pm.add(trie, prefix_size)
            n = (PageLeaf*)pm.parent().get_node();
            n.child = 
          }
      }

      pm.add(me, mykey);
      
      BitTrie* tn = (BitTrie*)pm.parent.get_node();
      tn->add(mykey.trie_index(), trie.id);

      // pop back trace to trie
      pm.pop();
      pm.current().add(key.advance(prefix_size), value, context);
    }
};

static PageLeafHandler pageleaf;
// A Node containing a string part equal to all descendants
// the equal part fit into a page
struct Compressed {
  nodeid_t child;
  bsize_t size;
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
    
  void add(const Slice& key, const Slice& value,
           NodRef& rnode, HandlerContext& context) {
      Compressed *n = (Compressed*)rnode.get_node();
      
      PageManipulator pm(context);      
      size_t prefix_size = common_prefix(key.data(), n->data,
                                         std::min(n->size, key.size()));
      TempCompressed me(n->data+prefix_size, n->size-prefix_size);
      TempTrie trie;
      nodeid_t child = n->child;
      switch(prefix_size) {
        case 0: {
            pm.change(rnode.id, trie);
            pm.add(me, me.data());
            break;
          }
        
        case 1: {
            TempTrie parent_trie;
            pm.change(rnode.id, parent_trie);
            pm.add(trie);
            
            BitTrie* bn = (BitTrie*)pm.parent().get_node();
            bn->add(key.trie_index(), trie.id);
            
            if (me.size() == 1) {
              child = pm.move(rnode.page, child);
              bn = (BitTrie*)pm.current().get_node();
              bn->add(me.data().trie_index(), child);
            }
            else {  
              pm.add(me, me.data());
              child = pm.move(rnode.page, child);
              n = (Compressed*)pm.current().get_node();
              n->child = child;
            }
            break;
          }
            
        default: {
            TempCompressed compressed(key.data(), prefix_size);
            pm.change(rnode.id, compressed);
            pm.add(trie)
            compressed.link(trie);
          }
      }

      pm.add(me);
      trie.set_child(index);
      
      // pop back trace to trie
      context.trace.nodes.back().pop(context);
      context.trace.nodes.back().add(key.advance(prefix_size), value, context);

      Slice mykey(n->data+prefix_size, n->key_size-prefix_size);
      TempCompressed me(mykey, myvalue);
                                                                        
      Slice mykey(n->data+prefix_size, n->key_size-prefix_size);
      Slice myvalue(n->data+key_size, n->value_size);
      TempLeaf me(mykey, myvalue);
      TempTrie trie;
      int index = trie.add(mykey);


};

static CompressedHandler compressed;
// links to another page
struct Link {
  pageid_t page;
};

struct LinkHandler : public NodeHandler, public LeafMixin {
  NodeRef link_node(NodeRef& rnode, HandlerContext& context) {
      Link *l = (Link*)node.get_node();
      PageRef next_page(context.get_page(l->page));
      context.trace.nodes.push_back(rnode);
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
    
  void pop(NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.pop_back();
      NodeRef& parent(context.trace.nodes.back());
      parent.pop(context);
    }
};

static LinkHandler link;
// Node with the complete alphabet 
// children count is > 56
struct Trie {
  nodeid_t children[64];
};

struct TrieHandler : public NodeHandler {
  virtual void add_to_trace(int key, NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.push_back(rnode);
      if (key >=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }
  
  void pop(Node* node, HandlerContext& context) {
      context.trace.nodes.pop_back();
      if (context.trace.last_was_empty)
        context.trace.last_was_empty = false
      else
        context.trace.key.pop_back();
  }
  
  void add(const Slice& key, const Slice& value,
           NodeRef& rnode, HandlerContext& context) {
      Trie *n = (Trie*)rnode.get_node();
      PageManipulator pm(context);
      TempLeaf child(key.advance(1), value);
      pm.add(child, key.slice(1));  // trace is now at child
      
      if (key.empty()) {
        pm.parent().extra(child.id)
      }
      else {
        n = (Trie*)pm.parent().get_node();
        n->children[key.trie_index()] = child.id;
      }
    }
    
  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      if (key.empty()) {
        nodeid_t end_child = rnode.extra();
        if (end_child) { 
          add_to_trace(-1, rnode, context);
          NodeRef(rnode.page, end_child).first(context);
        }
        return;
      }

      Trie *n = (Trie*)node.get_node();
      trieindex_t index = key.trie_index();
      nodeid_t child = n->children[index];
      if (child) {
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, child).find(key.advance(1), context);
      }
    }

  void next(NodeRef& rnode, HandlerContext& context) {
      trieindex_t *index;
          
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        context.trace.key.append((char)0);
        index = (trieindex_t*)&trace.key.back();
      }
      else {
        index = (trieindex_t*)&trace.key.back();
        (*index)++;
      }
          
      Trie*n = (Trie*)rnode.get_node();
      for(; *index < 64; (*index)++) {
        nodeid_t child = n->children[*index];
        if (child) {
          NodeRef(rnode.page, child).first();
          return;
      }

      parent_next(rnode, context);
    }
    
  void prev(NodeRef& rnode, HandlerContext& context) {
      if (context.trace.last_was_empty) {
        parent_prev(rnode, context);
        return;
      }

      trieindex_t *index = (trieindex_t*)&context.trace.key.back());
      Trie *n = (Trie*)rnode.get_node();
      while(*index > 0) {
        (*index)--;
        nodeid_t child = n->children[*index];
        if (child) {
          NodeRef(rnode.page, child).last();
          return;
        }
      }
      
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        context.trace.key.pop_back();
        context.trace.last_was_empty = true;
        NodeRef(rnode.page, end_child).last(context);
      }
      else {
        parent_prev(rnode, context);
      }
    }

  void first(NodeRef& rnode, HandlerContext& context) {
      nodeid_t end_child = rnode.extra();

      if (end_child ) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, end_child ).first();
      }

      Trie *n = (Trie*)node;
      for(int index = 0; index < 64; i++) {
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
      Trie *n = (Trie*)rnode.get_node();
      for(int index = 63; index >= 0; i--) {
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

static TrieHandler trie;


// Node with a range


struct BitTrie {
  boost::uint64_t bits;
  nodeid_t children[];

  size_t count() const {
      return popcount(bits);    
    }
  
  int get_child_index(trieindex_t index) const {
      if (bits & (1<<index)) {
        boost::uint64_t mask = ~0;
        mask <<= index;
        return popcount(bits & ~mask)
      }
      return -1;
    }
    
  int first_bit() const {
      return ffs(bits) - 1;
   }
  
  int lastbit() const {
      return 64 - clz(bits)
    }
  
  int next_bit(trieindex_t index) const {
      boost::uint64_t mask = ~0;
      mask <<= index;
      return ffs(bits & mask) - 1;
    }
    
  int prev_bit(trieindex_t index) const {
      boost::uint64_t mask = ~0;
      mask <<= index;
      return 64 - clz(bits & ~mask);
    }
    
  void add(trieindex_t index, nodeid_t node) {
      bits |= 1 << index;
      int child_index = get_child_index(index); 
      memove(children+child_index, children+child_index+1,
             count()-child_index-1);
      children[child_index] = node;
    }
};


struct BitTrieHandler : public NodeHandler {
  void add_to_trace(int key, NodeRef& rnode, HandlerContext& context) {
      context.trace.nodes.push_back(rnode);
      if (key >=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }

  void find(const Slice& key, NodeRef& rnode, HandlerContext& context) {
      if (key.empty()) {
        nodeid_t end_child = rnode.extra();
        if (end_child) { 
          add_to_trace(-1, rnode, context);
          NodeRef(rnode.page, end_child).first(context);
        }
        return;
      }
        
      trieindex_t index = key.trie_index();
      BitTrie *n = (BitTrie*)rnode.get_node();
      int child_index = n->get_child_index(index);
      if (child_index >= 0) {
        add_to_trace(index, rnode, context);
        nodeid_t child = n->children[child_index];
        NodeRef(rnode.page, child).find(key.advance(1), context);
      }
    }
    
  void next(NodeRef& rnode, HandlerContext& context) {
      BitTrie *n = (BitTrie*)rnode.get_node();
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        nodeid_t first_bit = n->first_bit();
        context.trace.key.append((char)first_bit);
        NodeRef(rnode.page, n->children[0]).first();
        return;
      }
      
      trieindex_t *index = (trieindex_t*)&trace.key.back();
      int child_index = n->get_child_index(*index) + 1;
      if (child_index < n->count()) {
        *index = n->next_bit(*index)
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
      
      BitTrie *n = (BitTrie*)rnode.get_node();
      trieindex_t *index = (trieindex_t*)&trace.key.back();
      int child_index = n->get_child_index(*index) - 1;
      if (child_index >= 0) {
        *index = n->prev_bit(*index);
        NodeRef(rnode.page, n->children[child_index]).last();
        return
      }
    
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        context.trace.key.pop_back();
        context.trace.last_was_empty = true;
        NodeRef(rnode.page, n->end).last(context);
        return;
      }
  
      parent_prev(rnode context);
    }
    
  void first(NodeRef& rnode, HandlerContext& context) {
      BitTrie *n = (BitTrie*)rnode.get_node();
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, end_child).first();
      }
      else {
        trieindex_t index = n->first_bit();
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, n->children[0]).first();
      }
    }
    
  void last(Node* node, PageRef& page, HandlerContext& context) {
      trieindex_t index = n->last_bit();
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
    
  void add(const Slice& key, const Slice& value,
           NodeRef& rnode, HandlerContext& context) {
      BitTrie *n = (BitTrie*)rnode.get_node();
      PageManipulator pm(context);      
      TempLeaf child(key.advance(1), value);
      
      if (key.empty()) {
        pm.add(child);
        pm.parent().get_node().extra(child.id);
        return;
      }
      
      size_t count = n->count();
      switch(count) {
        case 56: {
            TempTrie trie(n);
            pm.change(rnode.id, trie);
            pm.add(child);
            Trie *np = (Trie*)pm.parent().get_node();
            np->children[key.trie_index()] = child.id;
            break;
          }
      
        case 8:
        case 24:
        case 40:
          pm.grow_by(16);
          
        default: 
          pm.add(child);
          // renew pointer add could change it
          n = (BitTrie*)pm.parent().get_node();
          n->add(key.trie_index(), child.id);
      }
    }    
};

static BitTrieHandler bittrie;

NodeHandler* NodeHandler::handlers[200] = {
  &leaf;
  &pageleaf,
  &link,
  &trie,
  &bittrie,
  &compressed,
};
} // namespace larch_leaves 
