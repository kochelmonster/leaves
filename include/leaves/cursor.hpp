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

  TCursor(const TCursor& other) = default;

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

  // Returns the transaction id associated with the cursor read snapshot.
  tid_t txn_id() const { return _cursor->txn_id(); }

  // Returns true if the cursor is currently positioned on an existing record.
  bool is_valid() const { return _cursor->is_valid(); }

  // Seeks to key or to the insertion position for that key.
  void find(const Slice& key) { _cursor->find(key); }

  // Moves to the smallest key.
  void first() { _cursor->first(); }

  // Moves to the largest key.
  void last() { _cursor->last(); }

  // Advances to the next entry.
  void next() { _cursor->next(); }

  // Moves to the previous entry.
  void prev() { _cursor->prev(); }

  // Reserves size bytes for in-place value construction and returns a pointer.
  void* reserve(size_t size) { return _cursor->reserve(size); }

  // Inserts or overwrites the record at the current position.
  void value(const Slice& value) { _cursor->value(value); }

  // Returns the current value as a non-owning Slice.
  Slice value() const { return _cursor->value(); }

  // Returns the current key as a non-owning Slice.
  Slice key() const { return _cursor->key(); }

  // Deletes the current record.
  void remove() { _cursor->remove(); }

  // Refreshes cursor view after out-of-band mutation.
  void update() { _cursor->update(); }

  // Opens a write transaction.
  // Returns false if a transaction is already active, start is vetoed, or the
  // storage layer cannot acquire a write transaction.
  // Set non_blocking = true to fail instead of waiting.
  // Set use_wal = true for WAL semantics.
  bool start_transaction(bool non_blocking = false, bool use_wal = false) {
    return _cursor->start_transaction(non_blocking, use_wal,
                                      TransactionOrigin::user);
  }

  // Moves the transaction to prepared state.
  // Returns the prepared transaction id, or 0 if no transaction is active.
  tid_t prepare_commit(bool sync = false) {
    return _cursor->prepare_commit(sync);
  }

  // Finalizes the active transaction.
  // Returns false if no transaction is active, commit is rejected by a hook,
  // or the underlying commit fails.
  bool commit(bool sync = false) { return _cursor->commit(sync); }

  // Discards all changes since start_transaction().
  // Returns false if no transaction is active or rollback is rejected.
  bool rollback() { return _cursor->rollback(); }

  // Returns true while a transaction is active.
  bool is_transaction_active() const {
    return _cursor && _cursor->is_transaction_active();
  }

  // Returns mutable cursor-level aspect context.
  auto& aspect_context() { return _cursor->_aspect_context; }
  // Returns read-only cursor-level aspect context.
  const auto& aspect_context() const { return _cursor->_aspect_context; }

 private:
  storage_ptr _storage;
  cursor_ptr _cursor;
};
}  // namespace leaves

#endif  // _LEAVES_CURSOR_HPP