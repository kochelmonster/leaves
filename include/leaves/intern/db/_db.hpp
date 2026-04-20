#ifndef _LEAVES__DB_HPP
#define _LEAVES__DB_HPP

#include <array>
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

  // trie root for tracking deleted keys (used during merge commit)
  offset_e delete_root{0};

  // pointer to the oldest transaction
  offset_e start_txn;

  // pointer to the next higher transaction
  offset_e next_txn;

  // Area tracking: tail pointers for areas allocated during this transaction
  offset_e area_list_tail_single{0};
  offset_e area_list_tail_multi{0};

  MemManager mem_manager;

  // count of cursors accessing this transaction
  std::atomic<uint32_t> refs;
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
    page_ptr result = mem_manager.alloc(slot, resolver);
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
  using offset_e = typename Storage_::Traits::offset_e;

  offset_e prepared_txn{0};   // transaction being prepared for commit
  offset_e next_txn_page{0};  // pre-allocated next txn page
  offset_e active_txn{0};     // offset of active transaction (set during txn)
  offset_e last_txn{0};       // offset of last transaction (for rollback)
  offset_e area_list_head_single{0};  // head of single-AREA_SIZE area chain
  offset_e area_list_head_multi{0};   // head of multi-AREA_SIZE area chain
  offset_e _snapshot_txn{0};  // snapshot txn offset (ref held for txn lifetime)
  tid_t _recycle_txn_id{0};   // per-context recycle threshold
  std::atomic<uint8_t> _claimed{0};  // 0 = free, 1 = claimed (CAS to acquire)
  uint8_t _needs_arena_reset{0};  // 1 = worker needs arena reset on next txn
  std::atomic<uint8_t> _merge_done{0};  // 1 = leader merged this context's delta
};

// Default database header (defined outside _DB for reusability)
template <typename Storage_>
struct _DBHeader {
  using offset_e = typename Storage_::Traits::offset_e;

  // Cross-process wait mechanism for blocking context claims.
  // Types come from Storage_ so they match the storage backend.
  typename Storage_::CtxMutex ctx_wait_lock;
  typename Storage_::CtxCondVar ctx_wait_cv;

  offset_e read_txn;      // the current read transaction
  SpinLock txn_ref_lock;  // protects txn() + refs.fetch_add() atomicity

  // Global merge lock — serializes merge-on-commit when parallel
  // contexts conflict.  Cross-process safe (pure hardware atomics).
  SpinLock merge_lock;

  // Group commit: bitmap of contexts waiting for leader to merge their delta.
  // Bit i set = context i has called prepare_commit and is waiting.
  std::atomic<uint32_t> _pending_merge_bitmap{0};

  // Tracks whether this DB has been sanitized for the current storage
  // generation.  Compared against the FileHeader counter at open() time.
  uint32_t sanitize_generation;

  // Monotonic counter for unique txn_id assignment.  Incremented under
  // txn_ref_lock so parallel start_transaction() calls never collide.
  tid_t _last_assigned_tid{0};

  // Identifies the DB subtype for runtime type verification on open.
  // Written by init(), checked by open<>() to prevent type mismatches.
  uint16_t db_type_id;

  uint8_t num_contexts;  // number of parallel transaction contexts
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
  tid_t _writer_txn_id;
  tid_t _main_txn_id;
  // When non-zero, used by deferred commits to descend ALL worker-written nodes
  // (all batches since divergence, not just the current batch's txn_id).
  tid_t _diverge_txn_id;

  _ContextMergePolicy(DB_* db, TxnContext* ctx, tid_t writer_txn_id,
                      tid_t main_txn_id, tid_t diverge_txn_id = tid_t(0))
      : _db(db), _ctx(ctx), _writer_txn_id(writer_txn_id),
        _main_txn_id(main_txn_id), _diverge_txn_id(diverge_txn_id) {}

  // Descend into src nodes allocated by the writer.
  // Deferred mode (diverge_txn_id set): descend all batches since divergence.
  // Normal mode: descend only the current batch (txn_id == writer).
  template <typename PagePtr>
  bool should_descend_src(PagePtr page) {
    if (_diverge_txn_id != tid_t(0))
      return page->txn_id > _diverge_txn_id;
    return page->txn_id == _writer_txn_id;
  }

  // No-op: worker context areas are reset wholesale after commit.
  // Individual node freeing is not needed.
  template <typename NodePtr>
  void free_src(NodePtr& node) {}

  // Only free dst nodes allocated in this merge transaction.
  // Snapshot nodes (from the committed chain) are shared with the worker
  // tree — freeing them could cause the worker's src_cursor to read garbage
  // if the freed page gets reallocated during the same merge.
  template <typename PagePtr>
  bool should_free_dst(PagePtr page) {
    return page->txn_id == _main_txn_id;
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

  struct _DeferredCtxState {
    offset_e accumulated_txn{0};      // last deferred prepared_txn offset
    uint32_t pending_commit_count{0}; // deferred batches since last flush
    tid_t diverge_txn_id{0};          // snapshot txn_id at divergence
    uint8_t in_deferred_sequence{0};  // 1 = context holds deferred state
  };

  Storage& _storage;
  header_ptr _header;
  std::string _name;
  uint8_t _num_contexts;
  // When > 0, workers defer merging to main until this many batches accumulate.
  // 0 = always flush immediately (default, backward-compatible behavior).
  uint32_t _deferred_flush_threshold{0};
  // Runtime-only deferred state per context. Kept out of TxnContext to avoid
  // changing persisted layout used by graph-identity tests.
  std::array<_DeferredCtxState, 64> _deferred_ctx{};

  [[no_unique_address]] Aspect _aspect{};

  // Stored context count in header memory.
  // Single-thread mode stores only main context (0).
  // Multi-thread mode stores main context (0) plus worker contexts (1..N).
  uint8_t _context_count() const {
    if (_num_contexts <= 1) return 1;
    return static_cast<uint8_t>(_num_contexts + 1);
  }

  bool _is_multithread_mode() const { return _num_contexts > 1; }

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
    return padding(sizeof(Header) + _context_count() * sizeof(TxnContext),
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

  _DeferredCtxState& _deferred(TxnContext* ctx) {
    uint8_t idx = _ctx_index_of(ctx);
    assert(idx < _deferred_ctx.size());
    return _deferred_ctx[idx];
  }
  const _DeferredCtxState& _deferred(const TxnContext* ctx) const {
    uint8_t idx = static_cast<uint8_t>(ctx - context(0));
    assert(idx < _deferred_ctx.size());
    return _deferred_ctx[idx];
  }
  bool _ctx_in_deferred_sequence(const TxnContext* ctx) const {
    return _deferred(ctx).in_deferred_sequence != 0;
  }
  void _clear_deferred(TxnContext* ctx) {
    auto& d = _deferred(ctx);
    d.accumulated_txn = 0;
    d.pending_commit_count = 0;
    d.diverge_txn_id = tid_t(0);
    d.in_deferred_sequence = 0;
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
    typename std::enable_if<!std::is_pointer<Pointer>::value, offset_e>::type
    resolve(const Pointer& p) const {
      return _db._storage.resolve(p);
    }

    template <typename T, typename PagePtr>
    RecycleResult may_recycle(T& garbage_block, PagePtr page) const {
      // Page already recycled by another garbage entry — skip
      if (page->txn_id != garbage_block.org_txn_id) return RecycleResult::SKIP;
      // Entry too new — stop (queue is ordered within context)
      if (!(garbage_block.txn_id < _ctx->_recycle_txn_id))
        return RecycleResult::STOP;
      // CAS-stamp the page to claim it
      auto expected = garbage_block.org_txn_id._value;
      std::atomic_ref<uint32_t> atomic_tid(page->txn_id._value);
      if (!atomic_tid.compare_exchange_weak(
              expected, _active_txn->txn_id._value, std::memory_order_acquire,
              std::memory_order_relaxed))
        return RecycleResult::SKIP;
      return RecycleResult::RECYCLE;
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

  _DB(Storage& storage, offset_e header, std::string_view name,
      uint8_t num_contexts = 1)
      : _storage(storage),
        _header(storage.resolve(&header, READ)),
        _name(name),
        _num_contexts(num_contexts) {
    if (_header->num_contexts != _context_count()) throw TypeMismatch();
  }

  _DB(Storage& storage, offset_e* header, std::string_view name,
      uint8_t num_contexts = 1)
      : _storage(storage), _name(name), _num_contexts(num_contexts) {
    init(header);
  }

  Self& self() { return *static_cast<Self*>(this); }

  void init(offset_e* header) {
    auto area_ptr = _storage.alloc_single_area();

    *header = area_ptr->content_offset();  // Use content_offset, not get_offset
    _header = _storage.resolve(header, READ);
    memset((char*)_header, 0, sizeof(Header));
    _header->db_type_id = Self::DB_TYPE_ID;
    _header->num_contexts = _context_count();
    new (&_header->txn_ref_lock) SpinLock();
    new (&_header->ctx_wait_lock) typename Storage::CtxMutex();
    new (&_header->ctx_wait_cv) typename Storage::CtxCondVar();
    new (&_header->merge_lock) SpinLock();
    new (&_header->_pending_merge_bitmap) std::atomic<uint32_t>(0);
    auto* ctx = context(0);
    memset(ctx, 0, _context_count() * sizeof(TxnContext));
    ctx->_claimed.store(0, std::memory_order_relaxed);
    offset_e first_area_offset = _storage.resolve(area_ptr);
    ctx->area_list_head_single = first_area_offset;
    ctx->area_list_head_multi = 0;

    // Initialize remaining contexts (all start as unclaimed).
    for (uint8_t i = 1; i < _context_count(); i++)
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
    for (uint8_t i = 0; i < _context_count(); i++) {
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
  void reset(offset_e* header) {
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
  typename std::enable_if<!std::is_pointer<Pointer>::value, offset_e>::type
  resolve(const Pointer& p) const {
    return _storage.resolve(p);
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
    auto tail = resolve<Area>(&active->area_list_tail_single, WRITE);

    // Reuse existing next area in the chain (hot pages from previous txn).
    if (tail->next != 0) {
      active->area_list_tail_single = tail->next;
      auto reused = resolve<Area>(&active->area_list_tail_single, WRITE);
      make_dirty(reused);
      return reused;
    }

    // No reusable areas — allocate from global pool.
    std::scoped_lock lock(_storage.file_lock());

    auto ap = _storage.alloc_single_area();
    ap->next = 0;

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
  void iter_transactions(offset_e traversal_head, T caller) const {
    txn_ptr read_txn = resolve<Transaction>(&traversal_head);
    offset_e cur_off = read_txn->start_txn;

    while (cur_off != 0) {
      txn_ptr txn = resolve<Transaction>(&cur_off);
      // Read next before callback because callback may free txn.
      offset_e next_off = txn->next_txn;
      if (caller(txn)) break;

      // Terminate by chain links, not txn_id ordering.
      if (cur_off == traversal_head) break;
      if (next_off == 0 || next_off == cur_off) break;
      cur_off = next_off;
    }
  }

  txn_ptr txn() const { return resolve<Transaction>(&_header->read_txn); }

  // Return the raw offset of the current read transaction.
  // This is a plain aligned 64-bit load — no lock required.
  offset_e txn_offset() const { return _header->read_txn; }

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
  txn_ptr txn_ref_at(offset_e offset) {
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
    for (uint8_t i = 0; i < _context_count(); i++) {
      if (context(i)->active_txn) return true;
    }
    bool is_active_ = false;
    iter_transactions(txn_offset(), [&is_active_](txn_ptr txn) -> bool {
      if (txn->refs.load() > 0) is_active_ = true;
      return is_active_;
    });

    return is_active_;
  }

  // Try to CAS-claim any free context. Returns index or UINT8_MAX.
  uint8_t _try_claim_any() {
    uint8_t begin = _is_multithread_mode() ? 1 : 0;
    uint8_t end = _is_multithread_mode() ? _context_count() : 1;
    for (uint8_t i = begin; i < end; i++) {
      uint8_t expected = 0;
      if (context(i)->_claimed.compare_exchange_weak(expected, 1,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed))
        return i;
    }
    return UINT8_MAX;
  }

  // Claim main context (index 0). In commit path this blocks until available.
  TxnContext* _claim_main_context() {
    uint8_t expected = 0;
    if (context(0)->_claimed.compare_exchange_weak(expected, 1,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed))
      return context(0);

    std::unique_lock lk(_header->ctx_wait_lock);
    while (true) {
      expected = 0;
      if (context(0)->_claimed.compare_exchange_weak(
              expected, 1, std::memory_order_acquire,
              std::memory_order_relaxed))
        return context(0);
      _header->ctx_wait_cv.wait(lk);
    }
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

    offset_e txn_offset = ap->content_offset();
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
    // Set prepared_txn != next_txn_page so prepare_commit() always clones
    // and keeps next_txn_page valid for the following transaction.
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
    start_ctx_transaction(ctx);
    return ctx;
  }

  void start_ctx_transaction(TxnContext* ctx) {
    auto& deferred = _deferred(ctx);
    // Deferred continuation: arena is preserved, next_txn_page is a clone of the
    // accumulated state (root + delete_root already set by previous prepare_commit).
    if (deferred.in_deferred_sequence) {
      assert(ctx->next_txn_page);
      txn_ptr active = resolve<Transaction>(&ctx->next_txn_page);
      ctx->active_txn = ctx->next_txn_page;
      ctx->next_txn_page = 0;
      ctx->last_txn = ctx->prepared_txn;
      ctx->prepared_txn = 0;
      // active->root and active->delete_root already reflect accumulated state.
      // Keep ctx->_snapshot_txn (ref held from first batch in this sequence).
      // Keep deferred.diverge_txn_id (same divergence point throughout sequence).
      {
        std::lock_guard<SpinLock> guard(_header->txn_ref_lock);
        _header->_last_assigned_tid =
            tid_t(static_cast<uint64_t>(_header->_last_assigned_tid) + 1);
        active->txn_id = _header->_last_assigned_tid;
      }
      ctx->_recycle_txn_id = active->txn_id;
      active->next_txn = 0;
      return;
    }

    // Normal path: lazy reset for worker contexts in multithread mode.
    // Must run BEFORE the next_txn_page check because _reset_worker_context
    // sets next_txn_page, preventing a redundant _init_context allocation.
    if (ctx->_needs_arena_reset) {
      _reset_worker_context(ctx);
      ctx->_needs_arena_reset = 0;
    }

    // Lazy-init contexts that have never been used.
    if (!ctx->next_txn_page) {
      uint8_t idx = _ctx_index_of(ctx);
      _init_context(idx);
    }

    // Pre-allocated page is always ready (from commit, rollback, or init)
    assert(ctx->next_txn_page);
    txn_ptr active = resolve<Transaction>(&ctx->next_txn_page);
    ctx->active_txn = ctx->next_txn_page;
    ctx->next_txn_page = 0;
    ctx->last_txn = ctx->prepared_txn;
    // Ensure prepared_txn != active_txn so prepare_commit() always clones
    // a fresh next_txn_page (the clone will have refs=0).
    ctx->prepared_txn = 0;

    txn_ptr snapshot_txn = txn_ref();
    ctx->_snapshot_txn = txn_offset();
    active->root = snapshot_txn->root;
    {
      std::lock_guard<SpinLock> guard(_header->txn_ref_lock);
      _header->_last_assigned_tid =
          tid_t(static_cast<uint64_t>(_header->_last_assigned_tid) + 1);
      active->txn_id = _header->_last_assigned_tid;
    }
    assert(active->delete_root == 0);
    // Record divergence point so deferred-commit merges can identify all
    // worker-written nodes (txn_id > diverge_txn_id) across multiple batches.
    deferred.diverge_txn_id = snapshot_txn->txn_id;

    ctx->_recycle_txn_id = active->txn_id;
    active->next_txn = 0;

    _TxnResolver resolver{self(), active, ctx};
    iter_transactions(txn_offset(), [this, &resolver](txn_ptr txn) -> bool {
      if (txn->refs.load() > 0) {
        resolver._active_txn->start_txn = resolve(txn);
        resolver._ctx->_recycle_txn_id = txn->txn_id;
        return true;
      }

      static_cast<Self*>(this)->_on_txn_freed(txn);
      resolver._active_txn->mem_manager.free(txn, resolver);
      return false;
    });
  }

  void reset_context(TxnContext* ctx) {
    txn_ptr active = _resolve_active(ctx);
    txn_ptr last = resolve<Transaction>(&ctx->last_txn);
    return_areas_range(
        ctx, last->area_list_tail_single, active->area_list_tail_single,
        last->area_list_tail_multi, active->area_list_tail_multi);

    ctx->active_txn = ctx->prepared_txn = ctx->last_txn;

    memcpy(&*active, &*last, sizeof(Transaction));
    active->mem_manager.reinit_locks();
    new (&active->refs) std::atomic<uint32_t>(0);
    ctx->next_txn_page = resolve(active);

    make_dirty(active);
    make_dirty(_header);
  }

  bool rollback(TxnContext* ctx,
                TransactionOrigin origin = TransactionOrigin::user) {
    if (_is_multithread_mode()) {
      // Mark for lazy arena reset on next start_ctx_transaction.
      ctx->_needs_arena_reset = 1;
      // NOTE: Do NOT call end_transaction(ctx) here — same race as commit.
      // Caller must switch _txn first, then call end_transaction(ctx).
    } else {
      reset_context(ctx);
      end_transaction(ctx);
    }
    flush();
    return true;
  }

  // Reset a worker context's transaction to its initial state.
  // All arenas stay attached (hot pages), but the transaction is reset
  // so the worker can start a fresh transaction reusing its arenas.
  void _reset_worker_context(TxnContext* ctx) {
    // Resolve first area in the worker's single-area chain.
    area_ptr first_area = resolve<Area>(&ctx->area_list_head_single, WRITE);
    offset_e txn_offset = first_area->content_offset();

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
    wtxn->area_list_tail_single = ctx->area_list_head_single;
    wtxn->area_list_tail_multi = 0;
    wtxn->mem_manager.init(txn_offset + PAGE_SIZES[Transaction::SLOT_ID],
                           first_area->offset() + first_area->size());

    ctx->next_txn_page = txn_offset;
    // Set prepared_txn != next_txn_page so prepare_commit() always clones.
    ctx->prepared_txn = 0;
    ctx->active_txn = 0;

    _clear_deferred(ctx);

    make_dirty(wtxn);
    make_dirty(_header);
  }

  tid_t prepare_commit(TxnContext* ctx, bool sync = false,
                       TransactionOrigin origin = TransactionOrigin::user) {
    txn_ptr active = _resolve_active(ctx);

    // already prepared
    if (ctx->prepared_txn == ctx->active_txn) return active->txn_id;

    // Performance hack: Pre-allocate next transaction page before committing.
    // The allocation modifies active->mem_manager, which gets persisted
    // as part of this commit — no storage leak.
    _TxnResolver resolver{self(), active, ctx};
    auto next = active->clone(resolver);
    ctx->next_txn_page = resolve(next);
    ctx->prepared_txn = ctx->active_txn;

    make_dirty(_header);
    make_dirty(active);

    if (sync) flush(true, true);  // Only flush if explicitly requested
    return active->txn_id;
  }

  bool commit(TxnContext* ctx, bool sync = false,
              TransactionOrigin origin = TransactionOrigin::user) {
    if (!prepare_commit(ctx, false, origin)) return false;

    txn_ptr current = txn();

    if (!_is_multithread_mode()) {
      // hot path
      current->next_txn = ctx->prepared_txn;
      make_dirty(current);
      make_dirty(resolve<Transaction>(&ctx->prepared_txn));
      _header->read_txn = ctx->prepared_txn;
    } else {
      // Deferred commit: accumulate locally, flush to main only at threshold.
      // Workers build an accumulated snapshot — each next batch continues from
      // the previous prepared_txn root so read-your-own-writes is preserved.
      auto& deferred = _deferred(ctx);
      if (_deferred_flush_threshold > 0 && !sync &&
          deferred.pending_commit_count + 1 < _deferred_flush_threshold) {
        deferred.accumulated_txn = ctx->prepared_txn;
        deferred.pending_commit_count++;
        deferred.in_deferred_sequence = 1;
        return true;
      }

      // At threshold (or sync=true, or threshold disabled): flush accumulated state.
      // Clear deferred flags BEFORE group merge so cursor takes the flush path.
      deferred.accumulated_txn = ctx->prepared_txn;
      deferred.pending_commit_count = 0;
      deferred.in_deferred_sequence = 0;

      // Group commit: try to become leader, or enqueue for a leader to merge us.
      uint8_t idx = _ctx_index_of(ctx);
      bool i_am_leader = false;

      // Fast path: try to claim main context immediately
      uint8_t exp = 0;
      if (context(0)->_claimed.compare_exchange_strong(
              exp, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        i_am_leader = true;
      } else {
        // Slow path: enqueue our context in the pending bitmap and wait
        ctx->_merge_done.store(0, std::memory_order_relaxed);
        _header->_pending_merge_bitmap.fetch_or(1u << idx,
                                                std::memory_order_release);

        std::unique_lock lk(_header->ctx_wait_lock);
        while (true) {
          // Check if a leader already merged us
          if (ctx->_merge_done.load(std::memory_order_acquire)) break;
          // Try to become leader ourselves (main may have been released)
          exp = 0;
          if (context(0)->_claimed.compare_exchange_weak(
                  exp, 1, std::memory_order_acquire,
                  std::memory_order_relaxed)) {
            // Remove our pending bit — we'll merge ourselves as leader
            _header->_pending_merge_bitmap.fetch_and(~(1u << idx),
                                                     std::memory_order_relaxed);
            i_am_leader = true;
            break;
          }
          _header->ctx_wait_cv.wait(lk);
        }
        lk.unlock();
      }

      if (i_am_leader) {
        _do_leader_merge(ctx, sync, origin);
      }
    }

    if (!_is_multithread_mode()) {
      make_dirty(_header);
      flush(sync, true);
      end_transaction(ctx);
    } else {
      // Mark worker context for lazy arena reset on next start_ctx_transaction.
      ctx->_needs_arena_reset = 1;
      // NOTE: Do NOT call end_transaction(ctx) here.  The cursor still
      // holds _txn pointing into the worker arena.  Releasing the context
      // would allow another thread to claim it and memset the arena,
      // corrupting the cursor's _txn.  Caller must switch _txn first,
      // then call end_transaction(ctx).
    }
    return true;
  }

  // Merge writer's changes into the current committed state.
  // Uses _Merger with _ContextMergePolicy for O(delta) merge:
  //  - should_descend_src skips snapshot subtrees in O(1)
  //  - should_free_dst prevents freeing shared snapshot nodes
  //  - selective_deep_copy copies writer nodes into main arena
  void _merge_to_main(TxnContext* dst, TxnContext* src) {
    using RawCursor = _Cursor<CursorTraits>;
    using MergePolicy = _ContextMergePolicy<Self>;
    using Merger = _Merger<RawCursor, RawCursor, MergePolicy>;

    txn_ptr dst_txn = _resolve_active(dst);
    txn_ptr src_txn = _resolve_active(src);

    // dst cursor: main context, writable (COW via active_tid)
    RawCursor dst_cursor(&self(), &dst_txn->root);
    dst_cursor._ctx = dst;
    dst_cursor._active_tid = dst_txn->txn_id;

    // src cursor: worker context, read-only navigation
    RawCursor src_cursor(&self(), &src_txn->root);
    src_cursor._ctx = src;
    src_cursor._active_tid = tid_t(0);

    // Push src root so the merger can start from it
    if (src_txn->root != 0) {
      src_cursor.push(&src_txn->root);
    }

    MergePolicy policy(&self(), dst, src_txn->txn_id, dst_txn->txn_id,
                       _deferred(src).diverge_txn_id);
    Merger merger(dst_cursor, src_cursor, policy);
    merger.exec();

    // Apply deletes: walk the delete_root trie and remove each key
    if (src_txn->delete_root != 0) {
      RawCursor del_cursor(&self(), &src_txn->delete_root);
      del_cursor._ctx = src;
      del_cursor._active_tid = tid_t(0);
      del_cursor.first();
      while (del_cursor.is_valid()) {
        dst_cursor.find(del_cursor.key());
        if (dst_cursor.is_valid()) {
          dst_cursor.remove();
        }
        del_cursor.next();
      }
      src_txn->delete_root = 0;
    }
  }

  // Group commit: leader merges its own delta plus all pending worker deltas
  // in a single main-context round, avoiding N sequential claim/release cycles.
  void _do_leader_merge(TxnContext* my_ctx, bool sync,
                        TransactionOrigin origin) {
    txn_ptr current = txn();
    TxnContext* main = context(0);  // already claimed by caller

    start_ctx_transaction(main);
    _merge_to_main(main, my_ctx);

    // Drain pending merges — loop because new ones may arrive while we drain
    while (true) {
      uint32_t bitmap = _header->_pending_merge_bitmap.exchange(
          0, std::memory_order_acq_rel);
      if (!bitmap) break;
      while (bitmap) {
        // Portable lowest-bit extraction
        uint32_t lowest = bitmap & (~bitmap + 1);
        int i = 0;
        uint32_t tmp = lowest;
        while (tmp >>= 1) ++i;
        bitmap &= bitmap - 1;  // clear lowest set bit

        _merge_to_main(main, context(i));
        context(i)->_needs_arena_reset = 1;
        context(i)->_merge_done.store(1, std::memory_order_release);
      }
    }

    prepare_commit(main, false, origin);
    current->next_txn = main->prepared_txn;
    make_dirty(current);
    make_dirty(resolve<Transaction>(&main->prepared_txn));
    _header->read_txn = main->prepared_txn;
    end_transaction(main);

    make_dirty(_header);
    flush(sync, true);

    // Wake all waiting threads (both group-merged and those waiting to claim)
    {
      std::lock_guard lk(_header->ctx_wait_lock);
    }
    _header->ctx_wait_cv.notify_all();
  }

  // Recursively walk trie nodes modified by the writer (txn_id match).
  // Snapshot subtrees (different txn_id) are skipped entirely — O(1) per skip.
  // Only calls cb for modified leaf nodes with the reconstructed full key.
  template <typename TrieNode, typename LeafNode, typename Callback>
  void _walk_modified_leaves(offset_e off, tid_t writer_tid,
                             std::string& key, Callback&& cb) {
    using PageHeader = typename Traits::PageHeader;
    if (off == 0) return;

    if (off.type() == LEAF) {
      auto leaf = resolve<LeafNode>(&off);
      auto page = reinterpret_cast<const PageHeader*>(
          reinterpret_cast<const char*>(&*leaf) - sizeof(PageHeader));
      if (page->txn_id != writer_tid) return;
      size_t saved = key.size();
      key.append((const char*)leaf->data, leaf->key_size);
      cb(Slice(key), leaf->value(), leaf->is_big());
      key.resize(saved);
    } else {
      auto trie = resolve<TrieNode>(&off);
      auto page = reinterpret_cast<const PageHeader*>(
          reinterpret_cast<const char*>(&*trie) - sizeof(PageHeader));
      if (page->txn_id != writer_tid) return;
      size_t saved = key.size();
      key.append((const char*)trie->compressed(), trie->len());
      trie->for_each_branch([&](int k, auto* child_off) {
        if (k != TrieNode::NONE) key.push_back(k);
        _walk_modified_leaves<TrieNode, LeafNode>(*child_off, writer_tid, key, cb);
        if (k != TrieNode::NONE) key.pop_back();
      });
      key.resize(saved);
    }
  }

  void end_transaction(TxnContext* ctx) {
    // Release the snapshot ref held since start_transaction().
    if (ctx->_snapshot_txn) {
      txn_ptr snap = resolve<Transaction>(&ctx->_snapshot_txn);
      snap->refs.fetch_sub(1);
      ctx->_snapshot_txn = 0;
    }
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
      offset_e o = slot.ostart;
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

  void _node_statistics(Statistics& stat, offset_e offset) {
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

    iter_transactions(txn_offset(), [this, &stat](txn_ptr txn) -> bool {
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

  void return_areas_range(TxnContext* ctx, offset_e start_single,
                          offset_e end_single, offset_e start_multi,
                          offset_e end_multi) {
    // Return area range [start->next ... end] to storage
    // This is used during rollback to return areas allocated during write
    // transaction

    if (start_single && end_single && start_single != end_single) {
      area_ptr start_area = resolve<Area>(&start_single, WRITE);
      offset_e range_head = start_area->next;
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
        offset_e range_head = start_area->next;
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
    new (&_header->_pending_merge_bitmap) std::atomic<uint32_t>(0);
    _header->_last_assigned_tid = txn()->txn_id;
    iter_transactions(txn_offset(), [this](txn_ptr txn) -> bool {
      txn->refs.store(0);
      txn->mem_manager.reinit_locks();
      make_dirty(txn);
      return false;
    });

    // Sanitize each transaction context.
    for (uint8_t i = 0; i < _context_count(); i++) {
      auto* ctx = context(i);
      ctx->_claimed.store(0, std::memory_order_relaxed);
      ctx->_merge_done.store(0, std::memory_order_relaxed);

      // Crash during merge: active_txn was switched to workspace
      // (next_txn_page) but merge/publish didn't complete.  Restore
      // next_txn_page from the untouched prepared_txn page.
      if (ctx->active_txn != 0 && ctx->active_txn == ctx->next_txn_page &&
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

      // Snapshot ref is void after crash (refs were zeroed above).
      ctx->_snapshot_txn = 0;

      // Restore active_txn for consistency after any crash.
      // Clear deferred commit state — not valid across restarts.
      _clear_deferred(ctx);

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