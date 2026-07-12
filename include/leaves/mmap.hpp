#ifndef _LEAVES_MMAP_HPP
#define _LEAVES_MMAP_HPP

#include <memory>
#include <type_traits>
#include <utility>

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
  template <typename ConflictPolicy_>
  class ConfluenceDB_;
  class ConfluenceReplicationDB;
  template <typename ConflictPolicy_>
  class ConfluenceReplicationDB_;

  template <typename T>
  struct _is_confluence_db : std::false_type {};

  template <typename ConflictPolicy_>
  struct _is_confluence_db<ConfluenceDB_<ConflictPolicy_>> : std::true_type {};

  template <typename T>
  struct _is_confluence_replication_db : std::false_type {};

  template <typename ConflictPolicy_>
  struct _is_confluence_replication_db<
      ConfluenceReplicationDB_<ConflictPolicy_>> : std::true_type {};

  template <typename T>
  struct _is_allowed_dbclass
      : std::bool_constant<std::is_same_v<T, DB> ||
                           std::is_same_v<T, ReplicationDB> ||
                           std::is_same_v<T, ConfluenceDB> ||
                           std::is_same_v<T, ConfluenceReplicationDB> ||
                           _is_confluence_db<T>::value ||
                           _is_confluence_replication_db<T>::value> {};

  // map_size: virtual address space reservation. On mobile (iOS/Android), use a
  // smaller value (e.g. 256*M) to avoid jetsam/OOM kills.
  MapStorage_(const char* path, size_t map_size = 4 * G)
      : _storage(std::make_unique<StorageImpl>(path, map_size)) {}

  template <typename DBClass = DB, typename... Args>
  auto open(const char* name, Args&&... args) {
    static_assert(
        _is_allowed_dbclass<DBClass>::value,
        "MapStorage::open only accepts MapStorage facade types: "
        "MapStorage::DB, MapStorage::ReplicationDB, MapStorage::ConfluenceDB, "
        "MapStorage::ConfluenceDB_<Policy>, "
        "MapStorage::ConfluenceReplicationDB, "
        "MapStorage::ConfluenceReplicationDB_<Policy>.");
    using Wrapper = typename DBClass::template DBWrapper<SelfStorage>;
    return Wrapper(this->shared_from_this(), name, std::forward<Args>(args)...);
  }

  template <typename DBClass = DB>
  void remove(const char* name) {
    static_assert(
        _is_allowed_dbclass<DBClass>::value,
        "MapStorage::remove only accepts MapStorage facade types: "
        "MapStorage::DB, MapStorage::ReplicationDB, MapStorage::ConfluenceDB, "
        "MapStorage::ConfluenceDB_<Policy>, "
        "MapStorage::ConfluenceReplicationDB, "
        "MapStorage::ConfluenceReplicationDB_<Policy>.");
    _storage->template remove<DBClass::template DBImpl>(name);
  }

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
  template <typename, template <typename> class>
  friend class TDB;
  std::unique_ptr<StorageImpl> _storage;
};

using MapStorage = MapStorage_<>;

}  // namespace leaves

#endif  // _LEAVES_MMAP_HPP