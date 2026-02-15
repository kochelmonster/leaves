#ifndef _LEAVES__REPLICATION_DB_HPP
#define _LEAVES__REPLICATION_DB_HPP

#include "../db/_db.hpp"

#include <atomic>
#include <chrono>

namespace leaves {

// Replication-specific DB header — extends _DBHeader with a fixed-size
// slot array for crash-safe tracking of pre-merge big-value multi-areas.
// Each active ReplicationReceiverFSM claims one slot.  The slot holds the
// offset of the multi-area currently being filled.  On crash recovery,
// sanitize() returns all non-zero slots to the pool.
//
// MAX_REPLICATION_SLOTS is read from Storage_::Traits if available,
// defaulting to 8 (= 64 bytes of offset_t).
template <typename Storage_>
struct _ReplicationDBHeader : public _DBHeader<Storage_> {
  using Traits = typename Storage_::Traits;

  // Detect MAX_REPLICATION_SLOTS from Traits, default to 8
  template <typename T, typename = void>
  struct get_max_replication_slots
      : std::integral_constant<uint16_t, 8> {};

  template <typename T>
  struct get_max_replication_slots<T, std::void_t<decltype(T::MAX_REPLICATION_SLOTS)>>
      : std::integral_constant<uint16_t, T::MAX_REPLICATION_SLOTS> {};

  static constexpr uint16_t MAX_REPLICATION_SLOTS =
      get_max_replication_slots<Traits>::value;

  // Sentinel value written by _claim_slot() via atomic CAS before the
  // real area offset is known.  sanitize() must skip these.
  static constexpr uint64_t REPLICATION_SLOT_SENTINEL = 1;

  // Each slot holds the offset of one pre-merge multi-area.
  // 0 = slot is free, SENTINEL = claimed but no area yet.
  offset_t replication_slots[MAX_REPLICATION_SLOTS];
};

// =============================================================================
// _ReplicationTransaction: Extends _Transaction with a deletion_root
// =============================================================================
// The deletion trie tracks keys removed from the main trie, enabling
// replication of deletes. Both root and deletion_root are persisted in
// each transaction and independently Merkle-hashed.

template <typename Traits_>
struct _ReplicationTransaction : public _Transaction<Traits_> {
  using Base = _Transaction<Traits_>;
  using offset_e = typename Traits_::offset_e;

  // Override ptr to point to _ReplicationTransaction so that _DB::txn_ptr
  // resolves to Pointer<_ReplicationTransaction> rather than Pointer<_Transaction>
  using ptr = typename Traits_::template Pointer<_ReplicationTransaction>;

  // Root of the deletion trie — records keys deleted from the main trie
  offset_e deletion_root;

  // Override size to include deletion_root
  uint16_t size() const { return sizeof(_ReplicationTransaction); }

  // Override SLOT_ID — use Base size + added field as upper bound
  // (sizeof(_ReplicationTransaction) is incomplete here; this is safe
  //  because assign_slot rounds up to coarse page-size buckets)
  static constexpr auto SLOT_ID =
      _MemManager<Traits_>::assign_slot(sizeof(Base) + sizeof(offset_e));

  // Override clone to copy the full struct including deletion_root
  template <typename Resolver>
  typename Traits_::template Pointer<_ReplicationTransaction> clone(
      Resolver& resolver) {
    auto new_txn = Base::alloc_slot(SLOT_ID, resolver);
    new_txn->used = sizeof(_ReplicationTransaction);
    memcpy((char*)new_txn, this, sizeof(_ReplicationTransaction));
    assert(new_txn->slot_id == SLOT_ID);
    return new_txn;
  }
};

// =============================================================================
// _ReplicationDB: Extends _DB with deletion trie support and background purge
// =============================================================================
// Uses _ReplicationTransaction which includes deletion_root.
// Overrides Cursor to be _ReplicationCursor which tracks deletes.
// Owns the purge lifecycle: start_purge() kicks off a self-rescheduling
// background job that removes expired entries from the deletion trie.

template <typename Traits_>
struct _ReplicationCursor;  // forward declaration

template <typename Storage_>
struct _ReplicationDB
    : public _DB<Storage_,
                 _ReplicationTransaction<typename Storage_::Traits>,
                 _ReplicationDBHeader<Storage_>> {
  using Base =
      _DB<Storage_, _ReplicationTransaction<typename Storage_::Traits>,
          _ReplicationDBHeader<Storage_>>;
  using CursorTraits = typename Base::CursorTraits;
  using Transaction = typename Base::Transaction;
  using Aspect = typename Base::Aspect;

  // Override cursor types to use _ReplicationCursor
  typedef _ReplicationCursor<CursorTraits> Cursor;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  // Inherit constructors
  using Base::Base;
  using txn_ptr = typename Base::txn_ptr;
  using offset_e = typename CursorTraits::offset_e;

  // --- Purge configuration ---
  uint64_t _retention_seconds = 86400;  // how long deleted keys stay (default 24h)

  // --- Purge state ---
  std::atomic<bool> _purge_interrupt{false};
  std::atomic<bool> _purge_cancelled{false};
  uint64_t _purge_job_id = 0;
  inline static thread_local bool _in_purge = false;

  ~_ReplicationDB() {
    cancel_purge();
  }

  void set_retention(uint64_t seconds) { _retention_seconds = seconds; }

  cursor_ptr create_cursor() {
    auto cursor = std::make_shared<Cursor>(this, &this->txn()->root);
    this->_aspect.init_cursor_context(cursor->_aspect_context);
    return cursor;
  }

  // Start the self-rescheduling purge.  Requires that _storage has
  // schedule_after() / cancel_job() / wait_all() (i.e. _ThreadPoolMixin).
  void start_purge() {
    _purge_cancelled.store(false, std::memory_order_relaxed);
    _purge_job_id = this->_storage.schedule_after(
        std::chrono::seconds(0), [this] { _run_purge(); });
  }

  // Cancel any scheduled or running purge and wait for completion.
  void cancel_purge() {
    _purge_cancelled.store(true, std::memory_order_release);
    _purge_interrupt.store(true, std::memory_order_release);
    if (_purge_job_id) {
      this->_storage.cancel_job(_purge_job_id);
      _purge_job_id = 0;
    }
    if (!this->_storage._pool_shutdown.load(std::memory_order_acquire))
      this->_storage.wait_all();
  }

  // Override sanitize() to also recover orphaned replication anchors.
  void sanitize() {
    Base::sanitize();
    _sanitize_replication_anchors();
    this->flush();
  }

  // Override: signal background purge to stop before acquiring txn_lock
  // so the purge commits quickly and releases the lock.
  txn_ptr start_transaction(uint64_t cursor_id, bool nonblocking = false) {
    if (!_in_purge) {
      _purge_interrupt.store(true, std::memory_order_release);
    }
    return Base::start_transaction(cursor_id, nonblocking);
  }

  void _sanitize_replication_anchors() {
    constexpr auto N = Base::Header::MAX_REPLICATION_SLOTS;
    constexpr uint64_t SENTINEL = Base::Header::REPLICATION_SLOT_SENTINEL;
    for (uint16_t i = 0; i < N; ++i) {
      auto& slot = this->_header->replication_slots[i];
      if (slot._offset == SENTINEL) {
        // Sentinel: slot was claimed but no area allocated yet — just clear.
        slot = 0;
      } else if (slot) {
        this->_storage.return_multi_areas(slot, slot);
        slot = 0;
      }
    }
    this->make_dirty(this->_header);
  }

 private:
  struct PurgeResult {
    size_t purged;
    uint64_t oldest_remaining_ts;  // 0 = deletion trie empty
  };

  static uint64_t _current_time() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  // The scheduled purge job — runs on the storage's thread pool.
  // After purging, schedules the next run based on the oldest remaining
  // entry and the retention period.
  void _run_purge() {
    _in_purge = true;
    _purge_interrupt.store(false, std::memory_order_relaxed);

    uint64_t now = _current_time();
    uint64_t older_than =
        (now > _retention_seconds) ? now - _retention_seconds : 0;

    auto result = _do_purge(older_than);

    // Schedule the next run (unless cancelled)
    if (!_purge_cancelled.load(std::memory_order_acquire)) {
      uint64_t next_seconds;
      if (result.oldest_remaining_ts > 0) {
        // Items remain — wake when the oldest one expires
        uint64_t expire_at = result.oldest_remaining_ts + _retention_seconds;
        next_seconds = (expire_at > now) ? expire_at - now : 0;
      } else {
        // Deletion trie is empty — check again after one retention period
        next_seconds = _retention_seconds;
      }
      _purge_job_id = this->_storage.schedule_after(
          std::chrono::seconds(next_seconds), [this] { _run_purge(); });
    }

    _in_purge = false;
  }

  // Iterate the deletion trie and remove entries older than the threshold.
  // Returns the number of entries purged and the timestamp of the oldest
  // remaining entry (0 if the trie is empty after purge).
  PurgeResult _do_purge(uint64_t older_than) {
    auto cursor = create_cursor();
    [[maybe_unused]] bool r = cursor->start_transaction();
    auto& del_cursor = cursor->get_deletion_cursor();

    size_t purged = 0;
    uint64_t oldest_ts = 0;
    del_cursor.first();
    while (del_cursor.is_valid()) {
      if (_purge_interrupt.load(std::memory_order_relaxed)) break;

      Slice val = del_cursor.value();
      if (val.size() >= sizeof(uint64_t)) {
        boost::endian::little_uint64_t ts_le;
        std::memcpy(&ts_le, val.data(), sizeof(ts_le));
        uint64_t ts = ts_le;
        if (ts <= older_than) {
          del_cursor.remove();
          ++purged;
          continue;  // remove() advances to next position
        } else {
          if (oldest_ts == 0 || ts < oldest_ts) oldest_ts = ts;
        }
      } else {
        // Legacy entry with no timestamp — purge it
        del_cursor.remove();
        ++purged;
        continue;
      }
      del_cursor.next();
    }
    cursor->commit();
    return {purged, oldest_ts};
  }
};

// =============================================================================
// _ReplicationCursor: Extends _TransactionalCursor with deletion tracking
// =============================================================================
// On remove(), inserts the deleted key into the deletion trie before
// deleting from the main trie. The deletion trie stores keys with a
// timestamp value recording when the deletion occurred.

template <typename Traits_>
struct _ReplicationCursor : public _TransactionalCursor<Traits_> {
  using Base = _TransactionalCursor<Traits_>;
  using DB = typename Traits_::DB;
  using txn_ptr = typename DB::txn_ptr;
  using offset_e = typename Traits_::offset_e;
  using Transition = typename Base::Cursor::Transition;
  using LeafNode = typename Transition::LeafNode;
  using BigValue = typename Base::BigValue;
  using BigMemory = typename Base::BigMemory;
  using Transaction = typename DB::Transaction;

  // Internal cursor for the deletion trie (non-transactional, shares the
  // same DB/transaction as the main cursor)
  using DeletionCursor = _Cursor<Traits_>;

  // Lazily initialized deletion cursor
  std::unique_ptr<DeletionCursor> _deletion_cursor;

  _ReplicationCursor(DB* db, offset_e* root) : Base(db, root) {}

  DeletionCursor& get_deletion_cursor() {
    if (!_deletion_cursor) {
      auto* txn = static_cast<Transaction*>(&*this->_txn);
      _deletion_cursor =
          std::make_unique<DeletionCursor>(this->_db, &txn->deletion_root);
    }
    return *_deletion_cursor;
  }

  void remove(Slice meta = Slice()) {
    [[maybe_unused]] bool r = this->start_transaction();
    if (!this->is_valid()) throw NoValidPosition();

    // Aspect gate — may throw or return false to reject
    Slice cur_value = this->_raw_value();
    if (!this->_aspect().may_delete(this->key(), cur_value,
                                    this->_aspect_context)) {
      throw NoValidPosition();  // Aspect rejected the delete
    }

    // Record the key in the deletion trie before removing from main trie
    Slice deleted_key = this->key();
    auto& del_cursor = get_deletion_cursor();
    del_cursor.find(deleted_key);
    // Store current timestamp (seconds since epoch) followed by optional
    // metadata (e.g. a version vector) as the deletion entry value.
    // Layout: [uint64_le timestamp][meta bytes...]
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    boost::endian::little_uint64_t ts_le = now;
    if (meta.size() == 0) {
      Slice ts_value(reinterpret_cast<const char*>(&ts_le), sizeof(ts_le));
      del_cursor.value(ts_value);
    } else {
      // Concatenate timestamp + meta into a single buffer
      std::vector<char> buf(sizeof(ts_le) + meta.size());
      std::memcpy(buf.data(), &ts_le, sizeof(ts_le));
      std::memcpy(buf.data() + sizeof(ts_le), meta.data(), meta.size());
      del_cursor.value(Slice(buf.data(), buf.size()));
    }

    // Now remove from main trie (same as _TransactionalCursor::remove)
    const Transition& back = this->stack.back();
    if (back.leaf()->is_big()) {
      BigValue* bvalue = (BigValue*)back.leaf()->vdata();
      this->get_bigmemory().free(bvalue);
    }
    _Deleter(*this).exec();
  }

  // Override _set_txn to also update the deletion cursor's root
  void _set_txn(txn_ptr& txn) {
    Base::_set_txn(txn);
    if (_deletion_cursor) {
      auto* rtxn = static_cast<Transaction*>(&*txn);
      _deletion_cursor->set_root(&rtxn->deletion_root);
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES__REPLICATION_DB_HPP
