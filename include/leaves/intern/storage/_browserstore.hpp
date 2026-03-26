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
 * - Link flags: -sASYNCIFY -sASYNCIFY_ADD_IMPORTS=['emscripten_idb_load','emscripten_idb_store','emscripten_idb_delete']
 */

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/val.h>
#include <emscripten/bind.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "_cachestore.hpp"
#include "../db/_db.hpp"
#include "../core/_exception.hpp"
#include "../memory/_memory.hpp"
#include "../core/_node.hpp"
#include "../core/_traits.hpp"

// EM_JS functions must be at file scope (outside namespace).
// Async IDB writes with a JS-only pending counter — avoids calling back
// into WASM from IDB callbacks, which is unsafe during Asyncify sleep.
EM_JS(void, leaves_idb_async_write,
      (const char* db, const char* key, const void* data, int size), {
  if (!Module._leavesWrites) Module._leavesWrites = 0;
  Module._leavesWrites++;
  IDBStore.setFile(UTF8ToString(db), UTF8ToString(key),
    new Uint8Array(HEAPU8.subarray(data, data + size)),
    function(error) {
      Module._leavesWrites--;
      if (error) err('[leaves] IDB async write error');
    });
});

EM_JS(int, leaves_pending_writes, (), {
  return Module._leavesWrites || 0;
});

namespace leaves {

static const char BROWSERSTORE_SIGNATURE[] = "leaves-browserstore";
static const size_t BROWSERSTORE_SIGNATURE_SIZE =
    padding(sizeof(BROWSERSTORE_SIGNATURE), 8);

// Store traits - same as _StoreTraits but tuned for browser environment
struct _BrowserStoreTraits {
  using Aspect = DefaultAspect;
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
  static constexpr size_t AREA_SIZE = 64 * K;  // Smaller blocks for IDB
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t PAGE_SIZES[] = {
      _TrieNode<_BrowserStoreTraits>::size(1, 10),
      _TrieNode<_BrowserStoreTraits>::size(1, 16),
      _TrieNode<_BrowserStoreTraits>::size(1, 64),
      _TrieNode<_BrowserStoreTraits>::size(1, 127),
      _TrieNode<_BrowserStoreTraits>::size(1, 256),
      4 * K};
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
 */
struct _BrowserOperations : _CacheBase {
  bool has_pending_writes() const { return leaves_pending_writes() > 0; }

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
    uint16_t db_count;
    DBEntry dbs[0];

    FileHeader(uint16_t db_count_)
        : signature{},
          db_version(0),
          file_size(0),
          file_lock{},
          area_pool{},
          db_count(db_count_) {
      std::memset(signature, 0, sizeof(signature));
      std::strcpy(signature, BROWSERSTORE_SIGNATURE);
      area_pool.init();
      std::memset((void*)dbs, 0, sizeof(DBEntry) * db_count);
    }
  };

  std::string _db_name;       // IndexedDB database name
  std::string _store_name;    // Object store name
  FileHeader* _header;
  mutable std::mutex _io_mutex;  // For thread-safety even in browser

  // Read-ahead buffer: avoids double IDB round-trips when resolve()
  // first reads the AreaSlice header, then the full area from the same key.
  mutable uint64_t _read_cache_key = UINT64_MAX;
  mutable std::vector<char> _read_cache;

  size_t file_size() const { return _header->file_size; }

  size_t calc_header_size() const {
    return leaves::padding(
        sizeof(FileHeader) + sizeof(DBEntry) * _header->db_count, 4 * K);
  }

  // Initialize IndexedDB connection
  void open(const char* db_name) {
    _db_name = db_name;
    _store_name = "leaves_data";
    // IndexedDB initialization happens on first access
  }

  void close() {
    // IndexedDB connections auto-close; flush any pending data
    _flush_to_idb();
  }

  // Write data to IndexedDB
  // We batch operations by storing entire areas as single records
  void write(offset_t offset, const void* ptr, size_t size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    _idb_store_data(offset, ptr, size);
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
    // Sort by offset for locality (helps with browser cache)
    std::sort(blocks_to_write.begin(), blocks_to_write.end(),
              [](const auto& a, const auto& b) {
                return a.area()->offset() < b.area()->offset();
              });

    // Fire-and-forget async writes — data is copied by Emscripten JS glue
    size_t total_bytes = 0;
    for (const auto& block : blocks_to_write) {
      const auto& area = block.area();
      total_bytes += area->size();
      _idb_async_store_data(area->offset(), area, area->size());
    }

    if (write_header) {
      _idb_async_store_data(0, _header, calc_header_size());
    }
#ifndef NDEBUG
    if (!blocks_to_write.empty()) {
      std::fprintf(stderr, "[flush] %zu areas, %zu KB async to IDB\n",
                   blocks_to_write.size(), total_bytes / 1024);
    }
#endif
  }

  // Drain all in-flight async IDB writes by yielding to the event loop.
  // The pending counter lives in JS (Module._leavesWrites) so IDB
  // callbacks can decrement it without calling back into WASM.
  void sync_writes() {
    while (leaves_pending_writes() > 0) {
      emscripten_sleep(1);
    }
  }

  const char* filename() const { return _db_name.c_str(); }

  Mutex& file_lock() { return _header->file_lock; }

  // IndexedDB store operation using Emscripten Asyncify
  void _idb_store_data(uint64_t key, const void* data, size_t size) const {
    std::string key_str = (key == 0) ? "header" : ("area_" + std::to_string(key));

    int error = 0;
    emscripten_idb_store(
        _db_name.c_str(),
        key_str.c_str(),
        const_cast<void*>(data),
        size,
        &error
    );

    if (error) {
      throw std::runtime_error("IndexedDB store failed for key: " + key_str);
    }
  }

  // IndexedDB load operation using Emscripten Asyncify
  void _idb_load_data(uint64_t key, void* data, size_t size) const {
    std::string key_str = (key == 0) ? "header" : ("area_" + std::to_string(key));

    // Serve from read-ahead buffer if same key (avoids double IDB load)
    if (key == _read_cache_key && !_read_cache.empty()) {
      size_t copy_size = std::min(size, _read_cache.size());
      std::memcpy(data, _read_cache.data(), copy_size);
      if (copy_size < size) {
        std::memset(static_cast<char*>(data) + copy_size, 0, size - copy_size);
      }
      // Second read consumes the buffer (resolve pattern: header then full)
      _read_cache_key = UINT64_MAX;
      return;
    }

    void* loaded_data = nullptr;
    int loaded_size = 0;
    int error = 0;

    emscripten_idb_load(
        _db_name.c_str(),
        key_str.c_str(),
        &loaded_data,
        &loaded_size,
        &error
    );

    if (error || !loaded_data) {
      // Key not found - return zeros (new area)
      std::memset(data, 0, size);
      return;
    }

    // Copy loaded data
    size_t copy_size = std::min(size, static_cast<size_t>(loaded_size));
    std::memcpy(data, loaded_data, copy_size);

    // Zero remaining bytes if loaded size was smaller
    if (copy_size < size) {
      std::memset(static_cast<char*>(data) + copy_size, 0, size - copy_size);
    }

    // Buffer this read so the second read for the same key is free
    if (static_cast<size_t>(loaded_size) > size) {
      _read_cache_key = key;
      _read_cache.assign(static_cast<char*>(loaded_data),
                         static_cast<char*>(loaded_data) + loaded_size);
    } else {
      _read_cache_key = UINT64_MAX;
    }

    // Free the allocated buffer from emscripten
    free(loaded_data);
  }

  // Fire-and-forget async IDB write — JS glue copies buffer immediately
  void _idb_async_store_data(uint64_t key, const void* data, size_t size) {
    std::string key_str = (key == 0) ? "header" : ("area_" + std::to_string(key));
    leaves_idb_async_write(
        _db_name.c_str(),
        key_str.c_str(),
        data,
        static_cast<int>(size)
    );
  }

  // Flush any pending data
  void _flush_to_idb() const {
    // Async writes drain via sync_writes() called from _CacheStore
  }
};

/**
 * _BrowserStore: Main storage class for WebAssembly/browser environment
 *
 * Usage:
 *   _BrowserStore store("my_database", 16, 100 * M);
 *   auto* db = store["my_collection"];
 *   db->put(key, value);
 */
struct _BrowserStore : _CacheStore<_BrowserStoreTraits, _BrowserOperations> {
  typedef _CacheStore<_BrowserStoreTraits, _BrowserOperations> base_t;
  using DB = base_t::DB;

  _BrowserStore(const char* db_name, uint16_t db_count = 48,
                size_t capacity = 100 * M, size_t pool_threads = 0)
      : base_t(db_count, capacity, pool_threads, _BrowserStoreTraits::AREA_SIZE) {
    // Note: pool_threads=0 for browser (single-threaded)
    _init_browser_db(db_name, db_count);
  }

  ~_BrowserStore() {
    destroy();
    delete[] reinterpret_cast<char*>(_header);
  }

  void _init_browser_db(const char* db_name, uint16_t db_count) {
    size_t header_size =
        leaves::padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
    char* buffer = new char[header_size];

    open(db_name);

    // Try to load existing header from IndexedDB
    bool exists = _try_load_header(buffer, header_size);

    if (!exists) {
      // Create new database
      _header = new (buffer) FileHeader(db_count);
      // Align file_size to AREA_SIZE so areas are AREA_SIZE-aligned
      _header->file_size = leaves::padding(header_size, _BrowserStoreTraits::AREA_SIZE);
      // Write initial header
      write(0, buffer, header_size);
    } else {
      _header = reinterpret_cast<FileHeader*>(buffer);

      if (strcmp(_header->signature, BROWSERSTORE_SIGNATURE)) {
        throw std::runtime_error("Invalid browser store signature");
      }
      if (_header->db_count != db_count) {
        throw WrongValue("db_count may not be changed.");
      }
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
    _sanitize_dbs();
  }

  void _sanitize_dbs() {
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        assert(!_dbs[i]);
        _DB(*this, _header->dbs[i].offset, i).sanitize();
      }
    }
  }

  // Compatibility method
  AreaSlice get_area(size_t size) {
    auto area_ptr = alloc_multi_area(size);
    return *area_ptr;
  }

  // Browser-specific: Export database to transferable format
  std::vector<char> export_to_buffer() const {
    std::vector<char> result;
    size_t total_size = _header->file_size;
    result.resize(total_size);

    // Export header
    size_t header_size = calc_header_size();
    std::memcpy(result.data(), _header, header_size);

    // Export all areas (would need iteration over stored keys)
    // This is a simplified version - full implementation would
    // enumerate all IndexedDB keys
    return result;
  }

  // Browser-specific: Import database from buffer
  void import_from_buffer(const std::vector<char>& data) {
    if (data.size() < sizeof(FileHeader)) {
      throw std::runtime_error("Invalid import data");
    }

    size_t header_size = calc_header_size();
    std::memcpy(static_cast<void*>(_header), data.data(), header_size);

    // Write header
    write(0, _header, header_size);

    // Import areas (simplified - full version would parse all areas)
  }

  // Browser-specific: Clear all data
  void clear_database() {
    // Delete all known keys from IndexedDB
    int error = 0;
    emscripten_idb_delete(_db_name.c_str(), "header", &error);
    // Reinitialize
    _init_browser_db(_db_name.c_str(), _header->db_count);
  }
};

}  // namespace leaves

#else  // !__EMSCRIPTEN__

// Stub for non-Emscripten builds - provides compile-time error
namespace leaves {

struct _BrowserStore {
  _BrowserStore(...) {
    static_assert(false, "_BrowserStore requires Emscripten compilation");
  }
};

}  // namespace leaves

#endif  // __EMSCRIPTEN__

#endif  // _LEAVES__BROWSERSTORE_HPP
