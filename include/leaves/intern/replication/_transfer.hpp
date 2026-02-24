#ifndef _LEAVES__TRANSFER_HPP
#define _LEAVES__TRANSFER_HPP

#include <fstream>
#include <list>
#include <random>
#include <unordered_set>

#include "../core/_hash.hpp"
#include "../db/_check.hpp"
#include "../memory/_bigmemory.hpp"
#include "../util/_hash_updater.hpp"
#include "../util/_transfer_trie.hpp"

namespace leaves {

// Database type identifiers for replication
enum class DbType : uint8_t { DB_MAIN = 0x00, DB_DELETION = 0x01 };

// Wire format constants for replication protocol
constexpr uint32_t REPLICATION_TRANSFER_MAGIC = 0x4C565354;  // "LVST" little-endian
constexpr uint16_t REPLICATION_TRANSFER_VERSION = 0x0001;

// Backward compatibility aliases
constexpr uint32_t TRANSFER_MAGIC = REPLICATION_TRANSFER_MAGIC;
constexpr uint16_t TRANSFER_VERSION = REPLICATION_TRANSFER_VERSION;

// Wire format header for replication transfer messages
// All multi-byte fields are little-endian
// Nodes are stored in pre-order DFS (parent before children)
// The root is the FIRST node in the buffer
#pragma pack(push, 1)
struct ReplicationTransferHeader {
  boost::endian::little_uint32_t magic;
  boost::endian::little_uint16_t version;
  boost::endian::little_uint16_t subtrie_path_len;
  _Offset<boost::endian::little_uint64_t> root;  // aligned 8
  boost::endian::little_uint32_t node_count;
  boost::endian::little_uint64_t total_size;
  boost::endian::little_uint64_t session_id;
  boost::endian::little_uint64_t snapshot_id;
  uint8_t db_type;
  // Followed by: subtrie_path bytes (variable), then nodes in post-order

  bool is_valid() const {
    return magic == REPLICATION_TRANSFER_MAGIC && version == REPLICATION_TRANSFER_VERSION;
  }
};
#pragma pack(pop)

static_assert(sizeof(ReplicationTransferHeader) == 45,
              "ReplicationTransferHeader must be 45 bytes");

// Private base to hold buffer before TransferTrie base initialization
struct ReplicationTransferTrieBufferHolder {
  std::vector<uint8_t> _buffer;
};

// ReplicationTransferTrie: Serializes trie nodes for replication transfer.
// Manages header and protocol framing, inherits node storage from _TransferTrie.
//
// WIRE_MAX_KEY_SIZE: Maximum key size for cursor navigation (default 8192)
template <size_t WIRE_MAX_KEY_SIZE = 8192>
struct ReplicationTransferTrie
    : private ReplicationTransferTrieBufferHolder,
      public _TransferTrie<HASH_SIZE, WIRE_MAX_KEY_SIZE> {
  using Base = _TransferTrie<HASH_SIZE, WIRE_MAX_KEY_SIZE>;
  using TrieNode = typename Base::TrieNode;
  using LeafNode = typename Base::LeafNode;

  static constexpr size_t DEFAULT_MAX_SIZE = 1024 * 1024;  // 1MB

  size_t _header_size;
  bool _finalized;
  bool full;

  explicit ReplicationTransferTrie(size_t max_size = DEFAULT_MAX_SIZE)
      : ReplicationTransferTrieBufferHolder(),
        Base(_buffer, 0),  // grow_delta=0: don't grow, bail out
        _header_size(0), _finalized(false), full(false) {
    _buffer.reserve(max_size);
  }

  // Generate a random session ID
  static uint64_t generate_session_id() {
    std::random_device rd;
    return uint64_t(rd()) | (uint64_t(rd()) << 32);
  }

  // Initialize buffer with header for sending
  void begin(uint64_t session_id, uint64_t snapshot_id, DbType db_type,
             const Slice& subtrie_path) {
    Base::clear();
    _finalized = false;
    full = false;

    // Reserve space for header + subtrie_path, aligned to 8 bytes for nodes
    size_t raw_header_size = sizeof(ReplicationTransferHeader) + subtrie_path.size();
    _header_size = (raw_header_size + 7) & ~size_t(7);  // Align to 8
    _buffer.resize(_header_size, 0);  // Zero-pad alignment bytes

    // Write header (node_count and total_size will be updated in finalize)
    auto* hdr = reinterpret_cast<ReplicationTransferHeader*>(_buffer.data());
    hdr->magic = REPLICATION_TRANSFER_MAGIC;
    hdr->version = REPLICATION_TRANSFER_VERSION;
    hdr->db_type = static_cast<uint8_t>(db_type);
    hdr->node_count = 0;
    hdr->total_size = 0;
    hdr->session_id = session_id;
    hdr->snapshot_id = snapshot_id;
    hdr->subtrie_path_len = static_cast<uint16_t>(subtrie_path.size());

    // Write subtrie path
    if (!subtrie_path.empty()) {
      std::memcpy(_buffer.data() + sizeof(ReplicationTransferHeader),
                  subtrie_path.data(), subtrie_path.size());
    }
  }

  // Add a source trie node to the buffer (converts to wire format)
  // Returns pointer to copied wire-format node, or nullptr if doesn't fit
  template <typename SrcTrieNode>
  TrieNode* add_trie_node(const SrcTrieNode* src) {
    auto* result = Base::add_trie_node(src);
    if (!result) {
      full = true;
      return nullptr;
    }

    // Track first node (root is first in pre-order DFS)
    if (Base::node_count() == 1) {
      auto* hdr = reinterpret_cast<ReplicationTransferHeader*>(_buffer.data());
      hdr->root.set_relative(reinterpret_cast<uint8_t*>(result));
      hdr->root.type(TRIE);
    }
    return result;
  }

  // Add a source leaf node to the buffer (converts to wire format)
  // Returns pointer to copied node, or nullptr if doesn't fit
  template <typename SrcLeafNode>
  LeafNode* add_leaf_node(const SrcLeafNode* src) {
    auto* result = Base::add_leaf_node(src);
    if (!result) {
      full = true;
      return nullptr;
    }

    // Track first node (root is first in pre-order DFS)
    if (Base::node_count() == 1) {
      auto* hdr = reinterpret_cast<ReplicationTransferHeader*>(_buffer.data());
      hdr->root.set_relative(reinterpret_cast<uint8_t*>(result));
      hdr->root.type(LEAF);
    }
    return result;
  }

  // Remaining capacity in bytes
  size_t remaining_capacity() const {
    return _buffer.capacity() > _buffer.size() ? _buffer.capacity() - _buffer.size() : 0;
  }

  // Current buffer size
  size_t size() const { return _buffer.size(); }

  // Number of nodes added
  size_t node_count() const { return Base::node_count(); }

  // Check if buffer is empty (no nodes added)
  bool empty() const { return Base::node_count() == 0; }

  // Finalize the buffer, updating header fields
  // Returns the complete buffer as a Slice
  Slice finalize() {
    if (_buffer.empty()) return Slice();

    auto* hdr = reinterpret_cast<ReplicationTransferHeader*>(_buffer.data());
    hdr->node_count = static_cast<uint32_t>(Base::node_count());
    hdr->total_size = _buffer.size();
    _finalized = true;

#ifdef LEAVES_DEBUG
    // Dump to /tmp/sb-<N>.yaml for debugging
    {
      static int _sb_round = 0;
      std::ofstream out("/tmp/sb-" + std::to_string(_sb_round++) + ".yaml");
      typename Base::DB db;
      struct DumpContainer {
        using db_type = typename Base::DB;
        struct Cursor {};  // Dummy cursor type
        const typename Base::DB& _db;
        const typename Base::DB* _internal() const { return &_db; }
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
  static const ReplicationTransferHeader* parse_header(
      const Slice& data, Slice* out_subtrie_path = nullptr) {
    if (data.size() < sizeof(ReplicationTransferHeader)) {
      return nullptr;
    }

    const auto* hdr = reinterpret_cast<const ReplicationTransferHeader*>(data.data());

    if (!hdr->is_valid()) {
      return nullptr;
    }

    size_t path_len = hdr->subtrie_path_len;
    if (data.size() < sizeof(ReplicationTransferHeader) + path_len) {
      return nullptr;
    }

    if (out_subtrie_path) {
      *out_subtrie_path =
          Slice(data.data() + sizeof(ReplicationTransferHeader), path_len);
    }

    return hdr;
  }

  // Get pointer to start of node data in received buffer
  // Nodes are aligned to 8 bytes after header + subtrie_path
  static const uint8_t* nodes_data(const Slice& data,
                                   const ReplicationTransferHeader& header) {
    size_t raw_offset = sizeof(ReplicationTransferHeader) + header.subtrie_path_len;
    size_t nodes_offset = (raw_offset + 7) & ~size_t(7);  // Align to 8
    return reinterpret_cast<const uint8_t*>(data.data()) + nodes_offset;
  }
};

// Backward compatibility aliases
using TransferTrieHeader = ReplicationTransferHeader;
template <typename Traits>
using TransferTrie = ReplicationTransferTrie<Traits::MAX_KEY_SIZE>;

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

  void add_path(const std::string& path, bool is_leaf = false) {
    // path_len (2 bytes, aligned) + path data
    // Bit 15 of path_len is the leaf flag: set when the pruned node is a
    // NONE-branch leaf (a value stored at the exact path of an interior trie
    // node). A trie node can have both a NONE-branch leaf (value at "sender_7")
    // and non-NONE branches ('8', '9', ...) as children. An ACK for the
    // NONE-branch leaf must NOT cause the sender to discard its deferred
    // continuation for the trie's remaining children.
    size_t offset = _buffer.size();
    _buffer.resize(offset + 2 + path.size());

    uint16_t raw_len = (uint16_t)path.size();
    if (is_leaf) raw_len |= 0x8000u;  // set leaf flag in MSB
    boost::endian::little_uint16_t path_len = raw_len;
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
  bool _error = false;
  RequestChildrenIterator(const uint8_t* data, size_t size)
      : _data(data), _end(data + size), _current(data) {}

  bool valid() const { return !_error && _current + 2 <= _end; }
  bool error() const { return _error; }

  // Raw 16-bit field: bit 15 = leaf flag, bits 0-14 = path length
  uint16_t raw_path_len() const {
    // _current is guaranteed to be 2-byte aligned
    return *(boost::endian::little_uint16_t*)_current;
  }

  uint16_t path_len() const { return raw_path_len() & 0x7FFFu; }

  // True when the pruned node was a NONE-branch leaf (value at a trie node's
  // exact path), not a full trie subtrie match.
  bool is_leaf() const { return (raw_path_len() & 0x8000u) != 0; }

  Slice path() const {
    uint16_t len = path_len();
    if (_current + 2 + len > _end) return Slice();
    return Slice(_current + 2, len);
  }

  bool next() {
    if (!valid()) return false;
    uint16_t len = path_len();
    size_t advance = 2 + len + (len & 1 ? 1 : 0);
    if (_current + advance > _end) { _error = true; return false; }
    _current += advance;
    return valid();
  }

  void reset() { _current = _data; _error = false; }
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
// Memory: Allocates fixed-size blocks; old blocks are never reallocated,
//         so returned string_views remain valid until reset().
class PathArena {
  std::list<std::vector<char>> _blocks;
  size_t _used = 0;
  static constexpr size_t BLOCK_SIZE = 8192;  // 8KB per block

 public:
  PathArena() { _blocks.emplace_back(BLOCK_SIZE); }

  // Allocate space for a string and copy it.
  // The returned string_view is stable until reset() is called.
  std::string_view allocate(const std::string_view& str) {
    if (_used + str.size() > _blocks.back().size()) {
      // Current block is full — allocate a new one (old blocks stay intact)
      _blocks.emplace_back(std::max(BLOCK_SIZE, str.size()));
      _used = 0;
    }
    auto& block = _blocks.back();
    size_t offset = _used;
    std::memcpy(block.data() + offset, str.data(), str.size());
    _used += str.size();
    return std::string_view(block.data() + offset, str.size());
  }

  // Reset arena: free all memory. Only safe when no string_views remain
  // (e.g., when _pending is also cleared).
  void reset() {
    _blocks.clear();
    _blocks.emplace_back(BLOCK_SIZE);
    _used = 0;
  }

  size_t memory_used() const {
    size_t total = 0;
    for (const auto& b : _blocks) total += b.capacity();
    return total;
  }
};

// Pending trie node awaiting child transmission
struct PendingTrieNode {
  std::string_view path;  // Path to this node (points into PathArena)
  offset_t* offset;       // Absolute offset in sender's storage
  uint16_t next_child;  // Branch key to resume from (TrieNode::NONE = start, -2
                        // = node itself not sent)

  PendingTrieNode(std::string_view p, offset_t* off, uint16_t next = 0)
      : path(p), offset(off), next_child(next) {}
};

// Pending big value awaiting transmission after trie sync
struct PendingBigValue {
  offset_t leaf_offset;  // Source leaf offset for resolving data
  uint64_t
      wire_chunk_offset;  // The chunk_offset sent in wire format (for lookup)
  uint32_t value_size;    // Size of the actual value

  PendingBigValue(offset_t off, uint64_t wire_off, uint32_t size)
      : leaf_offset(off), wire_chunk_offset(wire_off), value_size(size) {}
};

// Sender for transferring trie nodes with relative offsets
// Uses pre-order DFS where children set their relative offsets in parent's
// array
// Walks the hash trie structure, but for leaf nodes looks up actual data
// from the data trie via cursor (combining hash + key/value).
template <typename DB>
struct TransferTrieSender {
  using Traits = typename DB::Traits;
  using DataCursorTraits = typename DB::CursorTraits;
  using HashTraits = HashTrieTraits<Traits>;

  using offset_e = typename Traits::offset_e;
  using hash_offset_e = typename HashTraits::offset_e;

  // Data trie node types (for cursor lookup)
  using DataTrieNode = _TrieNode<DataCursorTraits>;
  using DataLeafNode = _LeafNode<DataCursorTraits>;

  // Hash trie node types (for walking)
  using HashTrieNode = _TrieNode<HashTraits>;
  using HashLeafNode = _LeafNode<HashTraits>;

  using Transfer = ReplicationTransferTrie<Traits::MAX_KEY_SIZE>;
  using WireOffset = typename Transfer::Offset;

  // Chunk placeholder for big value data
  struct Chunk {
    char data[1];
  };
  using chunk_ptr = typename Traits::template Pointer<Chunk>;

  // BigValue uses the standalone _BigValue with fixed little-endian layout
  using BigValue = _BigValue;

  DB* _db;
  typename DB::txn_ptr _txn;
  typename DB::cursor_ptr _cursor;  // Reusable cursor for data trie lookup
  Transfer _transfer;
  std::list<PendingTrieNode> _pending;  // Nodes awaiting transmission
  std::list<PendingTrieNode>
      _last_batch;  // Nodes from last batch, awaiting ACK
  std::vector<PendingBigValue>
      _pending_big_values;  // Big values to send after trie sync
  std::vector<PendingBigValue>
      _last_big_values;      // Big values from last batch, awaiting ACK
  PathArena _path_arena;     // Arena allocator for path strings
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
        _snapshot_id(txn ? txn->txn_id : tid_t(0)),
        _db_type(DbType::DB_MAIN),
        _max_depth(max_depth) {
    // Pre-reserve capacity to avoid reallocations during DFS
    _path_buffer.reserve(max_depth * 16);
  }

  // Start from root
  void begin(DbType db_type = DbType::DB_MAIN) {
    _db_type = db_type;
    _snapshot_id = _txn->txn_id;  // Update snapshot for potentially new txn
    _pending.clear();
    _last_batch.clear();
    _pending_big_values.clear();
    _last_big_values.clear();
    _path_arena.reset();

    // Create cursor on the appropriate data trie root
    _cursor = _db->create_cursor();
    if (_db_type == DbType::DB_DELETION) {
      // Set cursor root to deletion trie for deletion database
      _cursor->set_root(&_txn->deletion_root);
    } else {
      _cursor->set_root(&_txn->root);
    }
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
        bool ack_is_leaf = iter.is_leaf();
        // Descendants of acked_path are always prunable.
        if (it->path.size() > acked_path.size() &&
            it->path.compare(0, acked_path.size(), acked_path.data(),
                             acked_path.size()) == 0) {
          should_remove = true;
          break;
        }
        // Exact path match: only prune if it's a trie ACK.
        // A NONE-branch leaf ACK (is_leaf=true) means the receiver already has
        // the value stored at this trie node's exact path, but the trie may
        // still have unset non-NONE branches whose deferred continuation must
        // be kept.
        if (!ack_is_leaf && it->path.size() == acked_path.size() &&
            it->path.compare(0, acked_path.size(), acked_path.data(),
                             acked_path.size()) == 0) {
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

    // Move non-pruned big values to pending
    // Note: Big values are identified by wire_chunk_offset - pruning is done
    // when the containing leaf's subtrie is pruned (hash matched)
    _pending_big_values.insert(_pending_big_values.end(),
                               _last_big_values.begin(),
                               _last_big_values.end());
    _last_big_values.clear();

    // Note: Don't reset arena here - pending nodes still reference it
    // Arena is reset in begin() when all pending lists are cleared
  }

  // Check if there are pending nodes to process
  bool has_pending() const { return !_pending.empty() || !_last_batch.empty(); }

  // Check if there are pending big values to send
  bool has_pending_big_values() const {
    return !_pending_big_values.empty() || !_last_big_values.empty();
  }

  // Check if transfer is complete (trie + big values)
  bool is_complete() const {
    return _pending.empty() && _last_batch.empty() &&
           _pending_big_values.empty() && _last_big_values.empty();
  }

  // Get pending big values for transmission
  const std::vector<PendingBigValue>& pending_big_values() const {
    return _pending_big_values;
  }

  // Clear pending big values after they've been sent and ACKed
  void clear_pending_big_values() { _pending_big_values.clear(); }

  // Resolve big value data from leaf offset
  // Returns pointer to actual value data and size
  std::pair<const uint8_t*, uint32_t> resolve_big_value(
      const PendingBigValue& bv) {
    offset_t off = bv.leaf_offset;
    auto leaf = _db->template resolve<DataLeafNode>(&off);
    BigValue* big_val = (BigValue*)leaf->vdata();

    // Resolve the chunk pointer
    offset_t chunk_offset(big_val->chunk_offset);
    chunk_ptr chunk = _db->template resolve<Chunk>(&chunk_offset, READ);
    const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(&*chunk);
    return {data_ptr, bv.value_size};
  }

  // Fill buffer - writes a complete trie structure to the buffer
  // If _pending is empty, writes the root trie
  // If _pending has nodes, picks the first and writes a subtrie of its next
  // child as root All written nodes go to _last_batch
  // Walks the hash trie structure, looking up data via cursor for leaves.
  void fill_buffer() {
    if (_pending.empty()) {
      // First transmission - write root node and its descendants up to
      // max_depth
      _path_buffer.clear();
      _transfer.begin(_session_id, _snapshot_id, _db_type, Slice());

      // Select hash root based on db_type
      hash_offset_e* hash_root = _db->hash_root_ptr();
      if (_db_type == DbType::DB_DELETION) {
        hash_root = _db->deletion_hash_root_ptr();
      }

      if (*hash_root)
        _write_subtree(_path_buffer, hash_root, 0, nullptr, true);
      return;
    }

    // Pick last pending node (LIFO) so that later-inserted nodes (higher
    // branch-key subtries, e.g. 'h' before 'e') are sent first. This ensures
    // that subtries which share a common prefix with the receiver's local trie
    // (and are thus prunable) are sent before lower-priority subtries.
    auto& pending = _pending.back();
    auto hash_trie = _db->template resolve<HashTrieNode>(pending.offset);
    assert(hash_trie);  // Should always resolve since it was transmitted before
    assert(pending.next_child < hash_trie->count());

    // Convert string_view to std::string for manipulation
    _path_buffer.assign(pending.path.data(), pending.path.size());

    size_t path_len = _path_buffer.size();

    // branch_key is already in path
    if (path_len)
      _path_buffer.append((char*)hash_trie->compressed() + 1, hash_trie->len() - 1);
    else
      // root
      _path_buffer.append((char*)hash_trie->compressed(), hash_trie->len());

    // When pending is a root-level deferred node (path_len == 0), the
    // _path_buffer contains only the root's own compressed prefix (e.g., "s"
    // when all keys share that prefix, or "" for a true empty root).
    // But the wire root being sent is actually a CHILD of the root node, so
    // its true global position is root_compressed + child_compressed.
    // Using just _path_buffer ("s") as the subtrie_path would mislead the
    // receiver into hashing against the 's' root entry instead of the child's
    // actual entry (e.g., "sender_"), causing all child leaves to appear as
    // new (local_hash=null) every round.
    //
    // Fix: when path_len == 0, peek at the child being sent and append its
    // compressed bytes to subtrie_header_path, giving the receiver the correct
    // full path to the wire root.
    //
    // The internal _path_buffer passed to _write_subtree must remain at
    // root_compressed (path_len==0 path) so the child's compressed is appended
    // only once inside _write_subtree (it would double-count otherwise).
    std::string subtrie_header_path = _path_buffer;
    if (path_len == 0) {
      auto* child_offset = hash_trie->array() + pending.next_child;
      if (child_offset->type() == TRIE) {
        auto child_trie = _db->template resolve<HashTrieNode>(child_offset);
        if (child_trie && child_trie->len() > 0)
          subtrie_header_path.append((char*)child_trie->compressed(), child_trie->len());
      } else {
        auto child_leaf = _db->template resolve<HashLeafNode>(child_offset);
        if (child_leaf && child_leaf->key_size > 0)
          subtrie_header_path.push_back((char)child_leaf->data[0]);
      }
    }

    _transfer.begin(_session_id, _snapshot_id, _db_type, Slice(subtrie_header_path));
    // Note: root=false because we're writing a CHILD of the pending node, not
    // the root. The leaf case in _write_subtree handles branch-char push/pop.
    _write_subtree(_path_buffer, hash_trie->array() + pending.next_child, 0,
                   nullptr, false);
    _path_buffer.resize(path_len);  // Restore path for retry

    // the child as the TransferTrie root is guaranteed to be written.
    if (++pending.next_child >= hash_trie->count()) {
      _pending.pop_back();
    }
  }

  // Write a subtrie with DFS up to max_depth
  // Walks the hash trie structure, looking up data via cursor for leaves.
  // All written nodes are added to _last_batch for ACK tracking
  // Nodes at max_depth have their children NOT written (will be handled later
  // if not pruned)
  // wire_link: If provided, the root node stores its relative offset here (for
  // linking to parent)
  // returns false if the node is not written
  bool _write_subtree(std::string& path, hash_offset_e* hash_offset, size_t depth,
                      WireOffset* wire_link, bool root = false) {
    size_t path_len = path.size();

    if (hash_offset->type() == LEAF) {
      // Hash leaf: need to combine hash + data from data trie
      auto hash_leaf = _db->template resolve<HashLeafNode>(hash_offset);
      assert(hash_leaf);

      // Hash leaves store their branch char in data[0] (key_size == 1).
      // NONE-branch leaves have key_size == 0 and path is already the full key.
      if (hash_leaf->key_size > 0) path.push_back((char)hash_leaf->data[0]);

      // Navigate cursor to the data leaf at this trie position.
      // Hash leaves do not store the full key — path is only a prefix of the
      // actual data key. find() lands on the correct leaf (is_leaf()==true)
      // without an exact match (is_valid()==false).
      _cursor->find(Slice(path.data(), path.size()));
      assert(_cursor->stack.size && _cursor->stack.back().is_leaf() &&
             "hash trie entry missing from data trie - corruption");
      assert(_cursor->key().size() >= path.size() &&
             memcmp(_cursor->key().data(), path.data(), path.size()) == 0 &&
             "cursor landed on wrong leaf - hash/data trie mismatch");

      // Get the data leaf from cursor's stack
      auto& data_trans = _cursor->stack.back();
      auto& data_leaf = *data_trans.leaf();

      // Add data leaf to wire buffer (copies key/value, zeroes hash)
      auto* dest = _transfer.add_leaf_node(&data_leaf);
      if (!dest) {
        path.resize(path_len);
        return false;
      }

      // Copy hash from hash trie leaf into wire leaf
      std::memcpy(dest->hash, hash_leaf->hash, HASH_SIZE);

      // Track big values for later transmission
      if (data_leaf.is_big()) {
        BigValue* bv = (BigValue*)data_leaf.vdata();
        _last_big_values.emplace_back(*data_trans.offset, bv->chunk_offset,
                                      bv->value_size);
      }

      // Set relative offset in parent if wire_link provided
      if (wire_link) {
        wire_link->set_relative(dest);
        wire_link->type(LEAF);
      }
      // Leaves don't need tracking in _last_batch (no children to process)
      path.resize(path_len);
      return true;
    }

    // Hash trie node: add directly (hash is in the node)
    auto hash_trie = _db->template resolve<HashTrieNode>(hash_offset);
    assert(hash_trie);

    auto* dest = _transfer.add_trie_node(&*hash_trie);
    if (!dest) return false;

    if (wire_link) {
      wire_link->set_relative(dest);
      wire_link->type(TRIE);
    }

    // Get wire array for setting child offsets
    auto* wire_trie = const_cast<typename Transfer::TrieNode*>(dest);
    WireOffset* wire_array = wire_trie->array();

    // Process children
    path.append((char*)hash_trie->compressed(), hash_trie->len());

    if (depth >= _max_depth) {
      // Undo the full compressed append — fill_buffer() expects only
      // the branch key (compressed[0]) and will itself append the rest.
      path.resize(path_len);
      if (!root) {
        assert(hash_trie->len() > 0);
        path.push_back((char)hash_trie->compressed()[0]);
      }
      // Store path in arena for efficient memory usage
      auto arena_path = _path_arena.allocate(path);
      _last_batch.emplace_back(arena_path, hash_offset, 0);
      path.resize(path_len);
      return true;
    }

    int count = hash_trie->count(), i;
    hash_offset_e* children = hash_trie->array();
    // Hash leaves carry their branch char in data[0] (key_size > 0);
    // _write_subtree pushes it before find() and restores path on return.
    for (i = 0; i < count && !_transfer.full;) {
      if (!_write_subtree(path, children + i, depth + 1, wire_array + i)) break;
      ++i;
    }

    path.resize(path_len);
    if (i < count) {
      if (!root) {
        assert(hash_trie->len() > 0);
        path.push_back((char)hash_trie->compressed()[0]);
      }
      // Store path in arena for efficient memory usage
      auto arena_path = _path_arena.allocate(path);
      _last_batch.emplace_back(arena_path, hash_offset, i);
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
