#include "trace.hpp"

#include "node.hpp"

namespace leaves {

INLINE Trace::Trace(Storage& storage_)
    : storage(storage_), transaction_active(false) {
  const PersistBlock& root_block = storage.memory->get_root()->block;
  root = root_block.offset;
  cursor_id = storage.alloc_cursor(root_block.transaction);
  current_key.reserve(1024);
  stack.resize(128);
}

INLINE Trace::~Trace() { storage.free_cursor(cursor_id); }

INLINE void Trace::find(const Slice& key) {
  if (key.size() > 1024) throw KeyToBig();

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

INLINE bool Trace::set_value(const Slice& value) {
  if (value.size() > T - sizeof(PersistBlock))
    throw WrongValue("value too big");

  if (!transaction_active) {
    if (!storage.start_transaction()) return false;
    transaction_active = true;
  }

  Transition& back = stack.back();

  if (back.block->block.transaction != storage.active_transaction) {
    // not a writable page -> change all pages in the cursor stack to writeables
    offset_ptr offset = 0;
    for (auto iter = stack.rbegin(); iter != stack.rend();) {
      if (iter->block->block.transaction != storage.active_transaction) {
        BlockUnion* block = storage.get_cow_block(iter->offset);
        block->block.transaction = storage.active_transaction;
        iter->to_writable(block);
        if (iter->pnode->type == kLink) iter->node->link = offset;

        offset = block->block.offset;
        offset_ptr old_offset = iter->offset;

        for (iter--; iter != stack.rend(); iter--) {
          if (iter->offset == old_offset) iter->to_writable(block);
        }
      } else {
        if (offset) {
          assert(iter->pnode->type == kLink);
          iter->node->link = offset;
          offset = 0;
        }
        break;
      }
    }
    if (offset) {
      assert(root != offset);
      root = offset;
    }
  }

  back.set_value(*this, value);
}

INLINE Transition* Trace::alloc_in_block(ssize_t size, ssize_t dnode,
                                         NodeType type) {
  Transition* back = &stack.back();
  ssize_t offset = back->block->trie.alloc(size);
  Transition new_trans(back->offset, back->pnode->point() + dnode,
                       current_key.size());

  if (!offset) {
    // cannot allocate size

    if (size <= sizeof(offset_ptr)) return NULL;

    // try to allocate a link
    offset = back->block->trie.alloc(sizeof(offset_ptr));
    if (!offset) return NULL;  // even not space left for a link

    stack.push_back(new_trans);
    Transition& link = stack.back();
    link.pnode = (node_ptr*)&back->block->trie.data[new_trans.onode];
    link.pnode->offset = offset;
    link.pnode->type = kLink;
    link.resolve(*this);
    BlockUnion* block = storage.alloc_cow_block(TrieBlock::SIZE);
    new_trans.offset = link.node->link = block->trie.offset;
    new_trans.block = block;
    new_trans.onode = 0;
    offset = block->trie.alloc(size);
    assert(offset != 0);
  }

  stack.push_back(new_trans);
  Transition& result = stack.back();
  result.pnode = (node_ptr*)&result.block->trie.data[result.onode];
  result.pnode->offset = offset;
  result.pnode->type = type;
  result.node = (Node*)&result.block->trie.data[offset];
  return &result;
}

INLINE Transition& Trace::alloc(ssize_t size, ssize_t dnode, NodeType type) {
  // round up size to a multiple of 8
  size = ((size + 7) >> 3) << 3;

  Transition* result = alloc_in_block(size, dnode, type);
  if (result) return *result;

  move_last_node();
  return *alloc_in_block(size, dnode, type);
}

INLINE void Trace::move_last_node() {
#if 0
  Transition& back = stack.back();

  // move the current node to a new block
  TrieBlock sizes;
  memset(sizes.data, 0, sizes.DATA_SIZE);
  leaves::mark_deep_size[back.pnode->type](*this, back, sizes);

  BlockUnion* block = storage.alloc_cow_block(TrieBlock::SIZE);
  leaves::move_node[back.pnode->type](*this, back, block, 0, sizes);

  if (back.onode == 0) {
    // the moved node is the root of the block

    // free the former block
    storage.memory->free_block(back.block);

    // update the last transition
    back.offset = block->block.offset;
    back.block = block;
    back.resolve(*this);

    if (stack.size() > 1) {
      // change the link to the block
      Transition& link = stack[stack.size() - 1];
      assert(link.pnode->type == kLink);
      link.node->link = block->block.offset;
      TESTPOINT("MoveBlockRoot");
    } else {
      // It is the root block -> change the root
      root = block->block.offset;
      TESTPOINT("MoveGlobalRoot");
    }
    return;
  }

  // replace the node by a link
  Transition cloned_node(block->block.offset, 0, back.keypos);
  back.pnode->offset = back.block->trie.alloc(sizeof(offset_ptr)) >> 3;
  back.pnode->type = kLink;
  back.resolve(*this);
  back.node->link = block->block.offset;
  stack.push_back(cloned_node);
  stack.back().resolve(*this);
#endif
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

INLINE Transition& Transition::resolve(Trace& cursor) {
  if (!block) {
    block = cursor.get_block(offset);
    pnode = (node_ptr*)&block->trie.data[onode];
    node = (Node*)&block->trie.data[pnode->point()];
  }
  return *this;
}

INLINE void Transition::to_writable(BlockUnion* block_) {
  offset = block->block.offset;
  block = block_;
  pnode = (node_ptr*)&block->trie.data[onode];
  node = (Node*)&block->trie.data[pnode->point()];
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
  // leaves::set_value[pnode->type](cursor, *this, value);
}

INLINE ssize_t Transition::mark_deep_size(Trace& cursor, TrieBlock& sizes) {
  resolve(cursor);
  size_t size = leaves::mark_deep_size[pnode->type](cursor, *this, sizes);
  *(ssize_t*)&sizes.data[onode] = size;
  return size;
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