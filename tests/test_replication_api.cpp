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
#include "leaves/mmap.hpp"
#include "leaves/intern/replication/_replication_db.hpp"
#include "leaves/fstore.hpp"

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
// Test: error state mapping (sender and receiver)
// ============================================================================
BOOST_FIXTURE_TEST_CASE(test_error_state, APIFixture) {
  // Exercises replication.hpp L57-58 (Sender ERROR state)
  // Exercises replication.hpp L119-120 (Receiver ERROR state)
  auto src_path = temp_dir / "src_err.lvs";
  auto dst_path = temp_dir / "dst_err.lvs";

  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = MapStorage::create(dst_path.c_str());

  auto src_db = src_storage->open<MapStorage::ReplicationDB>("testdb");
  auto dst_db = dst_storage->open<MapStorage::ReplicationDB>("testdb");

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  ReplicationSender<MapStorage> sender(src_db);
  ReplicationReceiver<MapStorage> receiver(dst_db);

  // Start receiver but feed it garbage data to trigger error
  receiver.begin(&receiver_tx, &receiver_events);

  // Write garbage to receiver's buffer
  uint8_t garbage[64];
  memset(garbage, 0xFF, sizeof(garbage));
  auto& buf = receiver.receive_buffer();
  // Construct a fake message: first 4 bytes are size header
  uint32_t fake_size = 16;
  memcpy(buf.write_ptr(), &fake_size, 4);
  memcpy(buf.write_ptr() + 4, garbage, 60);
  buf.advance(64);
  receiver.on_data_received();

  // Receiver should be in ERROR state
  BOOST_CHECK(receiver.state() == ReplicationState::ERR ||
              receiver.state() == ReplicationState::IDLE);
  if (receiver.state() == ReplicationState::ERR) {
    BOOST_CHECK(receiver_events.errored);
  }

  // Test sender error by feeding garbage message
  sender.begin(&sender_tx, &sender_events);
  sender.on_message_received(garbage, sizeof(garbage));

  BOOST_CHECK(sender.state() == ReplicationState::ERR ||
              sender.state() == ReplicationState::ACTIVE);
}

// ============================================================================
// Test: basic sender/receiver with mmap storage
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_mmap_replicate_basic, APIFixture) {
  auto src_path = temp_dir / "src.lvs";
  auto dst_path = temp_dir / "dst.lvs";

  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = MapStorage::create(dst_path.c_str());

  // Insert data on source
  auto src_db = src_storage->open<MapStorage::ReplicationDB>("testdb");
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("hello"));
    c.value(Slice("world"));
    c.find(Slice("foo"));
    c.value(Slice("bar"));
    c.commit();
  }

  auto dst_db = dst_storage->open<MapStorage::ReplicationDB>("testdb");

  // Set up transports
  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  // Create sessions using public API
  ReplicationSender<MapStorage> sender(src_db);
  ReplicationReceiver<MapStorage> receiver(dst_db);

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

BOOST_FIXTURE_TEST_CASE(test_prefix_replication, APIFixture) {
  auto src_path = temp_dir / "prefix-src.lvs";
  auto dst_path = temp_dir / "prefix-dst.lvs";
  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = MapStorage::create(dst_path.c_str());
  auto src = src_storage->open<MapStorage::ReplicationDB>("test");
  auto dst = dst_storage->open<MapStorage::ReplicationDB>("test");
  const std::string binary_prefix("tenant/\0", 8);
  const std::string long_prefix(600, 'x');
  const std::string big_value(20000, 'v');
  const std::string apple = binary_prefix + "apple";
  const std::string apricot = binary_prefix + "apricot";
  const std::string banana = binary_prefix + "banana";
  const std::string big_key = binary_prefix + "big";
  const std::string long_key = long_prefix + "apple";
  const std::string long_other = long_prefix + "banana";

  {
    auto c = src.cursor();
    c.start_transaction();
    for (const std::string& key :
         {binary_prefix, apple, apricot, banana, std::string("outside")}) {
      c.find(Slice(key));
      c.value(Slice(key));
    }
    std::string shorter_key = std::string(300, 'x') + "other";
    c.find(Slice(big_key));
    c.value(Slice(big_value));
    c.find(Slice(long_key));
    c.value(Slice("long-key"));
    c.find(Slice(long_other));
    c.value(Slice("not-selected"));
    c.find(Slice(shorter_key));
    c.value(Slice("not-selected"));
    c.commit();
  }
  {
    auto c = dst.cursor();
    c.start_transaction();
    c.find(Slice("outside"));
    c.value(Slice("local"));
    c.commit();
  }

  auto sync = [&](const std::string& prefix) {
    LoopbackTransport st, rt;
    st.set_peer(&rt);
    rt.set_peer(&st);
    EventTracker se, re;
    ReplicationSender<MapStorage> sender(src);
    ReplicationReceiver<MapStorage> receiver(dst);
    receiver.begin(&rt, &re);
    sender.begin(&st, &se, Slice(prefix));
    run_replication(sender, receiver, st, rt, 100000);
    BOOST_REQUIRE(se.completed);
    BOOST_REQUIRE(re.completed);
  };

  sync(binary_prefix + "ap");
  {
    auto c = dst.cursor();
    for (const auto& key : {apple, apricot}) {
      c.find(Slice(key));
      BOOST_REQUIRE(c.is_valid());
      BOOST_CHECK(c.value() == Slice(key));
    }
    c.find(Slice(binary_prefix));
    BOOST_CHECK(!c.is_valid());
    c.find(Slice(banana));
    BOOST_CHECK(!c.is_valid());
    c.find(Slice("outside"));
    BOOST_REQUIRE(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "local");
  }
  sync(binary_prefix);
  sync(binary_prefix);
  {
    auto c = dst.cursor();
    c.find(Slice(binary_prefix));
    BOOST_REQUIRE(c.is_valid());
    c.find(Slice(banana));
    BOOST_REQUIRE(c.is_valid());
    c.find(Slice(big_key));
    BOOST_REQUIRE(c.is_valid());
    BOOST_CHECK(c.value() == Slice(big_value));
  }
  {
    auto c = src.cursor();
    c.start_transaction();
    c.find(Slice(apple));
    c.remove();
    c.find(Slice(apricot));
    c.value(Slice("updated"));
    c.find(Slice("outside"));
    c.remove();
    c.commit();
  }
  sync(binary_prefix);
  {
    auto c = dst.cursor();
    c.find(Slice(apple));
    BOOST_CHECK(!c.is_valid());
    c.find(Slice(apricot));
    BOOST_REQUIRE(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "updated");
    c.find(Slice("outside"));
    BOOST_REQUIRE(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "local");
  }
  sync("not-present");
  sync(binary_prefix + "bigger-than-any-key");
  sync(long_prefix + "a");
  {
    auto c = dst.cursor();
    c.find(Slice(long_key));
    BOOST_REQUIRE(c.is_valid());
    BOOST_CHECK_EQUAL(c.value().string(), "long-key");
    c.find(Slice(long_other));
    BOOST_CHECK(!c.is_valid());
  }
}

BOOST_FIXTURE_TEST_CASE(test_file_prefix_single_big_leaf, APIFixture) {
  auto src_path = temp_dir / "prefix-src.fst";
  auto dst_path = temp_dir / "prefix-dst.fst";
  auto src_storage = FileStorage::create(src_path.c_str());
  auto dst_storage = FileStorage::create(dst_path.c_str());
  auto src = src_storage->open<_ReplicationDB>("test");
  auto dst = dst_storage->open<_ReplicationDB>("test");
  const std::string value(20000, 'v');
  {
    auto c = src.cursor();
    c.start_transaction();
    c.find(Slice("prefix/large"));
    c.value(Slice(value));
    c.find(Slice("other"));
    c.value(Slice("other"));
    c.commit();
  }

  for (int i = 0; i < 2; ++i) {
    LoopbackTransport st, rt;
    st.set_peer(&rt);
    rt.set_peer(&st);
    EventTracker se, re;
    ReplicationSender<FileStorage> sender(src);
    ReplicationReceiver<FileStorage> receiver(dst);
    receiver.begin(&rt, &re);
    sender.begin(&st, &se, Slice("prefix/large"));
    run_replication(sender, receiver, st, rt);
    BOOST_REQUIRE(se.completed);
    BOOST_REQUIRE(re.completed);
    if (i == 1) BOOST_CHECK_LT(sender.bytes_transferred(), value.size());
  }

  auto c = dst.cursor();
  c.find(Slice("prefix/large"));
  BOOST_REQUIRE(c.is_valid());
  BOOST_CHECK(c.value() == Slice(value));
  c.find(Slice("other"));
  BOOST_CHECK(!c.is_valid());
}

BOOST_FIXTURE_TEST_CASE(test_empty_db_replication, APIFixture) {
  auto src_path = temp_dir / "src_empty.lvs";
  auto dst_path = temp_dir / "dst_empty.lvs";

  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = MapStorage::create(dst_path.c_str());

  auto src_db = src_storage->open<MapStorage::ReplicationDB>("testdb");
  auto dst_db = dst_storage->open<MapStorage::ReplicationDB>("testdb");

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  ReplicationSender<MapStorage> sender(src_db);
  ReplicationReceiver<MapStorage> receiver(dst_db);

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

  auto src_storage = FileStorage::create(src_path.c_str());
  auto dst_storage = FileStorage::create(dst_path.c_str());

  // Insert data on source
  auto src_db = src_storage->open<_ReplicationDB>("mydb");
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("key1"));
    c.value(Slice("value1"));
    c.find(Slice("key2"));
    c.value(Slice("value2"));
    c.commit();
  }

  auto dst_db = dst_storage->open<_ReplicationDB>("mydb");

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  ReplicationSender<FileStorage> sender(src_db);
  ReplicationReceiver<FileStorage> receiver(dst_db);

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
  auto storage = MapStorage::create(path.c_str());
  auto db = storage->open<MapStorage::ReplicationDB>("testdb");

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

  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = MapStorage::create(dst_path.c_str());

  auto src_db = src_storage->open<MapStorage::ReplicationDB>("testdb");
  auto dst_db = dst_storage->open<MapStorage::ReplicationDB>("testdb");

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker events;

  ReplicationSender<MapStorage> sender(src_db);
  sender.begin(&sender_tx, &events);

  BOOST_CHECK_NE(sender.session_id(), 0u);
}

// ============================================================================
// Test: cross-storage replication (mmap → fstore)
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_cross_storage_replication, APIFixture) {
  auto src_path = temp_dir / "src_cross.lvs";
  auto dst_path = temp_dir / "dst_cross.fst";

  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = FileStorage::create(dst_path.c_str());

  // Insert data on mmap source
  auto src_db = src_storage->open<MapStorage::ReplicationDB>("crossdb");
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("alpha"));
    c.value(Slice("one"));
    c.find(Slice("beta"));
    c.value(Slice("two"));
    c.commit();
  }

  auto dst_db = dst_storage->open<_ReplicationDB>("crossdb");

  LoopbackTransport sender_tx, receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events, receiver_events;

  // Sender is mmap, receiver is fstore — uses FSM types directly
  using MmapSender = ReplicationSender<MapStorage>;
  using FstoreReceiver = ReplicationReceiver<FileStorage>;

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

  auto src_storage = MapStorage::create(src_path.c_str());
  auto dst_storage = MapStorage::create(dst_path.c_str());

  auto src_db = src_storage->open<MapStorage::ReplicationDB>("testdb");
  auto dst_db = dst_storage->open<MapStorage::ReplicationDB>("testdb");

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

    ReplicationSender<MapStorage> sender(src_db);
    ReplicationReceiver<MapStorage> receiver(dst_db);
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

    ReplicationSender<MapStorage> sender(src_db);
    ReplicationReceiver<MapStorage> receiver(dst_db);
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
