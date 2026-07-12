/*
Trie node layout and node-level metadata for the internal key/value tree.
*/
#ifndef _LEAVES__NODE_HPP
#define _LEAVES__NODE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

// dummy structure for all node pointers
struct _Node {};

/*
A compressed Trie node (https://www.geeksforgeeks.org/compressed-tries/)
Every node has at least one char in the compressed data (the branch_key of the
parent node) This makes the implmentation of many operations easier.
*/
#pragma pack(push, 1)
template <typename Traits>
struct _TrieNode {
  typedef _TrieNode<Traits> TrieNode;
  using hash_t = typename Traits::hash_t;
  using uint32_e = typename Traits::uint32_e;
  using uint16_e = typename Traits::uint16_e;
  using offset_e = typename Traits::offset_e;
  static constexpr uint16_t MAX_BRANCH_COUNT = 257;  // 256 (chars) + 1 (NULL)
  hash_t hash;
  uint16_e _array_len;  // < MAX_BRANCH_COUNT
  uint8_t _upper;
  uint8_t _compressed_len;
  uint8_t _lower_offset;
  uint8_t _array_offset;
  uint8_t _compressed_data[1];

  static constexpr uint16_t NULL_MASK = uint16_t(1) << 15;
  constexpr static int NONE = -1;
  constexpr static int OUT_OF_RANGE = -2;
  constexpr static uint16_t MAX_SIZE =
      align(padding(sizeof(TrieNode) + 255, sizeof(uint32_e)) +
            8 * sizeof(uint32_e)) +
      MAX_BRANCH_COUNT * sizeof(offset_e);
  constexpr static uint8_t LOWER_MASK = 0b00011111;

  uint8_t len() const { return _compressed_len; }
  int count() const { return (_array_len & ~NULL_MASK); }
  const uint8_t* compressed() const { return _compressed_data; }
  uint16_t lower_size() const { return bits::count(_upper) * sizeof(uint32_e); }
  uint16_t lower_start() const { return _lower_offset * sizeof(uint32_e); }
  uint16_t lower_end() const { return lower_start() + lower_size(); }
  uint32_e* lower() const {
    return (uint32_e*)((uint8_t*)this + lower_start());
  }
  uint16_t array_start() const { return _array_offset * sizeof(offset_e); }
  uint16_t array_end() const { return array_start() + array_size(); }
  uint16_t array_size() const { return count() * sizeof(offset_e); }
  offset_e* array() const {
    return (offset_e*)((uint8_t*)this + array_start());
  }
  static uint8_t ubit(uint8_t val) { return (val >> 5); }
  static uint32_t lbit(uint8_t val) { return (val & LOWER_MASK); }
  uint16_t size() const { return array_end(); }
  uint16_t changed_len(uint8_t new_compressed_len) const {
    uint16_t prefix_size =
        padding(sizeof(TrieNode) + new_compressed_len, sizeof(uint32_e));
    return align(prefix_size + lower_size() + array_size());
  }

  uint16_t increment_size(int key) const {
    uint16_t prefix_size =
        padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
    uint16_t lower_size_ = lower_size();
    if (key != NONE && !(_upper & (1u << ubit(key)))) {
      lower_size_ += sizeof(uint32_e);
    }
    uint16_t array_size_ = (count() + 1) * sizeof(offset_e);
    return align(prefix_size + lower_size_ + array_size_);
  }

  uint16_t decrement_size(int key) const {
    uint16_t prefix_size =
        padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
    uint16_t lower_size_ = lower_size();
    if (key != NONE && (_upper & (1u << ubit(key)))) {
      // Only decrement lower_size if this key is the only one in its lower
      // bucket
      int lidx = bits::index(_upper, ubit(key));
      if (bits::count(lower()[lidx]) == 1) {
        lower_size_ -= sizeof(uint32_e);
      }
    }
    uint16_t array_size_ = (count() - 1) * sizeof(offset_e);
    return align(prefix_size + lower_size_ + array_size_);
  }

  uint16_t calc_lower_start() const {
    return padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
  }

  uint16_t calc_array_start() const {
    return align(lower_start() + bits::count(_upper) * sizeof(uint32_e));
  }

  static constexpr uint16_t size(uint8_t prefix, int key1, int key2) {
    assert(key1 != key2);
    uint16_t prefix_size = padding(sizeof(TrieNode) + prefix, sizeof(uint32_e));
    uint16_t lower_size;
    if (key1 == NONE || key2 == NONE) {
      lower_size = sizeof(uint32_e);  // only key2's upper bit
    } else if (ubit(key1) == ubit(key2)) {
      lower_size = sizeof(uint32_e);  // same upper bit
    } else {
      lower_size = 2 * sizeof(uint32_e);  // different upper bits
    }
    uint16_t array_start = align(prefix_size + lower_size);
    return array_start + 2 * sizeof(offset_e);
  }

  // Estimates the max size for a trie node with a given prefix and branches.
  // This is an upper-bound estimate used for allocation - actual size may be smaller
  // due to bitmap compression of sparse branch arrays.
  static constexpr uint16_t size(uint8_t prefix, uint16_t branches) {
    uint16_t prefix_size = padding(sizeof(TrieNode) + prefix, sizeof(uint32_e));
    uint16_t lower_size = std::min(branches, (uint16_t)8) * sizeof(uint32_e);
    uint16_t array_size = branches * sizeof(offset_e);
    return align(prefix_size + lower_size + array_size);
  }

  bool has_none() const { return _array_len & NULL_MASK; }

  // create a trie node with a prefix and two keys; returns the array indexes
  // for key1 and key2
  std::pair<uint16_t, uint16_t> create(Slice prefix, int key1, int key2) {
    assert(key1 >= NONE);
    assert(key2 >= NONE);

    _compressed_len = prefix.size();
    memcpy(_compressed_data, prefix.data(), _compressed_len);

    std::pair<uint16_t, uint16_t> result(0, 1);
    if (key2 < key1) {
      std::swap(key1, key2);
      std::swap(result.first, result.second);
    }

    _array_len = 2;
    uint16_t array_start_, lower_start_ = calc_lower_start();
    uint32_e* lower_ = (uint32_e*)((char*)this + lower_start_);

    if (key1 != NONE) {
      _upper = (1u << ubit(key1)) | (1u << ubit(key2));
      if (bits::count(_upper) == 1) {
        lower_[0] = (1u << lbit(key1)) | (1u << lbit(key2));
        array_start_ = align(lower_start_ + sizeof(uint32_e));
      } else {
        lower_[0] = 1u << lbit(key1);
        lower_[1] = 1u << lbit(key2);
        array_start_ = align(lower_start_ + 2 * sizeof(uint32_e));
      }
    } else {
      _upper = 1u << ubit(key2);
      _array_len = _array_len | NULL_MASK;
      lower_[0] = 1u << lbit(key2);
      array_start_ = align(lower_start_ + sizeof(uint32_e));
    }

    _array_offset = array_start_ / sizeof(offset_e);
    _lower_offset = lower_start_ / sizeof(uint32_e);
    return result;
  }

  /**
   * @brief Creates and initializes a TrieNode with the given prefix and key.
   *
   * @param prefix A Slice object representing the prefix to be stored in the
   * node.
   * @param key An integer key used to set the upper and lower bitmaps. If the
   * key is NONE, the node is marked as null.
   * @return array_index of the key (0)
   */
  uint16_t create(Slice prefix, int key) {
    _compressed_len = prefix.size();
    memcpy(_compressed_data, prefix.data(), _compressed_len);

    _array_len = 1;
    uint16_t array_start_,
        lower_start_ =
            padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
    uint32_e* lower_ = (uint32_e*)((char*)this + lower_start_);

    if (key != NONE) {
      _upper = 1u << ubit(key);
      lower_[0] = 1u << lbit(key);
      array_start_ = align(lower_start_ + sizeof(uint32_e));
    } else {
      _upper = 0;
      array_start_ = align(lower_start_);
      _array_len = _array_len | NULL_MASK;
    }

    _array_offset = array_start_ / sizeof(offset_e);
    _lower_offset = lower_start_ / sizeof(uint32_e);
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
    _compressed_len = prefix.size();
    memcpy(_compressed_data, prefix.data(), _compressed_len);
    _array_len = src._array_len;
    _upper = src._upper;

    uint16_t bcount = bits::count(_upper);
    _lower_offset =
        padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e)) /
        sizeof(uint32_e);
    _array_offset =
        align(lower_start() + bcount * sizeof(uint32_e)) / sizeof(offset_e);

    memcpy(lower(), src.lower(), bcount * sizeof(uint32_e));
    memcpy((void*)array(), src.array(), count() * sizeof(offset_e));
  }

  /**
   * @brief Creates a new TrieNode by adding a branch to an existing node.
   *
   * This method copies the structure from a source node and adds a new branch
   * for the specified key. It handles both the case where the key's upper bit
   * already exists in the source (adding to an existing lower bitmap) and the
   * case where a new upper bit needs to be created.
   *
   * @param src The source TrieNode to copy from.
   * @param key The character key for the new branch, or NONE for a null branch.
   * @return The byte offset within this node to the newly created array slot
   *         for the key's link: (char*)this + offset points to the offset_e
   * slot.
   */
  template <typename TNode>
  uint16_t create(const TNode& src, int key) {
    _compressed_len = src._compressed_len;
    memcpy(_compressed_data, src.compressed(), _compressed_len);

    uint16_t lower_start_ =
        padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
    uint32_e* lower_ = (uint32_e*)((char*)this + lower_start_);

    _array_len = src._array_len + 1;
    _upper = src._upper;
    int oidx;
    if (key != NONE) {
      uint8_t bit = ubit(key);
      _upper |= (1u << bit);
      int lidx = bits::index(_upper, bit);
      if (src._upper & (1u << bit)) {
        // _upper == src._upper
        memcpy(lower_, src.lower(), bits::count(_upper) * sizeof(uint32_e));
        lower_[lidx] |= 1u << lbit(key);
      } else {
        // bits::count(_upper) == bits::count(src._upper) + 1
        memcpy(lower_, src.lower(), lidx * sizeof(uint32_e));
        memcpy(lower_ + lidx + 1, src.lower() + lidx,
               (bits::count(src._upper) - lidx) * sizeof(uint32_e));
        lower_[lidx] = 1u << lbit(key);
      }

      oidx = bits::count(lower_[lidx] & ((1u << lbit(key)) - 1)) +
             bool(_array_len & NULL_MASK);
      // Unrolled loop - lidx is at most 7
      if (lidx > 0) oidx += bits::count(lower_[0]);
      if (lidx > 1) oidx += bits::count(lower_[1]);
      if (lidx > 2) oidx += bits::count(lower_[2]);
      if (lidx > 3) oidx += bits::count(lower_[3]);
      if (lidx > 4) oidx += bits::count(lower_[4]);
      if (lidx > 5) oidx += bits::count(lower_[5]);
      if (lidx > 6) oidx += bits::count(lower_[6]);
    } else {
      assert((src._array_len & NULL_MASK) == 0);
      _array_len = _array_len | NULL_MASK;
      memcpy(lower_, src.lower(), bits::count(_upper) * sizeof(uint32_e));
      oidx = 0;
    }

    _lower_offset = lower_start_ / sizeof(uint32_e);
    uint16_t array_start_ = calc_array_start();
    _array_offset = array_start_ / sizeof(offset_e);

    offset_e* array_ = (offset_e*)((char*)this + array_start_);
    memcpy((void*)array_, src.array(), oidx * sizeof(offset_e));
    memcpy((void*)(array_ + oidx + 1), src.array() + oidx,
           (count() - 1 - oidx) * sizeof(offset_e));
    return oidx;
  }

  // create a new trie node without the branch of key
  void create_remove(const TrieNode& src, int key) {
    assert(src.isset(key));
    _compressed_len = src._compressed_len;
    memcpy(_compressed_data, src.compressed(), _compressed_len);

    uint16_t lower_start_ =
        padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
    uint32_e* lower_ = (uint32_e*)((char*)this + lower_start_);

    _array_len = src._array_len - 1;
    _upper = src._upper;
    int oidx;
    if (key != NONE) {
      uint8_t bit = ubit(key);
      int lidx = bits::index(src._upper, bit);
      uint32_e src_lower_val = src.lower()[lidx];

      // Calculate oidx using source value
      oidx = bits::count(src_lower_val & ((1u << lbit(key)) - 1)) +
             bool(_array_len & NULL_MASK);
      // Unrolled loop - lidx is at most 7
      uint32_e* src_lower = src.lower();
      if (lidx > 0) oidx += bits::count(src_lower[0]);
      if (lidx > 1) oidx += bits::count(src_lower[1]);
      if (lidx > 2) oidx += bits::count(src_lower[2]);
      if (lidx > 3) oidx += bits::count(src_lower[3]);
      if (lidx > 4) oidx += bits::count(src_lower[4]);
      if (lidx > 5) oidx += bits::count(src_lower[5]);
      if (lidx > 6) oidx += bits::count(src_lower[6]);

      // Check if removing this bit will empty the lower bucket
      uint32_e new_lower_val = src_lower_val & ~(1u << lbit(key));
      if (!new_lower_val) {
        // Remove the upper bit and copy lower array around the removed bucket
        _upper &= ~(1u << bit);
        memcpy(lower_, src.lower(), lidx * sizeof(uint32_e));
        memcpy(lower_ + lidx, src.lower() + lidx + 1,
               (bits::count(src._upper) - lidx - 1) * sizeof(uint32_e));
      } else {
        // Keep the lower bucket with the bit removed
        memcpy(lower_, src.lower(), bits::count(src._upper) * sizeof(uint32_e));
        lower_[lidx] = new_lower_val;
      }
    } else {
      assert(src._array_len & NULL_MASK);
      _array_len = _array_len & uint16_t(~NULL_MASK);
      memcpy(lower_, src.lower(), bits::count(_upper) * sizeof(uint32_e));
      oidx = 0;
    }

    _lower_offset = lower_start_ / sizeof(uint32_e);
    uint16_t array_start_ = calc_array_start();
    _array_offset = array_start_ / sizeof(offset_e);

    offset_e* array_ = (offset_e*)((char*)this + array_start_);
    memcpy((void*)array_, src.array(), oidx * sizeof(offset_e));
    memcpy((void*)(array_ + oidx), src.array() + oidx + 1,
           (count() - oidx) * sizeof(offset_e));
  }

  /**
   * @brief Insert a new branch for @p key in-place, without allocating a new
   * node.  The caller must guarantee:
   *   1. The page that owns this node belongs to the current write transaction
   *      (needs_cow() == false), so no reader can observe intermediate states.
   *   2. The page's slot budget allows at least increment_size(key) bytes.
   *
   * Memory layout invariant after return:
   *   lower[] may grow by one uint32_e entry; array[] shifts up by 8 bytes
   *   (one offset_e slot) when lower[] crosses an 8-byte alignment boundary.
   *   All moves are performed with memmove in an order that avoids aliasing.
   *
   * @return The array index of the freshly inserted (uninitialized) slot.
   */
  uint16_t insert_branch(int key) {
    assert(key != OUT_OF_RANGE);
    assert(!isset(key));
    assert(count() < int(MAX_BRANCH_COUNT) - 1);

    uint16_t old_count = (uint16_t)count();

    // NONE branch: stored at array[0], flagged by NULL_MASK in _array_len.
    if (key == NONE) {
      assert(!has_none());
      memmove((void*)(array() + 1), (void*)array(),
              old_count * sizeof(offset_e));
      _array_len = (_array_len + 1) | NULL_MASK;
      return 0;
    }

    uint8_t bit = ubit(key);

    if (_upper & (1u << bit)) {
      // Upper group already present: only the lower bitmap changes.
      // The lower[] array does not grow → array_start stays fixed.
      uint32_e* lower_ = lower();
      int lidx = bits::index(_upper, bit);

      // Compute insertion index before modifying the bitmap.
      int oidx = bits::count(lower_[lidx] & ((1u << lbit(key)) - 1)) +
                 bool(_array_len & NULL_MASK);
      if (lidx > 0) oidx += bits::count(lower_[0]);
      if (lidx > 1) oidx += bits::count(lower_[1]);
      if (lidx > 2) oidx += bits::count(lower_[2]);
      if (lidx > 3) oidx += bits::count(lower_[3]);
      if (lidx > 4) oidx += bits::count(lower_[4]);
      if (lidx > 5) oidx += bits::count(lower_[5]);
      if (lidx > 6) oidx += bits::count(lower_[6]);

      lower_[lidx] |= 1u << lbit(key);

      // Shift array right to open a slot at oidx.
      offset_e* array_ = array();
      memmove((void*)(array_ + oidx + 1), (void*)(array_ + oidx),
              (old_count - oidx) * sizeof(offset_e));
      _array_len++;
      return (uint16_t)oidx;
    }

    // New upper group: lower[] grows by one uint32_e entry.
    // array_start may move up by 8 bytes (one alignment step).
    uint16_t ls = lower_start();
    uint32_e* lower_ = lower();
    uint16_t old_bcount = bits::count(_upper);

    _upper |= (1u << bit);
    int new_lidx = bits::index(_upper, bit);

    // Insertion index: sum bit-counts of all lower groups before new_lidx
    // using the OLD lower[] (new entry not yet written).
    int oidx = bool(_array_len & NULL_MASK);
    for (int i = 0; i < new_lidx; i++) oidx += bits::count(lower_[i]);

    uint16_t old_as = align(ls + old_bcount * (uint16_t)sizeof(uint32_e));
    uint16_t new_as = align(ls + (old_bcount + 1) * (uint16_t)sizeof(uint32_e));
    int shift = new_as - old_as;  // 0 or 8

    offset_e* old_arr = (offset_e*)((char*)this + old_as);
    offset_e* new_arr = (offset_e*)((char*)this + new_as);

    if (shift > 0) {
      // lower_[old_bcount] sits exactly at old_arr; move array FIRST to avoid
      // clobbering when we extend lower[].
      // Move suffix (higher addresses first so memmove handles the overlap).
      memmove((void*)(new_arr + oidx + 1), (void*)(old_arr + oidx),
              (old_count - oidx) * sizeof(offset_e));
      // Move prefix.
      if (oidx > 0)
        memmove((void*)new_arr, (void*)old_arr, oidx * sizeof(offset_e));
      // Now safe to extend lower[].
      memmove((void*)(lower_ + new_lidx + 1), (void*)(lower_ + new_lidx),
              (old_bcount - new_lidx) * sizeof(uint32_e));
      lower_[new_lidx] = 1u << lbit(key);
    } else {
      // Array stays at old_as. Extend lower[] into the padding gap first,
      // then shift array right in place.
      memmove((void*)(lower_ + new_lidx + 1), (void*)(lower_ + new_lidx),
              (old_bcount - new_lidx) * sizeof(uint32_e));
      lower_[new_lidx] = 1u << lbit(key);
      memmove((void*)(old_arr + oidx + 1), (void*)(old_arr + oidx),
              (old_count - oidx) * sizeof(offset_e));
    }

    _array_offset = new_as / (uint16_t)sizeof(offset_e);
    _array_len++;
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
void create(const Slice& prefix, offset_e* offsets, uint8_t precomputed_upper) {
  assert(prefix.size() < 256);
  _compressed_len = prefix.size();
  memcpy(_compressed_data, prefix.data(), _compressed_len);

  _upper = precomputed_upper;

  // Now we can correctly calculate offsets
  _lower_offset = calc_lower_start() / sizeof(uint32_e);
  _array_offset = calc_array_start() / sizeof(offset_e);

  offset_e* array_ = array();
  uint32_e* lower_ = lower();
  memset(lower_, 0, lower_size());

  if (offsets[NONE]) {
    _array_len = 1 | TrieNode::NULL_MASK;
    *array_++ = offsets[NONE];
  } else {
    _array_len = 0;
  }

  // Iterate only groups with bits set in _upper
  uint8_t remaining = _upper;
  while (remaining) {
    int grp = bits::first(remaining);
    remaining &= remaining - 1;  // clear lowest set bit
    int base = grp << 5;
    uint32_t lbits = 0;
    for (int j = 0; j < 32; j++) {
      if (offsets[base + j]) {
        _array_len++;
        *array_++ = offsets[base + j];
        lbits |= 1u << j;
      }
    }
    lower_[bits::index(_upper, grp)] = lbits;
  }
}

// check if the index exists
bool isset(int nchar) const {
  if (nchar == NONE) return has_none();
  return (_upper & (1u << ubit(nchar))) &&
         (lower()[bits::index(_upper, ubit(nchar))] & (1u << lbit(nchar)));
}

int array_index(int nchar) const {
  assert(nchar >= NONE);
  if (nchar == NONE) return has_none() ? 0 : -1;

  uint32_e* lower_ = lower();
  int lidx = bits::index(_upper, ubit(nchar));
  if (_upper & (1u << ubit(nchar))) {
    if (!(lower_[lidx] & (1u << lbit(nchar)))) return -1;
    int oidx = bits::count(lower_[lidx] & ((1u << lbit(nchar)) - 1)) +
               bool(_array_len & NULL_MASK);
    if (lidx > 0) oidx += bits::count(lower_[0]);
    if (lidx > 1) oidx += bits::count(lower_[1]);
    if (lidx > 2) oidx += bits::count(lower_[2]);
    if (lidx > 3) oidx += bits::count(lower_[3]);
    if (lidx > 4) oidx += bits::count(lower_[4]);
    if (lidx > 5) oidx += bits::count(lower_[5]);
    if (lidx > 6) oidx += bits::count(lower_[6]);
    assert(oidx < count());
    return oidx;
  }

  return -1;
}

// returns the link for nchar
offset_e* offset(int nchar) {
  auto idx = array_index(nchar);
  return idx >= 0 ? &array()[idx] : nullptr;
}

int _prev_lower(int nchar) const {
  int lidx = bits::prev(_upper, ubit(nchar));
  if (lidx < 0) return has_none() ? NONE : OUT_OF_RANGE;
  int lbit_idx = bits::last(lower()[bits::index(_upper, lidx)]);
  return (lidx << 5) | lbit_idx;
}

int prev(int nchar) const {
  if (nchar == NONE) return OUT_OF_RANGE;
  if (!(_upper & (1u << ubit(nchar)))) return _prev_lower(nchar);

  int lidx = bits::index(_upper, ubit(nchar)),
      lbit_idx = bits::prev(lower()[lidx], lbit(nchar));
  if (lbit_idx < 0) return _prev_lower(nchar);
  return (ubit(nchar) << 5) | lbit_idx;
}

int _next_lower(int nchar) const {
  int lidx = bits::next(_upper, ubit(nchar));
  if (lidx < 0) return OUT_OF_RANGE;
  int lbit_idx = bits::first(lower()[bits::index(_upper, lidx)]);
  return (lidx << 5) | lbit_idx;
}

int next(int nchar) const {
  if (nchar == NONE) {
    int lidx = bits::first(_upper);
    if (lidx < 0) return OUT_OF_RANGE;
    return (lidx << 5) | bits::first(lower()[0]);
  }
  if (!(_upper & (1u << ubit(nchar)))) return _next_lower(nchar);
  int lidx = bits::index(_upper, ubit(nchar)),
      lbit_idx = bits::next(lower()[lidx], lbit(nchar));
  if (lbit_idx < 0) return _next_lower(nchar);
  return (ubit(nchar) << 5) | lbit_idx;
}

int first() const {
  if (has_none()) return NONE;
  return (bits::first(_upper) << 5) | bits::first(lower()[0]);
}

/**
 * @brief Iterate all branches via direct bitmap scan — no per-step popcount.
 *
 * Calls fn(key, offset_ptr) for every branch in sorted order (NONE first,
 * then 0-255).  array_idx is tracked with a simple increment instead of
 * recomputing bits::index on every step.
 */
template <typename Fn>
void for_each_branch(Fn fn) const {
  offset_e* arr = array();
  int arr_idx = 0;

  // NONE branch (stored at array position 0 when present)
  if (has_none()) {
    fn(NONE, &arr[arr_idx]);
    arr_idx++;
  }

  // Iterate upper bitmap groups
  uint8_t remaining_upper = _upper;
  const uint32_e* lower_ = lower();
  int lower_idx = 0;
  while (remaining_upper) {
    int grp = bits::first(remaining_upper);
    remaining_upper &= remaining_upper - 1;  // blsr: clear lowest set bit

    uint32_t remaining_lower = static_cast<uint32_t>(lower_[lower_idx]);
    int base = grp << 5;
    while (remaining_lower) {
      int bit = bits::first(remaining_lower);
      remaining_lower &= remaining_lower - 1;  // blsr
      fn(base | bit, &arr[arr_idx]);
      arr_idx++;
    }
    lower_idx++;
  }
}

};
#pragma pack(pop)

#pragma pack(push, 1)
template <typename Traits>
struct _LeafNode {
  typedef _LeafNode<Traits> LeafNode;

  using hash_t = typename Traits::hash_t;
  using uint16_e = typename Traits::uint16_e;
  using uint32_e = typename Traits::uint32_e;
  using uint64_e = typename Traits::uint64_e;
  using offset_e = typename Traits::offset_e;
  static constexpr uint16_t MAX_SIZE = Traits::PAGE_SIZES[Traits::PAGE_SIZES_COUNT - 1];
  static constexpr uint16_t BIG_VALUE_FLAG = uint16_t(1) << 15;

  uint16_e value_size;
  uint8_t key_size;
  hash_t hash;
  uint8_t data[];
  uint8_t* vdata() { return data + key_size; }
  const uint8_t* vdata() const { return data + key_size; }
  Slice key() { return Slice(data, key_size); }
  Slice value() const { return Slice(data + key_size, vsize()); }
  uint16_t vsize() const { return value_size & ~BIG_VALUE_FLAG; }
  uint16_t size() const { return sizeof(LeafNode) + key_size + vsize(); }

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
    assert(sizeof(LeafNode) + key + value <= MAX_SIZE);
    return sizeof(LeafNode) + key + value;
  }

  static uint16_t size(const Slice& key, const Slice& value) {
    return size(key.size(), value.size());
  }
};
#pragma pack(pop)

}  // namespace leaves

#endif  // _LEAVES__NODE_HPP
