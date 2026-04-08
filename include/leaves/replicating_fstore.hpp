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

  size_t _hash_threads = 4;

  _ReplicationCacheStore(const char* path,
                         size_t capacity = 500 * M, size_t pool_threads = 0,
                         size_t hash_threads = 4)
      : Base(capacity, pool_threads), _hash_threads(hash_threads) {
    init_dbfile(path);
  }

  ~_ReplicationCacheStore() {
    this->_dbs.clear();             // Cancel all purge jobs
    this->stop_pool();              // Join worker threads
    this->destroy();                // Flush and close
    delete[] (char*)this->_header;  // Free header last
  }

  void init_dbfile(const char* path) {
    size_t header_size = 4 * K;
    char* buffer = new char[header_size];
    if (!std::filesystem::is_regular_file(path)) {
      this->open(path);
      this->_header = new (buffer) FileHeader();
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
    }
    assert(((uint64_t)this->_header & 7) == 0);
    sanitize();
  }

  void recover_areas() {
    auto* self = this;
    _recover_areas<typename DB::Header, Base::AREA_SIZE>(
        this->_header->area_pool,
        [self](auto fn) { self->_for_each_db_entry([&](auto& e) { if (e.offset) fn(e.offset); }); },
        this->_header->file_size,
        leaves::padding(this->calc_header_size(), Base::AREA_SIZE),
        [self](uint64_t pos, void* buf, size_t size) {
          self->read(pos, buf, size);
        },
        [self](uint64_t pos, const void* buf, size_t size) {
          self->write(pos, buf, size);
        });
  }

  void sanitize() {
    this->recover_areas();
    this->_for_each_db_entry([&](auto& entry) {
      if (entry.offset) {
        DB(*this, entry.offset, std::string_view(entry.name)).sanitize();
      }
    });
    if (std::filesystem::file_size(this->filename()) !=
        this->_header->file_size)
      std::filesystem::resize_file(this->filename(), this->_header->file_size);
  }

  // Override make() to start purge on newly-created DBs
  DB* make(const char* name) {
    DB* db = Base::make(name);
    db->_hash_threads = _hash_threads;
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

  ReplicatingFileStorage(const char* path,
                         size_t cache_capacity = 500 * M,
                         size_t pool_threads = 0, size_t hash_threads = 4)
      : _storage(std::make_unique<StorageImpl>(path, cache_capacity,
                                               pool_threads, hash_threads)) {}

  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t cache_capacity = 500 * M,
                            size_t pool_threads = 0,
                            size_t hash_threads = 4) {
    return std::make_shared<ReplicatingFileStorage>(
        path, cache_capacity, pool_threads, hash_threads);
  }

  void debug_reset() { _storage->debug_reset(); }
  void debug_check_cache() const { _storage->debug_check_cache(); }

 private:
  friend class TDB<ReplicatingFileStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_REPLICATING_FSTORE_HPP
