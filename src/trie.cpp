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
  char data[];
};
//@+node:michael.20150116155028.18: *3* Leaf
// A leaf 
// the data is interpreted by the type (kLeaf od kBitLeaf)
// the size of data is saved in NodeRef::len()
struct Leaf {
  char data[];
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
//@+node:michael.20150116155028.21: *3* Bucket
// maximal 10 key/value pairs
// the format per key value pair is
// [KeyValueSize(2)][key0(1)]...[keyN(1)][val0(1)]...[valN(1)]

struct KeyValueSize {
  union {
    struct {
      boost::uint16_t vsize:7;
      boost::uint16_t padd: 9;
    };
    struct {
      boost::uint16_t value_size:6;
      boost::uint16_t value_is_link:1;
      boost::uint16_t key_size: 9;
    };
  };
};

struct Bucket {
  char data[];
  
  // get the key and value of current positions and returns the
  // position of the next key, value storage
  static char *get_key_value(char* pos, 
                             char** key, psize_t* ksize,
                             char** value, vsize_t* vsize) {
      KeyValueSize kvs = *(KeyValueSize*)pos;
      *ksize = kvs.key_size;
      *vsize = kvs.vsize;
      pos += sizeof(KeyValueSize);
  
      *key = pos;
      pos += kvs.key_size;
      *value = pos;
      return pos + kvs.value_size;
    }
    
  static void set_key_value(char* pos, 
                            const char* key, psize_t ksize,
                            const char* value, vsize_t vsize) {
      KeyValueSize kvs;
      kvs.vsize = vsize;
      kvs.key_size = ksize;
      *(KeyValueSize*)pos = kvs;
      pos += sizeof(KeyValueSize);
  
      memcpy(pos, key, kvs.key_size);
      pos += kvs.key_size;

      memcpy(pos, value, kvs.value_size);
    }
    
  static char *get_key(char* pos, char** key, psize_t* ksize) {
      KeyValueSize kvs = *(KeyValueSize*)pos;
      *ksize = kvs.key_size;
      pos += sizeof(KeyValueSize);
      *key = pos;
      return pos + kvs.key_size + kvs.value_size;
    }

  static char* value(char* pos, vsize_t* vsize) {
      KeyValueSize kvs = *(KeyValueSize*)pos;
      *vsize = kvs.vsize;
      return pos + sizeof(KeyValueSize) + kvs.key_size;
    }

  static char *next(char* pos) {
      KeyValueSize kvs = *(KeyValueSize*)pos;
      return pos + sizeof(KeyValueSize) + kvs.key_size +  kvs.value_size;
    }
    
  static size_t needed_size(size_t ksize, size_t vsize) {
      return sizeof(KeyValueSize) + ksize + (vsize & 0x3f);
    }
    
  // returns the real size of the bucket <= node.size()
  size_t size(psize_t count) {
      char* p = data;
      for(psize_t i = 0; i < count; i++)
        p = next(p);
      
      return p - data;
    }
};

//@+node:michael.20150116155028.22: *3* Hash
// a link to a hash Page

struct Hash {
  pageid_t pageids[HASH_PAGE_COUNT];
};



/*
  A Hash table page.
  
  This is one page inside a collection of 32 pages forming a hashtable.
  Each hash page has a table of 128 entries => the complete hash table
  has 128*32 = 4096 slots.
  
  Every slot points to a BucketNode. Because each BucketNode is aligned
  on a 64 byte address a 8byte value (256*64 == 16384 > 8192) is sufficent
  to address all slots on a page.

  every slot starts with a count byte (containing the count of buckets)
  followed by a Bucket Node.
  
  Like the Node Page BucketNodes grow from bottom up.
*/

#define HASHPAGE_HEADER (3*sizeof(boost::uint16_t)+sizeof(boost::uint8_t)*(PAGE_HASH_SIZE))

struct HashPage {
  typedef boost::uint8_t ptr; 
  union {
    char data[PAGE_SIZE];
    struct {
      boost::uint16_t type;
      boost::uint16_t count;     // node count
      boost::uint16_t end;
      ptr slots[PAGE_HASH_SIZE]; // pages hash table 
    };
  };

  void init() {
      type = kHashPage;
      count = 0;
      end = sizeof(data);
      memset(slots, 0, sizeof(slots));
    }
    
  char* get_slot(size_t slot_id) {
      ptr slot = slots[slot_id];
      if (!slot)
        return NULL; // slot does not exist yet
        
      return &data[((size_t)slot)*BUCKET_ALIGN];
    }
  
  char* new_slot(size_t slotid, size_t size) {
    assert(size % BUCKET_ALIGN == 0);
  
    if (end < HASHPAGE_HEADER + size)
      return NULL; // overflow
  
    end -= size;
    slots[slotid] = (ptr)(end / BUCKET_ALIGN);
    char *slot = &data[end];
    *slot = 0;
    return slot;
  }
  
  void remove_slot(size_t slotid, size_t size) {
      char* slot = &data[((size_t)slots[slotid])*BUCKET_ALIGN];
      slots[slotid] = 0;
      grow_slot(slot, size, 0);
    }
  
  char* grow_slot(char* slot, size_t old_size, size_t new_size) {
    if (new_size == old_size)
      return slot;
  
    assert(new_size % BUCKET_ALIGN == 0);
    assert(old_size % BUCKET_ALIGN == 0);
    assert(slot - data > 0);
    assert(slot - data < PAGE_SIZE);
    
    int delta = (int)new_size - old_size;
    
    if (old_size < new_size) {
      if (end < HASHPAGE_HEADER + delta)
        return NULL; // overflow
      
      memmove(&data[end-delta], &data[end], slot-&data[end]+old_size);
    }
    else {
      memmove(&data[end-delta], &data[end], slot-&data[end]+new_size);
    }
    
    end -= delta;
    delta /= BUCKET_ALIGN;
    
    ptr slot_ptr = (ptr)((slot-data)/BUCKET_ALIGN);
    ptr* p = slots;
    for(size_t i = 0; i < PAGE_HASH_SIZE; i++, p++) {
      if (*p && *p <= slot_ptr)
        *p -= delta;
    }
    return slot-delta;
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
    Bucket e;
    Hash h;
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

struct BucketBase : public LeafBase {
  virtual void sort(NodeRef& rnode, Node* data, Trace& trace) = 0;

  bool is_valid(const NodeRef& rnode, const Trace& trace) const {
      return trace.sorter.is_valid(trace.current_key());
    }
    
  Slice get_value(const NodeRef& rnode, Node* data, Trace& trace) {
      return trace.sorter.get_value();
    }  

  void prev(NodeRef& rnode, Node* data, Trace& trace) {
      sort(rnode, data, trace);
      trace.sorter.prev(trace);
    }
  
  void next(NodeRef& rnode, Node* data, Trace& trace) {
    sort(rnode, data, trace);
    trace.sorter.next(trace);
  }
  
  void first(NodeRef& rnode, Node* data, Trace& trace) {
    sort(rnode, data, trace);
    trace.sorter.first(trace);
  }

  void last(NodeRef& rnode, Node* data, Trace& trace) {
      sort(rnode, data, trace);
      trace.sorter.last(trace);
    }
};

struct TrieBase : public NodeHandler {
  size_t get_len(const NodeRef& rnode) {
      return 1;
    }

  virtual void add_node(Node* data, trieindex_t index, const TempNode& node, 
                        Trace& trace) = 0;

  bool add(const TempNode& leaf, bool with_buckets, NodeRef& rnode, 
           Node* data, Trace& trace) {
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
          trace.current().add(leaf, with_buckets, trace);
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
  bool change_to_bucket(const Slice& key, const TempNode& leaf, NodeRef& rnode, 
                        Node* data, Trace& trace) {
                        
    NodeRef child(rnode.page, data->c.child);
    if (child.type() == kLeaf) {
      // change to bucket
      char new_key[MAX_KEY_SIZE_64];
      char old_key[MAX_KEY_SIZE_64];
      char value_buffer[MAX_PAGE_VALUE_SIZE];
      size_t size = rnode.len();
      TempLeaf child_leaf(Slice(value_buffer, child.len()));
      memcpy(value_buffer, child.node(), child.len() & 0x3f);
      memcpy(new_key, key.data(), key.size());
      memcpy(old_key, data->c.data, size);
      Slice old(old_key, size);
      Slice new_(new_key, key.size());
      
      if (trace.free_node(child.page, child.id))
        child.page.defragment(trace);
      
      trace.change_node(TempBucket());
      
      trace.cut_key();
      trace.key.append(old.data(), old.size());
      trace.current().find(trace);
      trace.current().add(child_leaf, true, trace);
      
      trace.cut_key();
      trace.key.append(new_.data(), new_.size());
      trace.current().find(trace);
      trace.current().add(leaf, true, trace); 
      return true;
    }
    return false;
  }
    
  bool add(const TempNode& leaf, bool with_buckets, NodeRef& rnode, 
           Node* data, Trace& trace) {
      Slice key(trace.current_key());
      size_t size = rnode.len();
      
      if (!data->c.child) {
        // A new leaf with some rest key is inserted
        TESTPOINT(CompressAddNew);
        assert(size == key.size());
        trace.add_node(leaf);
        trace.parent().node()->c.child = trace.child_of_parent();
        return true;
      }
      
      if (with_buckets) {
        if (change_to_bucket(key, leaf, rnode, data, trace))
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

      trace.current().add(leaf, with_buckets, trace);
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
    
  bool add(const TempNode& leaf, bool with_buckets, NodeRef& rnode, 
           Node* data, Trace& trace) {
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
  bool add(const TempNode& end, bool with_buckets, NodeRef& rnode, 
           Node* data, Trace& trace) {
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
        trace.current().add(end, with_buckets, trace);
        return true; //debug
      }
      else {
        TESTPOINT(LeafAdd2);
      }

      trace.current().add(end, with_buckets, trace);
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
//@+node:michael.20150116155028.16: *3* Bucket
// A single bucket node
struct BucketHandler : public BucketBase {
  //@+others
  //@+node:kochelmonster-.20150117183203.5: *4* find
  void find(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      psize_t count = rnode.extra();
      
      if (trace.sorter.current_data == data && trace.sorter.size() == count) {
        trace.sorter.find(key);
        return;
      }
      
      char* pos = data->e.data;
      char* item_key;
      psize_t ksize;
      for(psize_t i = 0; i < count; i++) {
        char *old_pos = pos;
        pos = Bucket::get_key(pos, &item_key, &ksize);
        if (key.size() == ksize && memcmp(item_key, key.data(), ksize) == 0) {
          trace.sorter.index = 0;
          trace.sorter.pointers[0] = old_pos;
          return;
        }
      }
    }
  //@+node:kochelmonster-.20150117183203.4: *4* sort
  void sort(NodeRef& rnode, Node* data, Trace& trace) {
      size_t count = rnode.extra();
      if (trace.sorter.prepare_sort_bucket(data, count))
        return;

      Slice key(trace.current_key());
      bool do_find = trace.sorter.is_valid(key);
      char *pos = data->e.data;  
      char **dst = &trace.sorter.pointers[0];
      
      size_t i = 0;
      while(true) {
        *dst++ = pos;
        if (++i >= count)
          break; // don't do an unnecessary Bucket::next
        pos = Bucket::next(pos);
      }

      trace.sorter.sort();
      if (do_find)
        trace.sorter.find(key);
    }
  //@+node:kochelmonster-.20150117183203.6: *4* change_to_hash
  void change_to_hash(const TempNode& leaf, NodeRef& rnode, 
                      Node* data, Trace& trace) {
      Slice key(trace.current_key());
      std::string current_key(key.data(), key.size()); 
      
      // save the bucket data
      psize_t count = rnode.extra();
      char data_[MAX_BUCKET_SIZE];
      memcpy(data_, data->e.data, rnode.size());
        
      trace.change_node(TempHash());
      
      // adding nodes to hash will not change the page!
      // so we can use static pointers to hash
      NodeRef& current = trace.current();
      data = current.node();
      NodeHandler *handler = current.handler;
      
      // reinsert the bucket data
      char *p = data_;
      for(psize_t i = 0; i < count; i++) {
        char *key, *value;
        psize_t ksize;
        vsize_t vsize;
        p = Bucket::get_key_value(p, &key, &ksize, &value, &vsize);
        
        trace.cut_key();
        trace.key.append(key, ksize);
        handler->find(current, data, trace);
        handler->add(TempLeaf(Slice(value, vsize)), true, current, data, trace);
      }                                   

      // add the new key        
      trace.cut_key();
      trace.key.append(current_key);
      trace.current().add(leaf, true, trace);
    }
  //@+node:kochelmonster-.20150117183203.7: *4* remove_bucket
  void remove_bucket(NodeRef& rnode, Node* data, Trace& trace) {
      char *p1 = trace.sorter.pointers[trace.sorter.index];
      char *p2 = Bucket::next(p1);
      size_t node_size = rnode.size();
      memmove(p1, p2, node_size - (p2 - data->e.data));
    }
  //@+node:kochelmonster-.20150117183203.8: *4* add
  bool add(const TempNode& leaf, bool with_buckets, NodeRef& rnode, 
           Node* data, Trace& trace) {
      Slice key(trace.current_key());
      bool result = true;
      size_t count = rnode.extra();
      
      if (trace.sorter.is_valid(key)) {
        // the key is inside the bucket
        remove_bucket(rnode, data, trace);
        result = false;
      }
      else if (count == MAX_BUCKET_COUNT) {
        change_to_hash(leaf, rnode, data, trace);
        return true;
      }
      
      size_t bsize = data->e.size(count);
      size_t kv_size = Bucket::needed_size(key.size(), leaf.len());
      size_t new_node_size = pad<BUCKET_ALIGN>(bsize+kv_size);
      size_t node_size = rnode.size();
      
      if (new_node_size > node_size) 
        trace.grow_node_by((int)(new_node_size-node_size));
        
      data = trace.current().node();
      char* p = data->e.data;
      for(psize_t i = 0; i < count; i++)
        p = Bucket::next(p);
        
      Bucket::set_key_value(
        p, key.data(), key.size(), leaf.node()->l.data, leaf.len());
     
      trace.current().set_extra(count+1);
      
      trace.sorter.clear();
      return result;
    }
  //@+node:kochelmonster-.20150117183203.9: *4* remove_child
  bool remove_child(NodeRef& rnode, Node* data, Trace& trace) {
      size_t count = rnode.extra();
      if (count == 1) // no clear necessary (trace.remove will call pop)
        return false;
        
      remove_bucket(rnode, data, trace);
      trace.current().set_extra(--count);
      trace.sorter.clear();
      
      size_t bsize = data->e.size(count);
      size_t new_node_size = pad<BUCKET_ALIGN>(bsize);
      size_t node_size = rnode.size();
      if (new_node_size != node_size) 
        trace.grow_node_by((int)(new_node_size-node_size));    
      
      return true;
    }
  //@+node:kochelmonster-.20150117183203.10: *4* eat_child
  bool eat_child(NodeRef& rnode, Node* data) {
      return false;
    }
  //@+node:kochelmonster-.20150117183203.11: *4* dump
  #ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
    const char* t3 = "            ";
    NodePtr *ptr = page->node_ptr + nodeid;
    size_t count = ptr->extra;

    out << t3 << "type:   bucket" << std::endl
        << t3 << "count:  " << count << std::endl;
        
    char* pos = page->get_node(nodeid)->e.data;
    char *key, *value;
    psize_t ksize;
    vsize_t vsize;
    for(psize_t i = 0; i < count; i++) {
      pos = Bucket::get_key_value(pos, &key, &ksize, &value, &vsize);
      
      out << t3 << "key" << (int)i << ":   ";
      dump_key(key, ksize, out);
      out << std::endl;
      
      std::string val(value, vsize&0x3f);
      out << t3 << "value" << (int)i << ": " << val.c_str() << std::endl;
    }
  }
  #endif
  //@-others
};

static BucketHandler bucket;
//@+node:kochelmonster-.20150117183203.20: *3* Hash
// A hash node
struct HashHandler : public BucketBase {
  //@+others
  //@+node:michael.20150118002311.4: *4* get_slot
  char* get_slot(HashPage** page, size_t* slotid, Trace& trace) {
      size_t page_index = trace.sorter.hash / PAGE_HASH_SIZE;
      *slotid = trace.sorter.hash % PAGE_HASH_SIZE;
      
      assert(page_index >= 0);
      assert(page_index < HASH_PAGE_COUNT);
      PageRef& page_ = trace.sorter.hash_pages[page_index];
      *page = (HashPage*)page_.page;
      if (!*page) 
        return NULL;
      
      return (*page)->get_slot(*slotid);
    }
  //@+node:kochelmonster-.20150117183203.22: *4* find
  void find(NodeRef& rnode, Node* data, Trace& trace) {
      Slice key(trace.current_key());
      
      trace.sorter.init_hash(data, data->h.pageids, trace);
      trace.sorter.hash = calc_hash(key.data(), key.size()) % HASH_SIZE;
      
      HashPage *p;
      size_t slotid;
      char *slot = get_slot(&p, &slotid, trace);
      
      if (! slot)
        return;
      
      size_t count = *slot++;
      char* item_key;
      psize_t ksize;
      for(psize_t i = 0; i < count; i++) {
        char *old_pos = slot;
        slot= Bucket::get_key(slot, &item_key, &ksize);
        if (key.size() == ksize && memcmp(item_key, key.data(), ksize) == 0) {
          trace.sorter.index = 0;
          trace.sorter.pointers[0] = old_pos;
          trace.sorter.clear();
          return;
        }
      }
    }
  //@+node:michael.20150118002311.6: *4* calc_node_count
  size_t calc_node_count(Trace &trace) {
      size_t count = 0;
      PageRef* p = trace.sorter.hash_pages;
      for(size_t i = 0; i < HASH_PAGE_COUNT; i++, p++) {
        if (p->page)
          count += ((HashPage*)p->page)->count;
      }
      return count;
    }
  //@+node:kochelmonster-.20150117183203.23: *4* sort
  void sort(NodeRef& rnode, Node* data, Trace& trace) {
      trace.sorter.init_hash(data, data->h.pageids, trace);
      if (trace.sorter.prepare_sort_hash(data))
        return;

      Slice key(trace.current_key());
      bool do_find = trace.sorter.is_valid(key);
      size_t count = calc_node_count(trace);
      trace.sorter.pointers.resize(count);    

      // fill pointers
      char **dst = &trace.sorter.pointers[0];
      PageRef* p = trace.sorter.hash_pages;
      for(size_t i = 0; i < HASH_PAGE_COUNT; i++, p++) {
        HashPage* page = (HashPage*)p->page;
        if (!page)
          continue;
          
        for(size_t j = 0; j < PAGE_HASH_SIZE; j++) {
          char* bucket = page->get_slot(j);
          if (!bucket)
            continue;
            
          size_t k = 0, count = *bucket++;
          while(true) {
            *dst++ = bucket;
            if (++k >= count)
              break; // don't do an unnecessary Bucket::next
            bucket = Bucket::next(bucket);
          }
        }
      }
                
      trace.sorter.sort();
      if (do_find)
        trace.sorter.find(key);
    }
  //@+node:michael.20150118002311.5: *4* burst
  void burst(const TempNode& leaf, NodeRef& rnode, Node* data, Trace& trace) {
      sort(rnode, data, trace);
      size_t count = trace.sorter.size();
      char **p = &trace.sorter.pointers[0];
      
      trace.pop();
      if (trace.free_node(rnode.page, rnode.id))
        rnode.page.defragment(trace);
      
      size_t tsize = trace.size();
      
      for(size_t i = 0; i < count; i++, p++) {
        char *key, *value;
        psize_t ksize;
        vsize_t vsize;
        Bucket::get_key_value(*p, &key, &ksize, &value, &vsize);
        
        trace.cut_key();
        trace.key.append(key, ksize);
        trace.current().find(trace); 
        trace.current().add(TempLeaf(Slice(value, vsize)), false, trace); 
        trace.resize(tsize);
      }

      PageRef *dst = trace.sorter.hash_pages;
      for(size_t i = 0; i < HASH_PAGE_COUNT; i++, dst++)
        trace.map.free_page(*dst);

      trace.sorter.clear();
    }
  //@+node:kochelmonster-.20150117183203.29: *4* remove_bucket
  size_t remove_bucket(char *bucket, size_t slot_size, Trace& trace) {
      char *p1 = trace.sorter.pointers[trace.sorter.index];
      char *p2 = Bucket::next(p1);
      memmove(p1, p2, slot_size - (p2 - bucket));
      return p2 - p1;
    }
  //@+node:kochelmonster-.20150117183203.30: *4* add
  bool add(const TempNode& leaf, bool with_buckets, NodeRef& rnode, 
           Node* data, Trace& trace) {
      Slice key(trace.current_key());
      bool result = true;
      size_t slotid, bucket_size = 0, slot_size = 0, count = 0;
      HashPage *page;
      char *slot = get_slot(&page, &slotid, trace);
      
      if (slot) {
        count = *slot;
        bucket_size = ((Bucket*)(slot+1))->size(count) + 1; // +1 = count byte
        slot_size = pad<BUCKET_ALIGN>(bucket_size);
      }
      
      if (trace.sorter.is_valid(key)) {
        // the key is inside the hash
        remove_bucket(slot+1, slot_size, trace);
        result = false;
      }

      if (!page) {
        PageRef new_page = trace.storage.new_page();
        size_t page_index = trace.sorter.hash / PAGE_HASH_SIZE;
        trace.sorter.hash_pages[page_index] = new_page;
        data->h.pageids[page_index] = new_page.id;
        page = (HashPage*)new_page.page;
        page->init();
      }
      
      size_t kv_size = Bucket::needed_size(key.size(), leaf.len());
      
      if (! slot) {
        bucket_size = kv_size+1;
        slot_size = pad<BUCKET_ALIGN>(bucket_size);
        slot = page->new_slot(slotid, slot_size);
      }
      else {
        size_t new_slot_size = pad<BUCKET_ALIGN>(bucket_size + kv_size);
        slot = page->grow_slot(slot, slot_size, new_slot_size); 
      }
      
      if (!slot) {
        // hash overflow
        burst(leaf, rnode, data, trace);
        return result;
      }
       
      char *p = slot+1;
      // go to end of 
      for(psize_t i = 0; i < count; i++)
        p = Bucket::next(p);
        
      Bucket::set_key_value(
        p, key.data(), key.size(), leaf.node()->l.data, leaf.len());
        
      (*slot)++;
      page->count++;

      trace.sorter.clear();
      return result;
    }
  //@+node:kochelmonster-.20150117183203.28: *4* change_to_bucket
  void change_to_bucket(NodeRef& rnode, Node* data, Trace& trace) {
      sort(rnode, data, trace);
      size_t count = trace.sorter.size();

      // copy the pointers beause bucket will use the sorter
      PageRef pages[HASH_PAGE_COUNT];
      PageRef *dst = pages, *src = trace.sorter.hash_pages;
      for(size_t i = 0; i < HASH_PAGE_COUNT; i++)
        *dst++ = *src++;
      
      char *pointers[8];
      memcpy(pointers, &trace.sorter.pointers[0], 8*sizeof(char*));
      
      trace.sorter.clear();
      trace.change_node(TempBucket());
      
      char **p = pointers;
      for(size_t i = 0; i < count; i++, p++) {
        char *key, *value;
        psize_t ksize;
        vsize_t vsize;
        Bucket::get_key_value(*p, &key, &ksize, &value, &vsize);
        
        trace.cut_key();
        trace.key.append(key, ksize);
        trace.current().find(trace);
        trace.current().add(TempLeaf(Slice(value, vsize)), true, trace);
      }

      PageRef null;
      dst = pages;
      for(size_t i = 0; i < HASH_PAGE_COUNT; i++, dst++) {
        trace.map.free_page(*dst);
        *dst = null;
      }
    }
  //@+node:kochelmonster-.20150117183203.31: *4* remove_child
  bool remove_child(NodeRef& rnode, Node* data, Trace& trace) {
      size_t slotid, count, bucket_size, slot_size;
      HashPage *page;
      char *slot = get_slot(&page, &slotid, trace);
      count = *slot;
      bucket_size = ((Bucket*)(slot+1))->size(count) + 1; // +1 = count byte
      slot_size = pad<BUCKET_ALIGN>(bucket_size);
      
      if (count == 1) {
        page->remove_slot(slotid, slot_size);
      }
      else {
        size_t delta = remove_bucket(slot+1, slot_size, trace);
        size_t new_slot_size = pad<BUCKET_ALIGN>(bucket_size-delta);
        slot = page->grow_slot(slot, slot_size, new_slot_size);
        (*slot)--;
      }
      
      page->count--;
      if (page->count == 0) {
        size_t page_index = trace.sorter.hash / PAGE_HASH_SIZE;
        data->h.pageids[page_index] = 0;
        trace.map.free_page(trace.sorter.hash_pages[page_index]);
        trace.sorter.hash_pages[page_index] = PageRef();
      }
      
      if (calc_node_count(trace) <= 8)
        change_to_bucket(rnode, data, trace);
      
      return true;
    }
  //@+node:kochelmonster-.20150117183203.32: *4* eat_child
  bool eat_child(NodeRef& rnode, Node* data) {
      return false;
    }
  //@+node:kochelmonster-.20150117183203.33: *4* dump
  #ifdef DEBUG
  void dump(Page* page, nodeid_t nodeid, std::ostream& out) {
    const char* t3 = "            ";
    out << t3 << "type:   hash" << std::endl;
  }
  #endif
  //@-others
};

static HashHandler hash;
//@-others

NodeHandler* NodeHandler::handlers[7] = {
  &leaf,
  &hash,
  &bucket,
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

TempBucket::TempBucket() {
  _type = kBucket;
  _node = (Node*)_data;
  _size = BUCKET_ALIGN;
  set_extra(0);
}

TempHash::TempHash() {
  _type = kHash;
  _node = (Node*)_pageids;
  _size = sizeof(Hash);
  set_extra(0);
  memset(node(), 0, sizeof(Hash));
}
//@+node:michael.20150118002311.7: ** Sorter
int compare_slot(const char *a, const char* b) {
  KeyValueSize kvsa = *(KeyValueSize*)a;
  KeyValueSize kvsb = *(KeyValueSize*)b;
  a += sizeof(KeyValueSize);
  b += sizeof(KeyValueSize);
  
  int result = memcmp(a, b, std::min(kvsa.key_size, kvsb.key_size));
  if (result == 0)
    return kvsa.key_size < kvsb.key_size;

  return result < 0;
}

void Sorter::init_hash(Node* data, pageid_t pageids[HASH_PAGE_COUNT],
                       Trace& trace) {
  if (current_data == data)
    return;
    
  current_data = data;
  pageid_t *p = pageids;
  PageRef *dst = hash_pages;
  for(size_t i = 0; i < HASH_PAGE_COUNT; i++, p++, dst++) {
    if (*p)
      *dst = trace.map.get_page(*p);
  }
}

void Sorter::sort() {
  index = 0;
  std::sort(pointers.begin(), pointers.end(), compare_slot);
}

bool Sorter::is_valid(const Slice& key) const {
  char *key_, *p = pointers[index];
  psize_t ksize;
  
  if (!p)
    return false;
    
  Bucket::get_key(p, &key_, &ksize);
  return ksize == key.size() && memcmp(key.data(), key_, ksize) == 0;
}

Slice Sorter::get_value() const {
    vsize_t vsize;
    char *value = Bucket::value(pointers[index], &vsize);
    return Slice(value, vsize);
}

size_t Sorter::find(const Slice& key) {
  char key_buffer[MAX_KEY_SIZE_64+sizeof(KeyValueSize)];
  ((KeyValueSize*)key_buffer)->key_size = key.size();
  memcpy(key_buffer+sizeof(KeyValueSize), key.data(), key.size());
  index = std::lower_bound(pointers.begin(), pointers.end(), 
                           key_buffer, compare_slot) - pointers.begin();
  return index;
}

void Sorter::add_key_to_trace(Trace& trace) {
  char *key;
  psize_t size;
  Bucket::get_key(pointers[index], &key, &size);
  trace.cut_key();
  trace.key.append(key, size);
}

void Sorter::next(Trace& trace) {
  Slice key(trace.current_key());
  if (is_valid(key)) {
    if (index == size()-1) {
      trace.parent_next();
      return;
    }
    index++;
  } 
    // after the last item
  else if (find(key) >= size()) {
    trace.parent_next();
    return;
  }
  trace.cut_key();
  add_key_to_trace(trace);
}
    
void Sorter::prev(Trace& trace) {
  Slice key(trace.current_key());
  if (is_valid(key)) {
    if (index == 0) {
      trace.parent_prev();
      return;
    }
    index--;
  } 
  else {
    find(key);
    if (index == 0) {
      // before the first item
      trace.parent_prev();
      return;
    }
    index--;
  }
  trace.cut_key();
  add_key_to_trace(trace);
}

//@+node:michael.20150118002311.21: ** PageRef
//@+node:michael.20150118002311.20: *3* dump
#ifdef DEBUG
void PageRef::dump(std::ostream& out) {
  const char* t1 = "    ";
  const char* t2 = "      ";
  const char* t3 = "          ";
  out << t1 << "- id:         " << id << std::endl
      << t2 << "offset:     " << offset << std::endl;
      
  if (page->type == kTriePage) {
    out << t2 << "node_count: " << count() << std::endl
        << t2 << "size:       " << size() << std::endl
        << t2 << "free_size:  " << free_size() << std::endl
        << t2 << "sum_size:   " << size() + free_size() << std::endl
        << t2 << "nodes: " << std::endl;
  
    for(size_t id = 0; id < count(); id++) {
      NodePtr *ptr = page->node_ptr + id;
      out << t3 << "- id:    " << (int)id << std::endl
          << t3 << "  ptr:   " << (int)ptr->offset << std::endl;
          
      NodeHandler::handlers[ptr->type]->dump(page, id, out);
    }
  }
  else {
    HashPage* hpage = (HashPage*)page;
    out << t2 << "node_count: " << hpage->count << std::endl
        << t2 << "size:       " << sizeof(Page)-hpage->end << std::endl
        << t2 << "free_size:  " << hpage->end-HASHPAGE_HEADER << std::endl
        << t2 << "nodes: " << std::endl;
        
    for(size_t id = 0; id < PAGE_HASH_SIZE; id++) {
      char *slot = hpage->get_slot(id);
      if (slot) {
        size_t count = *slot++;
        out << t3 << "- id:    " << (int)id << std::endl
            << t3 << "  type:  bucket" << std::endl
            << t3 << "  count:  " << count << std::endl;
        
        char *key, *value;
        psize_t ksize;
        vsize_t vsize;
        for(psize_t i = 0; i < count; i++) {
          slot = Bucket::get_key_value(slot, &key, &ksize, &value, &vsize);
          
          out << t3 << "  key" << (int)i << ":   ";
          dump_key(key, ksize, out);
          out << std::endl;
          
          std::string val(value, vsize&0x3f);
          out << t3 << "  value" << (int)i << ": " << val.c_str() << std::endl;
        }
      }
    }
  }
}
#endif
//@-others
} // namespace larch_leaves 
//@-leo
