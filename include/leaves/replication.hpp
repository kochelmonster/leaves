#ifndef _LEAVES_REPLICATION_HPP
#define _LEAVES_REPLICATION_HPP

#include <blake3.h>

#include "db.hpp"
#include "intern/replication/_replication_fsm.hpp"

namespace leaves {

// Simplified replication state for public API
enum class ReplicationState {
  IDLE,    // Not started, or finished successfully
  ACTIVE,  // Replication in progress
  ERROR    // Error occurred
};

// ============================================================================
// ReplicationSender — wraps ReplicationSenderFSM for public use
// ============================================================================
//
// Usage:
//   auto storage = ReplicatingMapStorage::create("path.lvs");
//   auto db = (*storage)["mydb"];
//   // ... insert data, commit ...
//
//   ReplicationSender<ReplicatingMapStorage> sender(db);
//   sender.begin(&transport, &events);
//   // In your event loop: feed responses via on_message_received()
//
template <typename Storage>
class ReplicationSender {
 public:
  using DBImpl = typename Storage::StorageImpl::DB;
  using SenderFSM = ReplicationSenderFSM<DBImpl>;
  using DB = TDB<Storage>;

  explicit ReplicationSender(DB& db) : _fsm(db._internal()) {}

  // Start sending replication data.
  // transport: user-provided transport for sending data to the receiver.
  // events: optional callbacks for completion/error/progress notifications.
  void begin(ReplicationTransport* transport, ReplicationEvents* events,
             DbType db_type = DbType::DB_MAIN) {
    _fsm.begin(transport, events, db_type);
  }

  // Feed a response message from the receiver.
  void on_message_received(const uint8_t* data, size_t size) {
    _fsm.on_message_received(data, size);
  }

  ReplicationState state() const {
    switch (_fsm.state()) {
      case SenderFSM::State::ERROR:
        return ReplicationState::ERROR;
      case SenderFSM::State::IDLE:
        return ReplicationState::IDLE;
      default:
        return ReplicationState::ACTIVE;
    }
  }

  uint64_t session_id() const { return _fsm.session_id(); }
  ReplicationError error() const { return _fsm.error(); }
  size_t bytes_transferred() const { return _fsm._total_bytes; }
  size_t nodes_transferred() const { return _fsm._total_nodes; }

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
//   auto storage = ReplicatingMapStorage::create("path.lvs");
//   auto db = (*storage)["mydb"];
//
//   ReplicationReceiver<ReplicatingMapStorage> receiver(db);
//   receiver.begin(&transport, &events);
//   // In your event loop: write into receive_buffer(), call on_data_received()
//
template <typename Storage>
class ReplicationReceiver {
 public:
  using DBImpl = typename Storage::StorageImpl::DB;
  using ReceiverFSM = ReplicationReceiverFSM<DBImpl>;
  using DB = TDB<Storage>;

  explicit ReplicationReceiver(DB& db) : _fsm(db._internal()) {}

  // Start receiving replication data.
  // transport: user-provided transport for sending responses to the sender.
  // events: optional callbacks for completion/error/progress notifications.
  void begin(ReplicationTransport* transport, ReplicationEvents* events) {
    _fsm.begin(transport, events);
  }

  // Get the receive buffer for zero-copy data reception.
  // Write incoming data to receive_buffer().write_ptr(), then call
  // receive_buffer().advance(bytes_written), then call on_data_received().
  ReceiveBuffer& receive_buffer() { return _fsm.receive_buffer(); }

  // Notify that data has been written to the receive buffer.
  // Returns true if the message was complete and processed.
  bool on_data_received() { return _fsm.on_data_received(); }

  ReplicationState state() const {
    switch (_fsm.state()) {
      case ReceiverFSM::State::ERROR:
        return ReplicationState::ERROR;
      case ReceiverFSM::State::IDLE:
        return ReplicationState::IDLE;
      default:
        return ReplicationState::ACTIVE;
    }
  }

  uint64_t session_id() const { return _fsm.session_id(); }
  ReplicationError error() const { return _fsm.error(); }
  size_t bytes_transferred() const { return _fsm._total_bytes; }
  size_t nodes_transferred() const { return _fsm._total_nodes; }

  std::chrono::steady_clock::time_point last_activity() const {
    return _fsm.last_activity();
  }

 private:
  ReceiverFSM _fsm;
};

// ============================================================================
// Free function: run a complete replication protocol between sender and
// receiver using in-process transports (useful for testing and local sync).
// ============================================================================

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
