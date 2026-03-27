#ifndef _LEAVES_REPLICATING_BROWSERSTORE_HPP
#define _LEAVES_REPLICATING_BROWSERSTORE_HPP

#ifdef __EMSCRIPTEN__

#include <blake3.h>

#include <memory>

#include "db.hpp"
#include "intern/replication/_replication_db.hpp"
#include "intern/storage/_browserstore.hpp"

#include <emscripten.h>
#include <emscripten/eventloop.h>

namespace leaves {

// Forward-declare so Self_ can refer to it
struct _ReplicationBrowserStore;

// ── Emscripten setTimeout bridge ────────────────────────────────
//
// emscripten_set_timeout uses a raw C function-pointer callback, so we
// trampoline through a static map that maps timer IDs to std::function
// closures.  The map is only accessed from the main thread (WASM is
// single-threaded when compiled without -pthread).

namespace _browser_sched_detail {

inline std::unordered_map<int, std::function<void()>>& pending_timers() {
  static std::unordered_map<int, std::function<void()>> map;
  return map;
}

inline std::atomic<uint64_t>& next_job_id() {
  static std::atomic<uint64_t> id{1};
  return id;
}

// em_timer_id → leaves job_id mapping (for cancel_job)
inline std::unordered_map<uint64_t, int>& job_to_timer() {
  static std::unordered_map<uint64_t, int> map;
  return map;
}

inline void timer_callback(void* user_data) {
  int timer_id = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
  auto& timers = pending_timers();
  auto it = timers.find(timer_id);
  if (it != timers.end()) {
    auto fn = std::move(it->second);
    timers.erase(it);
    // Also clean up job_to_timer reverse mapping
    auto& j2t = job_to_timer();
    for (auto jt = j2t.begin(); jt != j2t.end(); ++jt) {
      if (jt->second == timer_id) {
        j2t.erase(jt);
        break;
      }
    }
    fn();
  }
}

}  // namespace _browser_sched_detail

// Replication-enabled CacheStore for browser/IndexedDB.
// Passes itself as Self_ so that DB::_storage is typed as
// _ReplicationBrowserStore& — giving direct access to the
// browser-native schedule_after() / cancel_job() / wait_all().
struct _ReplicationBrowserStore
    : public _CacheStore<_BrowserStoreTraits, _BrowserOperations,
                         _ReplicationDB, _ReplicationBrowserStore> {
  using Base = _CacheStore<_BrowserStoreTraits, _BrowserOperations,
                           _ReplicationDB, _ReplicationBrowserStore>;
  using DB = typename Base::DB;
  using DBEntry = typename Base::DBEntry;
  using FileHeader = typename _BrowserOperations::FileHeader;

  size_t _hash_threads = 1;  // always single-threaded in browser

  _ReplicationBrowserStore(const char* db_name, uint16_t db_count = 48,
                           size_t capacity = 100 * M)
      : Base(db_count, capacity, 0, _BrowserStoreTraits::AREA_SIZE) {
    _init_browser_db(db_name, db_count);
  }

  ~_ReplicationBrowserStore() {
    _dbs.clear();     // Destroy DBs first (cancels purge jobs)
    destroy();        // Flush and close
    delete[] reinterpret_cast<char*>(_header);
  }

  // ── Browser-native scheduling (shadows _ThreadPoolMixin) ──────

  template <typename Rep, typename Period>
  uint64_t schedule_after(std::chrono::duration<Rep, Period> delay,
                          std::function<void()> task) {
    using namespace _browser_sched_detail;
    uint64_t job_id = next_job_id().fetch_add(1, std::memory_order_relaxed);

    double delay_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(delay).count();

    // Use the timer_id as the user_data (cast to void*)
    static std::atomic<int> next_timer{1};
    int timer_id = next_timer.fetch_add(1, std::memory_order_relaxed);

    pending_timers()[timer_id] = std::move(task);
    job_to_timer()[job_id] = timer_id;

    emscripten_set_timeout(
        timer_callback,
        delay_ms,
        reinterpret_cast<void*>(static_cast<intptr_t>(timer_id)));

    return job_id;
  }

  void cancel_job(uint64_t job_id) {
    using namespace _browser_sched_detail;
    auto& j2t = job_to_timer();
    auto it = j2t.find(job_id);
    if (it != j2t.end()) {
      int timer_id = it->second;
      emscripten_clear_timeout(timer_id);
      pending_timers().erase(timer_id);
      j2t.erase(it);
    }
  }

  void wait_all() {
    // No-op in single-threaded browser environment.
    // Scheduled tasks fire asynchronously via the event loop.
  }

  // ── Init / sanitize (mirrors _BrowserStore) ───────────────────

  void _init_browser_db(const char* db_name, uint16_t db_count) {
    size_t header_size =
        leaves::padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
    char* buffer = new char[header_size];

    open(db_name);

    bool exists = _try_load_header(buffer, header_size);

    if (!exists) {
      _header = new (buffer) FileHeader(db_count);
      _header->file_size =
          leaves::padding(header_size, _BrowserStoreTraits::AREA_SIZE);
      write(0, buffer, header_size);
    } else {
      _header = reinterpret_cast<FileHeader*>(buffer);
      if (strcmp(_header->signature, BROWSERSTORE_SIGNATURE))
        throw std::runtime_error("Invalid browser store signature");
      if (_header->db_count != db_count)
        throw WrongValue("db_count may not be changed.");
    }

    assert(((uint64_t)_header & 7) == 0);
    _sanitize();
  }

  bool _try_load_header(char* buffer, size_t header_size) {
    try {
      read(0, buffer, header_size);
      auto* h = reinterpret_cast<FileHeader*>(buffer);
      return strcmp(h->signature, BROWSERSTORE_SIGNATURE) == 0;
    } catch (...) {
      return false;
    }
  }

  void _sanitize() {
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        assert(!_dbs[i]);
        DB(*this, _header->dbs[i].offset, i).sanitize();
      }
    }
  }

  // Override make() to set hash_threads and start purge
  DB* make(const char* name) {
    DB* db = Base::make(name);
    db->_hash_threads = _hash_threads;
    db->start_purge();
    return db;
  }

  DB* operator[](const char* name) { return make(name); }
};

// ── Public wrapper ──────────────────────────────────────────────

class ReplicatingBrowserStorage
    : public std::enable_shared_from_this<ReplicatingBrowserStorage> {
 public:
  typedef _ReplicationBrowserStore StorageImpl;
  typedef TDB<ReplicatingBrowserStorage> DB;
  typedef std::shared_ptr<ReplicatingBrowserStorage> storage_ptr;

  ReplicatingBrowserStorage(const char* db_name, uint16_t db_count = 48,
                            size_t capacity = 100 * M)
      : _storage(
            std::make_unique<StorageImpl>(db_name, db_count, capacity)) {}

  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  static storage_ptr create(const char* db_name, uint16_t db_count = 48,
                            size_t capacity = 100 * M) {
    return std::make_shared<ReplicatingBrowserStorage>(db_name, db_count,
                                                      capacity);
  }

 private:
  friend class TDB<ReplicatingBrowserStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#else  // !__EMSCRIPTEN__

namespace leaves {

struct _ReplicationBrowserStore {
  _ReplicationBrowserStore(...) {
    static_assert(sizeof(_ReplicationBrowserStore) == 0,
                  "ReplicatingBrowserStorage requires Emscripten compilation");
  }
};

class ReplicatingBrowserStorage {
 public:
  ReplicatingBrowserStorage(...) {
    static_assert(sizeof(ReplicatingBrowserStorage) == 0,
                  "ReplicatingBrowserStorage requires Emscripten compilation");
  }
};

}  // namespace leaves

#endif  // __EMSCRIPTEN__

#endif  // _LEAVES_REPLICATING_BROWSERSTORE_HPP
