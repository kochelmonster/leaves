/*
Trie node layout and node-level metadata for the internal key/value tree.
*/
#ifndef _LEAVES__NODE_HPP
#define _LEAVES__NODE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

// dummy structure for all node pointers
struct _Node {};

template <typename Traits>
struct _TrieNodeHeaderNoHash;

template <typename Traits, size_t HashSize>
struct _TrieNodeHeaderHash;

template <typename Traits>
struct _LeafNodeHeaderNoHash;

template <typename Traits, size_t HashSize>
struct _LeafNodeHeaderHash;

/*
A compressed Trie node (https://www.geeksforgeeks.org/compressed-tries/)
Every node has at least one char in the compressed data (the branch_key of the
parent node) This makes the implmentation of many operations easier.
*/
#pragma pack(push, 1)
template <typename Traits>
struct _TrieNodeHeaderBase {
  using traits_t = Traits;
  using uint16_e = typename Traits::uint16_e;

  uint16_e _branch_count;
  uint8_t _branch_bits_index;
  uint8_t _prefix_len;
  uint8_t _branch_bits_pos;
  uint8_t _branch_offsets_pos;
};

template <typename Traits>
struct _TrieNodeHeaderNoHash : _TrieNodeHeaderBase<Traits> {
  static constexpr size_t HASH_SIZE = 0;

  uint8_t _prefix[1];
};

template <typename Traits, size_t HashSize>
struct _TrieNodeHeaderHash : _TrieNodeHeaderBase<Traits> {
  static_assert(HashSize > 0, "HashSize must be greater than zero");

  static constexpr size_t HASH_SIZE = HashSize;

  uint8_t _hash[HashSize];
  uint8_t _prefix[1];
};

template <typename Header_>
struct _TrieNode : Header_ {
  typedef _TrieNode<Header_> TrieNode;
  using Header = Header_;
  using traits_t = typename Header::traits_t;
  using uint32_e = typename traits_t::uint32_e;
  using uint16_e = typename traits_t::uint16_e;
  using offset_e = typename traits_t::offset_e;
  static constexpr size_t HASH_SIZE = Header::HASH_SIZE;
  static constexpr bool HAS_HASH = HASH_SIZE > 0;
  using Header::_branch_count;
  using Header::_branch_bits_index;
  using Header::_prefix_len;
  using Header::_branch_bits_pos;
  using Header::_branch_offsets_pos;
  using Header::_prefix;
  static constexpr uint16_t MAX_BRANCH_COUNT = 257;  // 256 (chars) + 1 (NULL)

  static constexpr uint16_t NULL_MASK = uint16_t(1) << 15;
  constexpr static int NONE = -1;
  constexpr static int OUT_OF_RANGE = -2;
  static constexpr uint16_t HEADER_SIZE = sizeof(Header);
  constexpr static uint16_t MAX_SIZE =
      align(padding(HEADER_SIZE + 255, sizeof(uint32_e)) +
            8 * sizeof(uint32_e)) +
      MAX_BRANCH_COUNT * sizeof(offset_e);
  constexpr static uint8_t BRANCH_BIT_MASK = 0b00011111;

  uint8_t prefix_len() const { return _prefix_len; }
  int branch_count() const { return (_branch_count & ~NULL_MASK); }
  const uint8_t* prefix() const { return _prefix; }
  uint16_t branch_bits_size() const { return bits::count(_branch_bits_index) * sizeof(uint32_e); }
  uint16_t branch_bits_start() const { return _branch_bits_pos * sizeof(uint32_e); }
  uint16_t branch_bits_end() const { return branch_bits_start() + branch_bits_size(); }
  uint32_e* branch_bits() const {
    return (uint32_e*)((uint8_t*)this + branch_bits_start());
  }
  uint16_t branch_offsets_start() const { return _branch_offsets_pos * sizeof(offset_e); }
  uint16_t branch_offsets_end() const { return branch_offsets_start() + branch_offsets_size(); }
  uint16_t branch_offsets_size() const { return branch_count() * sizeof(offset_e); }
  offset_e* branch_offsets() const {
    return (offset_e*)((uint8_t*)this + branch_offsets_start());
  }
  static uint8_t branch_bits_group(uint8_t val) { return (val >> 5); }
  static uint32_t branch_bit(uint8_t val) { return (val & BRANCH_BIT_MASK); }
  uint16_t size() const { return branch_offsets_end(); }
  uint16_t changed_len(uint8_t new_prefix_len) const {
    uint16_t prefix_size = padding(HEADER_SIZE + new_prefix_len,
                                   sizeof(uint32_e));
    return align(prefix_size + branch_bits_size() + branch_offsets_size());
  }

  uint16_t increment_size(int key) const {
    uint16_t prefix_size =
        padding(HEADER_SIZE + _prefix_len, sizeof(uint32_e));
    uint16_t branch_bits_size_ = branch_bits_size();
    if (key != NONE && !(_branch_bits_index & (1u << branch_bits_group(key)))) {
      branch_bits_size_ += sizeof(uint32_e);
    }
    uint16_t branch_offsets_size_ = (branch_count() + 1) * sizeof(offset_e);
    return align(prefix_size + branch_bits_size_ + branch_offsets_size_);
  }

  uint16_t decrement_size(int key) const {
    uint16_t prefix_size =
        padding(HEADER_SIZE + _prefix_len, sizeof(uint32_e));
    uint16_t branch_bits_size_ = branch_bits_size();
    if (key != NONE && (_branch_bits_index & (1u << branch_bits_group(key)))) {
      // Only decrement branch_bits_size if this key is the only one in its
      // branch-bits word.
      int lidx = bits::index(_branch_bits_index, branch_bits_group(key));
      if (bits::count(branch_bits()[lidx]) == 1) {
        branch_bits_size_ -= sizeof(uint32_e);
      }
    }
    uint16_t branch_offsets_size_ = (branch_count() - 1) * sizeof(offset_e);
    return align(prefix_size + branch_bits_size_ + branch_offsets_size_);
  }

  uint16_t calc_branch_bits_start() const {
    return padding(HEADER_SIZE + _prefix_len, sizeof(uint32_e));
  }

  uint16_t calc_branch_offsets_start() const {
    return align(branch_bits_start() + bits::count(_branch_bits_index) * sizeof(uint32_e));
  }

  static constexpr uint16_t size(uint8_t prefix, int key1, int key2) {
    assert(key1 != key2);
    uint16_t prefix_size = padding(HEADER_SIZE + prefix, sizeof(uint32_e));
    uint16_t branch_bits_size;
    if (key1 == NONE || key2 == NONE) {
      branch_bits_size = sizeof(uint32_e);  // only key2's branch-bits group
    } else if (branch_bits_group(key1) == branch_bits_group(key2)) {
      branch_bits_size = sizeof(uint32_e);  // same branch-bits group
    } else {
      branch_bits_size = 2 * sizeof(uint32_e);  // different groups
    }
    uint16_t branch_offsets_start = align(prefix_size + branch_bits_size);
    return branch_offsets_start + 2 * sizeof(offset_e);
  }

  // Estimates the max size for a trie node with a given prefix and branches.
  // This is an upper-bound estimate used for allocation - actual size may be smaller
  // due to bitmap compression of sparse branch arrays.
  static constexpr uint16_t size(uint8_t prefix, uint16_t branches) {
    uint16_t prefix_size = padding(HEADER_SIZE + prefix, sizeof(uint32_e));
    uint16_t branch_bits_size = std::min(branches, (uint16_t)8) * sizeof(uint32_e);
    uint16_t branch_offsets_size = branches * sizeof(offset_e);
    return align(prefix_size + branch_bits_size + branch_offsets_size);
  }

  bool has_none() const { return _branch_count & NULL_MASK; }

  // create a trie node with a prefix and two keys; returns the branch offset
  // indexes for key1 and key2
  std::pair<uint16_t, uint16_t> create(Slice prefix, int key1, int key2) {
    assert(key1 >= NONE);
    assert(key2 >= NONE);

    _prefix_len = prefix.size();
    memcpy(_prefix, prefix.data(), _prefix_len);

    std::pair<uint16_t, uint16_t> result(0, 1);
    if (key2 < key1) {
      std::swap(key1, key2);
      std::swap(result.first, result.second);
    }

    _branch_count = 2;
    uint16_t branch_offsets_start_, branch_bits_start_ = calc_branch_bits_start();
    uint32_e* branch_bits_ = (uint32_e*)((char*)this + branch_bits_start_);

    if (key1 != NONE) {
      _branch_bits_index = (1u << branch_bits_group(key1)) | (1u << branch_bits_group(key2));
      if (bits::count(_branch_bits_index) == 1) {
        branch_bits_[0] = (1u << branch_bit(key1)) | (1u << branch_bit(key2));
        branch_offsets_start_ = align(branch_bits_start_ + sizeof(uint32_e));
      } else {
        branch_bits_[0] = 1u << branch_bit(key1);
        branch_bits_[1] = 1u << branch_bit(key2);
        branch_offsets_start_ = align(branch_bits_start_ + 2 * sizeof(uint32_e));
      }
    } else {
      _branch_bits_index = 1u << branch_bits_group(key2);
      _branch_count = _branch_count | NULL_MASK;
      branch_bits_[0] = 1u << branch_bit(key2);
      branch_offsets_start_ = align(branch_bits_start_ + sizeof(uint32_e));
    }

    _branch_offsets_pos = branch_offsets_start_ / sizeof(offset_e);
    _branch_bits_pos = branch_bits_start_ / sizeof(uint32_e);
    return result;
  }

  /**
   * @brief Creates and initializes a TrieNode with the given prefix and key.
   *
   * @param prefix A Slice object representing the prefix to be stored in the
   * node.
  * @param key An integer key used to set the branch bitmaps. If the key is
  * NONE, the node is marked as null.
  * @return branch offset index of the key (0)
   */
  uint16_t create(Slice prefix, int key) {
    _prefix_len = prefix.size();
    memcpy(_prefix, prefix.data(), _prefix_len);

    _branch_count = 1;
    uint16_t branch_offsets_start_,
        branch_bits_start_ = padding(HEADER_SIZE + _prefix_len,
                                     sizeof(uint32_e));
    uint32_e* branch_bits_ = (uint32_e*)((char*)this + branch_bits_start_);

    if (key != NONE) {
      _branch_bits_index = 1u << branch_bits_group(key);
      branch_bits_[0] = 1u << branch_bit(key);
      branch_offsets_start_ = align(branch_bits_start_ + sizeof(uint32_e));
    } else {
      _branch_bits_index = 0;
      branch_offsets_start_ = align(branch_bits_start_);
      _branch_count = _branch_count | NULL_MASK;
    }

    _branch_offsets_pos = branch_offsets_start_ / sizeof(offset_e);
    _branch_bits_pos = branch_bits_start_ / sizeof(uint32_e);
    return 0;
  }

  /**
   * @brief Creates a new TrieNode by merging data from a source node with a new
   * prefix slice.
   *
   * @param src Pointer to the source TrieNode from which data will be copied.
   * @param prefix A Slice object representing the prefix to be compressed and
   * stored.
   *
   */
  template <typename TNode>
  void create(const TNode& src, Slice prefix) {
    _prefix_len = prefix.size();
    memcpy(_prefix, prefix.data(), _prefix_len);
    _branch_count = src._branch_count;
    _branch_bits_index = src._branch_bits_index;

    uint16_t bcount = bits::count(_branch_bits_index);
    _branch_bits_pos = padding(HEADER_SIZE + _prefix_len, sizeof(uint32_e)) /
            sizeof(uint32_e);
    _branch_offsets_pos =
        align(branch_bits_start() + bcount * sizeof(uint32_e)) / sizeof(offset_e);

    memcpy(branch_bits(), src.branch_bits(), bcount * sizeof(uint32_e));
    memcpy((void*)branch_offsets(), src.branch_offsets(), branch_count() * sizeof(offset_e));
  }

  /**
   * @brief Creates a new TrieNode by adding a branch to an existing node.
   *
   * This method copies the structure from a source node and adds a new branch
  * for the specified key. It handles both the case where the key's branch-bits
  * group already exists in the source and the case where a new group needs to
  * be created.
   *
   * @param src The source TrieNode to copy from.
   * @param key The character key for the new branch, or NONE for a null branch.
  * @return The byte offset within this node to the newly created branch-offset slot
   *         for the key's link: (char*)this + offset points to the offset_e
   * slot.
   */
  template <typename TNode>
  uint16_t create(const TNode& src, int key) {
    _prefix_len = src._prefix_len;
    memcpy(_prefix, src.prefix(), _prefix_len);

    uint16_t branch_bits_start_ =
      padding(HEADER_SIZE + _prefix_len, sizeof(uint32_e));
    uint32_e* branch_bits_ = (uint32_e*)((char*)this + branch_bits_start_);

    _branch_count = src._branch_count + 1;
    _branch_bits_index = src._branch_bits_index;
    int oidx;
    if (key != NONE) {
      uint8_t bit = branch_bits_group(key);
      _branch_bits_index |= (1u << bit);
      int lidx = bits::index(_branch_bits_index, bit);
      if (src._branch_bits_index & (1u << bit)) {
        // _branch_bits_index == src._branch_bits_index
        memcpy(branch_bits_, src.branch_bits(), bits::count(_branch_bits_index) * sizeof(uint32_e));
        branch_bits_[lidx] |= 1u << branch_bit(key);
      } else {
        // bits::count(_branch_bits_index) == bits::count(src._branch_bits_index) + 1
        memcpy(branch_bits_, src.branch_bits(), lidx * sizeof(uint32_e));
        memcpy(branch_bits_ + lidx + 1, src.branch_bits() + lidx,
               (bits::count(src._branch_bits_index) - lidx) * sizeof(uint32_e));
        branch_bits_[lidx] = 1u << branch_bit(key);
      }

      oidx = bits::count(branch_bits_[lidx] & ((1u << branch_bit(key)) - 1)) +
             bool(_branch_count & NULL_MASK);
      // Unrolled loop - lidx is at most 7
      if (lidx > 0) oidx += bits::count(branch_bits_[0]);
      if (lidx > 1) oidx += bits::count(branch_bits_[1]);
      if (lidx > 2) oidx += bits::count(branch_bits_[2]);
      if (lidx > 3) oidx += bits::count(branch_bits_[3]);
      if (lidx > 4) oidx += bits::count(branch_bits_[4]);
      if (lidx > 5) oidx += bits::count(branch_bits_[5]);
      if (lidx > 6) oidx += bits::count(branch_bits_[6]);
    } else {
      assert((src._branch_count & NULL_MASK) == 0);
      _branch_count = _branch_count | NULL_MASK;
      memcpy(branch_bits_, src.branch_bits(), bits::count(_branch_bits_index) * sizeof(uint32_e));
      oidx = 0;
    }

    _branch_bits_pos = branch_bits_start_ / sizeof(uint32_e);
    uint16_t branch_offsets_start_ = calc_branch_offsets_start();
    _branch_offsets_pos = branch_offsets_start_ / sizeof(offset_e);

    offset_e* branch_offsets_ = (offset_e*)((char*)this + branch_offsets_start_);
    memcpy((void*)branch_offsets_, src.branch_offsets(), oidx * sizeof(offset_e));
    memcpy((void*)(branch_offsets_ + oidx + 1), src.branch_offsets() + oidx,
           (branch_count() - 1 - oidx) * sizeof(offset_e));
    return oidx;
  }

  // create a new trie node without the branch of key
  void create_remove(const TrieNode& src, int key) {
    assert((src.isset)(key));
    _prefix_len = src._prefix_len;
    memcpy(_prefix, src.prefix(), _prefix_len);

    uint16_t branch_bits_start_ =
      padding(HEADER_SIZE + _prefix_len, sizeof(uint32_e));
    uint32_e* branch_bits_ = (uint32_e*)((char*)this + branch_bits_start_);

    _branch_count = src._branch_count - 1;
    _branch_bits_index = src._branch_bits_index;
    int oidx;
    if (key != NONE) {
      uint8_t bit = branch_bits_group(key);
      int lidx = bits::index(src._branch_bits_index, bit);
      uint32_e src_branch_bits_val = src.branch_bits()[lidx];

      // Calculate oidx using source value
      oidx = bits::count(src_branch_bits_val & ((1u << branch_bit(key)) - 1)) +
             bool(_branch_count & NULL_MASK);
      // Unrolled loop - lidx is at most 7
      uint32_e* src_branch_bits = src.branch_bits();
      if (lidx > 0) oidx += bits::count(src_branch_bits[0]);
      if (lidx > 1) oidx += bits::count(src_branch_bits[1]);
      if (lidx > 2) oidx += bits::count(src_branch_bits[2]);
      if (lidx > 3) oidx += bits::count(src_branch_bits[3]);
      if (lidx > 4) oidx += bits::count(src_branch_bits[4]);
      if (lidx > 5) oidx += bits::count(src_branch_bits[5]);
      if (lidx > 6) oidx += bits::count(src_branch_bits[6]);

      // Check if removing this bit will empty the branch-bits word.
      uint32_e new_branch_bits_val = src_branch_bits_val & ~(1u << branch_bit(key));
      if (!new_branch_bits_val) {
        // Remove the branch-bits group and copy around the removed word.
        _branch_bits_index &= ~(1u << bit);
        memcpy(branch_bits_, src.branch_bits(), lidx * sizeof(uint32_e));
        memcpy(branch_bits_ + lidx, src.branch_bits() + lidx + 1,
               (bits::count(src._branch_bits_index) - lidx - 1) * sizeof(uint32_e));
      } else {
        // Keep the branch-bits word with the bit removed.
        memcpy(branch_bits_, src.branch_bits(), bits::count(src._branch_bits_index) * sizeof(uint32_e));
        branch_bits_[lidx] = new_branch_bits_val;
      }
    } else {
      assert(src._branch_count & NULL_MASK);
      _branch_count = _branch_count & uint16_t(~NULL_MASK);
      memcpy(branch_bits_, src.branch_bits(), bits::count(_branch_bits_index) * sizeof(uint32_e));
      oidx = 0;
    }

    _branch_bits_pos = branch_bits_start_ / sizeof(uint32_e);
    uint16_t branch_offsets_start_ = calc_branch_offsets_start();
    _branch_offsets_pos = branch_offsets_start_ / sizeof(offset_e);

    offset_e* branch_offsets_ = (offset_e*)((char*)this + branch_offsets_start_);
    memcpy((void*)branch_offsets_, src.branch_offsets(), oidx * sizeof(offset_e));
    memcpy((void*)(branch_offsets_ + oidx), src.branch_offsets() + oidx + 1,
           (branch_count() - oidx) * sizeof(offset_e));
  }

  /**
   * @brief Insert a new branch for @p key in-place, without allocating a new
   * node.  The caller must guarantee:
   *   1. The page that owns this node belongs to the current write transaction
   *      (needs_cow() == false), so no reader can observe intermediate states.
   *   2. The page's slot budget allows at least increment_size(key) bytes.
   *
   * Memory layout invariant after return:
  *   branch_bits[] may grow by one uint32_e entry; branch_offsets[] shifts up
  *   by 8 bytes (one offset_e slot) when branch_bits[] crosses an 8-byte alignment boundary.
   *   All moves are performed with memmove in an order that avoids aliasing.
   *
   * @return The array index of the freshly inserted (uninitialized) slot.
   */
  uint16_t insert_branch(int key) {
    assert(key != OUT_OF_RANGE);
    assert(!(this->isset)(key));
    assert(branch_count() < int(MAX_BRANCH_COUNT) - 1);

    uint16_t old_count = (uint16_t)branch_count();

    // NONE branch: stored at branch_offsets()[0], flagged by NULL_MASK.
    if (key == NONE) {
      assert(!has_none());
      memmove((void*)(branch_offsets() + 1), (void*)branch_offsets(),
              old_count * sizeof(offset_e));
      _branch_count = (_branch_count + 1) | NULL_MASK;
      return 0;
    }

    uint8_t bit = branch_bits_group(key);

    if (_branch_bits_index & (1u << bit)) {
      // Branch-bits group already present: only the bitmap word changes.
      // The branch_bits[] array does not grow, so branch_offsets_start stays fixed.
      uint32_e* branch_bits_ = branch_bits();
      int lidx = bits::index(_branch_bits_index, bit);

      // Compute insertion index before modifying the bitmap.
      int oidx = bits::count(branch_bits_[lidx] & ((1u << branch_bit(key)) - 1)) +
                 bool(_branch_count & NULL_MASK);
      if (lidx > 0) oidx += bits::count(branch_bits_[0]);
      if (lidx > 1) oidx += bits::count(branch_bits_[1]);
      if (lidx > 2) oidx += bits::count(branch_bits_[2]);
      if (lidx > 3) oidx += bits::count(branch_bits_[3]);
      if (lidx > 4) oidx += bits::count(branch_bits_[4]);
      if (lidx > 5) oidx += bits::count(branch_bits_[5]);
      if (lidx > 6) oidx += bits::count(branch_bits_[6]);

      branch_bits_[lidx] |= 1u << branch_bit(key);

      // Shift branch offsets right to open a slot at oidx.
      offset_e* branch_offsets_ = branch_offsets();
      memmove((void*)(branch_offsets_ + oidx + 1), (void*)(branch_offsets_ + oidx),
              (old_count - oidx) * sizeof(offset_e));
      _branch_count++;
      return (uint16_t)oidx;
    }

    // New branch-bits group: branch_bits[] grows by one uint32_e entry.
    // branch_offsets_start may move up by 8 bytes (one alignment step).
    uint16_t branch_bits_start_ = branch_bits_start();
    uint32_e* branch_bits_ = branch_bits();
    uint16_t old_bcount = bits::count(_branch_bits_index);

    _branch_bits_index |= (1u << bit);
    int new_lidx = bits::index(_branch_bits_index, bit);

    // Insertion index: sum bit-counts of all branch-bits groups before
    // new_lidx using the old branch_bits[] (new entry not yet written).
    int oidx = bool(_branch_count & NULL_MASK);
    for (int i = 0; i < new_lidx; i++) oidx += bits::count(branch_bits_[i]);

    uint16_t old_as = align(branch_bits_start_ + old_bcount * (uint16_t)sizeof(uint32_e));
    uint16_t new_as = align(branch_bits_start_ + (old_bcount + 1) * (uint16_t)sizeof(uint32_e));
    int shift = new_as - old_as;  // 0 or 8

    offset_e* old_branch_offsets = (offset_e*)((char*)this + old_as);
    offset_e* new_branch_offsets = (offset_e*)((char*)this + new_as);

    if (shift > 0) {
      // branch_bits_[old_bcount] sits exactly at old_branch_offsets; move
      // offsets first to avoid clobbering when branch_bits[] grows.
      // Move suffix (higher addresses first so memmove handles the overlap).
      memmove((void*)(new_branch_offsets + oidx + 1), (void*)(old_branch_offsets + oidx),
              (old_count - oidx) * sizeof(offset_e));
      // Move prefix.
      if (oidx > 0)
        memmove((void*)new_branch_offsets, (void*)old_branch_offsets, oidx * sizeof(offset_e));
      // Now safe to extend branch_bits[].
      memmove((void*)(branch_bits_ + new_lidx + 1), (void*)(branch_bits_ + new_lidx),
              (old_bcount - new_lidx) * sizeof(uint32_e));
      branch_bits_[new_lidx] = 1u << branch_bit(key);
    } else {
      // Offsets stay at old_as. Extend branch_bits[] into the padding gap
      // first, then shift offsets right in place.
      memmove((void*)(branch_bits_ + new_lidx + 1), (void*)(branch_bits_ + new_lidx),
              (old_bcount - new_lidx) * sizeof(uint32_e));
      branch_bits_[new_lidx] = 1u << branch_bit(key);
      memmove((void*)(old_branch_offsets + oidx + 1), (void*)(old_branch_offsets + oidx),
              (old_count - oidx) * sizeof(offset_e));
    }

    _branch_offsets_pos = new_as / (uint16_t)sizeof(offset_e);
    _branch_count++;
    return (uint16_t)oidx;
  }

/**
 * @brief Creates a TrieNode from an array of offsets for all possible byte
 * values.
 *
 * This method builds a complete trie node by iterating through all 256
 * possible byte values plus the NONE branch, creating the bitmap structure
 * for only the branches that have non-zero offsets. This is useful for bulk
 * creation of nodes where you have a pre-computed array of child offsets.
 *
 * @param prefix The compressed prefix data to store in the node.
 * @param offsets Array of 257 offsets indexed by byte value (0-255) plus NONE
 * at index -1. Non-zero values indicate branches to create; zero values are
 * skipped.
 *
 * @note The offsets array must have NONE at index -1, accessible as
 * offsets[NONE].
 */
void create(const Slice& prefix, offset_e* offsets,
            uint8_t precomputed_branch_bits_index) {
  assert(prefix.size() < 256);
  _prefix_len = prefix.size();
  memcpy(_prefix, prefix.data(), _prefix_len);

  _branch_bits_index = precomputed_branch_bits_index;

  // Now we can correctly calculate offsets
  _branch_bits_pos = calc_branch_bits_start() / sizeof(uint32_e);
  _branch_offsets_pos = calc_branch_offsets_start() / sizeof(offset_e);

  offset_e* branch_offsets_ = branch_offsets();
  uint32_e* branch_bits_ = branch_bits();
  memset(branch_bits_, 0, branch_bits_size());

  if (offsets[NONE]) {
    _branch_count = 1 | TrieNode::NULL_MASK;
    *branch_offsets_++ = offsets[NONE];
  } else {
    _branch_count = 0;
  }

  // Iterate only groups with bits set in _branch_bits_index
  uint8_t remaining = _branch_bits_index;
  while (remaining) {
    int grp = bits::first(remaining);
    remaining &= remaining - 1;  // clear lowest set bit
    int base = grp << 5;
    uint32_t lbits = 0;
    for (int j = 0; j < 32; j++) {
      if (offsets[base + j]) {
        _branch_count++;
        *branch_offsets_++ = offsets[base + j];
        lbits |= 1u << j;
      }
    }
    branch_bits_[bits::index(_branch_bits_index, grp)] = lbits;
  }
}

// check if the index exists
bool isset(int nchar) const {
  if (nchar == NONE) return has_none();
  return (_branch_bits_index & (1u << branch_bits_group(nchar))) &&
         (branch_bits()[bits::index(_branch_bits_index, branch_bits_group(nchar))] & (1u << branch_bit(nchar)));
}

int branch_index(int nchar) const {
  assert(nchar >= NONE);
  if (nchar == NONE) return has_none() ? 0 : -1;

  uint32_e* branch_bits_ = branch_bits();
  int lidx = bits::index(_branch_bits_index, branch_bits_group(nchar));
  if (_branch_bits_index & (1u << branch_bits_group(nchar))) {
    if (!(branch_bits_[lidx] & (1u << branch_bit(nchar)))) return -1;
    int oidx = bits::count(branch_bits_[lidx] & ((1u << branch_bit(nchar)) - 1)) +
               bool(_branch_count & NULL_MASK);
    if (lidx > 0) oidx += bits::count(branch_bits_[0]);
    if (lidx > 1) oidx += bits::count(branch_bits_[1]);
    if (lidx > 2) oidx += bits::count(branch_bits_[2]);
    if (lidx > 3) oidx += bits::count(branch_bits_[3]);
    if (lidx > 4) oidx += bits::count(branch_bits_[4]);
    if (lidx > 5) oidx += bits::count(branch_bits_[5]);
    if (lidx > 6) oidx += bits::count(branch_bits_[6]);
    assert(oidx < branch_count());
    return oidx;
  }

  return -1;
}

// returns the link for nchar
offset_e* branch_offset(int nchar) {
  auto idx = branch_index(nchar);
  return idx >= 0 ? &branch_offsets()[idx] : nullptr;
}

int _prev_branch_bits_group(int nchar) const {
  int lidx = bits::prev(_branch_bits_index, branch_bits_group(nchar));
  if (lidx < 0) return has_none() ? NONE : OUT_OF_RANGE;
  int lbit_idx = bits::last(branch_bits()[bits::index(_branch_bits_index, lidx)]);
  return (lidx << 5) | lbit_idx;
}

int prev(int nchar) const {
  if (nchar == NONE) return OUT_OF_RANGE;
  if (!(_branch_bits_index & (1u << branch_bits_group(nchar)))) return _prev_branch_bits_group(nchar);

  int lidx = bits::index(_branch_bits_index, branch_bits_group(nchar)),
      lbit_idx = bits::prev(branch_bits()[lidx], branch_bit(nchar));
  if (lbit_idx < 0) return _prev_branch_bits_group(nchar);
  return (branch_bits_group(nchar) << 5) | lbit_idx;
}

int _next_branch_bits_group(int nchar) const {
  int lidx = bits::next(_branch_bits_index, branch_bits_group(nchar));
  if (lidx < 0) return OUT_OF_RANGE;
  int lbit_idx = bits::first(branch_bits()[bits::index(_branch_bits_index, lidx)]);
  return (lidx << 5) | lbit_idx;
}

int next(int nchar) const {
  if (nchar == NONE) {
    int lidx = bits::first(_branch_bits_index);
    if (lidx < 0) return OUT_OF_RANGE;
    return (lidx << 5) | bits::first(branch_bits()[0]);
  }
  if (!(_branch_bits_index & (1u << branch_bits_group(nchar)))) return _next_branch_bits_group(nchar);
  int lidx = bits::index(_branch_bits_index, branch_bits_group(nchar)),
      lbit_idx = bits::next(branch_bits()[lidx], branch_bit(nchar));
  if (lbit_idx < 0) return _next_branch_bits_group(nchar);
  return (branch_bits_group(nchar) << 5) | lbit_idx;
}

int first() const {
  if (has_none()) return NONE;
  return (bits::first(_branch_bits_index) << 5) | bits::first(branch_bits()[0]);
}

/**
 * @brief Iterate all branches via direct bitmap scan — no per-step popcount.
 *
 * Calls fn(key, offset_ptr) for every branch in sorted order (NONE first,
 * then 0-255).  branch_offsets_idx is tracked with a simple increment instead of
 * recomputing bits::index on every step.
 */
template <typename Fn>
void for_each_branch(Fn fn) const {
  offset_e* branch_offsets_ = branch_offsets();
  int branch_offsets_idx = 0;

  // NONE branch (stored at array position 0 when present)
  if (has_none()) {
    fn(NONE, &branch_offsets_[branch_offsets_idx]);
    branch_offsets_idx++;
  }

  // Iterate branch-bit groups
  uint8_t remaining_branch_bits_index = _branch_bits_index;
  const uint32_e* branch_bits_ = branch_bits();
  int branch_bits_idx = 0;
  while (remaining_branch_bits_index) {
    int grp = bits::first(remaining_branch_bits_index);
    remaining_branch_bits_index &= remaining_branch_bits_index - 1;

    uint32_t remaining_branch_bits = static_cast<uint32_t>(branch_bits_[branch_bits_idx]);
    int base = grp << 5;
    while (remaining_branch_bits) {
      int bit = bits::first(remaining_branch_bits);
      remaining_branch_bits &= remaining_branch_bits - 1;
      fn(base | bit, &branch_offsets_[branch_offsets_idx]);
      branch_offsets_idx++;
    }
    branch_bits_idx++;
  }
}

};
#pragma pack(pop)

#pragma pack(push, 1)
template <typename Traits>
struct _LeafNodeHeaderBase {
  using traits_t = Traits;
  using uint16_e = typename Traits::uint16_e;

  uint16_e value_size;
  uint8_t key_size;
};

template <typename Traits>
struct _LeafNodeHeaderNoHash : _LeafNodeHeaderBase<Traits> {
  static constexpr size_t HASH_SIZE = 0;
};

template <typename Traits, size_t HashSize>
struct _LeafNodeHeaderHash : _LeafNodeHeaderBase<Traits> {
  static_assert(HashSize > 0, "HashSize must be greater than zero");

  static constexpr size_t HASH_SIZE = HashSize;

  uint8_t _hash[HashSize];
};

template <typename Header_>
struct _LeafNode : Header_ {
  typedef _LeafNode<Header_> LeafNode;

  using Header = Header_;
  using traits_t = typename Header::traits_t;
  using uint16_e = typename traits_t::uint16_e;
  using uint32_e = typename traits_t::uint32_e;
  using uint64_e = typename traits_t::uint64_e;
  using offset_e = typename traits_t::offset_e;
  static constexpr size_t HASH_SIZE = Header::HASH_SIZE;
  static constexpr bool HAS_HASH = HASH_SIZE > 0;
  using Header::value_size;
  using Header::key_size;
  static constexpr uint16_t HEADER_SIZE = sizeof(Header);
  static constexpr uint16_t MAX_SIZE =
      traits_t::PAGE_SIZES[traits_t::PAGE_SIZES_COUNT - 1];
  static constexpr uint16_t BIG_VALUE_FLAG = uint16_t(1) << 15;

  uint8_t data[1];
  uint8_t* vdata() { return data + key_size; }
  const uint8_t* vdata() const { return data + key_size; }
  Slice key() { return Slice(data, key_size); }
  Slice value() const { return Slice(data + key_size, vsize()); }
  uint16_t vsize() const { return value_size & ~BIG_VALUE_FLAG; }
  uint16_t size() const { return HEADER_SIZE + key_size + vsize(); }

  void set_big() { value_size |= BIG_VALUE_FLAG; }
  void clear_big() { value_size &= ~BIG_VALUE_FLAG; }

  void set(const Slice& key, size_t value_size_) {
    assert(key.size() < 256);
    key_size = key.size();
    memcpy(data, key.data(), key.size());
    value_size = value_size_;
  }

  Slice memory() { return Slice((char*)this, size()); }

  bool is_big() const {
    return (value_size & BIG_VALUE_FLAG) == BIG_VALUE_FLAG;
  }

  static uint16_t size(uint16_t key, size_t value) {
    assert(HEADER_SIZE + key + value <= MAX_SIZE);
    return HEADER_SIZE + key + value;
  }

  static uint16_t size(const Slice& key, const Slice& value) {
    return size(key.size(), value.size());
  }
};
#pragma pack(pop)

}  // namespace leaves

#endif  // _LEAVES__NODE_HPP
