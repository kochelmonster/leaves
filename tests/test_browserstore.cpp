#ifndef TESTING
#error "TESTING must be defined"
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "leaves/intern/storage/_browserstore.hpp"

using namespace leaves;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_ASSERT(cond, msg)                                          \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("  FAIL: %s (line %d)\n", msg, __LINE__);                  \
      return false;                                                     \
    }                                                                   \
  } while (0)

#define RUN_TEST(fn)                                                    \
  do {                                                                  \
    tests_run++;                                                        \
    printf("Running %s...\n", #fn);                                     \
    if (fn()) {                                                         \
      tests_passed++;                                                   \
      printf("  OK\n");                                                 \
    } else {                                                            \
      printf("  FAILED\n");                                             \
    }                                                                   \
  } while (0)

// ── Test: create a fresh store and verify header ─────────────────────
static bool test_init() {
  _BrowserStore store("test_init_db", 16, 10 * M, 0);

  TEST_ASSERT(store._header != nullptr, "header should be non-null");
  TEST_ASSERT(
      strcmp(store._header->signature, BROWSERSTORE_SIGNATURE) == 0,
      "signature must match");
  TEST_ASSERT(store._header->db_count == 16, "db_count must match");
  TEST_ASSERT(store._header->db_version == 0, "initial version is 0");
  TEST_ASSERT(store._header->file_size > 0, "file_size > 0");

  return true;
}

// ── Test: reopen an existing store ───────────────────────────────────
static bool test_reopen() {
  {
    _BrowserStore store("test_reopen_db", 8, 10 * M, 0);
    // insert something so the header is persisted
    auto* db = store["mydb"];
    auto cursor = db->create_cursor();
    cursor->find(Slice("hello"));
    cursor->value(Slice("world"));
  }

  // Reopen the same IDB database
  _BrowserStore store("test_reopen_db", 8, 10 * M, 0);
  TEST_ASSERT(store._header != nullptr, "header non-null after reopen");
  TEST_ASSERT(
      strcmp(store._header->signature, BROWSERSTORE_SIGNATURE) == 0,
      "signature valid after reopen");
  TEST_ASSERT(store._header->db_count == 8, "db_count preserved");

  return true;
}

// ── Test: wrong db_count on reopen ───────────────────────────────────
static bool test_wrong_db_count() {
  {
    _BrowserStore store("test_wrong_count_db", 10, 10 * M, 0);
  }

  bool threw = false;
  try {
    _BrowserStore store("test_wrong_count_db", 20, 10 * M, 0);
  } catch (const WrongValue&) {
    threw = true;
  }
  TEST_ASSERT(threw, "mismatched db_count must throw WrongValue");

  return true;
}

// ── Test: single put / get ───────────────────────────────────────────
static bool test_put_get() {
  _BrowserStore store("test_put_get_db", 16, 10 * M, 0);
  auto* db = store["collection"];
  auto cursor = db->create_cursor();

  cursor->find(Slice("key1"));
  cursor->value(Slice("value1"));

  cursor->find(Slice("key1"));
  TEST_ASSERT(cursor->is_valid(), "key1 must be found");
  Slice v = cursor->value();
  TEST_ASSERT(v == Slice("value1"), "value must match");

  return true;
}

// ── Test: overwrite existing key ─────────────────────────────────────
static bool test_overwrite() {
  _BrowserStore store("test_overwrite_db", 16, 10 * M, 0);
  auto* db = store["data"];
  auto cursor = db->create_cursor();

  cursor->find(Slice("ow"));
  cursor->value(Slice("first"));

  cursor->find(Slice("ow"));
  cursor->value(Slice("second"));

  cursor->find(Slice("ow"));
  TEST_ASSERT(cursor->is_valid(), "overwritten key must be found");
  TEST_ASSERT(cursor->value() == Slice("second"),
              "value must be the latest");

  return true;
}

// ── Test: delete a key ───────────────────────────────────────────────
static bool test_remove() {
  _BrowserStore store("test_remove_db", 16, 10 * M, 0);
  auto* db = store["rm"];
  auto cursor = db->create_cursor();

  cursor->find(Slice("delme"));
  cursor->value(Slice("gone"));

  cursor->find(Slice("delme"));
  TEST_ASSERT(cursor->is_valid(), "key exists before remove");
  cursor->remove();

  cursor->find(Slice("delme"));
  TEST_ASSERT(!cursor->is_valid(), "key must be gone after remove");

  return true;
}

// ── Test: multiple keys with iteration ───────────────────────────────
static bool test_iteration() {
  _BrowserStore store("test_iter_db", 16, 10 * M, 0);
  auto* db = store["iter"];
  auto cursor = db->create_cursor();

  // Insert keys in non-sorted order; trie keeps them sorted
  const char* keys[] = {"cherry", "apple", "banana", "date"};
  for (auto k : keys) {
    cursor->find(Slice(k));
    cursor->value(Slice(k));  // value = key for simplicity
  }

  // Forward iteration – must see sorted order
  std::vector<std::string> forward;
  for (cursor->first(); cursor->is_valid(); cursor->next()) {
    forward.push_back(cursor->key().string());
  }

  TEST_ASSERT(forward.size() == 4, "must see 4 keys");
  TEST_ASSERT(forward[0] == "apple", "first is apple");
  TEST_ASSERT(forward[1] == "banana", "second is banana");
  TEST_ASSERT(forward[2] == "cherry", "third is cherry");
  TEST_ASSERT(forward[3] == "date", "fourth is date");

  return true;
}

// ── Test: bulk insert ────────────────────────────────────────────────
static bool test_bulk_insert() {
  _BrowserStore store("test_bulk_db", 16, 50 * M, 0);
  auto* db = store["bulk"];
  auto cursor = db->create_cursor();

  constexpr int N = 500;
  char key[32], val[64];

  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof(key), "%08d", i);
    snprintf(val, sizeof(val), "v-%08d", i);
    cursor->find(Slice(key));
    cursor->value(Slice(val));
  }

  // Verify all keys
  int count = 0;
  for (cursor->first(); cursor->is_valid(); cursor->next()) {
    count++;
  }
  TEST_ASSERT(count == N, "all 500 keys must be present");

  // Spot-check a specific key
  snprintf(key, sizeof(key), "%08d", 250);
  snprintf(val, sizeof(val), "v-%08d", 250);
  cursor->find(Slice(key));
  TEST_ASSERT(cursor->is_valid(), "key 250 must exist");
  TEST_ASSERT(cursor->value() == Slice(val), "value 250 must match");

  return true;
}

// ── Test: multiple DBs in one store ──────────────────────────────────
static bool test_multiple_dbs() {
  _BrowserStore store("test_multi_db", 16, 10 * M, 0);

  auto* db_a = store["alpha"];
  auto* db_b = store["beta"];

  auto ca = db_a->create_cursor();
  auto cb = db_b->create_cursor();

  ca->find(Slice("shared_key"));
  ca->value(Slice("from_alpha"));

  cb->find(Slice("shared_key"));
  cb->value(Slice("from_beta"));

  // Each DB keeps its own value
  ca->find(Slice("shared_key"));
  cb->find(Slice("shared_key"));
  TEST_ASSERT(ca->value() == Slice("from_alpha"), "alpha value isolated");
  TEST_ASSERT(cb->value() == Slice("from_beta"), "beta value isolated");

  return true;
}

int main() {
  printf("=== _BrowserStore test suite ===\n\n");

  RUN_TEST(test_init);
  RUN_TEST(test_reopen);
  RUN_TEST(test_wrong_db_count);
  RUN_TEST(test_put_get);
  RUN_TEST(test_overwrite);
  RUN_TEST(test_remove);
  RUN_TEST(test_iteration);
  RUN_TEST(test_bulk_insert);
  RUN_TEST(test_multiple_dbs);

  printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
