#ifndef _LEAVES__CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include <vector>

#include "_exception.hpp"
#include "_node.hpp"

namespace leaves {

template <typename BlockHeader_>
struct _Transition {
  typedef BlockHeader_ BlockHeader;
  using block_ptr = typename BlockHeader::ptr;
  typedef _BranchNode<BlockHeader> BranchNode;
  using LeafNode = typename BranchNode::LeafNode;
  using Compressed = typename BranchNode::Compressed;
  using ArrayBranch = typename BranchNode::ArrayBranch;
  using SparseBranch = typename BranchNode::SparseBranch;
  using Leaf = typename BranchNode::Leaf;
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using leaf_ptr = typename LeafNode::ptr;
  using branch_ptr = typename BranchNode::ptr;

  static const int NOT_SAME = 2;   // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  branch_ptr branch;  // the branch block referenced in this transition
  leaf_ptr leaf;      // the final leaf block if any
  uint16_t prefix;    // count of equal chars in stringnode
  uint16_t suffix;    // count of equal chars in keyvaluenode
  uint8_t branch_key;
  bsize_t olink;  // the offset inside block that points to the output link

  const Compressed* compressed;
  union {
    const ArrayBranch* array;
    const SparseBranch* sparse;
  };
  const Leaf* found_leaf;

  // 1: the key to find is bigger than the found node
  // 0: the key is found
  // -1: the key to find is smalled than the found node
  // NOT_SAME: it is not equal but not known if -1 or 1
  int cmp;

  // position inside the key
  bsize_t keypos;

  bool success() const { return found_leaf && cmp == 0; }
  offset_t* plink() { return branch->plink(olink); }
  void reset() {
    branch = nullptr;
    leaf = nullptr;
  }

  template <typename Cursor, typename Caller>
  bool follow_link(Cursor& cursor, const offset_t* link, Caller& c) {
    if (!link) return false;
    assert(link);

    olink = (const uint8_t*)link - branch->data;
    if (isleaf(*link)) {
      leaf = cursor.storage.resolve(branch->leaves);
      c(cursor, leaf->leaf(*link));
      return false;
    }

    cursor.stack.back().cmp = 0;
    cursor._push(*link);
    return true;
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
    back.branch = block;
    back.leaf = nullptr;
    back.keypos = keypos;
    back.prefix = back.suffix = 0;
    back.olink = 0;
    back.compressed = nullptr;
    back.array = nullptr;
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
  using BranchNode = typename Transition::BranchNode;
  using LeafNode = typename Transition::LeafNode;
  using SparseBranch = typename Transition::SparseBranch;
  using ArrayBranch = typename Transition::ArrayBranch;
  using Compressed = typename Transition::Compressed;
  using Leaf = typename Transition::Leaf;
  using block_ptr = typename Transition::block_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using branch_ptr = typename Transition::branch_ptr;
  using offset_t = typename Transition::offset_t;

  static const bsize_t OFFSET_LINK2 = offsetof(ArrayBranch, links[1]);
  static const bsize_t OFFSET_LINK1 = offsetof(ArrayBranch, links[0]);

  Cursor& _cursor;
  const Slice& _value;
  Transition* _back;

  offset_t _new_leaf_link;   // result of alloc or grow
  offset_t _move_leaf_link;  // result and input of alloc
  bool _mvalid;              // _mkey and _mvalue are valid
  Slice _mkey;               // key and value
  Slice _mvalue;             // of the move leaf

  Inserter(Cursor& cursor, const Slice& value)
      : _cursor(cursor),
        _value(value),
        _back(&_cursor.stack.back()),
        _mvalid(false) {}

  Inserter(Cursor& cursor, const Slice& value, bool first)
      : _cursor(cursor),
        _value(value),
        _back(&_cursor.stack.back()),
        _mvalid(false) {}

  block_ptr resolve(const offset_t& offset) {
    return _cursor.storage.resolve(offset);
  }
  offset_t resolve(block_ptr p) { return _cursor.storage.resolve(p); }

  // start inserting the new value
  void start() {
    _start();
    check_stack();
  }

  void _start() {
    if (_back->cmp == 0) return change_leaf();

    _cursor.storage._txn.leaves++;

    if (_back->found_leaf) {
      if (split_leaf()) return;
      return extend_leaf();
    }

    bsize_t ioffset = 0;
    if (_back->branch->has_compressed()) {
      if (split_compressed()) return;
      ioffset += _back->branch->compressed()->nodesize();
    }
    if (_back->branch->has_null_leaf()) ioffset += sizeof(offset_t);
    if (_back->branch->has_array()) return add_to_array(ioffset);
    assert(_back->branch->has_sparse());
    add_to_sparse(ioffset);
  }

  // insert the very first value
  void first() {
    // reserve enough space for a future branch_node
    _back->prefix = 0;
    _back->cmp = 0;
    _back->suffix = _cursor.rest_key.size();
    alloc(sizeof(offset_t));
    _back->olink = _back->branch->add_null_leaf();
    *_back->plink() = _new_leaf_link;
    _cursor.storage._txn.leaves++;
    _cursor.storage._txn.branches++;
  }

  bool split_compressed() {
    /*
      Split compressed node

      Before
      ------
        parent -> ["abcdef"][VO?][B]..[L]

      Insert
      ------
        key = "abce"

      After
      -----
                  new node              orignal node
        parent -> ["abc"][BA'd'] ->     [ef][VO?][B]..[L]
                        [  'e']..[L0]

      special cases:
      key = "" (empty)
      key = "abc" (subkey of string)

      condtion:
      size(key) < compressed.size
    */
    Compressed* cn = _back->branch->compressed();
    if (_back->prefix == cn->size) return false;  // no split

    assert(_back->prefix < cn->size);
    make_back_writable();

    // _brack->branch may have changed in make_back_writable
    cn = _back->branch->compressed();
    // save the prefix of new block
    Slice prefix(_cursor.rest_key.data() - _back->prefix, _back->prefix);
    uint8_t key1 = cn->key[_back->prefix];

    // cut compressed of original block
    bsize_t cut_size = _back->prefix + 1;
    bsize_t before = cn->nodesize(), after = cn->nodesize(cn->size - cut_size);

    if (after) {
      // cut the first bytes of prefix
      assert(cn->size > cut_size);
      cn->size -= cut_size;
      memmove(cn->key, &cn->key[cut_size], cn->size);
    } else
      _back->branch->clear_compressed();

    if (before != after) {
      // compressed went smaller move everything after compressed
      assert(before > after);
      memmove(_back->branch->data + after, _back->branch->data + before,
              _back->branch->b.used - before);
      _back->branch->b.used -= (before - after);
    }

    block_ptr org = _back->branch;

    // create the new branch
    if (_cursor.rest_key.size()) {
      uint8_t key2 = _cursor.rest_key[0];
      _cursor._advance_key(1);

      alloc(Compressed::nodesize(_back->prefix) + ArrayBranch::nodesize(2));
      _back->branch->add_compressed(prefix.data(), prefix.size());
      _back->olink = _back->branch->add_array(key2, key1, resolve(org));
      *_back->plink() = _new_leaf_link;
    } else {
      // with empty key
      alloc(Compressed::nodesize(_back->prefix) + ArrayBranch::nodesize(1) +
            sizeof(offset_t));
      _back->branch->add_compressed(prefix.data(), prefix.size());
      _back->olink = _back->branch->add_null_leaf();
      *_back->plink() = _new_leaf_link;
      *_back->branch->plink(_back->branch->add_array(key1)) = resolve(org);
    }

    _cursor.storage._txn.branches++;
    return true;
  }

  bool split_leaf() {
    /* Split the key of leaf node

      Before
      ------
        parent -> ["abc"][B'd']..[L("efgh")]

      Insert
      ------
        key = "abcdegh"

      After
      -----
                  orig node        new node
        parent -> ["abc"][B'd'] -> ["e"][B'f']..[L("gh")]
                                        [ 'g']..[L("h")]

      special cases:
      key = "abcd" (rest_key is empty)
      key = "abc" (subkey of string)

      condtion:
      size(key) < compressed.size
    */
    assert(_back->found_leaf != nullptr);

    if (_back->suffix == _back->found_leaf->key_size) return false;  // extend
    assert(_back->suffix < _back->found_leaf->key_size);

    // save the prefix (_cursor.rest_key.data()-_back->suffix is safe!)
    Slice prefix(_cursor.rest_key.data() - _back->suffix, _back->suffix);
    _back->suffix = 0;

    // Mark the node to move and add the size to free space
    _back->branch->leaves_free += _back->found_leaf->nodesize();
    _mvalid = true;
    _mkey = _back->found_leaf->key();
    _mvalue = _back->found_leaf->value();
    uint8_t key2 = _mkey[prefix.size()];
    _mkey.iadvance(prefix.size() + 1);

    // extend the stack
    _back->found_leaf = nullptr;
    _back->cmp = 0;
    _cursor.stack.push(nullptr);
    _back = &_cursor.stack.back();
    _back->keypos = _cursor.current_key.size() - prefix.size();
    _back->prefix = prefix.size();

    // create new block
    if (_cursor.rest_key.size()) {
      uint8_t key1 = _cursor.rest_key[0];
      _cursor._advance_key(1);
      alloc(Compressed::nodesize(prefix.size()) + ArrayBranch::nodesize(2));
      _back->branch->add_compressed(prefix.data(), prefix.size());
      _back->olink = _back->branch->add_array(key1, key2, _move_leaf_link);
    } else {
      alloc(Compressed::nodesize(prefix.size()) + ArrayBranch::nodesize(1) +
            sizeof(offset_t));
      _back->branch->add_compressed(prefix.data(), prefix.size());
      _back->olink = _back->branch->add_null_leaf();
      *_back->branch->plink(_back->branch->add_array(key2)) = _move_leaf_link;
    }

    *_back->plink() = _new_leaf_link;
    _cursor.storage._txn.branches++;
    if (!purge_first()) make_stack_writable();
    return true;
  }

  void extend_leaf() {
    /* extend the key of leaf node

      Before
      ------
      parent -> ["abc"][B'd']..[L("efgh")]

      Insert
      ------
        key = "abcdefghjk"

      After
      -----
                  orig node        new node
        parent -> ["abc"][B'd'] -> ["efgh"][V][B'j']..[L("k")]
                the former leaf with no key ^          ^the new leaf
    */

    assert(_back->found_leaf != nullptr);
    assert(_back->suffix == _back->found_leaf->key_size);
    assert(_cursor.rest_key.size() >= 1);

    // save the prefix
    Slice prefix(_cursor.rest_key.data() - _back->suffix, _back->suffix);
    _back->suffix = 0;

    // mark the node to move
    _back->branch->leaves_free += _back->found_leaf->nodesize();
    _mvalid = true;
    _mkey = Slice();
    _mvalue = _back->found_leaf->value();

    // extend the stack
    _back->found_leaf = nullptr;
    _back->cmp = 0;

    _cursor.stack.push(nullptr);
    _back = &_cursor.stack.back();
    _back->keypos = _cursor.current_key.size() - prefix.size();
    _back->prefix = prefix.size();

    uint8_t key = _cursor.rest_key[0];
    _cursor._advance_key(1);

    // create new block
    alloc(Compressed::nodesize(prefix.size()) + ArrayBranch::nodesize(1) +
          sizeof(offset_t));
    _back->branch->add_compressed(prefix.data(), prefix.size());
    *_back->branch->plink(_back->branch->add_null_leaf()) = _move_leaf_link;
    _back->olink = _back->branch->add_array(key);
    *_back->plink() = _new_leaf_link;
    _cursor.storage._txn.branches++;
    if (!purge_first()) make_stack_writable();
  }

  // change the value of leaf
  void change_leaf() {
    assert(_back->found_leaf);

    make_back_writable();
    assert(_cursor.rest_key.empty());
    // manipulate the key managment for grow leaf
    _cursor.rest_key =
        Slice(_cursor.rest_key.data() - _back->suffix, _back->suffix);
    _cursor.current_key.resize(_cursor.current_key.size() - _back->suffix);

    grow_leaf();
    *_back->plink() = _new_leaf_link;
  }

  void add_to_array(bsize_t ioffset) {
    if (_cursor.rest_key.empty()) {
      add_null_leaf(ioffset);
      return;
    }

    assert(_back->array);
    assert(_back->suffix == 0);
    assert(_back->branch_key == (uint8_t)_cursor.rest_key[0]);

    uint8_t nkey = _cursor.rest_key[0];
    _cursor._advance_key(1);

    ArrayBranch* n = (ArrayBranch*)&_back->branch->data[ioffset];
    if (n->size < ArrayBranch::COUNT) {
      _back->branch->b.used += alloc_branch(sizeof(offset_t));

      // _back->branch may have changed!
      n = (ArrayBranch*)&_back->branch->data[ioffset];
      n->keys[n->size] = _back->branch_key = nkey;
      n->links[n->size] = _new_leaf_link;
      _back->olink = _back->branch->olink(&n->links[n->size]);
      n->size++;
    } else {
      // change ArrayBranch to Trie branch
      const bsize_t MAX_ARRAY =
          sizeof(ArrayBranch) + ArrayBranch::COUNT * sizeof(offset_t);
      union {
        SparseBranch tmp;
        char buffer[SparseBranch::nodesize(256)];
      };

      tmp.trie.init();

      _back->branch->b.used +=
          alloc_branch(tmp.nodesize(ArrayBranch::COUNT + 1) - MAX_ARRAY);

      // _block may have changed
      n = (ArrayBranch*)&_back->branch->data[ioffset];
      for (int i = 0; i < n->size; i++) {
        tmp.trie.bits.set(n->keys[i]);
      }
      tmp.trie.bits.set(nkey);

      for (int i = 0; i < n->size; i++) {
        tmp.trie.values[tmp.trie.bits.index(n->keys[i])] = n->links[i];
      }

      int index = tmp.trie.bits.index(nkey);
      // tmp.trie.values[index] = _new_leaf_link;  <- gcc "optimize" this line
      // away
      memcpy(n, &tmp, tmp.nodesize());

      /*
         The next line is a bit weird:
         Original the above commented line was used, but
         it crashed if compiled under gcc 13.3.0 with the -O2 flag.
         After some debugging, it turns out that -O2 optimation
         removed the comment statement.
         Therefore the optimizer had to be outfoxed.
      */
      SparseBranch* sn = (SparseBranch*)n;
      _back->olink = _back->branch->olink(&sn->trie.values[index]);
      *_back->plink() = _new_leaf_link;
      _back->branch->clear_array();
      _back->branch->set_sparse();
    }
  }

  void add_to_sparse(bsize_t ioffset) {
    if (_cursor.rest_key.empty()) {
      add_null_leaf(ioffset);
      return;
    }

    assert(_back->branch_key == (uint8_t)_cursor.rest_key[0]);

    SparseBranch* n = (SparseBranch*)&_back->branch->data[ioffset];
    assert(!n->trie.get(_back->branch_key));

    uint8_t c = _cursor.rest_key[0];
    _cursor._advance_key(1);
    _back->branch->b.used += alloc_branch(sizeof(offset_t));
    n = (SparseBranch*)&_back->branch->data[ioffset];
    int index = n->trie.insert(c, _new_leaf_link);
    _back->olink = _back->branch->olink(&n->trie.values[index]);
  }

  void add_null_leaf(bsize_t ioffset) {
    _back->branch->b.used += alloc_branch(sizeof(offset_t));

    assert(!_back->branch->has_null_leaf());
    assert(_back->branch->has_array() || _back->branch->has_sparse());

    memmove(&_back->branch->data[ioffset + sizeof(offset_t)],
            &_back->branch->data[ioffset], _back->branch->b.used - ioffset);

    _back->branch->set_null_leaf();
    _back->olink = ioffset;
    *_back->plink() = _new_leaf_link;
  }

  bool purge_first() {
    // purge the first block if is a non canonic block
    // it is recognized by "used == sizeof(offset_t)""
    Transition& front = _cursor.stack.front();
    if (_cursor.stack.size == 2 && front.branch->b.used == sizeof(offset_t)) {
      _cursor.storage.free(front.leaf);
      _cursor.storage.free(front.branch);
      front = *_back;
      _cursor.storage._txn.root = _cursor.root = resolve(_back->branch);
      _cursor.stack.size = 1;
      assert(_cursor.storage._txn.branches > 0);
      _cursor.storage._txn.branches--;
      return true;
    }
    return false;
  }

  // add a leaf to the _back->branch
  offset_t add_leaf(const Slice& key, const Slice& value) {
    // TODO: Big Value handling
    offset_t pos = _back->branch->leaves_used;
    setleaf(pos);

    Leaf* l = _back->leaf->leaf(pos);
    l->key_size = key.size();
    l->value_size = value.size();
    memcpy(l->key_value, key.data(), l->key_size);
    memcpy(l->key_value + align(l->key_size), value.data(), l->value_size);
    _back->branch->leaves_used += l->nodesize();

    assert(_back->branch->leaves_used + sizeof(LeafNode) <=
           _back->leaf->block_size);
    return pos;
  }

  // alloc a new branch with at least size capacity and addtionally
  // create the leafs for _value and _mvalue in the new branch
  void alloc(bsize_t size) {
    _back->branch = BranchNode::alloc(size, _cursor.storage);

    bsize_t leaf_size = Leaf::nodesize(_cursor.rest_key.size(), _value.size());
    if (_mvalid) leaf_size += Leaf::nodesize(_mkey.size(), _mvalue.size());

    _back->branch->size = 0;
    _back->leaf = LeafNode::alloc(leaf_size, _cursor.storage);
    _back->branch->leaves = resolve(_back->leaf);
    _back->branch->leaves_free = 0;
    _back->branch->leaves_used = 0;

    _new_leaf_link = add_leaf(_cursor.rest_key, _value);
    _back->found_leaf = _back->leaf->leaf(_new_leaf_link);
    _back->cmp = 0;
    _cursor._advance_key(_cursor.rest_key.size());
    if (_cursor.stack.size > 1)
      *_cursor.stack.data[_cursor.stack.size - 2].plink() =
          resolve(_back->branch);
    else
      _cursor.storage._txn.root = _cursor.root = resolve(_back->branch);

    if (_mvalid) _move_leaf_link = add_leaf(_mkey, _mvalue);
  }

  // Alloc space for a branch and create new leaf
  bsize_t alloc_branch(bsize_t space) {
    if (_back->branch->freespace() < space) {
      branch_ptr org = _back->branch;
      _back->branch = BranchNode::alloc(space + org->b.used, _cursor.storage);
      copy(*_back->branch, *org, org->b.used);
      _cursor.storage.free(org);
      make_stack_writable();
    } else
      make_back_writable();

    assert(_back->branch->freespace() >= space);
    grow_leaf();
    return space;
  }

  // grow the leaf and replace found_leaf with value
  void grow_leaf() {
    if (!_back->leaf) _back->leaf = resolve(_back->branch->leaves);

    auto space = _back->branch->leaves_used +
                 Leaf::nodesize(_cursor.rest_key.size(), _value.size());
    if (_back->leaf->block_size < sizeof(LeafNode) + space) {
      leaf_ptr nleaf =
          LeafNode::alloc(space - _back->branch->leaves_free, _cursor.storage);
      _back->branch->copy_leaf(nleaf, _back->leaf);
      _cursor.storage.free(_back->leaf);
      _back->branch->leaves = resolve(nleaf);
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

    if (_back->branch->txn_id != _cursor.storage._txn.txn_id) {
      _back->branch = _back->branch->cow_replace(_cursor.storage);
      make_stack_writable();
    }
  }

  // make the stack without back writeable
  void make_stack_writable() {
    if (!Cursor::is_transactional) return;

    int i = _cursor.stack.size - 2;
    for (; i >= 0; i--) {
      Transition& t = _cursor.stack.data[i];
      if (t.branch->txn_id != _cursor.storage._txn.txn_id) {
        t.branch = t.branch->cow_replace(_cursor.storage);
        *t.plink() = resolve(_cursor.stack.data[i + 1].branch);
      } else {
        *t.plink() = resolve(_cursor.stack.data[i + 1].branch);
        break;
      }
    }

    if (i < 0) {
      _cursor.storage._txn.root = _cursor.root =
          resolve(_cursor.stack.front().branch);
    }
  }

  void check_stack() {
#if defined(DEBUG) && !defined(NDEBUG)
    for (size_t i = 0; i < _cursor.stack.size; i++) {
      Transition& t = _cursor.stack.data[i];
      assert(t.cmp == 0);
      assert(t.found_leaf == nullptr || i == _cursor.stack.size - 1);
      t.branch->check();
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
  using branch_ptr = typename Transition::branch_ptr;

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
    while (stack.back().branch->first(*this));
  }

  void last() {
    _update();
    stack.clear(0);
    if (!root) return;
    rest_key = Slice();
    current_key.clear();
    _push(root);
    while (stack.back().branch->last(*this));
  }

  void next() {
    rest_key.iadvance(rest_key.size());
    while (stack.size) {
      auto old = stack.size;
      if (!stack.back().branch->next(*this)) break;
      if (old < stack.size) {
        while (stack.back().branch->first(*this));
        break;
      }
    }
  }

  void prev() {
    rest_key.iadvance(rest_key.size());
    auto old = stack.size;
    while (stack.size) {
      auto old = stack.size;
      if (!stack.back().branch->prev(*this)) break;
      if (old < stack.size) {
        while (stack.back().branch->last(*this));
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
    back.array = nullptr;
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

    assert(storage.resolve(stack.front().branch) == root);
    while (true) {
      if (!stack.back().branch->find(*this)) break;
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
    branch_ptr branch = storage.resolve(obranch);
    branch->check();
    branch->iterate_links([this](offset_t& offset) {
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
