#ifndef _LEAVES_FSTORE_HPP
#define _LEAVES_FSTORE_HPP

#include <memory>

#include "db.hpp"
#include "intern/_fstore.hpp"

namespace leaves {

class FileStorage {
 public:
  typedef TDB<_FileStore> DB;
  typedef std::shared_ptr<_FileStore> storage_ptr;

  FileStorage(const char* path, uint16_t db_count = 48, size_t cache_capacity = 500 * M)
      : _storage(std::make_shared<_FileStore>(path, db_count, cache_capacity)) {}

  DB operator[](const char* name) { return DB(_storage, name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  void debug_reset() { _storage->debug_reset(); }
  void debug_check_cache() const { _storage->debug_check_cache(); }

 private:
  storage_ptr _storage;
};


}  // namespace leaves

#endif  // _LEAVES_FSTORE_HPP