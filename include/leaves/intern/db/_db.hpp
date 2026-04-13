#ifndef _LEAVES__DB_HPP
#define _LEAVES__DB_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "../core/_port.hpp"
#include "../memory/_memory.hpp"
#include "../util/_merger.hpp"
#include "_aspect.hpp"
#include "_cursor.hpp"

namespace leaves {

template <typename Traits_>
struct _TransactionBase : public Traits_::PageHeader {
  typedef _TransactionBase<Traits_> TransactionBase;
  typedef Traits_ Traits;
  typedef _MemManager<Traits> MemManager;
  using Traits::PageHeader::txn_id;
  using offset_e = typename Traits::offset_e;

  // pointer to the active root of the trie
  offset_e root;

  // pointer to the active bigmem freelist trie root
  offset_e free_bigmem_root;

  // pointer to the oldest transaction
  offset_e start_txn;

  // pointer to the next higher transaction
  offset_e next_txn;

  // count of cursors accessing this transaction
  std::atomic<uint32_t> refs;

  // trie root for tracking deleted keys (used during merge commit)
  offset_e delete_root{0};

  // Area tracking: tail pointers for areas allocated during this transaction
  offset_e area_list_tail_single{0};
  offset_e area_list_tail_multi{0};

  MemManager mem_manager;
};

template <typename Traits_>
struct _Transaction : public _TransactionBase<Traits_> {
  typedef Traits_ Traits;
  typedef _TransactionBase<Traits_> TransactionBase;
  typedef _MemManager<Traits> MemManager;
  using ptr = typename Traits::template Pointer<_Transaction>;
  using page_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using TransactionBase::area_list_tail_multi;
  using TransactionBase::area_list_tail_single;
  using TransactionBase::mem_manager;
  using TransactionBase::txn_id;

  static constexpr auto SLOT_ID =
      MemManager::assign_slot(sizeof(TransactionBase));
  uint16_t size() const { return sizeof(TransactionBase); }

  template <typename Resolver>
  page_ptr alloc_slot(uint16_t slot, Resolver& resolver) {
    page_ptr result;
    for (;;) {
      tid_t freed_tid;
      result = mem_manager.alloc(slot, resolver, &freed_tid);
      if (freed_tid == tid_t(0)) break;  // Bump-allocated, no guard needed
      // Double-recycle guard: if page's txn_id > garbage entry's txn_id,
      // the page was already recycled through another garbage entry — retry.
      if (result->txn_id > freed_tid) continue;
      // CAS the txn_id to claim the page — if another context grabbed it, retry.
      auto expected = result->txn_id._value;
      std::atomic_ref<uint32_t> atomic_tid(result->txn_id._value);
      if (atomic_tid.compare_exchange_weak(expected, txn_id._value,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed))
        break;
    }
    result->txn_id = txn_id;
    resolver.make_dirty(result);
    return result;
  }

  template <typename Resolver>
  ptr clone(Resolver& resolver) {
    ptr new_txn = alloc_slot(SLOT_ID, resolver);
    memcpy((void*)&*new_txn, this, sizeof(TransactionBase));
    new_txn->used = sizeof(TransactionBase);
    new_txn->mem_manager.reinit_locks();
    new (&new_txn->refs) std::atomic<uint32_t>(0);
    assert(new_txn->slot_id == SLOT_ID);
    return new_txn;
  }
};

// Per-context persistent state for parallel transactions.
// Stored contiguously after the DB header in storage.
template <typename Storage_>
struct _TxnContext {
  std::atomic<uint8_t> _claimed{0};  // 0 = free, 1 = claimed (CAS to acquire)
  offset_t prepared_txn{0};          // transaction being prepared for commit
  offset_t next_txn_page{0};         // pre-allocated next txn page
  offset_t active_txn{0};  // offset of active transaction (set during txn)
  offset_t area_list_head_single{0};  // head of single-AREA_SIZE area chain
  offset_t area_list_head_multi{0};   // head of multi-AREA_SIZE area chain
  tid_t _snapshot_txn_id{0};          // txn_id at start_transaction() time
};

// Default database header (defined outside _DB for reusability)
template <typename Storage_>
struct _DBHeader {
  offset_t read_txn;      // the current read transaction
  SpinLock txn_ref_lock;  // protects txn() + refs.fetch_add() atomicity

  // Identifies the DB subtype for runtime type verification on open.
  // Written by init(), checked by open<>() to prevent type mismatches.
  uint16_t db_type_id;

  uint8_t num_contexts;  // number of parallel transaction contexts

  // Tracks whether this DB has been sanitized for the current storage
  // generation.  Compared against the FileHeader counter at open() time.
  uint32_t sanitize_generation;

  // Cross-process wait mechanism for blocking context claims.
  // Types come from Storage_ so they match the storage backend.
  typename Storage_::CtxMutex ctx_wait_lock;
  typename Storage_::CtxCondVar ctx_wait_cv;

  // Global merge lock — serializes merge-on-commit when parallel
  // contexts conflict.  Cross-process safe (pure hardware atomics).
  SpinLock merge_lock;

  // Monotonic counter for unique txn_id assignment.  Incremented under
  // txn_ref_lock so parallel start_transaction() calls never collide.
  tid_t _last_assigned_tid{0};
};

/**
 * @brief Merge policy for same-storage move-merge during commit.
 *
 * Controls which src subtrees to descend into (writer-allocated only)
 * and frees orphaned src nodes that are replaced during merge.
 */
template <typename DB_>
struct _ContextMergePolicy : StandardMergePolicy {
  using Traits = typename DB_::Traits;
  using page_ptr = typename Traits::ptr;
  using PageHeader = typename Traits::PageHeader;
  using TxnContext = typename DB_::TxnContext;
  using txn_ptr = typename DB_::txn_ptr;

  DB_* _db;
  TxnContext* _ctx;
  tid_t _base_txn_id;

  _ContextMergePolicy(DB_* db, TxnContext* ctx, tid_t base_txn_id)
      : _db(db), _ctx(ctx), _base_txn_id(base_txn_id) {}

  // Only descend into src nodes allocated by the writer (txn_id > snapshot).
  // Snapshot nodes are already in committed — skip entirely.
  template <typename PagePtr>
  bool should_descend_src(PagePtr page) {
    return page->txn_id > _base_txn_id;
  }

  // Free orphaned src node (writer-allocated, not adopted into dst).
  template <typename NodePtr>
  void free_src(NodePtr& node) {
    page_ptr page = node - sizeof(PageHeader);
    _db->_free(page, _ctx);
  }

  // Override free_big: use DB's active txn's free_bigmem_root directly.
  // _Cursor doesn't have get_bigmemory(), so we access BigMemory through the
  // DB.
  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& /*dst_cursor*/) {
    using RawCursor = _Cursor<typename DB_::CursorTraits>;
    using BigMemory = _BigMemory<RawCursor>;
    txn_ptr active = _db->_resolve_active(_ctx);
    _BigValue* bvalue = (_BigValue*)leaf->vdata();
    BigMemory big_mem(_db, &active->free_bigmem_root);
    big_mem.set_ctx(_ctx, active->txn_id);
    big_mem.free(bvalue);
  }

  // Override migrate_big_value: src and dst share storage, so the big value
  // chunk is already in the right place — just copy the _BigValue descriptor.
  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& /*src_cursor*/,
                                  DstCursor& /*dst_cursor*/) {
    // Same storage — the chunk offset is already valid in dst.
    return {Slice((uint8_t*)leaf.vdata(), sizeof(_BigValue)), true};
  }
};

// Make _DB accept Transaction and Header as template parameters
// Self_ enables CRTP: derived classes pass themselves so _DB can
// statically dispatch to overrides (e.g. _on_txn_freed) without virtual.
template <typename Storage_,
          typename Transaction_ = _Transaction<typename Storage_::Traits>,
          typename Header_ = _DBHeader<Storage_>, typename Self_ = void>
struct _DB {
  typedef Storage_ Storage;
  typedef Transaction_ Transaction;
  typedef Header_ Header;
  using Traits = typename Storage::Traits;
  using TxnContext = _TxnContext<Storage>;
  using area_ptr = typename Storage::area_ptr;
  using txn_ptr = typename Transaction::ptr;
  using page_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;

  // CRTP self-type: derived class if provided, otherwise this class.
  using Self =
      std::conditional_t<std::is_void_v<Self_>,
                         _DB<Storage_, Transaction_, Header_, Self_>, Self_>;

  typedef _DB<Storage_, Transaction_, Header_, Self_> DB;

  struct CursorTraits : public Storage::Traits {
    typedef _DB<Storage_, Transaction_, Header_, Self_> DB;
    using tid_t = leaves::tid_t;
  };

  using Aspect = typename Traits::Aspect;

  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto& PAGE_SIZES = Traits::PAGE_SIZES;
  static constexpr uint16_t PAGE_SIZES_COUNT = Traits::PAGE_SIZES_COUNT;
  static constexpr uint16_t MIN_PAGE_SIZE = PAGE_SIZES[0];
  static constexpr uint16_t MAX_PAGE_SIZE = PAGE_SIZES[PAGE_SIZES_COUNT - 1];
  static constexpr uint64_t SIZE_BIT = uint64_t(1) << 63;

  typedef _TransactionalCursor<CursorTraits> Cursor;
  typedef _MemManager<Traits> MemManager;
  typedef DB db_type;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  static constexpr uint16_t DB_TYPE_ID = 0;

  static_assert(
      sizeof(Transaction) >= sizeof(_TransactionBase<Traits>),
      "Size of Transaction must be at least size of _TransactionBase");

  static_assert(sizeof(Header) + sizeof(TxnContext) + sizeof(Transaction) <
                    AREA_SIZE,
                "DB Header too big");

  using header_ptr = typename Traits::template Pointer<Header>;

  Storage& _storage;
  header_ptr _header;
  std::string _name;
  uint8_t _num_contexts;

  [[no_unique_address]] Aspect _aspect{};

  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id{0};

  // Access the i-th TxnContext stored after the header in storage.
  TxnContext* context(uint8_t i) {
    return reinterpret_cast<TxnContext*>(reinterpret_cast<char*>(&*_header) +
                                         sizeof(Header)) +
           i;
  }
  const TxnContext* context(uint8_t i) const {
    return reinterpret_cast<const TxnContext*>(
               reinterpret_cast<const char*>(&*_header) + sizeof(Header)) +
           i;
  }

  // Total header size including all contexts (for computing txn offset).
  uint16_t _header_total_size() const {
    return padding(sizeof(Header) + _num_contexts * sizeof(TxnContext),
                   MIN_PAGE_SIZE);
  }

  // Compute context index from pointer (inverse of context(i)).
  uint8_t _ctx_index_of(TxnContext* ctx) const {
    return static_cast<uint8_t>(ctx - context(0));
  }

  // Resolve active transaction from a TxnContext.
  txn_ptr _resolve_active(TxnContext* ctx) const {
    return resolve<Transaction>(&ctx->active_txn);
  }

  // Internal resolver for lifecycle code (start_transaction GC, clone,
  // sanitize) where no cursor exists.  Implements the MemManager resolver
  // duck-type by wrapping _DB& + txn_ptr.
  struct _TxnResolver {
    DB& _db;
    txn_ptr _active_txn;
    TxnContext* _ctx;

    template <typename T>
    typename Traits::template Pointer<T> resolve(const offset_e* offset_ptr,
                                                 Access access = READ) const {
      return _db._storage.resolve(offset_ptr, access);
    }

    template <typename Pointer>
    typename std::enable_if<!std::is_pointer<Pointer>::value, offset_t>::type
    resolve(const Pointer& p) const {
      return _db._storage.resolve(p);
    }

    template <typename T>
    bool may_recycle(T& garbage_block) const {
      return garbage_block.txn_id < _db._start_txn_id;
    }

    template <typename T>
    void mark_for_recycle(T& garbage_block) const {
      garbage_block.txn_id = _active_txn->txn_id;
    }

    template <typename PtrType>
    void make_dirty(PtrType block) {
      _db._storage.make_dirty(block);
    }

    area_ptr alloc_single_area() { return _db._alloc_single_area(_active_txn); }
  };

  _DB(Storage& storage, offset_t header, std::string_view name,
      uint8_t num_contexts = 1)
      : _storage(storage),
        _header(storage.resolve(&header, READ)),
        _name(name),
        _num_contexts(num_contexts) {
    if (_header->num_contexts != _num_contexts) throw TypeMismatch();
  }

  _DB(Storage& storage, offset_t* header, std::string_view name,
      uint8_t num_contexts = 1)
      : _storage(storage), _name(name), _num_contexts(num_contexts) {
    init(header);
  }

  Self& self() { return *static_cast<Self*>(this); }

  void init(offset_t* header) {
    auto area_ptr = _storage.alloc_single_area();

    *header = area_ptr->content_offset();  // Use content_offset, not get_offset
    _header = _storage.resolve(header, READ);
    memset((char*)_header, 0, sizeof(Header));
    _header->db_type_id = Self::DB_TYPE_ID;
    _header->num_contexts = _num_contexts;
    new (&_header->txn_ref_lock) SpinLock();
    new (&_header->ctx_wait_lock) typename Storage::CtxMutex();
    new (&_header->ctx_wait_cv) typename Storage::CtxCondVar();
    new (&_header->merge_lock) SpinLock();
    auto* ctx = context(0);
    memset(ctx, 0, _num_contexts * sizeof(TxnContext));
    ctx->_claimed.store(0, std::memory_order_relaxed);
    offset_t first_area_offset = _storage.resolve(area_ptr);
    ctx->area_list_head_single = first_area_offset;
    ctx->area_list_head_multi = 0;

    // Initialize remaining contexts (all start as unclaimed).
    for (uint8_t i = 1; i < _num_contexts; i++)
      context(i)->_claimed.store(0, std::memory_order_relaxed);

    uint16_t header_size = _header_total_size();
    ctx->prepared_txn = _header->read_txn = *header + header_size;
    txn_ptr txn = resolve<Transaction>(&_header->read_txn);
    memset((char*)txn, 0, sizeof(Transaction));
    txn->slot_id = Transaction::SLOT_ID;
    txn->used = sizeof(Transaction);
    txn->txn_id = tid_t(1);
    _header->_last_assigned_tid = tid_t(1);
    txn->root = txn->free_bigmem_root = 0;
    txn->next_txn = 0;
    txn->refs.store(0);
    txn->start_txn = _header->read_txn;
    txn->area_list_tail_single =
        first_area_offset;  // First area is also the tail
    txn->area_list_tail_multi = 0;
    txn->delete_root = 0;
    txn->mem_manager.init(_header->read_txn + PAGE_SIZES[txn->slot_id],
                          area_ptr->end());

    // Pre-allocate the next transaction page so start_transaction()
    // can skip the double-copy on the very first call.
    _TxnResolver resolver{self(), txn, ctx};
    auto next = txn->clone(resolver);
    ctx->next_txn_page = resolve(next);

    make_dirty(_header);
    flush();
  }

  // Return all areas to storage pool (iterates all contexts)
  void return_areas() {
    auto read_txn = resolve<Transaction>(&_header->read_txn);
    for (uint8_t i = 0; i < _num_contexts; i++) {
      auto* ctx = context(i);
      if (ctx->area_list_head_single && read_txn->area_list_tail_single) {
        _storage.return_single_areas(ctx->area_list_head_single,
                                     read_txn->area_list_tail_single);
      }
      if (ctx->area_list_head_multi && read_txn->area_list_tail_multi) {
        _storage.return_multi_areas(ctx->area_list_head_multi,
                                    read_txn->area_list_tail_multi);
      }
    }
  }

  // Reset the DB by returning all areas to storage and reinitializing
  void reset(offset_t* header) {
    if (is_active()) throw TransactionActive();
    if (!_aspect.before_reset(self())) return;
    return_areas();
    init(header);
    _aspect.on_reset(self());
  }

  Aspect& aspect() { return _aspect; }
  const Aspect& aspect() const { return _aspect; }

  cursor_ptr create_cursor() {
    auto cursor = std::make_shared<Cursor>(this, &txn()->root);
    _aspect.init_cursor_context(cursor->_aspect_context);
    return cursor;
  }

  const db_type* _internal() const { return this; }  // for _Dumper

  Slice name() const { return Slice(_name); }

  template <typename T>
  typename Traits::template Pointer<T> resolve(const offset_e* offset_ptr,
                                               Access access = READ) const {
    return _storage.resolve(offset_ptr, access);
  }

  template <typename Pointer>
  typename std::enable_if<!std::is_pointer<Pointer>::value, offset_t>::type
  resolve(const Pointer& p) const {
    return _storage.resolve(p);
  }

  template <typename T>
  bool may_recycle(T& garbage_block) const {
    return garbage_block.txn_id < _start_txn_id;
  }

  template <typename PtrType>
  void make_dirty(PtrType block) {
    _storage.make_dirty(block);
  }

  void flush(bool sync = false, bool force = false) {
    _storage.flush(sync, force);
  }

  void prefetch(const offset_e* offset, Access access = READ) const {
    _storage.prefetch(offset, access);
  }

  // ── Context-parameterized allocation (used by _TxnResolver & lifecycle) ──

  page_ptr _alloc_slot(uint16_t slot, TxnContext* ctx) {
    txn_ptr active = _resolve_active(ctx);
    _TxnResolver resolver{self(), active, ctx};
    return active->alloc_slot(slot, resolver);
  }

  page_ptr _alloc_page(uint16_t space, TxnContext* ctx) {
    using PageHeader = typename Traits::PageHeader;
    assert(space + sizeof(PageHeader) <= PAGE_SIZES[PAGE_SIZES_COUNT - 1]);
    page_ptr result =
        _alloc_slot(MemManager::assign_slot(space + sizeof(PageHeader)), ctx);
    result->used = space;
    return result;
  }

  template <typename NodePtr>
  NodePtr _alloc_node(uint16_t node_size, TxnContext* ctx) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = _alloc_page(node_size, ctx);
    return page + sizeof(PageHeader);
  }

  void _free(page_ptr page, TxnContext* ctx) {
    txn_ptr active = _resolve_active(ctx);
    _TxnResolver resolver{self(), active, ctx};
    active->mem_manager.free(page, resolver);
  }

  area_ptr _alloc_single_area(txn_ptr active) {
    std::scoped_lock lock(_storage.file_lock());

    auto ap = _storage.alloc_single_area();
    ap->next = 0;

    auto tail = resolve<Area>(&active->area_list_tail_single, WRITE);
    tail->next = resolve(ap);
    make_dirty(tail);
    active->area_list_tail_single = resolve(ap);

    make_dirty(ap);
    return ap;
  }

  area_ptr _alloc_multi_area(uint64_t size, TxnContext* ctx) {
    txn_ptr active = _resolve_active(ctx);
    std::scoped_lock lock(_storage.file_lock());

    auto ap = _storage.alloc_multi_area(size);
    ap->next = 0;

    if (active->area_list_tail_multi) {
      auto tail = resolve<Area>(&active->area_list_tail_multi, WRITE);
      tail->next = resolve(ap);
      make_dirty(tail);
    } else {
      ctx->area_list_head_multi = resolve(ap);
      make_dirty(_header);
    }
    active->area_list_tail_multi = resolve(ap);

    make_dirty(ap);
    return ap;
  }

  template <typename T>
  void iter_transactions(T caller) const {
    txn_ptr txn = _storage.resolve(&_header->read_txn);
    tid_t end = txn->txn_id;
    offset_t* link = &txn->start_txn;
    do {
      txn = resolve<Transaction>(link);
      link = &txn->next_txn;  // read before callback (callback may free txn)
      if (caller(txn)) break;
    } while (txn->txn_id < end);
  }

  txn_ptr txn() const { return resolve<Transaction>(&_header->read_txn); }

  // Return the raw offset of the current read transaction.
  // This is a plain aligned 64-bit load — no lock required.
  offset_t read_txn_offset() const { return _header->read_txn; }

  // Atomically resolve current read txn and increment its refcount.
  // Uses SpinLock to prevent a concurrent start_transaction() from
  // freeing the txn between resolve and refs.fetch_add().
  txn_ptr txn_ref() {
    std::lock_guard<SpinLock> guard(_header->txn_ref_lock);
    txn_ptr t = txn();
    t->refs.fetch_add(1);
    return t;
  }

  // Atomically pin a txn by raw storage offset and increment its refcount.
  // Returns null if offset is 0 — txn was cleaned between the caller's read
  // of the offset and acquiring txn_ref_lock. Caller must treat null as stale.
  txn_ptr txn_ref_at(offset_t offset) {
    std::lock_guard<SpinLock> guard(_header->txn_ref_lock);
    if (!offset) return txn_ptr();
    txn_ptr t = resolve<Transaction>(&offset);
    t->refs.fetch_add(1);
    return t;
  }

  // Called (under txn_ref_lock) just before a stale transaction is freed.
  // Derived classes override via CRTP (Self_ parameter) to invalidate
  // any cached references to the txn.  No virtual dispatch needed.
  void _on_txn_freed(txn_ptr) {}

  bool is_active() const {
    // Check if any context has an active transaction.
    for (uint8_t i = 0; i < _num_contexts; i++) {
      if (context(i)->active_txn) return true;
    }
    bool is_active_ = false;
    iter_transactions([&is_active_](txn_ptr txn) -> bool {
      if (txn->refs.load() > 0) is_active_ = true;
      return is_active_;
    });

    return is_active_;
  }

  // Try to CAS-claim any free context. Returns index or UINT8_MAX.
  uint8_t _try_claim_any() {
    for (uint8_t i = 0; i < _num_contexts; i++) {
      uint8_t expected = 0;
      if (context(i)->_claimed.compare_exchange_weak(expected, 1,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed))
        return i;
    }
    return UINT8_MAX;
  }

  // Claim a free transaction context. Returns the context index.
  // CAS fast-path; on contention, waits on shared-memory condvar.
  uint8_t _claim_context(bool nonblocking) {
    // Fast path: CAS scan.
    uint8_t idx = _try_claim_any();
    if (idx != UINT8_MAX) return idx;
    if (nonblocking) return UINT8_MAX;

    // Slow path: wait for a context to be released.
    std::unique_lock lk(_header->ctx_wait_lock);
    while (true) {
      idx = _try_claim_any();
      if (idx != UINT8_MAX) return idx;
      _header->ctx_wait_cv.wait(lk);
    }
  }

  void _release_context(uint8_t idx) {
    context(idx)->_claimed.store(0, std::memory_order_release);
    // Wake one blocked waiter (if any).  The lock/unlock pair is
    // required to avoid a lost-wakeup race with _claim_context's
    // wait() call.
    {
      std::lock_guard lk(_header->ctx_wait_lock);
    }
    _header->ctx_wait_cv.notify_one();
  }

  // Lazily initialise a context that has never been used.
  // Allocates a fresh area with a bare Transaction page.
  void _init_context(uint8_t idx) {
    auto* ctx = context(idx);
    std::scoped_lock flock(_storage.file_lock());
    auto ap = _storage.alloc_single_area();
    ap->next = 0;
    make_dirty(ap);

    offset_t txn_offset = ap->content_offset();
    txn_ptr wtxn = resolve<Transaction>(&txn_offset);
    memset(static_cast<void*>(&*wtxn), 0, sizeof(Transaction));
    wtxn->slot_id = Transaction::SLOT_ID;
    wtxn->used = sizeof(Transaction);
    wtxn->txn_id = tid_t(0);
    new (&wtxn->refs) std::atomic<uint32_t>(0);
    wtxn->root = wtxn->free_bigmem_root = 0;
    wtxn->delete_root = 0;
    wtxn->start_txn = txn_offset;
    wtxn->next_txn = 0;
    wtxn->area_list_tail_single = _storage.resolve(ap);
    wtxn->area_list_tail_multi = 0;
    wtxn->mem_manager.init(txn_offset + PAGE_SIZES[Transaction::SLOT_ID],
                           ap->offset() + ap->size());

    ctx->area_list_head_single = _storage.resolve(ap);
    ctx->area_list_head_multi = 0;
    ctx->next_txn_page = txn_offset;

    // Match the "unprepared" state so prepare_commit() doesn't early-return.
    ctx->prepared_txn = _header->read_txn;

    make_dirty(wtxn);
    make_dirty(_header);
    flush();
  }

  // Start a write transaction. If nonblocking is true, this will return nullptr
  // immediately when no context can be acquired.
  // Returns TxnContext* — caller resolves active txn from ctx->active_txn.
  TxnContext* start_transaction(
      bool nonblocking = false,
      TransactionOrigin origin = TransactionOrigin::user) {
    uint8_t idx = _claim_context(nonblocking);
    if (idx == UINT8_MAX) return nullptr;

    auto* ctx = context(idx);
    assert(!ctx->active_txn);

    // Lazy-init contexts that have never been used.
    if (!ctx->next_txn_page) _init_context(idx);

    txn_ptr last_txn = txn();
    ctx->_snapshot_txn_id = last_txn->txn_id;

    // Pre-allocated page is always ready (from commit, rollback, or init)
    assert(ctx->next_txn_page);
    txn_ptr active = resolve<Transaction>(&ctx->next_txn_page);
    ctx->active_txn = ctx->next_txn_page;
    ctx->next_txn_page = 0;

    // Pin last_txn so it isn't freed during the GC walk.
    last_txn->refs.fetch_add(1);

    // Find the oldest used transaction and free unused old transactions.
    // Hold txn_ref_lock to prevent a concurrent txn_ref() from resolving
    // a txn between the refs==0 check and free().
    // Assign txn_id inside txn_ref_lock to prevent a concurrent reader
    // from seeing a half-initialised txn_id.
    _start_txn_id = last_txn->txn_id;
    {
      std::lock_guard<SpinLock> guard(_header->txn_ref_lock);
      _header->_last_assigned_tid = _header->_last_assigned_tid + tid_t(1);
      active->txn_id = _header->_last_assigned_tid;
      active->next_txn = 0;
      _TxnResolver resolver{self(), active, ctx};
      iter_transactions([this, &resolver](txn_ptr txn) -> bool {
        if (txn->refs.load() > 0) {
          resolver._active_txn->start_txn = resolve(txn);
          _start_txn_id = txn->txn_id;
          return true;
        }
        static_cast<Self*>(this)->_on_txn_freed(txn);
        resolver._active_txn->mem_manager.free(static_cast<page_ptr>(txn),
                                               resolver);
        return false;
      });
    }

    last_txn->refs.fetch_sub(1);
    return ctx;
  }

  bool rollback(TxnContext* ctx,
                TransactionOrigin origin = TransactionOrigin::user) {
    txn_ptr active = _resolve_active(ctx);

    // Return areas allocated during write transaction
    txn_ptr read_txn = resolve<Transaction>(&_header->read_txn);
    return_areas_range(
        ctx, read_txn->area_list_tail_single, active->area_list_tail_single,
        read_txn->area_list_tail_multi, active->area_list_tail_multi);

    ctx->active_txn = ctx->prepared_txn = _header->read_txn;

    // Reuse active's page for next pre-allocated transaction.
    // active was allocated from read_txn's committed space, so it
    // remains valid after rollback. Just overwrite with read_txn state.
    memcpy(&*active, &*read_txn, sizeof(Transaction));
    active->mem_manager.reinit_locks();
    new (&active->refs) std::atomic<uint32_t>(0);
    ctx->next_txn_page = resolve(active);

    make_dirty(active);
    make_dirty(_header);
    flush();
    end_transaction(ctx);
    return true;
  }

  tid_t prepare_commit(TxnContext* ctx, bool sync = false,
                       TransactionOrigin origin = TransactionOrigin::user) {
    txn_ptr active = _resolve_active(ctx);

    // already prepared
    if (ctx->prepared_txn == ctx->active_txn) return active->txn_id;

    // Pre-allocate next transaction page before committing.
    // The allocation modifies active->mem_manager, which gets persisted
    // as part of this commit — no storage leak.
    _TxnResolver resolver{self(), active, ctx};
    auto next = active->clone(resolver);
    ctx->next_txn_page = resolve(next);

    ctx->prepared_txn = ctx->active_txn;

    txn_ptr current = resolve<Transaction>(&_header->read_txn);
    current->next_txn = ctx->prepared_txn;
    make_dirty(_header);
    make_dirty(current);
    make_dirty(active);

    if (sync) flush(true, true);  // Only flush if explicitly requested
    return active->txn_id;
  }

  bool commit(TxnContext* ctx, bool sync = false,
              TransactionOrigin origin = TransactionOrigin::user) {
    if (!prepare_commit(ctx, false, origin)) return false;

    // Under merge_lock: detect conflict and merge if needed, then publish.
    {
      std::lock_guard<SpinLock> guard(_header->merge_lock);
      txn_ptr current = txn();
      if (current->txn_id != ctx->_snapshot_txn_id) {
        // Another context committed since our snapshot — merge required.
        // Switch active_txn to the pre-allocated workspace (next_txn_page)
        // so all merge allocations go there, leaving prepared_txn untouched
        // for crash recovery.
        ctx->active_txn = ctx->next_txn_page;
        _merge_into_committed(current, ctx);

        // Free old prepared page into the workspace's garbage collector,
        // then clone the merged result as the new prepared page.
        txn_ptr workspace = _resolve_active(ctx);
        _TxnResolver resolver{self(), workspace, ctx};
        offset_t old_prepared = ctx->prepared_txn;
        auto old_page = resolve<Transaction>(&old_prepared);
        workspace->mem_manager.free(static_cast<page_ptr>(old_page), resolver);
        auto cloned = workspace->clone(resolver);
        ctx->prepared_txn = resolve(cloned);
      }

      current->next_txn = ctx->prepared_txn;
      _header->read_txn = ctx->prepared_txn;
    }

    make_dirty(_header);
    flush(sync, true);
    end_transaction(ctx);
    return true;
  }

  // Merge writer's changes into the current committed state.
  // Called under merge_lock when a conflict is detected (another context
  // committed after our snapshot).
  void _merge_into_committed(txn_ptr committed, TxnContext* ctx) {
    using RawCursor = _Cursor<CursorTraits>;
    using MergePolicy = _ContextMergePolicy<Self>;

    txn_ptr active = _resolve_active(ctx);

    // Save writer's root and replace with committed root (dst = committed)
    offset_e writer_root = active->root;
    active->root = committed->root;

    // Move-merge writer's changes into committed trie
    if (writer_root != 0) {
      RawCursor dst_cursor(&self(), &active->root);
      dst_cursor._ctx = ctx;
      dst_cursor._active_tid = active->txn_id;
      RawCursor src_cursor(&self(), &writer_root);
      src_cursor._ctx = ctx;
      src_cursor._active_tid = active->txn_id;
      src_cursor.clear();  // push root onto stack for merger
      MergePolicy policy(&self(), ctx, ctx->_snapshot_txn_id);
      _MoveMerger<RawCursor, RawCursor, MergePolicy> merger(dst_cursor,
                                                            src_cursor, policy);
      merger.exec();
    }

    // Apply deletes: walk the delete_root trie and remove each key
    if (active->delete_root != 0) {
      RawCursor del_cursor(&self(), &active->delete_root);
      del_cursor._ctx = ctx;
      del_cursor._active_tid = active->txn_id;
      RawCursor main_cursor(&self(), &active->root);
      main_cursor._ctx = ctx;
      main_cursor._active_tid = active->txn_id;
      del_cursor.first();
      while (del_cursor.is_valid()) {
        main_cursor.find(del_cursor.key());
        if (main_cursor.is_valid()) {
          main_cursor.remove();
        }
        del_cursor.next();
      }
      active->delete_root = 0;
    }
  }

  void end_transaction(TxnContext* ctx) {
    ctx->active_txn = 0;
    _release_context(_ctx_index_of(ctx));
  }

  typedef _MemStatistics<Traits> MemStatistics;

  struct Statistics {
    MemStatistics garbage, branch, leaf, transaction;
  };

  void _garbage_statistics(MemStatistics& tofill) {
    using PageContainer = typename MemManager::PageContainer;
    txn_ptr txn_ = txn();
    const int garbage =
        MemManager::assign_slot(MemManager::PageContainer::SIZE);
    for (int i = 0; i < MemManager::COUNT; i++) {
      auto slot = txn_->mem_manager.slots[i];
      // collect blocks
      offset_t o = slot.ostart;
      if (!o) {
        assert(slot.count == 0 && "empty garbage slot with non-zero count");
        continue;
      }
      size_t count = 0;
      while (true) {
        typename MemManager::Slot::cont_ptr gc = resolve<PageContainer>(&o);
        count++;
        if (o == slot.oend) break;
        o = gc->next;
      }
      tofill.add(garbage, count);
      tofill.add(i, slot.count);
    }
  }

  void _node_statistics(Statistics& stat, offset_t offset) {
    typedef _TrieNode<Traits> TrieNode;
    typedef _LeafNode<Traits> LeafNode;
    using PageHeader = typename Traits::PageHeader;
    using trie_ptr = typename Traits::template Pointer<TrieNode>;
    using leaf_ptr = typename Traits::template Pointer<LeafNode, LEAF>;

    if (offset.type() == TRIE) {
      trie_ptr branch = resolve<TrieNode>(&offset);
      auto* hdr =
          reinterpret_cast<PageHeader*>((char*)branch - sizeof(PageHeader));
      stat.branch.add(
          hdr->slot_id, 1,
          PAGE_SIZES[hdr->slot_id] - sizeof(PageHeader) - branch->size());
      auto count = branch->count();
      offset_e* array = branch->array();
      for (int i = 0; i < count; i++) {
        _node_statistics(stat, array[i]);
      }
      return;
    }
    if (offset.type() == LEAF) {
      leaf_ptr leaf = resolve<LeafNode>(&offset);
      auto* hdr =
          reinterpret_cast<PageHeader*>((char*)leaf - sizeof(PageHeader));
      stat.leaf.add(
          hdr->slot_id, 1,
          PAGE_SIZES[hdr->slot_id] - sizeof(PageHeader) - leaf->size());
      return;
    }
  }

  void statistics(Statistics& stat) {
    _garbage_statistics(stat.garbage);
    _node_statistics(stat, txn()->root);

    iter_transactions([this, &stat](txn_ptr txn) -> bool {
      uint16_t bsize = PAGE_SIZES[txn->slot_id];
      stat.transaction.add(txn->slot_id, 1, bsize - sizeof(Transaction));
      return false;
    });
  }

  // Defragment big memory - merge adjacent free chunks across all contexts.
  void defrag() {
    if (!_aspect.before_defrag(self())) return;

    using RawCursor = _Cursor<CursorTraits>;
    using BigMemory = _BigMemory<RawCursor>;

    BigMemory big_mem(this, &txn()->free_bigmem_root);
    big_mem.defrag();

    _aspect.on_defrag(self());
  }

  void return_areas_range(TxnContext* ctx, offset_t start_single,
                          offset_t end_single, offset_t start_multi,
                          offset_t end_multi) {
    // Return area range [start->next ... end] to storage
    // This is used during rollback to return areas allocated during write
    // transaction

    if (start_single && end_single && start_single != end_single) {
      area_ptr start_area = resolve<Area>(&start_single, WRITE);
      offset_t range_head = start_area->next;
      _storage.return_single_areas(range_head, end_single);
      start_area->next = 0;
      make_dirty(start_area);
    }

    if (end_multi && start_multi != end_multi) {
      if (start_multi == 0) {
        // First-ever multi-area allocation: no committed tail to walk from,
        // return the whole chain from head.
        _storage.return_multi_areas(ctx->area_list_head_multi, end_multi);
        ctx->area_list_head_multi = 0;
      } else {
        area_ptr start_area = resolve<Area>(&start_multi, WRITE);
        offset_t range_head = start_area->next;
        _storage.return_multi_areas(range_head, end_multi);
        start_area->next = 0;
        make_dirty(start_area);
      }
    }
  }

  void sanitize() {
    new (&_header->txn_ref_lock) SpinLock();
    new (&_header->ctx_wait_lock) typename Storage::CtxMutex();
    new (&_header->ctx_wait_cv) typename Storage::CtxCondVar();
    new (&_header->merge_lock) SpinLock();
    _header->_last_assigned_tid = txn()->txn_id;
    iter_transactions([this](txn_ptr txn) -> bool {
      txn->refs.store(0);
      txn->mem_manager.reinit_locks();
      make_dirty(txn);
      return false;
    });

    // Sanitize each transaction context.
    for (uint8_t i = 0; i < _num_contexts; i++) {
      auto* ctx = context(i);
      ctx->_claimed.store(0, std::memory_order_relaxed);

      // Crash during merge: active_txn was switched to workspace
      // (next_txn_page) but merge/publish didn't complete.  Restore
      // next_txn_page from the untouched prepared_txn page.
      if (ctx->active_txn != 0 &&
          ctx->active_txn == ctx->next_txn_page &&
          ctx->prepared_txn != ctx->active_txn) {
        auto prepared = resolve<Transaction>(&ctx->prepared_txn);
        auto next = resolve<Transaction>(&ctx->next_txn_page, WRITE);
        memcpy((void*)&*next, (void*)&*prepared, sizeof(Transaction));
        next->mem_manager.reinit_locks();
        new (&next->refs) std::atomic<uint32_t>(0);
        make_dirty(next);
      } else if (ctx->next_txn_page) {
        // Normal reinit of locks/refs on pre-allocated page.
        auto next = resolve<Transaction>(&ctx->next_txn_page);
        next->refs.store(0);
        next->mem_manager.reinit_locks();
        make_dirty(next);
      } else if (i == 0) {
        // Context 0 must always have a next_txn_page.
        txn_ptr read_txn = resolve<Transaction>(&_header->read_txn);
        _TxnResolver resolver{self(), read_txn, ctx};
        auto next = read_txn->clone(resolver);
        ctx->next_txn_page = resolve(next);
      }
      // Contexts 1..N-1 with next_txn_page==0 are uninitialised — left as-is.

      // Restore active_txn for consistency after any crash.
      if (ctx->active_txn != 0) {
        ctx->active_txn = ctx->prepared_txn;
      } else if (ctx->prepared_txn != _header->read_txn) {
        // Prepared but not committed — restore active_txn so the
        // prepared transaction can be finalized on next open.
        ctx->active_txn = ctx->prepared_txn;
      }
    }

    make_dirty(_header);
    flush();
    _aspect.on_sanitize(self());
  }
};

}  // namespace leaves

#endif  // _LEAVES__DB_HPP