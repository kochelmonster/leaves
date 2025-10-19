#ifndef _LEAVES__CACHESTORE_HPP
#define _LEAVES__CACHESTORE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>  // for std::memcpy
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "_db.hpp"
#include "_exception.hpp"
#include "_memory.hpp"  // for AreaSlice, SmartPointer
#include "_port.hpp"
#include "_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access
#include "_twoquecache.hpp"

namespace leaves {

struct _CacheBase {
  struct DBEntry {
    char name[21];
    offset_t offset;
  };
};

template <typename Traits_, typename Opers_>
struct _CacheStore : public Opers_ {
  typedef Traits_ Traits;
  typedef _CacheStore<Traits_, Opers_> Self;
  using block_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  typedef Opers_ Operations;
  typedef _DB<_CacheStore> DB;
  typedef std::shared_ptr<DB> db_ptr;
  typedef std::weak_ptr<DB> wdb_ptr;
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
  using Cache = TwoQCache<uint64_t, block_ptr>;

  std::vector<wdb_ptr> _dbs;  // databases
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
  std::unordered_map<uint64_t, block_ptr> _pending_dirty_areas;
  std::unordered_map<uint64_t, block_ptr> _dirty_areas;
  std::mutex _dirty_areas_mutex;
  std::thread _write_back_thread;
  std::atomic<bool> _should_stop;
  std::mutex _dirty_mutex;
  std::condition_variable _dirty_cv;
  std::atomic<bool> _header_dirty{false};
  std::atomic<int64_t> _last_cursor_id{0};

  _CacheStore(uint16_t db_count = 48, size_t capacity = 500 * M)
      : _cache(capacity), _capacity(capacity), _should_stop(false) {
    _dbs.resize(db_count);
  }

  // must be called in the subclasses' destructor
  void destroy() {
    // Stop the _write_back_thread
    _should_stop.store(true, std::memory_order_release);
    if (_write_back_thread.joinable()) {
      {
        std::lock_guard<std::mutex> lock(_dirty_mutex);
        _dirty_cv.notify_all();  // Wake up the thread to process remaining work
      }
      _write_back_thread.join();
    }

    write_dirty_blocks(calc_header_size());
    close();
  }

  void start_write_back_thread() {
    _write_back_thread = std::thread(&_CacheStore::write_back_loop, this);
  }

  uint64_t new_cursor_id() {
    return _last_cursor_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void flush(bool sync = false, bool force = false) {
    {
      std::lock_guard<std::mutex> lock(_dirty_areas_mutex);
      _dirty_areas.insert(_pending_dirty_areas.begin(),
                          _pending_dirty_areas.end());
    }
    bool has_pending = !_pending_dirty_areas.empty();
    _pending_dirty_areas.clear();

    if (sync) {
      write_dirty_blocks(calc_header_size());
    } else if (has_pending) {
      _dirty_cv.notify_one();
    }
  }

  block_ptr resolve(offset_t offset, Access access = READ) const {
    uint64_t raw_offset = (uint64_t)offset;
    uint64_t area_offset = raw_offset - (raw_offset % AREA_SIZE);
    // Check cache first
    block_ptr cached;
    if (_cache.get(area_offset, cached)) {
      AreaSlice* slice = cached.area();
      assert(slice->offset() == area_offset);
      block_ptr result = cached;  // copy increments refcount
      result._offset = static_cast<uint32_t>(raw_offset - area_offset);
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

    block_ptr result(slice);
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

  void prefetch(offset_t offset, Access access = READ) const {
    // For file storage, prefetch is essentially a no-op
    // Could potentially implement with platform-specific hints
  }

  void prefetch(void* mem, Access access = READ) const {
    // For file storage, prefetch is essentially a no-op
    // Could potentially implement with platform-specific hints
  }

  void make_dirty(block_ptr& block) {
    _pending_dirty_areas[block.area()->offset()] = block;
  }

  // Mark the file header as dirty; background loop will flush it
  void make_header_dirty() {
    _header_dirty.store(true, std::memory_order_release);
    _dirty_cv.notify_one();
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
    block_ptr blk(area);
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

  void return_single_areas(AreaList& areas) {
    _header->area_pool.return_single_areas(areas, *this);
    make_header_dirty();
  }

  void return_multi_areas(AreaList& areas) {
    _header->area_pool.return_multi_areas(areas, *this);
    make_header_dirty();
  }

  // Process all dirty blocks from the queue and write them to storage
  void write_dirty_blocks(size_t header_size) {
    // Process all dirty areas in the set
    // Use a batch approach for blocks with contiguous offsets
    
    // We'll collect blocks to write in this vector
    std::vector<block_ptr> blocks_to_write;
    
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

  void write_back_loop() {
    std::unique_lock<std::mutex> lock(_dirty_mutex);
    size_t header_size = calc_header_size();

    while (true) {
      // Wait for notification or stop signal
      _dirty_cv.wait(lock, [this]() {
        // Check if we should stop OR have work to do
        bool should_stop = _should_stop.load(std::memory_order_acquire);

        // Check other conditions that require locks
        std::lock_guard<std::mutex> queue_lock(_dirty_areas_mutex);
        bool has_dirty_work = _header_dirty.load(std::memory_order_acquire) ||
                              !_dirty_areas.empty();

        // Wake up either if stopping OR have work
        return should_stop || has_dirty_work;
      });

      // Check again immediately after wait returns
      if (_should_stop.load(std::memory_order_acquire)) {
        break;
      }

      // Call the extracted method to process dirty blocks
      write_dirty_blocks(header_size);
    }
  }

  void list_dbs(std::vector<std::string>& result) {
    for (uint16_t i = 0; i < _header->db_count; i++) {
      result.push_back(_header->dbs[i].name);
    }
  }

  Slice db_name(int index) const { return Slice(_header->dbs[index].name); }

  db_ptr operator[](const char* name) { return make(name); }

  db_ptr make(const char* name) {
    if (strlen(name) >= sizeof(_CacheBase::DBEntry::name)) throw KeyTooBig();

    // No locking needed since we're single-process
    int free = -1;
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        if (!strcmp(_header->dbs[i].name, name)) {
          if (_dbs[i].expired()) {
            db_ptr tmp = std::make_shared<DB>(*this, _header->dbs[i].offset, i);
            make_header_dirty();
            _dbs[i] = tmp;
            return _dbs[i].lock();
          }
          return _dbs[i].lock();
        }
      } else if (free < 0)
        free = i;
    }

    if (free < 0) throw LeavesException();
    std::snprintf(_header->dbs[free].name, sizeof(_header->dbs[free].name),
                  "%s", name);
    db_ptr tmp = std::make_shared<DB>(*this, &_header->dbs[free].offset, free);
    make_header_dirty();
    _dbs[free] = tmp;
    return _dbs[free].lock();
  }

  void remove_db(const char* name) {
    // No locking needed since we're single-process

    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset && !strcmp(_header->dbs[i].name, name)) {
        if (_dbs[i].use_count()) throw TransactionActive();
        DB tmp(*this, _header->dbs[i].offset, i);
        // Merge the DB's area lists back into storage
        _header->area_pool.single_areas.move(tmp._header->single_areas, *this);
        _header->area_pool.multi_areas.move(tmp._header->multi_areas, *this);
        _header->dbs[i].offset = 0;
        flush(true, true);
        return;
      }
    }
    throw WrongValue("database does not exist.");
  }
};

} // namespace leaves

#endif // _LEAVES__CACHESTORE_HPP