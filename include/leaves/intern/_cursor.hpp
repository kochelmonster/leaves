#ifndef _LEAVES_ICURSOR_HPP
#define _LEAVES_ICURSOR_HPP

#include <string>
#include <vector>

#include "_bigmemory.hpp"
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

  void set_root(offset_t offset_) { *cursor->_root = offset_; }

  void replace(offset_t offset_) {
    offset = offset_;
    if (!is_root())
      *parent().update(*this) = offset_;
    else
      set_root(offset);
  }

  offset_e* update(Transition& child) {
    if (!block->needs_cow(*child.block.operator->())) {
      cursor->_db->make_dirty(block);
      return link();
    }

    // copy-on-write trie
    assert(is_trie());
    trie_ptr old_trie = trie();
    trie() = cursor->_db->clone(old_trie);
    offset = cursor->_db->resolve(trie());
    assert(trie()->count() < trie()->MAX_BRANCH_COUNT);
    cursor->_db->free(old_trie);

    if (!is_root())
      *parent().update(*this) = offset;
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
    return leaf()->value();
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
    prefix = trie_._compressed_len;
    cmp = 0;
    auto& child = push(trie_.array());
    child.first();
    if (child.keypos < current_key().size()) {
      branch_key = current_key()[child.keypos];
    }
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
    prefix = trie_._compressed_len;
    cmp = 0;
    auto& child = push(trie_.array() + trie_.count() - 1);
    child.last();
    assert(child.keypos < current_key().size());
    branch_key = current_key()[child.keypos];
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
template <typename Traits_>
struct _CursorBase {
  typedef Traits_ Traits;
  typedef _CursorBase<Traits_> CursorBase;
  typedef _Stack<CursorBase> Stack;
  typedef typename Traits::DB DB;
  using offset_e = typename Traits::offset_e;
  using block_ptr = typename Traits::ptr;
  using Transition = typename Stack::Transition;

  DB* _db;
  offset_e* _root;
  Stack stack;
  Slice rest_key;
  std::string current_key;

  _CursorBase() = default;
  _CursorBase(DB* db, offset_e* root) : _db(db), _root(root) {
    current_key.reserve(128);
  }

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

  // Allocation methods delegated through cursor to DB
  block_ptr alloc(uint16_t size) { return this->_db->alloc(size); }

  AreaSlice alloc_big(uint64_t size) { return this->_db->alloc_big(size); }
};

// Full cursor with find, transactions, and modification operations
template <typename Traits_>
struct _Cursor : public _CursorBase<Traits_> {
  typedef Traits_ Traits;
  typedef _Cursor<Traits_> Cursor;
  typedef _CursorBase<Traits_> CursorBase;
  using DB = typename Traits::DB;
  using offset_e = typename Traits::offset_e;
  using Transition = typename CursorBase::Transition;
  using Stack = typename CursorBase::Stack;
  using Hasher = typename Traits::Hasher;
  using hash_t = typename Hasher::hash_t;

  static constexpr size_t MAX_KEY_SIZE = Traits::MAX_KEY_SIZE;

  _Cursor(DB* db, offset_e* root) : CursorBase(db, root) {}

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

  void set_root(offset_e* root) { 
    if (*root != *this->_root) {
      this->_root = root; 
      clear();
    }
    else {
      this->_root = root;
    }
  }

  bool clear() {
    this->stack.clear(0);
    this->rest_key.reset();
    this->current_key.clear();
    auto root = *this->_root;
    if (!root) return true;
    this->push(root);
    return false;
  }

  void first() {
    if (clear()) return;
    this->stack.back().first();
  }

  void last() {
    if (clear()) return;
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
    if (!this->stack.size) {
      if (!*this->_root) {
        this->push(offset_t());
        _Inserter(&this->stack.back(), size).first_exec();
        return (void*)this->stack.back().value().data();
      }
      throw NoValidPosition();
    }

    _Inserter(&this->stack.back(), size).exec();
    return (void*)this->stack.back().value().data();
  }

  void value(const Slice& value) {
    void* space = reserve(value.size());
    assert(space);
    optimized_memcpy(space, value.data(), value.size());
    this->_db->flush();
  }

  Slice value() const {
    const Transition& back = this->stack.back();
    return back.cmp == 0 && back.is_leaf() ? back.value() : Slice();
  }

  Slice key() const { return this->current_key; }

  void remove() {
    if (!is_valid()) throw NoValidPosition();
    _Deleter(*this).exec();
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
      auto root = *this->_root;
      if (!root) return;  // empty db
      this->current_key.clear();
      this->push(root);
    }
    this->stack.back().find();
  }
};

// Full cursor with find, transactions, and modification operations
template <typename Traits_>
struct _TransactionalCursor : public _Cursor<Traits_> {
  typedef Traits_ Traits;
  typedef _Cursor<Traits_> Cursor;
  typedef _BigMemory<Cursor> BigMemory;
  using DB = typename Traits::DB;
  using txn_ptr = typename DB::txn_ptr;
  using offset_e = typename Traits::offset_e;
  using Transition = typename Cursor::Transition;
  using LeafNode = typename Transition::LeafNode;
  typedef typename BigMemory::BigValue BigValue;

  std::unique_ptr<BigMemory> _bigmemory;
  txn_ptr _txn;
  uint64_t _id{0};
  std::string _refind_buffer;

  _TransactionalCursor(DB* db, offset_e* root) : Cursor(db, root) {
    _id = this->_db->new_cursor_id();
    update();
  }

  ~_TransactionalCursor() {
    if (is_transaction_active()) {
      if (this->_txn) this->_txn->refs.fetch_sub(1);
    }
  }

  bool is_transaction_active() const {
    return this->_db->txn_cursor_id() == _id;
  }

  BigMemory& get_bigmemory() {
    if (!_bigmemory) {
      _bigmemory = std::make_unique<BigMemory>(
          this->_db, &this->_txn->free_bigmem_root);
    }
    return *_bigmemory;
  }

  void* reserve(size_t size) {
    [[maybe_unused]] bool r = start_transaction();
    assert(r);

    const Transition& back = this->stack.back();
    if (this->is_valid() && back.leaf()->is_big()) {
      BigValue* bvalue = (BigValue*)back.leaf()->vdata();
      get_bigmemory().free(bvalue);
    }

    uint16_t size_modified =
        BigMemory::template modify_size<LeafNode>(this->rest_key.size(), size);
    void* result = Cursor::reserve(size_modified);
    if (size_modified != size) {
      BigValue* bvalue = (BigValue*)result;
      get_bigmemory().alloc(size, bvalue);
      this->stack.back().leaf()->set_big();
      return bvalue->data(this->_db);
    }
    return result;
  }

  Slice value() const {
    const Transition& back = this->stack.back();
    if (back.cmp == 0 && back.is_leaf()) {
      if (back.leaf()->is_big()) {
        BigValue* bvalue = (BigValue*)back.leaf()->vdata();
        return Slice(bvalue->data(this->_db), bvalue->value_size);
      }
      return back.value();
    }
    return Slice();
  }

  void value(const Slice& value) {
    void* space = reserve(value.size());
    optimized_memcpy(space, value.data(), value.size());
    this->_db->flush();
  }

  void remove() {
    [[maybe_unused]] bool r = start_transaction();
    if (!this->is_valid()) throw NoValidPosition();
    const Transition& back = this->stack.back();
    if (back.leaf()->is_big()) {
      BigValue* bvalue = (BigValue*)back.leaf()->vdata();
      get_bigmemory().free(bvalue);
    }
    _Deleter(*this).exec();
  }

  bool start_transaction(bool non_blocking = false) {
    if (this->_db->txn_cursor_id() != _id) {
      txn_ptr new_txn = this->_db->start_transaction(_id, non_blocking);
      if (!new_txn) return false;
      assert(new_txn->refs.load() == 0);  // no one can reference it yet
      _set_txn(new_txn);
    }
    return true;
  }

  tid_t prepare_commit(bool sync = false) {
    return this->_db->prepare_commit(_id, sync);
  }

  bool commit(bool sync = false) { return this->_db->commit(_id, sync); }

  bool rollback() {
    if (this->_db->rollback(_id)) {
      this->stack.clear();
      this->find(this->current_key);
      return true;
    }
    return false;
  }

  void update() {
    auto new_txn = this->_db->txn();
    assert(new_txn);
    if (!this->_txn || new_txn->txn_id > this->_txn->txn_id) {
      _set_txn(new_txn);
    }
  }

  void _set_txn(txn_ptr& txn) {
    assert(txn);
    if (this->_txn != txn) {
      offset_t old_root_val = this->_root ? *this->_root : offset_t();
      if (this->_txn) {
        this->_txn->refs.fetch_sub(1);
      }
      this->_txn = txn;
      this->_txn->refs.fetch_add(1);
      this->_root = &this->_txn->root;
      if (_bigmemory)
        _bigmemory->reset(&this->_txn->free_bigmem_root);
      if ((this->current_key.size() || this->rest_key.size()) &&
          old_root_val != *this->_root) {
        // adjust to new root
        this->stack.clear();
        _refind_buffer.reserve(this->current_key.size() +
                               this->rest_key.size());
        _refind_buffer = this->current_key;
        _refind_buffer.append(this->rest_key.data(), this->rest_key.size());
        this->find(_refind_buffer);
      }
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_ICURSOR_HPP
