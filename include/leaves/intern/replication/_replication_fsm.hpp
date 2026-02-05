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
  TRIE_DATA = 0x01,         // TransferTrie buffer (sender → receiver)
  REQUEST_CHILDREN = 0x02,  // Request subtries at paths (receiver → sender)
  COMPLETE = 0x03,          // Replication complete (either side)
  ERROR = 0x04              // Error occurred (either side)
};

constexpr uint32_t REPLICATION_MSG_MAGIC = 0x4C565250;  // "LVRP"

// Message envelope wrapping all replication messages
#pragma pack(push, 1)
struct ReplicationMsgHeader {
  boost::endian::little_uint32_t magic;
  uint8_t msg_type;  // ReplicationMsgType
  boost::endian::little_uint64_t session_id;
  boost::endian::little_uint32_t payload_size;
  // Followed by: payload bytes

  bool is_valid() const { return magic == REPLICATION_MSG_MAGIC; }
};
#pragma pack(pop)

static_assert(sizeof(ReplicationMsgHeader) == 17,
              "ReplicationMsgHeader must be 17 bytes");

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
      return 0;  // Have header but expected not set - caller should call parse_expected()
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

  void send(Slice s) { send(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
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

    auto* hdr = reinterpret_cast<ReplicationMsgHeader*>(_buffer.data());
    hdr->payload_size = static_cast<uint32_t>(_buffer.size() -
                                               sizeof(ReplicationMsgHeader));
  }

  void append_payload(Slice s) {
    append_payload(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

  Slice finalize() { return Slice(_buffer.data(), _buffer.size()); }
  const uint8_t* data() const { return _buffer.data(); }
  size_t size() const { return _buffer.size(); }
};

// =============================================================================
// Message Parser Helper
// =============================================================================

inline const ReplicationMsgHeader* parse_replication_msg(const uint8_t* data, size_t size,
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
    *out_payload = Slice(data + sizeof(ReplicationMsgHeader),
                         hdr->payload_size);
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
    IDLE,              // Not started
    SENDING,           // Sending trie data
    AWAITING_RESPONSE, // Waiting for REQUEST_CHILDREN or COMPLETE
    COMPLETE,          // Replication finished
    ERROR              // Error occurred
  };

  DB* _db;
  typename DB::txn_ptr _txn;
  ReplicationTransport* _transport;
  ReplicationEvents* _events;
  Sender _sender;
  ReplicationMsgBuilder _msg_builder;
  std::vector<std::string> _request_paths;
  State _state;
  uint64_t _session_id;
  size_t _total_nodes;
  size_t _total_bytes;
  ReplicationError _error;

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
        _error(ReplicationError::NONE) {}

  // Start replication
  void begin(ReplicationTransport* transport, ReplicationEvents* events,
             DbType db_type = DbType::DB_MAIN) {
    _transport = transport;
    _events = events;
    _state = State::SENDING;
    _total_nodes = 0;
    _total_bytes = 0;
    _error = ReplicationError::NONE;

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
        _state = State::COMPLETE;
        if (_events) {
          _events->on_complete(_session_id, _total_nodes);
        }
        break;

      case ReplicationMsgType::REQUEST_CHILDREN:
        _handle_request_children(payload);
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

  void _handle_request_children(const Slice& payload) {
    // Parse REQUEST_CHILDREN message
    RequestChildrenHeader req_hdr;
    if (!parse_request_children(payload, &req_hdr)) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                          "Failed to parse REQUEST_CHILDREN");
      return;
    }

    // Extract paths into reusable member vector
    _request_paths.clear();
    auto iter = request_children_iterator(payload, req_hdr);
    while (iter.valid()) {
      _request_paths.push_back(iter.path());
      iter.next();
    }

    if (_request_paths.empty()) {
      // No paths requested - receiver is complete
      _send_complete();
      return;
    }

    // Continue from requested paths
    _sender.continue_from(std::move(_request_paths));
    _state = State::SENDING;
    _send_next_buffer();
  }

  void _send_next_buffer() {
    bool sent_any = false;
    
    while (!_sender.is_complete()) {
      bool subtrie_complete = _sender.fill_buffer();
      Slice buffer = _sender.finalize();

      if (buffer.size() >= sizeof(TransferTrieHeader)) {
        // Parse to get node count
        const TransferTrieHeader* hdr = Transfer::parse_header(buffer);

        _total_nodes += hdr->node_count;
        _total_bytes += buffer.size();

        // Wrap in message envelope and send
        _msg_builder.begin(ReplicationMsgType::TRIE_DATA, _session_id);
        _msg_builder.append_payload(buffer);
        _transport->send(_msg_builder.data(), _msg_builder.size());
        sent_any = true;

        if (_events) {
          _events->on_progress(_session_id, _total_bytes, _total_nodes);
        }
      }

      if (!subtrie_complete) {
        // Buffer full mid-subtrie, wait for more sends
        // In practice with BFS we keep sending until subtrie done
        continue;
      }
    }

    // If we have an empty DB, still send one empty TRIE_DATA to start session
    if (!sent_any) {
      _sender.fill_buffer();  // Creates empty buffer
      Slice buffer = _sender.finalize();
      
      _msg_builder.begin(ReplicationMsgType::TRIE_DATA, _session_id);
      _msg_builder.append_payload(buffer);
      _transport->send(_msg_builder.data(), _msg_builder.size());
    }

    // All subtries sent, wait for response
    _state = State::AWAITING_RESPONSE;
  }

  void _send_complete() {
    _msg_builder.begin(ReplicationMsgType::COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    _state = State::COMPLETE;
    if (_events) {
      _events->on_complete(_session_id, _total_nodes);
    }
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
struct ReplicationOverwriteHandler {
  // Always overwrite local with source (wire) data
  bool check_overwrite(const std::string& key, const Slice& dst,
                       const Slice& src) {
    return true;
  }

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

template <typename DB, typename OverwriteHandler = ReplicationOverwriteHandler>
struct ReplicationReceiverFSM {
  using Traits = typename DB::Traits;
  using Transfer = TransferTrie<Traits>;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using offset_e = typename Traits::offset_e;
  using WireCursor = _Cursor<WireTempTraits>;
  using TempOffset = typename WireTempTraits::offset_e;  // Native offset type for temp DB
  using TempTrieNode = _TrieNode<WireTempTraits>;  // Temp DB trie node (native layout)
  using TempLeafNode = _LeafNode<WireTempTraits>;  // Temp DB leaf node (native layout)
  using LocalCursor = typename DB::Cursor;

  // Marker for pruned children (matches local, skip in merger)
  // Value 7 has all flag bits set, so operator uint64_t() returns 0,
  // but _value != 0 distinguishes it from "not yet sent" (actual 0)
  static constexpr uint64_t PRUNED_MARKER = 7;

  enum class State {
    IDLE,        // Not started
    RECEIVING,   // Receiving and processing trie data
    COMPLETE,    // Replication finished
    ERROR        // Error occurred
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
  std::vector<std::string> _pending_requests;  // Paths to request
  std::string _path_buffer;  // Reusable buffer for path construction

  // Receive buffer management (Temp DB)
  // ====================================
  // Each received message buffer becomes part of the temp DB and cannot be reused.
  // Design constraints:
  // 1. _temp_buffers owns all received message memory (the "temp DB")
  // 2. This memory is freed only after replication completes or errors
  // 3. Subtries within temp DB use relative offsets (wire format)
  // 4. When connecting subtries to persistent storage, offsets must be translated
  //    since absolute offsets from temp DB don't work in persistent storage
  std::vector<std::vector<uint8_t>> _temp_buffers;  // Owned buffers (temp DB memory)
  ReceiveBuffer _receive_buffer;  // Current receive buffer being filled
  size_t _initial_buffer_size;
  static constexpr size_t DEFAULT_RECEIVE_BUFFER_SIZE = 64 * 1024;  // 64KB, grows as needed

  // Temp DB root tracking
  uint8_t* _temp_root;              // Root node of temp DB (first received subtrie)
  TransferNodeType _temp_root_type; // Type of temp DB root

  // Overwrite handler for merge conflicts
  OverwriteHandler _overwrite_handler;

  ReplicationReceiverFSM(DB* db, typename DB::txn_ptr txn,
                         OverwriteHandler handler = OverwriteHandler(),
                         size_t receive_buffer_size = DEFAULT_RECEIVE_BUFFER_SIZE)
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
        _initial_buffer_size(receive_buffer_size),
        _temp_root(nullptr),
        _temp_root_type(TransferNodeType::TRIE_NODE),
        _overwrite_handler(std::move(handler)) {
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

  // Start receiving - called when ready to accept data
  void begin(ReplicationTransport* transport, ReplicationEvents* events) {
    _transport = transport;
    _events = events;
    _state = State::RECEIVING;
    _session_id = 0;  // Will be set from first message
    _total_nodes = 0;
    _total_bytes = 0;
    _error = ReplicationError::NONE;
    _pending_requests.clear();

    // Reset temp buffers from any previous session
    _temp_buffers.clear();
    _alloc_receive_buffer();
    _temp_root = nullptr;

    // Start a transaction for receiving data
    if (!_cursor->start_transaction()) {
      _transition_to_error(ReplicationError::INTERNAL_ERROR,
                          "Failed to start transaction");
    }
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
  // Returns true if a complete message was processed (buffer is reset for next message)
  // Returns false if more data is needed
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
    auto* hdr = reinterpret_cast<const ReplicationMsgHeader*>(_receive_buffer._data);

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

      default:
        _transition_to_error(ReplicationError::INVALID_STATE,
                            "Received message in invalid state");
        break;
    }
  }

  // ==========================================================================
  // Legacy Interface (copies data)
  // ==========================================================================

  // Called by transport layer when a message is received (legacy, copies data)
  void on_message_received(const uint8_t* data, size_t size) {
    Slice payload;
    const auto* hdr = parse_replication_msg(data, size, &payload);

    if (!hdr) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                          "Failed to parse message");
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

    switch (_state) {
      case State::RECEIVING:
        _handle_incoming(msg_type, payload);
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
        _state = State::COMPLETE;
        if (_events) {
          _events->on_complete(_session_id, _total_nodes);
        }
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
    const TransferTrieHeader* hdr = Transfer::parse_header(payload, &subtrie_path);

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
      // Empty transfer - check if we're done
      _maybe_send_requests_or_complete();
      return;
    }

    // Get pointer to the original local node at this path (before we compare)
    const offset_e* original_root = _get_original_root(path);

    // Get pointer to root node in wire buffer (no copy)
    // Note: cast to non-const since we may need to modify offsets in temp DB
    const TransferTrieHeader* transfer_hdr = Transfer::parse_header(payload);
    if (!transfer_hdr || hdr.node_count == 0) {
      _maybe_send_requests_or_complete();
      return;
    }
    uint8_t* wire_root = const_cast<uint8_t*>(Transfer::nodes_data(payload, *transfer_hdr));

    // Get root type from header
    auto wire_root_type = static_cast<TransferNodeType>(hdr.root_node_type);

    // Connect received subtrie to temp DB
    if (path.empty()) {
      // First subtrie - this becomes the temp DB root
      _temp_root = wire_root;
      _temp_root_type = wire_root_type;
    } else {
      // Subsequent subtrie - connect to parent in temp DB
      _connect_subtrie_to_parent(path, wire_root, wire_root_type);
    }

    // Compare wire nodes with local nodes, collect paths where we differ
    _path_buffer.assign(reinterpret_cast<const char*>(path.data()), path.size());
    
    const uint8_t* payload_end = reinterpret_cast<const uint8_t*>(payload.data()) + payload.size();
    _compare_wire_with_local(wire_root, wire_root_type, payload_end, original_root, _path_buffer,
                             nullptr, -1);  // No parent for root

    // After processing, decide what to do next
    _maybe_send_requests_or_complete();
  }

  // Get pointer to the original offset at a path (what we had before receiving)
  // Returns pointer to offset for future relative offset handling compatibility
  const offset_e* _get_original_root(const Slice& path) {
    if (path.empty()) {
      return &_txn->root;
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

    return trans.offset;
  }

  // Connect a received subtrie to its parent in the temp DB
  // path: the full path to this subtrie (e.g., "abc")
  // subtrie_root: pointer to the received subtrie's root node
  // subtrie_type: type of the subtrie root (TRIE_NODE or LEAF_NODE)
  void _connect_subtrie_to_parent(const Slice& path, uint8_t* subtrie_root,
                                  TransferNodeType subtrie_type) {
    if (path.empty() || !_temp_root) return;

    // Navigate temp DB to find the parent's child offset slot
    TempOffset* parent_offset = _find_temp_parent_offset(path);
    if (!parent_offset) {
      // This shouldn't happen - parent should exist in temp DB
      assert(false && "Parent not found in temp DB");
      return;
    }

    // Set the relative offset from parent's offset slot to subtrie root
    // with the correct node type
    NodeTypes type = (subtrie_type == TransferNodeType::LEAF_NODE) ? LEAF : TRIE;
    parent_offset->set_relative(subtrie_root);
    parent_offset->type(type);
  }

  // Navigate temp DB to find the parent's child offset slot for a path
  // path: full path (e.g., "abc"), we find parent at "ab" and return offset for 'c'
  // Returns pointer to the TempOffset slot, or nullptr if not found
  // Uses _Cursor with WireTempDB for navigation (handles compressed paths)
  TempOffset* _find_temp_parent_offset(const Slice& path) {
    if (path.empty() || !_temp_root) return nullptr;
    if (_temp_root_type != TransferNodeType::TRIE_NODE) return nullptr;

    // Create temp root offset with relative pointer to temp_root
    TempOffset temp_root_offset;
    temp_root_offset.set_relative(_temp_root);
    temp_root_offset.type(TRIE);

    // Create cursor with WireTempDB
    WireTempDB wire_db;
    _Cursor<WireTempTraits> cursor(&wire_db, &temp_root_offset);
    
    // Navigate to the path - cursor.find() handles compressed paths
    cursor.find(path);
    
    // After find(), check if we found the parent's offset slot
    // The cursor navigates to where the path would be, with link_idx set
    if (cursor.stack.size == 0) return nullptr;
    
    auto& back = cursor.stack.back();
    
    // If we're on a trie and the key matched up to this point
    // then link() gives us the child offset slot
    if (back.is_trie() && back.link_idx != 0xFFFF) {
      return back.link();
    }
    
    return nullptr;
  }

  // Compare wire node with local node, collect paths where we need more data
  // - If hashes match: local is correct, prune from wire trie (return false)
  // - If hashes differ and wire is leaf: will be merged to local via _Merger
  // - If hashes differ and wire is trie: recurse into children
  // - If child offset == 0 in wire: sender didn't send it, add to pending_requests
  // Returns: true if wire node differs from local (keep it), false if matches (prune it)
  // parent_trie: the parent trie node (for removing matched children)
  // parent_branch_key: the branch key in parent that leads to this node
  bool _compare_wire_with_local(const uint8_t* wire_node, TransferNodeType wire_type,
                                const uint8_t* buffer_end, const offset_e* local_offset,
                                std::string& path,
                                TempTrieNode* parent_trie = nullptr,
                                int parent_branch_key = -1) {
    if (!wire_node || wire_node >= buffer_end) return false;

    // Get wire node's hash
    const uint8_t* wire_hash;
    if (wire_type == TransferNodeType::LEAF_NODE) {
      auto* wire_leaf = reinterpret_cast<const TempLeafNode*>(wire_node);
      wire_hash = wire_leaf->hash;
    } else {
      auto* wire_trie = reinterpret_cast<const TempTrieNode*>(wire_node);
      wire_hash = wire_trie->hash;
    }

    // Compare with local
    if (local_offset && *local_offset) {
      offset_t local_off(*local_offset);
      const uint8_t* local_hash = _get_local_hash(local_off);
      if (local_hash && wire_hash &&
          std::memcmp(wire_hash, local_hash, HASH_SIZE) == 0) {
        // Hashes match - local subtrie is correct, remove from parent and prune
        if (parent_trie && parent_branch_key >= -1) {
          parent_trie->remove_child(parent_branch_key);
        }
        return false;
      }
    }

    // Hashes differ or local doesn't exist
    if (wire_type == TransferNodeType::LEAF_NODE) {
      // Keep leaf - will be merged by _Merger
      return true;
    }

    // Wire is trie node - iterate through children
    // We need to cast away const since we may call remove_child
    auto* wire_trie = const_cast<TempTrieNode*>(reinterpret_cast<const TempTrieNode*>(wire_node));
    TempOffset* wire_array = const_cast<TempOffset*>(wire_trie->array());

    // Collect children to process first, since remove_child modifies the trie
    std::vector<int> children_to_process;
    for (int branch_key = wire_trie->first();
         branch_key != TempTrieNode::OUT_OF_RANGE;
         branch_key = wire_trie->next(branch_key)) {
      children_to_process.push_back(branch_key);
    }

    // Process collected children
    size_t path_len = path.size();
    for (int branch_key : children_to_process) {
      // Get current array index (may change as we remove children)
      int array_idx = wire_trie->array_index(branch_key);
      if (array_idx < 0) continue;  // Already removed
      
      TempOffset& child_offset = wire_trie->array()[array_idx];

      // Check raw _offset to distinguish:
      // - _offset == 0: sender didn't include this child, request it
      // - _offset == PRUNED_MARKER: already matched local, skip
      // - other: valid offset, recurse
      if (child_offset._offset == 0) {
        // Sender didn't include this child - request it
        if (branch_key != TrieNode::NONE) {
          path.push_back(static_cast<char>(branch_key));
        }
        _pending_requests.push_back(path);
        path.resize(path_len);
        continue;
      }
      if (child_offset._offset == PRUNED_MARKER) {
        // Already marked as matching local - skip
        continue;
      }

      // Child was sent - resolve relative offset and recurse
      if (!child_offset.is_relative()) {
        assert(false && "Child offsets in wire format must be relative");
        continue;
      }

      int64_t relative = child_offset.as_signed();
      const uint8_t* child_wire = reinterpret_cast<const uint8_t*>(&child_offset) + relative;
      if (child_wire && child_wire < buffer_end) {
        TransferNodeType child_type = child_offset.type() == LEAF
                                          ? TransferNodeType::LEAF_NODE
                                          : TransferNodeType::TRIE_NODE;

        // Extend path with branch key and recurse
        if (branch_key != TrieNode::NONE) {
          path.push_back(static_cast<char>(branch_key));
        }
        
        // Get corresponding local child by navigating to the extended path
        // (can't use local_trie->array_index because compressed paths may differ)
        const offset_e* local_child = _get_original_root(Slice(path));
        
        bool keep = _compare_wire_with_local(child_wire, child_type, buffer_end, local_child, path,
                                             wire_trie, branch_key);
        // Note: if !keep, the child was already removed by the recursive call
        path.resize(path_len);
      }
    }
    
    // Keep this trie node (it has differing children)
    return true;
  }

  const uint8_t* _get_local_hash(offset_t offset) {
    if (offset.type() == LEAF) {
      auto leaf = _db->template resolve<LeafNode>(&offset);
      return leaf->hash;
    } else {
      auto trie = _db->template resolve<TrieNode>(&offset);
      return trie->hash;
    }
  }

  // Merge temp DB (received wire nodes) into local DB using _Merger
  void _merge_temp_to_local() {
    if (!_temp_root) return;

    // Create temp root offset 
    // Value must be >= 8 to not be masked to 0 by operator uint64_t() (which masks out lower 3 bits)
    TempOffset temp_root_offset;
    temp_root_offset = 8;  // Any value >= 8 works (lower 3 bits are type/flags)
    temp_root_offset.type(_temp_root_type == TransferNodeType::LEAF_NODE ? LEAF : TRIE);

    // Create WireTempDB with root pointer set for special handling
    WireTempDB wire_db;
    wire_db.root_ptr = _temp_root;
    wire_db.root_offset = &temp_root_offset;
    
    // Create cursor for temp DB (source)
    WireCursor src_cursor(&wire_db, &temp_root_offset);

    // Position source cursor at first leaf
    src_cursor.first();

    // Use _Merger to merge wire trie into local DB
    _Merger<LocalCursor, WireCursor, OverwriteHandler> merger(
        *_cursor, src_cursor, _overwrite_handler);
    merger.exec();
  }

  void _maybe_send_requests_or_complete() {
    if (_pending_requests.empty()) {
      // No more paths to request - send COMPLETE
      _send_complete();
    } else {
      // Request children at pending paths
      _send_request_children();
    }
  }

  void _send_request_children() {
    _request_builder.begin(_session_id, DbType::DB_MAIN);

    for (const auto& path : _pending_requests) {
      _request_builder.add_path(path);
    }
    _pending_requests.clear();

    _msg_builder.begin(ReplicationMsgType::REQUEST_CHILDREN, _session_id);
    _msg_builder.append_payload(_request_builder.finalize());
    _transport->send(_msg_builder.data(), _msg_builder.size());
  }

  void _send_complete() {
    // Merge temp DB into local DB using _Merger
    if (_temp_root) {
      _merge_temp_to_local();
    }

    // Commit the transaction after merge
    _cursor->commit();

    // Free temp buffers - data has been integrated into persistent storage
    _free_temp_buffers();

    _msg_builder.begin(ReplicationMsgType::COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    _state = State::COMPLETE;
    if (_events) {
      _events->on_complete(_session_id, _total_nodes);
    }
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
