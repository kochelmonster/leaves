#ifndef _LEAVES__DB_HPP
#define _LEAVES__DB_HPP

#include <boost/endian/arithmetic.hpp>

#include "_check.hpp"
#include "_cursor.hpp"
#include "_hash.hpp"
#include "_memory.hpp"

// #define DEBUG_MEM

namespace leaves {

template <typename Traits_>
struct _TransactionBase : public Traits_::BlockHeader {
  typedef Traits_ Traits;
  typedef _MemManager<Traits> MemManager;
  using Traits::BlockHeader::txn_id;

  // pointer to the active root of the trie
  offset_t root;

  // pointer to the active root of the mem trie
  offset_t mem_root;

  // pointer to the oldest transaction
  offset_t start_txn;

  // pointer ot the next higher transaction
  offset_t next_txn;

  // count of cursors accessing this transaction
  uint32_t count;

  MemManager mem_manager;
};

template <typename Traits_>
struct _Transaction : public _TransactionBase<Traits_> {
  typedef Traits_ Traits;
  typedef _TransactionBase<Traits_> TransactionBase;
  typedef _MemManager<Traits> MemManager;
  using ptr = typename Traits::Pointer<_Transaction>;
  using block_ptr = typename Traits::ptr;
  using TransactionBase::mem_manager;
  using TransactionBase::txn_id;

  static constexpr auto SLOT_ID =
      MemManager::assign_slot(sizeof(TransactionBase));
  uint16_t size() const { return sizeof(TransactionBase); }

  template <typename Resolver>
  block_ptr alloc_slot(uint16_t slot, Resolver& resolver) {
    block_ptr result = mem_manager.alloc(slot, resolver);
    result->txn_id = txn_id;
    resolver.make_dirty(result);
    return result;
  }

  template <typename Resolver>
  ptr clone(Resolver& resolver) {
    ptr new_txn = alloc_slot(SLOT_ID, resolver);
    copy(*new_txn, *this);
    return new_txn;
  }
};

template <typename Storage_>
struct _DB {
  typedef Storage_ Storage;
  using Traits = typename Storage::Traits;
  typedef _Transaction<Traits> Transaction;
  using Mutex = typename Storage::Mutex;
  using txn_ptr = typename Transaction::ptr;
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  typedef _DB<Storage> DB;

  struct ValueTraits : public Storage::Traits {
    typedef std::shared_ptr<DB> db_ptr;
    typedef ::Hasher Hasher;
    constexpr static bool transactional = true;
    static void set_root(DB& db, offset_t offset) { db._wtxn.root = offset; }
    static offset_t get_root(DB& db) { return db.txn()->root; }
  };

  struct MemoryTraits : public Storage::Traits {
    typedef DB* db_ptr;
    typedef ::NullHasher Hasher;
    typedef uint8_t hash_t[0];
    constexpr static bool transactional = false;
    static void set_root(DB& db, offset_t offset) {
      db._wtxn.mem_root = offset;
    }
    static offset_t get_root(DB& db) { return db.txn()->mem_root; }
  };

  struct BigSizeKey {
    boost::endian::big_uint64_t first;
    uint64_t second;
  };

  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static constexpr uint16_t BLOCK_SIZES_COUNT = Traits::BLOCK_SIZES_COUNT;
  static constexpr uint16_t MIN_BLOCK_SIZE = BLOCK_SIZES[0];
  static constexpr uint16_t MAX_BLOCK_SIZE = BLOCK_SIZES[BLOCK_SIZES_COUNT - 1];
  static constexpr uint64_t SIZE_BIT = uint64_t(1) << 63;

  typedef _Cursor<DB, ValueTraits> Cursor;
  typedef _Cursor<DB, MemoryTraits> MemCursor;
  typedef _MemManager<Traits> MemManager;
  typedef DB db_type;

  static_assert(
      sizeof(_Transaction<Traits>) == sizeof(_TransactionBase<Traits>),
      "Size of _Transaction must be equal to size of _TransactionBase");

  struct Header {
    offset_t read_txn;             // the current read transaction
    offset_t prepared_txn;         // the transaction being prepared for commit
    Mutex txn_lock;
    AreaList single_areas;         // single AREA_SIZE areas
    AreaList multi_areas;          // multi-AREA_SIZE areas
    AreaList pending_single_areas; // single areas pending commit/rollback
    AreaList pending_multi_areas;  // multi areas pending commit/rollback
  };
  static_assert(sizeof(Header) + sizeof(Transaction) < AREA_SIZE,
                "DB Header too big");

  using header_ptr = typename Traits::Pointer<Header>;

  Storage& _storage;
  Transaction _wtxn;
  header_ptr _header;
  MemCursor _mem_cursor;
  uint16_t _index;

  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id;

  _DB(Storage& storage, offset_t header, uint16_t index)
      : _storage(storage),
        _header(storage.resolve(header)),
        _mem_cursor(this),
        _index(index) {
    _wtxn.txn_id = 0;
  }

  _DB(Storage& storage, offset_t* header, uint16_t index)
      : _storage(storage), _mem_cursor(this), _index(index) {
    init(header);
  }

  void init(offset_t* header) {
    auto area_ptr = _storage.alloc_single_area();

    *header = area_ptr->content_offset();  // Use content_offset, not get_offset
    _header = _storage.resolve(*header);
    memset((void*)_header, 0, sizeof(Header));
    new (&_header->txn_lock) Mutex;
    _header->single_areas.init();
    _header->multi_areas.init();
    _header->pending_single_areas.init();
    _header->pending_multi_areas.init();

    uint16_t header_size = padding(sizeof(Header), MIN_BLOCK_SIZE);
    _header->prepared_txn = _header->read_txn = *header + header_size;
    txn_ptr txn = resolve(_header->read_txn);
    memset((void*)txn, 0, sizeof(Transaction));
    txn->slot_id = Transaction::SLOT_ID;
    txn->txn_id = 1;
    txn->slot_id = Transaction::SLOT_ID;
    txn->root = txn->mem_root = 0;
    txn->next_txn = 0;
    txn->count = 0;
    txn->start_txn = _header->read_txn;
    txn->mem_manager.init(_header->read_txn + BLOCK_SIZES[txn->slot_id],
                          area_ptr->end());
    _wtxn.txn_id = 0;
    flush();
  }

  Slice name() const { return _storage.db_name(_index); }

  template <typename T>
  typename Traits::Pointer<T> resolve(offset_t offset,
                                      Access access = READ) const {
    return _storage.resolve(offset, access);
  }

  block_ptr resolve(offset_t offset, Access access = READ) const {
    return _storage.resolve(offset, access);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return _storage.resolve(p);
  }

  template <typename T>
  bool may_recycle(T& garbage_block) const {
    return garbage_block.txn_id < _start_txn_id;
  }

  template <typename T>
  void mark_for_recycle(T& garbage_block) const {
    garbage_block.txn_id = _wtxn.txn_id;
  }

  void make_dirty(block_ptr& block) { _storage.make_dirty(block); }

  template <typename ptr>
  ptr clone(const ptr& src) {
    ptr dest = alloc_slot(src->slot_id);
    copy(*dest, *src);
    return dest;
  }

  template <typename ptr>
  ptr cow(ptr& src) {
    auto result = clone(src);
    free(src);
    return result;
  }

  block_ptr alloc(uint16_t space) {
    assert(space <= BLOCK_SIZES[BLOCK_SIZES_COUNT - 1]);
    return alloc_slot(MemManager::assign_slot(space));
  }

  block_ptr alloc_slot(uint16_t slot) {
    assert(transaction_active());
    return _wtxn.alloc_slot(slot, *this);
  }

  void free(block_ptr& block) {
    assert(transaction_active());
    _wtxn.mem_manager.free(block, *this);
  }

  void _add_to_bigmem(offset_t offset, size_t size) {
    assert(size % MAX_BLOCK_SIZE == 0);
#ifdef DEBUG_MEM
    std::stringstream cstr;
    cstr << offset._offset << "-" << size;
#endif

    BigSizeKey bkey;
    bkey.first = offset;
    bkey.second = size;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(!_mem_cursor.is_valid());

#ifdef DEBUG_MEM
    _mem_cursor.value(cstr.str());
#else
    _mem_cursor.value(Slice());
#endif

    bkey.first = size | SIZE_BIT;
    bkey.second = offset;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(!_mem_cursor.is_valid());
#ifdef DEBUG_MEM
    _mem_cursor.value(cstr.str());
#else
    _mem_cursor.value(Slice());
#endif
  }

  void _remove_from_bigmem(offset_t offset, size_t size) {
    BigSizeKey bkey;
    bkey.first = size | SIZE_BIT;
    bkey.second = offset;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(_mem_cursor.is_valid());
    _mem_cursor.remove();

    bkey.first = offset;
    bkey.second = size;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(_mem_cursor.is_valid());
    _mem_cursor.remove();
  }

  AreaSlice alloc_big(uint64_t size) {
    assert(_wtxn.txn_id);

    size = padding(size, MAX_BLOCK_SIZE);
    uint32_t found_size;
    offset_t found_offset;

    // find from big memory storage
    BigSizeKey bkey;
    bkey.first = size | SIZE_BIT;
    bkey.second = 0;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    _mem_cursor.next();

    if (_mem_cursor.is_valid()) {
      BigSizeKey* found = (BigSizeKey*)_mem_cursor.key().data();
      found_size = found->first & ~SIZE_BIT;
      found_offset = found->second;

      // remove the block from bit memory storage
      assert(found_size >= size);
      _mem_cursor.remove();

      // remove from offset list
      bkey.first = found_offset;
      bkey.second = found_size;
      _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
      assert(_mem_cursor.is_valid());
      _mem_cursor.remove();
    } else {
      // allocate new multi-area
      uint64_t psize = padding(size, AREA_SIZE);
      auto slice = alloc_multi_area(psize);
      found_size = slice.get_size();
      found_offset = slice.get_offset();
    }

    uint32_t delta = found_size - size;
    if (delta >= MAX_BLOCK_SIZE) {
      // enough space left -> reuse the rest
      _add_to_bigmem(found_offset + size, delta);
      found_size -= delta;
    }

    return AreaSlice{found_offset, found_size};
  }

  void free_big(offset_e offset, size_t size) {
    assert(_wtxn.txn_id);
    size = padding(size, MAX_BLOCK_SIZE);

    BigSizeKey bkey;
    bkey.first = offset;
    bkey.second = size;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(!_mem_cursor.is_valid());

    _mem_cursor.prev();
    if (_mem_cursor.is_valid()) {
      BigSizeKey* found = (BigSizeKey*)_mem_cursor.key().data();
      if (found->first + found->second == offset) {
        offset = found->first;
        size += found->second;
        _remove_from_bigmem((uint64_t)found->first, found->second);
      } else
        _mem_cursor.next();
    } else
      _mem_cursor.first();

    if (_mem_cursor.is_valid()) {
      BigSizeKey* found = (BigSizeKey*)_mem_cursor.key().data();
      uint64_t foffset = found->first;
      if (found->first == offset + size) {
        size += found->second;
        _remove_from_bigmem((uint64_t)found->first, found->second);
      }
    }

    _add_to_bigmem(offset, size);
  }

  void prefetch(offset_t offset) const { _storage.prefetch(offset); }

  AreaSlice alloc_single_area() {
    assert(_wtxn.txn_id);
    std::scoped_lock lock(_storage.file_lock());
    
    auto area_ptr = _storage.alloc_single_area();
    _header->pending_single_areas.push(*area_ptr, _storage);  // Convert Area* to AreaSlice for push
    flush();
    
    return *area_ptr;  // Convert Area* to AreaSlice for return
  }

  AreaSlice alloc_multi_area(uint64_t size) {
    assert(_wtxn.txn_id);
    std::scoped_lock lock(_storage.file_lock());
    
    auto area_ptr = _storage.alloc_multi_area(size);
    _header->pending_multi_areas.push(*area_ptr, _storage);  // Convert Area* to AreaSlice for push
    flush();
    
    return *area_ptr;  // Convert Area* to AreaSlice for return
  }

  template <typename T>
  void iter_transactions(T caller) {
    txn_ptr txn = _storage.resolve(_header->read_txn);
    tid_t end = txn->txn_id;
    offset_t* link = &txn->start_txn;
    do {
      txn = resolve(*link);
      if (caller(txn)) break;
      link = &txn->next_txn;
    } while (txn->txn_id < end);
  }

  void sanitize_transactions() {
    iter_transactions([](txn_ptr txn) -> bool {
      txn->count = 0;
      return false;
    });
  }

  txn_ptr txn() const { return resolve(_header->read_txn); }

  bool transaction_active() const { return _wtxn.txn_id != 0; }

  // Start a write transaction. If nonblocking is true, this will return false
  // immediately when the txn_lock cannot be acquired.
  bool start_transaction(bool nonblocking = false) {
    if (_wtxn.txn_id) return false;

    if (!nonblocking)
      _header->txn_lock.lock();
    else if (!_header->txn_lock.try_lock())
      return false;

    txn_ptr active = txn();
    memcpy(&_wtxn, &*active, sizeof(Transaction));
    active->count++;  // ensure the last read transaction is not freed

    _wtxn.txn_id = active->txn_id + 1;
    _wtxn.next_txn = 0;
    _start_txn_id = active->txn_id;

    _storage.prefetch(&_wtxn.mem_manager);
    for (int i = 0; i < MemManager::COUNT; i++) {
      _storage.prefetch(&_wtxn.mem_manager.slots[i]);
    }

    // find the oldest used transaction
    iter_transactions([this](txn_ptr txn) -> bool {
      if (txn->count) {
        _wtxn.start_txn = resolve(txn);
        _start_txn_id = txn->txn_id;
        return true;
      }
      free(txn);
      return false;
    });

    active->count--;
    _mem_cursor.root = _wtxn.mem_root;
    return true;
  }

  void rollback() {
    // Return pending areas to storage
    return_pending_areas();
    
    _header->prepared_txn = _header->read_txn;
    flush();
    end_transaction();
  }

  void prepare_commit(bool sync = false) {
    // No transaction active or already prepared
    if (!_wtxn.txn_id || _header->prepared_txn != _header->read_txn) return;
       
    txn_ptr prepared = _wtxn.clone(*this);
    _header->prepared_txn = resolve(prepared);

    assert(_wtxn.start_txn);
    txn_ptr active = resolve(_header->read_txn);
    active->next_txn = _header->prepared_txn;
    make_dirty(active);
    make_dirty(prepared);

    flush(!sync);
  }

  void commit(bool sync = false) {
    prepare_commit(sync);
    
    // Now atomically move pending areas to committed areas
    _header->single_areas.move(_header->pending_single_areas, _storage);
    _header->multi_areas.move(_header->pending_multi_areas, _storage);
    
    _header->read_txn = _header->prepared_txn;
    flush(!sync);
    end_transaction();
  }

  void end_transaction() {
    _wtxn.txn_id = 0;
    _header->txn_lock.unlock();
  }

  void flush(bool async = true) { _storage.flush(async); }

  typedef _MemStatistics<Traits> MemStatistics;

  struct Statistics {
    MemStatistics garbage, branch, leaf, transaction;
  };

  void _garbage_statistics(MemStatistics& tofill) {
    txn_ptr txn_ = txn();
    const int garbage =
        MemManager::assign_slot(MemManager::BlockContainer::SIZE);
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
    typedef _LeafNode<Traits> LeafNode;
    using trie_ptr = typename Traits::Pointer<TrieNode>;
    using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;

    if (offset.type() == TRIE) {
      trie_ptr branch = resolve(offset);
      stat.branch.add(branch->slot_id, 1,
                      BLOCK_SIZES[branch->slot_id] - branch->size());
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
      uint16_t bsize = BLOCK_SIZES[txn->slot_id];
      stat.transaction.add(txn->slot_id, 1, bsize - sizeof(Transaction));
      offset_t offset = resolve(txn);
      return false;
    });
  }

  void return_pending_areas() {
    // Return pending areas to storage
    _storage.return_single_areas(_header->pending_single_areas);
    _storage.return_multi_areas(_header->pending_multi_areas);
  }

  void sanitize() {
    // Return any pending areas from incomplete transactions back to storage
    if (_header->prepared_txn != _header->read_txn)
      commit();
    return_pending_areas();
    flush();
  }

  const db_type& dump_storage() const { return *this; }
};

}  // namespace leaves

#endif  // _LEAVES__DB_HPP