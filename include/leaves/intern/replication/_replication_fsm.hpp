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
#include "_hash.hpp"
#include "../util/_merger.hpp"
#include "_big_value_receiver.hpp"
#include "_replication_protocol.hpp"
#include "_replication_slot.hpp"
#include "_transfer.hpp"

namespace leaves {

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
  using Transfer = ReplicationTransferTrie<>;

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

  ReplicationSenderFSM(DB* db,
                       size_t buffer_size = Transfer::DEFAULT_MAX_SIZE)
      : _db(db),
        _txn(nullptr),
        _transport(nullptr),
        _events(nullptr),
        _sender(db, typename DB::txn_ptr(nullptr), buffer_size),
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

  ~ReplicationSenderFSM() {
    if (_txn) _db->release_hash_trie(_txn);
  }

  // Start replication
  void begin(ReplicationTransport* transport, ReplicationEvents* events,
             DbType db_type = DbType::DB_MAIN) {
    _transport = transport;
    _events = events;

    // Acquire the hash trie — updates it synchronously if stale, then pins
    // the matching txn for the duration of this replication session.
    _txn = _db->acquire_hash_trie();
    _sender._txn = _txn;
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
        _db->release_hash_trie(_txn);
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
        // Re-acquire hash trie so the sender reads an updated snapshot.
        _db->release_hash_trie(_txn);
        _txn = _db->acquire_hash_trie();
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
    size_t node_count = _sender.fill_buffer();
    Slice buffer = _sender.finalize();

    _total_nodes += node_count;
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

    _db->release_hash_trie(_txn);
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
    _db->release_hash_trie(_txn);
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
    if (!big_value_offsets) {
      return {Slice(), false};
    }

    const auto* bv = reinterpret_cast<const BigValueDataHeader*>(leaf.vdata());
    uint64_t wire_offset = bv->wire_chunk_offset;
    uint32_t value_size = bv->value_size;

    auto it = big_value_offsets->find(wire_offset);
    if (it == big_value_offsets->end()) {
      return {Slice(), false};
    }

    // Fill the inline _BigValue with pre-allocated destination offset.
    // The data was already copied during _handle_big_value_data
    _big_value_storage.chunk_offset = (uint64_t)it->second;
    _big_value_storage.value_size = value_size;

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }
};

template <typename DB, typename MergePolicy = ReplicationMergePolicy<DB>>
struct ReplicationReceiverFSM {
  using Traits = typename DB::Traits;
  using Transfer = ReplicationTransferTrie<>;
  using WireTempTraits = typename Transfer::DBTraits;
  using WireTempDB = typename Transfer::DB;
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
  // Hash trie node types (hash stored outside data trie)
  using HashTraits_ = HashTrieTraits<Traits>;
  using HashTrieNode_ = _TrieNode<HashTraits_>;
  using HashLeafNode_ = _LeafNode<HashTraits_>;
  using BigValueRx = _BigValueReceiver<DB>;

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
  std::vector<std::pair<std::string, bool>>
      _prune_paths;          // Paths where hashes match (tell sender to prune)
                             // pair: (path, is_leaf) — leaf flag prevents the
                             // sender from discarding pending trie continuations
                             // stored at an identical path.
  std::string _path_buffer;  // Reusable buffer for path construction
  size_t _pending_children;  // Count of 0 offsets not yet connected (receiver
                             // completion tracking)
  size_t _new_leaves;        // Count of new/changed leaf nodes in current
                             // round (must be > 0 before FRACTION_COMPLETE).
  size_t _memory_budget;     // Max temp DB bytes before forcing a fraction
                             // merge. SIZE_MAX = unlimited (single round).
  size_t _max_payload_size;  // Max single-message payload size (reject larger)
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

  // Hash trie lookup (owns cursor + root pointer)
  _HashLookup<DB> _hash_lookup;

  // Big value receiver — manages big value streaming and storage
  BigValueRx _big_value;

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
  _ReplicationSlot<DB> _replication_slot;

  static constexpr size_t DEFAULT_MAX_PAYLOAD_SIZE = 64 * 1024 * 1024;  // 64MB
  static constexpr size_t DEFAULT_MEMORY_BUDGET = 256 * 1024 * 1024;    // 256MB
  static constexpr size_t DEFAULT_MAX_BIG_VALUE_SIZE =
      256 * 1024 * 1024;  // 256MB

  ReplicationReceiverFSM(
      DB* db, MergePolicy handler = MergePolicy(),
      size_t receive_buffer_size = DEFAULT_RECEIVE_BUFFER_SIZE,
      size_t memory_budget = DEFAULT_MEMORY_BUDGET,
      size_t max_payload_size = DEFAULT_MAX_PAYLOAD_SIZE,
      size_t max_big_value_size = DEFAULT_MAX_BIG_VALUE_SIZE)
      : _db(db),
        _txn(nullptr),
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
        _initial_buffer_size(receive_buffer_size),
        _temp_root(0),
        _wire_cursor(&_wire_db, &_temp_root),
        _merge_policy(std::move(handler)),
        _hash_lookup(db, &db->_header->hash_control.hash_root),
        _big_value(max_big_value_size),
        _deferred_wire_root(nullptr),
        _deferred_wire_root_type(0),
        _replication_slot(db),
        _last_activity(std::chrono::steady_clock::now()) {
    // Allocate first receive buffer
    _alloc_receive_buffer();

    // Wire up the aspect from the DB for merge decisions
    _merge_policy.aspect = &_db->aspect();
    _db->aspect().init_cursor_context(_merge_policy._merge_context);
  }

  ~ReplicationReceiverFSM() {
    if (_txn) {
      _db->release_hash_trie(_txn);
    }
    _replication_slot.release();
  }

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

    // Acquire the hash trie — updates it synchronously if stale, then pins
    // the matching txn for the duration of this replication session.
    _txn = _db->acquire_hash_trie();
    _state = State::RECEIVING;
    _session_id = 0;  // Will be set from first message
    _total_nodes = 0;
    _total_bytes = 0;
    _error = ReplicationError::NONE;
    _prune_paths.clear();
    _pending_children = 0;
    _new_leaves = 0;
    _last_activity = std::chrono::steady_clock::now();

    // Reset temp buffers from any previous session
    _temp_buffers.clear();
    _alloc_receive_buffer();
    _temp_root = 0;

    // Reset hash lookup for fresh session (root may have changed)
    _hash_lookup.set_root(&_db->_header->hash_control.hash_root);

    // Reset big value state
    _big_value.reset();

    // Reset deferred merge state
    _deferred_wire_root = nullptr;
    _deferred_wire_root_type = 0;
    _deferred_temp_buffers.clear();

    // Claim a replication slot for crash-safe area tracking
    _replication_slot.claim();
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
        _replication_slot.release();
        _free_temp_buffers();
        _db->release_hash_trie(_txn);
        if (_events) {
          _events->on_complete(_session_id, _total_nodes);
        }
        _state = State::IDLE;
        break;

      case ReplicationMsgType::ERROR:
        _db->release_hash_trie(_txn);
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
    auto err = _big_value.handle_start(_db, payload, _replication_slot);
    if (err) {
      _transition_to_error(err.code, err.message);
      return;
    }
    _state = State::AWAITING_BIG_VALUES;
    _send_big_value_ack();
  }

  void _handle_big_value_data(const Slice& payload) {
    size_t bytes_delta = 0;
    bool all_received = false;
    auto err = _big_value.handle_data(payload, bytes_delta, all_received);
    if (err) {
      _transition_to_error(err.code, err.message);
      return;
    }
    _total_bytes += bytes_delta;
    _state = State::RECEIVING_BIG_VALUES;

    if (_events) {
      _events->on_progress(_session_id, _total_bytes, _total_nodes);
    }

    if (all_received) {
      if (_current_db_type == DbType::DB_MAIN) {
        _defer_current_temp();
      }
      _state = State::RECEIVING;
    }
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
    if (!_compare_wire_with_local((TempOffset*)&transfer_hdr->root, payload_start,
                                  payload_end, _path_buffer)) {
      _transition_to_error(ReplicationError::INVALID_MESSAGE,
                           "Wire node offset out of bounds");
      return;
    }

    // Now connect to temp DB — but only if the root wasn't pruned.
    if (transfer_hdr->root) {
      if (path.empty()) {
        // First subtrie - this becomes the temp DB root
        char* wire_root = transfer_hdr->root.resolve<char>();
        _temp_root.set_relative(wire_root);
        _temp_root.type(transfer_hdr->root.type());
      } else {
        // Subsequent subtrie - connect to parent in temp DB.
        // subtrie_path is the parent path; append child's compressed/key
        // for cursor navigation, then restore.
        size_t parent_len = _path_buffer.size();
        if (transfer_hdr->root.type() == TRIE) {
          auto* trie = transfer_hdr->root.template resolve<TempTrieNode>();
          _path_buffer.append((char*)trie->compressed(), trie->len());
        } else {
          auto* leaf = transfer_hdr->root.template resolve<TempLeafNode>();
          auto key = leaf->key();
          _path_buffer.append(key.data(), key.size());
        }
        _connect_subtrie_to_parent(Slice(_path_buffer), (TempOffset*)&transfer_hdr->root);
        _path_buffer.resize(parent_len);
        if (_state == State::ERROR) return;  // temp buffers freed; path is dangling
      }
    }

    // Check if all pending children are now connected
    if (_pending_children == 0) {
      // All children for this trie phase are resolved.
      // Don't send COMPLETE — let the sender decide completion
      // (the sender may still need to send a deletion trie phase).
      if (_big_value.trie_count() == 0 && _temp_root) {
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

  // Connect a received subtrie to its parent in the temp DB
  // path: the full path to this subtrie's position in the global trie.
  //   For a trie child, path already includes the child's compressed bytes
  //   (e.g., "shared_8").  For a NONE-branch leaf, path is the parent trie's
  //   path (e.g., "shared_8") — cursor.find() follows the NONE branch.
  // subtrie_root: pointer to the received subtrie's root node
  void _connect_subtrie_to_parent(const Slice& path, TempOffset* subtrie_root) {
    if (path.empty() || !_temp_root) return;  // _temp_root == 0 means not set

    TempOffset* parent_offset = _find_temp_parent_offset(path);

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

  // Handle a wire leaf node: bounds-check, hash compare, prune/mismatch.
  // Only adds to _prune_paths for big-value leaves — small leaves are fully
  // contained in wire data so there is nothing for the sender to skip.
  // Called after _compare_wire_with_local has validated wire_node pointer and
  // resolved-pointer bounds.
  bool _compare_leaf_wire_with_local(TempOffset* wire_node,
                                     const char* buffer_end,
                                     std::string& path) {
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

    size_t path_len = path.size();
    const uint8_t* wire_hash = leaf->hash;
    {
      auto key = leaf->key();
      path.append(key.data(), key.size());
    }

    // Compare hash from hash trie at this path
    const uint8_t* local_hash = _hash_lookup.find(path, LEAF);
    if (local_hash && std::memcmp(wire_hash, local_hash, HASH_SIZE) == 0) {
      // Hashes match — zero wire node to avoid merging.
      // Only ACK big-value leaves so the sender prunes the pending big
      // value data.  Small leaves are fully contained in the wire message
      // so pruning them has no effect on the sender.
      bool is_big = leaf->is_big();
      *wire_node = 0;
      if (is_big) {
        _prune_paths.emplace_back(path, true);
      }
      path.resize(path_len);
      return true;
    }

    // Hashes differ or local doesn't exist — leaf will be merged
    path.resize(path_len);
    ++_new_leaves;
    if (leaf->is_big()) {
      // Bounds-check the BigValueDataHeader within the leaf's value data
      if (leaf->vsize() < sizeof(BigValueDataHeader)) {
        *wire_node = 0;
        return true;
      }
      const auto* bv_hdr =
          reinterpret_cast<const BigValueDataHeader*>(leaf->vdata());
      if (bv_hdr->value_size > _big_value._max_size) {
        *wire_node = 0;
        return true;
      }
      _big_value.increment_trie_count();
    }
    return true;
  }

  // Handle a wire trie node: bounds-check, hash compare, prune or recurse.
  // Always adds to _prune_paths on hash match (trie ACK tells sender to
  // prune entire subtree).
  // Called after _compare_wire_with_local has validated wire_node pointer and
  // resolved-pointer bounds.
  bool _compare_trie_wire_with_local(TempOffset* wire_node,
                                     const char* buffer_start,
                                     const char* buffer_end,
                                     std::string& path) {
    auto* trie = wire_node->template resolve<TempTrieNode>();
    // Bounds-check the fixed trie header first
    if ((char*)(trie + 1) > buffer_end) {
      *wire_node = 0;
      return true;
    }
    // Bounds-check the full trie node including compressed path and child array
    if ((char*)trie + trie->size() > buffer_end) {
      *wire_node = 0;
      return true;
    }

    size_t path_len = path.size();
    const uint8_t* wire_hash = trie->hash;
    path.append((char*)trie->compressed(), trie->len());

    // Compare hash from hash trie at this path
    const uint8_t* local_hash = _hash_lookup.find(path, TRIE);
    if (local_hash && std::memcmp(wire_hash, local_hash, HASH_SIZE) == 0) {
      // Hashes match — zero wire node and tell sender to prune subtree
      *wire_node = 0;
      _prune_paths.emplace_back(path, false);
      path.resize(path_len);
      return true;
    }

    // Hashes differ — iterate through children
    TempOffset* wire_array = trie->array();
    int count = trie->count();

    // Bounds-check the trie's child array against the buffer
    if ((char*)(wire_array + count) > buffer_end) {
      // Array extends past buffer — prune the whole trie node
      *wire_node = 0;
      path.resize(path_len);
      return true;
    }

    // All children start as pending; decrement as each is handled
    _pending_children += count;

    // path already includes the full compressed prefix (appended above
    // during hash lookup).  Children will append their own key/compressed.

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

  // Compare wire node with local node, collect paths where we need more data.
  // Common preamble: bounds-check wire_node, decrement _pending_children,
  // validate resolved pointer.  Dispatches to leaf/trie-specific methods.
  bool _compare_wire_with_local(TempOffset* wire_node, const char* buffer_start,
                                const char* buffer_end, std::string& path) {
    if ((char*)wire_node < buffer_start ||
        (char*)(wire_node + 1) > buffer_end) {
      // wire_node itself is out of bounds — malformed message from sender
      return false;
    }

    --_pending_children;

    // Bounds-check the resolved pointer
    if (*wire_node == 0) {
      return true;  // No node — nothing to compare
    }
    char* resolved = wire_node->template resolve<char>();
    if (resolved < buffer_start || resolved >= buffer_end) {
      // Relative offset points outside the buffer — malformed message
      return false;
    }

    if (wire_node->type() == LEAF) {
      return _compare_leaf_wire_with_local(wire_node, buffer_end, path);
    }
    return _compare_trie_wire_with_local(wire_node, buffer_start, buffer_end,
                                         path);
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

    // Point hash lookup at the correct hash trie root for the new db type
    auto& hc = _db->_header->hash_control;
    auto* root = (new_db_type == DbType::DB_DELETION)
                     ? &hc.deletion_hash_root
                     : &hc.hash_root;
    _hash_lookup.set_root(root);
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

  // Run merger with parallel dispatch when the storage supports it.
  void _exec_merger_parallel() {
#if LEAVES_HAS_THREADS
    if constexpr (Traits::MERGE_POOL_THREADS > 0) {
      _PoolExecutor exec(_db->_storage, Traits::MERGE_POOL_THREADS);
      _TaskGroup<_PoolExecutor> tg(exec);
      tg._concurrency = Traits::MERGE_DISPATCH_THRESHOLD;
      _Merger<LocalCursor, WireCursor, MergePolicy, _PoolExecutor> merger(
          *_cursor, _wire_cursor, _merge_policy);
      merger._tg = &tg;
      merger.exec();
      return;
    }
#endif
    _Merger<LocalCursor, WireCursor, MergePolicy> merger(
        *_cursor, _wire_cursor, _merge_policy);
    merger.exec();
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
      _big_value.link_area(_db, _replication_slot);

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
          _merge_policy.set_big_value_storage(&_big_value._offsets, _db);
          _exec_merger_parallel();
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

        _merge_policy.set_big_value_storage(&_big_value._offsets, _db);
        _exec_merger_parallel();

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
    _big_value.clear();
    return true;
  }

  // Send ACK with prune paths (where hashes matched)
  // Sender will respond with next subtrie or COMPLETE
  void _send_prune_ack() {
    _request_builder.begin(_session_id, _current_db_type);

    // Add all paths where hashes matched (sender should prune these)
    for (const auto& [path, is_leaf] : _prune_paths) {
      _request_builder.add_path(path, is_leaf);
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
    // Merge all accumulated data (deferred + current) in one transaction.
    if (!_merge_all_phases()) return;  // error already reported

    // Re-acquire hash trie so next round sees the freshly merged state.
    _db->release_hash_trie(_txn);
    _txn = _db->acquire_hash_trie();

    // Reset hash lookup cursor — the old hash trie pages may have been
    // freed/reused, so the cursor's keep_stack() cache is stale.
    _hash_lookup.set_root(&_db->_header->hash_control.hash_root);

    // Reset temp DB for the next fraction
    _free_temp_buffers();
    _temp_root = 0;
    _pending_children = 0;
    _new_leaves = 0;
    _big_value.reset_trie_count();
    _prune_paths.clear();
    _msg_builder.begin(ReplicationMsgType::FRACTION_COMPLETE, _session_id);
    _transport->send(_msg_builder.data(), _msg_builder.size());
  }

  void _transition_to_error(ReplicationError error, const char* reason) {
    _db->release_hash_trie(_txn);
    _state = State::ERROR;
    _error = error;

    // Discard deferred data
    _deferred_wire_root = nullptr;
    _deferred_wire_root_type = 0;
    _deferred_temp_buffers.clear();

    // Release slot — returns any un-merged big value areas to the pool
    _replication_slot.release();

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
