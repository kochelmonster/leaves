#include "trace.hpp"

#include "node.hpp"

namespace leaves {

INLINE Trace::Trace(Storage& storage_)
    : storage(storage_), transaction_active(false) {
  const PersistBlock& root_block = storage.memory->get_root()->block;
  root = root_block.offset;
  cursor_id = storage.alloc_cursor(root_block.offset);
  current_key.reserve(MAX_KEY_SIZE);
  stack.resize(128);
}

INLINE Trace::~Trace() { storage.free_cursor(cursor_id); }

INLINE void Trace::find(const Slice& key) {
  if (key.size() > MAX_KEY_SIZE) throw KeyToBig();

  rest_key = key;

  for (auto iter = stack.begin(); iter != stack.end(); iter++) {
    if (!iter->advance(*this)) {
      stack.erase(iter, stack.end());
      current_key.resize(iter->keypos);
      break;
    }
  }

  if (stack.empty()) {
    current_key.clear();
    stack.push_back(Transition(root));
  }

  while (stack.back().find(*this));
}

INLINE bool Trace::isvalid() const {
  if (rest_key.empty() || stack.empty()) return false;

  const Transition& back = stack.back();
  return leaves::is_valid[back.pnode->type](back);
}

INLINE void Trace::make_stack_writable() {
  // not a writable page -> change all pages in the cursor stack to writeables
  offset_ptr offset = 0;
  for (auto iter = stack.rbegin(); iter != stack.rend();) {
    if (!iter->block->block.writable) {
      BlockUnion* block = storage.get_cow_block(iter->offset);
      iter->to_writable(block);
      if (iter->pnode->type == kLink) {
        assert(offset);  // the offset of last block
        iter->node->link = offset;
      }

      offset = block->block.offset;
      offset_ptr old_offset = iter->offset;

      // change all stack items of the same block
      for (iter++; iter != stack.rend(); iter++) {
        if (iter->offset == old_offset) iter->to_writable(block);
      }
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
    root = offset;
  }
}

INLINE bool Trace::set_value(const Slice& value) {
  if (value.size() > T - sizeof(PersistBlock))
    throw WrongValue("value too big");

  if (!transaction_active) {
    if (!storage.start_transaction()) return false;
    transaction_active = true;
  }

  Transition* back = &stack.back();
  if (!back->block->block.writable) {
    make_stack_writable();
  }

  do {
    if (rest_key.empty()) {
      // put a value before the node in back
      add_value(*this, value);
      return;
    }
    stack.back().set_value(*this, value);
  } while (true);
}

INLINE Node* Trace::resolve(node_ptr node) {
  return (Node*)&back().block->trie.data[node.offset()];
}

INLINE Trace::alloc_ptr Trace::alloc(ssize_t size, NodeType type) {
  Transition* back = &stack.back();
  node_ptr result = back->block->trie.alloc(size, type);
  if (result.type) return alloc_ptr{result, false};

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
    storage.memory->head.root = root = splitter.split_block(block_root);
  }
  else {
    Transition& t = stack[i];
    assert(t.pnode->type == kLink);
    t.node->link = splitter.split_block(block_root);
  }

  stack.resize(i+1);
  
  if (stack.empty()) {
    assert(current_key.empty());
    stack.push_back(Transition(root));
  }

  // new find through splitted block
  while (stack.back().find(*this));
  return alloc_ptr{back->block->trie.alloc(size, type), true};
}

INLINE void Trace::commit() {
  storage.prepare_commit();
  storage.commit();
}

INLINE void Trace::rollback() {
  storage.rollback();
  stack.clear();
  find(current_key);
}

INLINE Transition::Transition(offset_ptr offset_, ssize_t onode_,
                              ssize_t keypos_)
    : offset(offset_), onode(onode_), keypos(keypos_), trie_state(Upper) {}

INLINE Transition Transition::derive(ssize_t donode, ssize_t keypos_) const {
  return Transition(offset, pnode->offset() + donode, keypos_);
}

INLINE Transition& Transition::clear(Trace& cursor) {
  if (!block) {
    block = cursor.get_block(offset);
    pnode = (node_ptr*)&block->trie.data[onode];
  }
  pnode->val = 0;
}

INLINE Transition& Transition::resolve(Trace& cursor) {
  if (!block) {
    block = cursor.get_block(offset);
    pnode = (node_ptr*)&block->trie.data[onode];
  }
  node = (Node*)&block->trie.data[pnode->offset()];
  return *this;
}

INLINE void Transition::to_writable(BlockUnion* block_) {
  offset = block->block.offset;
  block = block_;
  pnode = (node_ptr*)&block->trie.data[onode];
  node = (Node*)&block->trie.data[pnode->offset()];
}

INLINE bool Transition::advance(Trace& cursor) {
  return leaves::advance[pnode->type](cursor, *this);
}

INLINE bool Transition::find(Trace& cursor) {
  resolve(cursor);
  keypos = cursor.current_key.size();
  return leaves::find[pnode->type](cursor, *this);
}

INLINE Slice Transition::get_value(Trace& cursor) const {
  return leaves::get_value[pnode->type](cursor, *this);
}

INLINE void Transition::set_value(Trace& cursor, const Slice& value) {
  leaves::set_value[pnode->type](cursor, value);
}

INLINE size_t BlockSplitter::find_splitpoint(Transition& transition) {
  if (is_finished) return 0;

  transition.resolve(cursor);
  size_t size =
      leaves::find_splitpoint[transition.pnode->type](*this, transition);
  if (size > 4 * K) {
    splitpoint = transition;
    is_finished = true;
  }
  return size;
}

INLINE offset_ptr
BlockSplitter::split_block(Transition& block_root) {
  assert(block_root.onode == 0);

  find_splitpoint(block_root);

  TrieBlock* split1 = &cursor.storage.alloc_cow_block(TrieBlock::SIZE)->trie;
  node_ptr result = leaves::move[splitpoint.pnode->type](
      &splitpoint.block->trie, *splitpoint.pnode, split1);
  *((node_ptr*)&split1->data[0]) = result;

  // change the splitpoint to a link to the new block
  splitpoint.pnode->type = kLink;
  splitpoint.node->link = split1->offset;

  TrieBlock* split2 = &cursor.storage.alloc_cow_block(TrieBlock::SIZE)->trie;
  node_ptr result = leaves::move[block_root.pnode->type](
      &block_root.block->trie, *block_root.pnode, split2);

  cursor.storage.free_cow_block(block_root.block);
  return split2->offset;
}

}  // namespace leaves
/*
void first();
void last();
void next();
void prev();
void set_value(const Slice& value);
Slice get_value() const;
void remove();
void commit();
void rollback();


*/