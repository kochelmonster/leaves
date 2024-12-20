#include "trace.hpp"

#include <fstream>
#include <iostream>

#include "block.hpp"

namespace leaves {

struct Inserter {
  static const bsize_t OFFSET_LINK2 = offsetof(ArrayBranch, links[1]);
  static const bsize_t OFFSET_LINK1 = offsetof(ArrayBranch, links[0]);

  Trace& _trace;
  const Slice& _value;
  Transition& _back;
  Transition& _last_branch;

  Block* _block;        // the original block
  block_ptr _branch;    // a new branch block
  block_ptr _new_leaf;  // block to hold the new leaf
  block_ptr _old_leaf;  // block to transfer an old leaf
  lsize_t _extra_grow;  // optimation for grow

  Inserter(Trace& trace, const Slice& value)
      : _trace(trace),
        _value(value),
        _back(_trace.stack.back()),
        _block(_back.block),
        _last_branch(_block->isleaf() ? _trace.stack.parent() : _back) {}

  Inserter(Trace& trace, const Slice& value, bool first)
      : _trace(trace),
        _value(value),
        _back(_trace.stack.back()),
        _last_branch(_back) {}

  // start inserting the new value
  void start();

  // insert the very first value
  void first();
  void add_to_null_key();

  bool split_compressed();
  void extend_compressed();
  bool add_to_array(bsize_t ioffset);
  bool add_to_trie(bsize_t ioffset);
  bool split_leaf();
  void extend_leaf();
  void change_leaf();

  // complete the stack if the new leaf is not on branch
  void complete_stack(offset_ptr link);

  // allocs branch, new_leaf, old_leaf
  void alloc(bsize_t branch_size, size_t new_leaf_size, size_t old_leaf_size);

  // extend the stack with the alloced blocks
  void alloc_to_stack(bsize_t branch_size, size_t new_leaf_size,
                      size_t old_leaf_size);

  void remove_leaf(const offset_ptr& link);

  // Alloc space for a branch node
  void alloc_branch(bsize_t space);

  // grows _block
  bool grow_block(lsize_t size, int max_pool);

  // grows src
  block_ptr grow_block(Block* src, lsize_t size, int max_pool);

  /* add the leaf to block, with the necessary allocations
     _new_leaf contains the block of leaf.*/
  offset_ptr add_leaf();

  /* add leaf to _block moving it to an extenal _leaf_block if
     necessary. _new_leaf contains the block of leaf.
  */
  offset_ptr add_leaf(const Slice& key, const Slice& value);

  // Create leaf in dest leaf block must be big enough to contain leaf
  offset_ptr create_leaf(Block* dest);

  // Create leaf in dest leaf block must be big enough to contain leaf
  offset_ptr create_leaf(Block* dest, const Slice& key, const Slice& value);

  block_ptr get_block(offset_ptr ptr) { return _trace.storage.get_block(ptr); }
  block_ptr alloc_block(size_t size) {
    return _trace.storage.alloc_block_size(size);
  }
};

inline void Inserter::start() {
  if (_back.leaf) {
    change_leaf();
    return;
  }

  if (_block->isbranch()) {
    bsize_t ioffset = 0;
    if (_block->has_compressed()) {
      if (split_compressed()) return;
      ioffset += _block->compressed()->nodesize();
    }
    if (_block->has_value()) {
      if (_block->bits & Block::BITS == Block::VALUE) {
        // very special case there is only a value and noting else
        // See first().value_only
        add_to_null_key();
        return;
      }

      ioffset += sizeof(offset_ptr);
    }
    if (_block->has_array()) {
      if (add_to_array(ioffset)) return;
    }
    else if (_block->has_trie()) {
      if (add_to_trie(ioffset)) return;
    }
    else
      extend_compressed();
  }

  if (split_leaf()) return;
  extend_leaf();
}

inline void Inserter::add_to_null_key() {
  Block* block = _back.block;  // _back.block will be changed by first()

  assert(_trace.rest_key.size());
  assert(ArrayBranch::nodesize(1) <= block->freespace());

  _back.index = 0;
  _back.olink = _block->lower_bound - OFFSET_LINK1;
  _trace.current_key.push_back(_trace.rest_key[0]);
  _trace.rest_key.iadvance(1);

  assert(_trace.stack.size == 1);
  _trace.stack.size = 2;

  first();  // repeat the first insert operation

  uint8_t key = _trace.current_key.back();
  block->add_array(_trace.current_key.back(), _block->offset, 0,
                   offset_ptr{.offset = 0});
}

inline void Inserter::first() {
  // don't use _back! (see add_to_null_key)
  Transition& back = _trace.stack.back();

  bsize_t branch_size = Compressed::nodesize(_trace.rest_key.size()) +
                        sizeof(offset_ptr) +
                        // also for _trace.rest_key.empty() reserve enough space
                        // for a future branch_node
                        ArrayBranch::nodesize(1);

  lsize_t leaf_size = Leaf::nodesize(0, _value.size());

  alloc(branch_size, leaf_size, 0);
  _block = _branch;
  back.block = _branch;
  back.offset = _branch->offset;

  back.prefix = _trace.rest_key.size();
  if (back.prefix) {
    _block->add_compressed(back.prefix, _trace.rest_key.data());
    _trace.current_key.append(_trace.rest_key.data(), back.prefix);
    _trace.rest_key.iadvance(back.prefix);
  }
  // else value_only: insert a value with an empty key

  assert(_trace.rest_key.empty());

  back.index = -2;
  back.olink = _block->lower_bound;
  offset_ptr link = create_leaf(_new_leaf);
  _block->add_value(link);

  complete_stack(link);
}

inline block_ptr Inserter::grow_block(Block* src, lsize_t size, int max_pool) {
  assert(src->freespace() < size);
  size += src->used() + _extra_grow;  // try with extra grow
  int pool = get_pool(size + Block::HEADER_SIZE);
  if (pool > max_pool) {
    size -= _extra_grow;
    pool = get_pool(size + Block::HEADER_SIZE);
    if (pool > max_pool) return block_ptr{.ptr = 0};  // cannot grow
  }

  block_ptr result = alloc_block(size);
  result->copy(src);
  return result;
}

inline bool Inserter::grow_block(lsize_t size, int max_pool) {
  block_ptr result = grow_block(_block, size, max_pool);
  if (!result) return false;

  _block = _back.block = result;
  _trace.storage.txn.root = _trace.root = _trace.stack.front().block->offset;

  return true;
}

inline void Inserter::alloc_branch(bsize_t space) {
  assert(_block->isbranch());

  // optimation: grow
  _extra_grow = Leaf::nodesize(_trace.rest_key.size(), _value.size());

  if (_block->freespace() < space) {
    if (!grow_block(space, Block::MAX_BRANCH_POOL)) {
      // could not grow block
      offset_ptr* ppleaf = _block->find_leaf_to_move(space);
      offset_ptr pleaf = *ppleaf;
      Leaf* leaf = _block->leaf(pleaf);
      *ppleaf = add_leaf(leaf->key(), leaf->value());
      int delta = leaf->nodesize();
      // remove the node
      remove_leaf(pleaf);
    }
  }

  assert(_block->freespace() >= space);
  _block->lower_bound += space;  // allocated space
  _extra_grow = 0;
}

inline offset_ptr Inserter::add_leaf() {
  return add_leaf(_trace.rest_key, _value);
}

inline offset_ptr Inserter::add_leaf(const Slice& key, const Slice& value) {
  lsize_t size = Leaf::nodesize(key.size(), value.size());
  if (_block->freespace() > size) {
    // leaf fits in block
    _new_leaf = _back.block;
    return create_leaf(_new_leaf);
  }

  if (grow_block(size, Block::MAX_BRANCH_POOL)) {
    // branch block could be grown to fit the leaf
    _new_leaf = _back.block;
    return create_leaf(_block);
  }

  if (_block->leaves) {
    _new_leaf = get_block(_block->leaves);
    if (_new_leaf->freespace() > size) {
      // enough space in _leaves
      return create_leaf(_new_leaf, key, value);
    }

    block_ptr gblock = grow_block(_new_leaf, size, Block::MAX_LEAF_POOL);
    if (gblock) {
      // _leaves block could be grown to fit
      _block->change_leaf_blocks(_block->leaves, gblock->offset);
      _block->leaves = gblock->offset;
      _new_leaf = gblock;
      return create_leaf(_new_leaf, key, value);
    }
  }

  // start a new leaves
  _new_leaf = alloc_block(size + _extra_grow);
  _block->leaves = _new_leaf->offset;
  return create_leaf(_new_leaf, key, value);
}

inline offset_ptr Inserter::create_leaf(Block* dest) {
  return create_leaf(dest, _trace.rest_key, _value);
}

inline offset_ptr Inserter::create_leaf(Block* dest, const Slice& key,
                                        const Slice& value) {
  size_t size = Leaf::nodesize(key.size(), value.size());
  assert(dest->freespace() >= size);
  dest->upper_bound -= size;

  Leaf* leaf = (Leaf*)&dest->data[dest->upper_bound];
  leaf->key_size = key.size();
  leaf->value_size = value.size();
  memcpy(leaf->key_value, key.data(), leaf->key_size);
  memcpy(leaf->key_value + leaf->key_size, value.data(), leaf->value_size);

  return offset_ptr{
      .pool_id = dest->offset.pool_id,
      .offset = dest->offset.start() + dest->space() - dest->upper_bound};
}

// allocs a new branch block, a necessary space for new leaf and a old leaf
// (old leaf is a leaf moved from another node)
inline void Inserter::alloc(bsize_t branch_size, size_t new_leaf_size,
                            size_t old_leaf_size) {
  size_t size;
  do {
    if ((size = branch_size + new_leaf_size + old_leaf_size) <
        Block::MAX_BRANCH_SPACE) {
      _branch = _new_leaf = _old_leaf = alloc_block(size);
      break;
    }

    if ((size = branch_size + old_leaf_size) < Block::MAX_BRANCH_SPACE) {
      _branch = _old_leaf = alloc_block(size);
      _new_leaf = alloc_block(new_leaf_size);
      _branch->leaves = _new_leaf->offset;
      break;
    }

    if ((size = branch_size + new_leaf_size) < Block::MAX_BRANCH_SPACE) {
      _branch = _new_leaf = alloc_block(size);
      _old_leaf = alloc_block(old_leaf_size);
      break;
    }

    _branch = alloc_block(branch_size);
    _new_leaf = alloc_block(new_leaf_size);
    _old_leaf = alloc_block(old_leaf_size);
  } while (0);
}

inline void Inserter::alloc_to_stack(bsize_t branch_size, size_t new_leaf_size,
                                     size_t old_leaf_size) {
  alloc(branch_size, new_leaf_size, old_leaf_size);
  *_last_branch.plink() = _branch->offset;
  if (_block->isleaf()) {
    _back.block = _branch;
    _back.offset = _branch->offset;
  } else {
    _trace.stack.push(_branch->offset, _branch);
  }
}

inline void Inserter::complete_stack(offset_ptr link) {
  if (_new_leaf->isleaf()) {
    // add the leaf block to stack
    _trace.stack.push(_new_leaf->offset, _new_leaf);
    Transition& back = _trace.stack.back();
    back.offset = link;
    back.keypos = _trace.current_key.size();
    back.suffix = _trace.rest_key.size();
    back.leaf = _new_leaf->leaf(link);
  } else {
    Transition& back = _trace.stack.back();
    back.leaf = _new_leaf->leaf(link);
    back.suffix = _trace.rest_key.size();
  }
  _trace.current_key.append(_trace.rest_key.data(), _trace.rest_key.size());
  _trace.rest_key.iadvance(_trace.rest_key.size());
}

struct StackTrieBranch : public TrieBranch {
  static const bsize_t MIN_TRIE = sizeof(TrieBranch) + 16 * sizeof(offset_ptr);
  uint8_t padding[MIN_TRIE - sizeof(TrieBranch)];  // exten
} trie;

/*
  Add leaf to an arraybranch
*/
inline bool Inserter::add_to_array(bsize_t ioffset) {
  if (_back.index >= 0) return false;

  assert(_trace.rest_key.size());

  uint8_t c = _trace.rest_key[0];
  _trace.current_key.push_back(c);
  _trace.rest_key.iadvance(1);
  offset_ptr leaf;

  ArrayBranch* n = (ArrayBranch*)&_block->data[ioffset];
  if (n->size < 15) {
    alloc_branch(sizeof(offset_ptr));
    leaf = add_leaf();
    assert(_block->freespace() >= sizeof(offset_ptr));
    n = (ArrayBranch*)&_block->data[ioffset];  // _block may have changed
    n->keys[n->size] = c;
    n->links[n->size] = leaf;
    n->size++;
  } else {
    // change ArrayBranch to Trie branch
    const bsize_t MAX_ARRAY = sizeof(ArrayBranch) + 15 * sizeof(offset_ptr);
    StackTrieBranch tmp;
    alloc_branch(sizeof(tmp) - MAX_ARRAY);
    leaf = add_leaf();
    n = (ArrayBranch*)&_block->data[ioffset];  // _block may have changed
    for (int i = 0; i < n->size; i++) {
      tmp.set(n->keys[i]);
    }
    tmp.set(c);

    for (int i = 0; i < n->size; i++)
      tmp.links[tmp.index(n->keys[i])] = n->links[i];

    tmp.links[tmp.index(c)] = leaf;

    memcpy(n, &tmp, sizeof(tmp));
  }

  complete_stack(leaf);
  return true;
}

inline bool Inserter::add_to_trie(bsize_t ioffset) {
  assert(_trace.rest_key.size());
  TrieBranch* n = (TrieBranch*)&_block->data[ioffset];
  if (n->bits[_back.tindex.idx] & (1 << _back.tindex.bit)) return false;

  uint8_t c = _trace.rest_key[0];
  _trace.current_key.push_back(c);
  _trace.rest_key.iadvance(1);
  alloc_branch(sizeof(offset_ptr));
  offset_ptr leaf = add_leaf();  // could change n
  n = (TrieBranch*)&_block->data[ioffset];
  n->set(c);
  int index = n->index(c);
  memmove(&n->links[index + 1], &n->links[index],
          (n->count() - index - 1) * sizeof(offset_ptr));
  n->links[index] = leaf;
  complete_stack(leaf);
  return true;
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
inline bool Inserter::split_compressed() {
  Compressed* cn = _block->compressed();

  if (_back.prefix == cn->size)
    // no split
    return false;

  assert(_back.prefix < cn->size);
  bool empty_key = _trace.rest_key.empty();

  // create new node
  // ---------------
  bsize_t branch_size =
      Compressed::nodesize(_back.prefix) +
      (empty_key ? sizeof(offset_ptr) : 0)  // Special case empty key
      + ArrayBranch::nodesize(empty_key ? 1 : 2);

  bsize_t rest_key_size = empty_key ? 0 : _trace.rest_key.size() - 1;
  size_t leaf_size = Leaf::nodesize(rest_key_size, _value.size());
  offset_ptr org_ptr = _block->offset;

  if (!(_block->has_array() || _block->has_trie())) {
    assert(_block->has_value());

    // compress the orginal block to a leaf
    offset_ptr* pleaf =
        (offset_ptr*)&_block->data[_block->lower_bound - sizeof(offset_ptr)];
    block_ptr lblock = get_block(*pleaf);
    Leaf* oleaf = lblock->leaf(*pleaf);
    bsize_t osize =
        Leaf::nodesize(cn->size - _back.prefix - 1, oleaf->value_size);
    alloc(branch_size, leaf_size, osize);

    org_ptr = create_leaf(
        _old_leaf,
        Slice(cn->key + _back.prefix + 1, cn->size - _back.prefix - 1),
        oleaf->value());
  } else
    alloc(branch_size, leaf_size, 0);

  assert(_back.suffix == 0);

  offset_ptr link = {.offset = 0};
  uint8_t key2;
  if (_back.prefix) {
    _branch->add_compressed(_back.prefix, cn->key);
  }
  if (empty_key) {
    _back.olink = _branch->lower_bound;
    _branch->add_value(create_leaf(_new_leaf));
    _back.index = -2;
  } else {
    _trace.current_key.push_back(_trace.rest_key[0]);
    _trace.rest_key.iadvance(1);
    key2 = _trace.current_key.back();
    link = create_leaf(_new_leaf);
    _back.index = 1;
    _back.olink = _branch->lower_bound + OFFSET_LINK2;
  }

  // create the branch of new_block
  _branch->add_array(cn->key[_back.prefix], org_ptr, key2, link);

  if (org_ptr == _block->offset) {
    // cut compressed of original block
    cn->size -= (_back.prefix + 1);
    _block->lower_bound -= (_back.prefix + 1);
    if (cn->size) {
      memmove(cn->key, &cn->key[_back.prefix + 1],
             _block->lower_bound - sizeof(cn->size));
    } else {
      _block->lower_bound -= sizeof(cn->size);
      memmove(_block->data, &cn->key[_back.prefix + 1],
             _block->lower_bound);
      _block->clear_compressed();
    }
  } else  // org block not needed anymore
    _trace.storage.free_block(_back.block);

  _back.block = _branch;
  _back.offset = _branch->offset;

  complete_stack(*_back.plink());

  _trace.storage.txn.root = _trace.root = _trace.stack.front().block->offset;
  return true;
}

inline void Inserter::extend_compressed() {
  assert(!_block->has_array());
  assert(!_block->has_trie());
  assert(_block->has_value());
  assert(_block->has_compressed());
  assert(!_block->leaves);
  assert(_trace.rest_key.size());

  uint8_t key = _trace.rest_key[0];
  _trace.current_key.push_back(key);
  _trace.rest_key.iadvance(1);

  alloc_branch(ArrayBranch::nodesize(1));

  _block->add_array(key, add_leaf(), 0, offset_ptr{.offset = 0});
  complete_stack(*_back.plink());
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
inline bool Inserter::split_leaf() {
  offset_ptr leaf_link = *_last_branch.plink();
  Leaf* leaf = _block->leaf(leaf_link);
  bsize_t keypos = _trace.current_key.size();
  bool empty_key = _trace.rest_key.empty();

  if (_back.suffix == leaf->key_size) return false;
  assert(_back.suffix < leaf->key_size);

  // create new node
  // ---------------
  bsize_t branch_size =
      Compressed::nodesize(_back.suffix) +
      (empty_key ? sizeof(offset_ptr) : 0)  // Special case empty key
      + ArrayBranch::nodesize(empty_key ? 1 : 2);

  bsize_t orest_key_size = leaf->key_size - _back.suffix - 1;
  size_t old_leaf_size = Leaf::nodesize(orest_key_size, leaf->value_size);

  bsize_t nrest_key_size = empty_key ? 0 : _trace.rest_key.size() - 1;
  size_t new_leaf_size = Leaf::nodesize(nrest_key_size, _value.size());

  alloc_to_stack(branch_size, new_leaf_size, old_leaf_size);

  Transition& back = _trace.stack.back();
  back.keypos = _trace.current_key.size() - _back.suffix;
  back.prefix = _back.suffix;
  _back.suffix = 0;

  offset_ptr link = {.offset = 0};
  uint8_t key2;
  if (back.prefix) {
    _branch->add_compressed(back.prefix, leaf->key_value);
  }

  if (empty_key) {
    back.olink = _branch->lower_bound;
    back.index = -2;
    _branch->add_value(create_leaf(_new_leaf));
  } else {
    _trace.current_key.push_back(_trace.rest_key[0]);
    _trace.rest_key.iadvance(1);
    key2 = _trace.current_key.back();
    link = create_leaf(_new_leaf);
    back.index = 1;
    back.olink = _branch->lower_bound + OFFSET_LINK2;
  }
  // create the branch of new_block
  _branch->add_array(
      leaf->key_value[back.prefix],
      create_leaf(_old_leaf, leaf->key().advance(back.prefix + 2),
                  leaf->value()),
      key2, link);

  complete_stack(*back.plink());
  remove_leaf(leaf_link);
  return true;
}

inline void Inserter::remove_leaf(const offset_ptr& link) {
  assert(link.start() == _block->offset.start());
  Leaf* leaf = _block->leaf(link);
  int nsize = leaf->nodesize();
  lsize_t bytes = (uint8_t*)leaf - (_block->data + _block->upper_bound);
  memmove(_block->data + _block->upper_bound + nsize,
          _block->data + _block->upper_bound, bytes);

  _block->upper_bound += nsize;
  if (_block->used() == 0) {
    _trace.storage.free_block(_back.block);
  } else {
    _last_branch.block->move_leaf_ioffsets(link, nsize);
  }
}

// change the value of leaf
inline void Inserter::change_leaf() {
  offset_ptr leaf_link = *_last_branch.plink();
  Leaf* leaf = _block->leaf(leaf_link);
  int delta = Leaf::nodesize(0, _value.size()) - leaf->nodesize();

  // new value does not fit into block
  if (_value.size() > Leaf::BIG_VAL_SIZE &&
      leaf->value_size < Leaf::BIG_VAL_SIZE) {
    // Move leaf to an own block
    _new_leaf = alloc_block(Leaf::nodesize(leaf->key_size, _value.size()));
    *_last_branch.plink() =
        create_leaf(_new_leaf, Slice(leaf->key_value, leaf->key_size), _value);
  }
  // TODO: BIGVALUE Handling

  if (_block->freespace() >= delta && delta) {
    lsize_t bytes = (uint8_t*)leaf - &_block->data[_block->upper_bound];
    memmove(_block->data + _block->upper_bound - delta,
            _block->data + _block->upper_bound, bytes);
    _block->upper_bound -= delta;
    _last_branch.block->move_leaf_ioffsets(leaf_link, delta);
    leaf_link = *_last_branch.plink();
    _back.leaf = leaf = _block->leaf(leaf_link);
    leaf->value_size = _value.size();
  }

  memcpy(leaf->key_value + leaf->key_size, _value.data(), _value.size());
  if (_block->isleaf()) {
    _back.offset = leaf_link;
  }
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
inline void Inserter::extend_leaf() {
  offset_ptr leaf_link = *_last_branch.plink();
  Leaf* leaf = _block->leaf(leaf_link);
  assert(_back.suffix == leaf->key_size);
  assert(_trace.rest_key.size() >= 1);

  // create new node
  // ---------------
  bsize_t branch_size = Compressed::nodesize(_back.suffix) +
                        sizeof(offset_ptr) + ArrayBranch::nodesize(1);

  size_t old_leaf_size = Leaf::nodesize(0, leaf->value_size);
  size_t new_leaf_size =
      Leaf::nodesize(_trace.rest_key.size() - 1, _value.size());

  alloc_to_stack(branch_size, new_leaf_size, old_leaf_size);

  Transition& back = _trace.stack.back();
  back.keypos = _trace.current_key.size() - _back.suffix;
  back.prefix = _back.suffix;
  _back.suffix = 0;

  _branch->add_compressed(leaf->key_size, leaf->key_value);
  offset_ptr link = create_leaf(_old_leaf, Slice(), leaf->value());
  _branch->add_value(link);

  _trace.current_key.push_back(_trace.rest_key[0]);
  _trace.rest_key.iadvance(1);

  back.olink = _branch->lower_bound + OFFSET_LINK1;
  back.index = 0;

  // create the branch of new_block
  _branch->add_array(_trace.current_key.back(), create_leaf(_new_leaf), 0,
                     offset_ptr{.offset = 0});

  complete_stack(link);
  remove_leaf(leaf_link);
}

INLINE Stack::Stack() : size(0) { data.resize(100); }

INLINE void Stack::push(const offset_ptr& offset, block_ptr block) {
  if (size == data.size()) data.resize(size * 2);
  Transition& back = data[size++];
  back.block = block;
  back.offset = offset;
  back.keypos = 0;
  back.index = -2;
  back.prefix = back.suffix = 0;
  back.olink = 0;
  back.leaf = nullptr;
}

INLINE Trace::Trace(DBMemory& storage_)
    : storage(storage_), transaction_active(false) {
  root = storage.active_txn()->root;
  cursor_id = storage.alloc_cursor();
  current_key.reserve(1024);
}

INLINE Trace::~Trace() { storage.free_cursor(cursor_id); }

INLINE void Trace::_update() {
  // check if there is a new view available
  root = storage.update_cursor(cursor_id);
  if (stack.size && stack.front().block->offset != root) stack.clear();
}

INLINE void Trace::_keep_stack() {
  assert(stack.size > 0);

  size_t size = std::min(rest_key.size(), current_key.size());
  size_t same = 0;
  for (; same < size; same++) {
    if (rest_key[same] != current_key[same]) break;
  }
  int i = 0;
  int keep = 0;

  for (; i < stack.size && stack.data[i].keypos <= same; i++) {
    Transition& item = stack.data[i];
    keep = item.keypos;
  }

  rest_key.iadvance(keep);
  current_key.resize(keep);
  stack.clear(i);
}

INLINE void Trace::find(const Slice& key) {
  rest_key = key;
  if (stack.size) _keep_stack();
  if (rest_key.size() || !stack.size) _find();
}

INLINE void Trace::_find() {
  if (!stack.size) {
    if (!root) return;  // empty db
    current_key.clear();
    push(root);
  }

  while (true) {
    Transition& b = stack.back();
    b.keypos = current_key.size();
    if (!b.block->find(*this)) break;
  }
}

INLINE void Trace::first() {}

INLINE void Trace::last() {}

INLINE void Trace::next() {}

INLINE void Trace::prev() {}

INLINE void Trace::remove() {}

INLINE bool Trace::is_valid() const {
  if (stack.size) return stack.back().leaf != nullptr;
  return false;
}

INLINE void Trace::_make_writable() {
  Transition& back = stack.back();
  if (back.block->txn_id != storage.txn.txn_id) {
    // not writeable -> make stack writable
    offset_ptr old_offset = back.block->offset;
    offset_ptr child_offset;
    back.block = storage.clone_cow_block(back.block);

    int i = stack.size - 2;
    if (back.block->isleaf()) {
      assert(i >= 0);
      // block is a leaf block => there can be muliple offsets
      // pointing to this block all of them must be changed.
      Transition& item = stack.data[i--];
      if (item.block->txn_id != storage.txn.txn_id) {
        offset_ptr old_offset = item.block->offset;
        item.block = storage.clone_cow_block(item.block);
        item.block->change_leaf_blocks(old_offset, item.block->offset);
        child_offset = item.block->offset;
      }
      item.block->change_leaf_blocks(old_offset, back.block->offset);
    } else {
      child_offset = back.block->offset;
      back.block->change_leaf_blocks(old_offset, back.block->offset);
    }

    bool go_on = true;
    for (; i >= 0 && go_on; i--) {
      Transition& item = stack.data[i];
      if (go_on = (item.block->txn_id != storage.txn.txn_id)) {
        offset_ptr old_offset = item.block->offset;
        item.block = storage.clone_cow_block(item.block);
        item.block->change_leaf_blocks(old_offset, item.block->offset);
      }

      *item.plink() = child_offset;
      child_offset = item.block->offset;
    }

    if (i < 0) root = storage.txn.root = child_offset;
  }
}

INLINE void Trace::set_value(const Slice& value) {
  if (!transaction_active) {
    if (!storage.start_transaction()) throw TransactionActive();
    transaction_active = true;
  }

  if (!stack.size) {
    if (!root) {
      stack.push(root, block_ptr{.ptr = 0});
      Inserter(*this, value, true).first();
      root = storage.txn.root = stack.back().offset;
      return;
    }
    throw NoValidPosition();
  }

  _make_writable();
  Inserter(*this, value).start();
}

INLINE Slice Trace::get_value() {
  const Transition& back = stack.back();
  if (!back.leaf) return Slice();
  return back.leaf->value();
}

INLINE void Trace::commit() {
  storage.prepare_commit();
  storage.commit();
  transaction_active = false;
  _update();
}

INLINE void Trace::rollback() {
  storage.rollback();
  stack.clear();
  transaction_active = false;
  find(current_key);
}

}  // namespace leaves
