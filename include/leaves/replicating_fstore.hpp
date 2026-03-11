#ifndef _LEAVES_REPLICATING_FSTORE_HPP
#define _LEAVES_REPLICATING_FSTORE_HPP

#include <blake3.h>

#include <memory>

#include "db.hpp"
#include "intern/replication/_replication_db.hpp"
#include "intern/storage/_fstore.hpp"

namespace leaves {

// Replication-enabled file storage traits
typedef _StoreTraits _ReplicatingStoreTraits;

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
  using DBEntry = typename Base::DBEntry;
  using FileHeader = typename _FileOperations::FileHeader;

  _ReplicationCacheStore(const char* path, uint16_t db_count = 48,
                         size_t capacity = 500 * M, size_t pool_threads = 0)
      : Base(db_count, capacity, pool_threads) {
    init_dbfile(path, db_count);
  }

  ~_ReplicationCacheStore() {
    this->_dbs.clear();             // Cancel all purge jobs
    this->stop_pool();              // Join worker threads
    this->destroy();                // Flush and close
    delete[] (char*)this->_header;  // Free header last
  }

  void init_dbfile(const char* path, uint16_t db_count) {
    size_t header_size =
        leaves::padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
    char* buffer = new char[header_size];
    if (!std::filesystem::is_regular_file(path)) {
      this->open(path);
      this->_header = new (buffer) FileHeader(db_count);
      // Align initial file_size to AREA_SIZE so areas are AREA_SIZE-aligned
      this->_header->file_size = leaves::padding(header_size, Base::AREA_SIZE);
      this->resize(this->_header->file_size);
      this->write(0, buffer, header_size);
    } else {
      this->open(path);
      this->read(0, buffer, header_size);
      this->_header = (FileHeader*)buffer;
      if (strcmp(this->_header->signature, FSTORE_SIGNATURE))
        throw std::runtime_error("wrong filetype");
      if (this->_header->db_count != db_count)
        throw WrongValue("db_count may not be changed.");
    }
    assert(((uint64_t)this->_header & 7) == 0);
    sanitize();
  }

  void sanitize() {
    for (uint16_t i = 0; i < this->_header->db_count; i++) {
      if (this->_header->dbs[i].offset) {
        assert(!this->_dbs[i]);
        DB(*this, this->_header->dbs[i].offset, i).sanitize();
      }
    }
    if (std::filesystem::file_size(this->filename()) !=
        this->_header->file_size)
      std::filesystem::resize_file(this->filename(), this->_header->file_size);
  }

  // Override make() to start purge on newly-created DBs
  DB* make(const char* name) {
    DB* db = Base::make(name);
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
                         size_t cache_capacity = 500 * M,
                         size_t pool_threads = 0)
      : _storage(std::make_unique<StorageImpl>(path, db_count, cache_capacity,
                                               pool_threads)) {}

  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t cache_capacity = 500 * M,
                            uint16_t db_count = 48, size_t pool_threads = 0) {
    return std::make_shared<ReplicatingFileStorage>(
        path, db_count, cache_capacity, pool_threads);
  }

  void debug_reset() { _storage->debug_reset(); }
  void debug_check_cache() const { _storage->debug_check_cache(); }

 private:
  friend class TDB<ReplicatingFileStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_REPLICATING_FSTORE_HPP
