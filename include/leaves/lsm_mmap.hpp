#ifndef _LEAVES_LSM_MMAP_HPP
#define _LEAVES_LSM_MMAP_HPP

#include <memory>

#include "db.hpp"
#include "intern/_lsm_db.hpp"
#include "intern/_lsm_cursor.hpp"
#include "intern/_mmap.hpp"

namespace leaves {

class LSMMapStorage {
 public:
  typedef _MemoryMapFile<_MemoryMapTraits, _LSMDB> StorageImpl;
  typedef TDB<StorageImpl> DB;
  typedef std::shared_ptr<StorageImpl> storage_ptr;

  LSMMapStorage(const char* path, size_t map_size = 4 * G, uint16_t db_count = 48)
      : _storage(std::make_shared<StorageImpl>(path, map_size, db_count)) {}

  DB operator[](const char* name) { return DB(_storage, name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

 private:
  storage_ptr _storage;
};

}  // namespace leaves

#endif  // _LEAVES_LSM_MMAP_HPP
