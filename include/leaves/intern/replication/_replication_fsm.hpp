#ifndef _LEAVES__REPLICATION_FSM_HPP
#define _LEAVES__REPLICATION_FSM_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
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
  BIG_VALUE_DATA = 0x06,     // Big value data transfer (sender → receiver)
  BIG_VALUE_ACK =
      0x07,  // Big value received acknowledgement (receiver → sender)
  BIG_VALUE_START = 0x08,  // Big value transfer start with total size
                           // (sender → receiver)
};

constexpr uint32_t REPLICATION_MSG_MAGIC = 0x4C565250;  // "LVRP"

// Message envelope wrapping all replication messages
constexpr uint8_t REPLICATION_PROTOCOL_VERSION = 1;

#pragma pack(push, 1)
struct ReplicationMsgHeader {
  boost::endian::little_uint32_t magic;
  uint8_t msg_type;  // ReplicationMsgType
  boost::endian::little_uint64_t session_id;
  boost::endian::little_uint32_t payload_size;
  uint8_t version;      // Protocol version (REPLICATION_PROTOCOL_VERSION)
  uint8_t reserved[6];  // Padding to 24 bytes for 8-byte payload alignment
  // Followed by: payload bytes

  bool is_valid() const {
    return magic == REPLICATION_MSG_MAGIC &&
           version == REPLICATION_PROTOCOL_VERSION;
  }
};
#pragma pack(pop)

static_assert(sizeof(ReplicationMsgHeader) == 24,
              "ReplicationMsgHeader must be 24 bytes");

// Big value data header - sent in BIG_VALUE_DATA messages
// Multiple big values can be batched in one message
#pragma pack(push, 1)
struct BigValueDataHeader {
  boost::endian::little_uint64_t
      wire_chunk_offset;                      // Original offset (lookup key)
  boost::endian::little_uint32_t value_size;  // Size of value data
  // Followed by: value_size bytes of data
};
#pragma pack(pop)

static_assert(sizeof(BigValueDataHeader) == 12,
              "BigValueDataHeader must be 12 bytes");

// Big value start header - sent in BIG_VALUE_START messages
// Announces total count and aligned size so receiver can pre-allocate
#pragma pack(push, 1)
struct BigValueStartHeader {
  boost::endian::little_uint32_t count;  // Number of big values to follow
  boost::endian::little_uint64_t
      total_aligned_size;  // Total size with MAX_PAGE_SIZE alignment
};
#pragma pack(pop)

static_assert(sizeof(BigValueStartHeader) == 12,
              "BigValueStartHeader must be 12 bytes");

// Error codes for ERROR messages
enum class ReplicationError : uint8_t {
  NONE = 0x00,
  SESSION_MISMATCH = 0x01,
  INVALID_MESSAGE = 0x02,
  INVALID_STATE = 0x03,
  INTERNAL_ERROR = 0x04,
  PAYLOAD_TOO_LARGE = 0x05,
  RESOURCE_LIMIT = 0x06,
  STORAGE_FULL = 0x07
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
    hdr->version = REPLICATION_PROTOCOL_VERSION;
    std::memset(hdr->reserved, 0, sizeof(hdr->reserved));
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
    IDLE,                          // Not started
    SENDING,                       // Sending trie data
    AWAITING_RESPONSE,             // Waiting for SUBTRIE_ACK or COMPLETE
    AWAITING_BIG_VALUE_START_ACK,  // Waiting for BIG_VALUE_ACK after START
    SENDING_BIG_VALUES,            // Sending big value data
    AWAITING_BIG_VALUE_ACK,        // Waiting for BIG_VALUE_ACK
    COMPLETE,                      // Replication finished
    ERROR                          // Error occurred
  };

  // Constants for big value streaming
  static constexpr size_t BIG_VALUE_CHUNK_SIZE = 1024 * 1024;  // 1MB per chunk

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

  // Big value streaming state
  size_t _bv_current_idx;     // Current big value index being sent
  size_t _bv_current_offset;  // Offset within current big value's data
  bool _bv_header_sent;       // Whether header for current value was sent
  size_t _bv_header_bytes_sent;  // Bytes of header already sent (for partial)

  // Activity tracking for application-level timeouts
  std::chrono::steady_clock::time_point _last_activity;

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
        _db_type(DbType::DB_MAIN),
        _bv_current_idx(0),
        _bv_current_offset(0),
        _bv_header_sent(false),
        _bv_header_bytes_sent(0),
        _last_activity(std::chrono::steady_clock::now()) {}

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
    _last_activity = std::chrono::steady_clock::now();

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

    _last_activity = std::chrono::steady_clock::now();

    auto msg_type = static_cast<ReplicationMsgType>(hdr->msg_type);

    switch (_state) {
      case State::AWAITING_RESPONSE:
      case State::AWAITING_BIG_VALUE_START_ACK:
      case State::AWAITING_BIG_VALUE_ACK:
        _handle_response(msg_type, payload);
        break;

      case State::SENDING:
      case State::SENDING_BIG_VALUES:
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

  // Time of last received message (for application-level timeout detection)
  std::chrono::steady_clock::time_point last_activity() const {
    return _last_activity;
  }

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

      case ReplicationMsgType::BIG_VALUE_ACK:
        _handle_big_value_ack();
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

    // Check for malformed message (bounds violation during iteration)
    if (iter.error()) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Malformed SUBTRIE_ACK: path length exceeds buffer");
      return;
    }

    // Continue sending remaining nodes
    if (_sender.has_pending()) {
      _state = State::SENDING;
      _send_next_buffer();
    } else if (_sender.has_pending_big_values()) {
      // Trie sync done, now send big value start announcement
      _send_big_value_start();
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

  // Send BIG_VALUE_START message with total count and aligned size
  void _send_big_value_start() {
    const auto& big_values = _sender.pending_big_values();
    if (big_values.empty()) {
      _send_complete();
      return;
    }

    // Calculate total aligned size
    // Each big value in BigMemory is stored as:
    //   _FreeKey header + value_data, aligned to MAX_PAGE_SIZE
    constexpr size_t MAX_PAGE_SIZE =
        Traits::PAGE_SIZES[Traits::PAGE_SIZES_COUNT - 1];
    constexpr size_t FREE_KEY_SIZE = sizeof(_FreeKey);

    uint64_t total_aligned_size = 0;
    for (const auto& bv : big_values) {
      // Each value needs: FreeKey header + value data, aligned to MAX_PAGE_SIZE
      size_t chunk_size = FREE_KEY_SIZE + bv.value_size;
      total_aligned_size += padding(chunk_size, MAX_PAGE_SIZE);
    }

    // Send BIG_VALUE_START message
    _msg_builder.begin(ReplicationMsgType::BIG_VALUE_START, _session_id);
    BigValueStartHeader hdr;
    hdr.count = big_values.size();
    hdr.total_aligned_size = total_aligned_size;
    _msg_builder.append_payload((const uint8_t*)&hdr, sizeof(hdr));
    _transport->send(_msg_builder.data(), _msg_builder.size());

    // Reset streaming state for chunked sending
    _bv_current_idx = 0;
    _bv_current_offset = 0;
    _bv_header_sent = false;
    _bv_header_bytes_sent = 0;
    _state = State::AWAITING_BIG_VALUE_START_ACK;
  }

  // Handle BIG_VALUE_ACK - either continue sending or complete
  void _handle_big_value_ack() {
    const auto& big_values = _sender.pending_big_values();

    if (_state == State::AWAITING_BIG_VALUE_START_ACK) {
      // START was ACKed, begin sending data in chunks
      _state = State::SENDING_BIG_VALUES;
      _send_big_value_chunk();
    } else if (_state == State::AWAITING_BIG_VALUE_ACK) {
      // A chunk was ACKed
      if (_bv_current_idx >= big_values.size()) {
        // All chunks sent, complete
        _sender.clear_pending_big_values();
        _send_complete();
      } else {
        // Send next chunk
        _state = State::SENDING_BIG_VALUES;
        _send_big_value_chunk();
      }
    }
  }

  // Send a chunk of the big value stream (up to BIG_VALUE_CHUNK_SIZE bytes)
  // Stream format: [header1][data1][header2][data2]...
  // Headers and values may span chunk boundaries
  void _send_big_value_chunk() {
    const auto& big_values = _sender.pending_big_values();
    if (_bv_current_idx >= big_values.size()) {
      // All values sent
      _sender.clear_pending_big_values();
      _send_complete();
      return;
    }

    _msg_builder.begin(ReplicationMsgType::BIG_VALUE_DATA, _session_id);

    size_t chunk_bytes = 0;
    while (_bv_current_idx < big_values.size() &&
           chunk_bytes < BIG_VALUE_CHUNK_SIZE) {
      const auto& bv = big_values[_bv_current_idx];

      // First, send the header if not already sent
      if (!_bv_header_sent) {
        auto [data_ptr, size] = _sender.resolve_big_value(bv);

        BigValueDataHeader hdr;
        hdr.wire_chunk_offset = bv.wire_chunk_offset;
        hdr.value_size = size;

        // How much of the header can we send?
        size_t header_remaining = sizeof(hdr) - _bv_header_bytes_sent;
        size_t header_space = BIG_VALUE_CHUNK_SIZE - chunk_bytes;
        size_t header_to_send = std::min(header_remaining, header_space);

        _msg_builder.append_payload(
            (const uint8_t*)&hdr + _bv_header_bytes_sent, header_to_send);
        chunk_bytes += header_to_send;

        if (header_to_send < header_remaining) {
          // Header spans chunk boundary — finish this chunk and
          // resume sending the rest of the header on the next call.
          _bv_header_bytes_sent += header_to_send;
          break;
        }

        _bv_header_sent = true;
        _bv_header_bytes_sent = 0;
        _bv_current_offset = 0;
      }

      // Now send value data
      auto [data_ptr, size] = _sender.resolve_big_value(bv);
      size_t data_remaining = size - _bv_current_offset;
      size_t data_space = BIG_VALUE_CHUNK_SIZE - chunk_bytes;
      size_t data_to_send = std::min(data_remaining, data_space);

      if (data_to_send > 0) {
        _msg_builder.append_payload(data_ptr + _bv_current_offset,
                                    data_to_send);
        chunk_bytes += data_to_send;
        _bv_current_offset += data_to_send;
        _total_bytes += data_to_send;
      }

      // Check if we finished this value
      if (_bv_current_offset >= size) {
        ++_bv_current_idx;
        _bv_current_offset = 0;
        _bv_header_sent = false;
        _bv_header_bytes_sent = 0;
      } else {
        // Value spans to next chunk
        break;
      }
    }

    _transport->send(_msg_builder.data(), _msg_builder.size());

    if (_events) {
      _events->on_progress(_session_id, _total_bytes, _total_nodes);
    }

    _state = State::AWAITING_BIG_VALUE_ACK;
  }

  void _send_complete() {
    // If we just finished the main trie, check for deletion trie
    if (_db_type == DbType::DB_MAIN) {
      if (_start_deletion_phase()) return;
    }

    _msg_builder.begin(ReplicationMsgType::COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());

    if (_events) {
      _events->on_complete(_session_id, _total_nodes);
    }
    _state = State::IDLE;
  }

  // Check if deletion trie needs to be sent; if so, start sending it.
  // Returns true if deletion phase was started.
  bool _start_deletion_phase() {
    if constexpr (requires { _txn->deletion_root; }) {
      if (_txn->deletion_root) {
        _db_type = DbType::DB_DELETION;
        _sender.begin(_db_type);
        _state = State::SENDING;
        _send_next_buffer();
        return true;
      }
    }
    return false;
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
// Inherits free_big from StandardMergePolicy; overrides migrate_big_value for
// wire format
template <typename DstDB>
struct ReplicationMergePolicy : public StandardMergePolicy {
  // Dummy struct for raw pointer resolution
  struct Chunk {};

  using InternalCursor = _Cursor<typename DstDB::CursorTraits>;
  using CursorTraits_ = typename DstDB::CursorTraits;
  using BigMemory = _BigMemory<InternalCursor>;
  using BigValue = typename BigMemory::BigValue;
  using Aspect = typename DstDB::Aspect;
  using CursorContext = typename Aspect::CursorContext;

  // Big value mapping: wire_offset -> offset_t in persistent storage
  const std::unordered_map<uint64_t, offset_t>* big_value_offsets = nullptr;
  DstDB* db = nullptr;

  // Internal cursor pointing at main trie root — set during deletion phase
  // merge so that may_add_leaf can delete keys from main when merging new
  // deletion records.  Uses _Cursor (non-transactional) to avoid conflicting
  // with the already-active write transaction.
  InternalCursor* main_cursor = nullptr;

  // BigMemory for freeing big values when deleting keys from main trie
  BigMemory* bigmemory = nullptr;

  // Aspect-based merge policy (owned by _DB, accessed via pointer)
  Aspect* aspect = nullptr;
  [[no_unique_address]] CursorContext _merge_context;

  // Set the big value mapping (called before merge)
  void set_big_value_storage(
      const std::unordered_map<uint64_t, offset_t>* offsets, DstDB* dst_db) {
    big_value_offsets = offsets;
    db = dst_db;
  }

  // Main trie merge: check aspect before overwriting existing leaf
  bool may_overwrite(const std::string& key, const Slice& dst, const Slice& src,
                     bool dst_is_big, bool src_is_big) {
    if (!main_cursor) {
      // main_cursor == nullptr means we are merging the main trie
      return aspect->may_merge_overwrite(Slice(key), dst, dst_is_big, src,
                                         src_is_big, _merge_context);
    }
    return true;
  }

  // When merging the deletion trie, new deletion keys trigger removal
  // from the main trie within the same transaction.
  // When merging the main trie, delegates to the aspect.
  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    if (main_cursor) {
      // Deletion trie merge — consult may_merge_delete
      // The deletion entry value is [uint64_le timestamp][meta...]
      Slice meta;
      if (src.size() > sizeof(uint64_t)) {
        meta =
            Slice(src.data() + sizeof(uint64_t), src.size() - sizeof(uint64_t));
      }
      if (!aspect->may_merge_delete(Slice(key), meta, _merge_context)) {
        return false;  // aspect rejected this deletion
      }

      main_cursor->find(Slice(key));
      if (main_cursor->is_valid()) {
        // Free big value storage before deleting, otherwise it leaks
        auto& back = main_cursor->stack.back();
        if (back.leaf()->is_big() && bigmemory) {
          BigValue* bvalue = (BigValue*)back.leaf()->vdata();
          bigmemory->free(bvalue);
        }
        main_cursor->remove();
      }
    } else {
      // Main trie merge — consult may_merge_add
      if (!aspect->may_merge_add(Slice(key), src, is_big, _merge_context)) {
        return false;
      }
    }
    return true;
  }

  // Migrate big value from source to destination
  // Returns a MigratedValue with the pre-allocated destination offset
  // Overrides StandardMergePolicy::migrate_big_value for wire format big values
  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor,
                                  DstCursor& dst_cursor) {
    _BigValue* dst_bvalue = &_big_value_storage;

    if (!big_value_offsets) {
      return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
    }

    const auto* bv = reinterpret_cast<const BigValueDataHeader*>(leaf.vdata());
    uint64_t wire_offset = bv->wire_chunk_offset;
    uint32_t value_size = bv->value_size;

    auto it = big_value_offsets->find(wire_offset);
    if (it == big_value_offsets->end()) {
      return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
    }

    // Return a MigratedValue pointing to the _BigValue with pre-allocated
    // destination offset. The data was already copied during
    // _handle_big_value_data
    dst_bvalue->chunk_offset = (uint64_t)it->second;
    dst_bvalue->value_size = value_size;

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }
};

template <typename DB, typename MergePolicy = ReplicationMergePolicy<DB>>
struct ReplicationReceiverFSM {
  using Traits = typename DB::Traits;
  using Transfer = TransferTrie<Traits>;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using offset_e = typename Traits::offset_e;
  using offset_t = leaves::offset_t;
  using WireCursor = _Cursor<WireTempTraits>;
  using TempOffset =
      typename WireTempTraits::offset_e;  // Native offset type for temp DB
  using TempTrieNode =
      _TrieNode<WireTempTraits>;  // Temp DB trie node (native layout)
  using TempLeafNode =
      _LeafNode<WireTempTraits>;  // Temp DB leaf node (native layout)
  using LocalCursor = typename DB::Cursor;

  // Dummy struct for raw data pointers (similar to BigMemory::Chunk)
  struct Chunk {};
  using chunk_ptr = typename Traits::template Pointer<Chunk>;

  enum class State {
    IDLE,                  // Not started
    RECEIVING,             // Receiving and processing trie data
    AWAITING_BIG_VALUES,   // Received BIG_VALUE_START, awaiting data
    RECEIVING_BIG_VALUES,  // Receiving big value data
    COMPLETE,              // Replication finished
    ERROR                  // Error occurred
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
  DbType _current_db_type;  // Tracks which trie we're currently receiving
  std::vector<std::string>
      _prune_paths;          // Paths where hashes match (tell sender to prune)
  std::string _path_buffer;  // Reusable buffer for path construction
  size_t _pending_children;  // Count of 0 offsets not yet connected (receiver
                             // completion tracking)
  size_t _new_leaves;        // Count of new/changed leaf nodes in current
                             // round (must be > 0 before FRACTION_COMPLETE).
  size_t _memory_budget;     // Max temp DB bytes before forcing a fraction
                             // merge. SIZE_MAX = unlimited (single round).
  size_t _max_payload_size;  // Max single-message payload size (reject larger)
  size_t _max_big_value_size;  // Max total big value allocation size

  // Activity tracking for application-level timeouts
  std::chrono::steady_clock::time_point _last_activity;

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

  // Big value storage
  // ====================================
  // Maps wire_chunk_offset (sender's offset) -> offset_t in persistent storage
  // Big values are allocated directly from storage (bypassing transaction) and
  // written as they arrive. The multi-area is linked to the transaction only
  // during merge, keeping transaction lifetime short. Receiver detects
  // completion when all expected big values are received, then merges
  // immediately.
  std::unordered_map<uint64_t, offset_t> _big_value_offsets;
  typename DB::Storage::area_ptr
      _big_value_multi_area;        // Multi-area from storage
  chunk_ptr _big_value_area;        // Pointer to allocated multi_area storage
  offset_t _big_value_area_offset;  // Base offset in persistent storage
  size_t _big_value_area_size;      // Total allocated size
  size_t _big_value_write_pos;      // Current write position in area
  uint32_t _big_value_expected_count;  // Expected count from BIG_VALUE_START
  uint32_t _big_value_received_count;  // Count of big values received so far
  uint32_t _big_value_trie_count;  // Count of big values found in received trie

  // Big value stream parsing state
  // ====================================
  // The sender sends a virtual stream of [header1][data1][header2][data2]...
  // in fixed-size chunks. Headers/values may span chunk boundaries.
  uint8_t _bv_header_buf[sizeof(BigValueDataHeader)];  // Partial header buffer
  size_t _bv_header_pos;             // Bytes of header received so far
  uint64_t _bv_current_wire_offset;  // Current value's wire_chunk_offset
  uint32_t _bv_current_size;         // Current value's total size
  size_t _bv_current_received;       // Bytes received for current value
  bool _bv_parsing_header;  // true = parsing header, false = parsing data

  // Deferred merge state (single-transaction mode)
  // ====================================
  // When the DB supports a deletion trie, the main trie data is deferred
  // (kept in memory without merging) until COMPLETE arrives.  Then both
  // tries are merged in one short atomic transaction.
  char* _deferred_wire_root;         // Saved main trie wire root pointer
  uint8_t _deferred_wire_root_type;  // Type of deferred root (TRIE or LEAF)
  std::vector<std::vector<uint8_t>>
      _deferred_temp_buffers;  // Keeps deferred wire data alive

  // Replication slot — crash-safe tracking of pre-merge multi-areas
  // ====================================
  // The FSM claims one slot in _header->replication_slots[].  The slot
  // holds the offset of the multi-area currently being filled for a big
  // value.  On merge, the area is handed to the transaction and the slot
  // is cleared.  On crash, sanitize() returns all non-zero slots to the pool.
  int16_t _replication_slot;  // -1 = no slot claimed

  // Constants for big value alignment (must match BigMemory)
  static constexpr size_t MAX_PAGE_SIZE =
      Traits::PAGE_SIZES[Traits::PAGE_SIZES_COUNT - 1];
  static constexpr size_t FREE_KEY_SIZE = sizeof(_FreeKey);

  static constexpr size_t DEFAULT_MAX_PAYLOAD_SIZE = 64 * 1024 * 1024;  // 64MB
  static constexpr size_t DEFAULT_MEMORY_BUDGET = 256 * 1024 * 1024;    // 256MB
  static constexpr size_t DEFAULT_MAX_BIG_VALUE_SIZE =
      256 * 1024 * 1024;  // 256MB

  ReplicationReceiverFSM(
      DB* db, typename DB::txn_ptr txn, MergePolicy handler = MergePolicy(),
      size_t receive_buffer_size = DEFAULT_RECEIVE_BUFFER_SIZE,
      size_t memory_budget = DEFAULT_MEMORY_BUDGET,
      size_t max_payload_size = DEFAULT_MAX_PAYLOAD_SIZE,
      size_t max_big_value_size = DEFAULT_MAX_BIG_VALUE_SIZE)
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
        _current_db_type(DbType::DB_MAIN),
        _memory_budget(memory_budget),
        _max_payload_size(max_payload_size),
        _max_big_value_size(max_big_value_size),
        _initial_buffer_size(receive_buffer_size),
        _temp_root(0),
        _wire_cursor(&_wire_db, &_temp_root),
        _merge_policy(std::move(handler)),
        _big_value_multi_area(nullptr),
        _big_value_area(nullptr),
        _big_value_area_offset(0),
        _big_value_area_size(0),
        _big_value_write_pos(0),
        _big_value_expected_count(0),
        _big_value_received_count(0),
        _big_value_trie_count(0),
        _bv_header_pos(0),
        _bv_current_wire_offset(0),
        _bv_current_size(0),
        _bv_current_received(0),
        _bv_parsing_header(true),
        _deferred_wire_root(nullptr),
        _deferred_wire_root_type(0),
        _replication_slot(-1),
        _last_activity(std::chrono::steady_clock::now()) {
    // Allocate first receive buffer
    _alloc_receive_buffer();

    // Wire up the aspect from the DB for merge decisions
    _merge_policy.aspect = &_db->aspect();
    _db->aspect().init_cursor_context(_merge_policy._merge_context);
  }

  ~ReplicationReceiverFSM() { _release_slot(); }

  // Non-copyable, non-movable (owns replication slot)
  ReplicationReceiverFSM(const ReplicationReceiverFSM&) = delete;
  ReplicationReceiverFSM& operator=(const ReplicationReceiverFSM&) = delete;
  ReplicationReceiverFSM(ReplicationReceiverFSM&&) = delete;
  ReplicationReceiverFSM& operator=(ReplicationReceiverFSM&&) = delete;

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
    _new_leaves = 0;
    _big_value_trie_count = 0;
    _last_activity = std::chrono::steady_clock::now();

    // Reset temp buffers from any previous session
    _temp_buffers.clear();
    _alloc_receive_buffer();
    _temp_root = 0;

    // Reset big value storage
    _big_value_offsets.clear();
    _big_value_multi_area = nullptr;
    _big_value_area = nullptr;
    _big_value_area_offset = 0;
    _big_value_area_size = 0;
    _big_value_write_pos = 0;

    // Reset deferred merge state
    _deferred_wire_root = nullptr;
    _deferred_wire_root_type = 0;
    _deferred_temp_buffers.clear();

    // Claim a replication slot for crash-safe area tracking
    _claim_slot();
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

      // Reject messages that exceed the configured payload limit
      size_t payload_size =
          _receive_buffer._expected - sizeof(ReplicationMsgHeader);
      if (payload_size > _max_payload_size) {
        _transition_to_error(ReplicationError::PAYLOAD_TOO_LARGE,
                             "Message payload exceeds size limit");
        return true;
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

    _last_activity = std::chrono::steady_clock::now();

    auto msg_type = static_cast<ReplicationMsgType>(hdr->msg_type);
    Slice payload = _receive_buffer.payload();

    switch (_state) {
      case State::RECEIVING:
      case State::AWAITING_BIG_VALUES:
      case State::RECEIVING_BIG_VALUES:
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

  // Time of last received message (for application-level timeout detection)
  std::chrono::steady_clock::time_point last_activity() const {
    return _last_activity;
  }

  void _handle_incoming(ReplicationMsgType msg_type, const Slice& payload) {
    switch (msg_type) {
      case ReplicationMsgType::TRIE_DATA:
        _handle_trie_data(payload);
        break;

      case ReplicationMsgType::BIG_VALUE_START:
        _handle_big_value_start(payload);
        break;

      case ReplicationMsgType::BIG_VALUE_DATA:
        _handle_big_value_data(payload);
        break;

      case ReplicationMsgType::COMPLETE:
        // Sender indicates sync is complete — merge all deferred and
        // current temp data in one short atomic transaction.
        if (!_merge_all_phases()) break;  // error already reported
        _release_slot();
        _free_temp_buffers();
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

    // Detect transition from main trie to deletion trie
    auto msg_db_type = static_cast<DbType>(hdr->db_type);
    if (msg_db_type != _current_db_type) {
      _transition_db_type(msg_db_type);
    }

    _total_bytes += payload.size();
    _total_nodes += hdr->node_count;

    if (_events) {
      _events->on_progress(_session_id, _total_bytes, _total_nodes);
    }

    // Process nodes: compare hashes and collect paths where we differ
    _process_received_nodes(payload, *hdr, subtrie_path);
  }

  void _handle_big_value_start(const Slice& payload) {
    // Parse BIG_VALUE_START header
    if (payload.size() < sizeof(BigValueStartHeader)) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "BIG_VALUE_START payload too small");
      return;
    }

    const auto* hdr = (const BigValueStartHeader*)payload.data();

    _big_value_expected_count = hdr->count;
    _big_value_received_count = 0;
    uint64_t total_aligned_size = hdr->total_aligned_size;

    // Reject big value allocations that exceed the configured limit
    if (total_aligned_size > _max_big_value_size) {
      _transition_to_error(ReplicationError::RESOURCE_LIMIT,
                           "Big value total size exceeds limit");
      return;
    }

    // Allocate persistent storage directly from storage layer (bypasses
    // transaction) The area is allocated now but only linked to transaction
    // during merge
    static constexpr size_t AREA_SIZE = Traits::AREA_SIZE;

    // Use uint64_t arithmetic to detect overflow before casting to size_t
    uint64_t alloc_size_64 =
        ((total_aligned_size + AREA_SIZE - 1) / AREA_SIZE) * AREA_SIZE;
    if (alloc_size_64 > SIZE_MAX) {
      _transition_to_error(ReplicationError::RESOURCE_LIMIT,
                           "Big value allocation overflow");
      return;
    }
    size_t alloc_size = static_cast<size_t>(alloc_size_64);
    try {
      _big_value_multi_area = _db->_storage.alloc_multi_area(alloc_size);
    } catch (const StorageFull&) {
      _transition_to_error(ReplicationError::STORAGE_FULL,
                           "Storage full during big value allocation");
      return;
    } catch (const std::exception& e) {
      _transition_to_error(ReplicationError::INTERNAL_ERROR, e.what());
      return;
    }

    // Track the newly allocated multi-area in the replication slot
    // so it can be reclaimed on crash before merge completes
    _track_area_in_slot(_big_value_multi_area);

    _big_value_area_offset = _big_value_multi_area->content_offset();
    _big_value_area_size = _big_value_multi_area->end() - _big_value_area_offset;
    _big_value_area =
        _db->template resolve<Chunk>(&_big_value_area_offset, WRITE);
    _big_value_write_pos = 0;
    _big_value_offsets.clear();

    // Reset stream parsing state
    _bv_header_pos = 0;
    _bv_current_wire_offset = 0;
    _bv_current_size = 0;
    _bv_current_received = 0;
    _bv_parsing_header = true;

    _state = State::AWAITING_BIG_VALUES;

    // Send ACK to indicate we're ready to receive data
    _send_big_value_ack();
  }

  void _handle_big_value_data(const Slice& payload) {
    // Parse streaming big value data - format is a continuous stream of
    // [header][data][header][data]... that may span chunk boundaries
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(payload.data());
    const uint8_t* end = ptr + payload.size();

    _state = State::RECEIVING_BIG_VALUES;

    while (ptr < end) {
      if (_bv_parsing_header) {
        // Parse header (may be partial from previous chunk)
        size_t header_needed = sizeof(BigValueDataHeader) - _bv_header_pos;
        size_t header_available = end - ptr;
        size_t header_to_read = std::min(header_needed, header_available);

        std::memcpy(_bv_header_buf + _bv_header_pos, ptr, header_to_read);
        _bv_header_pos += header_to_read;
        ptr += header_to_read;

        if (_bv_header_pos < sizeof(BigValueDataHeader)) {
          // Header incomplete, wait for next chunk
          break;
        }

        // Header complete, parse it
        const auto* hdr = (const BigValueDataHeader*)_bv_header_buf;
        _bv_current_wire_offset = hdr->wire_chunk_offset;
        _bv_current_size = hdr->value_size;
        _bv_current_received = 0;
        _bv_parsing_header = false;

        // Calculate space for this value with MAX_PAGE_SIZE alignment
        size_t chunk_size = FREE_KEY_SIZE + _bv_current_size;
        size_t aligned_size = padding(chunk_size, MAX_PAGE_SIZE);

        // Check that pre-allocated area has enough space
        size_t needed = _big_value_write_pos + aligned_size;
        if (needed > _big_value_area_size) {
          _transition_to_error(ReplicationError::INTERNAL_ERROR,
                               "Big value area overflow");
          return;
        }

        // Write _FreeKey header at the current position in persistent storage
        // _FreeKey layout: { big_uint64_t size, uint64_t offset }
        // The size is the aligned chunk size (header + data + padding)
        // The offset is the chunk's own offset with has_successor flag in bit 0
        char* chunk_ptr = (char*)_big_value_area + _big_value_write_pos;

        // Calculate offset_t for this chunk header
        offset_t header_offset = _big_value_area_offset + _big_value_write_pos;

        // Write the _FreeKey header directly
        _FreeKey* header = (_FreeKey*)chunk_ptr;
        header->size = aligned_size;
        // Set has_successor flag (bit 0) for all chunks except the last one
        // This allows BigMemory to merge adjacent freed chunks
        bool has_successor =
            (_big_value_received_count + 1) < _big_value_expected_count;
        header->offset = header_offset._offset | (has_successor ? 1 : 0);

        // Record mapping: wire_offset -> offset_t in persistent storage
        // The offset points to the data (past _FreeKey header)
        offset_t data_offset = header_offset + FREE_KEY_SIZE;
        _big_value_offsets[_bv_current_wire_offset] = data_offset;
      }

      // Parse data (may be partial)
      size_t data_needed = _bv_current_size - _bv_current_received;
      size_t data_available = end - ptr;
      size_t data_to_read = std::min(data_needed, data_available);

      if (data_to_read > 0) {
        // Copy data after _FreeKey header into persistent storage
        char* chunk_ptr = (char*)_big_value_area + _big_value_write_pos;
        std::memcpy(chunk_ptr + FREE_KEY_SIZE + _bv_current_received, ptr,
                    data_to_read);
        _bv_current_received += data_to_read;
        ptr += data_to_read;
        _total_bytes += data_to_read;
      }

      if (_bv_current_received >= _bv_current_size) {
        // Value complete - advance write position by aligned size
        size_t chunk_size = FREE_KEY_SIZE + _bv_current_size;
        _big_value_write_pos += padding(chunk_size, MAX_PAGE_SIZE);

        ++_big_value_received_count;

        // Reset for next value
        _bv_header_pos = 0;
        _bv_parsing_header = true;
      }
    }

    if (_events) {
      _events->on_progress(_session_id, _total_bytes, _total_nodes);
    }

    // Check if all big values have been received
    if (_big_value_received_count >= _big_value_expected_count) {
      // All big values received — defer main trie data so that
      // _merge_all_phases() at COMPLETE merges everything atomically.
      // Deletion trie data stays in _temp_root for _merge_all_phases().
      if (_current_db_type == DbType::DB_MAIN) {
        _defer_current_temp();
      }
      _state = State::RECEIVING;
      // Send ACK so sender can proceed
      _send_big_value_ack();
      return;
    }

    // Send ACK to sender
    _send_big_value_ack();
  }

  void _send_big_value_ack() {
    _msg_builder.begin(ReplicationMsgType::BIG_VALUE_ACK, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());
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

    // Beginning of a new round — refresh cursor to latest committed
    // state so hash comparisons see previously merged data.
    if (path.empty()) {
      _cursor->update();
      _ensure_cursor_root();
    }

    // Compare wire nodes with local nodes *before* connecting to
    // the temp trie.  If the subtrie root's hash already matches
    // local, _compare_wire_with_local zeroes it and there is
    // nothing to connect (avoids feeding skeleton/identical data
    // to the merger).
    _path_buffer.assign(path.data(), path.size());

    // For the root subtrie, count it as pending so the decrement at
    // _compare_wire_with_local entry balances. Subsequent subtries were
    // already counted as pending children of their parent trie.
    if (path.empty()) {
      ++_pending_children;
    }

    const char* payload_start = payload.data();
    const char* payload_end = payload_start + payload.size();
    _compare_wire_with_local((TempOffset*)&transfer_hdr->root, payload_start,
                             payload_end, _path_buffer);

    // Now connect to temp DB — but only if the root wasn't pruned.
    if (transfer_hdr->root) {
      if (path.empty()) {
        // First subtrie - this becomes the temp DB root
        char* wire_root = transfer_hdr->root.resolve<char>();
        _temp_root.set_relative(wire_root);
        _temp_root.type(transfer_hdr->root.type());
      } else {
        // Subsequent subtrie - connect to parent in temp DB
        _connect_subtrie_to_parent(path, (TempOffset*)&transfer_hdr->root);
      }
    }

    // Check if all pending children are now connected
    if (_pending_children == 0) {
      // All children for this trie phase are resolved.
      // Don't send COMPLETE — let the sender decide completion
      // (the sender may still need to send a deletion trie phase).
      if (_big_value_trie_count == 0 && _temp_root) {
        // Defer main trie data: keep in memory without merging.
        // _merge_all_phases() at COMPLETE will merge everything
        // (main + deletion) in one short atomic transaction.
        // Deletion trie data stays in _temp_root — _merge_all_phases()
        // picks it up as the "current" phase.
        if (_current_db_type == DbType::DB_MAIN) {
          _defer_current_temp();
        }
      }
      // Big values expected - just ACK and wait for BIG_VALUE_START
      _send_prune_ack();
      return;
    }

    // If temp DB exceeds memory budget, merge what we have and ask the
    // sender to restart from root.  The next round's hash comparisons
    // will prune already-replicated subtrees automatically.
    // Guard: at least one new leaf must have been received, otherwise
    // a fraction merge would make no progress and loop forever.
    if (_temp_buffer_memory() > _memory_budget && _new_leaves > 0) {
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
      parent_offset = _find_temp_parent_offset(Slice(_path_buffer));
    }

    if (!parent_offset) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Parent not found in temp DB for subtrie");
      return;
    }

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
  bool _compare_wire_with_local(TempOffset* wire_node, const char* buffer_start,
                                const char* buffer_end, std::string& path) {
    if ((char*)wire_node < buffer_start ||
        (char*)(wire_node + 1) > buffer_end) {
      // wire_node itself is out of bounds — prune
      *wire_node = 0;
      --_pending_children;
      return true;
    }

    --_pending_children;

    // Bounds-check the resolved pointer
    if (*wire_node != 0) {
      char* resolved = wire_node->template resolve<char>();
      if (resolved < buffer_start || resolved >= buffer_end) {
        // Relative offset points outside the buffer — prune
        *wire_node = 0;
        return true;
      }
    }

    // Get wire node's hash
    const uint8_t* wire_hash;
    int branch_key;
    if (wire_node->type() == LEAF) {
      auto* leaf = wire_node->template resolve<TempLeafNode>();
      // Bounds-check the fixed leaf header first
      if ((char*)(leaf + 1) > buffer_end) {
        *wire_node = 0;
        return true;
      }
      // Bounds-check the full leaf node including key and value data
      if ((char*)leaf + leaf->size() > buffer_end) {
        *wire_node = 0;
        return true;
      }
      wire_hash = leaf->hash;
      branch_key = get_branch_key(wire_node);
    } else {
      auto* trie = wire_node->template resolve<TempTrieNode>();
      // Bounds-check the fixed trie header first
      if ((char*)(trie + 1) > buffer_end) {
        *wire_node = 0;
        return true;
      }
      // Bounds-check the full trie node including compressed path and child
      // array
      if ((char*)trie + trie->size() > buffer_end) {
        *wire_node = 0;
        return true;
      }
      wire_hash = trie->hash;
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
        // Hashes match - tell sender to prune, set the offset 0 to avoid
        // merging
        *wire_node = 0;
        _prune_paths.push_back(path);
        path.resize(path_len);
        return true;
      }
    }

    path.resize(path_len);

    // Hashes differ or local doesn't exist
    if (wire_node->type() == LEAF) {
      ++_new_leaves;
      auto* leaf = wire_node->template resolve<TempLeafNode>();
      if (leaf->is_big()) {
        // Bounds-check the BigValueDataHeader within the leaf's value data
        if (leaf->vsize() < sizeof(BigValueDataHeader)) {
          *wire_node = 0;
          return true;
        }
        const auto* bv_hdr =
            reinterpret_cast<const BigValueDataHeader*>(leaf->vdata());
        if (bv_hdr->value_size > _max_big_value_size) {
          *wire_node = 0;
          return true;
        }
        ++_big_value_trie_count;
      }
#ifdef LEAVES_DEBUG
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

    // Bounds-check the trie's child array against the buffer
    if ((char*)(wire_array + count) > buffer_end) {
      // Array extends past buffer — prune the whole trie node
      *wire_node = 0;
      return true;
    }

#ifdef LEAVES_DEBUG
    if (!path.empty()) path.push_back((char)wire_trie->compressed()[0]);
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

      assert(child_offset->is_relative() &&
             "Child offsets in wire format must be relative");

      if (!_compare_wire_with_local(child_offset, buffer_start, buffer_end,
                                    path)) {
        path.resize(path_len);
        return false;
      }
    }
    // Note: partially-pruned skeleton tries (some children zeroed) are
    // safe — _Merger::merge_into_trie() skips zero-offset src branches
    // for both shared and src-only cases.

    // Restore path to original length (remove compressed portion we added)
    path.resize(path_len);
    return true;
  }

  const uint8_t* _get_local_hash(const offset_t* offset) {
    if (offset->type() == LEAF)
      return _db->template resolve<LeafNode>(offset)->hash;

    return _db->template resolve<TrieNode>(offset)->hash;
  }

  // Handle transition from one db_type to another (e.g., DB_MAIN → DB_DELETION)
  // Defers any pending temp data from the previous phase (no merge, no
  // transaction).
  void _transition_db_type(DbType new_db_type) {
    // Defer any pending temp data from the previous phase.
    // No transaction is started — data stays in memory until COMPLETE.
    _defer_current_temp();

    // Reset state for the new phase
    _pending_children = 0;
    _new_leaves = 0;
    _prune_paths.clear();

    _current_db_type = new_db_type;
  }

  // After any cursor operation that calls _set_txn() (update(),
  // start_transaction()), re-set the cursor root if we're in the deletion
  // phase.
  void _ensure_cursor_root() {
    if (_current_db_type == DbType::DB_DELETION) {
      if constexpr (requires { _txn->deletion_root; }) {
        using Transaction = typename DB::Transaction;
        auto* txn = static_cast<Transaction*>(&*_cursor->_txn);
        _cursor->set_root(&txn->deletion_root);
      }
    }
  }

  // Save current temp data to deferred storage without merging.
  // The data stays in memory until _merge_all_phases() is called.
  // Safe to call multiple times — second call is a no-op if _temp_root
  // was already cleared.
  void _defer_current_temp() {
    if (!_temp_root) return;

    // Only one deferred root is supported (one main-to-deletion transition).
    // If this fires, the protocol was changed to allow multiple deferrals.
    assert(_deferred_wire_root == nullptr &&
           "double defer: only one deferred root is supported");

    // Save the resolved root pointer and type before clearing _temp_root.
    // The pointer is into the temp buffer memory which we keep alive.
    _deferred_wire_root = _temp_root.template resolve<char>();
    _deferred_wire_root_type = _temp_root.type();

    // Move temp buffers to deferred storage (memory stays at same address)
    for (auto& buf : _temp_buffers) {
      _deferred_temp_buffers.push_back(std::move(buf));
    }
    _temp_buffers.clear();
    _temp_root = 0;
    _new_leaves = 0;
    _alloc_receive_buffer();
  }

  // Merge all phases (deletion trie + deferred main trie) in one
  // short atomic transaction.  Called at COMPLETE or fraction-complete.
  //
  // Deletion trie is merged FIRST so that any re-inserted keys are
  // restored by the subsequent main trie merge (Phase 2).
  // Returns true on success, false on error (already reported via
  // _transition_to_error).  Callers must check and skip post-merge work.
  bool _merge_all_phases() {
    bool has_deferred = (_deferred_wire_root != nullptr);
    bool has_current = (_temp_root != 0);

    if (!has_deferred && !has_current) return true;

    // Save current temp root before we overwrite _temp_root for deferred
    char* current_wire_root = nullptr;
    uint8_t current_wire_root_type = 0;
    if (has_current) {
      current_wire_root = _temp_root.template resolve<char>();
      current_wire_root_type = _temp_root.type();
    }

    _cursor->start_transaction();
    try {
      // Link pre-allocated big value multi-area to this transaction
      // (must happen before any merge phase that references big values)
      _link_big_value_area();

      // Phase 1: Merge current (deletion trie) data and apply deletions
      if (current_wire_root) {
        // Switch cursor root to deletion trie within the same transaction
        _ensure_cursor_root();

        _temp_root.set_relative(current_wire_root);
        _temp_root.type((NodeTypes)current_wire_root_type);
        _wire_cursor.clear();

        // Only set main_cursor when actually merging deletion trie data.
        // During main-phase fraction merge, current data is main trie —
        // setting main_cursor would incorrectly delete-then-reinsert each key.
        if (_current_db_type == DbType::DB_DELETION) {
          using Transaction = typename DB::Transaction;
          auto* txn = static_cast<Transaction*>(&*_cursor->_txn);
          using CursorTraits_ = typename DB::CursorTraits;
          _Cursor<CursorTraits_> main_cursor(_db, &txn->root);
          using BigMemoryType = _BigMemory<_Cursor<CursorTraits_>>;
          BigMemoryType bigmem(_db, &txn->free_bigmem_root);
          _merge_policy.main_cursor = &main_cursor;
          _merge_policy.bigmemory = &bigmem;

          // Deletion trie has no big values; no need for big value storage
          _Merger<LocalCursor, WireCursor, MergePolicy> merger(
              *_cursor, _wire_cursor, _merge_policy);
          merger.exec();

          _merge_policy.main_cursor = nullptr;
          _merge_policy.bigmemory = nullptr;
        } else {
          _merge_policy.set_big_value_storage(&_big_value_offsets, _db);
          _Merger<LocalCursor, WireCursor, MergePolicy> merger(
              *_cursor, _wire_cursor, _merge_policy);
          merger.exec();
        }
      }

      // Phase 2: Merge deferred (main trie) data
      if (has_deferred) {
        // If Phase 1 switched cursor to deletion trie, switch back to main
        if (current_wire_root) {
          using Transaction = typename DB::Transaction;
          auto* txn = static_cast<Transaction*>(&*_cursor->_txn);
          _cursor->set_root(&txn->root);
        }

        _temp_root.set_relative(_deferred_wire_root);
        _temp_root.type((NodeTypes)_deferred_wire_root_type);
        _wire_cursor.clear();

        _merge_policy.set_big_value_storage(&_big_value_offsets, _db);
        _Merger<LocalCursor, WireCursor, MergePolicy> merger(
            *_cursor, _wire_cursor, _merge_policy);
        merger.exec();

        _deferred_wire_root = nullptr;
        _deferred_wire_root_type = 0;
      }

      _cursor->commit();
    } catch (const StorageFull&) {
      _cursor->rollback();
      _transition_to_error(ReplicationError::STORAGE_FULL,
                           "Storage full during replication merge");
      return false;
    } catch (const std::exception& e) {
      _cursor->rollback();
      _transition_to_error(ReplicationError::INTERNAL_ERROR, e.what());
      return false;
    }

    // Clean up all state
    _temp_root = 0;
    _deferred_temp_buffers.clear();
    _clear_big_value_state();
    return true;
  }

  // Link pre-allocated big value multi-area to the active transaction.
  // Uses _big_value_multi_area directly, then clears the replication slot.
  void _link_big_value_area() {
    if (!_big_value_multi_area) return;

    _big_value_multi_area->next = 0;
    offset_t area_off = _db->resolve(_big_value_multi_area);
    if (_db->_active_txn->area_list_tail_multi) {
      auto tail =
          _db->template resolve<Area>(&_db->_active_txn->area_list_tail_multi);
      tail->next = area_off;
      _db->make_dirty(tail);
    } else {
      _db->_header->area_list_head_multi = area_off;
      _db->make_dirty(_db->_header);
    }
    _db->_active_txn->area_list_tail_multi = area_off;
    _db->make_dirty(_big_value_multi_area);

    // Reset slot to sentinel — area is now transaction-owned but keep the
    // slot claimed so another receiver cannot steal it between rounds.
    // On crash: _sanitize_replication_anchors treats SENTINEL as no-op;
    // the area is reclaimed by Base::sanitize() via return_areas_range.
    if (_replication_slot >= 0) {
      constexpr uint64_t SENTINEL = DB::Header::REPLICATION_SLOT_SENTINEL;
      _db->_header->replication_slots[_replication_slot] = SENTINEL;
      _db->make_dirty(_db->_header);
      _db->flush();
    }
  }

  // Clear big value state after merge
  void _clear_big_value_state() {
    _big_value_offsets.clear();
    _big_value_multi_area = nullptr;
    _big_value_area = nullptr;
    _big_value_area_offset = 0;
    _big_value_area_size = 0;
    _big_value_write_pos = 0;
  }

  // Claim a replication slot in _header->replication_slots[].
  // Uses atomic CAS (0 → sentinel) to claim without file_lock().
  void _claim_slot() {
    constexpr auto N = DB::Header::MAX_REPLICATION_SLOTS;
    constexpr uint64_t SENTINEL = DB::Header::REPLICATION_SLOT_SENTINEL;
    for (uint16_t i = 0; i < N; ++i) {
      auto& slot = _db->_header->replication_slots[i];
      uint64_t expected = 0;
      if (std::atomic_ref<uint64_t>(slot._offset)
              .compare_exchange_strong(expected, SENTINEL,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
        _replication_slot = static_cast<int16_t>(i);
        return;
      }
    }
    // All slots occupied — proceed without crash-safety tracking.
    // This is a soft failure: the session will work but a crash could
    // leak one multi-area.
    _replication_slot = -1;
  }

  // Store the multi-area offset in the claimed slot and flush.
  // Sole-owner operation — no lock needed; atomic store for visibility.
  void _track_area_in_slot(typename DB::Storage::area_ptr area) {
    if (_replication_slot < 0) return;

    auto& slot = _db->_header->replication_slots[_replication_slot];
    offset_t off = _db->resolve(area);
    std::atomic_ref<uint64_t>(slot._offset)
        .store(off._offset, std::memory_order_release);
    _db->make_dirty(_db->_header);
    _db->flush();
  }

  // Release the replication slot.  If it still holds a non-zero offset
  // (error path — area was never merged), return it to the pool first.
  // Sole-owner operation — no lock needed; atomic store for visibility.
  void _release_slot() {
    if (_replication_slot < 0) return;

    constexpr uint64_t SENTINEL = DB::Header::REPLICATION_SLOT_SENTINEL;
    auto& slot = _db->_header->replication_slots[_replication_slot];
    uint64_t raw =
        std::atomic_ref<uint64_t>(slot._offset).load(std::memory_order_acquire);
    if (raw && raw != SENTINEL) {
      offset_t off;
      off._offset = raw;
      _db->_storage.return_multi_areas(off, off);
    }
    if (raw) {
      std::atomic_ref<uint64_t>(slot._offset)
          .store(0, std::memory_order_release);
      _db->make_dirty(_db->_header);
      _db->flush();
    }
    _replication_slot = -1;
  }

  // Merge temp DB (received wire nodes) into local DB using _Merger.
  // Starts and commits its own transaction.  Used for non-ReplicationDB
  // merges and for fraction-complete when no deferred data exists.
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
      std::ofstream dst_out("/tmp/rb-dst-" + std::to_string(_rb_round - 1) +
                            ".yaml");
      _Dumper<DB, true> dst_dumper(*_db, _cursor->_root, false);
      dst_dumper.dump(dst_out);

      std::cerr << "DEBUG: dumped replication buffers to /tmp/rb-"
                << (_rb_round - 1) << ".yaml and rb-dst-" << (_rb_round - 1)
                << ".yaml\n";
    }
#endif

    // set position to root
    _wire_cursor.clear();
    _cursor->start_transaction();
    try {
      _ensure_cursor_root();

      // Link pre-allocated big value multi-area to this transaction
      _link_big_value_area();

      // Set big value storage on merge policy before merging
      _merge_policy.set_big_value_storage(&_big_value_offsets, _db);

      // Use _Merger to merge wire trie into local DB
      _Merger<LocalCursor, WireCursor, MergePolicy> merger(
          *_cursor, _wire_cursor, _merge_policy);
      merger.exec();
      _cursor->commit();
    } catch (const StorageFull&) {
      _cursor->rollback();
      _transition_to_error(ReplicationError::STORAGE_FULL,
                           "Storage full during replication merge");
      return;
    } catch (const std::exception& e) {
      _cursor->rollback();
      _transition_to_error(ReplicationError::INTERNAL_ERROR, e.what());
      return;
    }

    // Clear big value storage after merge (data is in persistent storage)
    _clear_big_value_state();
  }

  // Send ACK with prune paths (where hashes matched)
  // Sender will respond with next subtrie or COMPLETE
  void _send_prune_ack() {
    _request_builder.begin(_session_id, _current_db_type);

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
  // Always commits — the next round needs to see the merged state.
  void _send_fraction_complete() {
    // Merge all accumulated data (deferred + current) in one transaction
    if (!_merge_all_phases()) return;  // error already reported

    // Reset temp DB for the next fraction
    _free_temp_buffers();
    _temp_root = 0;
    _pending_children = 0;
    _new_leaves = 0;
    _big_value_trie_count = 0;
    _prune_paths.clear();

    _msg_builder.begin(ReplicationMsgType::FRACTION_COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());
  }

  void _send_complete() {
    // Merge all accumulated data in one short atomic transaction
    if (!_merge_all_phases()) return;  // error already reported

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

    // Discard deferred data
    _deferred_wire_root = nullptr;
    _deferred_wire_root_type = 0;
    _deferred_temp_buffers.clear();

    // Release slot — returns any un-merged big value areas to the pool
    _release_slot();

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
