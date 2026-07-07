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
  typedef std::shared_ptr<MapStorage_> storage_ptr;

  // map_size: virtual address space reservation. On mobile (iOS/Android), use a
  // smaller value (e.g. 256*M) to avoid jetsam/OOM kills.
  MapStorage_(const char* path, size_t map_size = 4 * G)
      : _storage(std::make_unique<StorageImpl>(path, map_size)) {}

  template <template <typename> class DBClass = _DB, typename... Args>
  TDB<MapStorage_, DBClass> open(const char* name, Args&&... args) {
    return TDB<MapStorage_, DBClass>(
        this->shared_from_this(), name, std::forward<Args>(args)...);
  }

  template <template <typename> class DBClass = _DB>
  void remove(const char* name) { _storage->template remove<DBClass>(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  typename StorageImpl::PoolMixin& thread_pool() { return *_storage; }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t map_size = 4 * G) {
    return std::make_shared<MapStorage_>(path, map_size);
  }

 private:
  template <typename, template <typename> class> friend class TDB;
  std::unique_ptr<StorageImpl> _storage;
};

using MapStorage = MapStorage_<>;

}  // namespace leaves

#endif  // _LEAVES_MMAP_HPP