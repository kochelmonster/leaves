#ifndef _LEAVES_CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include <memory>

#include "intern/core/_util.hpp"
#include "intern/db/_cursor.hpp"
#include "intern/db/_db.hpp"

namespace leaves {

template <typename Storage, template <typename> class DBClass = _DB>
class TCursor {
 public:
  typedef typename Storage::storage_ptr storage_ptr;
  typedef typename Storage::StorageImpl StorageImpl;
  typedef DBClass<StorageImpl> DBImpl;
  typedef typename DBImpl::cursor_ptr cursor_ptr;

  TCursor(storage_ptr storage, DBImpl* db)
      : _storage(storage), _cursor(db->create_cursor()) {}
  TCursor() = default;

  TCursor(TCursor&& other) noexcept
      : _storage(std::move(other._storage)),
        _cursor(std::move(other._cursor)) {}

  ~TCursor() {
    _cursor.reset();
    _storage.reset();
  }

  TCursor& operator=(TCursor&& other) noexcept {
    if (this != &other) {
      _cursor = std::move(other._cursor);
      _storage = std::move(other._storage);
    }
    return *this;
  }

  tid_t txn_id() const { return _cursor->txn_id(); }

  // return true if the cursor is on a valid position
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

  void update() { _cursor->update(); }

  bool start_transaction(bool non_blocking = false, bool use_wal = false) {
    return _cursor->start_transaction(non_blocking, use_wal,
                                      TransactionOrigin::user);
  }

  tid_t prepare_commit(bool sync = false) {
    return _cursor->prepare_commit(sync);
  }

  void commit(bool sync = false) { _cursor->commit(sync); }

  void rollback() { _cursor->rollback(); }

  bool is_transaction_active() const {
    return _cursor && _cursor->is_transaction_active();
  }

  auto& aspect_context() { return _cursor->_aspect_context; }
  const auto& aspect_context() const { return _cursor->_aspect_context; }

 private:
  storage_ptr _storage;
  cursor_ptr _cursor;
};
}  // namespace leaves

#endif  // _LEAVES_CURSOR_HPP