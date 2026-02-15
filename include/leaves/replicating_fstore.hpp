#ifndef _LEAVES_REPLICATING_FSTORE_HPP
#define _LEAVES_REPLICATING_FSTORE_HPP

#include <blake3.h>
#include <memory>

#include "db.hpp"
#include "intern/replication/_replication_db.hpp"
#include "intern/storage/_fstore.hpp"

namespace leaves {

// Replication-enabled traits mixin for file storage
// Inherits from base traits and enables 32-byte hash storage
template <typename BaseTraits>
struct _ReplicationTraits : public BaseTraits {
  // Enable 32-byte hash storage in nodes
  typedef uint8_t hash_t[HASH_SIZE];
  
  // Use Blake3Hasher for replication
  typedef Blake3Hasher ReplicationHasher;
};

// Replication-enabled file storage traits
typedef _ReplicationTraits<_StoreTraits> _ReplicatingStoreTraits;

// Forward-declare so Self_ can refer to it
struct _ReplicationCacheStore;

// Replication-enabled CacheStore: passes itself as Self_ so that
// DB::_storage is typed as _ReplicationCacheStore& — giving direct
// access to schedule_after() / cancel_job() / wait_all().
struct _ReplicationCacheStore
    : public _CacheStore<_ReplicatingStoreTraits, _FileOperations,
                         _ReplicationDB, _ReplicationCacheStore> {
  using Base = _CacheStore<_ReplicatingStoreTraits, _FileOperations,
                           _ReplicationDB, _ReplicationCacheStore>;
  using DB = typename Base::DB;
  using Base::Base;  // inherit constructors

  // Override make() to start purge on newly-created DBs
  DB* make(const char* name) {
    DB* db = Base::make(name);
    if (!db->_purge_job_id && !db->_purge_cancelled.load())
      db->start_purge();
    return db;
  }

  DB* operator[](const char* name) { return make(name); }
};

class ReplicatingFileStorage
    : public std::enable_shared_from_this<ReplicatingFileStorage> {
 public:
  typedef _ReplicationCacheStore StorageImpl;
  typedef TDB<ReplicatingFileStorage> DB;
  typedef std::shared_ptr<ReplicatingFileStorage> storage_ptr;

  ReplicatingFileStorage(const char* path, uint16_t db_count = 48,
                         size_t cache_capacity = 500 * M)
      : _storage(
            std::make_unique<StorageImpl>(path, db_count, cache_capacity)) {}

  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t cache_capacity = 500 * M,
                            uint16_t db_count = 48) {
    return std::make_shared<ReplicatingFileStorage>(path, db_count,
                                                    cache_capacity);
  }

  void debug_reset() { _storage->debug_reset(); }
  void debug_check_cache() const { _storage->debug_check_cache(); }

 private:
  friend class TDB<ReplicatingFileStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_REPLICATING_FSTORE_HPP
