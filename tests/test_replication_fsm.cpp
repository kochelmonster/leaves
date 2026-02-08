#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE ReplicationFSMTest

#include <boost/test/included/unit_test.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <vector>

#define LEAVES_DEBUG

#ifndef TESTING
#define TESTING
#endif

#include "leaves/replicating_mmap.hpp"
#include "leaves/intern/db/_check.hpp"
#include "leaves/intern/replication/_replication_fsm.hpp"

using namespace leaves;

// Use replicating map storage for testing
using Storage = ReplicatingMapStorage;
using DBImpl = Storage::StorageImpl::DB;
using SenderFSM = ReplicationSenderFSM<DBImpl>;
using ReceiverFSM = ReplicationReceiverFSM<DBImpl>;

// =============================================================================
// Test Transport - connects sender and receiver directly
// =============================================================================

struct TestTransport : ReplicationTransport {
  std::queue<std::vector<uint8_t>> _incoming;
  TestTransport* _peer = nullptr;

  void set_peer(TestTransport* peer) { _peer = peer; }

  void send(const uint8_t* data, size_t size) override {
    if (_peer) {
      _peer->_incoming.emplace(data, data + size);
    }
  }

  bool has_message() const { return !_incoming.empty(); }

  std::vector<uint8_t> receive() {
    if (_incoming.empty()) return {};
    auto msg = std::move(_incoming.front());
    _incoming.pop();
    return msg;
  }
};

// =============================================================================
// Test Events
// =============================================================================

struct TestEvents : ReplicationEvents {
  bool completed = false;
  bool errored = false;
  uint64_t session_id = 0;
  size_t nodes = 0;
  size_t progress_calls = 0;
  ReplicationError error = ReplicationError::NONE;
  std::string error_reason;

  void on_complete(uint64_t sid, size_t nodes_transferred) override {
    completed = true;
    session_id = sid;
    nodes = nodes_transferred;
  }

  void on_error(uint64_t sid, ReplicationError err,
                const char* reason) override {
    errored = true;
    session_id = sid;
    error = err;
    error_reason = reason ? reason : "";
  }

  void on_progress(uint64_t sid, size_t bytes, size_t nodes_count) override {
    session_id = sid;
    progress_calls++;
  }

  void reset() {
    completed = false;
    errored = false;
    session_id = 0;
    nodes = 0;
    progress_calls = 0;
    error = ReplicationError::NONE;
    error_reason.clear();
  }
};

// =============================================================================
// Test Fixture
// =============================================================================

struct ReplicationFixture {
  std::filesystem::path test_temp_dir;

  ReplicationFixture() {
    test_temp_dir =
        std::filesystem::temp_directory_path() / "test_replication_fsm";
    std::filesystem::remove_all(test_temp_dir);
    std::filesystem::create_directory(test_temp_dir);
  }

  ~ReplicationFixture() { std::filesystem::remove_all(test_temp_dir); }

  // Run the FSM protocol until both sides complete or error
  static void run_protocol(SenderFSM& sender, ReceiverFSM& receiver,
                           TestTransport& sender_transport,
                           TestTransport& receiver_transport,
                           int max_rounds = 100) {
    int rounds = 0;
    while (rounds < max_rounds) {
      bool activity = false;

      // Process messages for receiver
      while (receiver_transport.has_message()) {
        auto msg = receiver_transport.receive();

        // Use zero-copy interface
        auto& buf = receiver.receive_buffer();
        size_t to_copy = std::min(msg.size(), buf.available());
        std::memcpy(buf.write_ptr(), msg.data(), to_copy);
        buf.advance(to_copy);
        receiver.on_data_received();

        activity = true;
      }

      // Process messages for sender
      while (sender_transport.has_message()) {
        auto msg = sender_transport.receive();
        sender.on_message_received(msg.data(), msg.size());
        activity = true;
      }

      // Check if both are done
      if ((sender.state() == SenderFSM::State::IDLE ||
           sender.state() == SenderFSM::State::ERROR) &&
          (receiver.state() == ReceiverFSM::State::IDLE ||
           receiver.state() == ReceiverFSM::State::ERROR)) {
        break;
      }

      if (!activity) {
        // No messages to process - protocol stuck
        break;
      }

      rounds++;
    }
  }
};

// =============================================================================
// Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(ReplicationFSMTests)

BOOST_AUTO_TEST_CASE(test_message_header_size) {
  static_assert(sizeof(ReplicationMsgHeader) == 24);
  BOOST_CHECK_EQUAL(sizeof(ReplicationMsgHeader), 24);
}

BOOST_AUTO_TEST_CASE(test_message_builder_parser) {
  ReplicationMsgBuilder builder;
  builder.begin(ReplicationMsgType::TRIE_DATA, 0x123456789ABCDEF0ULL);

  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  builder.append_payload(payload, sizeof(payload));

  Slice result = builder.finalize();
  BOOST_CHECK_EQUAL(result.size(),
                    sizeof(ReplicationMsgHeader) + sizeof(payload));

  Slice parsed_payload;
  const auto* hdr =
      parse_replication_msg(reinterpret_cast<const uint8_t*>(result.data()),
                            result.size(), &parsed_payload);

  BOOST_REQUIRE(hdr);
  BOOST_CHECK(hdr->is_valid());
  BOOST_CHECK_EQUAL(hdr->msg_type,
                    static_cast<uint8_t>(ReplicationMsgType::TRIE_DATA));
  BOOST_CHECK_EQUAL(hdr->session_id, 0x123456789ABCDEF0ULL);
  BOOST_CHECK_EQUAL(hdr->payload_size, sizeof(payload));
  BOOST_CHECK_EQUAL(parsed_payload.size(), sizeof(payload));
  BOOST_CHECK_EQUAL(
      std::memcmp(parsed_payload.data(), payload, sizeof(payload)), 0);
}

BOOST_FIXTURE_TEST_CASE(test_empty_db_replication, ReplicationFixture) {
  auto sender_path = test_temp_dir / "sender_empty.lvs";
  auto receiver_path = test_temp_dir / "receiver_empty.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();

  TestTransport sender_transport, receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events, receiver_events;

  SenderFSM sender(sender_impl, sender_impl->txn());
  ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

  receiver.begin(&receiver_transport, &receiver_events);
  sender.begin(&sender_transport, &sender_events);

  run_protocol(sender, receiver, sender_transport, receiver_transport);

  BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
  BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);
}

BOOST_FIXTURE_TEST_CASE(test_single_key_replication, ReplicationFixture) {
  auto sender_path = test_temp_dir / "sender_single.lvs";
  auto receiver_path = test_temp_dir / "receiver_single.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  // Insert a key in sender
  {
    auto cursor = sender_db.cursor();
    cursor.find(Slice("hello"));
    cursor.value(Slice("world"));
    cursor.commit();
  }

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();

  TestTransport sender_transport, receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events, receiver_events;

  SenderFSM sender(sender_impl, sender_impl->txn());
  ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

  receiver.begin(&receiver_transport, &receiver_events);
  sender.begin(&sender_transport, &sender_events);

  run_protocol(sender, receiver, sender_transport, receiver_transport);

  BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
  BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);
  BOOST_CHECK_GE(sender_events.nodes, 1);

  // Verify data was actually replicated
  {
    auto cursor = receiver_db.cursor();
    cursor.find(Slice("hello"));
    BOOST_REQUIRE_MESSAGE(cursor.is_valid(),
                          "Key 'hello' not found in receiver");
    BOOST_CHECK_MESSAGE(cursor.value() == Slice("world"),
                        "Value mismatch for 'hello'");
  }
}

BOOST_FIXTURE_TEST_CASE(test_multiple_keys_replication, ReplicationFixture) {
  auto sender_path = test_temp_dir / "sender_multi.lvs";
  auto receiver_path = test_temp_dir / "receiver_multi.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  // Insert multiple keys in sender
  {
    auto cursor = sender_db.cursor();
    cursor.find(Slice("aaa"));
    cursor.value(Slice("value_a"));
    cursor.find(Slice("bbb"));
    cursor.value(Slice("value_b"));
    cursor.find(Slice("ccc"));
    cursor.value(Slice("value_c"));
    cursor.commit();
  }

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();

  TestTransport sender_transport, receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events, receiver_events;

  SenderFSM sender(sender_impl, sender_impl->txn());
  ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

  receiver.begin(&receiver_transport, &receiver_events);
  sender.begin(&sender_transport, &sender_events);

  run_protocol(sender, receiver, sender_transport, receiver_transport);

  BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
  BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);
  BOOST_CHECK_GE(sender_events.nodes, 3);

  // Verify data was actually replicated
  {
    auto cursor = receiver_db.cursor();

    cursor.find(Slice("aaa"));
    BOOST_REQUIRE_MESSAGE(cursor.is_valid(), "Key 'aaa' not found in receiver");
    BOOST_CHECK_MESSAGE(cursor.value() == Slice("value_a"),
                        "Value mismatch for 'aaa'");

    cursor.find(Slice("bbb"));
    BOOST_REQUIRE_MESSAGE(cursor.is_valid(), "Key 'bbb' not found in receiver");
    BOOST_CHECK_MESSAGE(cursor.value() == Slice("value_b"),
                        "Value mismatch for 'bbb'");

    cursor.find(Slice("ccc"));
    BOOST_REQUIRE_MESSAGE(cursor.is_valid(), "Key 'ccc' not found in receiver");
    BOOST_CHECK_MESSAGE(cursor.value() == Slice("value_c"),
                        "Value mismatch for 'ccc'");
  }
}

BOOST_FIXTURE_TEST_CASE(test_session_id_mismatch, ReplicationFixture) {
  auto sender_path = test_temp_dir / "sender_mismatch.lvs";
  auto receiver_path = test_temp_dir / "receiver_mismatch.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();

  TestTransport sender_transport, receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events, receiver_events;

  SenderFSM sender(sender_impl, sender_impl->txn());
  ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

  receiver.begin(&receiver_transport, &receiver_events);
  sender.begin(&sender_transport, &sender_events);

  // Inject a message with wrong session ID
  ReplicationMsgBuilder bad_msg;
  bad_msg.begin(ReplicationMsgType::COMPLETE, 0xBADBADBADBADBAD);
  sender.on_message_received(bad_msg.data(), bad_msg.size());

  BOOST_CHECK(sender.state() == SenderFSM::State::ERROR);
  BOOST_CHECK(sender.error() == ReplicationError::SESSION_MISMATCH);
  BOOST_CHECK(sender_events.errored);
}

BOOST_FIXTURE_TEST_CASE(test_cross_buffer_subtrie, ReplicationFixture) {
  auto sender_path = test_temp_dir / "sender_crossbuf.lvs";
  auto receiver_path = test_temp_dir / "receiver_crossbuf.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  // Insert many keys to create a large trie that won't fit in small buffer
  {
    auto cursor = sender_db.cursor();
    for (int i = 0; i < 100; ++i) {
      std::string key = "key_" + std::to_string(i);
      std::string value = "value_" + std::to_string(i) +
                          "_with_extra_padding_to_make_it_larger";
      cursor.find(Slice(key));
      cursor.value(Slice(value));
    }
    cursor.commit();
  }

  // Dump sender_db for debugging
  {
    std::ofstream out("/tmp/sender.yaml");
    _Dumper dumper(sender_db, &sender_db._internal()->txn()->root, false);
    dumper.dump(out);
  }

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();
  TestTransport sender_transport, receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events, receiver_events;

  // Use a small buffer size to force multiple round trips
  constexpr size_t SMALL_BUFFER = 512;

  SenderFSM sender(sender_impl, sender_impl->txn(), SMALL_BUFFER);
  ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

  receiver.begin(&receiver_transport, &receiver_events);
  sender.begin(&sender_transport, &sender_events);

  run_protocol(sender, receiver, sender_transport, receiver_transport, 200);

  BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
  BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);

  // Verify all keys were replicated
  {
    auto cursor = receiver_db.cursor();
    // auto cursor = sender_db.cursor();
    int found = 0;
    for (int i = 0; i < 100; ++i) {
      std::string key = "key_" + std::to_string(i);
      std::string expected = "value_" + std::to_string(i) +
                             "_with_extra_padding_to_make_it_larger";
      cursor.find(Slice(key));
      if (cursor.is_valid() && cursor.value() == Slice(expected)) {
        ++found;
      }
    }
    BOOST_CHECK_EQUAL(found, 100);
  }
}

BOOST_FIXTURE_TEST_CASE(test_differential_update, ReplicationFixture) {
  auto sender_path = test_temp_dir / "sender_diff.lvs";
  auto receiver_path = test_temp_dir / "receiver_diff.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  // Create a larger dataset with common data distributed throughout the tree
  // This ensures pruning happens across multiple buffer transmissions
  
  // Insert common keys with various prefixes to spread them across trie
  // Use larger values to ensure we need multiple buffers
  {
    auto sender_cursor = sender_db.cursor();
    auto receiver_cursor = receiver_db.cursor();

    for (int i = 0; i < 100; ++i) {
      std::string key = "common_" + std::to_string(i);
      // Make values large enough to force multiple buffers
      std::string value = "shared_value_" + std::to_string(i) + 
                         "_padding_to_increase_size_" + std::string(200, 'x');
      sender_cursor.find(Slice(key));
      sender_cursor.value(Slice(value));
      receiver_cursor.find(Slice(key));
      receiver_cursor.value(Slice(value));
    }
    sender_cursor.commit();
    receiver_cursor.commit();
  }

  // Insert keys that are DIFFERENT (sender has, receiver doesn't)
  // Spread these throughout the tree
  {
    auto cursor = sender_db.cursor();
    for (int i = 0; i < 50; ++i) {
      std::string key = "sender_only_" + std::to_string(i);
      std::string value = "new_value_" + std::to_string(i) + std::string(200, 'y');
      cursor.find(Slice(key));
      cursor.value(Slice(value));
    }
    cursor.commit();
  }

  // Insert keys that receiver has but sender doesn't (should remain after sync)
  {
    auto cursor = receiver_db.cursor();
    for (int i = 0; i < 30; ++i) {
      std::string key = "receiver_only_" + std::to_string(i);
      std::string value = "local_value_" + std::to_string(i) + std::string(200, 'z');
      cursor.find(Slice(key));
      cursor.value(Slice(value));
    }
    cursor.commit();
  }

  // Insert a key with SAME name but DIFFERENT value (should be overwritten)
  {
    auto sender_cursor = sender_db.cursor();
    sender_cursor.find(Slice("conflict_key"));
    sender_cursor.value(Slice("sender_wins"));
    sender_cursor.commit();

    auto receiver_cursor = receiver_db.cursor();
    receiver_cursor.find(Slice("conflict_key"));
    receiver_cursor.value(Slice("receiver_loses"));
    receiver_cursor.commit();
  }

  // Dump sender_db for debugging
  {
    std::ofstream out("/tmp/sender.yaml");
    _Dumper dumper(sender_db, &sender_db._internal()->txn()->root, false);
    dumper.dump(out);
  }

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();

  TestTransport sender_transport, receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events, receiver_events;

  // Use a small buffer size to force multiple round trips and test pruning
  constexpr size_t SMALL_BUFFER = 8192;  // 8KB buffer forces multiple transmissions
  SenderFSM sender(sender_impl, sender_impl->txn(), SMALL_BUFFER);
  ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

  receiver.begin(&receiver_transport, &receiver_events);
  sender.begin(&sender_transport, &sender_events);

  run_protocol(sender, receiver, sender_transport, receiver_transport, 500);

  BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
  BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);

  // Verify progress callbacks were made (indicating multiple buffer transmissions)
  BOOST_CHECK_GT(sender_events.progress_calls, 0);

  // Verify results in receiver
  {
    auto cursor = receiver_db.cursor();

    {
      std::ofstream out("/tmp/receiver.yaml");
      _Dumper dumper(receiver_db, &receiver_db._internal()->txn()->root, false);
      dumper.dump(out);
    }

    // Common keys should still exist with same values
    for (int i = 0; i < 100; ++i) {
      std::string key = "common_" + std::to_string(i);
      std::string expected = "shared_value_" + std::to_string(i) + 
                            "_padding_to_increase_size_" + std::string(200, 'x');
      cursor.find(Slice(key));
      BOOST_REQUIRE_MESSAGE(cursor.is_valid(), "Common key missing: " + key);
      BOOST_CHECK_MESSAGE(cursor.value() == Slice(expected),
                          "Common key value mismatch: " + key);
    }

    // Sender-only keys should now exist in receiver
    for (int i = 0; i < 50; ++i) {
      std::string key = "sender_only_" + std::to_string(i);
      std::string expected = "new_value_" + std::to_string(i) + std::string(200, 'y');
      cursor.find(Slice(key));
      BOOST_REQUIRE_MESSAGE(cursor.is_valid(),
                            "Sender-only key not replicated: " + key);
      BOOST_CHECK_MESSAGE(cursor.value() == Slice(expected),
                          "Sender-only key value mismatch: " + key);
    }

    // Receiver-only keys should still exist (merge, not replace)
    for (int i = 0; i < 30; ++i) {
      std::string key = "receiver_only_" + std::to_string(i);
      std::string expected = "local_value_" + std::to_string(i) + std::string(200, 'z');
      cursor.find(Slice(key));
      BOOST_REQUIRE_MESSAGE(cursor.is_valid(),
                            "Receiver-only key was deleted: " + key);
      BOOST_CHECK_MESSAGE(cursor.value() == Slice(expected),
                          "Receiver-only key value changed: " + key);
    }

    // Conflict key should have sender's value (sender wins)
    cursor.find(Slice("conflict_key"));
    BOOST_REQUIRE_MESSAGE(cursor.is_valid(), "Conflict key missing");
    BOOST_CHECK_MESSAGE(cursor.value() == Slice("sender_wins"),
                        "Conflict not resolved correctly");
  }
}

BOOST_FIXTURE_TEST_CASE(test_fsm_reuse, ReplicationFixture) {
  // This test verifies that FSMs can transition back to IDLE state after
  // completion, allowing the same FSM code path to be used for multiple
  // replication rounds. In practice, each round uses fresh FSM instances
  // with new transactions.
  
  auto sender_path = test_temp_dir / "sender_reuse.lvs";
  auto receiver_path = test_temp_dir / "receiver_reuse.lvs";

  auto sender_storage = Storage::create(sender_path.c_str());
  auto receiver_storage = Storage::create(receiver_path.c_str());
  BOOST_REQUIRE(sender_storage);
  BOOST_REQUIRE(receiver_storage);

  auto sender_db = (*sender_storage)["testdb"];
  auto receiver_db = (*receiver_storage)["testdb"];

  TestTransport sender_transport;
  TestTransport receiver_transport;
  sender_transport.set_peer(&receiver_transport);
  receiver_transport.set_peer(&sender_transport);

  TestEvents sender_events;
  TestEvents receiver_events;

  // Round 1: Initial replication
  {
    auto cursor = sender_db.cursor();
    cursor.find(Slice("key_a"));
    cursor.value(Slice("value_a"));
    cursor.find(Slice("key_b"));
    cursor.value(Slice("value_b"));
    cursor.find(Slice("key_c"));
    cursor.value(Slice("value_c"));
    cursor.commit();
  }

  auto* sender_impl = sender_db._internal();
  auto* receiver_impl = receiver_db._internal();

  {
    SenderFSM sender(sender_impl, sender_impl->txn());
    ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

    receiver.begin(&receiver_transport, &receiver_events);
    sender.begin(&sender_transport, &sender_events);

    run_protocol(sender, receiver, sender_transport, receiver_transport);

    BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
    BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
    BOOST_CHECK(sender_events.completed);
    BOOST_CHECK(receiver_events.completed);

    // Verify first replication
    {
      auto cursor = receiver_db.cursor();
      cursor.find(Slice("key_a"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_a"));
      cursor.find(Slice("key_b"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_b"));
      cursor.find(Slice("key_c"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_c"));
    }
  }  // FSMs go out of scope, demonstrating they cleanly transition to IDLE

  // Clear any leftover messages in transport queues between rounds
  while (sender_transport.has_message()) sender_transport.receive();
  while (receiver_transport.has_message()) receiver_transport.receive();

  // Reset event counters for round 2
  sender_events.reset();
  receiver_events.reset();

  // Round 2: Add more data and replicate again with new FSM instances
  {
    auto cursor = sender_db.cursor();
    cursor.find(Slice("key_d"));
    cursor.value(Slice("value_d"));
    cursor.find(Slice("key_e"));
    cursor.value(Slice("value_e"));
    cursor.commit();
  }

  // Create fresh FSM instances for second replication round
  {
    SenderFSM sender(sender_impl, sender_impl->txn());
    ReceiverFSM receiver(receiver_impl, receiver_impl->txn());

    receiver.begin(&receiver_transport, &receiver_events);
    sender.begin(&sender_transport, &sender_events);

    run_protocol(sender, receiver, sender_transport, receiver_transport);

    BOOST_CHECK(sender.state() == SenderFSM::State::IDLE);
    BOOST_CHECK(receiver.state() == ReceiverFSM::State::IDLE);
    BOOST_CHECK(sender_events.completed);
    BOOST_CHECK(receiver_events.completed);

    // Verify second replication - should have all keys
    {
      auto cursor = receiver_db.cursor();
      cursor.find(Slice("key_a"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_a"));
      cursor.find(Slice("key_b"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_b"));
      cursor.find(Slice("key_c"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_c"));
      cursor.find(Slice("key_d"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_d"));
      cursor.find(Slice("key_e"));
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_CHECK(cursor.value() == Slice("value_e"));
    }
  }  // FSMs cleanly complete and transition to IDLE again
}

BOOST_AUTO_TEST_SUITE_END()
