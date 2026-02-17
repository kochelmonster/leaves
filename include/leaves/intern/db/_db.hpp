#ifndef _LEAVES__DB_HPP
#define _LEAVES__DB_HPP

#include <boost/endian/arithmetic.hpp>
#include <memory>
#include <mutex>

#include "../core/_hash.hpp"
#include "../core/_port.hpp"
#include "../memory/_memory.hpp"
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

  // pointer ot the next higher transaction
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
  using ptr = typename Traits::Pointer<_Transaction>;
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
    new_txn->used = sizeof(TransactionBase);
    memcpy((char*)new_txn, this, sizeof(TransactionBase));
    new (&new_txn->refs) std::atomic<uint32_t>(this->refs.load(std::memory_order_relaxed));
    assert(new_txn->slot_id == SLOT_ID);
    return new_txn;
  }
};

// Default database header (defined outside _DB for reusability)
template <typename Storage_>
struct _DBHeader {
  using Mutex = typename Storage_::Mutex;

  offset_t read_txn;      // the current read transaction
  offset_t prepared_txn;  // the transaction being prepared for commit
  Mutex txn_lock;
  std::atomic<uint64_t>
      txn_cursor_id;  // the id of the cursor holding the transaction

  // Atomic area management - no AreaList objects, just head pointers
  // Areas are linked lists in storage, operations use atomic head/tail pattern
  offset_t area_list_head_single;  // head of single AREA_SIZE areas linked list
  offset_t area_list_head_multi;   // head of multi-AREA_SIZE areas linked list
  AreaPool area_pool;              // area pool for allocating areas
};

// Make _DB accept Transaction and Header as template parameters
template <typename Storage_,
          typename Transaction_ = _Transaction<typename Storage_::Traits>,
          typename Header_ = _DBHeader<Storage_>>
struct _DB {
  typedef Storage_ Storage;
  typedef Transaction_ Transaction;
  typedef Header_ Header;
  using Traits = typename Storage::Traits;
  using Mutex = typename Storage::Mutex;
  using area_ptr = typename Storage::area_ptr;
  using txn_ptr = typename Transaction::ptr;
  using page_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;

  typedef _DB<Storage_, Transaction_, Header_> DB;

  struct CursorTraits : public Storage::Traits {
    typedef _DB<Storage_, Transaction_, Header_> DB;
    using tid_t = leaves::tid_t;
  };

  // Detect ReplicationHasher from Traits, fallback to NullHasher
  template <typename T, typename = void>
  struct get_replication_hasher { using type = NullHasher; };
  
  template <typename T>
  struct get_replication_hasher<T, std::void_t<typename T::ReplicationHasher>> {
    using type = typename T::ReplicationHasher;
  };
  
  using ReplicationHasher = typename get_replication_hasher<Traits>::type;

  // Detect Aspect from Traits, fallback to DefaultAspect
  template <typename T, typename = void>
  struct get_aspect { using type = DefaultAspect; };
  
  template <typename T>
  struct get_aspect<T, std::void_t<typename T::Aspect>> {
    using type = typename T::Aspect;
  };
  
  using Aspect = typename get_aspect<Traits>::type;

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

  static_assert(
      sizeof(Transaction) >= sizeof(_TransactionBase<Traits>),
      "Size of Transaction must be at least size of _TransactionBase");

  static_assert(sizeof(Header) + sizeof(Transaction) < AREA_SIZE,
                "DB Header too big");

  using header_ptr = typename Traits::template Pointer<Header>;

  Storage& _storage;
  Transaction* _active_txn = nullptr;
  txn_ptr _wtxn;
  header_ptr _header;
  uint16_t _index;

  [[no_unique_address]] Aspect _aspect{};

  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id;

  _DB(Storage& storage, offset_t header, uint16_t index)
      : _storage(storage),
        _header(storage.resolve(&header, READ)),
        _index(index) {
    if (_header->prepared_txn != _header->read_txn) {
      // Recover a prepared transaction - set both _active_txn and _wtxn
      _wtxn = resolve<Transaction>(&_header->prepared_txn);
      _active_txn = &*_wtxn;
    }
  }

  _DB(Storage& storage, offset_t* header, uint16_t index)
      : _storage(storage), _index(index) {
    init(header);
  }

  void init(offset_t* header) {
    auto area_ptr = _storage.alloc_single_area();

    *header = area_ptr->content_offset();  // Use content_offset, not get_offset
    _header = _storage.resolve(header, READ);
    memset((char*)_header, 0, sizeof(Header));
    new (&_header->txn_lock) Mutex();

    // Initialize area lists with the first allocated area (area_ptr)
    offset_t first_area_offset = _storage.resolve(area_ptr);
    _header->area_list_head_single = first_area_offset;
    _header->area_list_head_multi = 0;
    _header->area_pool.init();

    uint16_t header_size = padding(sizeof(Header), MIN_PAGE_SIZE);
    _header->prepared_txn = _header->read_txn = *header + header_size;
    txn_ptr txn = resolve<Transaction>(&_header->read_txn);
    memset((char*)txn, 0, sizeof(Transaction));
    txn->slot_id = Transaction::SLOT_ID;
    txn->txn_id = tid_t(1);
    txn->slot_id = Transaction::SLOT_ID;
    txn->root = txn->offset_root = txn->free_bigmem_root = 0;
    txn->next_txn = 0;
    txn->refs.store(0);
    txn->start_txn = _header->read_txn;
    txn->area_list_tail_single =
        first_area_offset;  // First area is also the tail
    txn->area_list_tail_multi = 0;
    txn->mem_manager.init(_header->read_txn + PAGE_SIZES[txn->slot_id],
                          area_ptr->end());
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
    return_areas();
    init(header);
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

  Slice name() const { return _storage.db_name(_index); }

  template <typename T>
  typename Traits::Pointer<T> resolve(const offset_e* offset_ptr,
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
    assert(_active_txn);
    garbage_block.txn_id = _active_txn->txn_id;
  }

  template <typename PtrType>
  void make_dirty(PtrType block) {
    _storage.make_dirty(block);
  }

  void flush(bool sync = false, bool force = false) {
    _storage.flush(sync, force);
  }

  // space without PageHeader
  page_ptr alloc(uint16_t space) {
    assert(space + sizeof(typename Traits::PageHeader) <=
           PAGE_SIZES[PAGE_SIZES_COUNT - 1]);
    page_ptr result = alloc_slot(
        MemManager::assign_slot(space + sizeof(typename Traits::PageHeader)));
    result->used = space;
    return result;
  }

  page_ptr alloc_slot(uint16_t slot) {
    assert(transaction_active());
    assert(_active_txn);
    return _active_txn->alloc_slot(slot, *this);
  }

  void free(page_ptr page) {
    assert(transaction_active());
    assert(_active_txn);
    _active_txn->mem_manager.free(page, *this);
  }

  void prefetch(const offset_e* offset, Access access = READ) const {
    _storage.prefetch(offset, access);
  }

  area_ptr alloc_single_area() {
    assert(_active_txn);
    std::scoped_lock lock(_storage.file_lock());

    auto area_ptr = _storage.alloc_single_area();
    area_ptr->next = 0;

    // Append to transaction's area list tail
    if (_active_txn->area_list_tail_single) {
      auto tail = resolve<Area>(&_active_txn->area_list_tail_single, READ);
      tail->next = resolve(area_ptr);
      make_dirty(tail);
    } else {
      // First area in this transaction - update head
      _header->area_list_head_single = resolve(area_ptr);
      make_dirty(_header);
    }
    _active_txn->area_list_tail_single = resolve(area_ptr);

    make_dirty(area_ptr);
    flush();
    return area_ptr;  // Convert Area* to AreaSlice for return
  }

  area_ptr alloc_multi_area(uint64_t size) {
    assert(_active_txn);
    std::scoped_lock lock(_storage.file_lock());

    auto area_ptr = _storage.alloc_multi_area(size);
    area_ptr->next = 0;

    // Append to transaction's area list tail
    if (_active_txn->area_list_tail_multi) {
      auto tail = resolve<Area>(&_active_txn->area_list_tail_multi, READ);
      tail->next = resolve(area_ptr);
      make_dirty(tail);
    } else {
      // First area in this transaction - update head
      _header->area_list_head_multi = resolve(area_ptr);
      make_dirty(_header);
    }
    _active_txn->area_list_tail_multi = resolve(area_ptr);

    make_dirty(area_ptr);
    flush();
    return area_ptr;  // Convert Area* to AreaSlice for return
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

  tid_t transaction_active() const {
    return _active_txn ? _active_txn->txn_id : tid_t(0);
  }

  uint64_t txn_cursor_id() const { return _header->txn_cursor_id.load(); }

  bool is_active() const {
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
  txn_ptr start_transaction(uint64_t cursor_id, bool nonblocking = false) {
    if (!nonblocking)
      _header->txn_lock.lock();
    else if (!_header->txn_lock.try_lock())
      return txn_ptr();

    assert(!_active_txn);
    assert(_header->txn_cursor_id.load() == 0);
    _header->txn_cursor_id.store(cursor_id);

    txn_ptr last_txn = txn();

    Transaction tmp;  // needed to alloc the next transaction itself
    memcpy((void*)&tmp, &*last_txn, sizeof(Transaction));
    new (&tmp.refs) std::atomic<uint32_t>(last_txn->refs.load(std::memory_order_relaxed));
    _active_txn = &tmp;
    _wtxn = tmp.clone(*this);
    _active_txn = &*_wtxn;
    _active_txn->refs.store(0);

    // Initialize area list tails from the last transaction
    _active_txn->area_list_tail_single = last_txn->area_list_tail_single;
    _active_txn->area_list_tail_multi = last_txn->area_list_tail_multi;

    // ensure last_txn is not freed
    last_txn->refs.fetch_add(1);

    _active_txn->txn_id = last_txn->txn_id + tid_t(1);
    _active_txn->next_txn = 0;
    _start_txn_id = last_txn->txn_id;

    _storage.prefetch(&_active_txn->mem_manager);
    for (int i = 0; i < MemManager::COUNT; i++) {
      _storage.prefetch(&_active_txn->mem_manager.slots[i]);
    }

    // find the oldest used transaction
    iter_transactions([this](txn_ptr txn) -> bool {
      if (txn->refs.load() > 0) {
        _active_txn->start_txn = resolve(txn);
        _start_txn_id = txn->txn_id;
        return true;
      }
      free(txn);
      return false;
    });

    last_txn->refs.fetch_sub(1);
    return _wtxn;
  }

  bool rollback(uint64_t cursor_id) {
    if (_header->txn_cursor_id.load() != cursor_id) return false;

    // Return areas allocated during write transaction
    txn_ptr read_txn = resolve<Transaction>(&_header->read_txn);
    return_areas_range(
        read_txn->area_list_tail_single, _wtxn->area_list_tail_single,
        read_txn->area_list_tail_multi, _wtxn->area_list_tail_multi);

    _header->prepared_txn = _header->read_txn;
    flush();
    end_transaction();
    return true;
  }

  tid_t prepare_commit(uint64_t cursor_id, bool sync = false) {
    // Not my transaction or not started
    if (_header->txn_cursor_id.load() != cursor_id) return tid_t(0);

    // already prepared
    if (_header->prepared_txn != _header->read_txn) return _wtxn->txn_id;

    // Compute merkle hashes for all new nodes in this transaction
    compute_hashes(ReplicationHasher{}, this, _wtxn);

    _header->prepared_txn = resolve(_wtxn);

    txn_ptr active = resolve<Transaction>(&_header->read_txn);
    active->next_txn = _header->prepared_txn;
    make_dirty(_header);
    make_dirty(active);
    make_dirty(_wtxn);

    if (sync) flush(true, true);  // Only flush if explicitly requested
    return _wtxn->txn_id;
  }

  bool commit(uint64_t cursor_id, bool sync = false) {
    if (!prepare_commit(cursor_id, false)) return false;

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
    _wtxn.reset();
    _active_txn = nullptr;
    _header->txn_lock.unlock();
  }

  typedef _MemStatistics<Traits> MemStatistics;

  struct Statistics {
    MemStatistics garbage, branch, leaf, transaction;
  };

  void _garbage_statistics(MemStatistics& tofill) {
    txn_ptr txn_ = txn();
    const int garbage =
        MemManager::assign_slot(MemManager::PageContainer::SIZE);
    for (int i = 0; i < MemManager::COUNT; i++) {
      auto slot = txn_->mem_manager.slots[i];
      // collect blocks
      offset_t o = slot.ostart;
      size_t count = 0;
      while (true) {
        typename MemManager::Slot::cont_ptr gc = resolve(o);
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
    using trie_ptr = typename Traits::Pointer<TrieNode>;

    if (offset.type() == TRIE) {
      trie_ptr branch = resolve(offset);
      stat.branch.add(branch->slot_id, 1,
                      PAGE_SIZES[branch->slot_id] - branch->size());
      auto count = branch->count();
      offset_e* array = branch->array();
      for (int i = 0; i < count; i++) {
        _node_statistics(stat, array[i]);
      }
      return;
    }
  }

  void statistics(Statistics& stat) {
    _garbage_statistics(stat.garbage);
    _node_statistics(stat, txn()->root);

    iter_transactions([this, &stat](txn_ptr txn) -> bool {
      uint16_t bsize = PAGE_SIZES[txn->slot_id];
      stat.transaction.add(txn->slot_id, 1, bsize - sizeof(Transaction));
      offset_t offset = resolve(txn);
      return false;
    });
  }

  // Defragment big memory - merge adjacent free chunks
  void defrag() {
    uint64_t defrag_cursor_id = new_cursor_id();
    auto txn = start_transaction(defrag_cursor_id);
    assert(txn);

    if (!txn->free_bigmem_root) {
      rollback(defrag_cursor_id);
      return;  // No big memory allocated yet
    }

    // Use the non-transactional cursor type for the free-bigmem trie.
    // _TransactionalCursor rewires its root to txn->root in update(), which
    // would ignore &txn->free_bigmem_root and prevent defrag from working.
    using RawCursor = typename Cursor::Cursor;
    using BigMemory = _BigMemory<RawCursor>;
    BigMemory big_mem(this, &txn->free_bigmem_root);
    big_mem.defrag(txn);
    flush();
    commit(defrag_cursor_id);
  }

  void return_areas_range(offset_t start_single, offset_t end_single,
                          offset_t start_multi, offset_t end_multi) {
    // Return area range [start->next ... end] to storage
    // This is used during rollback to return areas allocated during write
    // transaction

    if (start_single && end_single && start_single != end_single) {
      area_ptr start_area = resolve<Area>(&start_single, READ);
      offset_t range_head = start_area->next;
      _storage.return_single_areas(range_head, end_single);
    }

    if (start_multi && end_multi && start_multi != end_multi) {
      area_ptr start_area = resolve<Area>(&start_multi, READ);
      offset_t range_head = start_area->next;
      _storage.return_multi_areas(range_head, end_multi);
    }
  }

  void sanitize() {
    new (&_header->txn_lock) Mutex();
    _header->txn_cursor_id.store(0);
    iter_transactions([](txn_ptr txn) -> bool {
      txn->refs.store(0);
      return false;
    });

    if (_header->prepared_txn == _header->read_txn) {
      // Return any uncommitted areas
      txn_ptr read_txn = resolve<Transaction>(&_header->read_txn);

      // Find actual tail by iterating single area list
      offset_t stail = Area::get_end(read_txn->area_list_tail_single, *this);
      offset_t mtail = Area::get_end(read_txn->area_list_tail_multi, *this);
      return_areas_range(read_txn->area_list_tail_single, stail,
                         read_txn->area_list_tail_multi, mtail);

      // Clear transaction state after rolling back
      _header->prepared_txn = _header->read_txn;
      _wtxn.reset();
      _active_txn = nullptr;
    }
    // otherwise we have to wait for rollback or commit

    flush();
  }
};

}  // namespace leaves

#endif  // _LEAVES__DB_HPP