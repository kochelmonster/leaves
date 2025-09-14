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

static const char SIGNATURE[] = "larch-leaves";
static const size_t SIGNATURE_SIZE = padding(sizeof(SIGNATURE), 8);

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
    char signature[SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;
    AreaManager areas;
    uint16_t db_count;
    DBEntry dbs[0];

    FileHeader(uint16_t db_count_) {
      memset(this, 0, sizeof(FileHeader));
      strcpy(signature, SIGNATURE);
      db_count = db_count_;
      db_version = 0;
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
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static const bool is_transactional = true;
  typedef Opers_ Operations;
  typedef _DB<_CacheStore> DB;
  typedef std::shared_ptr<DB> db_ptr;
  typedef std::weak_ptr<DB> wdb_ptr;
  typedef std::vector<char> area_chunk_t;
  using Opers_::_header;
  using Opers_::write;
  using Opers_::read;
  using Opers_::resize;
  using CacheBase::DBEntry;

  struct LRUCache {
    using area_mem_t = typename Traits::Pointers::_AreaPtr;
    struct Entry {
      offset_t key;
      block_ptr block; // holds reference to area via ptr refcount
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
      _data.insert(Entry{key, block}); // inserts at end of sequenced index
      _size += block.size();
      prune();
    }

  void prune() {
      auto& seq = _data.template get<1>();
      while (_size > _capacity && !seq.empty()) {
        auto& entry = const_cast<Entry&>(seq.front());
        AreaSlice* slice = entry.block.area();
        if (slice->is_dirty()) break;
        _size -= entry.block.size();
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
  std::queue<block_ptr> dirty_areas;
  std::mutex dirty_areas_mutex;
  std::thread dirty_processor_thread;
  std::atomic<bool> should_stop;
  std::mutex dirty_mutex;
  std::condition_variable dirty_cv;

  _CacheStore(uint16_t db_count = 48) : should_stop(false) {
    _dbs.resize(db_count);

    // Start the dirty area processor thread
    dirty_processor_thread =
        std::thread(&_CacheStore::dirty_processor_loop, this);
  }

  ~_CacheStore() {
    // Signal the thread to stop and wake it up
    should_stop.store(true);
    dirty_cv.notify_all();

    // Wait for the thread to finish
    if (dirty_processor_thread.joinable()) {
      dirty_processor_thread.join();
    }
  }

  void flush(bool async = true) {}

  block_ptr resolve(offset_t offset, Access access = READ) const {
    using area_mem_t = typename Traits::Pointers::_AreaPtr;
    block_ptr cached;
    offset_t area_offset = offset - (offset % AREA_SIZE);
    if (_cache.get(area_offset, cached)) {
      AreaSlice* slice = cached.area();
      assert(slice->offset == area_offset);
      block_ptr result = cached; // increments refcount
      result._offset = static_cast<uint32_t>(offset - area_offset);
      return result;
    }

    AreaSlice area_header;
    read(area_offset, &area_header, sizeof(area_header));
    // Allocate area memory: [_AreaPtr][area bytes]
    size_t total = sizeof(area_mem_t) + area_header.get_size();
    void* raw = ::operator new(total);
    area_mem_t* aptr = new (raw) area_mem_t();
    aptr->ref.store(0);
    aptr->_offset = area_offset;
    aptr->_size = area_header.get_size();
    // Read full area into p[] starting with AreaSlice header
    read(area_offset, aptr->p, aptr->_size);

    block_ptr result(aptr);
    result._offset = static_cast<uint32_t>(offset - area_offset);
    _cache.put(area_offset, result);
    return result;
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return p->_iref.offset.type(p.type);
  }

  void make_dirty(block_ptr& block) {
    block.area()->set_dirty();
    {
      std::lock_guard<std::mutex> lock(dirty_areas_mutex);
      dirty_areas.push(block);
    }

    // Notify the dirty processor thread
    dirty_cv.notify_one();
  }
  
  AreaSlice get_area(uint64_t size) {
    // Ensure size is at least one area and aligned to AREA_SIZE
    assert(size);
    const uint64_t aligned = padding(size, AREA_SIZE);
    auto result = _header->areas.get(aligned, *this);
    if (!result) {
      // Start each new area on an AREA_SIZE boundary
      const uint64_t start = padding(_header->file_size, AREA_SIZE);
      result.set_offset(start);
      result.set_size(aligned);
      _header->file_size = padding(start + aligned, AREA_SIZE);
      resize(_header->file_size);
    }
    return result;
  }

  // Background thread loop for processing dirty areas
  void dirty_processor_loop() {
    std::unique_lock<std::mutex> lock(dirty_mutex);

    while (!should_stop.load()) {
      // Wait for notification or stop signal
      dirty_cv.wait(lock, [this]() {
        std::lock_guard<std::mutex> queue_lock(dirty_areas_mutex);
        return should_stop.load() || !dirty_areas.empty();
      });

      if (should_stop.load()) {
        break;
      }

      // Process all dirty areas in the queue
      while (true) {
        std::optional<block_ptr> opt_block;
        {
          std::lock_guard<std::mutex> queue_lock(dirty_areas_mutex);
          if (dirty_areas.empty()) break;
          opt_block = dirty_areas.front();
          dirty_areas.pop();
        }
        
        // Atomically check and clear the dirty bit
        if (opt_block) {
          auto& block = opt_block.value();
          if (block.area()->clear_dirty()) {
            // Successfully cleared dirty bit, now write the block
            write(block.offset(), block.area(), block.size());
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
    _dbs[free] = tmp;
    return _dbs[free].lock();
  }

  void remove_db(const char* name) {
    std::scoped_lock lock(_header->file_lock);

    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset && !strcmp(_header->dbs[i].name, name)) {
        if (_dbs[i].use_count()) throw TransactionActive();
        DB tmp(*this, _header->dbs[i].offset, i);
        _header->areas.merge(&tmp._header->areas, *this);
        _header->areas.merge(&tmp._header->big_areas, *this);
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

  size_t _header_size;

  _FileStore(const char* path, uint16_t db_count = 48) : base_t(db_count) {
    _header_size =
        padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
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

      if (strcmp(_header->signature, SIGNATURE)) {
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
    if (std::filesystem::file_size(filename()) != _header->file_size)
      std::filesystem::resize_file(filename(), _header->file_size);
  }
};

}  // namespace leaves

#endif  // _LEAVES__FSTORE_HPP