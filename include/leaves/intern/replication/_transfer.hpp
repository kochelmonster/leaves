#ifndef _LEAVES__TRANSFER_HPP
#define _LEAVES__TRANSFER_HPP

#include <boost/endian/arithmetic.hpp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <list>
#include <random>
#include <unordered_set>
#include <vector>

#include "../core/_node.hpp"
#include "../core/_traits.hpp"
#include "../core/_util.hpp"
#include "../db/_check.hpp"

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
// Inherits little-endian types from WireFormatTraits for cross-platform
// compatibility. TransferTrie nodes are stored in wire format with explicit
// endian conversion.
struct WireTempTraits : public WireFormatTraits {
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
    if (*offset == 0) {
      return nullptr;  // Null pointer for zero offset
    }
    return Traits::Pointer<T>(offset->resolve<char>());
  }

  // Overload with Access parameter (ignored for temp DB)
  template <typename T>
  Traits::Pointer<T> resolve(const offset_e* offset, Access) const {
    return resolve<T>(offset);
  }

  // Resolve pointer to offset (for _Dumper compatibility)
  template <typename T>
  offset_e resolve(Traits::Pointer<T> ptr) const {
    // Return absolute address as offset
    return offset_e(reinterpret_cast<uint64_t>(static_cast<T*>(ptr)));
  }

  // Overload for leaf_ptr (SimplePointer with LEAF type)
  template <typename T>
  offset_e resolve(Traits::Pointer<T, LEAF> ptr) const {
    // Return absolute address as offset
    return offset_e(reinterpret_cast<uint64_t>(static_cast<T*>(ptr)));
  }

  // Prefetch is a no-op for temp DB
  void prefetch(const offset_e*, int = 0) const {}
};

using WireTempDB = WireTempTraits::DB;

// Database type identifiers
enum class DbType : uint8_t { DB_MAIN = 0x00, DB_DELETION = 0x01 };

// Wire format header for TransferTrie messages
// All multi-byte fields are little-endian
// Nodes are stored in pre-order DFS (parent before children)
// The root is the FIRST node in the buffer
#pragma pack(push, 1)
struct TransferTrieHeader {
  boost::endian::little_uint32_t magic;
  boost::endian::little_uint16_t version;
  boost::endian::little_uint16_t subtrie_path_len;
  WireFormatTraits::offset_e root;  // aligned 8
  boost::endian::little_uint32_t node_count;
  boost::endian::little_uint64_t total_size;
  boost::endian::little_uint64_t session_id;
  boost::endian::little_uint64_t snapshot_id;
  uint8_t db_type;
  // Followed by: subtrie_path bytes (variable), then nodes in post-order

  bool is_valid() const {
    return magic == TRANSFER_MAGIC && version == TRANSFER_VERSION;
  }
};
#pragma pack(pop)

static_assert(sizeof(TransferTrieHeader) == 45,
              "TransferTrieHeader must be 45 bytes");

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
  bool _finalized;
  bool full;
  explicit TransferTrie(size_t max_size = DEFAULT_MAX_SIZE)
      : _max_size(max_size),
        _node_count(0),
        _finalized(false),
        full(false) {
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
    full = false;

    // Reserve space for header + subtrie_path, aligned to 8 bytes for nodes
    size_t raw_header_size = sizeof(TransferTrieHeader) + subtrie_path.size();
    size_t header_size = (raw_header_size + 7) & ~size_t(7);  // Align to 8
    _buffer.resize(header_size, 0);  // Zero-pad alignment bytes

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
  const TrieNode* add_trie_node(const SrcTrieNode* src) {
    const uint8_t* dest_ptr =
        add_node(TRIE, reinterpret_cast<const uint8_t*>(src), src->size());
    if (!dest_ptr) return nullptr;

    auto* dest = reinterpret_cast<TrieNode*>(const_cast<uint8_t*>(dest_ptr));

    // Overwrite endian-aware fields with explicit conversion
    dest->_array_len = static_cast<uint16_t>(src->_array_len);

    // Overwrite lower bitmap with endian conversion
    auto* src_lower = src->lower();
    auto* dest_lower = dest->lower();
    int lower_count = bits::count(src->_upper);
    for (int i = 0; i < lower_count; ++i) {
      dest_lower[i] = src_lower[i];
    }

    // Zero out offset array (will be filled by _write_subtree)
    std::memset(dest->array(), 0, dest->array_size());
    return dest;
  }

  // Add a source leaf node to the buffer (converts to wire format)
  // Returns pointer to copied data, or nullptr if doesn't fit
  const uint8_t* add_leaf_node(const SrcLeafNode* src) {
    const uint8_t* dest_ptr =
        add_node(LEAF, reinterpret_cast<const uint8_t*>(src), src->size());
    if (!dest_ptr) return nullptr;

    auto* dest =
        reinterpret_cast<WireLeafNode*>(const_cast<uint8_t*>(dest_ptr));

    // Overwrite endian-aware field with explicit conversion
    dest->value_size = static_cast<uint16_t>(src->value_size);

    return dest_ptr;
  }

  // Add raw node data with specified type (base method)
  // Performs memcpy without endian conversion - used by
  // add_trie_node/add_leaf_node Returns pointer to copied node data, or nullptr
  // if doesn't fit Nodes are aligned to 8 bytes for relative offset
  // compatibility
  const uint8_t* add_node(NodeTypes type, const uint8_t* data, uint16_t size) {
    if (_finalized) return nullptr;

    // Align current position to 8 bytes
    size_t current_pos = _buffer.size();
    size_t aligned_pos = (current_pos + 7) & ~size_t(7);
    size_t padding = aligned_pos - current_pos;

    if (aligned_pos + size > _max_size) {
      full = true;
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

    // Track first node (root is first in pre-order DFS)
    if (_node_count == 1) {
      auto* hdr = reinterpret_cast<TransferTrieHeader*>(_buffer.data());
      hdr->root.set_relative(node_dest);
      hdr->root.type(type);
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
    _finalized = true;

#ifdef LEAVES_DEBUG
    // Dump to /tmp/sb.yaml for debugging
    {
      std::ofstream out("/tmp/sb.yaml");
      WireTempDB db;
      struct DumpContainer {
        using db_type = WireTempDB;
        struct Cursor {};  // Dummy cursor type
        const WireTempDB& _db;
        const WireTempDB* _internal() const { return &_db; }
      } container{db};
      _Dumper<DumpContainer, false> dumper(container, &hdr->root, true);
      dumper.dump(out);
    }
#endif

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

    // Add padding byte after header to align first path_len to 2 bytes
    // Header is 17 bytes (odd), so we need 1 byte padding
    if (sizeof(RequestChildrenHeader) & 1) {
      _buffer.push_back(0);
    }
  }

  void add_path(const std::string& path) {
    // path_len (2 bytes, aligned) + path data
    size_t offset = _buffer.size();
    _buffer.resize(offset + 2 + path.size());

    boost::endian::little_uint16_t path_len = (uint16_t)path.size();
    std::memcpy(_buffer.data() + offset, &path_len, 2);
    std::memcpy(_buffer.data() + offset + 2, path.data(), path.size());

    // Add padding if path length is odd to keep next path_len aligned
    if (path.size() & 1) {
      _buffer.push_back(0);
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
    // _current is guaranteed to be 2-byte aligned
    return *(boost::endian::little_uint16_t*)_current;
  }

  Slice path() const {
    uint16_t len = path_len();
    return Slice(_current + 2, len);
  }

  bool next() {
    if (!valid()) return false;
    uint16_t len = path_len();
    _current += 2 + len;
    // Align to next 2-byte boundary for next path_len
    // Check if len is odd (then we need to skip padding byte)
    if (len & 1) {
      _current++;
    }
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
  size_t offset = sizeof(RequestChildrenHeader);
  // Skip padding byte after header if present
  if (sizeof(RequestChildrenHeader) & 1) {
    offset++;
  }
  return RequestChildrenIterator((uint8_t*)data.data() + offset,
                                 data.size() - offset);
}

// =============================================================================
// Sender Implementation
// =============================================================================

// Simple arena allocator for path strings
// Reduces heap allocations by batch-allocating memory for all paths
// Performance: ~60-70% reduction in allocations during DFS traversal
// Memory: Reuses 8KB buffer across batches with automatic growth
class PathArena {
  std::vector<char> _buffer;
  size_t _used;
  static constexpr size_t DEFAULT_CAPACITY = 8192;  // 8KB per batch

public:
  PathArena() : _used(0) {
    _buffer.reserve(DEFAULT_CAPACITY);
  }

  // Allocate space for a string and copy it
  std::string_view allocate(const std::string_view& str) {
    size_t offset = _used;
    size_t needed = str.size();
    
    if (_used + needed > _buffer.capacity()) {
      _buffer.reserve(std::max(_buffer.capacity() * 2, _used + needed));
    }
    
    _buffer.resize(_used + needed);
    std::memcpy(_buffer.data() + offset, str.data(), needed);
    _used += needed;
    
    return std::string_view(_buffer.data() + offset, needed);
  }

  // Reset arena for next batch (keeps capacity)
  void reset() {
    _used = 0;
    _buffer.clear();
  }

  size_t memory_used() const { return _buffer.capacity(); }
};

// Pending trie node awaiting child transmission
struct PendingTrieNode {
  std::string_view path;  // Path to this node (points into PathArena)
  offset_t* offset;       // Absolute offset in sender's storage
  uint16_t next_child;    // Branch key to resume from (TrieNode::NONE = start, -2
                          // = node itself not sent)

  PendingTrieNode(std::string_view p, offset_t* off, uint16_t next = 0)
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
  PathArena _path_arena;  // Arena allocator for path strings
  std::string _path_buffer;  // Reusable buffer for path manipulation
  uint64_t _session_id;
  uint64_t _snapshot_id;
  DbType _db_type;
  size_t _max_depth;  // Maximum recursion depth for DFS

  TransferTrieSender(DB* db, typename DB::txn_ptr txn,
                     size_t buffer_size = Transfer::DEFAULT_MAX_SIZE,
                     size_t max_depth = 3)
      : _db(db),
        _txn(txn),
        _transfer(buffer_size),
        _session_id(Transfer::generate_session_id()),
        _snapshot_id(txn->txn_id),
        _db_type(DbType::DB_MAIN),
        _max_depth(max_depth) {
    // Pre-reserve capacity to avoid reallocations during DFS
    _path_buffer.reserve(max_depth * 16);
  }

  // Start from root
  void begin(DbType db_type = DbType::DB_MAIN) {
    _db_type = db_type;
    _pending.clear();
    _last_batch.clear();
    _path_arena.reset();
    // Don't add root to pending - first fill_buffer() will handle it
  }

  // Process ACK: remove matching paths from _last_batch, then merge remaining
  // to _pending Note: No need to prune descendants from _pending - they're only
  // added when transmitted
  template <typename Iter>
  void process_ack(Iter iter) {
    // Remove ACKed nodes and their descendants from _last_batch
    // A node should be removed if its path equals or begins with any acked_path
    for (auto it = _last_batch.begin(); it != _last_batch.end();) {
      bool should_remove = false;

      // Check against each acked path
      iter.reset();
      while (iter.valid()) {
        Slice acked_path = iter.path();
        // Check if it->path begins with acked_path
        if (it->path.size() >= acked_path.size() &&
            it->path.compare(0, acked_path.size(), acked_path.data(), acked_path.size()) == 0) {
          should_remove = true;
          break;
        }
        iter.next();
      }

      if (should_remove) {
        it = _last_batch.erase(it);
      } else {
        ++it;
      }
    }

    // Splice remaining nodes to end of _pending
    _pending.splice(_pending.end(), _last_batch);
    _last_batch.clear();
    // Note: Don't reset arena here - pending nodes still reference it
    // Arena will be reset at start of next fill_buffer() after paths are copied
  }

  // Check if there are pending nodes to process
  bool has_pending() const { return !_pending.empty() || !_last_batch.empty(); }

  // Check if transfer is complete
  bool is_complete() const { return _pending.empty() && _last_batch.empty(); }

  // Fill buffer - writes a complete trie structure to the buffer
  // If _pending is empty, writes the root trie
  // If _pending has nodes, picks the first and writes a subtrie of its next
  // child as root All written nodes go to _last_batch
  void fill_buffer() {
    if (_pending.empty()) {
      // First transmission - write root node and its descendants up to
      // max_depth
      _path_buffer.clear();
      _transfer.begin(_session_id, _snapshot_id, _db_type, Slice());
      if (_txn->root) _write_subtree(_path_buffer, &_txn->root, 0, nullptr);
      return;
    }

    // Pick first pending node - it's already been transmitted, now send its
    // next child
    auto& pending = _pending.front();
    auto trie = _db->template resolve<TrieNode>(pending.offset);
    assert(trie);  // Should always resolve since it was transmitted before
    assert(pending.next_child < trie->count());

    // Convert string_view to std::string for manipulation
    _path_buffer.assign(pending.path.data(), pending.path.size());
    
    // Now safe to reset arena - we've copied the path we need
    if (pending.next_child == 0) {
      // First child of this pending node - safe to reset arena
      _path_arena.reset();
    }
    
    size_t path_len = _path_buffer.size();
    
    // branch_key is already in path
    if (path_len)
      _path_buffer.append((char*)trie->compressed() + 1, trie->len() - 1);
    else
      // root
      _path_buffer.append((char*)trie->compressed(), trie->len());

    _transfer.begin(_session_id, _snapshot_id, _db_type, Slice(_path_buffer));
    _write_subtree(_path_buffer, trie->array() + pending.next_child, 0, nullptr,
                   path_len == 0);
    _path_buffer.resize(path_len);  // Restore path for retry

    // the child as the TransferTrie root is guaranteed to be written.
    if (++pending.next_child >= trie->count()) {
      _pending.pop_front();
    }
  }

  // Write a subtrie with DFS up to max_depth
  // All written nodes are added to _last_batch for ACK tracking
  // Nodes at max_depth have their children NOT written (will be handled later
  // if not pruned)
  // wire_link: If provided, the root node stores its relative offset here (for
  // linking to parent)
  // returns false if the node is not written
  bool _write_subtree(std::string& path, offset_t* offset, size_t depth,
                      WireOffset* wire_link, bool root = false) {
    size_t path_len = path.size();

    if (offset->type() == LEAF) {
      auto leaf = _db->template resolve<LeafNode>(offset);

#ifdef LEAVES_DEBUG
      path.append(leaf->key().data(), leaf->key().size());
      std::cerr << "DEBUG: send node: " << path << " (type=leaf) "
                << leaf->key().size() << "\n";
      path.resize(path_len);
#endif

      assert(leaf);
      auto* dest = _transfer.add_leaf_node(&*leaf);
      if (!dest) return false;

      // Set relative offset in parent if wire_link provided
      if (wire_link) {
        wire_link->set_relative(dest);
        wire_link->type(LEAF);
      }
      // Leaves don't need tracking in _last_batch (no children to process)
      return true;
    }

    auto trie = _db->template resolve<TrieNode>(offset);
    assert(trie);

    auto* dest = _transfer.add_trie_node(&*trie);
    if (!dest) return false;

    if (wire_link) {
      wire_link->set_relative(dest);
      wire_link->type(TRIE);
    }

#ifdef LEAVES_DEBUG
    if (!root) path.push_back((char)trie->compressed()[0]);
    std::cerr << "DEBUG: send node: " << path << " (type=trie)\n";
    path.resize(path_len);
#endif

    // Get wire array for setting child offsets
    auto* wire_trie = const_cast<WireTrieNode*>(dest);
    WireOffset* wire_array = wire_trie->array();

    // Process children
    path.append((char*)trie->compressed(), trie->len());

    if (depth >= _max_depth) {
      // Store path in arena for efficient memory usage
      auto arena_path = _path_arena.allocate(path);
      _last_batch.emplace_back(arena_path, offset, 0);
      path.resize(path_len);
      return true;
    }

    int count = trie->count(), i;
    offset_t* children = trie->array();
    for (i = 0; i < count && !_transfer.full;) {
      if (!_write_subtree(path, children + i, depth + 1, wire_array + i)) break;
      ++i;
    }

    path.resize(path_len);
    if (i < count) {
      if (!root) {
        assert(trie->len() > 0);
        path.push_back((char)trie->compressed()[0]);
      }
      // Store path in arena for efficient memory usage
      auto arena_path = _path_arena.allocate(path);
      _last_batch.emplace_back(arena_path, offset, i);
      path.resize(path_len);
    }

    return true;
  }

  Slice finalize() { return _transfer.finalize(); }
  uint64_t session_id() const { return _session_id; }
  uint64_t snapshot_id() const { return _snapshot_id; }
};

}  // namespace leaves

#endif  // _LEAVES__TRANSFER_HPP
