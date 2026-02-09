#ifndef _LEAVES__REPLICATION_FSM_HPP
#define _LEAVES__REPLICATION_FSM_HPP

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../db/_cursor.hpp"
#include "../util/_merger.hpp"
#include "_transfer.hpp"

namespace leaves {

// =============================================================================
// Message Envelope
// =============================================================================

// Message types for replication protocol
enum class ReplicationMsgType : uint8_t {
  TRIE_DATA = 0x01,  // TransferTrie buffer (sender → receiver)
  SUBTRIE_ACK =
      0x02,  // ACK subtries at paths - sender can skip (receiver → sender)
  COMPLETE = 0x03,           // Replication complete (either side)
  ERROR = 0x04,              // Error occurred (either side)
  FRACTION_COMPLETE = 0x05,  // Fraction merged, restart from root
                             // (receiver → sender)
};

constexpr uint32_t REPLICATION_MSG_MAGIC = 0x4C565250;  // "LVRP"

// Message envelope wrapping all replication messages
#pragma pack(push, 1)
struct ReplicationMsgHeader {
  boost::endian::little_uint32_t magic;
  uint8_t msg_type;  // ReplicationMsgType
  boost::endian::little_uint64_t session_id;
  boost::endian::little_uint32_t payload_size;
  uint8_t reserved[7];  // Padding to 24 bytes for 8-byte payload alignment
  // Followed by: payload bytes

  bool is_valid() const { return magic == REPLICATION_MSG_MAGIC; }
};
#pragma pack(pop)

static_assert(sizeof(ReplicationMsgHeader) == 24,
              "ReplicationMsgHeader must be 24 bytes");

// Error codes for ERROR messages
enum class ReplicationError : uint8_t {
  NONE = 0x00,
  SESSION_MISMATCH = 0x01,
  INVALID_MESSAGE = 0x02,
  INVALID_STATE = 0x03,
  INTERNAL_ERROR = 0x04
};

// =============================================================================
// Receive Buffer for Zero-Copy Message Reception
// =============================================================================

// Buffer that tracks partial message reception
// The FSM allocates this buffer and provides it to the transport layer
// Transport writes directly into it, avoiding copies
struct ReceiveBuffer {
  uint8_t* _data;
  size_t _capacity;
  size_t _received;  // How much data has been received so far
  size_t _expected;  // Expected total size (from header, once known)

  ReceiveBuffer() : _data(nullptr), _capacity(0), _received(0), _expected(0) {}

  void init(uint8_t* data, size_t capacity) {
    _data = data;
    _capacity = capacity;
    _received = 0;
    _expected = 0;
  }

  // Returns pointer where transport should write next chunk
  uint8_t* write_ptr() { return _data + _received; }

  // How much space is available for writing
  size_t available() const { return _capacity - _received; }

  // How much more data is needed to complete the message
  // Returns 0 if we don't know expected size yet (need header first)
  size_t remaining() const {
    if (_expected == 0) {
      // Need at least header to know expected size
      if (_received < sizeof(ReplicationMsgHeader)) {
        return sizeof(ReplicationMsgHeader) - _received;
      }
      return 0;  // Have header but expected not set - caller should call
                 // parse_expected()
    }
    return _expected > _received ? _expected - _received : 0;
  }

  // Called by transport after writing data
  void advance(size_t bytes) { _received += bytes; }

  // Parse expected size from header once we have enough data
  // Returns true if successful, false if not enough data yet
  bool parse_expected() {
    if (_received < sizeof(ReplicationMsgHeader)) {
      return false;
    }
    auto* hdr = reinterpret_cast<const ReplicationMsgHeader*>(_data);
    if (!hdr->is_valid()) {
      return false;
    }
    _expected = sizeof(ReplicationMsgHeader) + hdr->payload_size;
    return true;
  }

  // Is the message header received?
  bool has_header() const { return _received >= sizeof(ReplicationMsgHeader); }

  // Is the complete message received?
  bool is_complete() const { return _expected > 0 && _received >= _expected; }

  // Get the complete message as a slice (only valid if is_complete())
  Slice message() const { return Slice(_data, _received); }

  // Get just the payload (only valid if is_complete())
  Slice payload() const {
    return Slice(_data + sizeof(ReplicationMsgHeader),
                 _received - sizeof(ReplicationMsgHeader));
  }

  // Reset for next message
  void reset() {
    _received = 0;
    _expected = 0;
  }

  // Check if buffer is valid
  bool is_valid() const { return _data != nullptr && _capacity > 0; }
};

// =============================================================================
// Transport Interface
// =============================================================================

// Abstract transport - the FSM tells it what to send
struct ReplicationTransport {
  virtual void send(const uint8_t* data, size_t size) = 0;
  virtual ~ReplicationTransport() = default;

  void send(Slice s) {
    send(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
};

// =============================================================================
// Event Callbacks
// =============================================================================

// Events the FSM emits to the application
struct ReplicationEvents {
  virtual void on_complete(uint64_t session_id, size_t nodes_transferred) = 0;
  virtual void on_error(uint64_t session_id, ReplicationError error,
                        const char* reason) = 0;
  virtual void on_progress(uint64_t session_id, size_t bytes_transferred,
                           size_t nodes_transferred) = 0;
  virtual ~ReplicationEvents() = default;
};

// =============================================================================
// Message Builder Helper
// =============================================================================

struct ReplicationMsgBuilder {
  std::vector<uint8_t> _buffer;

  void begin(ReplicationMsgType type, uint64_t session_id) {
    _buffer.clear();
    _buffer.resize(sizeof(ReplicationMsgHeader));

    auto* hdr = reinterpret_cast<ReplicationMsgHeader*>(_buffer.data());
    hdr->magic = REPLICATION_MSG_MAGIC;
    hdr->msg_type = static_cast<uint8_t>(type);
    hdr->session_id = session_id;
    hdr->payload_size = 0;
  }

  void append_payload(const uint8_t* data, size_t size) {
    size_t offset = _buffer.size();
    _buffer.resize(offset + size);
    std::memcpy(_buffer.data() + offset, data, size);

    auto* hdr = (ReplicationMsgHeader*)_buffer.data();
    hdr->payload_size = _buffer.size() - sizeof(ReplicationMsgHeader);
  }

  void append_payload(Slice s) { append_payload((uint8_t*)s.data(), s.size()); }

  Slice finalize() { return Slice(_buffer.data(), _buffer.size()); }
  const uint8_t* data() const { return _buffer.data(); }
  size_t size() const { return _buffer.size(); }
};

// =============================================================================
// Message Parser Helper
// =============================================================================

inline const ReplicationMsgHeader* parse_replication_msg(const uint8_t* data,
                                                         size_t size,
                                                         Slice* out_payload) {
  if (size < sizeof(ReplicationMsgHeader)) {
    return nullptr;
  }

  const auto* hdr = reinterpret_cast<const ReplicationMsgHeader*>(data);

  if (!hdr->is_valid()) {
    return nullptr;
  }

  size_t expected_size = sizeof(ReplicationMsgHeader) + hdr->payload_size;
  if (size < expected_size) {
    return nullptr;
  }

  if (out_payload) {
    *out_payload =
        Slice(data + sizeof(ReplicationMsgHeader), hdr->payload_size);
  }

  return hdr;
}

// =============================================================================
// Sender FSM
// =============================================================================

template <typename DB>
struct ReplicationSenderFSM {
  using Traits = typename DB::Traits;
  using Sender = TransferTrieSender<DB>;
  using Transfer = TransferTrie<Traits>;

  enum class State {
    IDLE,               // Not started
    SENDING,            // Sending trie data
    AWAITING_RESPONSE,  // Waiting for SUBTRIE_ACK or COMPLETE
    COMPLETE,           // Replication finished
    ERROR               // Error occurred
  };

  DB* _db;
  typename DB::txn_ptr _txn;
  ReplicationTransport* _transport;
  ReplicationEvents* _events;
  Sender _sender;
  ReplicationMsgBuilder _msg_builder;
  State _state;
  uint64_t _session_id;
  size_t _total_nodes;
  size_t _total_bytes;
  ReplicationError _error;
  DbType _db_type;

  ReplicationSenderFSM(DB* db, typename DB::txn_ptr txn,
                       size_t buffer_size = Transfer::DEFAULT_MAX_SIZE)
      : _db(db),
        _txn(txn),
        _transport(nullptr),
        _events(nullptr),
        _sender(db, txn, buffer_size),
        _state(State::IDLE),
        _session_id(0),
        _total_nodes(0),
        _total_bytes(0),
        _error(ReplicationError::NONE),
        _db_type(DbType::DB_MAIN) {}

  // Start replication
  void begin(ReplicationTransport* transport, ReplicationEvents* events,
             DbType db_type = DbType::DB_MAIN) {
    _transport = transport;
    _events = events;
    _state = State::SENDING;
    _total_nodes = 0;
    _total_bytes = 0;
    _error = ReplicationError::NONE;
    _db_type = db_type;

    _sender.begin(db_type);
    _session_id = _sender.session_id();

    _send_next_buffer();
  }

  // Called by transport layer when a message is received
  void on_message_received(const uint8_t* data, size_t size) {
    Slice payload;
    const auto* hdr = parse_replication_msg(data, size, &payload);

    if (!hdr) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Failed to parse message");
      return;
    }

    if (hdr->session_id != _session_id) {
      _transition_to_error(ReplicationError::SESSION_MISMATCH,
                           "Session ID mismatch");
      return;
    }

    auto msg_type = static_cast<ReplicationMsgType>(hdr->msg_type);

    switch (_state) {
      case State::AWAITING_RESPONSE:
        _handle_response(msg_type, payload);
        break;

      case State::SENDING:
        // Unexpected message while sending
        _transition_to_error(ReplicationError::INVALID_STATE,
                             "Received message while sending");
        break;

      case State::IDLE:
        // Already completed, ignore any late messages
        // This can happen if both sides send COMPLETE simultaneously
        break;

      default:
        _transition_to_error(ReplicationError::INVALID_STATE,
                             "Received message in invalid state");
        break;
    }
  }

  State state() const { return _state; }
  uint64_t session_id() const { return _session_id; }
  ReplicationError error() const { return _error; }

  void _handle_response(ReplicationMsgType msg_type, const Slice& payload) {
    switch (msg_type) {
      case ReplicationMsgType::COMPLETE:
        if (_events) {
          _events->on_complete(_session_id, _total_nodes);
        }
        _state = State::IDLE;
        break;

      case ReplicationMsgType::SUBTRIE_ACK:
        _handle_subtrie_ack(payload);
        break;

      case ReplicationMsgType::FRACTION_COMPLETE:
        // Receiver merged what it had and wants a fresh round.
        // Refresh _txn to latest read txn so the sender reads the
        // current snapshot when restarting from root.
        _txn = _db->txn();
        _sender._txn = _txn;
        _sender.begin(_db_type);
        _state = State::SENDING;
        _send_next_buffer();
        break;

      case ReplicationMsgType::ERROR:
        _state = State::ERROR;
        _error = payload.size() > 0
                     ? static_cast<ReplicationError>(payload.data()[0])
                     : ReplicationError::INTERNAL_ERROR;
        if (_events) {
          _events->on_error(_session_id, _error, "Remote error");
        }
        break;

      default:
        _transition_to_error(ReplicationError::INVALID_MESSAGE,
                             "Unexpected message type");
        break;
    }
  }

  void _handle_subtrie_ack(const Slice& payload) {
    // Parse SUBTRIE_ACK message (same format as old REQUEST_CHILDREN)
    RequestChildrenHeader req_hdr;
    if (!parse_request_children(payload, &req_hdr)) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Failed to parse SUBTRIE_ACK");
      return;
    }

    // Pass iterator to sender - ACKed paths will be pruned from pending
    auto iter = request_children_iterator(payload, req_hdr);
    _sender.process_ack(iter);

    // Continue sending remaining nodes
    if (_sender.has_pending()) {
      _state = State::SENDING;
      _send_next_buffer();
    } else {
      // All done
      _send_complete();
    }
  }

  void _send_next_buffer() {
    // With the new strategy, each fill_buffer() produces exactly one complete
    // subtrie The subtrie_path in the header identifies where it connects to
    // the parent
    _sender.fill_buffer();
    Slice buffer = _sender.finalize();
#ifdef LEAVES_DEBUG
    std::cerr << "DEBUG: Sending buffer with " << buffer.size() << " bytes\n";
#endif

    // Parse to get node count
    const TransferTrieHeader* hdr = Transfer::parse_header(buffer);
    if (!hdr) {
      _transition_to_error(ReplicationError::INTERNAL_ERROR,
                           "Failed to parse transfer header");
      return;
    }

    _total_nodes += hdr->node_count;
    _total_bytes += buffer.size();

    // Wrap in message envelope and send
    _msg_builder.begin(ReplicationMsgType::TRIE_DATA, _session_id);
    _msg_builder.append_payload(buffer);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    if (_events) {
      _events->on_progress(_session_id, _total_bytes, _total_nodes);
    }

    // Transition to awaiting response
    _state = State::AWAITING_RESPONSE;
  }

  void _send_complete() {
    _msg_builder.begin(ReplicationMsgType::COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    if (_events) {
      _events->on_complete(_session_id, _total_nodes);
    }
    _state = State::IDLE;
  }

  void _transition_to_error(ReplicationError error, const char* reason) {
    _state = State::ERROR;
    _error = error;

    // Send error to remote
    _msg_builder.begin(ReplicationMsgType::ERROR, _session_id);
    uint8_t err_byte = static_cast<uint8_t>(error);
    _msg_builder.append_payload(&err_byte, 1);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    if (_events) {
      _events->on_error(_session_id, error, reason);
    }
  }
};

// =============================================================================
// Receiver FSM
// =============================================================================

// Default overwrite handler for replication - always accepts source (wire) data
struct ReplicationMergePolicy {
  // Always overwrite local with source (wire) data
  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src) {
    return true;
  }

  // Always add new leaves from source
  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return true;
  }

  // Always recurse into source tries
  bool may_add_trie(const std::string& key) { return true; }

  // Free big value - no-op for wire format (source doesn't own big memory)
  template <typename LeafNode>
  void free_big(LeafNode& leaf) {
    // Wire format leaves don't have big values that need freeing
  }

  // Migrate big value from source - wire format doesn't have big values
  template <typename LeafNode, typename DB>
  Slice migrate_big_value(LeafNode& leaf, DB& db) {
    // Wire format doesn't use big values, return regular value
    return leaf.value();
  }
};

template <typename DB, typename MergePolicy = ReplicationMergePolicy>
struct ReplicationReceiverFSM {
  using Traits = typename DB::Traits;
  using Transfer = TransferTrie<Traits>;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using offset_e = typename Traits::offset_e;
  using WireCursor = _Cursor<WireTempTraits>;
  using TempOffset =
      typename WireTempTraits::offset_e;  // Native offset type for temp DB
  using TempTrieNode =
      _TrieNode<WireTempTraits>;  // Temp DB trie node (native layout)
  using TempLeafNode =
      _LeafNode<WireTempTraits>;  // Temp DB leaf node (native layout)
  using LocalCursor = typename DB::Cursor;

  enum class State {
    IDLE,       // Not started
    RECEIVING,  // Receiving and processing trie data
    COMPLETE,   // Replication finished
    ERROR       // Error occurred
  };

  DB* _db;
  typename DB::txn_ptr _txn;
  typename DB::cursor_ptr _cursor;
  ReplicationMsgBuilder _msg_builder;
  RequestChildrenBuilder _request_builder;
  ReplicationTransport* _transport;
  ReplicationEvents* _events;
  State _state;
  uint64_t _session_id;
  size_t _total_nodes;
  size_t _total_bytes;
  ReplicationError _error;
  std::vector<std::string>
      _prune_paths;          // Paths where hashes match (tell sender to prune)
  std::string _path_buffer;  // Reusable buffer for path construction
  size_t _pending_children;  // Count of 0 offsets not yet connected (receiver
                             // completion tracking)
  size_t _memory_budget;     // Max temp DB bytes before forcing a fraction
                             // merge. SIZE_MAX = unlimited (single round).

  // Receive buffer management (Temp DB)
  // ====================================
  // Each received message buffer becomes part of the temp DB and cannot be
  // reused. Design constraints:
  // 1. _temp_buffers owns all received message memory (the "temp DB")
  // 2. This memory is freed only after replication completes or errors
  // 3. Subtries within temp DB use relative offsets (wire format)
  // 4. When connecting subtries to persistent storage, offsets must be
  // translated
  //    since absolute offsets from temp DB don't work in persistent storage
  std::vector<std::vector<uint8_t>>
      _temp_buffers;              // Owned buffers (temp DB memory)
  ReceiveBuffer _receive_buffer;  // Current receive buffer being filled
  size_t _initial_buffer_size;
  static constexpr size_t DEFAULT_RECEIVE_BUFFER_SIZE =
      64 * 1024;  // 64KB, grows as needed

  // Temp DB root tracking
  alignas(8)
      TempOffset _temp_root;  // Root offset of temp DB (first received subtrie)
  WireTempDB _wire_db;        // Stateless adapter for resolving wire offsets
  WireCursor _wire_cursor;    // Reusable cursor for navigating temp DB

  // Overwrite handler for merge conflicts
  MergePolicy _merge_policy;

  ReplicationReceiverFSM(
      DB* db, typename DB::txn_ptr txn, MergePolicy handler = MergePolicy(),
      size_t receive_buffer_size = DEFAULT_RECEIVE_BUFFER_SIZE,
      size_t memory_budget = SIZE_MAX)
      : _db(db),
        _txn(txn),
        _cursor(db->create_cursor()),
        _transport(nullptr),
        _events(nullptr),
        _state(State::IDLE),
        _session_id(0),
        _total_nodes(0),
        _total_bytes(0),
        _error(ReplicationError::NONE),
        _memory_budget(memory_budget),
        _initial_buffer_size(receive_buffer_size),
        _temp_root(0),
        _wire_cursor(&_wire_db, &_temp_root),
        _merge_policy(std::move(handler)) {
    // Allocate first receive buffer
    _alloc_receive_buffer();
  }

  // Allocate a new receive buffer (previous one becomes part of temp DB)
  void _alloc_receive_buffer() {
    _temp_buffers.emplace_back(_initial_buffer_size);
    auto& buf = _temp_buffers.back();
    _receive_buffer.init(buf.data(), buf.size());
  }

  // Called after message is complete - buffer is now part of temp DB
  // Allocate new buffer for next message
  void _finalize_receive_buffer() {
    // Current buffer stays in _temp_buffers (it's the temp DB memory)
    // Allocate fresh buffer for next message
    _alloc_receive_buffer();
  }

  // Free all temp buffers after replication is complete
  void _free_temp_buffers() {
    _temp_buffers.clear();
    _temp_buffers.shrink_to_fit();
    // Re-allocate a single buffer for potential future use
    _alloc_receive_buffer();
  }

  // Total bytes held in temp DB buffers
  size_t _temp_buffer_memory() const {
    size_t total = 0;
    for (const auto& buf : _temp_buffers) total += buf.size();
    return total;
  }

  // Start receiving - called when ready to accept data
  void begin(ReplicationTransport* transport, ReplicationEvents* events) {
    _transport = transport;
    _events = events;
    _state = State::RECEIVING;
    _session_id = 0;  // Will be set from first message
    _total_nodes = 0;
    _total_bytes = 0;
    _error = ReplicationError::NONE;
    _prune_paths.clear();
    _pending_children = 0;

    // Reset temp buffers from any previous session
    _temp_buffers.clear();
    _alloc_receive_buffer();
    _temp_root = 0;
  }

  // ==========================================================================
  // Zero-Copy Receive Interface
  // ==========================================================================
  // The transport layer uses these methods to write directly into FSM's buffer:
  // 1. Call receive_buffer() to get the buffer
  // 2. Write data to buffer.write_ptr(), up to buffer.available() bytes
  // 3. Call buffer.advance(bytes_written) after each write
  // 4. Call on_data_received() after each write to check if message is complete
  // 5. When on_data_received() returns true, the message has been processed

  // Get the receive buffer for zero-copy reception
  ReceiveBuffer& receive_buffer() { return _receive_buffer; }

  // Called by transport after writing data to the receive buffer
  // Returns true if a complete message was processed (buffer is reset for next
  // message) Returns false if more data is needed
  bool on_data_received() {
    // Try to parse expected size if we don't have it yet
    if (!_receive_buffer.has_header()) {
      return false;  // Need more data for header
    }

    if (_receive_buffer._expected == 0) {
      if (!_receive_buffer.parse_expected()) {
        _transition_to_error(ReplicationError::INVALID_MESSAGE,
                             "Invalid message header");
        return true;  // Error is a "complete" state
      }

      // Check if message fits in current buffer
      if (_receive_buffer._expected > _receive_buffer._capacity) {
        // Need to grow the current buffer
        auto& buf = _temp_buffers.back();
        buf.resize(_receive_buffer._expected);
        // Data is already at the beginning, just update the buffer pointer
        _receive_buffer._data = buf.data();
        _receive_buffer._capacity = buf.size();
      }
    }

    if (!_receive_buffer.is_complete()) {
      return false;  // Need more data
    }

    // Complete message received - process it
    _process_received_message();

    // Buffer is now part of temp DB - allocate new one for next message
    _finalize_receive_buffer();
    return true;
  }

  // Process a complete message from the receive buffer
  void _process_received_message() {
    auto* hdr = (ReplicationMsgHeader*)_receive_buffer._data;

    // Validate header
    if (!hdr->is_valid()) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Invalid message magic");
      return;
    }

    // First message sets session ID
    if (_session_id == 0) {
      _session_id = hdr->session_id;
    } else if (hdr->session_id != _session_id) {
      _transition_to_error(ReplicationError::SESSION_MISMATCH,
                           "Session ID mismatch");
      return;
    }

    auto msg_type = static_cast<ReplicationMsgType>(hdr->msg_type);
    Slice payload = _receive_buffer.payload();

    switch (_state) {
      case State::RECEIVING:
        _handle_incoming(msg_type, payload);
        break;

      case State::IDLE:
        // Already completed, ignore any late messages
        // This can happen if both sides send COMPLETE simultaneously
        break;

      default:
        _transition_to_error(ReplicationError::INVALID_STATE,
                             "Received message in invalid state");
        break;
    }
  }

  State state() const { return _state; }
  uint64_t session_id() const { return _session_id; }
  ReplicationError error() const { return _error; }

  void _handle_incoming(ReplicationMsgType msg_type, const Slice& payload) {
    switch (msg_type) {
      case ReplicationMsgType::TRIE_DATA:
        _handle_trie_data(payload);
        break;

      case ReplicationMsgType::COMPLETE:
        // Receiver determines completion autonomously by tracking pending
        // children If we receive this, sender thinks we're done - just ACK it
        if (_events) {
          _events->on_complete(_session_id, _total_nodes);
        }
        _state = State::IDLE;
        break;

      case ReplicationMsgType::ERROR:
        _state = State::ERROR;
        _error = payload.size() > 0
                     ? static_cast<ReplicationError>(payload.data()[0])
                     : ReplicationError::INTERNAL_ERROR;
        if (_events) {
          _events->on_error(_session_id, _error, "Remote error");
        }
        break;

      default:
        _transition_to_error(ReplicationError::INVALID_MESSAGE,
                             "Unexpected message type");
        break;
    }
  }

  void _handle_trie_data(const Slice& payload) {
    Slice subtrie_path;
    const TransferTrieHeader* hdr =
        Transfer::parse_header(payload, &subtrie_path);

    if (!hdr) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Failed to parse TRIE_DATA header");
      return;
    }

    _total_bytes += payload.size();
    _total_nodes += hdr->node_count;

    if (_events) {
      _events->on_progress(_session_id, _total_bytes, _total_nodes);
    }

    // Process nodes: compare hashes and collect paths where we differ
    _process_received_nodes(payload, *hdr, subtrie_path);
  }

  void _process_received_nodes(const Slice& payload,
                               const TransferTrieHeader& hdr,
                               const Slice& path) {
    if (hdr.node_count == 0) {
      // Empty transfer
      _send_prune_ack();
      return;
    }

    // Get pointer to root node in wire buffer (no copy)
    // Note: cast to non-const since we may need to modify offsets in temp DB
    const TransferTrieHeader* transfer_hdr = Transfer::parse_header(payload);
    if (!transfer_hdr || hdr.node_count == 0) {
      _send_prune_ack();
      return;
    }

    // Connect received subtrie to temp DB
    if (path.empty()) {
      // Beginning of a new round — refresh cursor to latest committed
      // state so hash comparisons see previously merged data.
      _cursor->update();

      // First subtrie - this becomes the temp DB root
      char* wire_root = transfer_hdr->root.resolve<char>();
      _temp_root.set_relative(wire_root);
      _temp_root.type(transfer_hdr->root.type());
    } else {
      // Subsequent subtrie - connect to parent in temp DB
      _connect_subtrie_to_parent(path, (TempOffset*)&transfer_hdr->root);
    }

    // Compare wire nodes with local nodes, collect paths where we differ
    _path_buffer.assign(path.data(), path.size());

    // For the root subtrie, count it as pending so the decrement at
    // _compare_wire_with_local entry balances. Subsequent subtries were
    // already counted as pending children of their parent trie.
    if (path.empty()) {
      ++_pending_children;
    }

    const char* payload_end = payload.data() + payload.size();
    _compare_wire_with_local((TempOffset*)&transfer_hdr->root, payload_end,
                             _path_buffer);

    // Check if all pending children are now connected - if so, replication is
    // complete
    if (_pending_children == 0) {
      _send_complete();
      return;
    }

    // If temp DB exceeds memory budget, merge what we have and ask the
    // sender to restart from root.  The next round's hash comparisons
    // will prune already-replicated subtrees automatically.
    if (_temp_buffer_memory() > _memory_budget) {
      _send_fraction_complete();
      return;
    }

    // Normal path: send ACK with prune paths
    _send_prune_ack();
  }

  // Get pointer to the original offset at a path (what we had before receiving)
  // Returns pointer to offset for future relative offset handling compatibility
  // expected_type: the wire node's type (LEAF or TRIE) — used to disambiguate
  // NONE-branch leaves from their parent trie node which share the same path.
  const offset_t* _get_original_node(const Slice& path, uint8_t expected_type) {
    if (path.empty()) {
      return _cursor->_root;
    }

    // Navigate to path in local trie using cursor
    _cursor->find(path);

    if (_cursor->stack.size == 0) {
      return nullptr;  // Path doesn't exist locally
    }

    auto& trans = _cursor->stack.back();
    if (!trans.offset || !*trans.offset) {
      return nullptr;  // No node at this path
    }

    // Wire node is a NONE-branch leaf but cursor landed on the parent trie
    // (they share the same path).  Resolve the trie's NONE child instead.
    if (expected_type == LEAF && trans.offset->type() == TRIE) {
      auto trie = _db->template resolve<TrieNode>(trans.offset);
      auto* none_child = trie->offset(TrieNode::NONE);
      return none_child ? reinterpret_cast<const offset_t*>(none_child)
                        : nullptr;
    }

    return trans.offset;
  }

  int get_branch_key(const TempOffset* subtrie_root) {
    if (subtrie_root->type() == TRIE) {
      auto* temp_trie = subtrie_root->template resolve<TempTrieNode>();
      assert(temp_trie->len() > 0 && "Received empty trie node");
      return temp_trie->compressed()[0];
    }
    assert(subtrie_root->type() == LEAF);
    auto* temp_leaf = subtrie_root->template resolve<TempLeafNode>();
    return temp_leaf->key().size() > 0 ? temp_leaf->key()[0]
                                       : TempTrieNode::NONE;
  }

  // Connect a received subtrie to its parent in the temp DB
  // path: the full path to this subtrie (e.g., "abc")
  // subtrie_root: pointer to the received subtrie's root node
  // subtrie_type: type of the subtrie root (TRIE_NODE or LEAF_NODE)
  void _connect_subtrie_to_parent(const Slice& path, TempOffset* subtrie_root) {
    if (path.empty() || !_temp_root) return;  // _temp_root == 0 means not set

    TempOffset* parent_offset;
    int key = get_branch_key(subtrie_root);
    if (key == TempTrieNode::NONE) {
      parent_offset = _find_temp_parent_offset(path);
    } else {
      _path_buffer.assign(path.data(), path.size());
      _path_buffer.push_back((char)key);

      WireTrieNode* temp_trie = _temp_root.template resolve<WireTrieNode>();
      parent_offset = _find_temp_parent_offset(Slice(_path_buffer));
    }

    assert(parent_offset && "Parent not found in temp DB");

    parent_offset->set_relative(subtrie_root->template resolve<char>());
    parent_offset->type(subtrie_root->type());
  }

  // Navigate temp DB to find the parent's child offset slot for a path
  // path: full path (e.g., "abc"), we find parent at "ab" and return offset for
  // 'c' Returns pointer to the TempOffset slot, or nullptr if not found Uses
  // _Cursor with WireTempDB for navigation (handles compressed paths)
  TempOffset* _find_temp_parent_offset(const Slice& path) {
    if (path.empty() || !_temp_root) return nullptr;
    if (_temp_root.type() != TRIE) return nullptr;

    // Navigate to the path using member cursor (already bound to _temp_root)
    _wire_cursor.find(path);

    // After find(), check if we found the parent's offset slot
    // The cursor navigates to where the path would be, with link_idx set
    if (_wire_cursor.stack.size == 0) return nullptr;

    return _wire_cursor.stack.back().offset;
  }

  // Compare wire node with local node, collect paths where we need more data
  // - If hashes match: tell sender to prune, _Merger handles identical subtrees
  // - If hashes differ and wire is leaf: will be merged to local via _Merger
  // - If hashes differ and wire is trie: recurse into children
  // - If child offset == 0 in wire: sender hasn't sent it yet, stays pending
  bool _compare_wire_with_local(TempOffset* wire_node, const char* buffer_end,
                                std::string& path) {
    if ((char*)wire_node >= buffer_end) return false;

    --_pending_children; 

    // Get wire node's hash
    const uint8_t* wire_hash;
    int branch_key;
    if (wire_node->type() == LEAF) {
      wire_hash = wire_node->template resolve<TempLeafNode>()->hash;
      branch_key = get_branch_key(wire_node);
    } else {
      wire_hash = wire_node->template resolve<TempTrieNode>()->hash;
      // path.empty() => wire_node is root => no branch_key
      branch_key =
          path.empty() ? TempTrieNode::NONE : get_branch_key(wire_node);
    }

    size_t path_len = path.size();
    if (branch_key != TempTrieNode::NONE) path.push_back((char)branch_key);

    auto* local_offset = _get_original_node(Slice(path), wire_node->type());
    // Compare with local
    if (local_offset) {
      const uint8_t* local_hash = _get_local_hash(local_offset);
      if (local_hash && std::memcmp(wire_hash, local_hash, HASH_SIZE) == 0) {
        // Hashes match - tell sender to prune, set the offset 0 to avoid merging
        *wire_node = 0;
        _prune_paths.push_back(path);
        path.resize(path_len);
        return true;
      }
    }

    path.resize(path_len);

    // Hashes differ or local doesn't exist
    if (wire_node->type() == LEAF) {
#ifdef LEAVES_DEBUG
      auto* leaf = wire_node->template resolve<TempLeafNode>();
      path.append(leaf->key().data(), leaf->key().size());
      std::cerr << "DEBUG: receive node: " << path << " (type=leaf) "
                << leaf->key().size() << "\n";
      path.resize(path_len);
#endif
      return true;
    }

    // Wire is trie node - iterate through children
    auto* wire_trie = wire_node->template resolve<TempTrieNode>();
    TempOffset* wire_array = wire_trie->array();
    int count = wire_trie->count();

#ifdef LEAVES_DEBUG
    if (!path.empty())
      path.push_back((char)wire_trie->compressed()[0]);
    std::cerr << "DEBUG: receive node: " << path << " (type=trie)\n";
    path.resize(path_len);
#endif

    // All children start as pending; decrement as each is handled
    _pending_children += count;

    path.append((char*)wire_trie->compressed(), wire_trie->len());
    for (int i = 0; i < count; ++i) {
      TempOffset* child_offset = wire_array + i;

      if (!*child_offset) {
        continue;  // Not yet received, stays pending
      }

      assert(child_offset->is_relative() && "Child offsets in wire format must be relative");

      if (!_compare_wire_with_local(child_offset, buffer_end, path)) {
        path.resize(path_len);
        return false;
      }
    }
#if 0
    // If every child was pruned (zeroed), this trie node is an empty
    // skeleton — zero it out so the merger doesn't try to merge it.
    {
      bool all_zero = true;
      for (int i = 0; i < count; ++i) {
        if (wire_array[i]) { all_zero = false; break; }
      }
      if (all_zero) *wire_node = 0;
    }
#endif 
    // Restore path to original length (remove compressed portion we added)
    path.resize(path_len);
    return true;
  }

  const uint8_t* _get_local_hash(const offset_t* offset) {
    if (offset->type() == LEAF)
      return _db->template resolve<LeafNode>(offset)->hash;

    return _db->template resolve<TrieNode>(offset)->hash;
  }

  // Merge temp DB (received wire nodes) into local DB using _Merger
  void _merge_temp_to_local() {
    if (!_temp_root) return;

#ifdef LEAVES_DEBUG
    // Dump to /tmp/rb-<N>.yaml for debugging
    {
      static int _rb_round = 0;
      std::ofstream out("/tmp/rb-" + std::to_string(_rb_round++) + ".yaml");
      WireTempDB db;
      struct DumpContainer {
        using db_type = WireTempDB;
        struct Cursor {};  // Dummy cursor type
        const WireTempDB& _db;
        const WireTempDB* _internal() const { return &_db; }
      } container{db};
      _Dumper<DumpContainer, false> dumper(container, _wire_cursor._root,
                                           false);
      dumper.dump(out);

      // Dump local (destination) DB state
      std::ofstream dst_out("/tmp/rb-dst-" + std::to_string(_rb_round - 1) + ".yaml");
      _Dumper<DB, false> dst_dumper(*_db, _cursor->_root, false);
      dst_dumper.dump(dst_out);
    }
#endif

    // set position to root
    _wire_cursor.clear();
    _cursor->start_transaction();
    // Use _Merger to merge wire trie into local DB
    _Merger<LocalCursor, WireCursor, MergePolicy> merger(*_cursor, _wire_cursor,
                                                         _merge_policy);
    merger.exec();
    _cursor->commit();
  }

  // Send ACK with prune paths (where hashes matched)
  // Sender will respond with next subtrie or COMPLETE
  void _send_prune_ack() {
    _request_builder.begin(_session_id, DbType::DB_MAIN);

    // Add all paths where hashes matched (sender should prune these)
    for (const auto& path : _prune_paths) {
      _request_builder.add_path(path);
    }
    _prune_paths.clear();

    _msg_builder.begin(ReplicationMsgType::SUBTRIE_ACK, _session_id);
    _msg_builder.append_payload(_request_builder.finalize());
    _transport->send(_msg_builder.data(), _msg_builder.size());
  }

  // Merge current fraction, commit so next round sees updated hashes,
  // reset temp state, and tell sender to restart from root.
  void _send_fraction_complete() {
    if (_temp_root) {
      _merge_temp_to_local();
    }

    // Reset temp DB for the next fraction
    _free_temp_buffers();
    _temp_root = 0;
    _pending_children = 0;
    _prune_paths.clear();

    _msg_builder.begin(ReplicationMsgType::FRACTION_COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());
  }

  void _send_complete() {
    // Merge temp DB into local DB using _Merger
    if (_temp_root) {  // _temp_root != 0 means it was set
      _merge_temp_to_local();
    }

    // Free temp buffers - data has been integrated into persistent storage
    _free_temp_buffers();

    _msg_builder.begin(ReplicationMsgType::COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    if (_events) {
      _events->on_complete(_session_id, _total_nodes);
    }
    _state = State::IDLE;
  }

  void _transition_to_error(ReplicationError error, const char* reason) {
    _state = State::ERROR;
    _error = error;

    // Free temp buffers on error
    _free_temp_buffers();

    if (_session_id != 0) {
      _msg_builder.begin(ReplicationMsgType::ERROR, _session_id);
      uint8_t err_byte = static_cast<uint8_t>(error);
      _msg_builder.append_payload(&err_byte, 1);
      _transport->send(_msg_builder.data(), _msg_builder.size());
    }

    if (_events) {
      _events->on_error(_session_id, error, reason);
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES__REPLICATION_FSM_HPP
