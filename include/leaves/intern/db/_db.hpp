#ifndef _LEAVES__DB_HPP
#define _LEAVES__DB_HPP

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "../core/_port.hpp"
#include "../memory/_memory.hpp"
#include "../storage/_wal.hpp"
#include "../util/_page_hist.hpp"
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

  // pointer to the active offset memory manager root
  offset_e offset_root;

  // pointer to the active bigmem freelist trie root
  offset_e free_bigmem_root;

  // pointer to the oldest transaction
  offset_e start_txn;

  // pointer to the next higher transaction
  offset_e next_txn;

  // count of cursors accessing this transaction
  std::atomic<uint32_t> refs;

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

// Default database header (defined outside _DB for reusability)
template <typename Storage_>
struct _DBHeader {
  offset_t read_txn;       // the current read transaction
  offset_t prepared_txn;   // the transaction being prepared for commit
  offset_t next_txn_page;  // Optimation: preprepared transaction
  SpinLock txn_lock;
  SpinLock txn_ref_lock;  // protects txn() + refs.fetch_add() atomicity
  std::atomic<uint64_t>
      txn_cursor_id;  // the id of the cursor holding the transaction

  // Atomic area management - no AreaList objects, just head pointers
  // Areas are linked lists in storage, operations use atomic head/tail pattern
  offset_t area_list_head_single;  // head of single AREA_SIZE areas linked list
  offset_t area_list_head_multi;   // head of multi-AREA_SIZE areas linked list

  // Identifies the DB subtype for runtime type verification on open.
  // Written by init(), checked by open<>() to prevent type mismatches.
  uint16_t db_type_id;

  // Tracks whether this DB has been sanitized for the current storage
  // generation.  Compared against the FileHeader counter at open() time.
  uint32_t sanitize_generation;

  // Extra offset for storing additional user-defined data beyond the header.
  // Defaults to 0 (no extra data).
  offset_t extra_offset{0};

  WalState wal;
};

// Write-Ahead Log mixin for _DB — provides WAL open/recover/begin/put/delete/
// prepare/commit/abort operations via CRTP. The derived type (_DB) must expose
// _storage, _name, and create_cursor().
template <typename Derived_>
struct _WalDbMixin {
  using Derived = Derived_;

  SpinLock _flush_lock;  // avoid DB is destroyed during a flush
  _WalWriter _wal;
  uint64_t _wal_flush_threshold{100 * 1024 * 1024};  // 1 MB default


  bool wal_enabled() const { return _wal.is_open(); }

  std::string wal_base_path() const {
    return std::string(_derived()._storage.filename()) + "." + _derived()._name;
  }

  void open_wal() {
    if (_wal.is_open()) return;
    // Serialise WAL open across processes so that only one process initialises
    // the WAL state in the DB Header.
    std::scoped_lock lock(_derived()._storage.file_lock());
    if (_wal.is_open()) return;  // double-check after acquiring lock
    if (!_wal.open(wal_base_path(), &_derived()._header->wal))
      throw std::runtime_error("failed to open WAL files");
  }

  void wal_recover() {
    _WalTxn dangling;
    std::vector<_WalTxn> all;
    _wal.parse(wal_base_path(), all, &dangling);

    tid_t read_txn_id = _derived().txn()->txn_id;

    // Replay committed transactions that are newer than the read transaction.
    // Any transaction whose txn_id <= read_txn_id was already applied.
    bool have_committed = false;
    if (!all.empty()) {
      auto cursor = _derived().create_cursor();
      for (const _WalTxn& t : all) {
        if (tid_t(t.txn_id) <= read_txn_id) continue;
        have_committed = true;
        cursor->start_transaction();
        for (const _WalOpRecord& op : t.ops) {
          cursor->find(Slice(op.key));
          if (op.is_delete) {
            if (cursor->is_valid()) cursor->remove();
          } else {
            cursor->value(Slice(op.val));
          }
        }
        cursor->commit(true);
      }
    }

    // Handle the dangling (prepared but not committed) transaction.
    // Apply its operations and leave it as prepared_txn so that sanitize()
    // can restore it as the active write transaction.
    if (dangling.txn_id != 0 && tid_t(dangling.txn_id) > read_txn_id) {
      auto cursor = _derived().create_cursor();
      cursor->start_transaction();
      for (const _WalOpRecord& op : dangling.ops) {
        cursor->find(Slice(op.key));
        if (op.is_delete) {
          if (cursor->is_valid()) cursor->remove();
        } else {
          cursor->value(Slice(op.val));
        }
      }
      // prepare_commit via cursor uses cursor's own _id (which matches
      // txn_cursor_id set by start_transaction).  This sets prepared_txn
      // but does NOT advance read_txn — the transaction stays "prepared"
      // so sanitize() can restore it as the active write transaction.
      cursor->prepare_commit(false);
      _derived().end_transaction();
      have_committed = true;
    }

    // Flush replayed data to storage before removing WAL files so crash
    // after this point does not lose recovered state.
    if (have_committed)
      _derived().flush(true, true);
    _wal.reset(wal_base_path());
  }

  void wal_begin(uint32_t txn_id) { _wal.begin(txn_id); }
  void wal_put(const Slice& key, const Slice& val) { _wal.put(key, val); }
  void wal_delete(const Slice& key) { _wal.del(key); }
  void wal_prepare(bool skip_sync = false) { _wal.prepare(skip_sync); }
  void set_wal_flush_threshold(uint64_t bytes) { _wal_flush_threshold = bytes; }

  void wal_commit(uint32_t txn_id) {
    _wal.commit();
    if (_wal.active_data_size() >= _wal_flush_threshold) {
      _derived()._storage.submit_task([this, txn_id] {
        std::lock_guard<SpinLock> lock(_flush_lock);
        _derived()._storage.flush(true, true);
        _wal.flushed(txn_id);
      });
    }
  }
  void wal_abort() { _wal.abort(); }

  void wal_close() {
    if (_wal.is_open()) {
      std::lock_guard<SpinLock> lock(_flush_lock);
      _wal.close();
    }
  }

  Derived& _derived() { return *static_cast<Derived*>(this); }
  const Derived& _derived() const { return *static_cast<const Derived*>(this); }
};

// Make _DB accept Transaction and Header as template parameters
// Self_ enables CRTP: derived classes pass themselves so _DB can
// statically dispatch to overrides (e.g. _on_txn_freed) without virtual.
template <typename Storage_,
          typename Transaction_ = _Transaction<typename Storage_::Traits>,
          typename Header_ = _DBHeader<Storage_>, typename Self_ = void>
struct _DB : public _WalDbMixin<_DB<Storage_, Transaction_, Header_, Self_>> {
  typedef Storage_ Storage;
  typedef Transaction_ Transaction;
  typedef Header_ Header;
  using Traits = typename Storage::Traits;
  using Mutex = typename Storage::Mutex;
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

  static_assert(sizeof(Header) + sizeof(Transaction) < AREA_SIZE,
                "DB Header too big");

  using header_ptr = typename Traits::template Pointer<Header>;

  Storage& _storage;
  txn_ptr _active_txn;
  header_ptr _header;
  std::string _name;

  [[no_unique_address]] Aspect _aspect{};

  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id{0};

  _DB(Storage& storage, offset_t header, std::string_view name)
      : _storage(storage),
        _header(storage.resolve(&header, READ)),
        _name(name) {
    if (_header->prepared_txn != _header->read_txn) {
      _active_txn = resolve<Transaction>(&_header->prepared_txn);
    }
  }

  _DB(Storage& storage, offset_t* header, std::string_view name)
      : _storage(storage), _name(name) {
    init(header);
  }

  ~_DB() { this->wal_close(); }

  Self& self() { return *static_cast<Self*>(this); }

  void init(offset_t* header) {
    auto area_ptr = _storage.alloc_single_area();
    *header = area_ptr->content_offset();  // Use content_offset, not get_offset
    _header = _storage.resolve(header, READ);
    memset((char*)_header, 0, sizeof(Header));
    _header->db_type_id = Self::DB_TYPE_ID;
    new (&_header->txn_lock) SpinLock();
    new (&_header->txn_ref_lock) SpinLock();

    // Initialize area lists with the first allocated area (area_ptr)
    offset_t first_area_offset = _storage.resolve(area_ptr);
    _header->area_list_head_single = first_area_offset;
    _header->area_list_head_multi = 0;

    uint32_t header_size = padding(sizeof(Header), MIN_PAGE_SIZE);
    _header->prepared_txn = _header->read_txn = *header + header_size;
    txn_ptr txn = resolve<Transaction>(&_header->read_txn);
    memset((char*)txn, 0, sizeof(Transaction));
    txn->slot_id = Transaction::SLOT_ID;
    txn->used = sizeof(Transaction);
    txn->txn_id = tid_t(1);
    txn->root = txn->offset_root = txn->free_bigmem_root = 0;
    txn->next_txn = 0;
    txn->refs.store(0);
    txn->start_txn = _header->read_txn;
    txn->area_list_tail_single =
        first_area_offset;  // First area is also the tail
    txn->area_list_tail_multi = 0;
    txn->mem_manager.init(_header->read_txn + PAGE_SIZES[txn->slot_id],
                          area_ptr->end());

    // Pre-allocate the next transaction page so start_transaction()
    // can skip the double-copy on the very first call.
    _active_txn = txn;
    auto next = txn->clone(*this);
    _header->next_txn_page = resolve(next);
    _active_txn.reset();

    make_dirty(_header);
    flush();
  }

  // Return all areas to storage pool
  void return_areas() {
    auto read_txn = resolve<Transaction>(&_header->read_txn);

    if (_header->area_list_head_single && read_txn->area_list_tail_single) {
      _storage.return_single_areas(_header->area_list_head_single,
                                   read_txn->area_list_tail_single);
    }
    if (_header->area_list_head_multi && read_txn->area_list_tail_multi) {
      _storage.return_multi_areas(_header->area_list_head_multi,
                                  read_txn->area_list_tail_multi);
    }
  }

  // Reset the DB by returning all areas to storage and reinitializing
  void reset(offset_t* header) {
    if (is_active()) throw TransactionActive();
    if (!_aspect.before_reset(self())) return;
    std::scoped_lock lock(_storage.file_lock());
    return_areas();
    init(header);
    _aspect.on_reset(self());
  }

  // Reset the DB in place: keep the existing header and the first single
  // area (which contains the header), return all other single areas plus
  // the entire multi chain to the storage pool, then re-initialize the
  // in-place transaction. After this call the DB is in the same logical
  // state as a freshly init()'d DB sharing the same header offset.
  // Precondition: no active transaction.
  // Derived-class header fields outside _DBHeader are preserved (no full
  // memset).
  void reset_in_place() {
    if (is_active()) throw TransactionActive();

    area_ptr first_area;
    txn_ptr txn;
    {
      std::scoped_lock lock(_storage.file_lock());

      auto read_txn = resolve<Transaction>(&_header->read_txn);

      // Return all single areas after the first (the first holds the header).
      if (_header->area_list_head_single) {
        area_ptr first = resolve<Area>(&_header->area_list_head_single, WRITE);
        offset_t second = first->next;
        if (second && read_txn->area_list_tail_single &&
            second != _header->area_list_head_single) {
          _storage.return_single_areas(second, read_txn->area_list_tail_single);
          first->next = 0;
          make_dirty(first);
        }
      }

      // Return the entire multi-area chain.
      if (_header->area_list_head_multi && read_txn->area_list_tail_multi) {
        _storage.return_multi_areas(_header->area_list_head_multi,
                                    read_txn->area_list_tail_multi);
        _header->area_list_head_multi = 0;
      }

      // Re-initialise the in-place transaction at header + header_size.
      offset_t header_off = (uint64_t)_storage.resolve(_header);
      uint32_t header_size = padding(sizeof(Header), MIN_PAGE_SIZE);
      _header->prepared_txn = _header->read_txn = header_off + header_size;
      _header->next_txn_page = 0;
      _header->txn_cursor_id.store(0);

      first_area = resolve<Area>(&_header->area_list_head_single, WRITE);
      offset_t first_area_offset = _header->area_list_head_single;

      txn = resolve<Transaction>(&_header->read_txn);
      memset((char*)txn, 0, sizeof(Transaction));
      txn->slot_id = Transaction::SLOT_ID;
      txn->used = sizeof(Transaction);
      txn->txn_id = tid_t(1);
      txn->root = txn->offset_root = txn->free_bigmem_root = 0;
      txn->next_txn = 0;
      txn->refs.store(0);
      txn->start_txn = _header->read_txn;
      txn->area_list_tail_single = first_area_offset;
      txn->area_list_tail_multi = 0;
      txn->mem_manager.init(_header->read_txn + PAGE_SIZES[txn->slot_id],
                            first_area->end());
    }
    // Release file_lock before clone(): clone() calls alloc_single_area()
    // which takes file_lock, and interprocess_mutex is non-reentrant.

    // Pre-allocate the next transaction page (mirrors init()).
    _active_txn = txn;
    auto next = txn->clone(*this);
    _header->next_txn_page = resolve(next);
    _active_txn.reset();

    make_dirty(first_area);
    make_dirty(txn);
    make_dirty(_header);
    flush();
  }

  Aspect& aspect() { return _aspect; }
  const Aspect& aspect() const { return _aspect; }

  cursor_ptr create_cursor() {
    auto cursor = std::make_shared<Cursor>(this, &txn()->root);
    _aspect.init_cursor_context(cursor->_aspect_context);
    return cursor;
  }

  const db_type* _internal() const { return this; }  // for _Dumper

  uint64_t new_cursor_id() { return _storage.new_cursor_id(); }

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

  template <typename T>
  void mark_for_recycle(T& garbage_block) const {
    assert(bool(_active_txn));
    garbage_block.txn_id = _active_txn->txn_id;
  }

  template <typename PtrType>
  void make_dirty(PtrType block) {
    _storage.make_dirty(block);
  }

  void flush(bool sync = false, bool force = false) {
    _storage.flush(sync, force);
  }

  // Allocate a page for content of 'space' bytes (PageHeader added internally).
  // Returns page_ptr pointing at the PageHeader.
  page_ptr alloc_page(uint16_t space) {
    using PageHeader = typename Traits::PageHeader;
    assert(space + sizeof(PageHeader) <= PAGE_SIZES[PAGE_SIZES_COUNT - 1]);
    // Record the raw requested size (incl. PageHeader) for PAGE_SIZES tuning.
    // No-op unless LEAVES_PAGE_HIST is defined at compile time.
    _page_hist_record((uint32_t)space + (uint32_t)sizeof(PageHeader));
    page_ptr result =
        alloc_slot(MemManager::assign_slot(space + sizeof(PageHeader)));
    result->used = space;
    return result;
  }

  // Allocate a node of 'node_size' bytes, returning a pointer past PageHeader.
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc_page(node_size);
    return page + sizeof(PageHeader);
  }

  page_ptr alloc_slot(uint16_t slot) {
    assert(transaction_active());
    assert(bool(_active_txn));
    return _active_txn->alloc_slot(slot, self());
  }

  void free(page_ptr page) {
    assert(transaction_active());
    assert(bool(_active_txn));
    _active_txn->mem_manager.free(page, *this);
  }

  void prefetch(const offset_e* offset, Access access = READ) const {
    _storage.prefetch(offset, access);
  }

  area_ptr alloc_single_area() {
    assert(bool(_active_txn));
    std::scoped_lock lock(_storage.file_lock());

    auto area_ptr = _storage.alloc_single_area();
    // Append to transaction's area list tail
    auto tail = resolve<Area>(&_active_txn->area_list_tail_single, WRITE);
    tail->next = resolve(area_ptr);
    make_dirty(tail);
    _active_txn->area_list_tail_single = resolve(area_ptr);

    make_dirty(area_ptr);
    return area_ptr;
  }

  area_ptr alloc_multi_area(uint64_t size) {
    assert(bool(_active_txn));
    std::scoped_lock lock(_storage.file_lock());

    auto area_ptr = _storage.alloc_multi_area(size);
    // Append to transaction's area list tail
    if (_active_txn->area_list_tail_multi) {
      auto tail = resolve<Area>(&_active_txn->area_list_tail_multi, WRITE);
      tail->next = resolve(area_ptr);
      make_dirty(tail);
    } else {
      // First area in this transaction - update head
      _header->area_list_head_multi = resolve(area_ptr);
      make_dirty(_header);
    }
    _active_txn->area_list_tail_multi = resolve(area_ptr);

    make_dirty(area_ptr);
    return area_ptr;
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

  tid_t transaction_active() const {
    return _active_txn ? _active_txn->txn_id : tid_t(0);
  }

  uint64_t txn_cursor_id() const { return _header->txn_cursor_id.load(); }

  bool is_active() const {
    if (_active_txn) return true;
    bool is_active_ = false;
    iter_transactions([&is_active_](txn_ptr txn) -> bool {
      if (txn->refs.load() > 0) is_active_ = true;
      return is_active_;
    });

    return is_active_;
  }

  // Start a write transaction. If nonblocking is true, this will return false
  // immediately when the txn_lock cannot be acquired.
  // May only be called by cursor
  txn_ptr start_transaction(
      uint64_t cursor_id, bool nonblocking = false,
      TransactionOrigin /*origin*/ = TransactionOrigin::user) {
    if (!nonblocking)
      _header->txn_lock.lock();
    else if (!_header->txn_lock.try_lock())
      return txn_ptr();

    assert(!_active_txn);
    assert(_header->txn_cursor_id.load() == 0);
    _header->txn_cursor_id.store(cursor_id);

    txn_ptr last_txn = txn();

    // Pre-allocated page is always ready (from commit, rollback, or init)
    assert(_header->next_txn_page);
    _active_txn = resolve<Transaction>(&_header->next_txn_page);
    _header->next_txn_page = 0;

    _active_txn->txn_id = last_txn->txn_id + tid_t(1);
    _active_txn->next_txn = 0;

    // Pin last_txn so it isn't freed during the GC walk.
    last_txn->refs.fetch_add(1);

    // Find the oldest used transaction and free unused old transactions.
    // Hold txn_ref_lock to prevent a concurrent txn_ref() from resolving
    // a txn between the refs==0 check and free().
    _start_txn_id = last_txn->txn_id;
    {
      std::lock_guard<SpinLock> guard(_header->txn_ref_lock);
      iter_transactions([this](txn_ptr txn) -> bool {
        if (txn->refs.load() > 0) {
          _active_txn->start_txn = resolve(txn);
          _start_txn_id = txn->txn_id;
          return true;
        }
        static_cast<Self*>(this)->_on_txn_freed(txn);
        free(txn);
        return false;
      });
    }

    last_txn->refs.fetch_sub(1);
    return _active_txn;
  }

  bool rollback(uint64_t cursor_id,
                TransactionOrigin /*origin*/ = TransactionOrigin::user) {
    if (_header->txn_cursor_id.load() != cursor_id) return false;

    // Return areas allocated during write transaction
    txn_ptr read_txn = resolve<Transaction>(&_header->read_txn);
    return_areas_range(
        read_txn->area_list_tail_single, _active_txn->area_list_tail_single,
        read_txn->area_list_tail_multi, _active_txn->area_list_tail_multi);

    _header->prepared_txn = _header->read_txn;

    // Reuse _active_txn's page for next pre-allocated transaction.
    // _active_txn was allocated from read_txn's committed space, so it
    // remains valid after rollback. Just overwrite with read_txn state.
    memcpy(reinterpret_cast<char*>(&*_active_txn),
           reinterpret_cast<const char*>(&*read_txn), sizeof(Transaction));
    _active_txn->mem_manager.reinit_locks();
    new (&_active_txn->refs) std::atomic<uint32_t>(0);
    _header->next_txn_page = resolve(_active_txn);

    make_dirty(_active_txn);
    make_dirty(_header);
    flush();
    end_transaction();
    return true;
  }

  tid_t prepare_commit(uint64_t cursor_id, bool sync = false,
                       TransactionOrigin /*origin*/ = TransactionOrigin::user) {
    // Not my transaction or not started
    if (_header->txn_cursor_id.load() != cursor_id) return tid_t(0);

    // already prepared
    if (_header->prepared_txn != _header->read_txn) return _active_txn->txn_id;

    // Pre-allocate next transaction page before committing.
    // The allocation modifies _active_txn->mem_manager, which gets persisted
    // as part of this commit — no storage leak.
    auto next = _active_txn->clone(*this);
    _header->next_txn_page = resolve(next);

    _header->prepared_txn = resolve(_active_txn);

    txn_ptr active = resolve<Transaction>(&_header->read_txn);
    active->next_txn = _header->prepared_txn;
    make_dirty(_header);
    make_dirty(active);
    make_dirty(_active_txn);

    if (sync) flush(true, true);  // Only flush if explicitly requested
    return _active_txn->txn_id;
  }

  bool commit(uint64_t cursor_id, bool sync = false,
              TransactionOrigin origin = TransactionOrigin::user) {
    try {
      if (!prepare_commit(cursor_id, false, origin)) return false;
    } catch (...) {
      rollback(cursor_id, origin);
      throw;
    }

    // Atomically switch to new transaction (area tails are preserved in
    // transaction)
    _header->read_txn = _header->prepared_txn;
    make_dirty(_header);
    flush(sync, true);
    end_transaction();
    return true;
  }

  void end_transaction() {
    _header->txn_cursor_id.store(0);
    _active_txn.reset();
    _header->txn_lock.unlock();
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

  // Defragment big memory - merge adjacent free chunks
  void defrag() {
    if (!_aspect.before_defrag(self())) return;

    uint64_t defrag_cursor_id = new_cursor_id();
    auto txn =
        start_transaction(defrag_cursor_id, false, TransactionOrigin::defrag);
    assert(txn);

    if (!txn->free_bigmem_root) {
      rollback(defrag_cursor_id, TransactionOrigin::defrag);
      return;  // No big memory allocated yet
    }

    // Use the non-transactional cursor type for the free-bigmem trie.
    // _TransactionalCursor rewires its root to txn->root in update(), which
    // would ignore &txn->free_bigmem_root and prevent defrag from working.
    using RawCursor = _Cursor<CursorTraits>;
    using BigMemory = _BigMemory<RawCursor>;
    BigMemory big_mem(this, &txn->free_bigmem_root);
    big_mem.defrag(txn);
    flush();
    commit(defrag_cursor_id, false, TransactionOrigin::defrag);
    _aspect.on_defrag(self());
  }

  void return_areas_range(offset_t start_single, offset_t end_single,
                          offset_t start_multi, offset_t end_multi) {
    std::scoped_lock lock(_storage.file_lock());
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
        _storage.return_multi_areas(_header->area_list_head_multi, end_multi);
        _header->area_list_head_multi = 0;
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
    new (&_header->txn_lock) SpinLock();
    new (&_header->txn_ref_lock) SpinLock();
    _header->txn_cursor_id.store(0);
    iter_transactions([this](txn_ptr txn) -> bool {
      txn->refs.store(0);
      txn->mem_manager.reinit_locks();
      make_dirty(txn);
      return false;
    });

    // Reinit locks/refs on pre-allocated transaction page (stale after crash).
    // If next_txn_page is 0 (crash between start_transaction clearing it and
    // prepare_commit allocating a replacement), re-create it from read_txn.
    if (_header->next_txn_page) {
      auto next = resolve<Transaction>(&_header->next_txn_page);
      next->refs.store(0);
      next->mem_manager.reinit_locks();
      make_dirty(next);
    } else {
      txn_ptr read_txn = resolve<Transaction>(&_header->read_txn);
      _active_txn = read_txn;
      auto next = read_txn->clone(*this);
      _header->next_txn_page = resolve(next);
      _active_txn.reset();
    }

    this->wal_recover();

    // If wal_recover left a dangling (prepared but not committed) transaction,
    // restore it as the active write transaction so the caller can commit or
    // rollback.  This happens when the last WAL transaction was prepared but
    // never committed before a crash.
    if (_header->prepared_txn != _header->read_txn) {
      _header->txn_lock.lock();
      _active_txn = resolve<Transaction>(&_header->prepared_txn);
    }

    make_dirty(_header);
    flush();
    _aspect.on_sanitize(self());
  }
};

}  // namespace leaves

#endif  // _LEAVES__DB_HPP