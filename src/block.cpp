#ifndef _LEAVES_BLOCKS_CPP
#define _LEAVES_BLOCKS_CPP

#include "block.hpp"

#include <leaves.hpp>

#include "trace.hpp"

#ifdef DEBUG
#include <algorithm>
#include <sstream>
#endif

namespace leaves {

INLINE bool Compressed::find(Trace& trace) const {
  uint16_t size_ = std::min(size_, (uint16_t)trace.rest_key.size());
  const char* rest_key = trace.rest_key.data();
  uint16_t i = 0;
  for (; i < size_; i++) {
    if (key[i] != rest_key[i]) break;
  }
  trace.stack.back().prefix = i;
  trace.advance_key(i);
  TESTPOINT(Compressed::find);
  return i == size;
}

INLINE const offset_ptr* ArrayBranch::find(Trace& trace) const {
  if (trace.rest_key.empty()) {
    trace.stack.back().index = -1;
    return nullptr;
  }
  uint8_t key = trace.rest_key[0];
  char i = 0;
  for (; i < size; i++) {
    if (keys[i] == key) {
      trace.stack.back().index = i;
      trace.advance_key(1);
      return &links[i];
    }
  }
  trace.stack.back().index = -1;
  return nullptr;
}

INLINE const offset_ptr* TrieBranch::find(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.index = trace.rest_key[0];
  if (bits[idx(back.index)] & ((uint64_t)1 << bit(back.index))) {
    trace.advance_key(1);
    return &links[index(back.index)];
  }
  return nullptr;
}

INLINE void Leaf::find(Trace& trace) const {
  Transition& back = trace.stack.back();

  back.found_leaf = this;

  uint16_t size_ = std::min(key_size, (uint16_t)trace.rest_key.size());
  const char* rest_key = trace.rest_key.data();
  uint16_t i = 0;
  for (; i < size_; i++) {
    if (key_value[i] != rest_key[i]) break;
  }
  back.suffix = i;
  trace.advance_key(back.suffix);

  back.success = back.suffix == key_size && trace.rest_key.size() == 0;
}


INLINE bool BranchBlock::find(Trace& trace) const {
  Transition& back = trace.stack.back();

  uint16_t ioffset = 0;
  if (has_compressed()) {
    const Compressed* n = (const Compressed*)&data[ioffset];
    if (!n->find(trace)) return false;
    ioffset += n->nodesize();
  }

  if (has_null_leaf()) {
    if (trace.rest_key.empty()) {
      back.index = -2;
      return back.follow_link(trace, (const offset_ptr*)&data[ioffset]);
    }

    ioffset += sizeof(offset_ptr);
  }

  if (has_array()) {
    const ArrayBranch* n = (const ArrayBranch*)&data[ioffset];
    const offset_ptr* link = n->find(trace);
    if (!link) return false;
    return back.follow_link(trace, link);
  }

  if (has_trie()) {
    const TrieBranch* n = (const TrieBranch*)&data[ioffset];
    const offset_ptr* link = n->find(trace);
    if (!link) return false;
    return back.follow_link(trace, link);
  }

  // special case there is only one item in the trie: a leaf
  assert(back.branch->used = sizeof(offset_ptr));
  assert(!back.branch->has_compressed());
  assert(!back.branch->has_array());
  assert(!back.branch->has_trie());
  assert(back.branch->has_null_leaf());
  assert(trace.stack.size == 1);
  back.index = -2;
  return back.follow_link(trace, (offset_ptr*)back.branch->data);
}


#ifdef DEBUG

INLINE std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit) && bit != '"' && bit != '<' && bit != '>' && bit != ']' &&
      bit != '\\' && bit != '}' && bit != '{') {
    cstr << bit;
  } else {
    cstr << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
  return cstr.str();
}


void dump_leaf(std::ostream& out, offset_ptr offset, block_ptr branch, DBMemory *storage) {
  block_ptr lb = storage->get_block(branch->leaves);
  LeafBlock* block = lb.leaf();

  out << "id: " << offset.merge(lb->offset) << std::endl;
  out << "block: " << branch->offset.offset() << std::endl;
  out << "type: leaf" << std::endl;
  
  const Leaf* leaf = block->leaf(offset);

  out << "keysize: " << leaf->key_size << std::endl;
  out << "key: \"";
  for (int i = 0; i < leaf->key_size; i++) {
    out << "[" << bitstr(leaf->key_value[i]) << "]";
  }
  out << "\"" << std::endl;

  out << "valuesize: " << leaf->value_size << std::endl;
  out << "value: \"";
  for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)10);
        i < end; i++) {
    out << "[" << bitstr(leaf->key_value[i + leaf->key_size]) << "]";
  }
  out << "\"" << std::endl;
  out << "---" << std::endl;
}

void dump_branch(std::ostream& out, offset_ptr offset, DBMemory* storage);

void dump_link(std::ostream& out, block_ptr parent, offset_ptr offset, DBMemory* storage) {
  if(offset.pool_id() == LEAF_BLOCK) {
    dump_leaf(out, offset, parent, storage);
    return;
  }
  assert(offset.pool_id() < AREA_COUNT);
  dump_branch(out, offset, storage);
}

void dump_branch(std::ostream& out, offset_ptr offset, DBMemory* storage) {
  block_ptr block = storage->get_block(offset);
  block_ptr lb = storage->get_block(block->leaves);
  LeafBlock* leaf = lb.leaf();
    
  out << "id: " << offset.offset() << std::endl;
  out << "block: " << block->offset.offset() << std::endl;
  out << "size: " << block->block_size() << std::endl;
  out << "space: " << block->space() << std::endl;
  out << "freespace: " << block->freespace() << std::endl;
  out << "leaf_size: " << leaf->block_size() << std::endl;
  out << "leaf_space: " << leaf->space() << std::endl;
  out << "leaf_free: " << leaf->space() - block->leaves_used << std::endl;
  out << "leaf_frag: " << block->leaves_free << std::endl;
  out << "type: branch" << std::endl;

  bsize_t ioffset = 0;
  if (block->has_compressed()) {
    const Compressed* n = (const Compressed*)&block->data[ioffset];
    out << "compressed: " << std::endl;
    out << "  size: " << (int)n->size << std::endl;
    out << "  key: \"";
    for (int i = 0; i < n->size; i++) {
      out << "[" << bitstr(n->key[i]) << "]";
    }
    out << "\"" << std::endl;
    ioffset += n->nodesize();
  }

  offset_ptr null_child = offset_ptr{.data = 0};
  if (block->has_null_leaf()) {
    null_child = *(const offset_ptr*)&block->data[ioffset];
    assert(null_child.pool_id() == LEAF_BLOCK);
    out << "nulllink: " << null_child.merge(block->leaves) << std::endl;
    ioffset += sizeof(offset_ptr);
  }

  if (block->has_array()) {
    const ArrayBranch* n = (const ArrayBranch*)&block->data[ioffset];
    out << "branch: array" << std::endl;
    out << "key: \"";
    for (int i = 0; i < n->size; i++) {
      out << "[" << bitstr(n->keys[i]) << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    for (int i = 0; i < n->size; i++) {
      out << "  - " << n->links[i].merge(block->leaves) << std::endl;
    }

    out << "---" << std::endl;

    if (null_child) dump_link(out, block, null_child, storage);

    for (int i = 0; i < n->size; i++) {
      dump_link(out, block, n->links[i], storage);
    }
    return;
  }

  if (!block->has_trie()) {
    // value without key
    assert(null_child);
    out << "---" << std::endl;
    dump_link(out, block, null_child, storage);
    return;
  }

  const TrieBranch* n = (const TrieBranch*)&block->data[ioffset];
  out << "branch: trie" << std::endl;
  out << "key: \"";
  for (int i = 0; i < 256; i++) {
    if (n->bits[TrieBranch::idx(i)] & (((uint64_t) 1) << TrieBranch::bit(i))) {
      out << "[" << bitstr(i) << "]";
    }
  }
  out << "\"" << std::endl;
  out << "children: " << std::endl;
  for (int i = 0, end = n->count(); i < end; i++) {
    out << "  - " << n->links[i].merge(block->leaves) << std::endl;
  }

  out << "---" << std::endl;

  if (null_child) dump_link(out, block, null_child, storage);

  for (int i = 0, end = n->count(); i < end; i++) {
    if (n->links[i]) {
      dump_link(out, block, n->links[i], storage);
    }
  }
}

#endif

}  // namespace leaves

#endif  // _LEAVES_BLOCKS_CPP