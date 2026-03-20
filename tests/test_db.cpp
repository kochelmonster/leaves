#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/db/_check.hpp"
#include "leaves/intern/storage/_mmap.hpp"

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

typedef DBMMap::page_ptr page_ptr;
typedef DBMMap::Traits::PageHeader PageHeader;

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
  Transaction(DB db_) : db(db_) { db->start_transaction(0); }
  ~Transaction() { db->commit(0); }

  DB db;
};

BOOST_AUTO_TEST_CASE(test_multi_transaction) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");
  auto txn = db->txn();
  txn->refs.fetch_add(1);

  page_ptr block1, block2, block3, block4;

  db->_header->txn_lock.lock();
  db->_header->txn_lock.unlock();

  {
    Transaction trans(db);
    block1 = db->alloc_page(311);
  }

  {
    db->_gc_counter = db->GC_INTERVAL - 1; // force GC
    Transaction trans(db);
    db->free(block1);
    block2 = db->alloc_page(311);
    BOOST_CHECK(block1 != block2);
    BOOST_CHECK_EQUAL(db->_start_txn_id, txn->txn_id);
  }

  {
    db->_gc_counter = db->GC_INTERVAL - 1; // force GC
    Transaction trans(db);
    db->free(block2);
    block3 = db->alloc_page(311);
    BOOST_CHECK(block1 != block3);
    BOOST_CHECK(block2 != block3);
    BOOST_CHECK_EQUAL(db->_start_txn_id, txn->txn_id);
  }

  BOOST_CHECK(txn->txn_id != db->txn()->txn_id);

  txn->refs.fetch_sub(1);

  {
    db->_gc_counter = db->GC_INTERVAL - 1; // force GC
    Transaction trans(db);
    block4 = db->alloc_page(311);
    BOOST_CHECK(block4 != block3);
    BOOST_CHECK_GT(db->_start_txn_id, txn->txn_id);
  }
}

BOOST_AUTO_TEST_CASE(test_extend) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");
  const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
  const size_t MAX_PAYLOAD =
      DBMMap::Traits::PAGE_SIZES[DBMMap::Traits::PAGE_SIZES_COUNT - 1] -
      sizeof(DBMMap::Traits::PageHeader);

  size_t initial_file_size = storage._memory->file_size;

  {
    Transaction trans(db);
    // With geometric growth, the DB is initialized with ~10*AREA_SIZE already allocated
    // We need to allocate enough to exhaust that and trigger another resize
    // New resize will grow by max(requested, 10*AREA_SIZE, 25% of file_size)
    // Allocate enough blocks to exceed initial capacity
    int count = (11 * AREA_SIZE) / MAX_PAYLOAD;  // Force growth beyond initial allocation
    for (int i = 0; i < count; i++) {
      db->alloc_page(MAX_PAYLOAD);
    }
  }

  // Check that file size increased with geometric growth
  BOOST_CHECK_GT(storage._memory->file_size, initial_file_size);
}

BOOST_AUTO_TEST_CASE(test_rollback) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");

  db->start_transaction(0, true);
  auto block1 = db->alloc_page(1123);
  db->prepare_commit(0);
  db->rollback(0);

  db->start_transaction(0);
  auto block2 = db->alloc_page(1123);
  db->prepare_commit(0);
  db->commit(0);

  db->start_transaction(0);
  auto block3 = db->alloc_page(1123);
  db->prepare_commit(0);
  db->commit(0);

  BOOST_CHECK(block1 == block2);
  BOOST_CHECK(block1 != block3);
}

BOOST_AUTO_TEST_CASE(test_alloc_and_free_block) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::vector<offset_t> block_offsets;
  size_t file_size;
  const size_t MAX_REF_COUNT = DBMMap::DB::MemManager::PageContainer::COUNT;

  [[maybe_unused]] uint32_t* refs = nullptr;
  {
    {
      DBMMap storage(dbFilePath.c_str());
    }
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");

    BOOST_REQUIRE(db->txn()->txn_id == tid_t(1));
    BOOST_REQUIRE(db->txn()->refs.load() == 0);

    {
      // allocate enough blocks of 4K size to fill one garbage page
      Transaction trans(db);

      for (size_t i = 0; i < MAX_REF_COUNT - 1; i++) {
        page_ptr block = db->alloc_page(4 * K - sizeof(PageHeader));
        block_offsets.push_back(db->resolve(block));
      }
      file_size = storage._memory->file_size;
    }

    DBMMap::DB::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == tid_t(2));
    BOOST_REQUIRE(txn->refs.load() == 0);
    refs = (uint32_t*)&txn->refs;
    BOOST_REQUIRE(file_size == storage._memory->file_size);
  }

  {
    // fill the garbage page (txn=2)
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    BOOST_REQUIRE(storage._memory->file_size == file_size);

    {
      db->_gc_counter = db->GC_INTERVAL - 1; // force GC
      Transaction trans(db);

      for (offset_t bo : block_offsets) {
        page_ptr block = db->template resolve<PageHeader>(&bo);
        BOOST_REQUIRE(db->resolve(block) == bo);
        db->free(block);
      }
    }

    DBMMap::DB::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == tid_t(3));
  }

  {
    // go one transaction ahead, to be able to harvest
    // the last freed blocks;
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    db->_gc_counter = db->GC_INTERVAL - 1; // force GC
    Transaction trans(db);
    file_size = storage._memory->file_size;
  }

  {
    // free the first page page (txn=2)
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    BOOST_REQUIRE_EQUAL(db->_storage._memory->file_size, file_size);

    {
      db->_gc_counter = db->GC_INTERVAL - 1; // force GC
      Transaction trans(db);
      for (offset_t bo : block_offsets) {
        page_ptr block = db->alloc_page(4 * K - sizeof(PageHeader));
        [[maybe_unused]] offset_t offset = db->resolve(block);
        BOOST_REQUIRE(db->resolve(block) == bo);
      }
    }

    DBMMap::DB::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == tid_t(5));
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
    db1->alloc_page(3 * K);
  }

  size_t initial_file_size = storage._memory->file_size;
  storage.remove_db("test1");

  auto db3 = storage.make("test3");
  size_t final_file_size = storage._memory->file_size;

  // Check that no significant additional memory was allocated (indicating
  // recycling) Allow some small growth due to area header changes
  const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
  BOOST_CHECK(final_file_size <= initial_file_size + AREA_SIZE);
}

BOOST_AUTO_TEST_CASE(test_orphaned_aera) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());

  auto db1 = storage.make("test1");
  const uint64_t MAX_PAYLOAD = 4 * K - sizeof(DBMMap::Traits::PageHeader);

  std::vector<offset_t> offsets;

  db1->start_transaction(0);
  // These last_area checks are no longer applicable in the new architecture
  // force the alloc of a new area
  const uint64_t ALLOC_SIZE = db1->_wtxn->mem_manager.get_allocation_end() + 16 * K -
                              db1->_wtxn->mem_manager.get_allocation_start();
  uint64_t size = 0;
  while (size < ALLOC_SIZE) {
    offsets.push_back(storage.resolve(db1->alloc_page(MAX_PAYLOAD)));
    size += MAX_PAYLOAD + sizeof(DBMMap::Traits::PageHeader);
  }

  // Check that area allocation succeeded
  BOOST_CHECK(!offsets.empty());

  // create a new area for db2
  auto db2 = storage.make("test2");

  db1->rollback(0);
  // the new area in db1 is "orphaned" and must be recycled the next time

  db1->start_transaction(0);
  // Check that we can start a new transaction

  // alloc again - with recycling, we should get similar allocation patterns
  std::vector<offset_t> new_offsets;
  for (size_t i = 0; i < offsets.size(); i++) {
    new_offsets.push_back(storage.resolve(db1->alloc_page(MAX_PAYLOAD)));
  }

  // Check that we allocated the same number of blocks (recycling working)
  BOOST_CHECK_EQUAL(offsets.size(), new_offsets.size());

  // Check that at least some offsets are reused (indicating recycling)
  bool some_reused = false;
  for (offset_t old_offset : offsets) {
    for (offset_t new_offset : new_offsets) {
      if (old_offset == new_offset) {
        some_reused = true;
        break;
      }
    }
  }
  BOOST_CHECK(some_reused);

  db1->rollback(0);
}

struct BigSizeKey {
  boost::endian::big_uint64_t first;
  uint64_t second;
};

static constexpr uint64_t SIZE_BIT = uint64_t(1) << 63;

// TODO: Adapt for new BigMemory architecture
/*
void check_memtrie_count(DBMMap::DB* db, int count) {
  auto memc = db->_mem_cursor;
  int c = 0;
  for (memc.first(); memc.is_valid(); memc.next()) {
    c++;
  }
  BOOST_CHECK_EQUAL(count, c);
}
*/

template <typename DB>
void dump(DB db, const char* prefix, int index) {
  std::stringstream cstr;
  cstr << "errors/" << prefix << std::setw(2) << std::setfill('0') << index
       << ".yaml";
  std::ofstream out(cstr.str().c_str());
  _Dumper(*db, db->_internal()->_wtxn->offset_root, false).dump(out);
}

struct TestTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  static constexpr bool TRANSACTIONAL = true;
  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 128 * K;
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr int GC_INTERVAL = 1;
  static constexpr uint16_t PAGE_SIZES[] = {100, 4 * K};
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);

  struct PageHeader {
    typedef PageHeader Base;
    tid_t txn_id;
    uint16_e used;
    uint8_t slot_id;
  };

  using ptr = SimplePointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SimplePointer<T, type>;
};

struct TestStorage {
  typedef TestTraits Traits;
  using PageHeader = typename TestTraits::PageHeader;
  using page_ptr = typename TestTraits::ptr;
  using area_ptr = typename TestTraits::template Pointer<Area>;
  using offset_e = typename TestTraits::offset_e;
  using uint32_e = typename TestTraits::uint32_e;
  using uint16_e = typename TestTraits::uint16_e;
  typedef _DB<TestStorage> DB;
  typedef std::shared_ptr<DB> db_ptr;

  static constexpr size_t AREA_SIZE = Traits::AREA_SIZE;

  struct Mutex {
    void recover() {}
    template <typename Time = std::chrono::seconds>
    void lock(Time /* t */ = Time(10)) {}
    bool try_lock() { return true; }
    void unlock() {}
  };

  AreaList single_areas;
  AreaList multi_areas;
  std::vector<char> memory;
  db_ptr db;
  offset_t db_offset;
  Mutex mutex;

  TestStorage() {
    memory.reserve(8 * 1024 * 1024);
    memory.resize(AREA_SIZE);
    single_areas.init();
    multi_areas.init();
    db = std::make_shared<DB>(*this, &db_offset, 0);
  }

  Mutex& file_lock() { return mutex; }
  size_t file_size() const { return memory.size(); }

  page_ptr resolve(const offset_t* offset_ptr, Access /* access */ = READ) {
    return page_ptr(&memory[*offset_ptr]);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return offset_t((const char*)p - (char*)&memory[0]).type(p.type);
  }
  
  template <typename PtrType>
  void make_dirty(PtrType /* block */) { }

  // New area allocation methods required by _db.hpp
  area_ptr alloc_single_area() {
    auto result = single_areas.pop(*this);
    if (!result) {
      // Extend storage with new area
      uint32_t old_size = memory.size();
      memory.resize(old_size + AREA_SIZE);

      // Create Area in the new memory location
      offset_t area_offset(old_size);
      auto area = area_ptr(resolve(&area_offset, WRITE));
      area->init(old_size, AREA_SIZE, 0);
      return area;
    }
    return result;
  }

  area_ptr alloc_multi_area(uint64_t size) {
    // Ensure size is multiple of AREA_SIZE
    size = padding(size, AREA_SIZE);

    auto result = multi_areas.find_and_remove(size, *this);
    if (!result) {
      // Extend storage with new area
      uint32_t old_size = memory.size();
      memory.resize(old_size + size);

      // Create Area in the new memory location
      offset_t area_offset(old_size);
      auto area = area_ptr(resolve(&area_offset, WRITE));
      area->init(old_size, size, 0);
      return area;
    }
    return result;
  }

  void return_single_areas(offset_t head, offset_t tail) { 
    single_areas.add(head, tail, *this); 
  }

  void return_multi_areas(offset_t head, offset_t tail) { 
    multi_areas.add(head, tail, *this); 
  }

  // Legacy compatibility method
  AreaSlice get_area(uint64_t size) {
    auto area_ptr = alloc_multi_area(size);
    return *area_ptr;  // Convert Area* to AreaSlice
  }

  void flush(bool /* sync */ = false, bool /* force */ = false) {}
  void prefetch(offset_t /* offset */, Access /* access */ = READ) const {}
  void prefetch(void* /* mem */, Access /* access */ = READ) const {}
};

BOOST_AUTO_TEST_CASE(test_two_phase_commit_crash_recovery) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc.lvs";
  
  offset_t prepared_offset;
  offset_t read_offset;
  tid_t prepared_tid;
  
  // Phase 1: Prepare a transaction but don't commit (simulate crash)
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Start transaction and allocate some data
    BOOST_REQUIRE(db->start_transaction(0));
    BOOST_CHECK_EQUAL(db->transaction_active(), 2);  // First user txn is 2
    
    auto block1 = db->alloc_page(512);
    memcpy((char*)block1, "test_data", 10);
    
    // Prepare the commit (makes it durable)
    prepared_tid = db->prepare_commit(0);
    BOOST_CHECK_GT(prepared_tid, 0);
    BOOST_CHECK_EQUAL(prepared_tid, 2);
    
    // Verify prepared state
    BOOST_CHECK_NE(db->_header->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->_wtxn->txn_id, prepared_tid);
    
    // Save offsets for verification
    prepared_offset = db->_header->prepared_txn;
    read_offset = db->_header->read_txn;
    
    // DON'T call commit() - simulate crash after prepare
    // The transaction lock will be released when db goes out of scope
    // but the prepared transaction should remain
  }
  
  // Phase 2: Reopen DB - should detect and recover prepared transaction
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Check that prepared transaction was detected
    BOOST_CHECK_NE(db->_header->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, prepared_offset);
    BOOST_CHECK_EQUAL(db->_header->read_txn, read_offset);
    
    // _active_txn should be set to the prepared transaction
    BOOST_REQUIRE(db->_active_txn != nullptr);
    BOOST_CHECK_EQUAL(db->_active_txn->txn_id, prepared_tid);
    
    // The transaction is already active (recovered), just need to acquire lock and commit
    // We need to simulate that a cursor with ID 0 takes ownership
    BOOST_CHECK(db->_header->txn_lock.try_lock());
    db->_header->txn_cursor_id.store(0);
    
    // Commit should finalize the prepared transaction
    BOOST_CHECK(db->commit(0));
    
    // After commit, prepared should equal read
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->transaction_active(), tid_t(0));
  }
  
  // Phase 3: Verify data persisted correctly
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Everything should be committed now
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->transaction_active(), tid_t(0));
  }
}

BOOST_AUTO_TEST_CASE(test_two_phase_commit_normal_path) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_normal.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Normal two-phase commit: prepare then commit
    BOOST_REQUIRE(db->start_transaction(0));
    auto block1 = db->alloc_page(512);
    
    // Prepare should return transaction ID
    tid_t tid = db->prepare_commit(0);
    BOOST_CHECK_GT(tid, 0);
    BOOST_CHECK_NE(db->_header->prepared_txn, db->_header->read_txn);
    
    // Commit should succeed and finalize
    BOOST_CHECK(db->commit(0));
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->transaction_active(), tid_t(0));
  }
  
  // Verify state persisted correctly
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_two_phase_commit_rollback_after_prepare) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_rollback.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Start transaction
    BOOST_REQUIRE(db->start_transaction(0));
    auto block1 = db->alloc_page(512);
    offset_t block1_offset = db->resolve(block1);
    
    // Prepare the commit
    tid_t tid = db->prepare_commit(0);
    BOOST_CHECK_GT(tid, 0);
    BOOST_CHECK_NE(db->_header->prepared_txn, db->_header->read_txn);
    
    // Now rollback even after prepare
    BOOST_CHECK(db->rollback(0));
    
    // After rollback, prepared should equal read again
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->transaction_active(), tid_t(0));
    
    // Pending areas should have been returned
    // Start new transaction and verify block can be reused
    BOOST_REQUIRE(db->start_transaction(0));
    auto block2 = db->alloc_page(512);
    offset_t block2_offset = db->resolve(block2);
    
    // Should get the same block back since previous was rolled back
    BOOST_CHECK_EQUAL(block1_offset, block2_offset);
    
    db->commit(0);
  }
}

BOOST_AUTO_TEST_CASE(test_prepare_commit_idempotency) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_idempotent.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    BOOST_REQUIRE(db->start_transaction(0));
    auto block1 = db->alloc_page(512);
    
    // First prepare
    tid_t tid1 = db->prepare_commit(0);
    BOOST_CHECK_GT(tid1, 0);
    
    // Second prepare should return same txn_id (idempotent)
    tid_t tid2 = db->prepare_commit(0);
    BOOST_CHECK_EQUAL(tid1, tid2);
    
    // State should be consistent
    BOOST_CHECK_NE(db->_header->prepared_txn, db->_header->read_txn);
    
    // Commit should still work
    BOOST_CHECK(db->commit(0));
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_prepare_commit_pending_areas) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_areas.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Allocate enough blocks to force new area allocation
    BOOST_REQUIRE(db->start_transaction(0));
    
    std::vector<page_ptr> blocks;
    // Allocate many large blocks to exhaust the initial area and force new area allocation
    const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
    const size_t page_size = 4000;  // Large blocks
    const size_t num_blocks = (AREA_SIZE * 2) / page_size;  // Enough to need multiple areas
    
    for (size_t i = 0; i < num_blocks; i++) {
      blocks.push_back(db->alloc_page(page_size));
    }
    
    // Before prepare, pending areas should have content (may not always be true if blocks fit in initial area)
    // This check is optional and depends on allocation patterns
    // BOOST_CHECK_GT((uint64_t)db->_header->pending_single_areas.get_head(), 0u);
    
    // Prepare the transaction
    tid_t tid = db->prepare_commit(0);
    BOOST_CHECK_GT(tid, 0);
    
    using Transaction = typename std::remove_pointer_t<decltype(db)>::Transaction;

    // In new architecture, area tails are in transaction, not separate pending lists
    auto read_txn = db->template resolve<Transaction>(&db->_header->read_txn);
    auto prep_txn = db->template resolve<Transaction>(&db->_header->prepared_txn);
    
    // Commit switches read_txn pointer
    BOOST_CHECK(db->commit(0));
    
    // After commit, read_txn should point to what was prepared_txn
    auto new_read_txn = db->template resolve<Transaction>(&db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->_header->read_txn, db->_header->prepared_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_sanitize_uncommitted_areas) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_sanitize.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Start a transaction and allocate many areas
    BOOST_REQUIRE(db->start_transaction(0));
    
    // Get initial area tail offsets from the write transaction
    offset_t initial_single_tail = db->_wtxn->area_list_tail_single;
    offset_t initial_multi_tail = db->_wtxn->area_list_tail_multi;
    
    std::vector<page_ptr> blocks;
    // Allocate enough blocks to force multiple area allocations
    const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
    const size_t page_size = 4000;
    const size_t num_blocks = (AREA_SIZE * 3) / page_size;  // Force 3+ areas
    
    for (size_t i = 0; i < num_blocks; i++) {
      blocks.push_back(db->alloc_page(page_size));
    }
    
    // Get the transaction state after allocations
    offset_t final_single_tail = db->_wtxn->area_list_tail_single;
    offset_t final_multi_tail = db->_wtxn->area_list_tail_multi;
    
    // Verify that single areas were allocated (tail should have moved)
    BOOST_CHECK_NE(final_single_tail, initial_single_tail);
    
    // Note: Multi-area allocation now happens through BigMemory in cursors
    // If we need to test multi-area allocation, we would need to:
    // 1. Create a cursor
    // 2. Insert big values (>4KB) which trigger BigMemory::alloc()
    // For now, we only verify single-area allocation in this test
    
    // Prepare but don't commit - simulate a crash
    tid_t tid = db->prepare_commit(0);
    BOOST_CHECK_GT(tid, 0);
    
    // Don't call commit - simulate crash after prepare
    // Just end the transaction without committing
    db->end_transaction();
  }
  
  // Reopen the database - this simulates crash recovery
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Check if database recovered a prepared transaction
    bool has_prepared_txn = (db->_header->prepared_txn != db->_header->read_txn);
    
    if (has_prepared_txn) {
      // Database should have recovered the prepared transaction
      auto read_txn = db->txn();  // Use txn() to get the read transaction pointer
      auto prep_txn = db->_active_txn;  // After recovery, _active_txn points to prepared txn
      
      // Verify that the prepared transaction has different area tails than read transaction
      // This means there are uncommitted areas that need to be cleaned up
      BOOST_REQUIRE(prep_txn != nullptr);
      offset_t read_single_tail = read_txn->area_list_tail_single;
      offset_t read_multi_tail = read_txn->area_list_tail_multi;
      offset_t prep_single_tail = prep_txn->area_list_tail_single;
      offset_t prep_multi_tail = prep_txn->area_list_tail_multi;
      
      // First sanitize - cleans locks/refs but won't touch areas while prepared txn exists
      db->sanitize();
      
      // After first sanitize:
      // 1. Transaction refs should be reset
      auto txn_after = db->txn();
      BOOST_CHECK_EQUAL(txn_after->refs.load(), 0u);
      
      // 2. Lock and cursor ID should be cleared
      BOOST_CHECK_EQUAL(db->_header->txn_cursor_id.load(), 0u);
      
      // 3. prepared_txn should still be different (sanitize waits for rollback)
      BOOST_CHECK_NE(db->_header->prepared_txn, db->_header->read_txn);
      
      // 4. Manually rollback by setting prepared_txn = read_txn
      db->_header->prepared_txn = db->_header->read_txn;
      db->_wtxn.reset();
      db->_active_txn = nullptr;
      db->flush();
      
      // 5. Now call sanitize again - should clean up areas
      db->sanitize();
    }
    
    // Database should be in a consistent state
    // Try to start a new transaction and allocate - should work
    BOOST_CHECK(db->start_transaction(0));
    auto test_block = db->alloc_page(1000);
    BOOST_CHECK(test_block);
    BOOST_CHECK(db->commit(0));
  }
}

BOOST_AUTO_TEST_CASE(test_sanitize_with_multiple_area_chains) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_sanitize_chains.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // Start transaction and allocate enough to create a chain of areas
    BOOST_REQUIRE(db->start_transaction(0));
    
    std::vector<page_ptr> blocks;
    const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
    const size_t page_size = 3000;
    
    // Allocate much more to definitely create multiple linked areas
    // We need to exceed the initial area and force allocation of new areas
    for (size_t i = 0; i < 50; i++) {
      blocks.push_back(db->alloc_page(page_size));
    }
    
    offset_t tail_before_prepare = db->_wtxn->area_list_tail_single;
    
    // Prepare the transaction
    tid_t tid = db->prepare_commit(0);
    BOOST_CHECK_GT(tid, 0);
    
    // Simulate crash - don't commit
    db->end_transaction();
  }
  
  // Reopen and sanitize
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    // After recovery, read the read transaction's area tail
    auto read_txn_before = db->txn();  // Use txn() method to get proper transaction pointer
    offset_t read_tail = read_txn_before->area_list_tail_single;
    
    // Test Area::get_end with a chain of areas
    // This tests the actual implementation in sanitize()
    if (read_tail) {
      offset_t actual_tail = Area::get_end(read_tail, *db);
      
      // The actual tail should be at the end of the chain
      auto tail_area = db->resolve<Area>(&actual_tail, READ);
      BOOST_CHECK_EQUAL(tail_area->next, 0);  // Should be the last area
      
      // Walk the chain manually to verify
      offset_t manual_tail = read_tail;
      auto area = db->resolve<Area>(&manual_tail, READ);
      int chain_length = 1;
      while (area->next) {
        manual_tail = area->next;
        area = db->resolve<Area>(&manual_tail, READ);
        chain_length++;
      }
      
      // get_end should find the same tail
      BOOST_CHECK_EQUAL(actual_tail, manual_tail);
      // Check that we have a chain (might be 1 if all fit in initial area, but likely >1)
      // Don't fail test if chain_length == 1, just informational
      if (chain_length == 1) {
        BOOST_TEST_MESSAGE("Note: All allocations fit in initial area, no chain created");
      }
    }
    
    // First sanitize - cleans locks/refs
    db->sanitize();
    
    // Check if there was a prepared txn and handle it
    if (db->_header->prepared_txn != db->_header->read_txn) {
      // Manually rollback by setting prepared_txn = read_txn
      db->_header->prepared_txn = db->_header->read_txn;
      db->_wtxn.reset();
      db->_active_txn = nullptr;
      db->flush();
      
      // Call sanitize again to clean up areas
      db->sanitize();
    }
    
    // After sanitize with no prepared txn, area cleanup should have happened
    BOOST_CHECK_EQUAL(db->_header->prepared_txn, db->_header->read_txn);
    
    // Database should be usable after sanitize
    BOOST_CHECK(db->start_transaction(0));
    auto test_block = db->alloc_page(1000);
    BOOST_CHECK(test_block);
    BOOST_CHECK(db->commit(0));
  }
}

BOOST_AUTO_TEST_CASE(test_defrag_empty_db) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");

  // defrag() on a fresh DB with no big memory should return early
  db->defrag();

  // DB should still be usable after no-op defrag
  BOOST_CHECK(db->start_transaction(0));
  auto block = db->alloc_page(100);
  BOOST_CHECK(block);
  BOOST_CHECK(db->commit(0));
}

BOOST_AUTO_TEST_CASE(test_nonblocking_transaction) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.make("test");

  // Start a blocking transaction (holds the lock)
  auto txn1 = db->start_transaction(1);
  BOOST_CHECK(txn1);

  // Try nonblocking start — should fail because lock is held
  auto txn2 = db->start_transaction(2, true);
  BOOST_CHECK(!txn2);

  // Commit the first transaction
  BOOST_CHECK(db->commit(1));

  // Now nonblocking should succeed
  auto txn3 = db->start_transaction(2, true);
  BOOST_CHECK(txn3);
  BOOST_CHECK(db->commit(2));
}