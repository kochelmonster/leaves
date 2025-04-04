// Trie Nodes
#ifndef _LEAVES__NODE_HPP
#define _LEAVES__NODE_HPP

#include <cstddef>
#include <cstdint>

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

/*
A compressed Trie node (https://www.geeksforgeeks.org/compressed-tries/)
Every node has at least one char in the compressed data (the branch_key of the
parent node) This makes the implmentation of many operations easier.
*/
#pragma pack(1)
template <typename Traits>
struct _TrieNode : public Traits::BlockHeader {
  typedef _TrieNode<Traits> TrieNode;
  using uint32_e = typename Traits::uint32_e;
  using uint16_e = typename Traits::uint16_e;
  using offset_e = typename Traits::offset_e;
  uint8_t _upper;
  uint8_t _compressed_len;
  uint8_t _lower_offset;
  uint8_t _array_offset;
  uint16_e _array_len;  // 256 (chars) + 1 (NULL)
  uint8_t _compressed_data[];

  const static uint16_e NULL_MASK = 0x8000;
  const static int NONE = -1;
  const static int OUT_OF_RANGE = -2;
  const static uint16_t MAX_SIZE =
      align(padding(sizeof(TrieNode) + 256, sizeof(uint32_e)) +
            8 * sizeof(uint32_e)) +
      257 * sizeof(offset_e);
  const static uint8_t LOWER_MASK = 0b00011111;

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
  uint8_t ubit(uint8_t val) const { return (val >> 5); }
  uint32_t lbit(uint8_t val) const { return (val & LOWER_MASK); }
  uint16_t size() const { return array_end(); }

  // estimates the max size for a trie node with a given prefix and branches
  static constexpr uint16_t size(uint8_t prefix, uint16_t branches) {
    return align(padding(sizeof(TrieNode) + prefix, sizeof(uint32_e)) +
                 std::min(branches, (uint16_t)8) * sizeof(uint32_e)) +
           branches * sizeof(offset_e);
  }

  Slice memory() const { return Slice((char*)this, size()); }
  bool has_null() const { return _array_len & NULL_MASK; }

  // create a trie node with a prefix and two keys; returns a the link offset
  // for key2
  uint16_t create(Slice prefix, int key1, offset_t offset1, int key2) {
    _compressed_len = prefix.size();
    memcpy(_compressed_data, prefix.data(), _compressed_len);

    bool swapped = false;
    if (key2 < key1) {
      std::swap(key1, key2);
      swapped = true;
    }

    _array_len = 2;
    uint16_t array_start_,
        lower_start_ =
            padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
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
    offset_e* array_ = (offset_e*)((char*)this + array_start_);
    array_[swapped ? 1 : 0] = offset1;
    return (char*)(swapped ? array_ : array_ + 1) - (char*)this;
  }

  /**
   * @brief Creates and initializes a TrieNode with the given prefix and key.
   *
   * @param prefix A Slice object representing the prefix to be stored in the
   * node.
   * @param key An integer key used to set the upper and lower bitmaps. If the
   * key is NONE, the node is marked as null.
   * @return The offset to the key link: (char*)node + offset ==
   * node->offset(key)
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
    offset_e* array_ = (offset_e*)((char*)this + array_start_);
    return (char*)array_ - (char*)this;
  }

  /**
   * @brief Creates a new TrieNode by merging data from a source node with new a
   * prefix slice.
   *
   * @param src Pointer to the source TrieNode from which data will be copied.
   * @param prefix A Slice object representing the prefix to be compressed and
   * stored.
   *
   */
  void create(const TrieNode& src, Slice prefix) {
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
    memcpy(array(), src.array(), count() * sizeof(offset_e));
  }

  uint16_t create(const TrieNode& src, int key) {
    _compressed_len = src._compressed_len;
    memcpy(_compressed_data, src.compressed(), _compressed_len);

    uint16_t lower_start_ =
        padding(sizeof(TrieNode) + _compressed_len, sizeof(uint32_e));
    uint32_e* lower_ = (uint32_e*)((char*)this + lower_start_);

    _array_len = src._array_len + 1;
    int oidx;
    if (key != NONE) {
      uint8_t bit = ubit(key);
      _upper = src._upper | (1 << bit);
      int lidx = bits::index(_upper, bit);
      if (src._upper & (1 << bit)) {
        memcpy(lower_, src.lower(), bits::count(_upper) * sizeof(uint32_e));
        lower_[lidx] |= 1 << lbit(key);
      } else {
        memcpy(lower_, src.lower(), lidx * sizeof(uint32_e));
        memcpy(lower_ + lidx + 1, src.lower() + lidx,
               (bits::count(_upper) - lidx - 1) * sizeof(uint32_e));
        lower_[lidx] = 1 << lbit(key);
      }

      oidx = bits::count(lower_[lidx] & ((1 << lbit(key)) - 1)) +
             bool(_array_len & NULL_MASK);
      for (int i = 0; i < lidx; i++) oidx += bits::count(lower_[i]);
    } else {
      assert((src._array_len & NULL_MASK) == 0);
      _upper = src._upper;
      _array_len |= NULL_MASK;
      memcpy(lower_, src.lower(), bits::count(_upper) * sizeof(uint32_e));
      oidx = 0;
    }

    uint16_t array_start_ =
        align(lower_start_ + bits::count(_upper) * sizeof(uint32_e));
    _array_offset = array_start_ / sizeof(offset_e);
    _lower_offset = lower_start_ / sizeof(uint32_e);

    offset_e* array_ = (offset_e*)((char*)this + array_start_);
    memcpy(array_, src.array(), oidx * sizeof(offset_e));
    memcpy(array_ + oidx + 1, src.array() + oidx,
           (count() - 1 - oidx) * sizeof(offset_e));
    return (char*)(array_ + oidx) - (char*)this;
  }

  void remove(int nchar) {
    __builtin_prefetch(array());
    if (nchar == NONE) {
      if (has_null()) {
        offset_e* a = array();
        memmove(a, a + 1, array_size() - sizeof(offset_e));
        _array_len &= ~NULL_MASK;
        _array_len--;
      }
      return;
    }

    int lidx = bits::index(_upper, ubit(nchar));
    if (!(_upper & (1 << ubit(nchar)))) {
      return;  // Character not present
    }

    uint32_e* lower_ = lower();
    __builtin_prefetch(lower_);
    if (!(lower_[lidx] & (1 << lbit(nchar)))) {
      return;  // Character not present
    }

    offset_e* array_ = array();
    int oidx = bits::count(lower_[lidx] & ((1 << lbit(nchar)) - 1)) +
               bool(_array_len & NULL_MASK);
    for (int i = 0; i < lidx; i++) {
      oidx += bits::count(lower_[i]);
    }

    lower_[lidx] &= ~(1 << lbit(nchar));
    _array_len--;
    memmove(array() + oidx, array() + oidx + 1,
            (count() - oidx) * sizeof(offset_e));

    if (lower_[lidx] == 0) {
      _upper &= ~(1 << ubit(nchar));
      memmove(&lower_[lidx], &lower_[lidx + 1],
              (bits::count(_upper) - lidx) * sizeof(uint32_e));
    }

    if (align(lower_end()) < array_start()) {
      assert(array_start() == align(lower_end()) + sizeof(offset_e));
      _array_offset--;
      memmove(array(), array() + 1, array_size());
    }
  }

  // check if the index exists
  bool isset(uint8_t nchar) const {
    return (_upper & (1 << ubit(nchar))) &&
           (lower()[bits::index(_upper, ubit(nchar))] & (1 << lbit(nchar)));
  }

  // returns the link for nchar
  const offset_e* offset(int nchar) const {
    if (nchar == NONE) return has_null() ? array() : nullptr;

    uint32_e* lower_ = lower();
    int lidx = bits::index(_upper, ubit(nchar));
    if (_upper & (1 << ubit(nchar))) {
      int oidx = bits::count(lower_[lidx] & ((1 << lbit(nchar)) - 1)) +
                 bool(_array_len & NULL_MASK);
      for (int i = 0; i < lidx; i++) {
        oidx += bits::count(lower_[i]);
      }
      return array() + oidx;
    }

    return nullptr;
  }

  int _prev_lower(int nchar) const {
    int lidx = bits::prev(_upper, ubit(nchar));
    if (lidx < 0) return has_null() ? NONE : OUT_OF_RANGE;
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

  int next(int nchar) {
    if (nchar == NONE) {
      int lidx = bits::first(_upper);
      return (lidx << 5) | bits::first(lower()[0]);
    }
    if (!(_upper & (1 << ubit(nchar)))) return _next_lower(nchar);
    int lidx = bits::index(_upper, ubit(nchar)),
        lbit_idx = bits::next(lower()[lidx], lbit(nchar));
    if (lbit_idx < 0) return _next_lower(nchar);
    return (ubit(nchar) << 5) | lbit_idx;
  }
};

template <typename Traits>
struct _LeafNode : public Traits::BlockHeader {
  typedef _LeafNode<Traits> LeafNode;
  using uint16_e = typename Traits::uint16_e;
  using uint32_e = typename Traits::uint32_e;
  using offset_e = typename Traits::offset_e;
  const static size_t MAX_SIZE = 1024;

  uint16_e value_size;
  uint8_t key_size;
  uint8_t data[];
  uint8_t* vdata() { return data + key_size; }
  Slice key() { return Slice(data, key_size); }
  Slice value() const { return Slice(data + key_size, value_size); }
  uint16_t vsize() const {
    return value_size > MAX_SIZE ? sizeof(offset_e) : value_size;
  }
  uint16_t size() const { return align(sizeof(LeafNode) + key_size + vsize()); }
  Slice memory() { return Slice((char*)this, size()); }
  static uint16_t size(const Slice& key, const Slice& value) {
    if (value.size() > MAX_SIZE) {
      return align(sizeof(LeafNode) + key.size() + sizeof(offset_e));
    }
    return align(sizeof(LeafNode) + key.size() + value.size());
  }
};

#pragma pack(0)

#ifdef DEBUG

inline std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit) && bit != '"' && bit != '<' && bit != '>' && bit != ']' &&
      bit != '\\' && bit != '}' && bit != '{') {
    cstr << bit;
  } else {
    cstr << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
  return cstr.str();
}

template <typename Storage>
struct _Dumper {
  using Traits = typename Storage::Traits;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using offset_e = typename Traits::offset_e;
  using uint16_e = typename Traits::uint16_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;

  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;

  static void dump_leaf(std::ostream& out, offset_e offset, Storage* storage) {
    leaf_ptr leaf = storage->resolve(offset);
    uint16_t size = leaf->size();
    out << "type: leaf" << std::endl;
    out << "id: " << (offset._offset) << std::endl;
    out << "page: " << offset.page() << std::endl;
    out << "freespace: " << BLOCK_SIZES[leaf->slot_id] - size << std::endl;
    out << "size: " << size << std::endl;
    out << "txn: " << leaf->txn_id << std::endl;
    out << "keysize: " << (uint16_t)leaf->key_size << std::endl;
    out << "key: \"";
    for (int i = 0; i < leaf->key_size; i++) {
      out << "[" << bitstr(leaf->data[i]) << "]";
    }
    out << "\"" << std::endl;
    out << "valuesize: " << leaf->value_size << std::endl;
    out << "value: \"";
    int delta = leaf->key_size;
    for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)10);
         i < end; i++) {
      out << "[" << bitstr(leaf->data[i + delta]) << "]";
    }
    out << "\"" << std::endl;

    out << "---" << std::endl;
  }

  static void dump_link(std::ostream& out, offset_t link, Storage* storage) {
    if (link.type() == LEAF)
      dump_leaf(out, link, storage);
    else
      dump_trie(out, link, storage);
  }

  static void dump_trie(std::ostream& out, offset_t offset, Storage* storage) {
    trie_ptr trie = storage->resolve(offset);

    uint16_t size = trie->size();
    out << "type: trie" << std::endl;
    out << "id: " << offset._offset << std::endl;
    out << "page: " << offset.page() << std::endl;
    out << "txn: " << trie->txn_id << std::endl;
    out << "size: " << size << std::endl;
    out << "freespace: " << BLOCK_SIZES[trie->slot_id] - size << std::endl;
    out << "compressed: " << std::endl;
    out << "  size: " << (int)trie->_compressed_len << std::endl;
    out << "  key: \"";
    for (int i = 0; i < trie->_compressed_len; i++) {
      out << "[" << bitstr(trie->compressed()[i]) << "]";
    }
    out << "\"" << std::endl;

    offset_e* start = trie->array();
    offset_e* end = start + trie->count();

    out << "branches: \"";
    for (offset_e* iter = start; iter < end; iter++) {
      if (iter->type() == LEAF) {
        leaf_ptr leaf = storage->resolve(*iter);
        if (leaf->key_size)
          out << "[" << bitstr(leaf->data[0]) << "]";
        else
          out << "[]";
      } else {
        trie_ptr node = storage->resolve(*iter);
        out << "[" << bitstr(node->compressed()[0]) << "]";
      }
    }
    out << "\"" << std::endl;

    out << "children: " << std::endl;
    for (offset_e* iter = start; iter < end; iter++) {
      out << "  - " << iter->_offset << std::endl;
    }

    out << "---" << std::endl;

    for (offset_e* iter = start; iter < end; iter++) {
      dump_link(out, *iter, storage);
    }
  }
};
#endif
}  // namespace leaves

#endif  // _LEAVES__NODE_HPP
