#ifndef _LEAVES_CONFLUENCE_DB_HPP
#define _LEAVES_CONFLUENCE_DB_HPP

#include <algorithm>
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

// _DefaultConflictPolicy: highest txn_id wins
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

// Forward declarations
template <typename ConfluenceDB_>
struct _ConfluenceCursor;

// _TributaryMergePolicy: bridges _Merger's may_overwrite() with ConflictPolicy
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

// _ConfluenceMeta: cross-process metadata stored in the extra_offset area
// Holds a fixed-size array of tributary slot offsets.  Lives in a dedicated
// mmap area pointed to by _DBHeader::extra_offset.
//
// Concurrency model:
//   * slots[i] is an atomic<uint64_t> holding an absolute mmap offset (or 0
//     if never allocated).  Allocators claim a slot via CAS(0 -> off).
//   * slots_count is a monotonic non-decreasing high-water-mark hint.
//     Updated via fetch_max-style CAS after the slot offset is published.
//   * Per-slot state/refs (in _TributaryHeader) remain atomic and may be
//     read without any external lock once the slot offset is observed.
//   * chain_lock no longer guards slot allocation (per-slot CAS suffices);
//     it is retained to serialize in-process publish into _tribs[] and to
//     coordinate other multi-step bookkeeping (e.g. merge planning).
template <typename Storage_>
struct _ConfluenceMeta {
  using Mutex = typename Storage_::Mutex;
  static constexpr size_t MAX_TRIBUTARIES = 128;
  Mutex chain_lock;                       // in-process publish serialization
  std::atomic<uint64_t> state_epoch{0};   // incremented when a tributary's readable status changes:
                                          //   FIRST_WRITING→ATTACHED (first commit; new readable slot)
                                          //   MERGING→MERGED         (slot leaves readable set)
  std::atomic<uint64_t> chain_epoch{0};   // incremented when a slot is added
  std::atomic<uint32_t> slots_count{0};   // monotonic high-water-mark hint
  std::atomic<uint64_t> slots[MAX_TRIBUTARIES]{};  // claimed via CAS(0->off)
};

// _ConfluenceDB: Multiproducer LSM meta-database wrapper
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

  std::atomic<uint32_t> _merge_write_threshold{50000};
  std::atomic<uint64_t> _idle_timeout_seconds{300};

  std::atomic<bool> _monitor_cancelled{false};
  std::atomic<bool> _monitor_interrupt{false};
  std::atomic<uint64_t> _monitor_job_id{0};
  ConflictPolicy_ _conflict_policy;

  // Hard cap on tributary slots, both per-process and cross-process.  The
  // persistent slot table in _ConfluenceMeta has this exact size; the
  // allocator scans the array for the first un-claimed entry via CAS.
  static constexpr size_t MAX_TRIBUTARIES = _ConfluenceMeta<Storage>::MAX_TRIBUTARIES;

  MainDB_& _main_db;
  meta_ptr _meta;
  // Fixed in-process mirror of the persistent slots array.  Index i in
  // _tribs corresponds 1:1 with _meta->slots[i].  unique_ptr storage in
  // std::array is stable for the lifetime of the _ConfluenceDB, so the
  // lock-free reader pattern
  //   n = _tributaries_count.load(acquire);
  //   for (i = 0; i < n; ++i) if (_tribs[i]) ...
  // is safe even while a writer concurrently publishes slot n.
  std::array<std::unique_ptr<TributaryDB>, MAX_TRIBUTARIES> _tribs{};
  std::atomic<size_t> _tributaries_count{0};
  // Cached chain_epoch reflecting the chain state mirrored by _tribs.
  // A stale value only triggers a slow-path reconcile via chain_lock; never
  // a correctness issue.
  std::atomic<uint64_t> _tributaries_chain_epoch{UINT64_MAX};
  std::vector<TributaryDB*> _merge_slots;  // reused across merge calls, avoids heap

  // Async merge error propagation: merge_tributary()'s catch block stores the
  // exception here; _ensure_sources() / start_transaction() rethrow it on the
  // next cursor access (one-shot: cleared on rethrow; first-error-wins).
  std::atomic<bool> _has_merge_error{false};
  std::mutex _merge_error_mutex;
  std::exception_ptr _pending_merge_error;

  // Reader: returns the in-process TributaryDB at index i, or nullptr if
  // not yet materialised.  Caller bounds i < _tributaries_count.load(acquire).
  TributaryDB* _trib_at(size_t i) const { return _tribs[i].get(); }

  // auto_monitor: start the background merge thread immediately.
  _ConfluenceDB(MainDB_& main_db, bool auto_monitor = true)
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
        memset(reinterpret_cast<char*>(&*meta), 0, sizeof(_ConfluenceMeta<Storage>));
        new (&meta->chain_lock) typename Storage::Mutex();
        new (&meta->state_epoch) std::atomic<uint64_t>(0);
        new (&meta->chain_epoch) std::atomic<uint64_t>(0);
        new (&meta->slots_count) std::atomic<uint32_t>(0);
        for (size_t i = 0; i < _ConfluenceMeta<Storage>::MAX_TRIBUTARIES; ++i)
          new (&meta->slots[i]) std::atomic<uint64_t>(0);
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
      // Single-threaded ctor: build the in-process mirror from the
      // persistent slots array, then publish.
      uint32_t hwm = _meta->slots_count.load(std::memory_order_acquire);
      size_t published = 0;
      for (size_t i = 0; i < hwm; ++i) {
        uint64_t raw = _meta->slots[i].load(std::memory_order_acquire);
        if (!raw) continue;  // never-allocated gap (should not occur)
        offset_t slot_off;
        slot_off = raw;
        _tribs[i] = std::make_unique<TributaryDB>(
            _main_db._storage, slot_off, "_tributary");
        published = i + 1;
      }
      _tributaries_count.store(published, std::memory_order_release);
      _tributaries_chain_epoch.store(
          _meta->chain_epoch.load(std::memory_order_relaxed),
          std::memory_order_release);
    }
    sanitize();
    if (auto_monitor) start_monitor();
  }

  ~_ConfluenceDB() {
    cancel_monitor();
    merge_all_now();
  }

  txn_ptr txn() { return _main_db.txn(); }
  txn_ptr txn_ref() { return _main_db.txn_ref(); }

  cursor_ptr create_cursor() { return std::make_shared<Cursor>(this); }

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
        TributaryDB* trib = _trib_at(i);
        uint8_t s = trib->_header->state.load(std::memory_order_relaxed);
        if (s == Slot::ATTACHED || s == Slot::MERGING)
          _merge_slots.push_back(trib);
      }
    }
    for (auto* trib : _merge_slots) merge_tributary(trib);
    _main_db.flush();
  }

  void sanitize() {
    // Reinitialise the cross-process mutex in case the previous owner crashed.
    new (&_meta->chain_lock) typename Storage::Mutex();
    _main_db.sanitize();
    _merge_unclaimed_tributaries();
    _main_db.flush();
  }

  // Allocate a new TributaryDB.  Called under _meta->chain_lock for
  // in-process publish serialization; cross-process arbitration uses
  // per-slot CAS on _meta->slots[].  Returns a borrowed pointer; state =
  // WRITING, refs = 1.  Returns nullptr if all MAX_TRIBUTARIES slots are
  // already claimed (cross-process cap reached).
  TributaryDB* _alloc_new_slot() {
    // Cheap cap pre-check: slots_count is monotonic, so once it reaches
    // MAX_TRIBUTARIES all per-slot CAS attempts will fail.  Avoid the
    // expensive header allocation in that case.
    if (_meta->slots_count.load(std::memory_order_acquire) >= MAX_TRIBUTARIES)
      return nullptr;

    offset_t trib_off{0};
    {
      std::scoped_lock flock(_main_db._storage.file_lock());
      TributaryDB trib(_main_db._storage, &trib_off, "_tributary");
      // trib destructor is harmless: it does not return areas
    }

    auto new_slot = _main_db.template resolve<Slot>(&trib_off, WRITE);
    new (&new_slot->last_used_time) std::atomic<uint64_t>(0);
    new (&new_slot->write_count) std::atomic<uint32_t>(0);
    new (&new_slot->state) std::atomic<uint8_t>(Slot::FIRST_WRITING);
    new (&new_slot->refs) std::atomic<uint32_t>(1);
    _main_db.make_dirty(new_slot);

    // Per-slot CAS claim: scan _meta->slots[] for the first unclaimed entry
    // and atomically install our offset.  Allocators in concurrent processes
    // race on the CAS; only one wins each index.
    uint64_t my_off = trib_off.raw();
    size_t claimed_idx = MAX_TRIBUTARIES;
    for (size_t i = 0; i < MAX_TRIBUTARIES; ++i) {
      uint64_t expected = 0;
      if (_meta->slots[i].compare_exchange_strong(
              expected, my_off,
              std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        claimed_idx = i;
        break;
      }
    }
    if (claimed_idx == MAX_TRIBUTARIES) {
      // Cap reached: leak the just-allocated tributary header (rare).
      // The slot is unreachable, no chain corruption.
      return nullptr;
    }

    // Bump slots_count to at least claimed_idx + 1 (monotonic high-water mark).
    uint32_t target = static_cast<uint32_t>(claimed_idx + 1);
    uint32_t cur = _meta->slots_count.load(std::memory_order_relaxed);
    while (cur < target &&
           !_meta->slots_count.compare_exchange_weak(
               cur, target, std::memory_order_release,
               std::memory_order_relaxed)) {
      // cur reloaded by CAS on failure
    }

    _main_db.make_dirty(_meta);
    _main_db.flush();
    _meta->chain_epoch.fetch_add(1, std::memory_order_release);
    // No state_epoch bump here: FIRST_WRITING has no readable snapshot yet.
    // The epoch is bumped in commit() when the first FIRST_WRITING→ATTACHED transition occurs.

    // Publish into in-process mirror.  Caller holds _meta->chain_lock so
    // writers within this process are serialized; cross-process writers
    // touch different _tribs[] arrays (one per process).
    _tribs[claimed_idx] = std::make_unique<TributaryDB>(
        _main_db._storage, trib_off, "_tributary");
    TributaryDB* result = _tribs[claimed_idx].get();
    size_t pub = _tributaries_count.load(std::memory_order_relaxed);
    while (pub < claimed_idx + 1 &&
           !_tributaries_count.compare_exchange_weak(
               pub, claimed_idx + 1, std::memory_order_release,
               std::memory_order_relaxed)) {
      // pub reloaded on failure
    }
    _tributaries_chain_epoch.store(
        _meta->chain_epoch.load(std::memory_order_relaxed),
        std::memory_order_release);
    return result;
  }

  // Sync _tribs with the persistent slots array, materializing any
  // cross-process slots not yet seen in this process.
  // Must be called under _meta->chain_lock (in-process writers serialized).
  void _update_tributaries() {
    uint64_t chain_epoch = _meta->chain_epoch.load(std::memory_order_acquire);
    if (chain_epoch ==
        _tributaries_chain_epoch.load(std::memory_order_relaxed))
      return;
    uint32_t hwm = _meta->slots_count.load(std::memory_order_acquire);
    size_t max_seen = _tributaries_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < hwm; ++i) {
      if (_tribs[i]) continue;
      uint64_t raw = _meta->slots[i].load(std::memory_order_acquire);
      if (!raw) continue;  // writer mid-CAS or gap; pick up next pass
      offset_t slot_off;
      slot_off = raw;
      _tribs[i] = std::make_unique<TributaryDB>(
          _main_db._storage, slot_off, "_tributary");
      if (i + 1 > max_seen) max_seen = i + 1;
    }
    _tributaries_count.store(max_seen, std::memory_order_release);
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
  // all slots are in use and no EMPTY slot is available.
  //
  // Callers that hold source refs on existing slots MUST release those refs
  // before spinning on this function; MERGED slots cannot be recycled while
  // any cursor holds a reference, causing a deadlock at MAX_TRIBUTARIES cap.
  TributaryDB* _try_claim_or_alloc() {
    if (auto* t = _try_claim()) return t;
    std::scoped_lock lock(_meta->chain_lock);
    if (auto* trib = _try_claim_free_slot()) return trib;
    return _alloc_new_slot();  // nullptr if cap reached
  }

  // CAS-claim the first FREE or EMPTY slot from the published prefix of
  // _tributaries. Lock-free: reads count with acquire ordering (pairs with
  // writer's release-store), then a per-slot CAS arbitrates between concurrent
  // claimers. Returns nullptr if none available.
  //
  // Claim path:
  //   EMPTY → FIRST_WRITING (slot was recycled via _recycle_slot(); trie is
  //                          already reset, write_count already zeroed)
  // ATTACHED slots are exclusively owned by a cursor and cannot be claimed.
  TributaryDB* _try_claim() {
    size_t n = _tributaries_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
      TributaryDB* t = _trib_at(i);
      slot_ptr slot = t->_header;

      // EMPTY → FIRST_WRITING (recycled slot; trie already reset by
      // _recycle_slot() before state was stored as EMPTY with release ordering,
      // so the acquire on the CAS success observes the completed reset).
      uint8_t expected = Slot::EMPTY;
      if (slot->state.compare_exchange_strong(expected, Slot::FIRST_WRITING,
                                              std::memory_order_acq_rel)) {
        slot->refs.fetch_add(1, std::memory_order_acq_rel);
        return t;
      }
    }
    return nullptr;
  }

  // Walk the chain, claim a free slot or allocate a new one.
  // Returns a borrowed TributaryDB* (state = WRITING, refs = 1).
  //
  // With MAX_TRIBUTARIES capped, if all slots are in use and no EMPTY slot
  // is available, this backs off (pause/yield/sleep) and retries until a
  // slot becomes claimable. Forward progress is provided by the background
  // merge monitor running on the storage thread pool.
  TributaryDB* claim_tributary() {
    unsigned spin = 0;
    for (;;) {
      // Fast path: lock-free CAS over published slots.
      if (auto* t = _try_claim()) return t;

      // Reconcile / allocate under chain_lock.
      {
        std::scoped_lock lock(_meta->chain_lock);
        if (auto* trib = _try_claim_free_slot()) return trib;
        if (auto* trib = _alloc_new_slot()) return trib;
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

  // Merge one tributary into the main DB and recycle its slot.
  // The slot stays linked in the chain and stays in `_tributaries`; after
  // the final unpin, `_recycle_slot` resets it back to EMPTY state for
  // future writers to reclaim via `_alloc_new_slot`.
  bool merge_tributary(TributaryDB* trib) {
    slot_ptr slot = trib->_header;
    // Accept ATTACHED as starting state (in addition to already-MERGING).
    uint8_t expected = Slot::ATTACHED;
    bool transitioned = slot->state.compare_exchange_strong(
        expected, Slot::MERGING, std::memory_order_acq_rel);
    if (!transitioned) {
      if (expected != Slot::MERGING) return false;
    }
    slot->refs.fetch_add(1, std::memory_order_acq_rel);
    try {
      _do_merge(trib);
    } catch (...) {
      // Merge failed (e.g. StorageFull). Slot stays in MERGING state.
      // Recovery happens on next process start via _merge_unclaimed_tributaries()
      // once the storage region has been enlarged.
      slot->refs.fetch_sub(1, std::memory_order_acq_rel);
      // Surface the exception to the next cursor access.
      {
        std::lock_guard<std::mutex> lk(_merge_error_mutex);
        if (!_pending_merge_error) _pending_merge_error = std::current_exception();
        _has_merge_error.store(true, std::memory_order_release);
      }
      return false;
    }
    slot->state.store(Slot::MERGED, std::memory_order_release);
    _meta->state_epoch.fetch_add(1, std::memory_order_release);
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
  // state MERGED→EMPTY.  The next _try_claim_or_alloc() will use
  // _alloc_new_slot() to transition EMPTY→FIRST_WRITING for a new writer.
  void _recycle_slot(TributaryDB* trib) {
    trib->reset_in_place();
    slot_ptr slot = trib->_header;
    slot->write_count.store(0, std::memory_order_relaxed);
    slot->last_used_time.store(0, std::memory_order_relaxed);
    slot->state.store(Slot::EMPTY, std::memory_order_release);
    // No state_epoch bump: EMPTY has no readable snapshot.
    _main_db.make_dirty(_meta);
    _main_db.flush();
  }

  void merge_eligible_tributaries() {
    uint64_t now = _current_time();
    uint64_t idle_timeout =
        _idle_timeout_seconds.load(std::memory_order_relaxed);
    _merge_slots.clear();
    {
      std::scoped_lock lock(_meta->chain_lock);
      size_t n = _tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        TributaryDB* trib = _trib_at(i);
        slot_ptr slot = trib->_header;
        uint8_t state = slot->state.load(std::memory_order_relaxed);
        // Already marked for merge by a writer — pick it up directly.
        if (state == Slot::MERGING) {
          _merge_slots.push_back(trib);
          continue;
        }
        bool should_merge = false;
        if (state == Slot::ATTACHED) {
          // ATTACHED: cursor owns this slot exclusively. Only the idle timeout
          // can trigger a merge. The CAS in merge_tributary() races with the
          // cursor's ATTACHED→WRITING CAS in start_transaction(); exactly one
          // winner proceeds.
          auto lut = slot->last_used_time.load(std::memory_order_acquire);
          if (lut > 0 && (now - lut) >= idle_timeout) should_merge = true;
        }
        if (should_merge) _merge_slots.push_back(trib);
      }
    }
    for (auto* trib : _merge_slots) merge_tributary(trib);
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

  static uint64_t _current_time() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  // Recycle MERGED slots whose last reference was orphaned (e.g. the holding
  // thread died between fill_sources and _release_sources).  A 2-second grace
  // period ensures any live cursor that pinned the slot while it was MERGING
  // has already called _release_sources before we touch refs.  Called once
  // per monitor period so contention is negligible.
  void _recover_merged_slots() {
    uint64_t now = _current_time();
    size_t n = _tributaries_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
      TributaryDB* t = _trib_at(i);
      slot_ptr slot = t->_header;
      if (slot->state.load(std::memory_order_acquire) != Slot::MERGED)
        continue;
      // Only act on slots that have been MERGED for at least 2 seconds.
      uint64_t lut = slot->last_used_time.load(std::memory_order_acquire);
      if (lut == 0 || now - lut < 2)
        continue;
      // CAS refs 1 → 0: act as the final unpinner that was never called.
      // If a live cursor concurrently calls _unpin_slot and wins the CAS,
      // it will trigger recycle instead — either way exactly one recycle runs.
      uint32_t expected = 1;
      if (slot->refs.compare_exchange_strong(expected, 0,
                                             std::memory_order_acq_rel)) {
        // Force-clear stale transaction refs that may have been left by a
        // crashed process.  State=MERGED and we own the last slot ref, so
        // no live cursor should hold a txn ref on this trib; any remaining
        // refs are orphaned and must be zeroed before reset_in_place()
        // checks is_active().
        t->iter_transactions([](auto txn) -> bool {
          txn->refs.store(0, std::memory_order_relaxed);
          return false;
        });
        _recycle_slot(t);
      }
    }
  }

  void _run_monitor() {
    if (!_monitor_cancelled.load(std::memory_order_acquire)) {
      try {
        merge_eligible_tributaries();
        _recover_merged_slots();
      } catch (...) {
      }
    }
    if (!_monitor_cancelled.load(std::memory_order_acquire)) {
      _monitor_job_id.store(
          _main_db._storage.schedule_after(std::chrono::seconds(1),
                                           [this] { _run_monitor(); }),
          std::memory_order_release);
    } else {
      _monitor_job_id.store(0, std::memory_order_release);
    }
  }

  void _do_merge(TributaryDB* trib) {
    using TxnType = typename TributaryDB::Transaction;
    using TribCursorTraits = typename TributaryDB::CursorTraits;

    tid_t main_txn_id = _main_db.txn()->txn_id;
    auto main_cursor = _main_db.create_cursor();
    main_cursor->start_transaction();

    _TxnPinGuard<typename TributaryDB::txn_ptr> trib_pin(trib->txn_ref());
    tid_t trib_txn_id = trib->txn()->txn_id;

    try {
      // Pass 1: apply deletions
      auto trib_txn = trib->txn();
      auto* ttxn = static_cast<TxnType*>(&*trib_txn);
      if (ttxn->delete_root) {
        _Cursor<TribCursorTraits> del_cursor(trib, &ttxn->delete_root);
        del_cursor.first();
        while (del_cursor.is_valid()) {
          main_cursor->find(Slice(del_cursor.current_key));
          if (main_cursor->is_valid() &&
              main_cursor->current_key == del_cursor.current_key)
            main_cursor->template remove<false>();
          del_cursor.next();
        }
      }

      // Pass 2: merge tributary data trie into main DB
      _Cursor<TribCursorTraits> src(trib, &trib->txn()->root);
      src.clear();
      _TributaryMergePolicy<ConflictPolicy_> policy(_conflict_policy,
                                                    main_txn_id, trib_txn_id);
      _Merger<typename MainDB_::Cursor, _Cursor<TribCursorTraits>,
              _TributaryMergePolicy<ConflictPolicy_>>(*main_cursor, src, policy)
          .exec();

      main_cursor->commit();
    } catch (...) {
      try { main_cursor->rollback(); } catch (...) {}
      throw;
    }
  }

  // Crash recovery for every tributary on reopen. The pool is idle when
  // called from sanitize(), so we can safely:
  //   * call _DB::sanitize() to fix txn_lock / txn_ref_lock /
  //     txn_cursor_id / per-txn refs / mem_manager locks / next_txn_page
  //     in each tributary's _DBHeader (none of which the slot-level
  //     fields in _TributaryHeader cover).
  //   * reset the slot-level state/refs in the _TributaryHeader.
  //   * for non-MERGED slots: drain them via merge_tributary().
  //   * for MERGED slots: a previous recycle was interrupted — finish it
  //     directly via _recycle_slot().
  void _merge_unclaimed_tributaries() {
    std::vector<TributaryDB*> to_merge;
    std::vector<TributaryDB*> to_recycle;
    {
      // Pool is idle when called from sanitize(); safe lock-free scan.
      size_t n = _tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        TributaryDB* t = _trib_at(i);
        slot_ptr slot = t->_header;
        uint8_t state = slot->state.load(std::memory_order_relaxed);

        // Sanitize the embedded _DBHeader of this tributary (clears any
        // stuck txn_ref_lock left by a crashed writer, etc.).
        t->sanitize();

        // Reset slot-level fields (live in _TributaryHeader, not _DBHeader).
        new (&slot->refs) std::atomic<uint32_t>(0);
        if (state == Slot::EMPTY) {
          // No data; nothing to do.
        } else if (state == Slot::MERGED) {
          // Recycle path was interrupted; finish it.
          to_recycle.push_back(t);
        } else {
          // FIRST_WRITING / WRITING / ATTACHED / MERGING: may have committed
          // data; reset to MERGING and drain via merge (no-op if empty).
          new (&slot->state) std::atomic<uint8_t>(Slot::MERGING);
          to_merge.push_back(t);
        }
      }
    }
    for (TributaryDB* t : to_recycle) _recycle_slot(t);
    for (TributaryDB* t : to_merge) merge_tributary(t);
  }
};

// _PinnedSource: one tributary read source held by _ConfluenceCursor
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
    if (_del_cursor) {
      _del_cursor->set_root(&ttxn->delete_root);
    } else {
      _del_cursor = std::make_unique<_Cursor<TribCursorTraits>>(
          _trib_db, &ttxn->delete_root);
    }
  }
};

// _ConfluenceCursor
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

  // Sticky write slot: held across transactions until threshold/idle/destroy.
  // When _in_transaction is true, _write_source._trib_cursor is in an active
  // write txn. Between transactions, state is ATTACHED (cursor owns it).
  PinnedSource _write_source;
  bool _in_transaction{false};
  uint32_t _pending_write_keys{0};

  // Main DB read snapshot — refreshed by _ensure_sources()
  MainTxnPtr _main_txn;
  std::unique_ptr<_Cursor<MainCursorTraits>> _main_cursor;

  // _sources[0.._sources_n) are active (pinned) read sources.
  // _sources[_sources_n..size) are parked (slot/txn released, cursors kept).
  // The write slot (_write_source) is NOT in this list.
  std::vector<PinnedSource> _sources;
  size_t _sources_n{0};
  uint64_t _sources_state_epoch{UINT64_MAX};

  // Reused hot-path buffers (avoid per-call allocation)
  std::string _iter_key;
  std::vector<Candidate> _candidates;

  // Result state (_value_storage is a non-owning Slice into trie memory;
  // valid while _sources are held — nulled by _release_sources())
  Slice _value_storage;
  bool _valid{false};
  bool _pending_find{false};  // find() is lazy; materialized on first is_valid/value/next/prev
  Slice _search_key;          // non-owning view into _iter_key; set eagerly by find()
  explicit _ConfluenceCursor(ConfluenceDB_* cdb) : _cdb(cdb) {
    _main_txn = _cdb->txn_ref();
    _main_cursor = std::make_unique<_Cursor<MainCursorTraits>>(
        &_cdb->_main_db, &_main_txn->root);
  }

  ~_ConfluenceCursor() {
    if (_in_transaction) rollback();
    _release_sources();
    _release_write_source();
    if (_main_txn) _main_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
  }

  bool start_transaction(bool non_blocking = false) {
    if (_in_transaction) return true;
    // Propagate any async merge error before entering a new transaction.
    if (_cdb->_has_merge_error.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(_cdb->_merge_error_mutex);
      auto e = std::exchange(_cdb->_pending_merge_error, nullptr);
      _cdb->_has_merge_error.store(false, std::memory_order_release);
      if (e) std::rethrow_exception(e);
    }

    // Fast path: reuse sticky write slot if still ATTACHED and not idle.
    if (_write_source._slot) {
      uint8_t expected = Slot::ATTACHED;
      if (_write_source._slot->state.compare_exchange_strong(
              expected, Slot::WRITING, std::memory_order_acq_rel)) {
        // We won the CAS — slot is ours again.
        if (!_write_source._trib_cursor->start_transaction(non_blocking)) {
            // Roll back state: WRITING → ATTACHED
            _write_source._slot->state.store(Slot::ATTACHED,
                                             std::memory_order_release);
            return false;
          }
          _write_source._refresh_del_cursor();
        _in_transaction = true;
        _pending_write_keys = 0;
        return true;
      }
      // CAS failed: monitor stole the slot (ATTACHED→MERGING) — release
      // our single claim ref and fall through to acquire a fresh slot.
      _write_source.park();  // _unpin_slot → refs--, resets slot+trib_db+cursors
    }

    // Slow path: acquire a fresh write slot.
    // _ensure_sources() deferred to first read (lazy) — pure write paths
    // never need it here.
    TributaryDB* trib = nullptr;
    unsigned spin = 0;
    trib = _cdb->_try_claim_or_alloc();
    if (!trib) {
      // All slots occupied — release read source refs and spin.
      for (;;) {
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
        trib = _cdb->_try_claim_or_alloc();
        if (trib) break;
      }
    }

    slot_ptr slot = trib->_header;
    // _try_claim/_alloc_new_slot already set refs=1 (our single claim pin).
    // Do NOT do an extra refs.fetch_add here — park() via _unpin_slot will
    // release that one pin.
    offset_t slot_off = (uint64_t)_cdb->_main_db._storage.resolve(slot);
    _write_source.park();  // drop any stale cursor objects (slot is null → no-op on refs)
    _write_source.reinit(_cdb, trib, slot, slot_off);
    if (!_write_source._trib_cursor->start_transaction(non_blocking)) {
      // Failed — transition to MERGING so monitor will clean up, then release pin.
      _write_source._slot->state.store(Slot::MERGING, std::memory_order_release);
      _write_source.park();  // refs-- (1→0)
      return false;
    }
    _write_source._refresh_del_cursor();
    _in_transaction = true;
    _pending_write_keys = 0;
    return true;
  }

  bool commit(bool sync = false) {
    if (!_in_transaction) return false;
    bool ok = _write_source._trib_cursor->commit(sync);
    if (!ok) {
      _write_source._trib_cursor->rollback();
    } else {
      uint32_t new_wc = _write_source._slot->write_count.fetch_add(
                            _pending_write_keys, std::memory_order_relaxed) +
                        _pending_write_keys;
      _write_source._slot->last_used_time.store(_current_time(),
                                                std::memory_order_release);
      uint8_t old_state = _write_source._slot->state.load(
          std::memory_order_relaxed);
      uint32_t threshold =
          _cdb->_merge_write_threshold.load(std::memory_order_relaxed);
      if (new_wc >= threshold) {
        // Threshold reached: transition to MERGING and release our claim pin.
        _write_source._slot->state.store(Slot::MERGING, std::memory_order_release);
        _write_source.park();  // _unpin_slot → refs-- (1→0)
      } else {
        // Stay attached: WRITING/FIRST_WRITING → ATTACHED
        bool first_commit = (old_state == Slot::FIRST_WRITING);
        _write_source._slot->state.store(Slot::ATTACHED,
                                         std::memory_order_release);
        if (first_commit) {
          // Slot now has a readable snapshot for the first time.
          _cdb->_meta->state_epoch.fetch_add(1, std::memory_order_release);
        }
      }
    }
    _in_transaction = false;
    _pending_write_keys = 0;
    _value_storage = Slice();
    return ok;
  }

  bool rollback() {
    if (!_in_transaction) return false;
    _write_source._trib_cursor->rollback();
    uint8_t s = _write_source._slot->state.load(std::memory_order_relaxed);
    if (s == Slot::WRITING) {
      // Slot had a prior committed snapshot: revert to ATTACHED, keep pin.
      _write_source._slot->state.store(Slot::ATTACHED, std::memory_order_release);
    } else {
      // FIRST_WRITING: no prior snapshot — release the slot (no-op merge).
      _write_source._slot->state.store(Slot::MERGING, std::memory_order_release);
      _write_source.park();  // _unpin_slot → refs-- (1→0)
    }
    _in_transaction = false;
    _pending_write_keys = 0;
    _value_storage = Slice();
    return true;
  }

  bool is_transaction_active() const { return _in_transaction; }

  void value(const Slice& v) {
    assert(_in_transaction);
    _materialize_write();
    _write_source._trib_cursor->value(v);
    ++_pending_write_keys;
  }

  void remove() {
    assert(_in_transaction);
    _materialize_write();
    if (_write_source._trib_cursor->is_valid()) {
      ++_pending_write_keys;
    }
    _write_source._trib_cursor->remove();
  }

  void _ensure_sources() {
    // Propagate any async merge error to the calling cursor op.
    if (_cdb->_has_merge_error.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(_cdb->_merge_error_mutex);
      auto e = std::exchange(_cdb->_pending_merge_error, nullptr);
      _cdb->_has_merge_error.store(false, std::memory_order_release);
      if (e) std::rethrow_exception(e);
    }
    uint64_t cur_state_epoch =
        _cdb->_meta->state_epoch.load(std::memory_order_acquire);
    if (_sources_state_epoch == cur_state_epoch)
      return;
    uint64_t cur_chain_epoch =
        _cdb->_meta->chain_epoch.load(std::memory_order_acquire);

    // Refresh main DB snapshot.
    // Pin the new txn first so the old _root pointer stays valid through
    // set_root's dereference, then release the old pin.
    auto new_txn = _cdb->txn_ref();
    _main_cursor->set_root(&new_txn->root);
    if (_main_txn) _main_txn->refs.fetch_sub(1, std::memory_order_acq_rel);
    _main_txn = std::move(new_txn);

    // Park old sources before rebuilding.
    // park() → _unpin_slot() may trigger _recycle_slot which scans the
    // published _tributaries — keep that work outside chain_lock.
    for (size_t i = 0; i < _sources_n; ++i) _sources[i].park();
    _sources_n = 0;

    auto fill_sources_lockfree = [&] {
      size_t fill = 0;
      size_t n =
          _cdb->_tributaries_count.load(std::memory_order_acquire);
      for (size_t i = 0; i < n; ++i) {
        TributaryDB* t = _cdb->_trib_at(i);
        slot_ptr slot = t->_header;
        uint8_t s = slot->state.load(std::memory_order_acquire);
        // Only include slots with a readable committed snapshot.
        // EMPTY and FIRST_WRITING have no snapshot yet; MERGED data is
        // already reflected in the main DB.
        if (s == Slot::EMPTY || s == Slot::FIRST_WRITING || s == Slot::MERGED)
          continue;
        // Exclude our own sticky write slot — it is handled separately.
        if (t == _write_source._trib_db) continue;
        slot->refs.fetch_add(1, std::memory_order_acq_rel);
        offset_t slot_off =
            (uint64_t)_cdb->_main_db._storage.resolve(t->_header);
        if (fill >= _sources.size()) _sources.emplace_back();
        _sources[fill].reinit(_cdb, t, slot, slot_off);
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

    _sources_state_epoch =
        _cdb->_meta->state_epoch.load(std::memory_order_relaxed);
  }

  void _release_sources() {
    for (size_t i = 0; i < _sources_n; ++i) _sources[i].park();
    _sources_n = 0;
    _value_storage = Slice();  // null to prevent dangling
    _sources_state_epoch = UINT64_MAX;  // force rebuild on next _ensure_sources
  }

  // Detach the sticky write slot.  Safe to call when _slot is null (no-op).
  // Detach the sticky write slot.  Safe to call when _slot is null (no-op).
  // ATTACHED → MERGING so the monitor picks it up on the next cycle.
  // For other states (e.g. MERGING after monitor steal) the state is already
  // correct — just release our claim pin via park().
  void _release_write_source() {
    if (!_write_source._slot) return;
    uint8_t s = _write_source._slot->state.load(std::memory_order_relaxed);
    if (s == Slot::ATTACHED) {
      _write_source._slot->state.store(Slot::MERGING, std::memory_order_release);
    }
    _write_source.park();  // _unpin_slot → refs--, resets slot+trib_db+cursors
  }

  bool _resolve_key(const Slice& key, Slice& value_out) {
    _ensure_sources();

    _candidates.clear();

    // Serial find on all read sources + write source (if in transaction) +
    // main cursor on the calling thread.
    for (size_t i = 0; i < _sources_n; ++i) {
      auto& src = _sources[i];
      src._trib_cursor->find(key);
      if (src._del_cursor) src._del_cursor->find(key);
    }
    if (_in_transaction) {
      _write_source._trib_cursor->find(key);
      if (_write_source._del_cursor) _write_source._del_cursor->find(key);
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
    if (_in_transaction) {
      bool found = _write_source._trib_cursor->is_valid() &&
                   key == _write_source._trib_cursor->current_key;
      bool deleted = false;
      if (_write_source._del_cursor) {
        deleted = _write_source._del_cursor->is_valid() &&
                  key == _write_source._del_cursor->current_key;
      }
      if (found || deleted) {
        _candidates.push_back(
            {_write_source._trib_cursor->_txn->txn_id,
             found ? _write_source._trib_cursor->value() : Slice(), deleted});
      }
    }

    if (_candidates.empty()) return false;
    int winner = _policy.resolve(key, _candidates);
    if (winner < 0) return false;
    value_out = _candidates[winner].value;
    return true;
  }

  void _materialize_full() {
    if (!_pending_find) return;
    _pending_find = false;
    Slice found_val;
    if (_resolve_key(Slice(_iter_key), found_val)) {
      _value_storage = found_val;
      _valid = true;
    } else {
      _valid = false;
    }
  }

  // Write-cursor-only materialization: positions only the write cursor.
  // Called by value(Slice) set and remove() to position the write cursor.
  // NOTE: does NOT set _valid — use is_valid() / value() for reads.
  void _materialize_write() {
    if (!_pending_find) return;
    _pending_find = false;
    _write_source._trib_cursor->find(Slice(_iter_key));
    _valid = _write_source._trib_cursor->is_valid() &&
             _search_key == _write_source._trib_cursor->current_key;
  }

  bool is_valid() {
    // Always use the full materialization so that is_valid() reflects the
    // merged view of all sources (main DB + all tributaries), not just the
    // write cursor.  _materialize_full() also calls find() on the write
    // cursor via _resolve_key(), which positions it for subsequent
    // value(Slice) writes even when _pending_find has been consumed.
    _materialize_full();
    return _valid;
  }
  Slice key() const { return _search_key; }
  Slice value() {
    _materialize_full();
    return _value_storage;
  }

  void find(const Slice& key) {
    _iter_key.assign(key.data(), key.size());
    _search_key = Slice(_iter_key);
    _pending_find = true;
    _valid = false;
  }

  void first() {
    _pending_find = false;
    _ensure_sources();
    _main_cursor->first();
    for (size_t i = 0; i < _sources_n; ++i)
      _sources[i]._trib_cursor->first();
    if (_in_transaction) _write_source._trib_cursor->first();
    _advance_to_next_valid();
  }

  void next() {
    _materialize_full();
    if (!_valid) return;
    if (_main_cursor->is_valid() && _search_key == _main_cursor->current_key)
      _main_cursor->next();
    for (size_t i = 0; i < _sources_n; ++i) {
      auto& src = _sources[i];
      if (src._trib_cursor->is_valid() &&
          _search_key == src._trib_cursor->current_key)
        src._trib_cursor->next();
    }
    if (_in_transaction && _write_source._trib_cursor->is_valid() &&
        _search_key == _write_source._trib_cursor->current_key)
      _write_source._trib_cursor->next();
    _advance_to_next_valid();
  }

  void last() {
    _pending_find = false;
    _ensure_sources();
    _main_cursor->last();
    for (size_t i = 0; i < _sources_n; ++i)
      _sources[i]._trib_cursor->last();
    if (_in_transaction) _write_source._trib_cursor->last();
    _advance_to_prev_valid();
  }

  void prev() {
    _materialize_full();
    if (!_valid) return;
    if (_main_cursor->is_valid() && _search_key == _main_cursor->current_key)
      _main_cursor->prev();
    for (size_t i = 0; i < _sources_n; ++i) {
      auto& src = _sources[i];
      if (src._trib_cursor->is_valid() &&
          _search_key == src._trib_cursor->current_key)
        src._trib_cursor->prev();
    }
    if (_in_transaction && _write_source._trib_cursor->is_valid() &&
        _search_key == _write_source._trib_cursor->current_key)
      _write_source._trib_cursor->prev();
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
      if (_in_transaction && _write_source._trib_cursor->is_valid()) {
        if (!min_key || _write_source._trib_cursor->current_key < *min_key)
          min_key = &_write_source._trib_cursor->current_key;
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
      if (_in_transaction && _write_source._trib_cursor->is_valid() &&
          _write_source._trib_cursor->current_key == _iter_key) {
        bool deleted = false;
        if (_write_source._del_cursor) {
          _write_source._del_cursor->find(Slice(_iter_key));
          deleted = _write_source._del_cursor->is_valid() &&
                    _write_source._del_cursor->current_key == _iter_key;
        }
        _candidates.push_back({_write_source._trib_cursor->_txn->txn_id,
                                _write_source._trib_cursor->value(), deleted});
      }

      if (_main_cursor->is_valid() && _main_cursor->current_key == _iter_key)
        _main_cursor->next();
      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (src._trib_cursor->is_valid() &&
            src._trib_cursor->current_key == _iter_key)
          src._trib_cursor->next();
      }
      if (_in_transaction && _write_source._trib_cursor->is_valid() &&
          _write_source._trib_cursor->current_key == _iter_key)
        _write_source._trib_cursor->next();

      int winner = _policy.resolve(Slice(_iter_key), _candidates);
      if (winner >= 0) {
        _search_key = Slice(_iter_key);
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
      if (_in_transaction && _write_source._trib_cursor->is_valid()) {
        if (!max_key || _write_source._trib_cursor->current_key > *max_key)
          max_key = &_write_source._trib_cursor->current_key;
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
      if (_in_transaction && _write_source._trib_cursor->is_valid() &&
          _write_source._trib_cursor->current_key == _iter_key) {
        bool deleted = false;
        if (_write_source._del_cursor) {
          _write_source._del_cursor->find(Slice(_iter_key));
          deleted = _write_source._del_cursor->is_valid() &&
                    _write_source._del_cursor->current_key == _iter_key;
        }
        _candidates.push_back({_write_source._trib_cursor->_txn->txn_id,
                                _write_source._trib_cursor->value(), deleted});
      }

      if (_main_cursor->is_valid() && _main_cursor->current_key == _iter_key)
        _main_cursor->prev();
      for (size_t i = 0; i < _sources_n; ++i) {
        auto& src = _sources[i];
        if (src._trib_cursor->is_valid() &&
            src._trib_cursor->current_key == _iter_key)
          src._trib_cursor->prev();
      }
      if (_in_transaction && _write_source._trib_cursor->is_valid() &&
          _write_source._trib_cursor->current_key == _iter_key)
        _write_source._trib_cursor->prev();

      int winner = _policy.resolve(Slice(_iter_key), _candidates);
      if (winner >= 0) {
        _search_key = Slice(_iter_key);
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
