#include "trace.hpp"

#include <fstream>
#include <iostream>

#include "node.hpp"

namespace leaves {

INLINE Stack::Stack() : size(0) { data.resize(100); }

INLINE void Stack::push(block_ptr block, node_ptr* pnode, size_t keypos,
                        int index) {
  if (size == data.size()) data.resize(size * 2);
  Transition& back = data[size++];
  back.block = block;
  back.pnode = pnode;
  back.keypos = keypos;
  back.index = index;
}

INLINE Trace::Trace(DBMemory& storage_)
    : storage(storage_), transaction_active(false), _debug_stat_page_splits(0) {
  root = storage.get_root()->offset;
  cursor_id = storage.alloc_cursor();
  current_key.reserve(1024);
}

INLINE Trace::~Trace() {
  storage.free_cursor(cursor_id);
  std::cout << "stat_page_splits: " << _debug_stat_page_splits << std::endl;
}

#ifdef DEBUG
inline void check_offsets(Stack& stack) {
  if (stack.size == 0) return;

  offset_ptr offset = stack.data[0].block->offset;
  for (int i = 0; i < stack.size; i++) {
    Transition& item = stack.data[i];
    assert(offset == item.block->offset);
    if (item.pnode->type == kLink) {
      offset = (*item.pnode)->link.link;
    }
  }
}
#else
inline void check_offsets(Stack& stack) {};
#endif

INLINE void Trace::_update() {
  // check if there is a new view available
  root = storage.update_cursor(cursor_id);
  if (stack.size && stack.front().block->offset != root) stack.clear();
}

INLINE void Trace::_advance_stack() {
  assert(stack.size > 0);

  size_t size = std::min(rest_key.size(), current_key.size());
  size_t same = 0;
  for (; same < size; same++) {
    if (rest_key[same] != current_key[same]) break;
  }
  int i = 0;
  int keep = 0;
  node_ptr* pnode = stack.front().pnode;
  for (; i < stack.size && stack.data[i].keypos <= same; i++) {
    Transition& item = stack.data[i];
    keep = item.keypos;
    if (item.pnode->type == kLink) last_root = i + 1;
  }

  rest_key.iadvance(keep);
  current_key.resize(keep);
  stack.clear(i);
}

INLINE void Trace::find(const Slice& key) {
  rest_key = key;
  if (stack.size) _advance_stack();
  if (rest_key.size() || !stack.size) _find();
}

INLINE void Trace::_clear_value() {
  current_value.reset();
  current_big_value.reset();
}

INLINE void Trace::_find() {
  block_ptr block;
  node_ptr* pnode;

  _clear_value();

  if (!stack.size) {
    current_key.clear();
    block = storage.get_block(root);
    pnode = &block.trie()->root;
  } else {
    Transition& b = stack.back();
    block = b.block;
    pnode = b.pnode;
  }

  last_root = 0;
  int keypos = 0;
  int index = 0;
  bool go_on = true;
  while (go_on && rest_key.size()) {
    Node* node = pnode->resolve();
    node_ptr* next;
    keypos = current_key.size();

    switch (pnode->type) {
      case kUpperTrie:
        index = UpperTrieNode::calc_index(rest_key[0]);
        next = (node_ptr*)&node->trie.children[index];
        go_on = next->is_valid();
        break;

      case kLowerTrie:
        index = LowerTrieNode::calc_index(rest_key[0]);
        next = (node_ptr*)&node->trie.children[index];
        if (next->is_valid()) {
          current_key.push_back(rest_key[0]);
          rest_key.iadvance(1);
        } else {
          go_on = false;
        }
        break;

      case kArray:
        go_on = false;
        for (index = 0; index < node->atrie.size; index++) {
          if (node->atrie.keys[index] == rest_key[0]) {
            go_on = true;
            next = (node_ptr*)&node->atrie.children[index];
            current_key.push_back(rest_key[0]);
            rest_key.iadvance(1);
          }
        }
        break;

      case kString:
        index = node->string.find(rest_key);
        go_on = index == 0;
        if (go_on) {
          current_key.append(rest_key.data(), node->string.size);
          rest_key.iadvance(node->string.size);
          next = &node->string.child;
        }
        break;

      case kValue:
        assert(rest_key.size());
        index = 1;
        next = &node->value.child;
        go_on = next->is_valid();
        break;

      case kLink:
        index = 0;
        block = storage.get_block(node->link.link);
        last_root = stack.size;
        break;

      case kNull:
        go_on = false;
        break;
    }

    stack.push(block, pnode, keypos, index);
    pnode = next;
  }

  if (rest_key.empty() && go_on) {
    index = (pnode->type == kValue) ? 0 : -1;
    stack.push(block, pnode, keypos, index);
  }

  check_offsets(stack);
}

INLINE void Trace::first() {}

INLINE void Trace::last() {}

INLINE void Trace::next() {}

INLINE void Trace::prev() {}

INLINE void Trace::remove() {}

INLINE bool Trace::is_valid() const {
  if (rest_key.empty() && stack.size) return stack.back().pnode->type == kValue;
  return false;
}

INLINE void Trace::_make_stack_writable() {
  // not a writable page -> change all pages in the cursor stack to writeables
  block_ptr block;

  assert(stack.size > 0);
  int fri = 0;  // first index of a readonly block
  for (; stack.data[fri].block->txn_id == storage.txn.txn_id; fri++);
  assert(fri < stack.size);

  if (!fri) {
    block = storage.clone_cow_block(root);
    storage.txn.root = root = block->offset;
  } else {
    Transition& t = stack.data[fri - 1];
    assert(t.pnode->type == kLink);
    LinkNode& link = (*t.pnode)->link;
    block = storage.clone_cow_block(link.link);
    link.link = block->offset;
  }

  for (; fri < stack.size; fri++) {
    Transition& t = stack.data[fri];
    assert(t.block->txn_id != storage.txn.txn_id);
    // the pnodes are pointer with the same offsets to the new block
    t.rebase(block);

    if (t.pnode->type == kLink) {
      Node* node = t.pnode->resolve();
      block = storage.clone_cow_block(node->link.link);
      node->link.link = block->offset;
    }
  }
  check_offsets(stack);
}

INLINE void Trace::set_value(Slice value) {
  if (!transaction_active) {
    if (!storage.start_transaction()) throw TransactionActive();
    transaction_active = true;
  }
  _prepare_trie();
  _add_key_to_trie();

  Transition& back = stack.back();
  assert(back.pnode->type == kValue);

  ValueNode& vnode = back.pnode->resolve()->value;
  size_t size;

  if (vnode.bigval && vnode.index) {
    // remove old big_value
    current_value = storage.get_block(back.block.trie()->value);
    auto old = current_value.val()->get_value(vnode.index);
    assert(old.size() == sizeof(offset_ptr));
    storage.free_block(storage.get_block(*(offset_ptr*)old.data()));
  }

  if (value.size() > ValueNode::SMALL_SIZE) {
    size = sizeof(offset_ptr);
    vnode.bigval = 1;
    current_big_value = storage.write_value(value);
    value = Slice((char*)&current_big_value.ptr, sizeof(offset_ptr));
  } else {
    vnode.bigval = 0;
    size = value.size();
  }

  if (!back.block.trie()->value) {
    // first value in this block
    assert(vnode.index == 0);
    if (!value.size()) return; // No Value

    auto pool_id = get_pool(size + ValueBlock::INITAL_OVERHEAD);
    current_value = storage.alloc_block(pool_id);
    back.block.trie()->value = current_value->offset;
    vnode.index = current_value.val()->set_init_value(value);
    return;
  }

  auto old_value = storage.get_block(back.block.trie()->value);
  auto new_size = old_value.val()->calc_copy_size(vnode.index, value);
  auto pool_id = get_pool(new_size);

  current_value = storage.alloc_block(pool_id);
  vnode.index =
      current_value.val()->copy_block(old_value.val(), vnode.index, value);
  back.block.trie()->value = current_value->offset;

  storage.free_block(old_value);
}

INLINE void Trace::_prepare_trie() {
  // change the existing trie structure and prepare it for adding a new key

  Transition& back = stack.back();
  if (back.block->txn_id != storage.txn.txn_id) _make_stack_writable();
  if (!rest_key.empty()) {
    switch (back.pnode->type) {
      case kUpperTrie:
        (*back.pnode)->utrie.insert(*this);
        break;

      case kLowerTrie:
        (*back.pnode)->ltrie.insert(*this);
        break;

      case kArray:
        (*back.pnode)->atrie.insert(*this);
        break;

      case kString:
        (*back.pnode)->string.insert(*this);
        break;

      case kValue:
        break;

      case kNull:
        assert(stack.size == 1);  // an emtpy trie
        _add_string(back.block, back.pnode);
    }
  } else if (back.pnode->type != kValue) {
    // insert a value node
    ValueNode& value_ = back.block.trie()->alloc(sizeof(ValueNode))->value;
    value_.child = *back.pnode;
    back.pnode->set(&value_, kValue);
  }

  ssize_t min_size =
      rest_key.size() ? NODE_SIZE + MIN_NODE_SIZE : sizeof(ValueBlock);
  if (back.block.trie()->needs_split(min_size)) {
    _split_block();
  }
}

INLINE node_ptr* Trace::_add_string(block_ptr block, node_ptr* pnode) {
  assert(rest_key.size());
  Node* node = block.trie()->alloc(NODE_SIZE);
  if (rest_key.size() > 1) {
    pnode->set(node, kString);
    node->string.add_key(*this);
  } else {
    // only one char left use an ArrayNode not a StringNode
    pnode->set(node, kArray);
    node->atrie.add_key(*this);
  }
  return &node->trie.children[0];
}

INLINE void Trace::_add_key_to_trie() {
  // Adds the rest_key to trie

  Transition& back = stack.back();
  node_ptr* pnode = &(*back.pnode)->trie.children[back.index];
  block_ptr block = back.block;

  if (rest_key.size()) {
    assert(!pnode->is_valid());
    assert(block.trie()->free_space() >= sizeof(StringNode) + sizeof(LinkNode));
    while (rest_key.size()) {
      if (block.trie()->free_space() < sizeof(StringNode) + sizeof(ValueNode)) {
        // continue in a new block
        assert(block.trie()->free_space() >= sizeof(LinkNode));
        block_ptr new_block = storage.alloc_block(TrieBlock::POOL_ID);
        Node* link = block.trie()->alloc(sizeof(LinkNode));
        pnode->set(link, kLink);
        (*pnode)->link.link = new_block->offset;
        stack.push(block, pnode, current_key.size(), 0);
        block = new_block;
        pnode = &block.trie()->root;
      }

      stack.push(block, pnode, current_key.size(), 0);
      pnode = _add_string(block, pnode);
    }
  } else if (back.pnode->type == kValue) {
    // nothing has to be inserted just change value
    return;
  }

  stack.push(block, pnode, current_key.size(), 0);
  if (!pnode->is_valid()) {
    assert(block.trie()->free_space() >= sizeof(ValueNode));
    Node* node = block.trie()->alloc(sizeof(ValueNode));
    pnode->set(node, kValue);
  }
}

template <typename TrieType>
void move_node(node_ptr* psrc, node_ptr* pdest, TrieBlock& block,
               int space_left, DBMemory& storage) {
  TrieType& src = TrieType::cast(*psrc);
  space_left = src.reduce_space(space_left);
  if (space_left < src.min_space()) {
    block_ptr new_block = storage.alloc_block(block.POOL_ID);
    TrieBlock& tblock = *new_block.trie();
    move_node<TrieType>(psrc, &tblock.root, tblock, block.MAX_SPACE, storage);

    Node* lnode = block.alloc(sizeof(LinkNode));
    pdest->set(lnode, kLink);
    lnode->link.link = tblock.offset;
    return;
  }
  TrieType& dest = TrieType::cast(block.alloc());
  pdest->set(&dest, TrieType::ntype);
  src.move(dest, block, space_left, storage);
}

INLINE void move_node(node_ptr* psrc, node_ptr* pdest, TrieBlock& block,
                      int space_left, DBMemory& storage) {
  switch (psrc->type) {
    case kNull:
      return;

    case kUpperTrie:
      move_node<UpperTrieNode>(psrc, pdest, block, space_left, storage);
      return;

    case kLowerTrie:
      move_node<LowerTrieNode>(psrc, pdest, block, space_left, storage);
      return;

    case kArray:
      move_node<ArrayNode>(psrc, pdest, block, space_left, storage);
      return;

    case kString:
      move_node<StringNode>(psrc, pdest, block, space_left, storage);
      return;

    case kValue:
      move_node<ValueNode>(psrc, pdest, block, space_left, storage);
      return;

    case kLink:
      move_node<LinkNode>(psrc, pdest, block, space_left, storage);
  }
}

INLINE void Trace::_split_block() {
  block_ptr block = stack.back().block;
  block_ptr dest_block = storage.alloc_block(TrieBlock::POOL_ID);
  TrieBlock& dest = *(dest_block.trie());
  move_node(&block.trie()->root, &dest.root, dest, dest.MAX_SPACE, storage);

  storage.free_block(block, dest.POOL_ID);

  if (last_root > 0) {
    assert(stack.data[last_root].pnode->type == kLink);
    (*stack.data[last_root].pnode)->link.link = dest.offset;
  } else {
    storage.txn.root = root = dest.offset;
  }

  int delta = current_key.size() - stack.data[last_root].keypos;
  rest_key = Slice(rest_key.data() - delta, rest_key.size() + delta);
  current_key.resize(stack.data[last_root].keypos);
  stack.size = last_root;
  _find();
}

INLINE void TrieNode::move(TrieNode& dest, TrieBlock& block, int space_left,
                           DBMemory& storage) {
  for (int i = 0; i < CHILDREN; i++) {
    move_node(&children[i], &dest.children[i], block, space_left, storage);
  }
}

INLINE void ArrayNode::move(ArrayNode& dest, TrieBlock& block, int space_left,
                            DBMemory& storage) {
  dest.size = size;
  for (int i = 0; i < size; i++) {
    move_node(&children[i], &dest.children[i], block, space_left, storage);
  }
}

INLINE void StringNode::move(StringNode& dest, TrieBlock& block, int space_left,
                             DBMemory& storage) {
  dest.size = size;
  memcpy(dest.key, key, size);
  move_node(&child, &dest.child, block, space_left, storage);
}

INLINE void ValueNode::move(ValueNode& dest, TrieBlock& block, int space_left,
                            DBMemory& storage) {
  dest.index = index;
  move_node(&child, &dest.child, block, space_left, storage);
}

INLINE void LinkNode::move(LinkNode& dest, TrieBlock& block, int space_left,
                           DBMemory& storage) {
  dest.link = link;
}

size_t dump_node(std::ostream& out, const TrieBlock* page, node_ptr nid,
                 DBMemory* storage, int upper);

INLINE Slice Trace::get_value() {
  if (!is_valid()) return Slice();

  const Transition& back = stack.back();
  ValueNode& value = (*back.pnode)->value;
  if (!value.index) return Slice();  // no value

  current_value = storage.get_block(back.block.trie()->value);
  ValueBlock& cv = *current_value.val();
  const Slice& val = cv.get_value(value.index);
  if (value.bigval) {
    assert(val.size() == sizeof(offset_ptr));
    current_big_value = storage.get_block(*(offset_ptr*)val.data());
    BigValueBlock* bval = current_big_value.bval();
    return Slice(bval->data, bval->size);
  }

  current_big_value.reset();
  return val;
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
