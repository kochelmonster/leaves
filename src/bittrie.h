#include <stdint.h>
/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/

struct NodeRef {
  byte_t type;
  void* ptr;
};



// Interface for NodeRefs
struct NodeHandler {
  // returns a 16 byte aligned size
  virtual size_t size() const = 0;
  
  // returns true if the node is a leaf
  virtual bool is_leaf() const = 0;
  
  // get the child index by key (signed and unsigned version)
  virtual byte get_child_index(unsigned key) const;
  virtual byte get_child_index(char key) const;
  
  // returns the next node of child index NULL if not found
  virtual NodeRef* find(byte_t child_index, Storage& storage) = 0;
  
  // returns the end node of NULL if None there
  virtual NodeRef* find_end(Storage& storage) = 0;

  // adds a new node (this is the parent)
  // returns the Ref to the new parent (if the parent changed)
  virtual NodeHandler* add(byte_t child_index, const Slice& rest_key, 
                           const Slice& value, Storage& storage) = 0;
                           
  virtual NodeHandler* add_end(const Slice& rest_key, 
                               const Slice& value, Storage& storage) = 0;

  // removes a node 
  virtual void remove(byte child_index);
  virtual void remove_end();
};

struct LeafNodeHandler : public NodeHandler {
  bool key_equal(const Slice& rest_key) const = 0;
};

  
// a leaf node with data in Leaf memory
struct PageLeaf {
  size_t segment_no;
  offset_t pointer;
};
// a leaf node wich key and values are <= 256 bytes
struct PageLeaf {
  byte_t key_size;
  byte_t value_size;
  char data[];
};
// links to another page
struct LinkNode {
  pageid_t page;
};
// Node with 256 values
struct Node256 {
  nodeid_t children[256];
};

// Node with 257 values (256 for each byte value + 1 for end)
struct Node257 {
  nodeid_t children[256];
  nodeid_t end;
};
// A Node containing a string part equal to all descendants
struct CompressNode {
  nodeid_t child;
  byte_t size;
  char data[]
};
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


struct BitNode32 {
  boost::uint32_t bits[1];
};

struct BitNode64 {
  boost::uint32_t bits[2];
};

struct BitNode128 {
  boost::uint32_t bits[4];
};

struct BitNode192 {
  boost::uint32_t bits[6];

};

struct BitNode256 {
  boost::uint32_t  bits[8];
};



