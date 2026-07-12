/*
Transactional cursor internals for traversing and mutating trie data.
*/
#ifndef _LEAVES_ICURSOR_HPP
#define _LEAVES_ICURSOR_HPP

#include <string>
#include <vector>

#include "../core/_exception.hpp"
#include "../core/_node.hpp"
#include "../memory/_bigmemory.hpp"
#include "_aspect.hpp"
#include "_deleter.hpp"
#include "_inserter.hpp"

namespace leaves {

template <typename Cursor_>
struct _Transition {
  typedef Cursor_ Cursor;
  using Traits = typename Cursor::Traits;
  typedef _Transition<Cursor> Transition;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using page_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using trie_ptr = typename Traits::template Pointer<TrieNode>;
  using leaf_ptr = typename Traits::template Pointer<LeafNode, LEAF>;
  using node_ptr = typename Traits::template Pointer<_Node>;

  static const int NOT_FOUND = 2;  // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  Cursor* cursor;
  node_ptr node;

  static_assert(sizeof(node_ptr) == sizeof(trie_ptr) &&
                    sizeof(node_ptr) == sizeof(leaf_ptr),
                "pointer sizes must match for type-punning");

  // All three Pointer<> instantiations have identical layout (same underlying
  // storage type, same size). The reinterpret_cast is intentional and safe;
  // suppress the GCC strict-aliasing warning at this specific point.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
  trie_ptr& trie() { return reinterpret_cast<trie_ptr&>(node); }
  leaf_ptr& leaf() { return reinterpret_cast<leaf_ptr&>(node); }
  const leaf_ptr& leaf() const { return reinterpret_cast<const leaf_ptr&>(node); }
#pragma GCC diagnostic pop

  uint16_t prefix;     // count of equal chars in compressed node
  uint16_t keypos;     // position inside the key
  uint8_t branch_key;  // The char branching the

  // 1: the key to find is bigger than the found node
  // 0: the key is found
  // -1: the key to find is smaller than the found node
  // NOT_FOUND: it is not equal but not known if -1 or 1
  // UNDEFINED: not tested
  int cmp;

  offset_e* offset;
  uint16_t link_idx;  // the array index of the link in the trie node

  offset_e* link() {
    assert(link_idx != 0xFFFF);
    return trie()->array() + link_idx;
  }

  bool is_leaf() const { return offset->type() == LEAF; }
  bool is_trie() const { return offset->type() == TRIE; }

  bool success() const { return cmp == 0 && is_leaf(); }

  bool init(Cursor* cursor_, offset_e* offset_, uint16_t keypos_ = 0) {
    cursor = cursor_;
    keypos = keypos_;
    prefix = 0;
    cmp = Transition::UNDEFINED;
    offset = offset_;
    link_idx = 0xFFFF;
    node = cursor->_db->template resolve<_Node>(offset);
    return true;  // the caller shall set the trie root
  }

  void update_trie_offset() { update_offset(cursor->_db->resolve(trie())); }

  void update_leaf_offset() { update_offset(cursor->_db->resolve(leaf())); }

  void update_offset(offset_e new_offset) {
    // set the offset to the current node that has been changed.
    // to transfer the node type to offset we have to distinguish between leaf
    // and trie
    assert(*offset != new_offset);
    if (!is_root()) offset = parent().update();
    *offset = new_offset;
  }

  offset_e* update() {
    using PageHeader = typename Traits::PageHeader;

    // Compute PageHeader addresses from node pointers
    page_ptr page_header = node - sizeof(PageHeader);
    if (!page_header->needs_cow(cursor->_db)) {
      cursor->_db->make_dirty(node);
      return link();
    }

    // copy-on-write trie
    // Keep in mind: old_trie is the page root and therefore the start of page
    // data

    assert(is_trie());
    trie_ptr old_trie = trie();

    page_ptr new_page = cursor->_db->alloc_slot(page_header->slot_id);
    new_page->used = page_header->used;
    assert(new_page->used <= Traits::PAGE_SIZES[new_page->slot_id]);

    assert(old_trie->size() <= page_header->used);
    trie() = new_page + sizeof(PageHeader);
    memcpy((char*)trie(), (char*)old_trie, page_header->used);

    auto new_offset = cursor->_db->resolve(trie());
    cursor->_db->free(page_header);
    assert(trie()->count() < trie()->MAX_BRANCH_COUNT);

    // Propagate COW upward: grandparent's needs_cow will compare its txn_id
    // with our NEW cloned trie's txn_id. If they match (same transaction), no
    // COW needed. If different, grandparent will also COW and recursively
    // propagate upward.
    if (!is_root()) offset = parent().update();
    *offset = new_offset;

    // Return the child's offset location in the NEW cloned trie
    return link();
  }

  void advance_key(uint16_t count) { cursor->advance_key(count); }

  void append_key(const uint8_t* data, size_t size) {
    cursor->current_key.append((const char*)data, size);
  }

  void resize_key(size_t size) { cursor->current_key.resize(size); }

  Transition& push() {
    cursor->push(link());
    return cursor->stack.back();
  }

  void pop() { cursor->pop(); }

  void reset() {
    node.reset();
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
    if (!node) {
      cmp = NOT_FOUND;
      return;
    }

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
        link_idx = 0;
        cursor->_db->prefetch(&trie_.array()[link_idx]);
        push().find();
      } else
        cmp = -1;
      return;
    }

    branch_key = key()[0];
    int idx = trie_.array_index(branch_key);
    if (idx < 0) {
      cmp = NOT_FOUND;
      return;
    }
    cmp = 0;
    link_idx = idx;
    cursor->_db->prefetch(link());
    push().find();
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
    link_idx = 0;
    auto& child = push();
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
      if (++link_idx >= trie_.count()) return false;
      offset_e* lnk = link();
      if (link_idx + 1 < trie_.count()) cursor->_db->prefetch(lnk + 1);

      auto& child = push();
      cursor->_db->prefetch(lnk);

      child.first();
      branch_key = current_key()[child.keypos];
      assert(trie_.isset(branch_key));
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
    cmp = 0;
    branch_key = (uint8_t)next_;
    link_idx = trie_.array_index(next_);
    assert(link_idx < trie_.count());
    push().first();
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
      if (!link_idx--) return false;
      offset_e* lnk = link();
      if (link_idx > 0) cursor->_db->prefetch(lnk - 1);

      auto& child = push();
      cursor->_db->prefetch(lnk);

      child.last();
      if (child.keypos < current_key().size()) {
        branch_key = current_key()[child.keypos];
        assert(trie_.isset(branch_key));
      } else {
        assert(trie_.isset(TrieNode::NONE));
      }
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
    cmp = 0;
    branch_key = (uint8_t)prev_;
    link_idx = trie_.array_index(prev_);
    assert(link_idx < trie_.count());
    push().last();
    return true;
  }

  void last() {
    if (is_leaf()) return leaf_step();
    TrieNode& trie_ = *trie();
    append_key(trie_.compressed(), trie_._compressed_len);
    prefix = trie_._compressed_len;
    cmp = 0;
    link_idx = trie_.count() - 1;
    auto& child = push();
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

  template <typename offset_e>
  void push(Cursor* cursor, offset_e* offset, uint16_t keypos = 0) {
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
template <typename Traits_, typename Derived>
struct _CursorBase {
  typedef Traits_ Traits;
  typedef _CursorBase<Traits_, Derived> CursorBase;
  typedef _Stack<Derived> Stack;
  typedef typename Traits::DB DB;
  using offset_e = typename Traits::offset_e;
  using page_ptr = typename Traits::ptr;
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

  // return true if the cursor is on a valid position
  bool is_valid() const {
    return this->stack.size ? this->stack.back().success() : false;
  }

  void reset_key(size_t keypos) {
    assert(keypos <= current_key.size());
    size_t delta = current_key.size() - keypos;
    current_key.resize(keypos);
    rest_key = Slice(rest_key.data() - delta, rest_key.size() + delta);
  }

  void advance_key(size_t size) {
    current_key.append(rest_key.data(), size);
    rest_key.iadvance(size);
  }

  void push(offset_e* ptr) {
    _db->prefetch(ptr);
    stack.push(static_cast<Derived*>(this), ptr, current_key.size());
  }

  void pop() {
    assert(stack.size > 0);
    current_key.resize(stack.back().keypos);
    stack.size--;
  }

  // Allocation methods delegated through cursor to DB
  page_ptr alloc_page(uint16_t size) { return this->_db->alloc_page(size); }
};

// Full cursor with find, transactions, and modification operations
template <typename Traits_, typename Derived>
struct _ICursor : public _CursorBase<Traits_, Derived> {
  typedef Traits_ Traits;
  typedef _ICursor<Traits_, Derived> Cursor;
  typedef _CursorBase<Traits_, Derived> CursorBase;
  using DB = typename Traits::DB;
  using offset_e = typename Traits::offset_e;
  using Transition = typename CursorBase::Transition;
  using Stack = typename CursorBase::Stack;

  static constexpr size_t MAX_KEY_SIZE = Traits::MAX_KEY_SIZE;

  _ICursor(DB* db, offset_e* root) : CursorBase(db, root) {}

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
    } else {
      this->_root = root;
    }
    this->stack.front().offset = this->_root;
  }

  bool clear() {
    this->stack.clear(0);
    this->rest_key.reset();
    this->current_key.clear();
    if (!*this->_root) return true;
    static_cast<Derived*>(this)->push(this->_root);
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
        static_cast<Derived*>(this)->push(this->_root);
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
    if (!this->is_valid()) throw NoValidPosition();
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

    // Scan backwards — keypos is monotonically non-decreasing, so the
    // split point is usually near the end for high-locality workloads.
    size_t i = this->stack.size;
    while (i > 0 && this->stack.data[i - 1].keypos >= same) {
      i--;
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
      if (!*this->_root) return;  // empty db
      this->current_key.clear();
      this->push(this->_root);
    }
    this->stack.back().find();
  }
};

template <typename Traits_>
struct _Cursor : public _ICursor<Traits_, _Cursor<Traits_>> {
  typedef Traits_ Traits;
  typedef _ICursor<Traits_, _Cursor<Traits_>> Cursor;
  using Cursor::Cursor;
};

// Write-Ahead Log mixin — provides WAL operations via CRTP to any cursor.
// The derived type must be a _TransactionalCursor exposing _db, _id, _aspect,
// and _aspect_context.
template <typename Derived_>
struct _WalAware {
  using Derived = Derived_;

  bool _use_wal{false};

  void _wal_setup(bool use_wal) {
    if (use_wal)
      _wal_open();
    else
      _wal_close();
  }

  void _wal_open() {
    _use_wal = true;
    _derived()._db->open_wal();
  }

  void _wal_close() { _use_wal = false; }

  void _wal_begin(uint32_t txn_id) {
    if (_use_wal) _derived()._db->wal_begin(txn_id);
  }

  void _wal_log_put(const Slice& key, const Slice& value) {
    if (_use_wal) _derived()._db->wal_put(key, value);
  }

  void _wal_log_delete(const Slice& key) {
    if (_use_wal) _derived()._db->wal_delete(key);
  }

  void _wal_prepare_commit(bool skip_sync = false) {
    if (_use_wal) _derived()._db->wal_prepare(skip_sync);
  }

  void _wal_commit(uint32_t txn_id) {
    if (_use_wal) _derived()._db->wal_commit(txn_id);
  }

  void _wal_abort() {
    if (_use_wal) _derived()._db->wal_abort();
  }

  Derived& _derived() { return *static_cast<Derived*>(this); }
};

// Full cursor with find, transactions, and modification operations
template <typename Traits_>
struct _TransactionalCursor
    : public _ICursor<Traits_, _TransactionalCursor<Traits_>>,
      public _WalAware<_TransactionalCursor<Traits_>> {
  typedef Traits_ Traits;
  typedef _ICursor<Traits_, _TransactionalCursor<Traits_>> Cursor;
  typedef _BigMemory<_Cursor<Traits_>> BigMemory;
  using DB = typename Traits::DB;
  using txn_ptr = typename DB::txn_ptr;
  using offset_e = typename Traits::offset_e;
  using Transition = typename Cursor::Transition;
  using LeafNode = typename Transition::LeafNode;
  typedef typename BigMemory::BigValue BigValue;
  using Aspect = typename DB::Aspect;
  using CursorContext = typename Aspect::CursorContext;

  std::unique_ptr<BigMemory> _bigmemory;
  txn_ptr _txn;
  uint64_t _id{0};
  std::string _refind_buffer;
  [[no_unique_address]] CursorContext _aspect_context;

  _TransactionalCursor(DB* db, offset_e* root) : Cursor(db, root) {
    _id = this->_db->new_cursor_id();
    update();
  }

  Aspect& _aspect() { return this->_db->aspect(); }

  ~_TransactionalCursor() {
    if (is_transaction_active()) rollback();
    if (this->_txn) this->_txn->refs.fetch_sub(1);
  }

  tid_t txn_id() const { return _txn ? _txn->txn_id : tid_t(0); }

  bool is_transaction_active() const {
    return this->_db->txn_cursor_id() == _id;
  }

  void push(offset_e* ptr) {
    this->_db->prefetch(ptr, is_transaction_active() ? WRITE : READ);
    this->stack.push(this, ptr, this->current_key.size());
  }

  BigMemory& get_bigmemory() {
    if (!_bigmemory) {
      _bigmemory =
          std::make_unique<BigMemory>(this->_db, &this->_txn->free_bigmem_root);
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

    // Account for inline aspect metadata alongside _BigValue
    static constexpr size_t BIG_INLINE_SIZE =
        sizeof(BigValue) + Aspect::big_meta_size;

    uint16_t size_modified = BigMemory::template modify_size<LeafNode>(
        this->rest_key.size(), size, BIG_INLINE_SIZE);

    void* result = Cursor::reserve(size_modified);

    if (size_modified != size) {
      BigValue* bvalue = (BigValue*)result;
      get_bigmemory().alloc(size, bvalue);
      this->stack.back().leaf()->set_big();
      // Let aspect write inline metadata after BigValue
      if constexpr (Aspect::big_meta_size > 0) {
        _aspect().init_big_meta(this->key(), (char*)result + sizeof(BigValue),
                                _aspect_context);
      }
      auto data_ptr = bvalue->data(this->_db);
      this->_db->make_dirty(data_ptr);
      result = static_cast<char*>(data_ptr);
    }
    return result;
  }

  Slice _raw_value() const {
    const Transition& back = this->stack.back();
    if (back.cmp == 0 && back.is_leaf()) {
      if (back.leaf()->is_big()) {
        BigValue* bvalue = (BigValue*)back.leaf()->vdata();
        this->_db->prefetch((offset_e*)&bvalue->chunk_offset);
        auto data_ptr = bvalue->data(this->_db);
        return Slice((char*)data_ptr, bvalue->value_size);
      }
      return back.value();
    }
    return Slice();
  }

  Slice _big_meta() const {
    if constexpr (Aspect::big_meta_size > 0) {
      const Transition& back = this->stack.back();
      if (back.cmp == 0 && back.is_leaf() && back.leaf()->is_big()) {
        char* meta = (char*)back.leaf()->vdata() + sizeof(BigValue);
        return Slice(meta, Aspect::big_meta_size);
      }
    }
    return Slice();
  }

  Slice value() const {
    Slice raw = _raw_value();
    if (!raw.data()) return raw;
    return const_cast<_TransactionalCursor*>(this)->_aspect().on_read(
        this->key(), raw, _big_meta(),
        const_cast<CursorContext&>(_aspect_context));
  }

  void value(const Slice& value) {
    Slice transformed = _aspect().on_write(this->key(), value, _aspect_context);
    void* space = reserve(transformed.size());
    optimized_memcpy(space, transformed.data(), transformed.size());
    this->_wal_log_put(this->key(), value);
    this->_db->flush();
  }

  template <bool callaspect = true>
  void remove() {
    [[maybe_unused]] bool r = start_transaction();
    if (!this->is_valid()) throw NoValidPosition();
    if constexpr (callaspect) {
      // Aspect gate — may throw or return false to reject
      Slice cur_value = _raw_value();
      if (!_aspect().may_delete(this->key(), cur_value, _aspect_context)) {
        throw NoValidPosition();  // Aspect rejected the delete
      }
    }
    this->_wal_log_delete(this->key());
    const Transition& back = this->stack.back();
    if (back.leaf()->is_big()) {
      BigValue* bvalue = (BigValue*)back.leaf()->vdata();
      get_bigmemory().free(bvalue);
    }
    _Deleter(*this).exec();
  }

  bool start_transaction(bool non_blocking = false, bool use_wal = false,
                         TransactionOrigin origin = TransactionOrigin::user) {
    if (this->_db->txn_cursor_id() != _id) {
      this->_wal_setup(use_wal);
      if (!_aspect().before_start_transaction(*this->_db, origin,
                                              _aspect_context))
        return false;
      txn_ptr new_txn = this->_db->start_transaction(_id, non_blocking, origin);
      if (!new_txn) return false;
      assert(new_txn->refs.load() == 0);  // no one can reference it yet
      _set_txn(new_txn);
      this->_wal_begin(new_txn->txn_id.value());
      _aspect().on_start_transaction(*this->_db, new_txn->txn_id, origin,
                                     _aspect_context);
    }
    return true;
  }

  tid_t prepare_commit(bool sync = true) {
    if (this->_db->txn_cursor_id() != _id) return tid_t(0);
    this->_wal_prepare_commit();
    return this->_db->prepare_commit(_id, sync);
  }

  bool commit(bool sync = false,
              TransactionOrigin origin = TransactionOrigin::user) {
    if (this->_db->txn_cursor_id() != _id) return false;

    if (!_aspect().before_commit(*this->_db, origin, _aspect_context))
      return false;

    this->_wal_prepare_commit(true);
    this->_wal_commit(this->_txn->txn_id.value());

    bool committed = this->_db->commit(_id, sync, origin);
    if (committed) {
      _aspect().on_commit(*this->_db, origin, _aspect_context);
    }
    return committed;
  }

  bool rollback(TransactionOrigin origin = TransactionOrigin::user) {
    if (this->_db->txn_cursor_id() != _id) return false;
    if (!_aspect().before_rollback(*this->_db, this->_txn->txn_id, origin,
                                   _aspect_context))
      return false;
    this->_wal_abort();
    if (this->_db->rollback(_id, origin)) {
      _aspect().on_rollback(*this->_db, this->_txn->txn_id, origin,
                            _aspect_context);
      // Switch back to the committed read transaction.
      // Don't decrement _txn->refs — rollback() already reset the recycled
      // page to refs=0 and repurposed it as next_txn_page.
      auto read_txn = this->_db->txn_ref();
      this->_txn = read_txn;
      this->_root = &this->_txn->root;
      if (_bigmemory) _bigmemory->reset(&this->_txn->free_bigmem_root);
      this->stack.clear();
      _refind_buffer = this->current_key;
      this->find(_refind_buffer);
      return true;
    }
    return false;
  }

  // --- Navigation overrides with aspect hooks ---

  void find(const Slice& key) {
    if (!_aspect().before_find(key, _aspect_context)) return;
    Cursor::find(key);
    _aspect().on_find(key, this->is_valid(), _aspect_context);
  }

  void next() {
    Cursor::next();
    _aspect().on_next(this->is_valid(), _aspect_context);
  }

  void prev() {
    Cursor::prev();
    _aspect().on_prev(this->is_valid(), _aspect_context);
  }

  void update() {
    // Hot path: check if the committed transaction has changed before
    // acquiring the SpinLock inside txn_ref(). read_txn_offset() is a
    // plain aligned 64-bit load — no lock needed.
    if (this->_txn &&
        this->_db->read_txn_offset() == this->_db->resolve(this->_txn))
      return;

    // txn_ref() atomically resolves and increments refs under SpinLock,
    // preventing a concurrent start_transaction() from freeing the txn.
    auto new_txn = this->_db->txn_ref();
    assert(new_txn);
    if (!this->_txn || new_txn->txn_id > this->_txn->txn_id) {
      _set_txn(new_txn);
    }
    new_txn->refs.fetch_sub(1);  // revert the prior increment
  }

  void _set_txn(txn_ptr& txn) {
    assert(txn);
    assert(this->_txn != txn);
    offset_e old_root_val = this->_root ? *this->_root : offset_e();
    if (this->_txn) {
      this->_txn->refs.fetch_sub(1);
    }
    this->_txn = txn;
    this->_txn->refs.fetch_add(1);
    this->_root = &this->_txn->root;
    this->stack.front().offset = this->_root;
    if (_bigmemory) _bigmemory->reset(&this->_txn->free_bigmem_root);
    if ((this->current_key.size() || this->rest_key.size()) &&
        old_root_val != *this->_root) {
      // adjust to new root
      this->stack.clear();
      _refind_buffer.reserve(this->current_key.size() + this->rest_key.size());
      _refind_buffer = this->current_key;
      _refind_buffer.append(this->rest_key.data(), this->rest_key.size());
      this->find(_refind_buffer);
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_ICURSOR_HPP
