#ifndef _LARCH_LEAVES_BITTRIE_HPP
#define _LARCH_LEAVES_BITTRIE_HPP
#include "node.hpp"
#include "port.hpp"

namespace larch_leaves {

typedef unsigned char trieindex_t;

// Node with a range
struct BitTrie {
  boost::uint64_t bits;

  static inline BitTrie *cast(Node *node) {
    return (BitTrie *)((char *)node + node->pad_size() - sizeof(BitTrie));
  }

  static inline const BitTrie *cast(const Node *node) {
    return (BitTrie *)((char *)node + node->pad_size() - sizeof(BitTrie));
  }

  size_t count() const { return popcount(bits); }

  int get_child_index(trieindex_t index) const {
    if (bits & (((boost::uint64_t)1) << index)) {
      boost::uint64_t mask = ~0;
      mask <<= index;
      return (int)popcount(bits & ~mask) + 1; // 0 is end node
    }
    return 0;
  }

  int first_bit() const { return ffs(bits) - 1; }

  int last_bit() const {
    if (!bits)
      return -1;
    return 63 - clz(bits);
  }

  int next_bit(int index) const {
    boost::uint64_t mask = ~0;
    mask <<= (index + 1);
    return ffs(bits & mask) - 1;
  }

  int prev_bit(int index) const {
    boost::uint64_t mask = ~0;
    mask <<= index;
    mask = bits & ~mask;
    if (!mask)
      return -1;
    return 63 - clz(mask);
  }

  // index is zero based
  void add(int index, Page::ptr node, Page::ptr *children) {
    bits |= ((boost::uint64_t)1) << index;
    int child_index = get_child_index(index);
    memmove(children + child_index + 1, children + child_index,
            sizeof(Page::ptr) * (count() - child_index));
    children[child_index] = node;
  }

  // index is zero based
  void remove(int index, Page::ptr *children) {
    int child_index = get_child_index(index);
    assert(child_index >= 0);
    bits &= ~(((boost::uint64_t)1) << index);
    memmove(children + child_index, children + child_index + 1,
            sizeof(Page::ptr) * (count() - child_index + 1));
  }
};

} // namespace larch_leaves
#endif // _LARCH_LEAVES_BITTRIE_HPP
