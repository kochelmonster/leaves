#ifndef _LEAVES__REPLICATION_DB_HPP
#define _LEAVES__REPLICATION_DB_HPP

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "../db/_check.hpp"
#include "../db/_db.hpp"
#include "_hash.hpp"

namespace leaves {

// Replication-specific DB header — extends _DBHeader with:
// - hash_mem_manager: Separate memory manager for hash trie (independent of
// data transactions)
// - hash_root: Root offset of the hash trie
// - replication_slots: Fixed-size slot array for crash-safe big-value anchoring
//
// The hash trie uses its own memory manager so hash updates don't interfere
// with data trie transactions. This allows background hash computation.
//
// MAX_REPLICATION_SLOTS is read from Storage_::Traits if available,
// defaulting to 8 (= 64 bytes of offset_t).
template <typename Storage_>
struct _ReplicationDBHeader : public _DBHeader<Storage_> {
  using Traits = typename Storage_::Traits;
  using offset_e = typename Traits::offset_e;
  using HashMemManager = _MemManagerPool<Traits>;

  // Detect MAX_REPLICATION_SLOTS from Traits, default to 8
  template <typename T, typename = void>
  struct get_max_replication_slots : std::integral_constant<uint16_t, 8> {};

  template <typename T>
  struct get_max_replication_slots<
      T, std::void_t<decltype(T::MAX_REPLICATION_SLOTS)>>
      : std::integral_constant<uint16_t, T::MAX_REPLICATION_SLOTS> {};

  static constexpr uint16_t MAX_REPLICATION_SLOTS =
      get_max_replication_slots<Traits>::value;

  // Sentinel value written by _claim_slot() via atomic CAS before the
  // real area offset is known.  sanitize() must skip these.
  static constexpr uint64_t REPLICATION_SLOT_SENTINEL = 1;

  // Hash trie control: synchronization, storage, and root offsets.
  //
  // Protocol: acquire update_lock FIRST, then ref_count++.
  // This ensures exactly one FSM runs the hash update; all others
  // wait on the lock and find the trie already current when they enter.
  // The lock is held only for check+update, not during replication.
  //
  // SpinLock uses TTAS with CPU yield — safe in shared memory (mmap)
  // and single-process (CacheStore). No kernel state involved.
  struct HashTrieControl {
    SpinLock update_lock;             // TTAS spinlock — safe in shared memory
    std::atomic<uint32_t> ref_count;  // Active replication session count
    std::atomic<uint64_t>
        hashed_txn_offset;  // Raw offset of txn matching hash trie (0=stale)
    HashMemManager hash_mem_manager;  // Separate allocator for hash trie nodes
    offset_e hash_root;               // Root of main hash trie
    offset_e deletion_hash_root;      // Root of deletion hash trie

    // Reset synchronization state after file reopen or sanitize.
    // hashed_txn_offset is intentionally preserved: the hash trie on disk
    // is still valid and should not be recomputed unnecessarily.
    void reset() noexcept {
      new (&update_lock) SpinLock();
      ref_count.store(0, std::memory_order_relaxed);
      hash_mem_manager.reinit_locks();
    }
  } hash_control;

  // Each slot holds the offset of one pre-merge multi-area.
  // 0 = slot is free, SENTINEL = claimed but no area yet.
  offset_t replication_slots[MAX_REPLICATION_SLOTS];
};

// _ReplicationTransaction: Extends _Transaction with a deletion_root
// The deletion trie tracks keys removed from the main trie, enabling
// replication of deletes. Both root and deletion_root are persisted in
// each transaction and independently Merkle-hashed.

template <typename Traits_>
struct _ReplicationTransaction : public _Transaction<Traits_> {
  using Base = _Transaction<Traits_>;
  using offset_e = typename Traits_::offset_e;

  // Override ptr to point to _ReplicationTransaction so that _DB::txn_ptr
  // resolves to Pointer<_ReplicationTransaction> rather than
  // Pointer<_Transaction>
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
    auto page = Base::alloc_slot(SLOT_ID, resolver);
    page->used = sizeof(_ReplicationTransaction);
    memcpy(&*page, this, sizeof(_ReplicationTransaction));
    auto new_txn = reinterpret_cast<_ReplicationTransaction*>(&*page);
    new_txn->mem_manager.reinit_locks();
    new (&new_txn->refs) std::atomic<uint32_t>(0);
    assert(page->slot_id == SLOT_ID);
    return page;
  }
};

// _ReplicationDB: Extends _DB with deletion trie support and background purge
// Uses _ReplicationTransaction which includes deletion_root.
// Overrides Cursor to be _ReplicationCursor which tracks deletes.
// Owns the purge lifecycle: start_purge() kicks off a self-rescheduling
// background job that removes expired entries from the deletion trie.

template <typename Traits_>
struct _ReplicationCursor;  // forward declaration

template <typename Storage_>
struct _ReplicationDB
    : public _DB<Storage_, _ReplicationTransaction<typename Storage_::Traits>,
                 _ReplicationDBHeader<Storage_>, _ReplicationDB<Storage_>> {
  using Base = _DB<Storage_, _ReplicationTransaction<typename Storage_::Traits>,
                   _ReplicationDBHeader<Storage_>, _ReplicationDB<Storage_>>;
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

  // HashDB: Minimal DB adapter for _HashUpdater
  // Provides the interface needed by _HashUpdater to manage the hash trie.
  // Uses hash_mem_manager for allocation (independent of data transactions).
  // Uses the parent _ReplicationDB's storage for offset resolution.
  struct HashDB {
    using Traits = typename Storage_::Traits;
    using offset_e = typename Traits::offset_e;
    using MemManager = _MemManagerPool<Traits>;
    using PageHeader = typename Traits::PageHeader;
    using page_ptr = typename Traits::ptr;

    _ReplicationDB* _parent;

    explicit HashDB(_ReplicationDB* parent) : _parent(parent) {}

    // Resolve offset to node pointer
    template <typename T>
    typename Traits::template Pointer<T> resolve(offset_e* offset,
                                                 Access access = READ) {
      return _parent->template resolve<T>(offset, access);
    }

    template <typename T>
    typename Traits::template Pointer<T> resolve(const offset_e* offset,
                                                 Access access = READ) {
      return _parent->template resolve<T>(offset, access);
    }

    // Convert any typed pointer (TRIE or LEAF) to an offset in persistent
    // storage.  Works for both SimplePointer<T,TRIE> and SimplePointer<T,LEAF>.
    template <typename Ptr>
    offset_e resolve(Ptr ptr) {
      return _parent->resolve(ptr);
    }

    // Allocate a node of given size using hash_control.hash_mem_manager.
    // Passes *this as the resolver so that _MemManager calls our
    // alloc_single_area() below instead of _DB::alloc_single_area() (which
    // requires an active write transaction).
    template <typename NodePtr>
    NodePtr alloc_node(uint16_t size) {
      uint8_t slot_id = MemManager::assign_slot(size + sizeof(PageHeader));
      auto& hash_mem = _parent->_header->hash_control.hash_mem_manager;
      page_ptr page = hash_mem.alloc(slot_id, *this);
      _parent->make_dirty(page);
      return NodePtr(page + sizeof(PageHeader));
    }

    // Non-transactional area allocation for the hash mem-manager.
    // Called by _MemManager when its current area is exhausted.
    // Inserts the new area at the head of area_list_head_single so it is
    // tracked for reclamation on close/reset without requiring _active_txn.
    typename _ReplicationDB::area_ptr alloc_single_area() {
      std::scoped_lock lock(_parent->_storage.file_lock());
      auto area = _parent->_storage.alloc_single_area();
      area->next = _parent->_header->area_list_head_single;
      _parent->_header->area_list_head_single = _parent->_storage.resolve(area);
      _parent->make_dirty(area);
      _parent->make_dirty(_parent->_header);
      _parent->flush();
      return area;
    }

    // Free a page using hash_control.hash_mem_manager
    void free(page_ptr page) {
      auto& hash_mem = _parent->_header->hash_control.hash_mem_manager;
      hash_mem.free(page, *this);
    }

    // Allocate a slot page from hash_mem_manager.
    // Called by _GarbageSlot::push when it needs a new PageContainer.
    page_ptr alloc_slot(uint16_t slot) {
      auto& hash_mem = _parent->_header->hash_control.hash_mem_manager;
      return hash_mem.alloc(slot, *this);
    }

    // Hash trie nodes are freed explicitly by _HashUpdater when stale —
    // they are always safe to recycle immediately.
    template <typename T>
    bool may_recycle(T&) const {
      return true;
    }

    // No transaction tracking needed for hash nodes.
    template <typename T>
    void mark_for_recycle(T&) const {}

    // Make page dirty (for persistence)
    void make_dirty(page_ptr page) { _parent->make_dirty(page); }
  };

  // Get HashDB adapter for use with _HashUpdater
  HashDB hash_db() { return HashDB(this); }

  // Get hash root pointer for main trie
  auto* hash_root_ptr() { return &this->_header->hash_control.hash_root; }

  // Get hash root pointer for deletion trie
  auto* deletion_hash_root_ptr() {
    return &this->_header->hash_control.deletion_hash_root;
  }

  static constexpr uint16_t DB_TYPE_ID = 1;

  using txn_ptr = typename Base::txn_ptr;
  using offset_e = typename CursorTraits::offset_e;

  // --- Hash trie configuration ---
  size_t _hash_threads = 4;  // max threads for parallel hash trie updates

  // Construct from existing DB (open)
  _ReplicationDB(typename Base::Storage& storage, offset_t header,
                 std::string_view name, size_t hash_threads = 4,
                 bool auto_purge = true)
      : Base(storage, header, name), _hash_threads(hash_threads) {
    if (auto_purge) start_purge();
  }

  // Construct new DB (create)
  _ReplicationDB(typename Base::Storage& storage, offset_t* header,
                 std::string_view name, size_t hash_threads = 4,
                 bool auto_purge = true)
      : Base(storage, header, name), _hash_threads(hash_threads) {
    if (auto_purge) start_purge();
  }

  // --- Purge configuration ---
  std::atomic<uint64_t> _retention_seconds{
      86400};  // how long deleted keys stay (default 24h)

  // --- Purge state ---
  std::atomic<bool> _purge_interrupt{false};
  std::atomic<bool> _purge_cancelled{false};
  std::atomic<uint64_t> _purge_job_id{0};
  std::atomic<bool> _in_purge{false};

  ~_ReplicationDB() { cancel_purge(); }

  // Override init to also initialize hash_mem_manager with its own area
  void init(offset_t* header) {
    Base::init(header);

    // Allocate a separate area for hash trie memory management
    auto hash_area = this->_storage.alloc_single_area();
    this->_header->hash_control.hash_mem_manager.init(
        hash_area->content_offset(), hash_area->end());
    this->_header->hash_control.hash_root = 0;
    this->_header->hash_control.deletion_hash_root = 0;
    this->_header->hash_control.reset();
    this->_header->hash_control.hashed_txn_offset.store(
        0, std::memory_order_relaxed);

    this->make_dirty(this->_header);
  }

  void set_retention(uint64_t seconds) {
    _retention_seconds.store(seconds, std::memory_order_relaxed);
  }

  // Override commit.
  bool commit(uint64_t cursor_id, bool sync = false,
              TransactionOrigin origin = TransactionOrigin::user) {
    // Prepare commit without computing hashes (base doesn't hash either)
    if (!Base::prepare_commit(cursor_id, false, origin)) return false;

    // Atomically switch to new transaction
    this->_header->read_txn = this->_header->prepared_txn;
    this->make_dirty(this->_header);
    this->flush(sync, true);
    this->end_transaction();

    return true;
  }

  cursor_ptr create_cursor() {
    auto cursor = std::make_shared<Cursor>(this, &this->txn()->root);
    this->_aspect.init_cursor_context(cursor->_aspect_context);
    return cursor;
  }

  // Start the self-rescheduling purge.  Requires that _storage has
  // schedule_after() / cancel_job() / wait_idle() (i.e. _ThreadPoolMixin).
  void start_purge() {
    _purge_cancelled.store(false, std::memory_order_release);
    uint64_t expected = 0;
    if (!_purge_job_id.compare_exchange_strong(expected, UINT64_MAX,
                                               std::memory_order_acq_rel))
      return;  // Another purge is already scheduled
    _purge_job_id.store(this->_storage.schedule_after(std::chrono::seconds(0),
                                                      [this] { _run_purge(); }),
                        std::memory_order_release);
  }

  // Cancel any scheduled or running purge and wait for completion.
  void cancel_purge() {
    _purge_cancelled.store(true, std::memory_order_release);
    _purge_interrupt.store(true, std::memory_order_release);
    uint64_t job_id = _purge_job_id.exchange(0, std::memory_order_acq_rel);
    if (job_id && job_id != UINT64_MAX) this->_storage.cancel_job(job_id);
    if (!this->_storage._pool_shutdown.load(std::memory_order_acquire))
      this->_storage.wait_idle();
  }

  // Override sanitize() to also recover orphaned replication anchors.
  void sanitize() {
    Base::sanitize();

    // Reset hash trie synchronization primitives (stale lock/ref after reopen)
    this->_header->hash_control.reset();

    _sanitize_replication_anchors();
    this->flush();
  }

  // Override: signal background purge to stop before acquiring txn_lock.
  // No waiting for hashes - they run independently.
  txn_ptr start_transaction(
      uint64_t cursor_id, bool nonblocking = false,
      TransactionOrigin origin = TransactionOrigin::user) {
    if (_in_purge.load(std::memory_order_relaxed)) {
      _purge_interrupt.store(true, std::memory_order_release);
    }
    return Base::start_transaction(cursor_id, nonblocking, origin);
  }

  // Called under txn_ref_lock just before a stale txn is freed.
  // Zeros hashed_txn_offset if it pointed at the freed txn so that
  // the next acquire_hash_trie() knows it must recompute the hash trie.
  void _on_txn_freed(txn_ptr t) {
    uint64_t freed_off = (uint64_t)this->resolve(t);
    auto& atom = this->_header->hash_control.hashed_txn_offset;
    if (atom.load(std::memory_order_relaxed) == freed_off)
      atom.store(0, std::memory_order_relaxed);
  }

  // Acquire the hash trie for a replication session.
  //
  // Locking order: update_lock FIRST, then ref_count++.
  // The FSM that wins update_lock is "first" and updates the hash trie
  // if stale. All subsequent FSMs wait on the lock, then find the trie
  // current and skip the update. The lock is released before replication
  // begins, so it is held only for the check+update phase.
  //
  // Returns a pinned txn_ptr whose snapshot matches hash_root /
  // deletion_hash_root. Caller MUST call release_hash_trie() when done.
  txn_ptr acquire_hash_trie() {
    auto& hc = this->_header->hash_control;
    txn_ptr first_fsm_txn;  // non-null only for the first FSM
    {
      std::lock_guard<SpinLock> lock(hc.update_lock);
      uint32_t prev = hc.ref_count.fetch_add(1, std::memory_order_acq_rel);
      if (prev == 0) {
        // First FSM: update hash trie if stale.
        // Keep the ref on current alive — returning it directly avoids
        // a window where refs==0 lets GC free the page before non-first
        // FSMs can pin it via hashed_txn_offset.
        first_fsm_txn = this->txn_ref();
        uint64_t htxn_off =
            hc.hashed_txn_offset.load(std::memory_order_relaxed);
        bool needs_update = (htxn_off == 0);
        if (!needs_update) {
          offset_t off(htxn_off);
          auto hashed = this->template resolve<Transaction>(&off);
          needs_update = (hashed->txn_id < first_fsm_txn->txn_id);
        }
        if (needs_update) {
          auto hdb = this->hash_db();
          auto* rtxn = static_cast<Transaction*>(&*first_fsm_txn);
#if LEAVES_HAS_THREADS
          hc.hash_mem_manager.set_single_thread(false);
          update_hash_trie(this->_storage, this, &hdb, first_fsm_txn->root,
                           &hc.hash_root, _hash_threads);
          update_hash_trie(this->_storage, this, &hdb, rtxn->deletion_root,
                           &hc.deletion_hash_root, _hash_threads);
          hc.hash_mem_manager.set_single_thread(true);
#else
          update_hash_trie(this, &hdb, first_fsm_txn->root, &hc.hash_root);
          update_hash_trie(this, &hdb, rtxn->deletion_root,
                           &hc.deletion_hash_root);
#endif
          hc.hashed_txn_offset.store((uint64_t)this->resolve(first_fsm_txn),
                                     std::memory_order_release);
        }
        // Do NOT drop the ref here — first_fsm_txn keeps it alive.
      }
    }  // update_lock released — subsequent FSMs will not recompute

    // First FSM already holds a pinned ref — return it directly.
    if (first_fsm_txn) return first_fsm_txn;

    // Non-first FSMs: pin the txn whose snapshot matches the hash trie.
    // Read hashed_txn_offset inside txn_ref_lock so the GC walk
    // (which zeros it and frees the page under the same lock)
    // cannot free the page between our read and the refs++ pin.
    txn_ptr pinned;
    {
      std::lock_guard<SpinLock> guard(this->_header->txn_ref_lock);
      uint64_t htxn_off = hc.hashed_txn_offset.load(std::memory_order_acquire);
      if (htxn_off) {
        offset_t off(htxn_off);
        pinned = this->template resolve<Transaction>(&off);
        pinned->refs.fetch_add(1);
      }
    }
    if (!pinned) {
      hc.hashed_txn_offset.store(0, std::memory_order_relaxed);
      pinned = this->txn_ref();
    }
    return pinned;
  }

  // Release a replication session acquired via acquire_hash_trie().
  // Idempotent: safe to call even if pinned is null (no-op).
  void release_hash_trie(txn_ptr& pinned) {
    if (!pinned) return;
    pinned->refs.fetch_sub(1, std::memory_order_acq_rel);
    pinned.reset();
    this->_header->hash_control.ref_count.fetch_sub(1,
                                                    std::memory_order_acq_rel);
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
  // After purging, schedules the next run based on the oldest remainings
  // entry and the retention period.
  void _run_purge() {
    _in_purge.store(true, std::memory_order_relaxed);
    _purge_interrupt.store(false, std::memory_order_relaxed);

    uint64_t now = _current_time();
    uint64_t retention = _retention_seconds.load(std::memory_order_relaxed);
    uint64_t older_than = (now > retention) ? now - retention : 0;

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
      _purge_job_id.store(
          this->_storage.schedule_after(std::chrono::seconds(next_seconds),
                                        [this] { _run_purge(); }),
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
    // Fast path: skip the write transaction entirely when the deletion
    // trie is empty.  txn_ref() is lock-free (atomic ref bump).
    {
      auto snap = this->txn_ref();
      auto* rtxn = static_cast<Transaction*>(&*snap);
      bool empty = (rtxn->deletion_root == 0);
      snap->refs.fetch_sub(1, std::memory_order_acq_rel);
      if (empty) return {0, 0};
    }

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
        _little_uint64_t ts_le;
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
};

// _ReplicationCursor: Extends _TransactionalCursor with deletion tracking
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
    _little_uint64_t ts_le = now;
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
