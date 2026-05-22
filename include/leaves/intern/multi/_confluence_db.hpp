#ifndef _LEAVES_CONFLUENCE_DB_HPP
#define _LEAVES_CONFLUENCE_DB_HPP

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../core/_util.hpp"
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
  int resolve(const Slice& /*key*/,
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
template <typename ConfluenceDB_>
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
// _ConfluenceMeta: cross-process metadata stored in the extra_offset area
// =============================================================================
// The head of the tributary slot linked list and the cross-process lock that
// guards list traversal/mutation.  Lives in a dedicated mmap area pointed to
// by _DBHeader::extra_offset.
//
// Locking invariant for the tributary slot list rooted at chain_head:
//   * Every WRITE to chain_head OR to any reachable slot->next MUST be
//     performed under chain_lock.
//   * Every READ that walks chain_head / slot->next MUST hold chain_lock.
//   * Per-slot state/refs are atomic / per-slot owned and may be
//     accessed without chain_lock once a slot pointer has been obtained.
template <typename Storage_>
struct _ConfluenceMeta {
  using Mutex = typename Storage_::Mutex;
  offset_t chain_head{0};  // head of tributary slot linked list
  Mutex chain_lock;        // guards chain_head + slot->next per invariant
  std::atomic<uint64_t> merge_epoch{0};  // incremented on each tributary merge
  std::atomic<uint64_t> chain_epoch{0};  // incremented when a slot is added
};

// =============================================================================
// _ConfluenceDB: Multiproducer LSM meta-database wrapper
// =============================================================================
// Wraps a MainDB_ (typically _DB<StorageImpl>) and manages tributary slots.
// - Allocates a _ConfluenceMeta in the main DB's extra_offset area on first
//   creation; reopens it on subsequent opens.
// - Keeps an in-process vector of all live TributaryDB instances.
// - Each producer calls claim_tributary() → gets a TributaryDB* to write into.
// - A background thread merges tributaries exceeding the write_count threshold
//   or idle for longer than idle_timeout_seconds.
// - Reads merge-scan the main DB + all live tributaries.

template <typename MainDB_, typename ConflictPolicy_ = _DefaultConflictPolicy>
struct _ConfluenceDB {
  using MainDB = MainDB_;
  using Storage = typename MainDB_::Storage;
  using Traits = typename Storage::Traits;
  using ConflictPolicy = ConflictPolicy_;
  using TributaryDB = _TributaryDB<Storage>;
  using Slot = _TributaryHeader<Storage>;
  using slot_ptr = typename Traits::template Pointer<Slot>;
  using txn_ptr = typename MainDB_::txn_ptr;
  using meta_ptr = typename Traits::template Pointer<_ConfluenceMeta<Storage>>;

  // CursorTraits for the main DB cursor used in read snapshots.
  // DB = MainDB_ so _Cursor reads from the main DB trie.
  struct MainCursorTraits : public MainDB_::CursorTraits {
    typedef MainDB_ DB;
  };

  typedef _ConfluenceCursor<_ConfluenceDB<MainDB_, ConflictPolicy_>> Cursor;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  // -------------------------------------------------------------------------
  // In-process configuration
  // -------------------------------------------------------------------------
  std::atomic<uint32_t> _merge_write_threshold{20000};
  std::atomic<uint64_t> _idle_timeout_seconds{300};

  // -------------------------------------------------------------------------
  // Background merge monitor state
  // -------------------------------------------------------------------------
  std::atomic<bool> _monitor_cancelled{false};
  std::atomic<bool> _monitor_interrupt{false};
  std::atomic<uint64_t> _monitor_job_id{0};
  ConflictPolicy_ _conflict_policy;

  // -------------------------------------------------------------------------
  // Core members
  // -------------------------------------------------------------------------
  // Maximum number of live tributaries (per process). The in-process
  // mirror of the mmap chain is a fixed-capacity array so readers can
  // iterate it lock-free via _tributaries_count.
  static constexpr size_t MAX_TRIBUTARIES = 128;

  MainDB_& _main_db;
  meta_ptr _meta;
  // _tributaries[0 .. _tributaries_count) are the published live entries.
  // Writers (constructor / _alloc_new_slot / _update_tributaries) are all
  // serialized by _meta->chain_lock (cross-process). Readers do
  //   n = _tributaries_count.load(acquire);
  //   for (i = 0; i < n; ++i) use _tributaries[i];
  // The release-store of the count after the slot is assigned publishes
  // the unique_ptr to readers.
  std::array<std::unique_ptr<TributaryDB>, MAX_TRIBUTARIES> _tributaries{};
  std::atomic<size_t> _tributaries_count{0};
  // Cached chain_epoch reflecting the chain state mirrored by _tributaries.
  // A stale value only triggers a slow-path reconcile via chain_lock; never
  // a correctness issue.
  std::atomic<uint64_t> _tributaries_chain_epoch{UINT64_MAX};
  std::vector<TributaryDB*> _merge_slots;  // reused across merge calls, avoids heap

  // -------------------------------------------------------------------------
  // Constructors / Destructor
  // -------------------------------------------------------------------------
  // auto_monitor: start the background merge thread immediately.
  // auto_sanitize: on reopen, merge unclaimed tributaries (crash recovery).
  _ConfluenceDB(MainDB_& main_db, bool auto_monitor = true,
                bool auto_sanitize = true)
      : _main_db(main_db) {
    if (_main_db._header->extra_offset == 0) {
      // Optimistic first-creation path: acquire the write lock then re-check.
      auto init_cursor = _main_db.create_cursor();
      init_cursor->start_transaction();
      if (_main_db._header->extra_offset == 0) {
        // Still uninitialized: we own the allocation.
        meta_ptr meta = _main_db.template alloc_node<meta_ptr>(
            sizeof(_ConfluenceMeta<Storage>));
        _main_db._header->extra_offset = _main_db.resolve(meta);
        _main_db.make_dirty(_main_db._header);
        memset(&*meta, 0, sizeof(_ConfluenceMeta<Storage>));
        new (&meta->chain_lock) typename Storage::Mutex();
        new (&meta->merge_epoch) std::atomic<uint64_t>(0);
        new (&meta->chain_epoch) std::atomic<uint64_t>(0);
        _main_db.make_dirty(meta);
        init_cursor->commit(true);
      } else {
        // Another process committed while we waited for the write lock.
        init_cursor->rollback();
      }
    }
    // extra_offset is valid in all paths: resolve meta and rebuild local state.
    _meta = _main_db.template resolve<_ConfluenceMeta<Storage>>(
        &_main_db._header->extra_offset, READ);
    {
      // Single-threaded ctor: build the in-process mirror, then publish.
      offset_t cur = _meta->chain_head;
      size_t n = 0;
      while (cur) {
        auto slot = _main_db.template resolve<Slot>(&cur);
        offset_t next_off = slot->next;
        assert(n < MAX_TRIBUTARIES &&
               "On-disk chain exceeds MAX_TRIBUTARIES");
        _tributaries[n++] = std::make_unique<TributaryDB>(
            _main_db._storage, cur, "_tributary");
        cur = next_off;
      }
      _tributaries_count.store(n, std::memory_order_release);
      _tributaries_chain_epoch.store(
          _meta->chain_epoch.load(std::memory_order_relaxed),
          std::memory_order_release);
    }
    if (auto_sanitize) sanitize();
    _ensure_read_workers();
    if (auto_monitor) start_monitor();
  }

  ~_ConfluenceDB() {
    cancel_monitor();
    merge_all_now();
  }

  // -------------------------------------------------------------------------
  // Delegate transaction / cursor factories to main DB
  // -------------------------------------------------------------------------
  txn_ptr txn() { return _main_db.txn(); }
  txn_ptr txn_ref() { return _main_db.txn_ref(); }

  cursor_ptr create_cursor() { return std::make_shared<Cursor>(this); }

  // -------------------------------------------------------------------------
  // Configuration
  // -------------------------------------------------------------------------
  void set_merge_write_threshold(uint32_t n) {
    _merge_write_threshold.store(n, std::memory_order_relaxed);
  }

  void set_idle_timeout_seconds(uint64_t s) {
    _idle_timeout_seconds.store(s, std::memory_order_relaxed);
  }

  void merge_now() { merge_eligible_tributaries(); }

  // Merge ALL free tributaries synchronously regardless of threshold/idle.
  void merge_all_now() {
    _merge_slots.clear();
    {
      std::scoped_lock lock(_meta->chain_lock);
      size_t n = _tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        TributaryDB* trib = _tributaries[i].get();
        if (trib->_header->state.load(std::memory_order_relaxed) == Slot::FREE)
          _merge_slots.push_back(trib);
      }
    }
    for (auto* trib : _merge_slots) merge_tributary(trib);
    _main_db.flush();
  }

  // -------------------------------------------------------------------------
  // Sanitize: crash recovery
  // -------------------------------------------------------------------------
  void sanitize() {
    // Reinitialise the cross-process mutex in case the previous owner crashed.
    new (&_meta->chain_lock) typename Storage::Mutex();
    _main_db.sanitize();
    _merge_unclaimed_tributaries();
    _main_db.flush();
  }

  // =========================================================================
  // Tributary slot management
  // =========================================================================

  // Allocate a new TributaryDB.  Called under _meta->chain_lock.
  // Returns a borrowed pointer into _tributaries; state = WRITING, refs = 1.
  TributaryDB* _alloc_new_slot() {
    offset_t trib_off{0};
    {
      std::scoped_lock flock(_main_db._storage.file_lock());
      TributaryDB trib(_main_db._storage, &trib_off, "_tributary");
      // trib destructor is harmless: it does not return areas
    }

    auto new_slot = _main_db.template resolve<Slot>(&trib_off, WRITE);
    new_slot->next = 0;
    new (&new_slot->last_used_time) std::atomic<uint64_t>(0);
    new (&new_slot->write_count) std::atomic<uint32_t>(0);
    new (&new_slot->state) std::atomic<uint8_t>(Slot::WRITING);
    new (&new_slot->refs) std::atomic<uint32_t>(1);

    // Prepend to chain
    new_slot->next = _meta->chain_head;
    _meta->chain_head = trib_off;

    _main_db.make_dirty(new_slot);
    _main_db.make_dirty(_meta);
    _main_db.flush();
    _meta->chain_epoch.fetch_add(1, std::memory_order_release);

    auto trib_uptr = std::make_unique<TributaryDB>(_main_db._storage, trib_off,
                                                   "_tributary");
    TributaryDB* result = trib_uptr.get();
    // Caller holds _meta->chain_lock — writers are serialized, so we can
    // publish lock-free via a release-store of the count.
    size_t idx = _tributaries_count.load(std::memory_order_relaxed);
    assert(idx < MAX_TRIBUTARIES &&
           "MAX_TRIBUTARIES exceeded in _alloc_new_slot");
    _tributaries[idx] = std::move(trib_uptr);
    _tributaries_count.store(idx + 1, std::memory_order_release);
    _tributaries_chain_epoch.store(
        _meta->chain_epoch.load(std::memory_order_relaxed),
        std::memory_order_release);
    return result;
  }

  // Sync _tributaries with the mmap chain, adding any cross-process slots.
  // Must be called under _meta->chain_lock (writers serialized).
  void _update_tributaries() {
    uint64_t chain_epoch = _meta->chain_epoch.load(std::memory_order_acquire);
    if (chain_epoch ==
        _tributaries_chain_epoch.load(std::memory_order_relaxed))
      return;
    offset_t cur = _meta->chain_head;
    while (cur) {
      offset_t slot_off = cur;
      auto slot = _main_db.template resolve<Slot>(&cur);
      offset_t next_off = slot->next;
      size_t n = _tributaries_count.load(std::memory_order_relaxed);
      bool found = false;
      for (size_t i = 0; i < n; ++i) {
        if ((uint64_t)_main_db._storage.resolve(_tributaries[i]->_header) ==
            slot_off) {
          found = true;
          break;
        }
      }
      if (!found) {
        assert(n < MAX_TRIBUTARIES &&
               "MAX_TRIBUTARIES exceeded in _update_tributaries");
        _tributaries[n] = std::make_unique<TributaryDB>(
            _main_db._storage, slot_off, "_tributary");
        _tributaries_count.store(n + 1, std::memory_order_release);
      }
      cur = next_off;
    }
    _tributaries_chain_epoch.store(chain_epoch, std::memory_order_release);
  }

  // Sync _tributaries from the mmap chain then try a lock-free claim.
  // Must be called under _meta->chain_lock.
  TributaryDB* _try_claim_free_slot() {
    _update_tributaries();
    return _try_claim();
  }

  // Non-blocking single-attempt claim: try fast path then under chain_lock,
  // allocating a new slot if count < MAX_TRIBUTARIES.  Returns nullptr when
  // all slots are in use with no FREE slot available.
  //
  // Callers that hold source refs on existing slots MUST release those refs
  // before spinning on this function; MERGED slots cannot be recycled while
  // any cursor holds a reference, causing a deadlock at MAX_TRIBUTARIES cap.
  TributaryDB* _try_claim_or_alloc() {
    if (auto* t = _try_claim()) return t;
    std::scoped_lock lock(_meta->chain_lock);
    if (auto* trib = _try_claim_free_slot()) return trib;
    if (_tributaries_count.load(std::memory_order_acquire) < MAX_TRIBUTARIES)
      return _alloc_new_slot();
    return nullptr;
  }

  // CAS-claim the first FREE+under-threshold slot from the published
  // prefix of _tributaries. Lock-free: reads count with acquire ordering
  // (pairs with writer's release-store), then a per-slot CAS arbitrates
  // between concurrent claimers. Returns nullptr if none available.
  TributaryDB* _try_claim() {
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    size_t n = _tributaries_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
      auto& t = _tributaries[i];
      slot_ptr slot = t->_header;
      uint8_t expected = Slot::FREE;
      if (slot->write_count.load(std::memory_order_relaxed) < threshold &&
          slot->state.compare_exchange_strong(expected, Slot::WRITING,
                                              std::memory_order_acq_rel)) {
        slot->refs.fetch_add(1, std::memory_order_acq_rel);
        return t.get();
      }
    }
    return nullptr;
  }

  // Walk the chain, claim a free slot or allocate a new one.
  // Returns a borrowed TributaryDB* (state = WRITING, refs = 1).
  //
  // With MAX_TRIBUTARIES capped, if all slots are in use AND no FREE slot
  // is available, this back-offs (pause/yield/sleep) and retries until a
  // slot becomes claimable. Forward progress is provided by the background
  // merge monitor and by release_tributary()'s scheduled merge job, both
  // running on the storage thread pool.
  TributaryDB* claim_tributary() {
    unsigned spin = 0;
    for (;;) {
      // Fast path: lock-free CAS over published slots.
      if (auto* t = _try_claim()) return t;

      // Reconcile / allocate under chain_lock.
      {
        std::scoped_lock lock(_meta->chain_lock);
        if (auto* trib = _try_claim_free_slot()) return trib;
        if (_tributaries_count.load(std::memory_order_acquire) <
            MAX_TRIBUTARIES)
          return _alloc_new_slot();
      }

      // Cap reached and no FREE slot — back off and retry.
      if (spin < 16) {
#if defined(LEAVES_X86_64)
        _mm_pause();
#elif defined(LEAVES_ARM64) && defined(_MSC_VER)
        __yield();
#elif defined(LEAVES_ARM64)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
      } else if (spin < 64) {
        std::this_thread::yield();
      } else {
        unsigned shift = std::min<unsigned>(spin - 64, 10);
        std::this_thread::sleep_for(
            std::chrono::microseconds(1u << shift));
      }
      ++spin;
    }
  }

  void release_tributary(TributaryDB* trib) {
    slot_ptr slot = trib->_header;
    uint32_t wc = slot->write_count.load(std::memory_order_relaxed);
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    bool should_merge = (wc >= threshold) &&
                        !_monitor_cancelled.load(std::memory_order_relaxed);
    if (should_merge) {
      slot->state.store(Slot::MERGING, std::memory_order_release);
      slot->refs.fetch_sub(1, std::memory_order_acq_rel);
      _main_db._storage.schedule_after(
          std::chrono::seconds(0),
          [this, trib]() mutable { merge_tributary(trib); });
    } else {
      slot->refs.fetch_sub(1, std::memory_order_acq_rel);
      slot->state.store(Slot::FREE, std::memory_order_release);
    }
  }

  // Merge one tributary into the main DB and recycle its slot.
  // The slot stays linked in the chain and stays in `_tributaries`; after
  // the final unpin, `_recycle_slot` resets it back to FREE state for
  // future writers to reclaim via `_try_claim_free_slot`.
  bool merge_tributary(TributaryDB* trib) {
    slot_ptr slot = trib->_header;
    uint8_t expected = Slot::FREE;
    bool transitioned = slot->state.compare_exchange_strong(
        expected, Slot::MERGING, std::memory_order_acq_rel);
    if (!transitioned) {
      if (expected != Slot::MERGING) return false;
    }
    slot->refs.fetch_add(1, std::memory_order_acq_rel);
    _do_merge(slot);
    _meta->merge_epoch.fetch_add(1, std::memory_order_release);
    slot->state.store(Slot::MERGED, std::memory_order_release);
    _unpin_slot(trib);
    return true;
  }

  void _unpin_slot(TributaryDB* trib) {
    slot_ptr slot = trib->_header;
    if (slot->refs.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        slot->state.load(std::memory_order_acquire) == Slot::MERGED) {
      _recycle_slot(trib);
    }
  }

  // Reset the merged tributary in place so it can be re-claimed as a
  // fresh DB.  Keeps the slot's header + first single area + chain link,
  // returns the remaining areas to the storage pool, then transitions
  // state from MERGED back to FREE.  Bumps merge_epoch a second time so
  // readers re-pin against the reset slot rather than the just-merged
  // (now-empty) one.
  void _recycle_slot(TributaryDB* trib) {
    trib->reset_in_place();
    slot_ptr slot = trib->_header;
    slot->state.store(Slot::FREE, std::memory_order_release);
    _meta->merge_epoch.fetch_add(1, std::memory_order_release);
    _main_db.make_dirty(_meta);
    _main_db.flush();
  }

  void merge_eligible_tributaries() {
    uint64_t now = _current_time();
    uint32_t threshold = _merge_write_threshold.load(std::memory_order_relaxed);
    uint64_t idle_timeout =
        _idle_timeout_seconds.load(std::memory_order_relaxed);
    _merge_slots.clear();
    {
      std::scoped_lock lock(_meta->chain_lock);
      size_t n = _tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        TributaryDB* trib = _tributaries[i].get();
        slot_ptr slot = trib->_header;
        if (slot->state.load(std::memory_order_relaxed) != Slot::FREE) continue;
        bool should_merge = false;
        if (slot->write_count.load(std::memory_order_relaxed) >= threshold)
          should_merge = true;
        auto lut = slot->last_used_time.load(std::memory_order_acquire);
        if (lut > 0 && (now - lut) >= idle_timeout) should_merge = true;
        if (should_merge) _merge_slots.push_back(trib);
      }
    }
    for (auto* trib : _merge_slots) merge_tributary(trib);
  }

  // =========================================================================
  // Background monitor
  // =========================================================================

  void _ensure_read_workers() {
#ifndef __EMSCRIPTEN__
    size_t n = std::max<size_t>(2, std::thread::hardware_concurrency() / 2);
    n = std::min<size_t>(n, 16u);
    _main_db._storage.ensure_pool(n);
#endif
  }

  void start_monitor() {
    _monitor_cancelled.store(false, std::memory_order_release);
    uint64_t expected = 0;
    if (!_monitor_job_id.compare_exchange_strong(expected, UINT64_MAX,
                                                 std::memory_order_acq_rel))
      return;
    _monitor_job_id.store(
        _main_db._storage.schedule_after(std::chrono::seconds(0),
                                         [this] { _run_monitor(); }),
        std::memory_order_release);
  }

  void cancel_monitor() {
    _monitor_cancelled.store(true, std::memory_order_release);
    _monitor_interrupt.store(true, std::memory_order_release);
    for (;;) {
      uint64_t job_id = _monitor_job_id.exchange(0, std::memory_order_acq_rel);
      if (!job_id) break;
      if (job_id != UINT64_MAX) _main_db._storage.cancel_job(job_id);
      std::this_thread::yield();
    }
    if (!_main_db._storage._pool_shutdown.load(std::memory_order_acquire))
      _main_db._storage.wait_idle();
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
    if (!_monitor_cancelled.load(std::memory_order_acquire))
      merge_eligible_tributaries();
    if (!_monitor_cancelled.load(std::memory_order_acquire)) {
      _monitor_job_id.store(
          _main_db._storage.schedule_after(std::chrono::seconds(1),
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
    using MainCursor = typename MainDB_::Cursor;

    offset_t trib_off = (uint64_t)_main_db._storage.resolve(slot);
    TributaryDB trib(_main_db._storage, trib_off, "_tributary_merge");

    _TxnPinGuard<typename TributaryDB::txn_ptr> trib_pin(trib.txn_ref());

    tid_t main_txn_id = _main_db.txn()->txn_id;
    tid_t trib_txn_id = trib.txn()->txn_id;

    auto main_cursor_ptr = _main_db.create_cursor();
    main_cursor_ptr->start_transaction();

    // Pass 1: apply deletions
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
      src.clear();

      _TributaryMergePolicy<ConflictPolicy_> policy(_conflict_policy,
                                                    main_txn_id, trib_txn_id);
      _Merger<MainCursor, _Cursor<TribCursorTraits>,
              _TributaryMergePolicy<ConflictPolicy_>>
          merger(*main_cursor_ptr, src, policy);
      merger.exec();
    }

    main_cursor_ptr->commit();
  }

  // Merge all tributaries not currently claimed by a live writer.
  // Called from sanitize() before start_monitor(); pool must be idle.
  void _merge_unclaimed_tributaries() {
    std::vector<TributaryDB*> to_recover;
    {
      // Pool is idle when called from sanitize(); safe lock-free scan.
      size_t n = _tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        auto& t = _tributaries[i];
        slot_ptr slot = t->_header;
        uint8_t state = slot->state.load(std::memory_order_relaxed);
        if (state == Slot::FREE || state == Slot::MERGED) {
          new (&slot->state) std::atomic<uint8_t>(Slot::FREE);
          new (&slot->refs) std::atomic<uint32_t>(0);
          to_recover.push_back(t.get());
        }
      }
    }
    for (TributaryDB* t : to_recover) merge_tributary(t);
  }
};

// =============================================================================
// _PinnedSource: one tributary read source held by _ConfluenceCursor
// =============================================================================
// Borrows a TributaryDB* from _ConfluenceDB::_tributaries (not owned).
// The slot ref-count is incremented on construction and decremented via
// _unpin_slot() in the destructor, keeping areas alive while the cursor reads.

template <typename ConfluenceDB_>
struct _PinnedSource {
  using TributaryDB = typename ConfluenceDB_::TributaryDB;
  using Traits = typename ConfluenceDB_::Traits;
  using Slot = typename ConfluenceDB_::Slot;
  using slot_ptr = typename ConfluenceDB_::slot_ptr;
  using TribCursorTraits = typename TributaryDB::CursorTraits;
  using TxnType = typename TributaryDB::Transaction;
  using TribCursor = _TributaryCursor<TribCursorTraits>;

  ConfluenceDB_* _cdb{nullptr};
  slot_ptr _slot;
  offset_t _slot_off{0};  // mmap offset of _slot (for cross-process comparison)
  TributaryDB* _trib_db{nullptr};  // BORROWED from _cdb->_tributaries
  // _trib_cursor is a write-capable cursor. For non-writer sources it is used
  // only for navigation; its base _TransactionalCursor manages the read_txn
  // pin via its own _txn field. For the writer entry (the last source during
  // a write txn) we additionally call start_transaction()/value()/remove()/
  // commit()/rollback() on it.
  std::unique_ptr<TribCursor> _trib_cursor;
  std::unique_ptr<_Cursor<TribCursorTraits>> _del_cursor;

  _PinnedSource() = default;
  _PinnedSource(const _PinnedSource&) = delete;
  _PinnedSource& operator=(const _PinnedSource&) = delete;

  // SimplePointer copies rather than nulling on move; manually null _slot
  // to prevent double-unpin.
  _PinnedSource(_PinnedSource&& o) noexcept
      : _cdb(o._cdb),
        _slot(o._slot),
        _slot_off(o._slot_off),
        _trib_db(o._trib_db),
        _trib_cursor(std::move(o._trib_cursor)),
        _del_cursor(std::move(o._del_cursor)) {
    o._slot.reset();
  }

  _PinnedSource& operator=(_PinnedSource&& o) noexcept {
    if (this == &o) return *this;
    if (_slot) _cdb->_unpin_slot(_trib_db);
    _cdb = o._cdb;
    _slot = o._slot;
    _slot_off = o._slot_off;
    o._slot.reset();
    _trib_db = o._trib_db;
    _trib_cursor = std::move(o._trib_cursor);
    _del_cursor = std::move(o._del_cursor);
    return *this;
  }

  ~_PinnedSource() {
    if (_slot) _cdb->_unpin_slot(_trib_db);
  }

  // Release the slot pin and destroy cursors (cursor dtor releases its own
  // _txn ref). After park(), the entry is safe to reinit() with a new
  // tributary.
  void park() {
    _trib_cursor.reset();
    _del_cursor.reset();
    if (_slot) {
      _cdb->_unpin_slot(_trib_db);
      _slot.reset();
    }
    _trib_db = nullptr;
    _slot_off = 0;
  }

  // Reinitialize a parked entry for a new tributary. Constructs a fresh
  // _TributaryCursor which auto-pins the tributary's current read_txn via
  // its base update(). Precondition: _slot is null.
  void reinit(ConfluenceDB_* cdb, TributaryDB* trib, slot_ptr slot,
              offset_t slot_off) {
    _cdb = cdb;
    _trib_db = trib;
    _slot = slot;
    _slot_off = slot_off;
    _trib_cursor =
        std::make_unique<TribCursor>(trib, &trib->txn()->root);
    trib->aspect().init_cursor_context(_trib_cursor->_aspect_context);
    _refresh_del_cursor();
  }

  // (Re-)build _del_cursor pointing at the cursor's current _txn delete_root.
  // Called after reinit() and after a writer's start_transaction()/commit()/
  // rollback() since those change the cursor's _txn.
  void _refresh_del_cursor() {
    auto* ttxn = static_cast<TxnType*>(&*_trib_cursor->_txn);
    _del_cursor = std::make_unique<_Cursor<TribCursorTraits>>(
        _trib_db, &ttxn->delete_root);
  }
};

// =============================================================================
// _ConfluenceCursor
// =============================================================================
// - start_transaction(): borrows a tributary from _ConfluenceDB::_tributaries.
// - commit()/rollback(): finish write, update slot metadata.
// - find()/first()/next()/prev()/last(): N-way merge read.

template <typename ConfluenceDB_>
struct _ConfluenceCursor {
  using ConfluenceDB = ConfluenceDB_;
  using TributaryDB = typename ConfluenceDB_::TributaryDB;
  using Traits = typename ConfluenceDB_::Traits;
  using Slot = typename ConfluenceDB_::Slot;
  using slot_ptr = typename ConfluenceDB_::slot_ptr;
  using ConflictPolicy = typename ConfluenceDB_::ConflictPolicy;
  using TribCursorTraits = typename TributaryDB::CursorTraits;
  using TribCursor = _TributaryCursor<TribCursorTraits>;
  using MainCursorTraits = typename ConfluenceDB_::MainCursorTraits;
  using TxnType = typename TributaryDB::Transaction;
  using Candidate = typename ConflictPolicy::_Candidate;
  using PinnedSource = _PinnedSource<ConfluenceDB_>;
  using MainTxnPtr = typename ConfluenceDB_::txn_ptr;

  ConfluenceDB_* _cdb;
  ConflictPolicy _policy;

  // Write state.
  // No separate write cursor: when _in_transaction is true, the LAST entry
  // of _sources is the writer. Its _trib_cursor is in an active write txn
  // and is used for both reading (via the N-way merge alongside the other
  // sources) and writing (value() / remove() / commit() / rollback()).
  bool _in_transaction{false};
  uint32_t _pending_write_keys{0};

  // Main DB read snapshot — refreshed by _ensure_sources()
  MainTxnPtr _main_txn;
  std::unique_ptr<_Cursor<MainCursorTraits>> _main_cursor;

  // Tributary read sources.
  // _sources[0.._sources_n) are active (pinned).
  // _sources[_sources_n..size) are parked (slot/txn released, cursors kept).
  std::vector<PinnedSource> _sources;
  size_t _sources_n{0};
  bool _sources_valid{false};
  uint64_t _sources_merge_epoch{UINT64_MAX};
  uint64_t _sources_chain_epoch{UINT64_MAX};

  // Reused hot-path buffers (avoid per-call allocation)
  std::string _iter_key;
  std::vector<Candidate> _candidates;

  // Result state (_value_storage is a non-owning Slice into trie memory;
  // valid while _sources are held — nulled by _release_sources())
  Slice _value_storage;
  bool _valid{false};

  // Public current key (returned by key())
  std::string current_key;

  explicit _ConfluenceCursor(ConfluenceDB_* cdb) : _cdb(cdb) {
    _main_txn = _cdb->txn_ref();
    _main_cursor = std::make_unique<_Cursor<MainCursorTraits>>(
        &_cdb->_main_db, &_main_txn->root);
  }

  ~_ConfluenceCursor() {
    if (_in_transaction) rollback();
    _release_sources();
    if (_main_txn) _main_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
  }

  // -------------------------------------------------------------------------
  // Write path
  // -------------------------------------------------------------------------

  bool start_transaction(bool non_blocking = false) {
    if (_in_transaction) return true;

    // Claim a tributary slot without holding source refs.  When all
    // MAX_TRIBUTARIES slots are occupied, _try_claim_or_alloc returns nullptr
    // and we must yield _before_ re-pinning sources.  Holding source refs
    // while spinning would prevent MERGED slots from being recycled (each
    // cursor ref keeps state != FREE), creating a livelock.
    TributaryDB* trib = nullptr;
    unsigned spin = 0;
    for (;;) {
      // Pin the current tributary snapshot for reads.
      _release_sources();
      _ensure_sources();

      trib = _cdb->_try_claim_or_alloc();
      if (trib) break;

      // Cap reached: release source refs so the pool can recycle MERGED
      // slots, then back off before retrying.
      _release_sources();
      if (spin < 16) {
#if defined(LEAVES_X86_64)
        _mm_pause();
#elif defined(LEAVES_ARM64) && defined(_MSC_VER)
        __yield();
#elif defined(LEAVES_ARM64)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
      } else if (spin < 64) {
        std::this_thread::yield();
      } else {
        unsigned shift = std::min<unsigned>(spin - 64, 10);
        std::this_thread::sleep_for(
            std::chrono::microseconds(1u << shift));
      }
      ++spin;
    }

    slot_ptr slot = trib->_header;
    slot->refs.fetch_add(1, std::memory_order_acq_rel);
    offset_t slot_off =
        (uint64_t)_cdb->_main_db._storage.resolve(slot);

    if (_sources_n >= _sources.size()) _sources.emplace_back();
    auto& w = _sources[_sources_n];
    w.reinit(_cdb, trib, slot, slot_off);
    if (!w._trib_cursor->start_transaction(non_blocking)) {
      w.park();
      _cdb->release_tributary(trib);
      return false;
    }
    // The cursor's _txn changed from read_txn to the new active txn —
    // re-point _del_cursor at active_txn->delete_root so reads of the
    // writer's source observe its pending deletes.
    w._refresh_del_cursor();
    ++_sources_n;
    _in_transaction = true;
    _pending_write_keys = 0;
    return true;
  }

  bool commit(bool sync = false) {
    if (!_in_transaction) return false;
    auto& w = _sources[_sources_n - 1];
    bool ok = w._trib_cursor->commit(sync);
    if (!ok) {
      w._trib_cursor->rollback();
    } else {
      w._slot->write_count.fetch_add(_pending_write_keys,
                                     std::memory_order_relaxed);
      w._slot->last_used_time.store(_current_time(),
                                    std::memory_order_release);
    }
    TributaryDB* trib = w._trib_db;
    w.park();
    --_sources_n;
    _cdb->release_tributary(trib);
    _in_transaction = false;
    _pending_write_keys = 0;
    // Other sources are stale (no longer reflect the writer's commit, and
    // the next find should pick up a fresh snapshot including the merged
    // state). Release them so _ensure_sources() rebuilds on next access.
    _release_sources();
    return ok;
  }

  bool rollback() {
    if (!_in_transaction) return false;
    auto& w = _sources[_sources_n - 1];
    w._trib_cursor->rollback();
    TributaryDB* trib = w._trib_db;
    w.park();
    --_sources_n;
    _cdb->release_tributary(trib);
    _in_transaction = false;
    _pending_write_keys = 0;
    _release_sources();
    return true;
  }

  bool is_transaction_active() const { return _in_transaction; }

  void value(const Slice& v) {
    assert(_in_transaction);
    auto& w = _sources[_sources_n - 1];
    w._trib_cursor->value(v);
    ++_pending_write_keys;
  }

  void remove() {
    assert(_in_transaction);
    auto& w = _sources[_sources_n - 1];
    if (w._trib_cursor->is_valid()) {
      ++_pending_write_keys;
    }
    w._trib_cursor->remove();
  }

  // -------------------------------------------------------------------------
  // Read path — source management
  // -------------------------------------------------------------------------

  void _ensure_sources() {
    // Snapshot is frozen for the duration of a write transaction:
    // start_transaction() materialized it once; mid-txn refresh would let
    // the cursor observe other tributaries that committed after the
    // writer began, breaking snapshot isolation.
    if (_sources_valid && _in_transaction) return;

    uint64_t cur_merge_epoch =
        _cdb->_meta->merge_epoch.load(std::memory_order_acquire);
    uint64_t cur_chain_epoch =
        _cdb->_meta->chain_epoch.load(std::memory_order_acquire);
    if (_sources_valid && _sources_merge_epoch == cur_merge_epoch &&
        _sources_chain_epoch == cur_chain_epoch)
      return;
    _sources_valid = false;

    // Refresh main DB snapshot.
    // Pin the new txn first so the old _root pointer stays valid through
    // set_root's dereference, then release the old pin.
    auto new_txn = _cdb->txn_ref();
    _main_cursor->set_root(&new_txn->root);
    if (_main_txn) _main_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
    _main_txn = std::move(new_txn);

    // Park all old active sources before acquiring chain_lock.
    // park() → _unpin_slot() may trigger _recycle_slot which scans the
    // published _tributaries — keep that work outside chain_lock.
    for (size_t i = 0; i < _sources_n; ++i) _sources[i].park();
    _sources_n = 0;

    auto fill_sources_lockfree = [&] {
      size_t fill = 0;
      size_t n =
          _cdb->_tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        auto& t = _cdb->_tributaries[i];
        slot_ptr slot = t->_header;
        if (slot->state.load(std::memory_order_acquire) == Slot::MERGED)
          continue;
        slot->refs.fetch_add(1, std::memory_order_acq_rel);
        offset_t slot_off =
            (uint64_t)_cdb->_main_db._storage.resolve(t->_header);
        if (fill >= _sources.size()) _sources.emplace_back();
        _sources[fill].reinit(_cdb, t.get(), slot, slot_off);
        ++fill;
      }
      _sources_n = fill;
    };

    // Fast path: if the local _tributaries cache is already in sync with
    // the mmap chain, iterate lock-free.
    if (_cdb->_tributaries_chain_epoch.load(std::memory_order_acquire) ==
        cur_chain_epoch) {
      fill_sources_lockfree();
    } else {
      // Slow path: chain advanced — reconcile under chain_lock, then
      // iterate lock-free.
      {
        std::scoped_lock chain_l(_cdb->_meta->chain_lock);
        _cdb->_update_tributaries();
      }
      fill_sources_lockfree();
    }

    _sources_merge_epoch =
        _cdb->_meta->merge_epoch.load(std::memory_order_relaxed);
    _sources_chain_epoch =
        _cdb->_meta->chain_epoch.load(std::memory_order_relaxed);
    _sources_valid = true;
  }

  void _release_sources() {
    for (size_t i = 0; i < _sources_n; ++i) _sources[i].park();
    _sources_n = 0;
    _value_storage = Slice();  // null to prevent dangling
    _sources_valid = false;
  }

  bool _resolve_key(const Slice& key, Slice& value_out) {
    _ensure_sources();

    if (!_sources_n) {
      _main_cursor->find(key);
      if (_main_cursor->is_valid() && key == _main_cursor->current_key) {
        value_out = _main_cursor->value();
        return true;
      }
      return false;
    }

    _candidates.clear();

    // Serial find on all sources + main cursor on the calling thread.
    // Parallel fan-out (shared pool / per-cursor spin dispatcher) was
    // measured to regress readrandom: dispatch overhead (~2 µs shared
    // pool, ~5-10 µs spin dispatcher) dwarfs the actual find (~120 ns).
    for (size_t i = 0; i < _sources_n; ++i) {
      auto& src = _sources[i];
      src._trib_cursor->find(key);
      if (src._del_cursor) src._del_cursor->find(key);
    }
    _main_cursor->find(key);

    if (_main_cursor->is_valid() && key == _main_cursor->current_key)
      _candidates.push_back({_main_txn->txn_id, _main_cursor->value(), false});

    for (size_t _si = 0; _si < _sources_n; ++_si) {
      auto& src = _sources[_si];
      bool found =
          src._trib_cursor->is_valid() && key == src._trib_cursor->current_key;
      bool deleted = false;
      if (src._del_cursor) {
        deleted =
            src._del_cursor->is_valid() && key == src._del_cursor->current_key;
      }
      if (found || deleted) {
        _candidates.push_back(
            {src._trib_cursor->_txn->txn_id,
             found ? src._trib_cursor->value() : Slice(), deleted});
      }
    }

    if (_candidates.empty()) return false;
    int winner = _policy.resolve(key, _candidates);
    if (winner < 0) return false;
    value_out = _candidates[winner].value;
    return true;
  }

  // -------------------------------------------------------------------------
  // Public read API
  // -------------------------------------------------------------------------

  bool is_valid() const { return _valid; }
  Slice key() const { return Slice(current_key); }
  Slice value() const { return _value_storage; }

  void find(const Slice& key) {
    // Write-transaction fast path: only the writer's cursor (last source)
    // is consulted. No _ensure_sources, no _resolve_key, no delete check.
    if (_in_transaction) {
      auto& w = _sources[_sources_n - 1];
      w._trib_cursor->find(key);
      current_key.assign(key.data(), key.size());
      _valid = w._trib_cursor->is_valid() &&
               w._trib_cursor->current_key == current_key;
      return;
    }

    Slice found_val;
    if (_resolve_key(key, found_val)) {
      current_key.assign(key.data(), key.size());
      _value_storage = found_val;
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
    _main_cursor->first();
    for (size_t i = 0; i < _sources_n; ++i)
      _sources[i]._trib_cursor->first();
    _advance_to_next_valid();
  }

  void next() {
    if (!_valid) return;
    if (_main_cursor->is_valid() && _main_cursor->current_key == current_key)
      _main_cursor->next();
    for (size_t i = 0; i < _sources_n; ++i) {
      auto& src = _sources[i];
      if (src._trib_cursor->is_valid() &&
          src._trib_cursor->current_key == current_key)
        src._trib_cursor->next();
    }
    _advance_to_next_valid();
  }

  void last() {
    _ensure_sources();
    _main_cursor->last();
    for (size_t i = 0; i < _sources_n; ++i)
      _sources[i]._trib_cursor->last();
    _advance_to_prev_valid();
  }

  void prev() {
    if (!_valid) return;
    if (_main_cursor->is_valid() && _main_cursor->current_key == current_key)
      _main_cursor->prev();
    for (size_t i = 0; i < _sources_n; ++i) {
      auto& src = _sources[i];
      if (src._trib_cursor->is_valid() &&
          src._trib_cursor->current_key == current_key)
        src._trib_cursor->prev();
    }
    _advance_to_prev_valid();
  }

  void _advance_to_next_valid() {
    for (;;) {
      const std::string* min_key = nullptr;
      if (_main_cursor && _main_cursor->is_valid())
        min_key = &_main_cursor->current_key;
      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (src._trib_cursor->is_valid()) {
          if (!min_key || src._trib_cursor->current_key < *min_key)
            min_key = &src._trib_cursor->current_key;
        }
      }
      if (!min_key) {
        _valid = false;
        return;
      }

      _iter_key.assign(min_key->data(), min_key->size());
      _candidates.clear();

      if (_main_cursor->is_valid() && _main_cursor->current_key == _iter_key)
        _candidates.push_back(
            {_main_txn->txn_id, _main_cursor->value(), false});

      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (!src._trib_cursor->is_valid()) continue;
        if (src._trib_cursor->current_key != _iter_key) continue;
        bool deleted = false;
        if (src._del_cursor) {
          src._del_cursor->find(Slice(_iter_key));
          deleted = src._del_cursor->is_valid() &&
                    src._del_cursor->current_key == _iter_key;
        }
        _candidates.push_back(
            {src._trib_cursor->_txn->txn_id, src._trib_cursor->value(), deleted});
      }

      if (_main_cursor->is_valid() && _main_cursor->current_key == _iter_key)
        _main_cursor->next();
      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (src._trib_cursor->is_valid() &&
            src._trib_cursor->current_key == _iter_key)
          src._trib_cursor->next();
      }

      int winner = _policy.resolve(Slice(_iter_key), _candidates);
      if (winner >= 0) {
        current_key = _iter_key;
        _value_storage = _candidates[winner].value;
        _valid = true;
        return;
      }
    }
  }

  void _advance_to_prev_valid() {
    for (;;) {
      const std::string* max_key = nullptr;
      if (_main_cursor && _main_cursor->is_valid())
        max_key = &_main_cursor->current_key;
      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (src._trib_cursor->is_valid()) {
          if (!max_key || src._trib_cursor->current_key > *max_key)
            max_key = &src._trib_cursor->current_key;
        }
      }
      if (!max_key) {
        _valid = false;
        return;
      }

      _iter_key.assign(max_key->data(), max_key->size());
      _candidates.clear();

      if (_main_cursor->is_valid() && _main_cursor->current_key == _iter_key)
        _candidates.push_back(
            {_main_txn->txn_id, _main_cursor->value(), false});

      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (!src._trib_cursor->is_valid()) continue;
        if (src._trib_cursor->current_key != _iter_key) continue;
        bool deleted = false;
        if (src._del_cursor) {
          src._del_cursor->find(Slice(_iter_key));
          deleted = src._del_cursor->is_valid() &&
                    src._del_cursor->current_key == _iter_key;
        }
        _candidates.push_back(
            {src._trib_cursor->_txn->txn_id, src._trib_cursor->value(), deleted});
      }

      if (_main_cursor->is_valid() && _main_cursor->current_key == _iter_key)
        _main_cursor->prev();
      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (src._trib_cursor->is_valid() &&
            src._trib_cursor->current_key == _iter_key)
          src._trib_cursor->prev();
      }

      int winner = _policy.resolve(Slice(_iter_key), _candidates);
      if (winner >= 0) {
        current_key = _iter_key;
        _value_storage = _candidates[winner].value;
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
