#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_check.hpp"
#include "leaves/intern/_mmap.hpp"

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

typedef DBMMap::block_ptr block_ptr;
typedef DBMMap::Traits::BlockHeader BlockHeader;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_db";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    std::filesystem::path dbFilePath = tempDir / "test.lvs";
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

template <typename DB>
struct Transaction {
  Transaction(DB db_) : db(db_) { db->start_transaction(); }
  ~Transaction() { db->commit(); }

  DB db;
};

BOOST_AUTO_TEST_CASE(test_multi_transaction) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");
  auto txn = db->txn();
  txn->count++;

  block_ptr block1, block2, block3, block4;

  db->_header->txn_lock.lock();
  db->_header->txn_lock.unlock();

  {
    Transaction trans(db);
    block1 = db->alloc(311);
  }

  {
    Transaction trans(db);
    db->free(block1);
    block2 = db->alloc(311);
    BOOST_CHECK(block1 != block2);
  }

  {
    Transaction trans(db);
    db->free(block2);
    block3 = db->alloc(311);
    BOOST_CHECK(block1 != block3);
    BOOST_CHECK(block2 != block3);
  }

  BOOST_CHECK(txn->txn_id != db->txn()->txn_id);

  txn->count--;

  {
    Transaction trans(db);
    block4 = db->alloc(311);
    BOOST_CHECK(block4 != block3);
  }
}

BOOST_AUTO_TEST_CASE(test_extend) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");
  const size_t PAGE_SIZE = DBMMap::Traits::PAGE_SIZE;
  const size_t BLOCK_SIZE =
      DBMMap::Traits::BLOCK_SIZES[DBMMap::Traits::BLOCK_SIZES_COUNT - 1];
  {
    Transaction trans(db);
    int count = PAGE_SIZE / BLOCK_SIZE;
    for (int i = 0; i < count; i++) {
      db->alloc(BLOCK_SIZE);
    }
  }

  BOOST_CHECK_EQUAL(storage._memory->file_size, 2 * PAGE_SIZE);
}

BOOST_AUTO_TEST_CASE(test_rollback) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");

  db->start_transaction(true);
  auto block1 = db->alloc(1123);
  db->prepare_commit();
  db->rollback();

  db->start_transaction();
  auto block2 = db->alloc(1123);
  db->prepare_commit();
  db->commit();

  db->start_transaction();
  auto block3 = db->alloc(1123);
  db->prepare_commit();
  db->commit();

  BOOST_CHECK(block1 == block2);
  BOOST_CHECK(block1 != block3);
}

BOOST_AUTO_TEST_CASE(test_alloc_and_free_block) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::vector<offset_t> block_offsets;
  size_t file_size;

  {
    {
      DBMMap storage(dbFilePath.c_str());
    }
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");

    BOOST_REQUIRE(db->txn()->txn_id == 1);

    {
      Transaction trans(db);

      for (int i = 0; i < 64; i++) {
        block_ptr block = db->alloc(4 * K - sizeof(BlockHeader));
        block_offsets.push_back(db->resolve(block));
      }
      file_size = storage._memory->file_size;
    }

    DBMMap::DB::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == 2);
    BOOST_REQUIRE(file_size == storage._memory->file_size);
  }

  {
    // free the first page page (txn=2)
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    BOOST_REQUIRE(storage._memory->file_size == file_size);

    {
      Transaction trans(db);

      for (offset_t bo : block_offsets) {
        block_ptr block = db->resolve(bo);
        BOOST_REQUIRE(db->resolve(block) == bo);
        db->free(block);
      }
      // file_size = db->_txn.file_size;
    }

    DBMMap::DB::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == 3);
  }

  {
    // go one transaction ahead, to be able to harvest
    // the last freed bocks;
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    Transaction trans(db);
  }

  {
    // free the first page page (txn=2)
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    BOOST_REQUIRE_EQUAL(db->_storage._memory->file_size, file_size);

    {
      Transaction trans(db);
      for (offset_t bo : block_offsets) {
        block_ptr block = db->alloc(4 * K - sizeof(BlockHeader));
        offset_t offset = db->resolve(block);
        BOOST_REQUIRE(db->resolve(block) == bo);
      }
      // file_size = db->_txn.file_size;
    }

    DBMMap::DB::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == 5);
  }
}

BOOST_AUTO_TEST_CASE(test_recycle_db) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());

  auto db1 = storage.make("test1");
  auto db2 = storage.make("test2");

  {
    Transaction t(db1);
    db1->alloc(3 * K);
  }

  offset_t old_header = db1->resolve(db1->_header);
  db1.reset();
  storage.remove_db("test1");

  auto db3 = storage.make("test3");
  offset_t new_header = db3->resolve(db3->_header);
  BOOST_CHECK_EQUAL(old_header, new_header);
}

BOOST_AUTO_TEST_CASE(test_orphaned_aera) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());

  typedef typename DBMMap::Traits::template Pointer<AreaRegister> ptr;

  auto db1 = storage.make("test1");

  std::vector<offset_t> offsets;

  db1->start_transaction();
  BOOST_CHECK_EQUAL(db1->_wtxn.last_area.ilast, 0);
  // force the alloc of a new area
  const uint64_t ALLOC_SIZE = db1->_wtxn.mem_manager.allocation_end + 16 * K -
                              db1->_wtxn.mem_manager.allocation_start;
  int size = 0;
  while (size < ALLOC_SIZE) {
    offsets.push_back(storage.resolve(db1->alloc(4 * K)));
    size += 4 * K;
  }

  BOOST_CHECK_EQUAL(db1->_wtxn.last_area.ilast, 1);

  // create a new area for db2
  auto db2 = storage.make("test2");

  db1->rollback();
  // the new area in db1 is "orphaned" and must be recycled the next time

  db1->start_transaction();
  BOOST_CHECK_EQUAL(db1->_wtxn.last_area.ilast, 0);

  // alloc again in the new area must be reused
  for (offset_t offset : offsets) {
    offset_t cmp = storage.resolve(db1->alloc(4 * K));
    BOOST_CHECK_EQUAL(cmp, offset);
  }
  db1->rollback();
}

struct BigSizeKey {
  boost::endian::big_uint64_t first;
  uint64_t second;
};

static constexpr uint64_t SIZE_BIT = uint64_t(1) << 63;

void check_memtrie_count(DBMMap::db_ptr db, int count) {
  auto memc = db->_mem_cursor;
  int c = 0;
  for (memc.first(); memc.is_valid(); memc.next()) {
    c++;
  }
  BOOST_CHECK_EQUAL(count, c);
}

template <typename DB>
void dump(DB db, const char* prefix, int index) {
  std::stringstream cstr;
  cstr << "errors/" << prefix << std::setw(2) << std::setfill('0') << index
       << ".yaml";
  std::ofstream out(cstr.str().c_str());
  _Dumper(*db, false, true).dump(out);
}

BOOST_AUTO_TEST_CASE(test_big_allocs) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());

  auto db = storage.make("test");

  std::vector<AreaSlice> slices;

  {
    Transaction t(db);

    for (int i = 0; i < 10; i++) {
      auto slice = db->alloc_big(10 * K);
      BOOST_CHECK_EQUAL(slice.size, 12 * K);
      slices.push_back(slice);

      // one offset and one size item
      check_memtrie_count(db, 2);
      // dump(db, "alloc_", i);
    }

    for (int i = 0; i < 10; i += 2) {
      db->free_big(slices[i].offset, slices[i].size);
      // dump(db, "freea_", i);
      check_memtrie_count(db, 4 + i);
    }

    int item_count = 12;
    for (int i = 1; i < 10; i += 2) {
      db->free_big(slices[i].offset, slices[i].size);
      check_memtrie_count(db, 11 - i);
      // dump(db, "freeb_", i);
    }

    // dump(db, "end_", 0);
  }
}

struct TestTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  static constexpr size_t PAGE_SIZE = 8 * K;
  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr uint16_t BLOCK_SIZES[] = {100, 4 * K};
  static constexpr uint16_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

  struct BlockHeader {
    typedef BlockHeader Base;
    tid_t txn_id;
    uint16_t slot_id;
    uint16_t free_idx;
  };

  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

struct TestStorage {
  typedef TestTraits Traits;
  using BlockHeader = typename TestTraits::BlockHeader;
  using block_ptr = typename TestTraits::ptr;
  using offset_e = typename TestTraits::offset_e;
  using uint32_e = typename TestTraits::uint32_e;
  using uint16_e = typename TestTraits::uint16_e;
  typedef _DB<TestStorage> DB;
  typedef std::shared_ptr<DB> db_ptr;

  static constexpr size_t PAGE_SIZE = Traits::PAGE_SIZE;

  struct Mutex {
    void recover() {}
    template <typename Time = std::chrono::seconds>
    void lock(Time t = Time(10)) {}
    bool try_lock() { return true; }
    void unlock() {}
  };

  AreaManager areas;
  std::vector<char> memory;
  db_ptr db;
  offset_t db_offset;
  Mutex mutex;

  TestStorage() {
    memory.reserve(8 * 1024 * 1024);
    memory.resize(PAGE_SIZE);
    db = std::make_shared<DB>(*this, &db_offset, 0);
  }

  Mutex& file_lock() { return mutex; }

  block_ptr resolve(offset_t offset, Access access = READ) {
    return block_ptr(&memory[offset]);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return offset_t((const char*)p - (char*)&memory[0]).type(p.type);
  }

  AreaSlice get_area(uint64_t size) {
    auto result = areas.get(size, *this);
    if (!result) {
      size_t old_size = memory.size();
      size_t next_size = padding(old_size + size, PAGE_SIZE);
      memory.resize(next_size);
      return AreaSlice{old_size, next_size - old_size};
    }
    return result;
  }

  void flush(bool async = true) {}
  void prefetch(offset_t offset, Access access = READ) const {}
  void prefetch(void* mem, Access access = READ) const {}
};

BOOST_AUTO_TEST_CASE(test_area_revolve) {
  TestStorage storage;

  BOOST_CHECK_EQUAL(storage.db->_header->areas.start,
                    storage.db->_header->areas.end);
  storage.db->start_transaction();
  for (int i = 0; i < AreaRegister::COUNT + 2; i++) {
    auto slice = storage.db->alloc_page();
    if (i != AreaRegister::COUNT - 1)
      BOOST_CHECK_EQUAL(slice.size, storage.PAGE_SIZE);
    else
      BOOST_CHECK_EQUAL(slice.size, storage.PAGE_SIZE / 2);
  }
  storage.db->rollback();

  BOOST_CHECK(storage.db->_header->areas.start !=
              storage.db->_header->areas.end);

  typedef typename TestTraits::template Pointer<AreaRegister> ptr;

  ptr ar = storage.resolve(storage.db->_header->areas.start);
  BOOST_CHECK(ar->next);
  BOOST_CHECK(storage.db->_header->areas.start);
  BOOST_CHECK_EQUAL(storage.db->txn()->last_area.olast,
                    storage.db->_header->areas.start);
  BOOST_CHECK_EQUAL(storage.db->txn()->last_area.ilast, 0);

  storage.db->start_transaction();
  for (int i = 0; i < AreaRegister::COUNT + 2; i++) {
    auto slice = storage.db->alloc_page();
    if (i != AreaRegister::COUNT - 1)
      BOOST_CHECK_EQUAL(slice.size, storage.PAGE_SIZE);
    else
      BOOST_CHECK_EQUAL(slice.size, storage.PAGE_SIZE / 2);
  }
  storage.db->commit();

  BOOST_CHECK_EQUAL(storage.db->txn()->last_area.olast, ar->next);
  BOOST_CHECK_EQUAL(storage.db->txn()->last_area.ilast, 3);
}

BOOST_AUTO_TEST_CASE(test_big_area_revolve) {
  TestStorage storage;

  BOOST_CHECK_EQUAL(storage.db->_header->big_areas.start,
                    storage.db->_header->big_areas.end);
  storage.db->start_transaction();
  for (int i = 0; i < AreaRegister::COUNT + 2; i++) {
    auto slice = storage.db->alloc_big(storage.PAGE_SIZE);
    BOOST_CHECK_EQUAL(slice.size, storage.PAGE_SIZE);
  }
  storage.db->rollback();

  BOOST_CHECK(storage.db->_header->big_areas.start !=
              storage.db->_header->big_areas.end);

  typedef typename TestTraits::template Pointer<AreaRegister> ptr;

  ptr ar = storage.resolve(storage.db->_header->big_areas.start);
  BOOST_CHECK(ar->next);
  BOOST_CHECK(storage.db->_header->big_areas.start);

  storage.db->start_transaction();
  storage.db->alloc_big(5 * storage.PAGE_SIZE);
  storage.db->commit();

  BOOST_CHECK_EQUAL(storage.db->txn()->last_big_area.olast, ar->next);
  BOOST_CHECK_EQUAL(storage.db->txn()->last_big_area.ilast, 3);
}