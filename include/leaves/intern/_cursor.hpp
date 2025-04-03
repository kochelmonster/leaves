#ifndef _LEAVES_CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include <vector>

#include "_exception.hpp"
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
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode>;

  static const int NOT_FOUND = 2;  // branch_key was not found
  static const int UNDEFINED = 3;  // initial state of cmp

  Cursor* cursor;

  block_ptr block;
  trie_ptr& trie;
  leaf_ptr& leaf;

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
    return (offset_e*)(trie.link(link_offset));
  }

  _Transition() : trie(*(trie_ptr*)&block), leaf(*(leaf_ptr*)&block) {}

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

    if (is_trie())
      offset = cursor->storage.resolve(cursor->storage.cow(trie));
    else
      offset = cursor->storage.resolve(cursor->storage.cow(leaf));
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
      if (trie_.has_null()) {
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

  bool is_root() const { return this - &cursor->stack.data[0] == 0; }

  void first() {
    if (is_leaf()) return leaf_step();
    TrieNode& trie_ = *trie;
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

    TrieNode& trie_ = *trie;
    if (cmp == 0) {
      link_offset += sizeof(offset_e);
      offset_e* lnk = link();
      if (lnk >= trie_.array() + trie_.count()) return false;
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
    push(trie_.offset(next_)).first();
    branch_key = (uint8_t)next_;
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

    TrieNode& trie_ = *trie;
    if (cmp == 0) {
      link_offset -= sizeof(offset_e);
      offset_e* lnk = link();
      if (lnk < trie_.array()) return false;
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
    push(trie_.offset(prev_)).last();
    branch_key = (uint8_t)prev_;
    return true;
  }

  void last() {
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

template <typename Transition>
struct _Inserter {
  typedef _Inserter<Transition> Inserter;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using block_ptr = typename Transition::block_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  const Slice& value;
  Transition* back;

  _Inserter(Transition* back_, const Slice& value_)
      : value(value_), back(back_) {}

  _Inserter(Transition* back_, const Slice& value_, bool first)
      : value(value_), back(back_) {}

  tid_t txn_id() const { return back->cursor->storage.prepared_txn->txn_id; }

  template <typename T>
  offset_t resolve(T ptr) {
    return back->cursor->storage.resolve(ptr);
  }

  block_ptr alloc(uint16_t size) { return back->cursor->storage.alloc(size); }

  void free(block_ptr& block) { back->cursor->storage.free(block); }

  void start() {
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // insert the very first value
  void first() {
    Slice bkey = back->key();
    back->prefix = std::min(bkey.size(), (size_t)10);
    if (back->prefix > 1) back->prefix--;  // keep one for trie

    back->trie = alloc(TrieNode::size(back->prefix, 1));
    back->offset = resolve(back->trie);
    back->link_offset = back->trie->create(
        Slice(bkey.data(), back->prefix),
        (bkey.size() > back->prefix ? (back->branch_key = bkey[back->prefix])
                                    : TrieNode::NONE));
    back->cursor->set_root(back->offset);
    back->advance_key(back->prefix);
    create_leaf();
  }

  bool split_compressed() {
    if (back->is_trie() && back->prefix == back->trie->_compressed_len)
      return false;  // no split

    /*
    Operation:

      Before:
        parent -> [abcd] -> children

      Insert: [abef]

      After:
        parent -> [ab] -> [cd] -> children
                       -> [ef] -> table with new value
    */

    assert(back->prefix < back->trie->_compressed_len);

    auto otrie = back->trie;

    // copy the original trie node with second part of compressed
    // to a new page
    uint8_t prefix_len = otrie->_compressed_len - back->prefix;
    trie_ptr child_trie = alloc(TrieNode::size(prefix_len, otrie->count()));
    child_trie->create(*otrie,
                       Slice(&otrie->compressed()[back->prefix], prefix_len));

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int key =
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE;
    back->trie = alloc(TrieNode::size(back->prefix, 2));
    back->link_offset = back->trie->create(
        Slice(back->trie->compressed(), back->prefix),
        otrie->compressed()[back->prefix], resolve(child_trie), key);
    free(otrie);
    back->replace(resolve(back->trie));
    create_leaf();
    return true;
  }

  void create_leaf() {
    assert(back->key().size() < 255);
    const Slice& bkey = back->key();
    leaf_ptr leaf = fill_leaf(bkey);
    Transition& bottom = back->push(resolve(leaf));
    bottom.cmp = 0;
    bottom.prefix = bkey.size();
    bottom.advance_key(bottom.prefix);
    *back->link() = bottom.offset;
  }

  leaf_ptr fill_leaf(const Slice& key) {
    leaf_ptr leaf = alloc(LeafNode::size(key, value));
    leaf->key_size = key.size();
    leaf->value_size = value.size();
    // TODO: big value handling
    memcpy(leaf->data, key.data(), key.size());
    memcpy(leaf->vdata(), value.data(), value.size());
    return leaf;
  }

  const uint16_t MAX_SIZE = TrieNode::MAX_SIZE;

  void add_to_array() {
    int key = back->key() ? back->branch_key : TrieNode::NONE;
    key = back->trie->prev(key);
    if (key == TrieNode::OUT_OF_RANGE) key = back->trie->next(TrieNode::NONE);
    assert(key != TrieNode::OUT_OF_RANGE);

    trie_ptr otrie = back->trie;
    back->trie = alloc(std::min((uint16_t)(otrie->size() + 2 * sizeof(offset_e)),
                                (uint16_t)MAX_SIZE));
    back->link_offset = back->trie->create(
        *otrie, back->key() ? back->branch_key : TrieNode::NONE);

    free(otrie);
    create_leaf();
  }

  // change the value of leaf
  void change_leaf() {
    assert(back->is_leaf());
    leaf_ptr oleaf = back->leaf;

    if (back->cmp == 0) {
      assert(back->prefix == back->leaf->key_size);
      assert(back->key().empty());
      back->leaf = fill_leaf(oleaf->key());
      back->replace(resolve(back->leaf));
      free(oleaf);
      return;
    }
    leaf_ptr copy = alloc(
        LeafNode::size(Slice(nullptr, oleaf->key_size - back->prefix), value));
    copy->key_size = oleaf->key_size - back->prefix;
    copy->value_size = oleaf->value_size;
    memcpy(copy->data, oleaf->data + back->prefix,
           copy->key_size + copy->vsize());

    int bkey = !copy->key_size ? TrieNode::NONE : copy->data[0];

    back->trie = alloc(TrieNode::size(back->prefix, 2));
    back->link_offset = back->trie->create(
        Slice(oleaf->data, back->prefix), bkey, resolve(copy),
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE);

    free(oleaf);
    back->replace(resolve(back->trie));
    create_leaf();
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

  tid_t txn_id() const { return storage.txn()->txn_id; }

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
    if (back.cmp || !back.is_leaf()) return Slice();
    return back.leaf->value();
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
