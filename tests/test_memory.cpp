#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBMemoryTest

#include <boost/test/included/unit_test.hpp>

#include "../src/memory.hpp"
#include "testpoints.hpp"

using namespace leaves;

void wrong_signature(const char* path) { DBMemory db(path); }

BOOST_AUTO_TEST_CASE(test_init) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  {
    // Create a DBMemory instance and initialize it
    DBMemory db(dbFilePath.c_str());

    // Check if the database file is created
    BOOST_REQUIRE(std::filesystem::exists(dbFilePath));

    // Check if the active head is not null after initialization
    const DBTransaction* head = db.get_active_txn();
    BOOST_REQUIRE(head != nullptr);

    BOOST_REQUIRE_EQUAL(db.db->db_version, 0);
    BOOST_REQUIRE_EQUAL(db.db->active, 0);
    BOOST_REQUIRE_EQUAL(db.db->signature, SIGNATURE);
  }

  { DBMemory db(dbFilePath.c_str()); }

  // Change the first byte of the file to 0
  std::fstream file(dbFilePath,
                    std::ios::in | std::ios::out | std::ios::binary);
  if (file.is_open()) {
    file.seekp(0);
    file.put(0);
    file.close();
  }

  BOOST_CHECK_THROW(wrong_signature(dbFilePath.c_str()), std::runtime_error);

  const char* test_points[] = {"DBMemory::init::1", "DBMemory::init::2",
                               "DBMemory::init::3", NULL};
  check_testpoints(test_points);
}

struct Transaction {
  Transaction(DBMemory& db_) : db(db_) { db.start_transaction(); }
  ~Transaction() {
    db.prepare_commit();
    db.commit();
  }

  DBMemory& db;
};

BOOST_AUTO_TEST_CASE(test_alloc_and_free_block) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::vector<offset_ptr> block_offsets;
  offset_ptr file_size;
  const int TRIE_POOL = get_pool(TrieBlock::SIZE);


  {
    // Allocate more pages than one container can hold
    {DBMemory db(dbFilePath.c_str());}
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.get_active_txn()->txn_id == 1);

    {
      Transaction trans(db);

      for(int i = 0; i < 2; i++) {
        block_ptr block = db.alloc_cow_block();
        BOOST_REQUIRE(block->type == kTrieBlock);
        BOOST_REQUIRE(block->offset != 0);
        BOOST_REQUIRE(block->size == TrieBlock::SIZE);
        block_offsets.push_back(block->offset);
      }
      file_size = db.txn.file_size;
    }
    
    BOOST_REQUIRE(db.get_active_txn()->txn_id == 2);
  }

  {
    // free the first page page (txn=2)
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 1);
    BOOST_REQUIRE(db.db->txn[1].file_size == file_size);

    {
      Transaction trans(db);

      for (offset_ptr bo : block_offsets) {
        block_ptr block = db.get_block(bo);
        BOOST_REQUIRE(block->offset == bo);
        BOOST_REQUIRE(block->size == TrieBlock::SIZE);
        BOOST_REQUIRE(*(int*)&block.trie()->data[0] == 0);
        db.free_block(block);
      }
      file_size = db.txn.file_size;
    }
    BOOST_REQUIRE(db.get_active_txn()->txn_id == 3);
    
    auto &pool = db.get_active_txn()->pools[TRIE_POOL];
    BOOST_REQUIRE(pool.last_free_start == block_offsets[1]);
    BOOST_REQUIRE(pool.last_free_end == block_offsets[0]);
    BOOST_REQUIRE(pool.free_start == 0);
    BOOST_REQUIRE(pool.free_end == 0);
  }

  {
    // free all the pages => two containers are needed for the freed pages
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.db->txn[0].file_size == file_size);

    {
      Transaction trans(db);

      auto &pool = db.txn.pools[TRIE_POOL];
      BOOST_REQUIRE(pool.last_free_start == 0);
      BOOST_REQUIRE(pool.last_free_end == 0);
      BOOST_REQUIRE(pool.free_start == block_offsets[1]);
      BOOST_REQUIRE(pool.free_end == block_offsets[0]);
    }

    BOOST_REQUIRE(db.get_active_txn()->txn_id == 4);
    auto &pool = db.get_active_txn()->pools[TRIE_POOL];
    BOOST_REQUIRE(pool.last_free_start == 0);
    BOOST_REQUIRE(pool.last_free_end == 0);
    BOOST_REQUIRE(pool.free_start == block_offsets[1]);
    BOOST_REQUIRE(pool.free_end == block_offsets[0]);
  }

  {
    // free all the pages => two containers are needed for the freed pages
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 1);
    BOOST_REQUIRE(db.db->txn[0].file_size == file_size);

    {
      Transaction trans(db);

      int cursor_id = db.alloc_cursor();
      db.shared->readers[cursor_id].txn_id = 1;

      block_ptr block = db.alloc_cow_block();
      BOOST_REQUIRE(block->offset != block_offsets[1]);
      BOOST_REQUIRE(block->offset != block_offsets[0]);
      block_offsets.push_back(block->offset);
      db.free_block(block);

      auto &pool = db.txn.pools[TRIE_POOL];
      BOOST_REQUIRE(pool.last_free_start == block_offsets[2]);
      BOOST_REQUIRE(pool.last_free_end == block_offsets[2]);
      BOOST_REQUIRE(pool.free_start == block_offsets[1]);
      BOOST_REQUIRE(pool.free_end == block_offsets[0]);

      db.free_cursor(cursor_id);
    }

    BOOST_REQUIRE(db.get_active_txn()->txn_id == 5);
    auto &pool = db.get_active_txn()->pools[TRIE_POOL];
    BOOST_REQUIRE(pool.last_free_start == block_offsets[2]);
    BOOST_REQUIRE(pool.last_free_end == block_offsets[2]);
    BOOST_REQUIRE(pool.free_start == block_offsets[1]);
    BOOST_REQUIRE(pool.free_end == block_offsets[0]);
  }


  {
    // free all the pages => two containers are needed for the freed pages
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.db->txn[0].file_size == file_size);

    {
      Transaction trans(db);
      auto &pool = db.txn.pools[TRIE_POOL];
      BOOST_REQUIRE(pool.last_free_start == 0);
      BOOST_REQUIRE(pool.last_free_end == 0);
      BOOST_REQUIRE(pool.free_start == block_offsets[1]);
      BOOST_REQUIRE(pool.free_end == block_offsets[2]);

      block_ptr block = db.alloc_cow_block();
      BOOST_REQUIRE(block->offset == block_offsets[1]);
      
      BOOST_REQUIRE(pool.last_free_start == 0);
      BOOST_REQUIRE(pool.last_free_end == 0);
      BOOST_REQUIRE(pool.free_start == block_offsets[0]);
      BOOST_REQUIRE(pool.free_end == block_offsets[2]);
    }

    BOOST_REQUIRE(db.get_active_txn()->txn_id == 6);
    auto &pool = db.get_active_txn()->pools[TRIE_POOL];
    BOOST_REQUIRE(pool.last_free_start == 0);
    BOOST_REQUIRE(pool.last_free_end == 0);
    BOOST_REQUIRE(pool.free_start == block_offsets[0]);
    BOOST_REQUIRE(pool.free_end == block_offsets[2]);
  }

  const char* test_points[] = {
      "DBMemory::alloc_block::1", "DBMemory::alloc_block::2",
      "DBMemory::alloc_block::3", "DBMemory::alloc_block::4",
      "DBMemory::free_block::1",  "DBMemory::free_block::2",
      "DBMemory::free_block::3",  NULL};
  check_testpoints(test_points);
}

#if 0

BOOST_AUTO_TEST_CASE(test_write_value) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  DBMemory::init(dbFilePath.c_str());
  DBMemory db(dbFilePath.c_str());

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