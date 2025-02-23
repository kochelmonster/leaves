#ifndef _LEAVES__MMAP_HPP
#define _LEAVES__MMAP_HPP

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/detail/atomic.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/named_recursive_mutex.hpp>
#include <boost/process/v2/pid.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "./_cursor.hpp"
#include "./_memory.hpp"

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::named_recursive_mutex;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;

namespace leaves {

static const char SIGNATURE[] = "larch-leaves";
static const size_t SIGNATURE_SIZE = padding(sizeof(SIGNATURE), 8);

// definition og all headers and data types
struct _MemoryMapBlocks {
  struct BlockHeader {
    typedef uint8_t hash_t[0];
    typedef uint32_t bsize_t;  // block size
    typedef uint16_t ksize_t;  // key size
    typedef uint32_t vsize_t;  // value size
    typedef uint16_t scount_t;
    typedef uint64_t tid_t;  // transaction id
    typedef BlockHeader Base;
    typedef uint64_t offset_t;

    struct ptr {
      ptr(const ptr& src) : p(src.p) {}
      ptr(const void* p_ = nullptr) : p((BlockHeader*)p_) {}
      BlockHeader* operator->() { return p; }
      const BlockHeader* operator->() const { return p; }
      operator const BlockHeader*() const { return p; }
      operator BlockHeader*() { return p; }
      operator char*() { return (char*)p; }
      operator const uint8_t*() const { return (uint8_t*)p; }
      operator uint64_t() const { return (uint64_t)p; }
      operator uint64_t() { return (uint64_t)p; }
      operator bool() const { return p != nullptr; }
      operator bool() { return p != nullptr; }
      bool operator==(const ptr& other) const { return p == other.p; }
      bool operator!=(const ptr& other) const { return p != other.p; }
      bool operator!=(const void* other) const { return p != other; }
      BlockHeader* p;
    };

    template <typename T>
    struct Pointer : public ptr {
      static_assert(std::is_base_of<BlockHeader, T>::value,
                    "T must derive from BlockHeader");

      Pointer(void* src = nullptr) : ptr(src) {}
      Pointer(const ptr& src) : ptr(src.p) {}
      Pointer(const Pointer<T>& src) : ptr(src.p) {}
      T* operator->() { return static_cast<T*>(p); }
      const T* operator->() const { return static_cast<const T*>(p); }
      T& operator*() { return *static_cast<T*>(p); }
      const T& operator*() const { return *static_cast<T*>(p); }
    };

    struct BitsAndUsed {
      uint8_t features;
      uint8_t bits;
      uint16_t used;
    };

    static_assert(sizeof(BitsAndUsed) == 4);

    tid_t txn_id;
    bsize_t block_size;
    union {
      bsize_t size;
      bsize_t count;
      bsize_t bits;
      BitsAndUsed b;
    };
  };

  struct Transaction : public BlockHeader {
    typedef BlockHeader Base;
    typedef BlockHeader::Pointer<Transaction> ptr;
    typedef _MemManager<BlockHeader> MemManager;

    /* the size of the file, this should be always equal the
       size of the database file. But in case of a crash during
       an transaction, the phyiscal file size could be bigger because
       of an alloc_new. */
    size_t file_size;

    // pointer to the active root of the trie
    offset_t root;

    // pointer to the active root of the mem trie
    offset_t mem_root;

    // the number of leaves in the database
    size_t leaves;

    // the number of branches in the database
    size_t branches;

    // pointer to the oldest transaction
    offset_t start_txn;

    // pointer ot the next higher transaction
    offset_t next_txn;

    MemManager garbage;

    constexpr static size_t space(int slots) {
      return sizeof(Transaction) + MemManager::extra_space(slots);
    }

    template <typename Storage>
    static ptr alloc(Storage& storage, size_t space) {
      return storage.alloc(sizeof(Transaction) + space);
    }

    template <typename Storage>
    ptr clone(Storage& storage) const {
      ptr new_txn = alloc(storage, garbage.extra_space());
      copy(*new_txn, *this, garbage.extra_space());
      return new_txn;
    }
  };
};

template <typename Headers>
struct _MemoryMapFile {
  using BlockHeader = typename Headers::BlockHeader;
  using Transaction = typename Headers::Transaction;
  using MemManager = typename Transaction::MemManager;
  using offset_t = typename BlockHeader::offset_t;
  using tid_t = typename BlockHeader::tid_t;
  using bsize_t = typename BlockHeader::bsize_t;
  using block_ptr = typename BlockHeader::ptr;
  using txn_ptr = typename Transaction::ptr;
  using mem_ptr = typename MemManager::ptr;
  static const bool is_transactional = true;
  typedef _MemoryMapFile<Headers> MemoryMapFile;
  
  struct FileHeader {
    char signature[SIGNATURE_SIZE];
    uint16_t db_version;
    offset_t active_txn;
    offset_t prepared_txn;
  };

  struct _FileStart {
    union {
      FileHeader header;
      char _buffer1[padding(sizeof(FileHeader), MIN_BLOCK)];
    };
    union {
      Transaction _txn;
      char _buffer2[BLOCK_SIZES[assign_block(Transaction::space(2))]];
    };
  };

  named_recursive_mutex* _mutex;
  file_mapping _file;
  mapped_region _region;
  FileHeader* _db;
  union {
    // the current transaction used with enough extra space
    // for all GarbageSlots;
    Transaction _txn;
    char _buffer[sizeof(Transaction) + Transaction::MemManager::EXTRA_SPACE];
  };
  // All Transactions with a tid >= _start_txn_id may not be recycled
  tid_t _start_txn_id;

  _MemoryMapFile(const char* path, size_t map_size = G) {
    init_dbfile(path, map_size);
    // transaction is active if _txn.txn_id > active_txn()->txn_id
    _txn.txn_id = 0;
  }

  ~_MemoryMapFile() {
    delete _mutex;
    std::string mname(filename());
    std::replace(mname.begin() + 1, mname.end(), '/', '-');
    mname.append("-lock");
    named_recursive_mutex::remove(mname.c_str());
  }

  const char* filename() const { return _file.get_name(); }

  template <typename T>
  const uint8_t* puint8(const T* p) const {
    return reinterpret_cast<const uint8_t*>(p);
  }

  void init_dbfile(const char* path, size_t map_size) {
    std::string mname(path);
    std::replace(mname.begin() + 1, mname.end(), '/', '-');
    mname.append("-lock");

    if (!std::filesystem::is_regular_file(path)) {
      named_recursive_mutex::remove(mname.c_str());
      _FileStart start;
      memset(&start, 0, sizeof(start));
      strcpy(start.header.signature, SIGNATURE);
      start.header.db_version = 0;
      start.header.active_txn = start.header.prepared_txn =
          puint8(&start._txn) - puint8(&start);

      // manually setup the first transaction
      start._txn.block_size = sizeof(start._buffer2);
      start._txn.txn_id = 1;
      start._txn.start_txn = start.header.active_txn;
      start._txn.file_size = start._txn.garbage.init(sizeof(start));

      std::ofstream fhead(path, std::ios::out | std::ios::binary);
      fhead.write((const char*)&start, sizeof(start));
      fhead.close();
      std::filesystem::resize_file(path, start._txn.file_size);
    } else {
      std::ifstream fin(path);
      char signature[sizeof(SIGNATURE)];
      fin.read(signature, sizeof(signature));
      if (strcmp(signature, SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }
    }

    _file = file_mapping(path, read_write);
    _region = mapped_region(_file, read_write, 0, map_size);
    _db = (FileHeader*)_region.get_address();
    assert(((uint64_t)_db & 7) == 0);
    try {
      _mutex = new named_recursive_mutex(create_only, mname.c_str());
      sanitize_transactions();
    } catch (...) {
      _mutex = new named_recursive_mutex(open_only, mname.c_str());
    }
  }

  template <typename ptr>
  ptr cow_replace(ptr& src) {
    ptr result = clone(src);
    free(src);
    return result;
  }

  template <typename ptr>
  ptr clone(const ptr& src) {
    ptr dest = alloc(src->block_size);
    copy(*dest, *src, src->space());
    return dest;
  }

  block_ptr alloc(bsize_t space) {
    auto result = _txn.garbage.alloc(space, *this);
    if (!result) {
      assert(space > 1000000);
      assert(0);
      // big value handling
    }
    result->txn_id = _txn.txn_id;
    return result;
  }

  void free(const block_ptr& block) {
    bool done = _txn.garbage.free(block, *this);
    if (!done) {
      assert(block->block_size > 1000000);
      // the key will be block_size (big endian 4byte) + transaction id (8byte
      // big endian)
    }
  }

  block_ptr resolve(offset_t offset) const {
    return (block_ptr)(puint8(_db) + offset);
  }

  offset_t resolve(const block_ptr& p) const {
    return (uint64_t)p - (uint64_t)_db;
  }

  template <typename T>
  bool may_recycle(T& garbage_block) const {
    return garbage_block.tid < _start_txn_id;
  }

  template <typename T>
  void mark_for_recycle(T& garbage_block) const {
    garbage_block.tid = _txn.txn_id;
  }

  void extend_file(size_t size) {
    _txn.file_size = size;
    if (size > _region.get_size()) throw std::bad_alloc();
    std::filesystem::resize_file(filename(), size);
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

  txn_ptr active_txn() { return resolve(_db->active_txn); }

  bool start_transaction(bool wait = false) {
    if (wait)
      _mutex->lock();
    else if (!_mutex->try_lock())
      return false;

    // find a free transaction and the oldest used transaction
    txn_ptr active = active_txn();
    copy(_txn, *active, active->garbage.extra_space());
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

  void end_transaction() { _mutex->unlock(); }

  void garbage_statistics(MemStatistics& tofill) {
    txn_ptr txn = active_txn();
    const int garbage = assign_block(MemManager::GarbageContainer::SIZE);
    for (auto iter = txn->garbage.slots.begin();
         iter != txn->garbage.slots.end(); ++iter) {
      auto slot = *iter;

      // collect bocks
      offset_t o = slot.ostart;
      size_t count = 0;
      while (true) {
        typename MemManager::Slot::garb_ptr gc = resolve(o);
        count++;
        if (o == slot.oend) break;
        o = gc->next;
      }
      tofill.add(BLOCK_SIZES[garbage], count);
      tofill.add(BLOCK_SIZES[iter.index], slot.count);
    }
  }

  void _add_node_statistics(MemStatistics& tofill, offset_t boffset) {
    size_t size1 = 0, size2 = 0;
    typedef _UpperBranchNode<BlockHeader> UpperBranchNode;
    typedef _LowerBranchNode<BlockHeader> LowerBranchNode;
    typedef typename LowerBranchNode::ptr lbranch_ptr;
    typedef typename UpperBranchNode::ptr ubranch_ptr;
    ubranch_ptr ubranch = resolve(boffset);
    lbranch_ptr lbranch;
    tofill.add(ubranch->block_size, 1, ubranch->freespace());
    ubranch->iterate_links(*this, [&lbranch, &tofill, this](
                                      const lbranch_ptr& lb, offset_t& offset) {
      if (lb && lb != lbranch) {
        lbranch = lb;
        block_ptr leaf = resolve(lb->leaves);
        tofill.add(lb->block_size, 1, lb->freespace());
        tofill.add(leaf->block_size, 1,
                   leaf->block_size - lb->leaves_used - sizeof(BlockHeader));
      }
      
      if (!isleaf(offset) && lb) {
        _add_node_statistics(tofill, offset);
      }
    });
  }

  void node_statistics(MemStatistics& tofill) {
    _add_node_statistics(tofill, active_txn()->root);

    iter_transactions([this, &tofill](txn_ptr txn) -> bool {
      tofill.add(
          txn->block_size, 1,
          txn->block_size - txn->garbage.extra_space() - sizeof(Transaction));
      offset_t offset = resolve(txn);
      return false;
    });
  }
};

typedef _MemoryMapFile<_MemoryMapBlocks> DBMMap;
typedef _Cursor<DBMMap> Cursor;

}  // namespace leaves

#endif  // _LEAVES__MMAP_HPP