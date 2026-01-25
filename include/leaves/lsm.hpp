#ifndef _LEAVES_LSM_PUBLIC_HPP
#define _LEAVES_LSM_PUBLIC_HPP

#include <memory>

#include "mmap.hpp"
#include "intern/storage/_lsm_mmap.hpp"

namespace leaves {

/**
 * @brief Public LSM cursor wrapper
 */
template <typename Storage>
class TLSMCursor {
 public:
  typedef typename Storage::storage_ptr storage_ptr;
  typedef typename Storage::StorageImpl StorageImpl;
  typedef typename StorageImpl::LSMDB LSMDBImpl;
  typedef typename LSMDBImpl::cursor_ptr cursor_ptr;

  TLSMCursor(storage_ptr storage, LSMDBImpl* db)
      : _storage(storage), _cursor(db->create_cursor()) {}
  TLSMCursor() = default;

  TLSMCursor(TLSMCursor&& other) noexcept
      : _storage(std::move(other._storage)),
        _cursor(std::move(other._cursor)) {}

  ~TLSMCursor() {
    _cursor.reset();
    _storage.reset();
  }

  TLSMCursor& operator=(TLSMCursor&& other) noexcept {
    if (this != &other) {
      _cursor = std::move(other._cursor);
      _storage = std::move(other._storage);
    }
    return *this;
  }

  bool is_valid() const { return _cursor->is_valid(); }

  void find(const Slice& key) { _cursor->find(key); }

  void first() { _cursor->first(); }

  void last() { _cursor->last(); }

  void next() { _cursor->next(); }

  void prev() { _cursor->prev(); }

  void value(const Slice& value) { _cursor->value(value); }

  Slice value() const { return _cursor->value(); }

  Slice key() const { return _cursor->key(); }

  void remove() { _cursor->remove(); }

  bool start_transaction(bool non_blocking = false) {
    return _cursor->start_transaction(non_blocking);
  }

  tid_t prepare_commit(bool sync = false) {
    return _cursor->prepare_commit(sync);
  }

  void commit(bool sync = false) { _cursor->commit(sync); }

  void rollback() { _cursor->rollback(); }

 private:
  storage_ptr _storage;
  cursor_ptr _cursor;
};

/**
 * @brief Public LSM database wrapper
 */
template <typename Storage>
class TLSMDB {
 public:
  typedef typename Storage::storage_ptr storage_ptr;
  typedef typename Storage::StorageImpl StorageImpl;
  typedef typename StorageImpl::LSMDB LSMDBImpl;
  typedef LSMDBImpl db_type;
  typedef TLSMCursor<Storage> Cursor;

  TLSMDB(storage_ptr storage, const char* name)
      : _storage(storage), _db(storage->_storage->make_lsm(name)) {}

  Cursor cursor() { return Cursor(_storage, _db); }

  Slice name() const { return _db->name(); }

  db_type* _internal() const { return _db; }

  void schedule_merge() { _db->schedule_merge(); }

  void do_merge() { _db->do_merge(); }

  bool should_merge() const { return _db->should_merge(); }

  void set_merge_threshold(uint64_t bytes) { _db->_merge_threshold = bytes; }

 private:
  storage_ptr _storage;
  LSMDBImpl* _db;
};

/**
 * @brief LSM-enabled Map Storage
 *
 * Extended MapStorage that supports both regular tries and LSM-structured
 * tries
 */
class LSMMapStorage : public std::enable_shared_from_this<LSMMapStorage> {
 public:
  typedef _LSMMemoryMapFile<_MemoryMapTraits> StorageImpl;
  typedef TLSMDB<LSMMapStorage> LSMDB;
  typedef TDB<LSMMapStorage> DB;
  typedef std::shared_ptr<LSMMapStorage> storage_ptr;

  LSMMapStorage(const char* path, size_t map_size = 4 * G,
                uint16_t db_count = 48, size_t pool_threads = 0)
      : _storage(std::make_unique<StorageImpl>(path, map_size, db_count,
                                               pool_threads)) {}

  // Create regular trie DB
  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  // Create LSM-structured DB
  LSMDB lsm(const char* name) { return LSMDB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t map_size = 4 * G,
                            uint16_t db_count = 48, size_t pool_threads = 0) {
    return std::make_shared<LSMMapStorage>(path, map_size, db_count,
                                           pool_threads);
  }

 private:
  friend class TDB<LSMMapStorage>;
  friend class TLSMDB<LSMMapStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_LSM_PUBLIC_HPP
