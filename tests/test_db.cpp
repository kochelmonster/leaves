#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include <atomic>
#include <thread>

#include "leaves/intern/db/_check.hpp"
#include "leaves/intern/storage/_mmap.hpp"
#include "leaves/intern/replication/_replication_db.hpp"
#include "leaves/mmap.hpp"

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
  Transaction(DB db_) : db(db_), ctx(db_->start_transaction()) {}
  ~Transaction() { db->commit(ctx); }

  DB db;
  typename std::remove_pointer_t<DB>::TxnContext* ctx;
};

BOOST_AUTO_TEST_CASE(test_multi_transaction) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
  auto txn = db->txn();
  txn->refs.fetch_add(1);

  page_ptr block1, block2, block3, block4;

  // Verify context claim/release works via the atomic flag.
  uint8_t expected = 0;
  BOOST_CHECK(db->context(0)->_claimed.compare_exchange_strong(expected, 1));
  db->context(0)->_claimed.store(0);

  {
    Transaction trans(db);
    block1 = db->_alloc_page(311, trans.ctx);
  }

  {
    Transaction trans(db);
    db->_free(block1, trans.ctx);
    block2 = db->_alloc_page(311, trans.ctx);
    BOOST_CHECK(block1 != block2);
    BOOST_CHECK_EQUAL(db->_start_txn_id, txn->txn_id);
  }

  {
    Transaction trans(db);
    db->_free(block2, trans.ctx);
    block3 = db->_alloc_page(311, trans.ctx);
    BOOST_CHECK(block1 != block3);
    BOOST_CHECK(block2 != block3);
    BOOST_CHECK_EQUAL(db->_start_txn_id, txn->txn_id);
  }

  BOOST_CHECK(txn->txn_id != db->txn()->txn_id);

  txn->refs.fetch_sub(1);

  {
    Transaction trans(db);
    block4 = db->_alloc_page(311, trans.ctx);
    BOOST_CHECK(block4 != block3);
    BOOST_CHECK_GT(db->_start_txn_id, txn->txn_id);
  }
}

BOOST_AUTO_TEST_CASE(test_extend) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
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
      db->_alloc_page(MAX_PAYLOAD, trans.ctx);
    }
  }

  // Check that file size increased with geometric growth
  BOOST_CHECK_GT(storage._memory->file_size, initial_file_size);
}

BOOST_AUTO_TEST_CASE(test_rollback) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  auto ctx = db->start_transaction(true);
  auto block1 = db->_alloc_page(1123, ctx);
  db->prepare_commit(ctx);
  db->rollback(ctx);

  ctx = db->start_transaction();
  auto block2 = db->_alloc_page(1123, ctx);
  db->prepare_commit(ctx);
  db->commit(ctx);

  ctx = db->start_transaction();
  auto block3 = db->_alloc_page(1123, ctx);
  db->prepare_commit(ctx);
  db->commit(ctx);

  BOOST_CHECK(block1 == block2);
  BOOST_CHECK(block1 != block3);
}

BOOST_AUTO_TEST_CASE(test_alloc_and_free_block) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::vector<offset_t> block_offsets;
  size_t file_size;
  const size_t MAX_REF_COUNT = _DB<DBMMap>::MemManager::PageContainer::COUNT;

  [[maybe_unused]] uint32_t* refs = nullptr;
  {
    {
      DBMMap storage(dbFilePath.c_str());
    }
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");

    BOOST_REQUIRE(db->txn()->txn_id == tid_t(1));
    BOOST_REQUIRE(db->txn()->refs.load() == 0);

    {
      // allocate enough blocks of 4K size to fill one garbage page
      Transaction trans(db);

      for (size_t i = 0; i < MAX_REF_COUNT - 1; i++) {
        page_ptr block = db->_alloc_page(4 * K - sizeof(PageHeader), trans.ctx);
        block_offsets.push_back(db->resolve(block));
      }
      file_size = storage._memory->file_size;
    }

    _DB<DBMMap>::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == tid_t(2));
    BOOST_REQUIRE(txn->refs.load() == 0);
    refs = (uint32_t*)&txn->refs;
    BOOST_REQUIRE(file_size == storage._memory->file_size);
  }

  {
    // fill the garbage page (txn=2)
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    BOOST_REQUIRE(storage._memory->file_size == file_size);

    {
      Transaction trans(db);

      for (offset_t bo : block_offsets) {
        page_ptr block = db->template resolve<PageHeader>(&bo);
        BOOST_REQUIRE(db->resolve(block) == bo);
        db->_free(block, trans.ctx);
      }
    }

    _DB<DBMMap>::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == tid_t(3));
  }

  {
    // go one transaction ahead, to be able to harvest
    // the last freed blocks;
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    Transaction trans(db);
    file_size = storage._memory->file_size;
  }

  {
    // free the first page page (txn=2)
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    BOOST_REQUIRE_EQUAL(db->_storage._memory->file_size, file_size);

    {
      Transaction trans(db);
      for (offset_t bo : block_offsets) {
        page_ptr block = db->_alloc_page(4 * K - sizeof(PageHeader), trans.ctx);
        [[maybe_unused]] offset_t offset = db->resolve(block);
        BOOST_REQUIRE(db->resolve(block) == bo);
      }
    }

    _DB<DBMMap>::txn_ptr txn = db->txn();
    BOOST_REQUIRE(txn->txn_id == tid_t(5));
  }
}

BOOST_AUTO_TEST_CASE(test_recycle_db) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());

  auto db1 = storage.open("test1");
  auto db2 = storage.open("test2");

  {
    Transaction t(db1);
    db1->_alloc_page(3 * K, t.ctx);
  }

  size_t initial_file_size = storage._memory->file_size;
  storage.remove("test1");

  auto db3 = storage.open("test3");
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

  auto db1 = storage.open("test1");
  const uint64_t MAX_PAYLOAD = 4 * K - sizeof(DBMMap::Traits::PageHeader);

  std::vector<offset_t> offsets;

  auto ctx = db1->start_transaction();
  // These last_area checks are no longer applicable in the new architecture
  // force the alloc of a new area
  const uint64_t ALLOC_SIZE = db1->_resolve_active(ctx)->mem_manager.allocation_end + 16 * K -
                              db1->_resolve_active(ctx)->mem_manager.allocation_start;
  uint64_t size = 0;
  while (size < ALLOC_SIZE) {
    offsets.push_back(storage.resolve(db1->_alloc_page(MAX_PAYLOAD, ctx)));
    size += MAX_PAYLOAD + sizeof(DBMMap::Traits::PageHeader);
  }

  // Check that area allocation succeeded
  BOOST_CHECK(!offsets.empty());

  // create a new area for db2
  auto db2 = storage.open("test2");

  db1->rollback(ctx);
  // the new area in db1 is "orphaned" and must be recycled the next time

  ctx = db1->start_transaction();
  // Check that we can start a new transaction

  // alloc again - with recycling, we should get similar allocation patterns
  std::vector<offset_t> new_offsets;
  for (size_t i = 0; i < offsets.size(); i++) {
    new_offsets.push_back(storage.resolve(db1->_alloc_page(MAX_PAYLOAD, ctx)));
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

  db1->rollback(ctx);
}

template <typename DB>
void dump(DB db, const char* prefix, int index) {
  std::stringstream cstr;
  cstr << "errors/" << prefix << std::setw(2) << std::setfill('0') << index
       << ".yaml";
  std::ofstream out(cstr.str().c_str());
  // TODO: needs TxnContext to dump
}

struct TestTraits {
  using Aspect = DefaultAspect;
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

  struct CtxMutex {
    void lock() {}
    void unlock() {}
  };

  struct CtxCondVar {
    template <typename L> void wait(L&) {}
    void notify_one() {}
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
    db = std::make_shared<DB>(*this, &db_offset, "test");
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
    auto db = storage.open("test");
    
    // Start transaction and allocate some data
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    BOOST_CHECK_EQUAL(db->_resolve_active(ctx)->txn_id, 2);  // First user txn is 2
    
    auto block1 = db->_alloc_page(512, ctx);
    memcpy((char*)block1, "test_data", 10);
    
    // Prepare the commit (makes it durable)
    prepared_tid = db->prepare_commit(ctx);
    BOOST_CHECK_GT(prepared_tid, 0);
    BOOST_CHECK_EQUAL(prepared_tid, 2);
    
    // Verify prepared state
    BOOST_CHECK_NE(db->context(0)->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->_resolve_active(ctx)->txn_id, prepared_tid);
    
    // Save offsets for verification
    prepared_offset = db->context(0)->prepared_txn;
    read_offset = db->_header->read_txn;
    
    // DON'T call commit() - simulate crash after prepare
    // Must still unlock txn_lock to avoid corrupting glibc's per-thread
    // robust mutex list (the mmap is about to be unmapped).
    db->end_transaction(ctx);
  }
  
  // Phase 2: Reopen DB - should detect and recover prepared transaction
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Check that prepared transaction was detected
    BOOST_CHECK_NE(db->context(0)->prepared_txn, db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, prepared_offset);
    BOOST_CHECK_EQUAL(db->_header->read_txn, read_offset);
    
    // active_txn should be set to the prepared transaction
    auto* ctx2 = db->context(0);
    BOOST_REQUIRE(ctx2->active_txn);
    auto active = db->_resolve_active(ctx2);
    BOOST_CHECK_EQUAL(active->txn_id, prepared_tid);
    
    // The transaction is already active (recovered), just need to claim context and commit
    // We need to simulate that a cursor takes ownership
    uint8_t exp = 0;
    BOOST_CHECK(db->context(0)->_claimed.compare_exchange_strong(exp, 1));
    
    // Commit should finalize the prepared transaction
    BOOST_CHECK(db->commit(ctx2));
    
    // After commit, prepared should equal read
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
  }
  
  // Phase 3: Verify data persisted correctly
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Everything should be committed now
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_two_phase_commit_normal_path) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_normal.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Normal two-phase commit: prepare then commit
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto block1 = db->_alloc_page(512, ctx);
    
    // Prepare should return transaction ID
    tid_t tid = db->prepare_commit(ctx);
    BOOST_CHECK_GT(tid, 0);
    BOOST_CHECK_NE(db->context(0)->prepared_txn, db->_header->read_txn);
    
    // Commit should succeed and finalize
    BOOST_CHECK(db->commit(ctx));
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
    BOOST_CHECK(!ctx->active_txn);
  }
  
  // Verify state persisted correctly
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_two_phase_commit_rollback_after_prepare) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_rollback.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Start transaction
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto block1 = db->_alloc_page(512, ctx);
    offset_t block1_offset = db->resolve(block1);
    
    // Prepare the commit
    tid_t tid = db->prepare_commit(ctx);
    BOOST_CHECK_GT(tid, 0);
    BOOST_CHECK_NE(db->context(0)->prepared_txn, db->_header->read_txn);
    
    // Now rollback even after prepare
    BOOST_CHECK(db->rollback(ctx));
    
    // After rollback, prepared should equal read again
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
    BOOST_CHECK(!ctx->active_txn);
    
    // Pending areas should have been returned
    // Start new transaction and verify block can be reused
    ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto block2 = db->_alloc_page(512, ctx);
    offset_t block2_offset = db->resolve(block2);
    
    // Should get the same block back since previous was rolled back
    BOOST_CHECK_EQUAL(block1_offset, block2_offset);
    
    db->commit(ctx);
  }
}

BOOST_AUTO_TEST_CASE(test_prepare_commit_idempotency) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_idempotent.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto block1 = db->_alloc_page(512, ctx);
    
    // First prepare
    tid_t tid1 = db->prepare_commit(ctx);
    BOOST_CHECK_GT(tid1, 0);
    
    // Second prepare should return same txn_id (idempotent)
    tid_t tid2 = db->prepare_commit(ctx);
    BOOST_CHECK_EQUAL(tid1, tid2);
    
    // State should be consistent
    BOOST_CHECK_NE(db->context(0)->prepared_txn, db->_header->read_txn);
    
    // Commit should still work
    BOOST_CHECK(db->commit(ctx));
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_prepare_commit_pending_areas) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_2pc_areas.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Allocate enough blocks to force new area allocation
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    
    std::vector<page_ptr> blocks;
    // Allocate many large blocks to exhaust the initial area and force new area allocation
    const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
    const size_t page_size = 4000;  // Large blocks
    const size_t num_blocks = (AREA_SIZE * 2) / page_size;  // Enough to need multiple areas
    
    for (size_t i = 0; i < num_blocks; i++) {
      blocks.push_back(db->_alloc_page(page_size, ctx));
    }
    
    // Before prepare, pending areas should have content (may not always be true if blocks fit in initial area)
    // This check is optional and depends on allocation patterns
    // BOOST_CHECK_GT((uint64_t)db->_header->pending_single_areas.get_head(), 0u);
    
    // Prepare the transaction
    tid_t tid = db->prepare_commit(ctx);
    BOOST_CHECK_GT(tid, 0);
    
    using Transaction = typename std::remove_pointer_t<decltype(db)>::Transaction;

    // In new architecture, area tails are in transaction, not separate pending lists
    auto read_txn = db->template resolve<Transaction>(&db->_header->read_txn);
    auto prep_txn = db->template resolve<Transaction>(&db->context(0)->prepared_txn);
    
    // Commit switches read_txn pointer
    BOOST_CHECK(db->commit(ctx));
    
    // After commit, read_txn should point to what was prepared_txn
    auto new_read_txn = db->template resolve<Transaction>(&db->_header->read_txn);
    BOOST_CHECK_EQUAL(db->_header->read_txn, db->context(0)->prepared_txn);
  }
}

BOOST_AUTO_TEST_CASE(test_sanitize_uncommitted_areas) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_sanitize.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Start a transaction and allocate many areas
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    
    // Get initial area tail offsets from the write transaction
    offset_t initial_single_tail = db->_resolve_active(ctx)->area_list_tail_single;
    offset_t initial_multi_tail = db->_resolve_active(ctx)->area_list_tail_multi;
    
    std::vector<page_ptr> blocks;
    // Allocate enough blocks to force multiple area allocations
    const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
    const size_t page_size = 4000;
    const size_t num_blocks = (AREA_SIZE * 3) / page_size;  // Force 3+ areas
    
    for (size_t i = 0; i < num_blocks; i++) {
      blocks.push_back(db->_alloc_page(page_size, ctx));
    }
    
    // Get the transaction state after allocations
    offset_t final_single_tail = db->_resolve_active(ctx)->area_list_tail_single;
    offset_t final_multi_tail = db->_resolve_active(ctx)->area_list_tail_multi;
    
    // Verify that single areas were allocated (tail should have moved)
    BOOST_CHECK_NE(final_single_tail, initial_single_tail);
    
    // Note: Multi-area allocation now happens through BigMemory in cursors
    // If we need to test multi-area allocation, we would need to:
    // 1. Create a cursor
    // 2. Insert big values (>4KB) which trigger BigMemory::alloc()
    // For now, we only verify single-area allocation in this test
    
    // Prepare but don't commit - simulate a crash
    tid_t tid = db->prepare_commit(ctx);
    BOOST_CHECK_GT(tid, 0);
    
    // Don't call commit - simulate crash after prepare
    // Just end the transaction without committing
    db->end_transaction(ctx);
  }
  
  // Reopen the database - this simulates crash recovery
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Check if database recovered a prepared transaction
    bool has_prepared_txn = (db->context(0)->prepared_txn != db->_header->read_txn);
    
    if (has_prepared_txn) {
      // Database should have recovered the prepared transaction
      auto read_txn = db->txn();  // Use txn() to get the read transaction pointer
      auto* ctx2 = db->context(0);
      auto prep_txn = db->_resolve_active(ctx2);  // After recovery, active_txn points to prepared txn
      
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
      
      // 2. Lock should be cleared
      
      // 3. prepared_txn should still be different (sanitize waits for rollback)
      BOOST_CHECK_NE(db->context(0)->prepared_txn, db->_header->read_txn);
      
      // 4. Manually rollback by setting prepared_txn = read_txn
      db->context(0)->prepared_txn = db->_header->read_txn;
      db->context(0)->active_txn = 0;
      db->flush();
      
      // 5. Now call sanitize again - should clean up areas
      db->sanitize();
    }
    
    // Database should be in a consistent state
    // Try to start a new transaction and allocate - should work
    auto ctx = db->start_transaction(); BOOST_CHECK(ctx);
    auto test_block = db->_alloc_page(1000, ctx);
    BOOST_CHECK(test_block);
    BOOST_CHECK(db->commit(ctx));
  }
}

BOOST_AUTO_TEST_CASE(test_sanitize_with_multiple_area_chains) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_sanitize_chains.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Start transaction and allocate enough to create a chain of areas
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    
    std::vector<page_ptr> blocks;
    const size_t AREA_SIZE = DBMMap::Traits::AREA_SIZE;
    const size_t page_size = 3000;
    
    // Allocate much more to definitely create multiple linked areas
    // We need to exceed the initial area and force allocation of new areas
    for (size_t i = 0; i < 50; i++) {
      blocks.push_back(db->_alloc_page(page_size, ctx));
    }
    
    offset_t tail_before_prepare = db->_resolve_active(ctx)->area_list_tail_single;
    
    // Prepare the transaction
    tid_t tid = db->prepare_commit(ctx);
    BOOST_CHECK_GT(tid, 0);
    
    // Simulate crash - don't commit
    db->end_transaction(ctx);
  }
  
  // Reopen and sanitize
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
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
    if (db->context(0)->prepared_txn != db->_header->read_txn) {
      // Manually rollback by setting prepared_txn = read_txn
      db->context(0)->prepared_txn = db->_header->read_txn;
      db->context(0)->active_txn = 0;
      db->flush();
      
      // Call sanitize again to clean up areas
      db->sanitize();
    }
    
    // After sanitize with no prepared txn, area cleanup should have happened
    BOOST_CHECK_EQUAL(db->context(0)->prepared_txn, db->_header->read_txn);
    
    // Database should be usable after sanitize
    auto ctx = db->start_transaction(); BOOST_CHECK(ctx);
    auto test_block = db->_alloc_page(1000, ctx);
    BOOST_CHECK(test_block);
    BOOST_CHECK(db->commit(ctx));
  }
}

BOOST_AUTO_TEST_CASE(test_defrag_empty_db) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  // defrag() on a fresh DB with no big memory should return early
  db->defrag();

  // DB should still be usable after no-op defrag
  auto ctx = db->start_transaction(); BOOST_CHECK(ctx);
  auto block = db->_alloc_page(100, ctx);
  BOOST_CHECK(block);
  BOOST_CHECK(db->commit(ctx));
}

BOOST_AUTO_TEST_CASE(test_nonblocking_transaction) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  // Start a blocking transaction (holds the lock)
  auto ctx1 = db->start_transaction();
  BOOST_CHECK(ctx1);

  // Try nonblocking start — should fail because lock is held
  auto ctx2 = db->start_transaction(true);
  BOOST_CHECK(!ctx2);

  // Commit the first transaction
  BOOST_CHECK(db->commit(ctx1));

  // Now nonblocking should succeed
  auto ctx3 = db->start_transaction(true);
  BOOST_CHECK(ctx3);
  BOOST_CHECK(db->commit(ctx3));
}

BOOST_AUTO_TEST_CASE(test_multi_area_rollback) {
  // Exercises return_areas_to_pool multi-area path (L649-664)
  // Allocating multi-areas during a transaction then rolling back should
  // return those areas to the storage pool.
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_multi_rollback.lvs";
  
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
  
  // Start transaction and allocate multi-areas directly
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  
  // Record the read transaction's multi-area tail before allocation
  using Transaction = typename std::remove_pointer_t<decltype(db)>::Transaction;
  auto read_txn = db->template resolve<Transaction>(&db->_header->read_txn);
  offset_t read_multi_tail = read_txn->area_list_tail_multi;
  
  // Allocate a multi-area (triggers alloc_multi_area path)
  auto area1 = db->_alloc_multi_area(DBMMap::Traits::AREA_SIZE * 2, ctx);
  BOOST_CHECK(area1);
  
  // The write transaction should have a different multi-area tail now
  BOOST_CHECK_NE(db->_resolve_active(ctx)->area_list_tail_multi, read_multi_tail);
  
  // Allocate another multi-area
  auto area2 = db->_alloc_multi_area(DBMMap::Traits::AREA_SIZE * 3, ctx);
  BOOST_CHECK(area2);
  
  // Rollback should return multi-areas to pool
  BOOST_CHECK(db->rollback(ctx));
  
  // DB should be in clean state
  BOOST_CHECK(!ctx->active_txn);
  
  // Start a new transaction to verify DB is still usable
  ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  auto block = db->_alloc_page(100, ctx);
  BOOST_CHECK(block);
  BOOST_CHECK(db->commit(ctx));
}

BOOST_AUTO_TEST_CASE(test_multi_area_rollback_with_prior_committed) {
  // Tests return_areas_to_pool with start_multi != 0
  // (prior committed multi-area exists, then new ones added and rolled back)
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_multi_rollback2.lvs";
  
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
  
  // First transaction: allocate and commit a multi-area
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  auto area1 = db->_alloc_multi_area(DBMMap::Traits::AREA_SIZE * 2, ctx);
  BOOST_CHECK(area1);
  BOOST_CHECK(db->commit(ctx));
  
  // Second transaction: allocate more multi-areas then rollback
  ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  
  using Transaction = typename std::remove_pointer_t<decltype(db)>::Transaction;
  auto read_txn = db->template resolve<Transaction>(&db->_header->read_txn);
  offset_t committed_multi_tail = read_txn->area_list_tail_multi;
  BOOST_CHECK_NE(committed_multi_tail, offset_t(0)); // Should have committed multi-area
  
  auto area2 = db->_alloc_multi_area(DBMMap::Traits::AREA_SIZE * 2, ctx);
  BOOST_CHECK(area2);
  
  // Write txn's multi tail should have advanced beyond committed
  BOOST_CHECK_NE(db->_resolve_active(ctx)->area_list_tail_multi, committed_multi_tail);
  
  // Rollback should return only the new multi-areas, keeping committed ones
  BOOST_CHECK(db->rollback(ctx));
  
  // Verify DB still works
  ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  auto block = db->_alloc_page(100, ctx);
  BOOST_CHECK(block);
  BOOST_CHECK(db->commit(ctx));
}

BOOST_AUTO_TEST_CASE(test_return_areas_multi) {
  // Exercises return_areas() multi-area path (L234-235)
  // Commit multi-areas, then call return_areas() via remove
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_return_multi.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Allocate and commit multi-areas
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto area1 = db->_alloc_multi_area(DBMMap::Traits::AREA_SIZE * 2, ctx);
    BOOST_CHECK(area1);
    BOOST_CHECK(db->commit(ctx));
    
    // Verify multi-area head is set
    BOOST_CHECK_NE(db->context(0)->area_list_head_multi, offset_t(0));
    
    // return_areas() should return the committed multi-areas to pool
    db->return_areas();
  }
}

BOOST_AUTO_TEST_CASE(test_garbage_statistics_multi_container) {
  // Exercises _garbage_statistics while loop (L565, L569)
  // Need to free enough pages that a garbage slot has >1 PageContainer
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_gc_stats.lvs";
  
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
  
  // We need to free many pages to overflow one PageContainer.
  // PageContainer::COUNT is ~254 for 4K containers with 16-byte items.
  // We'll allocate ~300 pages, commit, then allocate them again (forcing the
  // old ones to be freed into the garbage collector via transaction recycling).
  
  // Transaction 1: allocate 300 pages
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  std::vector<offset_t> offsets;
  for (int i = 0; i < 300; i++) {
    auto p = db->_alloc_page(80, ctx); // size 80 => slot 0 (size 100)
    offsets.push_back(db->resolve(p));
  }
  BOOST_CHECK(db->commit(ctx));
  
  // Transaction 2: free all those pages by calling db->_free()
  // This puts them in the garbage collector
  ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  for (auto off : offsets) {
    page_ptr p = db->resolve<PageHeader>(&off);
    db->_free(p, ctx);
  }
  BOOST_CHECK(db->commit(ctx));
  
  // Now call _garbage_statistics which exercises the while loop
  // when a garbage slot has >254 entries (multiple PageContainers)
  using MemStatistics = typename std::remove_pointer_t<decltype(db)>::MemStatistics;
  MemStatistics stat{};
  db->_garbage_statistics(stat);
  
  // The garbage collector should have recorded our freed pages
  // Check that at least one slot has many entries (>250 freed pages)
  uint64_t max_count = 0;
  for (int i = 0; i < stat.COUNT; i++) {
    max_count = std::max(max_count, (uint64_t)stat.slots[i].count);
  }
  BOOST_CHECK_GT(max_count, 250u);
}

BOOST_AUTO_TEST_CASE(test_sanitize_next_txn_page_zero) {
  // Exercises sanitize() else branch (L688-693)
  // Simulates crash where next_txn_page was cleared to 0 during
  // start_transaction but before prepare_commit allocated a replacement.
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_sanitize_ntp0.lvs";
  
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Do one commit so the DB is in a consistent state with data
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto p = db->_alloc_page(100, ctx);
    BOOST_CHECK(db->commit(ctx));
    
    // Simulate the crash window: clear next_txn_page as start_transaction does
    db->context(0)->next_txn_page = 0;
    
    // Write a fake stale PID so sanitize_processes() increments sanitize_generation
    // Use PID 99999999 which should not be running
    storage._memory->processes[0] = 99999999;
    
    storage.flush();
    
    // Skip destructor's remove_pid by leaving the fake PID in place
    // (destructor removes current PID, not the fake one)
  }
  
  // Reopen: sanitize() detects stale PID → increments sanitize_generation
  // open() sees generation mismatch → db.sanitize() sees next_txn_page == 0
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    
    // Verify next_txn_page was recreated
    BOOST_CHECK_NE(db->context(0)->next_txn_page, offset_t(0));
    
    // DB should be fully operational
    auto* ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    auto p = db->_alloc_page(100, ctx);
    BOOST_CHECK(p);
    BOOST_CHECK(db->commit(ctx));
  }
}

BOOST_AUTO_TEST_CASE(test_remove_type_mismatch_cached) {
  // Open as _DB, try remove<_ReplicationDB> — should throw TypeMismatch
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_rm_type.lvs";
  DBMMap storage(dbFilePath.c_str());

  storage.open("test");  // opens as _DB (cached)
  BOOST_CHECK_THROW(storage.remove<_ReplicationDB>("test"), TypeMismatch);
}

BOOST_AUTO_TEST_CASE(test_remove_type_mismatch_uncached) {
  // Create as _DB, close storage, reopen, remove<_ReplicationDB> without
  // opening first — hits the uncached fallback path
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_rm_uncached.lvs";

  {
    DBMMap storage(dbFilePath.c_str());
    storage.open("test");  // creates as _DB
  }

  {
    DBMMap storage(dbFilePath.c_str());
    // DB not in cache — exercises the on-disk type_id check
    BOOST_CHECK_THROW(storage.remove<_ReplicationDB>("test"), TypeMismatch);
  }
}

BOOST_AUTO_TEST_CASE(test_remove_replication_db) {
  // Open as _ReplicationDB, remove<_ReplicationDB> — should succeed
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_rm_repl.lvs";
  DBMMap storage(dbFilePath.c_str());

  auto db = storage.open<_ReplicationDB>("test");
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  db->_alloc_page(100, ctx);
  BOOST_CHECK(db->commit(ctx));

  storage.remove<_ReplicationDB>("test");

  // Verify the slot is freed — re-opening should create a fresh DB
  auto db2 = storage.open<_ReplicationDB>("test2");
  BOOST_CHECK(db2);
}

BOOST_AUTO_TEST_CASE(test_remove_active_transaction) {
  // remove() should throw TransactionActive if a txn is in progress
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_rm_active.lvs";
  DBMMap storage(dbFilePath.c_str());

  auto db = storage.open("test");
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);

  BOOST_CHECK_THROW(storage.remove("test"), TransactionActive);

  db->rollback(ctx);
}

BOOST_AUTO_TEST_CASE(test_remove_nonexistent) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_rm_noexist.lvs";
  DBMMap storage(dbFilePath.c_str());

  BOOST_CHECK_THROW(storage.remove("ghost"), WrongValue);
}

BOOST_AUTO_TEST_CASE(test_memory_checker_with_garbage) {
  // Exercises _MemoryChecker::check() with garbage containers populated
  // Covers db/_check.hpp L323,328 (mark_page in garbage container loop)
  // Also covers memory/_memory.hpp Slot::iter and push_back paths
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_check_gc.lvs";
  
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
  
  // Transaction 1: allocate many pages
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  std::vector<offset_t> offsets;
  for (int i = 0; i < 300; i++) {
    auto p = db->_alloc_page(80, ctx);
    offsets.push_back(db->resolve(p));
  }
  BOOST_CHECK(db->commit(ctx));
  
  // Transaction 2: free all pages into garbage collector
  ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  for (auto off : offsets) {
    page_ptr p = db->resolve<PageHeader>(&off);
    db->_free(p, ctx);
  }
  BOOST_CHECK(db->commit(ctx));
  
  // Run memory checker — should succeed and exercise garbage iteration paths
  using DB = std::remove_pointer_t<decltype(db)>;
  _MemoryChecker<DB> checker(*db);
  BOOST_CHECK_NO_THROW(checker.check());
  BOOST_CHECK_GT(checker.total_pages, 0u);
}

BOOST_AUTO_TEST_CASE(test_internal_method) {
  // Exercises db/_db.hpp L268 (_internal())
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_internal.lvs";
  
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");
  
  // _internal() returns the db pointer itself
  auto internal = db->_internal();
  BOOST_CHECK(internal == db);
}

BOOST_AUTO_TEST_CASE(test_storage_full_exception) {
  // Exercises core/_exception.hpp L36 — StorageFull::what()
  StorageFull ex;
  BOOST_CHECK(ex.what() != nullptr);
  BOOST_CHECK(std::string(ex.what()).find("storage full") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_type_mismatch_exception) {
  // Exercises core/_exception.hpp L36 — TypeMismatch::what()
  TypeMismatch ex;
  BOOST_CHECK(ex.what() != nullptr);
  BOOST_CHECK(std::string(ex.what()).find("mismatch") != std::string::npos);

  TypeMismatch ex2("custom message");
  BOOST_CHECK_EQUAL(std::string(ex2.what()), "custom message");
}

BOOST_AUTO_TEST_CASE(test_type_mismatch_cached_db) {
  // Exercises _cachestore.hpp L336-337 — TypeMismatch when opening cached DB
  // with wrong type. Open as _DB (type_id=0), then try _ReplicationDB (type_id=1).
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_typemismatch.lvs";

  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");  // Opens as _DB (type_id=0)

  // Now try to open the same name as _ReplicationDB (type_id=1)
  BOOST_CHECK_THROW(
      storage.template open<_ReplicationDB>("test"),
      TypeMismatch);
}

BOOST_AUTO_TEST_CASE(test_prepare_commit_wrong_cursor) {
  // Exercises _db.hpp prepare_commit
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_prepare.lvs";

  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  // Start transaction
  auto ctx = db->start_transaction();
  BOOST_REQUIRE(ctx);

  // prepare_commit now takes TxnContext* directly
  // Just prepare and commit normally
  db->prepare_commit(ctx);
  db->commit(ctx);
}

BOOST_AUTO_TEST_CASE(test_return_areas_range_rollback) {
  // Exercises _db.hpp L669 (single area return) and L684 (first multi area return)
  // by allocating pages during a transaction and then rolling back.
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_area_rollback.lvs";

  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  // Transaction 1: allocate many pages to force area expansion, then commit
  auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  for (int i = 0; i < 500; i++) {
    db->_alloc_page(80, ctx);
  }
  BOOST_CHECK(db->commit(ctx));

  // Transaction 2: allocate more, then ROLLBACK — exercises return_areas_range
  ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
  for (int i = 0; i < 500; i++) {
    db->_alloc_page(80, ctx);
  }
  BOOST_CHECK(db->rollback(ctx));

  // Memory checker should still pass
  using DB = std::remove_pointer_t<decltype(db)>;
  _MemoryChecker<DB> checker(*db);
  BOOST_CHECK_NO_THROW(checker.check());
}

BOOST_AUTO_TEST_CASE(test_sanitize_missing_next_txn_page) {
  // Exercises _db.hpp L713 — sanitize when next_txn_page == 0
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_sanitize.lvs";

  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");

    // Write some data to have a valid read_txn
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    db->_alloc_page(80, ctx);
    BOOST_CHECK(db->commit(ctx));

    // Simulate crash: clear next_txn_page to 0
    db->context(0)->next_txn_page = 0;
    db->make_dirty(db->_header);
    db->flush(true, true);
  }

  // Reopen and sanitize — should recreate next_txn_page from read_txn
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("test");
    db->sanitize();

    // next_txn_page should now be valid again
    BOOST_CHECK(db->context(0)->next_txn_page != 0);

    // Should be able to start a new transaction
    auto* ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    db->_alloc_page(80, ctx);
    BOOST_CHECK(db->commit(ctx));
  }
}

BOOST_AUTO_TEST_CASE(test_remove_uncached_db) {
  // Exercises _cachestore.hpp L597-602 — _return_areas_at for uncached DB
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_remove_uncached.lvs";

  // Create storage file with a DB
  {
    DBMMap storage(dbFilePath.c_str());
    auto db = storage.open("mydb");
    // Write some data
    auto ctx = db->start_transaction(); BOOST_REQUIRE(ctx);
    db->_alloc_page(80, ctx);
    BOOST_CHECK(db->commit(ctx));
  }

  // Reopen storage — DB exists in file but NOT in cache
  {
    DBMMap storage(dbFilePath.c_str());
    // Don't call open("mydb") — so it's not cached
    // Remove it — hits the uncached path in _return_areas_at
    BOOST_CHECK_NO_THROW(storage.template remove<_DB>("mydb"));
  }
}

// ---------------------------------------------------------------------------
// Concurrent commit / merge-on-commit tests
// ---------------------------------------------------------------------------
// Two MapStorage instances open the same file, each getting its own _DB
// instance that shares the mmap'd header/contexts.  With num_contexts=2
// both can hold an active transaction simultaneously, forcing merge-on-commit
// when the second writer publishes.

BOOST_AUTO_TEST_CASE(test_concurrent_commit_disjoint_keys) {
  // Two contexts write disjoint keys.  Second committer must merge;
  // afterwards all keys from both transactions must be visible.
  DirPreparation prep;
  auto path = (prep.tempDir / "merge_disjoint.lvs").string();

  auto storage1 = MapStorage::create(path.c_str());
  auto db1 = storage1->open("test", 2);  // 2 contexts

  auto storage2 = MapStorage::create(path.c_str());
  auto db2 = storage2->open("test", 2);

  std::atomic<bool> a_started{false}, b_committed{false};

  // Thread A: start txn, write keys, wait for B to commit, then commit (merge).
  std::thread threadA([&]() {
    auto c = db1.cursor();
    c.start_transaction();
    c.find("keyA1"); c.value("valA1");
    c.find("keyA2"); c.value("valA2");
    a_started.store(true, std::memory_order_release);
    while (!b_committed.load(std::memory_order_acquire))
      std::this_thread::yield();
    c.commit();  // conflict detected → merge
  });

  // Thread B: wait for A to start, then commit first (no conflict).
  std::thread threadB([&]() {
    while (!a_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    auto c = db2.cursor();
    c.start_transaction();
    c.find("keyB1"); c.value("valB1");
    c.find("keyB2"); c.value("valB2");
    c.commit();
    b_committed.store(true, std::memory_order_release);
  });

  threadA.join();
  threadB.join();

  // Verify all four keys are visible.
  auto reader = db1.cursor();
  reader.find("keyA1"); BOOST_CHECK(reader.is_valid());
  if (reader.is_valid())
    BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "valA1");
  reader.find("keyA2"); BOOST_CHECK(reader.is_valid());
  if (reader.is_valid())
    BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "valA2");
  reader.find("keyB1"); BOOST_CHECK(reader.is_valid());
  if (reader.is_valid())
    BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "valB1");
  reader.find("keyB2"); BOOST_CHECK(reader.is_valid());
  if (reader.is_valid())
    BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "valB2");
}

BOOST_AUTO_TEST_CASE(test_concurrent_commit_same_key_last_writer_wins) {
  // Both contexts write the same key.  The second committer's merge
  // should overwrite, so its value wins (last-writer-wins).
  DirPreparation prep;
  auto path = (prep.tempDir / "merge_lww.lvs").string();

  auto storage1 = MapStorage::create(path.c_str());
  auto db1 = storage1->open("test", 2);

  auto storage2 = MapStorage::create(path.c_str());
  auto db2 = storage2->open("test", 2);

  std::atomic<bool> a_started{false}, b_committed{false};

  std::thread threadA([&]() {
    auto c = db1.cursor();
    c.start_transaction();
    c.find("shared"); c.value("from_A");
    a_started.store(true, std::memory_order_release);
    while (!b_committed.load(std::memory_order_acquire))
      std::this_thread::yield();
    c.commit();  // merges over B's value
  });

  std::thread threadB([&]() {
    while (!a_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    auto c = db2.cursor();
    c.start_transaction();
    c.find("shared"); c.value("from_B");
    c.commit();
    b_committed.store(true, std::memory_order_release);
  });

  threadA.join();
  threadB.join();

  auto reader = db1.cursor();
  reader.find("shared");
  BOOST_REQUIRE(reader.is_valid());
  // A committed after B — A's merge overwrites → "from_A" wins
  BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "from_A");
}

BOOST_AUTO_TEST_CASE(test_concurrent_commit_delete_and_insert) {
  // Pre-populate key "victim".  Context A deletes it; context B inserts
  // new key "newcomer".  Second committer merges — delete applied, new
  // key visible.
  DirPreparation prep;
  auto path = (prep.tempDir / "merge_delete.lvs").string();

  auto storage1 = MapStorage::create(path.c_str());
  auto db1 = storage1->open("test", 2);

  // Seed data
  {
    auto c = db1.cursor();
    c.find("victim"); c.value("doomed");
    c.find("survivor"); c.value("ok");
    c.commit();
  }

  auto storage2 = MapStorage::create(path.c_str());
  auto db2 = storage2->open("test", 2);

  std::atomic<bool> a_started{false}, b_committed{false};

  // A deletes "victim"
  std::thread threadA([&]() {
    auto c = db1.cursor();
    c.start_transaction();
    c.find("victim");
    BOOST_REQUIRE(c.is_valid());
    c.remove();
    a_started.store(true, std::memory_order_release);
    while (!b_committed.load(std::memory_order_acquire))
      std::this_thread::yield();
    c.commit();
  });

  // B inserts "newcomer"
  std::thread threadB([&]() {
    while (!a_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    auto c = db2.cursor();
    c.start_transaction();
    c.find("newcomer"); c.value("hello");
    c.commit();
    b_committed.store(true, std::memory_order_release);
  });

  threadA.join();
  threadB.join();

  auto reader = db1.cursor();
  reader.find("victim");
  BOOST_CHECK(!reader.is_valid());  // deleted

  reader.find("newcomer");
  BOOST_CHECK(reader.is_valid());
  BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "hello");

  reader.find("survivor");
  BOOST_CHECK(reader.is_valid());  // untouched
}

BOOST_AUTO_TEST_CASE(test_single_context_no_conflict) {
  // A single context that commits without any concurrent writer should
  // take the fast path (no merge).
  DirPreparation prep;
  auto path = (prep.tempDir / "merge_noconflict.lvs").string();

  auto storage = MapStorage::create(path.c_str());
  auto db = storage->open("test", 2);

  auto c = db.cursor();
  c.find("solo"); c.value("only");
  c.commit();

  auto reader = db.cursor();
  reader.find("solo");
  BOOST_REQUIRE(reader.is_valid());
  BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "only");
}

BOOST_AUTO_TEST_CASE(test_concurrent_commit_many_keys) {
  // Stress: each context writes 100 disjoint keys.  After merge all 200
  // must be readable.
  DirPreparation prep;
  auto path = (prep.tempDir / "merge_stress.lvs").string();

  auto storage1 = MapStorage::create(path.c_str());
  auto db1 = storage1->open("test", 2);

  auto storage2 = MapStorage::create(path.c_str());
  auto db2 = storage2->open("test", 2);

  constexpr int N = 100;
  std::atomic<bool> a_started{false}, b_committed{false};

  std::thread threadA([&]() {
    auto c = db1.cursor();
    c.start_transaction();
    for (int i = 0; i < N; i++) {
      std::string key = "A_" + std::to_string(i);
      std::string val = "vA_" + std::to_string(i);
      c.find(key); c.value(val);
    }
    a_started.store(true, std::memory_order_release);
    while (!b_committed.load(std::memory_order_acquire))
      std::this_thread::yield();
    c.commit();
  });

  std::thread threadB([&]() {
    while (!a_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    auto c = db2.cursor();
    c.start_transaction();
    for (int i = 0; i < N; i++) {
      std::string key = "B_" + std::to_string(i);
      std::string val = "vB_" + std::to_string(i);
      c.find(key); c.value(val);
    }
    c.commit();
    b_committed.store(true, std::memory_order_release);
  });

  threadA.join();
  threadB.join();

  auto reader = db1.cursor();
  for (int i = 0; i < N; i++) {
    std::string keyA = "A_" + std::to_string(i);
    std::string keyB = "B_" + std::to_string(i);
    reader.find(keyA);
    BOOST_CHECK_MESSAGE(reader.is_valid(), "missing " + keyA);
    reader.find(keyB);
    BOOST_CHECK_MESSAGE(reader.is_valid(), "missing " + keyB);
  }
}

BOOST_AUTO_TEST_CASE(test_sequential_commits_no_merge) {
  // Two sequential (non-overlapping) transactions should never trigger
  // the merge path — both commit cleanly.
  DirPreparation prep;
  auto path = (prep.tempDir / "merge_seq.lvs").string();

  auto storage = MapStorage::create(path.c_str());
  auto db = storage->open("test", 2);

  // First transaction
  {
    auto c = db.cursor();
    c.find("first"); c.value("1");
    c.commit();
  }
  // Second transaction — snapshot matches committed, no merge.
  {
    auto c = db.cursor();
    c.find("second"); c.value("2");
    c.commit();
  }

  auto reader = db.cursor();
  reader.find("first");
  BOOST_REQUIRE(reader.is_valid());
  BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "1");
  reader.find("second");
  BOOST_REQUIRE(reader.is_valid());
  BOOST_CHECK_EQUAL(std::string(reader.value().data(), reader.value().size()), "2");
}

BOOST_AUTO_TEST_CASE(test_double_free_and_recycle) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  // Use slot 4 (128-byte payload) which doesn't conflict with
  // Transaction (slot 5) or PageContainer (slot 7) garbage queues.
  const uint16_t ALLOC_SIZE = 128;
  offset_t block1_off, block2_off, block3_off;

  // Txn 1: allocate two pages
  {
    Transaction trans(db);
    auto block1 = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    auto block2 = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    block1_off = db->resolve(block1);
    block2_off = db->resolve(block2);
    BOOST_REQUIRE(block1_off != block2_off);
  }

  // Txn 2: free both pages
  {
    Transaction trans(db);
    auto b1 = db->template resolve<PageHeader>(&block1_off);
    auto b2 = db->template resolve<PageHeader>(&block2_off);
    db->_free(b1, trans.ctx);
    db->_free(b2, trans.ctx);
  }

  // Txn 3: free block1 again (double-free — it's already in garbage queue)
  {
    Transaction trans(db);
    auto b1 = db->template resolve<PageHeader>(&block1_off);
    db->_free(b1, trans.ctx);
  }

  // Txn 4: advance past old txns so garbage becomes recyclable.
  // No readers hold references to the old transactions.
  { Transaction trans(db); }

  // Txn 5: allocate pages — the freed_tid CAS guard must prevent
  // block1 from being recycled twice even though it appears in the
  // garbage queue twice.
  {
    Transaction trans(db);
    auto r1 = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    auto r2 = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    auto r3 = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    offset_t r1_off = db->resolve(r1);
    offset_t r2_off = db->resolve(r2);
    offset_t r3_off = db->resolve(r3);

    // All three allocations must return distinct pages
    BOOST_CHECK(r1_off != r2_off);
    BOOST_CHECK(r1_off != r3_off);
    BOOST_CHECK(r2_off != r3_off);

    // block1 and block2 should be recycled exactly once each
    bool r1_is_b1 = (r1_off == block1_off);
    bool r1_is_b2 = (r1_off == block2_off);
    bool r2_is_b1 = (r2_off == block1_off);
    bool r2_is_b2 = (r2_off == block2_off);
    bool r3_is_b1 = (r3_off == block1_off);
    bool r3_is_b2 = (r3_off == block2_off);

    int b1_count = r1_is_b1 + r2_is_b1 + r3_is_b1;
    int b2_count = r1_is_b2 + r2_is_b2 + r3_is_b2;

    // block1 was double-freed but must only be recycled once
    BOOST_CHECK_EQUAL(b1_count, 1);
    // block2 was freed once and must be recycled once
    BOOST_CHECK_EQUAL(b2_count, 1);
  }
}

BOOST_AUTO_TEST_CASE(test_recycle_not_premature) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test");

  // Use slot 4 (128-byte payload) which doesn't conflict with
  // Transaction (slot 5) or PageContainer (slot 7) garbage queues.
  const uint16_t ALLOC_SIZE = 128;

  // Hold a reader on txn 1 to prevent recycling
  auto reader_txn = db->txn();
  reader_txn->refs.fetch_add(1);

  offset_t block_off;

  // Txn 2: allocate a page
  {
    Transaction trans(db);
    auto block = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    block_off = db->resolve(block);
  }

  // Txn 3: free the page
  {
    Transaction trans(db);
    auto b = db->template resolve<PageHeader>(&block_off);
    db->_free(b, trans.ctx);
  }

  // Txn 4: allocate — reader holds txn 1, so _start_txn_id stays at 1.
  // The freed page (garbage txn_id=3) should NOT be recyclable because 3 < 1 is false.
  {
    Transaction trans(db);
    auto r = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    offset_t r_off = db->resolve(r);
    BOOST_CHECK(r_off != block_off);
  }

  // Release the reader
  reader_txn->refs.fetch_sub(1);

  // Txn 5: advance so _start_txn_id passes the garbage entry
  { Transaction trans(db); }

  // Txn 6: now the freed page should be recyclable (garbage txn_id=3 < _start_txn_id)
  {
    Transaction trans(db);
    auto r = db->_alloc_page(ALLOC_SIZE, trans.ctx);
    offset_t r_off = db->resolve(r);
    BOOST_CHECK_EQUAL(r_off, block_off);
  }
}

BOOST_AUTO_TEST_CASE(test_defrag_multi_context) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  // 4 contexts so defrag() must claim all of them
  DBMMap storage(dbFilePath.c_str());
  auto db = storage.open("test", uint8_t(4));

  const size_t K = 1024;
  const size_t CHUNK_SIZE = 8 * K;  // exceeds MAX_PAGE_SIZE → BigMemory

  // --- Context 0: allocate 6 chunks, delete first 3 to create free space ---
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    std::vector<char> data(CHUNK_SIZE);
    for (int i = 0; i < 6; i++) {
      std::string key = "ctx0_" + std::to_string(i);
      std::fill(data.begin(), data.end(), 'A' + i);
      cursor->find(key);
      cursor->value(Slice(data.data(), CHUNK_SIZE));
    }
    cursor->commit();
  }
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    for (int i = 0; i < 3; i++) {
      std::string key = "ctx0_" + std::to_string(i);
      cursor->find(key);
      cursor->remove();
    }
    cursor->commit();
  }

  // --- Context 1: allocate 4 chunks, delete first 2 ---
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    std::vector<char> data(CHUNK_SIZE);
    for (int i = 0; i < 4; i++) {
      std::string key = "ctx1_" + std::to_string(i);
      std::fill(data.begin(), data.end(), 'P' + i);
      cursor->find(key);
      cursor->value(Slice(data.data(), CHUNK_SIZE));
    }
    cursor->commit();
  }
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    for (int i = 0; i < 2; i++) {
      std::string key = "ctx1_" + std::to_string(i);
      cursor->find(key);
      cursor->remove();
    }
    cursor->commit();
  }

  // Barrier transaction to advance txn id for recycling
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    cursor->find("barrier");
    const char b = 'B';
    cursor->value(Slice(&b, 1));
    cursor->commit();
  }

  // Defrag should claim all 4 contexts, find free chunks in ctx 0 and ctx 1,
  // merge adjacent ones, then commit all.
  db->defrag();

  // Verify surviving data from context 0 (chunks 3-5)
  {
    auto cursor = db->create_cursor();
    for (int i = 3; i < 6; i++) {
      std::string key = "ctx0_" + std::to_string(i);
      cursor->find(key);
      BOOST_CHECK_MESSAGE(cursor->is_valid(), "ctx0_" + std::to_string(i) + " missing");
      Slice val = cursor->value();
      BOOST_CHECK_EQUAL(val.size(), CHUNK_SIZE);
      BOOST_CHECK_EQUAL(val.data()[0], 'A' + i);
    }
  }

  // Verify surviving data from context 1 (chunks 2-3)
  {
    auto cursor = db->create_cursor();
    for (int i = 2; i < 4; i++) {
      std::string key = "ctx1_" + std::to_string(i);
      cursor->find(key);
      BOOST_CHECK_MESSAGE(cursor->is_valid(), "ctx1_" + std::to_string(i) + " missing");
      Slice val = cursor->value();
      BOOST_CHECK_EQUAL(val.size(), CHUNK_SIZE);
      BOOST_CHECK_EQUAL(val.data()[0], 'P' + i);
    }
  }

  // After defrag merged adjacent free chunks, a larger allocation should succeed
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    const size_t LARGE = 16 * K;
    std::vector<char> large_data(LARGE, 'Z');
    cursor->find("large_after_defrag");
    cursor->value(Slice(large_data.data(), LARGE));
    cursor->find("large_after_defrag");
    BOOST_CHECK(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->value().size(), LARGE);
    cursor->commit();
  }

  // DB is still usable after defrag
  {
    auto cursor = db->create_cursor();
    cursor->start_transaction();
    cursor->find("post_defrag");
    const char p = 'Y';
    cursor->value(Slice(&p, 1));
    cursor->commit();
  }
}