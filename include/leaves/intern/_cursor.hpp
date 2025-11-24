#ifndef _LEAVES_ICURSOR_HPP
#define _LEAVES_ICURSOR_HPP

#include <string>
#include <vector>

#include "_deleter.hpp"
#include "_exception.hpp"
#include "_inserter.hpp"
#include "_node.hpp"

namespace leaves {

template <typename Cursor_>
struct _Transition {
  typedef Cursor_ Cursor;
  using Traits = typename Cursor::Traits;
  typedef _Transition<Cursor> Transition;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;

  static const int NOT_FOUND = 2;  // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  Cursor* cursor;
  block_ptr block;

  trie_ptr& trie() const { return *(trie_ptr*)&block; }

  leaf_ptr& leaf() const { return *(leaf_ptr*)&block; }

  uint16_t prefix;     // count of equal chars in compressed node
  uint16_t keypos;     // position inside the key
  uint8_t branch_key;  // The char branching the

  // 1: the key to find is bigger than the found node
  // 0: the key is found
  // -1: the key to find is smaller than the found node
  // NOT_FOUND: it is not equal but not known if -1 or 1
  // UNDEFINED: not tested
  int cmp;

  offset_t offset;
  uint16_t link_offset;

  offset_e* link() {
    assert(link_offset != 0xFFFF);
    return (offset_e*)(trie().link(link_offset));
  }

  //_Transition() : trie(*(trie_ptr*)&block), leaf(*(leaf_ptr*)&block) {}

  bool is_leaf() const { return offset.type() == LEAF; }
  bool is_trie() const { return offset.type() == TRIE; }

  bool success() const { return cmp == 0 && is_leaf(); }

  bool init(Cursor* cursor_, offset_t offset_, uint16_t keypos_ = 0) {
    cursor = cursor_;
    keypos = keypos_;
    prefix = 0;
    cmp = Transition::UNDEFINED;
    offset = offset_;
    link_offset = 0xFFFF;
    block = resolve(offset);
    return true;  // the caller shall set the trie root
  }

  block_ptr resolve(offset_t offset) { return cursor->_db->resolve(offset); }

  void set_root(offset_t offset_) { Traits::set_root(cursor->_txn, offset_); }

  void replace(offset_t offset_) {
    offset = offset_;
    if (!is_root())
      *parent().update() = offset_;
    else
      set_root(offset);
  }

  offset_e* update() {
    if constexpr (!Cursor::DB::Traits::TRANSACTIONAL) {
      cursor->_db->make_dirty(block);
      return link();
    }

    if (block->txn_id == cursor->_txn->txn_id) {
      assert(cursor->_db->transaction_active());
      cursor->_db->make_dirty(block);
      return link();
    }

    assert(is_trie());
    trie() = cursor->_db->cow(trie());
    offset = cursor->_db->resolve(trie());
    assert(trie()->count() < trie()->MAX_BRANCH_COUNT);

    if (!is_root())
      *parent().update() = offset;
    else
      set_root(offset);

    return link();
  }

  void advance_key(uint16_t count) { cursor->advance_key(count); }

  void append_key(const uint8_t* data, size_t size) {
    cursor->current_key.append((const char*)data, size);
  }

  void resize_key(size_t size) { cursor->current_key.resize(size); }

  Transition& push(const offset_e* lnk) {
    link_offset = (char*)lnk - (char*)block;
    cursor->push(*lnk);
    return cursor->stack.back();
  }

  Transition& push(offset_e lnk) {
    cursor->push(lnk);
    return cursor->stack.back();
  }

  void pop() { cursor->pop(); }

  void reset() {
    block.reset();
    cmp = UNDEFINED;
  }

  Slice& key() { return cursor->rest_key; }

  Slice value() const {
    assert(is_leaf());
    return leaf()->value(*cursor->_db);
  }

  std::string& current_key() { return cursor->current_key; }

  Transition& child() {
    assert(static_cast<size_t>(this - &cursor->stack.data[0]) <
           cursor->stack.size - 1);
    return this[1];
  }

  Transition& parent() {
    assert(this - &cursor->stack.data[0] > 0);
    return this[-1];
  }

  bool is_root() const { return this - &cursor->stack.data[0] == 0; }

  void find() {
    // find the next node
    if (is_leaf()) {
      LeafNode& leaf_ = *leaf();
      prefix = get_prefix(key().data(), (char*)leaf_.data, key().size(),
                          leaf_.key_size, cmp);
      advance_key(prefix);
      return;
    }

    TrieNode& trie_ = *trie();
    assert(trie_.count() < trie_.MAX_BRANCH_COUNT);
    prefix = get_prefix(key().data(), (char*)trie_.compressed(), key().size(),
                        trie_.len(), cmp);
    advance_key(prefix);
    if (prefix < trie_.len()) return;

    if (key().empty()) {
      if (trie_.has_none()) {
        push(trie_.offset(TrieNode::NONE));
        child().find();
      } else
        cmp = -1;
      return;
    }

    branch_key = key()[0];
    if (!trie_.isset(branch_key)) {
      cmp = NOT_FOUND;
      return;
    }
    cmp = 0;
    push(trie_.offset(branch_key));
    child().find();
  }

  void leaf_step() {
    assert(is_leaf());
    LeafNode& leaf_ = *leaf();
    resize_key(keypos);
    append_key(leaf_.data, leaf_.key_size);
    prefix = leaf_.key_size;
    cmp = 0;
  }

  void first() {
    if (is_leaf()) return leaf_step();
    TrieNode& trie_ = *trie();
    append_key(trie_.compressed(), trie_._compressed_len);
    cmp = 0;
    push(trie_.array()).first();
  }

  bool next() {
    if (is_leaf()) {
      if (cmp < 0) {
        leaf_step();
        return true;
      }
      return false;
    }

    TrieNode& trie_ = *trie();
    if (cmp == 0) {
      link_offset += sizeof(offset_e);
      offset_e* lnk = link();
      offset_e* end = trie_.array() + trie_.count();
      if (lnk >= end) return false;

      if (lnk + 1 < end) {
        cursor->_db->prefetch(*(lnk + 1));
      }
      auto& child = push(lnk);
      cursor->_db->prefetch(*lnk); 

      child.first();
      branch_key = current_key()[child.keypos];
      return true;
    }

    resize_key(keypos);
    if (prefix < trie_._compressed_len) {
      if (cmp > 0) return false;
      assert(cmp < 0);
      first();
      return true;
    }
    append_key(trie_.compressed(), trie_._compressed_len);
    int next_ = trie_.next(branch_key);
    if (next_ == TrieNode::OUT_OF_RANGE) return false;
    const offset_e* next_offset = trie_.offset(next_);
    branch_key = (uint8_t)next_;
    push(next_offset).first();
    return true;
  }

  bool prev() {
    if (is_leaf()) {
      if (cmp > 0) {
        leaf_step();
        return true;
      }
      return false;
    }

    TrieNode& trie_ = *trie();
    if (cmp == 0) {
      link_offset -= sizeof(offset_e);
      offset_e* lnk = link();
      offset_e* begin = trie_.array();
      if (lnk < begin) return false;

      if (lnk - 1 >= begin) {
        cursor->_db->prefetch(*(lnk - 1));
      }
      auto& child = push(lnk);
      cursor->_db->prefetch(*lnk); 

      child.last();
      branch_key = current_key()[child.keypos];
      return true;
    }

    resize_key(keypos);
    if (prefix < trie_._compressed_len) {
      if (cmp < 0) return false;
      assert(cmp > 0);
      last();
      return true;
    }
    append_key(trie_.compressed(), trie_._compressed_len);
    int prev_ = trie_.prev(branch_key);
    if (prev_ == TrieNode::OUT_OF_RANGE) return false;
    branch_key = (uint8_t)prev_;
    push(trie_.offset(prev_)).last();
    return true;
  }

  void last() {
    if (is_leaf()) return leaf_step();
    TrieNode& trie_ = *trie();
    append_key(trie_.compressed(), trie_._compressed_len);
    cmp = 0;
    push(trie_.array() + trie_.count() - 1).last();
  }
};

template <typename Cursor>
struct _Stack {
  typedef _Stack<Cursor> Stack;
  typedef _Transition<Cursor> Transition;
  typedef std::vector<Transition> stack_v;
  stack_v data;
  size_t size;

  _Stack() : size(0) { data.resize(100); }

  void push(Cursor* cursor, offset_t offset, uint16_t keypos = 0) {
    if (size == data.size()) data.resize(size * 2);
    data[size].init(cursor, offset, keypos);
    size++;
  }

  Transition& front() { return data[0]; }
  Transition& back() { return data[size - 1]; }
  const Transition& back() const { return data[size - 1]; }

  Transition& parent() {
    assert(size > 1);
    return data[size - 2];
  }

  void clear(size_t size_ = 0) {
    for (size_t i = size_; i < size; i++) {
      data[i].reset();
    }
    size = size_;
  }
};

// Base cursor with stack and core navigation functionality
template <typename DB_, typename Traits_>
struct _CursorBase {
  typedef DB_ DB;
  typedef Traits_ Traits;
  typedef _CursorBase<DB, Traits_> CursorBase;
  typedef _Stack<CursorBase> Stack;
  using db_ptr = typename Traits::db_ptr;
  using txn_ptr = typename DB::txn_ptr;
  using Transition = typename Stack::Transition;

  db_ptr _db;
  txn_ptr _txn;
  Stack stack;
  Slice rest_key;
  std::string current_key;

  _CursorBase() = default;
  _CursorBase(db_ptr db) : _db(db) { current_key.reserve(128); }

  void advance_key(size_t size) {
    current_key.append(rest_key.data(), size);
    rest_key.iadvance(size);
  }

  void push(offset_t ptr) { stack.push(this, ptr, current_key.size()); }

  void pop() {
    assert(stack.size > 0);
    current_key.resize(stack.back().keypos);
    stack.size--;
  }
};

// Full cursor with find, transactions, and modification operations
template <typename DB_, typename Traits_>
struct _Cursor : public _CursorBase<DB_, Traits_> {
  typedef DB_ DB;
  typedef Traits_ Traits;
  typedef _Cursor<DB, Traits_> Cursor;
  typedef _CursorBase<DB, Traits_> CursorBase;
  using db_ptr = typename Traits::db_ptr;
  using txn_ptr = typename DB::txn_ptr;
  using Transition = typename CursorBase::Transition;
  using Stack = typename CursorBase::Stack;
  using Hasher = typename Traits::Hasher;
  using hash_t = typename Hasher::hash_t;

  static constexpr size_t MAX_KEY_SIZE = Traits::MAX_KEY_SIZE;
  uint64_t _id{0};
  std::string _refind_buffer;

  _Cursor(db_ptr db) : CursorBase(db) {
    if constexpr (Traits::TRANSACTIONAL) {
      _id = this->_db->new_cursor_id();
      update();
    }
  }

  ~_Cursor() {
    if constexpr (Traits::TRANSACTIONAL) {
      if (is_transaction_active()) {
        if (this->_txn) this->_txn->refs.fetch_sub(1);
      }
    }
  }

  bool is_transaction_active() const {
    if constexpr (Traits::TRANSACTIONAL) {
      return this->_db->txn_cursor_id() == _id;
    }
    return false;
  }

  // return true if the cursor is on a valid position
  bool is_valid() const {
    return this->stack.size ? this->stack.back().success() : false;
  }

  void find(const Slice& key) {
    if (key.size() > MAX_KEY_SIZE) throw KeyTooBig();
    this->rest_key = key;
    if (this->stack.size && keep_stack()) return;
    _find();
  }

  bool _prepare_move() {
    this->stack.clear(0);
    if (! this->_txn) return true;
    auto root = Traits::get_root(this->_txn);
    if (!root) return true;
    this->rest_key.reset();
    this->current_key.clear();
    this->push(root);
    return false;
  }

  void first() {
    if (_prepare_move()) return;
    this->stack.back().first();
  }

  void last() {
    if (_prepare_move()) return;
    this->stack.back().last();
  }

  void next() {
    this->rest_key.reset();
    while (this->stack.size) {
      if (this->stack.back().next()) return;
      this->pop();
    }
  }

  void prev() {
    this->rest_key.reset();
    while (this->stack.size) {
      if (this->stack.back().prev()) return;
      this->pop();
    }
  }

  void* reserve(size_t size) {
    [[maybe_unused]] bool r = start_transaction();
    assert(r);

    if (!this->stack.size) {
      if (!Traits::get_root(this->_txn)) {
        this->push(offset_t());
        _Inserter(&this->stack.back(), size).first_exec();
        return (void*)this->stack.back().value().data();
      }
      throw NoValidPosition();
    }

    if (!_Inserter(&this->stack.back(), size).exec()) return nullptr;
    return (void*)this->stack.back().value().data();
  }

  void value(const Slice& value) {
    void* space = reserve(value.size());
    assert(space);
    memcpy(space, value.data(), value.size());
    this->_db->flush();
  }

  Slice value() const {
    const Transition& back = this->stack.back();
    return back.cmp == 0 && back.is_leaf() ? back.value() : Slice();
  }

  Slice key() const { return this->current_key; }

  void remove() {
    if (!is_valid()) throw NoValidPosition();
    [[maybe_unused]] bool r = start_transaction();
    assert(r);
    _Deleter(*this).exec();
  }

  bool start_transaction(bool non_blocking = false) {
    if constexpr (Traits::TRANSACTIONAL) {
      if (this->_db->txn_cursor_id() != _id) {
        txn_ptr new_txn = this->_db->start_transaction(_id, non_blocking);
        if (!new_txn) return false;
        assert(new_txn->refs.load() == 0);  // no one can reference it yet
        _set_txn(new_txn);
      }
    }
    return true;
  }

  tid_t prepare_commit(bool sync = false) {
    if constexpr (Traits::TRANSACTIONAL) {
      return this->_db->prepare_commit(_id, sync);
    }
    return tid_t(0);
  }

  bool commit(bool sync = false) {
    if constexpr (Traits::TRANSACTIONAL) {
      return this->_db->commit(_id, sync);
    }
    return false;
  }

  bool rollback() {
    if constexpr (Traits::TRANSACTIONAL) {
      if (this->_db->rollback(_id)) {
        this->stack.clear();
        find(this->current_key);
        return true;
      }
    }
    return false;
  }

  /* Helpers */

  bool keep_stack() {
    // Don't start from the very start. Try to start as deep in the stack as
    // possible
    assert(this->stack.size > 0);

    int cmp;
    size_t same =
        get_prefix(this->rest_key.data(), this->current_key.data(),
                   this->rest_key.size(), this->current_key.size(), cmp);
    if (same == this->rest_key.size() && same == this->current_key.size()) {
      // already found
      this->rest_key.iadvance(same);
      return true;
    }

    size_t i = 0;
    for (; i < this->stack.size; i++) {
      Transition& item = this->stack.data[i];
      if (item.keypos >= same) break;
    }

    this->stack.clear(i);
    if (!this->stack.size) return false;

    Transition& back = this->stack.back();
    back.prefix = 0;
    back.cmp = Transition::UNDEFINED;
    this->rest_key.iadvance(back.keypos);
    this->current_key.resize(back.keypos);
    return false;
  }

  void _find() {
    if (!this->stack.size) {
      auto root = Traits::get_root(this->_txn);
      if (!root) return;  // empty db
      this->current_key.clear();
      this->push(root);
    }
    this->stack.back().find();
  }

  void update() {
    if constexpr (Traits::TRANSACTIONAL) {
      auto new_txn = this->_db->txn();
      assert(new_txn);
      if (!this->_txn || new_txn->txn_id > this->_txn->txn_id) {
        _set_txn(new_txn);
      }
    }
  }

  void _set_txn(txn_ptr& txn) {
    if constexpr (Traits::TRANSACTIONAL) {
      assert(txn);
      if (this->_txn != txn) {
        offset_t old_root;
        if (this->_txn) {
          this->_txn->refs.fetch_sub(1);
          old_root = Traits::get_root(this->_txn);
        }
        this->_txn = txn;
        this->_txn->refs.fetch_add(1);
        if ((this->current_key.size() || this->rest_key.size()) &&
            old_root != Traits::get_root(this->_txn)) {
          // adjust to new root
          this->stack.clear();
          _refind_buffer.reserve(this->current_key.size() +
                                 this->rest_key.size());
          _refind_buffer = this->current_key;
          _refind_buffer.append(this->rest_key.data(), this->rest_key.size());
          find(_refind_buffer);
        }
      }
    }
  }
};

// Node-by-node iterator for merging - no find, no transactions, just forward
// traversal
template <typename DB_, typename Traits_>
struct _NodeIterator : public _CursorBase<DB_, Traits_> {
  typedef DB_ DB;
  typedef Traits_ Traits;
  typedef _NodeIterator<DB, Traits_> NodeIterator;
  typedef _CursorBase<DB, Traits_> CursorBase;
  using db_ptr = typename Traits::db_ptr;
  using Transition = typename CursorBase::Transition;

  int stack_level;
  offset_t root;

  _NodeIterator(db_ptr db, offset_t root = 0) : CursorBase(db) {
    this->_txn = this->_db->txn();
    this->_txn->refs.fetch_add(1);
    this->root = root ? root : Traits::get_root(this->_txn);
    first();
  }

  ~_NodeIterator() { this->_txn->refs.fetch_sub(1); }

  void first() {
    this->stack.clear(0);
    if (!root) return;
    this->current_key.clear();
    this->push(root);
    this->stack.back().first();
    stack_level = 0;
  }

  bool next() {
    if (!this->stack.size) return false;  // empty iterator

    if (stack_level < (int)this->stack.size - 1) {
      stack_level++;  // the next node in stack
      return true;
    }

    // We're at the last node in stack, try to advance to next leaf
    while (this->stack.size) {
      if (this->stack.back().next()) {
        stack_level++;
        return true;
      }
      this->pop();
      stack_level--;
      assert(stack_level == (int)this->stack.size - 1);
    }
    return false;
  }

  // skip the next subtrie
  bool skip() {
    if (!this->stack.size) return false;  // empty iterator

    this->stack.clear(stack_level + 1);

    // We're at the last node in stack, try to advance to next leaf
    while (this->stack.size) {
      if (this->stack.back().next()) {
        stack_level++;
        return true;
      }
      this->pop();
      stack_level--;
      assert(stack_level == (int)this->stack.size - 1);
    }
    return false;
  }

  Slice node_key() const {
    if (this->stack.data[stack_level].is_leaf()) {
      return Slice(this->current_key);
    }
    assert(this->current_key.size() > this->stack.data[stack_level].keypos);
    return Slice(this->current_key.c_str(),
                 this->stack.data[stack_level].keypos);
  }

  Transition& node() { return this->stack.data[stack_level]; }
};

}  // namespace leaves

#endif  // _LEAVES_ICURSOR_HPP
