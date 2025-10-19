#ifndef _LEAVES_CURSOR_HPP
#define _LEAVES_CURSOR_HPP

#include "intern/_cursor.hpp"
#include "intern/_util.hpp"

namespace leaves {

template <typename CursorImpl>
class TCursor {
 public:
  template <typename DB>
  TCursor(DB db) : _cursor(db) {}

  tid_t txn_id() const { return _cursor.txn_id(); }

  // return true if the cursor is on a valid position
  bool is_valid() const { return _cursor.is_valid(); }

  void find(const Slice& key) { _cursor.find(key); }

  void first() { _cursor.first(); }

  void last() { _cursor.last(); }

  void next() { _cursor.next(); }

  void prev() { _cursor.prev(); }

  void value(const Slice& value) { _cursor.value(value); }

  Slice value() const { return _cursor.value(); }

  Slice key() const { return _cursor.key(); }

  void remove() { _cursor.remove(); }

  bool start_transaction(bool non_blocking = false) {
    return _cursor.start_transaction(non_blocking);
  }

  void prepare_commit(bool sync = false) { _cursor.prepare_commit(sync); }

  void commit(bool sync = false) { _cursor.commit(sync); }

  void rollback() { _cursor.rollback(); }

 private:
  CursorImpl _cursor;
};
}  // namespace leaves

#endif  // _LEAVES_CURSOR_HPP