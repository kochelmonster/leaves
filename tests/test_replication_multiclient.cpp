/**
 * Multi-client replication stress test
 *
 * Simulates the kv_browser demo scenario with 3 concurrent clients:
 *   - Each client creates 1000 items locally (one transaction), then syncs
 *   - Each client removes 500 of its own items (one transaction), then syncs
 *   - All actions are logged in chronological order for deterministic replay
 *
 * The "server" is an in-process MapStorage protected by a mutex,
 * mirroring the g_db_mutex in kv_demo_server.
 *
 * Sync mirrors the kv_demo_server bidirectional LVRP protocol:
 *   Phase 1: server → client  (ReplicationSender(server) + ReplicationReceiver(client))
 *   Phase 2: client → server  (ReplicationSender(client) + ReplicationReceiver(server))
 *
 * Seeds are fixed per client so any failure is reproducible from the log.
 * Build:
 *   cmake --build build -j --target test_replication_multiclient
 *   ./build/test_replication_multiclient
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MultiClientReplicationTest

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#define TESTING
#endif

#include "leaves/replication.hpp"
#include "leaves/mmap.hpp"
#include "leaves/intern/replication/_replication_db.hpp"

using namespace leaves;

// ── In-process loopback transport ────────────────────────────────────────────

struct LoopbackTransport : ReplicationTransport {
  std::queue<std::vector<uint8_t>> _incoming;
  LoopbackTransport* _peer = nullptr;

  void set_peer(LoopbackTransport* p) { _peer = p; }

  void send(const uint8_t* data, size_t size) override {
    if (_peer) _peer->_incoming.emplace(data, data + size);
  }

  bool has_message() const { return !_incoming.empty(); }

  std::vector<uint8_t> receive() {
    auto m = std::move(_incoming.front());
    _incoming.pop();
    return m;
  }
};

// ── Bidirectional sync ───────────────────────────────────────────────────────
// Mirrors kv_demo_server handle_client() protocol. Caller holds server mutex.

struct SyncResult { bool ok; std::string error; };

static SyncResult sync_bidirectional(
    MapStorage::storage_ptr& server_storage,
    TDB<MapStorage, _ReplicationDB>& client_db) {
  // Open fresh from the calling thread — TDB must be used from the thread
  // that creates it; server_storage is shared but opening a DB handle is safe.
  auto server_db = server_storage->open<_ReplicationDB>("main");

  struct Events : ReplicationEvents {
    bool completed = false;
    bool errored   = false;
    std::string msg;
    void on_complete(uint64_t, size_t) override { completed = true; }
    void on_error(uint64_t, ReplicationError, const char* r) override {
      errored = true;
      msg = r ? r : "(unknown)";
    }
    void on_progress(uint64_t, size_t, size_t) override {}
  };

  // Phase 1: server → client
  {
    LoopbackTransport srv_tx, cli_tx;
    srv_tx.set_peer(&cli_tx);
    cli_tx.set_peer(&srv_tx);
    Events se, re;
    ReplicationSender<MapStorage>   sender(server_db);
    ReplicationReceiver<MapStorage> receiver(client_db);
    sender.begin(&srv_tx, &se);
    receiver.begin(&cli_tx, &re);
    run_replication(sender, receiver, srv_tx, cli_tx, 100000);
    if (se.errored) return {false, "phase1 sender: "   + se.msg};
    if (re.errored) return {false, "phase1 receiver: " + re.msg};
    if (!se.completed) return {false, "phase1 sender incomplete (re.ok=" + std::to_string(re.completed) + ",re.err=" + std::to_string(re.errored) + ")"};
    if (!re.completed) return {false, "phase1 receiver incomplete"};
  }

  // Phase 2: client → server
  {
    LoopbackTransport cli_tx, srv_tx;
    cli_tx.set_peer(&srv_tx);
    srv_tx.set_peer(&cli_tx);
    Events se, re;
    ReplicationSender<MapStorage>   sender(client_db);
    ReplicationReceiver<MapStorage> receiver(server_db);
    sender.begin(&cli_tx, &se);
    receiver.begin(&srv_tx, &re);
    run_replication(sender, receiver, cli_tx, srv_tx, 100000);
    if (se.errored) return {false, "phase2 sender: "   + se.msg};
    if (re.errored) return {false, "phase2 receiver: " + re.msg};
    if (!se.completed) return {false, "phase2 sender incomplete (re.ok=" + std::to_string(re.completed) + ",re.err=" + std::to_string(re.errored) + ")"};
    if (!re.completed) return {false, "phase2 receiver incomplete"};
  }

  return {true, {}};
}

// ── Action log ───────────────────────────────────────────────────────────────
// Every operation is appended in arrival order under a mutex.
// On failure, dump the log to reproduce the exact sequence.

enum class Op { PUT, REMOVE, SYNC_START, SYNC_DONE };

struct LogEntry {
  std::chrono::steady_clock::time_point ts;
  int         client_id;
  Op          op;
  std::string key;
  std::string value;  // non-empty only for PUT
};

static std::mutex            g_log_mutex;
static std::vector<LogEntry> g_log;

static void log_op(int id, Op op,
                   const std::string& key = {},
                   const std::string& val = {}) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log.push_back({std::chrono::steady_clock::now(), id, op, key, val});
}

static void dump_log(std::ostream& out) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log.empty()) return;
  const auto base = g_log.front().ts;
  static const char* names[] = {"PUT", "REMOVE", "SYNC_START", "SYNC_DONE"};
  for (const auto& e : g_log) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  e.ts - base).count();
    out << "[+" << std::setw(6) << ms << "ms] client=" << e.client_id
        << " " << names[static_cast<int>(e.op)];
    if (!e.key.empty())   out << "  key=" << e.key;
    if (!e.value.empty()) out << "  val=" << e.value;
    out << "\n";
  }
}

// ── Fixture ───────────────────────────────────────────────────────────────────

struct Fixture {
  std::filesystem::path dir;

  Fixture() {
    dir = std::filesystem::temp_directory_path() / "test_replication_multiclient";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log.clear();
  }

  ~Fixture() { std::filesystem::remove_all(dir); }
};

// ── Test ──────────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(test_three_clients_put_and_remove, Fixture) {
  constexpr int NUM_CLIENTS        = 3;
  constexpr int ITEMS_PER_CLIENT   = 1000;
  constexpr int REMOVES_PER_CLIENT = 500;
  constexpr int EXPECTED_TOTAL     =
      (ITEMS_PER_CLIENT - REMOVES_PER_CLIENT) * NUM_CLIENTS;

  // Shared server storage — protected by server_mutex for exclusive sync access
  auto server_storage = MapStorage::create((dir / "server.lvs").c_str());
  std::mutex server_mutex;

  // Per-client storages
  std::vector<MapStorage::storage_ptr> client_storages;
  for (int i = 0; i < NUM_CLIENTS; i++) {
    auto path = dir / ("client" + std::to_string(i) + ".lvs");
    client_storages.push_back(MapStorage::create(path.c_str()));
  }

  // Per-thread errors — checked by main thread after join
  std::vector<std::string> thread_errors(NUM_CLIENTS);

  auto worker = [&](int id) {
    auto client_db = client_storages[id]->open<_ReplicationDB>("main");

    // Fixed seed per client — deterministic key/value sequence for replay
    std::mt19937 rng(static_cast<uint32_t>(0xdeadbeef ^ static_cast<uint32_t>(id)));

    auto rand_val = [&]() -> std::string {
      std::uniform_int_distribution<int> dist('a', 'z');
      std::string s(12, ' ');
      for (char& c : s) c = static_cast<char>(dist(rng));
      return s;
    };

    // ── Phase 1: create ITEMS_PER_CLIENT items locally (one transaction) ──
    std::vector<std::string> my_keys;
    my_keys.reserve(ITEMS_PER_CLIENT);
    {
      auto c = client_db.cursor();
      for (int i = 0; i < ITEMS_PER_CLIENT; i++) {
        std::string key = "c" + std::to_string(id) + "/" + std::to_string(i);
        std::string val = rand_val();
        c.find(Slice(key.data(), key.size()));
        c.value(Slice(val.data(), val.size()));
        log_op(id, Op::PUT, key, val);
        my_keys.push_back(key);
      }
      c.commit();
    }

    // ── Sync 1: push creates to server ───────────────────────────────────
    log_op(id, Op::SYNC_START);
    {
      std::lock_guard<std::mutex> lock(server_mutex);
      auto r = sync_bidirectional(server_storage, client_db);
      if (!r.ok) { thread_errors[id] = "sync1: " + r.error; return; }
    }
    log_op(id, Op::SYNC_DONE);

    // ── Phase 2: remove REMOVES_PER_CLIENT of own items (one transaction) ─
    // Shuffle own keys with the same rng → deterministic remove set
    std::shuffle(my_keys.begin(), my_keys.end(), rng);

    {
      auto c = client_db.cursor();
      for (int i = 0; i < REMOVES_PER_CLIENT; i++) {
        const std::string& key = my_keys[i];
        c.find(Slice(key.data(), key.size()));
        if (c.is_valid()) {
          c.remove();
          log_op(id, Op::REMOVE, key);
        } else {
          log_op(id, Op::REMOVE, key + " (NOT FOUND — bug!)");
        }
      }
      c.commit();
    }

    // ── Sync 2: push removes to server ────────────────────────────────────
    log_op(id, Op::SYNC_START);
    {
      std::lock_guard<std::mutex> lock(server_mutex);
      auto r = sync_bidirectional(server_storage, client_db);
      if (!r.ok) { thread_errors[id] = "sync2: " + r.error; return; }
    }
    log_op(id, Op::SYNC_DONE);
  };

  // ── Run all clients concurrently ─────────────────────────────────────────
  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_CLIENTS; i++)
    threads.emplace_back(worker, i);
  for (auto& t : threads)
    t.join();

  // ── Print replay log ──────────────────────────────────────────────────────
  {
    std::ostringstream oss;
    dump_log(oss);
    BOOST_TEST_MESSAGE("=== Action log (" << g_log.size() << " entries) ===\n"
                       << oss.str() << "=== End of log ===");
  }

  // ── Report thread errors ──────────────────────────────────────────────────
  for (int i = 0; i < NUM_CLIENTS; i++)
    if (!thread_errors[i].empty())
      BOOST_FAIL("client " << i << ": " << thread_errors[i]);

  // ── Verify server state ───────────────────────────────────────────────────
  {
    auto server_db = server_storage->open<_ReplicationDB>("main");
    auto c = server_db.cursor();
    std::vector<std::string> server_keys;
    c.first();
    while (c.is_valid()) {
      server_keys.push_back(c.key().string());
      c.next();
    }
    BOOST_CHECK_EQUAL(static_cast<int>(server_keys.size()), EXPECTED_TOTAL);
    if (static_cast<int>(server_keys.size()) != EXPECTED_TOTAL) {
      std::ostringstream oss;
      oss << "Server has " << server_keys.size()
          << " items, expected " << EXPECTED_TOTAL << "\n";
      for (const auto& k : server_keys) oss << "  " << k << "\n";
      BOOST_TEST_MESSAGE(oss.str());
    }
  }

  // ── Final sync: each client converges to the full server state ────────────
  for (int i = 0; i < NUM_CLIENTS; i++) {
    auto client_db = client_storages[i]->open<_ReplicationDB>("main");
    {
      std::lock_guard<std::mutex> lock(server_mutex);
      auto r = sync_bidirectional(server_storage, client_db);
      BOOST_CHECK_MESSAGE(r.ok,
          "final sync client " << i << " failed: " << r.error);
    }
    auto c = client_db.cursor();
    int count = 0;
    c.first();
    while (c.is_valid()) { count++; c.next(); }
    BOOST_CHECK_EQUAL(count, EXPECTED_TOTAL);
    if (count != EXPECTED_TOTAL)
      BOOST_TEST_MESSAGE("client " << i << " has " << count
                         << " after final sync, expected " << EXPECTED_TOTAL);
  }
}
