// Benchmark: _Merger performance across thread counts
//
// Measures time to merge source trie into destination trie using:
//   - Inline executor (single-threaded baseline)
//   - _PoolExecutor with 1 through 8 threads
//
// Uses MapStorage (mmap-backed), matching the production call path.
//
// Build:  cmake --build build -j --target bench_merger
// Run:    ./build/bench_merger [--num=100000] [--vsize=100]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "leaves/mmap.hpp"
#include "leaves/intern/db/_cursor.hpp"
#include "leaves/intern/util/_merger.hpp"
#include "leaves/intern/util/_threadpool.hpp"

using namespace leaves;

static int FLAGS_num = 100000;
static int FLAGS_vsize = 100;
static int FLAGS_max_threads = 5;
static int FLAGS_iterations = 3;
static bool FLAGS_profile = false;
static bool FLAGS_overlap = false;

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

using Storage = MapStorage;
using InternalDB = Storage::StorageImpl::DB;
using CTraits = InternalDB::CursorTraits;
using DstCursor = _TransactionalCursor<CTraits>;
using SrcCursor = _Cursor<CTraits>;
using TrieNode = _TrieNode<CTraits>;
using LeafNode = _LeafNode<CTraits>;
using offset_e = typename InternalDB::offset_e;

// ---------------------------------------------------------------------------
// Populate data trie with N keys
// ---------------------------------------------------------------------------

static void populate(Storage::DB& db, int n, int vsize,
                     const char* prefix = "key/") {
  std::string val(vsize, 'x');
  auto cursor = db.cursor();
  for (int i = 0; i < n; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%08d", prefix, i);
    cursor.find(buf);
    cursor.value(val);
  }
  cursor.commit();
}

// ---------------------------------------------------------------------------
// Count nodes in data trie
// ---------------------------------------------------------------------------

static void count_nodes(InternalDB* db, offset_e offset,
                        int& trie_count, int& leaf_count) {
  if (!offset) return;
  if (offset.type() == LEAF) {
    leaf_count++;
  } else {
    trie_count++;
    auto trie = db->template resolve<TrieNode>(&offset);
    trie->for_each_branch([&](int k, auto* off) {
      count_nodes(db, *off, trie_count, leaf_count);
    });
  }
}

// ---------------------------------------------------------------------------
// Run merge — inline (single-threaded)
// ---------------------------------------------------------------------------

static double bench_inline(InternalDB* dst_db, InternalDB* src_db) {
  auto src_root = &src_db->txn()->root;
  auto dst_root = &dst_db->txn()->root;
  uint64_t cursor_id = dst_db->new_cursor_id();

  DstCursor dst_cursor(dst_db, dst_root);
  SrcCursor src_cursor(src_db, src_root);
  src_cursor.clear();
  dst_cursor.start_transaction();

  StandardMergePolicy handler;
  _Merger<DstCursor, SrcCursor, StandardMergePolicy> merger(
      dst_cursor, src_cursor, handler);

  double start = now_seconds();
  merger.exec();
  double elapsed = now_seconds() - start;

  dst_cursor.commit(cursor_id);
  return elapsed;
}

// ---------------------------------------------------------------------------
// Run merge — pooled (N threads)
// ---------------------------------------------------------------------------

static double bench_pooled(InternalDB* dst_db, InternalDB* src_db,
                           int threads) {
  auto src_root = &src_db->txn()->root;
  auto dst_root = &dst_db->txn()->root;
  uint64_t cursor_id = dst_db->new_cursor_id();

  DstCursor dst_cursor(dst_db, dst_root);
  SrcCursor src_cursor(src_db, src_root);
  src_cursor.clear();
  dst_cursor.start_transaction();

  StandardMergePolicy handler;
  BenchPool pool(threads);
  _PoolExecutor exec(pool);

  _Merger<DstCursor, SrcCursor, StandardMergePolicy, _PoolExecutor> merger(
      dst_cursor, src_cursor, handler);
  _TaskGroup<_PoolExecutor> tg(exec);
  merger._tg = &tg;

  double start = now_seconds();
  merger.exec();
  double elapsed = now_seconds() - start;

  dst_cursor.commit(cursor_id);
  return elapsed;
}

// ---------------------------------------------------------------------------
// Cost breakdown — measure individual operations in isolation
// ---------------------------------------------------------------------------

static void profile_breakdown(InternalDB* src_db, offset_e data_root) {
  int trie_count = 0, leaf_count = 0;
  count_nodes(src_db, data_root, trie_count, leaf_count);
  int total_nodes = trie_count + leaf_count;

  fprintf(stdout,
          "\nCost breakdown (source trie: %d tries, %d leaves, %d total)\n",
          trie_count, leaf_count, total_nodes);
  fprintf(stdout, "%-35s %10s %10s\n", "Component", "Time(ms)", "Per-node");
  fprintf(stdout, "%-35s %10s %10s\n",
          "-----------------------------------", "----------", "----------");

  // 1. Trie traversal cost: walk all source nodes
  {
    volatile int sink = 0;
    double t0 = now_seconds();
    for (int rep = 0; rep < 3; rep++) {
      std::vector<offset_e> stack;
      stack.reserve(total_nodes);
      stack.push_back(data_root);
      while (!stack.empty()) {
        offset_e off = stack.back();
        stack.pop_back();
        if (!off) continue;
        if (off.type() == LEAF) {
          auto leaf = src_db->template resolve<LeafNode>(&off);
          sink += leaf->key_size;
        } else {
          auto trie = src_db->template resolve<TrieNode>(&off);
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

  // 2. Memcpy cost: copy value data
  {
    std::string val(FLAGS_vsize, 'x');
    std::string dst(FLAGS_vsize, '\0');
    double t0 = now_seconds();
    for (int i = 0; i < leaf_count; i++) {
      memcpy(&dst[0], val.data(), FLAGS_vsize);
    }
    double dt = now_seconds() - t0;
    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "Memcpy (value data)", dt * 1000,
            dt / leaf_count * 1e9);
  }

  // 3. String operations cost: append + resize for key tracking
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

  // 4. Allocation cost (alloc + free) — no data, just pages
  {
    const char* alloc_path = "bench_merger_profile_alloc.lvs";
    std::remove(alloc_path);

    double dt;
    {
      auto alloc_storage = Storage::create(alloc_path);
      auto alloc_db_handle = (*alloc_storage)["prof"];
      auto* alloc_db = alloc_db_handle._internal();
      uint64_t cid = alloc_db->new_cursor_id();

      DstCursor dc(alloc_db, &alloc_db->txn()->root);
      dc.start_transaction();

      using page_ptr = typename InternalDB::page_ptr;
      using PageHeader = typename CTraits::PageHeader;

      // Pre-allocate vector for pages
      std::vector<page_ptr> pages;
      pages.reserve(total_nodes);

      double t0 = now_seconds();
      // Allocate leaf_count leaf-sized pages + trie_count trie-sized pages
      uint16_t leaf_size = 12 + FLAGS_vsize + 100;  // approximate leaf size
      uint16_t trie_size = 64;  // approximate small trie

      for (int i = 0; i < leaf_count; i++) {
        pages.push_back(alloc_db->alloc_page(leaf_size));
      }
      for (int i = 0; i < trie_count; i++) {
        pages.push_back(alloc_db->alloc_page(trie_size));
      }
      dt = now_seconds() - t0;

      dc.commit(cid);
    }

    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "Allocation only (alloc pages)", dt * 1000,
            dt / total_nodes * 1e9);
    std::remove(alloc_path);
  }

  // 5. Full inline merge for reference
  {
    const char* ref_path = "bench_merger_profile_ref.lvs";
    std::remove(ref_path);

    double dt;
    {
      auto ref_storage = Storage::create(ref_path);
      auto ref_db_handle = (*ref_storage)["prof"];
      auto* ref_db = ref_db_handle._internal();

      double t0 = now_seconds();
      auto src_root_ptr = &src_db->txn()->root;
      auto dst_root_ptr = &ref_db->txn()->root;
      uint64_t cid = ref_db->new_cursor_id();
      DstCursor dc(ref_db, dst_root_ptr);
      SrcCursor sc(src_db, src_root_ptr);
      sc.clear();
      dc.start_transaction();
      StandardMergePolicy h;
      _Merger<DstCursor, SrcCursor, StandardMergePolicy> m(dc, sc, h);
      m.exec();
      dc.commit(cid);
      dt = now_seconds() - t0;
    }

    fprintf(stdout, "%-35s %10.2f %8.0fns\n",
            "Full inline merge (reference)", dt * 1000,
            dt / total_nodes * 1e9);

    std::remove(ref_path);
  }

  fprintf(stdout, "\n");
}

// ---------------------------------------------------------------------------
// Verify merge correctness: count keys in destination
// ---------------------------------------------------------------------------

static int count_keys(Storage::DB& db) {
  auto cursor = db.cursor();
  cursor.first();
  int count = 0;
  while (cursor.is_valid()) {
    count++;
    cursor.next();
  }
  return count;
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
    if (strcmp(argv[i], "--overlap") == 0) { FLAGS_overlap = true; continue; }
    fprintf(stderr, "Unknown flag: %s\n", argv[i]);
    exit(1);
  }
}

int main(int argc, char** argv) {
  parse_flags(argc, argv);

  const char* src_path = "bench_merger_src.lvs";
  const char* dst_path = "bench_merger_dst.lvs";
  std::remove(src_path);
  std::remove(dst_path);

  fprintf(stdout, "Merger benchmark\n");
  fprintf(stdout, "  keys:       %d\n", FLAGS_num);
  fprintf(stdout, "  value_size: %d\n", FLAGS_vsize);
  fprintf(stdout, "  iterations: %d\n", FLAGS_iterations);
  fprintf(stdout, "  overlap:    %s\n\n", FLAGS_overlap ? "yes" : "no");

  // --- Setup: populate source trie ---
  auto src_storage = Storage::create(src_path);
  auto src_db_handle = (*src_storage)["bench"];
  {
    fprintf(stdout, "Populating source with %d keys...\n", FLAGS_num);
    double t0 = now_seconds();
    populate(src_db_handle, FLAGS_num, FLAGS_vsize);
    fprintf(stdout, "  done in %.1f ms\n\n", (now_seconds() - t0) * 1000);
  }

  auto* src_internal = src_db_handle._internal();
  auto data_root = src_internal->txn()->root;

  // Count nodes
  {
    int trie_count = 0, leaf_count = 0;
    count_nodes(src_internal, data_root, trie_count, leaf_count);
    fprintf(stdout, "Source trie: %d trie nodes, %d leaves, %d total\n\n",
            trie_count, leaf_count, trie_count + leaf_count);
  }

  if (FLAGS_profile) {
    profile_breakdown(src_internal, data_root);
  }

  fprintf(stdout, "%-20s %10s %10s %10s\n",
          "Configuration", "Best(ms)", "Avg(ms)", "Speedup");
  fprintf(stdout, "%-20s %10s %10s %10s\n",
          "--------------------", "----------", "----------", "----------");

  // --- Inline baseline ---
  double inline_best = 1e9;
  double inline_total = 0;
  for (int it = 0; it < FLAGS_iterations; it++) {
    std::remove(dst_path);
    auto dst_storage = Storage::create(dst_path);
    {
      auto dst_db = (*dst_storage)["bench"];
      if (FLAGS_overlap) {
        populate(dst_db, FLAGS_num / 2, FLAGS_vsize);
      }

      double t = bench_inline(dst_db._internal(), src_internal);
      if (t < inline_best) inline_best = t;
      inline_total += t;

      if (it == 0) {
        int got = count_keys(dst_db);
        if (got != FLAGS_num) {
          fprintf(stderr, "ERROR: inline merge produced %d keys, expected %d\n",
                  got, FLAGS_num);
        }
      }
    } // dst_db destroyed before storage
  }
  double inline_avg = inline_total / FLAGS_iterations;
  fprintf(stdout, "%-20s %10.1f %10.1f %10s\n",
          "inline (1 thread)", inline_best * 1000, inline_avg * 1000, "1.00x");

  // --- Pooled with varying thread counts ---
  for (int threads = 1; threads <= FLAGS_max_threads; threads++) {
    double best = 1e9;
    double total = 0;
    for (int it = 0; it < FLAGS_iterations; it++) {
      std::remove(dst_path);
      auto dst_storage = Storage::create(dst_path);
      {
        auto dst_db = (*dst_storage)["bench"];
        if (FLAGS_overlap) {
          populate(dst_db, FLAGS_num / 2, FLAGS_vsize);
        }

        double t = bench_pooled(dst_db._internal(), src_internal, threads);
        if (t < best) best = t;
        total += t;

        if (it == 0) {
          int got = count_keys(dst_db);
          if (got != FLAGS_num) {
            fprintf(stderr,
                    "ERROR: pool(%d) merge produced %d keys, expected %d\n",
                    threads, got, FLAGS_num);
          }
        }
      } // dst_db destroyed before storage
    }
    double avg = total / FLAGS_iterations;
    char label[32];
    snprintf(label, sizeof(label), "pool (%d threads)", threads);
    fprintf(stdout, "%-20s %10.1f %10.1f %9.2fx\n",
            label, best * 1000, avg * 1000, inline_best / best);
  }

  fprintf(stdout, "\n");
  return 0;
  // src_db_handle, src_storage destroyed here by stack unwinding;
  // files cleaned up by storage destructors
}
