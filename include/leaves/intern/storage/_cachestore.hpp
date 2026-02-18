#ifndef _LEAVES__CACHESTORE_HPP
#define _LEAVES__CACHESTORE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>  // for std::memcpy
#include <memory>
#include <mutex>
#include <thread>
#include "../third_party/unordered_dense.h"

#include "../core/_exception.hpp"
#include "../core/_port.hpp"
#include "../core/_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access
#include "../db/_db.hpp"
#include "../memory/_memory.hpp"  // for AreaSlice, SmartPointer
#include "../memory/_twoquecache.hpp"
#include "../util/_threadpool.hpp"

namespace leaves {

struct _CacheBase {
  struct DBEntry {
    char name[21];
    offset_t offset;
  };
};

template <typename Traits_, typename Opers_,
          template <typename> class DB_ = _DB,
          typename Self_ = void>
struct _CacheStore : public Opers_,
                     public _ThreadPoolMixin<_CacheStore<Traits_, Opers_, DB_, Self_>> {
  typedef Traits_ Traits;
  // CRTP: if Self_ is provided, use it as the storage type seen by DB;
  // otherwise default to this class itself (non-derived usage).
  using CacheStore = std::conditional_t<
      std::is_void_v<Self_>, _CacheStore<Traits_, Opers_, DB_, Self_>, Self_>;
  using page_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  typedef Opers_ Operations;
  typedef DB_<CacheStore> DB;
  typedef std::unique_ptr<DB> _db_ptr;

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
  size_t _capacity;  // Cache capacity in bytes

  void debug_check_cache() const {
    std::cout << "\n==== DEBUG CACHE CHECK ====\n";
    std::cout << "Cache entries: " << _cache.size() << " entries\n";

    // Cannot check cache contents with the new implementation
    // as we don't have direct access to internal structures
    std::cout << "Cannot check cache contents with TwoQCache implementation\n";
    std::cout << "============================\n\n";
  }

  void debug_reset() {
    std::cout << "Resetting cache (clearing all entries)\n";
    // Create a new cache instance
    _cache = Cache(_capacity);

    // Also reset the dirty areas maps to avoid dangling references
    {
      std::lock_guard<std::mutex> lock(_dirty_areas_mutex);
      _pending_dirty_areas.clear();
      _dirty_areas.clear();
    }
  }

  // Handling for dirty areas - using mutex-protected map for thread safety
  ankerl::unordered_dense::map<uint64_t, page_ptr> _pending_dirty_areas;
  ankerl::unordered_dense::map<uint64_t, page_ptr> _dirty_areas;
  std::mutex _dirty_areas_mutex;
  std::atomic<bool> _header_dirty{false};
  std::atomic<int64_t> _last_cursor_id{0};
  std::atomic<bool> _flush_pending{false};
  std::vector<_db_ptr> _dbs;

  _CacheStore(uint16_t db_count = 48, size_t capacity = 500 * M,
              size_t pool_threads = 1)
      : _ThreadPoolMixin<_CacheStore>(pool_threads),
        _cache(capacity),
        _capacity(capacity) {
    _dbs.resize(db_count);
  }

  // must be called in the subclasses' destructor
  void destroy() {
    // Wait for any pending flush tasks to complete
    this->wait_all();

    // Final flush of any remaining dirty blocks
    write_dirty_blocks(calc_header_size());
    close();
  }

  uint64_t new_cursor_id() {
    return _last_cursor_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void flush(bool sync = false, bool /*force*/ = false) {
    {
      std::lock_guard<std::mutex> lock(_dirty_areas_mutex);
      _dirty_areas.insert(_pending_dirty_areas.begin(),
                          _pending_dirty_areas.end());
    }
    bool has_pending = !_pending_dirty_areas.empty();
    _pending_dirty_areas.clear();

    if (sync) {
      write_dirty_blocks(calc_header_size());
    } else if (has_pending && !_flush_pending.exchange(true)) {
      // Submit async flush task to thread pool (only if not already pending)
      this->submit_task([this]() {
        write_dirty_blocks(calc_header_size());
        _flush_pending.store(false);
      });
    }
  }

  page_ptr resolve(const offset_t* offset_ptr, Access /*access*/ = READ) const {
    offset_t offset = *offset_ptr;
    uint64_t raw_offset = (uint64_t)offset;
    uint64_t area_offset = raw_offset - (raw_offset % AREA_SIZE);
    
    // Check cache first - use pointer-returning get to avoid copy
    // For multi-area allocations the offset may fall past the first
    // AREA_SIZE chunk.  Walk backwards by AREA_SIZE until we hit the
    // cached entry for the area's true start.
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

    uint64_t read_offset = area_offset + calc_header_size();

    // Read on-disk header (could be partial / uninitialized)
    AreaSlice disk_header;
    read((uint64_t)read_offset, &disk_header, sizeof(disk_header));

    // Allocate full region (header + payload)
    AreaSlice* slice = (AreaSlice*)::operator new(disk_header.size());
    read((uint64_t)read_offset, slice, disk_header.size());
    slice->_ref.store(0);

    page_ptr result(slice);
    result._offset = static_cast<uint32_t>(raw_offset - area_offset);
    _cache.put(area_offset, result);
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
    _pending_dirty_areas[block.area()->offset()] = block;
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

    start -= calc_header_size();  // adjust for header

    // Allocate a contiguous buffer for [AreaSlice/Area header + payload]
    Area* area = reinterpret_cast<Area*>(::operator new(size));
    area->init(start, size, 0);
    area->_ref.store(0);

    // Insert into cache as a block starting at area base
    page_ptr blk(area);
    blk._offset = 0;
    _cache.put(start, blk);
    return area_ptr(area);
  }

  area_ptr alloc_single_area() {
    auto result = _header->area_pool.alloc_single_area(*this);
    return result ? result : emplace_new_area(AREA_SIZE);
  }

  area_ptr alloc_multi_area(uint64_t size) {
    // Ensure size is multiple of AREA_SIZE
    const uint64_t aligned = padding(size, AREA_SIZE);
    auto result = _header->area_pool.alloc_multi_area(aligned, *this);
    return result ? result : emplace_new_area(aligned);
  }

  void return_single_areas(offset_t head, offset_t tail) {
    _header->area_pool.return_single_areas(head, tail, *this);
    make_header_dirty();
  }

  void return_multi_areas(offset_t head, offset_t tail) {
    _header->area_pool.return_multi_areas(head, tail, *this);
    make_header_dirty();
  }

  // Process all dirty blocks from the queue and write them to storage
  void write_dirty_blocks(size_t header_size) {
    // Process all dirty areas in the set
    // Use a batch approach for blocks with contiguous offsets

    // We'll collect blocks to write in this vector
    std::vector<page_ptr> blocks_to_write;

    // Get all dirty blocks under a single lock to reduce lock contention
    {
      std::lock_guard<std::mutex> queue_lock(_dirty_areas_mutex);
      blocks_to_write.reserve(_dirty_areas.size());

      // Extract all dirty blocks
      for (auto& entry : _dirty_areas) {
        blocks_to_write.emplace_back(entry.second);
      }

      // Clear the dirty areas map
      _dirty_areas.clear();
    }

    write_batch(blocks_to_write, header_size);

    if (_header_dirty.exchange(false, std::memory_order_acq_rel)) {
      // Write the header
      write(0, _header, header_size);
    }
  }

  void list_dbs(std::vector<std::string>& result) {
    for (uint16_t i = 0; i < _header->db_count; i++) {
      result.push_back(_header->dbs[i].name);
    }
  }

  Slice db_name(int index) const { return Slice(_header->dbs[index].name); }

  DB* operator[](const char* name) { return make(name); }

  DB* make(const char* name) {
    if (strlen(name) >= sizeof(_CacheBase::DBEntry::name)) throw KeyTooBig();

    // No locking needed since we're single-process
    int free = -1;
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        if (!strcmp(_header->dbs[i].name, name)) {
          if (!_dbs[i]) {
            _dbs[i] = std::make_unique<DB>(_self(), _header->dbs[i].offset, i);
            make_header_dirty();
            return _dbs[i].get();
          }
          return _dbs[i].get();
        }
      } else if (free < 0)
        free = i;
    }

    if (free < 0) throw LeavesException();
    std::strncpy(_header->dbs[free].name, name,
                 sizeof(_header->dbs[free].name) - 1);
    _header->dbs[free].name[sizeof(_header->dbs[free].name) - 1] = '\0';
    _dbs[free] = std::make_unique<DB>(_self(), &_header->dbs[free].offset, free);
    make_header_dirty();
    return _dbs[free].get();
  }

  void remove_db(const char* name) {
    // No locking needed since we're single-process

    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset && !strcmp(_header->dbs[i].name, name)) {
        if (_dbs[i] && _dbs[i]->is_active()) throw TransactionActive();
        DB tmp(_self(), _header->dbs[i].offset, i);
        tmp.return_areas();
        _header->dbs[i].offset = 0;
        flush(true, true);
        return;
      }
    }
    throw WrongValue("database does not exist.");
  }
};

}  // namespace leaves

#endif  // _LEAVES__CACHESTORE_HPP