//@+leo-ver=5-thin
//@+node:michael.20150101205559.42: * @file test_bits.cpp
//@@language cplusplus
//@@tabwidth -2
//@+<< includes >>
//@+node:michael.20150101205559.43: ** << includes >>
#define BOOST_TEST_MODULE TrieTest
//#define BOOST_TEST_NO_MAIN
#include <boost/test/included/unit_test.hpp>
#include "larch/leaves.h"
#include "node.h"
#include "port.h"
//@-<< includes >>
using namespace larch_leaves;

//@+others
//@+node:michael.20150101205559.45: ** Test Utils
namespace larch_leaves {

void testpoint(const char* str) {
}

}

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
//@+node:michael.20150101205559.44: ** TestSuite
BOOST_AUTO_TEST_CASE(add_bits) {
  union {
    boost::uint64_t data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    BitTrie bits;
  };
  int settings[8] = { 3, 15, 20, 26, 32, 45, 50, 60 };
  
  memset(data, 0, sizeof(data));

  BOOST_REQUIRE_EQUAL(bits.last_bit(), -1);
  BOOST_REQUIRE_EQUAL(bits.first_bit(), -1);
  
  for(int i = 7; i >= 0; i--) 
    bits.add(settings[i], i+1);
  
  int b = bits.first_bit();
  int i = 0;
  while(b >= 0) {
    int ci = bits.get_child_index(b);
    BOOST_REQUIRE_EQUAL(settings[i], b);
    BOOST_REQUIRE_EQUAL(ci, i);
    BOOST_REQUIRE_EQUAL(bits.children[ci], i+1);
    b = bits.next_bit(b);
    i++;
  }
  
  b = bits.last_bit();
  i = 7;
  while(b >= 0) {
    int ci = bits.get_child_index(b);
    BOOST_REQUIRE_EQUAL(settings[i], b);
    BOOST_REQUIRE_EQUAL(ci, i);
    BOOST_REQUIRE_EQUAL(bits.children[ci], i+1);
    b = bits.prev_bit(b);
    i--;
  }
  
  for(int i = 0; i < 8; i++) {
    int b = settings[i];
    int ci = bits.get_child_index(b);
    BOOST_REQUIRE_EQUAL(ci, i);
    BOOST_REQUIRE_EQUAL(bits.children[ci], i+1);
    BOOST_REQUIRE_EQUAL(bits.get_child_index(b-1), -1);
    BOOST_REQUIRE_EQUAL(bits.get_child_index(b+1), -1);
  }
  
  for(int i = 0; i < 8; i++)
    bits.remove(settings[i]);
    
  BOOST_REQUIRE_EQUAL(bits.bits, 0);
}
//@-others

//@-leo
