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
  trace.current_key.append(trace.rest_key.data(), i);
  trace.rest_key.iadvance(i);
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
      trace.current_key.push_back(trace.rest_key[0]);
      trace.rest_key.iadvance(1);
      return &links[i];
    }
  }
  trace.stack.back().index = -1;
  return nullptr;
}

INLINE const offset_ptr* TrieBranch::find(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.index = trace.rest_key[0];
  if (bits[back.tindex.idx] & (1 << back.tindex.bit)) {
    trace.current_key.push_back(back.index);
    trace.rest_key.iadvance(1);
    return &links[index(back.index)];
  }
  return nullptr;
}

INLINE void Leaf::find(Trace& trace) const {
  Transition& back = trace.stack.back();

  if (key_size) {
    uint16_t size_ = std::min(key_size, (uint16_t)trace.rest_key.size());
    const char* rest_key = trace.rest_key.data();
    uint16_t i = 0;
    for (; i < size_; i++) {
      if (key_value[i] != rest_key[i]) break;
    }
    back.suffix = i;
    trace.current_key.append(trace.rest_key.data(), i);
    trace.rest_key.iadvance(i);
  }
  if (back.suffix == key_size && trace.rest_key.empty()) back.leaf = this;
}

INLINE bool Block::follow_link(Trace& trace, const offset_ptr* link) const {
  assert(link->offset);

  assert(data + MAX_BRANCH_SPACE > (const uint8_t*)link);
  trace.stack.back().olink = (const uint8_t*)link - data;

  if (link->start() != offset.start()) {
    // another block
    trace.push(*link);
    return true;
  }
  
  leaf(*link)->find(trace);
  return false;
}

INLINE bool Block::find(Trace& trace) const {
  Transition& back = trace.stack.back();

  uint16_t ioffset = back.offset.ioffset();
  if (ioffset) {
    leaf(back.offset)->find(trace);
    return false;
  }

  if (has_compressed()) {
    const Compressed* n = (const Compressed*)&data[ioffset];
    if (!n->find(trace)) return false;
    ioffset += n->nodesize();
  }

  if (has_value()) {
    if (trace.rest_key.empty()) {
      back.index = -2;
      return follow_link(trace, (const offset_ptr*)&data[ioffset]);
    }

    ioffset += sizeof(offset_ptr);
  }

  if (has_array()) {
    const ArrayBranch* n = (const ArrayBranch*)&data[ioffset];
    const offset_ptr* link = n->find(trace);
    if (!link) return false;
    return follow_link(trace, link);
  }

  if (has_trie()) {
    const TrieBranch* n = (const TrieBranch*)&data[ioffset];
    const offset_ptr* link = n->find(trace);
    if (!link) return false;
    return follow_link(trace, link);
  }

  return false;
}

INLINE offset_ptr* Block::find_leaf_to_move(bsize_t space_) {
  uint8_t* p = &data[upper_bound];
  uint8_t* end = &data[space()];
  while (p < end) {
    const Leaf* leaf = (Leaf*)leaf;
    bsize_t lsize = leaf->nodesize();
    if (lsize >= space_) break;
    p += lsize;
  }

  offset_ptr cmp_offset = offset_ptr{.pool_id = offset.pool_id,
                                     .offset = offset.mask() | (end - p)};
  // find the link to the leaf
  bsize_t ioffset = 0;
  if (has_compressed()) ioffset = ioffset + compressed()->nodesize();
  if (has_value()) {
    offset_ptr* ptr = (offset_ptr*)&data[ioffset];
    if (*ptr == cmp_offset) return ptr;
    ioffset += sizeof(offset_ptr);
  }
  offset_ptr *i, *pend = (offset_ptr*)&data[lower_bound];
  i = has_trie() ? ((TrieBranch*)&data[ioffset])->links
                 : ((ArrayBranch*)&data[ioffset])->links;
  for (; *i != cmp_offset && i != pend; i++);
  return i;
}

INLINE void Block::change_leaf_blocks(const offset_ptr& old_,
                                      const offset_ptr& new_) {
  uint64_t old_start = old_.start();
  uint64_t new_start = new_.start();
  bsize_t ioffset = 0;

  leaves.change_block(old_start, new_start);
  if (has_compressed()) ioffset += compressed()->nodesize();
  if (has_value()) {
    ((offset_ptr*)&data[ioffset])->change_block(old_start, new_start);
    ioffset += sizeof(offset_ptr);
  }
  if (has_array())
    ((ArrayBranch*)&data[ioffset])->change_block(old_start, new_start);
  else if (has_trie())
    ((TrieBranch*)&data[ioffset])->change_block(old_start, new_start);
}

INLINE void Block::move_leaf_ioffsets(const offset_ptr& pivot, int delta) {
  bsize_t ioffset = 0;

  if (has_compressed()) ioffset += compressed()->nodesize();
  if (has_value()) {
    ((offset_ptr*)&data[ioffset])->move_ioffset(pivot, delta);
    ioffset += sizeof(offset_ptr);
  }
  if (has_array()) {
    ((ArrayBranch*)&data[ioffset])->move_ioffset(pivot, delta);
    return;
  }
  assert(has_trie());
  ((TrieBranch*)&data[ioffset])->move_ioffset(pivot, delta);
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

void dump_block(std::ostream& out, offset_ptr offset, DBMemory* storage) {
  block_ptr block = storage->get_block(offset);

  out << "id: " << offset.offset << std::endl;
  out << "block: " << block->offset.offset << std::endl;
  out << "size: " << block->block_size() << std::endl;
  out << "space: " << block->space() << std::endl;
  out << "freespace: " << block->freespace() << std::endl;

  if (offset.ioffset()) {
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
    return;
  }

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

  offset_ptr null_child = offset_ptr{.offset = 0};
  if (block->has_value()) {
    null_child = *(const offset_ptr*)&block->data[ioffset];
    out << "nulllink: " << null_child.offset << std::endl;
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
      out << "  - " << n->links[i].offset << std::endl;
    }

    out << "---" << std::endl;

    if (null_child) dump_block(out, null_child, storage);

    for (int i = 0; i < n->size; i++) {
      if (n->links[i].offset) dump_block(out, n->links[i], storage);
    }
    return;
  }

  if (!block->has_trie()) {
    // value without key
    assert(null_child);
    out << "---" << std::endl;
    dump_block(out, null_child, storage);
    return;
  }

  const TrieBranch* n = (const TrieBranch*)&block->data[ioffset];
  out << "branch: trie" << std::endl;
  out << "key: \"";
  for (int i = 0; i < 256; i++) {
    TrieBranch::Index idx = {.val = (uint8_t)i};
    if (n->bits[idx.idx] & (1 << idx.bit)) {
      out << "[" << bitstr(i) << "]";
    }
  }
  out << "\"" << std::endl;
  out << "children: " << std::endl;
  for (int i = 0, end = n->count(); i < end; i++) {
    out << "  - " << n->links[i].offset << std::endl;
  }

  out << "---" << std::endl;

  if (null_child) dump_block(out, null_child, storage);

  for (int i = 0, end = n->count(); i < end; i++) {
    if (n->links[i].offset) {
      dump_block(out, n->links[i], storage);
    }
  }
}

#endif

}  // namespace leaves

#endif  // _LEAVES_BLOCKS_CPP