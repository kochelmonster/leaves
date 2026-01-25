#ifndef _LEAVES_LSM_MMAP_HPP
#define _LEAVES_LSM_MMAP_HPP

#include "../lsm/_lsm.hpp"
#include "../util/_threadpool.hpp"
#include "_mmap.hpp"

namespace leaves {

/**
 * @brief Memory-mapped file storage with thread pool for LSM operations
 *
 * Extends _MemoryMapFile with a shared thread pool that LSM databases
 * can use for background merge operations.
 *
 * Use this instead of _MemoryMapFile when you need LSM databases with
 * background merging. For standard _DB usage without LSM, use _MemoryMapFile
 * directly to avoid the thread pool overhead.
 */
template <typename Traits_, template <typename> class DB_ = _DB>
struct _LSMMemoryMapFile
    : public _MemoryMapFile<Traits_, DB_>,
      public _ThreadPoolMixin<_LSMMemoryMapFile<Traits_, DB_>> {
  using Base = _MemoryMapFile<Traits_, DB_>;
  using ThreadPool = _ThreadPoolMixin<_LSMMemoryMapFile<Traits_, DB_>>;
  using LSMMemoryMapFile = _LSMMemoryMapFile<Traits_, DB_>;

  typedef _LSMDB<LSMMemoryMapFile, _Transaction<Traits_>,
                 _DBHeader<LSMMemoryMapFile>>
      LSMDB;
  typedef std::unique_ptr<LSMDB> _lsmdb_ptr;

  std::vector<_lsmdb_ptr> _lsm_dbs;

  _LSMMemoryMapFile(const char* path, size_t map_size = 2 * G,
                    uint16_t db_count = 48, size_t pool_threads = 0)
      : Base(path, map_size, db_count), ThreadPool(pool_threads) {
    _lsm_dbs.resize(db_count);
  }

  ~_LSMMemoryMapFile() {
    // Stop thread pool first to ensure all tasks complete
    // before base class destructor runs
    this->stop_pool();
  }

  // Non-copyable, non-movable (inherited from both bases, but be explicit)
  _LSMMemoryMapFile(const _LSMMemoryMapFile&) = delete;
  _LSMMemoryMapFile& operator=(const _LSMMemoryMapFile&) = delete;
  _LSMMemoryMapFile(_LSMMemoryMapFile&&) = delete;
  _LSMMemoryMapFile& operator=(_LSMMemoryMapFile&&) = delete;

  LSMDB* make_lsm(const char* name) {
    using DBEntry = typename Base::DBEntry;
    if (strlen(name) >= sizeof(DBEntry::name)) throw KeyTooBig();

    boost::interprocess::file_lock flock(this->filename());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock>
        flock_guard(flock);
    int free = -1;
    for (uint16_t i = 0; i < this->_memory->db_count; i++) {
      if (this->_memory->dbs[i].offset) {
        if (!strcmp(this->_memory->dbs[i].name, name)) {
          if (!_lsm_dbs[i]) {
            _lsm_dbs[i] =
                std::make_unique<LSMDB>(*this, this->_memory->dbs[i].offset, i);
            return _lsm_dbs[i].get();
          }
          return _lsm_dbs[i].get();
        }
      } else if (free < 0)
        free = i;
    }

    if (free < 0) throw LeavesException();
    strcpy(this->_memory->dbs[free].name, name);
    _lsm_dbs[free] =
        std::make_unique<LSMDB>(*this, &this->_memory->dbs[free].offset, free);
    return _lsm_dbs[free].get();
  }
};

}  // namespace leaves

#endif  // _LEAVES_LSM_MMAP_HPP
