#ifndef _LEAVES_ICURSOR_HPP
#define _LEAVES_ICURSOR_HPP

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

  block_ptr big_value;  // pointer to a big value if available

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

  void replace(offset_t offset_) {
    offset = offset_;
    if (!is_root())
      *parent().update() = offset_;
    else
      cursor->set_root(offset_);
  }

  offset_e* update() {
    if (block->txn_id == cursor->txn_id()) return link();

    assert(is_trie());
    trie() = cursor->_db->cow(trie());
    offset = cursor->_db->resolve(trie());

    if (!is_root())
      *parent().update() = offset;
    else
      cursor->set_root(offset);

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
    big_value.reset();
    cmp = UNDEFINED;
  }

  Slice& key() { return cursor->rest_key; }

  Slice value() const {
    assert(is_leaf());
    if (big_value) {
      assert(leaf()->is_big());
      auto bv = leaf()->big();
      return Slice(((const char*)big_value), bv->value_size);
    }
    return leaf()->value();
  }

  std::string& current_key() { return cursor->current_key; }

  Transition& child() {
    assert(this - &cursor->stack.data[0] < cursor->stack.size - 1);
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
      if (!cmp && leaf()->is_big()) big_value = resolve(leaf()->big()->offset);
      return;
    }

    TrieNode& trie_ = *trie();
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
    if (leaf()->is_big()) big_value = resolve(leaf()->big()->offset);
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
      for (offset_e* l = lnk; l < end; l++) cursor->_db->prefetch(*l);
      push(lnk).first();
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
      for (offset_e* l = lnk; l >= begin; l--) cursor->_db->prefetch(*l);
      push(lnk).last();
      branch_key = current_key()[child().keypos];
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
    const offset_e* prev_offset = trie_.offset(prev_);
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

  void clear(int size_ = 0) {
    for (int i = size_; i < size; i++) {
      data[i].reset();
    }
    size = size_;
  }
};

// A cursor to
template <typename DB_, typename Traits_>
struct _Cursor {
  typedef DB_ DB;
  typedef Traits_ Traits;
  typedef _Cursor<DB, Traits_> Cursor;
  typedef _Stack<Cursor> Stack;
  using db_ptr = typename Traits::db_ptr;
  using Transition = typename Stack::Transition;
  using Hasher = typename Traits::Hasher;
  using hash_t = typename Hasher::hash_t;

  static constexpr size_t MAX_KEY_SIZE = Traits::MAX_KEY_SIZE;

  _Cursor(db_ptr db) : _db(db), transaction_active(false) {
    update();
    current_key.reserve(128);
  }

  tid_t txn_id() const {
    return !Traits::transactional || transaction_active ? _db->_wtxn.txn_id
                                                        : _db->txn()->txn_id;
  }

  // return true if the cursor is on a valid position
  bool is_valid() const { return stack.size ? stack.back().success() : false; }

  void find(const Slice& key) {
    if (key.size() > MAX_KEY_SIZE) throw KeyTooBig();

    update();
    rest_key = key;
    if (stack.size && keep_stack()) return;
    _find();
  }

  void first() {
    update();
    stack.clear(0);
    if (!root) return;
    rest_key.reset();
    current_key.clear();
    push(root);
    stack.back().first();
  }

  void last() {
    update();
    stack.clear(0);
    if (!root) return;
    rest_key.reset();
    current_key.clear();
    push(root);
    stack.back().last();
  }

  void next() {
    rest_key.reset();
    while (stack.size) {
      if (stack.back().next()) return;
      pop();
    }
  }

  void prev() {
    rest_key.reset();
    while (stack.size) {
      if (stack.back().prev()) return;
      pop();
    }
  }

  void value(const Slice& value) {
    if (Traits::transactional && !transaction_active) {
      if (!_db->start_transaction()) throw TransactionActive();
      transaction_active = true;
    }

    if (!stack.size) {
      if (!root) {
        push(offset_t());
        _Inserter(&stack.back(), value).first_exec();
        return;
      }
      throw NoValidPosition();
    }

    _Inserter(&stack.back(), value).exec();
  }

  Slice value() const {
    const Transition& back = stack.back();
    return back.cmp == 0 && back.is_leaf() ? back.value() : Slice();
  }

  Slice key() const { return current_key; }

  void remove() {
    if (!is_valid()) throw NoValidPosition();
    if (Traits::transactional && !transaction_active) {
      if (!_db->start_transaction()) throw TransactionActive();
      transaction_active = true;
    }
    _Deleter(&stack.back()).exec();
  }

  void prepare_commit() {
    if (Traits::transactional && transaction_active) _db->prepare_commit();
  }

  void commit() {
    if (Traits::transactional && transaction_active) {
      _db->commit();
      transaction_active = false;
      auto root_ = root;
      update();
      assert(root_ == root);
    }
  }

  void rollback() {
    if (Traits::transactional && transaction_active) {
      _db->rollback();
      stack.clear();
      transaction_active = false;
      find(current_key);
    }
  }

  /* Helpers */

  void set_root(offset_t offset) {
    Traits::set_root(*_db, offset);
    root = offset;
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

  bool keep_stack() {
    // Don't start from the very start. Try to start as deep in the stack as
    // possible
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
    for (; i < stack.size; i++) {
      Transition& item = stack.data[i];
      if (item.keypos >= same) break;
    }

    stack.clear(i);
    if (!stack.size) return false;

    Transition& back = stack.back();
    back.prefix = 0;
    back.cmp = Transition::UNDEFINED;
    rest_key.iadvance(back.keypos);
    current_key.resize(back.keypos);
    return false;
  }

  void _find() {
    if (!stack.size) {
      if (!root) return;  // empty db
      current_key.clear();
      push(root);
    }
    stack.back().find();
  }

  void update() {
    if (Traits::transactional && !transaction_active)
      root = Traits::get_root(*_db);
  }

  db_ptr _db;
  offset_t root;
  Stack stack;
  Slice rest_key;
  std::string current_key;
  bool transaction_active;
};

}  // namespace leaves

#endif  // _LEAVES_ICURSOR_HPP
