#ifndef _LEAVES__FSTORE_HPP
#define _LEAVES__FSTORE_HPP

#include <iostream>

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <thread>

#include "_db.hpp"
#include "_exception.hpp"
#include "_memory.hpp"  // for AreaSlice, SmartPointer
#include "_node.hpp"    // for _TrieNode
#include "_port.hpp"
#include "_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access

// Removed interprocess mutex and process ID references since they're no longer
// needed

namespace leaves {

static const char FSTORE_SIGNATURE[] = "larch-leaves";
static const size_t FSTORE_SIGNATURE_SIZE =
    padding(sizeof(FSTORE_SIGNATURE), 8);

// definition of all headers and data types
struct _StoreTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  /*
  Typical node sizes
  digits: 0-9:     104
  hex:    0-9A-F   160
  base64: 64       564
  utf-8:  127      1056
  binary: 256      2088
  max: 2264
  */

#pragma pack(1)
  struct BlockHeader {
    typedef BlockHeader Base;
    tid_t txn_id;
    uint8_t slot_id;
    uint8_t free_idx;
  };
#pragma pack(0)

  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 64 * K;  // not OS AREA_SIZE
  static constexpr uint16_t BLOCK_SIZES[] = {
      _TrieNode<_StoreTraits>::size(1, 10),   // digits 0-9
      _TrieNode<_StoreTraits>::size(1, 16),   // hex 0-9A-F
      _TrieNode<_StoreTraits>::size(1, 64),   // base64
      _TrieNode<_StoreTraits>::size(1, 127),  // utf-8
      _TrieNode<_StoreTraits>::size(1, 256),  // binary
      4 * K};
  static constexpr uint16_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);
  typedef SmartPointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

struct CacheBase {
  struct DBEntry {
    char name[21];
    offset_t offset;
  };
};

struct _FileOperations : CacheBase {
  // Simple placeholder for compatibility, no actual locking
  struct Mutex {
    // No mutex needed for single-process use

    template <typename Time = std::chrono::seconds>
    void lock(Time t = Time(10)) {
      // No locking needed
    }

    bool try_lock() {
      // Always succeeds since no locking is needed
      return true;
    }

    void unlock() {
      // No unlocking needed
    }
  };

  struct FileHeader {
    char signature[FSTORE_SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;     // Kept for compatibility but no longer needed
    AreaPool area_pool;  // pool for both single and multi areas
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
      std::strcpy(signature, FSTORE_SIGNATURE);
      area_pool.init();
      std::memset(dbs, 0, sizeof(DBEntry) * db_count);
    }
  };

  std::string _filepath;
  mutable std::fstream _file;
  FileHeader* _header;
  // Protect concurrent read/write/resize operations on the same fstream
  mutable std::mutex _io_mutex;

  size_t file_size() const { return _header->file_size; }

  size_t calc_header_size() const {
    return leaves::padding(
        sizeof(FileHeader) + sizeof(DBEntry) * _header->db_count, 4 * K);
  }

  void open(const char* path) {
    std::lock_guard<std::mutex> lock(_io_mutex);
    _filepath = path;
    _file.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!_file.is_open()) {
      // Try to create the file if it doesn't exist
      _file.open(path, std::ios::in | std::ios::out | std::ios::binary |
                           std::ios::trunc);
      if (!_file.is_open()) {
        throw std::runtime_error("Failed to open file");
      }
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (_file.is_open()) {
      _file.close();
    }
  }

  void write(offset_t offset, const void* ptr, size_t size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
    _file.clear();
    _file.seekp(static_cast<std::streampos>(offset));
    if (_file.fail()) {
      throw std::runtime_error("Failed to seek to offset");
    }
    _file.write(static_cast<const char*>(ptr), size);
    if (_file.fail()) {
      throw std::runtime_error("Failed to write data");
    }
    _file.flush();
  }

  void read(offset_t offset, void* ptr, size_t size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
    _file.clear();
    _file.seekg(static_cast<std::streampos>(offset));
    if (_file.fail()) {
      throw std::runtime_error("Failed to seek to offset");
    }
    _file.read(static_cast<char*>(ptr), size);
    if (_file.fail() || _file.gcount() != static_cast<std::streamsize>(size)) {
      throw std::runtime_error("Failed to read data");
    }
  }

  void resize(size_t new_size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
    _file.close();
    std::filesystem::resize_file(_filepath, new_size);
    _file.open(_filepath, std::ios::in | std::ios::out | std::ios::binary);
    if (!_file.is_open()) {
      throw std::runtime_error("Failed to reopen file after resize");
    }
  }

  const char* filename() const { return _filepath.c_str(); }

  Mutex& file_lock() { return _header->file_lock; }
};

using boost::multi_index::hashed_unique;
using boost::multi_index::indexed_by;
using boost::multi_index::member;
using boost::multi_index::multi_index_container;
using boost::multi_index::sequenced;

template <typename Traits_, typename Opers_>
struct _CacheStore : public Opers_ {
  typedef Traits_ Traits;
  typedef _CacheStore<Traits_, Opers_> Self;
  using block_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static const bool is_transactional = true;
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
  using DBEntry = typename CacheBase::DBEntry;

  struct LRUCache {
    struct Entry {
      uint64_t key;
      block_ptr block;  // holds reference to area via ptr refcount
    };
    // Use using-declarations for multi_index components
    typedef multi_index_container<
        Entry, indexed_by<hashed_unique<member<Entry, uint64_t, &Entry::key>>,
                          sequenced<>>>
        container_t;
    container_t _data;

    LRUCache(size_t capacity = 500 * M) : _capacity(capacity) {
      _data.reserve(_capacity / AREA_SIZE);
    }

    bool get(uint64_t key, block_ptr& out) {
      auto& idx = _data.template get<0>();
      auto it = idx.find(key);
      if (it == idx.end()) return false;
      out = it->block;
      // Move accessed entry to the back of the sequenced index (MRU)
      auto& seq = _data.template get<1>();
      seq.relocate(seq.end(), seq.iterator_to(*it));
      return true;
    }

    void put(uint64_t key, const block_ptr& block) {
      auto& idx = _data.template get<0>();
      auto it = idx.find(key);
      assert(it == idx.end());
      _data.insert(Entry{key, block});  // inserts at end of sequenced index
      _size += block.area()->get_size();
      prune();
    }

    void prune() {
      auto& seq = _data.template get<1>();
      constexpr size_t WINDOW = 64;  // bounded scan to avoid O(n)
      while (_size > _capacity && !seq.empty()) {
        // Try to evict first clean entry within the window
        bool evicted = false;
        size_t scanned = 0;
        for (auto it = seq.begin(); it != seq.end() && scanned < WINDOW;
             ++it, ++scanned) {
          auto& entry = const_cast<Entry&>(*it);
          AreaSlice* slice = entry.block.area();
          if (slice->get_ref() == 1) {
            _size -= slice->get_size();
            seq.erase(it);
            evicted = true;
            break;
          }
        }
        if (!evicted) break;  // all in window dirty; stop for now
      }
    }

    size_t size() const { return _data.size(); }

    size_t _size = 0;
    size_t _capacity;
  };

  std::vector<wdb_ptr> _dbs;  // databases
  mutable LRUCache _cache;

  void debug_check_cache() const {
    std::cout << "\n==== DEBUG CACHE CHECK ====\n";
    std::cout << "Cache entries: " << _cache.size()
              << ", Total size: " << _cache._size << " / " << _cache._capacity
              << " bytes\n";

    // Get header size once for efficiency
    size_t header_size = calc_header_size();
    size_t mismatches = 0;
    size_t checked = 0;

    // Iterate through all cache entries
    const auto& seq = _cache._data.template get<1>();
    for (const auto& entry : seq) {
      checked++;
      uint64_t area_offset = entry.key;
      uint64_t read_offset = area_offset + header_size;
      const AreaSlice* cached_slice = entry.block.area();

      // Get the size of the cached block
      size_t size = cached_slice->get_size();

      // Allocate memory for on-disk data
      AreaSlice* disk_data = (AreaSlice*)::operator new(size);

      try {
        // Read the block from disk
        read(read_offset, disk_data, size);

        // Compare content byte by byte (we're skipping the _ref field and dirty
        // bit since they're in-memory only)
        bool content_mismatch = false;
        uint8_t* cached_bytes = (uint8_t*)cached_slice;
        uint8_t* disk_bytes = (uint8_t*)disk_data;

        // Compare bytes excluding _ref field which is memory-only
        size_t diff_start = 0;
        for (size_t i = 0; i < size; i++) {
          // Skip the reference count field which is memory-only
          if (i == offsetof(AreaSlice, _ref)) {
            i += sizeof(std::atomic<uint32_t>) - 1;
            continue;
          }

          // Skip the dirty bit in the offset field (lowest bit)
          if (i == offsetof(AreaSlice, _offset) ||
              i == offsetof(AreaSlice, _offset) + 1 ||
              i == offsetof(AreaSlice, _offset) + 2 ||
              i == offsetof(AreaSlice, _offset) + 3) {
            continue;
          }

          if (cached_bytes[i] != disk_bytes[i]) {
            if (!content_mismatch) {
              content_mismatch = true;
              diff_start = i;
            }
          }
        }

        if (content_mismatch) {
          mismatches++;
          std::cout << "Content mismatch at offset " << area_offset
                    << ", diff starts at header offset " << diff_start
                    << std::endl;

          // Print a hex dump of the differing bytes (16 bytes)
          std::cout << "  Cached: ";
          for (size_t i = diff_start; i < diff_start + 16 && i < size; i++) {
            printf("%02X ", cached_bytes[i]);
          }
          std::cout << std::endl;

          std::cout << "  Disk:   ";
          for (size_t i = diff_start; i < diff_start + 16 && i < size; i++) {
            printf("%02X ", disk_bytes[i]);
          }
          std::cout << std::endl;
        }

        ::operator delete(disk_data);
      } catch (const std::exception& e) {
        ::operator delete(disk_data);
        std::cerr << "Exception during cache check: " << e.what() << std::endl;
      }
    }

    std::cout << "==== CACHE CHECK COMPLETE ====\n";
    std::cout << "Checked " << checked << " entries, found " << mismatches
              << " mismatches\n";
    if (mismatches == 0) {
      std::cout << "Cache is in sync with disk data.\n";
    } else {
      std::cout << "WARNING: Cache has " << mismatches
                << " mismatches with disk data!\n";
    }
    std::cout << "============================\n\n";
  }

  void debug_reset() {
    std::cout << "Resetting cache (clearing all " << _cache.size()
              << " entries)\n";
    _cache._data.clear();
    _cache._size = 0;
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

  _CacheStore(uint16_t db_count = 48, size_t capacity = 10 * M)
      : _cache(capacity), _should_stop(false) {
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

  void flush(bool sync = false, bool force = false) {
    bool has_pending = false;
    {
      std::lock_guard<std::mutex> lock(_dirty_areas_mutex);
      has_pending = !_pending_dirty_areas.empty();
      _dirty_areas.insert(_pending_dirty_areas.begin(), _pending_dirty_areas.end());
      _pending_dirty_areas.clear();
    }

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
      assert(slice->get_offset() == area_offset);
      block_ptr result = cached;  // copy increments refcount
      result._offset = static_cast<uint32_t>(raw_offset - area_offset);
      return result;
    }

    uint64_t read_offset = area_offset + calc_header_size();

    // Read on-disk header (could be partial / uninitialized)
    AreaSlice disk_header;
    read((uint64_t)read_offset, &disk_header, sizeof(disk_header));

    // Allocate full region (header + payload)
    AreaSlice* slice = (AreaSlice*)::operator new(disk_header.get_size());
    read((uint64_t)read_offset, slice, disk_header.get_size());
    slice->_ref.store(0);

    block_ptr result(slice);
    result._offset = static_cast<uint32_t>(raw_offset - area_offset);
    _cache.put(area_offset, result);
    return result;
  }

  // Resolve function for SmartPointer ptr type
  offset_t resolve(const typename Traits::ptr& p) const {
    return offset_t(p._iref->get_offset() + p._offset).type(p.type);
  }

  // Resolve function for SmartPointer Pointer<T> type
  template <typename T, NodeTypes type>
  offset_t resolve(const typename Traits::template Pointer<T, type>& p) const {
    return offset_t(p._iref->get_offset() + p._offset).type(p.type);
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
    _pending_dirty_areas[block.area()->get_offset()] = block;
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
    while (true) {
      block_ptr block;
      {
        std::lock_guard<std::mutex> queue_lock(_dirty_areas_mutex);
        if (_dirty_areas.empty()) break;
        auto it = _dirty_areas.begin();
        block = it->second;
        _dirty_areas.erase(it);
      }

      assert(block);
      const auto& area = block.area();

      // Write the block to disk
      write(header_size + area->get_offset(), area, area->get_size());
    }

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
    if (strlen(name) >= sizeof(CacheBase::DBEntry::name)) throw KeyTooBig();

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

struct _FileStore : _CacheStore<_StoreTraits, _FileOperations> {
  typedef _CacheStore<_StoreTraits, _FileOperations> base_t;

  _FileStore(const char* path, uint16_t db_count = 48, size_t capacity = 10 * M)
      : base_t(db_count, capacity) {
    init_dbfile(path, db_count);
    start_write_back_thread();
  }

  ~_FileStore() {
    destroy();
    delete[] (char*)_header;
  }

  void init_dbfile(const char* path, uint16_t db_count) {
    // Compute header size based on requested db_count first
    size_t header_size =
        leaves::padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
    char* buffer = new char[header_size];
    if (!std::filesystem::is_regular_file(path)) {
      open(path);
      _header = new (buffer) FileHeader(db_count);
      // Ensure enough space on disk for first area aligned to AREA_SIZE
      // boundary
      _header->file_size = header_size;
      resize(_header->file_size);
      // Write header
      write(0, buffer, header_size);
    } else {
      open(path);
      read(0, buffer, header_size);
      _header = (FileHeader*)buffer;

      if (strcmp(_header->signature, FSTORE_SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }
      if (_header->db_count != db_count)
        throw WrongValue("db_count may not be changed.");
    }

    assert(((uint64_t)_header & 7) == 0);
    sanitize();
  }

  void sanitize() {
    // No locking needed since we're single-process
    sanitize_dbs();
    if (std::filesystem::file_size(filename()) != _header->file_size)
      std::filesystem::resize_file(filename(), _header->file_size);
  }

  void sanitize_dbs() {
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        assert(_dbs[i].expired());
        _DB(*this, _header->dbs[i].offset, i).sanitize();
      }
    }
  }

  // Compatibility method for tests
  AreaSlice get_area(size_t size) {
    auto area_ptr = alloc_multi_area(size);
    return *area_ptr;  // Convert Area* to AreaSlice
  }
};

}  // namespace leaves

#endif  // _LEAVES__FSTORE_HPP