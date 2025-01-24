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
#include <vector>

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
    typedef uint8_t hash_t[16];
    typedef uint32_t bsize_t;  // block size
    typedef uint16_t ksize_t;  // key size
    typedef uint32_t vsize_t;  // value size
    typedef uint16_t scount_t;
    typedef uint64_t offset_t;  // offset of blocks
    typedef uint64_t tid_t;     // transaction id
    typedef BlockHeader* ptr;

    tid_t txn_id;
    offset_t next_free;  // the next free block
    bsize_t block_size;
    union {
      bsize_t size;
      bsize_t count;
    };

    template <typename T>
    void clone(const T& src) {
      memcpy((char*)this + sizeof(BlockHeader),
             (char*)&src + sizeof(BlockHeader), src.space());
      size = src.size;
      assert(block_size > src.space());
    }

    /* all BlockHeader subclasses must implement a space method
       that returns the space used by the block (without BlockHeader)
     */
  };

  struct MemManager : public _MemManager<BlockHeader> {
    typedef MemManager* ptr;
  };

  struct Transaction : public BlockHeader {
    typedef Transaction* ptr;

    /* the size of the file, this should be always equal the
      size of the database file. But in case of a crash during
      an transaction, the phyiscal file size could be bigger because
      of an alloc_new.
    */
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

    union {
      MemManager* src;  // available free blocks
      uint64_t src_offset;
    };

    union {
      MemManager* sink;  // where free blocks are saved to
      uint64_t sink_offset;
    };

    void add_ref() { count++; }
    void dec_ref() { count--; }

    size_t space() const { return sizeof(Transaction) - sizeof(BlockHeader); }
  };
};

template <typename Headers>
struct _MemoryMapFile {
  using BlockHeader = typename Headers::BlockHeader;
  using Transaction = typename Headers::Transaction;
  using MemManager = typename Headers::MemManager;
  using offset_t = typename BlockHeader::offset_t;
  using tid_t = typename BlockHeader::offset_t;
  using bsize_t = typename BlockHeader::bsize_t;
  using ptr_t = typename BlockHeader::ptr;
  using txnptr_t = typename Transaction::ptr;
  typedef _MemoryMapFile<Headers> MemoryMapFile;

  // A MemManager that has enough space to hold a full dumps array
  union FullMemManager {
    MemManager mm;
    char buffer[MemManager::FULL_SIZE];
  };

  struct FileHeader {
    char signature[SIGNATURE_SIZE];
    uint16_t db_version;
    offset_t active_txn;
    offset_t prepared_txn;
  };

  struct _FileStart {
    union {
      FileHeader header;
      char _buffer1[128];
    };
    union {
      Transaction _txn;
      char _buffer2[128];
    };
    union {
      MemManager _isrc;  // initial src
      char _buffer3[128];
    };
    union {
      MemManager _isink;  // initial sink
      char _buffer4[128];
    };
  };

  named_recursive_mutex* _mutex;
  file_mapping _file;
  mapped_region _region;
  FileHeader* _db;
  Transaction _txn;  // the current transaction used
  FullMemManager _src, _sink;

  _MemoryMapFile(const char* path, size_t map_size = G) {
    init_dbfile(path, map_size);
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
  uint8_t* puint8(T* p) {
    return reinterpret_cast<uint8_t*>(p);
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
      start._txn.block_size = 128;
      start._txn.txn_id = 1;
      start._txn.start_txn = start.header.active_txn;
      start._txn.file_size = MemManager::AREA_SIZE;
      start._txn.src_offset = puint8(&start._isrc) - puint8(&start);
      start._txn.sink_offset = puint8(&start._isink) - puint8(&start);
      start._isrc.block_size = 128;
      start._isrc.txn_id = 1;
      start._isrc.small.next_free = sizeof(start);
      start._isrc.small.end_current_area = start._txn.file_size;
      start._isink.block_size = 128;
      start._isink.txn_id = 1;
      std::ofstream fhead(path, std::ios::out | std::ios::binary);
      fhead.write((const char*)&start, sizeof(start));
      fhead.close();
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
    try {
      _mutex = new named_recursive_mutex(create_only, mname.c_str());
      sanitize_transactions();
    } catch (...) {
      _mutex = new named_recursive_mutex(open_only, mname.c_str());
    }
  }

  template <typename Block>
  typename Block::ptr clone_branch(typename Block::ptr src) {
    typename Block::ptr dest = alloc<Block>(src->space());
    dest->clone(src);
    free(src);
    return dest;
  }

  template <typename Block>
  typename Block::ptr alloc(bsize_t space) {
    auto result = _txn.src->alloc(space + sizeof(BlockHeader), *this);
    if (!result) {
      assert(space > 1000000);
      // big value handling
    }
    result->txn_id = _txn.txn_id;
    return (typename Block::ptr)result;
  }

  template <typename Block>
  void free(typename Block::ptr block) {
    bool done = block->txn_id == _txn.txn_id ? _txn.src->free(block, *this)
                                             : _txn.sink->free(block, *this);
    if (!done) {
      assert(block->block_size > 1000000);
      // the key will be block_size (big endian 4byte) + transaction id (8byte big endian)
    }                                             
  }

  template <typename Block>
  typename Block::ptr resolve(offset_t offset) {
    return (typename Block::ptr)(puint8(_db) + offset);
  }

  template <typename Block>
  offset_t resolve(typename Block::ptr p) {
    return puint8(p) - puint8(_db);
  }

  offset_t alloc_area(bsize_t size) {
    offset_t result = _txn.file_size;
    _txn.file_size += size;
    std::filesystem::resize_file(filename(), _txn.file_size);
    return result;
  }

  template <typename T>
  void iter_transactions(T caller) {
    Transaction* txn = resolve<Transaction>(_db->active_txn);
    tid_t end = txn->txn_id;
    offset_t* link = &txn->start_txn;
    do {
      txn = resolve<Transaction>(*link);
      if (caller(txn)) break;
      link = &txn->next_txn;
    } while (txn->txn_id < end);
  }

  void sanitize_transactions() {
    tid_t max_txn = 0;
    iter_transactions([](Transaction* txn) -> bool {
      txn->count = 0;
      return false;
    });
    if (_db->active_txn != _db->prepared_txn) {
      _db->active_txn = _db->prepared_txn;
      _region.flush();
    }

    auto txn = resolve<Transaction>(_db->active_txn);
    std::filesystem::resize_file(filename(), txn->file_size);
  }

  txnptr_t active_txn() { return resolve<Transaction>(_db->active_txn); }

  bool start_transaction(bool wait = false) {
    if (wait)
      _mutex->lock();
    else if (!_mutex->try_lock())
      return false;

    // find a free transaction and the minimal used transaction
    memset(_src.buffer, 0, sizeof(_src));
    memset(_sink.buffer, 0, sizeof(_sink));
    txnptr_t active = active_txn();
    auto active_src = resolve<MemManager>(active->src_offset);

    _txn.src = &_src.mm;
    _txn.sink = &_sink.mm;
    _txn.file_size = active->file_size;
    _txn.txn_id = active->txn_id + 1;
    _txn.root = active->root;
    _txn.leaves = active->leaves;
    _txn.branches = active->branches;
    _txn.next_txn = _txn.start_txn = 0;
    _txn.src->pclone(*active_src);

    // Fillup bitfield for sparse dumps array
    _txn.src->unify(*active_src);
    iter_transactions([this](txnptr_t txn) -> bool {
      if (txn->count) return true;
      auto sink = resolve<MemManager>(txn->sink_offset);
      _txn.src->unify(*sink);
      return false;
    });

    // add the dumps array data
    _txn.src->add(*active_src);
    iter_transactions([this](txnptr_t txn) -> bool {
      if (txn->count) {
        _txn.start_txn = resolve<Transaction>(txn);
        return true;
      }
      auto sink = resolve<MemManager>(txn->sink_offset);
      _txn.src->add(*sink);
      free<MemManager>(resolve<MemManager>(txn->src_offset));
      free<MemManager>(sink);
      free<Transaction>(txn);
      return false;
    });

    return true;
  }

  void rollback() {
    _db->prepared_txn = _db->active_txn;
    _region.flush();
    end_transaction();
  }

  void prepare_commit() {
    txnptr_t new_txn = alloc<Transaction>(sizeof(Transaction));
    new_txn->clone(_txn);

    auto bsrc = alloc<MemManager>(_txn.src->space());
    auto bsink = alloc<MemManager>(_txn.sink->space());

    bsrc->clone(*_txn.src);
    bsink->clone(*_txn.sink);

    new_txn->src_offset = resolve<MemManager>(bsrc);
    new_txn->sink_offset = resolve<MemManager>(bsink);

    _db->prepared_txn = resolve<Transaction>(new_txn);
    if (!_txn.start_txn) {
      // active_txn has been freed already
      new_txn->start_txn = _db->prepared_txn;
    } else {
      resolve<Transaction>(_db->active_txn)->next_txn = _db->prepared_txn;
    }
    _region.flush();
  }

  void commit() {
    _db->active_txn = _db->prepared_txn;
    _region.flush();
    end_transaction();
  }

  void end_transaction() { _mutex->unlock(); }
};

typedef _MemoryMapFile<_MemoryMapBlocks> DBMMap;

}  // namespace leaves

#endif  // _LEAVES__MMAP_HPP