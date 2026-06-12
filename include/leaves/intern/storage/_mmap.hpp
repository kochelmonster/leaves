#ifndef _LEAVES__IMMAP_HPP
#define _LEAVES__IMMAP_HPP

#include <algorithm>
#include <atomic>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#ifndef LEAVES_SINGLE_PROCESS
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/process/v2/pid.hpp>
#else
#include <mutex>
#endif
#include <cstdint>
#include <cstring>
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
#include "../third_party/unordered_dense.h"
#include "../util/_threadpool.hpp"
#include "_db_directory.hpp"

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
#ifndef LEAVES_SINGLE_PROCESS
using boost::process::v2::all_pids;
using boost::process::v2::current_pid;
using boost::process::v2::pid_type;
#else
typedef uint32_t pid_type;
#endif

namespace leaves {

static const char MMAP_SIGNATURE[] = "larch-leaves-mmap";
static const size_t MMAP_SIGNATURE_SIZE = padding(sizeof(MMAP_SIGNATURE), 8);

// definition of all headers and data types
struct _MemoryMapTraits {
  using Aspect = DefaultAspect;
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
#ifdef LEAVES_SINGLE_PROCESS
  static constexpr uint16_t MAX_PROCESSES = 1;
#else
  static constexpr uint16_t MAX_PROCESSES = 100;
#endif
  static constexpr uint16_t MEM_MANAGER_POOL_SIZE = 3;

  static constexpr uint16_t PAGE_SIZES_DECL[] = {  // Page sizes (header + node)
      sizeof(PageHeader) +
          _TrieNode<_MemoryMapTraits>::size(1, 2),  // 2 branches
      sizeof(PageHeader) +
          _TrieNode<_MemoryMapTraits>::size(1, 3),  // 3 branches
      sizeof(PageHeader) +
          _TrieNode<_MemoryMapTraits>::size(1, 4),  // 4 branches
      sizeof(PageHeader) +
          _TrieNode<_MemoryMapTraits>::size(1, 10),  // 5-10 branches
      sizeof(PageHeader) +
          _TrieNode<_MemoryMapTraits>::size(1, 16),  // hex 0-9A-F
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 64),   // base64
      sizeof(PageHeader) + _TrieNode<_MemoryMapTraits>::size(1, 256),  // binary
      sizeof(PageHeader) + 1024,
      sizeof(PageHeader) + 1024 + 512,
      4 * K};
  static constexpr auto PAGE_SIZES = [] {
    std::array<uint16_t, std::size(PAGE_SIZES_DECL)> a{};
    for (std::size_t i = 0; i < std::size(PAGE_SIZES_DECL); ++i)
      a[i] = PAGE_SIZES_DECL[i];
    std::sort(a.begin(), a.end());
    return a;
  }();
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);

  using ptr = SimplePointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SimplePointer<T, type>;
};

template <typename Traits_, typename Self_ = void>
struct _MemoryMapFile
    : public _ThreadPoolMixin<_MemoryMapFile<Traits_, Self_>> {
  typedef Traits_ Traits;
  using PoolMixin = _ThreadPoolMixin<_MemoryMapFile<Traits_, Self_>>;
  // CRTP: if Self_ is provided, use it as the storage type seen by DB;
  // otherwise default to this class itself (non-derived usage).
  using MemoryMapFile =
      std::conditional_t<std::is_void_v<Self_>, _MemoryMapFile<Traits_, Self_>,
                         Self_>;
  using page_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto MAX_PROCESSES = Traits::MAX_PROCESSES;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;

  // When Self_ is provided, DB's Storage_ is the derived type.
  // _self() downcasts *this so references match DB's constructor.
  MemoryMapFile& _self() { return static_cast<MemoryMapFile&>(*this); }

#ifdef LEAVES_SINGLE_PROCESS
  using Mutex = std::recursive_mutex;
#else
  using Mutex = boost::interprocess::interprocess_recursive_mutex;
#endif

  using DBEntry = _DBDirectoryEntry;

  struct FileHeader {
    char signature[MMAP_SIGNATURE_SIZE];
    uint16_t db_version;
    uint16_t max_processes;
    size_t file_size;
    Mutex file_lock;
    AreaPool area_pool;  // pool for both single and multi areas
    pid_type processes[MAX_PROCESSES];
    std::atomic<int64_t> last_cursor_id;
    uint32_t sanitize_generation;  // incremented when first process opens file
    uint32_t
        clean_close;  // 1 = last close was clean, 0 = dirty (open or crashed)
    uint16_t db_entry_count;  // entries used in first directory page
    offset_t db_next_page;    // link to overflow directory page (0 = none)
    DBEntry dbs[];            // flexible array fills to 4K boundary

    FileHeader()
        : db_version(0),
          max_processes(MAX_PROCESSES),
          file_size(0),
          file_lock(),
          processes{},
          last_cursor_id(0),
          sanitize_generation(0),
          clean_close(0),
          db_entry_count(0),
          db_next_page(0) {
      // Set signature and initialize pools/arrays
      memset(signature, 0, sizeof(signature));
      strcpy(signature, MMAP_SIGNATURE);
      area_pool.init();
      uint16_t cap = _DBDirectoryPage::capacity_for(4 * K - sizeof(FileHeader));
      memset((void*)dbs, 0, sizeof(DBEntry) * cap);
    }
  };

  file_mapping _file;
  mapped_region _region;
  FileHeader* _memory;
  pid_type _pid;
  ankerl::unordered_dense::map<std::string, _DBSlot> _dbs;

  _MemoryMapFile(const char* path, size_t map_size = 2 * G,
                 size_t pool_threads = SIZE_MAX)
      : PoolMixin(_lazy_pool) {
#ifndef LEAVES_SINGLE_PROCESS
    _pid = current_pid();
#else
    _pid = 1;
#endif
    init_dbfile(path, map_size);
    if (pool_threads != SIZE_MAX) {
      size_t n = pool_threads;
      if (n == 0)
        n = std::max<size_t>(1, std::thread::hardware_concurrency() / 2);
      this->start_pool(n);
    }
  }

  ~_MemoryMapFile() {
    _dbs.clear();       // destroy DBs first (cancels any scheduled jobs)
    this->stop_pool();  // stop worker threads before unmapping
    if constexpr (MAX_PROCESSES > 1) remove_pid();
    if (_memory) {
      _memory->clean_close = 1;
      _region.flush(0, _memory->file_size, false);
    }
  }

  const char* filename() const { return _file.get_name(); }

  Mutex& file_lock() { return _memory->file_lock; }

  size_t calc_header_size() const { return 4 * K; }

  size_t file_size() const { return _memory->file_size; }

  uint32_t sanitize_generation() { return _memory->sanitize_generation; }

  void init_dbfile(const char* path, size_t map_size) {
    if (!std::filesystem::is_regular_file(path)) {
      std::ofstream fhead(path, std::ios::out | std::ios::binary);
      fhead.put('l');
      fhead.close();
      uint64_t fsize =
          AREA_SIZE;  // reserve first area for header + overflow dir pages
      std::filesystem::resize_file(path, fsize);
      _file = file_mapping(path, read_write);
      _region = mapped_region(_file, read_write, 0, map_size);
      _memory = new (_region.get_address()) FileHeader();
      _memory->file_size = fsize;
      _region.flush();
    } else {
      std::ifstream fin(path);
      char signature[sizeof(MMAP_SIGNATURE)];
      fin.read(signature, sizeof(signature));
      if (strcmp(signature, MMAP_SIGNATURE)) throw TypeMismatch();

      _file = file_mapping(path, read_write);
      _region = mapped_region(_file, read_write, 0, map_size);
      _memory = (FileHeader*)_region.get_address();
      if (_memory->max_processes != MAX_PROCESSES)
        throw WrongValue("max_processes does not match.");
    }

    assert(((uint64_t)_memory & 7) == 0);
    sanitize();
  }

  void remove_pid() {
    std::scoped_lock flock_guard(file_lock());
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

  void recover_areas() {
    char* base = (char*)_memory;
    _recover_areas<_DBHeader<MemoryMapFile>, AREA_SIZE>(
        _memory->area_pool,
        [this](auto fn) {
          _for_each_db_entry([&](DBEntry& e) {
            if (e.offset) fn(e.offset);
            return true;
          });
        },
        _memory->file_size, padding(calc_header_size(), AREA_SIZE),
        [base](uint64_t pos, void* buf, size_t size) {
          memcpy(buf, base + pos, size);
        },
        [base](uint64_t pos, const void* buf, size_t size) {
          memcpy(base + pos, buf, size);
        });
  }

  void sanitize() {
    // Coordinate sanitization across processes with an OS file lock that is
    // automatically released if a process crashes.
#ifdef LEAVES_SINGLE_PROCESS
    std::scoped_lock flock_guard(file_lock());
#else
    boost::interprocess::file_lock flock(filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);
#endif

    if (sanitize_processes()) {
      new (&_memory->file_lock) Mutex();
      // Only rebuild the free-area pool if the previous close was NOT clean
      // (i.e. crash recovery). On a clean reopen the persisted pool state
      // is authoritative and rebuilding would lose ownership of any areas
      // not registered in the storage DB directory (e.g. confluence tributary
      // slots).
      if (!_memory->clean_close) recover_areas();
      _memory->clean_close = 0;  // now dirty until next clean close
      ++_memory->sanitize_generation;
      _memory->last_cursor_id.store(0);
    }
    // Register our pid inside the same critical section so concurrent openers
    // are serialized: a later opener observes our pid and is NOT a first
    // opener.
    if constexpr (MAX_PROCESSES > 1) {
      bool placed = false;
      for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!_memory->processes[i]) {
          _memory->processes[i] = _pid;
          placed = true;
          break;
        }
      }
      if (!placed) throw NoProcess();
    }
    if (std::filesystem::file_size(filename()) != _memory->file_size)
      std::filesystem::resize_file(filename(), _memory->file_size);

    assert(_region.get_size() >= _memory->file_size);
  }

  bool sanitize_processes() {
#ifndef LEAVES_SINGLE_PROCESS
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
#else
    // Single-process mode: clear all process slots and treat as first opener
    for (int i = 0; i < MAX_PROCESSES; i++) _memory->processes[i] = 0;
    return true;
#endif
  }

  // Resolve offset - handles both absolute and relative offsets uniformly
  page_ptr resolve(const offset_t* offset_ptr, Access access = READ) const {
    char* p;

    if (offset_ptr->is_relative()) {
      // Relative: calculate address relative to where offset_t is stored
      p = offset_ptr->resolve<char>();
    } else {
      // Absolute: offset from _memory base
      assert((uint64_t)*offset_ptr < _memory->file_size &&
             "absolute offset out of storage bounds");
      p = (char*)_memory + (uint64_t)*offset_ptr;
    }

    assert(p >= (char*)_memory && p < (char*)_memory + _memory->file_size &&
           "resolved pointer outside storage file");

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
    // caller (_db.hpp) already holds file_lock()

    // Geometric growth: grow by at least 25% of current size or 10*AREA_SIZE
    constexpr uint64_t MIN_GROWTH = 10 * AREA_SIZE;
    uint64_t geometric_growth = _memory->file_size / 4;  // 25% growth
    uint64_t total_growth = std::max({size, MIN_GROWTH, geometric_growth});
    total_growth = padding(total_growth, AREA_SIZE);

    if (_memory->file_size + total_growth > _region.get_size()) {
      throw StorageFull();
    }

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
    return result;
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
    _for_each_db_entry([&](DBEntry& entry) {
      if (entry.offset) result.push_back(entry.name);
      return true;
    });
  }

  // Open or create a DB of the given type. Additional args are forwarded
  // to the DB constructor (e.g. hash_threads for _ReplicationDB).
  template <template <typename> class DBClass = _DB, typename... Args>
  DBClass<MemoryMapFile>* open(const char* name, Args&&... args) {
    using DB = DBClass<MemoryMapFile>;
    if (strlen(name) >= sizeof(DBEntry::name)) throw KeyTooBig();

    std::scoped_lock flock_guard(file_lock());

    // 1. Check cache
    auto it = _dbs.find(name);
    if (it != _dbs.end()) {
      if (it->second.type_id != DB::DB_TYPE_ID) throw TypeMismatch();
      return static_cast<DB*>(it->second.db);
    }

    // 2. Scan directory for existing entry
    DBEntry* free_slot = nullptr;
    DBEntry* found = nullptr;
    _for_each_db_entry([&](DBEntry& entry) {
      assert(!found);
      if (entry.offset) {
        if (!strcmp(entry.name, name)) {
          found = &entry;
          return false;  // stop iteration
        }
      } else if (!free_slot) {
        free_slot = &entry;
      }
      return true;
    });

    if (found) {
      // Verify type before constructing
      auto* base_header = reinterpret_cast<_DBHeader<MemoryMapFile>*>(
          (char*)_memory + (uint64_t)found->offset);
      if (base_header->db_type_id != DB::DB_TYPE_ID) throw TypeMismatch();

      auto* db = new DB(_self(), found->offset, std::string_view(name),
                        std::forward<Args>(args)...);
      // Sanitize if this DB hasn't been sanitized for the current generation
      if (db->_header->sanitize_generation != _memory->sanitize_generation) {
        db->sanitize();
        db->_header->sanitize_generation = _memory->sanitize_generation;
      }
      _dbs[name] = _DBSlot::make(db);
      return db;
    }

    // 3. Create new — find or allocate a slot
    if (!free_slot) {
      uint16_t cap = _first_page_capacity();
      uint16_t hwm = std::min(_memory->db_entry_count, cap);
      if (hwm < cap) {
        free_slot = &_memory->dbs[hwm];
        _memory->db_entry_count = hwm + 1;
      } else {
        free_slot = _alloc_overflow_slot();
        if (!free_slot) throw LeavesException();
      }
    }

    std::strncpy(free_slot->name, name, sizeof(DBEntry::name) - 1);
    free_slot->name[sizeof(DBEntry::name) - 1] = '\0';
    auto* db = new DB(_self(), &free_slot->offset, std::string_view(name),
                      std::forward<Args>(args)...);
    db->_header->sanitize_generation = _memory->sanitize_generation;
    _dbs[name] = _DBSlot::make(db);
    return db;
  }

  template <template <typename> class DBClass = _DB>
  void remove(const char* name) {
    using DB = DBClass<MemoryMapFile>;
    std::scoped_lock flock_guard(file_lock());

    auto it = _dbs.find(name);
    if (it != _dbs.end()) {
      if (it->second.type_id != DB::DB_TYPE_ID) throw TypeMismatch();
      if (it->second.db && static_cast<DB*>(it->second.db)->is_active())
        throw TransactionActive();
    }

    bool found = false;
    _for_each_db_entry([&](DBEntry& entry) {
      assert(!found);
      if (entry.offset && !strcmp(entry.name, name)) {
        // Return areas via cached slot or typed DB
        auto dit = _dbs.find(name);
        if (dit != _dbs.end() && dit->second.db) {
          static_cast<DB*>(dit->second.db)->return_areas();
        } else {
          auto* base_header = reinterpret_cast<_DBHeader<MemoryMapFile>*>(
              (char*)_memory + (uint64_t)entry.offset);
          if (base_header->db_type_id != DB::DB_TYPE_ID) throw TypeMismatch();
          DB tmp(_self(), entry.offset, std::string_view(name));
          tmp.return_areas();
        }
        entry.offset = 0;
        found = true;
        return false;  // stop iteration
      }
      return true;
    });
    if (!found) throw WrongValue("database does not exist.");
    _dbs.erase(name);
    flush();
  }

  // Directory page helpers (mmap: all data is memory-mapped, pointers stable)
  uint16_t _first_page_capacity() const {
    return _DBDirectoryPage::capacity_for(4 * K - sizeof(FileHeader));
  }

  static constexpr uint16_t _overflow_page_capacity() {
    return _DBDirectoryPage::capacity_for(4 * K -
                                          offsetof(_DBDirectoryPage, entries));
  }

  template <typename Fn>
  void _for_each_db_entry(Fn fn) {
    uint16_t cap = _first_page_capacity();
    uint16_t count = std::min(_memory->db_entry_count, cap);
    for (uint16_t i = 0; i < count; i++) {
      if (!fn(_memory->dbs[i])) return;
    }

    offset_t next = _memory->db_next_page;
    while (next) {
      auto* page =
          reinterpret_cast<_DBDirectoryPage*>((char*)_memory + (uint64_t)next);
      uint16_t pcap = _overflow_page_capacity();
      uint16_t pcount = std::min(page->count, pcap);
      for (uint16_t i = 0; i < pcount; i++) {
        if (!fn(page->entries[i])) break;
      }
      next = page->next;
    }
  }

  // Allocate a slot in an overflow page. Returns stable pointer (mmap).
  // Overflow pages live within the reserved first AREA_SIZE block (after the
  // 4K file header), so they never conflict with the area pool.
  DBEntry* _alloc_overflow_slot() {
    offset_t next = _memory->db_next_page;
    uint64_t last_page_offset = 0;
    while (next) {
      auto* page =
          reinterpret_cast<_DBDirectoryPage*>((char*)_memory + (uint64_t)next);
      uint16_t pcap = _overflow_page_capacity();
      for (uint16_t i = 0; i < page->count; i++) {
        if (!page->entries[i].offset) return &page->entries[i];
      }
      if (page->count < pcap) {
        DBEntry* slot = &page->entries[page->count];
        page->count++;
        return slot;
      }
      last_page_offset = (uint64_t)next;
      next = page->next;
    }

    uint64_t new_off = last_page_offset ? last_page_offset + 4 * K : 4 * K;
    if (new_off + 4 * K > AREA_SIZE) return nullptr;

    auto* page = reinterpret_cast<_DBDirectoryPage*>((char*)_memory + new_off);
    page->init(4 * K - offsetof(_DBDirectoryPage, entries));
    page->count = 1;

    if (last_page_offset) {
      auto* prev = reinterpret_cast<_DBDirectoryPage*>((char*)_memory +
                                                       last_page_offset);
      prev->next = new_off;
    } else {
      _memory->db_next_page = new_off;
    }
    return &page->entries[0];
  }
};

}  // namespace leaves

#endif  // _LEAVES__IMMAP_HPP