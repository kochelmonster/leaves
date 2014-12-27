//@+leo-ver=4-thin
//@+node:michael.20141219202729.8:@shadow bittrie.cpp
//@@language cplusplus
//@@tabwidth -2
//@<< includes >>
//@+node:michael.20141219202729.11:<< includes >>
#include "bittrie.h"
//@nonl
//@-node:michael.20141219202729.11:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141220220750.3:Utils
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
//@nonl
//@-node:michael.20141220220750.3:Utils
//@+node:michael.20141220220750.4:struct TempNode
struct TempNode {
  Node* node; // points to data or theinserted note
  nodetype_t node;
  nodeid_t id; 
  char data[514]; // the biggest node



}
//@nonl
//@-node:michael.20141220220750.4:struct TempNode
//@+node:michael.20141220220750.9:PageManipulator
class PageManipulator {
 public:
  NodeRef& current() {
    }
  
  // returns the parent of current() parent is never a linknode
  NodeRef& parent() {
      
    }
  
  void add(TempNode& node, const Slice& key_add) {
   }

  void pop() {
    }

  // grows the current node by size bytes
  void grow_by(size_t size) {
    }


  // returns the node id of parent child
  // it handles linknodes transparent
  node_id child_of_parent() const {
      return;
    }
};
//@nonl
//@-node:michael.20141220220750.9:PageManipulator
//@+node:michael.20141215222649.83:Nodes
/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/



struct LeafMixin {
  void next(NodeRef& rnode, Trace& trace) {
      trace.parent_next();
    }
    
  void prev(Node* rnode, PageRef& page, Trace& trace) {
      trace.parent_prev();
    }
};

//@+others
//@+node:michael.20141215222649.85:Leaf
// a leaf node with data in Leaf memory
struct Leaf {
  offset_t pointer;
  size_t size;
};


struct LeafHandler : public NodeHandler, public LeafMixin {
  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
    }
    
  void first(NodeRef& rnode, Trace& trace) {
      //add_to_trace((Leaf*)node, page, context);
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      //add_to_trace((Leaf*)node, page, context);
    }
};

static LeafHandler leaf;
//@-node:michael.20141215222649.85:Leaf
//@+node:michael.20141215222649.84:PageLeaf
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
  void add_to_trace(PageLeaf *n, Trace& trace) {
      trace.key.append(n->data, n->key_size);
      trace.complete = true;
    }

  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      PageLeaf *n = (PageLeaf*)rnode.node();
      if (key.size() == n->key_size)
          && memcmp(key.data(), n->data, n->key_size) == 0) {
        add_to_trace(n, trace);
      }
    }
  
  void first(NodeRef& rnode, Trace& trace) {
      add_to_trace((PageLeaf*)rnode.node(), context);
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      add_to_trace((PageLeaf*)rnode.node(), context);
    }
    
  void add(const Slice& key, const Slice& value, NodeRef& rnode, Trace& trace) {
      PageLeaf *n = (PageLeave*)rnode.node();
      size_t prefix_size = common_prefix(key.data(), n->data, 
                                         std::min(key.size(), n->key_size));
      Slice mykey(n->data+prefix_size, n->key_size-prefix_size);
      Slice myvalue(n->data+key_size, n->value_size);
      TempLeaf me(mykey, myvalue);
      TempTrie trie;
      
      switch(prefix_size) {
        case 0:
          trace.change_node(trie);
          break;
        
        case 1: {
            TempTrie parent_trie;
            trace.change_node(parent_trie);
            trace.add_node(trie, key.slice(1));
            BitTrie* pn = (BitTrie*)trace.parent().node();
            pn->add(key.trie_index(), trace.child_of_parent());
            break;
          }
            
        default: {
            TempCompressed compressed(key.data(), prefix_size);
            trace.change_node(compressed);
            trace.add_node(trie, prefix_size)
            n = (PageLeaf*)trace.parent().node();
            n.child = trace.child_of_parent();
          }
      }

      trace.add_node(me, Slice());
      trace.key.append(mykey.data(), mykey.size());
      trace.complete = true;
      
      BitTrie* tn = (BitTrie*)trace.parent().node();
      tn->add(mykey.trie_index(), trace.child_of_parent());

      // pop back trace to trie
      trace.pop();
      trace.current().add(key.advance(prefix_size), value, context);
    }
};

static PageLeafHandler pageleaf;
//@nonl
//@-node:michael.20141215222649.84:PageLeaf
//@+node:michael.20141215222649.126:Compressed
// A Node containing a string part equal to all descendants
// the equal part fit into a page
struct Compressed {
  nodeid_t child;
  bsize_t size;
  char data[]
};


struct CompressHandler : public NodeHandler, public LeafMixin {
  void add_to_trace(Compressed* c, NodeRef& child, Trace& trace) {
      trace.push(child);
      trace.key.append(c->data, c->size);
    }

  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.gt_node();
      if (key.size() < n->size)
        return;
        
      if (memcmp(key.data(), n->data, n->size) == 0) {
        NodeRef child(rnode.page, n->child);
        add_to_trace(n, child, trace);
        child.find(key.advance(n->size), trace);
      }
    }
    
  void first(NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.gt_node();
      NodeRef child(rnode.page, n->child);
      add_to_trace(n, child, trace);
      child.first(trace);
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.gt_node();
      NodeRef child(rnode.page, n->child);
      add_to_trace(n, child, trace);
      child.last(trace);
    }
    
  void reinsert(Trace& trace, TempCompressed& me, 
                nodeid_t child, NodRef& rnode, trieindex_t index) {
      BitTrie* bn;
      switch(me.size()) {
        case 0:
          child = trace.move_node(rnode.page, child);
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, child);
          break;
          
        case 1:
          trace.add_node(TempTrie(), Slice(index));
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, trace.child_of_parent());
          child = trace.move_node(rnode.page, child);
          bn = (BitTrie*)trace.current().node();
          bn->add(me.data()[0], child);
          trace.pop()
          break;
          
        default: {
          trace.add(me, Slice(index));
          child = trace.move_node(rnode.page, child);
          Compressed *n = (Compressed*)trace.current().node()
          n->child = child;
          trace.pop();
          break;
      }
    }
    
  void add(const Slice& key, const Slice& value, NodRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.node();
      size_t prefix_size = common_prefix(key.data(), n->data,
                                         std::min(n->size, key.size()));
      trieindex_t first = n->data[0], second = n->data[1];
      TempCompressed me(n->data+prefix_size+1, n->size-prefix_size-1);
      nodeid_t child = n->child
      
      switch(prefix_size) {
        case 0: {
            // insert one trie
            trace.change_node(TempTrie());
            reinsert(trace, me, rnode, child, first);
            break;
          }
        
        case 1: {
            // insert two tries
            trace.change_node(TempTrie());
            trace.add_node(TempTrie(), Slice(first));
            BitTrie* bn = (BitTrie*)trace.parent().node();
            bn->add(first, trace.child_of_parent());
            reinsert(trace, me, rnode, child, second);
            break;
          }
            
        default: {
            // another compressed
            TempCompressed compressed(key.data(), prefix_size);
            first = n->data[prefix_size+1];
            trace.change_node(compressed);
            trace.add(trie, Slice(compressed.data(), compressed.size());
            n = (Compressed*)trace.current().node();
            n->child = trace.child_of_parent();
            reinsert(trace, me, rnode, child, first);
            break;
          }
      }

      trace.current().add(key.advance(prefix_size), value, context);
    }
};

static CompressedHandler compressed;


//@-node:michael.20141215222649.126:Compressed
//@+node:michael.20141215222649.137:Link
// links to another page

struct LinkHandler : public NodeHandler, public LeafMixin {
  NodeRef link_node(NodeRef& rnode, Trace& trace) {
      pageid_t *page_id = (pageid_t*)rnode.link();
      PageRef next_page(trace.map.get_page(*page_id));
      NodeRef child(next_page, 0)
      trace.push(child);
      return child;
    }

  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      link_node(rnode, trace).find(key, trace);
    }
    
  void first(NodeRef& rnode, Trace& trace) {
      link_node(rnode, trace).first(trace);
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      link_node(rnode, trace).last(trace);
    }
};

static LinkHandler link;
//@nonl
//@-node:michael.20141215222649.137:Link
//@+node:michael.20141215222649.93:Trie
// Node with the complete alphabet 
// children count is > 56
struct Trie {
  nodeid_t children[64];
};

struct TrieHandler : public NodeHandler {
  virtual void add_to_trace(int key, NodeRef& child, Trace& trace) {
      trace.push(child);
      if (key >=0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }
  
  void add(const Slice& key, const Slice& value, NodeRef& rnode, Trace& trace) {
      Trie *n = (Trie*)rnode.node();
      TempLeaf child(key.advance(1), value);
      trace.add_node(child, key.slice(1));  // trace is now at child
      
      if (key.empty()) {
        *trace.parent().extra() = trace.child_of_parent())
      }
      else {
        n = (Trie*)trace.parent().node();
        n->children[key.trie_index()] = trace.child_of_parent();
      }
    }
    
  bool remove_last_index(NodeRef& rnode, Trace& trace) {
      if (trace.last_index < 0) {
        *rnode.exra() = 0;
        return true;
      }

      Trie *n = (Trie*)rnode.node();
      n->children[trace.last_index] = 0;
      
      size_t count = 0;
      for(int i = 0; i < 64; i++) {
        if (n->children[i])
          count++;
      }
      if (count < 56) {
        TempTrie trie(n);
        trace.change_node(trie);
      }

      return true;
    }
    
    
  //@  @+others
  //@+node:michael.20141220220750.2:Navigation
  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      if (key.empty()) {
        nodeid_t *end_child = rnode.extra();
        if (*end_child) {
          NodeRef child(rnode.page, *end_child);
          add_to_trace(-1, child, trace);
          child.first(trace);
        }
        return;
      }

      Trie *n = (Trie*)rnode.node();
      trieindex_t index = key.trie_index();
      nodeid_t child_id= n->children[index];
      if (child_id) {
        NodeRef child(rnode.page, child_id)
        add_to_trace(index, child, trace);
        child.find(key.advance(1), context);
      }
    }

  void next(NodeRef& rnode, Trace& trace) {
      int index = trace.last_index < 0 ? 0 : (trace.last_index + 1);
      Trie *n = (Trie*)rnode.node();
      for(; index < 64; index++) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          NodeRef child(rnode.page, child_id);
          add_to_trace(index, child, trace);
          child.first(trace);
          return;
      }

      trace.parent_next();
    }
    
  void prev(NodeRef& rnode, Trace& trace) {
      if (trace.last_index < 0) {
        trace.parent_prev();
        return;
      }

      int index = trace.last_index - 1;
      Trie *n = (Trie*)rnode.node();
      for(; index >= 0; i--)
        nodeid_t child_id = n->children[index];
        if (child_id) {
          NodeRef child(rnode.page, child_id);
          add_to_trace(index, child, trace);
          child.last(trace);
          return;
        }
      }
      
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child);
        add_to_trace(index, child, trace);
        child.last(trace);
      }
      else {
        trace.parent_prev();
      }
    }

  void first(NodeRef& rnode, Trace& trace) {
      nodeid_t *end_child = rnode.extra();

      if (*end_child ) {
        NodeRef child(rnode.page, *end_child)
        add_to_trace(-1, child, trace);
        child.first(trace);
        return;
      }

      Trie *n = (Trie*)node;
      for(int index = 0; index < 64; i++) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          NodeRef child(rnode.page, child_id)
          add_to_trace(index, child, trace);
          child.first(trace);
          return;
        }
      }
      assert(0);
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      Trie *n = (Trie*)rnode.node();
      for(int index = 63; index >= 0; i--) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          NodeRef child(rnode.page, child_id);
          add_to_trace(index, child, trace);
          child.last(trace);
          return;
        }
      }
      assert(0);
    }


  //@-node:michael.20141220220750.2:Navigation
  //@-others
};

static TrieHandler trie;


//@-node:michael.20141215222649.93:Trie
//@+node:michael.20141215222649.95:BitTrie
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
    
  void remove(trieindex_t index) {
      int child_index = get_child_index(index);
      assert(child_index >= 0);
      bits &= ~(1 << index);
      memmove(children+child_index+1, children+child_index,
              count()-child_index-1);
    }
};


struct BitTrieHandler : public NodeHandler {
  void add_to_trace(int key, NodeRef& child, Trace& trace) {
      trace.push(child);
      if (key >= 0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }

  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      if (key.empty()) {
        nodeid_t *end_child = rnode.extra();
        if (*end_child) { 
          NodeRef child(rnode.page, *end_child);
          add_to_trace(-1, child, trace);
          child.first(trace);
        }
        return;
      }
        
      trieindex_t index = key.trie_index();
      BitTrie *n = (BitTrie*)rnode.node();
      int child_index = n->get_child_index(index);
      if (child_index >= 0) {
        nodeid_t child_id = n->children[child_index];
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, context);
        child.find(key.advance(1), trace);
      }
    }
    
  void next(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      int index = trace.last_index;
      
      if (index < 0) {
        int first_bit = n->first_bit();
        NodeRef child(rnode.page, n->children[0]).first();
        add_to_trace(first_bit, child, trace);
        return;
      }
     
      int child_index = n->get_child_index(index) + 1;
      if (child_index < n->count()) {
        index = n->next_bit(index)
        NodeRef child(rnode.page, n->children[child_index]);
        add_to_trace(index, child, trace);
        child.first();
        return;
      }
      
      trace.parent_next();
    }
    
  void prev(NodeRef& rnode, Trace& trace) {
      int index = trace.last_index;
      if (index < 0) {
        trace.parent_prev();
        return;
      }
      
      BitTrie *n = (BitTrie*)rnode.node();
      int child_index = n->get_child_index(index) - 1;
      if (child_index >= 0) {
        index = n->prev_bit(index);
        NodeRef child(rnode.page, n->children[child_index]);
        add_to_trace(index, child, trace);
        child.last(trace);
        return
      }
    
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child);
        add_to_trace(-1, child, trace);
        child.last(trace);
        return;
      }

      trace.parent_prev();
    }
    
  void first(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child)
        add_to_trace(-1, child, trace);
        child.first(trace);
      }
      else {
        trieindex_t index = n->first_bit();
        NodeRef child(rnode.page, n->children[0]);
        add_to_trace(index, rnode, context);
        child.first(trace);
      }
    }
    
  void last(Node* node, Trace& trace) {
      trieindex_t index = n->last_bit();
      int index = n->count() - 1;
      NodeRef child(rnode.page, n->children[index])
      add_to_trace(index, child, trace);
      child.first();
    }
    
  void add(const Slice& key, const Slice& value, NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      TempLeaf child(key.advance(1), value);
      
      if (key.empty()) {
        trace.add_node(child, Slice());
        *trace.parent().node().extra() = trace.child_of_parent();
        trace.complete = true;
        return;
      }
      
      size_t count = n->count();
      switch(count) {
        case 56: {
            TempTrie trie(n);
            trace.change_node(trie);
            trace.add_node(child, key.slice(1));
            Trie *np = (Trie*)trace.parent().node();
            np->children[key.trie_index()] = trace.child_of_parent();
            break;
          }
      
        case 8:
        case 24:
        case 40:
          trace.grow_by(16);
          
        default: 
          trace.add_node(child, key.slice(1));
          // renew pointer add could change it
          n = (BitTrie*)trace.parent().node();
          n->add(key.trie_index(), trace.child_of_parent());
        }
      }
      key = key.adavance(2):
      trace.key.append(key.data(), key.size());
      trace.complete = true;
    }
    
  bool remove_last_index(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      if (trace.last_index < 0) {
        *rnode.exra() = 0;
        if (n->count() == 1)
          change_node(NodeRef(rnode.page, n->children[0]), 
                      n->first_bit(), trace);
        return true;
      }
      
      n->remove(trace.last_index);
      switch(n->count()) {
        case 0: 
          if (*rnode.exra())
            change_node(NodeRef(rnode.page, *rnode.exra()), -1, trace);
          else
            return false;
        
          break;
          
        case 1: 
          if (!*rnode.exra())
            change_node(NodeRef(rnode.page, n->children[0]), 
                        n->first_bit(), trace);
          break;
      
        case 24:
        case 40:
          trace.shrink_node_by(16);
      }
       
      return true;
    }
    
  void change_node(NodeRef& child, int index, Trace& trace) {
      switch(child.type()) {
        case kCompressed: {
            Compressed* n = (Compressed*)child.node();
            if (n->size < 0xff) {
              TempCompressed compressed;
              compressed.node.data[0] = (char)index;
              memcpy(node.data+1, n->data, n->size);
              compressed.node.size = n->size+1;
              child.page.free_node(child.id, trace.map);
              trace.change_node(compressed);
            }
            break;
          }
                
        case kPageLeaf: 
        case kLeaf: {
          TempLeaf leaf(child);
          string key_buffer;
          if (index >= 0) {
            // we have to enhance the key
            key_buffer.append(leaf.key.data(), leaf.key.size());
            key_buffer.insert(0, 1, (char)index);
            leaf.key = Slice(key_buffer.data(), key_buffer.size());
          }
          child.page.free_node(child.id, trace.map);
          trace.change_node(leaf);
        }
      }
    }
};


static BitTrieHandler bittrie;


//@-node:michael.20141215222649.95:BitTrie
//@-others

NodeHandler* NodeHandler::handlers[200] = {
  &leaf;
  &pageleaf,
  &link,
  &trie,
  &bittrie,
  &compressed,
};
//@nonl
//@-node:michael.20141215222649.83:Nodes
//@-others
} // namespace larch_leaves 
//@nonl
//@-node:michael.20141219202729.8:@shadow bittrie.cpp
//@-leo
