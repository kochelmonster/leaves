// Trie Nodes
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
template <typename Traits>
struct _TrieNode {
  typedef _TrieNode<Traits> TrieNode;
  using hash_t = typename Traits::hash_t;
  using uint32_e = typename Traits::uint32_e;
  using uint16_e = typename Traits::uint16_e;
  using offset_e = typename Traits::offset_e;
  static constexpr uint16_t MAX_BRANCH_COUNT = 257;  // 256 (chars) + 1 (NULL)
  uint8_t _upper;
  uint8_t _compressed_len;
  uint8_t _lower_offset;
  uint8_t _array_offset;
  uint16_e _array_len;  // < MAX_BRANCH_COUNT
  hash_t hash;
  uint8_t _compressed_data[];

  constexpr static uint16_e NULL_MASK = 0x8000;
  constexpr static int NONE = -1;
  constexpr static int OUT_OF_RANGE = -2;
  constexpr static uint16_t MAX_SIZE =
      align(padding(sizeof(TrieNode) + MAX_BRANCH_COUNT, sizeof(uint32_e)) +
            8 * sizeof(uint32_e)) +
      257 * sizeof(offset_e);
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

  uint16_t calc_lower_start() const {
    return padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
  }

  uint16_t calc_array_start() const {
    return align(lower_start() + bits::count(_upper) * sizeof(uint32_e));
  }

  // estimates the max size for a trie node with a given prefix and branches
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
    _compressed_len = prefix.size();
    memcpy(_compressed_data, prefix.data(), _compressed_len);

    uint16_t idx0 = 0, idx1 = 1;
    std::pair<uint16_t, uint16_t> result(0, 1);
    if (key2 < key1) {
      std::swap(key1, key2);
      std::swap(result.first, result.second);
    }

    _array_len = 2;
    uint16_t array_start_, lower_start_ = calc_lower_start();
    uint32_e* lower_ = (uint32_e*)((char*)this + lower_start_);

    if (key1 != NONE) {
      _upper = (1 << ubit(key1)) | (1 << ubit(key2));
      if (bits::count(_upper) == 1)
        lower_[0] = (1 << lbit(key1)) | (1 << lbit(key2));
      else {
        lower_[0] = 1 << lbit(key1);
        lower_[1] = 1 << lbit(key2);
      }
      array_start_ = align(lower_start_ + 2 * sizeof(uint32_e));
    } else {
      _upper = 1 << ubit(key2);
      _array_len |= NULL_MASK;
      lower_[0] = 1 << lbit(key2);
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
      _upper = 1 << ubit(key);
      lower_[0] = 1 << lbit(key);
      array_start_ = align(lower_start_ + sizeof(uint32_e));
    } else {
      _upper = 0;
      array_start_ = align(lower_start_);
      _array_len |= NULL_MASK;
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
      _upper |= (1 << bit);
      int lidx = bits::index(_upper, bit);
      if (src._upper & (1 << bit)) {
        // _upper == src._upper
        memcpy(lower_, src.lower(), bits::count(_upper) * sizeof(uint32_e));
        lower_[lidx] |= 1 << lbit(key);
      } else {
        // bits::count(_upper) == bits::count(src._upper) + 1
        memcpy(lower_, src.lower(), lidx * sizeof(uint32_e));
        memcpy(lower_ + lidx + 1, src.lower() + lidx,
               (bits::count(src._upper) - lidx) * sizeof(uint32_e));
        lower_[lidx] = 1 << lbit(key);
      }

      oidx = bits::count(lower_[lidx] & ((1 << lbit(key)) - 1)) +
             bool(_array_len & NULL_MASK);
      for (int i = 0; i < lidx; i++) oidx += bits::count(lower_[i]);
    } else {
      assert((src._array_len & NULL_MASK) == 0);
      _array_len |= NULL_MASK;
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
      int lidx = bits::index(_upper, bit);

      memcpy(lower_, src.lower(), bits::count(src._upper) * sizeof(uint32_e));
      oidx = bits::count(lower_[lidx] & ((1 << lbit(key)) - 1)) +
             bool(_array_len & NULL_MASK);
      for (int i = 0; i < lidx; i++) oidx += bits::count(lower_[i]);

      lower_[lidx] &= ~(1 << lbit(key));
      if (!lower_[lidx]) {
        _upper &= ~(1 << bit);
        memmove(&lower_[lidx], &lower_[lidx + 1],
                (bits::count(_upper) - lidx) * sizeof(uint32_e));
      }
    } else {
      assert(src._array_len & NULL_MASK);
      _array_len &= ~NULL_MASK;
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
  void create(const Slice& prefix, offset_e* offsets) {
    assert(prefix.size() < 256);
    _upper = 0;
    _compressed_len = prefix.size();
    memcpy(_compressed_data, prefix.data(), _compressed_len);
    _lower_offset = calc_lower_start() / sizeof(uint32_e);
    _array_offset = calc_array_start() / sizeof(offset_e);

    uint16_t j = 0;
    offset_e* array_ = array();
    uint32_e* lower_ = lower();
    memset(lower_, 0, lower_size());
    if (offsets[NONE]) {
      _array_len = 1 | TrieNode::NULL_MASK;
      *array_++ = offsets[NONE];
    } else
      _array_len = 0;

    for (int i = 0; i < 256; i++) {
      if (offsets[i]) {
        _array_len++;
        *array_++ = offsets[i];
        _upper |= (1 << ubit(i));
        lower_[bits::index(_upper, ubit(i))] |= 1 << lbit(i);
      }
    }
  }

  /**
   * @brief Creates a unified TrieNode by merging children from two source
   * nodes.
   *
   * This method creates a new TrieNode that contains all children present in
   * either src or dst (or both). The caller provides arrays where child
   * offset pointers will be stored, allowing determination of which child
   * offsets to use or merge.
   *
   * @param dst Pointer to the destination source TrieNode.
   * @param src Pointer to the source TrieNode.
   * @param dst_child Array of offset_e pointers to be filled with child offset
   *                  pointers from dst node (nullptr if child doesn't exist).
   * @param src_child Array of offset_e pointers to be filled with child offset
   *                  pointers from src node (nullptr if child doesn't exist).
   *                  Both arrays are indexed by unified children's position
   *                  (NONE branch at index 0 if present, followed by byte
   *                  values 0-255 in order).
   *
   * @note Both src and dst must have the same compressed prefix length and
   * content.
   */
  template <typename TNode1, typename TNode2>
  void create(TNode1& dst, TNode2& src,
              const typename TNode1::offset_e* dst_child[257],
              const typename TNode2::offset_e* src_child[257]) {
    assert(src._compressed_len == dst._compressed_len);
    assert(memcmp(src._compressed_data, dst._compressed_data,
                  src._compressed_len) == 0);

    // unify the children of src and dst
    // and fill the provenance array with the corresponding offsets

    _upper = 0;
    _compressed_len = src._compressed_len;
    memcpy(_compressed_data, src.compressed(), _compressed_len);

    uint16_t idx = 0;  // index into provenance array

    // Handle NONE branch
    bool src_has_none = src.has_none();
    bool dst_has_none = dst.has_none();
    if (src_has_none || dst_has_none) {
      _array_len = 1 | TrieNode::NULL_MASK;
      src_child[idx] = src_has_none ? src.offset(NONE) : nullptr;
      dst_child[idx] = dst_has_none ? dst.offset(NONE) : nullptr;
      idx++;
    } else {
      _array_len = 0;
    }

    // Iterate through all byte values and build _upper bitmap
    uint8_t unified_children[256];
    uint16_t child_count = 0;

    for (int i = 0; i < 256; i++) {
      bool src_has = src.isset(i);
      bool dst_has = dst.isset(i);

      if (src_has || dst_has) {
        _array_len++;
        _upper |= (1 << ubit(i));
        unified_children[child_count++] = i;
        src_child[idx] = src_has ? src.offset(i) : nullptr;
        dst_child[idx] = dst_has ? dst.offset(i) : nullptr;
        idx++;
      }
    }

    // Now that _upper is set, calculate offsets and build lower bitmap
    uint16_t lower_start_ = calc_lower_start();
    _lower_offset = lower_start_ / sizeof(uint32_e);
    _array_offset =
        align(lower_start_ + bits::count(_upper) * sizeof(uint32_e)) /
        sizeof(offset_e);

    uint32_e* lower_ = lower();
    memset(lower_, 0, bits::count(_upper) * sizeof(uint32_e));
    for (uint16_t j = 0; j < child_count; j++) {
      int i = unified_children[j];
      int lidx = bits::index(_upper, ubit(i));
      lower_[lidx] |= 1 << lbit(i);
    }

    assert(idx == count());
  }

  // check if the index exists
  bool isset(int nchar) const {
    if (nchar == NONE) return has_none();
    return (_upper & (1 << ubit(nchar))) &&
           (lower()[bits::index(_upper, ubit(nchar))] & (1 << lbit(nchar)));
  }

  int array_index(int nchar) const {
    if (nchar == NONE) return has_none() ? 0 : -1;

    uint32_e* lower_ = lower();
    int lidx = bits::index(_upper, ubit(nchar));
    if (_upper & (1 << ubit(nchar))) {
      int oidx = bits::count(lower_[lidx] & ((1 << lbit(nchar)) - 1)) +
                 bool(_array_len & NULL_MASK);
      for (int i = 0; i < lidx; i++) {
        oidx += bits::count(lower_[i]);
      }
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
    if (!(_upper & (1 << ubit(nchar)))) return _prev_lower(nchar);

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
    if (!(_upper & (1 << ubit(nchar)))) return _next_lower(nchar);
    int lidx = bits::index(_upper, ubit(nchar)),
        lbit_idx = bits::next(lower()[lidx], lbit(nchar));
    if (lbit_idx < 0) return _next_lower(nchar);
    return (ubit(nchar) << 5) | lbit_idx;
  }

  int first() const {
    if (has_none()) return NONE;
    return (bits::first(_upper) << 5) | bits::first(lower()[0]);
  }
};

template <typename Traits>
struct _LeafNode {
  typedef _LeafNode<Traits> LeafNode;

  using hash_t = typename Traits::hash_t;
  using uint16_e = typename Traits::uint16_e;
  using uint32_e = typename Traits::uint32_e;
  using uint64_e = typename Traits::uint64_e;
  using offset_e = typename Traits::offset_e;
  static constexpr auto& PAGE_SIZES = Traits::PAGE_SIZES;
  static constexpr auto BS_COUNT = Traits::PAGE_SIZES_COUNT;
  static constexpr auto MAX_SIZE = PAGE_SIZES[BS_COUNT - 1];
  static constexpr auto BIG_VALUE_FLAG = uint16_e(1) << 15;

  uint8_t key_size;
  uint16_e value_size;
  hash_t hash;
  uint8_t data[];
  uint8_t* vdata() { return data + key_size; }
  const uint8_t* vdata() const { return data + key_size; }
  Slice key() { return Slice(data, key_size); }
  Slice value() const { return Slice(data + key_size, value_size); }
  uint16_t vsize() const { return value_size & ~BIG_VALUE_FLAG; }
  uint16_t size() const { return sizeof(LeafNode) + key_size + vsize(); }

  void set_big() { value_size |= BIG_VALUE_FLAG; }

  void set(const Slice& key, size_t value_size_) {
    key_size = key.size();
    memcpy(data, key.data(), key.size());
    value_size = value_size_;
  }

  Slice memory() { return Slice((char*)this, size()); }

  bool is_big() const {
    return (value_size & BIG_VALUE_FLAG) == BIG_VALUE_FLAG;
  }

  // TODO: needed?
  static uint16_t size(uint16_t key, size_t value) {
    return sizeof(LeafNode) + key + value;
  }

  static uint16_t size(const Slice& key, const Slice& value) {
    return size(key.size(), value.size());
  }
};

}  // namespace leaves

#endif  // _LEAVES__NODE_HPP
