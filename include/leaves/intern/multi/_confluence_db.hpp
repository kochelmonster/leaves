#ifndef _LEAVES_CONFLUENCE_DB_HPP
#define _LEAVES_CONFLUENCE_DB_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "_tributary.hpp"

namespace leaves {

// =============================================================================
// _DefaultConflictPolicy: highest txn_id wins
// =============================================================================
// resolve() is given a list of candidates for the same key gathered from
// the main DB and all active tributaries.  Returns the index of the winner
// (or -1 if the winner is a deletion tombstone → key not found).

struct _DefaultConflictPolicy {
  struct _Candidate {
    tid_t txn_id;
    Slice value;
    bool is_deleted;
  };

  // Returns index of winner, or -1 if the winning candidate is a tombstone.
  int resolve(const std::string& /*key*/,
              const std::vector<_Candidate>& candidates) const {
    int winner = -1;
    tid_t best{0};
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
      if (candidates[i].txn_id > best) {
        best = candidates[i].txn_id;
        winner = i;
      }
    }
    if (winner < 0) return -1;
    return candidates[winner].is_deleted ? -1 : winner;
  }
};

// =============================================================================
// Forward declarations
// =============================================================================
template <typename Storage_, typename ConflictPolicy_>
struct _ConfluenceCursor;

// =============================================================================
// _TributaryMergePolicy: bridges _Merger's may_overwrite() with ConflictPolicy
// =============================================================================
// Used in _ConfluenceDB::_do_merge to apply the conflict policy when a key
// exists in both the tributary (src) and the main DB (dst).

template <typename ConflictPolicy>
struct _TributaryMergePolicy : public StandardMergePolicy {
  using Candidate = typename ConflictPolicy::_Candidate;

  const ConflictPolicy& _policy;
  tid_t _main_txn_id;
  tid_t _trib_txn_id;

  _TributaryMergePolicy(const ConflictPolicy& policy, tid_t main_txn_id,
                        tid_t trib_txn_id)
      : _policy(policy), _main_txn_id(main_txn_id), _trib_txn_id(trib_txn_id) {}

  bool may_overwrite(const std::string& key, const Slice& dst, const Slice& src,
                     bool /*dst_is_big*/, bool /*src_is_big*/) {
    // index 0 = main DB (dst), index 1 = tributary (src)
    std::vector<Candidate> cands = {
        {_main_txn_id, dst, false},
        {_trib_txn_id, src, false},
    };
    return _policy.resolve(key, cands) == 1;
  }
};

// =============================================================================
// _ConfluenceDB: Multiproducer LSM meta-database
// =============================================================================
// - Manages a list of _TributaryDB instances via _DBHeader::extra_offset.
// - Each producer claims a free _TributarySlot; writes go into the tributary.
// - A background thread merges tributaries that exceed write_count threshold
//   or have been idle for longer than idle_timeout_seconds.
// - Reads (find / iteration) merge-scan main DB + all live tributaries.

template <typename Storage_, typename ConflictPolicy_ = _DefaultConflictPolicy>
struct _ConfluenceDB
    : public _DB<Storage_, _Transaction<typename Storage_::Traits>,
                 _DBHeader<Storage_>,
                 _ConfluenceDB<Storage_, ConflictPolicy_>> {
  using Traits = typename Storage_::Traits;
  using Base = _DB<Storage_, _Transaction<Traits>, _DBHeader<Storage_>,
                   _ConfluenceDB<Storage_, ConflictPolicy_>>;
  using ConflictPolicy = ConflictPolicy_;
  using TributaryDB = _TributaryDB<Storage_>;
  using Slot = _TributarySlot<Traits>;
  using area_ptr = typename Storage_::area_ptr;
  using offset_e = typename Traits::offset_e;
  using txn_ptr = typename Base::txn_ptr;

  // Override CursorTraits to point DB to _ConfluenceDB
  struct CursorTraits : public Base::CursorTraits {
    typedef _ConfluenceDB<Storage_, ConflictPolicy_> DB;
  };

  typedef _ConfluenceCursor<Storage_, ConflictPolicy_> Cursor;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  // DB_TYPE_ID = 4 (0=_DB, 1=_ReplicationDB, 3=_TributaryDB, 4=_ConfluenceDB)
  static constexpr uint16_t DB_TYPE_ID = 4;

  // -------------------------------------------------------------------------
  // In-process configuration
  // -------------------------------------------------------------------------
  std::atomic<uint32_t> _merge_write_threshold{Slot::BLOOM_MAX_KEYS};
  std::atomic<uint64_t> _idle_timeout_seconds{300};

  // Protects the slot linked-list (extra_offset, slot->next).
  // Kept in-process (not shared-memory) because it only guards brief list
  // pointer updates.  Using this instead of txn_lock avoids contention
  // with long-running _do_merge transactions.
  std::mutex _slot_list_lock;

  // -------------------------------------------------------------------------
  // Background merge monitor state  (same pattern as
  // _ReplicationDB::_run_purge)
  // -------------------------------------------------------------------------
  std::atomic<bool> _monitor_cancelled{false};
  std::atomic<bool> _monitor_interrupt{false};
  std::atomic<uint64_t> _monitor_job_id{0};
  ConflictPolicy_ _conflict_policy;

  // -------------------------------------------------------------------------
  // Constructors
  // -------------------------------------------------------------------------
  _ConfluenceDB(Storage_& storage, offset_t header, std::string_view name,
                bool auto_monitor = true)
      : Base(storage, header, name) {
    if (auto_monitor) start_monitor();
  }

  _ConfluenceDB(Storage_& storage, offset_t* header, std::string_view name,
                bool auto_monitor = true)
      : Base(storage, header, name) {
    if (auto_monitor) start_monitor();
  }

  ~_ConfluenceDB() { cancel_monitor(); }

  // -------------------------------------------------------------------------
  // cursor factory
  // -------------------------------------------------------------------------
  cursor_ptr create_confluence_cursor() {
    return std::make_shared<Cursor>(this);
  }

  // -------------------------------------------------------------------------
  // Configuration
  // -------------------------------------------------------------------------
  void set_merge_write_threshold(uint32_t n) {
    _merge_write_threshold.store(n, std::memory_order_relaxed);
  }

  void set_idle_timeout_seconds(uint64_t s) {
    _idle_timeout_seconds.store(s, std::memory_order_relaxed);
  }

  // Synchronously merge all write-count-eligible and idle tributaries on the
  // calling thread, without waiting for the background monitor timer.
  void merge_now() { merge_eligible_tributaries(); }

  // Synchronously merge ALL free tributaries regardless of threshold / idle
  // timeout.  Call this after a write phase completes to ensure reads see a
  // clean single-source state without waiting for the background timer.
  void merge_all_now() {
    offset_t cur = this->_header->extra_offset;
    while (cur) {
      auto slot = this->template resolve<Slot>(&cur);
      offset_t next_off = slot->next;
      if (slot->in_use.load(std::memory_order_relaxed) == Slot::FREE &&
          slot->db_header) {
        merge_tributary(slot);
      }
      cur = next_off;
    }
    this->flush();
  }

  // -------------------------------------------------------------------------
  // Sanitize: crash recovery — merge any unclaimed tributaries on reopen
  // -------------------------------------------------------------------------
  void sanitize() {
    Base::sanitize();
    _merge_unclaimed_tributaries();
    this->flush();
  }

  // =========================================================================
  // Tributary slot management
  // =========================================================================

  // Allocate a brand-new slot area and initialize a fresh TributaryDB in it.
  // Returns a pointer to the slot (lives at area->content_offset()).
  // Caller must set in_use = CLAIMED before releasing the list lock.
  //
  // Must be called under this->_header->txn_lock (caller holds it).
  typename Traits::template Pointer<Slot> _alloc_new_slot() {
    // Allocate an area for the slot itself
    {
      std::scoped_lock lock(this->_storage.file_lock());
      auto slot_area = this->_storage.alloc_single_area();
      slot_area->next = 0;

      offset_t slot_area_off = this->_storage.resolve(slot_area);
      offset_t slot_off = slot_area->content_offset();

      auto slot_ptr = this->template resolve<Slot>(&slot_off, WRITE);
      memset(&*slot_ptr, 0, sizeof(Slot));
      new (&slot_ptr->in_use) std::atomic<uint8_t>(Slot::CLAIMED);
      new (&slot_ptr->reader_refs) std::atomic<uint32_t>(0);
      slot_ptr->self_area = slot_area_off;

      // Allocate a separate area for the tributary's _DBHeader, then init
      // a fresh _TributaryDB using that area.
      offset_t trib_header_off{0};
      {
        // _TributaryDB constructor with offset_t* calls init() which
        // allocates the first area from storage.  We pass a stack variable
        // for the header location; the tributary writes its header offset
        // into it.
        TributaryDB trib(this->_storage, &trib_header_off, "_tributary");
        slot_ptr->db_header = trib_header_off;
        // trib destructor is harmless — it does not return areas
      }

      // Prepend to linked list (extra_offset = head)
      slot_ptr->next = this->_header->extra_offset;
      this->_header->extra_offset = slot_off;

      this->make_dirty(slot_ptr);
      this->make_dirty(slot_area);
      this->make_dirty(this->_header);
      this->flush();

      return slot_ptr;
    }
  }

  // Scan the slot list starting at `start` and CAS the first FREE slot to
  // CLAIMED. Returns the claimed slot, or a null pointer if none found.
  // Slots that have reached the merge threshold are skipped — they are
  // considered "full" and must be merged before being reused.
  typename Traits::template Pointer<Slot> _try_claim_free_slot(offset_t start) {
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    offset_t cur = start;
    while (cur) {
      auto slot = this->template resolve<Slot>(&cur);
      uint8_t expected = Slot::FREE;
      if (slot->db_header &&
          slot->write_count < threshold &&
          slot->in_use.compare_exchange_strong(expected, Slot::CLAIMED,
                                               std::memory_order_acq_rel)) {
        return slot;
      }
      cur = slot->next;
    }
    return typename Traits::template Pointer<Slot>{nullptr};
  }

  // Walk the slot list, CAS a free slot to CLAIMED.
  // If none found, allocates a new slot.
  // Returns the claimed slot pointer.
  typename Traits::template Pointer<Slot> claim_tributary() {
    // Hold _slot_list_lock for the entire claim operation.
    //
    // A fully lockless first pass is unsafe because a slot's area can be
    // freed (after merge) and immediately reallocated as a trie page by a
    // concurrent write transaction.  If the scanner follows a stale
    // slot->next pointer into a reallocated trie page it reads garbage as a
    // slot header, which can corrupt the slot list or claim an invalid slot.
    //
    // _slot_list_lock is held only for brief pointer-walk operations, so
    // contention is negligible.
    std::scoped_lock lock(_slot_list_lock);

    // Try to CAS an existing free slot first.
    if (auto slot = _try_claim_free_slot(this->_header->extra_offset)) {
      return slot;
    }

    // _alloc_new_slot initialises in_use = CLAIMED before linking the slot.
    return _alloc_new_slot();
  }

  void release_tributary(typename Traits::template Pointer<Slot> slot) {
    slot->in_use.store(Slot::FREE, std::memory_order_release);
    // The first time a slot crosses the write threshold, schedule an immediate
    // merge so reads don't wait up to 1 second for the monitor timer to fire.
    uint32_t wc = slot->write_count;
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    if (wc >= threshold &&
        !_monitor_cancelled.load(std::memory_order_relaxed)) {
      this->_storage.schedule_after(std::chrono::seconds(0),
                                    [this] { merge_eligible_tributaries(); });
    }
  }

  // Merge one tributary into the main DB and free its slot + areas.
  // Returns false if the slot is currently claimed (skip it).
  bool merge_tributary(typename Traits::template Pointer<Slot> slot) {
    uint8_t expected = Slot::FREE;
    if (!slot->in_use.compare_exchange_strong(expected, Slot::MERGING,
                                              std::memory_order_acq_rel))
      return false;  // claimed by a producer — skip

    // Spin-wait until all readers have released their pin on this slot.
    // If the monitor is being cancelled, release the MERGING claim back to
    // FREE and abort — the DB is being torn down anyway.
    while (slot->reader_refs.load(std::memory_order_acquire) != 0) {
      if (_monitor_cancelled.load(std::memory_order_relaxed)) {
        slot->in_use.store(Slot::FREE, std::memory_order_release);
        return false;
      }
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#endif
    }

    if (slot->db_header) {
      _do_merge(slot);
    }

    // Remove slot from linked list.  Use _slot_list_lock (not txn_lock) so
    // this brief pointer-update does not contend with long _do_merge
    // transactions.
    {
      std::scoped_lock lock(_slot_list_lock);
      _unlink_slot(slot);
    }

    // Return slot's area to the storage pool under file_lock, since
    // AreaList::pop/add are not independently thread-safe.
    offset_t area_off = slot->self_area;
    if (area_off) {
      std::scoped_lock flock(this->_storage.file_lock());
      auto area = this->template resolve<Area>(&area_off, WRITE);
      area->next = 0;
      this->make_dirty(area);
      this->_storage.return_single_areas(area_off, area_off);
    }

    this->make_dirty(this->_header);
    this->flush();
    return true;
  }

  // Walk the list and merge eligible free slots.
  // Snapshot the list under _slot_list_lock first: after merge_tributary()
  // frees a slot's area, that area can be reallocated as a trie page by a
  // concurrent writer.  Reading slot->next on the next iteration would then
  // read trie-node bytes as a slot-next pointer and follow a wild address.
  void merge_eligible_tributaries() {
    uint64_t now = _current_time();
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    uint64_t idle_timeout =
        _idle_timeout_seconds.load(std::memory_order_relaxed);

    // Collect eligible slot pointers under the lock so the traversal is
    // protected from concurrent unlink+free by other merger calls.
    std::vector<typename Traits::template Pointer<Slot>> eligible;
    {
      std::scoped_lock lock(_slot_list_lock);
      offset_t cur = this->_header->extra_offset;
      while (cur) {
        auto slot = this->template resolve<Slot>(&cur);
        offset_t next_off = slot->next;
        if (slot->in_use.load(std::memory_order_relaxed) == Slot::FREE) {
          bool should_merge = false;
          if (slot->write_count >= threshold) should_merge = true;
          if (slot->last_used_time > 0 &&
              (now - slot->last_used_time) >= idle_timeout)
            should_merge = true;
          if (should_merge) eligible.push_back(slot);
        }
        cur = next_off;
      }
    }

    for (auto& slot : eligible)
      merge_tributary(slot);
  }

  // =========================================================================
  // Background monitor (self-rescheduling, same pattern as _run_purge)
  // =========================================================================

  void start_monitor() {
    _monitor_cancelled.store(false, std::memory_order_release);
    uint64_t expected = 0;
    if (!_monitor_job_id.compare_exchange_strong(expected, UINT64_MAX,
                                                 std::memory_order_acq_rel))
      return;
    _monitor_job_id.store(
        this->_storage.schedule_after(std::chrono::seconds(0),
                                      [this] { _run_monitor(); }),
        std::memory_order_release);
  }

  void cancel_monitor() {
    _monitor_cancelled.store(true, std::memory_order_release);
    _monitor_interrupt.store(true, std::memory_order_release);
    uint64_t job_id = _monitor_job_id.exchange(0, std::memory_order_acq_rel);
    if (job_id && job_id != UINT64_MAX) this->_storage.cancel_job(job_id);
    if (!this->_storage._pool_shutdown.load(std::memory_order_acquire))
      this->_storage.wait_all();
  }

  // =========================================================================
  // Internal helpers
  // =========================================================================

  static uint64_t _current_time() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  void _run_monitor() {
    if (!_monitor_cancelled.load(std::memory_order_acquire)) {
      merge_eligible_tributaries();
    }

    if (!_monitor_cancelled.load(std::memory_order_acquire)) {
      // Reschedule after 1 second
      _monitor_job_id.store(
          this->_storage.schedule_after(std::chrono::seconds(1),
                                        [this] { _run_monitor(); }),
          std::memory_order_release);
    } else {
      _monitor_job_id.store(0, std::memory_order_release);
    }
  }

  // Two-pass merge: delete pass + copy pass, single commit on main DB.
  void _do_merge(typename Traits::template Pointer<Slot> slot) {
    using TxnType = typename TributaryDB::Transaction;
    using TribCursorTraits = typename TributaryDB::CursorTraits;
    using TribCursor = _TributaryCursor<TribCursorTraits>;
    using MainCursorTraits = typename Base::CursorTraits;
    using MainCursor = _TransactionalCursor<MainCursorTraits>;

    // Open the tributary (read-only open via header offset)
    TributaryDB trib(this->_storage, slot->db_header, "_tributary_merge");
    // Capture txn_ids before start_transaction() mutates the committed txn
    tid_t main_txn_id = this->txn()->txn_id;
    tid_t trib_txn_id = trib.txn()->txn_id;
    auto main_cursor_ptr =
        std::make_shared<MainCursor>(this, &this->txn()->root);
    this->_aspect.init_cursor_context(main_cursor_ptr->_aspect_context);

    main_cursor_ptr->start_transaction();

    // Pass 1: apply deletions from delete_root into main DB
    {
      auto trib_txn = trib.txn();
      auto* ttxn = static_cast<TxnType*>(&*trib_txn);
      if (ttxn->delete_root) {
        _Cursor<TribCursorTraits> del_cursor(&trib, &ttxn->delete_root);
        del_cursor.first();
        while (del_cursor.is_valid()) {
          main_cursor_ptr->find(Slice(del_cursor.current_key));
          if (main_cursor_ptr->is_valid() &&
              main_cursor_ptr->current_key == del_cursor.current_key) {
            main_cursor_ptr->template remove<false>();
          }
          del_cursor.next();
        }
      }
    }

    // Pass 2: merge tributary data trie into main DB
    {
      _Cursor<TribCursorTraits> src(&trib, &trib.txn()->root);
      src.clear();  // position at start of source trie
      _TributaryMergePolicy<ConflictPolicy_> policy(_conflict_policy,
                                                    main_txn_id, trib_txn_id);
      _Merger<MainCursor, _Cursor<TribCursorTraits>,
              _TributaryMergePolicy<ConflictPolicy_>>
          merger(*main_cursor_ptr, src, policy);
      merger.exec();
    }

    main_cursor_ptr->commit();

    // Return all tributary areas to the storage pool under file_lock, since
    // AreaList operations are not independently thread-safe.
    {
      std::scoped_lock flock(this->_storage.file_lock());
      trib.return_areas();
    }

    slot->db_header = 0;
    slot->write_count = 0;
    slot->last_used_time = 0;
    slot->bloom_reset();
    this->make_dirty(slot);
  }

  // Remove slot from the extra_offset linked list.
  // Must be called under txn_lock.
  void _unlink_slot(typename Traits::template Pointer<Slot> target_slot) {
    offset_t target_off = (uint64_t)this->_storage.resolve(target_slot);

    if (this->_header->extra_offset == target_off) {
      this->_header->extra_offset = target_slot->next;
      this->make_dirty(this->_header);
      return;
    }

    offset_t cur = this->_header->extra_offset;
    while (cur) {
      auto prev = this->template resolve<Slot>(&cur);
      if (prev->next == target_off) {
        prev->next = target_slot->next;
        this->make_dirty(prev);
        return;
      }
      cur = prev->next;
    }
  }

  // Merge all tributaries that are not currently claimed (crash recovery).
  void _merge_unclaimed_tributaries() {
    offset_t cur = this->_header->extra_offset;
    while (cur) {
      auto slot = this->template resolve<Slot>(&cur);
      offset_t next_off = slot->next;
      uint8_t state = slot->in_use.load(std::memory_order_relaxed);
      if (state != Slot::CLAIMED && slot->db_header) {
        // Reset atomics that may hold stale kernel-state-free hardware values
        new (&slot->in_use) std::atomic<uint8_t>(Slot::FREE);
        new (&slot->reader_refs) std::atomic<uint32_t>(0);
        merge_tributary(slot);
      }
      cur = next_off;
    }
  }
};

// =============================================================================
// _PinnedSource: one read source held by _ConfluenceCursor
// =============================================================================
template <typename Storage_, typename ConflictPolicy_>
struct _PinnedSource {
  using TributaryDB = _TributaryDB<Storage_>;
  using Traits = typename Storage_::Traits;
  using Slot = _TributarySlot<Traits>;
  using TribCursorTraits = typename TributaryDB::CursorTraits;
  using TribTxnPtr = typename TributaryDB::txn_ptr;

  // For main DB source
  using MainDB = _ConfluenceDB<Storage_, ConflictPolicy_>;
  using MainCursorTraits = typename MainDB::CursorTraits;
  using MainTxnPtr = typename MainDB::txn_ptr;

  // Source type discriminator
  bool _is_main{false};

  // Main DB source
  MainTxnPtr _main_txn;
  std::unique_ptr<_Cursor<MainCursorTraits>> _main_cursor;

  // Tributary source
  typename Traits::template Pointer<Slot> _slot;
  std::unique_ptr<TributaryDB> _trib_db;
  TribTxnPtr _trib_txn;
  std::unique_ptr<_Cursor<TribCursorTraits>> _trib_cursor;
  std::unique_ptr<_Cursor<TribCursorTraits>> _del_cursor;

  // Current key and value after a find/first/next
  std::string _key;
  Slice _value;
  bool _deleted{false};
  bool _valid{false};

  _PinnedSource() = default;
  _PinnedSource(const _PinnedSource&) = delete;
  _PinnedSource& operator=(const _PinnedSource&) = delete;

  // SimplePointer has no move semantics (it copies rather than nulling on
  // move), so we must manually null _slot, _main_txn, and _trib_txn in the
  // move constructor/assignment to prevent double-decrement of refcounts.
  _PinnedSource(_PinnedSource&& o) noexcept
      : _is_main(o._is_main),
        _main_txn(o._main_txn),
        _main_cursor(std::move(o._main_cursor)),
        _slot(o._slot),
        _trib_db(std::move(o._trib_db)),
        _trib_txn(o._trib_txn),
        _trib_cursor(std::move(o._trib_cursor)),
        _del_cursor(std::move(o._del_cursor)),
        _key(std::move(o._key)),
        _value(o._value),
        _deleted(o._deleted),
        _valid(o._valid) {
    o._main_txn.reset();
    o._trib_txn.reset();
    o._slot.reset();
  }

  _PinnedSource& operator=(_PinnedSource&& o) noexcept {
    if (this == &o) return *this;
    // Release our own held references before overwriting
    if (_main_txn) _main_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
    if (_trib_txn) _trib_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
    if (_slot) _slot->reader_refs.fetch_sub(1, std::memory_order_acq_rel);
    _is_main = o._is_main;
    _main_txn = o._main_txn;
    o._main_txn.reset();
    _main_cursor = std::move(o._main_cursor);
    _slot = o._slot;
    o._slot.reset();
    _trib_db = std::move(o._trib_db);
    _trib_txn = o._trib_txn;
    o._trib_txn.reset();
    _trib_cursor = std::move(o._trib_cursor);
    _del_cursor = std::move(o._del_cursor);
    _key = std::move(o._key);
    _value = o._value;
    _deleted = o._deleted;
    _valid = o._valid;
    return *this;
  }

  ~_PinnedSource() {
    if (_main_txn) {
      _main_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (_trib_txn) {
      _trib_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (_slot) {
      _slot->reader_refs.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
};

// =============================================================================
// _ConfluenceCursor
// =============================================================================
// - start_transaction(): claims a tributary, starts a write cursor on it.
// - commit()/rollback(): finish write, update slot metadata.
// - find()/first()/next()/prev()/last(): multi-source merge read.
//
// Reader pinning protocol for slot access:
//   1. Increment reader_refs
//   2. Include the slot in the source list (FREE, CLAIMED, or MERGING).
//      Holding reader_refs > 0 prevents _do_merge from proceeding, so
//      db_header stays valid for the life of this snapshot.
//   3. Release pin in ~_PinnedSource(), allowing any blocked merger to resume.

template <typename Storage_, typename ConflictPolicy_ = _DefaultConflictPolicy>
struct _ConfluenceCursor {
  using ConfluenceDB = _ConfluenceDB<Storage_, ConflictPolicy_>;
  using TributaryDB = _TributaryDB<Storage_>;
  using Traits = typename Storage_::Traits;
  using Slot = _TributarySlot<Traits>;
  using ConflictPolicy = ConflictPolicy_;
  using TribCursorTraits = typename TributaryDB::CursorTraits;
  using TribCursor = _TributaryCursor<TribCursorTraits>;
  using MainCursorTraits = typename ConfluenceDB::CursorTraits;
  using MainCursor = _TransactionalCursor<MainCursorTraits>;
  using TxnType = typename TributaryDB::Transaction;
  using Candidate = typename ConflictPolicy::_Candidate;
  using PinnedSource = _PinnedSource<Storage_, ConflictPolicy_>;

  ConfluenceDB* _cdb;
  ConflictPolicy _policy;

  // Write state
  typename Traits::template Pointer<Slot> _write_slot;
  std::unique_ptr<TributaryDB> _write_trib;
  std::shared_ptr<TribCursor> _write_cursor;
  uint32_t _pending_write_keys{0};  // key count for current transaction

  // Read snapshot (built on first read op, released on next start_transaction)
  std::vector<PinnedSource> _sources;
  bool _sources_valid{false};

  // Reused buffers to avoid per-call heap allocation in the hot read path.
  std::string _search_key;  // reused key string for find()
  std::vector<Candidate>
      _candidates;  // reused candidate list for _resolve_key()

  // Read-cursor state (mirrors _CursorBase naming conventions)
  std::string _value_storage;
  bool _valid{false};

  explicit _ConfluenceCursor(ConfluenceDB* cdb) : _cdb(cdb) {}

  ~_ConfluenceCursor() {
    if (_write_cursor) rollback();
    _release_sources();
  }

  // -------------------------------------------------------------------------
  // Write path
  // -------------------------------------------------------------------------

  bool start_transaction(bool non_blocking = false) {
    if (_write_cursor) return true;  // already active

    _release_sources();  // invalidate read snapshot

    _pending_write_keys = 0;
    _write_slot = _cdb->claim_tributary();
    _write_trib = std::make_unique<TributaryDB>(
        _cdb->_storage, _write_slot->db_header, "_tributary_write");
    _write_cursor = _write_trib->create_cursor();
    return _write_cursor->start_transaction(non_blocking);
  }

  bool commit(bool sync = false) {
    if (!_write_cursor) return false;
    bool ok = _write_cursor->commit(sync);
    if (ok) {
      _write_slot->write_count += _pending_write_keys;
      _write_slot->last_used_time = _current_time();
      _write_slot->db_header = _write_trib_header();
    }
    _pending_write_keys = 0;
    _write_cursor.reset();
    _write_trib.reset();
    _cdb->release_tributary(_write_slot);
    _write_slot = {};
    return ok;
  }

  bool rollback() {
    if (!_write_cursor) return false;
    _write_cursor->rollback();
    _pending_write_keys = 0;
    _write_cursor.reset();
    _write_trib.reset();
    _cdb->release_tributary(_write_slot);
    _write_slot = {};
    return true;
  }

  // Delegate write operations to the tributary cursor
  bool is_transaction_active() const {
    return static_cast<bool>(_write_cursor);
  }

  void value(const Slice& v) {
    assert(_write_cursor);
    _write_cursor->value(v);
    if (_write_slot) {
      _write_slot->bloom_add(_write_cursor->current_key);
      ++_pending_write_keys;
    }
  }

  void remove() {
    assert(_write_cursor);
    // Capture key before remove() repositions the cursor.
    if (_write_slot && _write_cursor->is_valid()) {
      _write_slot->bloom_add(_write_cursor->current_key);
      ++_pending_write_keys;
    }
    _write_cursor->remove();
  }

  // Forward find/position calls to write cursor when a txn is active
  void write_find(const Slice& key) {
    assert(_write_cursor);
    _write_cursor->find(key);
  }

  bool write_is_valid() const {
    return _write_cursor && _write_cursor->is_valid();
  }

  // -------------------------------------------------------------------------
  // Read path — helpers
  // -------------------------------------------------------------------------

  // Build or validate the pinned source list.
  void _ensure_sources() {
    if (_sources_valid) return;
    _sources.clear();

    // Source 0: main DB
    {
      PinnedSource s;
      s._is_main = true;
      s._main_txn = _cdb->txn_ref();
      s._main_cursor =
          std::make_unique<_Cursor<MainCursorTraits>>(_cdb, &s._main_txn->root);
      _sources.push_back(std::move(s));
    }

    // Sources 1..N: tributary slots.
    //
    // Pin each slot (reader_refs++) while holding _slot_list_lock to close
    // the window where the slot area could be freed and reallocated between
    // reading the slot pointer (from extra_offset / slot->next) and the pin.
    // After pinning, reader_refs > 0 prevents the merger from freeing the
    // area, so TributaryDB objects can be opened safely after releasing the
    // lock.
    {
      // Collect (slot, db_header) pairs while holding _slot_list_lock.
      struct PinEntry {
        typename Traits::template Pointer<Slot> slot;
        offset_t db_header;
      };
      std::vector<PinEntry> pinned;
      {
        std::scoped_lock lock(_cdb->_slot_list_lock);
        offset_t cur = _cdb->_header->extra_offset;
        while (cur) {
          auto slot = _cdb->template resolve<Slot>(&cur);
          // Pin immediately, then check in_use.
          // We must pin BEFORE checking in_use to close the window where the
          // merge worker CASes FREE→MERGING, then checks reader_refs == 0,
          // and then we increment reader_refs (too late).
          slot->reader_refs.fetch_add(1, std::memory_order_acq_rel);
          // After pinning, re-read in_use.  If the merge worker already
          // transitioned to MERGING it is now committed to wait (or has
          // already passed the spin-wait).
          //
          // A slot in MERGING state with db_header == 0 means _do_merge
          // has already cleared the header (tributary pages freed).
          // A slot in MERGING state with db_header != 0 means _do_merge
          // has NOT yet freed the pages; our reader_refs pin will hold it.
          //
          // However, there is a narrow window:
          //   merge spin-wait exits (seeing reader_refs==0)
          //   → reader increments reader_refs to 1
          //   → merge proceeds to trib.return_areas() (pages freed)
          //   → reader reads db_header (non-zero, pages already freed)
          // To close this window: if in_use==MERGING, skip unconditionally.
          // reader_refs > 0 only blocks _do_merge *before* the spin-wait
          // exits.  Once the spin-wait has exited we cannot safely use the
          // tributary data.
          uint8_t state = slot->in_use.load(std::memory_order_acquire);
          offset_t hdr = slot->db_header;
          offset_t nxt = slot->next;

          bool skip = (state == Slot::MERGING) || !hdr;
          if (skip) {
            slot->reader_refs.fetch_sub(1, std::memory_order_acq_rel);
          } else {
            pinned.push_back({slot, hdr});
          }
          cur = nxt;
        }
      }

      // Open TributaryDB objects after releasing the lock.
      for (auto& pe : pinned) {
        PinnedSource s;
        s._slot = pe.slot;
        s._trib_db = std::make_unique<TributaryDB>(
            _cdb->_storage, pe.db_header, "_tributary_read");
        s._trib_txn = s._trib_db->txn_ref();
        s._trib_cursor = std::make_unique<_Cursor<TribCursorTraits>>(
            &*s._trib_db, &s._trib_txn->root);
        auto* ttxn = static_cast<TxnType*>(&*s._trib_txn);
        s._del_cursor = std::make_unique<_Cursor<TribCursorTraits>>(
            &*s._trib_db, &ttxn->delete_root);
        _sources.push_back(std::move(s));
      }
    }

    // If there is an active write tributary, also add it as a source
    // (reads see own uncommitted state)
    if (_write_cursor) {
      PinnedSource s;
      s._is_main = false;
      s._trib_db = std::make_unique<TributaryDB>(
          _cdb->_storage, _write_slot->db_header, "_tributary_write_read");
      // For own writes we read directly from the active txn root
      auto* ttxn = static_cast<TxnType*>(&*_write_cursor->_txn);
      s._trib_cursor = std::make_unique<_Cursor<TribCursorTraits>>(&*s._trib_db,
                                                                   &ttxn->root);
      s._del_cursor = std::make_unique<_Cursor<TribCursorTraits>>(
          &*s._trib_db, &ttxn->delete_root);
      _sources.push_back(std::move(s));
    }

    _sources_valid = true;
  }

  void _release_sources() {
    _sources.clear();
    _sources_valid = false;
  }

  // Gather candidates for `key` from all sources, apply conflict policy.
  // Returns true and fills value_out if found; false if not found/deleted.

  // Helper: search a single PinnedSource for key and return a Candidate if
  // found.
  std::optional<Candidate> _search_source(PinnedSource& src,
                                          const std::string& key) {
    if (src._is_main) {
      src._main_cursor->find(Slice(key));
      if (src._main_cursor->is_valid() && src._main_cursor->current_key == key)
        return Candidate{src._main_txn->txn_id, src._main_cursor->value(),
                         false};
    } else if (src._trib_cursor) {
      // Bloom filter: if the key was never written to this tributary, skip
      // both trie searches entirely.  A miss is definitive; a hit just
      // means we proceed to the trie (may be a false positive).
      if (src._slot && !src._slot->bloom_test(key)) return std::nullopt;
      src._trib_cursor->find(Slice(key));
      bool found =
          src._trib_cursor->is_valid() && src._trib_cursor->current_key == key;
      bool deleted = false;
      if (src._del_cursor) {
        src._del_cursor->find(Slice(key));
        deleted =
            src._del_cursor->is_valid() && src._del_cursor->current_key == key;
      }
      if (found || deleted) {
        tid_t txn_id = src._trib_txn   ? src._trib_txn->txn_id
                       : _write_cursor ? _write_cursor->_txn->txn_id
                                       : tid_t(0);
        return Candidate{txn_id, found ? src._trib_cursor->value() : Slice(),
                         deleted};
      }
    }
    return std::nullopt;
  }

  bool _resolve_key(const std::string& key, Slice& value_out) {
    _ensure_sources();
    const int n = static_cast<int>(_sources.size());

    // Fast path: main DB only — no active tributaries
    if (n == 1) {
      auto& src = _sources[0];
      src._main_cursor->find(Slice(key));
      if (src._main_cursor->is_valid() &&
          src._main_cursor->current_key == key) {
        value_out = src._main_cursor->value();
        return true;
      }
      return false;
    }

    // Multi-source path: serial scan, candidates collected into reused buffer.
    _candidates.clear();
    for (int i = 0; i < n; ++i) {
      auto opt = _search_source(_sources[i], key);
      if (opt) _candidates.push_back(*opt);
    }

    if (_candidates.empty()) return false;
    int winner = _policy.resolve(key, _candidates);
    if (winner < 0) return false;
    value_out = _candidates[winner].value;
    return true;
  }

  // -------------------------------------------------------------------------
  // Public read API  (same interface as _Cursor / _CursorBase)
  // -------------------------------------------------------------------------

  // current_key is valid (and value() non-empty) when is_valid() == true.
  std::string current_key;

  bool is_valid() const { return _valid; }

  Slice key()   const { return Slice(current_key); }
  Slice value() const { return Slice(_value_storage); }

  // Position cursor on key.  is_valid() is true iff the key exists and is
  // not deleted.  Matching the normal _Cursor contract: no return value.
  void find(const Slice& key) {
    _search_key.assign(key.data(), key.size());
    Slice found_val;
    if (_resolve_key(_search_key, found_val)) {
      current_key = _search_key;
      _value_storage.assign(
          reinterpret_cast<const char*>(found_val.data()),
          found_val.size());
      _valid = true;
    } else {
      _valid = false;
    }
  }

  // -------------------------------------------------------------------------
  // Ordered iteration — N-way merge
  // -------------------------------------------------------------------------

  void first() {
    _ensure_sources();
    for (auto& src : _sources) {
      if (src._is_main)
        src._main_cursor->first();
      else if (src._trib_cursor)
        src._trib_cursor->first();
    }
    _advance_to_next_valid(true);
  }

  void next() {
    if (!_valid) return;
    // Advance all sources that are currently AT current_key
    for (auto& src : _sources) {
      bool at_key = false;
      if (src._is_main && src._main_cursor->is_valid())
        at_key = src._main_cursor->current_key == current_key;
      else if (src._trib_cursor && src._trib_cursor->is_valid())
        at_key = src._trib_cursor->current_key == current_key;
      if (at_key) {
        if (src._is_main)
          src._main_cursor->next();
        else if (src._trib_cursor)
          src._trib_cursor->next();
      }
    }
    _advance_to_next_valid(false);
  }

  void last() {
    _ensure_sources();
    for (auto& src : _sources) {
      if (src._is_main)
        src._main_cursor->last();
      else if (src._trib_cursor)
        src._trib_cursor->last();
    }
    _advance_to_prev_valid(true);
  }

  void prev() {
    if (!_valid) return;
    for (auto& src : _sources) {
      bool at_key = false;
      if (src._is_main && src._main_cursor->is_valid())
        at_key = src._main_cursor->current_key == current_key;
      else if (src._trib_cursor && src._trib_cursor->is_valid())
        at_key = src._trib_cursor->current_key == current_key;
      if (at_key) {
        if (src._is_main)
          src._main_cursor->prev();
        else if (src._trib_cursor)
          src._trib_cursor->prev();
      }
    }
    _advance_to_prev_valid(false);
  }

  // Find the minimum current key across all valid source cursors.
  // Collects all candidates at that key and resolves via policy.
  // Skips tombstones (sets _valid = false when exhausted).
  void _advance_to_next_valid(bool /*from_first*/) {
    for (;;) {
      // Find minimum key
      const std::string* min_key = nullptr;
      for (auto& src : _sources) {
        const std::string* k = nullptr;
        if (src._is_main && src._main_cursor && src._main_cursor->is_valid())
          k = &src._main_cursor->current_key;
        else if (src._trib_cursor && src._trib_cursor->is_valid())
          k = &src._trib_cursor->current_key;
        if (k && (!min_key || *k < *min_key)) min_key = k;
      }
      if (!min_key) {
        _valid = false;
        return;
      }

      std::string candidate_key = *min_key;
      std::vector<Candidate> cands;
      for (auto& src : _sources) {
        bool at_key = false;
        if (src._is_main && src._main_cursor->is_valid())
          at_key = src._main_cursor->current_key == candidate_key;
        else if (src._trib_cursor && src._trib_cursor->is_valid())
          at_key = src._trib_cursor->current_key == candidate_key;

        if (at_key) {
          Candidate c;
          bool deleted = false;
          if (src._del_cursor) {
            src._del_cursor->find(Slice(candidate_key));
            deleted = src._del_cursor->is_valid() &&
                      src._del_cursor->current_key == candidate_key;
          }
          if (src._is_main) {
            c.txn_id = src._main_txn->txn_id;
            c.value = src._main_cursor->value();
          } else {
            c.txn_id =
                src._trib_txn
                    ? src._trib_txn->txn_id
                    : (_write_cursor ? _write_cursor->_txn->txn_id : tid_t(0));
            c.value = src._trib_cursor->value();
          }
          c.is_deleted = deleted;
          cands.push_back(c);
        }
      }

      // Advance all sources at this key
      for (auto& src : _sources) {
        bool at_key = false;
        if (src._is_main && src._main_cursor->is_valid())
          at_key = src._main_cursor->current_key == candidate_key;
        else if (src._trib_cursor && src._trib_cursor->is_valid())
          at_key = src._trib_cursor->current_key == candidate_key;
        if (at_key) {
          if (src._is_main)
            src._main_cursor->next();
          else if (src._trib_cursor)
            src._trib_cursor->next();
        }
      }

      int winner = _policy.resolve(candidate_key, cands);
      if (winner >= 0) {
        current_key = candidate_key;
        _value_storage.assign(
            reinterpret_cast<const char*>(cands[winner].value.data()),
            cands[winner].value.size());
        _valid = true;
        return;
      }
      // Tombstone — continue to next key
    }
  }

  void _advance_to_prev_valid(bool /*from_last*/) {
    for (;;) {
      const std::string* max_key = nullptr;
      for (auto& src : _sources) {
        const std::string* k = nullptr;
        if (src._is_main && src._main_cursor && src._main_cursor->is_valid())
          k = &src._main_cursor->current_key;
        else if (src._trib_cursor && src._trib_cursor->is_valid())
          k = &src._trib_cursor->current_key;
        if (k && (!max_key || *k > *max_key)) max_key = k;
      }
      if (!max_key) {
        _valid = false;
        return;
      }

      std::string candidate_key = *max_key;
      std::vector<Candidate> cands;
      for (auto& src : _sources) {
        bool at_key = false;
        if (src._is_main && src._main_cursor->is_valid())
          at_key = src._main_cursor->current_key == candidate_key;
        else if (src._trib_cursor && src._trib_cursor->is_valid())
          at_key = src._trib_cursor->current_key == candidate_key;

        if (at_key) {
          Candidate c;
          bool deleted = false;
          if (src._del_cursor) {
            src._del_cursor->find(Slice(candidate_key));
            deleted = src._del_cursor->is_valid() &&
                      src._del_cursor->current_key == candidate_key;
          }
          if (src._is_main) {
            c.txn_id = src._main_txn->txn_id;
            c.value = src._main_cursor->value();
          } else {
            c.txn_id =
                src._trib_txn
                    ? src._trib_txn->txn_id
                    : (_write_cursor ? _write_cursor->_txn->txn_id : tid_t(0));
            c.value = src._trib_cursor->value();
          }
          c.is_deleted = deleted;
          cands.push_back(c);
        }
      }

      // Move all sources at this key backward
      for (auto& src : _sources) {
        bool at_key = false;
        if (src._is_main && src._main_cursor->is_valid())
          at_key = src._main_cursor->current_key == candidate_key;
        else if (src._trib_cursor && src._trib_cursor->is_valid())
          at_key = src._trib_cursor->current_key == candidate_key;
        if (at_key) {
          if (src._is_main)
            src._main_cursor->prev();
          else if (src._trib_cursor)
            src._trib_cursor->prev();
        }
      }

      int winner = _policy.resolve(candidate_key, cands);
      if (winner >= 0) {
        current_key = candidate_key;
        _value_storage.assign(
            reinterpret_cast<const char*>(cands[winner].value.data()),
            cands[winner].value.size());
        _valid = true;
        return;
      }
    }
  }

  static uint64_t _current_time() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  // Write slot header snapshot (call before resetting _write_trib)
  offset_t _write_trib_header() const { return _write_slot->db_header; }
};

}  // namespace leaves

#endif  // _LEAVES_CONFLUENCE_DB_HPP
