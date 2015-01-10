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
// the equal part fit into a page; the data size in NodeRef::len()
struct Compressed {
  nodeid_t child;
  char data[];
};

// A leaf 
// the data is interpreted by the type (kLeaf od kBitLeaf)
// the size of data is saved in NodeRef::len()
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
      if (!bits)
        return -1;
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
      mask = bits & ~mask;
      if (!mask)
        return -1;
      return 63 - clz(mask);
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
              count()-child_index);
    }
};
//@+node:michael.20141215222649.83: ** NodeHandlers
struct LeafBase : public NodeHandler {
  size_t get_len(const NodeRef& rnode) {
      return 0;
    }

  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      return 0;
    }
    
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
    }
  
  void next(NodeRef& rnode, Trace& trace) {
      trace.parent_next();
    }
    
  void prev(NodeRef& rnode, Trace& trace) {
      trace.parent_prev();
    }
    
  void first(NodeRef& rnode, Trace& trace) {
    }
    
  void last(NodeRef& rnode, Trace& trace) {
    }
};


struct TrieBase : public NodeHandler {
  size_t get_len(const NodeRef& rnode) {
      return 1;
    }

  virtual void add_node(trieindex_t index, const TempNode& node, 
                        Trace& trace) = 0;

  void add(const TempNode& leaf, NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
  
      switch(key.size()) {
        case 0:
          TESTPOINT(TrieBaseAdd0);
          trace.add_node(leaf);
          *trace.parent().extra() = trace.child_of_parent();
          break;
          
        case 1: 
          TESTPOINT(TrieBaseAdd1);
          add_node(key.trieindex(), leaf, trace);
          break;
          
        case 2:
          TESTPOINT(TrieBaseAdd2);
          add_node(key.trieindex(), TempTrie(), trace);
          trace.current().add(leaf, trace);
          break;

        default: {
          TESTPOINT(TrieBaseAdd3);
          TempCompressed compressed(key.advance(1));
          add_node(key.trieindex(), compressed, trace);
          trace.current().add(leaf, trace);
          break;
          }
      }
    }
};
 
//@+others
//@+node:michael.20150106224503.25: *3* Eat Tools
// if bitrie has only one child it fills its index
// and child_id and returns true or false otherwise

struct SingleChild {
  int index;   // transitions index to id
  nodeid_t child;
};

inline bool get_single_child(const NodeRef& bittrie, SingleChild* child) {
    assert(bittrie.type() == kBitTrie);
    BitTrie *bt = (BitTrie*)bittrie.node();
    nodeid_t end = *bittrie.extra();
    
    if (end && bt->count() == 0) {
      child->index = -1;
      child->child = end;
      return true;
    }
    
    if (bt->count() == 1 && end == 0) {
      child->index = bt->first_bit();
      assert(child->index >= 0);
      child->child = bt->children[bt->get_child_index(child->index)];
      return true;
    }
    
    return false;
}

// eat all childrens with only end nodes left
inline bool eat_null_children(NodeRef& rnode) {
    bool eaten = false;
    nodeid_t children[65];
    size_t count = rnode.get_children(children);
    
    for(size_t i = 0; i < count; i++) {
      nodeid_t child_id = children[i];
      if (!child_id)
        continue;
        
      NodeRef child(rnode.page, child_id);
      if (child.type() != kBitTrie) 
        continue;
      
      SingleChild s_child;
      if (get_single_child(child, &s_child) && s_child.index < 0) {
        children[i] = s_child.child;
        rnode.page.free_node(child_id);
        eaten = true;
      }
    }
    
    if (eaten) {
      TESTPOINT(NodeEatSingle);
      rnode.replace_children(children);
    }
      
    return eaten;
}

struct CompressBuffer {
  char data[MAX_KEY_SIZE_64];
  size_t size;
  nodeid_t child;
};

// saves the compressed data
inline void save_compressed(const NodeRef& node, CompressBuffer* buffer) {
  assert(node.type() == kCompressed);
  Compressed *c = (Compressed*)node.node();
  buffer->size = node.len();
  buffer->child = c->child;
  memcpy(buffer->data, c->data, buffer->size);
}

//@+node:michael.20141215222649.126: *3* Compressed
struct CompressedHandler : public NodeHandler {
  //@+others
  //@+node:michael.20150106224503.49: *4* keycmp
  // returns 
  //   < 0 if key <  data
  //  == 0 if key == data
  //   > 0 if key > data
  int keycmp(NodeRef& rnode, Trace& trace, nodeid_t* child_id) {
      Slice key(trace.current_key());
      Compressed *n = (Compressed*)rnode.node();
      size_t len = rnode.len();

      *child_id = n->child;    
      
     
      int cmp = memcmp(key.data(), n->data, std::min(len, key.size()));
      if (cmp != 0)
        return cmp;
        
      return key.size() >= len ? 0 : -1;
    }
  //@+node:michael.20150106224503.59: *4* keyappend
  void keyappend(NodeRef& rnode, Trace& trace, nodeid_t* child_id) {
      trace.cut_key();
      Compressed *n = (Compressed*)rnode.node();
      trace.key.append(n->data, rnode.len());
      *child_id = n->child;
    }
  //@+node:michael.20150106224503.50: *4* get_len
  size_t get_len(const NodeRef& rnode) {
      return rnode.len();
    }
  //@+node:michael.20141230111914.82: *4* find
  void find(NodeRef& rnode, Trace& trace) {
      nodeid_t child_id;
      if (keycmp(rnode, trace, &child_id) == 0 && child_id)
        rnode.child_find(child_id, trace);
    }
  //@+node:michael.20141230111914.83: *4* first
  void first(NodeRef& rnode, Trace& trace) {
      nodeid_t child_id;
      keyappend(rnode, trace, &child_id);
      rnode.child_first(child_id, trace);
    }
  //@+node:michael.20141230111914.84: *4* last
  void last(NodeRef& rnode, Trace& trace) {
      nodeid_t child_id;
      keyappend(rnode, trace, &child_id);
      rnode.child_last(child_id, trace);
    }
  //@+node:michael.20150106224503.43: *4* next
  void next(NodeRef& rnode, Trace& trace) {
      nodeid_t child_id;
      if (keycmp(rnode, trace, &child_id) < 0) {
        keyappend(rnode, trace, &child_id);
        rnode.child_first(child_id, trace);
      }
      else {
        trace.parent_next();
      }
    }
  //@+node:michael.20150106224503.44: *4* prev
  void prev(NodeRef& rnode, Trace& trace) {
      nodeid_t child_id;
      if (keycmp(rnode, trace, &child_id) > 0) {
        keyappend(rnode, trace, &child_id);
        rnode.child_last(child_id, trace);
      } 
      else {
        trace.parent_prev();
      }
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
  //@+node:michael.20141230111914.86: *4* reinsert
  void reinsert(TempCompressed& rest_me, nodeid_t child_id, 
                trieindex_t index, Trace& trace) {
      BitTrie* bn;

      switch(rest_me.len()) {
        case 0:
          TESTPOINT(CompressReinsert0);
          bn = (BitTrie*)trace.current().node();
          bn->add(index, child_id);
          break;
          
        case 1: {
          TESTPOINT(CompressReinsert1);
          trace.add_node(TempTrie());
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, trace.child_of_parent());
          
          bn = (BitTrie*)trace.current().node();
          bn->add(((Compressed*)rest_me.node())->data[0], child_id);
          trace.pop();
          break;
          }
          
        default: {
          TESTPOINT(CompressReinsert2);
          trace.add_node(rest_me);
          bn = (BitTrie*)trace.parent().node();
          bn->add(index, trace.child_of_parent());
          
          Compressed *n = (Compressed*)trace.current().node();
          n->child = child_id;
          trace.pop();
          break;
          }
      }
    }
  //@+node:michael.20141230111914.87: *4* add
  void add(const TempNode& leaf, NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      Compressed *n = (Compressed*)rnode.node();
      size_t size = rnode.len();
      
      if (!n->child) {
        // A new leaf with some rest key is inserted
        TESTPOINT(CompressAddNew);
        assert(size == key.size());
        trace.add_node(leaf);
        n = (Compressed*)trace.parent().node();
        n->child = trace.child_of_parent();
        return;
      }
      
      size_t prefix_size = common_prefix(key.data(), n->data,
                                         std::min(size, key.size()));
      TempCompressed rest_me(Slice(n->data+prefix_size+1, size-prefix_size-1));
            
      trace.reserve_space(48+3*sizeof(NodePtr)); 
        // the maximum of additional space we need
        // if current() and its child has been moved to 
        // another page with enough space      
            
      n = (Compressed*)trace.current().node();
      nodeid_t child_id = n->child;

      trieindex_t first = n->data[0], second = n->data[1];    
      switch(prefix_size) {
        case 0:
          // insert one trie node
          TESTPOINT(CompressAdd0);
          trace.change_node(TempTrie());
          reinsert(rest_me, child_id, first, trace);
          break;
          
        case 1: {
          // insert two trie nodes
          TESTPOINT(CompressAdd1);
          trace.change_node(TempTrie());
          
          trace.add_node(TempTrie());
          BitTrie* bn = (BitTrie*)trace.parent().node();
          bn->add(first, trace.child_of_parent());
          
          reinsert(rest_me, child_id, second, trace);
          break;
          }
        default: {
          // another compressed
          TESTPOINT(CompressAdd2);
          first = n->data[prefix_size];

          Slice next_key(key.data(), prefix_size);
          trace.change_node(TempCompressed(next_key));
          
          trace.add_node(TempTrie());
          n = (Compressed*)trace.parent().node();
          n->child = trace.child_of_parent();
          
          reinsert(rest_me, child_id, first, trace);
        }
      }
      trace.current().add(leaf, trace);
    }
  //@+node:michael.20150106224503.21: *4* eat_child
  void append_data(NodeRef& rnode, const CompressBuffer& buffer) {
      size_t size = rnode.len();
      size_t nsize = page_pad(size+buffer.size+sizeof(nodeid_t));
      // it is guaranteed that enough space is on page (see eat_child)
      rnode.page.grow_node_by(rnode.id, nsize-rnode.size());
      Compressed *c = (Compressed*)rnode.node();
      memcpy(c->data+size, buffer.data, buffer.size);
      rnode.set_len(size+buffer.size);
      c->child = buffer.child;
    }

  bool eat_child(NodeRef& rnode) {
      if (eat_null_children(rnode))
        return true;

      CompressBuffer buffer;
      Compressed *c = (Compressed*)rnode.node();
      NodeRef child(rnode.page, c->child);

      switch(child.type()) {
        case kCompressed: 
          // only when a link was between me and child
          TESTPOINT(CompressedEatCompressed);
          save_compressed(child, &buffer);
          // make space on page
          rnode.page.grow_node_by(child.id, -(int)child.size());
          append_data(rnode, buffer);
          rnode.page.free_node(child.id);
          return true;
        
        case kBitTrie: {
          SingleChild s_child;
          if (get_single_child(child, &s_child)) {
            TESTPOINT(CompressedEatSingle);
            assert(s_child.index >= 0); // null children are done before
            buffer.data[0] = (char)s_child.index;
            buffer.size = 1;
            buffer.child = s_child.child;
            rnode.page.grow_node_by(child.id, -(int)child.size());
            append_data(rnode, buffer);
            rnode.page.free_node(child.id);
          }
          return true;
          }
      }
      return false;
    }
  //@+node:michael.20150101205559.5: *4* dump
  #ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      Compressed *n = (Compressed*)rnode.node();
      out << t3 << "type:  compressed" << std::endl;
      out << t3 << "data:  " << std::setw(2) << std::setfill('0') 
                             << (int)n->data[0];
      for(size_t i = 1; i < rnode.len(); i++)
        out << "|" << std::setw(2) << std::setfill('0') << (int)n->data[i];
      out << std::endl;
      out << t3 << "size:  " << (int)rnode.len() << std::endl
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
    
  void find(NodeRef& rnode, Trace& trace) {
      link_node(rnode, trace).find(trace);
    }
    
  void first(NodeRef& rnode, Trace& trace) {
      link_node(rnode, trace).first(trace);
    }
    
  void last(NodeRef& rnode, Trace& trace) {
      link_node(rnode, trace).last(trace);
    }
    
  void add(const TempNode& leaf, NodeRef& rnode, Trace& trace) {
      assert(0); // may never be called
    }
    
  bool eat_child(NodeRef& rnode) {
      return false;
    }
  
    
#ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      pageid_t *page_id = (pageid_t*)rnode.link();
      out << t3 << "type:  link" << std::endl
          << t3 << "page:  " << *page_id << std::endl;
    }
#endif    
};

static LinkHandler link;
//@+node:michael.20141215222649.93: *3* Trie
struct TrieHandler : public TrieBase {
  //@+others
  //@+node:michael.20141230111914.94: *4* find
  void find(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      
      if (key.empty()) {
        nodeid_t end_child = *rnode.extra();
        if (end_child)
          rnode.child_find(end_child, trace);
        return;
      }

      Trie *n = (Trie*)rnode.node();
      trieindex_t index = key.trieindex();
      nodeid_t child_id= n->children[index];
      if (child_id) 
        rnode.child_find(child_id, trace);
    }
  //@+node:michael.20141230111914.97: *4* first
  void first(NodeRef& rnode, Trace& trace) {
      nodeid_t end_child = *rnode.extra();
      if (end_child) {
        rnode.child_first(end_child, trace);
        return;
      }

      Trie *n = (Trie*)rnode.node();
      for(int index = 0; index < 64; index++) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          trace.key.push_back((char)index);
          rnode.child_first(child_id, trace);
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
          trace.key.push_back((char)index);
          rnode.child_last(child_id, trace);
          return;
        }
      }
      assert(0);
    }
  //@+node:michael.20141230111914.95: *4* next
  void next(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? 0 : key.trieindex()+1;
      
      Trie *n = (Trie*)rnode.node();
      for(; index < 64; index++) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          trace.cut_key();
          trace.key.push_back((char)index);
          rnode.child_first(child_id, trace);
          return;
        }
      }

      trace.parent_next();
    }
  //@+node:michael.20141230111914.96: *4* prev
  void prev(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());

      if (key.empty()) {
        trace.parent_prev();
        return;
      }

      int index = ((int)key.trieindex())-1;
      Trie *n = (Trie*)rnode.node();
      for(; index >= 0; index--) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          trace.cut_key();
          trace.key.push_back((char)index);
          rnode.child_last(child_id, trace);
          return;
        }
      }
      
      nodeid_t end_child = *rnode.extra();
      if (end_child) {
        trace.cut_key();
        rnode.child_last(end_child, trace);
      }
      else 
        trace.parent_prev();
    }
  //@+node:michael.20141230111914.89: *4* get_children
  size_t get_children(NodeRef& rnode, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = (Trie*)rnode.node();
      for(int i = 0; i < 64; i++) {
        if (n->children[i]) 
          children[count++] = n->children[i];
      }
      nodeid_t end_child = *rnode.extra();
      if (end_child)
        children[count++] = end_child;
        
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
      
      nodeid_t *end_child = rnode.extra();
      if (*end_child)
        *end_child = children[count];
    }
  //@+node:michael.20141230111914.92: *4* add_node
  void add_node(trieindex_t index, const TempNode& node, Trace& trace) {
      trace.add_node(node);
      Trie *n = (Trie*)trace.parent().node();
      n->children[index] = trace.child_of_parent();
    }
  //@+node:michael.20141230111914.93: *4* remove_child
  bool remove_child(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key.trieindex();

      if (index < 0) {
        *rnode.extra() = 0;
        return true;
      }

      Trie *n = (Trie*)rnode.node();
      n->children[index] = 0;
      
      size_t count = 0;
      for(int i = 0; i < 64; i++) {
        if (n->children[i])
          count++;
      }
      
      if (count <= 56) {
        TESTPOINT(TrieRemove);
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
  //@+node:michael.20150106224503.23: *4* eat_child
  bool eat_child(NodeRef& rnode) {
      return eat_null_children(rnode);
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
  //@+node:michael.20141230111914.103: *4* find
  void find(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      
      if (key.empty()) {
        nodeid_t end_child = *rnode.extra();
        if (end_child)
          rnode.child_find(end_child, trace);
        return;
      }
        
      trieindex_t index = key.trieindex();
      BitTrie *n = (BitTrie*)rnode.node();
      int child_index = n->get_child_index(index);
      if (child_index >= 0) {
        nodeid_t child_id = n->children[child_index];
        rnode.child_find(child_id, trace);
      }
    }
  //@+node:michael.20141230111914.106: *4* first
  void first(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      nodeid_t end_child = *rnode.extra();
      if (end_child) {
        rnode.child_first(end_child, trace);
      }
      else {
        int index = n->first_bit();
        if (index < 0)
          return;
        
        trace.key.push_back((char)index);
        rnode.child_first(n->children[0], trace);
      }
    }
  //@+node:michael.20141230111914.107: *4* last
  void last(NodeRef& rnode, Trace& trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      int index = n->last_bit();
      if (index < 0)
        return;

      trace.key.push_back((char)index);
      int child_index = n->get_child_index(index);
      rnode.child_last(n->children[child_index], trace);
    }
  //@+node:michael.20141230111914.104: *4* next
  void next(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key.trieindex();
      BitTrie *n = (BitTrie*)rnode.node();
      
      if (index < 0) {
        int first_bit = n->first_bit();
        if (first_bit < 0)
          return;

        trace.cut_key();
        trace.key.push_back((char)first_bit);
        rnode.child_first(n->children[0], trace);
        return;
      }
     
      index = n->next_bit(index);
      if (index >= 0) {
        trace.cut_key();
        trace.key.push_back((char)index);
        int child_index = n->get_child_index(index);
        rnode.child_first(n->children[child_index], trace);
        return;
      }
      
      trace.parent_next();
    }
  //@+node:michael.20141230111914.105: *4* prev
  void prev(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key.trieindex();
      
      if (index < 0) {
        trace.parent_prev();
        return;
      }
          
      BitTrie *n = (BitTrie*)rnode.node();
      index = n->prev_bit(index);
      if (index >= 0) {
        trace.cut_key();
        trace.key.push_back((char)index);
        int child_index = n->get_child_index(index);
        rnode.child_last(n->children[child_index], trace);
        return;
      }
    
      nodeid_t end_child = *rnode.extra();
      if (end_child) {
        trace.cut_key();
        rnode.child_last(end_child, trace);
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

      nodeid_t end_child = *rnode.extra();
      if (end_child)
        children[count++] = end_child;

      return count;
    }
  //@+node:michael.20141230111914.101: *4* replace_children
  void replace_children(NodeRef& rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n->count();
      for(size_t i = 0; i < count; i++) 
          n->children[i] = children[i];

      nodeid_t *end_child = rnode.extra();
      if (*end_child)
        *end_child = children[count];
    }
  //@+node:michael.20141230111914.108: *4* add_node
  void add_node(trieindex_t index, const TempNode& node, Trace& trace) {
      BitTrie *n = (BitTrie*)trace.current().node();
      size_t count = n->count();
      switch(count) {
        case 56: {
            TESTPOINT(BitTrieAdd0);
            TempTrie trie(count+1);
            *trie.extra() = *trace.current().extra();
            Trie *np = (Trie*)trie.node();
            int bit = n->first_bit(), i = 0;
            while(bit >= 0) {
              np->children[bit] = n->children[i++];
              bit = n->next_bit(bit);
            }
            trace.change_node(trie);
            trace.add_node(node);
            np = (Trie*)trace.parent().node();
            np->children[index] = trace.child_of_parent();
            break;
          }
      
        case 8:
        case 16:
        case 24:
        case 32:
        case 40:
        case 48:
          TESTPOINT(BitTrieAdd1);
          trace.grow_node_by(8);
          
        default: 
          TESTPOINT(BitTrieAdd2);
          trace.add_node(node);
          n = (BitTrie*)trace.parent().node();
          n->add(index, trace.child_of_parent());
      }
    }
  //@+node:michael.20141230111914.109: *4* remove_child
  bool remove_child(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key.trieindex();

      BitTrie *n = (BitTrie*)rnode.node();
      if (index < 0) {
        TESTPOINT(BitTrieRemove0);
        *rnode.extra() = 0;
        return n->count() > 0;
      }
      
      n->remove(index);
      switch(n->count()) {
        case 0: 
          return *rnode.extra() != 0;
          
        case 8:
        case 16:
        case 24:
        case 32:
        case 40:
        case 48:
          TESTPOINT(BitTrieRemove1);
          trace.grow_node_by(-8);
      }
       
      return true;
    }
  //@+node:michael.20150106224503.24: *4* eat_child
  void change_to_compressed(NodeRef& rnode, const CompressBuffer& buffer) {
    int nsize = (int)page_pad(buffer.size+sizeof(nodeid_t));
    rnode.page.grow_node_by(rnode.id, nsize -(int)rnode.size());
    
    Compressed *c = (Compressed*)rnode.node();
    c->child = buffer.child;
    memcpy(c->data, buffer.data, buffer.size);
    rnode.set_len(buffer.size);
    rnode.set_type(kCompressed);
  }

  bool eat_child(NodeRef& rnode) {
      SingleChild me;
      if (get_single_child(rnode, &me) && me.index >= 0) {
        NodeRef child(rnode.page, me.child);
        switch(child.type())  {
          case kCompressed: {
              child.eat_child();
              TESTPOINT(BitTrieEatCompressed);
              CompressBuffer buffer;
              save_compressed(child, &buffer);
              memmove(buffer.data+1, buffer.data, buffer.size);
              buffer.data[0] = (char)me.index;
              buffer.size++;
              rnode.page.grow_node_by(child.id, -(int)child.size());
              change_to_compressed(rnode, buffer);
              rnode.page.free_node(child.id);
              return true;
            }
          
          case kBitTrie: {
              SingleChild s_child;
              if (get_single_child(child, &s_child) && s_child.index >= 0) {
                TESTPOINT(BitTrieEatSingle);
                CompressBuffer buffer;
                buffer.data[0] = (char)me.index;
                buffer.data[1] = (char)s_child.index;
                buffer.size = 2;
                buffer.child = s_child.child;
                change_to_compressed(rnode, buffer);
                rnode.page.free_node(child.id);
                return true;
              }
            }
        }
        //if me.index < 0: me must be eaten by parent
      }

      if (eat_null_children(rnode))
        return true;
      
      return false;
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
// a leaf node
struct LeafHandler : public LeafBase {
  //@+others
  //@+node:michael.20150106224503.51: *4* find
  void find(NodeRef& rnode, Trace& trace) {
    }
  //@+node:michael.20150110130802.12: *4* prev
  void prev(NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      if (key.empty())
          trace.parent_prev();
      else
        trace.cut_key();
    }
  //@+node:michael.20141230111914.78: *4* reinsert_me
  void reinsert_me(TempLeaf& me, Trace& trace) {
      trace.add_node(me);
      // extra() is the end node
      *trace.parent().extra() = trace.child_of_parent();
      trace.pop(); // back to parent
    }
  //@+node:michael.20141230111914.77: *4* add
  void add(const TempNode& end, NodeRef& rnode, Trace& trace) {
      Slice key(trace.current_key());
      assert(key.size() != 0);
      Leaf *n = (Leaf*)rnode.node();
      TempLeaf me(Slice(n->data, rnode.len()));
      TempTrie trie;
      
      if (key.size() == 2) {
        TESTPOINT(LeafAdd0);
        trace.change_node(TempTrie());
        reinsert_me(me, trace);
        trace.add_node(trie);
        BitTrie* pn = (BitTrie*)trace.parent().node();
        pn->add(key.trieindex(), trace.child_of_parent());
      }
      else {
        trace.change_node(trie);
        reinsert_me(me, trace);
        if (key.size() > 2) {
          TESTPOINT(LeafAdd1);
          TempCompressed compressed(key.advance(1));
          trace.add_node(compressed);
          BitTrie* pn = (BitTrie*)trace.parent().node();
          pn->add(key.trieindex(), trace.child_of_parent());
        }
        else {
          TESTPOINT(LeafAdd2);
        }
      }

      trace.current().add(end, trace);
    }
  //@+node:michael.20150106224503.26: *4* eat_child
  bool eat_child(NodeRef& rnode) {
      assert(0);
      return false;
    }
  //@+node:michael.20150101205559.8: *4* dump
  #ifdef DEBUG
  void dump(NodeRef& rnode, std::ostream& out) {
      const char* t3 = "            ";
      Leaf *n = (Leaf*)rnode.node();
      std::string data(n->data, rnode.len());
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
  pageref.new_node(page_pad(size));
  noderef.set_len(value.size());
  Leaf* n = (Leaf*)node();
  memcpy(n->data, value.data(), value.size());
  noderef.set_type(kLeaf);
}

TempTrie::TempTrie(size_t child_count) : TempNode() {
  size_t size;
  if (child_count > 56) {
    pageref.new_node(size=64);
    noderef.set_type(kTrie);
  }
  else {
    size = page_pad(pad8(sizeof(BitTrie)+child_count));
    pageref.new_node(size);
    noderef.set_type(kBitTrie);
  }

  memset(node(), 0, size);
}

TempCompressed::TempCompressed(const Slice& part) : TempNode() {
  size_t size(part.size());
  pageref.new_node(page_pad(sizeof(nodeid_t)+size));
  noderef.set_len(size);
  Compressed* n = (Compressed*)node();
  memcpy(n->data, part.data(), size);
  n->child = 0;
  noderef.set_type(kCompressed);
}
//@-others
} // namespace larch_leaves 
//@-leo
