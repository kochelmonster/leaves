#ifndef _LEAVES__TRANSFER_TRIE_HPP
#define _LEAVES__TRANSFER_TRIE_HPP

#include <boost/endian/arithmetic.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../core/_node.hpp"
#include "../core/_traits.hpp"
#include "../core/_util.hpp"

namespace leaves {

// _TransferTrie: Simple buffer for serializing trie nodes in wire format.
// Manages buffer growth and node serialization with endian conversion.
// Protocol-agnostic - higher-level classes add headers and framing.
//
// WIRE_HASH_SIZE: Size of hash in wire format (default 0 = no hash)
// WIRE_MAX_KEY_SIZE: Maximum key size for cursor navigation (default 8192)
// Nodes are stored in wire format for cross-platform transfer.
// Source nodes can have different hash sizes - field-by-field copy handles this.
template <size_t WIRE_HASH_SIZE = 0, size_t WIRE_MAX_KEY_SIZE = 8192>
struct _TransferTrie {
  // Wire format traits for nodes in this TransferTrie
  // Uses explicit little-endian types for cross-platform compatibility
  struct Traits {
    typedef uint8_t hash_t[WIRE_HASH_SIZE];
    typedef boost::endian::little_uint32_t uint32_e;
    typedef boost::endian::little_uint16_t uint16_e;
    typedef boost::endian::little_uint64_t uint64_e;
    typedef _Offset<boost::endian::little_uint64_t> offset_e;
  };

  // Wire format node types
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using Offset = typename Traits::offset_e;

  // DBTraits: Traits for navigating DB nodes with _Cursor
  struct DBTraits : public Traits {
    template <typename T, NodeTypes type = TRIE>
    using Pointer = SimplePointer<T, type>;

    static constexpr size_t MAX_KEY_SIZE = WIRE_MAX_KEY_SIZE;
    struct DB;  // Forward declaration
  };

  // DB: Read-only DB adapter for wire format nodes
  // Allows using _Cursor to navigate with relative offsets
  struct DB {
    using Traits = DBTraits;
    using offset_e = typename Traits::offset_e;

    // Resolve offset to node pointer using relative addressing
    template <typename T>
    typename Traits::template Pointer<T> resolve(const offset_e* offset) const {
      if (*offset == 0) {
        return nullptr;  // Null pointer for zero offset
      }
      return typename Traits::template Pointer<T>(offset->template resolve<char>());
    }

    // Overload with Access parameter (ignored for temp DB)
    template <typename T>
    typename Traits::template Pointer<T> resolve(const offset_e* offset, Access) const {
      return resolve<T>(offset);
    }

    // Resolve pointer to offset (for _Dumper compatibility)
    template <typename T>
    offset_e resolve(typename Traits::template Pointer<T> ptr) const {
      return offset_e(reinterpret_cast<uint64_t>(static_cast<T*>(ptr)));
    }

    // Overload for leaf_ptr (SimplePointer with LEAF type)
    template <typename T>
    offset_e resolve(typename Traits::template Pointer<T, LEAF> ptr) const {
      return offset_e(reinterpret_cast<uint64_t>(static_cast<T*>(ptr)));
    }

    // Prefetch is a no-op for temp DB
    void prefetch(const offset_e*, int = 0) const {}
  };

  std::vector<uint8_t>& _buffer;  // External buffer, not owned
  size_t _grow_delta;
  size_t _node_count;

  // Create a _TransferTrie using an external buffer.
  // Buffer should already be reserved to desired initial capacity.
  // grow_delta: amount to grow when full (0 = don't grow, return nullptr)
  explicit _TransferTrie(std::vector<uint8_t>& buffer, size_t grow_delta = 0)
      : _buffer(buffer), _grow_delta(grow_delta), _node_count(0) {}

  // Reset buffer to empty state
  void clear() {
    _buffer.clear();
    _node_count = 0;
  }

  // Add a source trie node to the buffer (converts to wire format)
  // Handles different source/dest hash sizes via field-by-field copy.
  // Returns pointer to copied wire-format node, or nullptr if doesn't fit
  template <typename SrcTrieNode>
  TrieNode* add_trie_node(const SrcTrieNode* src) {
    // Calculate dest size based on wire format, not source size
    // Use src's lower_size/array_size since element sizes (uint32/offset) are same
    uint16_t prefix_size = padding(sizeof(TrieNode) + src->len(), sizeof(typename Traits::uint32_e));
    uint16_t dest_size = align(prefix_size + src->lower_size() + src->array_size());

    uint8_t* dest_ptr = _alloc_node(dest_size);
    if (!dest_ptr) return nullptr;

    auto* dest = (TrieNode*)dest_ptr;

    // Copy or zero-init hash depending on size match
    if constexpr (WIRE_HASH_SIZE > 0) {
      if constexpr (sizeof(src->hash) == WIRE_HASH_SIZE) {
        std::memcpy(dest->hash, src->hash, WIRE_HASH_SIZE);
      } else {
        std::memset(dest->hash, 0, WIRE_HASH_SIZE);
      }
    }

    // Copy fixed fields with endian conversion
    dest->_array_len = static_cast<uint16_t>(src->_array_len);
    dest->_upper = src->_upper;
    dest->_compressed_len = src->_compressed_len;

    // Copy compressed prefix
    std::memcpy(dest->_compressed_data, src->compressed(), src->len());

    // Calculate and set dest offsets (may differ from src due to hash size)
    uint16_t dest_lower_start = padding(sizeof(TrieNode) + dest->_compressed_len, sizeof(typename Traits::uint32_e));
    uint16_t dest_array_start = align(dest_lower_start + src->lower_size());
    dest->_lower_offset = dest_lower_start / sizeof(typename Traits::uint32_e);
    dest->_array_offset = dest_array_start / sizeof(typename Traits::offset_e);

    // Copy lower bitmap with endian conversion
    auto* src_lower = src->lower();
    auto* dest_lower = dest->lower();
    uint16_t lower_count = bits::count(src->_upper);
    for (uint16_t i = 0; i < lower_count; ++i) {
      dest_lower[i] = static_cast<uint32_t>(src_lower[i]);
    }

    // Zero out offset array (will be filled by caller)
    std::memset(dest->array(), 0, src->array_size());
    return dest;
  }

  // Add a source leaf node to the buffer (converts to wire format)
  // Handles different source/dest hash sizes via field-by-field copy.
  // Returns pointer to copied node, or nullptr if doesn't fit
  template <typename SrcLeafNode>
  LeafNode* add_leaf_node(const SrcLeafNode* src) {
    // Calculate dest size using wire format LeafNode::size()
    uint16_t dest_size = LeafNode::size(src->key_size, src->vsize());

    uint8_t* dest_ptr = _alloc_node(dest_size);
    if (!dest_ptr) return nullptr;

    auto* dest = (LeafNode*)dest_ptr;

    // Copy fixed fields with endian conversion
    dest->value_size = static_cast<uint16_t>(src->value_size);  // Preserve BIG_VALUE_FLAG
    dest->key_size = src->key_size;

    // Copy or zero-init hash depending on size match
    if constexpr (WIRE_HASH_SIZE > 0) {
      if constexpr (sizeof(src->hash) == WIRE_HASH_SIZE) {
        std::memcpy(dest->hash, src->hash, WIRE_HASH_SIZE);
      } else {
        std::memset(dest->hash, 0, WIRE_HASH_SIZE);
      }
    }

    // Copy key and value data
    std::memcpy(dest->data, src->data, src->key_size + src->vsize());
    return dest;
  }

  // Allocate space for a node of given size (aligned to 8 bytes)
  // Returns pointer to allocated space, or nullptr if doesn't fit and can't grow.
  uint8_t* _alloc_node(uint16_t size) {
    // Align current position to 8 bytes
    size_t current_pos = _buffer.size();
    size_t aligned_pos = (current_pos + 7) & ~size_t(7);
    size_t required = aligned_pos + size;

    // Grow buffer if needed (capacity check)
    if (required > _buffer.capacity()) {
      if (_grow_delta == 0) {
        return nullptr;  // Can't grow, bail out
      }
      // Grow by grow_delta, ensuring we have enough space
      size_t new_capacity = _buffer.capacity();
      while (new_capacity < required) {
        new_capacity += _grow_delta;
      }
      _buffer.reserve(new_capacity);
    }

    // Add padding bytes if needed
    if (aligned_pos > current_pos) {
      _buffer.resize(aligned_pos, 0);
    }

    size_t offset = _buffer.size();
    _buffer.resize(offset + size);
    ++_node_count;

    return _buffer.data() + offset;
  }

  // Add raw node data with specified type (for tests and raw copying)
  // Nodes are aligned to 8 bytes for relative offset compatibility.
  // Returns pointer to copied node data, or nullptr if doesn't fit and can't grow.
  uint8_t* add_node(NodeTypes type, const uint8_t* data, uint16_t size) {
    (void)type;  // Type stored externally by caller
    uint8_t* dest = _alloc_node(size);
    if (!dest) return nullptr;
    std::memcpy(dest, data, size);
    return dest;
  }

  // Current capacity (from underlying buffer)
  size_t capacity() const { return _buffer.capacity(); }

  // Remaining capacity before growth needed
  size_t remaining_capacity() const {
    return _buffer.capacity() > _buffer.size() ? _buffer.capacity() - _buffer.size() : 0;
  }

  // Current buffer size
  size_t size() const { return _buffer.size(); }

  // Number of nodes added
  size_t node_count() const { return _node_count; }

  // Check if buffer is empty (no nodes added)
  bool empty() const { return _node_count == 0; }

  // Get the raw buffer data
  const uint8_t* data() const { return _buffer.data(); }
  uint8_t* data() { return _buffer.data(); }

  // Direct access to buffer
  std::vector<uint8_t>& buffer() { return _buffer; }
  const std::vector<uint8_t>& buffer() const { return _buffer; }
};

}  // namespace leaves

#endif  // _LEAVES__TRANSFER_TRIE_HPP
