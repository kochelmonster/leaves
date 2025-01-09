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

void find_(Trace& trace, const Leaf* leaf) { leaf->find(trace); }
void step_(Trace& trace, const Leaf* leaf) { leaf->step(trace); }

/* find
-------------------------
*/

INLINE bool Compressed::find(Trace& trace) const {
  Transition& back = trace.stack.back();
  size_t same = get_prefix(trace.rest_key.data(), (const char*)key,
                           trace.rest_key.size(), size, back.cmp);
  back.prefix = same;
  trace.advance_key(same);
  TESTPOINT(Compressed::find);
  return same == size;
}

INLINE uint16_t Compressed::step(Trace& trace) const {
  trace.stack.back().prefix = size;
  trace.current_key.append((char*)key, size);
  return nodesize();
}

INLINE const offset_ptr* ArrayBranch::find(Trace& trace) const {
  Transition& back = trace.stack.back();

  assert(size <= ArrayBranch::COUNT);
  back.branch_key = trace.rest_key[0];
  char i = 0;
  for (; i < size; i++) {
    if (keys[i] == back.branch_key) {
      trace.advance_key(1);
      return &links[i];
    }
  }
  back.cmp = Transition::NOT_SAME;
  return nullptr;
}

INLINE const offset_ptr* ArrayBranch::first(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.branch_key = 0xff;
  char imin = 0;
  for (char i = 0; i < size; i++) {
    if (keys[i] <= back.branch_key) {
      back.branch_key = keys[i];
      imin = i;
    }
  }
  trace.current_key.push_back(back.branch_key);
  return &links[imin];
}

INLINE const offset_ptr* ArrayBranch::last(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.branch_key = 0;
  char ilast = -1;
  for (char i = 0; i < size; i++) {
    if (keys[i] >= back.branch_key) {
      back.branch_key = keys[i];
      ilast = i;
    }
  }
  trace.current_key.push_back(back.branch_key);
  return &links[ilast];
}

INLINE const offset_ptr* ArrayBranch::next(Trace& trace) const {
  Transition& back = trace.stack.back();
  uint8_t max = 0xff, min = back.branch_key;
  char inext = -1;
  for (char i = 0; i < size; i++) {
    if (keys[i] > min && keys[i] <= max) {
      max = back.branch_key = keys[i];
      inext = i;
    }
  }
  if (inext >= 0) {
    trace.changed_branch_key();
    return &links[inext];
  }
  /* trace.current_key.pop_back(); not needed:
    this node will be removed from stack and
    current_key resized to the nodes keypos
  */
  return nullptr;
}

INLINE const offset_ptr* ArrayBranch::prev(Trace& trace) const {
  Transition& back = trace.stack.back();
  uint8_t min = 0, max = back.branch_key;
  char iprev = -1;
  for (char i = 0; i < size; i++) {
    if (keys[i] >= min && keys[i] < max) {
      min = back.branch_key = keys[i];
      iprev = i;
    }
  }

  if (iprev >= 0) {
    trace.changed_branch_key();
    return &links[iprev];
  }
  trace.current_key.resize(back.keypos + back.prefix);
  // null_key might come
  return nullptr;
}

INLINE const offset_ptr* TrieBranch::find(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.branch_key = trace.rest_key[0];
  if (bits[idx(back.branch_key)] & ((uint64_t)1 << bit(back.branch_key))) {
    trace.advance_key(1);
    return &links[index(back.branch_key)];
  }
  back.cmp = Transition::NOT_SAME;
  return nullptr;
}

INLINE const offset_ptr* TrieBranch::first(Trace& trace) const {
  Transition& back = trace.stack.back();
  for (char i = 0; i < 4; i++) {
    if (bits[i]) {
      back.branch_key = i * 64 + ctz(bits[i]);
      break;
    }
  }
  trace.current_key.push_back(back.branch_key);
  return &links[0];
}

INLINE const offset_ptr* TrieBranch::last(Trace& trace) const {
  Transition& back = trace.stack.back();
  for (char i = 3; i >= 0; i--) {
    if (bits[i]) {
      back.branch_key = i * 64 + (63 - clz(bits[i]));
      break;
    }
  }
  trace.current_key.push_back(back.branch_key);
  return &links[count() - 1];
}

template <typename F>
const offset_ptr* TrieBranch::step(Trace& trace, F&& move) const {
  Transition& back = trace.stack.back();
  char i = idx(back.branch_key);
  uint8_t bit_ = bit(back.branch_key);
  bool exists = bits[i] & (1 << bit_);
  uint8_t old = back.branch_key;

  move(bit_, i, back.branch_key);

  if (back.branch_key != old) {
    trace.changed_branch_key();
    return &links[index(back.branch_key)];
  }
  if (exists) 
    trace.current_key.pop_back();
  return nullptr;
}

INLINE const offset_ptr* TrieBranch::next(Trace& trace) const {
  return step(trace, [this](uint8_t bit_, char i, uint8_t& key) {
    uint64_t mask = ~((1ul << (bit_ + 1)) - 1);
    uint64_t v = bits[i] & mask;
    if (!v || bit_ == 63) {
      for (i++; i < 4; i++) {
        if (bits[i]) {
          key = i * 64 + ctz(bits[i]);
          break;
        }
      }
    } else
      key = i * 64 + ctz(v);
  });
}

INLINE const offset_ptr* TrieBranch::prev(Trace& trace) const {
  return step(trace, [this](uint8_t bit_, char i, uint8_t& key) {
    uint64_t mask = (1ul << bit_) - 1;
    uint64_t v = bits[i] & mask;
    if (!v) {
      for (i--; i >= 0; i--) {
        if (bits[i]) {
          key = i * 64 + (63 - clz(bits[i]));
          break;
        }
      }
    } else
      key = i * 64 + 63 - clz(v);
  });
}

INLINE void Leaf::find(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.found_leaf = this;
  size_t same = get_prefix(trace.rest_key.data(), (const char*)key_value,
                           trace.rest_key.size(), key_size, back.cmp);
  back.suffix = same;
  trace.advance_key(back.suffix);
}

INLINE void Leaf::step(Trace& trace) const {
  Transition& back = trace.stack.back();
  back.found_leaf = this;
  trace.current_key.append((char*)key_value, key_size);
  back.suffix = key_size;
  back.cmp = 0;
}

INLINE bool BranchBlock::find(Trace& trace) const {
  Transition& back = trace.stack.back();
  uint16_t ioffset = 0;

  if (has_compressed()) {
    back.compressed = (const Compressed*)&data[ioffset];
    if (!back.compressed->find(trace)) return false;
    ioffset += back.compressed->nodesize();
  }

  auto follow_link = [&trace, &back](const offset_ptr* ptr) -> bool {
    if (!ptr) return false;
    return back.follow_link(trace, ptr, find_);
  };

  if (has_null_leaf()) {
    if (trace.rest_key.empty())
      return follow_link((const offset_ptr*)&data[ioffset]);
    ioffset += sizeof(offset_ptr);
  }

  if (trace.rest_key.empty()) {
    back.cmp = -1;
    return false;
  }

  assert(ioffset % ALIGN == 0);

  if (has_array()) {
    back.array = (const ArrayBranch*)&data[ioffset];
    return follow_link(back.array->find(trace));
  }

  if (has_trie()) {
    back.trie = (const TrieBranch*)&data[ioffset];
    return follow_link(back.trie->find(trace));
  }

  // special case there is only one item in the trie: a leaf
  assert(back.branch->used = sizeof(offset_ptr));
  assert(!back.branch->has_compressed());
  assert(!back.branch->has_array());
  assert(!back.branch->has_trie());
  assert(back.branch->has_null_leaf());
  assert(trace.stack.size == 1);
  return follow_link((offset_ptr*)back.branch->data);
}

INLINE bool BranchBlock::first(Trace& trace) const {
  Transition& back = trace.stack.back();

  uint16_t ioffset = 0;
  if (has_compressed()) {
    back.compressed = ((const Compressed*)&data[ioffset]);
    ioffset += back.compressed->step(trace);
  }

  if (has_null_leaf())
    return back.follow_link(trace, (const offset_ptr*)&data[ioffset], step_);

  if (has_array()) {
    back.array = (const ArrayBranch*)&data[ioffset];
    return back.follow_link(trace, back.array->first(trace), step_);
  }

  assert(has_trie());
  back.trie = (const TrieBranch*)&data[ioffset];
  return back.follow_link(trace, back.trie->first(trace), step_);
}

INLINE bool BranchBlock::last(Trace& trace) const {
  Transition& back = trace.stack.back();

  uint16_t ioffset = 0;
  if (has_compressed()) {
    back.compressed = ((const Compressed*)&data[ioffset]);
    ioffset += back.compressed->step(trace);
  }

  if (has_null_leaf()) ioffset += sizeof(offset_ptr);

  if (has_array()) {
    back.array = (const ArrayBranch*)&data[ioffset];
    return back.follow_link(trace, back.array->last(trace), step_);
  }

  if (has_trie()) {
    back.trie = (const TrieBranch*)&data[ioffset];
    return back.follow_link(trace, back.trie->last(trace), step_);
  }

  return back.follow_link(trace, (offset_ptr*)back.branch->data, step_);
}

INLINE bool BranchBlock::next(Trace& trace) const {
  Transition& back = trace.stack.back();
  uint16_t ioffset = 0;

  auto follow_link = [&trace, &back](const offset_ptr* ptr) -> bool {
    if (!ptr) {
      trace.pop();
      return true;
    }
    return back.follow_link(trace, ptr, step_);
  };

  if (back.found_leaf) {
    if (back.cmp < 0) {
      trace.current_key.resize(trace.current_key.size() - back.suffix);
      back.found_leaf->step(trace);
      return false;
    }
    if (back.cmp == 0) {
      trace.current_key.resize(trace.current_key.size() - back.suffix);
      back.found_leaf = nullptr;
    }
  }

  if (back.compressed) {
    ioffset += back.compressed->nodesize();
    if (back.prefix != back.compressed->size) {
      assert(back.prefix < back.compressed->size);
      if (back.cmp > 0) {
        // behind this node
        trace.pop();
        return true;
      }

      assert(back.cmp < 0);
      trace.current_key.resize(back.keypos);
      return first(trace);
    }
  }

  if (has_null_leaf())
    ioffset += sizeof(offset_ptr);
  
  if (has_array()) {
    if (!back.array || back.cmp == Transition::UNDEFINED) {
      back.array = (const ArrayBranch*)&data[ioffset];
      return follow_link(back.array->first(trace));  
    }
    return follow_link(back.array->next(trace));
  }

  if (has_trie()) {
    if (!back.array || back.cmp == Transition::UNDEFINED) {
      back.trie = (const TrieBranch*)&data[ioffset];
      return follow_link(back.trie->first(trace));  
    }
    return follow_link(back.trie->next(trace));
  }

  // only one item
  if (back.cmp < 0) {
    return back.follow_link(trace, (offset_ptr*)back.branch->data, step_);
  }

  trace.pop();
  return false;
}

INLINE bool BranchBlock::prev(Trace& trace) const {
  Transition& back = trace.stack.back();
  uint16_t ioffset = 0;

  if (back.found_leaf) {
    if (back.cmp > 0) {
      trace.current_key.resize(trace.current_key.size() - back.suffix);
      back.found_leaf->step(trace);
      return false;
    }
    if (back.cmp == 0) {
      trace.current_key.resize(trace.current_key.size() - back.suffix);
      back.found_leaf = nullptr;
    }
  }

  if (back.compressed) {
    ioffset += back.compressed->nodesize();
    if (back.prefix != back.compressed->size) {
      if (back.cmp < 0) {
        trace.pop();
        return true;
      }

      assert(back.cmp > 0);
      trace.current_key.resize(back.keypos);
      return last(trace);
    }
  }

  if (!back.array) {
    trace.pop();
    return true;
  }

  bsize_t null_offset = 0;
  if (has_null_leaf()) {
    null_offset = ioffset;
    ioffset += sizeof(offset_ptr);
  }

  auto follow_link = [&trace, &back](const offset_ptr* ptr) -> bool {
    return back.follow_link(trace, ptr, step_);
  };

  const offset_ptr* link = nullptr;
  bool in_branch = false;
  if (has_array()) {
    back.array = (const ArrayBranch*)&data[ioffset];
    link = back.array->prev(trace);
    in_branch = true;
  }

  if (has_trie()) {
    back.trie = (const TrieBranch*)&data[ioffset];
    link = back.trie->prev(trace);
    in_branch = true;
  }

  if (link) 
    return follow_link(link);

  if (in_branch && has_null_leaf()) {
    back.array = nullptr;  // the next time walk to the next node
    return follow_link((const offset_ptr*)&data[null_offset]);
  }
  trace.pop();
  return true;
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

void dump_leaf(std::ostream& out, offset_ptr offset, block_ptr branch,
               DBMemory* storage) {
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
  int delta = align(leaf->key_size);
  for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)10);
       i < end; i++) {
    out << "[" << bitstr(leaf->key_value[i + delta]) << "]";
  }
  out << "\"" << std::endl;
  out << "---" << std::endl;
}

void dump_branch(std::ostream& out, offset_ptr offset, DBMemory* storage);

void dump_link(std::ostream& out, block_ptr parent, offset_ptr offset,
               DBMemory* storage) {
  if (offset.pool_id() == LEAF_BLOCK) {
    dump_leaf(out, offset, parent, storage);
    return;
  }
  assert(offset.pool_id() < POOL_COUNT);
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
    assert(n->size > 0);
    assert(n->size <= ArrayBranch::COUNT);
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
    if (n->bits[TrieBranch::idx(i)] & (((uint64_t)1) << TrieBranch::bit(i))) {
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