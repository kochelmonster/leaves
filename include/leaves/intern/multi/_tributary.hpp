#ifndef _LEAVES_TRIBUTARY_HPP
#define _LEAVES_TRIBUTARY_HPP

#include <atomic>
#include <chrono>

#include "../db/_cursor.hpp"
#include "../db/_db.hpp"
#include "../util/_merger.hpp"

namespace leaves {

// =============================================================================
// _TributaryTransaction: Extends _Transaction with a delete_root
// =============================================================================
// delete_root is a trie of keys deleted from this tributary's data trie.
// During merge into the main DB the delete_root is used to remove those keys
// from the main DB before copying tributary data in.

template <typename Traits_>
struct _TributaryTransaction : public _Transaction<Traits_> {
  using Base = _Transaction<Traits_>;
  using offset_e = typename Traits_::offset_e;

  // Override ptr so _DB::txn_ptr resolves to Pointer<_TributaryTransaction>
  using ptr = typename Traits_::template Pointer<_TributaryTransaction>;

  // Root of the deletion trie — keys deleted from this tributary
  offset_e delete_root{0};

  uint16_t size() const { return sizeof(_TributaryTransaction); }

  // SLOT_ID — assign_slot rounds up to coarse page-size buckets so using
  // Base size + one extra offset_e as upper bound is safe and matches the
  // pattern used by _ReplicationTransaction.
  static constexpr auto SLOT_ID =
      _MemManager<Traits_>::assign_slot(sizeof(Base) + sizeof(offset_e));

  template <typename Resolver>
  typename Traits_::template Pointer<_TributaryTransaction> clone(
      Resolver& resolver) {
    auto page = Base::alloc_slot(SLOT_ID, resolver);
    page->used = sizeof(_TributaryTransaction);
    memcpy(&*page, this, sizeof(_TributaryTransaction));
    auto new_txn = reinterpret_cast<_TributaryTransaction*>(&*page);
    new_txn->mem_manager.reinit_locks();
    new (&new_txn->refs) std::atomic<uint32_t>(0);
    assert(page->slot_id == SLOT_ID);
    return page;
  }
};

// =============================================================================
// _TributarySlot: Persistent metadata for one per-producer tributary
// =============================================================================
// Lives at the content_offset() of a dedicated single area allocated by
// _ConfluenceDB.  Slots are linked via `next` forming a singly-linked list
// rooted at _DBHeader::extra_offset.
//
// Lifetime safety: readers increment reader_refs BEFORE checking in_use.
// Merger CASes in_use 0->2 then spin-waits for reader_refs==0 before
// freeing the slot area.

template <typename Traits_>
struct _TributarySlot {
  // in_use states
  static constexpr uint8_t FREE    = 0;
  static constexpr uint8_t CLAIMED = 1;
  static constexpr uint8_t MERGING = 2;

  std::atomic<uint8_t>  in_use{FREE};
  uint8_t               _pad[3]{};
  std::atomic<uint32_t> reader_refs{0};   // pinned reader count
  uint64_t              last_used_time{0}; // epoch seconds, set on commit
  uint32_t              write_count{0};    // committed writes since creation
  uint32_t              _pad2{0};
  offset_t              self_area{0};      // offset of this slot's area (for freeing)
  offset_t              db_header{0};      // offset of tributary's _DBHeader
  offset_t              next{0};           // next slot in list (0 = end)
};

// =============================================================================
// Forward declarations
// =============================================================================
template <typename CursorTraits_>
struct _TributaryCursor;

// =============================================================================
// _TributaryDB: A per-producer write-buffer DB
// =============================================================================
// Uses _TributaryTransaction (extends _Transaction with delete_root).
// Reuses _DBHeader (no custom header needed).
// Overrides cursor factory to return _TributaryCursor.

template <typename Storage_>
struct _TributaryDB
    : public _DB<Storage_,
                 _TributaryTransaction<typename Storage_::Traits>,
                 _DBHeader<Storage_>,
                 _TributaryDB<Storage_>> {
  using Traits = typename Storage_::Traits;
  using Base = _DB<Storage_,
                   _TributaryTransaction<Traits>,
                   _DBHeader<Storage_>,
                   _TributaryDB<Storage_>>;
  using Transaction = typename Base::Transaction;

  // Override CursorTraits so cursors point back to _TributaryDB
  struct CursorTraits : public Base::CursorTraits {
    typedef _TributaryDB<Storage_> DB;
  };

  typedef _TributaryCursor<CursorTraits> Cursor;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  // DB_TYPE_ID = 3 (0 = _DB, 1 = _ReplicationDB, 2 = reserved, 3 = _TributaryDB)
  static constexpr uint16_t DB_TYPE_ID = 3;

  // Open existing tributary
  _TributaryDB(Storage_& storage, offset_t header, std::string_view name)
      : Base(storage, header, name) {}

  // Create and init new tributary
  _TributaryDB(Storage_& storage, offset_t* header, std::string_view name)
      : Base(storage, header, name) {}

  cursor_ptr create_cursor() {
    auto cursor = std::make_shared<Cursor>(this, &this->txn()->root);
    this->_aspect.init_cursor_context(cursor->_aspect_context);
    return cursor;
  }
};

// =============================================================================
// _TributaryCursor: Extends _TransactionalCursor with deletion tracking
// =============================================================================
// remove() inserts the key into delete_root in addition to deleting from main.
// value() (write) removes the key from delete_root if it was previously marked
// deleted.
// get_deletion_cursor() provides a read-only cursor into delete_root.

template <typename CursorTraits_>
struct _TributaryCursor : public _TransactionalCursor<CursorTraits_> {
  using Base = _TransactionalCursor<CursorTraits_>;
  using Traits = CursorTraits_;
  using DB = typename Traits::DB;
  using offset_e = typename Traits::offset_e;
  using Transaction = typename DB::Transaction;  // = _TributaryTransaction<Traits>

  using Base::Base;

  // Convenience: cast _txn to the concrete _TributaryTransaction type
  Transaction* _trib_txn() {
    return static_cast<Transaction*>(&*this->_txn);
  }

  // Read-only cursor into the delete_root trie (read from current txn)
  _Cursor<CursorTraits_> get_deletion_cursor() {
    return _Cursor<CursorTraits_>(this->_db, &_trib_txn()->delete_root);
  }

  // Override remove(): delete from main trie + record in delete_root
  template <bool callaspect = true>
  void remove() {
    if (!this->is_valid()) throw NoValidPosition();
    std::string key = this->current_key;

    // 1. Remove from main trie (base handles big-value freeing + aspect)
    Base::template remove<callaspect>();

    // 2. Record deletion in delete_root (empty value = tombstone)
    _insert_tombstone(key);
  }

  // Override value(slice) write: write to main trie + un-delete if needed
  void value(const Slice& v) {
    // Clear any tombstone for this key before writing
    _remove_tombstone(this->current_key);
    Base::value(v);
  }

  void _insert_tombstone(const std::string& key) {
    assert(this->is_transaction_active());
    _Cursor<CursorTraits_> del_cursor(this->_db, &_trib_txn()->delete_root);
    del_cursor.find(Slice(key));
    del_cursor.value(Slice());
  }

  void _remove_tombstone(const std::string& key) {
    if (!this->_txn) return;
    if (!_trib_txn()->delete_root) return;
    _Cursor<CursorTraits_> del_cursor(this->_db, &_trib_txn()->delete_root);
    del_cursor.find(Slice(key));
    if (del_cursor.is_valid() && del_cursor.current_key == key) {
      del_cursor.remove();
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_TRIBUTARY_HPP
