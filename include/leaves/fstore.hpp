#ifndef _LEAVES_FSTORE_HPP
#define _LEAVES_FSTORE_HPP

#include <memory>

#include "db.hpp"
#include "intern/storage/_fstore.hpp"

namespace leaves {

class FileStorage : public std::enable_shared_from_this<FileStorage> {
 public:
  typedef _FileStore StorageImpl;
  typedef TDB<FileStorage> DB;
  typedef std::shared_ptr<FileStorage> storage_ptr;

  FileStorage(const char* path, uint16_t db_count = 48,
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
    return std::make_shared<FileStorage>(path, db_count, cache_capacity);
  }

  void debug_reset() { _storage->debug_reset(); }
  void debug_check_cache() const { _storage->debug_check_cache(); }

 private:
  friend class TDB<FileStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_FSTORE_HPP