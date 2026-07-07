#ifndef _LEAVES_FSTORE_HPP
#define _LEAVES_FSTORE_HPP

#include <memory>

#include "db.hpp"
#include "intern/storage/_fstore.hpp"

namespace leaves {

typedef _StoreTraits FileTraits;

template <typename Traits = FileTraits>
class FileStorage_ : public std::enable_shared_from_this<FileStorage_<Traits>> {
 public:
  typedef _FileStore<Traits> StorageImpl;
  typedef std::shared_ptr<FileStorage_> storage_ptr;

  FileStorage_(const char* path,
              size_t cache_capacity = 500 * M)
      : _storage(
            std::make_unique<StorageImpl>(path, cache_capacity)) {}

  template <template <typename> class DBClass = _DB, typename... Args>
  TDB<FileStorage_, DBClass> open(const char* name, Args&&... args) {
    return TDB<FileStorage_, DBClass>(
        this->shared_from_this(), name, std::forward<Args>(args)...);
  }

  template <template <typename> class DBClass = _DB>
  void remove(const char* name) { _storage->template remove<DBClass>(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  ThreadPool<typename StorageImpl::base_t>& thread_pool() { return *_storage; }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t cache_capacity = 500 * M) {
    return std::make_shared<FileStorage_>(path, cache_capacity);
  }

  void debug_reset() { _storage->debug_reset(); }
  void debug_check_cache() const { _storage->debug_check_cache(); }

 private:
  template <typename, template <typename> class> friend class TDB;
  std::unique_ptr<StorageImpl> _storage;
};

using FileStorage = FileStorage_<>;

}  // namespace leaves

#endif  // _LEAVES_FSTORE_HPP