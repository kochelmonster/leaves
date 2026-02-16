#ifndef _LEAVES_REPLICATING_MMAP_HPP
#define _LEAVES_REPLICATING_MMAP_HPP

#include <blake3.h>
#include <memory>

#include "db.hpp"
#include "intern/replication/_replication_db.hpp"
#include "intern/storage/_mmap.hpp"
#include "intern/util/_threadpool.hpp"

namespace leaves {

// Replication-enabled memory-mapped storage traits
typedef _ReplicationTraits<_MemoryMapTraits> _ReplicatingMemoryMapTraits;

// Forward-declare so Self_ can refer to it
template <typename Traits_>
struct _ReplicationMemoryMapFile;

// Replication-enabled memory-mapped storage: adds a thread pool for
// background purge.  Passes itself as Self_ to _MemoryMapFile so that
// DB::_storage is typed as _ReplicationMemoryMapFile& — giving direct
// access to schedule_after() / cancel_job() / wait_all().
template <typename Traits_>
struct _ReplicationMemoryMapFile
    : public _MemoryMapFile<Traits_, _ReplicationDB,
                            _ReplicationMemoryMapFile<Traits_>>,
      public _ThreadPoolMixin<_ReplicationMemoryMapFile<Traits_>> {
  using Base =
      _MemoryMapFile<Traits_, _ReplicationDB,
                     _ReplicationMemoryMapFile<Traits_>>;
  using PoolMixin = _ThreadPoolMixin<_ReplicationMemoryMapFile<Traits_>>;
  using DB = typename Base::DB;

  _ReplicationMemoryMapFile(const char* path, size_t map_size = 2 * G,
                            uint16_t db_count = 48)
      : Base(path, map_size, db_count), PoolMixin(1) {}

  ~_ReplicationMemoryMapFile() {
    this->_dbs.clear();  // Destroy DBs first (cancels purge jobs)
    this->stop_pool();   // Then stop the thread pool
  }

  // Override make() to start purge on newly-created DBs
  DB* make(const char* name) {
    DB* db = Base::make(name);
    if (!db->_purge_job_id && !db->_purge_cancelled.load())
      db->start_purge();
    return db;
  }

  DB* operator[](const char* name) { return make(name); }
};

class ReplicatingMapStorage
    : public std::enable_shared_from_this<ReplicatingMapStorage> {
 public:
  typedef _ReplicationMemoryMapFile<_ReplicatingMemoryMapTraits> StorageImpl;
  typedef typename StorageImpl::DB DBImpl;
  typedef TDB<ReplicatingMapStorage> DB;
  typedef std::shared_ptr<ReplicatingMapStorage> storage_ptr;

  ReplicatingMapStorage(const char* path, size_t map_size = 4 * G,
                        uint16_t db_count = 48)
      : _storage(std::make_unique<StorageImpl>(path, map_size, db_count)) {}

  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t map_size = 4 * G,
                            uint16_t db_count = 48) {
    return std::make_shared<ReplicatingMapStorage>(path, map_size, db_count);
  }

 private:
  friend class TDB<ReplicatingMapStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_REPLICATING_MMAP_HPP
