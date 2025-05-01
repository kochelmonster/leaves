#ifndef _LEAVES__MMAP_HPP
#define _LEAVES__MMAP_HPP

#include <algorithm>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/process/v2/pid.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "_db.hpp"
#include "_exception.hpp"
#include "_memory.hpp"
#include "_node.hpp"
#include "_port.hpp"
#include "_traits.hpp"

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::file_mapping;
using boost::interprocess::interprocess_mutex;
using boost::interprocess::mapped_region;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using boost::process::v2::all_pids;
using boost::process::v2::current_pid;
using boost::process::v2::pid_type;

namespace leaves {

static const char SIGNATURE[] = "larch-leaves";
static const size_t SIGNATURE_SIZE = padding(sizeof(SIGNATURE), 8);

// definition og all headers and data types
struct _MemoryMapTraits {
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

  static constexpr size_t PAGE_SIZE = 1 * M;  // not OS PAGE_SIZE
  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr uint16_t BLOCK_SIZES[] = {
      _TrieNode<_MemoryMapTraits>::size(1, 10),   // digits 0-9
      _TrieNode<_MemoryMapTraits>::size(1, 16),   // hex 0-9A-F
      _TrieNode<_MemoryMapTraits>::size(1, 64),   // base64
      _TrieNode<_MemoryMapTraits>::size(1, 127),  // utf-8
      _TrieNode<_MemoryMapTraits>::size(1, 256),  // binary
      4 * K};
  static constexpr uint16_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

template <typename Traits_>
struct _MemoryMapFile {
  typedef Traits_ Traits;
  typedef _MemoryMapFile<Traits_> MemoryMapFile;
  using block_ptr = typename Traits::ptr;
  static constexpr auto MAX_PROCESSES = Traits::MAX_PROCESSES;
  static constexpr auto PAGE_SIZE = Traits::PAGE_SIZE;
  static const bool is_transactional = true;
  typedef _DB<MemoryMapFile> DB;
  typedef std::shared_ptr<DB> db_ptr;
  typedef std::weak_ptr<DB> wdb_ptr;

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

  struct DBEntry {
    char name[21];
    offset_t offset;
  };

  struct FileHeader {
    char signature[SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;
    AreaManager areas;
    pid_type processes[MAX_PROCESSES];
    uint16_t db_count;
    DBEntry dbs[0];

    FileHeader(uint16_t db_count_) {
      memset(this, 0, sizeof(FileHeader));
      strcpy(signature, SIGNATURE);
      db_count = db_count_;
      db_version = 0;
      memset(processes, 0, sizeof(processes));
      memset(dbs, 0, sizeof(DBEntry) * db_count);
    }
  };

  file_mapping _file;
  mapped_region _region;
  FileHeader* _memory;
  pid_type _pid;
  std::vector<wdb_ptr> _dbs;

  _MemoryMapFile(const char* path, size_t map_size = 2 * G,
                 uint16_t db_count = 48) {
    _pid = current_pid();
    init_dbfile(path, map_size, db_count);
    _dbs.resize(db_count);
  }

  ~_MemoryMapFile() {
    remove_pid();
    _region.flush();
  }

  const char* filename() const { return _file.get_name(); }

  Mutex& file_lock() { return _memory->file_lock; }

  void init_dbfile(const char* path, size_t map_size, uint16_t db_count) {
    if (!std::filesystem::is_regular_file(path)) {
      std::ofstream fhead(path, std::ios::out | std::ios::binary);
      fhead.put('l');
      fhead.close();
      uint64_t fsize =
          padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
      std::filesystem::resize_file(path, fsize);
      _file = file_mapping(path, read_write);
      _region = mapped_region(_file, read_write, 0, map_size);
      _memory = new (_region.get_address()) FileHeader(db_count);
      _memory->file_size = fsize;
      new (&_memory->file_lock) Mutex;
      _region.flush();
    } else {
      std::ifstream fin(path);
      char signature[sizeof(SIGNATURE)];
      fin.read(signature, sizeof(signature));
      if (strcmp(signature, SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }
      _file = file_mapping(path, read_write);
      _region = mapped_region(_file, read_write, 0, map_size);
      _memory = (FileHeader*)_region.get_address();
      if (db_count && _memory->db_count != db_count)
        throw WrongValue("db_count may not be changed.");
    }

    assert(((uint64_t)_memory & 7) == 0);
    sanitize();
    set_pid();
  }

  void set_pid() {
    std::scoped_lock lock(_memory->file_lock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (!_memory->processes[i]) {
        _memory->processes[i] = _pid;
        return;
      }
    }
    throw NoProcess();
  }

  void remove_pid() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (_memory->processes[i] == _pid) {
        _memory->processes[i] = 0;
        return;
      }
    }
  }

  void flush(bool async = true) { _region.flush(0, 0, async); }

  void sanitize() {
    sanitize_processes();
    std::scoped_lock lock(_memory->file_lock);
    if (std::filesystem::file_size(filename()) != _memory->file_size)
      std::filesystem::resize_file(filename(), _memory->file_size);
  }

  bool sanitize_processes() {
    auto ap = all_pids();
    std::sort(ap.begin(), ap.end());

    int free_count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (_memory->processes[i]) {
        if (!std::binary_search(ap.begin(), ap.end(), _memory->processes[i])) {
          _memory->processes[i] = 0;
          free_count++;
        }
      } else
        free_count++;
    }
    return free_count == MAX_PROCESSES;  // the first to open the db
  }

  block_ptr resolve(offset_t offset, Access access = READ) const {
    char* p = (char*)_memory + (uint64_t)offset;
    prefetch(p, access);
    return block_ptr(p);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return offset_t((uint64_t)p - (uint64_t)_memory).type(p.type);
  }

  void prefetch(offset_t offset, Access access = READ) const {
    prefetch((char*)_memory + (uint64_t)offset, access);
  }

  void prefetch(void* mem, Access access = READ) const {
    leaves::prefetch(mem, access);
  }

  AreaSlice get_area(uint64_t size) {
    auto result = _memory->areas.get(size, *this);
    if (!result) {
      result.offset = _memory->file_size;
      _memory->file_size = padding(_memory->file_size + size, PAGE_SIZE);
      result.size = _memory->file_size - result.offset;
      std::filesystem::resize_file(filename(), _memory->file_size);
    }
    return result;
  }

  void list_dbs(std::vector<std::string>& result) {
    for (uint16_t i = 0; i < _memory->db_count; i++) {
      result.push_back(_memory->dbs[i].name);
    }
  }

  db_ptr operator[](const char* name) { return make(name); }

  db_ptr make(const char* name) {
    if (strlen(name) > sizeof(DBEntry::name)) throw KeyToBig();

    std::scoped_lock lock(_memory->file_lock);
    int free = -1;
    for (uint16_t i = 0; i < _memory->db_count; i++) {
      if (_memory->dbs[i].offset) {
        if (!strcmp(_memory->dbs[i].name, name)) {
          if (_dbs[i].expired()) {
            db_ptr tmp = std::make_shared<DB>(*this, _memory->dbs[i].offset, i);
            _dbs[i] = tmp;
            return _dbs[i].lock();
          }
          return _dbs[i].lock();
        }
      } else if (free < 0)
        free = i;
    }

    if (free < 0) throw LeavesException();
    strcpy(_memory->dbs[free].name, name);
    db_ptr tmp = std::make_shared<DB>(*this, &_memory->dbs[free].offset, free);
    _dbs[free] = tmp;
    return _dbs[free].lock();
  }

  void remove_db(const char* name) {
    std::scoped_lock lock(_memory->file_lock);

    for (uint16_t i = 0; i < _memory->db_count; i++) {
      if (_memory->dbs[i].offset && !strcmp(_memory->dbs[i].name, name)) {
        if (_dbs[i].use_count()) throw TransactionActive();
        DB tmp(*this, _memory->dbs[i].offset, i);
        _memory->areas.merge(&tmp._header->areas, *this);
        _memory->areas.merge(&tmp._header->big_areas, *this);
        _memory->dbs[i].offset = 0;
        flush();
        return;
      }
    }
    throw WrongValue("database does not exist.");
  }
};

}  // namespace leaves

#endif  // _LEAVES__MMAP_HPP