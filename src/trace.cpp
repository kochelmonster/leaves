#include "trace.hpp"

#include <fstream>
#include <iostream>

#include "block.hpp"

namespace leaves {

// TODO: Remove these debugging vars
size_t _grow_leaf = 0;
size_t _grow_branch = 0;


struct Inserter {
  static const bsize_t OFFSET_LINK2 = offsetof(ArrayBranch, links[1]);
  static const bsize_t OFFSET_LINK1 = offsetof(ArrayBranch, links[0]);

  Trace& _trace;
  const Slice& _value;
  Transition* _back;

  offset_ptr _new_leaf_link;   // result of alloc or grow
  offset_ptr _move_leaf_link;  // result and input of alloc
  bool _mvalid;
  Slice _mkey;    // key and value
  Slice _mvalue;  // of the move leaf

  Inserter(Trace& trace, const Slice& value)
      : _trace(trace),
        _value(value),
        _back(&_trace.stack.back()),
        _mvalid(false) {}

  Inserter(Trace& trace, const Slice& value, bool first)
      : _trace(trace),
        _value(value),
        _back(&_trace.stack.back()),
        _mvalid(false) {}

  // start inserting the new value
  void start();
  void _start();

  // insert the very first value
  void first();

  bool split_compressed();
  void add_to_array(bsize_t ioffset);
  void add_to_trie(bsize_t ioffset);
  void add_null_leaf(bsize_t ioffset);
  bool split_leaf();
  void extend_leaf();
  void change_leaf();

  bool purge_first();

  // alloc a new branch with at least size capacity and addtionally
  // create the leafs for _value and _mvalue in the new branch
  void alloc(bsize_t size);

  // add a leaf to the _back->branch
  offset_ptr add_leaf(const Slice& key, const Slice& value);

  // Alloc space for a branch node and create new leaf
  bsize_t alloc_branch(bsize_t space);

  // grow the leaf and replace found_leaf with value
  void grow_leaf();

  void make_back_writable();   // make back and the stack writable
  void make_stack_writable();  // make the stack without back writeable

  block_ptr get_block(offset_ptr ptr) { return _trace.storage.get_block(ptr); }
  block_ptr alloc_block(size_t size) {
    return _trace.storage.alloc_block(size);
  }

  bool check_stack();
};

INLINE void Inserter::start() {
  _start();
  assert(check_stack());
}

inline void Inserter::_start() {
  if (_back->cmp == 0) return change_leaf();

  _trace.storage.txn.leaves++;

  if (_back->found_leaf) {
    if (split_leaf()) return;
    return extend_leaf();
  }

  bsize_t ioffset = 0;
  if (_back->branch->has_compressed()) {
    if (split_compressed()) return;
    ioffset += _back->branch->compressed()->nodesize();
  }
  if (_back->branch->has_null_leaf()) ioffset += sizeof(offset_ptr);
  if (_back->branch->has_array()) return add_to_array(ioffset);
  assert(_back->branch->has_trie());
  add_to_trie(ioffset);
}

inline bool Inserter::check_stack() {
  for (size_t i = 0; i < _trace.stack.size; i++) {
    Transition& t = _trace.stack.data[i];
    assert(t.cmp == 0);
    assert(t.found_leaf == nullptr || i == _trace.stack.size-1);
  }
  return true;
}

INLINE void Inserter::first() {
  // reserve enough space for a future branch_node
  _back->prefix = 0;
  _back->cmp = 0;
  _back->suffix = _trace.rest_key.size();
  alloc(sizeof(offset_ptr));
  _back->olink = _back->branch->add_null_leaf();
  *_back->plink() = _new_leaf_link;
  _trace.storage.txn.leaves++;
  _trace.storage.txn.branches++;
}

struct StackTrieBranch : public TrieBranch {
  offset_ptr _links[ArrayBranch::COUNT + 1];  // enough space for links
} trie;

/*
  Add leaf to an arraybranch
*/
INLINE void Inserter::add_to_array(bsize_t ioffset) {
  if (_trace.rest_key.empty()) {
    add_null_leaf(ioffset);
    return;
  }

  assert(_back->array);
  assert(_back->suffix == 0);
  assert(_back->branch_key == (uint8_t)_trace.rest_key[0]);

  uint8_t nkey = _trace.rest_key[0];
  _trace.advance_key(1);

  ArrayBranch* n = (ArrayBranch*)&_back->branch->data[ioffset];
  if (n->size < ArrayBranch::COUNT) {
    _back->branch->used += alloc_branch(sizeof(offset_ptr));
    assert(_back->branch->freespace() >= sizeof(offset_ptr));
    n = (ArrayBranch*)&_back->branch->data[ioffset];  // _block may have changed
    n->keys[n->size] = _back->branch_key = nkey;
    n->links[n->size] = _new_leaf_link;
    _back->olink = _back->branch->olink(&n->links[n->size]);
    n->size++;
  } else {
    // change ArrayBranch to Trie branch
    const bsize_t MAX_ARRAY =
        sizeof(ArrayBranch) + ArrayBranch::COUNT * sizeof(offset_ptr);
    StackTrieBranch tmp;

    memset(&tmp.bits, 0, sizeof(tmp.bits));
   
    _back->branch->used += alloc_branch(sizeof(tmp) - MAX_ARRAY);

    n = (ArrayBranch*)&_back->branch->data[ioffset];  // _block may have changed
    for (int i = 0; i < n->size; i++) {
      tmp.set(n->keys[i]);
    }
    tmp.set(nkey);
    
    for (int i = 0; i < n->size; i++) {
      tmp.links[tmp.index(n->keys[i])] = n->links[i];
    }

    int index = tmp.index(nkey);
    //tmp.links[index] = _new_leaf_link;  <- gcc "optimize" this line away
    memcpy(n, &tmp, sizeof(tmp));

    /*
       The next line is a bit weird:
       Original the above commented line was used, but
       it crashed if compiled under gcc 13.3.0 with the -O2 flag.
       After some debugging, it turns out that -O2 optimation
       removed the comment statement.
       Therefor the optimizer had to be outfoxed.
    */
    ((TrieBranch*)n)->links[index] = _new_leaf_link;
    _back->olink = _back->branch->olink(&n->links[index]);
    _back->branch->clear_array();
    _back->branch->set_trie();
  }
}

INLINE void Inserter::add_to_trie(bsize_t ioffset) {
  if (_trace.rest_key.empty()) {
    add_null_leaf(ioffset);
    return;
  }

  assert(_back->branch_key == (uint8_t)_trace.rest_key[0]);

  TrieBranch* n = (TrieBranch*)&_back->branch->data[ioffset];
  assert((n->bits[n->idx(_back->branch_key)] &
          ((uint64_t)1 << n->bit(_back->branch_key))) == 0);

  uint8_t c = _trace.rest_key[0];
  _trace.advance_key(1);
  _back->branch->used += alloc_branch(sizeof(offset_ptr));
  n = (TrieBranch*)&_back->branch->data[ioffset];
  n->set(c);
  int index = n->index(c);
  memmove(&n->links[index + 1], &n->links[index],
          (n->count() - index - 1) * sizeof(offset_ptr));
  n->links[index] = _new_leaf_link;
  _back->olink = _back->branch->olink(&n->links[index]);
}

INLINE void Inserter::add_null_leaf(bsize_t ioffset) {
  _back->branch->used += alloc_branch(sizeof(offset_ptr));

  assert(!_back->branch->has_null_leaf());
  assert(_back->branch->has_array() || _back->branch->has_trie());

  memmove(&_back->branch->data[ioffset + sizeof(offset_ptr)],
          &_back->branch->data[ioffset], _back->branch->used - ioffset);

  _back->branch->set_null_leaf();
  _back->olink = ioffset;
  *_back->plink() = _new_leaf_link;
}

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
INLINE bool Inserter::split_compressed() {
  Compressed* cn = _back->branch->compressed();
  if (_back->prefix == cn->size) return false;  // no split

  assert(_back->prefix < cn->size);
  make_back_writable();

  // _brack->branch may have changed in make_back_writable
  cn = _back->branch->compressed();
  // save the prefix of new block
  Slice prefix(_trace.rest_key.data() - _back->prefix, _back->prefix);
  uint8_t key1 = cn->key[_back->prefix];

  // cut compressed of original block
  bsize_t cut_size = _back->prefix + 1;
  bsize_t before = cn->nodesize(), after = cn->nodesize(cn->size - cut_size);
  if (before != after) {
    assert(before > after);
    memmove(_back->branch->data + after, _back->branch->data + before,
            _back->branch->used - before);
    _back->branch->used -= (before - after);
  }

  if (after) {
    cn->size -= cut_size;
    memmove(cn->key, &cn->key[cut_size], cn->size);
  } else
    _back->branch->clear_compressed();

  block_ptr org = _back->branch;

  // create the new branch
  if (_trace.rest_key.size()) {
    uint8_t key2 = _trace.rest_key[0];
    _trace.advance_key(1);

    alloc(Compressed::nodesize(_back->prefix) + ArrayBranch::nodesize(2));
    _back->branch->add_compressed(prefix.data(), prefix.size());
    _back->olink = _back->branch->add_array(key2, key1, org->offset);
    *_back->plink() = _new_leaf_link;
  } else {
    // with empty key
    alloc(Compressed::nodesize(_back->prefix) + ArrayBranch::nodesize(1));
    _back->branch->add_compressed(prefix.data(), prefix.size());
    _back->olink = _back->branch->add_null_leaf();
    *_back->plink() = _new_leaf_link;
    *_back->branch->plink(_back->branch->add_array(key1)) = org->offset;
  }

  _trace.storage.txn.branches++;
  return true;
}

/*
  Split the key of leaf node

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
INLINE bool Inserter::split_leaf() {
  assert(_back->found_leaf != nullptr);

  if (_back->suffix == _back->found_leaf->key_size) return false;  // extend
  assert(_back->suffix < _back->found_leaf->key_size);

  // save the prefix (_trace.rest_key.data()-_back->suffix is safe!)
  Slice prefix(_trace.rest_key.data() - _back->suffix, _back->suffix);
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
  _trace.stack.push(block_ptr{.ptr = 0});
  _back = &_trace.stack.back();
  _back->keypos = _trace.current_key.size() - prefix.size();
  _back->prefix = prefix.size();

  // create new block
  if (_trace.rest_key.size()) {
    uint8_t key1 = _trace.rest_key[0];
    _trace.advance_key(1);
    alloc(Compressed::nodesize(prefix.size()) + ArrayBranch::nodesize(2));
    _back->branch->add_compressed(prefix.data(), prefix.size());
    _back->olink = _back->branch->add_array(key1, key2, _move_leaf_link);
  } else {
    alloc(Compressed::nodesize(prefix.size()) + ArrayBranch::nodesize(1) +
          sizeof(offset_ptr));
    _back->branch->add_compressed(prefix.data(), prefix.size());
    _back->olink = _back->branch->add_null_leaf();
    *_back->branch->plink(_back->branch->add_array(key2)) = _move_leaf_link;
  }

  *_back->plink() = _new_leaf_link;
  _trace.storage.txn.branches++;
  if (!purge_first())
      make_stack_writable();
  return true;
}

/*
  extend the key of leaf node

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
INLINE void Inserter::extend_leaf() {
  assert(_back->found_leaf != nullptr);
  assert(_back->suffix == _back->found_leaf->key_size);
  assert(_trace.rest_key.size() >= 1);

  // save the prefix
  Slice prefix(_trace.rest_key.data() - _back->suffix, _back->suffix);
  _back->suffix = 0;

  // mark the node to move
  _back->branch->leaves_free += _back->found_leaf->nodesize();
  _mvalid = true;
  _mkey = Slice();
  _mvalue = _back->found_leaf->value();

  // extend the stack
  _back->found_leaf = nullptr;
  _back->cmp = 0;
  _trace.stack.push(block_ptr{.ptr = 0});
  _back = &_trace.stack.back();
  _back->keypos = _trace.current_key.size() - prefix.size();
  _back->prefix = prefix.size();

  uint8_t key = _trace.rest_key[0];
  _trace.advance_key(1);

  // create new block
  alloc(Compressed::nodesize(prefix.size()) + ArrayBranch::nodesize(1) +
        sizeof(offset_ptr));
  _back->branch->add_compressed(prefix.data(), prefix.size());
  *_back->branch->plink(_back->branch->add_null_leaf()) = _move_leaf_link;
  _back->olink = _back->branch->add_array(key);
  *_back->plink() = _new_leaf_link;

  _trace.storage.txn.branches++;
  if (!purge_first())
    make_stack_writable();
}

INLINE bool Inserter::purge_first() {
  // purge the first block if is a non canonic block
  // it is recognized by "used == sizeof(offset_ptr)""
  Transition& front = _trace.stack.front();
  if (_trace.stack.size == 2 && front.branch->used == sizeof(offset_ptr)) {
    _trace.storage.free_block(front.leaf);
    _trace.storage.free_block(front.branch);
    front = *_back;
    _trace.storage.txn.root = _trace.root = _back->branch->offset;
    _trace.stack.size = 1;
    assert(_trace.storage.txn.branches > 0);
    _trace.storage.txn.branches--;
    return true;
  }
  return false;
}

// change the value of leaf
INLINE void Inserter::change_leaf() {
  assert(_back->found_leaf);

  make_back_writable();
  assert(_trace.rest_key.empty());
  // manipulate the key managment for grow leaf
  _trace.rest_key =
      Slice(_trace.rest_key.data() - _back->suffix, _back->suffix);
  _trace.current_key.resize(_trace.current_key.size() - _back->suffix);

  grow_leaf();
  *_back->plink() = _new_leaf_link;
}

INLINE offset_ptr Inserter::add_leaf(const Slice& key, const Slice& value) {
  // TODO: Big Value handling
  offset_ptr pos;
  pos.set(LEAF_BLOCK, _back->branch->leaves_used);

  Leaf* l = _back->leaf.leaf()->leaf(pos);
  l->key_size = key.size();
  l->value_size = value.size();
  memcpy(l->key_value, key.data(), l->key_size);
  memcpy(l->key_value + align(l->key_size), value.data(), l->value_size);
  _back->branch->leaves_used += l->nodesize();

  assert(_back->branch->leaves_used <= _back->leaf.leaf()->space());
  return pos;
}

INLINE void Inserter::alloc(bsize_t size) {
  _back->branch = alloc_block(sizeof(BranchBlock) + size);

  lsize_t leaf_size =
      sizeof(LeafBlock) + Leaf::nodesize(_trace.rest_key.size(), _value.size());
  if (_mvalid) leaf_size += Leaf::nodesize(_mkey.size(), _mvalue.size());

  _back->branch->used = 0;
  // _back->leaf = alloc_block(std::max(leaf_size, (lsize_t)600));
  _back->leaf = alloc_block(leaf_size);
  _back->branch->leaves = _back->leaf->offset;
  _back->branch->leaves_free = 0;
  _back->branch->leaves_used = 0;

  _new_leaf_link = add_leaf(_trace.rest_key, _value);
  _back->found_leaf = _back->leaf.leaf()->leaf(_new_leaf_link);
  _back->cmp = 0;
  _trace.advance_key(_trace.rest_key.size());
  if (_trace.stack.size > 1)
    *_trace.stack.data[_trace.stack.size - 2].plink() = _back->branch->offset;
  else
    _trace.storage.txn.root = _trace.root = _back->branch->offset;

  if (_mvalid) _move_leaf_link = add_leaf(_mkey, _mvalue);
}

INLINE bsize_t Inserter::alloc_branch(bsize_t space) {
  if (_back->branch->freespace() < space) {
    block_ptr org = _back->branch;
    assert(_back->branch->offset.size() < BranchBlock::MAX_SIZE);
    _back->branch =
        _trace.storage.alloc_block_by_pool(org->offset.pool_id() + 1);
    _back->branch->copy(org);
    _trace.storage.free_block(org);
    make_stack_writable();
    _grow_branch++;
  } else
    make_back_writable();

  assert(_back->branch->freespace() >= space);
  grow_leaf();
  return space;
}

INLINE void Inserter::grow_leaf() {
  lsize_t leaf_size = Leaf::nodesize(_trace.rest_key.size(), _value.size());

  auto space = _back->branch->leaves_used + leaf_size;
  if (!_back->leaf) _back->leaf = get_block(_back->branch->leaves);

  if (_back->leaf.leaf()->space() < _back->branch->leaves_used + leaf_size) {
    block_ptr nleaf =
        alloc_block(sizeof(LeafBlock) + space - _back->branch->leaves_free);
    _back->branch->copy_leaf(nleaf.leaf(), _back->leaf.leaf());
    _trace.storage.free_block(_back->leaf);
    _back->branch->leaves = nleaf->offset;
    _back->leaf = nleaf;
    _grow_leaf++;
  }

  _new_leaf_link = add_leaf(_trace.rest_key, _value);
  _trace.advance_key(_trace.rest_key.size());
  _back->found_leaf = _back->leaf.leaf()->leaf(_new_leaf_link);
  _back->cmp = 0;
}

INLINE void Inserter::make_back_writable() {
  if (_back->branch->txn_id != _trace.storage.txn.txn_id) {
    _back->branch = _trace.storage.clone_branch(_back->branch);
    make_stack_writable();
  }
}

INLINE void Inserter::make_stack_writable() {
  int i = _trace.stack.size - 2;
  for (; i >= 0; i--) {
    Transition& t = _trace.stack.data[i];
    if (t.branch->txn_id != _trace.storage.txn.txn_id) {
      t.branch = _trace.storage.clone_branch(t.branch);
      *t.plink() = _trace.stack.data[i + 1].branch->offset;
    } else {
      *t.plink() = _trace.stack.data[i + 1].branch->offset;
      break;
    }
  }

  if (i < 0) {
    _trace.storage.txn.root = _trace.root = _trace.stack.front().branch->offset;
  }
}

INLINE Stack::Stack() : size(0) { data.resize(100); }

INLINE void Stack::push(block_ptr block, bsize_t keypos) {
  if (size == data.size()) data.resize(size * 2);
  Transition& back = data[size++];
  back.branch = block;
  back.leaf.reset();
  back.keypos = keypos;
  back.prefix = back.suffix = 0;
  back.olink = 0;
  back.compressed = nullptr;
  back.array = nullptr;
  back.found_leaf = nullptr;
  back.cmp = Transition::UNDEFINED;
}

INLINE Trace::Trace(DBMemory& storage_)
    : storage(storage_), transaction_active(false) {
  root = storage.active_txn()->root;
  cursor_id = storage.alloc_cursor();
}

INLINE Trace::~Trace() { storage.free_cursor(cursor_id); }

INLINE void Trace::_update() {
  // check if there is a new view available
  root = storage.update_cursor(cursor_id);
  if (stack.size && stack.front().branch->offset != root) stack.clear();
}

INLINE bool Trace::_keep_stack() {
  assert(stack.size > 0);

  int cmp;
  size_t same = get_prefix(rest_key.data(), current_key.data(), rest_key.size(),
                           current_key.size(), cmp);
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
  back.cmp = Transition::UNDEFINED;
  return false;
}

INLINE void Trace::find(const Slice& key) {
  rest_key = key;
  if (stack.size && _keep_stack()) return;
  _find();
}

INLINE void Trace::_find() {
  if (!stack.size) {
    if (!root) return;  // empty db
    current_key.clear();
    push(root);
  }

  assert(stack.front().branch->offset == root);
  while (true) {
    if (!stack.back().branch->find(*this)) break;
  }
}

INLINE void Trace::first() {
  if (!root) return
  stack.clear(0);
  rest_key = Slice();
  current_key.clear();
  push(root);
  while(stack.back().branch->first(*this));
}

INLINE void Trace::last() {
  if (!root) return
  stack.clear(0);
  rest_key = Slice();
  current_key.clear();
  push(root);
  while(stack.back().branch->last(*this));
}

INLINE void Trace::next() {
  rest_key.iadvance(rest_key.size());
  while(stack.size) {
    auto old = stack.size;
    if (!stack.back().branch->next(*this)) break;
    if (old < stack.size) {
      while(stack.back().branch->first(*this));
      break;
    }
  }
}

INLINE void Trace::prev() {
  rest_key.iadvance(rest_key.size());
  auto old = stack.size;
  while(stack.size) {
    auto old = stack.size;
    if (!stack.back().branch->prev(*this)) break;
    if (old < stack.size) {
      while(stack.back().branch->last(*this));
      break;
    }
  }
}

INLINE void Trace::remove() {}

INLINE bool Trace::is_valid() const {
  if (stack.size) return stack.back().success();
  return false;
}

INLINE void Trace::set_value(const Slice& value) {
  if (!transaction_active) {
    if (!storage.start_transaction()) throw TransactionActive();
    transaction_active = true;
  }

  if (!stack.size) {
    if (!root) {
      stack.push(block_ptr{.ptr = 0});
      Inserter(*this, value, true).first();
      return;
    }
    throw NoValidPosition();
  }

  Inserter(*this, value).start();
}

INLINE Slice Trace::get_value() {
  const Transition& back = stack.back();
  if (back.cmp) return Slice();
  assert(back.found_leaf);
  return back.found_leaf->value();
}

INLINE void Trace::commit() {
  if (transaction_active) {
    storage.prepare_commit();
    storage.commit();
    transaction_active = false;
    _update();
  }
}

INLINE void Trace::rollback() {
  if (transaction_active) {
    storage.rollback();
    stack.clear();
    transaction_active = false;
    find(current_key);
  }
}

}  // namespace leaves
