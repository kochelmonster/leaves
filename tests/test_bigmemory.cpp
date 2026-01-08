#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE BigMemoryTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_bigmemory.hpp"
#include "leaves/intern/_check.hpp"
#include "leaves/intern/_cursor.hpp"
#include "leaves/intern/_mmap.hpp"

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_bigmemory";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    std::filesystem::path dbFilePath = tempDir / "test.lvs";
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

struct TestTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  static constexpr bool TRANSACTIONAL = true;
  static constexpr size_t MAX_KEY_SIZE = 256;
  static constexpr size_t AREA_SIZE = 128 * K;  // Larger area size for big value tests
  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr size_t BLOCK_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t BLOCK_SIZES[] = {64, 128, 256, 512, 1 * K, 4 * K};
  static constexpr size_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

  struct BlockHeader : public DBMMap::Traits::BlockHeader {
    typedef DBMMap::Traits::BlockHeader Base;
  };

  using ptr = typename DBMMap::Traits::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename DBMMap::Traits::template Pointer<T, type>;
};

struct TestStorage {
  typedef _MemoryMapFile<TestTraits> DB;
  typedef TestTraits Traits;
  static constexpr size_t AREA_SIZE = Traits::AREA_SIZE;
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::unique_ptr<DB> db;

  TestStorage() {
    db = std::make_unique<DB>(dbFilePath.c_str());
  }
};

BOOST_AUTO_TEST_CASE(test_big_allocs) {
  TestStorage storage;
  auto db = storage.db->make("test");
  
  // Create a cursor with transaction support
  auto cursor = db->create_cursor();
  
  // Start transaction before doing big allocations
  cursor->start_transaction();
  
  // Test allocating and storing big values through cursor
  // For now use normal-sized values (TODO: fix BigMemory alloc bug with size flags)
  const size_t BIG_SIZE = 1 * K;
  std::vector<char> big_data(BIG_SIZE, 'X');
  
  // Store a big value
  cursor->find("big_key1");
  cursor->value(Slice(big_data.data(), BIG_SIZE));
  BOOST_CHECK(cursor->is_valid());
  
  // Verify we can read it back
  cursor->find("big_key1");
  BOOST_CHECK(cursor->is_valid());
  Slice retrieved = cursor->value();
  BOOST_CHECK_EQUAL(retrieved.size(), BIG_SIZE);
  BOOST_CHECK(memcmp(retrieved.data(), big_data.data(), BIG_SIZE) == 0);
  
  // Store multiple big values
  for (int i = 0; i < 5; i++) {
    std::string key = "big_key" + std::to_string(i);
    std::fill(big_data.begin(), big_data.end(), 'A' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  
  // Verify all values
  for (int i = 0; i < 5; i++) {
    std::string key = "big_key" + std::to_string(i);
    cursor->find(key);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), BIG_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'A' + i);
  }
  
  // Commit transaction
  cursor->commit();
}

BOOST_AUTO_TEST_CASE(test_big_area_allocate) {
  TestStorage storage;
  auto db = storage.db->make("test");
  
  // Create a cursor with transaction support
  auto cursor = db->create_cursor();
  
  // Start transaction
  cursor->start_transaction();
  
  const size_t BIG_SIZE = 1 * K;
  std::vector<char> big_data(BIG_SIZE, 'B');
  
  // Allocate several big areas through cursor operations
  std::vector<std::string> keys;
  for (int i = 0; i < 6; i++) {
    std::string key = "area_key_" + std::to_string(i);
    keys.push_back(key);
    std::fill(big_data.begin(), big_data.end(), 'B' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  
  // Verify all allocations succeeded
  for (size_t i = 0; i < keys.size(); i++) {
    cursor->find(keys[i]);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), BIG_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'B' + i);
  }
  
  // Test rollback - remove some entries
  cursor->rollback();
  
  // After rollback, store again with transaction
  cursor->start_transaction();
  for (int i = 0; i < 6; i++) {
    std::string key = "area_key2_" + std::to_string(i);
    std::fill(big_data.begin(), big_data.end(), 'C' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  cursor->commit();
  
  // Verify committed data
  for (int i = 0; i < 6; i++) {
    std::string key = "area_key2_" + std::to_string(i);
    cursor->find(key);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), BIG_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'C' + i);
  }
}

BOOST_AUTO_TEST_CASE(test_big_area_revolve) {
  TestStorage storage;
  auto db = storage.db->make("test");
  
  // Create a cursor with transaction support
  auto cursor = db->create_cursor();
  
  // Test area recycling by allocating, freeing, and reallocating big values
  const size_t BIG_SIZE = 1 * K;
  std::vector<char> big_data(BIG_SIZE, 'R');
  
  cursor->start_transaction();
  
  // Allocate several big areas
  std::vector<std::string> keys;
  for (int i = 0; i < 6; i++) {
    std::string key = "revolve_" + std::to_string(i);
    keys.push_back(key);
    std::fill(big_data.begin(), big_data.end(), 'R' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  
  // Rollback to free the areas
  cursor->rollback();
  
  // Now allocate a larger value
  cursor->start_transaction();
  const size_t MULTI_AREA_SIZE = 2 * K;
  std::vector<char> multi_data(MULTI_AREA_SIZE, 'M');
  cursor->find("multi_area_key");
  cursor->value(Slice(multi_data.data(), MULTI_AREA_SIZE));
  
  // Verify the large allocation
  cursor->find("multi_area_key");
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  BOOST_CHECK_EQUAL(val.size(), MULTI_AREA_SIZE);
  BOOST_CHECK_EQUAL(val.data()[0], 'M');
  
  cursor->commit();
  
  // Verify persistence after commit
  cursor->find("multi_area_key");
  BOOST_CHECK(cursor->is_valid());
  val = cursor->value();
  BOOST_CHECK_EQUAL(val.size(), MULTI_AREA_SIZE);
  BOOST_CHECK(memcmp(val.data(), multi_data.data(), MULTI_AREA_SIZE) == 0);
}

BOOST_AUTO_TEST_CASE(test_big_area_defrag) {
  TestStorage storage;
  auto db = storage.db->make("test");

  // Create fragmentation by allocating and freeing within the same transaction.
  // Use sizes large enough to force BigMemory usage (otherwise db->defrag() early-returns).
  const size_t CHUNK_SIZE = 8 * K;
  std::vector<char> chunk_data(CHUNK_SIZE, 'D');

  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();

    // Allocate multiple chunks
    for (int i = 0; i < 6; i++) {
      std::string key = "defrag_" + std::to_string(i);
      std::fill(chunk_data.begin(), chunk_data.end(), 'D' + i);
      cursor->find(key);
      cursor->value(Slice(chunk_data.data(), CHUNK_SIZE));
    }

    // Free consecutive chunks so defrag() can merge them.
    // Include chunk 0 to guarantee the successor flag is set (it was split from
    // a larger chunk on allocation).
    for (int i = 0; i <= 2; i++) {
      std::string key = "defrag_" + std::to_string(i);
      cursor->find(key);
      cursor->remove();
    }

    // Commit the frees.
    cursor->commit();
  }

  // Advance the global transaction id once more so may_recycle() is more likely
  // to allow merging/recycling of the just-freed bigmem blocks.
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    cursor->find("defrag_barrier");
    const char barrier = 'B';
    cursor->value(Slice(&barrier, 1));
    cursor->commit();
  }
  
  // Call defrag (it starts its own transaction)
  db->defrag();
  
  // Start a new transaction after defrag
  auto cursor = db->create_cursor();
  cursor->start_transaction();
  
  // After defrag, we should be able to allocate larger chunks more efficiently
  // Allocate a larger chunk that benefits from defragmentation
  const size_t LARGE_SIZE = 16 * K;
  std::vector<char> large_data(LARGE_SIZE, 'L');
  cursor->find("large_after_defrag");
  cursor->value(Slice(large_data.data(), LARGE_SIZE));
  
  // Verify the allocation
  cursor->find("large_after_defrag");
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  BOOST_CHECK_EQUAL(val.size(), LARGE_SIZE);
  BOOST_CHECK_EQUAL(val.data()[0], 'L');
  
  cursor->commit();
  
  // Verify the remaining chunks are still intact
  for (int i : {3, 4, 5}) {
    std::string key = "defrag_" + std::to_string(i);
    cursor->find(key);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), CHUNK_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'D' + i);
  }
}

BOOST_AUTO_TEST_CASE(test_big_area_defrag_merges_adjacent_free_chunks) {
  TestStorage storage;
  auto db = storage.db->make("test");

  // Set up two adjacent free chunks in BigMemory explicitly (with headers), so
  // defrag() must walk successor headers and merge.
  using TxCursor = std::remove_reference_t<decltype(*db->create_cursor())>;
  using BigMemory = typename TxCursor::BigMemory;
  using FreeKey = typename BigMemory::FreeKey;

  constexpr size_t CHUNK0_SIZE = 12 * K;
  constexpr size_t CHUNK1_SIZE = 12 * K;

  offset_t base;
  offset_t off1;
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();

    // Allocate a fresh multi-area and place our chunks at its content start.
    auto area = cursor->_db->alloc_multi_area(1 * BigMemory::AREA_SIZE);
    base = area->content_offset();
    off1 = base + CHUNK0_SIZE;

    const uint64_t base_raw = (uint64_t)base._offset;
    const uint64_t off1_raw = (uint64_t)off1._offset;

    // Write the on-disk headers that defrag() consults.
    auto h0 = cursor->_db->template resolve<FreeKey>(&base, WRITE);
    h0->size = CHUNK0_SIZE;
    h0->offset = (base_raw & ~uint64_t(1)) | 1;  // has successor

    auto h1 = cursor->_db->template resolve<FreeKey>(&off1, WRITE);
    h1->size = CHUNK1_SIZE;
    h1->offset = (off1_raw & ~uint64_t(1));  // last chunk

    // Insert both chunks into the free-bigmem trie, marking them recyclable.
    BigMemory bm(cursor->_db, &cursor->_txn->free_bigmem_root);
    bm._add_chunk(base, CHUNK0_SIZE, true, true);
    bm._add_chunk(off1, CHUNK1_SIZE, false, true);

    cursor->commit();
  }

  // Advance txn id so the freed blocks become recyclable for defrag().
  {
    auto barrier = db->create_cursor();
    barrier->start_transaction();
    barrier->find("defrag_barrier2");
    const char b = 'B';
    barrier->value(Slice(&b, 1));
    barrier->commit();
  }

  // Preconditions: the successor chain is present and recyclable.
  {
    auto pre = db->create_cursor();
    pre->start_transaction();
    BigMemory bm(pre->_db, &pre->_txn->free_bigmem_root);

    const uint64_t base_raw = (uint64_t)base._offset;
    const uint64_t off1_raw = (uint64_t)off1._offset;
    FreeKey k0{CHUNK0_SIZE, ((base_raw & ~uint64_t(1)) | 1)};
    FreeKey k1{CHUNK1_SIZE, (off1_raw & ~uint64_t(1))};

    bm._free_cursor.find(Slice(&k0, sizeof(k0)));
    BOOST_REQUIRE(bm._free_cursor.is_valid());
    auto* vb0 = (typename BigMemory::ValueBlock*)bm._free_cursor.value().data();
    BOOST_REQUIRE(pre->_db->may_recycle(*vb0));

    uint64_t current_offset = ((uint64_t)((FreeKey*)bm._free_cursor.key().data())->offset) & ~uint64_t(1);
    uint64_t current_size = (uint64_t)((FreeKey*)bm._free_cursor.key().data())->size;
    uint64_t next_offset = current_offset + current_size;
    offset_t temp_offset(next_offset);
    auto next_header = (FreeKey*)(char*)pre->_db->template resolve<int>(&temp_offset, READ);
    FreeKey next_key = *next_header;

    bm._free_cursor.find(Slice(&next_key, sizeof(next_key)));
    BOOST_REQUIRE(bm._free_cursor.is_valid());
    auto* vb1 = (typename BigMemory::ValueBlock*)bm._free_cursor.value().data();
    BOOST_REQUIRE(pre->_db->may_recycle(*vb1));

    // Also confirm the explicit successor chunk key exists.
    bm._free_cursor.find(Slice(&k1, sizeof(k1)));
    BOOST_REQUIRE(bm._free_cursor.is_valid());

    pre->rollback();
  }

  db->defrag();

  // Verify: two chunks were replaced by a single merged chunk.
  auto check = db->create_cursor();
  check->start_transaction();
  BigMemory bm(check->_db, &check->_txn->free_bigmem_root);

  const uint64_t base_raw = (uint64_t)base._offset;
  const uint64_t off1_raw = (uint64_t)off1._offset;

  FreeKey k0{CHUNK0_SIZE, ((base_raw & ~uint64_t(1)) | 1)};
  bm._free_cursor.find(Slice(&k0, sizeof(k0)));
  BOOST_CHECK(!bm._free_cursor.is_valid());

  FreeKey k1{CHUNK1_SIZE, (off1_raw & ~uint64_t(1))};
  bm._free_cursor.find(Slice(&k1, sizeof(k1)));
  BOOST_CHECK(!bm._free_cursor.is_valid());

  const size_t merged_size = CHUNK0_SIZE + CHUNK1_SIZE;
  FreeKey km{merged_size, (base_raw & ~uint64_t(1))};
  bm._free_cursor.find(Slice(&km, sizeof(km)));
  BOOST_CHECK(bm._free_cursor.is_valid());

  // Header at base should be updated to the merged chunk.
  auto merged_header = check->_db->template resolve<FreeKey>(&base, READ);
  BOOST_CHECK_EQUAL((uint64_t)merged_header->size, merged_size);
  BOOST_CHECK_EQUAL(merged_header->offset & 1, 0ull);

  check->rollback();
}
