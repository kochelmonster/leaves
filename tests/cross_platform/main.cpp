#include <filesystem>
#include <iostream>
#include <queue>
#include <string>
#include <system_error>
#include <vector>

#include "leaves/confluence.hpp"
#include "leaves/mmap.hpp"
#include "leaves/replication.hpp"

namespace fs = std::filesystem;

using namespace leaves;

namespace {

void log_progress(const std::string& message) {
  std::cerr << message << std::endl;
}

int fail(const char* message) {
  std::cerr << "[FAIL] " << message << std::endl;
  return 1;
}

struct LoopbackTransport : ReplicationTransport {
  std::queue<std::vector<uint8_t>> incoming;
  LoopbackTransport* peer = nullptr;

  void set_peer(LoopbackTransport* p) { peer = p; }

  void send(const uint8_t* data, size_t size) override {
    if (peer) {
      peer->incoming.emplace(data, data + size);
    }
  }

  bool has_message() const { return !incoming.empty(); }

  std::vector<uint8_t> receive() {
    if (incoming.empty()) {
      return {};
    }
    auto msg = std::move(incoming.front());
    incoming.pop();
    return msg;
  }
};

struct EventTracker : ReplicationEvents {
  bool completed = false;
  bool errored = false;

  void on_complete(uint64_t, size_t) override { completed = true; }

  void on_error(uint64_t, ReplicationError, const char*) override {
    errored = true;
  }

  void on_progress(uint64_t, size_t, size_t) override {}
};

int exercise_map_storage_base(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);

  const std::string path_string = path.string();
  const std::string db_name = "smoke_base";

  {
    auto storage = MapStorage::create(path_string.c_str());
    auto db = storage->open(db_name);
    auto cursor = db.cursor();

    cursor.find("alpha");
    if (cursor.is_valid()) {
      return fail("unexpected pre-existing alpha key");
    }

    cursor.value("bravo");
    if (!cursor.is_valid()) {
      return fail("inserted alpha key is not valid");
    }
    cursor.commit();

    cursor.find("charlie");
    cursor.value("delta");
    cursor.commit();

    cursor.first();
    if (!cursor.is_valid() || cursor.key() != Slice("alpha") ||
        cursor.value() != Slice("bravo")) {
      return fail("first key/value mismatch after initial insert");
    }

    cursor.next();
    if (!cursor.is_valid() || cursor.key() != Slice("charlie") ||
        cursor.value() != Slice("delta")) {
      return fail("second key/value mismatch after initial insert");
    }

    cursor.next();
    if (cursor.is_valid()) {
      return fail("unexpected extra row after second insert");
    }
  }

  {
    auto storage = MapStorage::create(path_string.c_str());
    auto db = storage->open(db_name);
    auto cursor = db.cursor();

    cursor.find("alpha");
    if (!cursor.is_valid() || cursor.key() != Slice("alpha") ||
        cursor.value() != Slice("bravo")) {
      return fail("persistent alpha key/value mismatch");
    }

    cursor.find("charlie");
    if (!cursor.is_valid() || cursor.key() != Slice("charlie") ||
        cursor.value() != Slice("delta")) {
      return fail("persistent charlie key/value mismatch");
    }

    cursor.first();
    if (!cursor.is_valid() || cursor.key() != Slice("alpha") ||
        cursor.value() != Slice("bravo")) {
      return fail("persistent iteration start mismatch");
    }
  }

  fs::remove(path, ec);

  std::cout << "MapStorage baseline smoke test passed\n";
  return 0;
}

int exercise_replication(const fs::path& src_path, const fs::path& dst_path) {
  std::error_code ec;
  fs::remove(src_path, ec);
  fs::remove(dst_path, ec);

  const std::string src_string = src_path.string();
  const std::string dst_string = dst_path.string();
  const std::string db_name = "smoke_replication";

  auto src_storage = MapStorage::create(src_string.c_str());
  auto dst_storage = MapStorage::create(dst_string.c_str());

  auto src_db = src_storage->open<MapStorage::ReplicationDB>(db_name);
  {
    auto c = src_db.cursor();
    c.start_transaction();
    c.find(Slice("alpha"));
    c.value(Slice("bravo"));
    c.find(Slice("charlie"));
    c.value(Slice("delta"));
    c.commit();
  }

  auto dst_db = dst_storage->open<MapStorage::ReplicationDB>(db_name);

  LoopbackTransport sender_tx;
  LoopbackTransport receiver_tx;
  sender_tx.set_peer(&receiver_tx);
  receiver_tx.set_peer(&sender_tx);

  EventTracker sender_events;
  EventTracker receiver_events;

  ReplicationSender<MapStorage> sender(src_db);
  ReplicationReceiver<MapStorage> receiver(dst_db);

  receiver.begin(&receiver_tx, &receiver_events);
  sender.begin(&sender_tx, &sender_events);

  run_replication(sender, receiver, sender_tx, receiver_tx);

  if (sender.state() != ReplicationState::IDLE ||
      receiver.state() != ReplicationState::IDLE) {
    return fail("replication did not reach idle state");
  }
  if (!sender_events.completed || !receiver_events.completed ||
      sender_events.errored || receiver_events.errored) {
    return fail("replication events did not report successful completion");
  }

  {
    auto c = dst_db.cursor();
    c.find(Slice("alpha"));
    if (!c.is_valid() || c.value() != Slice("bravo")) {
      return fail("replicated alpha key/value mismatch");
    }
    c.find(Slice("charlie"));
    if (!c.is_valid() || c.value() != Slice("delta")) {
      return fail("replicated charlie key/value mismatch");
    }
  }

  fs::remove(src_path, ec);
  fs::remove(dst_path, ec);
  std::cout << "ReplicationDB smoke test passed\n";
  return 0;
}

int exercise_confluence(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);

  const std::string path_string = path.string();
  const std::string db_name = "smoke_confluence";

  {
    auto storage = MapStorage::create(path_string.c_str());
    auto db = storage->open<MapStorage::ConfluenceDB>(db_name);
    auto c = db.cursor();

    if (!c.start_transaction()) {
      return fail("confluence start_transaction failed");
    }
    c.find(Slice("alpha"));
    c.value(Slice("bravo"));
    if (!c.commit()) {
      return fail("confluence first commit failed");
    }

    if (!c.start_transaction()) {
      return fail("confluence second start_transaction failed");
    }
    c.find(Slice("charlie"));
    c.value(Slice("delta"));
    if (!c.commit()) {
      return fail("confluence second commit failed");
    }

    db.merge_all_now();
  }

  {
    auto storage = MapStorage::create(path_string.c_str());
    auto db = storage->open<MapStorage::ConfluenceDB>(db_name);
    auto c = db.cursor();

    c.find(Slice("alpha"));
    if (!c.is_valid() || c.key() != Slice("alpha") ||
        c.value() != Slice("bravo")) {
      return fail("confluence persistent alpha key/value mismatch");
    }

    c.find(Slice("charlie"));
    if (!c.is_valid() || c.key() != Slice("charlie") ||
        c.value() != Slice("delta")) {
      return fail("confluence persistent charlie key/value mismatch");
    }

    c.first();
    if (!c.is_valid() || c.key() != Slice("alpha") ||
        c.value() != Slice("bravo")) {
      return fail("confluence persistent iteration start mismatch");
    }

    c.next();
    if (!c.is_valid() || c.key() != Slice("charlie") ||
        c.value() != Slice("delta")) {
      return fail("confluence persistent second row mismatch");
    }
  }

  fs::remove(path, ec);
  std::cout << "ConfluenceDB smoke test passed\n";
  return 0;
}

}  // namespace

int main() {
  const fs::path temp_dir = fs::temp_directory_path();

  log_progress("[INFO] leaves_cross_platform_smoke starting");
  log_progress("[INFO] temp_directory_path=" + temp_dir.string());

  log_progress("[INFO] phase=MapStorage baseline start");

  if (int rc = exercise_map_storage_base(
          temp_dir / "leaves-cross-platform-map-base.lvs")) {
    return rc;
  }
  log_progress("[INFO] phase=MapStorage baseline done");

  log_progress("[INFO] phase=Replication start");

  if (int rc = exercise_replication(
          temp_dir / "leaves-cross-platform-map-repl-src.lvs",
          temp_dir / "leaves-cross-platform-map-repl-dst.lvs")) {
    return rc;
  }
  log_progress("[INFO] phase=Replication done");

  log_progress("[INFO] phase=Confluence start");

  if (int rc = exercise_confluence(
          temp_dir / "leaves-cross-platform-map-confluence.lvs")) {
    return rc;
  }
  log_progress("[INFO] phase=Confluence done");

  std::cout << "cross-platform smoke tests passed (MapStorage + Replication + Confluence)\n";
  return 0;
}