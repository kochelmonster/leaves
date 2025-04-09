#ifndef _LEAVES__MMAP_HPP
#define _LEAVES__MMAP_HPP

#include <algorithm>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/process/v2/pid.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "./_cursor.hpp"
#include "./_exception.hpp"
#include "./_memory.hpp"
#include "./_node.hpp"
#include "./_traits.hpp"

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::file_mapping;
using boost::interprocess::interprocess_mutex;
using boost::interprocess::mapped_region;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using boost::process::v2::all_pids;
using boost::process::v2::current_pid;
using boost::process::v2::pid_type;

namespace leaves {

static const char SIGNATURE[] = "larch-leaves";
static const size_t SIGNATURE_SIZE = padding(sizeof(SIGNATURE), 8);

// definition og all headers and data types
struct _MemoryMapTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef tid_t tid_e;
  typedef offset_t offset_e;

  /*
  Typical node sizes
  digits: 0-9:     104
  hex:    0-9A-F   160
  base64: 64       564
  utf-8:  127      1056
  binary: 256      2088
  max: 2264
  */

#pragma pack(1)
  struct BlockHeader {
    typedef BlockHeader Base;
    tid_e txn_id;
    uint8_t slot_id;
    uint8_t free_idx;
  };
#pragma pack(0)

  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr uint16_t BLOCK_SIZES[] = {
      _TrieNode<_MemoryMapTraits>::size(1, 10),   // digits 0-9
      _TrieNode<_MemoryMapTraits>::size(1, 16),   // hex 0-9A-F
      _TrieNode<_MemoryMapTraits>::size(1, 64),   // base64
      _TrieNode<_MemoryMapTraits>::size(1, 127),  // utf-8
      _TrieNode<_MemoryMapTraits>::size(1, 256),  // binary
      PAGE_SIZE};

  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;

  struct _Transaction : public BlockHeader {
    typedef _MemManager<_MemoryMapTraits> MemManager;

    /* the size of the file, this should be always equal the
       size of the database file. But in case of a crash during
       an transaction, the phyiscal file size could be bigger because
       of an alloc_new. */
    size_t file_size;

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

  struct Transaction : public _Transaction {
    typedef Pointer<Transaction> ptr;
    static constexpr auto SLOT_ID =
        MemManager::assign_slot(sizeof(_Transaction));

    uint16_t size() const { return sizeof(Transaction); }

    template <typename Storage>
    static ptr alloc(Storage& storage) {
      return storage.alloc_slot(SLOT_ID);
    }

    template <typename Storage>
    ptr clone(Storage& storage) const {
      ptr new_txn = alloc(storage);
      copy(*new_txn, *this);
      return new_txn;
    }
  };
};

template <typename Traits_>
struct _MemoryMapFile {
  typedef Traits_ Traits;
  using BlockHeader = typename Traits::BlockHeader;
  using Transaction = typename Traits::Transaction;
  using MemManager = typename Transaction::MemManager;
  using offset_e = typename Traits::offset_e;
  using tid_e = typename Traits::tid_e;
  using block_ptr = typename Traits::ptr;
  using txn_ptr = typename Transaction::ptr;
  using mem_ptr = typename MemManager::ptr;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static constexpr auto& MAX_PROCESSES = Traits::MAX_PROCESSES;
  static const bool is_transactional = true;
  typedef _MemoryMapFile<Traits> MemoryMapFile;
  typedef _MemStatistics<Traits> MemStatistics;
  typedef _Cursor<MemoryMapFile> Cursor;

  struct FileHeader {
    char signature[SIGNATURE_SIZE];
    uint16_t db_version;
    offset_t active_txn;
    offset_t prepared_txn;
    interprocess_mutex txn_mutex;
    interprocess_mutex file_mutex;
    pid_type txn_locker;
    pid_type file_locker;
    pid_type processes[MAX_PROCESSES];

    FileHeader() {
      strcpy(signature, SIGNATURE);
      db_version = 0;
      memset(processes, 0, sizeof(processes));
    }
  };

  file_mapping _file;
  mapped_region _region;
  FileHeader* _db;
  pid_type _pid;

  // the current transaction 
  Transaction _txn;

  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id;

  _MemoryMapFile(const char* path, size_t map_size = 2 * G) {
    _pid = current_pid();
    init_dbfile(path, map_size);
    // transaction is active if _txn.txn_id > active_txn()->txn_id
    _txn.txn_id = 0;
  }

  ~_MemoryMapFile() {
    remove_pid();
    _region.flush();
  }

  const char* filename() const { return _file.get_name(); }

  void init_dbfile(const char* path, size_t map_size) {
    if (!std::filesystem::is_regular_file(path)) {
      std::ofstream fhead(path, std::ios::out | std::ios::binary);
      fhead.put('l');
      fhead.close();
      std::filesystem::resize_file(path, AREA_SIZE);

      _file = file_mapping(path, read_write);
      _region = mapped_region(_file, read_write, 0, map_size);
      _db = new (_region.get_address()) FileHeader;

      _txn.mem_manager.init(sizeof(FileHeader));
      _txn.txn_id = 1;
      _txn.file_size = _txn.mem_manager. allocation_end;
      _txn.root = _txn.mem_root = 0;
      _txn.next_txn = 0;
      txn_ptr new_txn = _txn.clone(*this);
      new_txn->count = 0;
      new_txn->start_txn = _db->active_txn = _db->prepared_txn =
          resolve(new_txn);
      _region.flush();
    } else {
      std::ifstream fin(path);
      char signature[sizeof(SIGNATURE)];
      fin.read(signature, sizeof(signature));
      if (strcmp(signature, SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }

      _file = file_mapping(path, read_write);
      _region = mapped_region(_file, read_write, 0, map_size);
      _db = (FileHeader*)_region.get_address();
    }

    assert(((uint64_t)_db & 7) == 0);
    sanitize();
    set_pid();
  }

  void set_pid() {
    std::scoped_lock lock(_db->file_mutex);
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (!_db->processes[i]) {
        _db->processes[i] = _pid;
        return;
      }
    }
    throw NoProcess();
  }

  void remove_pid() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (_db->processes[i] == _pid) {
        _db->processes[i] = 0;
        return;
      }
    }
  }

  void sanitize() {
    if (sanitize_processes()) {
      if (_db->txn_locker) recover_mutex(_db->txn_mutex, _db->txn_locker);
      if (_db->file_locker) recover_mutex(_db->file_mutex, _db->file_locker);
      sanitize_transactions();
      if (std::filesystem::file_size(filename()) != txn()->file_size) {
        std::filesystem::resize_file(filename(), txn()->file_size);
      }
    }
  }

  bool sanitize_processes() {
    auto ap = all_pids();
    std::sort(ap.begin(), ap.end());

    int free_count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (_db->processes[i]) {
        if (!std::binary_search(ap.begin(), ap.end(), _db->processes[i])) {
          _db->processes[i] = 0;
          free_count++;
        }
      } else
        free_count++;
    }

    return free_count == MAX_PROCESSES;  // the first to open the db
  }

  void recover_mutex(interprocess_mutex& mutex, pid_type& locker) {
    auto ap = all_pids();
    for (const auto& pid : ap) {
      if (pid == locker) return;
    }
    // locker does not exist anymore
    locker = 0;
    while (!mutex.try_lock()) mutex.unlock();
    mutex.unlock();  // one more unlock
  }

  void save_lock(interprocess_mutex& mutex, pid_type& locker, int secs = 10) {
    while (!mutex.try_lock_for(std::chrono::seconds(secs))) {
      recover_mutex(mutex, locker);
    }
    locker = _pid;
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

  block_ptr alloc(uint16_t space) {
    return alloc_slot(MemManager::assign_slot(space));
  }

  block_ptr alloc_slot(uint16_t slot) {
    block_ptr result = _txn.mem_manager.alloc(slot, *this);
    if (!result) {
      assert(0);
      // big value handling
    }
    result->txn_id = _txn.txn_id;
    return result;
  }

  void free(block_ptr& block) {
    bool done = _txn.mem_manager.free(block, *this);
    if (!done) {
      // the key will be block_size (big endian 4byte) + transaction id (8byte
      // big endian)
    }
  }

  block_ptr resolve(offset_t offset) const {
    char* p = (char*)_db + (uint64_t)offset;
#ifdef __GNUC__
    __builtin_prefetch(p);
#endif
    return block_ptr(p);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return offset_t((uint64_t)p - (uint64_t)_db).type(p.type);
  }

  void prefetch(offset_t offset) const {
    char* p = (char*)_db + (uint64_t)offset;
#ifdef __GNUC__
    __builtin_prefetch(p);
#endif
  }

  template <typename T>
  bool may_recycle(T& garbage_block) const {
    return garbage_block.txn_id < _start_txn_id;
  }

  template <typename T>
  void mark_for_recycle(T& garbage_block) const {
    garbage_block.txn_id = _txn.txn_id;
  }

  uint64_t alloc_space(size_t space) {
    if (_db->file_locker != _pid) save_lock(_db->file_mutex, _db->file_locker);
    assert(_txn.file_size == std::filesystem::file_size(filename()));
    uint64_t result = _txn.file_size;
    uint64_t size = _txn.file_size + space;
    if (size > _region.get_size()) throw std::bad_alloc();
    _txn.file_size = size;
    std::filesystem::resize_file(filename(), size);
    return result;
  }

  template <typename T>
  void iter_transactions(T caller) {
    txn_ptr txn = resolve(_db->active_txn);
    tid_t end = txn->txn_id;
    offset_t* link = &txn->start_txn;
    do {
      txn = resolve(*link);
      if (caller(txn)) break;
      link = &txn->next_txn;
    } while (txn->txn_id < end);
  }

  void sanitize_transactions() {
    tid_t max_txn = 0;
    iter_transactions([](txn_ptr txn) -> bool {
      txn->count = 0;
      return false;
    });
    if (_db->active_txn != _db->prepared_txn) {
      _db->active_txn = _db->prepared_txn;
      _region.flush();
    }

    txn_ptr txn = resolve(_db->active_txn);
    std::filesystem::resize_file(filename(), txn->file_size);
  }

  txn_ptr txn() { return resolve(_db->active_txn); }

  bool start_transaction(bool wait = false) {
    if (_db->txn_locker == _pid) throw TransactionActive();

    if (wait)
      save_lock(_db->txn_mutex, _db->txn_locker);
    else if (!_db->txn_mutex.try_lock())
      return false;

    _db->txn_locker = _pid;

    // find a free transaction and the oldest used transaction
    txn_ptr active = txn();
    copy(_txn, *active);
    _txn.txn_id = active->txn_id + 1;
    _txn.next_txn = _txn.start_txn = 0;
    _start_txn_id = active->txn_id;

    iter_transactions([this](txn_ptr txn) -> bool {
      if (txn->count) {
        _txn.start_txn = resolve(txn);
        _start_txn_id = txn->txn_id;
        return true;
      }
      free(txn);
      return false;
    });

    return true;
  }

  void rollback() {
    _txn.txn_id = 0;
    _db->prepared_txn = _db->active_txn;
    _region.flush();
    end_transaction();
  }

  void prepare_commit() {
    // sink must be first! Because clone changes _txn.src.
    txn_ptr new_txn = _txn.clone(*this);
    new_txn->count = 0;

    _db->prepared_txn = resolve(new_txn);
    if (!_txn.start_txn) {
      // active_txn has been freed already
      new_txn->start_txn = _db->prepared_txn;
    } else {
      txn_ptr active = resolve(_db->active_txn);
      active->next_txn = _db->prepared_txn;
    }
    _region.flush();
  }

  void commit() {
    _db->active_txn = _db->prepared_txn;
    _region.flush();
    end_transaction();
  }

  void end_transaction() {
    if (_db->file_locker) {
      _db->file_locker = 0;
      _db->file_mutex.unlock();
    }
    assert(_db->txn_locker == _pid);
    _db->txn_locker = 0;
    _db->txn_mutex.unlock();
  }

  struct Statistics {
    MemStatistics garbage, branch, leaf, transaction;
  };

  void _garbage_statistics(MemStatistics& tofill) {
    txn_ptr txn_ = txn();
    const int garbage =
        MemManager::assign_slot(MemManager::GarbageContainer::SIZE);
    for (int i = 0; i < MemManager::COUNT; i++) {
      auto slot = txn_->mem_manager.slots[i];
      // collect blocks
      offset_t o = slot.ostart;
      size_t count = 0;
      while (true) {
        typename MemManager::Slot::garb_ptr gc = resolve(o);
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

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

}  // namespace leaves

#endif  // _LEAVES__MMAP_HPP