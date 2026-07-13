#ifndef _LEAVES_REPLICATION_HPP
#define _LEAVES_REPLICATION_HPP

#include <blake3.h>

#include "db.hpp"
#include "intern/replication/_replication_db.hpp"
#include "intern/replication/_replication_fsm.hpp"
#include "mmap.hpp"

namespace leaves {

// ReplicationState values:
// IDLE means not started or completed,
// ACTIVE means session in progress,
// ERROR means session failure.
enum class ReplicationState {
  IDLE,    // Not started, or finished successfully
  ACTIVE,  // Replication in progress
  ERR      // Error occurred
};

template <typename Traits>
class MapStorage_<Traits>::ReplicationDB
    : public TDB<MapStorage_<Traits>, ::leaves::_ReplicationDB> {
 public:
  using Base = TDB<MapStorage_<Traits>, ::leaves::_ReplicationDB>;
  template <typename AnyStorageImpl>
  using DBImpl = ::leaves::_ReplicationDB<AnyStorageImpl>;
  template <typename AnyStorage>
  using DBWrapper = typename AnyStorage::ReplicationDB;

  ReplicationDB() = default;
  using Base::Base;
};

// ============================================================================
// ReplicationSender — wraps ReplicationSenderFSM for public use
// ============================================================================
//
// Usage:
//   auto storage = MapStorage::create("path.lvs");
//   auto db = storage->open<MapStorage::ReplicationDB>("mydb");
//   // ... insert data, commit ...
//
//   ReplicationSender<MapStorage> sender(db);
//   sender.begin(&transport, &events);
//   // In your event loop: feed responses via on_message_received()
//
template <typename Storage,
          template <typename> class DBClass = _ReplicationDB>
class ReplicationSender {
 public:
  using DBImpl = DBClass<typename Storage::StorageImpl>;
  using SenderFSM = ReplicationSenderFSM<DBImpl>;
  using DB = TDB<Storage, DBClass>;

  explicit ReplicationSender(DB& db) : _fsm(db._internal()) {}

  // Starts replication using transport and optional events, with selected
  // database type.
  void begin(ReplicationTransport* transport, ReplicationEvents* events,
             DbType db_type = DbType::DB_MAIN) {
    _fsm.begin(transport, events, db_type);
  }

  // Feeds receiver responses into the sender FSM.
  void on_message_received(const uint8_t* data, size_t size) {
    _fsm.on_message_received(data, size);
  }

  ReplicationState state() const {
    switch (_fsm.state()) {
      case SenderFSM::State::ERR:
        return ReplicationState::ERR;
      case SenderFSM::State::IDLE:
        return ReplicationState::IDLE;
      default:
        return ReplicationState::ACTIVE;
    }
  }

  // Returns current session identifier.
  uint64_t session_id() const { return _fsm.session_id(); }
  // Returns the last replication error code.
  ReplicationError error() const { return _fsm.error(); }
  // Returns transferred bytes in the current session.
  size_t bytes_transferred() const { return _fsm._total_bytes; }
  // Returns transferred node count in the current session.
  size_t nodes_transferred() const { return _fsm._total_nodes; }

  // Returns timestamp of last FSM activity.
  std::chrono::steady_clock::time_point last_activity() const {
    return _fsm.last_activity();
  }

 private:
  SenderFSM _fsm;
};

// ============================================================================
// ReplicationReceiver — wraps ReplicationReceiverFSM for public use
// ============================================================================
//
// Usage:
//   auto storage = MapStorage::create("path.lvs");
//   auto db = storage->open<MapStorage::ReplicationDB>("mydb");
//
//   ReplicationReceiver<MapStorage> receiver(db);
//   receiver.begin(&transport, &events);
//   // In your event loop: write into receive_buffer(), call on_data_received()
//
template <typename Storage,
          template <typename> class DBClass = _ReplicationDB>
class ReplicationReceiver {
 public:
  using DBImpl = DBClass<typename Storage::StorageImpl>;
  using ReceiverFSM = ReplicationReceiverFSM<DBImpl>;
  using DB = TDB<Storage, DBClass>;

  explicit ReplicationReceiver(DB& db) : _fsm(db._internal()) {}

  // Starts a receiver session with transport and optional event callbacks.
  void begin(ReplicationTransport* transport, ReplicationEvents* events) {
    _fsm.begin(transport, events);
  }

  // Returns mutable internal receive buffer for zero-copy ingest.
  ReceiveBuffer& receive_buffer() { return _fsm.receive_buffer(); }

  // Processes newly received buffer data and returns true if a full message
  // was handled.
  bool on_data_received() { return _fsm.on_data_received(); }

  ReplicationState state() const {
    switch (_fsm.state()) {
      case ReceiverFSM::State::ERR:
        return ReplicationState::ERR;
      case ReceiverFSM::State::IDLE:
        return ReplicationState::IDLE;
      default:
        return ReplicationState::ACTIVE;
    }
  }

  // Returns current session identifier.
  uint64_t session_id() const { return _fsm.session_id(); }
  // Returns the last replication error code.
  ReplicationError error() const { return _fsm.error(); }
  // Returns transferred bytes in the current session.
  size_t bytes_transferred() const { return _fsm._total_bytes; }
  // Returns transferred node count in the current session.
  size_t nodes_transferred() const { return _fsm._total_nodes; }

  // Returns timestamp of last FSM activity.
  std::chrono::steady_clock::time_point last_activity() const {
    return _fsm.last_activity();
  }

 private:
  ReceiverFSM _fsm;
};

// Runs an in-process replication loop until both sides finish, no progress
// remains, or max_rounds is reached.

template <typename Sender, typename Receiver, typename Transport>
void run_replication(Sender& sender, Receiver& receiver,
                     Transport& sender_transport,
                     Transport& receiver_transport,
                     int max_rounds = 1000) {
  int rounds = 0;
  while (rounds < max_rounds) {
    bool activity = false;

    // Deliver messages to receiver
    while (receiver_transport.has_message()) {
      auto msg = receiver_transport.receive();
      auto& buf = receiver.receive_buffer();
      size_t to_copy = std::min(msg.size(), buf.available());
      std::memcpy(buf.write_ptr(), msg.data(), to_copy);
      buf.advance(to_copy);
      receiver.on_data_received();
      activity = true;
    }

    // Deliver messages to sender
    while (sender_transport.has_message()) {
      auto msg = sender_transport.receive();
      sender.on_message_received(msg.data(), msg.size());
      activity = true;
    }

    // Both sides done?
    if (sender.state() != ReplicationState::ACTIVE &&
        receiver.state() != ReplicationState::ACTIVE) {
      break;
    }

    if (!activity) break;
    rounds++;
  }
}

}  // namespace leaves

#endif  // _LEAVES_REPLICATION_HPP
