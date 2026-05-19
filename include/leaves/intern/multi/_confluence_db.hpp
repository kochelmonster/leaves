#ifndef _LEAVES_CONFLUENCE_DB_HPP
#define _LEAVES_CONFLUENCE_DB_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <iostream>
#include <vector>

#include "../db/_check.hpp"
#include "_tributary.hpp"

namespace leaves {

// RAII guard that releases the +1 refs taken by _DB::txn_ref().  Exception
// safe; correctly handles moves between Tributary / main-DB pins inside
// _do_merge.
template <typename TxnPtr>
struct _TxnPinGuard {
  TxnPtr _p;
  _TxnPinGuard() = default;
  explicit _TxnPinGuard(TxnPtr p) : _p(p) {}
  _TxnPinGuard(const _TxnPinGuard&) = delete;
  _TxnPinGuard& operator=(const _TxnPinGuard&) = delete;
  _TxnPinGuard(_TxnPinGuard&& o) noexcept : _p(o._p) { o._p.reset(); }
  _TxnPinGuard& operator=(_TxnPinGuard&& o) noexcept {
    if (this != &o) {
      release();
      _p = o._p;
      o._p.reset();
    }
    return *this;
  }
  ~_TxnPinGuard() { release(); }
  void release() {
    if (_p) {
      _p->refs.fetch_sub(1, std::memory_order_acq_rel);
      _p.reset();
    }
  }
};

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
// _ConfluenceDBHeader: header stored in shared mmap for _ConfluenceDB
// =============================================================================
// Extends _DBHeader with the tributary slot linked-list head and a
// cross-process lock that guards list traversal/mutation.
//
// Locking invariant for the tributary slot list rooted at extra_offset:
//   * Every WRITE to extra_offset OR to any reachable slot->next MUST be
//     performed under slot_list_lock.
//   * Every READ that walks extra_offset / slot->next MUST hold
//     slot_list_lock for the entire walk.
//   * Per-slot state/refs/bloom are atomic / per-slot owned and may be
//     accessed without slot_list_lock once a slot pointer has been obtained.
template <typename Storage_>
struct _ConfluenceDBHeader : public _DBHeader<Storage_> {
  using Mutex = typename Storage_::Mutex;
  offset_t extra_offset{0};  // head of tributary slot linked list (see above)
  Mutex slot_list_lock;      // guards extra_offset + slot->next per invariant
};

// =============================================================================
// _ConfluenceDB: Multiproducer LSM meta-database
// =============================================================================
// - Manages a list of _TributaryDB instances via extra_offset.
// - Each producer claims a free _TributarySlot; writes go into the tributary.
// - A background thread merges tributaries that exceed write_count threshold
//   or have been idle for longer than idle_timeout_seconds.
// - Reads (find / iteration) merge-scan main DB + all live tributaries.

template <typename Storage_, typename ConflictPolicy_ = _DefaultConflictPolicy>
struct _ConfluenceDB
    : public _DB<Storage_, _Transaction<typename Storage_::Traits>,
                 _ConfluenceDBHeader<Storage_>,
                 _ConfluenceDB<Storage_, ConflictPolicy_>> {
  using Traits = typename Storage_::Traits;
  using Base = _DB<Storage_, _Transaction<Traits>, _ConfluenceDBHeader<Storage_>,
                   _ConfluenceDB<Storage_, ConflictPolicy_>>;
  using ConflictPolicy = ConflictPolicy_;
  using TributaryDB = _TributaryDB<Storage_>;
  using Slot = _TributaryHeader<Storage_>;
  using slot_ptr = typename Traits::template Pointer<Slot>;
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

  // Incremented each time a tributary slot is unlinked from the list
  // (merge completed).  Read cursors compare against this to detect when
  // their cached source snapshot is stale and needs a cheap rebuild.
  std::atomic<uint64_t> _merge_epoch{0};

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
    _ensure_read_workers();
    if (auto_monitor) start_monitor();
  }

  _ConfluenceDB(Storage_& storage, offset_t* header, std::string_view name,
                bool auto_monitor = true)
      : Base(storage, header, name) {
    new (&this->_header->slot_list_lock) typename Storage_::Mutex();
    _ensure_read_workers();
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
    // Snapshot the list under _slot_list_lock: merge_tributary() calls
    // _unlink_slot() which modifies the list, so we must not walk it live.
    std::vector<slot_ptr> to_merge;
    {
      std::scoped_lock lock(this->_header->slot_list_lock);
      offset_t cur = this->_header->extra_offset;
      while (cur) {
        auto slot = this->template resolve<Slot>(&cur);
        offset_t next_off = slot->next;
        if (slot->state.load(std::memory_order_relaxed) == Slot::FREE)
          to_merge.push_back(slot);
        cur = next_off;
      }
    }
    for (auto& slot : to_merge)
      merge_tributary(slot);
    this->flush();
  }

  // -------------------------------------------------------------------------
  // Sanitize: crash recovery — merge any unclaimed tributaries on reopen
  // -------------------------------------------------------------------------
  void sanitize() {
    new (&this->_header->slot_list_lock) typename Storage_::Mutex();
    Base::sanitize();
    _merge_unclaimed_tributaries();
    this->flush();
  }

  // =========================================================================
  // Tributary slot management
  // =========================================================================

  // Allocate a brand-new tributary: creates a _TributaryDB whose header IS
  // the slot descriptor.  No separate slot area is needed.
  // Initialises state = WRITING and refs = 1 (writer's pin) before returning.
  //
  // Must be called under _header->slot_list_lock (caller holds it).
  slot_ptr _alloc_new_slot() {
    // Create a new TributaryDB.  The constructor with offset_t* allocates
    // the first area and writes the header offset into trib_off.
    offset_t trib_off{0};
    {
      std::scoped_lock flock(this->_storage.file_lock());
      TributaryDB trib(this->_storage, &trib_off, "_tributary");
      // trib destructor is harmless — it does not return areas
    }

    auto new_slot = this->template resolve<Slot>(&trib_off, WRITE);

    // Zero the slot extension fields.  (The header area may be a recycled area
    // whose tail bytes were left over from a previous tributary.)
    new_slot->next           = 0;
    new_slot->last_used_time = 0;
    new_slot->write_count    = 0;
    new_slot->bloom_count    = 0;
    new_slot->_bloom_pad     = 0;
    memset(new_slot->bloom, 0, Slot::BLOOM_BYTES);
    new (&new_slot->state) std::atomic<uint8_t>(Slot::WRITING);
    new (&new_slot->refs)  std::atomic<uint32_t>(1);  // writer holds the initial pin

    // Prepend to linked list (extra_offset = head)
    new_slot->next = this->_header->extra_offset;
    this->_header->extra_offset = trib_off;

    this->make_dirty(new_slot);
    this->make_dirty(this->_header);
    this->flush();

    return new_slot;
  }

  // Scan the slot list starting at `start` and CAS the first FREE slot to
  // WRITING. Returns the claimed slot, or a null pointer if none found.
  // Slots that have reached the merge threshold are skipped — they are
  // considered "full" and must be merged before being reused.
  slot_ptr _try_claim_free_slot(offset_t start) {
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    offset_t cur = start;
    while (cur) {
      auto slot = this->template resolve<Slot>(&cur);
      uint8_t expected = Slot::FREE;
      if (slot->write_count < threshold &&
          slot->state.compare_exchange_strong(expected, Slot::WRITING,
                                               std::memory_order_acq_rel)) {
        slot->refs.fetch_add(1, std::memory_order_acq_rel);  // writer's pin
        return slot;
      }
      cur = slot->next;
    }
    return slot_ptr{nullptr};
  }

  // Walk the slot list, CAS a free slot to WRITING.
  // If none found, allocates a new slot.
  // Returns the claimed slot pointer.
  slot_ptr claim_tributary() {
    // Hold slot_list_lock for the entire claim operation.
    //
    // A fully lockless first pass is unsafe because a slot's area can be
    // freed (after merge) and immediately reallocated as a trie page by a
    // concurrent write transaction.  If the scanner follows a stale
    // slot->next pointer into a reallocated trie page it reads garbage as a
    // slot header, which can corrupt the slot list or claim an invalid slot.
    //
    // slot_list_lock is held only for brief pointer-walk operations, so
    // contention is negligible.
    std::scoped_lock lock(this->_header->slot_list_lock);

    // Try to CAS an existing free slot first.
    if (auto slot = _try_claim_free_slot(this->_header->extra_offset)) {
      return slot;
    }

    // _alloc_new_slot initialises state = WRITING before linking the slot.
    return _alloc_new_slot();
  }

  void release_tributary(slot_ptr slot) {
    uint32_t wc = slot->write_count;
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    bool should_merge = (wc >= threshold) &&
                        !_monitor_cancelled.load(std::memory_order_relaxed);
    if (should_merge) {
      // WRITING → MERGING: set state before dropping the writer's pin so no
      // new writer can claim the slot in the window between the store and the
      // scheduled merge call.
      slot->state.store(Slot::MERGING, std::memory_order_release);
      slot->refs.fetch_sub(1, std::memory_order_acq_rel);  // writer unpins
      // Schedule an immediate merge on this specific slot.
      this->_storage.schedule_after(std::chrono::seconds(0),
                                    [this, slot]() mutable {
                                      merge_tributary(slot);
                                    });
    } else {
      // WRITING → FREE
      slot->refs.fetch_sub(1, std::memory_order_acq_rel);  // writer unpins
      slot->state.store(Slot::FREE, std::memory_order_release);
    }
  }

  // Merge one tributary into the main DB and free its slot + areas.
  //
  // Accepts slots in either FREE or MERGING state:
  //   FREE   → this call transitions it to MERGING (idle-timeout / merge_all path).
  //   MERGING → already transitioned by the writer (threshold path); proceed.
  //   WRITING / MERGED → not eligible, returns false.
  //
  // Ownership: each MERGING slot has exactly one merger.  The writer-threshold
  // path establishes ownership by being the one to set WRITING→MERGING in
  // release_tributary() and then scheduling exactly one merge_tributary() call.
  // The FREE→MERGING path establishes ownership via the CAS below.  Callers
  // must therefore never invoke merge_tributary() on a slot that is already
  // MERGING unless they are that scheduled callback.
  //
  // The merger holds its own `refs` pin for the duration of the merge.
  // Readers may be active concurrently — the pin keeps the slot areas alive.
  // There is NO spin-wait: areas are freed by _free_slot() only when the last
  // user (writer, reader, or merger itself) decrements refs to 0 with
  // state == MERGED.
  bool merge_tributary(slot_ptr slot) {
    uint8_t expected = Slot::FREE;
    bool transitioned = slot->state.compare_exchange_strong(
        expected, Slot::MERGING, std::memory_order_acq_rel);
    if (!transitioned) {
      // expected now holds the actual state
      if (expected != Slot::MERGING) return false;  // WRITING or MERGED — skip
      // else: already MERGING by writer-threshold path; we are the scheduled
      // callback that owns this slot.  See ownership note above.
    }

    // Merger pins the slot.  Pages remain valid as long as refs > 0.
    slot->refs.fetch_add(1, std::memory_order_acq_rel);

    _do_merge(slot);

    // Remove slot from linked list before transitioning to MERGED so that
    // no new reader can observe this slot and try to pin it.
    {
      std::scoped_lock lock(this->_header->slot_list_lock);
      _unlink_slot(slot);
    }

    // Signal read cursors that the source list has shrunk.  Readers that
    // cached a stale snapshot will detect this on their next find() and
    // rebuild with one fewer (or zero) tributaries.
    _merge_epoch.fetch_add(1, std::memory_order_release);

    // Transition to terminal state.  Any reader that already holds a pin will
    // keep the slot areas alive; _free_slot() runs when refs drops to 0.
    slot->state.store(Slot::MERGED, std::memory_order_release);

    // Merger unpins — may immediately call _free_slot if no readers are pinned.
    _unpin_slot(slot);
    return true;
  }

  // Decrement the unified ref-count.  When refs reaches 0 AND state == MERGED,
  // all users have released their pin, so it is safe to free the slot's areas.
  void _unpin_slot(slot_ptr slot) {
    if (slot->refs.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        slot->state.load(std::memory_order_acquire) == Slot::MERGED) {
      _free_slot(slot);
    }
  }

  // Free all mmap areas belonging to this tributary (including the header,
  // which IS the slot descriptor).  Only called when refs == 0 &&
  // state == MERGED — guaranteed single-caller.
  void _free_slot(slot_ptr slot) {
    // The slot IS the TributaryDB header.  return_areas() frees every area
    // belonging to this tributary, including the header area itself.
    offset_t hdr_off = (uint64_t)this->_storage.resolve(slot);
    {
      TributaryDB trib(this->_storage, hdr_off, "_tributary_free");
      std::scoped_lock flock(this->_storage.file_lock());
      trib.return_areas();
      // trib destructor is harmless — it does not return areas
    }

    this->make_dirty(this->_header);
    this->flush();
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
    std::vector<slot_ptr> eligible;
    {
      std::scoped_lock lock(this->_header->slot_list_lock);
      offset_t cur = this->_header->extra_offset;
      while (cur) {
        auto slot = this->template resolve<Slot>(&cur);
        offset_t next_off = slot->next;
        if (slot->state.load(std::memory_order_relaxed) == Slot::FREE) {
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

  // Ensure the thread pool has enough workers for parallel tributary reads.
  // Called once from the constructor; idempotent if the pool is already large.
  void _ensure_read_workers() {
    // Use hardware_concurrency / 2 workers, clamped to [2, 16].
    // Tributaries are searched in parallel via submit_imm(), so we need at
    // least as many workers as the expected number of live tributaries.
#ifndef __EMSCRIPTEN__
    size_t n = std::max<size_t>(2, std::thread::hardware_concurrency() / 2);
    n = std::min<size_t>(n, 16u);
    this->_storage.ensure_pool(n);
#endif
  }

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
    // Loop: the monitor body may reschedule itself between our exchange and
    // the rescheduled job actually running, so we cancel repeatedly until
    // _monitor_job_id settles at 0 (which happens only after the monitor
    // body observes _monitor_cancelled == true and exits the reschedule
    // branch).
    for (;;) {
      uint64_t job_id = _monitor_job_id.exchange(0, std::memory_order_acq_rel);
      if (!job_id) break;
      if (job_id != UINT64_MAX) this->_storage.cancel_job(job_id);
      // Yield: if the monitor body is currently mid-execution it will set
      // _monitor_job_id again after we zero it; loop until it stops.
      std::this_thread::yield();
    }
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
  void _do_merge(slot_ptr slot) {
    using TxnType = typename TributaryDB::Transaction;
    using TribCursorTraits = typename TributaryDB::CursorTraits;
    using MainCursor = typename Base::Cursor;

    // Open the tributary (the slot IS its header)
    offset_t trib_off = (uint64_t)this->_storage.resolve(slot);
    TributaryDB trib(this->_storage, trib_off, "_tributary_merge");

    // Pin the tributary read snapshot for the whole merge (RAII).  We read
    // trib.txn()->root and trib.txn()->delete_root via bare offsets below;
    // without this pin the tributary's page recycler could free those pages
    // out from under the merger.  Main-DB snapshot is pinned implicitly by
    // create_cursor() (the _TransactionalCursor ctor calls update() which
    // takes a txn_ref).
    _TxnPinGuard<typename TributaryDB::txn_ptr> trib_pin(trib.txn_ref());

    // Capture txn_ids before start_transaction() mutates the committed txn
    tid_t main_txn_id = this->txn()->txn_id;
    tid_t trib_txn_id = trib.txn()->txn_id;

    // Use create_cursor() rather than constructing MainCursor directly: it
    // installs the aspect context AND, via the ctor's update() call, pins the
    // main DB committed txn (refs++).  Without that pin, _start_txn_id could
    // advance past the snapshot the merger reads from while we are still
    // walking it, allowing the allocator to recycle pages out from under us.
    auto main_cursor_ptr = this->create_cursor();

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
    // trib_pin / main_cursor_ptr destructors release their refs (RAII).
  }

  // Remove slot from the extra_offset linked list.
  // Must be called under _header->slot_list_lock.
  void _unlink_slot(slot_ptr target_slot) {
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
  //
  // PRECONDITION: pool is idle (no background monitor / no concurrent
  // claim_tributary).  Currently invoked only from sanitize() on reopen,
  // before start_monitor() runs.  Resetting state on a slot that another
  // thread legitimately moved to MERGING would break ownership; the FREE
  // reset below is therefore guarded against that.
  void _merge_unclaimed_tributaries() {
    offset_t cur = this->_header->extra_offset;
    while (cur) {
      auto slot = this->template resolve<Slot>(&cur);
      offset_t next_off = slot->next;
      uint8_t state = slot->state.load(std::memory_order_relaxed);
      // Skip WRITING (active writer in another process) and MERGING (already
      // owned by an active merger).  Only FREE and MERGED slots are safe to
      // reset and re-merge during crash recovery.
      if (state == Slot::FREE || state == Slot::MERGED) {
        // Reset atomics that may hold stale kernel-state-free hardware values
        new (&slot->state) std::atomic<uint8_t>(Slot::FREE);
        new (&slot->refs) std::atomic<uint32_t>(0);
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
  using Slot = _TributaryHeader<Storage_>;
  using slot_ptr = typename Traits::template Pointer<Slot>;
  using TribCursorTraits = typename TributaryDB::CursorTraits;
  using TribTxnPtr = typename TributaryDB::txn_ptr;

  // For main DB source
  using MainDB = _ConfluenceDB<Storage_, ConflictPolicy_>;
  using MainCursorTraits = typename MainDB::CursorTraits;
  using MainTxnPtr = typename MainDB::txn_ptr;

  // Source type discriminator
  bool _is_main{false};

  // Back-pointer to the owning ConfluenceDB; needed to call _unpin_slot().
  MainDB* _cdb{nullptr};

  // Main DB source
  MainTxnPtr _main_txn;
  std::unique_ptr<_Cursor<MainCursorTraits>> _main_cursor;

  // Tributary source
  slot_ptr _slot;
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
        _cdb(o._cdb),
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
    if (_slot) _cdb->_unpin_slot(_slot);
    _is_main = o._is_main;
    _cdb = o._cdb;
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
      _cdb->_unpin_slot(_slot);
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
//   1. Increment refs (under _slot_list_lock to prevent use-after-free of
//      the slot pointer itself).
//   2. Check state.  Include the slot if state is FREE, WRITING, or MERGING.
//      Skip (and immediately unpin) if state is MERGED.
//   3. While refs > 0 the slot's tributary areas are guaranteed live.
//   4. Release pin in ~_PinnedSource() via _unpin_slot(), which calls
//      _free_slot() when refs drops to 0 with state == MERGED.

template <typename Storage_, typename ConflictPolicy_ = _DefaultConflictPolicy>
struct _ConfluenceCursor {
  using ConfluenceDB = _ConfluenceDB<Storage_, ConflictPolicy_>;
  using TributaryDB = _TributaryDB<Storage_>;
  using Traits = typename Storage_::Traits;
  using Slot = _TributaryHeader<Storage_>;
  using slot_ptr = typename Traits::template Pointer<Slot>;
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
  slot_ptr _write_slot;
  std::unique_ptr<TributaryDB> _write_trib;
  std::shared_ptr<TribCursor> _write_cursor;
  uint32_t _pending_write_keys{0};  // key count for current transaction

  // Read snapshot (built on first read op, released on next start_transaction)
  std::vector<PinnedSource> _sources;
  bool _sources_valid{false};

  // Reused buffers to avoid per-call heap allocation in the hot read path.
  std::string _search_key;  // reused key string for find()
  std::vector<Candidate> _candidates;       // reused candidate list for _resolve_key()


  // Epoch at which _sources was last built.  UINT64_MAX = "never built".
  // If _cdb->_merge_epoch differs from this, the source list is stale and
  // must be rebuilt on the next read operation.
  uint64_t _sources_epoch{UINT64_MAX};

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
    offset_t write_hdr_off = (uint64_t)_cdb->_storage.resolve(_write_slot);
    _write_trib = std::make_unique<TributaryDB>(
        _cdb->_storage, write_hdr_off, "_tributary_write");
    _write_cursor = _write_trib->create_cursor();
    if (!_write_cursor->start_transaction(non_blocking)) {
      // Non-blocking acquire failed: undo the claim so the slot is not
      // left permanently WRITING with no writer attached.
      _write_cursor.reset();
      _write_trib.reset();
      _cdb->release_tributary(_write_slot);
      _write_slot = {};
      return false;
    }
    return true;
  }

  bool commit(bool sync = false) {
    if (!_write_cursor) return false;
    // The read snapshot built by _ensure_sources() contains a write-tributary
    // PinnedSource that references _write_cursor->_txn->root/delete_root.
    // Once we commit() and release_tributary(), the underlying slot may be
    // merged and its areas freed, which would dangle those cursors.  Drop
    // the snapshot now — the next read op will rebuild it against the new
    // committed state.
    _release_sources();
    bool ok = _write_cursor->commit(sync);
    if (!ok) {
      // commit() returns false only if prepare_commit() sees a mismatched
      // txn_cursor_id — should not happen in normal operation, but as a
      // defensive measure roll back so txn_lock is released and the mmap
      // header is left in a consistent state.
      _write_cursor->rollback();
    } else {
      _write_slot->write_count += _pending_write_keys;
      _write_slot->last_used_time = _current_time();
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
    // Same reasoning as commit(): release the read snapshot before the
    // underlying write-txn refs are forcibly reset by _write_cursor->rollback().
    _release_sources();
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
    if (_sources_valid) {
      // Fast-check: if a merge completed since we last built, the source
      // list has shrunk.  Rebuild so subsequent reads skip merged tributaries
      // and eventually reach the n==1 fast path.
      uint64_t cur_epoch = _cdb->_merge_epoch.load(std::memory_order_acquire);
      if (_sources_epoch == cur_epoch) return;
      _sources.clear();
      _sources_valid = false;
    }
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
    // Pin each slot (refs++) while holding _slot_list_lock to close
    // the window where the slot area could be freed and reallocated between
    // reading the slot pointer (from extra_offset / slot->next) and the pin.
    // After pinning, refs > 0 prevents _free_slot() from running even if the
    // merger sets MERGED concurrently.  Readers may observe FREE, WRITING, or
    // MERGING slots — all are safe to read while pinned.  A slot seen as
    // MERGED after pinning must be skipped and immediately unpinned.
    {
      // Collect (slot, header_off) pairs while holding _slot_list_lock.
      struct PinEntry {
        slot_ptr slot;
        offset_t header_off;
      };
      std::vector<PinEntry> pinned;
      {
        std::scoped_lock lock(_cdb->_header->slot_list_lock);
        offset_t cur = _cdb->_header->extra_offset;
        while (cur) {
          offset_t hdr_off = cur;  // save offset before resolve
          auto slot = _cdb->template resolve<Slot>(&cur);
          // Pin BEFORE checking state to close the race where:
          //   1. merger transitions FREE→MERGING
          //   2. reader checks state (sees MERGING, would skip)
          //   3. merger finishes → MERGED → _free_slot()
          // By pinning first, we guarantee pages stay valid if we decide to
          // include the slot.
          slot->refs.fetch_add(1, std::memory_order_acq_rel);
          uint8_t state = slot->state.load(std::memory_order_acquire);
          offset_t nxt = slot->next;

          // Skip only MERGED slots (data already in main DB; areas being freed).
          // FREE, WRITING, and MERGING slots are all readable while we hold refs.
          bool skip = (state == Slot::MERGED);
          if (skip) {
            _cdb->_unpin_slot(slot);  // may trigger _free_slot if last pinner
          } else {
            pinned.push_back({slot, hdr_off});
          }
          cur = nxt;
        }
      }

      // Open TributaryDB objects after releasing the lock.
      for (auto& pe : pinned) {
        PinnedSource s;
        s._cdb = _cdb;
        s._slot = pe.slot;
        s._trib_db = std::make_unique<TributaryDB>(
            _cdb->_storage, pe.header_off, "_tributary_read");
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
    // (reads see own uncommitted state).  We deliberately do NOT take an
    // extra refs pin on the write txn: the active writer already holds a
    // refs pin via _TransactionalCursor::_set_txn, and rollback() forcibly
    // resets that txn's refs to 0 (see _TransactionalCursor::rollback) — a
    // duplicate pin would underflow.  This is safe ONLY because commit() and
    // rollback() below call _release_sources() *before* touching the write
    // txn, so this source can never outlive the underlying _txn.
    if (_write_cursor) {
      PinnedSource s;
      s._is_main = false;
      offset_t write_hdr_off = (uint64_t)_cdb->_storage.resolve(_write_slot);
      s._trib_db = std::make_unique<TributaryDB>(
          _cdb->_storage, write_hdr_off, "_tributary_write_read");
      // For own writes we read directly from the active txn root
      auto* ttxn = static_cast<TxnType*>(&*_write_cursor->_txn);
      s._trib_cursor = std::make_unique<_Cursor<TribCursorTraits>>(&*s._trib_db,
                                                                   &ttxn->root);
      s._del_cursor = std::make_unique<_Cursor<TribCursorTraits>>(
          &*s._trib_db, &ttxn->delete_root);
      _sources.push_back(std::move(s));
    }

    _sources_epoch = _cdb->_merge_epoch.load(std::memory_order_relaxed);
    _sources_valid = true;
  }

  void _release_sources() {
    _sources.clear();
    _sources_valid = false;
  }

  // Gather candidates for `key` from all sources, apply conflict policy.
  // Returns true and fills value_out if found; false if not found/deleted.

  // Helper: search a single PinnedSource for key and return a Candidate if
  // found.  Bloom pre-screening is the caller's responsibility; this method
  // does not re-test the bloom filter.
  std::optional<Candidate> _search_source(PinnedSource& src,
                                          const std::string& key) {
    if (src._is_main) {
      src._main_cursor->find(Slice(key));
      if (src._main_cursor->is_valid() && src._main_cursor->current_key == key)
        return Candidate{src._main_txn->txn_id, src._main_cursor->value(),
                         false};
    } else if (src._trib_cursor) {
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

    // Multi-source path.  Precompute the bloom hash once and reuse h1/h2 for
    // every tributary, avoiding N separate FNV-1a computations (each ~20 ns).
    // With precomputed hash each bloom probe is ~5 ns; dispatching to a pool
    // worker is counter-productive here because pool workers are contending on
    // the main-DB write lock for ongoing merges, so the tributary task would
    // queue behind them.  Keeping all work on the calling thread gives
    // predictable, low-latency access regardless of merge pressure.
    const int n_tribs = n - 1;  // _sources[0] is always main DB

    const uint64_t bh = Slot::_bloom_hash(key.data(), key.size());
    const uint32_t bh1 = static_cast<uint32_t>(bh);
    const uint32_t bh2 = static_cast<uint32_t>(bh >> 32) | 1u;

    _candidates.clear();
    auto main_r = _search_source(_sources[0], key);
    if (main_r) _candidates.push_back(*main_r);

    for (int i = 0; i < n_tribs; ++i) {
      PinnedSource& src = _sources[i + 1];
      if (src._slot && !src._slot->bloom_test(bh1, bh2)) continue;
      auto r = _search_source(src, key);
      if (r) _candidates.push_back(*r);
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
};

}  // namespace leaves

#endif  // _LEAVES_CONFLUENCE_DB_HPP
