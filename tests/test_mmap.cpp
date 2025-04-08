#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBMMapTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_mmap.hpp"

using namespace leaves;

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

void wrong_signature(const char* path) { DBMMap db(path); }
BOOST_AUTO_TEST_CASE(test_init) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  {
    // Create a DBMMap instance and initialize it
    DBMMap db(dbFilePath.c_str());

    // Check if the database file is created
    BOOST_REQUIRE(std::filesystem::exists(dbFilePath));

    // Check if the active head is not null after initialization
    const DBMMap::txn_ptr head = db.txn();
    BOOST_REQUIRE(head != nullptr);

    BOOST_REQUIRE_EQUAL(db._db->db_version, 0);
    BOOST_REQUIRE_EQUAL(db._db->signature, SIGNATURE);
  }

  {
    DBMMap db(dbFilePath.c_str());
  }

  // Change the first byte of the file to 0
  std::fstream file(dbFilePath,
                    std::ios::in | std::ios::out | std::ios::binary);
  if (file.is_open()) {
    file.seekp(0);
    file.put(0);
    file.close();
  }

  BOOST_CHECK_THROW(wrong_signature(dbFilePath.c_str()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_double_open) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db1(dbFilePath.c_str());
  DBMMap db2(dbFilePath.c_str());

  BOOST_CHECK_EQUAL(db1.txn()->txn_id, db2.txn()->txn_id);
}

struct Transaction {
  Transaction(DBMMap& db_) : db(db_) { db.start_transaction(); }
  ~Transaction() {
    db.prepare_commit();
    db.commit();
  }

  DBMMap& db;
};

using BlockHeader = DBMMap::BlockHeader;
using block_ptr = DBMMap::block_ptr;

BOOST_AUTO_TEST_CASE(test_mutex_recovery) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());
  db._db->file_locker = 0xFFFFFFFF;
  db._db->file_mutex.lock();
  db.save_lock(db._db->file_mutex, db._db->file_locker, 0);
  BOOST_CHECK_EQUAL(db._db->file_locker, db._pid);
}

BOOST_AUTO_TEST_CASE(test_sanitize_mutext) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());
  db._db->processes[2] = 0xFFFFFFFF;
  auto first = db.sanitize_processes();
  BOOST_CHECK(!first);
}

BOOST_AUTO_TEST_CASE(test_multi_transaction) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());

  auto txn = db.txn();
  txn->count++;

  block_ptr block1, block2, block3, block4;

  {
    Transaction trans(db);
    block1 = db.alloc(311);
  }

  {
    Transaction trans(db);
    db.free(block1);
    block2 = db.alloc(311);
    BOOST_CHECK(block1 != block2);
  }

  {
    Transaction trans(db);
    db.free(block2);
    block3 = db.alloc(311);
    BOOST_CHECK(block1 != block3);
    BOOST_CHECK(block2 != block3);
  }

  BOOST_CHECK(txn->txn_id != db.txn()->txn_id);

  txn->count--;

  {
    Transaction trans(db);
    block4 = db.alloc(311);
    BOOST_CHECK(block4 != block3);
  }
}

BOOST_AUTO_TEST_CASE(test_extend) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());
  {
    Transaction trans(db);
    int count = AREA_SIZE/PAGE_SIZE;
    for(int i = 0; i < count; i++) {
      db.alloc(PAGE_SIZE);
    }
  }

  BOOST_CHECK_EQUAL(db.txn()->file_size, 2*AREA_SIZE + PAGE_SIZE);
}

BOOST_AUTO_TEST_CASE(test_rollback) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());

  db.start_transaction(true);
  auto block1 = db.alloc(1123);
  db.prepare_commit();
  db.rollback();

  db.start_transaction();
  auto block2 = db.alloc(1123);
  db.prepare_commit();
  db.commit();

  db.start_transaction();
  auto block3 = db.alloc(1123);
  db.prepare_commit();
  db.commit();

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
      DBMMap db(dbFilePath.c_str());
    }
    DBMMap db(dbFilePath.c_str());

    BOOST_REQUIRE(db.txn()->txn_id == 1);

    {
      Transaction trans(db);

      for (int i = 0; i < 64; i++) {
        block_ptr block = db.alloc(4 * K - sizeof(BlockHeader));
        block_offsets.push_back(db.resolve(block));
      }
      file_size = db._txn.file_size;
    }

    DBMMap::txn_ptr txn = db.txn();
    BOOST_REQUIRE(txn->txn_id == 2);
    BOOST_REQUIRE(file_size == db._txn.file_size);
  }

  {
    // free the first page page (txn=2)
    DBMMap db(dbFilePath.c_str());
    BOOST_REQUIRE(db.txn()->file_size == file_size);

    {
      Transaction trans(db);

      for (offset_t bo : block_offsets) {
        block_ptr block = db.resolve(bo);
        BOOST_REQUIRE(db.resolve(block) == bo);
        db.free(block);
      }
      // file_size = db._txn.file_size;
    }

    DBMMap::txn_ptr txn = db.txn();
    BOOST_REQUIRE(txn->txn_id == 3);
  }

  {
    // go one transaction ahead, to be able to harvest 
    // the last freed bocks;
    DBMMap db(dbFilePath.c_str());
    Transaction trans(db);
  }

  {
    // free the first page page (txn=2)
    DBMMap db(dbFilePath.c_str());
    BOOST_REQUIRE(db.txn()->file_size == file_size);

    {
      Transaction trans(db);
      for (offset_t bo : block_offsets) {
        block_ptr block = db.alloc(4 * K - sizeof(BlockHeader));
        offset_t offset = db.resolve(block);
        BOOST_REQUIRE(db.resolve(block) == bo);
      }
      // file_size = db._txn.file_size;
    }

    DBMMap::txn_ptr txn = db.txn();
    BOOST_REQUIRE(txn->txn_id == 5);
  }
}

#if 0
  {
    // free all the pages => two containers are needed for the freed pages
    DBMMap db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.db->txn[0].file_size == file_size);

    {
      Transaction trans(db);

      auto &pool = db.txn.pools[0];
      BOOST_REQUIRE(pool.last_free_start == 0);
      BOOST_REQUIRE(pool.last_free_end == 0);
      BOOST_REQUIRE(pool.free_start == block_offsets[1].offset());
      BOOST_REQUIRE(pool.free_end == block_offsets[0].offset());
    }

    BOOST_REQUIRE(db.txn()->txn_id == 4);
    auto &pool = db.txn()->pools[0];
    BOOST_REQUIRE(pool.last_free_start == 0);
    BOOST_REQUIRE(pool.last_free_end == 0);
    BOOST_REQUIRE(pool.free_start == block_offsets[1].offset());
    BOOST_REQUIRE(pool.free_end == block_offsets[0].offset());
  }

  {
    // free all the pages => two containers are needed for the freed pages
    DBMMap db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 1);
    BOOST_REQUIRE(db.db->txn[0].file_size == file_size);

    {
      Transaction trans(db);

      int cursor_id = db.alloc_cursor();
      db.shared->readers[cursor_id].txn_id = 1;

      block_ptr block = db.alloc_block(200);
      BOOST_REQUIRE(block->offset != block_offsets[1]);
      BOOST_REQUIRE(block->offset != block_offsets[0]);
      block_offsets.push_back(block->offset);
      db.free_block(block);

      auto &pool = db.txn.pools[0];
      BOOST_REQUIRE(pool.last_free_start == block_offsets[2].offset());
      BOOST_REQUIRE(pool.last_free_end == block_offsets[2].offset());
      BOOST_REQUIRE(pool.free_start == block_offsets[1].offset());
      BOOST_REQUIRE(pool.free_end == block_offsets[0].offset());

      db.free_cursor(cursor_id);
    }

    BOOST_REQUIRE(db.txn()->txn_id == 5);
    auto &pool = db.txn()->pools[0];
    BOOST_REQUIRE(pool.last_free_start == block_offsets[2].offset());
    BOOST_REQUIRE(pool.last_free_end == block_offsets[2].offset());
    BOOST_REQUIRE(pool.free_start == block_offsets[1].offset());
    BOOST_REQUIRE(pool.free_end == block_offsets[0].offset());
  }


  {
    // free all the pages => two containers are needed for the freed pages
    DBMMap db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.db->txn[0].file_size == file_size);

    {
      Transaction trans(db);
      auto &pool = db.txn.pools[0];
      BOOST_REQUIRE(pool.last_free_start == 0);
      BOOST_REQUIRE(pool.last_free_end == 0);
      BOOST_REQUIRE(pool.free_start == block_offsets[1].offset());
      BOOST_REQUIRE(pool.free_end == block_offsets[2].offset());

      block_ptr block = db.alloc_block(200);
      BOOST_REQUIRE(block->offset == block_offsets[1]);
      
      BOOST_REQUIRE(pool.last_free_start == 0);
      BOOST_REQUIRE(pool.last_free_end == 0);
      BOOST_REQUIRE(pool.free_start == block_offsets[0].offset());
      BOOST_REQUIRE(pool.free_end == block_offsets[2].offset());
    }

    BOOST_REQUIRE(db.txn()->txn_id == 6);
    auto &pool = db.txn()->pools[0];
    BOOST_REQUIRE(pool.last_free_start == 0);
    BOOST_REQUIRE(pool.last_free_end == 0);
    BOOST_REQUIRE(pool.free_start == block_offsets[0].offset());
    BOOST_REQUIRE(pool.free_end == block_offsets[2].offset());
  }
}
#endif

#if 0

BOOST_AUTO_TEST_CASE(test_write_value) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  DBMMap::init(dbFilePath.c_str());
  DBMMap db(dbFilePath.c_str());

  std::string value("Hello, World!");
  offset_ptr offset;
  {
    Transaction trans(db);
    offset = db.alloc_block(1, value.size());
    db.write_value(offset, value);
  }

  const ValueBlock& vb = db.get_block(offset)->value;
  BOOST_CHECK_EQUAL_COLLECTIONS(value.begin(), value.end(), vb.data,
                                vb.data + value.size());
}

/*
BOOST_AUTO_TEST_CASE(test_get_pool) {
  BOOST_REQUIRE_THROW(get_pool(4 * G+1), boost::execution_exception);
}*/

BOOST_AUTO_TEST_CASE(start_storage) {
  Preparation p;

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);
  }

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);
  }
}

BOOST_AUTO_TEST_CASE(multi_open) {
  Preparation p;

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);

    {
      Storage storage(TEST_FILE);
      BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);
    }
  }
}
#endif