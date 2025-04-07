#ifndef _LEAVES_CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include <vector>

#include "_burst.hpp"
#include "_exception.hpp"
#include "_inserter.hpp"
#include "_node.hpp"

namespace leaves {

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

template <typename Cursor_>
struct _Transition {
  typedef Cursor_ Cursor;
  using Traits = typename Cursor::Traits;
  typedef _Transition<Cursor> Transition;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  typedef _BurstTable<Traits> BurstTable;
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;
  using burst_ptr = typename Traits::Pointer<BurstTable, BURST>;

  static const int NOT_FOUND = 2;  // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  Cursor* cursor;

  block_ptr block;
  trie_ptr& trie;
  leaf_ptr& leaf;
  burst_ptr& burst;

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

  struct Empty {
    const Empty& operator=(int) const { return *this; }
    Empty operator+(int) const { return Empty(); }
    Empty operator-(int) const { return Empty(); }
    Empty& operator+=(int) { return *this; }
    Empty& operator-=(int) { return *this; }
    Empty& operator*=(int) { return *this; }
    Empty operator++(int) { return Empty(); }
    Empty operator++() { return Empty(); }
    Empty operator--(int) { return Empty(); }
    Empty operator--() { return Empty(); }
    bool operator==(int) const { return true; }
    bool operator!=(int) const { return false; }
    bool operator<(int) const { return false; }
    bool operator<=(int) const { return true; }
    bool operator>(int) const { return false; }
    bool operator>=(int) const { return true; }
    operator int() const { return 0; }
  };
  ;
  std::conditional_t<Traits::BURST, int, Empty> index;

  offset_e* link() {
    assert(link_offset != 0xFFFF);
    return (offset_e*)(trie.link(link_offset));
  }

  _Transition()
      : trie(*(trie_ptr*)&block),
        leaf(*(leaf_ptr*)&block),
        burst(*(burst_ptr*)&block) {}

  bool is_leaf() const { return offset.type() == LEAF; }
  bool is_trie() const { return offset.type() == TRIE; }
  bool is_burst() const { return offset.type() == BURST; }

  bool success() const { return cmp == 0 && (is_leaf() || is_burst()); }

  bool init(Cursor* cursor_, offset_t offset_, uint16_t keypos_ = 0) {
    cursor = cursor_;
    keypos = keypos_;
    prefix = 0;
    if (Traits::BURST) index = 0;
    cmp = Transition::UNDEFINED;
    offset = offset_;
    link_offset = 0xFFFF;
    block = resolve(offset);
    return true;  // the caller shall set the trie root
  }

  block_ptr resolve(offset_t offset) { return cursor->storage.resolve(offset); }

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
    trie = cursor->storage.cow(trie);
    offset = cursor->storage.resolve(trie);

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
    cmp = UNDEFINED;
  }

  Slice& key() { return cursor->rest_key; }

  KeyString& current_key() { return cursor->current_key; }

  void find() {
    if (Traits::BURST && is_burst()) return burst->find(*this);

    if (is_leaf()) {
      LeafNode& leaf_ = *leaf;
      prefix = get_prefix(key().data(), (char*)leaf_.data, key().size(),
                          leaf_.key_size, cmp);
      advance_key(prefix);
      return;
    }

    TrieNode& trie_ = *trie;
    prefix = get_prefix(key().data(), (char*)trie_.compressed(), key().size(),
                        trie_._compressed_len, cmp);
    advance_key(prefix);
    if (prefix < trie_._compressed_len) return;

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
    LeafNode& leaf_ = *leaf;
    resize_key(keypos);
    append_key(leaf_.data, leaf_.key_size);
    cmp = 0;
  }

  Transition& child() {
    assert(this - &cursor->stack.data[0] < cursor->stack.size - 1);
    return this[1];
  }

  Transition& parent() {
    assert(this - &cursor->stack.data[0] > 0);
    return this[-1];
  }

  Slice value() const {
    if (Traits::BURST && is_burst()) return burst->value(index);
    if (is_leaf()) return leaf->value();
    return Slice();
  }

  bool is_root() const { return this - &cursor->stack.data[0] == 0; }

  void first() {
    if (Traits::BURST && is_burst()) return burst->first(*this);
    if (is_leaf()) return leaf_step();
    TrieNode& trie_ = *trie;
    append_key(trie_.compressed(), trie_._compressed_len);
    cmp = 0;
    push(trie_.array()).first();
  }

  bool next() {
    if (Traits::BURST && is_burst()) {
      resize_key(keypos);
      if (burst->next(*this)) return true;
      return false;
    }
    if (is_leaf()) {
      if (cmp < 0) {
        leaf_step();
        return true;
      }
      return false;
    }

    TrieNode& trie_ = *trie;
    if (cmp == 0) {
      offset_e* lnk;
      if (Traits::BURST) {
        offset_e* last = link();
        offset_e* end = trie_.array() + trie_.count();
        link_offset += sizeof(offset_e);
        lnk = link();
        while (*lnk == *last && lnk < end) {
          link_offset += sizeof(offset_e);
          lnk = link();
        }
        if (lnk >= end) return false;
      } else {
        link_offset += sizeof(offset_e);
        lnk = link();
        offset_e* end = trie_.array() + trie_.count();
        if (lnk >= end) return false;
#ifdef __GNUC__
        for(offset_e* l = lnk; l < end; l++) {
          __builtin_prefetch(resolve(*(l)));
        }
#endif
      }
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

    if (Traits::BURST) {
      Transition& bottom = push(next_offset);
      assert(bottom.is_burst());
      cursor->rest_key = Slice(&branch_key, 1);
      bottom.burst->find(bottom);
      assert(bottom.cmp <= 0);
      if (bottom.cmp) {
        bool found = bottom.next();
        assert(found);
      }
      cursor->rest_key.reset();
    } else
      push(next_offset).first();

    return true;
  }

  bool prev() {
    if (Traits::BURST && is_burst()) {
      resize_key(keypos);
      if (burst->prev(*this)) return true;
      return false;
    }

    if (is_leaf()) {
      if (cmp > 0) {
        leaf_step();
        return true;
      }
      return false;
    }

    TrieNode& trie_ = *trie;
    if (cmp == 0) {
      offset_e* lnk;
      if (Traits::BURST) {
        offset_e* last = link();
        offset_e* begin = trie_.array();
        link_offset -= sizeof(offset_e);
        lnk = link();
        while (*lnk == *last && lnk >= begin) {
          link_offset -= sizeof(offset_e);
          lnk = link();
        }
        if (lnk < begin) return false;
      } else {
        link_offset -= sizeof(offset_e);
        lnk = link();
        offset_e* begin = trie_.array();
        if (lnk < begin) return false;
#ifdef __GNUC__
        for(offset_e* l = lnk; l >= begin; l--) {
          __builtin_prefetch(resolve(*(l)));
        }
#endif
      }

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

    if (Traits::BURST && prev_offset->type() == BURST) {
      Transition& bottom = push(prev_offset);
      assert(bottom.is_burst());
      cursor->rest_key = Slice(&branch_key, 1);
      bottom.burst->find(bottom);
      assert(bottom.cmp <= 0);
      if (bottom.cmp) {
        bool found = bottom.prev();
        assert(found);
      }
      cursor->rest_key.reset();
    } else
      push(trie_.offset(prev_)).last();

    return true;
  }

  void last() {
    if (Traits::BURST && is_burst()) return burst->last(*this);
    if (is_leaf()) return leaf_step();
    TrieNode& trie_ = *trie;
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
template <typename Storage_>
struct _Cursor {
  typedef Storage_ Storage;
  using Traits = typename Storage::Traits;
  typedef _Cursor<Storage> Cursor;
  typedef _Stack<Cursor> Stack;
  using Transition = typename Stack::Transition;

  _Cursor(Storage& storage_) : storage(storage_), transaction_active(false) {
    root = 0;
  }

  tid_t txn_id() const {
    return transaction_active ? storage._txn.txn_id : storage.txn()->txn_id;
  }

  // return true if the cursor is on a valid position
  bool is_valid() const {
    if (stack.size) return stack.back().success();
    return false;
  }

  void find(const Slice& key) {
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
    if (!transaction_active) {
      if (!storage.start_transaction()) throw TransactionActive();
      transaction_active = true;
    }

    if (!stack.size) {
      if (!root) {
        push(offset_t());
        _Inserter(&stack.back(), value).first();
        return;
      }
      throw NoValidPosition();
    }

    int ssize = stack.size;
    _Inserter(&stack.back(), value).start();
  }

  Slice value() const {
    const Transition& back = stack.back();
    return back.cmp == 0 ? back.value() : Slice();
  }

  Slice key() const { return current_key; }

  void remove() {}
  void commit() {
    if (transaction_active) {
      storage.prepare_commit();
      storage.commit();
      transaction_active = false;
      auto root_ = root;
      update();
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

  void set_root(offset_t offset) { storage._txn.root = root = offset; }

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
    if (!transaction_active) {
      root = storage.txn()->root;
    }
  }

  Storage& storage;
  offset_t root;
  Stack stack;
  Slice rest_key;
  KeyString current_key;
  bool transaction_active;
};

}  // namespace leaves

#endif  // _LEAVES_CURSOR_HPP
