// Trie Nodes
#ifndef _LEAVES__NODE_HPP
#define _LEAVES__NODE_HPP

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

/*
Generally the Tries are a smart version of a compressed trie
(https://www.geeksforgeeks.org/compressed-tries/)


A node is composed of several parts

one UpperBranchNode
(Saves a compressed, null_leaf and the upper 3bits of key)

0 to 8 LowerBranchNode saving up to 32 links to Leaves and other
UpperBranchNodes

Every LowerBranchNode has a LeafNode



UpperBranch Node
================

Memory Layout:

The layout is

------------------------
BlockHeader
------------------------
hash?
------------------------
compressed:size,data
------------------------
Link LowerBranch 1
------------------------
Link LowerBranch 2
------------------------
....
-----------------------
Null Leaf
------------------------


LowerBranch Node
================
------------------------
BlockHeader
------------------------
hash?
------------------------
leaves meta data
-----------------------
Link 1
-----------------------
Link 2
-----------------------
....
-----------------------
Link N
-----------------------



Example:
-------------------------
[thequick]  <- compressed
-------------------------
b|g|r|y    <- branches
-------------------------
Link for b (A)
-------------------------
Link for g (B)
-------------------------
Link for r (C)
-------------------------
Link for y (D)
-------------------------
Link for None Branch (E)
-------------------------

the key: thequickb -> follows Link A
the key: thequickr -> follows Link C
the key: thequick -> follows Link E



Leaf Node
=========

Leaf Node aggregates all leafes of a branch node
their layout is:

----------------
BlockHeader
----------------
leaf node header
----------------
leaf node 1:
key_size
value_size
key_value_data
----------------
leaf node 2:
key_size
value_size
key_value_data
----------------

Leaf Nodes

  - [L] The rest key (suffix) and value.

  A block can contain multiple leaf nodes. Leaf nodes grow from the end
  of the block to the start. In this way, branch nodes can grow without
  moving leaf nodes.

A block that contains branch nodes is called branch block in contrary
to a leaf block that only contains leaf nodes and is associated to a
branch block. (Be aware a branch block can also contain leaf nodes!)

a branch block can have size from 256 to to 4096 Bytes.
leaf blocks can be up to 4G.

The layout is CPU cache friendly
*/

template <typename BlockHeader>
struct _Compressed {
  using bsize_t = typename BlockHeader::bsize_t;

  bsize_t size;
  uint8_t key[0];

  template <typename Cursor>
  bool find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    size_t same = get_prefix(cursor.rest_key.data(), (const char*)key,
                             cursor.rest_key.size(), size, back.cmp);
    back.prefix = same;
    cursor._advance_key(same);
    return same == size;
  }

  template <typename Cursor>
  uint16_t step(Cursor& cursor) const {  // for first, next, prev, last
    cursor.stack.back().prefix = size;
    cursor.current_key.append((char*)key, size);
    return nodesize();
  }

  static bsize_t nodesize(bsize_t s) {
    return s ? align(sizeof(_Compressed) + s) : 0;
  }

  bsize_t nodesize() const { return nodesize(size); }
};

// A leaf of the trie (a rest key and the value)
template <typename BlockHeader>
struct _Leaf {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;

  static const size_t MAX_LEAF_SIZE = 2048;
  /* BIG_VALUES are two time defered
    -> Normal Leaf with an offset as value
    -> pure value data in extra allocated block (with no header?) */

  hash_t hash;
  bsize_t value_size;
  bsize_t key_size;  // actually we only need 2 bytes but we align to 8
  uint8_t key_value[0];

  template <typename Cursor>
  void find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.found_leaf = this;
    size_t same = get_prefix(cursor.rest_key.data(), (const char*)key_value,
                             cursor.rest_key.size(), key_size, back.cmp);
    back.suffix = same;
    cursor._advance_key(back.suffix);
  }

  template <typename Cursor>
  void step(Cursor& cursor) const {  // for first, next, prev, last
    typename Cursor::Transition& back = cursor.stack.back();
    back.found_leaf = this;
    cursor.current_key.append((char*)key_value, key_size);
    back.suffix = key_size;
    back.cmp = 0;
  }

  uint8_t* pvalue() { return key_value + align(key_size); }

  Slice value() const {
    return Slice((char*)key_value + align(key_size), value_size);
  }

  Slice key() const { return Slice((char*)key_value, key_size); }

  bsize_t fill(const Slice& value) {
    key_size = 0;
    value_size = value.size();
    memcpy(key_value, value.data(), value_size);
    return nodesize();
  }

  bsize_t fill(const Slice& key, const Slice& value) {
    key_size = key.size();
    value_size = value.size();
    memcpy(key_value, key.data(), key_size);
    memcpy(pvalue(), value.data(), value_size);
    return nodesize();
  }

  static bsize_t nodesize(bsize_t ksize, size_t vsize) {
    bsize_t tmp = sizeof(_Leaf) + align(ksize) + align(vsize);
    return tmp <= MAX_LEAF_SIZE
               ? tmp
               : sizeof(_Leaf) + align(ksize) + sizeof(offset_t);
  }

  bsize_t nodesize() const { return nodesize(key_size, value_size); }
};

template <typename BlockHeader>
struct _LeafNode : public BlockHeader {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;
  typedef _Leaf<BlockHeader> Leaf;
  typedef _LeafNode<BlockHeader> LeafNode;
  typedef typename BlockHeader::template Pointer<LeafNode> ptr;

  uint8_t data[0];

  Leaf* leaf(const offset_t& ptr) {
    return (Leaf*)&data[(uint64_t)ptr & (uint64_t)~1];
  }

  const Leaf* leaf(const offset_t& ptr) const {
    return (Leaf*)&data[(uint64_t)ptr & (uint64_t)~1];
  }

  offset_t olink(const Leaf* leaf) const { return (uint8_t*)leaf - data + 1; }

  template <typename Storage>
  static ptr alloc(bsize_t size, Storage& storage) {
    static_assert(std::is_class<Storage>::value,
                  "Storage must be a class type");
    return storage.alloc(size + sizeof(LeafNode));
  }
};

template <typename Cursor, typename Leaf>
void find_(Cursor& cursor, const Leaf* leaf) {
  leaf->find(cursor);
}

template <typename Cursor, typename Leaf>
void step_(Cursor& cursor, const Leaf* leaf) {
  leaf->step(cursor);
}

inline bool isleaf(uint64_t offset) { return offset & 1 == 1; };

inline uint64_t setleaf(uint64_t offset) { return offset + 1; }

template <typename BlockHeader>
struct _LowerBranchNode : public BlockHeader {
  static const uint8_t MASK = 0b00011111;
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;
  using LeafNode = _LeafNode<BlockHeader>;
  using leaf_ptr = typename LeafNode::ptr;
  using BlockHeader::bits;
  typedef _LowerBranchNode<BlockHeader> LowerBranchNode;
  typedef _Leaf<BlockHeader> Leaf;

  hash_t hash;
  offset_t leaves;      // link to the associated leaf block
  bsize_t leaves_used;  // space used in leaves
  bsize_t leaves_free;  // free holes in leaves
  offset_t links[0];

  typedef typename BlockHeader::template Pointer<LowerBranchNode> ptr;

  static uint8_t lower(uint8_t key) { return key & MASK; }

  bool isset(uint8_t key) const { return bits::isset(bits, lower(key)); }
  void set(uint8_t key) { return bits::set(bits, lower(key)); }
  int count() const { return bits::count(BlockHeader::bits); }
  int index(uint8_t key) const { return bits::index(bits, lower(key)); }
  int first() const { return bits::first(bits); }
  int last() const { return bits::last(bits); }
  int next(uint8_t key) const { return bits::next(bits, lower(key)); }
  int prev(uint8_t key) const { return bits::prev(bits, lower(key)); }

  offset_t* link(uint8_t key) { return &links[index(key)]; }

  template <typename Cursor>
  bool find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    if (!isset(back.branch_key)) {
      back.cmp = back.NOT_FOUND;
      return false;
    }
    cursor._advance_key(1);
    int index_ = index(back.branch_key);
    return back.follow_link(cursor, index_, find_<Cursor, Leaf>);
  }

  template <typename Cursor>
  bool first(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    int key = first();
    assert(key >= 0);
    back.branch_key = (back.branch_key & ~MASK) | key;
    cursor.current_key.push_back(back.branch_key);
    return back.follow_link(cursor, index(key), step_<Cursor, Leaf>);
  }

  template <typename Cursor>
  bool last(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    int key = last();
    assert(key >= 0);
    back.branch_key = (back.branch_key & ~MASK) | key;
    cursor.current_key.push_back(back.branch_key);
    return back.follow_link(cursor, index(key), step_<Cursor, Leaf>);
  }

  template <typename Cursor>
  bool next(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    int key = next(back.branch_key);
    if (key < 0) return false;
    back.branch_key = (back.branch_key & ~MASK) | key;
    cursor.current_key.push_back(back.branch_key);
    return back.follow_link(cursor, index(key), step_<Cursor, Leaf>);
  }

  template <typename Cursor>
  bool prev(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    int key = prev(back.branch_key);
    if (key < 0) return false;
    back.branch_key = (back.branch_key & ~MASK) | key;
    cursor.current_key.push_back(back.branch_key);
    return back.follow_link(cursor, index(key), step_<Cursor, Leaf>);
  }

  template <typename Storage>
  static ptr alloc(bsize_t count, Storage& storage) {
    ptr result =
        storage.alloc(count * sizeof(offset_t) + sizeof(LowerBranchNode));
    result->size = 0;
    result->leaves = 0;
    result->leaves_used = 0;
    result->leaves_free = 0;
    return result;
  }

  void copy_leaf(leaf_ptr dest, const leaf_ptr src) {
    if (leaves_free) {
      bsize_t ls = 0;
      for (int i = 0, c = count(); i < c; i++) {
        offset_t& offset = links[i];
        if (isleaf(offset)) {
          const Leaf* l = src->leaf(offset);
          memcpy(&dest->data[ls], l, l->nodesize());
          offset = setleaf(ls);
          ls += l->nodesize();
        }
      }
      leaves_used = ls;
      leaves_free = 0;
      return;
    }

    memcpy(dest->data, src->data, leaves_used);
  }

  bsize_t space() const { return count() * sizeof(offset_t); }
  bsize_t freespace() const {
    return BlockHeader::block_size - sizeof(LowerBranchNode) - space();
  }
};

/* the metadata of a every block */
template <typename BlockHeader>
struct _UpperBranchNode : public BlockHeader {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;
  using BlockHeader::b;
  typedef _Compressed<BlockHeader> Compressed;
  typedef _UpperBranchNode<BlockHeader> UpperBranchNode;
  typedef _LowerBranchNode<BlockHeader> LowerBranchNode;
  typedef _Leaf<BlockHeader> Leaf;
  typedef typename BlockHeader::template Pointer<UpperBranchNode> ptr;
  using lbranch_ptr = typename LowerBranchNode::ptr;

  /*
  bit layout of bits
  struct {
    bsize_t has_compressed : 1;
    bsize_t has_null_leaf : 1;
  };*/
  static const uint16_t COMPRESSED = (uint16_t)2;
  static const uint16_t NULL_LEAF = (uint16_t)1;
  static const uint16_t BITS = COMPRESSED | NULL_LEAF;
  static const uint16_t SPACE = ~BITS;

  hash_t hash;
  uint8_t data[0];

  bool has_compressed() const { return b.features & COMPRESSED; }
  bool has_null_leaf() const { return b.features & NULL_LEAF; }
  bool has_branch() const { return b.bits != 0; }

  void set_compressed() { b.features |= COMPRESSED; }
  void set_null_leaf() { b.features |= NULL_LEAF; }

  void clear_compressed() { b.features &= ~COMPRESSED; }
  void clear_null_leaf() { b.features &= ~NULL_LEAF; }

  static uint8_t upper(uint8_t key) { return key >> 5; }
  int count() const { return bits::count(b.bits); }
  bool isset(uint8_t key) const { return bits::isset(b.bits, upper(key)); }
  void set(uint8_t key) { return bits::set(b.bits, upper(key)); }
  int index(uint8_t key) const { return bits::index(b.bits, upper(key)); }
  int first() const { return bits::first(b.bits); }
  int last() const { return bits::last(b.bits); }
  int next(uint8_t key) const { return bits::next(b.bits, upper(key)); }
  int prev(uint8_t key) const { return bits::prev(b.bits, upper(key)); }

  template <typename Storage>
  static ptr alloc(bsize_t size, Storage& storage) {
    ptr result = storage.alloc(size + sizeof(UpperBranchNode));
    result->size = 0;
    return result;
  }

  Compressed* compressed() { return (Compressed*)data; }
  const Compressed* compressed() const { return (const Compressed*)data; }
  void* end() { return (void*)&data[BlockHeader::b.used]; }

  // calculate the offset of a link pointer
  bsize_t olink(const offset_t* link) const { return (uint8_t*)link - data; }
  offset_t* plink(bsize_t offset) { return (offset_t*)&data[offset]; }
  bsize_t space() const { return b.used; }
  size_t freespace() const {
    return BlockHeader::block_size - sizeof(UpperBranchNode) - space();
  }

  static bsize_t branchsize(int links) {
    return padding(links * sizeof(offset_t), 32);
  }
  bsize_t branchsize() const { return branchsize(count()); }

  template <typename Cursor, typename Transition>
  lbranch_ptr resolve(Cursor& cursor, Transition& t, bsize_t ioffset,
                      int index) const {
    offset_t* link = (offset_t*)&data[ioffset] + index;
    t.uolink = olink(link);
    t.lbranch = cursor.storage.resolve(*link);
    return t.lbranch;
  }

  // find the next chunk of the key and update the stack
  template <typename Cursor>
  bool find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint16_t ioffset = 0;

    if (has_compressed()) {
      back.compressed = (const Compressed*)&data[ioffset];
      if (!back.compressed->find(cursor)) return false;
      ioffset += back.compressed->nodesize();
    }

    if (has_branch()) {
      if (cursor.rest_key.size()) {
        back.branch_key = cursor.rest_key[0];
        assert(b.bits);
        if (isset(back.branch_key)) {
          int index_ = index(back.branch_key);
          return resolve(cursor, back, ioffset, index_)->find(cursor);
        }
        back.cmp = back.NOT_FOUND;
        back.lbranch = nullptr;
        back.leaf = nullptr;
        return false;
      }
      ioffset += branchsize();
    }

    if (has_null_leaf() && cursor.rest_key.empty()) {
      back.found_leaf = (Leaf*)&data[ioffset];
      back.cmp = 0;
      back.lbranch = nullptr;
      back.leaf = nullptr;
      return false;
    }

    back.cmp = 1;
    return false;
  }

  template <typename Cursor>
  bool first(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();

    uint16_t ioffset = 0;
    if (has_compressed()) {
      back.compressed = ((const Compressed*)&data[ioffset]);
      ioffset += back.compressed->step(cursor);
    }

    if (has_null_leaf()) {
      ioffset += branchsize();
      back.found_leaf = (Leaf*)&data[ioffset];
      back.found_leaf->step(cursor);
      back.cmp = 0;
      return false;
    }

    assert(has_branch());
    assert(b.bits);
    int key = first();
    assert(key >= 0);
    back.branch_key = key << 5;
    return resolve(cursor, back, ioffset, 0)->first(cursor);
  }

  template <typename Cursor>
  bool last(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();

    uint16_t ioffset = 0;
    if (has_compressed()) {
      back.compressed = ((const Compressed*)&data[ioffset]);
      ioffset += back.compressed->step(cursor);
    }

    if (has_branch()) {
      int key = last();
      assert(key >= 0);
      back.branch_key = key << 5;
      return resolve(cursor, back, ioffset, index(back.branch_key))
          ->last(cursor);
    }

    assert(has_null_leaf());
    back.found_leaf = (Leaf*)&data[ioffset];
    back.found_leaf->step(cursor);
    back.cmp = 0;
    return false;
  }

  template <typename Cursor>
  bool next(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint16_t ioffset = 0;

    if (back.found_leaf) {
      if (back.cmp < 0) {
        // the found leaf the next one
        cursor.current_key.resize(cursor.current_key.size() - back.suffix);
        back.found_leaf->step(cursor);
        return false;
      }
      back.found_leaf = nullptr;
    }

    if (back.compressed) {
      ioffset += back.compressed->nodesize();
      if (back.prefix != back.compressed->size) {
        assert(back.prefix < back.compressed->size);
        if (back.cmp > 0) {
          // behind this node
          cursor._pop();
          return true;
        }

        assert(back.cmp < 0);
        cursor.current_key.resize(back.keypos);
        return first(cursor);
      }
    }

    if (has_branch()) {
      assert(b.bits);
      if (!back.lbranch) {
        // the last cursor pos was the null leaf
        assert(has_null_leaf());
        back.branch_key = first() << 5;
        return resolve(cursor, back, ioffset, 0)->first(cursor);
      }

      back.resize_key_to_branch(cursor);
      back.cmp = 1;
      if (back.lbranch->next(cursor)) return true;
      if (back.cmp == 0) return false;

      // LowerBranch after end
      int key = next(back.branch_key);
      if (key >= 0) {
        back.branch_key = key << 5;
        return resolve(cursor, back, ioffset, index(back.branch_key))
            ->first(cursor);
      }
    }

    cursor._pop();
    return true;
  }

  template <typename Cursor>
  bool prev(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint16_t ioffset = 0;

    if (back.found_leaf) {
      if (!back.lbranch && back.cmp == 0) {
        // the last cursor pos was the null leaf
        assert(has_null_leaf());
        cursor._pop();
        return true;
      }
      if (back.cmp > 0) {
        cursor.current_key.resize(cursor.current_key.size() - back.suffix);
        back.found_leaf->step(cursor);
        return false;
      }

      back.found_leaf = nullptr;
    }

    if (back.compressed) {
      ioffset += back.compressed->nodesize();
      if (back.prefix != back.compressed->size) {
        if (back.cmp < 0) {
          cursor._pop();
          return true;
        }

        assert(back.cmp > 0);
        cursor.current_key.resize(back.keypos);
        return last(cursor);
      }
    }

    if (has_branch()) {
      assert(b.bits);

      if (back.lbranch) {
        back.resize_key_to_branch(cursor);
        back.cmp = -1;
        if (back.lbranch->prev(cursor)) return true;
        if (back.cmp == 0) return false;
        // LowerBranch before beginning
      }

      int key = prev(back.branch_key);
      if (key >= 0) {
        back.branch_key = key << 5;
        return resolve(cursor, back, ioffset, index(back.branch_key))
            ->last(cursor);
      }

      ioffset += branchsize();
    }

    if (has_null_leaf()) {
      back.found_leaf = (Leaf*)&data[ioffset];
      back.found_leaf->step(cursor);
      back.lbranch = nullptr;
      back.leaf = nullptr;
      back.cmp = 0;
      return false;
    }

    cursor._pop();
    return true;
  }

  template <typename OP, typename Storage>
  void iterate_links(Storage& storage, OP oper) {
    bsize_t ioffset = 0;
    if (has_compressed()) ioffset = compressed()->nodesize();

    bsize_t boffset = ioffset;
    ioffset += branchsize();

    if (has_null_leaf()) oper(nullptr, *plink(ioffset));

    if (has_branch()) {
      offset_t* links = (offset_t*)&data[boffset];
      for (int count = bits::count(b.bits), i = 0; i < count; i++) {
        lbranch_ptr lbranch = storage.resolve(links[i]);
        for (int icount = bits::count(lbranch->bits), j = 0; j < icount; j++) {
          oper(lbranch, lbranch->links[j]);
        }
      }
    }
  }

  void add_compressed(const Slice& src) {
    if (src.size()) {
      Compressed* cn = compressed();
      cn->size = src.size();
      memcpy(cn->key, src.data(), cn->size);
      set_compressed();
      b.used = cn->nodesize();
    }
  }

  void check() const {
#if defined(DEBUG) && !defined(NDEBUG)
    assert(BlockHeader::b.used + sizeof(UpperBranchNode) <=
           BlockHeader::block_size);
    bsize_t ioffset = 0;
    if (has_compressed()) {
      const Compressed* n = compressed();
      ioffset += n->nodesize();
    }

    ioffset += branchsize();

    if (has_null_leaf()) {
      const Leaf* leaf = (const Leaf*)&data[ioffset];
      ioffset += leaf->nodesize();
    }

    assert(ioffset == BlockHeader::b.used);
    assert(ioffset > 0);
#endif
  }
};

#ifdef DEBUG

inline std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit) && bit != '"' && bit != '<' && bit != '>' && bit != ']' &&
      bit != '\\' && bit != '}' && bit != '{') {
    cstr << bit;
  } else {
    cstr << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
  return cstr.str();
}

template <typename Storage>
struct _Dumper {
  using offset_t = typename Storage::offset_t;
  using block_ptr = typename Storage::block_ptr;
  using bsize_t = typename Storage::bsize_t;
  typedef _UpperBranchNode<typename Storage::BlockHeader> UpperBranchNode;
  typedef _LowerBranchNode<typename Storage::BlockHeader> LowerBranchNode;
  typedef _LeafNode<typename Storage::BlockHeader> LeafNode;
  using ubranch_ptr = typename UpperBranchNode::ptr;
  using lbranch_ptr = typename LowerBranchNode::ptr;
  using leaf_ptr = typename LeafNode::ptr;
  using Leaf = typename UpperBranchNode::Leaf;
  using Compressed = typename UpperBranchNode::Compressed;

  static void dump_leaf(std::ostream& out, const Leaf* leaf, Storage* storage) {
    out << "type: leaf" << std::endl;
    out << "keysize: " << leaf->key_size << std::endl;
    out << "key: \"";
    for (int i = 0; i < leaf->key_size; i++) {
      out << "[" << bitstr(leaf->key_value[i]) << "]";
    }
    out << "\"" << std::endl;

    out << "valuesize: " << leaf->value_size << std::endl;
    out << "value: \"";
    int delta = align(leaf->key_size);
    for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)10);
         i < end; i++) {
      out << "[" << bitstr(leaf->key_value[i + delta]) << "]";
    }
    out << "\"" << std::endl;
    out << "---" << std::endl;
  }

  static void dump_leaf(std::ostream& out, const lbranch_ptr& parent,
                        offset_t leaves, offset_t link, Storage* storage) {
    leaf_ptr block = storage->resolve(leaves);
    int lspace = block->block_size - sizeof(LeafNode) - parent->leaves_used -
                 parent->leaves_free;
    out << "id: " << leaves + link << std::endl;
    out << "block: " << leaves << std::endl;
    out << "size: " << block->block_size << std::endl;
    out << "space: " << parent->leaves_used << std::endl;
    out << "freespace: " << lspace << std::endl;
    out << "frag: " << parent->leaves_free << std::endl;
    dump_leaf(out, block->leaf(link), storage);
  }

  static void dump_link(std::ostream& out, const lbranch_ptr& parent,
                        offset_t leaves, offset_t link, Storage* storage) {
    if (isleaf(link))
      dump_leaf(out, parent, leaves, link, storage);
    else
      dump_branch(out, link, storage);
  }

  static void dump_lbranch(std::ostream& out, offset_t offset, uint8_t key,
                           Storage* storage) {
    lbranch_ptr block = storage->resolve(offset);
    leaf_ptr leaf = storage->resolve(block->leaves);
    out << "id: " << offset << std::endl;
    out << "block: " << offset << std::endl;
    out << "size: " << block->block_size << std::endl;
    out << "space: " << block->space() << std::endl;
    out << "freespace: " << block->freespace() << std::endl;
    out << "type: lbranch" << std::endl;

    using namespace bits;
    auto lbits = block->bits;
    out << "key: \"";
    for (int li = first(lbits); li != -1; li = next(lbits, li)) {
      out << "[" << bitstr(key + li) << "]";
    }
    out << "\"" << std::endl;

    out << "children: " << std::endl;
    int count = bits::count(lbits);
    for (int i = 0; i < count; i++) {
      uint64_t id = block->links[i];
      if (isleaf(id)) id += block->leaves;
      out << "  - " << id << std::endl;
    }
    out << "---" << std::endl;

    for (int i = 0; i < count; i++) {
      dump_link(out, block, block->leaves, block->links[i], storage);
    }
  }

  static void dump_branch(std::ostream& out, offset_t offset,
                          Storage* storage) {
    ubranch_ptr block = storage->resolve(offset);
    out << "id: " << offset << std::endl;
    out << "block: " << offset << std::endl;
    out << "size: " << block->block_size << std::endl;
    out << "space: " << block->b.used << std::endl;
    out << "freespace: " << block->freespace() << std::endl;
    out << "type: ubranch" << std::endl;

    bsize_t ioffset = 0;
    if (block->has_compressed()) {
      const Compressed* n = (const Compressed*)&block->data[ioffset];
      out << "compressed: " << std::endl;
      out << "  size: " << (int)n->size << std::endl;
      out << "  key: \"";
      for (int i = 0; i < n->size; i++) {
        out << "[" << bitstr(n->key[i]) << "]";
      }
      out << "\"" << std::endl;
      ioffset += n->nodesize();
    }

    using namespace bits;
    auto ubits = block->b.bits;
    int count = bits::count(ubits);
    offset_t* links = (offset_t*)&block->data[ioffset];
    out << "key: \"";
    for (int ui = first(ubits); ui != -1; ui = next(ubits, ui)) {
      out << "[" << (ui << 5) << "]";
    }
    out << "\"" << std::endl;

    if (count) {
      out << "children: " << std::endl;
      for (int i = 0; i < count; i++) {
        assert(!isleaf(links[i]));
        out << "  - " << links[i] << std::endl;
      }
    }

    ioffset += block->branchsize();
    if (block->has_null_leaf()) {
      out << "nulllink: " << offset + ioffset + 1 << std::endl;
      out << "---" << std::endl;
      const Leaf* leaf = (const Leaf*)&block->data[ioffset];
      out << "id: " << offset + ioffset + 1 << std::endl;
      out << "block: " << offset << std::endl;
      out << "size: " << block->block_size << std::endl;
      out << "space: " << block->b.used << std::endl;
      out << "freespace: " << block->freespace() << std::endl;
      dump_leaf(out, leaf, storage);
    } else
      out << "---" << std::endl;

    for (int ui = first(ubits), i = 0; i < count; ui = next(ubits, ui)) {
      dump_lbranch(out, links[i++], ui << 5, storage);
    }
  }
};
#endif
}  // namespace leaves

#endif  // _LEAVES__NODE_HPP
