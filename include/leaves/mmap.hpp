#ifndef _LEAVES_MMAP_HPP
#define _LEAVES_MMAP_HPP

#include <memory>
#include <type_traits>
#include <utility>
#include <string_view>

#include "db.hpp"
#include "intern/storage/_mmap.hpp"

namespace leaves {

typedef _MemoryMapTraits MapTraits;

template <typename Traits = MapTraits>
class MapStorage_ : public std::enable_shared_from_this<MapStorage_<Traits>> {
 public:
  using SelfStorage = MapStorage_<Traits>;
  typedef _MemoryMapFile<Traits> StorageImpl;
  typedef std::shared_ptr<MapStorage_> storage_ptr;
  using DB = TDB<MapStorage_, _DB>;
  class ReplicationDB;
  class ConfluenceDB;
  class ConfluenceReplicationDB;

  template <typename T>
  struct _is_allowed_dbclass
      : std::bool_constant<std::is_same_v<T, DB> ||
                           std::is_same_v<T, ReplicationDB> ||
                           std::is_same_v<T, ConfluenceDB> ||
                 std::is_same_v<T, ConfluenceReplicationDB>> {};

  // Direct constructor variant; prefer create() for shared-pointer ownership.
  // map_size is the virtual-address reservation limit.
  // On mobile (iOS/Android), use a smaller value (e.g. 256*M) to avoid
  // jetsam/OOM kills.
  MapStorage_(const char* path, size_t map_size = 4 * G)
      : _storage(std::make_unique<StorageImpl>(path, map_size)) {}

  // Opens or creates a named database.
  // DBClass selects the backend and args are forwarded to that DB class.
  template <typename DBClass = DB, typename... Args>
  auto open(std::string_view name, Args&&... args) {
    static_assert(
        _is_allowed_dbclass<DBClass>::value,
        "MapStorage::open only accepts MapStorage facade types: "
        "MapStorage::DB, MapStorage::ReplicationDB, MapStorage::ConfluenceDB, "
        "MapStorage::ConfluenceReplicationDB.");
    using Wrapper = typename DBClass::template DBWrapper<SelfStorage>;
    return Wrapper(this->shared_from_this(), name, std::forward<Args>(args)...);
  }

  // Removes the named database from storage.
  template <typename DBClass = DB>
  void remove(std::string_view name) {
    static_assert(
        _is_allowed_dbclass<DBClass>::value,
        "MapStorage::remove only accepts MapStorage facade types: "
        "MapStorage::DB, MapStorage::ReplicationDB, MapStorage::ConfluenceDB, "
        "MapStorage::ConfluenceReplicationDB.");
    _storage->template remove<DBClass::template DBImpl>(name);
  }

  // Appends all known database names to result.
  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  // Returns mutable access to the internal thread pool.
  typename StorageImpl::PoolMixin& thread_pool() { return *_storage; }

  // Returns the opened storage filename.
  Slice filename() const { return Slice(_storage->filename()); }

  // Returns the current on-disk file size in bytes.
  size_t file_size() const { return _storage->file_size(); }

  // Creates and initializes storage backed by path.
  // map_size is the virtual-address reservation limit.
  static storage_ptr create(const char* path, size_t map_size = 4 * G) {
    return std::make_shared<MapStorage_>(path, map_size);
  }

 private:
  template <typename, template <typename> class>
  friend class TDB;
  std::unique_ptr<StorageImpl> _storage;
};

using MapStorage = MapStorage_<>;

}  // namespace leaves

#endif  // _LEAVES_MMAP_HPP