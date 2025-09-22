#ifndef _LEAVES__FSTORE_HPP
#define _LEAVES__FSTORE_HPP

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/process/v2/pid.hpp>
#include <condition_variable>
#include <cstdint>  // for uint8_t, uint16_t, uint32_t, uint64_t
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

#include "_db.hpp"
#include "_exception.hpp"
#include "_memory.hpp"  // for AreaSlice, SmartPointer
#include "_node.hpp"    // for _TrieNode
#include "_port.hpp"
#include "_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access

using boost::interprocess::interprocess_mutex;
using boost::process::v2::all_pids;
using boost::process::v2::current_pid;
using boost::process::v2::pid_type;

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
  struct Mutex {
    interprocess_mutex mutex;
    pid_type owner;

    void recover() {
      auto ap = all_pids();
      for (const auto& pid : ap) {
        if (pid == owner) return;
      }
      // locker does not exist anymore
      owner = 0;
      while (!mutex.try_lock()) mutex.unlock();
      mutex.unlock();  // one more unlock
    }

    template <typename Time = std::chrono::seconds>
    void lock(Time t = Time(10)) {
      while (!mutex.try_lock_for(t)) recover();

      owner = current_pid();
    }

    bool try_lock() {
      if (mutex.try_lock()) {
        owner = current_pid();
        return true;
      }
      return false;
    }

    void unlock() {
      owner = 0;
      mutex.unlock();
    }
  };

  struct FileHeader {
    char signature[FSTORE_SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;
    AreaPool area_pool;  // pool for both single and multi areas
    uint16_t db_count;
    DBEntry dbs[0];

    FileHeader(uint16_t db_count_) {
      memset(this, 0, sizeof(FileHeader));
      strcpy(signature, FSTORE_SIGNATURE);
      db_count = db_count_;
      db_version = 0;
      area_pool.init();
      memset(dbs, 0, sizeof(DBEntry) * db_count);
    }
  };

  std::string _filepath;
  mutable std::fstream _file;
  FileHeader* _header;

  void open(const char* path) {
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
    if (_file.is_open()) {
      _file.close();
    }
  }

  void write(offset_t offset, const void* ptr, size_t size) const {
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
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
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
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
  using Opers_::read;
  using Opers_::resize;
  using Opers_::write;
  using typename CacheBase::DBEntry;
  using typename Opers_::FileHeader;

  struct LRUCache {
    struct Entry {
      offset_t key;
      block_ptr block;  // holds reference to area via ptr refcount
    };
    // Use using-declarations for multi_index components
    typedef multi_index_container<
        Entry, indexed_by<hashed_unique<member<Entry, offset_t, &Entry::key>>,
                          sequenced<>>>
        container_t;
    container_t _data;

    LRUCache(size_t capacity = 10 * M) : _capacity(capacity) {}

    bool get(offset_t key, block_ptr& out) {
      auto& idx = _data.template get<0>();
      auto it = idx.find(key);
      if (it == idx.end()) return false;
      out = it->block;
      // Move accessed entry to the back of the sequenced index (MRU)
      auto& seq = _data.template get<1>();
      seq.relocate(seq.end(), seq.iterator_to(*it));
      return true;
    }

    void put(offset_t key, const block_ptr& block) {
      auto& idx = _data.template get<0>();
      auto it = idx.find(key);
      assert(it == idx.end());
      _data.insert(Entry{key, block});  // inserts at end of sequenced index
      _size += block.area()->get_size();
      prune();
    }

    void prune() {
      auto& seq = _data.template get<1>();
      while (_size > _capacity && !seq.empty()) {
        auto& entry = const_cast<Entry&>(seq.front());
        AreaSlice* slice = entry.block.area();
        if (slice->is_dirty()) break;
        _size -= entry.block.area()->get_size();
        seq.pop_front();
      }
    }

    size_t size() const { return _data.size(); }

    size_t _size = 0;
    size_t _capacity;
  };

  std::vector<wdb_ptr> _dbs;  // databases
  mutable LRUCache _cache;

  // Handling for dirty areas - using mutex-protected queue for thread safety
  std::queue<block_ptr> _dirty_areas;
  std::mutex _dirty_areas_mutex;
  std::thread _dirty_processor_thread;
  std::atomic<bool> _should_stop;
  std::mutex _dirty_mutex;
  std::condition_variable _dirty_cv;
  size_t _header_size;

  _CacheStore(uint16_t db_count = 48) : _should_stop(false) {
    _dbs.resize(db_count);
    _header_size =
        leaves::padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);

    // Start the dirty area processor thread
    _dirty_processor_thread =
        std::thread(&_CacheStore::dirty_processor_loop, this);
  }

  ~_CacheStore() {
    // Signal the thread to stop and wake it up
    _should_stop.store(true);
    _dirty_cv.notify_all();

    // Wait for the thread to finish
    if (_dirty_processor_thread.joinable()) {
      _dirty_processor_thread.join();
    }
  }

  void flush(bool async = true) {}

  block_ptr resolve(offset_t offset, Access access = READ) const {
    offset_t area_offset = offset - (offset % AREA_SIZE);
    // Check cache first
    block_ptr cached;
    if (_cache.get(area_offset, cached)) {
      AreaSlice* slice = cached.area();
      assert(slice->get_offset() == area_offset);
      block_ptr result = cached;  // copy increments refcount
      result._offset =
          static_cast<uint32_t>((uint64_t)offset - (uint64_t)area_offset);
      return result;
    }

    // Read on-disk header (could be partial / uninitialized)
    AreaSlice disk_header;
    read((uint64_t)area_offset, &disk_header, sizeof(disk_header));

    // Allocate full region (header + payload)
    AreaSlice* slice = (AreaSlice*)::operator new(disk_header.get_size());
    read((uint64_t)area_offset, slice, disk_header.get_size());
    slice->_ref.store(0);

    block_ptr result(slice);
    result._offset =
        static_cast<uint32_t>((uint64_t)offset - (uint64_t)area_offset);
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
    block.area()->set_dirty();
    {
      std::lock_guard<std::mutex> lock(_dirty_areas_mutex);
      _dirty_areas.push(block);
    }

    // Notify the dirty processor thread
    _dirty_cv.notify_one();
  }

  area_ptr alloc_single_area() {
    auto result = _header->area_pool.alloc_single_area(*this);
    if (!result) {
      // Extend storage with new area
      const uint64_t start = _header->file_size;
      _header->file_size = start + AREA_SIZE;
      resize(_header->file_size);
      save_header();
      
      // Create Area in the new memory location
      auto area = area_ptr(resolve(start, WRITE));
      area->init(start, AREA_SIZE, 0);
      return area;
    }
    return result;  // Return Area* directly
  }

  area_ptr alloc_multi_area(uint64_t size) {
    // Ensure size is multiple of AREA_SIZE
    const uint64_t aligned = padding(size, AREA_SIZE);

    auto result = _header->area_pool.alloc_multi_area(aligned, *this);
    if (!result) {
      // Extend storage with new area
      const uint64_t start = _header->file_size;
      _header->file_size = start + aligned;
      resize(_header->file_size);
      save_header();
      
      // Create Area in the new memory location
      auto area = area_ptr(resolve(start, WRITE));
      area->init(start, aligned, 0);
      return area;
    }
    return result;  // Return Area* directly
  }

  void return_single_areas(AreaList& areas) {
    _header->area_pool.return_single_areas(areas, *this);
    save_header();
  }

  void return_multi_areas(AreaList& areas) {
    _header->area_pool.return_multi_areas(areas, *this);
    save_header();
  }

  void save_header() { write(0, _header, _header_size); }

  // Background thread loop for processing dirty areas
  void dirty_processor_loop() {
    std::unique_lock<std::mutex> lock(_dirty_mutex);

    while (!_should_stop.load()) {
      // Wait for notification or stop signal
      _dirty_cv.wait(lock, [this]() {
        std::lock_guard<std::mutex> queue_lock(_dirty_areas_mutex);
        return _should_stop.load() || !_dirty_areas.empty();
      });

      if (_should_stop.load()) {
        break;
      }

      // Process all dirty areas in the queue
      while (true) {
        std::optional<block_ptr> opt_block;
        {
          std::lock_guard<std::mutex> queue_lock(_dirty_areas_mutex);
          if (_dirty_areas.empty()) break;
          opt_block = _dirty_areas.front();
          _dirty_areas.pop();
        }

        // Atomically check and clear the dirty bit
        if (opt_block) {
          auto& block = opt_block.value();
          if (block.area()->clear_dirty()) {
            // Successfully cleared dirty bit, now write the block
            write(block.area()->get_offset(), block.area(),
                  block.area()->get_size());
          }
        }
      }
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
    if (strlen(name) > sizeof(CacheBase::DBEntry::name)) throw KeyTooBig();

    std::scoped_lock lock(_header->file_lock);
    int free = -1;
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        if (!strcmp(_header->dbs[i].name, name)) {
          if (_dbs[i].expired()) {
            db_ptr tmp = std::make_shared<DB>(*this, _header->dbs[i].offset, i);
            save_header();
            _dbs[i] = tmp;
            return _dbs[i].lock();
          }
          return _dbs[i].lock();
        }
      } else if (free < 0)
        free = i;
    }

    if (free < 0) throw LeavesException();
    strcpy(_header->dbs[free].name, name);
    db_ptr tmp = std::make_shared<DB>(*this, &_header->dbs[free].offset, free);
    save_header();
    _dbs[free] = tmp;
    return _dbs[free].lock();
  }

  void remove_db(const char* name) {
    std::scoped_lock lock(_header->file_lock);

    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset && !strcmp(_header->dbs[i].name, name)) {
        if (_dbs[i].use_count()) throw TransactionActive();
        DB tmp(*this, _header->dbs[i].offset, i);
        // Merge the DB's area lists back into storage
        _header->area_pool.single_areas.move(tmp._header->single_areas, *this);
        _header->area_pool.multi_areas.move(tmp._header->multi_areas, *this);
        _header->dbs[i].offset = 0;
        flush();
        return;
      }
    }
    throw WrongValue("database does not exist.");
  }
};

struct _FileStore : _CacheStore<_StoreTraits, _FileOperations> {
  typedef _CacheStore<_StoreTraits, _FileOperations> base_t;

  _FileStore(const char* path, uint16_t db_count = 48) : base_t(db_count) {
    init_dbfile(path, db_count);
  }

  ~_FileStore() { delete[] (char*)_header; }

  void init_dbfile(const char* path, uint16_t db_count) {
    char* buffer = new char[_header_size];
    if (!std::filesystem::is_regular_file(path)) {
      open(path);
      _header = new (buffer) FileHeader(db_count);
      _header->file_size = _header_size;
      write(0, buffer, _header_size);
    } else {
      open(path);
      read(0, buffer, _header_size);
      _header = (FileHeader*)buffer;

      if (strcmp(_header->signature, FSTORE_SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }
      if (db_count && _header->db_count != db_count)
        throw WrongValue("db_count may not be changed.");
    }

    assert(((uint64_t)_header & 7) == 0);
    sanitize();
  }

  void sanitize() {
    std::scoped_lock lock(_header->file_lock);
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