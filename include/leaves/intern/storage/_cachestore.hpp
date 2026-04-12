#ifndef _LEAVES__CACHESTORE_HPP
#define _LEAVES__CACHESTORE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>  // for std::memcpy
#include <memory>
#include <thread>
#include "../third_party/unordered_dense.h"

#include "../core/_exception.hpp"
#include "../core/_port.hpp"
#include "../core/_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access
#include "../db/_db.hpp"
#include "../memory/_memory.hpp"  // for AreaSlice, SmartPointer
#include "../memory/_twoquecache.hpp"
#include "../util/_threadpool.hpp"
#include "_db_directory.hpp"

namespace leaves {

struct _CacheBase {
  using DBEntry = _DBDirectoryEntry;
};

template <typename Traits_, typename Opers_,
          typename Self_ = void>
struct _CacheStore : public Opers_,
                     public _ThreadPoolMixin<_CacheStore<Traits_, Opers_, Self_>> {
  typedef Traits_ Traits;
  // CRTP: if Self_ is provided, use it as the storage type seen by DB;
  // otherwise default to this class itself (non-derived usage).
  using CacheStore = std::conditional_t<
      std::is_void_v<Self_>, _CacheStore<Traits_, Opers_, Self_>, Self_>;
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
  using Opers_::write;
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

  void debug_reset() {
    std::cout << "Resetting cache (clearing all entries)\n";
    {
      std::lock_guard<SpinLock> lock(_cache_mutex);
      _cache = Cache(_capacity);
    }

    // Also reset the dirty areas map to avoid dangling references
    {
      std::lock_guard<SpinLock> lock(_dirty_mutex);
      _dirty_pending.clear();
      _dirty_committed.clear();
      _dirty_inflight.clear();
    }
  }

  // Dirty area tracking — guarded by _dirty_mutex (shared with background flush)
  mutable SpinLock _dirty_mutex;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_pending;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_committed;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_inflight;
  std::atomic<bool> _header_dirty{false};

  std::atomic<bool> _flush_pending{false};
  ankerl::unordered_dense::map<std::string, _DBSlot> _dbs;

  _CacheStore(size_t capacity = 500 * M,
              size_t pool_threads = 1, size_t avg_item_size = 512 * K)
      : _ThreadPoolMixin<_CacheStore>(pool_threads),
        _cache(capacity, 0.25f, 0.5f, avg_item_size),
        _capacity(capacity) {
  }

  // must be called in the subclasses' destructor
  void destroy() {
    // Wait for any pending flush tasks to complete
    this->wait_all();

    // Final flush of any remaining dirty blocks
    write_dirty_blocks();
    this->sync_writes();
    close();
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
      this->sync_writes();
    } else if (has_pending) {
      if (this->has_workers()) {
        // Native: thread pool coalescing via _flush_pending
        if (!_flush_pending.exchange(true)) {
          this->submit_task([this]() {
            write_dirty_blocks();
            _flush_pending.store(false);
          });
        }
      } else if (!this->has_pending_writes()) {
        // WASM: fire async IDB writes if previous batch is done.
        // If IDB is still busy, dirty pages accumulate in
        // _dirty_committed until the next flush sees them.
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
        if (cached && cached->area()->offset() + cached->area()->size() <= area_offset) {
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

  FORCE_INLINE void prefetch(const offset_t* /*offset_ptr*/, Access /*access*/ = READ) const {
    // No-op for file storage: data gets cached in TwoQCache on first access
  }

  FORCE_INLINE void prefetch(void* mem, Access access=READ) const {
    leaves::prefetch(mem, access);
  }

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
    if (result) { make_header_dirty(); return result; }
    return emplace_new_area(AREA_SIZE);
  }

  area_ptr alloc_multi_area(uint64_t size) {
    // Ensure size is multiple of AREA_SIZE
    const uint64_t aligned = padding(size, AREA_SIZE);
    auto result = _header->area_pool.alloc_multi_area(aligned, *this);
    if (result) { make_header_dirty(); return result; }
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
    {
      std::lock_guard<SpinLock> lock(_dirty_mutex);
      // If no async writes are pending, previous inflight batch has
      // landed in IDB — safe to release those page_ptr refs.
      if (!this->has_pending_writes()) {
        _dirty_inflight.clear();
      }
      blocks_to_write.reserve(_dirty_committed.size());
      for (auto& entry : _dirty_committed) {
        blocks_to_write.emplace_back(entry.second);
        _dirty_inflight[entry.first] = entry.second;
      }
      _dirty_committed.clear();
    }

    bool header_dirty = _header_dirty.exchange(false, std::memory_order_acq_rel);
    write_batch(blocks_to_write, header_dirty);
  }

  void list_dbs(std::vector<std::string>& result) {
    _for_each_db_entry([&](DBEntry& entry) {
      if (entry.offset) result.push_back(entry.name);
    });
  }

  // Open or create a DB of the given type. Additional args are forwarded
  // to the DB constructor (e.g. hash_threads for _ReplicationDB).
  template <template <typename> class DBClass = _DB, typename... Args>
  DBClass<CacheStore>* open(const char* name, Args&&... args) {
    using DB = DBClass<CacheStore>;
    if (strlen(name) >= sizeof(_CacheBase::DBEntry::name)) throw KeyTooBig();

    // 1. Check in-memory cache
    auto it = _dbs.find(name);
    if (it != _dbs.end()) {
      if (it->second.type_id != DB::DB_TYPE_ID) throw TypeMismatch();
      return static_cast<DB*>(it->second.db);
    }

    // 2. Scan first page (pointers are stable — _header is heap-allocated)
    uint16_t cap = _first_page_capacity();
    uint16_t hwm = std::min(_header->db_entry_count, cap);
    DBEntry* free_slot = nullptr;

    for (uint16_t i = 0; i < hwm; i++) {
      auto& entry = _header->dbs[i];
      if (entry.offset) {
        if (!strcmp(entry.name, name))
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
      std::strncpy(free_slot->name, name, sizeof(DBEntry::name) - 1);
      free_slot->name[sizeof(DBEntry::name) - 1] = '\0';
      auto* db = new DB(_self(), &free_slot->offset, std::string_view(name),
                        std::forward<Args>(args)...);
      db->_header->sanitize_generation = _header->sanitize_generation;
      make_header_dirty();
      _dbs[name] = _DBSlot::make(db);
      return db;
    }

    // 5. Expand first-page high-water mark
    if (hwm < cap) {
      auto& slot = _header->dbs[hwm];
      _header->db_entry_count = hwm + 1;
      std::strncpy(slot.name, name, sizeof(DBEntry::name) - 1);
      slot.name[sizeof(DBEntry::name) - 1] = '\0';
      auto* db = new DB(_self(), &slot.offset, std::string_view(name),
                        std::forward<Args>(args)...);
      db->_header->sanitize_generation = _header->sanitize_generation;
      make_header_dirty();
      _dbs[name] = _DBSlot::make(db);
      return db;
    }

    // 6. Create in overflow page via read-modify-write
    return _create_in_overflow<DBClass>(name, std::forward<Args>(args)...);
  }

  template <template <typename> class DBClass = _DB>
  void remove(const char* name) {
    using DB = DBClass<CacheStore>;
    auto it = _dbs.find(name);
    if (it != _dbs.end()) {
      if (it->second.type_id != DB::DB_TYPE_ID) throw TypeMismatch();
      if (it->second.db && static_cast<DB*>(it->second.db)->is_active())
        throw TransactionActive();
    }

    // Check first page
    uint16_t cap = _first_page_capacity();
    uint16_t hwm = std::min(_header->db_entry_count, cap);
    for (uint16_t i = 0; i < hwm; i++) {
      auto& entry = _header->dbs[i];
      if (entry.offset && !strcmp(entry.name, name)) {
        _return_areas_at<DBClass>(entry.offset, name);
        entry.offset = 0;
        _dbs.erase(name);
        make_header_dirty();
        flush(true, true);
        return;
      }
    }

    // Check overflow pages
    if (_remove_from_overflow_pages<DBClass>(name)) {
      _dbs.erase(name);
      flush(true, true);
      return;
    }

    throw WrongValue("database does not exist.");
  }

  // First-page capacity for DB entries
  uint16_t _first_page_capacity() const {
    return _DBDirectoryPage::capacity_for(
        4 * K - sizeof(typename Opers_::FileHeader));
  }

  // Overflow page capacity
  static constexpr uint16_t _overflow_page_capacity() {
    return _DBDirectoryPage::capacity_for(
        4 * K - offsetof(_DBDirectoryPage, entries));
  }

  // Walk all DB entries across all directory pages, calling fn(DBEntry&)
  template <typename Fn>
  void _for_each_db_entry(Fn fn) {
    // First page: entries embedded in FileHeader
    uint16_t cap = _first_page_capacity();
    uint16_t count = std::min(_header->db_entry_count, cap);
    for (uint16_t i = 0; i < count; i++) fn(_header->dbs[i]);

    // Overflow pages
    offset_t next = _header->db_next_page;
    while (next) {
      alignas(8) char buf[4 * K];
      this->read((uint64_t)next, buf, 4 * K);
      auto* page = reinterpret_cast<_DBDirectoryPage*>(buf);
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      for (uint16_t i = 0; i < pcount; i++) fn(page->entries[i]);
      next = page->next;
    }
  }

  // Search overflow pages for an existing DB by name; return its offset or 0.
  offset_t _find_in_overflow_pages(const char* name) {
    offset_t next = _header->db_next_page;
    while (next) {
      alignas(8) char buf[4 * K];
      this->read((uint64_t)next, buf, 4 * K);
      auto* page = reinterpret_cast<_DBDirectoryPage*>(buf);
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      for (uint16_t i = 0; i < pcount; i++) {
        if (page->entries[i].offset && !strcmp(page->entries[i].name, name))
          return page->entries[i].offset;
      }
      next = page->next;
    }
    return 0;
  }

  // Create a new DB in an overflow page via read-modify-write.
  // Returns the DB pointer (already cached in _dbs).
  template <template <typename> class DBClass, typename... Args>
  DBClass<CacheStore>* _create_in_overflow(const char* name, Args&&... args) {
    using DB = DBClass<CacheStore>;
    uint64_t prev_offset = 0;
    offset_t cur = _header->db_next_page;

    while (cur) {
      alignas(8) char buf[4 * K];
      this->read((uint64_t)cur, buf, 4 * K);
      auto* page = reinterpret_cast<_DBDirectoryPage*>(buf);
      uint16_t pcap = _overflow_page_capacity();

      // Re-use a free slot (zeroed offset) in existing entries
      for (uint16_t i = 0; i < page->count; i++) {
        if (!page->entries[i].offset) {
          offset_t tmp_offset = 0;
          auto* db = new DB(_self(), &tmp_offset, std::string_view(name),
                            std::forward<Args>(args)...);
          db->_header->sanitize_generation = _header->sanitize_generation;
          std::strncpy(page->entries[i].name, name, sizeof(DBEntry::name) - 1);
          page->entries[i].name[sizeof(DBEntry::name) - 1] = '\0';
          page->entries[i].offset = tmp_offset;
          this->write((uint64_t)cur, buf, 4 * K);
          make_header_dirty();
          _dbs[name] = _DBSlot::make(db);
          return db;
        }
      }

      // Expand high-water mark if room
      if (page->count < pcap) {
        offset_t tmp_offset = 0;
        auto* db = new DB(_self(), &tmp_offset, std::string_view(name),
                          std::forward<Args>(args)...);
        db->_header->sanitize_generation = _header->sanitize_generation;
        auto& slot = page->entries[page->count];
        std::strncpy(slot.name, name, sizeof(DBEntry::name) - 1);
        slot.name[sizeof(DBEntry::name) - 1] = '\0';
        slot.offset = tmp_offset;
        page->count++;
        this->write((uint64_t)cur, buf, 4 * K);
        make_header_dirty();
        _dbs[name] = _DBSlot::make(db);
        return db;
      }

      prev_offset = (uint64_t)cur;
      cur = page->next;
    }

    // Allocate a new overflow page
    uint64_t new_off = prev_offset ? prev_offset + 4 * K : 4 * K;
    if (new_off + 4 * K > AREA_SIZE) throw LeavesException();

    alignas(8) char buf[4 * K];
    std::memset(buf, 0, 4 * K);
    auto* page = reinterpret_cast<_DBDirectoryPage*>(buf);

    offset_t tmp_offset = 0;
    auto* db = new DB(_self(), &tmp_offset, std::string_view(name),
                      std::forward<Args>(args)...);
    db->_header->sanitize_generation = _header->sanitize_generation;
    std::strncpy(page->entries[0].name, name, sizeof(DBEntry::name) - 1);
    page->entries[0].name[sizeof(DBEntry::name) - 1] = '\0';
    page->entries[0].offset = tmp_offset;
    page->count = 1;
    page->next = 0;
    this->write(new_off, buf, 4 * K);

    // Link from predecessor
    if (prev_offset) {
      alignas(8) char prev_buf[4 * K];
      this->read(prev_offset, prev_buf, 4 * K);
      auto* prev = reinterpret_cast<_DBDirectoryPage*>(prev_buf);
      prev->next = new_off;
      this->write(prev_offset, prev_buf, 4 * K);
    } else {
      _header->db_next_page = new_off;
    }
    make_header_dirty();
    _dbs[name] = _DBSlot::make(db);
    return db;
  }

  // Open an existing DB from a known offset — verifies type, sanitizes if needed.
  template <template <typename> class DBClass, typename... Args>
  DBClass<CacheStore>* _open_existing(const char* name, offset_t offset,
                                      Args&&... args) {
    using DB = DBClass<CacheStore>;
    // Verify db_type_id before constructing (prevents reading garbage
    // if the actual header is a different DB subtype).
    _DBHeader<CacheStore> base_header;
    read((uint64_t)offset, &base_header, sizeof(base_header));
    if (base_header.db_type_id != DB::DB_TYPE_ID) throw TypeMismatch();

    auto* db = new DB(_self(), offset, std::string_view(name),
                      std::forward<Args>(args)...);
    // Sanitize if this DB hasn't been sanitized for the current generation
    if (db->_header->sanitize_generation != _header->sanitize_generation) {
      db->sanitize();
      db->_header->sanitize_generation = _header->sanitize_generation;
    }
    _dbs[name] = _DBSlot::make(db);
    return db;
  }

  // Return all areas owned by a DB at the given offset.
  template <template <typename> class DBClass = _DB>
  void _return_areas_at(offset_t offset, const char* name) {
    using DB = DBClass<CacheStore>;
    // If DB is cached, use it directly
    auto it = _dbs.find(name);
    if (it != _dbs.end() && it->second.db) {
      static_cast<DB*>(it->second.db)->return_areas();
      return;
    }
    // Otherwise construct the correctly-typed DB to return areas
    _DBHeader<CacheStore> base_header;
    read((uint64_t)offset, &base_header, sizeof(base_header));
    if (base_header.db_type_id != DB::DB_TYPE_ID) throw TypeMismatch();
    DB tmp(_self(), offset, std::string_view(name));
    tmp.return_areas();
  }

  // Remove a DB from overflow pages via read-modify-write. Returns true if found.
  template <template <typename> class DBClass = _DB>
  bool _remove_from_overflow_pages(const char* name) {
    offset_t next = _header->db_next_page;
    while (next) {
      alignas(8) char buf[4 * K];
      this->read((uint64_t)next, buf, 4 * K);
      auto* page = reinterpret_cast<_DBDirectoryPage*>(buf);
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      for (uint16_t i = 0; i < pcount; i++) {
        if (page->entries[i].offset && !strcmp(page->entries[i].name, name)) {
          _return_areas_at<DBClass>(page->entries[i].offset, name);
          page->entries[i].offset = 0;
          this->write((uint64_t)next, buf, 4 * K);
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