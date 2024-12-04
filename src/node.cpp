#include "node.hpp"

#include "trace.hpp"

#ifdef DEBUG
#include <sstream>
#endif

namespace leaves {

INLINE int UpperTrieNode::reduce_space(int space) const {
  int count = 0;
  bool has_array = false;
  for (int i = 0; i < CHILDREN; i++) {
    switch (children[i].type) {
      case kArray:
        if (has_array) {
          bool is_double = false;
          Node* n = children[i].resolve();
          for (int j = 0; j < i; j++) {
            if (children[j].type == kArray) {
              if (n == children[j].resolve()) {
                is_double = true;
                break;
              }
            }
          }
          if (is_double) break;
        }
        space -= sizeof(ArrayNode);
        has_array = true;
        count += children[i]->atrie.size;
        break;

      case kLowerTrie: {
        LowerTrieNode& ltrie = children[i]->ltrie;
        space -= sizeof(LowerTrieNode);
        for (int i = 0; i < CHILDREN; i++) {
          count += ltrie.children[i].is_valid();
        }
      }
    }
  }

  // | 1 marks space for next array
  return ((space - sizeof(TrieNode)) / count) | 1;
}

/*
Insert Methods

Prepares the trie structure to include the new key.
*/

INLINE void UpperTrieNode::insert(Trace& trace) {
  Transition& back = trace.stack.back();
  node_ptr* aptr = &children[back.index];

  assert(!children[back.index].is_valid());

  for (int i = 0; i < CHILDREN; i++) {
    node_ptr& ptr = children[i];
    if (ptr.type == kArray && ptr->atrie.size < ArrayNode::MAX_SIZE) {
      // This array node has still space
      children[back.index] = ptr;
      break;
    }
  }
  if (!aptr->is_valid()) {
    // Add a new array node to put the new key to
    aptr->set(back.block.trie()->alloc(), kArray);
  }

  // fill the array node and add to stack
  Node* anode = aptr->resolve();
  anode->atrie.add_key(trace);
  trace.stack.push(back.block, aptr, back.keypos, anode->atrie.size - 1);
}

INLINE void LowerTrieNode::insert(Trace& trace) {
  Transition& back = trace.stack.back();
  node_ptr* aptr = &children[back.index];
  assert(!children[back.index].is_valid());
  children[back.index].type = trace.rest_key.size() ? kString : kValue;
  trace.current_key.push_back(trace.rest_key[0]);
  trace.rest_key.iadvance(1);
}

INLINE void ArrayNode::add_key(Trace& trace) {
  keys[size] = trace.rest_key[0];
  trace.current_key.push_back(trace.rest_key[0]);
  trace.rest_key.iadvance(1);
  children[size].type = trace.rest_key.size() ? kString : kValue;
  children[size].offset = 0;
  size++;
}

INLINE UpperTrieNode& ArrayNode::make_parent(Trace& trace, char key) {
  // Ensure we have UpperTrieNode as parent
  if (trace.stack.size > 1) {
    Transition& parent = trace.stack.data[trace.stack.size - 2];
    if (parent.pnode->type == kUpperTrie) {
      return parent.pnode->resolve()->utrie;
    }
  }

  Transition& back = trace.stack.back();

  Node* node = back.block.trie()->alloc();
  back.pnode->set(node, kUpperTrie);
  UpperTrieNode& utrie = node->utrie;

  for (int i = 0; i < size; i++) {
    utrie.children[utrie.calc_index(keys[i])].set(this, kArray);
  }
  return utrie;
}

INLINE void ArrayNode::insert(Trace& trace) {
  Transition& back = trace.stack.back();

  assert(back.index == size);

  if (size < MAX_SIZE) {
    // there is still some space
    add_key(trace);

    if (size == MAX_SIZE) {
      // we are on the edge to a transform to a LowerTrieNode

      int uindex = UpperTrieNode::calc_index(trace.current_key.back());
      NodeType types[MAX_SIZE];
      Node* nodes[MAX_SIZE];
      char keys_[MAX_SIZE];

      for (int i = 0; i < size; i++) {
        if (UpperTrieNode::calc_index(keys[i]) != uindex) return;
        types[i] = (NodeType)children[i].type;
        nodes[i] = children[i].resolve();
        keys_[i] = keys[i];
      }

      UpperTrieNode& utrie = make_parent(trace, keys_[0]);
      utrie.children[uindex].set(this, kLowerTrie);
      back.index = uindex;

      // All keys have the same upper bits -> transform to LowerTrie
      LowerTrieNode& ltrie = *(LowerTrieNode*)this;
      memset(&ltrie, 0, sizeof(ltrie));
      int i;
      for (i = 0; i < MAX_SIZE - 1; i++) {
        ltrie.children[ltrie.calc_index(keys_[i])].set(nodes[i], types[i]);
      }
      // the last one ist the inserted node and it has no child yet.
      int lindex = ltrie.calc_index(keys_[i]);
      ltrie.children[lindex].type = types[i];

      // push the lower trie to the stack
      trace.stack.push(back.block, &utrie.children[uindex], back.keypos,
                       lindex);
    }
    return;
  }

  // Overflow

  // Split the array
  ArrayNode& ntrie = back.block.trie()->alloc()->atrie;
  int uindex = UpperTrieNode::calc_index(trace.rest_key[0]);
  for (int i = size - 1; i >= 0; i--) {
    if (UpperTrieNode::calc_index(keys[i]) == uindex) {
      ntrie.keys[ntrie.size] = keys[i];
      ntrie.children[ntrie.size] = children[i];
      ntrie.size++;
      size--;
      memmove(&keys[i], &keys[i + 1], size - i);
      for (int j = i; j < size; j++) {
        children[j] = children[j + 1];
      }
    }
  }
  assert(size > 0);  // No LowerTrieNode transformation

  UpperTrieNode& utrie = make_parent(trace, trace.rest_key[0]);
  ntrie.add_key(trace);
  utrie.children[uindex].set(&ntrie, kArray);

  // push the new array to the stack
  trace.stack.push(back.block, &utrie.children[uindex], back.keypos,
                   ntrie.size - 1);
}

INLINE void StringNode::add_key(Trace& trace) {
  size = std::min((size_t)MAX_SIZE, trace.rest_key.size());
  memcpy(key, trace.rest_key.data(), size);
  trace.current_key.append(trace.rest_key.data(), size);
  trace.rest_key.iadvance(size);
}

INLINE void StringNode::create_split_part(Trace& trace, ssize_t index,
                                          node_ptr& ptr) {
  // create the second half of string split

  if (index == size - 1) {
    ptr = child;
    return;
  }

  Node* new_node = trace.stack.back().block.trie()->alloc();

  if (index == size - 2) {
    // just one letter left -> Array Node
    ArrayNode& atrie = new_node->atrie;
    atrie.keys[0] = key[index];
    atrie.children[0] = child;
    atrie.size = 1;
    ptr.set(new_node, kArray);
    return;
  }
  StringNode& string = new_node->string;
  string.child = child;
  string.size = size - index - 1;
  memcpy(string.key, &key[index + 1], string.size);
  ptr.set(new_node, kString);
}

INLINE void StringNode::insert(Trace& trace) {
  size_t size_ = std::min((size_t)size, trace.rest_key.size());
  size_t index;
  for (index = 0; index < size_; index++) {
    if (key[index] != trace.rest_key[index]) break;
  }
  assert(index < size);

  trace.current_key.append(trace.rest_key.data(), index);
  trace.rest_key.iadvance(index);

  Transition& back = trace.stack.back();
  back.index = 0;

  if (trace.rest_key.empty()) {
    // Split the string with a value node between.
    ValueNode& value = back.block.trie()->alloc(sizeof(ValueNode))->value;
    create_split_part(trace, index - 1, value.child);
    child.set(&value, kValue);
    size = index;
    return;
  }

  if (index == 0) {
    // Transform this to an array node
    back.pnode->type = kArray;
    ArrayNode& atrie = (*back.pnode)->atrie;
    char key_ = key[0];
    create_split_part(trace, index, atrie.children[0]);
    atrie.keys[0] = key_;
    atrie.size = 1;
    atrie.add_key(trace);
    back.index = 1;
    return;
  }

  // Split the string with an array node between
  ArrayNode& atrie = back.block.trie()->alloc()->atrie;
  atrie.keys[0] = key[index];
  create_split_part(trace, index, atrie.children[0]);
  atrie.size = 1;
  child.set(&atrie, kArray);
  size = index;
  trace.stack.push(back.block, &child, trace.current_key.size(), 1);
  atrie.add_key(trace);
}

#ifdef DEBUG

size_t dump_node(std::ostream& out, const TrieBlock* block,
                 const node_ptr& pnode, DBMemory* storage, int upper = 0);

const char* handler_names[] = {"kNull",  "kUpperTrie", "kLowerTrie", "kArray",
                               "kValue", "kLink",      "kString"};

INLINE std::string idstr(const TrieBlock* block, const node_ptr& ptr) {
  std::stringstream cstr;
  int delta = (char*)ptr.resolve() - (const char*)block->data;
  cstr << handler_names[ptr.type] << "-" << delta << "-" << block->offset;
  return cstr.str();
}

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

INLINE void dump_id_space(std::ostream& out, const TrieBlock* block,
                          const node_ptr& pnode) {
  out << "id: " << idstr(block, pnode) << std::endl;
  out << "pspace: " << TrieBlock::DATA_SIZE - block->used << std::endl;
}

INLINE size_t dump_null(std::ostream& out, const TrieBlock* block,
                        const node_ptr& pnode, DBMemory* storage,
                        int upper = 0) {
  dump_id_space(out, block, pnode);
  out << "---" << std::endl;
  return 0;
}

INLINE size_t dump_link(std::ostream& out, const TrieBlock* block,
                        const node_ptr& pnode, DBMemory* storage,
                        int upper = 0) {
  dump_id_space(out, block, pnode);

  offset_ptr link = pnode->link.link;
  out << "link: " << link << std::endl;

  TrieBlock* next_block = storage->get_block(link).trie();
  if (storage->transaction_active()) {
    next_block = storage->get_block(link).trie();
  }

  node_ptr& root = next_block->root;
  out << "children: " << std::endl;
  out << "  - " << idstr(next_block, root) << std::endl;
  out << "---" << std::endl;
  return dump_node(out, next_block, root, storage, -1);
}

INLINE size_t dump_utrie(std::ostream& out, const TrieBlock* block,
                         const node_ptr& pnode, DBMemory* storage,
                         int upper = 0) {
  const UpperTrieNode& trie = pnode->utrie;
  dump_id_space(out, block, pnode);

  bool first = true;
  out << "bytes: [";
  for (int i = 0; i < TrieNode::CHILDREN; i++) {
    if (trie.children[i].offset) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << '"';
      out << bitstr((char)i);
      out << '"';
    }
  }
  out << "]" << std::endl;

  out << "children: " << std::endl;
  for (int i = 0; i < TrieNode::CHILDREN; i++) {
    if (trie.children[i].offset) {
      out << "  - " << idstr(block, trie.children[i]) << std::endl;
    }
  }
  out << "---" << std::endl;
  size_t value_count = 0;
  for (int i = 0; i < TrieNode::CHILDREN; i++) {
    bool dump_down = true;
    if (trie.children[i].is_valid()) {
      for (int j = 0; j < i; j++) {
        if (trie.children[j].is_valid() &&
            trie.children[j].resolve() == trie.children[i].resolve()) {
          dump_down = false;
        }
      }

      if (dump_down)
        value_count += dump_node(out, block, trie.children[i], storage, i << 4);
    }
  }
  return value_count;
}

INLINE size_t dump_ltrie(std::ostream& out, const TrieBlock* block,
                         const node_ptr& pnode, DBMemory* storage,
                         int upper = 0) {
  const LowerTrieNode& trie = pnode->ltrie;
  dump_id_space(out, block, pnode);

  bool first = true;
  out << "bytes: [";
  for (int i = 0; i < TrieNode::CHILDREN; i++) {
    if (trie.children[i].offset) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << '"';
      out << bitstr((char)(upper | i));
      out << '"';
    }
  }
  out << "]" << std::endl;

  out << "children: " << std::endl;
  for (int i = 0; i < TrieNode::CHILDREN; i++) {
    if (trie.children[i].offset) {
      out << "  - " << idstr(block, trie.children[i]) << std::endl;
    }
  }
  out << "---" << std::endl;
  size_t value_count = 0;
  for (int i = 0; i < TrieNode::CHILDREN; i++) {
    if (trie.children[i].offset) {
      value_count += dump_node(out, block, trie.children[i], storage, 0);
    }
  }
  return value_count;
}

INLINE size_t dump_atrie(std::ostream& out, const TrieBlock* block,
                         const node_ptr& pnode, DBMemory* storage,
                         int upper = 0) {
  const ArrayNode& atrie = pnode->atrie;
  dump_id_space(out, block, pnode);

  bool first = true;
  out << "bytes: [";
  for (int i = 0; i < atrie.size; i++) {
    if (!first) {
      out << ", ";
    }
    first = false;
    out << '"';
    out << bitstr(atrie.keys[i]);
    out << '"';
  }
  out << "]" << std::endl;

  out << "children: " << std::endl;
  for (int i = 0; i < atrie.size; i++) {
    out << "  - " << idstr(block, atrie.children[i]) << std::endl;
  }
  out << "---" << std::endl;
  size_t value_count = 0;
  for (int i = 0; i < atrie.size; i++) {
    value_count += dump_node(out, block, atrie.children[i], storage);
  }
  return value_count;
}

INLINE size_t dump_value(std::ostream& out, const TrieBlock* block,
                         const node_ptr& pnode, DBMemory* storage,
                         int upper = 0) {
  const ValueNode& node = pnode->value;
  dump_id_space(out, block, pnode);

  block_ptr vblock = storage->get_block(block->value);
  Slice val = vblock.val()->get_value(node.index);
  size_t size;
  const char* p;

  if (node.bigval) {
    assert(val.size() == sizeof(offset_ptr));
    block_ptr bvblock = storage->get_block(*(offset_ptr*)val.data());
    size = bvblock.bval()->data_size;
    p = bvblock.bval()->data;
  } else {
    size = val.size();
    p = val.data();
  }

  out << "size: " << (int)size << std::endl;
  out << "value: \"";

  for (size_t i = 0; i < std::min(size, (size_t)10); i++) {
    out << "[" << bitstr(p[i]) << "]";
  }
  out << "\"" << std::endl;
  if (node.child.is_valid()) {
    out << "children: " << std::endl;
    out << "  - " << idstr(block, node.child) << std::endl;
    out << "---" << std::endl;
    return dump_node(out, block, node.child, storage, -1) + 1;
  }

  out << "children: []" << std::endl;
  out << "---" << std::endl;
  return 1;
}

INLINE size_t dump_string(std::ostream& out, const TrieBlock* block,
                          const node_ptr& pnode, DBMemory* storage, int upper) {
  const StringNode& node = pnode->string;
  dump_id_space(out, block, pnode);
  out << "size: " << (int)node.size << std::endl;
  out << "keys: \"";
  for (int i = 0; i < node.size; i++) {
    out << "[" << bitstr(node.key[i]) << "]";
  }
  out << "\"" << std::endl;
  out << "children: " << std::endl;
  out << "  - " << idstr(block, node.child) << std::endl;
  out << "---" << std::endl;
  return dump_node(out, block, node.child, storage);
}

INLINE size_t dump_node(std::ostream& out, const TrieBlock* block,
                        const node_ptr& pnode, DBMemory* storage, int upper) {
  switch (pnode.type) {
    case kNull:
      return dump_null(out, block, pnode, storage, upper);

    case kUpperTrie:
      return dump_utrie(out, block, pnode, storage, upper);

    case kLowerTrie:
      return dump_ltrie(out, block, pnode, storage, upper);

    case kArray:
      return dump_atrie(out, block, pnode, storage, upper);

    case kValue:
      return dump_value(out, block, pnode, storage, upper);

    case kLink:
      return dump_link(out, block, pnode, storage, upper);

    case kString:
      return dump_string(out, block, pnode, storage, upper);
  };
  return 0;
}

#endif

}  // namespace leaves
