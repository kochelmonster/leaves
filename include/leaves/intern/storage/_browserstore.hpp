#ifndef _LEAVES__BROWSERSTORE_HPP
#define _LEAVES__BROWSERSTORE_HPP

/**
 * _BrowserStorage: WebAssembly-compatible storage using IndexedDB
 *
 * This provides a browser-based alternative to _FileStore, using IndexedDB
 * as the persistence layer. It maintains API compatibility with _CacheStore
 * while adapting to the async, key-value nature of IndexedDB.
 *
 * Architecture:
 * - IndexedDB database stores areas as key-value pairs
 * - Object store "header" stores file header and metadata
 * - Object store "areas" stores data blocks keyed by offset
 * - Uses Emscripten Asyncify for synchronous-looking async operations
 *
 * Build requirements:
 * - Compile with Emscripten: emcc -sASYNCIFY
 * - Link flags: -sASYNCIFY
 * -sASYNCIFY_ADD_IMPORTS=['emscripten_idb_load','emscripten_idb_store','emscripten_idb_delete']
 */

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/_exception.hpp"
#include "../core/_node.hpp"
#include "../core/_traits.hpp"
#include "../db/_db.hpp"
#include "../memory/_memory.hpp"
#include "_cachestore.hpp"

// EM_JS functions must be at file scope (outside namespace).
// Async IDB completion is tracked in JS and polled from C++.
//
// C++ owns the write queue and coalescing map using SmartPointer<Area>.
// JS only tracks write request state and performs the actual async IDB write.
EM_JS(void, leaves_init_write_state, (), {
  if (Module._leavesWriteState) {
    return;
  }

  var state = {
    firstError: null,
    writeStatus: Object.create(null),
    cppQueued: 0,
    inFlight: 0,
    nextRequestId: 1,
  };

  Module._leavesWriteState = state;

  function formatError(error) {
    if (!error) {
      return 'unknown error';
    }
    if (typeof error === 'string') {
      return error;
    }
    if (error.message) {
      return error.message;
    }
    return String(error);
  }
  state.formatError = formatError;
});

EM_JS(void, leaves_idb_cpp_queued_delta, (int delta), {
  var s = Module._leavesWriteState;
  if (!s) {
    return;
  }
  s.cppQueued += delta;
  if (s.cppQueued < 0) {
    s.cppQueued = 0;
  }
});

// Start one async IDB write and return a request id for polling.
// Returns 0 only if write state is unavailable.
EM_JS(uint32_t, leaves_idb_async_write_start,
      (const char* db, const char* key, const void* data, int size), {
        var s = Module._leavesWriteState;
        if (!s) {
          err('[leaves] IDB async write init missing state');
          return 0;
        }

        var dbName = UTF8ToString(db);
        var keyName = UTF8ToString(key);
        var reqId = s.nextRequestId++;
        s.writeStatus[reqId] = 0;
        s.inFlight += 1;

        function complete(status, error) {
          if (s.inFlight > 0) {
            s.inFlight -= 1;
          } else {
            s.inFlight = 0;
          }
          if (status === 2) {
            var formatter = s.formatError || function(x) { return String(x); };
            var details = '[leaves] IDB async write error for db=' + dbName +
                          ' key=' + keyName +
                          ' reason=' + formatter(error);
            if (!s.firstError) {
              s.firstError = details;
            }
            err(details);
          }
          s.writeStatus[reqId] = status;
        }

        try {
          var payload = new Uint8Array(HEAPU8.subarray(data, data + size));
          IDBStore.setFile(dbName, keyName, payload, function(error) {
            if (error) {
              complete(2, error);
              return;
            }
            complete(1, null);
          });
        } catch (error) {
          complete(2, error);
        }

        return reqId;
      });

// Poll request status.
// 0: still in flight, 1: success, 2: failed, 3: unknown/consumed.
EM_JS(int, leaves_idb_async_write_poll, (uint32_t request_id), {
  var s = Module._leavesWriteState;
  if (!s) {
    return 3;
  }
  var st = s.writeStatus[request_id];
  if (st === undefined) {
    return 3;
  }
  if (st === 0) {
    return 0;
  }
  delete s.writeStatus[request_id];
  return st;
});

EM_JS(int, leaves_pending_writes, (), {
  var s = Module._leavesWriteState;
  if (s) {
    return s.inFlight + s.cppQueued;
  }
  return 0;
});

EM_JS(int, leaves_idb_store_error_size, (), {
  var s = Module._leavesWriteState;
  if (!s || !s.firstError) {
    return 0;
  }
  return lengthBytesUTF8(s.firstError) + 1;
});

EM_JS(void, leaves_idb_take_store_error, (char* out, int out_size), {
  var s = Module._leavesWriteState;
  if (!s || !s.firstError) {
    return;
  }

  if (out && out_size > 0) {
    stringToUTF8(s.firstError, out, out_size);
  }
  s.firstError = null;
});

namespace leaves {

static const char BROWSERSTORE_SIGNATURE[] = "leaves-browserstore";
static const size_t BROWSERSTORE_SIGNATURE_SIZE =
    padding(sizeof(BROWSERSTORE_SIGNATURE), 8);

// Stack-allocated helper for IDB keys — eliminates std::string allocation
// on every I/O call. Keys are always "area_<uint64_t>", which fits in 32 bytes.
static inline const char* idb_key_format(char* buf, size_t buf_size,
                                          uint64_t n) {
  std::snprintf(buf, buf_size, "area_%llu", (unsigned long long)n);
  return buf;
}

// Store traits - same as _StoreTraits but tuned for browser environment
// Parameterized on AspectType to allow JS callbacks via JSAspect.
template <typename AspectType = DefaultAspect>
struct _BrowserStoreTraits {
  using Aspect = AspectType;
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  struct PageHeader {
    typedef PageHeader Base;
    tid_t txn_id;
    uint16_e used;
    uint8_t slot_id;

    template <typename DB>
    bool needs_cow(const DB* db) const {
      return txn_id != db->transaction_active();
    }
  };

  // Slightly smaller sizes for browser memory constraints
  static constexpr size_t MAX_KEY_SIZE = 512 * K;
  static constexpr size_t AREA_SIZE = 256 * K;
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  // PAGE_SIZES values include sizeof(PageHeader); they are matched against
  // (space + sizeof(PageHeader)) by _DB::alloc_page / _MemManager::assign_slot.
  //
  // These 10 buckets are the optimized layout derived from
  // benchmarks/bench_page_sizes_browser + tools/page_sizes_solver.py,
  // running against fake-indexeddb on emscripten. The allocation
  // histogram is captured directly from _DB::alloc_page hooks under
  // BrowserStorage (LEAVES_PAGE_HIST), so the buckets reflect actual
  // browser-backend workloads rather than mmap-backend inference.
  // Aggregate waste vs. the prior file-backend buckets drops ~3 pp on
  // most scenarios and ~10 pp on hex_strings/v1000.
  static constexpr uint16_t PAGE_SIZES[] = {
      40,    // smallest TrieNode + PageHeader
      56,    // 3 branches
      64,    // 4 branches (8B-aligned)
      80,    // 5-6 branches
      120,   // 7-8 branches
      152,   // hex 0-9A-F peak
      192,   // mid-fanout (10-12 branches)
      1024,  // mid-fanout peak
      1056,  // mid-fanout peak (adjacent, different vsize regime)
      1536,  // for data nodes
      2560,  // for data nodes
      3072,  // for data nodes
      4096,  // PAGE_CONTAINER_SIZE cap (binary / overflow)
  };
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);
  using ptr = SmartPointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SmartPointer<T, type>;
};

/**
 * IndexedDB operations wrapper for Emscripten
 *
 * Uses emscripten_idb_* APIs which require Asyncify for synchronous behavior.
 * Each IDB operation is wrapped to provide a file-like interface.
 *
 * Template parameter AreaSize defines the area alignment used for IDB keys.
 * This must match the trait AREA_SIZE so that reads at arbitrary content offsets
 * (e.g. a DB header at area_base + sizeof(Area)) resolve to the correct stored area.
 */
template <size_t AreaSize_ = 64 * 1024>
struct _BrowserOperations : _CacheBase {
  // Align IDB keys to this boundary — matches the storage's AREA_SIZE.
  // Areas are always stored under key "area_<aligned_offset>"; reads at
  // any offset within the area are served from the area data starting at
  // the appropriate sub-offset.
  static constexpr size_t AREA_ALIGNMENT = AreaSize_;

  struct QueuedWrite {
    SmartPointer<Area> area_owner;
    uint64_t store_key = 0;
  };

  bool has_pending_writes() const {
    std::lock_guard<std::mutex> lock(_write_mutex);
    _poll_write_completion_locked();
    if (!_write_inflight) {
      _start_next_write_locked();
    }
    return _write_inflight || !_queued_writes.empty();
  }

  std::exception_ptr get_store_error() const {
    int size = leaves_idb_store_error_size();
    if (size <= 0) {
      return nullptr;
    }

    std::vector<char> message(static_cast<size_t>(size), '\0');
    leaves_idb_take_store_error(message.data(), size);
    if (message.empty() || message[0] == '\0') {
      return nullptr;
    }

    return std::make_exception_ptr(LeavesException(std::string(message.data())));
  }

  // No mutex needed for single-threaded browser environment
  struct Mutex {
    template <typename Time = std::chrono::seconds>
    void lock(Time /*t*/ = Time(10)) {}
    bool try_lock() { return true; }
    void unlock() {}
  };

  struct FileHeader {
    char signature[BROWSERSTORE_SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;  // Logical file size for compatibility
    Mutex file_lock;
    AreaPool area_pool;
    uint32_t sanitize_generation;  // incremented on each storage open
    uint16_t db_entry_count;       // entries used in first directory page
    offset_t db_next_page;         // link to overflow directory page (0 = none)
    DBEntry dbs[];                 // flexible array fills to 4K boundary

    FileHeader()
        : signature{},
          db_version(0),
          file_size(0),
          file_lock{},
          area_pool{},
          sanitize_generation(0),
          db_entry_count(0),
          db_next_page(0) {
      std::memset(signature, 0, sizeof(signature));
      std::strcpy(signature, BROWSERSTORE_SIGNATURE);
      area_pool.init();
      uint16_t cap = _DBDirectoryPage::capacity_for(4 * K - sizeof(FileHeader));
      std::memset((void*)dbs, 0, sizeof(DBEntry) * cap);
    }
  };

  std::string _store_name;  // Object store name (IndexedDB name) — set once in init()
  FileHeader* _header;
  mutable std::mutex _io_mutex;  // For thread-safety even in browser
  mutable std::mutex _write_mutex;
  mutable std::deque<uint64_t> _write_order;
  mutable std::unordered_map<uint64_t, QueuedWrite> _queued_writes;
  mutable QueuedWrite _inflight_write;
  mutable uint32_t _inflight_request_id = 0;
  mutable bool _write_inflight = false;

  // Read-ahead cache: avoids double IDB round-trips when resolve()
  // first reads the AreaSlice header, then the full area from the same key.
  // Uses raw pointers instead of vector to eliminate a heap copy.
  mutable uint64_t _read_cache_key = UINT64_MAX;
  mutable void* _read_cache_data = nullptr;   // owned (malloc'd from emscripten_idb_load)
  mutable int _read_cache_size = 0;

  ~_BrowserOperations() {
    if (_read_cache_data) {
      free(_read_cache_data);
      _read_cache_data = nullptr;
    }
  }

  size_t file_size() const { return _header->file_size; }

  size_t calc_header_size() const { return 4 * K; }

  // Initialize IndexedDB connection
  void init(const char* name) {
    _store_name = name;
    leaves_init_write_state();
    // IndexedDB initialization happens on first access
  }

  void close() {
    // IndexedDB connections auto-close; flush any pending data
    _flush_to_idb();
  }

  // Read data from IndexedDB
  void read(offset_t offset, void* ptr, size_t size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    _idb_load_data(offset, ptr, size);
  }

  // Resize is a no-op for IndexedDB (no fixed file size)
  void resize(size_t new_size) const {
    // IndexedDB grows automatically; just track logical size
    // Header update happens separately
  }

  template <typename BlockVector>
  void write_batch(BlockVector& blocks_to_write, bool write_header = false) {
    // Enqueue writes in FIFO order while coalescing same-key pending writes.
    // Queue ownership stays in C++ as SmartPointer<Area> to avoid enqueue-time
    // byte copies for normal area writes.
    size_t total_bytes = 0;
    for (const auto& block : blocks_to_write) {
      SmartPointer<Area> area(static_cast<Area*>(block.area()));
      total_bytes += area->size();
      _enqueue_write(_make_area_write(std::move(area)));
    }

    if (write_header) {
      _enqueue_write(_make_header_write());
    }
    if (!blocks_to_write.empty()) {
      LEAVES_INTERNAL_LOG(LEAVES_LOG_DEBUG, "[flush] %zu areas, %zu KB async to IDB\n",
                         blocks_to_write.size(), total_bytes / 1024);
    }
  }

  // Wait until this store's queued and in-flight writes are fully drained.
  void wait_for_writes() {
#if defined(LEAVES_LOG)
    const double started = emscripten_get_now();
    double next_log = started + 1000.0;
#endif
    while (true) {
      int pending = 0;
      {
        std::lock_guard<std::mutex> lock(_write_mutex);
        _poll_write_completion_locked();
        if (!_write_inflight) {
          _start_next_write_locked();
        }
        pending = static_cast<int>(_queued_writes.size()) +
                  (_write_inflight ? 1 : 0);
      }

      if (pending == 0) {
        break;
      }
#if defined(LEAVES_LOG)
      const double now = emscripten_get_now();
      if (now >= next_log) {
        LEAVES_INTERNAL_LOG(LEAVES_LOG_DEBUG, "[diag] wait_for_writes waiting: store=%s pending=%d elapsed=%.0fms\n",
            _store_name.c_str(), pending, now - started);
        next_log = now + 1000.0;
      }
#endif
      emscripten_sleep(1);
    }
  }

  const char* filename() const { return _store_name.c_str(); }

  Mutex& file_lock() { return _header->file_lock; }

  // Queue an aligned area write and start draining if idle.
  void _enqueue_write(QueuedWrite&& write) const {
    std::lock_guard<std::mutex> lock(_write_mutex);
    auto it = _queued_writes.find(write.store_key);
    if (it != _queued_writes.end()) {
      it->second = std::move(write);
      return;
    }

    _write_order.push_back(write.store_key);
    _queued_writes.emplace(write.store_key, std::move(write));
    leaves_idb_cpp_queued_delta(1);

    _poll_write_completion_locked();
    if (!_write_inflight) {
      _start_next_write_locked();
    }
  }

  QueuedWrite _make_area_write(SmartPointer<Area>&& area) const {
    QueuedWrite write;
    uint64_t aligned_offset = area->offset();
    assert((aligned_offset % AREA_ALIGNMENT) == 0);
    assert(area->size() >= AREA_ALIGNMENT);
    write.store_key = aligned_offset;
    write.area_owner = std::move(area);
    return write;
  }

  // Header writes are serialized as full aligned blocks under key area_0.
  // The queued item still owns its payload through SmartPointer<Area>.
  QueuedWrite _make_header_write() const {
    const size_t payload_size = AREA_ALIGNMENT;
    const size_t alloc_size = sizeof(Area) + payload_size;

    Area* owner = reinterpret_cast<Area*>(::operator new(alloc_size));
    owner->init(0, static_cast<uint32_t>(payload_size), 0);
    owner->_ref.store(0, std::memory_order_relaxed);

    char* payload = reinterpret_cast<char*>(owner) + sizeof(Area);
    std::memset(payload, 0, payload_size);
    std::memcpy(payload, _header, calc_header_size());

    QueuedWrite write;
    write.store_key = 0;
    write.area_owner = SmartPointer<Area>(owner);
    return write;
  }

  void _poll_write_completion_locked() const {
    if (!_write_inflight) {
      return;
    }

    int status = leaves_idb_async_write_poll(_inflight_request_id);
    if (status == 0) {
      return;
    }

    _inflight_request_id = 0;
    _write_inflight = false;
    _inflight_write = QueuedWrite{};
  }

  void _start_next_write_locked() const {
    if (_write_inflight) {
      return;
    }

    while (!_write_order.empty()) {
      uint64_t key = _write_order.front();
      _write_order.pop_front();

      auto it = _queued_writes.find(key);
      if (it == _queued_writes.end()) {
        continue;
      }

      QueuedWrite next = std::move(it->second);
      _queued_writes.erase(it);

      char key_buf[32];
      idb_key_format(key_buf, sizeof(key_buf), key);

      const bool is_header_write = (key == 0);
      const char* owner_bytes = static_cast<const char*>(next.area_owner);
      const uint8_t* write_data = reinterpret_cast<const uint8_t*>(
          owner_bytes + (is_header_write ? sizeof(Area) : 0));
      int write_size = static_cast<int>(next.area_owner->size());
      if (is_header_write) {
        assert(next.area_owner->size() >= AREA_ALIGNMENT);
      }

      LEAVES_INTERNAL_LOG(LEAVES_LOG_DEBUG,
                          "_idb_queue_start_write: key=%llu size=%d\n",
                          (unsigned long long)key,
                          write_size);

      uint32_t request_id = leaves_idb_async_write_start(
          _store_name.c_str(), key_buf, write_data, write_size);
      if (request_id == 0) {
        // Keep the write queued and let a later pump retry.
        _queued_writes.emplace(key, std::move(next));
        _write_order.push_front(key);
        return;
      }

      leaves_idb_cpp_queued_delta(-1);
      _inflight_write = std::move(next);
      _inflight_request_id = request_id;
      _write_inflight = true;
      return;
    }
  }

  // IndexedDB load operation using Emscripten Asyncify
  //
  // All data is stored under key "area_<aligned_offset>" where aligned_offset
  // is the offset rounded down to AREA_ALIGNMENT.  When reading at an
  // arbitrary content offset (e.g. a DB header at area_base + sizeof(Area)),
  // we align the key to AREA_ALIGNMENT, load the full area, then copy the
  // requested sub-range.  The header at offset 0 is stored as "area_0".
  void _idb_load_data(uint64_t key, void* data, size_t size) const {
    // Align the key down to AREA_ALIGNMENT.
    uint64_t aligned_key = key - (key % AREA_ALIGNMENT);
    uint64_t sub_offset = key - aligned_key;  // offset within the loaded area
    char key_buf[32];
    idb_key_format(key_buf, 32, aligned_key);

    LEAVES_INTERNAL_LOG(LEAVES_LOG_DEBUG, "_idb_load_data: key=%llu key_str='%s' sub_offset=%llu size=%zu\n",
      (unsigned long long)key, key_buf, (unsigned long long)sub_offset,
      size);

    // Serve from read-ahead buffer if same aligned key
    if (aligned_key == _read_cache_key && _read_cache_data) {
      if (sub_offset + size <= static_cast<size_t>(_read_cache_size)) {
        std::memcpy(data, static_cast<const char*>(_read_cache_data) + sub_offset, size);
      } else {
        size_t copy_size = std::min<size_t>(size, static_cast<size_t>(_read_cache_size) - sub_offset);
        std::memcpy(data, static_cast<const char*>(_read_cache_data) + sub_offset, copy_size);
        if (copy_size < size) {
          std::memset(static_cast<char*>(data) + copy_size, 0, size - copy_size);
        }
      }
      // Second read consumes the buffer (resolve pattern: header then full)
      _read_cache_key = UINT64_MAX;
      return;
    }

    void* loaded_data = nullptr;
    int loaded_size = 0;
    int error = 0;

    LEAVES_INTERNAL_LOG(LEAVES_LOG_DEBUG, "_idb_load_data: key=%llu key_str='%s' sub_offset=%llu size=%zu\n",
      (unsigned long long)key, key_buf, (unsigned long long)sub_offset,
      size);
    emscripten_idb_load(_store_name.c_str(), key_buf, &loaded_data,
                        &loaded_size, &error);

    if (error || !loaded_data) {
      LEAVES_INTERNAL_LOG(LEAVES_LOG_DEBUG, "_idb_load_data: key=%llu key_str='%s' NOT FOUND — returning zeros (size=%zu)\n",
             (unsigned long long)key, key_buf, size);
      // Key not found - return zeros (new area)
      std::memset(data, 0, size);
      return;
    }

    // Copy the requested sub-range from the loaded area.
    // sub_offset + size may exceed loaded_size for a partial read at the
    // end of the area; clamp to available bytes and zero-fill the rest.
    size_t copy_from_area = 0;
    if (static_cast<size_t>(loaded_size) > sub_offset) {
      copy_from_area = std::min<size_t>(size, static_cast<size_t>(loaded_size) - sub_offset);
      std::memcpy(data, static_cast<const char*>(loaded_data) + sub_offset, copy_from_area);
    }
    if (copy_from_area < size) {
      std::memset(static_cast<char*>(data) + copy_from_area, 0, size - copy_from_area);
    }

    // Buffer this read so the second read for the same aligned key is free.
    // Store the raw malloc'd pointer directly — avoids copying into a vector.
    if (static_cast<size_t>(loaded_size) > sub_offset + size ||
        (static_cast<size_t>(loaded_size) > sub_offset && loaded_size > (int)size)) {
      // Free any previous cache entry before replacing
      if (_read_cache_data) {
        free(_read_cache_data);
      }
      _read_cache_key = aligned_key;
      _read_cache_data = loaded_data;
      _read_cache_size = loaded_size;
      // Note: loaded_data is now owned by _read_cache; do NOT free it below
    } else {
      _read_cache_key = UINT64_MAX;
      // Free the allocated buffer from emscripten
      free(loaded_data);
    }
  }

  // Flush any pending data
  void _flush_to_idb() const {
    // Async writes drain via wait_for_writes() called from _CacheStore
  }
};

/**
 * _BrowserStore: Main storage class for WebAssembly/browser environment
 *
 * Usage:
 *   _BrowserStore store("my_database", 16, 100 * M);
 *   auto* db = store["my_collection"];
 *   db->put(key, value);
 *
 * Template parameter AspectType selects the Aspect implementation
 * (DefaultAspect for no-op, JSAspect for JS callbacks).
 */
template <typename AspectType = DefaultAspect>
struct _BrowserStore
    : _CacheStore<_BrowserStoreTraits<AspectType>,
                  _BrowserOperations<_BrowserStoreTraits<AspectType>::AREA_SIZE>,
                  _BrowserStore<AspectType>> {
 public:
  using traits_t = _BrowserStoreTraits<AspectType>;
  using base_t =
      _CacheStore<traits_t,
                  _BrowserOperations<traits_t::AREA_SIZE>,
                  _BrowserStore<AspectType>>;

  _BrowserStore(const char* store_name, size_t capacity = 100 * M,
                size_t pool_threads = 0)
      : base_t(capacity, pool_threads, traits_t::AREA_SIZE) {
    // Note: pool_threads=0 for browser (single-threaded)
    _init_browser_db(store_name);
  }

  ~_BrowserStore() {
    this->destroy();
    delete[] reinterpret_cast<char*>(this->_header);
  }

  void _init_browser_db(const char* store_name) {
    size_t header_size = 4 * K;
    char* buffer = new char[header_size];

    this->init(store_name);

    // Try to load existing header from IndexedDB
    bool exists = _try_load_header(buffer, header_size);
    if (!exists) {
      // Create new database
      this->_header = new (buffer) typename base_t::Operations::FileHeader();
      // Align file_size to AREA_SIZE so areas are AREA_SIZE-aligned
      this->_header->file_size =
          leaves::padding(header_size, traits_t::AREA_SIZE);
      this->make_header_dirty();
      this->flush(true, true);
    } else {
      this->_header =
          reinterpret_cast<typename base_t::Operations::FileHeader*>(buffer);

      if (strcmp(this->_header->signature, BROWSERSTORE_SIGNATURE)) {
        throw TypeMismatch(
            std::format("Invalid database signature: expected '{}' got '{}'",
                        BROWSERSTORE_SIGNATURE, this->_header->signature));
      }

      // ── Diagnostic: dump all DB directory entries with their on-disk type_id ──
      this->_for_each_db_entry([&](typename base_t::DBEntry& entry) {
        if (entry.offset) {
          _DBHeader<base_t> hdr;
          this->read((uint64_t)entry.offset, &hdr, sizeof(hdr));
        }
        return true;
      });
    }

    assert(((uint64_t)this->_header & 7) == 0);
    this->_sanitize();
  }

  uint32_t sanitize_generation() { return this->_header->sanitize_generation; }

  bool _try_load_header(char* buffer, size_t header_size) {
    try {
      this->read(0, buffer, header_size);
      auto* h =
          reinterpret_cast<typename base_t::Operations::FileHeader*>(buffer);
      return strcmp(h->signature, BROWSERSTORE_SIGNATURE) == 0;
    } catch (...) {
      return false;
    }
  }

  void _sanitize() {
    this->_recover_areas();
    ++this->_header->sanitize_generation;
    this->make_header_dirty();
    this->flush();
  }

  void _recover_areas() {
    auto* self = this;
    _recover_areas<_DBHeader<base_t>, traits_t::AREA_SIZE>(
        this->_header->area_pool,
        [self](auto fn) {
          self->_for_each_db_entry([&](auto& e) {
            if (e.offset) fn(e.offset);
            return true;
          });
        },
        this->_header->file_size,
        leaves::padding(this->calc_header_size(), traits_t::AREA_SIZE),
        [self](uint64_t pos, void* buf, size_t size) {
          self->read(pos, buf, size);
        },
        [self](uint64_t pos, const void* buf, size_t size) {
          offset_t write_offset = static_cast<offset_t>(pos);
          auto block = self->resolve(&write_offset, WRITE);
          const uint64_t rel = pos - block.area()->offset();
          assert(rel + size <= block.area()->size());
          std::memcpy(static_cast<char*>(block), buf, size);
          self->make_dirty(block);
        },
        [self](auto&& mark_occupied_range) {
          struct DirectoryPageHeader {
            uint16_t count;
            offset_t next;
          };

          offset_t next = self->_header->db_next_page;
          const uint64_t max_pages =
              self->_header->file_size / traits_t::AREA_SIZE + 1;
          uint64_t visited_pages = 0;
          while (next && visited_pages++ < max_pages) {
            const uint64_t area_pos = (uint64_t)next - sizeof(Area);
            mark_occupied_range(area_pos, traits_t::AREA_SIZE);
            DirectoryPageHeader page_header{};
            self->read((uint64_t)next, &page_header, sizeof(page_header));
            next = page_header.next;
          }

          if (next) {
            LEAVES_INTERNAL_LOG(
                LEAVES_LOG_ERROR,
                "_BrowserStore::_recover_areas stopping overflow-page occupancy scan due to cycle/overflow guard\n");
          }
        });
    this->flush();
  }

  // Compatibility method
  AreaSlice get_area(size_t size) {
    auto area_ptr = this->alloc_multi_area(size);
    return *area_ptr;
  }

  // Browser-specific: Export database to transferable format
  std::vector<char> export_to_buffer() const {
    std::vector<char> result;
    size_t total_size = this->_header->file_size;
    result.resize(total_size);

    // Export header
    size_t header_size = this->calc_header_size();
    std::memcpy(result.data(), this->_header, header_size);

    // Export all areas (would need iteration over stored keys)
    // This is a simplified version - full implementation would
    // enumerate all IndexedDB keys
    return result;
  }

  // Browser-specific: Import database from buffer
  void import_from_buffer(const std::vector<char>& data) {
    if (data.size() < sizeof(typename base_t::Operations::FileHeader)) {
      throw LeavesException("Invalid import data");
    }

    size_t header_size = this->calc_header_size();
    std::memcpy(static_cast<void*>(this->_header), data.data(), header_size);

    this->make_header_dirty();
    this->flush(true, true);

    // Import areas (simplified - full version would parse all areas)
  }

  // Browser-specific: Clear all data
  void clear_database() {
    // Delete all known keys from IndexedDB
    int error = 0;
    emscripten_idb_delete(this->_db_name.c_str(), "area_0", &error);
    // Reinitialize
    this->_init_browser_db(this->_db_name.c_str());
  }
};

}  // namespace leaves

#else  // !__EMSCRIPTEN__

#include "../db/_aspect.hpp"

// Stub for non-Emscripten builds - provides compile-time error
namespace leaves {

template <typename AspectType = DefaultAspect>
struct _BrowserStore {
  template <typename... Args>
  _BrowserStore(Args&&...) {
    static_assert(sizeof...(Args) < 0,
                  "_BrowserStore requires Emscripten compilation");
  }
};

}  // namespace leaves

#endif  // __EMSCRIPTEN__

#endif  // _LEAVES__BROWSERSTORE_HPP