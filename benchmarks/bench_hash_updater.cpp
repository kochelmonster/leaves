// Benchmark: _HashUpdater performance across thread counts
//
// Measures time to sync hash trie from data trie using:
//   - Inline executor (single-threaded baseline)
//   - _PoolExecutor with 1, 2, 4, 8 threads
//
// Uses MapStorage + HashDB adapter (non-transactional),
// matching the production call path in _ReplicationDB::acquire_hash_trie.
//
// Build:  cmake --build build -j --target bench_hash_updater
// Run:    ./build/bench_hash_updater [--num=100000] [--vsize=100]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "leaves/mmap.hpp"
#include "leaves/intern/replication/_replication_db.hpp"
#include "leaves/intern/replication/_hash.hpp"
#include "leaves/intern/util/_threadpool.hpp"

using namespace leaves;

static int FLAGS_num = 100000;
static int FLAGS_vsize = 100;
static int FLAGS_max_threads = 5;
static int FLAGS_iterations = 3;
static bool FLAGS_profile = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct BenchPool : _ThreadPoolMixin<BenchPool> {
  explicit BenchPool(size_t n) : _ThreadPoolMixin(n) {}
};

// Type aliases — use MapStorage to get the real HashDB adapter
using RStorage = MapStorage;
using RDB = _ReplicationDB<RStorage::StorageImpl>;  // _ReplicationDB
using HashDB = RDB::HashDB;
using CTraits = RDB::CursorTraits;
using offset_e = typename RDB::offset_e;
using DataTrieNode = _TrieNode<CTraits>;
using DataLeafNode = _LeafNode<CTraits>;
using HashTraits = HashTrieTraits<CTraits>;
using HashTrieNode = _TrieNode<HashTraits>;
using HashLeafNode = _LeafNode<HashTraits>;

// ---------------------------------------------------------------------------
// Populate data trie with N keys
// ---------------------------------------------------------------------------

static void populate(TDB<RStorage, _ReplicationDB>& db, int n, int vsize) {
  std::string val(vsize, 'x');
  for (int i = 0; i < n; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key/%08d", i);
    auto cursor = db.cursor();
    cursor.find(buf);
    cursor.value(val);
    cursor.commit();
  }
}

// ---------------------------------------------------------------------------
// Run hash update — inline (single-threaded, no transaction)
// ---------------------------------------------------------------------------

static double bench_inline(RDB* rdb, offset_e data_root,
                           offset_e* hash_root) {
  auto hdb = rdb->hash_db();
  double start = now_seconds();
  update_hash_trie(rdb, &hdb, data_root, hash_root);
  return now_seconds() - start;
}

// ---------------------------------------------------------------------------
// Run hash update — pooled (N threads, no transaction)
// ---------------------------------------------------------------------------

static double bench_pooled(RDB* rdb, offset_e data_root,
                           offset_e* hash_root, int threads) {
  auto hdb = rdb->hash_db();
  BenchPool pool(threads);
  _PoolExecutor exec(pool);

  double start = now_seconds();
  update_hash_trie(exec, rdb, &hdb, data_root, hash_root);
  double elapsed = now_seconds() - start;
  pool.wait_all();
  return elapsed;
}

// ---------------------------------------------------------------------------
// Cost breakdown — measure individual operations in isolation
// ---------------------------------------------------------------------------

static void count_nodes(RDB* rdb, offset_e offset,
                        int& trie_count, int& leaf_count) {
  if (!offset) return;
  if (offset.type() == LEAF) {
    leaf_count++;
  } else {
    trie_count++;
    auto trie = rdb->template resolve<DataTrieNode>(&offset);
    trie->for_each_branch([&](int k, auto* off) {
      count_nodes(rdb, *off, trie_count, leaf_count);
    });
  }
}

static void profile_breakdown(RDB* rdb, offset_e data_root) {
  int trie_count = 0, leaf_count = 0;
  count_nodes(rdb, data_root, trie_count, leaf_count);
  int total_nodes = trie_count + leaf_count;

  fprintf(stdout, "\nCost breakdown (data trie: %d tries, %d leaves, %d total)\n",
          trie_count, leaf_count, total_nodes);
  fprintf(stdout, "%-35s %10s %10s %8s\n",
          "Component", "Time(ms)", "Per-node", "% Total");
  fprintf(stdout, "%-35s %10s %10s %8s\n",
          "-----------------------------------", "----------", "----------", "--------");

  // 1. Blake3 hashing cost: hash N random payloads
  {
    std::string key_path = "key/00000000";
    std::string val(FLAGS_vsize, 'x');
    double t0 = now_seconds();
    for (int i = 0; i < total_nodes; i++) {
      Blake3Hasher hasher;
      hasher.update(key_path.data(), key_path.size());
      hasher.update(val.data(), val.size());
      uint8_t hash[32];
      hasher.finalize(hash);
    }
    double dt = now_seconds() - t0;
    fprintf(stdout, "%-35s %10.2f %8.0fns %7.1f%%\n",
            "BLAKE3 hashing", dt * 1000,
            dt / total_nodes * 1e9, 0.0);
  }

  // 2. Memory allocation cost: alloc + free N pages via HashDB (non-transactional)
  {
    using page_ptr = typename RDB::Traits::ptr;
    using PageHeader = typename RDB::Traits::PageHeader;
    auto hdb = rdb->hash_db();
    uint16_t leaf_size = sizeof(HashLeafNode);
    std::vector<page_ptr> pages(total_nodes);

    double t0 = now_seconds();
    for (int i = 0; i < total_nodes; i++) {
      pages[i] = hdb.template alloc_node<page_ptr>(leaf_size);
    }
    double alloc_dt = now_seconds() - t0;

    double t1 = now_seconds();
    for (int i = 0; i < total_nodes; i++) {
      hdb.free(pages[i]);
    }
    double free_dt = now_seconds() - t1;

    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "  alloc_node", alloc_dt * 1000,
            alloc_dt / total_nodes * 1e9);
    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "  free (page)", free_dt * 1000,
            free_dt / total_nodes * 1e9);
    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "Memory alloc+free total", (alloc_dt + free_dt) * 1000,
            (alloc_dt + free_dt) / total_nodes * 1e9);
  }

  // 3. Trie traversal cost: walk all nodes (resolve + branch iteration)
  {
    volatile int sink = 0;
    double t0 = now_seconds();
    // Walk 3x to get measurable time
    for (int rep = 0; rep < 3; rep++) {
      std::vector<offset_e> stack;
      stack.reserve(total_nodes);
      stack.push_back(data_root);
      while (!stack.empty()) {
        offset_e off = stack.back();
        stack.pop_back();
        if (!off) continue;
        if (off.type() == LEAF) {
          auto leaf = rdb->template resolve<DataLeafNode>(&off);
          sink += leaf->key_size;
        } else {
          auto trie = rdb->template resolve<DataTrieNode>(&off);
          sink += trie->len();
          trie->for_each_branch([&](int k, auto* branch_off) {
            stack.push_back(*branch_off);
          });
        }
      }
    }
    double dt = (now_seconds() - t0) / 3.0;
    (void)sink;
    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "Trie traversal (resolve+iterate)", dt * 1000,
            dt / total_nodes * 1e9);
  }

  // 4. String operations cost: append + resize for key_path
  {
    std::string key_path;
    key_path.reserve(256);
    double t0 = now_seconds();
    for (int i = 0; i < total_nodes; i++) {
      size_t saved = key_path.size();
      key_path.append("key/00000000", 12);
      key_path.resize(saved);
    }
    double dt = now_seconds() - t0;
    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "String ops (append+resize)", dt * 1000,
            dt / total_nodes * 1e9);
  }

  // 5. Full hash update for reference (non-transactional, production path)
  {
    auto hdb = rdb->hash_db();
    offset_e hash_root{};
    double t0 = now_seconds();
    update_hash_trie(rdb, &hdb, data_root, &hash_root);
    double dt = now_seconds() - t0;
    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "Full HashUpdater (reference)", dt * 1000,
            dt / total_nodes * 1e9);
  }

  fprintf(stdout, "\n");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void parse_flags(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (sscanf(argv[i], "--num=%d", &FLAGS_num) == 1) continue;
    if (sscanf(argv[i], "--vsize=%d", &FLAGS_vsize) == 1) continue;
    if (sscanf(argv[i], "--max_threads=%d", &FLAGS_max_threads) == 1) continue;
    if (sscanf(argv[i], "--iterations=%d", &FLAGS_iterations) == 1) continue;
    if (strcmp(argv[i], "--profile") == 0) { FLAGS_profile = true; continue; }
    fprintf(stderr, "Unknown flag: %s\n", argv[i]);
    exit(1);
  }
}

int main(int argc, char** argv) {
  parse_flags(argc, argv);

  const char* db_path = "bench_hash_updater.lvs";
  std::remove(db_path);

  fprintf(stdout, "HashUpdater benchmark\n");
  fprintf(stdout, "  keys:       %d\n", FLAGS_num);
  fprintf(stdout, "  value_size: %d\n", FLAGS_vsize);
  fprintf(stdout, "  iterations: %d\n\n", FLAGS_iterations);

  // --- Setup: populate data trie ---
  auto storage = RStorage::create(db_path);
  {
  auto db = storage->open<_ReplicationDB>("bench");
  populate(db, FLAGS_num, FLAGS_vsize);

  auto* rdb = db._internal();
  auto txn = rdb->txn();
  auto data_root = txn->root;

  if (FLAGS_profile) {
    profile_breakdown(rdb, data_root);
  }

  fprintf(stdout, "%-20s %10s %10s %10s\n",
          "Configuration", "Best(ms)", "Avg(ms)", "Speedup");
  fprintf(stdout, "%-20s %10s %10s %10s\n",
          "--------------------", "----------", "----------", "----------");

  // --- Inline baseline ---
  double inline_best = 1e9;
  double inline_total = 0;
  for (int it = 0; it < FLAGS_iterations; it++) {
    offset_e hash_root{};
    double t = bench_inline(rdb, data_root, &hash_root);
    if (t < inline_best) inline_best = t;
    inline_total += t;
  }
  double inline_avg = inline_total / FLAGS_iterations;
  fprintf(stdout, "%-20s %10.1f %10.1f %10s\n",
          "inline (1 thread)", inline_best * 1000, inline_avg * 1000, "1.00x");

  // --- Pooled with varying thread counts ---
  for (int threads = 1; threads <= FLAGS_max_threads; threads++) {
    double best = 1e9;
    double total = 0;
    for (int it = 0; it < FLAGS_iterations; it++) {
      offset_e hash_root{};
      double t = bench_pooled(rdb, data_root, &hash_root, threads);
      if (t < best) best = t;
      total += t;
    }
    double avg = total / FLAGS_iterations;
    char label[32];
    snprintf(label, sizeof(label), "pool (%d threads)", threads);
    fprintf(stdout, "%-20s %10.1f %10.1f %9.2fx\n",
            label, best * 1000, avg * 1000, inline_best / best);
  }
  } // drop db

  fprintf(stdout, "\n");
  storage.reset();
  std::remove(db_path);
  return 0;
}
