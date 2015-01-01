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
  Page page;
  PageRef pageref;
  NodeRef noderef;
  
  TempNode()
    : pageref(&page, 0, 0), noderef(pageref, 0) { 
      *noderef.extra() = 0;
    }
    
  size_t size() const {
      return noderef.size();
    }
    
  Node* node() const {
      return nodref.node();
    }
    
  bsize_t* extra() const {
      return nodref.extra();
    }

  nodetype_t type() const {
      return nodref.type();
    }
};


inline size_t pad16(size_t size) {
  size_t rest = size & 0xf
  if (rest)
    size += 16-rest
    
  return size;
}

struct TempLeaf : public TempNode {
  TempLeaf(const Slice& value)
    : TempNode() {
      size_t size = value.size()
      if (size > 255) {
        // Leaf
      }
      else {
        // PageLeaf
        pageref.new_node(pad16(size));
        *extra() = key.size();
        PageLeaf* n = (PageLeaf*)node();
        memcpy(n->data, value.data(), value.size());
        nodref.set_type(kPageLeaf);
      }
    }
};

struct TempTrie : public TempNode {
  TempTrie(size_t child_count=1) 
    : TempNode() {
      size_t size;
      if (child_count >= 56) {
        pageref.new_node(size=64);
        nodref.set_type(kTrie);
      }
      else {
        size = pag16(child_count);
        pageref.new_node(size);
        nodref.set_type(kBitTrie);
      }

      memset(node(), 0, size);
    }
};

struct TempCompressed : public TempNode {
  TempCompressed(const Slice& part)
    : TempNode() {
      size_t size(part.size());
      *extra() = (bsize_t)size;
      pageref.new_node(pad16(sizeof(nodeid_t)+size));
      Compressed* n = (Compressed*)node();
      memcpy(n->data, part.data(), size);
    }
};
/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/



struct LeafMixin {
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      return 0;
    }
    
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
    }
    
  void replace_child(NodeRef& rnode, int index, nodeid_t child) {
      // this may be only called in trie nodes
      assert(0);
    }

  void next(NodeRef& rnode, Trace& trace) {
      trace.parent_next();
    }
    
  void prev(Node* rnode, PageRef& page, Trace& trace) {
      trace.parent_prev();
    }
};


struct TrieMixin {
  virtual void add_node(const Slice& key, TempNode& node, Trace& trace) = 0;

  void add(const Slice& key, TempNode& end, NodeRef& rnode, Trace& trace) {
      switch(key.size()) {
        case 0:
          trace.add_node(end, Slice());
          *trace.parent().extra() = trace.child_of_parent());
          trace.complete = true;
          break;
          
        case 1: 
          add_node(key, end, trace);
          trace.complete = true;
          break;
          
        case 2:
          add_node(key.slice(1), TempTrie(), trace);
          trace.current().add(key.advance(1), end);
          break;
          
        default: {
          Slice next_key(key.advance(1));
          add_node(key.slice(1), TempCompressed(next_key), trace);
          trace.current().add(next_key, end);
          break;
      }
    }
};    
  

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
// a leaf node with key and values <= 256 bytes
// the key_size is saved in Nodeptr.extra
struct PageLeaf {
  char data[];
};


struct PageLeafHandler : public NodeHandler, public LeafMixin {
  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      if (!key.size())
        trace.complete = true;
    }
  
  void first(NodeRef& rnode, Trace& trace) {
      trace.complete = true;
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      trace.complete = true;
    }
    
  void reinsert_me(TempLeaf& me, Trace& trace) {
      trace.add_node(me, Slice());
      // extra() is the end node
      *trace.parent().extra() = trace.child_of_parent());
      pop(); // back to parent
    }
    
  void add(const Slice& key, TempNode& end, NodeRef& rnode, Trace& trace) {
      assert(key.size() != 0);
      PageLeaf *n = (PageLeave*)rnode.node();
      TempLeaf me(Slice(n->value, *rnode.extra()));
      TempTrie trie;
      Slice rest_key;
      
      if (key.size() == 2) {
        TempTrie parent_trie;
        trace.change_node(parent_trie);
        reinsert_me(me, trace);
        trace.add_node(trie, key.slice(1));
        BitTrie* pn = (BitTrie*)trace.parent().node();
        pn->add(key.trie_index(), trace.child_of_parent());
        rest_key = key.advance(1);
      }
      else {
        trace.change.node(trie);
        reinsert_me(me, trace);
        if (key.size() > 2) {
          rest_key = key.advance(1);
          TempCompressed compressed(rest_key);
          trace.add_node(compressed, key.slice(1));
          BitTrie* pn = (BitTrie*)trace.parent().node();
          pn->add(key.trie_index(), trace.child_of_parent());
        }
        else {
          rest_key = key;
        }
      }

      trace.current().add(rest_key, end, trace);
    }
};

static PageLeafHandler pageleaf;
// A Node containing a string part equal to all descendants
// the equal part fit into a page; the data size is in Nodeptr.extra
struct Compressed {
  nodeid_t child;
  char data[]
};


struct CompressHandler : public NodeHandler, public LeafMixin {
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      Compressed *n = (Compressed*)rnode.node();
      if (n->child) {
        children[0] = n->child;
        return 1;
      }
      return 0;
    }
    
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
      if (n->child) {
        n->child = children[0];
      }
    }

  void add_to_trace(Compressed* c, NodeRef& child, Trace& trace) {
      trace.push(child);
      trace.key.append(c->data, c->size);
    }
   
  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.node();
      bsize_t size = (bsize_t)*rnode.extra() ;
      if (key.size() < size)
        return;
        
      if (memcmp(key.data(), n->data, size) == 0) {
        NodeRef child(rnode.page, n->child);
        add_to_trace(n, child, trace);
        child.find(key.advance(size), trace);
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
    
  nodeid_t move_child(NodeRef& child, Trace& trace) {
      // moves the child and its descendant to current page
      PageRef& page(trace.current().page);
      if (page.id != child.page.id) {
        size_t sizes[256];
        memset(sizes, 0, sizeof(sizes));
        size_t size = calc_sizes(child, sizes);
        trace.reserve_space(size, current().id);
        nodeid_t r = trace.move_node(page, child);
        child.page.defragment();
        return r;
      }
      return child.id;
    }
    
  void reinsert(TempCompressed& rest_me, NodeRef& child, 
                trieindex_t index, Trace& trace) {
      BitTrie* bn;
      nodeid_t child_id;
      
      switch(rest_me.size()) {
        case 0:
          child_id = move_child(child, trace);
          bn = (BitTrie*)trace.current().node();
          bn->add(index, child_id);
          break;
          
        case 1:
          trace.add_node(TempTrie(), Slice(index));
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, trace.child_of_parent());
          
          child_id = move_child(child, trace);
          bn = (BitTrie*)trace.current().node();
          bn->add(me.data()[0], child_id);
          trace.pop()
          break;
          
        default: {
          trace.add(me, Slice(index));
          child_id = move_child(child, trace);
          Compressed *n = (Compressed*)trace.current().node()
          n->child = child_id;
          trace.pop();
          break;
      }
    }
    
  void add(const Slice& key, TempnNode& end, NodRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.node();
      bsize_t size = (bsize_t)*rnode.extra();
      
      if (!n->child) {
        // we insert a
        assert(size == key.size());
        trace.add_node(end, key);
        n->child = trace.child_of_parent();
        trace.complete = true;
        return;
      }
      
      size_t prefix_size = common_prefix(key.data(), n->data,
                                         std::min(size, key.size()));
      trieindex_t first = n->data[0], second = n->data[1];
      TempCompressed rest_me(n->data+prefix_size+1, size-prefix_size-1);
            
      // move my page descendants to a temporary page to insert them later
      NodeRef child(rnode.page, n->child);
      n->child = 0;
      
      switch(prefix_size) {
        case 0:
          // insert one trie
          trace.change_node(TempTrie());
          reinsert(rest_me, child, first, trace);
          break;

        case 1: {
          // insert two tries
          trace.change_node(TempTrie());
          trace.add_node(TempTrie(), Slice(first));
          BitTrie* bn = (BitTrie*)trace.parent().node();
          bn->add(first, trace.child_of_parent());
          reinsert(rest_me, child, second, trace);
          break;
          }
            
        default: {
          // another compressed
          Slice next_key(key.data(), prefix_size)
          TempCompressed compressed(next_key);
          first = n->data[prefix_size+1];
          trace.change_node(compressed);
          trace.add(trie, Slice(key.data(), compressed.size());
          n = (Compressed*)trace.current().node();
          n->child = trace.child_of_parent();
          reinsert(rest_me, child, first, trace);
      }

      trace.current().add(key.advance(prefix_size), end, context);
    }
};

static CompressedHandler compressed;


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
// Node with the complete alphabet 
// children count is > 56
struct Trie {
  nodeid_t children[64];
};

struct TrieHandler : public NodeHandler, public TrieMixin {
  virtual void add_to_trace(int key, NodeRef& child, Trace& trace) {
      trace.push(child);
      if (key >=0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }
 
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = (Trie*)rnode.node();
      for(int i = 0; i < 64; i++) {
        if (n->children[i]) 
          children[count++] = n->children[i];
      }
      if (*rnode.extra())
        children[count++] = *rnode.extra();
        
      return count;
    }
   
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = (Trie*)rnode.node();
      for(int i = 0; i < 64; i++) {
        if (n->children[i]) 
          n->children[i] = children[count++];
      }
      if (*rnode.extra())
        *rnode.extra() = children[count];
    }

  void replace_child(NodeRef& rnode, int index, nodeid_t child) {
      if (index < 0) {
        *rnode.extra() = child;
      }
      else {
        Trie *n = (Trie*)rnode.node();
        n->children[index] = child;
      }
    }
    
  void add_node(const Slice& key, TempNode& node, Trace& trace) {
      trace.add_node(node, key);
      Trie *n = (Trie*)trace.parent().node();
      n->children[key.trie_index()] = trace.child_of_parent();
    }
 
  bool remove_last_index(NodeRef& rnode, Trace& trace) {
      if (trace.last_index < 0) {
        *rnode.exra() = 0;
        return true;
      }

      Trie *n = (Trie*)rnode.node();
      n->children[trace.last_index] = 0;
      
      nodeid_t children[64];
      size_t count = 0;
      for(int i = 0; i < 64; i++) {
        if (n->children[i])
          count++;
      }
      if (count < 56) {
        TempTrie trie(count);
        *trie.extra() = *rnode.extra();
        BitTrie* bt = (BitTrie*)trie.node();
        for(int i = 0; i < 64; i++) {
          if (n->children[i])
            bt->add(i, n->children[i]);
        }
        trace.change_node(trie);
      }

      return true;
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
  
  int next_bit(int index) const {
      boost::uint64_t mask = ~0;
      mask <<= index;
      return ffs(bits & mask) - 1;
    }
    
  int prev_bit(int index) const {
      boost::uint64_t mask = ~0;
      mask <<= index;
      return 64 - clz(bits & ~mask);
    }
    
  void add(int index, nodeid_t node) {
      bits |= 1 << index;
      int child_index = get_child_index(index); 
      memove(children+child_index, children+child_index+1,
             count()-child_index-1);
      children[child_index] = node;
    }
    
  void remove(int index) {
      int child_index = get_child_index(index);
      assert(child_index >= 0);
      bits &= ~(1 << index);
      memmove(children+child_index+1, children+child_index,
              count()-child_index-1);
    }
};


struct BitTrieHandler : public NodeHandler, public TrieMixin  {
  void add_to_trace(int key, NodeRef& child, Trace& trace) {
      trace.push(child);
      if (key >= 0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }
    
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n->count();
      for(int i = 0; i < count; i++) 
          children[i] = n->children[i];

      if (*rnode.extra())
        children[count++] = *rnode.extra();

      return count;
    }
    
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n->count();
      for(int i = 0; i < count; i++) 
          n->children[i] = children[i];

      if (*rnode.extra())
        *rnode.extra() = children[count];
    }
    
  void replace_child(NodeRef& rnode, int index, nodeid_t child) {
      if (index < 0) {
        *rnode.extra() = child;
      }
      else {
        BitTrie *n = (Trie*)rnode.node();
        n->children[n->get_childindex(index)] = child;
      }
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
    
  void add_node(const Slice& key, TempNode& node, Trace& trace) {
      BitTrie *n = (BitTrie*)trace.current().node();
      size_t count = n->count();
      switch(count) {
        case 56: {
            TempTrie trie(count);
            *trie.extra() = *trace.current().extra();
            Trie *np = (Trie*)trie.node();
            int bit = n->first_bit(), i = 0;
            while(bit >= 0) {
              np->children[bit] = n->children[i++];
              bit = n->next_bit(bit);
            }
            trace.change_node(trie);
            trace.add_node(node, key);
            np = (Trie*)trace.parent().node();
            np->children[key.trie_index()] = trace.child_of_parent();
            break;
          }
      
        case 8:
        case 24:
        case 40:
          trace.grow_node_by(16);
          
        default: 
          trace.add_node(node, key);
          n = (BitTrie*)trace.parent().node();
          n->add(key.trie_index(), trace.child_of_parent());
      }
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
          trace.grow_node_by(-16);
      }
       
      return true;
    }
    
  void change_node(NodeRef& child, int index, Trace& trace) {
      switch(child.type()) {
        case kCompressed: {
          assert(index >= 0); // is certainly no end_node
          Compressed* n = (Compressed*)child.node();
          bsize_t size = (bsize_t)*child.extra();
          NodeRef compress_child(child.page, n->child);
          
          // insert the index in compressed and replace me with compressed
          char data[256];
          data[0] = (char)index;
          memcpy(data+1, n->data, size);
          TempCompressed compressed(Slice(data, size+1));
          trace.free_node(child.page, child.id);
          trace.change_node(compressed);
          
          // ensure compress_child is on same page
          PageRef& page(trace.current().page);
          if (page.id != compress_child.page.id) {
            size_t sizes[256];
            memset(sizes, 0, sizeof(sizes));
            size_t size = calc_sizes(child, sizes);
            trace.reserve_space(size, current().id);
            compress_child.id = trace.move_node(page, compress_child);
            compress_child.page.defragment();
          }
          
          n = (Compressed*)current().node()
          n->child = compress_child.id;
          break;
          }
                
        case kPageLeaf: 
        case kLeaf: 
          if (index < 0) {
            // make the trie to leaf
            TempNode leaf;
            trace.move_node(leaf.page_ref, child);
            trace.free_node(child.page, child.id);
            trace.change_node(leaf);
          }
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
