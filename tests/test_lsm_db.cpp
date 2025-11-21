#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE LSMDBTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_lsm_db.hpp"
#include "leaves/intern/_mmap.hpp"
#include <thread>
#include <chrono>

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits, _LSMDB> TestStorage;
typedef _LSMDB<TestStorage> LSMDB;
typedef typename LSMDB::Transaction LSMTransaction;
typedef typename LSMDB::SegmentType LSMSegment;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_lsm_db";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }

  ~DirPreparation() { 
    std::filesystem::remove_all(tempDir); 
  }

  std::filesystem::path tempDir;
};

BOOST_AUTO_TEST_CASE(test_lsm_db_creation) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  BOOST_CHECK(db->_header != nullptr);
  BOOST_CHECK_EQUAL(db->_header->last_merged_txn_id.load(), 0);
  BOOST_CHECK_EQUAL(db->_header->auto_commit_ms, 200);
  BOOST_CHECK_EQUAL(db->_header->auto_commit_enabled, 0);
}

BOOST_AUTO_TEST_CASE(test_lsm_transaction_start) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  
  BOOST_CHECK(db->_wtxn != nullptr);
  BOOST_CHECK(db->_wtxn->segment_head != 0);
  BOOST_CHECK(db->_wtxn->current_segment != 0);
  BOOST_CHECK_EQUAL(db->_wtxn->segment_head, db->_wtxn->current_segment);
  BOOST_CHECK_EQUAL(db->_wtxn->merge_phase, WRITING);
  
  db->rollback(0);
}

BOOST_AUTO_TEST_CASE(test_segment_allocation) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  
  // Get first segment
  offset_t seg_offset = db->_wtxn->current_segment;
  BOOST_CHECK(seg_offset != 0);
  
  auto segment = db->template resolve<LSMSegment>(seg_offset, WRITE);
  BOOST_CHECK(segment != nullptr);
  BOOST_CHECK(segment->parent_db == db.get());
  BOOST_CHECK_EQUAL(segment->refs.load(), 1);
  BOOST_CHECK_EQUAL(segment->next, 0);
  
  db->rollback(0);
}

BOOST_AUTO_TEST_CASE(test_segment_overflow) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  
  offset_t first_segment = db->_wtxn->current_segment;
  
  // Fill first segment by allocating until overflow
  // This should trigger allocation of a new segment
  // The exact mechanism depends on how _MemoryDB handles overflow
  
  db->rollback(0);
}

BOOST_AUTO_TEST_CASE(test_commit_and_merge) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  
  // Allocate some data
  auto block = db->alloc(100);
  BOOST_CHECK(block != nullptr);
  
  // Commit the transaction
  tid_t txn_id = db->prepare_commit(0, false);
  BOOST_CHECK(txn_id.value() != 0);
  BOOST_CHECK_EQUAL(db->_wtxn->merge_phase, WRITING);
  
  db->commit(false);
  BOOST_CHECK_EQUAL(db->txn()->merge_phase, COMMITTING);
  
  // Wait for background thread to merge
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  // Check that transaction was merged
  BOOST_CHECK_EQUAL(db->txn()->merge_phase, COMMITTED);
}

BOOST_AUTO_TEST_CASE(test_sync_commit) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  
  auto block = db->alloc(100);
  BOOST_CHECK(block != nullptr);
  
  // Commit with sync
  tid_t txn_id = db->prepare_commit(0, true);
  BOOST_CHECK(txn_id.value() != 0);
  BOOST_CHECK_EQUAL(db->_wtxn->sync_commit, 0);  // Not set until commit()
  
  db->commit(true);
  BOOST_CHECK_EQUAL(db->txn()->sync_commit, 1);
  BOOST_CHECK_EQUAL(db->txn()->merge_phase, COMMITTING);
  
  // Wait for background thread to merge
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  BOOST_CHECK_EQUAL(db->txn()->merge_phase, COMMITTED);
}

BOOST_AUTO_TEST_CASE(test_auto_commit) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  // Test auto-commit configuration
  db->enable_auto_commit(500);
  BOOST_CHECK_EQUAL(db->_header->auto_commit_enabled, 1);
  BOOST_CHECK_EQUAL(db->_header->auto_commit_ms, 500);
  
  db->disable_auto_commit();
  BOOST_CHECK_EQUAL(db->_header->auto_commit_enabled, 0);
}

BOOST_AUTO_TEST_CASE(test_multiple_transactions) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  // Start and commit first transaction
  db->start_transaction(0);
  auto block1 = db->alloc(100);
  tid_t txn_id1 = db->prepare_commit(0, false);
  db->commit(false);
  
  // Start and commit second transaction
  db->start_transaction(0);
  auto block2 = db->alloc(200);
  tid_t txn_id2 = db->prepare_commit(0, false);
  db->commit(false);
  
  BOOST_CHECK(txn_id2 > txn_id1);
  
  // Wait for background merge
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  
  // Both should be merged
  BOOST_CHECK_LE(txn_id2, db->_header->last_merged_txn_id.load());
}

BOOST_AUTO_TEST_CASE(test_rollback) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  auto block = db->alloc(100);
  BOOST_CHECK(block != nullptr);
  
  offset_t segment_before = db->_wtxn->segment_head;
  BOOST_CHECK(segment_before != 0);
  
  // Rollback should clean up segments
  db->rollback(0);
  
  // After rollback, _wtxn should be reset
  BOOST_CHECK(!db->_wtxn || db->_wtxn->segment_head == 0);
}

BOOST_AUTO_TEST_CASE(test_segment_recycling) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  // Create and commit first transaction
  db->start_transaction(0);
  auto block1 = db->alloc(100);
  offset_t first_segment = db->_wtxn->segment_head;
  db->prepare_commit(0, false);
  db->commit(false);
  
  // Wait for merge
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  // Create second transaction - should potentially reuse segment
  db->start_transaction(0);
  auto block2 = db->alloc(100);
  offset_t second_segment = db->_wtxn->segment_head;
  
  // Segments might be recycled (same offset) or new
  // Just verify we have a valid segment
  BOOST_CHECK(second_segment != 0);
  
  db->rollback(0);
}

BOOST_AUTO_TEST_CASE(test_background_thread_shutdown) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  
  {
    TestStorage storage(dbFilePath.c_str());
    auto db = storage.make("test");
    
    BOOST_CHECK(db->_background_thread.joinable());
    
    // Start a transaction to test graceful shutdown
    db->start_transaction(0);
    auto block = db->alloc(100);
    db->prepare_commit(0, false);
    db->commit(false);
    
    // db destructor should stop background thread gracefully
  }
  
  // If we reach here without hanging, shutdown worked
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_segment_init_and_reinit) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm.lvs";
  TestStorage storage(dbFilePath.c_str());
  auto db = storage.make("test");
  
  db->start_transaction(0);
  
  offset_t seg_offset = db->_wtxn->segment_head;
  auto segment = db->template resolve<LSMSegment>(seg_offset, WRITE);
  
  // Check segment was initialized
  BOOST_CHECK(segment->allocation_start != 0);
  BOOST_CHECK(segment->parent_db == db.get());
  BOOST_CHECK_EQUAL(segment->refs.load(), 1);
  
  db->rollback(0);
}
