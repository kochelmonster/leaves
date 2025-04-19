#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

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
  const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
  const size_t PAGE_SIZE =
      DBMMap::Traits::BLOCK_SIZES[DBMMap::Traits::BLOCK_SIZES_COUNT - 1];
  {
    Transaction trans(db);
    int count = AREA_SIZE / PAGE_SIZE;
    for (int i = 0; i < count; i++) {
      db->alloc(PAGE_SIZE);
    }
  }

  BOOST_CHECK_EQUAL(storage._memory->file_size, 2 * AREA_SIZE);
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
  BOOST_CHECK_EQUAL(db1->_wtxn.ilast_area, 0);
  // force the alloc of a new area
  const uint64_t ALLOC_SIZE = db1->_wtxn.mem_manager.allocation_end + 16 * K -
                              db1->_wtxn.mem_manager.next_free;
  int size = 0;
  while (size < ALLOC_SIZE) {
    offsets.push_back(storage.resolve(db1->alloc(4 * K)));
    size += 4 * K;
  }

  BOOST_CHECK_EQUAL(db1->_wtxn.ilast_area, 1);

  // create a new area for db2
  auto db2 = storage.make("test2");

  db1->rollback();
  // the new area in db1 is "orphaned" and must be recycled the next time

  db1->start_transaction();
  BOOST_CHECK_EQUAL(db1->_wtxn.ilast_area, 0);

  // alloc again in the new area must be reused
  for (offset_t offset : offsets) {
    offset_t cmp = storage.resolve(db1->alloc(4 * K));
    BOOST_CHECK_EQUAL(cmp, offset);
  }
  db1->rollback();
}
