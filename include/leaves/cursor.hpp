#ifndef _LEAVES_CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include <memory>

#include "intern/_cursor.hpp"
#include "intern/_util.hpp"

namespace leaves {

template <typename CursorImpl, typename storage_ptr>
class TCursor {
 public:
  typedef std::shared_ptr<CursorImpl> cursor_shared_ptr;

  template <typename DB>
  TCursor(storage_ptr storage, DB db)
      : _storage(storage), _cursor(std::make_shared<CursorImpl>(db)) {}
  TCursor() = default;

  TCursor(TCursor&& other) noexcept
    : _storage(std::move(other._storage), _cursor(std::move(other._cursor))) {}

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

  bool start_transaction(bool non_blocking = false) {
    return _cursor->start_transaction(non_blocking);
  }

  void prepare_commit(bool sync = false) { _cursor->prepare_commit(sync); }

  void commit(bool sync = false) { _cursor->commit(sync); }

  void rollback() { _cursor->rollback(); }

 private:
  storage_ptr _storage;
  cursor_shared_ptr _cursor;
};
}  // namespace leaves

#endif  // _LEAVES_CURSOR_HPP