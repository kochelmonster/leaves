#ifndef _LEAVES__IMMAP_HPP
#define _LEAVES__IMMAP_HPP

#include <algorithm>
#include <atomic>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/process/v2/pid.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <type_traits>

#include "../core/_exception.hpp"
#include "../core/_node.hpp"
#include "../core/_port.hpp"
#include "../core/_traits.hpp"
#include "../core/_util.hpp"
#include "../db/_db.hpp"
#include "../memory/_memory.hpp"

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using boost::process::v2::all_pids;
using boost::process::v2::current_pid;
using boost::process::v2::pid_type;

namespace leaves {

static const char MMAP_SIGNATURE[] = "larch-leaves-mmap";
static const size_t MMAP_SIGNATURE_SIZE = padding(sizeof(MMAP_SIGNATURE), 8);

// definition of all headers and data types
struct _MemoryMapTraits {
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

  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 512 * K;
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr uint16_t MEM_MANAGER_POOL_SIZE = 3;
  static constexpr int GC_INTERVAL = 10;
  static constexpr uint16_t MERGE_POOL_THREADS = 5;
  static constexpr uint16_t MERGE_DISPATCH_THRESHOLD = 10;  // minimum trie fan-out

  static constexpr uint16_t PAGE_SIZES[] = {                         // Page sizes (header + node)
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 10),   // digits 0-9
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 16),   // hex 0-9A-F
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 64),   // base64
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 127),  // utf-8
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 256),  // binary
      4 * K};
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);

  using ptr = SimplePointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SimplePointer<T, type>;
};

template <typename Traits_, template <typename> class DB_ = _DB,
          typename Self_ = void>
struct _MemoryMapFile {
  typedef Traits_ Traits;
  // CRTP: if Self_ is provided, use it as the storage type seen by DB;
  // otherwise default to this class itself (non-derived usage).
  using MemoryMapFile = std::conditional_t<
      std::is_void_v<Self_>, _MemoryMapFile<Traits_, DB_, Self_>, Self_>;
  using page_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto MAX_PROCESSES = Traits::MAX_PROCESSES;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  typedef DB_<MemoryMapFile> DB;
  typedef std::unique_ptr<DB> _db_ptr;

  // When Self_ is provided, DB's Storage_ is the derived type.
  // _self() downcasts *this so references match DB's constructor.
  MemoryMapFile& _self() { return static_cast<MemoryMapFile&>(*this); }

  using Mutex = boost::interprocess::interprocess_mutex;

  struct DBEntry {
    char name[21];
    offset_t offset;
  };

  struct FileHeader {
    char signature[MMAP_SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;
    AreaPool area_pool;  // pool for both single and multi areas
    pid_type processes[MAX_PROCESSES];
    uint16_t db_count;
    std::atomic<int64_t> last_cursor_id;
    DBEntry dbs[0];

    FileHeader(uint16_t db_count_)
        : db_version(0),
          file_size(0),
          file_lock(),
          processes{},
          db_count(db_count_) {
      // Set signature and initialize pools/arrays
      memset(signature, 0, sizeof(signature));
      strcpy(signature, MMAP_SIGNATURE);
      area_pool.init();
      // processes[] already zero-initialized via aggregate init
      memset((void*)dbs, 0, sizeof(DBEntry) * db_count);
    }
  };

  file_mapping _file;
  mapped_region _region;
  FileHeader* _memory;
  pid_type _pid;
  std::vector<_db_ptr> _dbs;

  _MemoryMapFile(const char* path, size_t map_size = 2 * G,
                 uint16_t db_count = 48) {
    _pid = current_pid();
    _dbs.resize(db_count);
    init_dbfile(path, map_size, db_count);
  }

  ~_MemoryMapFile() {
    remove_pid();
    _region.flush();
  }

  const char* filename() const { return _file.get_name(); }

  Mutex& file_lock() { return _memory->file_lock; }

  size_t file_size() const { return _memory->file_size; }

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
      _region.flush();
    } else {
      std::ifstream fin(path);
      char signature[sizeof(MMAP_SIGNATURE)];
      fin.read(signature, sizeof(signature));
      if (strcmp(signature, MMAP_SIGNATURE)) {
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
    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (!_memory->processes[i]) {
        _memory->processes[i] = _pid;
        return;
      }
    }
    throw NoProcess();
  }

  void remove_pid() {
    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (_memory->processes[i] == _pid) {
        _memory->processes[i] = 0;
        return;
      }
    }
  }

  uint64_t new_cursor_id() {
    return _memory->last_cursor_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void flush(bool sync = false, bool force = false) {
    if (force) {
      // Only sync actual file size, not entire mapped region
      _region.flush(0, _memory->file_size, !sync);
    }
  }

  void sanitize() {
    // Coordinate sanitization across processes with an OS file lock that is
    // automatically released if a process crashes.
    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);

    if (sanitize_processes()) {
      new (&_memory->file_lock) Mutex();
      sanitize_dbs();
      _memory->last_cursor_id.store(0);
    }
    if (std::filesystem::file_size(filename()) != _memory->file_size)
      std::filesystem::resize_file(filename(), _memory->file_size);

    assert(_region.get_size() >= _memory->file_size);
  }

  void sanitize_dbs() {
    for (uint16_t i = 0; i < _memory->db_count; i++) {
      if (_memory->dbs[i].offset) {
        assert(!_dbs[i]);
        DB(_self(), _memory->dbs[i].offset, i).sanitize();
      }
    }
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

  // Resolve offset - handles both absolute and relative offsets uniformly
  page_ptr resolve(const offset_t* offset_ptr, Access access = READ) const {
    char* p;

    if (offset_ptr->is_relative()) {
      // Relative: calculate address relative to where offset_t is stored
      p = offset_ptr->resolve<char>();
    } else {
      // Absolute: offset from _memory base
      p = (char*)_memory + (uint64_t)*offset_ptr;
    }

    prefetch(p, access);
    return page_ptr(p);
  }

  template <typename Pointer>
  typename std::enable_if<!std::is_pointer<Pointer>::value, offset_t>::type
  resolve(const Pointer& p) const {
    uint64_t offset = (uint64_t)p - (uint64_t)_memory;
    assert(offset < _memory->file_size);
    return offset_t(offset).type(p.type);
  }

  template <typename PtrType>
  void make_dirty(PtrType /*block*/) {}

  void prefetch(const offset_t* offset_ptr, Access access = READ) const {
    offset_t offset = *offset_ptr;
    if (offset.is_relative()) {
      int64_t rel_value = offset.as_signed();
      prefetch((char*)offset_ptr + rel_value, access);
    } else {
      prefetch((char*)_memory + (uint64_t)offset, access);
    }
  }

  void prefetch(void* mem, Access access = READ) const {
    leaves::prefetch(mem, access);
  }

  area_ptr resize_file(uint64_t size) {
    // Extend storage with new area - grow by at least 10*AREA_SIZE
    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);

    // Geometric growth: grow by at least 25% of current size or 10*AREA_SIZE
    constexpr uint64_t MIN_GROWTH = 10 * AREA_SIZE;
    uint64_t geometric_growth = _memory->file_size / 4;  // 25% growth
    uint64_t total_growth = std::max({size, MIN_GROWTH, geometric_growth});
    total_growth = padding(total_growth, AREA_SIZE);

    if (_memory->file_size + total_growth > _region.get_size())
      throw StorageFull();

    offset_t new_offset = _memory->file_size;
    _memory->file_size = _memory->file_size + total_growth;
    std::filesystem::resize_file(filename(), _memory->file_size);

    // Create Area for the requested size
    auto area = area_ptr(resolve(&new_offset, WRITE));
    area->init(new_offset, size, 0);

    // Add remaining space to multi_areas pool
    if (total_growth > size) {
      offset_t extra_offset = new_offset + size;
      uint64_t extra_size = total_growth - size;
      auto extra_area = area_ptr(resolve(&extra_offset, WRITE));
      extra_area->init(extra_offset, extra_size, 0);
      _memory->area_pool.multi_areas.push(*extra_area, *this);
    }

    return area;
  }

  area_ptr alloc_single_area() {
    auto result = _memory->area_pool.alloc_single_area(*this);
    if (!result) return resize_file(AREA_SIZE);
    return result;  // Return Area* directly
  }

  area_ptr alloc_multi_area(uint64_t size) {
    // Ensure size is multiple of AREA_SIZE
    size = padding(size, AREA_SIZE);
    auto result = _memory->area_pool.alloc_multi_area(size, *this);
    if (!result) return resize_file(size);
    return result;  // Return Area* directly
  }

  void return_single_areas(offset_t head, offset_t tail) {
    _memory->area_pool.return_single_areas(head, tail, *this);
  }

  void return_multi_areas(offset_t head, offset_t tail) {
    _memory->area_pool.return_multi_areas(head, tail, *this);
  }

  void list_dbs(std::vector<std::string>& result) {
    for (uint16_t i = 0; i < _memory->db_count; i++) {
      result.push_back(_memory->dbs[i].name);
    }
  }

  Slice db_name(int index) const { return Slice(_memory->dbs[index].name); }

  DB* operator[](const char* name) { return make(name); }

  DB* make(const char* name) {
    if (strlen(name) >= sizeof(DBEntry::name)) throw KeyTooBig();

    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);
    int free = -1;
    for (uint16_t i = 0; i < _memory->db_count; i++) {
      if (_memory->dbs[i].offset) {
        if (!strcmp(_memory->dbs[i].name, name)) {
          if (!_dbs[i]) {
            _dbs[i] = std::make_unique<DB>(_self(), _memory->dbs[i].offset, i);
            return _dbs[i].get();
          }
          return _dbs[i].get();
        }
      } else if (free < 0)
        free = i;
    }

    if (free < 0) throw LeavesException();
    strcpy(_memory->dbs[free].name, name);
    _dbs[free] = std::make_unique<DB>(_self(), &_memory->dbs[free].offset, free);
    return _dbs[free].get();
  }

  void remove_db(const char* name) {
    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);

    for (uint16_t i = 0; i < _memory->db_count; i++) {
      if (_memory->dbs[i].offset && !strcmp(_memory->dbs[i].name, name)) {
        if (_dbs[i] && _dbs[i]->is_active()) throw TransactionActive();
        DB tmp(_self(), _memory->dbs[i].offset, i);
        tmp.return_areas();
        _memory->dbs[i].offset = 0;
        flush();
        return;
      }
    }
    throw WrongValue("database does not exist.");
  }
};

}  // namespace leaves

#endif  // _LEAVES__IMMAP_HPP