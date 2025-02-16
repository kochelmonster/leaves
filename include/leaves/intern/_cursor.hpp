#ifndef _LEAVES__CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include <vector>

#include "_exception.hpp"
#include "_node.hpp"

namespace leaves {

template <typename BlockHeader_>
struct _Transition {
  typedef BlockHeader_ BlockHeader;
  typedef _Transition<BlockHeader> Transition;
  using block_ptr = typename BlockHeader::ptr;
  typedef _UpperBranchNode<BlockHeader> UpperBranchNode;
  typedef _LowerBranchNode<BlockHeader> LowerBranchNode;
  using LeafNode = typename LowerBranchNode::LeafNode;
  using Compressed = typename UpperBranchNode::Compressed;
  using Leaf = typename UpperBranchNode::Leaf;
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using leaf_ptr = typename LeafNode::ptr;
  using ubranch_ptr = typename UpperBranchNode::ptr;
  using lbranch_ptr = typename LowerBranchNode::ptr;

  static const int NOT_FOUND = 2;  // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  ubranch_ptr ubranch;  // the upper branch block referenced in this transition
  lbranch_ptr lbranch;  // the lower branch block referenced in this transition
  leaf_ptr leaf;        // the final leaf block if any

  uint16_t prefix;  // count of equal chars in stringnode
  uint16_t suffix;  // count of equal chars in keyvaluenode
  uint8_t branch_key;

  // the offset inside block that points to the output link
  int uolink;  // upper branch
  int lolink;  // lower branch

  const Compressed* compressed;
  const Leaf* found_leaf;

  // 1: the key to find is bigger than the found node
  // 0: the key is found
  // -1: the key to find is smaller than the found node
  // NOT_FOUND: it is not equal but not known if -1 or 1
  // UNDEFINED: not tested
  int cmp;

  // position inside the key
  bsize_t keypos;

  bool success() const { return found_leaf && cmp == 0; }
  offset_t* uplink() { return ubranch->plink(uolink); }
  offset_t* plink() { return &lbranch->links[lolink]; }
  void reset() {
    ubranch = nullptr;
    lbranch = nullptr;
    leaf = nullptr;
  }

  template <typename Cursor, typename Caller>
  bool follow_link(Cursor& cursor, int index, Caller& c) {
    offset_t link = lbranch->links[index];
    assert(link);

    lolink = index;
    if (isleaf(link)) {
      leaf = cursor.storage.resolve(lbranch->leaves);
      c(cursor, leaf->leaf(link));
      return false;
    }

    cursor.stack.back().cmp = 0;
    cursor._push(link);
    return true;
  }

  template <typename Cursor>
  void resize_key_to_branch(Cursor& cursor) {
    cursor.current_key.resize(keypos + (compressed ? compressed->size : 0));
  }
};

template <typename BlockHeader>
struct _Stack {
  using block_ptr = typename BlockHeader::ptr;
  using bsize_t = typename BlockHeader::bsize_t;
  typedef _Stack<BlockHeader> Stack;
  typedef _Transition<BlockHeader> Transition;
  typedef std::vector<Transition> stack_v;
  stack_v data;
  size_t size;

  _Stack() : size(0) { data.resize(100); }

  void push(block_ptr block, bsize_t keypos = 0) {
    if (size == data.size()) data.resize(size * 2);
    Transition& back = data[size++];
    back.ubranch = block;
    back.lbranch = nullptr;
    back.leaf = nullptr;
    back.keypos = keypos;
    back.prefix = back.suffix = 0;
    back.uolink = back.lolink = 0;
    back.compressed = nullptr;
    back.found_leaf = nullptr;
    back.cmp = Transition::UNDEFINED;
  }

  Transition& front() { return data[0]; }
  Transition& back() { return data[size - 1]; }
  Transition& parent() {
    assert(size > 1);
    return data[size - 2];
  }
  const Transition& back() const { return data[size - 1]; }
  void clear(int size_ = 0) {
    for (int i = size_; i < size; i++) {
      data[i].reset();
    }
    size = size_;
  }
};

// A very simple implementation of strimg
struct KeyString {
  static const size_t MAX_SIZE = 512;
  typedef uint32_t bsize_t;

  const char* data() const { return _data; }
  bsize_t size() const { return _size; }

  void resize(bsize_t size) { _size = size; }
  char& back() { return _data[_size - 1]; }
  void pop_back() { _size--; }
  void push_back(char b) { _data[_size++] = b; }
  void clear() { _size = 0; }
  void append(const char* data, size_t size) {
    memcpy(&_data[_size], data, size);
    _size += size;
  }
  char operator[](bsize_t idx) { return _data[idx]; }
  operator Slice() const { return Slice(_data, _size); }
  template <typename T>
  bool operator==(T& other) const {
    return _size == other.size() && memcmp(_data, other.data(), _size) == 0;
  }

  KeyString() : _size(0) {}

  bsize_t _size;
  char _data[MAX_SIZE];

  friend std::ostream& operator<<(std::ostream& os,
                                  const leaves::KeyString& ks) {
    // Define how to print the object here
    char data[leaves::KeyString::MAX_SIZE];
    memcpy(data, ks.data(), ks.size());
    data[ks.size()] = 0;
    os << data;
    return os;
  }
};

template <typename Cursor>
struct Inserter {
  using bsize_t = typename Cursor::bsize_t;
  using Transition = typename Cursor::Transition;
  using BlockHeader = typename Transition::BlockHeader;
  using UpperBranchNode = typename Transition::UpperBranchNode;
  using LowerBranchNode = typename Transition::LowerBranchNode;
  using LeafNode = typename Transition::LeafNode;
  using Compressed = typename Transition::Compressed;
  using Leaf = typename Transition::Leaf;
  using block_ptr = typename Transition::block_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using ubranch_ptr = typename Transition::ubranch_ptr;
  using lbranch_ptr = typename Transition::lbranch_ptr;
  using offset_t = typename Transition::offset_t;

  typedef enum { no_null, new_null, old_null } leaftype_t;

  Cursor& _cursor;
  const Slice& _value;
  Transition* _back;
  Transition _new;

  Slice _prefix;
  Slice _old_branch_prefix;
  offset_t _old_branch_node;
  offset_t _new_leaf_link;

  Inserter(Cursor& cursor, const Slice& value)
      : _cursor(cursor), _value(value), _back(&_cursor.stack.back()) {}

  Inserter(Cursor& cursor, const Slice& value, bool first)
      : _cursor(cursor), _value(value), _back(&_cursor.stack.back()) {}

  block_ptr resolve(const offset_t& offset) {
    return _cursor.storage.resolve(offset);
  }
  offset_t resolve(block_ptr p) { return _cursor.storage.resolve(p); }
  uint8_t upper(uint8_t key) const { return UpperBranchNode::upper(key); }
  uint8_t lower(uint8_t key) const { return LowerBranchNode::lower(key); }

  // start inserting the new value
  void start() {
    _start();
    check_stack();
  }

  void _start() {
    if (_back->cmp == 0)
      return _back->lbranch ? change_leaf() : change_null_leaf();

    _cursor.storage._txn.leaves++;

    if (_back->found_leaf) return split_leaf();

    bsize_t ioffset = 0;
    if (_back->ubranch->has_compressed()) {
      if (split_compressed()) return;
      ioffset += _back->ubranch->compressed()->nodesize();
    }

    if (_back->ubranch->has_branch()) return add_to_branch(ioffset);

    assert(_back->ubranch->has_null_leaf());
    assert(_cursor.stack.size == 1);
    _prefix = _cursor.key();
    _old_branch_prefix = Slice();
    _old_branch_node = setleaf(ioffset);
    _back->found_leaf = (Leaf*)&_back->ubranch->data[ioffset];
    create_new_branch();
    _cursor.storage.free(_back->ubranch);
    _new.keypos = _back->keypos;
    *_back = _new;
    _cursor._set_root(resolve(_back->ubranch));
  }

  // insert the very first value
  void first() {
    // reserve enough space for a future branch_node
    _back->cmp = 0;
    _back->prefix = _cursor.rest_key.size();
    bsize_t space = Compressed::nodesize(_cursor.rest_key.size()) +
                    Leaf::nodesize(0, _value.size());
    _back->ubranch = UpperBranchNode::alloc(space, _cursor.storage);
    _back->ubranch->add_compressed(_cursor.rest_key);
    Leaf* leaf = (Leaf*)&_back->ubranch->data[_back->ubranch->b.used];
    _back->found_leaf = leaf;
    _back->ubranch->set_null_leaf();
    _back->ubranch->b.used += leaf->fill(_value);

    _cursor._advance_key(_back->prefix);
    _cursor.storage._txn.leaves++;
    _cursor.storage._txn.branches++;
    _cursor._set_root(resolve(_back->ubranch));
  }

  bool split_compressed() {
    assert(_back->compressed);

    const Compressed* cn = _back->compressed;
    if (_back->prefix == cn->size) return false;  // no split
    assert(_back->prefix < cn->size);

    if (_back->ubranch->txn_id != _cursor.storage._txn.txn_id) {
      _back->ubranch = _cursor.storage.cow_replace(_back->ubranch);
      cn = _back->compressed = _back->ubranch->compressed();
    }

    _prefix = Slice(cn->key, _back->prefix);
    _old_branch_prefix =
        Slice(&cn->key[_back->prefix], cn->size - _back->prefix);
    _old_branch_node = _cursor.storage.resolve(_back->ubranch);

    _new.keypos = _back->keypos;
    create_new_branch();

    *_back = _new;
    make_stack_writable();
    return true;
  }

  void split_leaf() {
    const Leaf* fl = _back->found_leaf;
    _prefix = Slice(fl->key_value, _back->suffix);
    assert(fl->key_size >= _back->suffix);

    // the new leaf prefix cuts the first suffix bytes
    int psize = fl->key_size - _back->suffix;
    _old_branch_prefix = Slice(fl->key_value + fl->key_size - psize, psize);
    _old_branch_node = *_back->plink();

    _new.keypos = _cursor.current_key.size() - _prefix.size();
    create_new_branch();
    make_back_writable();
    *_back->plink() = resolve(_new.ubranch);
    _back->cmp = 0;
    _back->found_leaf = nullptr;
    _cursor.stack.size++;
    _cursor.stack.back() = _new;
  }

  // change the value of leaf
  void change_leaf() {
    assert(_back->found_leaf);
    assert(_back->lbranch);
    assert(_cursor.rest_key.empty());

    make_back_writable();

    // manipulate the key managment for grow leaf
    _cursor.rest_key =
        Slice(_cursor.rest_key.data() - _back->suffix, _back->suffix);
    _cursor.current_key.resize(_cursor.current_key.size() - _back->suffix);

    grow_leaf();
    *_back->plink() = _new_leaf_link;
  }

  void change_null_leaf() {
    assert(_back->found_leaf);
    assert(!_back->lbranch);
    assert(!_back->leaf);
    assert(_back->suffix == 0);
    assert(_cursor.rest_key.empty());

    bsize_t nlsize = Leaf::nodesize(0, _value.size()),
            olsize = _back->found_leaf->nodesize();

    if (olsize == nlsize) {
      make_back_writable();
    } else {
      ubranch_ptr ub = UpperBranchNode::alloc(
          _back->ubranch->b.used + nlsize - olsize, _cursor.storage);
      copy(*ub, *_back->ubranch, _back->ubranch->b.used - olsize);
      ub->b.used -= olsize;
      ub->b.used += nlsize;
      _cursor.storage.free(_back->ubranch);
      _back->ubranch = ub;
      make_stack_writable();
    }

    bsize_t ioffset = _back->ubranch->branchsize();
    if (_back->ubranch->has_compressed()) {
      _back->compressed = _back->ubranch->compressed();
      ioffset += _back->compressed->nodesize();
    }
    Leaf* leaf = (Leaf*)&_back->ubranch->data[ioffset];
    leaf->fill(_value);
    _back->found_leaf = leaf;
  }

  void add_to_branch(bsize_t ioffset) {
    if (_cursor.rest_key.empty()) {
      // add null leaf
      assert(!_back->ubranch->has_null_leaf());
      bsize_t size = Leaf::nodesize(0, _value.size());
      bsize_t ioffset = _back->ubranch->branchsize();
      if (_back->ubranch->has_compressed()) {
        assert(_back->compressed);
        ioffset += _back->compressed->nodesize();
      }

      if (_back->ubranch->freespace() < size) {
        ubranch_ptr unew = UpperBranchNode::alloc(_back->ubranch->b.used + size,
                                                  _cursor.storage);
        copy(*unew, *_back->ubranch, _back->ubranch->b.used);
        _cursor.storage.free(_back->ubranch);
        _back->ubranch = unew;
        make_stack_writable();
      } else
        make_back_writable();

      Leaf* leaf = (Leaf*)&_back->ubranch->data[ioffset];
      _back->found_leaf = leaf;
      _back->ubranch->b.used += leaf->fill(_value);
      _back->ubranch->set_null_leaf();
      _back->cmp = 0;
      return;
    }

    assert(_back->suffix == 0);
    assert(_back->branch_key == (uint8_t)_cursor.rest_key[0]);

    uint8_t nkey = _cursor.rest_key[0];
    _cursor._advance_key(1);

    if (_back->lbranch) {
      // the upperkey already exists!
      assert(_back->ubranch->isset(nkey));
      assert(!(_back->lbranch->isset(nkey)));
      make_back_writable();

      int count = _back->lbranch->count();
      if (_back->lbranch->freespace() < sizeof(offset_t) ||
          _back->lbranch->txn_id != _cursor.storage._txn.txn_id) {
        lbranch_ptr olb = LowerBranchNode::alloc(count + 1, _cursor.storage);
        copy(*olb, *_back->lbranch, count * sizeof(offset_t));
        _cursor.storage.free(_back->lbranch);
        _back->lbranch = olb;
        *_back->uplink() = resolve(olb);
      }
      assert(_back->lbranch->freespace() >= sizeof(offset_t));
      int index = _back->lbranch->index(nkey);
      memmove(&_back->lbranch->links[index + 1], &_back->lbranch->links[index],
              sizeof(offset_t) * (count - index));

      grow_leaf();
      _back->lolink = index;
      *_back->plink() = _new_leaf_link;
      _back->lbranch->set(nkey);
      return;
    }

    // new upper key and new lower key
    int count = _back->ubranch->count();
    if ((count + 1) * sizeof(offset_t) >= _back->ubranch->branchsize() ||
        _back->ubranch->txn_id != _cursor.storage._txn.txn_id) {
      bsize_t nbsize = UpperBranchNode::branchsize(count + 1),
              obsize = _back->ubranch->branchsize();
      ubranch_ptr unew = UpperBranchNode::alloc(
          _back->ubranch->b.used - obsize + nbsize, _cursor.storage);
      bsize_t size = ioffset + obsize;
      copy(*unew, *_back->ubranch, size);
      memmove(&unew->data[ioffset + nbsize], &_back->ubranch->data[size],
              _back->ubranch->b.used - size);
      unew->b.used += nbsize - obsize;
      _cursor.storage.free(_back->ubranch);
      _back->ubranch = unew;
    }

    make_stack_writable();

    int index = _back->ubranch->index(nkey);
    offset_t* links = (offset_t*)&_back->ubranch->data[ioffset];
    memmove(&links[index + 1], &links[index],
            sizeof(offset_t) * (count - index));
    _back->ubranch->set(nkey);
    _back->uolink = _back->ubranch->olink(&links[index]);

    _back->lbranch = LowerBranchNode::alloc(1, _cursor.storage);
    _back->lbranch->set(nkey);
    _back->leaf =
        LeafNode::alloc(Leaf::nodesize(_cursor.rest_key.size(), _value.size()),
                        _cursor.storage);
    Leaf* leaf = _back->leaf->leaf(1);
    _back->lbranch->leaves_used = leaf->fill(_cursor.rest_key, _value);
    *_back->uplink() = resolve(_back->lbranch);
    _back->lbranch->leaves = resolve(_back->leaf);
    _back->lbranch->links[0] = 1;
    _back->lolink = 0;
    _back->found_leaf = leaf;
    _back->cmp = 0;
    _cursor._advance_key(_cursor.rest_key.size());
  }

  // add a leaf to the _back->branch
  offset_t add_leaf(const Slice& key, const Slice& value) {
    // TODO: Big Value handling
    offset_t pos = setleaf(_back->lbranch->leaves_used);
    _back->lbranch->leaves_used += _back->leaf->leaf(pos)->fill(key, value);
    assert(_back->lbranch->leaves_used + sizeof(LeafNode) <=
           _back->leaf->block_size);
    return pos;
  }

  // alloc a new branch with at least size capacity and addtionally
  // create the leafs for _value and _mvalue in the new branch
  void create_new_branch() {
    // Calculate UpperBranch size;
    bsize_t size = Compressed::nodesize(_prefix.size());

    leaftype_t lt = no_null;
    if (_old_branch_prefix.empty()) {
      // only possible if _old_branch_node is a leaf
      assert(isleaf(_old_branch_node));
      assert(_back->found_leaf);
      size += UpperBranchNode::branchsize(1) +
              Leaf::nodesize(0, _back->found_leaf->value_size);
      lt = old_null;
    } else if (_cursor.rest_key.empty()) {
      size += UpperBranchNode::branchsize(1) + Leaf::nodesize(0, _value.size());
      lt = new_null;
    } else {
      size += UpperBranchNode::branchsize(2);
    }

    // create the new branch
    _new.ubranch = UpperBranchNode::alloc(size, _cursor.storage);
    _new.leaf = nullptr;
    _new.prefix = _prefix.size();
    _cursor.storage._txn.branches++;
    _new.ubranch->add_compressed(_prefix);
    bsize_t ioffset = _new.ubranch->space();
    if (lt == no_null)
      create_double_lbranch(ioffset);
    else
      create_single_lbranch(ioffset, lt);
  }

  void create_single_lbranch(bsize_t ioffset, leaftype_t lt) {
    Leaf* leaf;
    _new.uolink = ioffset;
    _new.lolink = 0;

    if (lt == new_null) {
      assert(_old_branch_prefix.size());
      assert(_cursor.rest_key.empty());
      offset_t* links = (offset_t*)&_new.ubranch->data[ioffset];
      uint8_t key = _old_branch_prefix[0];
      _new.ubranch->set(key);
      _old_branch_prefix.iadvance(1);
      links[0] = create_lbranch_old(key);
      ioffset += _new.ubranch->branchsize();
      leaf = (Leaf*)&_new.ubranch->data[ioffset];
      leaf->fill(_value);
      _new.found_leaf = leaf;
      _new.suffix = 0;
      _new.cmp = 0;
    } else {
      assert(lt == old_null);
      assert(_old_branch_prefix.empty());
      uint8_t key = _cursor.rest_key[0];
      _new.ubranch->set(key);
      _cursor._advance_key(1);
      *_new.uplink() = create_lbranch_new(key);
      ioffset += _new.ubranch->branchsize();
      leaf = (Leaf*)&_new.ubranch->data[ioffset];
      leaf->fill(_back->found_leaf->value());
    }
    _new.ubranch->b.used = ioffset + leaf->nodesize();
    _new.ubranch->set_null_leaf();
  }

  void create_double_lbranch(bsize_t ioffset) {
    offset_t* links = (offset_t*)&_new.ubranch->data[ioffset];

    assert(_old_branch_prefix.size());
    assert(_cursor.rest_key.size());

    uint8_t old_key = _old_branch_prefix[0];
    uint8_t new_key = _cursor.rest_key[0];
    _old_branch_prefix.iadvance(1);
    _cursor._advance_key(1);

    assert(old_key != new_key);

    if (upper(old_key) == upper(new_key)) {
      _new.ubranch->set(old_key);
      _new.uolink = _new.ubranch->olink(&links[0]);
      *_new.uplink() = create_lbranch(old_key, new_key);
      _new.ubranch->b.used += _new.ubranch->branchsize();
    } else {
      _new.ubranch->set(old_key);
      _new.ubranch->set(new_key);
      links[_new.ubranch->index(old_key)] = create_lbranch_old(old_key);
      _new.uolink = _new.ubranch->olink(&links[_new.ubranch->index(new_key)]);
      *_new.uplink() = create_lbranch_new(new_key);
      _new.ubranch->b.used += _new.ubranch->branchsize();
    }
  }

  offset_t create_lbranch(uint8_t old_key, uint8_t new_key) {
    _new.lbranch = LowerBranchNode::alloc(2, _cursor.storage);
    _new.lbranch->set(old_key);
    _new.lbranch->set(new_key);

    bsize_t size = Leaf::nodesize(_cursor.rest_key.size(), _value.size());
    if (isleaf(_old_branch_node)) {
      _new.leaf = move_old_branch_leaf(_new.lbranch, old_key, size);
    } else {
      _new.leaf = LeafNode::alloc(size, _cursor.storage);
      *_new.lbranch->link(old_key) = modify_old_branch();
    }

    _new.lbranch->leaves = resolve(_new.leaf);
    *_new.lbranch->link(new_key) = add_new();
    return resolve(_new.lbranch);
  }

  offset_t create_lbranch_old(uint8_t old_key) {
    lbranch_ptr lbranch = LowerBranchNode::alloc(1, _cursor.storage);
    lbranch->set(old_key);
    if (isleaf(_old_branch_node)) {
      lbranch->leaves = resolve(move_old_branch_leaf(lbranch, old_key));
    } else {
      *lbranch->link(old_key) = modify_old_branch();
    }
    return resolve(lbranch);
  }

  offset_t modify_old_branch() {
    ubranch_ptr ub = _cursor.storage.resolve(_old_branch_node);
    assert(ub->has_compressed());
    Compressed* c = ub->compressed();
    assert(c->size > _old_branch_prefix.size());
    bsize_t ns = Compressed::nodesize(_old_branch_prefix.size()),
            ons = c->nodesize();

    c->size = _old_branch_prefix.size();
    memcpy(c->key, _old_branch_prefix.data(), c->size);

    if (ons != ns) {
      memmove(ub->data + ns, ub->data + ons, ub->b.used - ons);
      ub->b.used -= ons - ns;
      if (!ns) {
        ub->clear_compressed();
        return _old_branch_node;
      }
    }
    return _old_branch_node;
  }

  leaf_ptr move_old_branch_leaf(lbranch_ptr& lbranch, uint8_t old_key,
                                bsize_t extra = 0) {
    assert(_back->leaf);
    assert(isleaf(_old_branch_node));
    Leaf* old_leaf = _back->leaf->leaf(_old_branch_node);
    bsize_t size =
        Leaf::nodesize(_old_branch_prefix.size(), old_leaf->value_size);
    leaf_ptr leaf = LeafNode::alloc(size + extra, _cursor.storage);
    *lbranch->link(old_key) = 1;
    lbranch->leaves_used = size;
    leaf->leaf(0)->fill(_old_branch_prefix, old_leaf->value());
    return leaf;
  }

  offset_t create_lbranch_new(uint8_t new_key) {
    bsize_t size = Leaf::nodesize(_cursor.rest_key.size(), _value.size());
    _new.leaf = LeafNode::alloc(size, _cursor.storage);
    _new.lbranch = LowerBranchNode::alloc(1, _cursor.storage);
    _new.lbranch->leaves = resolve(_new.leaf);
    _new.lbranch->set(new_key);
    _new.lbranch->links[0] = add_new();
    return resolve(_new.lbranch);
  }

  offset_t add_new() {
    Leaf* leaf = _new.leaf->leaf(_new.lbranch->leaves_used);
    _new.lbranch->leaves_used += leaf->fill(_cursor.rest_key, _value);
    _new.found_leaf = leaf;
    _new.suffix = _cursor.rest_key.size();
    _new.cmp = 0;
    _cursor._advance_key(_new.suffix);
    return _new.leaf->olink(_new.found_leaf);
  }

  // grow the leaf and replace found_leaf with value
  void grow_leaf() {
    if (!_back->leaf) _back->leaf = resolve(_back->lbranch->leaves);

    auto space = _back->lbranch->leaves_used +
                 Leaf::nodesize(_cursor.rest_key.size(), _value.size());
    if (_back->leaf->block_size < sizeof(LeafNode) + space) {
      leaf_ptr nleaf =
          LeafNode::alloc(space - _back->lbranch->leaves_free, _cursor.storage);
      _back->lbranch->copy_leaf(nleaf, _back->leaf);
      _cursor.storage.free(_back->leaf);
      _back->lbranch->leaves = resolve(nleaf);
      _back->leaf = nleaf;
    }

    _new_leaf_link = add_leaf(_cursor.rest_key, _value);
    _cursor._advance_key(_cursor.rest_key.size());
    _back->found_leaf = _back->leaf->leaf(_new_leaf_link);
    _back->cmp = 0;
  }

  // make back and the stack writable
  void make_back_writable() {
    if (!Cursor::is_transactional) return;

    if (_back->ubranch->txn_id != _cursor.storage._txn.txn_id) {
      _back->ubranch = _cursor.storage.cow_replace(_back->ubranch);
      make_stack_writable();
    }
  }

  // make the stack without back writeable
  void make_stack_writable() {
    if (!Cursor::is_transactional) return;

    int i = _cursor.stack.size - 2;
    for (; i >= 0; i--) {
      Transition& t = _cursor.stack.data[i];
      if (t.lbranch->txn_id != _cursor.storage._txn.txn_id) {
        t.lbranch = _cursor.storage.cow_replace(t.lbranch);
        *t.plink() = resolve(_cursor.stack.data[i + 1].ubranch);
      } else {
        *t.plink() = resolve(_cursor.stack.data[i + 1].ubranch);
      }

      if (t.ubranch->txn_id != _cursor.storage._txn.txn_id) {
        t.ubranch = _cursor.storage.cow_replace(t.ubranch);
        *t.uplink() = resolve(t.lbranch);
      } else {
        *t.uplink() = resolve(t.lbranch);
        break;
      }
    }

    if (i < 0) {
      _cursor._set_root(resolve(_cursor.stack.front().ubranch));
    }
  }

  void check_stack() {
#if defined(DEBUG) && !defined(NDEBUG)
    for (size_t i = 0; i < _cursor.stack.size; i++) {
      Transition& t = _cursor.stack.data[i];
      assert(t.cmp == 0);
      assert(t.found_leaf == nullptr || i == _cursor.stack.size - 1);
      t.ubranch->check();
    }
#endif
  }
};

// A cursor to
template <typename Storage>
struct _Cursor {
  using BlockHeader = typename Storage::BlockHeader;
  using block_ptr = typename Storage::block_ptr;
  using offset_t = typename Storage::offset_t;
  using bsize_t = typename Storage::bsize_t;
  using txn_ptr = typename Storage::txn_ptr;
  const static bool is_transactional = Storage::is_transactional;
  typedef _Stack<BlockHeader> Stack;
  using Transition = typename Stack::Transition;
  using ubranch_ptr = typename Transition::ubranch_ptr;
  using lbranch_ptr = typename Transition::lbranch_ptr;

  struct Transaction {
    txn_ptr txn;

    Transaction() : txn(nullptr) {}
    ~Transaction() {
      if (txn) txn->count--;
    }

    void operator=(txn_ptr txn_) {
      if (txn) txn->count--;
      txn = txn_;
      txn->count++;
    }

    void reset() {
      if (txn) txn->count--;
      txn = nullptr;
    }

    txn_ptr operator->() { return txn; }
  };

  _Cursor(Storage& storage_) : storage(storage_), transaction_active(false) {
    root = 0;
  }

  // return true if the cursor is on a valid position
  bool is_valid() const {
    if (stack.size) return stack.back().success();
    return false;
  }

  void find(const Slice& key) {
    _update();
    rest_key = key;
    if (stack.size && _keep_stack()) return;
    _find();
  }

  void first() {
    _update();
    stack.clear(0);
    if (!root) return;
    rest_key = Slice();
    current_key.clear();
    _push(root);
    while (stack.back().ubranch->first(*this));
  }

  void last() {
    _update();
    stack.clear(0);
    if (!root) return;
    rest_key = Slice();
    current_key.clear();
    _push(root);
    while (stack.back().ubranch->last(*this));
  }

  void next() {
    rest_key.iadvance(rest_key.size());
    while (stack.size) {
      auto old = stack.size;
      if (!stack.back().ubranch->next(*this)) break;
      if (old < stack.size) {
        while (stack.back().ubranch->first(*this));
        break;
      }
    }
  }

  void prev() {
    rest_key.iadvance(rest_key.size());
    auto old = stack.size;
    while (stack.size) {
      auto old = stack.size;
      if (!stack.back().ubranch->prev(*this)) break;
      if (old < stack.size) {
        while (stack.back().ubranch->last(*this));
        break;
      }
    }
  }

  void value(const Slice& value) {
    if (!transaction_active) {
      if (!storage.start_transaction()) throw TransactionActive();
      transaction_active = true;
    }

    if (!stack.size) {
      if (!root) {
        stack.push(nullptr);
        Inserter(*this, value, true).first();
        return;
      }
      throw NoValidPosition();
    }

    Inserter(*this, value).start();
  }

  Slice value() const {
    const Transition& back = stack.back();
    if (back.cmp) return Slice();
    assert(back.found_leaf);
    return back.found_leaf->value();
  }

  Slice key() const { return current_key; }

  void remove() {}
  void commit() {
    if (transaction_active) {
      storage.prepare_commit();
      storage.commit();
      transaction_active = false;
      auto root_ = root;
      _update();
      assert(root_ == root);
    }
  }

  void rollback() {
    if (transaction_active) {
      storage.rollback();
      stack.clear();
      transaction_active = false;
      find(current_key);
    }
  }

  /* Helpers */

  void _set_root(offset_t offset) { storage._txn.root = root = offset; }

  void _advance_key(size_t size) {
    current_key.append(rest_key.data(), size);
    rest_key.iadvance(size);
  }

  void _push(const offset_t& ptr) {
    stack.push(storage.resolve(ptr), current_key.size());
  }

  void _pop() {
    assert(stack.size > 0);
    current_key.resize(stack.back().keypos);
    stack.size--;
  }

  void _changed_branch_key() {
    // used in move operations
    Transition& back = stack.back();
    bsize_t bpos = back.keypos + back.prefix;
    if (current_key.size() == bpos) {
      current_key.push_back(back.branch_key);
    } else {
      assert(current_key.size() == bpos + 1);
      current_key.back() = back.branch_key;
    }
  }

  bool _keep_stack() {
    assert(stack.size > 0);

    int cmp;
    size_t same = get_prefix(rest_key.data(), current_key.data(),
                             rest_key.size(), current_key.size(), cmp);
    if (same == rest_key.size() && same == current_key.size()) {
      // already found
      rest_key.iadvance(same);
      return true;
    }

    int i = 0;
    int keep = 0;
    for (; i < stack.size; i++) {
      Transition& item = stack.data[i];
      if (item.keypos > same) break;
      keep = item.keypos;
    }

    rest_key.iadvance(keep);
    current_key.resize(keep);
    stack.clear(i);

    Transition& back = stack.back();
    back.found_leaf = nullptr;
    back.compressed = nullptr;
    back.prefix = back.suffix = 0;
    back.cmp = Transition::UNDEFINED;
    return false;
  }
  void _find() {
    if (!stack.size) {
      if (!root) return;  // empty db
      current_key.clear();
      _push(root);
    }

    assert(storage.resolve(stack.front().ubranch) == root);
    while (true) {
      if (!stack.back().ubranch->find(*this)) break;
    }
  }

  void _update() {
    if (!transaction_active) {
      txn = storage.active_txn();
      root = txn->root;
    }
  }

  void check() {
    if (root) _check_trie(root);
  }

  void _check_trie(offset_t obranch) {
    ubranch_ptr branch = storage.resolve(obranch);
    branch->check();
    branch->iterate_links(storage, [this](lbranch_ptr lb, offset_t& offset) {
      if (!isleaf(offset)) {
        _check_trie(offset);
      }
    });
  }

  Storage& storage;
  Transaction txn;
  offset_t root;
  Stack stack;
  Slice rest_key;
  KeyString current_key;
  bool transaction_active;
};

}  // namespace leaves

#endif  // _LEAVES_CURSOR_HPP
