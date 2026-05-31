#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MetricsAspectsTest

#include <boost/test/included/unit_test.hpp>
#include <filesystem>
#include <string>

#ifndef TESTING
#define TESTING
#endif

#include "leaves/mmap.hpp"
#include "leaves/metrics.hpp"

using namespace leaves;

// =============================================================================
// Traits and storage types for each mixin combination under test
// =============================================================================

struct _OpsTraits : _MemoryMapTraits {
  using Aspect = _OperationAspect<>;
};

struct _TxnTraits : _MemoryMapTraits {
  using Aspect = _TransactionAspect<>;
};

struct _NavTraits : _MemoryMapTraits {
  using Aspect = _NavigationAspect<>;
};

struct _AllTraits : _MemoryMapTraits {
  using Aspect = _AllMetricsAspect<>;
};

// Composition: transaction + operation only
struct _TxnOpsTraits : _MemoryMapTraits {
  using Aspect = _TransactionAspect<_OperationAspect<>>;
};

using OpsMMap  = _MemoryMapFile<_OpsTraits>;
using TxnMMap  = _MemoryMapFile<_TxnTraits>;
using NavMMap  = _MemoryMapFile<_NavTraits>;
using AllMMap  = _MemoryMapFile<_AllTraits>;
using TxnOpsMMap = _MemoryMapFile<_TxnOpsTraits>;

// =============================================================================
// Test helper
// =============================================================================

struct TempDir {
  std::filesystem::path dir;

  TempDir(const char* name) {
    dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }

  std::string path(const char* file) const { return (dir / file).string(); }
};

// =============================================================================
// _OperationAspect tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(OperationAspectTests)

BOOST_AUTO_TEST_CASE(writes_and_bytes_written) {
  TempDir tmp("metrics_ops_write");
  OpsMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("key1"));
  cursor->value(Slice("hello"));   // 5 bytes
  cursor->find(Slice("key2"));
  cursor->value(Slice("world!"));  // 6 bytes
  cursor->commit();

  auto s = db->aspect().ops_snapshot();
  BOOST_CHECK_EQUAL(s.writes, 2u);
  BOOST_CHECK_EQUAL(s.bytes_written, 11u);
}

BOOST_AUTO_TEST_CASE(reads_and_bytes_read) {
  TempDir tmp("metrics_ops_read");
  OpsMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("k"));
  cursor->value(Slice("abc"));
  cursor->commit();

  // Fresh cursor for reads
  auto rc = db->create_cursor();
  rc->find(Slice("k"));
  BOOST_REQUIRE(rc->is_valid());
  Slice v = rc->value();
  (void)v;

  auto s = db->aspect().ops_snapshot();
  BOOST_CHECK_EQUAL(s.reads, 1u);
  BOOST_CHECK_EQUAL(s.bytes_read, 3u);
}

BOOST_AUTO_TEST_CASE(deletes_counted) {
  TempDir tmp("metrics_ops_delete");
  OpsMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("del_me"));
  cursor->value(Slice("v"));
  cursor->commit();

  cursor->find(Slice("del_me"));
  BOOST_REQUIRE(cursor->is_valid());
  cursor->remove();
  cursor->commit();

  auto s = db->aspect().ops_snapshot();
  BOOST_CHECK_EQUAL(s.deletes, 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// _TransactionAspect tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(TransactionAspectTests)

BOOST_AUTO_TEST_CASE(user_txn_started_and_committed) {
  TempDir tmp("metrics_txn_commit");
  TxnMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("a"));
  cursor->value(Slice("1"));
  cursor->commit();

  cursor->find(Slice("b"));
  cursor->value(Slice("2"));
  cursor->commit();

  auto s = db->aspect().txn_snapshot();
  BOOST_CHECK_EQUAL(s.user_txns_started, 2u);
  BOOST_CHECK_EQUAL(s.user_txns_committed, 2u);
  BOOST_CHECK_EQUAL(s.user_txns_rolled_back, 0u);
}

BOOST_AUTO_TEST_CASE(user_txn_rolled_back) {
  TempDir tmp("metrics_txn_rollback");
  TxnMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("x"));
  cursor->value(Slice("v"));
  // rollback instead of commit
  cursor->rollback();

  auto s = db->aspect().txn_snapshot();
  BOOST_CHECK_EQUAL(s.user_txns_started, 1u);
  BOOST_CHECK_EQUAL(s.user_txns_rolled_back, 1u);
  BOOST_CHECK_EQUAL(s.user_txns_committed, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// _NavigationAspect tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(NavigationAspectTests)

BOOST_AUTO_TEST_CASE(finds_hit_and_miss) {
  TempDir tmp("metrics_nav_find");
  NavMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("present"));
  cursor->value(Slice("yes"));
  cursor->commit();

  auto rc = db->create_cursor();
  rc->find(Slice("present"));   // hit
  rc->find(Slice("absent"));    // miss

  auto s = db->aspect().nav_snapshot();
  // write cursor calls find("present") = 1 miss; read cursor: find("present") = 1 hit, find("absent") = 1 miss
  BOOST_CHECK_EQUAL(s.finds, 3u);
  BOOST_CHECK_EQUAL(s.finds_hit, 1u);
}

BOOST_AUTO_TEST_CASE(next_and_prev_counted) {
  TempDir tmp("metrics_nav_nextprev");
  NavMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  for (int i = 0; i < 3; ++i) {
    std::string k = "k" + std::to_string(i);
    cursor->find(Slice(k));
    cursor->value(Slice("v"));
  }
  cursor->commit();

  auto rc = db->create_cursor();
  rc->find(Slice("k0"));
  rc->next();   // k1
  rc->next();   // k2
  rc->prev();   // k1

  auto s = db->aspect().nav_snapshot();
  // write cursor: find(k0), find(k1), find(k2) = 3 finds; read cursor: find(k0) = 1 find
  BOOST_CHECK_EQUAL(s.finds, 4u);
  BOOST_CHECK_EQUAL(s.navigations, 3u);  // 2 next + 1 prev
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Composition test: _TransactionAspect<_OperationAspect<>>
// — verify both layers fire simultaneously
// =============================================================================

BOOST_AUTO_TEST_SUITE(CompositionTests)

BOOST_AUTO_TEST_CASE(txn_and_ops_both_counted) {
  TempDir tmp("metrics_compose");
  TxnOpsMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("key"));
  cursor->value(Slice("value"));
  cursor->commit();

  auto& a = db->aspect();
  BOOST_CHECK_EQUAL(a.writes, 1u);
  BOOST_CHECK_EQUAL(a.user_txns_committed, 1u);
  BOOST_CHECK_EQUAL(a.user_txns_started, 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// _AllMetricsAspect integration test
// — all snapshot methods available on one DB
// =============================================================================

BOOST_AUTO_TEST_SUITE(AllMetricsTests)

BOOST_AUTO_TEST_CASE(all_snapshots_accessible) {
  TempDir tmp("metrics_all");
  AllMMap storage(tmp.path("db.lvs").c_str());
  auto* db = storage.open("test");

  auto cursor = db->create_cursor();
  cursor->find(Slice("foo"));
  cursor->value(Slice("bar"));
  cursor->commit();

  cursor->find(Slice("foo"));
  BOOST_REQUIRE(cursor->is_valid());
  (void)cursor->value();

  auto& a = db->aspect();

  auto ops   = a.ops_snapshot();
  auto txns  = a.txn_snapshot();
  auto nav   = a.nav_snapshot();

  BOOST_CHECK_EQUAL(ops.writes, 1u);
  BOOST_CHECK_EQUAL(ops.reads, 1u);
  BOOST_CHECK_EQUAL(txns.user_txns_committed, 1u);
  BOOST_CHECK_EQUAL(nav.finds_hit, 1u);
}

BOOST_AUTO_TEST_CASE(independent_dbs_have_independent_counters) {
  TempDir tmp("metrics_independent");
  AllMMap storage(tmp.path("db.lvs").c_str());
  auto* db1 = storage.open("db1");
  auto* db2 = storage.open("db2");

  auto c1 = db1->create_cursor();
  c1->find(Slice("k"));
  c1->value(Slice("v"));
  c1->commit();

  // db2 gets no writes
  BOOST_CHECK_EQUAL(db1->aspect().writes, 1u);
  BOOST_CHECK_EQUAL(db2->aspect().writes, 0u);
}

BOOST_AUTO_TEST_SUITE_END()
