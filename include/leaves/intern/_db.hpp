#ifndef _LEAVES__DB_HPP
#define _LEAVES__DB_HPP

#include "_cursor.hpp"
#include "_memory.hpp"

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

  // position of the last allocated area in the area register
  offset_t olast_area;
  uint16_t ilast_area;

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
  using tid_e = typename Traits::tid_e;

  static constexpr auto PAGE_SIZE = Traits::PAGE_SIZE;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static constexpr uint16_t BLOCK_SIZES_COUNT = Traits::BLOCK_SIZES_COUNT;
  static constexpr uint16_t MIN_BLOCK_SIZE = BLOCK_SIZES[0];
  static constexpr uint16_t MAX_BLOCK_SIZE = BLOCK_SIZES[BLOCK_SIZES_COUNT - 1];

  typedef _DB<Storage> DB;
  typedef _Cursor<DB> Cursor;
  typedef _MemManager<Traits> MemManager;

  static_assert(
      sizeof(_Transaction<Traits>) == sizeof(_TransactionBase<Traits>),
      "Size of _Transaction must be equal to size of _TransactionBase");

  struct Header {
    offset_t read_txn;
    offset_t prepared_txn;
    Mutex txn_lock;
    AreaManager areas;
  };
  static_assert(sizeof(Header) + sizeof(Transaction) < PAGE_SIZE,
                "DB Header too big");

  using header_ptr = typename Traits::Pointer<Header>;

  Storage& _storage;
  Transaction _wtxn;
  header_ptr _header;

  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id;

  _DB(Storage& storage, offset_t header)
      : _storage(storage), _header(storage.resolve(header)) {}

  _DB(Storage& storage, offset_t* header) : _storage(storage) { init(header); }

  void init(offset_t* header) {
    auto area = _storage.get_area(Traits::AREA_SIZE);

    *header = area.offset + sizeof(AreaRegister);
    _header = _storage.resolve(*header);
    memset((void*)_header, 0, sizeof(Header));
    new (&_header->txn_lock) Mutex;
    _header->areas.put(area, *this);

    assert(*header % MAX_BLOCK_SIZE == 0);
    uint16_t header_size = padding(sizeof(Header), MIN_BLOCK_SIZE);
    _header->prepared_txn = _header->read_txn = *header + header_size;
    txn_ptr txn = resolve(_header->read_txn);
    memset((void*)txn, 0, sizeof(Transaction));
    txn->slot_id = Transaction::SLOT_ID;
    txn->olast_area = area.offset;
    txn->ilast_area = 0;
    txn->txn_id = 1;
    txn->slot_id = Transaction::SLOT_ID;
    txn->root = txn->mem_root = 0;
    txn->next_txn = 0;
    txn->count = 0;
    txn->start_txn = _header->read_txn;
    txn->mem_manager.init(_header->read_txn + BLOCK_SIZES[txn->slot_id],
                          area.end(), *this);
    _wtxn.txn_id = 0;
    _storage.flush();
  }

  template <typename T>
  typename Traits::Pointer<T> resolve(offset_t offset) const {
    return _storage.resolve(offset);
  }

  template <typename T>
  bool may_recycle(T& garbage_block) const {
    return garbage_block.txn_id < _start_txn_id;
  }

  template <typename T>
  void mark_for_recycle(T& garbage_block) const {
    garbage_block.txn_id = _wtxn.txn_id;
  }

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

  Cursor cursor() { return Cursor(*this); }

  block_ptr alloc(uint16_t space) {
    assert(space <= BLOCK_SIZES[BLOCK_SIZES_COUNT - 1]);
    return alloc_slot(MemManager::assign_slot(space));
  }

  block_ptr alloc_slot(uint16_t slot) {
    assert(_wtxn.txn_id);
    return _wtxn.alloc_slot(slot, *this);
  }

  void free(block_ptr& block) {
    assert(_wtxn.txn_id);
    _wtxn.mem_manager.free(block, *this);
  }

  void prefetch(offset_t offset) const { _storage.prefetch(offset); }

  AreaSlice alloc_area(uint64_t min_size) {
#if 0
    cursor = find_bigvalue(big_endina(min_size));
    cursor.next()
    if (big_to_native(cursor.key()) > min_size)

#endif
    assert(_wtxn.txn_id);
    auto result = get_next_area();
    if (!result) {
      assert(_wtxn.olast_area == _header->areas.end);
      std::scoped_lock lock(_storage._memory->file_lock);
      auto area = _storage.get_area(min_size);
      _header->areas.put(area, _storage);
      _storage.flush();
      result = get_next_area();
    }
    assert(result);
    return result;
  }

  AreaSlice get_next_area() {
    typedef typename Traits::template Pointer<AreaRegister> ptr;
    ptr ar = resolve(_wtxn.olast_area);
    if (ar->last_index > _wtxn.ilast_area) return ar->areas[++_wtxn.ilast_area];

    if (ar->next) {
      _wtxn.olast_area = ar->next;
      _wtxn.ilast_area = 0;
      ar = resolve(_wtxn.olast_area);
      return ar->areas[0];
    }

    return AreaSlice{0, 0};
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

  block_ptr resolve(offset_t offset) const { return _storage.resolve(offset); }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return _storage.resolve(p);
  }

  txn_ptr txn() const { return resolve(_header->read_txn); }

  bool start_transaction(bool wait = false) {
    if (_wtxn.txn_id) return false;

    if (!wait)
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
    for(int i = 0; i < MemManager::COUNT; i++) {
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
    return true;
  }

  void rollback() {
    _header->prepared_txn = _header->read_txn;
    _storage.flush();
    end_transaction();
  }

  void prepare_commit() {
    if (!_wtxn.txn_id) return;

    txn_ptr prepared = _wtxn.clone(*this);
    _header->prepared_txn = resolve(prepared);

    assert(_wtxn.start_txn);
    txn_ptr active = resolve(_header->read_txn);
    active->next_txn = _header->prepared_txn;

    _storage.flush();
  }

  void commit() {
    prepare_commit();
    _header->read_txn = _header->prepared_txn;
    _storage.flush();
    end_transaction();
  }

  void end_transaction() {
    _wtxn.txn_id = 0;
    _header->txn_lock.unlock();
  }

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
};

}  // namespace leaves

#endif  // _LEAVES__DB_HPP