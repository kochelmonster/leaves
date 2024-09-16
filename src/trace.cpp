#include "trace.hpp"

#include <iostream>

#include "node.hpp"

#include <fstream>

namespace leaves {

INLINE Trace::Trace(DBMemory& storage_)
    : storage(storage_), transaction_active(false), _debug_stat_page_splits(0) {
  const BlockMeta& root_block = storage.get_root()->meta;
  root = root_block.offset;
  cursor_id = storage.alloc_cursor();
  current_key.reserve(MAX_KEY_SIZE);
  stack.reserve(128);
}

INLINE Trace::~Trace() {
  storage.free_cursor(cursor_id);
  std::cout << "stat_page_splits: " << _debug_stat_page_splits << std::endl;
}

#ifdef DEBUG
inline void check_offsets(Trace::stack_v stack) {
  if (stack.empty()) return;

  offset_ptr offset = stack.begin()->offset;
  for (auto i = stack.begin(); i != stack.end(); i++) {
    assert(offset == i->offset);
    assert(i->block->meta.offset == offset);
    if (i->pnode->type == kLink) {
      offset = i->node->link;
    }
  }
}
#else
inline void check_offsets(Trace::stack_v stack) {}
#endif

INLINE void Trace::update() {
  // check if there is a new view available
  root = storage.update_cursor(cursor_id);
  if (stack.size() && stack.begin()->offset != root) stack.clear();
}

INLINE void Trace::find(const Slice& key) {
  if (key.size() > MAX_KEY_SIZE) throw KeyToBig();

  rest_key = key;

  for (auto iter = stack.begin(); iter != stack.end(); iter++) {
    if (!iter->advance(*this)) {
      stack.erase(iter + 1, stack.end());
      current_key.resize(iter->keypos);
      break;
    }
  }

  if (stack.empty()) {
    current_key.clear();
    stack.push_back(Transition(root));
  }

  while (stack.back().find(*this));

  check_offsets(stack);
}

INLINE void Trace::first() {}

INLINE void Trace::last() {}

INLINE void Trace::next() {}

INLINE void Trace::prev() {}

INLINE void Trace::remove() {}

INLINE bool Trace::isvalid() const {
  if (!rest_key.empty() || stack.empty()) return false;

  const Transition& back = stack.back();
  return leaves::is_valid[back.pnode->type](back);
}

INLINE void Trace::make_stack_writable() {
  // not a writable page -> change all pages in the cursor stack to writeables
  offset_ptr offset = 0;
  for (auto iter = stack.rbegin(); iter != stack.rend();) {
    if (iter->block->meta.txn_id != storage.txn.txn_id) {
      offset_ptr old_offset = iter->offset;
      block_ptr block = storage.clone_cow_block(iter->offset);
      iter->to_writable(block);

      if (iter->pnode->type == kLink) {
        assert(offset);  // the offset of last block
        iter->node->link = offset;
      }

      // change all stack items of the same block
      for (iter++; iter != stack.rend(); iter++) {
        if (iter->offset == old_offset)
          iter->to_writable(block);
        else
          break;
      }

      offset = block->meta.offset;
    } else {
      if (offset) {
        // parent is already writable => just change the link
        assert(iter->pnode->type == kLink);
        iter->node->link = offset;
        offset = 0;
      }
      break;
    }
  }
  if (offset) {
    // change root
    assert(root != offset);
    storage.txn.root = root = offset;
  }
  check_offsets(stack);
}

INLINE bool Trace::set_value(const Slice& value) {
  if (value.size() > T - sizeof(BlockMeta))
    throw WrongValue("value too big");

  if (!transaction_active) {
    if (!storage.start_transaction()) return false;
    transaction_active = true;
  }

  Transition* back = &stack.back();
  if (back->block->meta.txn_id != storage.txn.txn_id)
    make_stack_writable();

  do {
    if (rest_key.empty()) {
      // put a value before the node in back
      return add_value(*this, value);
    }
    stack.back().set_value(*this, value);
  } while (true);

  return false;
}

INLINE Node* Trace::resolve(node_ptr pnode) {
  return back().block->trie.resolve(pnode);
}

size_t dump_node(std::ostream& out, const TrieBlock* page, node_ptr nid,
                 DBMemory* storage, int upper);


INLINE Trace::alloc_ptr Trace::alloc(ssize_t size, NodeType type) {
  node_ptr result = stack.back().block->trie.alloc(size, type);
  if (result.type) return alloc_ptr{result, false};

  _debug_stat_page_splits++;

  int last_keypos = 0;
  // find the beginning of the last block
  int i = stack.size() - 1;
  for (; i >= 0; i--) {
    Transition& t = stack[i];
    last_keypos = t.keypos;
    if (t.pnode->type == kLink) {
      // found root of the last block
      break;
    }
  }

  rest_key = Slice(rest_key.data() - current_key.size() + last_keypos,
                   rest_key.size() + current_key.size() - last_keypos);
  current_key.resize(last_keypos);

  Transition& block_root = stack[i + 1];
  BlockSplitter splitter(*this);

  if (i < 0) {
    storage.txn.root = root = splitter.split_block(block_root);
  } else {
    Transition& t = stack[i];
    assert(t.pnode->type == kLink);
    t.node->link = splitter.split_block(block_root);
  }

  stack.resize(i + 1);

  if (stack.empty()) {
    assert(current_key.empty());
    stack.push_back(Transition(root));
  }
  // new find through splitted block
  while (stack.back().find(*this));
  return alloc_ptr{stack.back().block->trie.alloc(size, type), true};
}

INLINE Slice Trace::get_value() const {
  if (!isvalid()) return Slice();

  Value& value = stack.back().node->value;
  if (value.size <= Value::SMALL_SIZE) {
    return Slice(value.data, value.size);
  }
  block_ptr block = get_block(value.link);
  return Slice(block->value.data, value.size);
}

INLINE void Trace::commit() {
  storage.prepare_commit();
  storage.commit();

  transaction_active = false;
  // update stack to not writeable
  for (auto iter = stack.begin(); iter != stack.end(); iter++) {
    iter->block = NULL;
    iter->resolve(*this);
  }
  update();
}

INLINE void Trace::rollback() {
  storage.rollback();
  stack.clear();
  transaction_active = false;
  find(current_key);
}

INLINE Transition::Transition(offset_ptr offset_, ssize_t onode_,
                              ssize_t keypos_)
    : offset(offset_),
      block(NULL),
      node(NULL),
      pnode(NULL),
      onode(onode_),
      keypos(keypos_),
      trie_state(Upper),
      index(-1) {}

INLINE Transition Transition::derive(ssize_t donode, ssize_t keypos_) const {
  return Transition(offset, pnode->offset() + donode, keypos_);
}

INLINE Transition& Transition::clear(Trace& cursor) {
  if (!block) {
    block = cursor.get_block(offset);
    pnode = (node_ptr*)&block->trie.data[onode];
  }
  pnode->type = 0;
  keypos = cursor.current_key.size();
  return *this;
}

INLINE Transition& Transition::resolve(Trace& cursor) {
  if (!block) {
    block = cursor.get_block(offset);
    assert(block->meta.type == kTrieBlock);
    pnode = block->trie.resolve_ptr(onode);
  }
  node = block->trie.resolve(*pnode);
  return *this;
}

INLINE void Transition::to_writable(block_ptr block_) {
  block = block_;
  offset = block->meta.offset;
  pnode = block->trie.resolve_ptr(onode);
  node = block->trie.resolve(*pnode);
}

INLINE bool Transition::advance(Trace& cursor) {
  return leaves::advance[pnode->type](cursor, *this);
}

INLINE bool Transition::find(Trace& cursor) {
  resolve(cursor);
  keypos = cursor.current_key.size();
  return leaves::find[pnode->type](cursor, *this);
}

INLINE void Transition::set_value(Trace& cursor, const Slice& value) {
  leaves::set_value[pnode->type](cursor, value);
}

INLINE size_t BlockSplitter::find_splitpoint(Transition& transition) {
  if (is_finished) return 0;

  transition.resolve(cursor);
  size_t size =
      leaves::find_splitpoint[transition.pnode->type](*this, transition);
  if (!is_finished && size > TrieBlock::SPLIT) {
    splitpoint = transition;
    is_finished = true;
  }
  return size;
}

INLINE offset_ptr BlockSplitter::split_block(Transition& block_root) {
  assert(block_root.onode == 0);

  find_splitpoint(block_root);

  if (splitpoint.offset) {
    TrieBlock* split = &cursor.storage.alloc_cow_block()->trie;
    node_ptr result = leaves::move[splitpoint.pnode->type](
        &splitpoint.block->trie, *splitpoint.pnode, split);
    *split->resolve_ptr(0) = result;

    // change the splitpoint to a link to the new block
    splitpoint.pnode->type = kLink;
    splitpoint.node->link = split->offset;
  }

  TrieBlock* split = &cursor.storage.alloc_cow_block()->trie;
  node_ptr result = leaves::move[block_root.pnode->type](
      &block_root.block->trie, *block_root.pnode, split);
  *split->resolve_ptr(0) = result;

  cursor.storage.free_cow_block(block_root.block);
  return split->offset;
}

}  // namespace leaves
