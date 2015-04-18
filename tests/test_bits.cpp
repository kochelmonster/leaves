#define BOOST_TEST_MODULE TrieTest
//#define BOOST_TEST_NO_MAIN
#include <boost/test/included/unit_test.hpp>
#include "larch/leaves.h"
#include "node.h"
#include "port.h"
using namespace larch_leaves;

namespace larch_leaves {

void testpoint(const char* str) {
}

}

typedef unsigned char trieindex_t;

// A Node containing a string part equal to all descendants
// the equal part fit into a page; the data size in NodeRef::len()
struct Compressed {
  nodeid_t child;
  char data[1];
};
// A leaf 
// the data is interpreted by the type (kLeaf od kBitLeaf)
// the size of data is saved in NodeRef::len()
struct Leaf {
  char data[1];
};
// Node with the complete alphabet 
// children count is > 56
struct Trie {
  nodeid_t children[64];
};
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
struct Node {
  union {
    Compressed c;
    Leaf l;
    Trie t;
    BitTrie b;
    pageid_t p;
  };
};
BOOST_AUTO_TEST_CASE(add_bits) {
  union {
    boost::uint64_t data[8];
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

