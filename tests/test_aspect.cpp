#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE AspectTest

#include <boost/test/included/unit_test.hpp>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef TESTING
#define TESTING
#endif

#include <blake3.h>

#include "leaves/mmap.hpp"
#include "leaves/replicating_mmap.hpp"
#include "leaves/intern/db/_check.hpp"
#include "leaves/intern/replication/_replication_fsm.hpp"

using namespace leaves;

// Wait for background hashing to catch up to the current transaction.
// Call after commit() and before begin() to ensure hashes are available.
template <typename DB>
void wait_for_hashing(DB* db, int timeout_ms = 5000) {
  auto target = db->txn()->txn_id;
  auto start = std::chrono::steady_clock::now();
  while (!db->hashes_ready_through(target)) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
      throw std::runtime_error("Timeout waiting for hashing to complete");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// =============================================================================
// Test Aspect — exercises all join points
// =============================================================================
//
// This aspect:
//   on_write:  prepends a 4-byte tag "TAG:" to every value
//   on_read:   strips the 4-byte tag, returning the original value
//   may_delete: rejects deletion of keys starting with "protected_"
//   init_cursor_context: sets a per-cursor counter to 0
//   may_merge_overwrite: rejects overwrite of keys starting with "locked_"
//   may_merge_add:       rejects add of keys starting with "blocked_"
//   may_merge_delete:    rejects deletion of keys starting with "pinned_"

struct TestAspect {
  static constexpr size_t big_meta_size = 0;

  struct CursorContext {
    int write_count = 0;   // counts on_write calls
    int read_count = 0;    // counts on_read calls
    int delete_count = 0;  // counts may_delete calls
    std::string write_buf; // scratch buffer for value transformation
  };

  void init_cursor_context(CursorContext& ctx) {
    ctx.write_count = 0;
    ctx.read_count = 0;
    ctx.delete_count = 0;
  }

  Slice on_write(const Slice& key, const Slice& value, CursorContext& ctx) {
    ctx.write_count++;
    ctx.write_buf.clear();
    ctx.write_buf.append("TAG:", 4);
    ctx.write_buf.append(value.data(), value.size());
    return Slice(ctx.write_buf);
  }

  Slice on_read(const Slice& key, const Slice& data,
                const Slice& big_meta, CursorContext& ctx) {
    ctx.read_count++;
    // Strip the TAG: prefix if present
    if (data.size() >= 4 && std::memcmp(data.data(), "TAG:", 4) == 0) {
      return Slice(data.data() + 4, data.size() - 4);
    }
    return data;
  }

  bool may_delete(const Slice& key, const Slice& value, CursorContext& ctx) {
    ctx.delete_count++;
    // Reject deletion of keys starting with "protected_"
    std::string k(key.data(), key.size());
    return k.find("protected_") != 0;
  }

  void init_big_meta(const Slice& key, char* meta_ptr, CursorContext& ctx) {}

  // --- Merge join points ---
  bool may_merge_overwrite(const Slice& key, const Slice& dst, bool dst_is_big,
                           const Slice& src, bool src_is_big,
                           CursorContext&) {
    std::string k(key.data(), key.size());
    return k.find("locked_") != 0;
  }

  bool may_merge_add(const Slice& key, const Slice& value, bool is_big,
                     CursorContext&) {
    std::string k(key.data(), key.size());
    return k.find("blocked_") != 0;
  }

  bool may_merge_delete(const Slice& key, const Slice& meta,
                        CursorContext&) {
    std::string k(key.data(), key.size());
    return k.find("pinned_") != 0;
  }
};

// =============================================================================
// BigMeta Aspect — stores 8 bytes of inline metadata alongside big values
// =============================================================================

struct BigMetaAspect {
  static constexpr size_t big_meta_size = 8;

  struct CursorContext {
    std::string write_buf;
  };

  void init_cursor_context(CursorContext&) {}

  Slice on_write(const Slice& key, const Slice& value, CursorContext&) {
    return value;
  }

  Slice on_read(const Slice& key, const Slice& data,
                const Slice& big_meta, CursorContext& ctx) {
    // If big_meta present, prepend it to the returned value so tests
    // can verify the metadata was stored.
    if (big_meta.size() > 0) {
      ctx.write_buf.clear();
      ctx.write_buf.append(big_meta.data(), big_meta.size());
      ctx.write_buf.append(data.data(), data.size());
      return Slice(ctx.write_buf);
    }
    return data;
  }

  bool may_delete(const Slice&, const Slice&, CursorContext&) { return true; }

  void init_big_meta(const Slice& key, char* meta_ptr, CursorContext&) {
    // Write a fixed 8-byte marker: "BMETA!!!"
    std::memcpy(meta_ptr, "BMETA!!!", 8);
  }

  bool may_merge_overwrite(const Slice&, const Slice&, bool,
                           const Slice&, bool, CursorContext&) { return true; }
  bool may_merge_add(const Slice&, const Slice&, bool, CursorContext&) { return true; }
  bool may_merge_delete(const Slice&, const Slice&, CursorContext&) { return true; }
};

// =============================================================================
// Traits with Aspect
// =============================================================================

// For plain (non-replication) tests
struct _AspectTraits : public _MemoryMapTraits {
  typedef TestAspect Aspect;
};

// For big-meta tests
struct _BigMetaTraits : public _MemoryMapTraits {
  typedef BigMetaAspect Aspect;
};

// For replication tests — inherits from _ReplicationTraits to get hashes
struct _ReplicationAspectTraits
    : public _ReplicationTraits<_MemoryMapTraits> {
  typedef TestAspect Aspect;
};

// =============================================================================
// Storage types
// =============================================================================

// Plain storage with TestAspect
using AspectMMap = _MemoryMapFile<_AspectTraits>;

// Plain storage with BigMetaAspect
using BigMetaMMap = _MemoryMapFile<_BigMetaTraits>;

// Replication storage with TestAspect
template <typename Traits_>
struct _AspectReplicationMMapFile
    : public _MemoryMapFile<Traits_, _ReplicationDB,
                            _AspectReplicationMMapFile<Traits_>>,
      public _ThreadPoolMixin<_AspectReplicationMMapFile<Traits_>> {
  using Base = _MemoryMapFile<Traits_, _ReplicationDB,
                              _AspectReplicationMMapFile<Traits_>>;
  using PoolMixin = _ThreadPoolMixin<_AspectReplicationMMapFile<Traits_>>;
  using DB = typename Base::DB;

  _AspectReplicationMMapFile(const char* path, size_t map_size = 2 * G,
                             uint16_t db_count = 48)
      : Base(path, map_size, db_count), PoolMixin(1) {}

  ~_AspectReplicationMMapFile() {
    this->_dbs.clear();
    this->stop_pool();
  }

  DB* make(const char* name) {
    DB* db = Base::make(name);
    db->start_purge();
    return db;
  }

  DB* operator[](const char* name) { return make(name); }
};

using AspectReplicationMMap = _AspectReplicationMMapFile<_ReplicationAspectTraits>;

// =============================================================================
// Test helpers
// =============================================================================

struct TempDir {
  std::filesystem::path dir;

  TempDir(const char* name) {
    dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }

  std::filesystem::path path(const char* file) const { return dir / file; }
};

// Replication test transport (same as in test_replication_fsm.cpp)
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

struct TestEvents : ReplicationEvents {
  bool completed = false;
  bool errored = false;
  ReplicationError error = ReplicationError::NONE;

  void on_complete(uint64_t, size_t) override { completed = true; }
  void on_error(uint64_t, ReplicationError err, const char*) override {
    errored = true;
    error = err;
  }
  void on_progress(uint64_t, size_t, size_t) override {}
};

template <typename Sender, typename Receiver>
static void run_protocol(Sender& sender, Receiver& receiver,
                         TestTransport& st, TestTransport& rt,
                         int max_rounds = 100) {
  for (int i = 0; i < max_rounds; i++) {
    bool activity = false;
    while (rt.has_message()) {
      auto msg = rt.receive();
      auto& buf = receiver.receive_buffer();
      size_t n = std::min(msg.size(), buf.available());
      std::memcpy(buf.write_ptr(), msg.data(), n);
      buf.advance(n);
      receiver.on_data_received();
      activity = true;
    }
    while (st.has_message()) {
      auto msg = st.receive();
      sender.on_message_received(msg.data(), msg.size());
      activity = true;
    }
    if ((sender.state() == Sender::State::IDLE ||
         sender.state() == Sender::State::ERROR) &&
        (receiver.state() == Receiver::State::IDLE ||
         receiver.state() == Receiver::State::ERROR))
      break;
    if (!activity) break;
  }
}

// =============================================================================
// Tests — DefaultAspect (no-op, zero-overhead)
// =============================================================================

BOOST_AUTO_TEST_SUITE(DefaultAspectTests)

BOOST_AUTO_TEST_CASE(test_default_aspect_passthrough) {
  // Verify that the default (un-aspected) storage works unchanged
  TempDir tmp("test_aspect_default");
  auto path = tmp.path("default.lvs");

  auto storage = MapStorage::create(path.c_str());
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  cursor.find(Slice("key1"));
  cursor.value(Slice("hello"));
  cursor.commit();

  cursor.find(Slice("key1"));
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(std::string(cursor.value().data(), cursor.value().size()),
                    "hello");

  cursor.remove();
  cursor.commit();

  cursor.find(Slice("key1"));
  BOOST_CHECK(!cursor.is_valid());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Tests — Custom Aspect (TestAspect with value transformation + gating)
// =============================================================================

BOOST_AUTO_TEST_SUITE(CustomAspectTests)

BOOST_AUTO_TEST_CASE(test_on_write_transforms_value) {
  TempDir tmp("test_aspect_write");
  auto path = tmp.path("write.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  cursor->find(Slice("mykey"));
  cursor->value(Slice("myvalue"));
  cursor->commit();

  // The raw stored value should contain the TAG: prefix
  // Read it back through on_read which strips it
  cursor->find(Slice("mykey"));
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  BOOST_CHECK_EQUAL(std::string(val.data(), val.size()), "myvalue");

  // Verify cursor context counts
  BOOST_CHECK(cursor->_aspect_context.write_count >= 1);
  BOOST_CHECK(cursor->_aspect_context.read_count >= 1);
}

BOOST_AUTO_TEST_CASE(test_on_write_actually_stored_with_tag) {
  TempDir tmp("test_aspect_rawtag");
  auto path = tmp.path("rawtag.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  cursor->find(Slice("key"));
  cursor->value(Slice("data"));
  cursor->commit();

  // Read the raw value (bypassing on_read) to verify it has the tag
  cursor->find(Slice("key"));
  BOOST_CHECK(cursor->is_valid());
  Slice raw = cursor->_raw_value();
  std::string raw_str(raw.data(), raw.size());
  BOOST_CHECK_EQUAL(raw_str, "TAG:data");
}

BOOST_AUTO_TEST_CASE(test_on_read_strips_tag) {
  TempDir tmp("test_aspect_read");
  auto path = tmp.path("read.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  // Write several keys
  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "value_" + std::to_string(i);
    cursor->find(Slice(key));
    cursor->value(Slice(val));
  }
  cursor->commit();

  // Read them back — on_read should strip the TAG:
  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected = "value_" + std::to_string(i);
    cursor->find(Slice(key));
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(std::string(val.data(), val.size()), expected);
  }
}

BOOST_AUTO_TEST_CASE(test_may_delete_allows_normal_keys) {
  TempDir tmp("test_aspect_delete");
  auto path = tmp.path("delete.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  cursor->find(Slice("normal_key"));
  cursor->value(Slice("some_value"));
  cursor->commit();

  cursor->find(Slice("normal_key"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_NO_THROW(cursor->remove());
  cursor->commit();

  cursor->find(Slice("normal_key"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_may_delete_rejects_protected_keys) {
  TempDir tmp("test_aspect_protected");
  auto path = tmp.path("protected.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  cursor->find(Slice("protected_secret"));
  cursor->value(Slice("precious"));
  cursor->commit();

  cursor->find(Slice("protected_secret"));
  BOOST_CHECK(cursor->is_valid());
  // Aspect should reject deletion by throwing NoValidPosition
  BOOST_CHECK_THROW(cursor->remove(), NoValidPosition);

  // Verify the key is still there after rollback
  cursor->rollback();
  cursor->find(Slice("protected_secret"));
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  BOOST_CHECK_EQUAL(std::string(val.data(), val.size()), "precious");
}

BOOST_AUTO_TEST_CASE(test_init_cursor_context_resets_counters) {
  TempDir tmp("test_aspect_ctx");
  auto path = tmp.path("ctx.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");

  // First cursor
  auto c1 = db->create_cursor();
  BOOST_CHECK_EQUAL(c1->_aspect_context.write_count, 0);
  BOOST_CHECK_EQUAL(c1->_aspect_context.read_count, 0);
  BOOST_CHECK_EQUAL(c1->_aspect_context.delete_count, 0);

  c1->find(Slice("a"));
  c1->value(Slice("b"));
  BOOST_CHECK_EQUAL(c1->_aspect_context.write_count, 1);
  c1->commit();

  c1->find(Slice("a"));
  c1->value();
  BOOST_CHECK_EQUAL(c1->_aspect_context.read_count, 1);

  // Second cursor — should have fresh counters
  auto c2 = db->create_cursor();
  BOOST_CHECK_EQUAL(c2->_aspect_context.write_count, 0);
  BOOST_CHECK_EQUAL(c2->_aspect_context.read_count, 0);
}

BOOST_AUTO_TEST_CASE(test_multiple_writes_increment_counter) {
  TempDir tmp("test_aspect_multi");
  auto path = tmp.path("multi.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  for (int i = 0; i < 5; i++) {
    std::string key = "k" + std::to_string(i);
    cursor->find(Slice(key));
    cursor->value(Slice("v"));
  }
  cursor->commit();

  BOOST_CHECK_EQUAL(cursor->_aspect_context.write_count, 5);
}

BOOST_AUTO_TEST_CASE(test_aspect_accessor_on_db) {
  TempDir tmp("test_aspect_accessor");
  auto path = tmp.path("accessor.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");

  // Verify aspect() returns a TestAspect reference
  TestAspect& a = db->aspect();
  (void)a;  // just verify it compiles and is accessible
}

BOOST_AUTO_TEST_CASE(test_overwrite_preserves_aspect_transform) {
  TempDir tmp("test_aspect_overwrite");
  auto path = tmp.path("overwrite.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  // Write initial value
  cursor->find(Slice("key"));
  cursor->value(Slice("initial"));
  cursor->commit();

  // Overwrite with new value
  cursor->find(Slice("key"));
  cursor->value(Slice("updated"));
  cursor->commit();

  // Read back — should see "updated" (after on_read strips TAG:)
  cursor->find(Slice("key"));
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  BOOST_CHECK_EQUAL(std::string(val.data(), val.size()), "updated");

  // Raw should be "TAG:updated"
  Slice raw = cursor->_raw_value();
  BOOST_CHECK_EQUAL(std::string(raw.data(), raw.size()), "TAG:updated");
}

BOOST_AUTO_TEST_CASE(test_iteration_with_aspect) {
  TempDir tmp("test_aspect_iter");
  auto path = tmp.path("iter.lvs");

  AspectMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  // Insert keys in order
  std::vector<std::string> keys = {"alpha", "beta", "gamma", "delta"};
  for (auto& k : keys) {
    cursor->find(Slice(k));
    cursor->value(Slice(k + "_val"));
  }
  cursor->commit();

  // Iterate using first/next — on_read should transform all values
  cursor->first();
  int count = 0;
  while (cursor->is_valid()) {
    Slice val = cursor->value();
    std::string k(cursor->key().data(), cursor->key().size());
    std::string v(val.data(), val.size());
    BOOST_CHECK_EQUAL(v, k + "_val");
    cursor->next();
    count++;
  }
  BOOST_CHECK_EQUAL(count, 4);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Tests — BigMeta Aspect
// =============================================================================

BOOST_AUTO_TEST_SUITE(BigMetaAspectTests)

BOOST_AUTO_TEST_CASE(test_big_value_has_inline_meta) {
  TempDir tmp("test_aspect_bigmeta");
  auto path = tmp.path("bigmeta.lvs");

  BigMetaMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  // Write a big value (> MAX_PAGE_SIZE so it uses BigValue path)
  std::string big_val(8 * 1024, 'X');  // 8KB should exceed page size
  cursor->find(Slice("bigkey"));
  cursor->value(Slice(big_val));
  cursor->commit();

  // Read it back — BigMetaAspect::on_read prepends the 8 big_meta bytes
  cursor->find(Slice("bigkey"));
  BOOST_CHECK(cursor->is_valid());

  Slice val = cursor->value();
  // The value should start with "BMETA!!!" followed by the original data
  BOOST_REQUIRE_GE(val.size(), 8u);
  BOOST_CHECK_EQUAL(std::string(val.data(), 8), "BMETA!!!");
  BOOST_CHECK_EQUAL(val.size(), 8 + big_val.size());
  BOOST_CHECK_EQUAL(
      std::string(val.data() + 8, big_val.size()), big_val);
}

BOOST_AUTO_TEST_CASE(test_small_value_no_big_meta) {
  TempDir tmp("test_aspect_smallnometa");
  auto path = tmp.path("smallnometa.lvs");

  BigMetaMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  // Write a small inline value — should NOT have big_meta
  cursor->find(Slice("small"));
  cursor->value(Slice("tiny"));
  cursor->commit();

  cursor->find(Slice("small"));
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  // on_read returns data as-is for non-big values (big_meta is empty)
  BOOST_CHECK_EQUAL(std::string(val.data(), val.size()), "tiny");
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Tests — Replication Merge Aspect
// =============================================================================

BOOST_AUTO_TEST_SUITE(ReplicationAspectTests)

BOOST_AUTO_TEST_CASE(test_may_merge_overwrite_rejects_locked) {
  TempDir tmp("test_aspect_merge_ow");
  auto sender_path = tmp.path("sender.lvs");
  auto receiver_path = tmp.path("receiver.lvs");

  using DB = AspectReplicationMMap::DB;
  using Sender = ReplicationSenderFSM<DB>;
  using Receiver = ReplicationReceiverFSM<DB>;

  AspectReplicationMMap sender_storage(sender_path.c_str());
  AspectReplicationMMap receiver_storage(receiver_path.c_str());

  auto* sender_db = sender_storage.make("test");
  auto* receiver_db = receiver_storage.make("test");

  // Insert a key in both sender and receiver with different values.
  // "locked_key" should have its overwrite blocked by the aspect.
  {
    auto c = sender_db->create_cursor();
    c->find(Slice("locked_key"));
    c->value(Slice("sender_value"));
    c->find(Slice("normal_key"));
    c->value(Slice("sender_normal"));
    c->commit();
  }
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("locked_key"));
    c->value(Slice("receiver_value"));
    c->find(Slice("normal_key"));
    c->value(Slice("receiver_normal"));
    c->commit();
  }

  // Wait for background hashing to complete before starting replication
  wait_for_hashing(sender_db);
  wait_for_hashing(receiver_db);

  // Replicate sender → receiver
  TestTransport st, rt;
  st.set_peer(&rt);
  rt.set_peer(&st);
  TestEvents se, re;

  Sender sender(sender_db);
  Receiver receiver(receiver_db);

  receiver.begin(&rt, &re);
  sender.begin(&st, &se);
  run_protocol(sender, receiver, st, rt);

  BOOST_CHECK(se.completed);
  BOOST_CHECK(re.completed);

  // "locked_key" should still have receiver's original value
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("locked_key"));
    BOOST_CHECK(c->is_valid());
    Slice raw = c->_raw_value();
    std::string v(raw.data(), raw.size());
    // The receiver's original value was written with the aspect (TAG: prefix)
    BOOST_CHECK_EQUAL(v, "TAG:receiver_value");
  }

  // "normal_key" should have been overwritten with sender's value
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("normal_key"));
    BOOST_CHECK(c->is_valid());
    Slice raw = c->_raw_value();
    std::string v(raw.data(), raw.size());
    BOOST_CHECK_EQUAL(v, "TAG:sender_normal");
  }
}

BOOST_AUTO_TEST_CASE(test_may_merge_add_rejects_blocked) {
  TempDir tmp("test_aspect_merge_add");
  auto sender_path = tmp.path("sender.lvs");
  auto receiver_path = tmp.path("receiver.lvs");

  using DB = AspectReplicationMMap::DB;
  using Sender = ReplicationSenderFSM<DB>;
  using Receiver = ReplicationReceiverFSM<DB>;

  AspectReplicationMMap sender_storage(sender_path.c_str());
  AspectReplicationMMap receiver_storage(receiver_path.c_str());

  auto* sender_db = sender_storage.make("test");
  auto* receiver_db = receiver_storage.make("test");

  // Sender has keys that receiver does not
  {
    auto c = sender_db->create_cursor();
    c->find(Slice("blocked_item"));
    c->value(Slice("should_not_arrive"));
    c->find(Slice("allowed_item"));
    c->value(Slice("should_arrive"));
    c->commit();
  }

  // Wait for background hashing to complete before starting replication
  wait_for_hashing(sender_db);

  TestTransport st, rt;
  st.set_peer(&rt);
  rt.set_peer(&st);
  TestEvents se, re;

  Sender sender(sender_db);
  Receiver receiver(receiver_db);

  receiver.begin(&rt, &re);
  sender.begin(&st, &se);
  run_protocol(sender, receiver, st, rt);

  BOOST_CHECK(se.completed);
  BOOST_CHECK(re.completed);

  // "blocked_item" should NOT exist in receiver
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("blocked_item"));
    BOOST_CHECK(!c->is_valid());
  }

  // "allowed_item" should exist
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("allowed_item"));
    BOOST_CHECK(c->is_valid());
  }
}

BOOST_AUTO_TEST_CASE(test_may_merge_delete_rejects_pinned) {
  TempDir tmp("test_aspect_merge_del");
  auto sender_path = tmp.path("sender.lvs");
  auto receiver_path = tmp.path("receiver.lvs");

  using DB = AspectReplicationMMap::DB;
  using Sender = ReplicationSenderFSM<DB>;
  using Receiver = ReplicationReceiverFSM<DB>;

  AspectReplicationMMap sender_storage(sender_path.c_str());
  AspectReplicationMMap receiver_storage(receiver_path.c_str());

  auto* sender_db = sender_storage.make("test");
  auto* receiver_db = receiver_storage.make("test");

  // Both have the same keys initially
  for (auto* db : {sender_db, receiver_db}) {
    auto c = db->create_cursor();
    c->find(Slice("pinned_important"));
    c->value(Slice("must_stay"));
    c->find(Slice("deletable"));
    c->value(Slice("can_go"));
    c->commit();
  }

  // Sender deletes both keys
  {
    auto c = sender_db->create_cursor();
    c->find(Slice("deletable"));
    c->remove();
    c->find(Slice("pinned_important"));
    c->remove();  // Aspect allows cursor-level delete on sender
    c->commit();
  }

  // Wait for background hashing to complete before starting replication
  wait_for_hashing(sender_db);
  wait_for_hashing(receiver_db);

  // Replicate sender → receiver (deletions propagate via deletion trie)
  TestTransport st, rt;
  st.set_peer(&rt);
  rt.set_peer(&st);
  TestEvents se, re;

  Sender sender(sender_db);
  Receiver receiver(receiver_db);

  receiver.begin(&rt, &re);
  sender.begin(&st, &se);
  run_protocol(sender, receiver, st, rt);

  BOOST_CHECK(se.completed);
  BOOST_CHECK(re.completed);

  // "pinned_important" should still exist in receiver (merge delete rejected)
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("pinned_important"));
    BOOST_CHECK(c->is_valid());
  }

  // "deletable" should be gone from receiver
  {
    auto c = receiver_db->create_cursor();
    c->find(Slice("deletable"));
    BOOST_CHECK(!c->is_valid());
  }
}

BOOST_AUTO_TEST_CASE(test_replication_cursor_may_delete_rejects_protected) {
  TempDir tmp("test_aspect_repl_del");
  auto path = tmp.path("repl.lvs");

  AspectReplicationMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  // Write a protected key
  cursor->find(Slice("protected_item"));
  cursor->value(Slice("safe"));
  cursor->commit();

  // Attempt to delete it — should throw
  cursor->find(Slice("protected_item"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_THROW(cursor->remove(), NoValidPosition);

  cursor->rollback();

  // Key should survive
  cursor->find(Slice("protected_item"));
  BOOST_CHECK(cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_replication_cursor_allows_normal_delete) {
  TempDir tmp("test_aspect_repl_ok_del");
  auto path = tmp.path("replok.lvs");

  AspectReplicationMMap storage(path.c_str());
  auto* db = storage.make("test");
  auto cursor = db->create_cursor();

  cursor->find(Slice("normal_item"));
  cursor->value(Slice("byebye"));
  cursor->commit();

  cursor->find(Slice("normal_item"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_NO_THROW(cursor->remove());
  cursor->commit();

  cursor->find(Slice("normal_item"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Tests — SFINAE detection
// =============================================================================

BOOST_AUTO_TEST_SUITE(SFINAETests)

BOOST_AUTO_TEST_CASE(test_traits_without_aspect_uses_default) {
  // _MemoryMapTraits does not define Aspect, so DefaultAspect is used
  using DB = _MemoryMapFile<_MemoryMapTraits>::DB;
  static_assert(std::is_same_v<DB::Aspect, DefaultAspect>,
                "Should fall back to DefaultAspect");
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_traits_with_aspect_uses_custom) {
  // _AspectTraits defines Aspect = TestAspect
  using DB = AspectMMap::DB;
  static_assert(std::is_same_v<DB::Aspect, TestAspect>,
                "Should use TestAspect from traits");
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_cursor_context_type_matches_aspect) {
  using Cursor = AspectMMap::DB::Cursor;
  static_assert(
      std::is_same_v<typename Cursor::CursorContext, TestAspect::CursorContext>,
      "CursorContext should come from TestAspect");
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_default_cursor_context_is_empty) {
  using Cursor = _MemoryMapFile<_MemoryMapTraits>::DB::Cursor;
  static_assert(std::is_empty_v<typename Cursor::CursorContext>,
                "DefaultAspect::CursorContext should be empty");
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()
