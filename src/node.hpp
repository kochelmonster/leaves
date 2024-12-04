#ifndef _LEAVES_NODE_HPP
#define _LEAVES_NODE_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <leaves.hpp>

#include "memory.hpp"

#ifdef TESTING
#include <sstream>
#endif

namespace leaves {

struct Trace;
struct Transition;
struct TrieBlock;
struct DBMemory;

inline char sign(int x) { return (x > 0) - (x < 0); }

struct Trace;

#pragma pack(1)

struct NodeBase {
  int min_space() const;
};

struct ValueNode : public NodeBase {
  static const NodeType ntype = kValue;
  static const size_t SMALL_SIZE =
      1015;  // 256*1024 - sizeof(BlockMeta) - sizeof
  int min_space() const { return child.is_valid() ? MIN_NODE_SIZE : 0; }

  struct {
    uint16_t bigval : 1;
    uint16_t index : 15;  // index in ValueBlock (1 based, index==0 means no
                          // value assigned)
  };

  /*
  in find index = 1 means to go on ==> 
  child is on same position as trie->children[1]
  */
  node_ptr child;

  static ValueNode& cast(Node* n);
  int reduce_space(int space) const { return space - sizeof(ValueNode); }
  void move(ValueNode& dest, TrieBlock& block, int space_left,
            DBMemory& storage);
  void set_value(Trace& trace, const Slice& value);
  const Slice& get_value(const Trace& trace) const;
};

struct StringNode : public NodeBase {
  static const NodeType ntype = kString;
  node_ptr child;
  uint8_t size;  // assert(size >= 2) !
  static const uint8_t MAX_SIZE = NODE_SIZE - sizeof(child) - sizeof(size);
  char key[MAX_SIZE];

  static StringNode& cast(Node* n);
  int find(const Slice& rest_key) const {
    size_t size_ = std::min((size_t)size, rest_key.size());
    int index = sign(memcmp(key, rest_key.data(), size_));
    return !index ? std::min(sign(rest_key.size() - size), (char)0) : index;
  }

  void add_key(Trace& trace);
  void insert(Trace& trace);
  void create_split_part(Trace& trace, ssize_t index, node_ptr& ptr);
  int reduce_space(int space) const { return space - sizeof(StringNode); }
  void move(StringNode& dest, TrieBlock& block, int space_left,
            DBMemory& storage);
};

struct TrieNode : public NodeBase {
  static const int CHILDREN = 16;
  node_ptr children[CHILDREN];

  int next(int bit) const {
    for (; bit < CHILDREN; bit++) {
      if (children[bit].is_valid()) break;
    }
    return bit;
  }

  void move(TrieNode& dest, TrieBlock& block, int space_left,
            DBMemory& storage);
};

struct UpperTrieNode : public TrieNode {
  static const NodeType ntype = kUpperTrie;
  static UpperTrieNode& cast(Node* n);
  static int calc_index(char key) { return (key >> 4) & 0xF; }
  void insert(Trace& trace);
  int reduce_space(int space) const;
};

inline int remove_last_bit(int number) { return (number >> 1) << 1; }

struct LowerTrieNode : public TrieNode {
  static const NodeType ntype = kLowerTrie;
  static LowerTrieNode& cast(Node* n);
  static int calc_index(char key) { return key & 0xF; }

  void insert(Trace& trace);
  int reduce_space(int space) const { return remove_last_bit(space); }
  int min_space() const {
    for (ssize_t i = 0; i < CHILDREN; i++) {
      switch (children->type) {
        case kArray:
        case kString:
        case kUpperTrie:
          return NODE_SIZE + MIN_NODE_SIZE;
      }
    }
    return MIN_NODE_SIZE;
  }
};

struct ArrayNode : public NodeBase {
  static const NodeType ntype = kArray;
  static const int MAX_SIZE = 10;
  node_ptr children[MAX_SIZE];
  char keys[MAX_SIZE];
  ssize_t size;

  static ArrayNode& cast(Node* n);
  UpperTrieNode& make_parent(Trace& trace, char key);
  void insert(Trace& trace);

  int min_space() const {
    for (ssize_t i = 0; i < size; i++) {
      switch (children->type) {
        case kArray:
        case kString:
        case kUpperTrie:
          return NODE_SIZE + MIN_NODE_SIZE;
      }
    }
    return MIN_NODE_SIZE;
  }

  int reduce_space(int space) const {
    if (space & 1 == 0) {
      // Parent is no UpperTrieNode
      space = (space - sizeof(ArrayNode)) / size;
    }
    return remove_last_bit(space);
  }

  void add_key(Trace& trace);
  void move(ArrayNode& dest, TrieBlock& block, int space_left,
            DBMemory& storage);
};

struct LinkNode : public NodeBase {
  static const NodeType ntype = kLink;
  int min_space() const { return 0; }
  int reduce_space(int space) const { return space; }
  offset_ptr link;

  static LinkNode& cast(Node* n);
  void move(LinkNode& dest, TrieBlock& block, int space_left,
            DBMemory& storage);
};

union Node {
  LinkNode link;
  TrieNode trie;
  UpperTrieNode utrie;
  LowerTrieNode ltrie;
  ArrayNode atrie;
  StringNode string;
  ValueNode value;
};

inline int NodeBase::min_space() const { return sizeof(LinkNode); }

inline UpperTrieNode& UpperTrieNode::cast(Node* n) { return n->utrie; }

inline LowerTrieNode& LowerTrieNode::cast(Node* n) { return n->ltrie; }

inline ArrayNode& ArrayNode::cast(Node* n) { return n->atrie; };

inline StringNode& StringNode::cast(Node* n) { return n->string; }

inline ValueNode& ValueNode::cast(Node* n) { return n->value; }

inline LinkNode& LinkNode::cast(Node* n) { return n->link; }

#pragma pack(0)

struct Trace;
struct Transition;

}  // namespace leaves

#endif  // _LEAVES_NODE_HPP