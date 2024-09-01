#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBMemoryTest

#include <boost/test/included/unit_test.hpp>
#include "../src/memory.hpp"
#include "testpoints.hpp"

using namespace leaves;

BOOST_AUTO_TEST_CASE(test_init) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  {
    // Create a DBMemory instance and initialize it
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());

    // Check if the database file is created
    BOOST_REQUIRE(std::filesystem::exists(dbFilePath));

    // Check if the active head is not null after initialization
    const DBMeta* head = db.get_active_head();
    BOOST_REQUIRE(head != nullptr);

    BOOST_REQUIRE_EQUAL(db.db->db_version, 0);
    BOOST_REQUIRE_EQUAL(db.db->active, 0);
    BOOST_REQUIRE_EQUAL(db.db->signature, SIGNATURE);
  }

  { DBMemory::init(dbFilePath.c_str()); }

  // Change the first byte of the file to 0
  std::fstream file(dbFilePath,
                    std::ios::in | std::ios::out | std::ios::binary);
  if (file.is_open()) {
    file.seekp(0);
    file.put(0);
    file.close();
  }

  BOOST_CHECK_THROW(DBMemory::init(dbFilePath.c_str()), std::runtime_error);

  const char* test_points[] = {"DBMemory::init::1", "DBMemory::init::2",
                               "DBMemory::init::3", NULL};
  check_testpoints(test_points);
}

struct Transaction {
  Transaction(DBMemory& db_) : db(db_) { db.prepare_transaction(); }
  ~Transaction() {
    db.write_transaction();
    db.commit_transaction();
    db.end_transaction();
  }

  DBMemory& db;
};

BOOST_AUTO_TEST_CASE(test_alloc_and_free_container) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::vector<offset_ptr> offsets;

  {
    // Allocate more containers than one container can hold
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());
    {
      Transaction trans(db);
      // MAX_ITEMS + 2 because the first will be used as pool container
      for (int i = 0; i < BlockContainer::MAX_ITEMS + 2; i++) {
        // Allocate a block
        BlockContainer* block = db.alloc_container();
        BOOST_REQUIRE(block->offset != 0);
        BOOST_REQUIRE(block->size == BlockContainer::SIZE);
        offsets.push_back(block->offset);
      }
    }
  }

  {
    // Free all the containers
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());
    {
      Transaction trans(db);
      for (const offset_ptr& offset : offsets) {
        // Get the block using the offset
        BlockContainer* block =
            &db.get_writeable_block(offset, BlockContainer::SIZE)->container;
        db.free_container(block);
      }
    }

    offset_ptr c1_offset = db.get_active_head()->pools[FREE_POOL].free;
    BOOST_REQUIRE(c1_offset != 0);
    BlockContainer& c1 = db.get_block(c1_offset)->container;
    BOOST_REQUIRE(c1.next != 0);
    BOOST_REQUIRE(c1.count == 1);
    BlockContainer& c2 = db.get_block(c1.next)->container;
    BOOST_REQUIRE(c2.next == 0);
    BOOST_REQUIRE(c2.count == BlockContainer::MAX_ITEMS);
  }

  {
    // Allocate again and get them from the free list
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());
    {
      Transaction trans(db);
      for (int i = 0; i < BlockContainer::MAX_ITEMS + 3; i++) {
        // Allocate a block
        BlockContainer* block = db.alloc_container();
        BOOST_REQUIRE(block->size == BlockContainer::SIZE);
        BOOST_REQUIRE(block->offset != 0);
        offsets.pop_back();
      }
    }
    offset_ptr c1_offset = db.get_active_head()->pools[FREE_POOL].free;
    BOOST_REQUIRE(c1_offset == 0);
  }

  const char* test_points[] = {
      "DBMemory::alloc_container::1", "DBMemory::alloc_container::2",
      "DBMemory::alloc_container::3", "DBMemory::free_container::1",
      "DBMemory::free_container::2",  NULL};
  check_testpoints(test_points);
}

BOOST_AUTO_TEST_CASE(test_alloc_and_free_block) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::vector<offset_ptr> offsets;
  offset_ptr file_size;

  {
    // Allocate more pages than one container can hold
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.get_active_head()->txn_id == 1);

    {
      Transaction trans(db);
      for (int i = 0; i < BlockContainer::MAX_ITEMS + 1; i++) {
        BlockUnion* block = db.alloc_cow_block(1);
        block->trie.init();
        *(int*)&block->trie.data[0] = i;
        BOOST_REQUIRE(block->block.writable);
        BOOST_REQUIRE(block->trie.offset != 0);
        BOOST_REQUIRE(block->trie.size == TrieBlock::SIZE);
        offsets.push_back(block->trie.offset);
      }
      file_size = db.head.file_size;
    }

    BOOST_REQUIRE(db.get_active_head()->txn_id == 2);
  }

  {
    // free the first page page (txn=2)
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 1);
    BOOST_REQUIRE(db.db->head[1].file_size == file_size);

    {
      Transaction trans(db);
      offset_ptr offset = *offsets.begin();
      BlockUnion* block = db.get_block(offset);
      BOOST_REQUIRE(block->trie.offset == offset);
      BOOST_REQUIRE(block->trie.size == TrieBlock::SIZE);
      BOOST_REQUIRE(!block->trie.writable);
      BOOST_REQUIRE(*(int*)&block->trie.data[0] == 0);
      db.free_block(block);
      file_size = db.head.file_size;
    }
    BOOST_REQUIRE(db.get_active_head()->txn_id == 3);
    auto pool_id = get_pool(TrieBlock::SIZE);
    offset_ptr c1_offset = db.get_active_head()->pools[pool_id].free;
    BOOST_REQUIRE(c1_offset != 0);
  }

  {
    // free all the pages => two containers are needed for the freed pages
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());

    BOOST_REQUIRE(db.db->active == 0);
    BOOST_REQUIRE(db.db->head[0].file_size == file_size);

    {
      Transaction trans(db);
      int i = 0;
      for (const offset_ptr& offset : offsets) {
        if (i > 0) {
          BlockUnion* block = db.get_block(offset);
          BOOST_REQUIRE(block->trie.offset == offset);
          BOOST_REQUIRE(block->trie.size == TrieBlock::SIZE);
          BOOST_REQUIRE(*(int*)&block->trie.data[0] == i);

          // Free the block
          db.free_block(block);
        }
        i++;
      }
    }
    BOOST_REQUIRE(db.get_active_head()->txn_id == 4);
    auto pool_id = get_pool(TrieBlock::SIZE);
    offset_ptr c1_offset = db.get_active_head()->pools[pool_id].free;
    BOOST_REQUIRE(c1_offset != 0);
  }

  {
    // alloc the only block from free that is available
    // in txn 3
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());

    {
      Transaction trans(db);
      BlockUnion* block = db.alloc_cow_block(4);
      BOOST_REQUIRE(block->trie.offset == offsets[0]);
      offsets.erase(offsets.begin());
    }
  }

  {
    // allocate all other again to free the complete freed container
    DBMemory::init(dbFilePath.c_str());
    DBMemory db(dbFilePath.c_str());

    {
      Transaction trans(db);
      for (int i = 0; i < BlockContainer::MAX_ITEMS; i++) {
        BlockUnion* block = db.alloc_cow_block(5);
        BOOST_REQUIRE(block->trie.offset == offsets.back());
        BOOST_REQUIRE(block->trie.size == TrieBlock::SIZE);
        offsets.pop_back();
      }
    }
    auto pool_id = get_pool(TrieBlock::SIZE);
    offset_ptr c1_offset = db.get_active_head()->pools[pool_id].free;
    BOOST_REQUIRE(c1_offset == 0);
  }

  const char* test_points[] = {
      "DBMemory::alloc_block::1", "DBMemory::alloc_block::2",
      "DBMemory::alloc_block::3", "DBMemory::alloc_block::4",
      "DBMemory::free_block::1",  "DBMemory::free_block::2",
      "DBMemory::free_block::3",  NULL};
  check_testpoints(test_points);
}

BOOST_AUTO_TEST_CASE(test_get_cow_block) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  DBMemory::init(dbFilePath.c_str());
  DBMemory db(dbFilePath.c_str());

  const BlockUnion* root = db.get_root();
  BlockUnion* block;

  { 
    Transaction trans(db); 
    block = db.get_cow_block(0, root->block.offset);
    db.head.root = block->block.offset;
  }
  auto pool_id = get_pool(TrieBlock::SIZE);
  offset_ptr c_offset = db.get_active_head()->pools[pool_id].free;
  BlockContainer& c = db.get_block(c_offset)->container;
  BOOST_REQUIRE(c.count == 1);
  BOOST_REQUIRE(c.blocks[0].offset == root->block.offset);
  BOOST_REQUIRE(db.get_active_head()->root == block->block.offset);
}

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

  const ValueBlock& vb =  db.get_block(offset)->value;
  BOOST_CHECK_EQUAL_COLLECTIONS(value.begin(), value.end(), vb.data, vb.data + value.size());
}

/*
BOOST_AUTO_TEST_CASE(test_get_pool) {
  BOOST_REQUIRE_THROW(get_pool(4 * G+1), boost::execution_exception);
}*/

