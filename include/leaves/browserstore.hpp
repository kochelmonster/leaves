#ifndef _LEAVES_BROWSERSTORE_HPP
#define _LEAVES_BROWSERSTORE_HPP

#ifdef __EMSCRIPTEN__

#include <memory>

#include "db.hpp"
#include "intern/storage/_browserstore.hpp"

#include <emscripten.h>
#include <emscripten/eventloop.h>

namespace leaves {

class BrowserStorage
    : public std::enable_shared_from_this<BrowserStorage> {
 public:
  typedef _BrowserStore StorageImpl;
  typedef std::shared_ptr<BrowserStorage> storage_ptr;

  BrowserStorage(const char* db_name, size_t capacity = 100 * M)
      : _storage(std::make_unique<StorageImpl>(db_name, capacity)) {}

  template <template <typename> class DBClass = _DB, typename... Args>
  TDB<BrowserStorage, DBClass> open(const char* name, Args&&... args) {
    return TDB<BrowserStorage, DBClass>(
        this->shared_from_this(), name, std::forward<Args>(args)...);
  }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  static storage_ptr create(const char* db_name,
                            size_t capacity = 100 * M) {
    return std::make_shared<BrowserStorage>(db_name, capacity);
  }

 private:
  template <typename, template <typename> class> friend class TDB;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#else  // !__EMSCRIPTEN__

namespace leaves {

class BrowserStorage {
 public:
  BrowserStorage(...) {
    static_assert(sizeof(BrowserStorage) == 0,
                  "BrowserStorage requires Emscripten compilation");
  }
};

}  // namespace leaves

#endif  // __EMSCRIPTEN__

#endif  // _LEAVES_BROWSERSTORE_HPP
