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
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/_exception.hpp"
#include "../core/_node.hpp"
#include "../core/_traits.hpp"
#include "../db/_db.hpp"
#include "../memory/_memory.hpp"
#include "_cachestore.hpp"

// EM_JS functions must be at file scope (outside namespace).
// Async IDB writes with a JS-side queue — avoids calling back into WASM
// from IDB callbacks, which is unsafe during Asyncify / JSPI sleep.
//
// Writes are serialized: at most one IDB transaction is in-flight at a time.
// _leavesWriteQueue accumulates pending write requests; the completion
// callback drains the next item from the queue, guaranteeing FIFO order.
// This prevents the header (which references areas via the DB directory)
// from landing in IndexedDB before the areas themselves.
EM_JS(void, leaves_idb_async_write,
      (const char* db, const char* key, const void* data, int size), {
        if (!Module._leavesWriteQueue) Module._leavesWriteQueue = [];
        if (!Module._leavesWrites) Module._leavesWrites = 0;

        Module._leavesWriteQueue.push({
          db: UTF8ToString(db),
          key: UTF8ToString(key),
          data: new Uint8Array(HEAPU8.subarray(data, data + size))
        });

        // If no write is currently in-flight, start draining the queue.
        if (Module._leavesWrites === 0) {
          (function drain() {
            if (Module._leavesWriteQueue.length === 0) return;
            Module._leavesWrites = 1;
            var item = Module._leavesWriteQueue.shift();
            IDBStore.setFile(item.db, item.key, item.data, function(error) {
              Module._leavesWrites = 0;
              if (error) err('[leaves] IDB async write error');
              drain();  // process next queued write
            });
          })();
        }
      });

// Debug variant — logs each completed write (caller must guard with #ifndef NDEBUG)
EM_JS(void, leaves_idb_async_write_dbg,
      (const char* db, const char* key, const void* data, int size), {
        if (!Module._leavesWriteQueue) Module._leavesWriteQueue = [];
        if (!Module._leavesWrites) Module._leavesWrites = 0;

        Module._leavesWriteQueue.push({
          db: UTF8ToString(db),
          key: UTF8ToString(key),
          data: new Uint8Array(HEAPU8.subarray(data, data + size))
        });

        // If no write is currently in-flight, start draining the queue.
        if (Module._leavesWrites === 0) {
          (function drain() {
            if (Module._leavesWriteQueue.length === 0) return;
            Module._leavesWrites = 1;
            var item = Module._leavesWriteQueue.shift();
            IDBStore.setFile(item.db, item.key, item.data, function(error) {
              Module._leavesWrites = 0;
              if (error) {
                console.error('[leaves] IDB async write error for db=' + item.db + ' key=' + item.key);
              } else {
                console.log('[browser] async write completed: db=' + item.db + ' key=' + item.key + ' size=' + item.data.byteLength + ' bytes');
              }
              drain();  // process next queued write
            });
          })();
        }
      });

EM_JS(void, leaves_idb_delete_database, (const char* db), {
  var dbName = UTF8ToString(db);
  // Remove the cached database handle so a subsequent create will re-open fresh
  if (IDBStore.dbs) {
    delete IDBStore.dbs[dbName];
  }
  var req = indexedDB.deleteDatabase(dbName);
  req.onsuccess = function() {
    console.log('[leaves] IndexedDB database deleted: ' + dbName);
  };
  req.onerror = function() {
    err('[leaves] Failed to delete IndexedDB database: ' + dbName);
  };
});

EM_JS(int, leaves_pending_writes, (), {
  var q = Module._leavesWriteQueue;
  return (Module._leavesWrites || 0) + (q ? q.length : 0);
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
  static constexpr size_t AREA_SIZE = 64 * K;  // Smaller blocks for IDB
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
    std::cout << "BrowserStorage initialized: " << name << std::endl;
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
    // Push blocks into the JS-side write queue.
    // Enqueue order is FIFO — areas are pushed before the header, so the
    // header (which references these areas via the DB directory) can never
    // land in IndexedDB before the areas themselves.
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
      std::fprintf(stdout, "[flush] %zu areas, %zu KB async to IDB\n",
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

  const char* filename() const { return _store_name.c_str(); }

  Mutex& file_lock() { return _header->file_lock; }

  // Load existing area data from IDB and patch in new data at the sub-offset.
  //
  // For a full aligned write (sub_offset == 0 && size >= AREA_ALIGNMENT),
  // returns nullptr — the caller should use the original data directly.
  // Otherwise, loads the existing data for the aligned key, copies the new
  // data in place at the correct sub-offset, and returns the loaded buffer
  // (caller must free() it).  out_size is set to the buffer size.
  //
  // key_buf must be at least 32 bytes.
  void* _idb_load_and_merge(uint64_t key, const void* data, size_t size,
                            char* key_buf, int& out_size) const {
    uint64_t store_key = key - (key % AREA_ALIGNMENT);
    uint64_t sub_offset = key - store_key;
    idb_key_format(key_buf, 32, store_key);

    // Fast path: writing a full aligned area (the common case for area data).
    if (sub_offset == 0 && size >= AREA_ALIGNMENT) {
      out_size = static_cast<int>(size);
      return nullptr;
    }

    // Slow path: read-modify-write for sub-range writes.
    void* existing = nullptr;
    int existing_size = 0;
    int load_error = 0;

#ifndef NDEBUG
    std::fprintf(stderr, "[dbg] _idb_load_and_merge: key=%llu key_str='%s' sub_offset=%llu size=%zu\n",
                 (unsigned long long)key, key_buf, (unsigned long long)sub_offset, size);
#endif

    emscripten_idb_load(_store_name.c_str(), key_buf, &existing,
                        &existing_size, &load_error);

    if (existing && existing_size > 0) {
      // Sub-range data never exceeds the loaded area, so patch in-place.
      std::memcpy(static_cast<char*>(existing) + sub_offset, data, size);
      out_size = existing_size;
      return existing;
    }

    // No existing data — allocate and return only the written sub-range.
    out_size = static_cast<int>(sub_offset + size);
    existing = std::malloc(out_size);
    if (existing) {
      std::memset(existing, 0, out_size);
      std::memcpy(static_cast<char*>(existing) + sub_offset, data, size);
    }
    return existing;
  }

  // IndexedDB store operation using Emscripten Asyncify
  //
  // Data is stored under key "area_<aligned_offset>" where aligned_offset
  // is the offset rounded down to AREA_ALIGNMENT.  This mirrors the
  // alignment in _idb_load_data so that data written at any offset within
  // an area ends up under the correct key.
  void _idb_store_data(uint64_t key, const void* data, size_t size) const {
    char key_buf[32];
    int write_size;
    void* buf = _idb_load_and_merge(key, data, size, key_buf, write_size);
    const void* write_data = buf ? buf : data;
#ifndef NDEBUG
    std::fprintf(stderr, "[dbg] _idb_store_data: key=%llu key_str='%s' size=%zu write_size=%d\n",
                 (unsigned long long)key, key_buf, size, write_size);
#endif 

    int error = 0;
    emscripten_idb_store(_store_name.c_str(), key_buf,
                         const_cast<void*>(write_data), write_size, &error);
    if (error) {
      throw LeavesException(std::string("IndexedDB store failed for key: ") + key_buf);
    }
    if (buf) free(buf);
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

#ifndef NDEBUG
    std::fprintf(stderr, "[dbg] _idb_load_data: key=%llu key_str='%s' sub_offset=%llu size=%zu\n",
                 (unsigned long long)key, key_buf, (unsigned long long)sub_offset, size);
#endif

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

#ifndef NDEBUG
    std::fprintf(stderr, "[dbg] _idb_load_data: key=%llu key_str='%s' sub_offset=%llu size=%zu\n",
                 (unsigned long long)key, key_buf, (unsigned long long)sub_offset, size);
#endif 
    emscripten_idb_load(_store_name.c_str(), key_buf, &loaded_data,
                        &loaded_size, &error);

    if (error || !loaded_data) {
#ifndef NDEBUG
      std::fprintf(stderr, "[dbg] _idb_load_data: key=%llu key_str='%s' NOT FOUND — returning zeros (size=%zu)\n",
                   (unsigned long long)key, key_buf, size);
#endif
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

  // Fire-and-forget async IDB write — JS glue copies buffer immediately
  //
  // When writing only a sub-range within an aligned area (e.g. a 4K header
  // flush into area_0), we perform a synchronous read-modify-write before
  // queueing the full merged buffer for async storage.  This prevents
  // sub-range writes from silently truncating the stored area to only the
  // written bytes, which would lose previously stored data at other offsets
  // within the same aligned block (e.g. overflow directory pages co-located
  // with the header in the first AREA_SIZE block).
  void _idb_async_store_data(uint64_t key, const void* data, size_t size) {
    char key_buf[32];
    int write_size;

#ifndef NDEBUG
    std::fprintf(stderr, "[dbg] _idb_async_store_data: key=%llu size=%zu\n",
                  (unsigned long long)key, size);
#endif

    void* buf = _idb_load_and_merge(key, data, size, key_buf, write_size);
    const void* write_data = buf ? buf : data;

#ifndef NDEBUG
    leaves_idb_async_write_dbg(_store_name.c_str(), key_buf, write_data,
                               write_size);
#else
    leaves_idb_async_write(_store_name.c_str(), key_buf, write_data,
                           write_size);
#endif
    if (buf) free(buf);
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
    std::cout << (exists ? "Existing storage loaded: "
                         : "No existing storage, creating new: ")
              << store_name << std::endl;
    if (!exists) {
      // Create new database
      this->_header = new (buffer) typename base_t::Operations::FileHeader();
      // Align file_size to AREA_SIZE so areas are AREA_SIZE-aligned
      this->_header->file_size =
          leaves::padding(header_size, traits_t::AREA_SIZE);
      // Write initial header
      this->write(0, buffer, header_size);
    } else {
      this->_header =
          reinterpret_cast<typename base_t::Operations::FileHeader*>(buffer);

      if (strcmp(this->_header->signature, BROWSERSTORE_SIGNATURE)) {
        throw TypeMismatch(
            std::format("Invalid database signature: expected '{}' got '{}'",
                        BROWSERSTORE_SIGNATURE, this->_header->signature));
      }

      // ── Diagnostic: dump all DB directory entries with their on-disk type_id ──
      std::cout << "[diag] DB directory contents:\n";
      this->_for_each_db_entry([&](typename base_t::DBEntry& entry) {
        if (entry.offset) {
          _DBHeader<base_t> hdr;
          this->read((uint64_t)entry.offset, &hdr, sizeof(hdr));
          std::cout << "[diag]   name='" << entry.name
                    << "' offset=" << entry.offset
                    << " db_type_id=" << hdr.db_type_id
                    << " (0=_DB, 1=_ReplicationDB, 2=reserved, 3=_TributaryDB)\n";
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
          self->write(pos, buf, size);
        });
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

    // Write header
    this->write(0, this->_header, header_size);

    // Import areas (simplified - full version would parse all areas)
  }

  // Browser-specific: Delete the entire IndexedDB database
  // Flushes pending writes, then removes the database entirely.
  void delete_storage() {
    // Flush and close all pending operations
    this->destroy();
    // Delete the IndexedDB database from JS
    leaves_idb_delete_database(this->_store_name.c_str());
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