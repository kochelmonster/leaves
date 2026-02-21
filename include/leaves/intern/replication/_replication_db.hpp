#ifndef _LEAVES__REPLICATION_DB_HPP
#define _LEAVES__REPLICATION_DB_HPP

#include "../db/_db.hpp"

#include <atomic>
#include <chrono>

namespace leaves {

// Replication-enabled traits mixin — inherits from base traits and enables
// 32-byte hash storage plus Blake3 hashing for replication.
template <typename BaseTraits>
struct _ReplicationTraits : public BaseTraits {
  typedef uint8_t hash_t[HASH_SIZE];
  typedef Blake3Hasher ReplicationHasher;
};

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

  // Transaction ID up to which all nodes have been hashed.
  // Background hasher updates this after catching up.
  // Replication sender must wait until last_hashed_txn_id >= target.
  std::atomic<tid_t> last_hashed_txn_id{tid_t(0)};

  // Offset of the last fully-hashed transaction (persisted for restart).
  // Atomic for lock-free thread-safe access from hashed_txn().
  // NOTE: std::atomic<uint64_t> is trivially copyable and works with mmap.
  std::atomic<uint64_t> hashed_txn_offset{0};
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
  using Transaction = typename Base::Transaction;
  using Aspect = typename Base::Aspect;

  // Override CursorTraits to point DB to _ReplicationDB (not _DB)
  // This ensures cursor.commit() calls _ReplicationDB::commit()
  struct CursorTraits : public Base::CursorTraits {
    typedef _ReplicationDB<Storage_> DB;
  };

  // Override cursor types to use _ReplicationCursor
  typedef _ReplicationCursor<CursorTraits> Cursor;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  // Inherit constructors
  using Base::Base;
  using txn_ptr = typename Base::txn_ptr;
  using offset_e = typename CursorTraits::offset_e;

  // --- Purge configuration ---
  std::atomic<uint64_t> _retention_seconds{86400};  // how long deleted keys stay (default 24h)

  // --- Purge state ---
  std::atomic<bool> _purge_interrupt{false};
  std::atomic<bool> _purge_cancelled{false};
  std::atomic<uint64_t> _purge_job_id{0};
  std::atomic<bool> _in_purge{false};

  // --- Background hashing state ---
  // Hashing runs in background, catching up to current txn_id.
  // Commits never wait; use on_hashes_ready aspect callback for event-driven replication.
  // Hashing is delayed until idle (no commits for _hash_idle_ms) to avoid
  // interfering with write-heavy workloads.
  std::atomic<bool> _hash_running{false};
  std::atomic<bool> _hash_cancelled{false};
  std::atomic<uint64_t> _hash_job_id{0};  // Scheduled delayed hash job
  std::atomic<uint64_t> _hash_idle_ms{100};  // Delay before hashing starts (default 100ms)

  ~_ReplicationDB() {
    cancel_purge();
    _stop_hash_catchup();
  }

  void set_retention(uint64_t seconds) { _retention_seconds.store(seconds, std::memory_order_relaxed); }

  // Set the idle delay before background hashing starts (default 100ms).
  // Higher values reduce interference with write-heavy workloads.
  void set_hash_idle_ms(uint64_t ms) { _hash_idle_ms.store(ms, std::memory_order_relaxed); }

  // Force immediate hashing, bypassing the idle delay.
  // Blocks until all pending transactions are hashed.
  // Use when you need hashes immediately (e.g., before replication).
  void flush_hashes() {
    // Cancel any pending delayed job and start immediately
    uint64_t old_job = _hash_job_id.exchange(0, std::memory_order_acq_rel);
    if (old_job && old_job != UINT64_MAX) {
      this->_storage.cancel_job(old_job);
    }
    _start_hash_catchup();
    // Wait for hashing to complete
    if (!this->_storage._pool_shutdown.load(std::memory_order_acquire)) {
      this->_storage.wait_all();
    }
  }

  // Check if hashing has caught up to a given transaction.
  bool hashes_ready_through(tid_t target_txn_id) const {
    return this->_header->last_hashed_txn_id.load(std::memory_order_acquire) >= target_txn_id;
  }

  // Get the last fully hashed transaction ID.
  // Get the last fully-hashed transaction, or nullptr if none available.
  // The returned transaction is pinned (ref-counted) and safe to use even
  // as new commits occur.  Returns nullptr only before the first transaction
  // has been hashed (e.g., immediately after DB creation).
  // Lock-free: uses atomic offset from header, resolved on demand.
  txn_ptr hashed_txn() const {
    uint64_t off = this->_header->hashed_txn_offset.load(std::memory_order_acquire);
    if (!off) return nullptr;
    offset_t offset(off);
    return this->template resolve<Transaction>(&offset);
  }

  // Override commit - no hashing overhead. Background hasher catches up.
  bool commit(uint64_t cursor_id, bool sync = false) {
    // Prepare commit without computing hashes (base doesn't hash either)
    if (!Base::prepare_commit(cursor_id, false)) return false;

    // Atomically switch to new transaction
    this->_header->read_txn = this->_header->prepared_txn;
    this->make_dirty(this->_header);
    this->flush(sync, true);
    this->end_transaction();

    // Schedule background hasher after idle delay (cancels any pending schedule).
    // This ensures hashing only starts after writes have been idle for a while,
    // avoiding interference with write-heavy workloads.
    _schedule_hash_catchup();

    return true;
  }

  cursor_ptr create_cursor() {
    auto cursor = std::make_shared<Cursor>(this, &this->txn()->root);
    this->_aspect.init_cursor_context(cursor->_aspect_context);
    return cursor;
  }

  // Start the self-rescheduling purge.  Requires that _storage has
  // schedule_after() / cancel_job() / wait_all() (i.e. _ThreadPoolMixin).
  void start_purge() {
    _purge_cancelled.store(false, std::memory_order_release);
    uint64_t expected = 0;
    if (!_purge_job_id.compare_exchange_strong(expected, UINT64_MAX,
            std::memory_order_acq_rel))
      return;  // Another purge is already scheduled
    _purge_job_id.store(this->_storage.schedule_after(
        std::chrono::seconds(0), [this] { _run_purge(); }),
        std::memory_order_release);
  }

  // Cancel any scheduled or running purge and wait for completion.
  void cancel_purge() {
    _purge_cancelled.store(true, std::memory_order_release);
    _purge_interrupt.store(true, std::memory_order_release);
    uint64_t job_id = _purge_job_id.exchange(0, std::memory_order_acq_rel);
    if (job_id && job_id != UINT64_MAX)
      this->_storage.cancel_job(job_id);
    if (!this->_storage._pool_shutdown.load(std::memory_order_acquire))
      this->_storage.wait_all();
  }

  // Override sanitize() to also recover orphaned replication anchors.
  void sanitize() {
    Base::sanitize();
    _sanitize_replication_anchors();

    // hashed_txn_offset is already atomic in header - no load needed

    this->flush();

    // Kick background hasher if there are unhashed transactions
    tid_t last_hashed = this->_header->last_hashed_txn_id.load(std::memory_order_acquire);
    tid_t current = this->txn()->txn_id;
    if (last_hashed < current) {
      _start_hash_catchup();
    }
  }

  // Override: signal background purge to stop before acquiring txn_lock.
  // No waiting for hashes - they run independently.
  txn_ptr start_transaction(uint64_t cursor_id, bool nonblocking = false) {
    if (_in_purge.load(std::memory_order_relaxed)) {
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
        // Zero the area's next pointer so the pool's linked list stays
        // consistent (the crash may have left a stale forward link).
        auto area = this->template resolve<Area>(&slot, WRITE);
        area->next = 0;
        this->make_dirty(area);
        offset_t head = this->resolve(area);
        this->_storage.return_multi_areas(head, head);
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
    _in_purge.store(true, std::memory_order_relaxed);
    _purge_interrupt.store(false, std::memory_order_relaxed);

    uint64_t now = _current_time();
    uint64_t retention = _retention_seconds.load(std::memory_order_relaxed);
    uint64_t older_than =
        (now > retention) ? now - retention : 0;

    auto result = _do_purge(older_than);

    // Schedule the next run (unless cancelled)
    if (!_purge_cancelled.load(std::memory_order_acquire)) {
      uint64_t next_seconds;
      if (result.oldest_remaining_ts > 0) {
        // Items remain — wake when the oldest one expires
        uint64_t expire_at = result.oldest_remaining_ts + retention;
        next_seconds = (expire_at > now) ? expire_at - now : 0;
      } else {
        // Deletion trie is empty — check again after one retention period
        next_seconds = retention;
      }
      _purge_job_id.store(this->_storage.schedule_after(
          std::chrono::seconds(next_seconds), [this] { _run_purge(); }),
          std::memory_order_release);
    } else {
      _purge_job_id.store(0, std::memory_order_release);
    }

    _in_purge.store(false, std::memory_order_relaxed);
  }

  // Iterate the deletion trie and remove entries older than the threshold.
  // Returns the number of entries purged and the timestamp of the oldest
  // remaining entry (0 if the trie is empty after purge).
  PurgeResult _do_purge(uint64_t older_than) {
    auto cursor = create_cursor();
    [[maybe_unused]] bool r = cursor->start_transaction();
    // Clear the interrupt that our own start_transaction() just set
    // (we are still _in_purge, so it would self-interrupt immediately).
    _purge_interrupt.store(false, std::memory_order_relaxed);
    auto& del_cursor = cursor->get_deletion_cursor();

    size_t purged = 0;
    uint64_t oldest_ts = 0;
    bool interrupted = false;
    del_cursor.first();
    while (del_cursor.is_valid()) {
      if (_purge_interrupt.load(std::memory_order_relaxed)) {
        interrupted = true;
        break;
      }

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
    // If interrupted before visiting all entries, force a near-immediate
    // reschedule so remaining expired entries are purged promptly.
    if (interrupted && oldest_ts == 0 && purged > 0)
      oldest_ts = older_than > 0 ? older_than : 1;
    return {purged, oldest_ts};
  }

  // --- Background hash catchup ---
  // Schedule hash catchup after idle delay. Cancels any pending schedule.
  // Called on every commit - rapid commits keep pushing hashing back.
  void _schedule_hash_catchup() {
    if (_hash_cancelled.load(std::memory_order_acquire)) return;  // Shutting down
    if (_hash_running.load(std::memory_order_acquire)) return;  // Already running

    // Cancel any pending scheduled job
    uint64_t old_job = _hash_job_id.exchange(0, std::memory_order_acq_rel);
    if (old_job && old_job != UINT64_MAX) {
      this->_storage.cancel_job(old_job);
    }

    // Schedule new job after idle delay
    uint64_t delay_ms = _hash_idle_ms.load(std::memory_order_relaxed);
    uint64_t new_job = this->_storage.schedule_after(
        std::chrono::milliseconds(delay_ms),
        [this]() { _start_hash_catchup(); });
    _hash_job_id.store(new_job, std::memory_order_release);
  }

  // Start the background hash catchup task if not already running.
  // Called from scheduled job after idle delay.
  void _start_hash_catchup() {
    _hash_job_id.store(0, std::memory_order_relaxed);  // Job has fired
    if (_hash_cancelled.load(std::memory_order_acquire)) return;  // Shutting down
    bool expected = false;
    if (!_hash_running.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
      return;  // Already running
    }
    this->_storage.submit_task([this]() { _run_hash_catchup(); });
  }

  // Stop background hashing and wait for completion.
  void _stop_hash_catchup() {
    _hash_cancelled.store(true, std::memory_order_release);
    // Cancel any pending scheduled job
    uint64_t job_id = _hash_job_id.exchange(0, std::memory_order_acq_rel);
    if (job_id && job_id != UINT64_MAX) {
      this->_storage.cancel_job(job_id);
    }
    // Wait for current run to complete
    if (!this->_storage._pool_shutdown.load(std::memory_order_acquire)) {
      this->_storage.wait_all();
    }
  }

  // Background hash catchup loop - runs until hashing is caught up to current txn.
  //
  // THEORETICAL RACE: There is a narrow window between txn() returning a pointer
  // and refs.fetch_add() protecting it. In theory, multiple rapid commits could
  // push this txn into the recyclable set and GC could free it before we increment
  // refs. This is not guarded because:
  //   1. Proper protection requires epoch-based reclamation (RCU) or locks
  //   2. The window is a few CPU instructions (~nanoseconds)
  //   3. Triggering it requires multiple commits completing end_transaction() GC
  //      within that window - practically impossible
  // The same theoretical race exists in cursors (_TransactionalCursor::rollback,
  // update) and is similarly accepted as negligible risk.
  void _run_hash_catchup() {
    while (!_hash_cancelled.load(std::memory_order_acquire)) {
      // Get txn and immediately protect it (minimize race window)
      auto txn = this->txn();
      const_cast<std::atomic<uint32_t>&>(txn->refs).fetch_add(1);

      tid_t last_hashed = this->_header->last_hashed_txn_id.load(std::memory_order_acquire);
      tid_t current = txn->txn_id;

      if (last_hashed >= current) {
        // Caught up - release refs and exit
        const_cast<std::atomic<uint32_t>&>(txn->refs).fetch_sub(1);
        _hash_running.store(false, std::memory_order_release);
        return;
      }

      // Hash all nodes newer than last_hashed
      compute_hashes_catchup(this, txn->root, txn->deletion_root, last_hashed);

      if (_hash_cancelled.load(std::memory_order_acquire)) {
        const_cast<std::atomic<uint32_t>&>(txn->refs).fetch_sub(1);
        _hash_running.store(false, std::memory_order_release);
        return;
      }

      // Store offset atomically in header for lock-free hashed_txn() access.
      // The header keeps the transaction alive (prevents GC) and persists it.
      offset_t txn_offset = this->resolve(txn);
      this->_header->hashed_txn_offset.store(txn_offset, std::memory_order_release);
      const_cast<std::atomic<uint32_t>&>(txn->refs).fetch_sub(1);

      // Update last_hashed_txn_id
      this->_header->last_hashed_txn_id.store(current, std::memory_order_release);
      this->make_dirty(this->_header);
      this->flush(false, false);

      // Notify aspect that hashes are ready for this transaction.
      // Applications can use this to trigger non-blocking replication.
      if constexpr (requires { this->_aspect.on_hashes_ready(this, current); }) {
        this->_aspect.on_hashes_ready(this, current);
      }

      // Loop to catch any commits that happened during hashing
    }
    _hash_running.store(false, std::memory_order_release);
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

    // Aspect gate first, while key() is still valid.
    Slice cur_value = this->_raw_value();
    if (!this->_aspect().may_delete(this->key(), cur_value,
                                    this->_aspect_context)) {
      throw NoValidPosition();
    }

    // Write to the deletion trie BEFORE Base::remove(), because the
    // deleter modifies current_key and COW may invalidate cursor stacks.
    // The aspect gate above ensures no spurious entries on rejection.
    auto& del_cursor = get_deletion_cursor();
    del_cursor.find(this->key());

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

    // Delegate to base — skip aspect (already checked above).
    Base::template remove<false>();
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
