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
#include "murmurhash3.h"
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20141230111914.148: ** Node Structs
typedef unsigned char trieindex_t;

//@+others
//@+node:michael.20150116155028.17: *3* Compressed
// A Node containing a string part equal to all descendants
// the equal part fit into a page; the data size in NodeRef::len()
struct Compressed {
  nodeid_t child;
  char data[1];
};
//@+node:michael.20150116155028.18: *3* Leaf
// A leaf 
// the data is interpreted by the type (kLeaf od kBitLeaf)
// the size of data is saved in NodeRef::len()
struct Leaf {
  char data[1];
};
//@+node:michael.20150116155028.19: *3* Trie
// Node with the complete alphabet 
// children count is > 56
struct Trie {
  nodeid_t children[64];
};
//@+node:michael.20150116155028.20: *3* BitTrie
// Node with a range
struct BitTrie {
  boost::uint64_t bits;
  nodeid_t children[1];

  size_t count() const {
      return popcount(bits);    
    }
  
  int get_child_index(trieindex_t index) const {
      if (bits & (((boost::uint64_t)1)<<index)) {
        boost::uint64_t mask = ~0;
        mask <<= index;
        return (int)popcount(bits & ~mask);
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
              sizeof(nodeid_t)*(count()-child_index-1));
      children[child_index] = node;
    }
    
  void remove(int index) {
      int child_index = get_child_index(index);
      assert(child_index >= 0);
      bits &= ~(((boost::uint64_t)1)<<index);
      memmove(children+child_index, children+child_index+1,
              sizeof(nodeid_t)*(count()-child_index));
    }
};
//@+node:michael.20150116155028.23: *3* Node
struct Node {
  union {
    Compressed c;
    Leaf l;
    Trie t;
    BitTrie b;
    pageid_t p;
  };
};
//@-others
//@+node:michael.20141215222649.83: ** NodeHandlers
struct LeafBase : public NodeHandler {
  size_t get_len(const NodeRef& rnode) {
      return 0;
    }

  size_t get_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      return 0;
    }
    
  void replace_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
    }
  
  void next(NodeRef& rnode, Node* data, Trace& trace) {
      trace.parent_next();
    }
    
  void prev(NodeRef& rnode, Node* data, Trace& trace) {
      trace.parent_prev();
    }
    
  void first(NodeRef& rnode, Node* data, Trace& trace) {
    }
    
  void last(NodeRef& rnode, Node* data, Trace& trace) {
    }
};


struct TrieBase : public NodeHandler {
  size_t get_len(const NodeRef& rnode) {
      return 1;
    }

  virtual void add_node(Node* data, trieindex_t index, const TempNode& node, 
                        Trace& trace) = 0;

  bool add(const TempNode& leaf, NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
  
      switch(key.size()) {
        case 0:
          TESTPOINT(TrieBaseAdd0);
          trace.add_node(leaf);
          trace.parent().set_extra(trace.child_of_parent());
          break;
          
        case 1: 
          TESTPOINT(TrieBaseAdd1);
          add_node(data, key[0], leaf, trace);
          break;
          
        default: {
          TESTPOINT(TrieBaseAdd2);
          TempCompressed compressed(key.advance(1));
          add_node(data, key[0], compressed, trace);
          trace.current().add(leaf, trace);
          break;
          }
      }
      return true;
    }
};

 
#ifdef DEBUG
void dump_key(const char* data, size_t size, std::ostream& out) {
  out << std::setw(2) << std::setfill('0') << (int)data[0];
  for(size_t i = 1; i < size; i++)
      out << "|" << std::setw(2) << std::setfill('0') << (int)data[i];
}
#endif
 
//@+others
//@+node:michael.20150106224503.25: *3* Eat Tools
// if bitrie has only one child it fills its index
// and child_id and returns true or false otherwise


inline bool get_single_child(const NodeRef& bittrie, nodeid_t* child) {
    assert(bittrie.type() == kBitTrie);
    BitTrie *bt = (BitTrie*)bittrie.node();
    nodeid_t end = bittrie.extra();
    
    if (end && bt->count() == 0) {
      *child = end;
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
      
      nodeid_t new_child;
      if (get_single_child(child, &new_child)) {
        children[i] = new_child;
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
  int keycmp(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      size_t len = rnode.len();
     
      int cmp = memcmp(key.data(), data->c.data, std::min(len, key.size()));
      if (cmp != 0)
        return cmp;
        
      return key.size() >= len ? 0 : -1;
    }
  //@+node:michael.20150106224503.59: *4* keyappend
  void keyappend(NodeRef& rnode, Node* data, Trace& trace) {
      trace.cut_key();
      trace.key.append(data->c.data, rnode.len());
    }
  //@+node:michael.20150106224503.50: *4* get_len
  size_t get_len(const NodeRef& rnode) {
      return rnode.len();
    }
  //@+node:michael.20141230111914.82: *4* find
  void find(NodeRef& rnode, Node* data, Trace& trace) {
      if (keycmp(rnode, data, trace) == 0 && data->c.child)
        rnode.child_find(data->c.child, trace);
    }
  //@+node:michael.20141230111914.83: *4* first
  void first(NodeRef& rnode, Node* data, Trace& trace) {
      keyappend(rnode, data, trace);
      rnode.child_first(data->c.child, trace);
    }
  //@+node:michael.20141230111914.84: *4* last
  void last(NodeRef& rnode, Node* data, Trace& trace) {
      keyappend(rnode, data, trace);
      rnode.child_last(data->c.child, trace);
    }
  //@+node:michael.20150106224503.43: *4* next
  void next(NodeRef& rnode, Node* data, Trace& trace) {
      if (keycmp(rnode, data, trace) < 0) {
        keyappend(rnode, data, trace);
        rnode.child_first(data->c.child, trace);
      }
      else {
        trace.parent_next();
      }
    }
  //@+node:michael.20150106224503.44: *4* prev
  void prev(NodeRef& rnode, Node* data, Trace& trace) {
      if (keycmp(rnode, data, trace) > 0) {
        keyappend(rnode, data, trace);
        rnode.child_last(data->c.child, trace);
      } 
      else {
        trace.parent_prev();
      }
    }
  //@+node:michael.20141230111914.79: *4* get_children
  size_t get_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      if (data->c.child) {
        children[0] = data->c.child;
        return 1;
      }
      return 0;
    }
  //@+node:michael.20141230111914.80: *4* replace_children
  void replace_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      if (data->c.child)
        data->c.child = children[0];
    }
  //@+node:michael.20141230111914.86: *4* reinsert
  void reinsert(TempCompressed& rest_me, nodeid_t child_id, 
                trieindex_t index, Trace& trace) {
      BitTrie* bn;

      if (rest_me.len() == 0) {
        TESTPOINT(CompressReinsert0);
        bn = (BitTrie*)trace.current().node();
        bn->add(index, child_id);
      }
      else {
        TESTPOINT(CompressReinsert1);
        trace.add_node(rest_me);
        bn = (BitTrie*)trace.parent().node();
        bn->add(index, trace.child_of_parent());
        trace.current().node()->c.child = child_id;
        trace.pop();
      }
    }
  //@+node:michael.20141230111914.87: *4* add
  bool add(const TempNode& leaf, NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      size_t size = rnode.len();
      
      if (!data->c.child) {
        // A new leaf with some rest key is inserted
        TESTPOINT(CompressAddNew);
        if (size != key.size()) {
          std::cerr << "wrong size: " << size << "!=" << key.size() << std::endl;
        }
        assert(size == key.size());
        trace.add_node(leaf);
        trace.parent().node()->c.child = trace.child_of_parent();
        return true;
      }
      
      size_t prefix_size = common_prefix(key.data(), data->c.data,
                                         std::min(size, key.size()));
      TempCompressed rest_me(Slice(data->c.data+prefix_size+1, 
                                   size-prefix_size-1));
            
      trace.reserve_space(48+3*sizeof(NodePtr)); 
        // the maximum of additional space we need
        // if current() and its child has been moved to 
        // another page with enough space      
            
      data = trace.current().node();
      nodeid_t child_id = data->c.child;

      trieindex_t first = data->c.data[0];
      if (prefix_size == 0) {
        // insert one trie node
        TESTPOINT(CompressAdd0);
        trace.change_node(TempTrie());
        reinsert(rest_me, child_id, first, trace);
      }
      else {
        // another compressed
        TESTPOINT(CompressAdd1);
        first = data->c.data[prefix_size];

        Slice next_key(key.data(), prefix_size);
        trace.change_node(TempCompressed(next_key));
        
        trace.add_node(TempTrie());
        trace.parent().node()->c.child = trace.child_of_parent();
        reinsert(rest_me, child_id, first, trace);
      }

      trace.current().add(leaf, trace);
      return true;
    }
  //@+node:michael.20150106224503.21: *4* eat_child
  void append_data(NodeRef& rnode, const CompressBuffer& buffer) {
      size_t size = rnode.len();
      size_t nsize = page_pad(size+buffer.size+sizeof(nodeid_t));
      // it is guaranteed that enough space is on page (see eat_child)
      rnode.page.grow_node_by(rnode.id, (int)nsize-(int)rnode.size());
      Compressed& c(rnode.node()->c);
      memcpy(c.data+size, buffer.data, buffer.size);
      rnode.set_len(size+buffer.size);
      c.child = buffer.child;
    }

  bool eat_child(NodeRef& rnode, Node* data) {
      if (eat_null_children(rnode))
        return true;

      CompressBuffer buffer;
      NodeRef child(rnode.page, data->c.child);

      if (child.type() == kCompressed) {
        TESTPOINT(CompressedEatCompressed);
        save_compressed(child, &buffer);
        // make space on page
        if (rnode.page.free_size() < child.size())
          rnode.page.grow_node_by(child.id, -(int)child.size());
          
        append_data(rnode, buffer);
        rnode.page.free_node(child.id);
        return true;
      }
      
      return false;
    }
  //@+node:michael.20150101205559.5: *4* dump
  #ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
      const char* t3 = "            ";
      
      NodePtr *ptr = page->node_ptr + nodeid;
      Compressed *n = &page->get_node(nodeid)->c;
      size_t len = ptr->extra;
      
      out << t3 << "type:  compressed" << std::endl;
      out << t3 << "data:  ";
      dump_key(n->data, len, out);
      out << std::endl;
      out << t3 << "size:  " << (int)len << std::endl
          << t3 << "child: " << (int)n->child << std::endl;
    }
  #endif
  //@-others
  
  Slice append(NodeRef& rnode, Node* data, nodeid_t child, Slice& key) {
      assert(data->c.child == child);
      memcpy((char*)key.data()+key.size(), data->c.data, rnode.len());
      return Slice(key.data(), key.size()+rnode.len());
    }
};

static CompressedHandler compressed;


//@+node:michael.20141215222649.137: *3* Link
// links to another page

struct LinkHandler : public LeafBase {
  NodeRef& link_node(NodeRef& rnode, Node* data, Trace& trace) {
      PageRef next_page(trace.map.get_page(data->p));
      return trace.push(NodeRef(next_page, 0));
    }
    
  void find(NodeRef& rnode, Node* data, Trace& trace) {
      link_node(rnode, data, trace).find(trace);
    }
    
  void first(NodeRef& rnode, Node* data, Trace& trace) {
      link_node(rnode, data, trace).first(trace);
    }
    
  void last(NodeRef& rnode, Node* data, Trace& trace) {
      link_node(rnode, data, trace).last(trace);
    }
    
  bool add(const TempNode& leaf, NodeRef& rnode, Node* data, Trace& trace) {
      assert(0); // may never be called
      return true;
    }
    
  bool eat_child(NodeRef& rnode, Node* data) {
      return false;
    }
  
    
#ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
      const char* t3 = "            ";
      pageid_t page_id = page->get_node(nodeid)->p;
      out << t3 << "type:  link" << std::endl
          << t3 << "page:  " << page_id << std::endl;
    }
#endif    
};

static LinkHandler link;
//@+node:michael.20141215222649.93: *3* Trie
struct TrieHandler : public TrieBase {
  //@+others
  //@+node:michael.20141230111914.94: *4* find
  void find(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      
      if (key.empty()) {
        nodeid_t end_child = rnode.extra();
        if (end_child)
          rnode.child_find(end_child, trace);
        return;
      }

      trieindex_t index = key[0];
      nodeid_t child_id= data->t.children[index];
      if (child_id) 
        rnode.child_find(child_id, trace);
    }
  //@+node:michael.20141230111914.97: *4* first
  void first(NodeRef& rnode, Node* data, Trace& trace) {
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        rnode.child_first(end_child, trace);
        return;
      }

      Trie* n = &data->t;
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
  void last(NodeRef& rnode, Node* data, Trace& trace) {
      Trie* n = &data->t;
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
  void next(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? 0 : key[0]+1;
      
      Trie *n = &data->t;
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
  void prev(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());

      if (key.empty()) {
        trace.parent_prev();
        return;
      }

      int index = ((int)key[0])-1;
      Trie *n = &data->t;
      for(; index >= 0; index--) {
        nodeid_t child_id = n->children[index];
        if (child_id) {
          trace.cut_key();
          trace.key.push_back((char)index);
          rnode.child_last(child_id, trace);
          return;
        }
      }
      
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        trace.cut_key();
        rnode.child_last(end_child, trace);
      }
      else 
        trace.parent_prev();
    }
  //@+node:michael.20141230111914.89: *4* get_children
  size_t get_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = &data->t;
      for(int i = 0; i < 64; i++) {
        if (n->children[i]) 
          children[count++] = n->children[i];
      }
      nodeid_t end_child = ptr->extra;
      if (end_child)
        children[count++] = end_child;
        
      return count;
    }
  //@+node:michael.20141230111914.90: *4* replace_children
  void replace_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = &data->t;
      for(int i = 0; i < 64; i++) {
        if (n->children[i]) 
          n->children[i] = children[count++];
      }
      
      if (ptr->extra)
        ptr->extra = children[count];
    }
  //@+node:michael.20141230111914.92: *4* add_node
  void add_node(Node* data, trieindex_t index, const TempNode& node, Trace& trace) {
      trace.add_node(node);
      data->t.children[index] = trace.child_of_parent();
    }
  //@+node:michael.20141230111914.93: *4* remove_child
  bool remove_child(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key[0];

      if (index < 0) {
        rnode.set_extra(0);
        return true;
      }

      Trie *n = &data->t;
      n->children[index] = 0;
      
      size_t count = 0;
      for(int i = 0; i < 64; i++) {
        if (n->children[i])
          count++;
      }
      
      if (count <= 56) {
        TESTPOINT(TrieRemove);
        TempTrie trie(count);
        trie.set_extra(rnode.extra());
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
  bool eat_child(NodeRef& rnode, Node* data) {
      return eat_null_children(rnode);
    }
  //@+node:michael.20150101205559.6: *4* dump
  #ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
      const char* t3 = "            ";
      
      NodePtr *ptr = page->node_ptr + nodeid;
      Trie *n = &page->get_node(nodeid)->t;
      
      out << t3 << "type:  trie" << std::endl;
      out << t3 << "data:  ";
      
      nodeid_t end = ptr->extra;
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
  
  Slice append(NodeRef& rnode, Node* data, nodeid_t child, Slice& key) {
      if (child == rnode.extra())
        return key;
        
      Trie *n = &data->t;
      for(size_t i = 0; i < 64; i++) {
        if (n->children[i] == child) {
          ((char*)key.data())[key.size()] = (char)i;
          return Slice(key.data(), key.size()+1);
        }
      }
      assert(0);
    }
};

static TrieHandler trie;


//@+node:michael.20141215222649.95: *3* BitTrie
struct BitTrieHandler : public TrieBase {
  //@+others
  //@+node:michael.20141230111914.103: *4* find
  void find(NodeRef& rnode, Node* data, Trace& trace) {
      BitTrie *n = &data->b;
      Slice key(trace.current_key());
      
      if (key.empty()) {
        nodeid_t end_child = rnode.extra();
        if (end_child)
          rnode.child_find(end_child, trace);
        return;
      }
        
      trieindex_t index = key[0];
      int child_index = n->get_child_index(index);
      if (child_index >= 0) {
        nodeid_t child_id = n->children[child_index];
        rnode.child_find(child_id, trace);
      }
    }
  //@+node:michael.20141230111914.106: *4* first
  void first(NodeRef& rnode, Node* data, Trace& trace) {
      BitTrie *n = &data->b;
      nodeid_t end_child = rnode.extra();
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
  void last(NodeRef& rnode, Node* data, Trace& trace) {
      BitTrie *n = &data->b;
      int index = n->last_bit();
      if (index < 0)
        return;

      trace.key.push_back((char)index);
      int child_index = n->get_child_index(index);
      rnode.child_last(n->children[child_index], trace);
    }
  //@+node:michael.20141230111914.104: *4* next
  void next(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key[0];
      BitTrie *n = &data->b;
      
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
  void prev(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key[0];
      
      if (index < 0) {
        trace.parent_prev();
        return;
      }
          
      BitTrie *n = &data->b;
      index = n->prev_bit(index);
      if (index >= 0) {
        trace.cut_key();
        trace.key.push_back((char)index);
        int child_index = n->get_child_index(index);
        rnode.child_last(n->children[child_index], trace);
        return;
      }
    
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        trace.cut_key();
        rnode.child_last(end_child, trace);
        return;
      }

      trace.parent_prev();
    }
  //@+node:michael.20141230111914.100: *4* get_children
  size_t get_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      BitTrie *n = &data->b;
      size_t count = n->count();
      for(size_t i = 0; i < count; i++) 
          children[i] = n->children[i];

      nodeid_t end_child = ptr->extra;
      if (end_child)
        children[count++] = end_child;

      return count;
    }
  //@+node:michael.20141230111914.101: *4* replace_children
  void replace_children(NodePtr* ptr, Node* data, nodeid_t children[65]) {
      BitTrie *n = &data->b;
      size_t count = n->count();
      for(size_t i = 0; i < count; i++) 
          n->children[i] = children[i];

      if (ptr->extra)
        ptr->extra = children[count];
    }
  //@+node:michael.20141230111914.108: *4* add_node
  void add_node(Node* data, trieindex_t index, const TempNode& node, Trace& trace) {
      BitTrie *n = &data->b;
      size_t count = n->count();
      if (count == 56) {
        TESTPOINT(BitTrieAdd0);
        TempTrie trie(count+1);
        trie.set_extra(trace.current().extra());
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
        return;
      }

      if (count && count % 4 == 0) {
        TESTPOINT(BitTrieAdd1);
        trace.grow_node_by(4*sizeof(nodeid_t));
      }
      
      TESTPOINT(BitTrieAdd2);
      trace.add_node(node);
      n = (BitTrie*)trace.parent().node();
      n->add(index, trace.child_of_parent());
    }
  //@+node:michael.20141230111914.109: *4* remove_child
  void change_to_compressed(NodeRef& rnode, const CompressBuffer& buffer) {
      int nsize = (int)page_pad(buffer.size+sizeof(nodeid_t));
      rnode.page.grow_node_by(rnode.id, nsize -(int)rnode.size());
      
      Compressed *c = (Compressed*)rnode.node();
      c->child = buffer.child;
      memcpy(c->data, buffer.data, buffer.size);
      rnode.set_len(buffer.size);
      rnode.set_type(kCompressed);
    }

  bool remove_child(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      int index = key.empty() ? -1 : key[0];

      BitTrie *n = &data->b;
      if (index < 0) {
        TESTPOINT(BitTrieRemove0);
        rnode.set_extra(0);
      }
      else
        n->remove(index);
        
      size_t count = n->count();
      switch(count) {
        case 0: 
          return rnode.extra() != 0;
          
        case 1: 
          if (!rnode.extra()) {
            // change to compressed
            TESTPOINT(BitTrieRemove2);
            CompressBuffer buffer;
            buffer.data[0] = (char)n->first_bit();
            buffer.size = 1;
            buffer.child = n->children[0];
            change_to_compressed(rnode, buffer);
          }
          return true;
        
        default:
          if (count > 4 && count % 4 == 0) {
            TESTPOINT(BitTrieRemove1);
            trace.grow_node_by(-4*(int)sizeof(nodeid_t));
          }
      }
       
      return true;
    }
  //@+node:michael.20150106224503.24: *4* eat_child
  bool eat_child(NodeRef& rnode, Node* data) {
      if (eat_null_children(rnode))
        return true;
      
      return false;
    }
  //@+node:michael.20150101205559.7: *4* dump
  #ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
      const char* t3 = "            ";
      
      NodePtr *ptr = page->node_ptr + nodeid;
      BitTrie *n = &page->get_node(nodeid)->b;

      out << t3 << "type:  bittrie" << std::endl;
      out << t3 << "data:  ";
      
      nodeid_t end = ptr->extra;
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
  
  Slice append(NodeRef& rnode, Node* data, nodeid_t child, Slice& key) {
      if (child == rnode.extra())
        return key;
        
      BitTrie *n = &data->b;
      for(int b = n->first_bit(); b >= 0; b = n->next_bit(b)) {
        int ci = n->get_child_index(b);
        if (n->children[ci] == child) {
          ((char*)key.data())[key.size()] = (char)b;
          return Slice(key.data(), key.size()+1);
        }
      }
      assert(0);
    }
};

static BitTrieHandler bittrie;
//@+node:michael.20141215222649.84: *3* Leaf
// a leaf node
struct LeafHandler : public LeafBase {
  //@+others
  //@+node:kochelmonster-.20150117183203.2: *4* is_valid
  bool is_valid(const NodeRef& rnode, const Trace& trace) const {
      return trace.back->end >= trace.key.size();
    }
  //@+node:michael.20150118002311.12: *4* get_value
  Slice get_value(const NodeRef& rnode, Node* data, Trace& trace) {
      return Slice(data->l.data, rnode.len());
    }
  //@+node:michael.20150106224503.51: *4* find
  void find(NodeRef& rnode, Node* data, Trace& trace) {
    }
  //@+node:michael.20150110130802.12: *4* prev
  void prev(NodeRef& rnode, Node* data, Trace& trace) {
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
      trace.parent().set_extra(trace.child_of_parent());
      trace.pop(); // back to parent
    }
  //@+node:michael.20141230111914.77: *4* add
  bool add(const TempNode& end, NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      
      if (key.size() == 0) {
        trace.change_node(end);
        return false;
      }
      
      char buffer[MAX_PAGE_VALUE_SIZE];
      size_t size = rnode.len();
      memcpy(buffer, data->l.data, size);
      TempLeaf me(Slice(buffer, size));
      trace.change_node(TempTrie());
      reinsert_me(me, trace);
      if (key.size() > 1) {
        TESTPOINT(LeafAdd0);
        TempCompressed compressed(key.advance(1));
        trace.add_node(compressed);
        BitTrie* pn = &trace.parent().node()->b;
        pn->add(key[0], trace.child_of_parent());
      }
      else {
        TESTPOINT(LeafAdd2);
      }

      trace.current().add(end, trace);
      return true;
    }
  //@+node:michael.20150106224503.26: *4* eat_child
  bool eat_child(NodeRef& rnode, Node* data) {
      assert(0);
      return false;
    }
  //@+node:michael.20150101205559.8: *4* dump
  #ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
      const char* t3 = "            ";
      
      NodePtr *ptr = page->node_ptr + nodeid;
      Leaf *n = &page->get_node(nodeid)->l;
      size_t len = ptr->extra & 0x3f;
      
      std::string data(n->data, len);
      out << t3 << "type:  leaf" << std::endl
          << t3 << "data:  " << data << std::endl
          << t3 << "size:  " << data.size() << std::endl;
    }
  #endif
  //@-others
};

static LeafHandler leaf;
//@-others

NodeHandler* NodeHandler::handlers[7] = {
  &leaf,
  &link,
  &compressed,
  &trie,
  &bittrie,
};
//@+node:michael.20141230111914.75: ** TempNode
TempLeaf::TempLeaf(const Slice& value) {
  _type = kLeaf;
  _size = value.size();
  assert(_size <= MAX_PAGE_VALUE_SIZE);
  set_len(_size);
  _node = (Node*)value.data();
}

TempLeaf::TempLeaf(leaf_ptr* ptr) {
  _type = kLeaf;
  _size = sizeof(leaf_ptr); 
  set_len(_size | 0x40);
  _node = (Node*)ptr;
}


TempTrie::TempTrie(size_t child_count) {
  _node = (Node*)_children;

  if (child_count > 56) {
    _size = sizeof(_children);
    _type = kTrie;
    memset(_children, 0, sizeof(_children));
  }
  else {
    _size = page_pad(
      pad<8*sizeof(nodeid_t)>(sizeof(BitTrie)+child_count*sizeof(nodeid_t)));

    _type = kBitTrie;
    node()->b.bits = 0;
  }
  set_extra(0);
}

TempCompressed::TempCompressed(const Slice& part) {
  _type = kCompressed;
  _node = (Node*)_data;
  _size = part.size() + sizeof(nodeid_t);
  set_len(part.size());
  Compressed* n = &node()->c;
  memcpy(n->data, part.data(), part.size());
  n->child = 0;
}
//@+node:michael.20150118002311.21: ** PageRef
//@+others
//@+node:michael.20150118002311.20: *3* dump
#ifdef DEBUG
void PageRef::dump(std::ostream& out) {
  const char* t1 = "    ";
  const char* t2 = "      ";
  const char* t3 = "          ";
  out << t1 << "- id:         " << id << std::endl
      << t2 << "offset:     " << offset << std::endl;
      
  if (page->type == kTriePage) {
    out << t2 << "type:       TriePage" << std::endl
        << t2 << "node_count: " << count() << std::endl
        << t2 << "size:       " << size() << std::endl
        << t2 << "free_size:  " << free_size() << std::endl
        << t2 << "sum_size:   " << size() + free_size() << std::endl
        << t2 << "nodes: " << std::endl;
  
    for(size_t id = 0; id < count(); id++) {
      NodePtr *ptr = page->node_ptr + id;
      out << t3 << "- id:    " << (int)id << std::endl
          << t3 << "  ptr:   " << (int)ptr->offset << std::endl;
          
      NodeHandler::handlers[ptr->type]->dump(page, (nodeid_t)id, out);
    }
  }
}
#endif
//@-others
//@-others
} // namespace larch_leaves 
//@-leo
