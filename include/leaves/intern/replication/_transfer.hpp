#ifndef _LEAVES__TRANSFER_HPP
#define _LEAVES__TRANSFER_HPP

#include <boost/endian/arithmetic.hpp>
#include <cstdint>
#include <cstring>
#include <list>
#include <random>
#include <unordered_set>
#include <vector>

#include "../core/_node.hpp"
#include "../core/_traits.hpp"
#include "../core/_util.hpp"

namespace leaves {

// Wire format traits for TransferTrie nodes
// Uses explicit little-endian types for cross-platform compatibility
struct WireFormatTraits {
  typedef uint8_t hash_t[HASH_SIZE];
  typedef boost::endian::little_uint32_t uint32_e;
  typedef boost::endian::little_uint16_t uint16_e;
  typedef boost::endian::little_uint64_t uint64_e;
  typedef _Offset<boost::endian::little_uint64_t> offset_e;
};

// Wire format node types for external access (reading node contents)
using WireTrieNode = _TrieNode<WireFormatTraits>;
using WireLeafNode = _LeafNode<WireFormatTraits>;
using WireOffset = WireFormatTraits::offset_e;

// Wire format constants
constexpr uint32_t TRANSFER_MAGIC = 0x4C565354;  // "LVST" little-endian
constexpr uint16_t TRANSFER_VERSION = 0x0001;

// WireTempTraits: Traits for navigating temp DB nodes with _Cursor
// Uses native types (same as storage) since TransferTrie copies raw bytes
// without endian conversion. The temp DB IS in native format.
struct WireTempTraits {
  typedef uint8_t hash_t[HASH_SIZE];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  struct PageHeader {
    uint16_e used;
    uint8_t slot_id;
  };

  template <typename T, NodeTypes type = TRIE>
  using Pointer = SimplePointer<T, type>;
  using ptr = SimplePointer<PageHeader, TRIE>;

  static constexpr size_t MAX_KEY_SIZE = 8192;
  struct DB;  // Forward declaration
};

// WireTempDB: Read-only DB adapter for wire format nodes
// Allows using _Cursor to navigate temp DB with relative offsets
struct WireTempTraits::DB {
  using Traits = WireTempTraits;
  using offset_e = typename Traits::offset_e;

  // Resolve offset to node pointer using relative addressing
  template <typename T>
  Traits::Pointer<T> resolve(const offset_e* offset) const {
    assert(offset != nullptr);
    assert(*offset != 0);

    // Wire format uses relative offsets from the offset's address
    int64_t rel = offset->as_signed();
    uint8_t* addr =
        reinterpret_cast<uint8_t*>(const_cast<offset_e*>(offset)) + rel;
    return Traits::Pointer<T>(addr);
  }

  // Prefetch is a no-op for temp DB
  void prefetch(const offset_e*, int = 0) const {}
};

using WireTempDB = WireTempTraits::DB;

// Database type identifiers
enum class DbType : uint8_t { DB_MAIN = 0x00, DB_DELETION = 0x01 };

// Node type markers in wire format
enum class TransferNodeType : uint8_t { TRIE_NODE = 0x01, LEAF_NODE = 0x02 };

// Wire format header for TransferTrie messages
// All multi-byte fields are little-endian
// Nodes are stored in pre-order DFS (parent before children)
// The root is the FIRST node in the buffer
#pragma pack(push, 1)
struct TransferTrieHeader {
  boost::endian::little_uint32_t magic;
  boost::endian::little_uint16_t version;
  uint8_t db_type;
  uint8_t root_node_type;  // TransferNodeType of root (first node in buffer)
  boost::endian::little_uint32_t node_count;
  boost::endian::little_uint64_t total_size;
  boost::endian::little_uint64_t session_id;
  boost::endian::little_uint64_t snapshot_id;
  boost::endian::little_uint16_t subtrie_path_len;
  // Followed by: subtrie_path bytes (variable), then nodes in post-order

  bool is_valid() const {
    return magic == TRANSFER_MAGIC && version == TRANSFER_VERSION;
  }
};
#pragma pack(pop)

static_assert(sizeof(TransferTrieHeader) == 38,
              "TransferTrieHeader must be 38 bytes");

// Chunk reference for CHUNK_REQ messages
struct ChunkRef {
  boost::endian::little_uint64_t offset;
  uint8_t security_token[32];  // HASH_SIZE
};

// TransferTrie buffer for serializing/deserializing trie nodes
// Used by sender to build transfer messages and receiver to parse them
// SrcTraits: The source database traits (platform-native format)
// Nodes are stored in wire format (WireFormatTraits) for cross-platform
// transfer
template <typename SrcTraits>
struct TransferTrie {
  using SrcTrieNode = _TrieNode<SrcTraits>;
  using SrcLeafNode = _LeafNode<SrcTraits>;
  using TrieNode = WireTrieNode;

  static constexpr size_t DEFAULT_MAX_SIZE = 1024 * 1024;  // 1MB

  std::vector<uint8_t> _buffer;
  size_t _max_size;
  size_t _node_count;
  size_t _header_size;  // Header + subtrie_path length (aligned to 8)
  TransferNodeType _first_node_type;  // Type of first (root) node added
  bool _finalized;
  explicit TransferTrie(size_t max_size = DEFAULT_MAX_SIZE)
      : _max_size(max_size),
        _node_count(0),
        _header_size(0),
        _first_node_type(TransferNodeType::TRIE_NODE),
        _finalized(false) {
    _buffer.reserve(max_size);
  }

  // Generate a random session ID
  static uint64_t generate_session_id() {
    std::random_device rd;
    uint64_t high = rd();
    uint64_t low = rd();
    return (high << 32) | low;
  }

  // Initialize buffer with header for sending
  void begin(uint64_t session_id, uint64_t snapshot_id, DbType db_type,
             const Slice& subtrie_path) {
    _buffer.clear();
    _node_count = 0;
    _finalized = false;

    // Reserve space for header + subtrie_path, aligned to 8 bytes for nodes
    size_t raw_header_size = sizeof(TransferTrieHeader) + subtrie_path.size();
    _header_size = (raw_header_size + 7) & ~size_t(7);  // Align to 8
    _buffer.resize(_header_size, 0);  // Zero-pad alignment bytes

    // Write header (node_count and total_size will be updated in finalize)
    auto* hdr = reinterpret_cast<TransferTrieHeader*>(_buffer.data());
    hdr->magic = TRANSFER_MAGIC;
    hdr->version = TRANSFER_VERSION;
    hdr->db_type = static_cast<uint8_t>(db_type);
    hdr->node_count = 0;
    hdr->total_size = 0;
    hdr->session_id = session_id;
    hdr->snapshot_id = snapshot_id;
    hdr->subtrie_path_len = static_cast<uint16_t>(subtrie_path.size());

    // Write subtrie path
    if (!subtrie_path.empty()) {
      std::memcpy(_buffer.data() + sizeof(TransferTrieHeader),
                  subtrie_path.data(), subtrie_path.size());
    }
  }

  // Add a source trie node to the buffer (converts to wire format)
  // Returns pointer to copied wire-format node, or nullptr if doesn't fit
  const TrieNode* add_trie_node(const SrcTrieNode* node) {
    return reinterpret_cast<const TrieNode*>(
        add_node(TransferNodeType::TRIE_NODE,
                 reinterpret_cast<const uint8_t*>(node), node->size()));
  }

  // Add a source leaf node to the buffer (converts to wire format)
  // Returns pointer to copied data, or nullptr if doesn't fit
  const uint8_t* add_leaf_node(const SrcLeafNode* node) {
    return add_node(TransferNodeType::LEAF_NODE,
                    reinterpret_cast<const uint8_t*>(node), node->size());
  }

  // Add raw node data with specified type
  // Returns pointer to copied node data, or nullptr if doesn't fit
  // Nodes are aligned to 8 bytes for relative offset compatibility
  const uint8_t* add_node(TransferNodeType type, const uint8_t* data,
                          uint16_t size) {
    if (_finalized) return nullptr;

    // Align current position to 8 bytes
    size_t current_pos = _buffer.size();
    size_t aligned_pos = (current_pos + 7) & ~size_t(7);
    size_t padding = aligned_pos - current_pos;

    if (aligned_pos + size > _max_size) {
      return nullptr;
    }

    // Add padding bytes if needed
    if (padding > 0) {
      _buffer.resize(aligned_pos, 0);
    }

    size_t offset = _buffer.size();
    _buffer.resize(offset + size);

    uint8_t* node_dest = _buffer.data() + offset;
    std::memcpy(node_dest, data, size);
    ++_node_count;

    // Track first node type (root is first in pre-order DFS)
    if (_node_count == 1) {
      _first_node_type = type;
    }

    return node_dest;
  }

  // Remaining capacity in bytes
  size_t remaining_capacity() const {
    return _max_size > _buffer.size() ? _max_size - _buffer.size() : 0;
  }

  // Current buffer size
  size_t size() const { return _buffer.size(); }

  // Number of nodes added
  size_t node_count() const { return _node_count; }

  // Check if buffer is empty (no nodes added)
  bool empty() const { return _node_count == 0; }

  // Finalize the buffer, updating header fields
  // Returns the complete buffer as a Slice
  Slice finalize() {
    if (_buffer.empty()) return Slice();

    auto* hdr = reinterpret_cast<TransferTrieHeader*>(_buffer.data());
    hdr->node_count = static_cast<uint32_t>(_node_count);
    hdr->total_size = _buffer.size();
    hdr->root_node_type = static_cast<uint8_t>(_first_node_type);
    _finalized = true;

    return Slice(_buffer.data(), _buffer.size());
  }

  // Get the raw buffer data
  const uint8_t* data() const { return _buffer.data(); }

  // --- Parsing (receiver side) ---

  // Parse header from received data
  // Returns pointer to header in buffer, sets out_subtrie_path slice
  // Returns nullptr if invalid
  static const TransferTrieHeader* parse_header(
      const Slice& data, Slice* out_subtrie_path = nullptr) {
    if (data.size() < sizeof(TransferTrieHeader)) {
      return nullptr;
    }

    const auto* hdr = reinterpret_cast<const TransferTrieHeader*>(data.data());

    if (!hdr->is_valid()) {
      return nullptr;
    }

    size_t path_len = hdr->subtrie_path_len;
    if (data.size() < sizeof(TransferTrieHeader) + path_len) {
      return nullptr;
    }

    if (out_subtrie_path) {
      *out_subtrie_path =
          Slice(data.data() + sizeof(TransferTrieHeader), path_len);
    }

    return hdr;
  }

  // Get pointer to start of node data in received buffer
  // Nodes are aligned to 8 bytes after header + subtrie_path
  static const uint8_t* nodes_data(const Slice& data,
                                   const TransferTrieHeader& header) {
    size_t raw_offset = sizeof(TransferTrieHeader) + header.subtrie_path_len;
    size_t nodes_offset = (raw_offset + 7) & ~size_t(7);  // Align to 8
    return reinterpret_cast<const uint8_t*>(data.data()) + nodes_offset;
  }
};

// Request for children at specific paths
// Wire format: magic(4), session_id(8), db_type(1), path_count(4),
//              [path_len(2), path(var)]...
constexpr uint32_t REQUEST_CHILDREN_MAGIC = 0x4C565352;  // "LVSR"

struct RequestChildrenHeader {
  boost::endian::little_uint32_t magic;
  boost::endian::little_uint64_t session_id;
  uint8_t db_type;
  boost::endian::little_uint32_t path_count;

  bool is_valid() const { return magic == REQUEST_CHILDREN_MAGIC; }
};

static_assert(sizeof(RequestChildrenHeader) == 17,
              "RequestChildrenHeader must be 17 bytes");

// Builder for REQUEST_CHILDREN messages
struct RequestChildrenBuilder {
  std::vector<uint8_t> _buffer;
  void begin(uint64_t session_id, DbType db_type) {
    _buffer.clear();
    _buffer.resize(sizeof(RequestChildrenHeader));

    auto* hdr = reinterpret_cast<RequestChildrenHeader*>(_buffer.data());
    hdr->magic = REQUEST_CHILDREN_MAGIC;
    hdr->session_id = session_id;
    hdr->db_type = static_cast<uint8_t>(db_type);
    hdr->path_count = 0;
  }

  void add_path(const std::string& path) {
    // path_len (2 bytes) + path data
    size_t offset = _buffer.size();
    _buffer.resize(offset + 2 + path.size());

    boost::endian::little_uint16_t path_len =
        static_cast<uint16_t>(path.size());
    std::memcpy(_buffer.data() + offset, &path_len, 2);
    if (!path.empty()) {
      std::memcpy(_buffer.data() + offset + 2, path.data(), path.size());
    }

    // Increment path count
    auto* hdr = reinterpret_cast<RequestChildrenHeader*>(_buffer.data());
    hdr->path_count = hdr->path_count + 1;
  }

  Slice finalize() { return Slice(_buffer.data(), _buffer.size()); }

  size_t size() const { return _buffer.size(); }
  bool empty() const {
    auto* hdr = reinterpret_cast<const RequestChildrenHeader*>(_buffer.data());
    return hdr->path_count == 0;
  }
};

// Iterator over paths in a REQUEST_CHILDREN message
struct RequestChildrenIterator {
  const uint8_t* _data;
  const uint8_t* _end;
  const uint8_t* _current;
  RequestChildrenIterator(const uint8_t* data, size_t size)
      : _data(data), _end(data + size), _current(data) {}

  bool valid() const { return _current + 2 <= _end; }

  uint16_t path_len() const {
    boost::endian::little_uint16_t len;
    std::memcpy(&len, _current, 2);
    return len;
  }

  std::string path() const {
    uint16_t len = path_len();
    std::string result(len, '\0');
    if (len > 0) {
      std::memcpy(result.data(), _current + 2, len);
    }
    return result;
  }

  bool next() {
    if (!valid()) return false;
    _current += 2 + path_len();
    return valid();
  }

  void reset() { _current = _data; }
};

// Parse REQUEST_CHILDREN message
inline bool parse_request_children(const Slice& data,
                                   RequestChildrenHeader* out_header) {
  if (data.size() < sizeof(RequestChildrenHeader)) {
    return false;
  }
  std::memcpy(out_header, data.data(), sizeof(RequestChildrenHeader));
  return out_header->is_valid();
}

// Get iterator over paths in REQUEST_CHILDREN message
inline RequestChildrenIterator request_children_iterator(
    const Slice& data, const RequestChildrenHeader& header) {
  return RequestChildrenIterator(reinterpret_cast<const uint8_t*>(data.data()) +
                                     sizeof(RequestChildrenHeader),
                                 data.size() - sizeof(RequestChildrenHeader));
}

// =============================================================================
// Sender Implementation
// =============================================================================

// Pending trie node awaiting child transmission
struct PendingTrieNode {
  std::string path;    // Path to this node (nibble indices)
  offset_t offset;     // Absolute offset in sender's storage
  uint8_t next_child;  // Next child index to transmit (resume point)

  PendingTrieNode(const std::string& p, offset_t off, uint8_t next = 0)
      : path(p), offset(off), next_child(next) {}
};

// Sender for transferring trie nodes with relative offsets
// Uses pre-order DFS where children set their relative offsets in parent's
// array
template <typename DB>
struct TransferTrieSender {
  using Traits = typename DB::Traits;
  using offset_e = typename Traits::offset_e;
  using SrcOffset = offset_e;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using Transfer = TransferTrie<Traits>;
  using WireOffset = typename WireFormatTraits::offset_e;

  DB* _db;
  typename DB::txn_ptr _txn;
  Transfer _transfer;
  std::list<PendingTrieNode> _pending;  // Nodes awaiting transmission
  std::list<PendingTrieNode>
      _last_batch;  // Nodes from last batch, awaiting ACK
  uint64_t _session_id;
  uint64_t _snapshot_id;
  DbType _db_type;

  TransferTrieSender(DB* db, typename DB::txn_ptr txn,
                     size_t buffer_size = Transfer::DEFAULT_MAX_SIZE)
      : _db(db),
        _txn(txn),
        _transfer(buffer_size),
        _session_id(Transfer::generate_session_id()),
        _snapshot_id(txn->txn_id),
        _db_type(DbType::DB_MAIN) {}

  // Start from root
  void begin(DbType db_type = DbType::DB_MAIN) {
    _db_type = db_type;
    _pending.clear();
    _last_batch.clear();

    if (_txn->root) {
      _pending.emplace_back("", _txn->root, 0);
    }
  }

  // Process ACK: remove matching paths from _last_batch, then merge remaining
  // to _pending
  template <typename Iter>
  void process_ack(Iter iter) {
    // Build set of ACKed paths for O(1) lookup
    std::unordered_set<std::string> acked_paths;
    while (iter.valid()) {
      acked_paths.insert(iter.path());
      iter.next();
    }

    // Remove ACKed nodes from _last_batch
    for (auto it = _last_batch.begin(); it != _last_batch.end();) {
      if (acked_paths.count(it->path)) {
        it = _last_batch.erase(it);
      } else {
        ++it;
      }
    }

    // Splice remaining nodes to end of _pending
    _pending.splice(_pending.end(), _last_batch);
    _last_batch.clear();
  }

  // Check if there are pending nodes to process
  bool has_pending() const { return !_pending.empty() || !_last_batch.empty(); }

  // Check if transfer is complete
  bool is_complete() const { return _pending.empty() && _last_batch.empty(); }

  // Fill buffer with nodes from pending list in BFS order
  // Writes nodes and adds their children to pending for next batch
  // Child offsets are left as 0 - receiver patches them when children arrive
  // Returns true if buffer was filled (may be empty if nothing to send)
  bool fill_buffer() {
    _transfer.begin(_session_id, _snapshot_id, _db_type, Slice());

    while (!_pending.empty()) {
      auto& node = _pending.front();

      if (node.offset.type() == LEAF) {
        // Write leaf node
        auto leaf = _db->template resolve<LeafNode>(&node.offset);
        if (!leaf) {
          _pending.pop_front();
          continue;
        }
        auto* dest = _transfer.add_leaf_node(&*leaf);
        if (!dest) break;  // Buffer full

        // Move to last_batch for ACK tracking
        _last_batch.splice(_last_batch.end(), _pending, _pending.begin());
      } else {
        // Write trie node
        auto trie = _db->template resolve<TrieNode>(&node.offset);
        if (!trie) {
          _pending.pop_front();
          continue;
        }
        auto* dest = _transfer.add_trie_node(&*trie);
        if (!dest) break;  // Buffer full

        // Child offsets are copied as-is (absolute offsets from source DB)
        // These will be zeroed/fixed by receiver - we just need to add children to pending
        const SrcOffset* src_array = trie->array();
        size_t count = trie->count();

        // Zero out child offsets in wire trie (children sent in later batches)
        auto* wire_trie = const_cast<WireTrieNode*>(dest);
        WireOffset* wire_array = wire_trie->array();
        for (size_t i = 0; i < count; ++i) {
          offset_t child_offset(src_array[i]);
          if (child_offset) {
            wire_array[i] = 0;  // Placeholder - receiver patches when child arrives
            // Add child to pending for next batch
            std::string child_path = node.path + static_cast<char>(i);
            _pending.emplace_back(child_path, child_offset, 0);
          }
        }

        // Move to last_batch for ACK tracking
        _last_batch.splice(_last_batch.end(), _pending, _pending.begin());
      }
    }

    return true;
  }

  Slice finalize() { return _transfer.finalize(); }
  uint64_t session_id() const { return _session_id; }
  uint64_t snapshot_id() const { return _snapshot_id; }
};

}  // namespace leaves

#endif  // _LEAVES__TRANSFER_HPP
