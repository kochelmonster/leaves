// Benchmark: RangeFilter scaling across thread counts
//
// Measures range filter scan performance using:
//   - Inline executor (single-threaded baseline)
//   - _PoolExecutor with 1, 2, 4, 8, ... threads
//   - Concurrent readers (multiple threads each running independent scans)
//
// Build:  cmake --build build -j --target bench_range_filter
// Run:    ./build/bench_range_filter [--num=100000] [--range=1000]
//                                   [--max_threads=8] [--iterations=5]
//                                   [--concurrent]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "leaves/filter.hpp"
#include "leaves/intern/storage/_memstore.hpp"
#include "leaves/intern/util/_threadpool.hpp"
#include "leaves/keycodec.hpp"

using namespace leaves;

static int FLAGS_num = 100000;
static int FLAGS_range = 1000;
static int FLAGS_max_threads = 8;
static int FLAGS_iterations = 5;
static bool FLAGS_concurrent = false;
static int FLAGS_readers = 0;  // 0 = auto (1..max_threads)

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

using DB = _MemoryStorage::DB;

// ---------------------------------------------------------------------------
// Populate index: encoded uint32 value -> "doc_<i>" key
// ---------------------------------------------------------------------------

static void populate(_MemoryStorage& storage, int n) {
  auto cursor = storage.create_cursor();
  for (int i = 0; i < n; i++) {
    KeyBuilder kb;
    kb.append_uint32(static_cast<uint32_t>(i));
    std::string doc_key = "doc_" + std::to_string(i);
    cursor->find(Slice(kb));
    cursor->value(Slice(doc_key));
  }
}

// ---------------------------------------------------------------------------
// Count results from a cursor
// ---------------------------------------------------------------------------

template <typename CursorType>
static int count_results(CursorType& cursor) {
  int count = 0;
  cursor.first();
  while (cursor.is_valid()) {
    count++;
    cursor.next();
  }
  return count;
}

// ---------------------------------------------------------------------------
// Bench 1: Executor scaling - single scan with different thread pool sizes
// ---------------------------------------------------------------------------

static void bench_executor_scaling(_MemoryStorage& storage) {
  auto& db = storage.db();

  // Build range bounds
  uint32_t lower_val = FLAGS_num / 2 - FLAGS_range / 2;
  uint32_t upper_val = lower_val + FLAGS_range - 1;

  KeyBuilder lb, ub;
  lb.append_uint32(lower_val);
  ub.append_uint32(upper_val);

  _RangeBound lower(Slice(lb), true);
  _RangeBound upper(Slice(ub), true);

  fprintf(stdout, "\n=== Executor Scaling (single scan, %d keys, range=%d) ===\n",
          FLAGS_num, FLAGS_range);
  fprintf(stdout, "%-20s %10s %10s %8s %8s\n",
          "Executor", "Time(ms)", "Ops/sec", "Results", "Speedup");
  fprintf(stdout, "%-20s %10s %10s %8s %8s\n",
          "--------------------", "----------", "----------", "--------", "--------");

  double baseline = 0;

  // Inline executor (baseline)
  {
    double total = 0;
    int result_count = 0;
    for (int iter = 0; iter < FLAGS_iterations; iter++) {
      _InlineExecutor exec;
      _TaskGroup<_InlineExecutor> tg(exec);
      _RangeFilter<DB, _InlineExecutor> filter(&db, &tg);

      double t0 = now_seconds();
      auto cursor = filter.scan(&db._root, lower, upper);
      double dt = now_seconds() - t0;
      total += dt;
      if (iter == 0) result_count = count_results(cursor);
    }

    double avg = total / FLAGS_iterations;
    baseline = avg;
    fprintf(stdout, "%-20s %10.2f %10.0f %8d %7.2fx\n",
            "Inline", avg * 1000, 1.0 / avg, result_count, 1.0);
  }

  // Pool executor with varying thread counts
  for (int threads = 1; threads <= FLAGS_max_threads; threads *= 2) {
    double total = 0;
    int result_count = 0;
    for (int iter = 0; iter < FLAGS_iterations; iter++) {
      BenchPool pool(threads);
      _PoolExecutor exec(pool);
      _TaskGroup<_PoolExecutor> tg(exec);
      _RangeFilter<DB, _PoolExecutor> filter(&db, &tg);

      double t0 = now_seconds();
      auto cursor = filter.scan(&db._root, lower, upper);
      double dt = now_seconds() - t0;
      total += dt;
      if (iter == 0) result_count = count_results(cursor);
    }

    double avg = total / FLAGS_iterations;
    char label[32];
    snprintf(label, sizeof(label), "Pool(%d)", threads);
    fprintf(stdout, "%-20s %10.2f %10.0f %8d %7.2fx\n",
            label, avg * 1000, 1.0 / avg, result_count,
            baseline / avg);
  }
}

// ---------------------------------------------------------------------------
// Bench 2: Concurrent readers - N threads each doing independent scans
// ---------------------------------------------------------------------------

static void bench_concurrent_readers(_MemoryStorage& storage) {
  auto& db = storage.db();

  uint32_t lower_val = FLAGS_num / 2 - FLAGS_range / 2;
  uint32_t upper_val = lower_val + FLAGS_range - 1;

  KeyBuilder lb, ub;
  lb.append_uint32(lower_val);
  ub.append_uint32(upper_val);

  _RangeBound lower(Slice(lb), true);
  _RangeBound upper(Slice(ub), true);

  fprintf(stdout, "\n=== Concurrent Readers (each thread does independent scans) ===\n");
  fprintf(stdout, "%-20s %10s %10s %8s %8s\n",
          "Readers", "Time(ms)", "Scans/sec", "Total", "Speedup");
  fprintf(stdout, "%-20s %10s %10s %8s %8s\n",
          "--------------------", "----------", "----------", "--------", "--------");

  int max_readers = FLAGS_readers > 0 ? FLAGS_readers : FLAGS_max_threads;
  double baseline = 0;

  for (int readers = 1; readers <= max_readers; readers *= 2) {
    double total_time = 0;

    for (int iter = 0; iter < FLAGS_iterations; iter++) {
      std::atomic<bool> start_flag{false};
      std::atomic<int> total_results{0};
      std::vector<std::thread> threads;

      auto worker = [&](int) {
        while (!start_flag.load(std::memory_order_acquire))
          std::this_thread::yield();

        // Each reader uses its own inline executor - no sharing
        _InlineExecutor exec;
        _TaskGroup<_InlineExecutor> tg(exec);
        _RangeFilter<DB, _InlineExecutor> filter(&db, &tg);

        auto cursor = filter.scan(&db._root, lower, upper);
        total_results.fetch_add(count_results(cursor),
                                std::memory_order_relaxed);
      };

      for (int r = 0; r < readers; r++)
        threads.emplace_back(worker, r);

      double t0 = now_seconds();
      start_flag.store(true, std::memory_order_release);
      for (auto& t : threads) t.join();
      double dt = now_seconds() - t0;
      total_time += dt;
    }

    double avg = total_time / FLAGS_iterations;
    if (readers == 1) baseline = avg;
    double scans_per_sec = readers / avg;

    char label[32];
    snprintf(label, sizeof(label), "%d readers", readers);
    fprintf(stdout, "%-20s %10.2f %10.0f %8d %7.2fx\n",
            label, avg * 1000, scans_per_sec,
            readers, (readers / avg) / (1 / baseline));
  }
}

// ---------------------------------------------------------------------------
// Bench 3: Concurrent readers with pooled executor - shared pool
// ---------------------------------------------------------------------------

static void bench_concurrent_pooled(_MemoryStorage& storage) {
  auto& db = storage.db();

  uint32_t lower_val = FLAGS_num / 2 - FLAGS_range / 2;
  uint32_t upper_val = lower_val + FLAGS_range - 1;

  KeyBuilder lb, ub;
  lb.append_uint32(lower_val);
  ub.append_uint32(upper_val);

  _RangeBound lower(Slice(lb), true);
  _RangeBound upper(Slice(ub), true);

  fprintf(stdout, "\n=== Concurrent Readers + Pool Executor ===\n");
  fprintf(stdout, "%-25s %10s %10s %8s\n",
          "Config", "Time(ms)", "Scans/sec", "Total");
  fprintf(stdout, "%-25s %10s %10s %8s\n",
          "-------------------------", "----------", "----------", "--------");

  int pool_sizes[] = {2, 4, 8};
  int reader_counts[] = {2, 4, 8};

  for (int pool_sz : pool_sizes) {
    if (pool_sz > FLAGS_max_threads) continue;
    for (int readers : reader_counts) {
      if (readers > FLAGS_max_threads) continue;

      double total_time = 0;

      for (int iter = 0; iter < FLAGS_iterations; iter++) {
        BenchPool pool(pool_sz);
        std::atomic<bool> start_flag{false};
        std::atomic<int> total_results{0};
        std::vector<std::thread> threads;

        auto worker = [&](int) {
          while (!start_flag.load(std::memory_order_acquire))
            std::this_thread::yield();

          _PoolExecutor exec(pool);
          _TaskGroup<_PoolExecutor> tg(exec);
          _RangeFilter<DB, _PoolExecutor> filter(&db, &tg);

          auto cursor = filter.scan(&db._root, lower, upper);
          total_results.fetch_add(count_results(cursor),
                                  std::memory_order_relaxed);
        };

        for (int r = 0; r < readers; r++)
          threads.emplace_back(worker, r);

        double t0 = now_seconds();
        start_flag.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        total_time += now_seconds() - t0;
      }

      double avg = total_time / FLAGS_iterations;
      char label[48];
      snprintf(label, sizeof(label), "pool=%d readers=%d", pool_sz, readers);
      fprintf(stdout, "%-25s %10.2f %10.0f %8d\n",
              label, avg * 1000, readers / avg, readers);
    }
  }
}

// ---------------------------------------------------------------------------
// Bench 4: Range size scaling
// ---------------------------------------------------------------------------

static void bench_range_scaling(_MemoryStorage& storage) {
  auto& db = storage.db();

  fprintf(stdout, "\n=== Range Size Scaling (inline executor) ===\n");
  fprintf(stdout, "%-20s %10s %10s %8s\n",
          "Range", "Time(ms)", "Results", "us/result");
  fprintf(stdout, "%-20s %10s %10s %8s\n",
          "--------------------", "----------", "----------", "--------");

  int ranges[] = {10, 100, 1000, 10000, FLAGS_num};

  for (int range : ranges) {
    if (range > FLAGS_num) continue;

    uint32_t lower_val = FLAGS_num / 2 - range / 2;
    uint32_t upper_val = lower_val + range - 1;

    KeyBuilder lb, ub;
    lb.append_uint32(lower_val);
    ub.append_uint32(upper_val);

    double total = 0;
    int result_count = 0;

    for (int iter = 0; iter < FLAGS_iterations; iter++) {
      _InlineExecutor exec;
      _TaskGroup<_InlineExecutor> tg(exec);
      _RangeFilter<DB, _InlineExecutor> filter(&db, &tg);

      double t0 = now_seconds();
      auto cursor = filter.scan(&db._root,
                                _RangeBound(Slice(lb), true),
                                _RangeBound(Slice(ub), true));
      double dt = now_seconds() - t0;
      total += dt;
      if (iter == 0) result_count = count_results(cursor);
    }

    double avg = total / FLAGS_iterations;
    char label[32];
    snprintf(label, sizeof(label), "[%d]", range);
    fprintf(stdout, "%-20s %10.2f %10d %8.2f\n",
            label, avg * 1000, result_count,
            result_count > 0 ? avg * 1e6 / result_count : 0.0);
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void parse_args(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (sscanf(argv[i], "--num=%d", &FLAGS_num) == 1) continue;
    if (sscanf(argv[i], "--range=%d", &FLAGS_range) == 1) continue;
    if (sscanf(argv[i], "--max_threads=%d", &FLAGS_max_threads) == 1) continue;
    if (sscanf(argv[i], "--iterations=%d", &FLAGS_iterations) == 1) continue;
    if (sscanf(argv[i], "--readers=%d", &FLAGS_readers) == 1) continue;
    if (strcmp(argv[i], "--concurrent") == 0) { FLAGS_concurrent = true; continue; }
    if (strcmp(argv[i], "--help") == 0) {
      fprintf(stderr,
              "Usage: bench_range_filter [options]\n"
              "  --num=N            Number of index entries (default: 100000)\n"
              "  --range=N          Range size to scan (default: 1000)\n"
              "  --max_threads=N    Max thread pool size (default: 8)\n"
              "  --iterations=N     Iterations per measurement (default: 5)\n"
              "  --readers=N        Max concurrent readers (default: max_threads)\n"
              "  --concurrent       Run concurrent reader benchmarks\n");
      exit(0);
    }
    fprintf(stderr, "Unknown flag: %s\n", argv[i]);
    exit(1);
  }
}

int main(int argc, char** argv) {
  parse_args(argc, argv);

  fprintf(stdout, "RangeFilter Scaling Benchmark\n");
  fprintf(stdout, "  entries:     %d\n", FLAGS_num);
  fprintf(stdout, "  range:       %d\n", FLAGS_range);
  fprintf(stdout, "  max_threads: %d\n", FLAGS_max_threads);
  fprintf(stdout, "  iterations:  %d\n", FLAGS_iterations);
  fprintf(stdout, "  hw threads:  %u\n", std::thread::hardware_concurrency());

  // Populate
  fprintf(stdout, "\nPopulating index with %d entries...", FLAGS_num);
  fflush(stdout);
  _MemoryStorage storage;
  double t0 = now_seconds();
  populate(storage, FLAGS_num);
  fprintf(stdout, " done (%.1fms)\n", (now_seconds() - t0) * 1000);

  // Run benchmarks
  bench_executor_scaling(storage);
  bench_range_scaling(storage);

  if (FLAGS_concurrent) {
    bench_concurrent_readers(storage);
    bench_concurrent_pooled(storage);
  }

  fprintf(stdout, "\n");
  return 0;
}
