#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE tributary
#include <boost/test/included/unit_test.hpp>

#include "../include/leaves/intern/multi/_confluence_db.hpp"
#include "test.hpp"

using namespace leaves;

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------
using StorageImpl  = MapStorage::StorageImpl;
using MainDB       = _DB<StorageImpl>;
using CDB          = _ConfluenceDB<MainDB>;
using TDB          = _TributaryDB<StorageImpl>;

struct TributaryPreparation {
  TributaryPreparation() { std::remove(TEST_FILE); }
  ~TributaryPreparation() {
    std::remove(TEST_FILE);
  }
};

// Helper: create a ConfluenceDB (no background monitor in tests)
using TestHandle = std::tuple<std::unique_ptr<StorageImpl>,
                              std::unique_ptr<MainDB>, CDB*>;
static TestHandle make_cdb(const char* path) {
  auto storage = std::make_unique<StorageImpl>(path);
  offset_t header{0};
  auto main_db = std::make_unique<MainDB>(*storage, &header, "main");
  auto* cdb = new CDB(*main_db, false, false);
  return {std::move(storage), std::move(main_db), cdb};
}

// Helper: open an existing ConfluenceDB
static TestHandle open_cdb(const char* path, offset_t header) {
  auto storage = std::make_unique<StorageImpl>(path);
  auto main_db = std::make_unique<MainDB>(*storage, header, "main");
  auto* cdb = new CDB(*main_db, false);
  return {std::move(storage), std::move(main_db), cdb};
}

// Helper: write a key/value pair via a ConfluenceCursor
static void write_kv(CDB& cdb, const std::string& key,
                     const std::string& value) {
  auto cursor = cdb.create_cursor();
  BOOST_REQUIRE(cursor->start_transaction());
  cursor->find(Slice(key));
  cursor->value(Slice(value));
  BOOST_REQUIRE(cursor->commit());
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(test_tributary_basic_write_read) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "hello", "world");

  // Read back without merging
  auto cursor = cdb->create_cursor();
  cursor->find(Slice("hello"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("world"));
}

BOOST_AUTO_TEST_CASE(test_tributary_two_producers) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Producer A
  write_kv(*cdb, "key_a", "value_a");
  // Producer B
  write_kv(*cdb, "key_b", "value_b");

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key_a"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("value_a"));
  cursor->find(Slice("key_b"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("value_b"));
}

BOOST_AUTO_TEST_CASE(test_tributary_overwrite_latest_wins) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Write first value (lower txn_id)
  write_kv(*cdb, "key", "old");
  // Write second value (higher txn_id)
  write_kv(*cdb, "key", "new");

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("new"));
}

BOOST_AUTO_TEST_CASE(test_tributary_delete_propagates) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "key", "value");

  // Delete the key
  {
    auto cursor = cdb->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("key"));
    BOOST_REQUIRE(cursor->is_valid());
    cursor->remove();
    BOOST_REQUIRE(cursor->commit());
  }

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_tributary_merge_into_main) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "alpha", "1");
  write_kv(*cdb, "beta",  "2");

  // Force merge all tributaries (set idle_timeout=0 so any written slot qualifies)
  cdb->set_idle_timeout_seconds(0);
  cdb->merge_eligible_tributaries();

  // After merge the data should be in the main DB
  auto cursor = cdb->create_cursor();
  cursor->find(Slice("alpha"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("1"));
  cursor->find(Slice("beta"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("2"));
}

BOOST_AUTO_TEST_CASE(test_tributary_delete_then_merge) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "k", "v");

  // Delete
  {
    auto cursor = cdb->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("k"));
    BOOST_REQUIRE(cursor->is_valid());
    cursor->remove();
    BOOST_REQUIRE(cursor->commit());
  }

  // Force merge — deletion should propagate to main DB
  cdb->set_idle_timeout_seconds(0);
  cdb->merge_eligible_tributaries();

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("k"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_tributary_iteration_forward) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "a", "1");
  write_kv(*cdb, "b", "2");
  write_kv(*cdb, "c", "3");

  auto cursor = cdb->create_cursor();
  cursor->first();
  BOOST_REQUIRE(cursor->is_valid());

  std::vector<std::string> keys;
  while (cursor->is_valid()) {
    keys.push_back(cursor->key().string());
    cursor->next();
  }

  BOOST_REQUIRE_EQUAL(keys.size(), 3u);
  BOOST_CHECK_EQUAL(keys[0], "a");
  BOOST_CHECK_EQUAL(keys[1], "b");
  BOOST_CHECK_EQUAL(keys[2], "c");
}

BOOST_AUTO_TEST_CASE(test_tributary_rollback) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Write something and rollback
  {
    auto cursor = cdb->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("x"));
    cursor->value(Slice("y"));
    cursor->rollback();
  }

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("x"));
  BOOST_CHECK(!cursor->is_valid());
}
