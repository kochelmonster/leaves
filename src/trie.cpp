//@+leo-ver=5-thin
//@+node:michael.20141219202729.8: * @file trie.cpp
//@@language cplusplus
//@@tabwidth -2
/*
  Handlers for all trie nodes
*/

//@+<< includes >>
//@+node:michael.20141219202729.11: ** << includes >>
#include <string.h>
#include "larch/leaves.h"
#include "node.h"
#include "port.h"
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20141230111914.148: ** Node Structs
// A Node containing a string part equal to all descendants
// the equal part fit into a page; the data size is in Nodeptr.extra
struct Compressed {
  nodeid_t child;
  char data[];
};

struct Leaf {
  char data[];
};


// Node with the complete alphabet 
// children count is > 56
struct Trie {
  nodeid_t children[64];
};

// Node with a range
struct BitTrie {
  boost::uint64_t bits;
  nodeid_t children[];

  size_t count() const {
      return popcount(bits);    
    }
  
  int get_child_index(trieindex_t index) const {
      if (bits & (((boost::uint64_t)1)<<index)) {
        boost::uint64_t mask = ~0;
        mask <<= index;
        return popcount(bits & ~mask);
      }
      return -1;
    }
    
  int first_bit() const {
      return ffs(bits) - 1;
    }
  
  int last_bit() const {
      return 63 - clz(bits);
    }
  
  int next_bit(int index) const {
      boost::uint64_t mask = ~0;
      mask <<= (index + 1);
      return ffs(bits & mask) - 1;
    }
    
  int prev_bit(int index) const {
      boost::uint64_t mask = ~0;
      mask <<= index;
      return 63 - clz(bits & ~mask);
    }
    
  void add(int index, nodeid_t node) {
      bits |= ((boost::uint64_t)1)<<index;
      int child_index = get_child_index(index); 
      memmove(children+child_index+1, children+child_index,
              count()-child_index-1);
      children[child_index] = node;
    }
    
  void remove(int index) {
      int child_index = get_child_index(index);
      assert(child_index >= 0);
      bits &= ~(((boost::uint64_t)1)<<index);
      memmove(children+child_index, children+child_index+1,
              count()-child_index-1);
    }
};
//@+node:michael.20141215222649.83: ** NodeHandlers
struct LeafBase : public NodeHandler {
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      return 0;
    }
    
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
    }
    
  void replace_child(NodeRef& rnode, int index, nodeid_t child) {
      // this may be only called in trie nodes
      assert(0);
    }

  void find(const Slice& key, NodeRef& rnode, Trace& trace) {
      if (!key.size())
        trace.complete = true;
    }

  void next(NodeRef& rnode, Trace& trace) {
      trace.parent_next();
    }
    
  void prev(NodeRef& rnode, Trace& trace) {
      trace.parent_prev();
    }
    
  void first(NodeRef& rnode, Trace& trace) {
      trace.complete = true;
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      trace.complete = true;
    }
};


struct TrieBase : public NodeHandler {
  virtual void add_node(const Slice& key, const TempNode& node, Trace& trace) = 0;

  void add(const Slice& key, const TempNode& end, NodeRef& rnode, Trace& trace) {
      switch(key.size()) {
        case 0:
          TESTCASE("TrieBaseAdd0");
          trace.add_node(end, Slice());
          *trace.parent().extra() = trace.child_of_parent();
          trace.complete = true;
          break;
          
        case 1: 
          TESTCASE("TrieBaseAdd1");
          add_node(key, end, trace);
          trace.complete = true;
          break;
          
        case 2:
          TESTCASE("TrieBaseAdd2");
          add_node(key.slice(1), TempTrie(), trace);
          trace.current().add(key.advance(1), end, trace);
          break;

        default: {
          TESTCASE("TrieBaseAdd3");
          Slice next_key(key.advance(1));
          TempCompressed compressed(next_key);
          add_node(key.slice(1), compressed, trace);
          trace.current().add(next_key, end, trace);
          break;
	  }
       }
    }
};
 
//@+others
//@+node:michael.20141215222649.126: *3* Compressed
struct CompressedHandler : public LeafBase {
  //@+others
  //@+node:michael.20141230111914.81: *4* add_to_trace
  void add_to_trace(Compressed* c, NodeRef& child, Trace& trace) {
      trace.push(child);
      trace.key.append(c->data, *child.extra());
    }
  //@+node:michael.20141230111914.82: *4* find
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
  //@+node:michael.20141230111914.83: *4* first
  void first(NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.node();
      NodeRef child(rnode.page, n->child);
      add_to_trace(n, child, trace);
      child.first(trace);
    }
  //@+node:michael.20141230111914.84: *4* last
  void last(NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.node();
      NodeRef child(rnode.page, n->child);
      add_to_trace(n, child, trace);
      child.last(trace);
    }
  //@+node:michael.20141230111914.79: *4* get_children
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      Compressed *n = (Compressed*)rnode.node();
      if (n->child) {
        children[0] = n->child;
        return 1;
      }
      return 0;
    }
  //@+node:michael.20141230111914.80: *4* replace_children
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
      Compressed *n = (Compressed*)rnode.node();
      if (n->child)
        n->child = children[0];
    }
  //@+node:michael.20141230111914.85: *4* move_child
  nodeid_t move_child(NodeRef& child, Trace& trace) {
      // moves the child and its descendant to current page
      PageRef& page(trace.current().page);
      if (page.id != child.page.id) {
        TESTCASE("CompressMoveChild");
        size_t sizes[256];
        memset(sizes, 0, sizeof(sizes));
        size_t size = trace.calc_sizes(child, sizes);
        trace.reserve_space(size, trace.current().id);
        nodeid_t r = trace.move_node(page, child);
        child.page.defragment();
        return r;
      }
      return child.id;
    }
  //@+node:michael.20141230111914.86: *4* reinsert
  void reinsert(TempCompressed& rest_me, NodeRef& child, 
                trieindex_t index, Trace& trace) {
      BitTrie* bn;
      nodeid_t child_id;
      
      switch(*rest_me.extra()) {
        case 0:
          TESTCASE("CompressReinsert0");
          child_id = move_child(child, trace);
          bn = (BitTrie*)trace.current().node();
          bn->add(index, child_id);
          break;
          
        case 1: {
          TESTCASE("CompressReinsert1");
          trace.add_node(TempTrie(), Slice(index));
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, trace.child_of_parent());
          
          child_id = move_child(child, trace);
          bn = (BitTrie*)trace.current().node();
          bn->add(((Compressed*)rest_me.node())->data[0], child_id);
          trace.pop();
          break;
          }
          
        default: {
          TESTCASE("CompressReinsert2");
          trace.add_node(rest_me, Slice(index));
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, trace.child_of_parent());
          
          child_id = move_child(child, trace);
          Compressed *n = (Compressed*)trace.current().node();
          n->child = child_id;
          trace.pop();
          break;
          }
      }
    }
  //@+node:michael.20141230111914.87: *4* add
  void add(const Slice& key, const TempNode& end, NodeRef& rnode, Trace& trace) {
      Compressed *n = (Compressed*)rnode.node();
      bsize_t size = (bsize_t)*rnode.extra();
      
      if (!n->child) {
        // A new leaf with some rest key is inserted
        TESTCASE("CompressAddNew");
        assert(size == key.size());
        trace.add_node(end, key);
        n->child = trace.child_of_parent();
        trace.complete = true;
        return;
      }
      
      size_t prefix_size = common_prefix(key.data(), n->data,
                                         std::min((size_t)size, key.size()));
      TempCompressed rest_me(Slice(n->data+prefix_size+1, size-prefix_size-1));
            
      NodeRef child(rnode.page, n->child);
      n->child = 0;

      trieindex_t first = n->data[0], second = n->data[1];    
      switch(prefix_size) {
        case 0:
          // insert one trie
          TESTCASE("CompressAdd0");
          trace.change_node(TempTrie());
          reinsert(rest_me, child, first, trace);
          break;
          
        case 1: {
          // insert two tries
          TESTCASE("CompressAdd1");
          trace.change_node(TempTrie());
          
          trace.add_node(TempTrie(), Slice(first));
          BitTrie* bn = (BitTrie*)trace.parent().node();
          bn->add(first, trace.child_of_parent());
          
          reinsert(rest_me, child, second, trace);
          break;
          }
        default: {
          // another compressed
          TESTCASE("CompressAdd2");
          first = n->data[prefix_size];

          Slice next_key(key.data(), prefix_size);
          trace.change_node(TempCompressed(next_key));
          
          trace.add_node(TempTrie(), next_key);
          n = (Compressed*)trace.parent().node();
          n->child = trace.child_of_parent();
          
          reinsert(rest_me, child, first, trace);
        }
      }
      trace.current().add(key.advance(prefix_size), end, trace);
    }
  //@+node:michael.20150101205559.5: *4* dump
  #ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      Compressed *n = (Compressed*)rnode.node();
      out << t3 << "type:  compressed" << std::endl;
      out << t3 << "data:  " << std::setw(2) << std::setfill('0') 
                             << (int)n->data[0];
      for(size_t i = 1; i < *rnode.extra(); i++)
        out << "|" << std::setw(2) << std::setfill('0') << (int)n->data[i];
      out << std::endl;
      out << t3 << "size:  " << (int)*rnode.extra() << std::endl
          << t3 << "child: " << (int)n->child << std::endl;
    }
  #endif
  //@-others
};

static CompressedHandler compressed;


//@+node:michael.20141215222649.137: *3* Link
// links to another page

struct LinkHandler : public LeafBase {
  NodeRef link_node(NodeRef& rnode, Trace& trace) {
      pageid_t *page_id = (pageid_t*)rnode.link();
      PageRef next_page(trace.map.get_page(*page_id));
      NodeRef child(next_page, 0);
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
    
  void add(const Slice& key, const TempNode& end, NodeRef& rnode, Trace& trace) {
      assert(0); // may never be called
    }
    
#ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      pageid_t *page_id = (pageid_t*)rnode.link();
      out << t3 << "type:  link" << std::endl
          << t3 << "child: link" << *page_id << std::endl;
    }
#endif    
};

static LinkHandler link;
//@+node:michael.20141215222649.93: *3* Trie
struct TrieHandler : public TrieBase {
  //@+others
  //@+node:michael.20141230111914.88: *4* add_to_trace
  void add_to_trace(int key, NodeRef& child, Trace& trace) {
      trace.push(child);
      if (key >=0)
        trace.key.append(1, (char)key);
      else
        trace.complete = true;
    }
  //@+node:michael.20141230111914.94: *4* find
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
      trieindex_t index = key.trieindex();
      nodeid_t child_id= n->children[index];
      if (child_id) {
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, trace);
        child.find(key.advance(1), trace);
      }
    }
  //@+node:michael.20141230111914.97: *4* first
  void first(NodeRef& rnode, Trace& trace) {
      nodeid_t *end_child = rnode.extra();

      if (*end_child ) {
        NodeRef child(rnode.page, *end_child);
        add_to_trace(-1, child, trace);
        child.first(trace);
        return;
      }

      Trie *n = (Trie*)rnode.node();
      for(int index = 0; index < 64; index++) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          NodeRef child(rnode.page, child_id);
          add_to_trace(index, child, trace);
          child.first(trace);
          return;
        }
      }
      assert(0);
    }
  //@+node:michael.20141230111914.98: *4* last
  void last(NodeRef& rnode, Trace& trace) {
      Trie *n = (Trie*)rnode.node();
      for(int index = 63; index >= 0; index--) {
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
  //@+node:michael.20141230111914.95: *4* next
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
      }

      trace.parent_next();
    }
  //@+node:michael.20141230111914.96: *4* prev
  void prev(NodeRef& rnode, Trace& trace) {
      if (trace.last_index < 0) {
        trace.parent_prev();
        return;
      }

      int index = trace.last_index - 1;
      Trie *n = (Trie*)rnode.node();
      for(; index >= 0; index--) {
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
  //@+node:michael.20141230111914.89: *4* get_children
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
  //@+node:michael.20141230111914.90: *4* replace_children
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
  //@+node:michael.20141230111914.91: *4* replace_child
  void replace_child(NodeRef& rnode, int index, nodeid_t child) {
      if (index < 0) {
        *rnode.extra() = child;
      }
      else {
        Trie *n = (Trie*)rnode.node();
        n->children[index] = child;
      }
    }
  //@+node:michael.20141230111914.92: *4* add_node
  void add_node(const Slice& key, const TempNode& node, Trace& trace) {
      trace.add_node(node, key);
      Trie *n = (Trie*)trace.parent().node();
      n->children[key.trieindex()] = trace.child_of_parent();
    }
  //@+node:michael.20141230111914.93: *4* remove_last_index
  bool remove_last_index(NodeRef& rnode, Trace& trace) {
      if (trace.last_index < 0) {
        *rnode.extra() = 0;
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
        TESTCASE("TrieRemove");
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
  //@+node:michael.20150101205559.6: *4* dump
  #ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      Trie *n = (Trie*)rnode.node();
      out << t3 << "type:  trie" << std::endl;
      out << t3 << "data:  ";
      
      nodeid_t end = *rnode.extra();
      if (end) {
        out << "E>" << (int)end;
        out << "|";
      }
            
      for(size_t i = 0; i < 64; i++) {
        if (i != 0)
          out << "|";
          
        out << std::setw(2) << std::setfill('0') 
            << i << ">" 
            << (int)n->children[i];
      }
      
      out << std::endl;
    }
  #endif
  //@-others
};

static TrieHandler trie;


//@+node:michael.20141215222649.95: *3* BitTrie
struct BitTrieHandler : public TrieBase {
  //@+others
  //@+node:michael.20141230111914.99: *4* add_to_trace
  void add_to_trace(int key, NodeRef& child, Trace& trace) {
      trace.push(child);
      if (key >= 0)
        trace.key.append(1, (char)key);
      else
        trace.complete = true;
    }
  //@+node:michael.20141230111914.103: *4* find
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
        
      trieindex_t index = key.trieindex();
      BitTrie *n = (BitTrie*)rnode.node();
      int child_index = n->get_child_index(index);
      if (child_index >= 0) {
        nodeid_t child_id = n->children[child_index];
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, trace);
        child.find(key.advance(1), trace);
      }
    }
  //@+node:michael.20141230111914.106: *4* first
  void first(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child);
        add_to_trace(-1, child, trace);
        child.first(trace);
      }
      else {
        trieindex_t index = n->first_bit();
        NodeRef child(rnode.page, n->children[0]);
        add_to_trace(index, rnode, trace);
        child.first(trace);
      }
    }
  //@+node:michael.20141230111914.107: *4* last
  void last(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      trieindex_t bit_index = n->last_bit();
      int index = n->count() - 1;
      NodeRef child(rnode.page, n->children[index]);
      add_to_trace(bit_index, child, trace);
      child.first(trace);
    }
  //@+node:michael.20141230111914.104: *4* next
  void next(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      int index = trace.last_index;
      
      if (index < 0) {
        int first_bit = n->first_bit();
        NodeRef child(rnode.page, n->children[0]);
        add_to_trace(first_bit, child, trace);
  child.first(trace);
        return;
      }
     
      int child_index = n->get_child_index(index) + 1;
      if (child_index < (int)n->count()) {
        index = n->next_bit(index);
        NodeRef child(rnode.page, n->children[child_index]);
        add_to_trace(index, child, trace);
        child.first(trace);
        return;
      }
      
      trace.parent_next();
    }
  //@+node:michael.20141230111914.105: *4* prev
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
        return;
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
  //@+node:michael.20141230111914.100: *4* get_children
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n->count();
      for(size_t i = 0; i < count; i++) 
          children[i] = n->children[i];

      if (*rnode.extra())
        children[count++] = *rnode.extra();

      return count;
    }
  //@+node:michael.20141230111914.101: *4* replace_children
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n->count();
      for(size_t i = 0; i < count; i++) 
          n->children[i] = children[i];

      if (*rnode.extra())
        *rnode.extra() = children[count];
    }
  //@+node:michael.20141230111914.102: *4* replace_child
  void replace_child(NodeRef& rnode, int index, nodeid_t child) {
      if (index < 0) {
        *rnode.extra() = child;
      }
      else {
        BitTrie *n = (BitTrie*)rnode.node();
        n->children[n->get_child_index(index)] = child;
      }
    }
  //@+node:michael.20141230111914.108: *4* add_node
  void add_node(const Slice& key, const TempNode& node, Trace& trace) {
      BitTrie *n = (BitTrie*)trace.current().node();
      size_t count = n->count();
      switch(count) {
        case 56: {
            TESTCASE("BitTrieAdd0");
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
            np->children[key.trieindex()] = trace.child_of_parent();
            break;
          }
      
        case 8:
        case 24:
        case 40:
          TESTCASE("BitTrieAdd1");
          trace.grow_node_by(16);
          
        default: 
          TESTCASE("BitTrieAdd2");
          trace.add_node(node, key);
          n = (BitTrie*)trace.parent().node();
          n->add(key.trieindex(), trace.child_of_parent());
      }
    }
  //@+node:michael.20141230111914.110: *4* change_node
  void change_node(const NodeRef& child, int index, Trace& trace) {
      switch(child.type()) {
        case kCompressed: {
          TESTCASE("BitTrieChange0");
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
            TESTCASE("BitTrieChange1");
            size_t sizes[256];
            memset(sizes, 0, sizeof(sizes));
            size_t size = trace.calc_sizes(child, sizes);
            trace.reserve_space(size, trace.current().id);
            compress_child.id = trace.move_node(page, compress_child);
            compress_child.page.defragment();
          }
          
          n = (Compressed*)trace.current().node();
          n->child = compress_child.id;
          break;
          }
                
        case kBigLeaf: 
        case kLeaf: 
          if (index < 0) {
            TESTCASE("BitTrieChange2");
            // make the trie to leaf
            TempNode leaf;
            trace.move_node(leaf.pageref, child);
            trace.free_node(child.page, child.id);
            trace.change_node(leaf);
          }
      }
    }
  //@+node:michael.20141230111914.109: *4* remove_last_index
  bool remove_last_index(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      if (trace.last_index < 0) {
        *rnode.extra() = 0;
        if (n->count() == 1) {
          TESTCASE("BitTrieRemove0");
          change_node(NodeRef(rnode.page, n->children[0]), 
                      n->first_bit(), trace);
        }
        return true;
      }
      
      n->remove(trace.last_index);
      switch(n->count()) {
        case 0: 
          if (*rnode.extra()) {
            TESTCASE("BitTrieRemove1");
            change_node(NodeRef(rnode.page, *rnode.extra()), -1, trace);
          }
          else
            return false;
        
          break;
          
        case 1: 
          if (!*rnode.extra()) {
            TESTCASE("BitTrieRemove2");
            change_node(NodeRef(rnode.page, n->children[0]),
                        n->first_bit(), trace);
          }
          break;
      
        case 24:
        case 40:
          TESTCASE("BitTrieRemove3");
          trace.grow_node_by(-16);
      }
       
      return true;
    }
  //@+node:michael.20150101205559.7: *4* dump
  #ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      BitTrie *n = (BitTrie*)rnode.node();
      out << t3 << "type:  bittrie" << std::endl;
      out << t3 << "data:  ";
      
      nodeid_t end = *rnode.extra();
      if (end) {
        out << "E>" << (int)end;
            
        if (n->count())
          out << "|";
      }
            
      int bit = n->first_bit(), i = 0;
      while(bit >= 0) {
        if (i != 0)
          out << "|";
        out << std::setw(2) << std::setfill('0') 
            << bit << ">" 
            << (int)n->children[i];
        bit = n->next_bit(bit);
        i++;
      }
      
      out << std::endl;
    }
  #endif
  //@-others
};


static BitTrieHandler bittrie;


//@+node:michael.20141215222649.84: *3* Leaf
// a leaf node with key and values <= 256 bytes
// the key_size is saved in Nodeptr.extra
struct LeafHandler : public LeafBase {
  //@+others
  //@+node:michael.20141230111914.78: *4* reinsert_me
  void reinsert_me(TempLeaf& me, Trace& trace) {
      trace.add_node(me, Slice());
      trace.complete = true;
      // extra() is the end node
      *trace.parent().extra() = trace.child_of_parent();
      trace.pop(); // back to parent
    }
  //@+node:michael.20141230111914.77: *4* add
  void add(const Slice& key, const TempNode& end, NodeRef& rnode, Trace& trace) {
      assert(key.size() != 0);
      Leaf *n = (Leaf*)rnode.node();
      TempLeaf me(Slice(n->data, *rnode.extra()));
      TempTrie trie;
      Slice rest_key;
      
      if (key.size() == 2) {
        TESTCASE("LeafAdd0");
        trace.change_node(TempTrie());
        reinsert_me(me, trace);
        trace.add_node(trie, key.slice(1));
        BitTrie* pn = (BitTrie*)trace.parent().node();
        pn->add(key.trieindex(), trace.child_of_parent());
        rest_key = key.advance(1);
      }
      else {
        trace.change_node(trie);
        reinsert_me(me, trace);
        if (key.size() > 2) {
          TESTCASE("LeafAdd1");
          rest_key = key.advance(1);
          TempCompressed compressed(rest_key);
          trace.add_node(compressed, key.slice(1));
          BitTrie* pn = (BitTrie*)trace.parent().node();
          pn->add(key.trieindex(), trace.child_of_parent());
        }
        else {
          TESTCASE("LeafAdd2");
          rest_key = key;
        }
      }

      trace.current().add(rest_key, end, trace);
    }
  //@+node:michael.20150101205559.8: *4* dump
  #ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      Leaf *n = (Leaf*)rnode.node();
      std::string data(n->data, *rnode.extra());
      if (rnode.type() == kLeaf) {
        out << t3 << "type:  leaf" << std::endl
            << t3 << "data:  " << data << std::endl
            << t3 << "size:  " << data.size() << std::endl;
      }
      else {
        out << t3 << "type:  bigleaf" << std::endl;
      }
    }
  #endif
  //@-others
};

static LeafHandler leaf;
//@-others

NodeHandler* NodeHandler::handlers[6] = {
  &leaf,
  &leaf,
  &link,
  &compressed,
  &trie,
  &bittrie,
};
//@+node:michael.20141230111914.75: ** TempNode
TempLeaf::TempLeaf(const Slice& value) : TempNode() {
  size_t size = value.size();
  pageref.new_node(pad16(size));
  *extra() = value.size();
  Leaf* n = (Leaf*)node();
  memcpy(n->data, value.data(), value.size());
  noderef.set_type(kLeaf);
}

TempTrie::TempTrie(size_t child_count) : TempNode() {
  size_t size;
  if (child_count >= 56) {
    pageref.new_node(size=64);
    noderef.set_type(kTrie);
  }
  else {
    size = pad16(child_count);
    pageref.new_node(size);
    noderef.set_type(kBitTrie);
  }

  memset(node(), 0, size);
}

TempCompressed::TempCompressed(const Slice& part) : TempNode() {
  size_t size(part.size());
  *extra() = (bsize_t)size;
  pageref.new_node(pad16(sizeof(nodeid_t)+size));
  Compressed* n = (Compressed*)node();
  memcpy(n->data, part.data(), size);
  n->child = 0;
  noderef.set_type(kCompressed);
}
//@-others
} // namespace larch_leaves 
//@-leo
