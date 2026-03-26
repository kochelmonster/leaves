#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE ReplicationAPITest

#include <boost/test/included/unit_test.hpp>
#include <cstring>
#include <filesystem>
#include <queue>
#include <vector>

#ifndef TESTING
#define TESTING
#endif

#include "leaves/replication.hpp"
#include "leaves/replicating_mmap.hpp"
#include "leaves/replicating_fstore.hpp"

using namespace leaves;

// ============================================================================
// Loopback transport for in-process testing
// ============================================================================

struct LoopbackTransport : ReplicationTransport {
  std::queue<std::vector<uint8_t>> _incoming;
  LoopbackTransport* _peer = nullptr;

  void set_peer(LoopbackTransport* peer) { _peer = peer; }

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

// ============================================================================
// Simple events tracker
// ============================================================================

struct EventTracker : ReplicationEvents {
  bool completed = false;
  bool errored = false;
  size_t nodes = 0;
  size_t progress_calls = 0;
  ReplicationError last_error = ReplicationError::NONE;

  void on_complete(uint64_t, size_t nodes_transferred) override {
    completed = true;
    nodes = nodes_transferred;
  }

  void on_error(uint64_t, ReplicationError err, const char*) override {
    errored = true;
    last_error = err;
  }

  void on_progress(uint64_t, size_t, size_t) override {
    progress_calls++;
  }
};

// ============================================================================
// Fixture
// ============================================================================

struct APIFixture {
  std::filesystem::path temp_dir;

  APIFixture() {
    temp_dir = std::filesystem::temp_directory_path() / "test_replication_api";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);
  }

  ~APIFixture() { std::filesystem::remove_all(temp_dir); }
};

BOOST_AUTO_TEST_SUITE(ReplicationPublicAPI)

// ============================================================================
// Test: basic sender/receiver with mmap storage
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_mmap_replicate_basic, APIFixture) {
  auto src_path = temp_dir / "src.lvs";
  auto dst_path = temp_dir / "dst.lvs";

  auto src_storage = ReplicatingMapStorage::create(src_path.c_str());
  auto dst_storage = ReplicatingMapStorage::create(dst_path.c_str());

  // Insert data on source
  auto src_db = (*src_storage)["testdb"];
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("hello"));
    c.value(Slice("world"));
    c.find(Slice("foo"));
    c.value(Slice("bar"));
    c.commit();
  }

  auto dst_db = (*dst_storage)["testdb"];

  // Set up transports
  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  // Create sessions using public API
  ReplicationSender<ReplicatingMapStorage> sender(src_db);
  ReplicationReceiver<ReplicatingMapStorage> receiver(dst_db);

  BOOST_CHECK(sender.state() == ReplicationState::IDLE);
  BOOST_CHECK(receiver.state() == ReplicationState::IDLE);

  // Start replication
  receiver.begin(&receiver_tx, &receiver_events);
  sender.begin(&sender_tx, &sender_events);

  // Run protocol using free function
  run_replication(sender, receiver, sender_tx, receiver_tx);

  // Both should be idle (completed)
  BOOST_CHECK(sender.state() == ReplicationState::IDLE);
  BOOST_CHECK(receiver.state() == ReplicationState::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);
  BOOST_CHECK_GT(sender.bytes_transferred(), 0u);

  // Verify data was replicated
  {
    auto c = dst_db.cursor();
    c.find(Slice("hello"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "world");
    c.find(Slice("foo"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "bar");
  }
}

// ============================================================================
// Test: empty DB replication
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_empty_db_replication, APIFixture) {
  auto src_path = temp_dir / "src_empty.lvs";
  auto dst_path = temp_dir / "dst_empty.lvs";

  auto src_storage = ReplicatingMapStorage::create(src_path.c_str());
  auto dst_storage = ReplicatingMapStorage::create(dst_path.c_str());

  auto src_db = (*src_storage)["testdb"];
  auto dst_db = (*dst_storage)["testdb"];

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  ReplicationSender<ReplicatingMapStorage> sender(src_db);
  ReplicationReceiver<ReplicatingMapStorage> receiver(dst_db);

  receiver.begin(&receiver_tx, &receiver_events);
  sender.begin(&sender_tx, &sender_events);

  run_replication(sender, receiver, sender_tx, receiver_tx);

  BOOST_CHECK(sender.state() == ReplicationState::IDLE);
  BOOST_CHECK(receiver.state() == ReplicationState::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);
}

// ============================================================================
// Test: file storage replication
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_fstore_replicate, APIFixture) {
  auto src_path = temp_dir / "src.fst";
  auto dst_path = temp_dir / "dst.fst";

  auto src_storage = ReplicatingFileStorage::create(src_path.c_str());
  auto dst_storage = ReplicatingFileStorage::create(dst_path.c_str());

  // Insert data on source
  auto src_db = (*src_storage)["mydb"];
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("key1"));
    c.value(Slice("value1"));
    c.find(Slice("key2"));
    c.value(Slice("value2"));
    c.commit();
  }

  auto dst_db = (*dst_storage)["mydb"];

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  ReplicationSender<ReplicatingFileStorage> sender(src_db);
  ReplicationReceiver<ReplicatingFileStorage> receiver(dst_db);

  receiver.begin(&receiver_tx, &receiver_events);
  sender.begin(&sender_tx, &sender_events);

  run_replication(sender, receiver, sender_tx, receiver_tx);

  BOOST_CHECK(sender.state() == ReplicationState::IDLE);
  BOOST_CHECK(receiver.state() == ReplicationState::IDLE);
  BOOST_CHECK(sender_events.completed);
  BOOST_CHECK(receiver_events.completed);

  // Verify
  {
    auto c = dst_db.cursor();
    c.find(Slice("key1"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "value1");
    c.find(Slice("key2"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "value2");
  }
}

// ============================================================================
// Test: set_retention on TDB
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_set_retention, APIFixture) {
  auto path = temp_dir / "retention.lvs";
  auto storage = ReplicatingMapStorage::create(path.c_str());
  auto db = (*storage)["testdb"];

  // Should not throw — sets retention on the underlying _ReplicationDB
  db.set_retention(3600);

  // Verify through internal API
  BOOST_CHECK_EQUAL(
      db._internal()->_retention_seconds.load(std::memory_order_relaxed),
      3600u);
}

// ============================================================================
// Test: session_id is set after begin
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_session_id, APIFixture) {
  auto src_path = temp_dir / "src_sid.lvs";
  auto dst_path = temp_dir / "dst_sid.lvs";

  auto src_storage = ReplicatingMapStorage::create(src_path.c_str());
  auto dst_storage = ReplicatingMapStorage::create(dst_path.c_str());

  auto src_db = (*src_storage)["testdb"];
  auto dst_db = (*dst_storage)["testdb"];

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker events;

  ReplicationSender<ReplicatingMapStorage> sender(src_db);
  sender.begin(&sender_tx, &events);

  BOOST_CHECK_NE(sender.session_id(), 0u);
}

// ============================================================================
// Test: cross-storage replication (mmap → fstore)
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_cross_storage_replication, APIFixture) {
  auto src_path = temp_dir / "src_cross.lvs";
  auto dst_path = temp_dir / "dst_cross.fst";

  auto src_storage = ReplicatingMapStorage::create(src_path.c_str());
  auto dst_storage = ReplicatingFileStorage::create(dst_path.c_str());

  // Insert data on mmap source
  auto src_db = (*src_storage)["crossdb"];
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("alpha"));
    c.value(Slice("one"));
    c.find(Slice("beta"));
    c.value(Slice("two"));
    c.commit();
  }

  auto dst_db = (*dst_storage)["crossdb"];

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  // Sender is mmap, receiver is fstore — uses FSM types directly
  using MmapSender = ReplicationSender<ReplicatingMapStorage>;
  using FstoreReceiver = ReplicationReceiver<ReplicatingFileStorage>;

  MmapSender sender(src_db);
  FstoreReceiver receiver(dst_db);

  receiver.begin(&receiver_tx, &receiver_events);
  sender.begin(&sender_tx, &sender_events);

  run_replication(sender, receiver, sender_tx, receiver_tx);

  BOOST_CHECK(sender.state() == ReplicationState::IDLE);
  BOOST_CHECK(receiver.state() == ReplicationState::IDLE);

  // Verify
  {
    auto c = dst_db.cursor();
    c.find(Slice("alpha"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "one");
    c.find(Slice("beta"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "two");
  }
}

// ============================================================================
// Test: incremental replication (second sync is no-op)
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_incremental_replication, APIFixture) {
  auto src_path = temp_dir / "src_incr.lvs";
  auto dst_path = temp_dir / "dst_incr.lvs";

  auto src_storage = ReplicatingMapStorage::create(src_path.c_str());
  auto dst_storage = ReplicatingMapStorage::create(dst_path.c_str());

  auto src_db = (*src_storage)["testdb"];
  auto dst_db = (*dst_storage)["testdb"];

  // Insert initial data
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("key1"));
    c.value(Slice("val1"));
    c.commit();
  }

  // First replication
  {
    LoopbackTransport stx, rtx;
    stx.set_peer(&rtx);
    rtx.set_peer(&stx);
    EventTracker se, re;

    ReplicationSender<ReplicatingMapStorage> sender(src_db);
    ReplicationReceiver<ReplicatingMapStorage> receiver(dst_db);
    receiver.begin(&rtx, &re);
    sender.begin(&stx, &se);
    run_replication(sender, receiver, stx, rtx);

    BOOST_CHECK(se.completed);
    BOOST_CHECK(re.completed);
  }

  // Add more data, replicate again
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("key2"));
    c.value(Slice("val2"));
    c.commit();
  }

  {
    LoopbackTransport stx, rtx;
    stx.set_peer(&rtx);
    rtx.set_peer(&stx);
    EventTracker se, re;

    ReplicationSender<ReplicatingMapStorage> sender(src_db);
    ReplicationReceiver<ReplicatingMapStorage> receiver(dst_db);
    receiver.begin(&rtx, &re);
    sender.begin(&stx, &se);
    run_replication(sender, receiver, stx, rtx);

    BOOST_CHECK(se.completed);
    BOOST_CHECK(re.completed);
  }

  // Verify both keys present
  {
    auto c = dst_db.cursor();
    c.find(Slice("key1"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "val1");
    c.find(Slice("key2"));
    BOOST_CHECK(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "val2");
  }
}

BOOST_AUTO_TEST_SUITE_END()
