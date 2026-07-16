/*
Cached file-backed storage helpers for page reuse and persistence access.
*/
#ifndef _LEAVES__CACHESTORE_HPP
#define _LEAVES__CACHESTORE_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>  // for std::memcpy
#include <format>
#include <memory>
#include <string_view>
#include <thread>

#include "../core/_exception.hpp"
#include "../core/_port.hpp"
#include "../core/_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access
#include "../core/_util.hpp"
#include "../db/_db.hpp"
#include "../memory/_memory.hpp"  // for AreaSlice, SmartPointer
#include "../memory/_twoquecache.hpp"
#include "../third_party/unordered_dense.h"
#ifdef __EMSCRIPTEN__
extern "C" {
#include <emscripten.h>
}
#include "../util/_browser_threadpool.hpp"
#else
#include "../util/_threadpool.hpp"
#endif
#include "_db_directory.hpp"

namespace leaves {

#ifdef __EMSCRIPTEN__
template <typename T>
using ThreadPool = _BrowserThreadPoolMixin<T>;
#else
template <typename T>
using ThreadPool = _ThreadPoolMixin<T>;
#endif

struct _CacheBase {
  using DBEntry = _DBDirectoryEntry;
};

template <typename Traits_, typename Opers_, typename Self_ = void>
struct _CacheStore : public Opers_,
                     public ThreadPool<_CacheStore<Traits_, Opers_, Self_>> {
  typedef Traits_ Traits;
  // CRTP: if Self_ is provided, use it as the storage type seen by DB;
  // otherwise default to this class itself (non-derived usage).
  using CacheStore =
      std::conditional_t<std::is_void_v<Self_>,
                         _CacheStore<Traits_, Opers_, Self_>, Self_>;
  using page_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  typedef Opers_ Operations;

  CacheStore& _self() { return static_cast<CacheStore&>(*this); }
  typedef std::vector<char> area_chunk_t;
  using Opers_::_header;
  using Opers_::calc_header_size;
  using Opers_::close;
  using Opers_::read;
  using Opers_::resize;
  using Opers_::write_batch;
  using DBEntry = typename _CacheBase::DBEntry;

  // Use TwoQCache instead of LRUCache
  using Cache = TwoQCache<uint64_t, page_ptr>;

  mutable Cache _cache;
  mutable SpinLock _cache_mutex;
  size_t _capacity;  // Cache capacity in bytes

  void debug_check_cache() const {
    std::lock_guard<SpinLock> lock(_cache_mutex);
    std::cout << "\n==== DEBUG CACHE CHECK ====\n";
    std::cout << "Cache entries: " << _cache.size() << " entries\n";

    // Cannot check cache contents with the new implementation
    // as we don't have direct access to internal structures
    std::cout << "Cannot check cache contents with TwoQCache implementation\n";
    std::cout << "============================\n\n";
  }

  // Internal helper to drop all cache and dirty-tracking state.
  // Callers must ensure pending writes are already drained.
  void reset_cache_state() {
    {
      std::lock_guard<SpinLock> lock(_cache_mutex);
      _cache = Cache(_capacity);
    }

    {
      std::lock_guard<SpinLock> lock(_dirty_mutex);
      _dirty_pending.clear();
      _dirty_committed.clear();
      _dirty_inflight.clear();
    }
  }

  void debug_reset() {
    std::cout << "Resetting cache (clearing all entries)\n";
    reset_cache_state();
  }

  // Dirty area tracking — guarded by _dirty_mutex (shared with background
  // flush)
  mutable SpinLock _dirty_mutex;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_pending;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_committed;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_inflight;
  std::atomic<bool> _header_dirty{false};
  std::atomic<int64_t> _last_cursor_id{0};
  std::atomic<bool> _flush_pending{false};

  ankerl::unordered_dense::map<std::string, _DBSlot> _dbs;

  _CacheStore(size_t capacity = 500 * M, size_t pool_threads = 1,
              size_t avg_item_size = 512 * K)
      : ThreadPool<_CacheStore>(pool_threads),
        _cache(capacity, 0.25f, 0.5f, avg_item_size),
        _capacity(capacity) {}

  // must be called in the subclasses' destructor
  void destroy() {
    // Wait for any pending flush tasks to complete
    this->wait_idle();

    // Final flush of any remaining dirty blocks
    write_dirty_blocks();
    this->wait_for_writes();
    close();
  }

  uint64_t new_cursor_id() {
    return _last_cursor_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void flush(bool sync = false, bool /*force*/ = false) {
    bool has_pending = false;
    {
      std::lock_guard<SpinLock> lock(_dirty_mutex);
      if (!_dirty_pending.empty()) {
        has_pending = true;
        _dirty_committed.insert(_dirty_pending.begin(), _dirty_pending.end());
        _dirty_pending.clear();
      }
    }

    if (sync) {
      write_dirty_blocks();
      this->wait_for_writes();
    } else if (has_pending || _header_dirty.load(std::memory_order_acquire)) {
      if (this->has_workers()) {
        // Native: thread pool coalescing via _flush_pending
        if (!_flush_pending.exchange(true)) {
          this->submit_task([this]() {
            write_dirty_blocks();
            _flush_pending.store(false);
          });
        }
      } else {
        // WASM
        write_dirty_blocks();
      }
    }
  }

  page_ptr resolve(const offset_t* offset_ptr, Access /*access*/ = READ) const {
    offset_t offset = *offset_ptr;
    if (!offset) return page_ptr();  // null offset → null pointer
    uint64_t raw_offset = (uint64_t)offset;
    uint64_t area_offset = raw_offset - (raw_offset % AREA_SIZE);

    // Check cache first - use pointer-returning get to avoid copy
    // For multi-area allocations the offset may fall past the first
    // AREA_SIZE chunk.  Walk backwards by AREA_SIZE until we hit the
    // cached entry for the area's true start.
    {
      std::lock_guard<SpinLock> cache_lock(_cache_mutex);
      auto* cached = _cache.get(area_offset);
      uint64_t probe = area_offset;
      while (!cached && probe >= AREA_SIZE) {
        probe -= AREA_SIZE;
        cached = _cache.get(probe);
        // Make sure the found entry actually spans our offset
        if (cached &&
            cached->area()->offset() + cached->area()->size() <= area_offset) {
          cached = nullptr;  // belongs to a different, earlier area
          break;
        }
      }
      if (cached) {
        uint64_t true_start = cached->area()->offset();
        page_ptr result = *cached;
        result._offset = static_cast<uint32_t>(raw_offset - true_start);
        return result;
      }
    }

    // Cache miss — check dirty maps before reading from storage.
    // A dirty page may have been evicted from the cache but is still
    // in-memory awaiting flush or async IDB write.
    {
      std::lock_guard<SpinLock> lock(_dirty_mutex);
      for (auto* map : {&_dirty_pending, &_dirty_committed, &_dirty_inflight}) {
        auto it = map->find(area_offset);
        if (it != map->end()) {
          page_ptr result = it->second;
          result._offset = static_cast<uint32_t>(raw_offset - area_offset);
          std::lock_guard<SpinLock> cache_lock(_cache_mutex);
          _cache.put(area_offset, result);
          return result;
        }
      }
    }

    // Read on-disk header (could be partial / uninitialized)
    AreaSlice disk_header;
    read(area_offset, &disk_header, sizeof(disk_header));

    // Allocate full region (header + payload)
    AreaSlice* slice = (AreaSlice*)::operator new(disk_header.size());
    read(area_offset, slice, disk_header.size());
    slice->_ref.store(0);

    page_ptr result(slice);
    result._offset = static_cast<uint32_t>(raw_offset - area_offset);

    {
      std::lock_guard<SpinLock> cache_lock(_cache_mutex);
      _cache.put(area_offset, result);
    }
    return result;
  }

  // Resolve function for SmartPointer ptr type
  offset_t resolve(const typename Traits::ptr& p) const {
    return offset_t(p._iref->offset() + p._offset).type(p.type);
  }

  // Resolve function for SmartPointer Pointer<T> type
  template <typename T, NodeTypes type>
  offset_t resolve(const typename Traits::template Pointer<T, type>& p) const {
    return offset_t(p._iref->offset() + p._offset).type(p.type);
  }

  FORCE_INLINE void prefetch(const offset_t* /*offset_ptr*/,
                             Access /*access*/ = READ) const {
    // No-op for file storage: data gets cached in TwoQCache on first access
  }

  FORCE_INLINE void prefetch(void* mem, Access access = READ) const {
    leaves::prefetch(mem, access);
  }

  bool copy(void* dest, const void* src, size_t n) {
    optimized_memcpy(dest, src, n);
    return false;
  }

  void sync_fd_for_commit() {}

  template <typename PtrType>
  void make_dirty(PtrType block_arg) {
    page_ptr block = block_arg;
    uint64_t offset = block.area()->offset();
    std::lock_guard<SpinLock> lock(_dirty_mutex);
    _dirty_pending[offset] = block;
  }

  // Mark the file header as dirty; background flush will write it
  void make_header_dirty() {
    _header_dirty.store(true, std::memory_order_release);
    // Header will be flushed in next write_dirty_blocks call
  }

  area_ptr emplace_new_area(uint64_t size) {
    uint64_t start = _header->file_size;
    _header->file_size = start + size;
    resize(_header->file_size);
    make_header_dirty();

    // Allocate a contiguous buffer for [AreaSlice/Area header + payload]
    Area* area = reinterpret_cast<Area*>(::operator new(size));
    area->init(start, size, 0);
    area->_ref.store(0);

    // Insert into cache as a block starting at area base
    page_ptr blk(area);
    blk._offset = 0;
    {
      std::lock_guard<SpinLock> cache_lock(_cache_mutex);
      _cache.put(start, blk);
    }
    return area_ptr(area);
  }

  area_ptr alloc_single_area() {
    auto result = _header->area_pool.alloc_single_area(*this);
    if (result) {
      make_header_dirty();
      return result;
    }
    return emplace_new_area(AREA_SIZE);
  }

  area_ptr alloc_multi_area(uint64_t size) {
    // Ensure size is multiple of AREA_SIZE
    const uint64_t aligned = padding(size, AREA_SIZE);
    auto result = _header->area_pool.alloc_multi_area(aligned, *this);
    if (result) {
      make_header_dirty();
      return result;
    }
    return emplace_new_area(aligned);
  }

  void return_single_areas(offset_t head, offset_t tail) {
    _header->area_pool.return_single_areas(head, tail, *this);
    make_header_dirty();
  }

  void return_multi_areas(offset_t head, offset_t tail) {
    _header->area_pool.return_multi_areas(head, tail, *this);
    make_header_dirty();
  }

  // Process all dirty blocks from the queue and write them to storage.
  // On WASM, blocks move through _dirty_committed → _dirty_inflight
  // so resolve() can still find them while async IDB writes are pending.
  void write_dirty_blocks() {
    std::vector<page_ptr> blocks_to_write;
    bool has_inflight_writes = false;
    {
      std::lock_guard<SpinLock> lock(_dirty_mutex);
      // If no async writes are pending, previous inflight batch has
      // landed in IDB — safe to release those page_ptr refs.
      has_inflight_writes = this->has_pending_writes();
      if (!has_inflight_writes) {
        _dirty_inflight.clear();
      }

      ankerl::unordered_dense::map<uint64_t, page_ptr> deferred_committed;
      deferred_committed.reserve(_dirty_committed.size());

      blocks_to_write.reserve(_dirty_committed.size());
      for (auto& entry : _dirty_committed) {
        // Avoid re-enqueueing the same offset while a prior async write for it
        // is still in-flight; keep only the latest dirty pointer to flush after
        // the queue drains.
        if (has_inflight_writes &&
            _dirty_inflight.find(entry.first) != _dirty_inflight.end()) {
          deferred_committed[entry.first] = entry.second;
          continue;
        }
        blocks_to_write.emplace_back(entry.second);
        _dirty_inflight[entry.first] = entry.second;
      }
      _dirty_committed.swap(deferred_committed);
    }

    bool header_dirty =
        _header_dirty.exchange(false, std::memory_order_acq_rel);

    // If only header is dirty while async writes are still in flight, defer
    // writing the header once to avoid spinning on duplicate async header
    // writes.
    if (has_inflight_writes && blocks_to_write.empty() && header_dirty) {
      _header_dirty.store(true, std::memory_order_release);
      header_dirty = false;
    }

    write_batch(blocks_to_write, header_dirty);
  }

  void list_dbs(std::vector<std::string>& result) {
    _for_each_db_entry([&](DBEntry& entry) {
      if (entry.offset) result.push_back(entry.name);
      return true;
    });
  }

  // Open or create a DB of the given type. Additional args are forwarded
  // to the DB constructor (e.g. hash_threads for _ReplicationDB).
  template <template <typename> class DBClass = _DB, typename... Args>
  DBClass<CacheStore>* open(std::string_view name, Args&&... args) {
    using DB = DBClass<CacheStore>;
    if (name.size() >= sizeof(_CacheBase::DBEntry::name)) {
      throw std::runtime_error("Database name too long");
    }
    const std::string db_name(name);

    std::scoped_lock flock_guard(this->_self().file_lock());

    _for_each_db_entry([&](DBEntry& entry) { return true; });

    // 1. Check in-memory cache
    auto it = _dbs.find(db_name);
    if (it != _dbs.end()) {
      if (it->second.type_id != DB::DB_TYPE_ID)
        throw TypeMismatch(
            std::format("Wrong database type while opening from cache {} "
                        "expected {} got {}",
                        db_name, DB::DB_TYPE_ID, it->second.type_id));
      return static_cast<DB*>(it->second.db);
    }

    // 2. Scan first page (pointers are stable — _header is heap-allocated)
    uint16_t cap = _first_page_capacity();
    uint16_t hwm = std::min(_header->db_entry_count, cap);
    DBEntry* free_slot = nullptr;

    for (uint16_t i = 0; i < hwm; i++) {
      auto& entry = _header->dbs[i];
      if (entry.offset) {
        if (std::string_view(entry.name) == name)
          return _open_existing<DBClass>(name, entry.offset,
                                         std::forward<Args>(args)...);
      } else if (!free_slot) {
        free_slot = &entry;
      }
    }

    // 3. Scan overflow pages (read-only — don't hold pointers)
    offset_t existing = _find_in_overflow_pages(name);
    if (existing)
      return _open_existing<DBClass>(name, existing,
                                     std::forward<Args>(args)...);

    // 4. Create new DB — prefer a free slot in first page
    if (free_slot) {
      std::strncpy(free_slot->name, db_name.c_str(),
                   sizeof(free_slot->name) - 1);
      free_slot->name[sizeof(free_slot->name) - 1] = '\0';
      auto* db = new DB(_self(), &free_slot->offset, name,
                        std::forward<Args>(args)...);
      db->_header->sanitize_generation = _header->sanitize_generation;
      make_header_dirty();
      _dbs[db_name] = _DBSlot::make(db);
      return db;
    }

    // 5. Expand first-page high-water mark
    if (hwm < cap) {
      auto& slot = _header->dbs[hwm];
      _header->db_entry_count = hwm + 1;
      std::strncpy(slot.name, db_name.c_str(), sizeof(slot.name) - 1);
      slot.name[sizeof(slot.name) - 1] = '\0';
      auto* db =
          new DB(_self(), &slot.offset, name, std::forward<Args>(args)...);
      db->_header->sanitize_generation = _header->sanitize_generation;
      make_header_dirty();
      _dbs[db_name] = _DBSlot::make(db);
      return db;
    }

    // 6. Create in overflow page via read-modify-write
    return _create_in_overflow<DBClass>(name, std::forward<Args>(args)...);
  }

  template <template <typename> class DBClass = _DB>
  void remove(std::string_view name) {
    using DB = DBClass<CacheStore>;
    const std::string db_name(name);
    std::scoped_lock flock_guard(this->_self().file_lock());
    auto it = _dbs.find(db_name);
    if (it != _dbs.end()) {
      if (it->second.type_id != DB::DB_TYPE_ID)
        throw TypeMismatch(std::format(
            "Wrong database type while removing {} expected {} got {}", db_name,
            DB::DB_TYPE_ID, it->second.type_id));
      if (it->second.db && static_cast<DB*>(it->second.db)->is_active())
        throw TransactionActive();
    }

    // Check first page
    uint16_t cap = _first_page_capacity();
    uint16_t hwm = std::min(_header->db_entry_count, cap);
    for (uint16_t i = 0; i < hwm; i++) {
      auto& entry = _header->dbs[i];
      if (entry.offset && std::string_view(entry.name) == name) {
        _return_areas_at<DBClass>(entry.offset, name);
        entry.offset = 0;
        _dbs.erase(db_name);
        make_header_dirty();
        flush(true, true);
        return;
      }
    }

    // Check overflow pages
    if (_remove_from_overflow_pages<DBClass>(name)) {
      _dbs.erase(db_name);
      flush(true, true);
      return;
    }

    throw WrongValue("database does not exist.");
  }

  // First-page capacity for DB entries
  uint16_t _first_page_capacity() const {
    return _DBDirectoryPage::capacity_for(4 * K -
                                          sizeof(typename Opers_::FileHeader));
  }

  // Overflow area capacity: one directory page per whole area payload.
  static constexpr size_t _overflow_page_bytes() {
    return AREA_SIZE - sizeof(Area);
  }

  static constexpr uint16_t _overflow_page_capacity() {
    return _DBDirectoryPage::capacity_for(_overflow_page_bytes() -
                                          offsetof(_DBDirectoryPage, entries));
  }

  // Walk all DB entries across all directory pages, calling fn(DBEntry&)
  template <typename Fn>
  void _for_each_db_entry(Fn fn) {
    // First page: entries embedded in FileHeader
    uint16_t cap = _first_page_capacity();
    uint16_t count = std::min(_header->db_entry_count, cap);
    for (uint16_t i = 0; i < count; i++) {
      if (!fn(_header->dbs[i])) return;
    }

    // Overflow pages
    offset_t next = _header->db_next_page;
    LEAVES_INTERNAL_LOG(
        LEAVES_LOG_DEBUG,
        "_for_each_db_entry db_next_page=%llu count=%u cap=%u\n",
        (unsigned long long)(uint64_t)_header->db_next_page,
        (unsigned)_header->db_entry_count, (unsigned)cap);
    while (next) {
      page_ptr page_block = resolve(&next, READ);
      auto* page =
          reinterpret_cast<_DBDirectoryPage*>(static_cast<char*>(page_block));
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      LEAVES_INTERNAL_LOG(
          LEAVES_LOG_DEBUG,
          "_for_each_db_entry page=%llu count=%u pcount=%u next=%llu\n",
          (unsigned long long)(uint64_t)next, (unsigned)page->count,
          (unsigned)pcount, (unsigned long long)(uint64_t)page->next);
      for (uint16_t i = 0; i < pcount; i++) {
        if (!fn(page->entries[i])) return;
      }
      next = page->next;
    }
  }

  // Search overflow pages for an existing DB by name; return its offset or 0.
  offset_t _find_in_overflow_pages(std::string_view name) {
    offset_t next = _header->db_next_page;
    while (next) {
      page_ptr page_block = resolve(&next, READ);
      auto* page =
          reinterpret_cast<_DBDirectoryPage*>(static_cast<char*>(page_block));
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      for (uint16_t i = 0; i < pcount; i++) {
        if (page->entries[i].offset &&
            std::string_view(page->entries[i].name) == name)
          return page->entries[i].offset;
      }
      next = page->next;
    }
    return 0;
  }

  // Create a new DB in an overflow page via read-modify-write.
  // Returns the DB pointer (already cached in _dbs).
  template <template <typename> class DBClass, typename... Args>
  DBClass<CacheStore>* _create_in_overflow(std::string_view name,
                                           Args&&... args) {
    using DB = DBClass<CacheStore>;
    const std::string db_name(name);
    offset_t prev_offset = 0;
    offset_t cur = _header->db_next_page;

    while (cur) {
      page_ptr cur_block = resolve(&cur, WRITE);
      auto* page =
          reinterpret_cast<_DBDirectoryPage*>(static_cast<char*>(cur_block));
      uint16_t pcap = _overflow_page_capacity();

      // Re-use a free slot (zeroed offset) in existing entries
      for (uint16_t i = 0; i < page->count; i++) {
        if (!page->entries[i].offset) {
          offset_t tmp_offset = 0;
          auto* db =
              new DB(_self(), &tmp_offset, name, std::forward<Args>(args)...);
          db->_header->sanitize_generation = _header->sanitize_generation;
          std::strncpy(page->entries[i].name, db_name.c_str(),
                       sizeof(page->entries[i].name) - 1);
          page->entries[i].name[sizeof(page->entries[i].name) - 1] = '\0';
          page->entries[i].offset = tmp_offset;
          make_dirty(cur_block);
          make_header_dirty();
          flush();
          _dbs[db_name] = _DBSlot::make(db);
          return db;
        }
      }

      // Expand high-water mark if room
      if (page->count < pcap) {
        offset_t tmp_offset = 0;
        auto* db =
            new DB(_self(), &tmp_offset, name, std::forward<Args>(args)...);
        db->_header->sanitize_generation = _header->sanitize_generation;
        auto& slot = page->entries[page->count];
        std::strncpy(slot.name, db_name.c_str(), sizeof(slot.name) - 1);
        slot.name[sizeof(slot.name) - 1] = '\0';
        slot.offset = tmp_offset;
        page->count++;
        make_dirty(cur_block);
        make_header_dirty();
        flush();
        _dbs[db_name] = _DBSlot::make(db);
        return db;
      }

      prev_offset = cur;
      cur = page->next;
    }

    // Allocate a new overflow directory page in area-backed storage so it can
    // be updated through resolve()+make_dirty()+flush.
    auto new_page_area = alloc_single_area();
    if (!new_page_area) throw LeavesException();
    offset_t new_off = new_page_area->content_offset();

    page_ptr new_page_block = resolve(&new_off, WRITE);
    auto* page =
        reinterpret_cast<_DBDirectoryPage*>(static_cast<char*>(new_page_block));
    std::memset((void*)page, 0, _overflow_page_bytes());

    offset_t tmp_offset = 0;
    auto* db = new DB(_self(), &tmp_offset, name, std::forward<Args>(args)...);
    db->_header->sanitize_generation = _header->sanitize_generation;
    std::strncpy(page->entries[0].name, db_name.c_str(),
                 sizeof(page->entries[0].name) - 1);
    page->entries[0].name[sizeof(page->entries[0].name) - 1] = '\0';
    page->entries[0].offset = tmp_offset;
    page->count = 1;
    page->next = 0;

    make_dirty(new_page_block);

    // Link from predecessor
    if (prev_offset) {
      page_ptr prev_page_block = resolve(&prev_offset, WRITE);
      auto* prev = reinterpret_cast<_DBDirectoryPage*>(
          static_cast<char*>(prev_page_block));
      prev->next = new_off;
      make_dirty(prev_page_block);
    } else {
      _header->db_next_page = new_off;
    }
    make_header_dirty();
    flush();
    _dbs[db_name] = _DBSlot::make(db);
    return db;
  }

  // Open an existing DB from a known offset — verifies type, sanitizes if
  // needed.
  template <template <typename> class DBClass, typename... Args>
  DBClass<CacheStore>* _open_existing(std::string_view name, offset_t offset,
                                      Args&&... args) {
    using DB = DBClass<CacheStore>;
    const std::string db_name(name);
    // Verify db_type_id before constructing (prevents reading garbage
    // if the actual header is a different DB subtype).
    _DBHeader<CacheStore> base_header;
    read((uint64_t)offset, &base_header, sizeof(base_header));
    LEAVES_INTERNAL_LOG(
        LEAVES_LOG_DEBUG,
        "_open_existing '%s' offset=%llu read db_type_id=%u expecting=%u "
        "read_txn=%llu\n",
        db_name, (unsigned long long)offset, (unsigned)base_header.db_type_id,
        (unsigned)DB::DB_TYPE_ID, (unsigned long long)base_header.read_txn);
    if (base_header.db_type_id != DB::DB_TYPE_ID) {
      throw TypeMismatch(
          std::format("Wrong database type while opening {} expected {} got {}",
                      db_name, DB::DB_TYPE_ID, base_header.db_type_id));
    }

    auto* db = new DB(_self(), offset, name, std::forward<Args>(args)...);

    LEAVES_INTERNAL_LOG(
        LEAVES_LOG_DEBUG,
        "sanitize db '%s' sanitize_generation=%llu current_generation=%llu\n",
        db_name, (unsigned long long)db->_header->sanitize_generation,
        (unsigned long long)_header->sanitize_generation);

    // Sanitize if this DB hasn't been sanitized for the current generation
    if (db->_header->sanitize_generation != _header->sanitize_generation) {
      db->sanitize();
      db->_header->sanitize_generation = _header->sanitize_generation;
    }
    _dbs[db_name] = _DBSlot::make(db);
    return db;
  }

  // Return all areas owned by a DB at the given offset.
  template <template <typename> class DBClass = _DB>
  void _return_areas_at(offset_t offset, std::string_view name) {
    using DB = DBClass<CacheStore>;
    const std::string db_name(name);
    // If DB is cached, use it directly
    auto it = _dbs.find(db_name);
    if (it != _dbs.end() && it->second.db) {
      if (it->second.type_id != DB::DB_TYPE_ID)
        throw TypeMismatch(std::format(
            "Wrong database type in return_areas {} expected {} got {}",
            db_name, DB::DB_TYPE_ID, it->second.type_id));
      static_cast<DB*>(it->second.db)->return_areas();
      return;
    }
    // Otherwise construct the correctly-typed DB to return areas
    _DBHeader<CacheStore> base_header;
    read((uint64_t)offset, &base_header, sizeof(base_header));
    if (base_header.db_type_id != DB::DB_TYPE_ID)
      throw TypeMismatch(std::format(
          "Wrong database type in return_areas {} expected {} got {}", db_name,
          DB::DB_TYPE_ID, base_header.db_type_id));
    DB tmp(_self(), offset, name);
    tmp.return_areas();
  }

  // Remove a DB from overflow pages via read-modify-write. Returns true if
  // found.
  template <template <typename> class DBClass = _DB>
  bool _remove_from_overflow_pages(std::string_view name) {
    offset_t next = _header->db_next_page;
    while (next) {
      page_ptr next_block = resolve(&next, WRITE);
      auto* page =
          reinterpret_cast<_DBDirectoryPage*>(static_cast<char*>(next_block));
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      for (uint16_t i = 0; i < pcount; i++) {
        if (page->entries[i].offset &&
            std::string_view(page->entries[i].name) == name) {
          _return_areas_at<DBClass>(page->entries[i].offset, name);
          page->entries[i].offset = 0;
          make_dirty(next_block);
          make_header_dirty();
          return true;
        }
      }
      next = page->next;
    }
    return false;
  }
};

}  // namespace leaves

#endif  // _LEAVES__CACHESTORE_HPP