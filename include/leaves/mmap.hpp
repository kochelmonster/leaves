#ifndef _LEAVES_MMAP_HPP
#define _LEAVES_MMAP_HPP

#include <memory>

#include "db.hpp"
#include "intern/storage/_mmap.hpp"

namespace leaves {

typedef _MemoryMapTraits MapTraits;

template <typename Traits = MapTraits>
class MapStorage_ : public std::enable_shared_from_this<MapStorage_<Traits>> {
 public:
  typedef _MemoryMapFile<Traits> StorageImpl;
  typedef TDB<MapStorage_> DB;
  typedef std::shared_ptr<MapStorage_> storage_ptr;

  // map_size: virtual address space reservation. On mobile (iOS/Android), use a
  // smaller value (e.g. 256*M) to avoid jetsam/OOM kills.
  MapStorage_(const char* path, size_t map_size = 4 * G, uint16_t db_count = 48)
      : _storage(std::make_unique<StorageImpl>(path, map_size, db_count)) {}

  DB operator[](const char* name) { return DB(this->shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t map_size = 4 * G,
                            uint16_t db_count = 48) {
    return std::make_shared<MapStorage_>(path, map_size, db_count);
  }

 private:
  friend class TDB<MapStorage_>;
  std::unique_ptr<StorageImpl> _storage;
};

using MapStorage = MapStorage_<>;




}  // namespace leaves

#endif  // _LEAVES_MMAP_HPP