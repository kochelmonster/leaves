// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <sys/stat.h>

#include <atomic>
#include <thread>

#include <boost/endian/conversion.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <type_traits>

#include "leaves/fstore.hpp"
#include "leaves/intern/replication/_replication_db.hpp"
#include "leaves/intern/db/_check.hpp"
#include "leaves/confluence.hpp"
#include "leaves/mmap.hpp"
#include "leaves/replication.hpp"
#include "util/histogram.h"
#include "util/random.h"
#include "util/testutil.h"

using boost::endian::big_to_native;
using boost::endian::native_to_big;

// Use binary (big-endian uint64) keys instead of decimal string keys
static bool FLAGS_binary_key = false;

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//
//   fillseq       -- write N values in sequential key order in async mode
//   fillrandom    -- write N values in random key order in async mode
//   overwrite     -- overwrite N values in random key order in async mode
//   fillseqsync   -- write N/100 values in sequential key order in sync mode
//   fillrandsync  -- write N/100 values in random key order in sync mode
//   fillrand100K  -- write N/1000 100K values in random order in async mode
//   fillseq100K   -- write N/1000 100K values in seq order in async mode
//   readseq       -- read N times sequentially
//   readseq100K   -- read N/1000 100K values in sequential order in async mode
//   readrand100K  -- read N/1000 100K values in sequential order in async mode
//   readrandom    -- read N times in random order
static const char* FLAGS_benchmarks =
    "fillseq,"
    //    "fillseqsync,"
    //    "fillrandsync,"
    "fillrandom,"
    "overwrite,"
    "readrandom,"
    "readseq,"
    "fillrand100K,"
    "fillseq100K,"
    "readseq100K,"
    "readrand100K,";

static const char* FLAGS_benchmarks1 =
    "fillseq,"
    "fillseqsync,"
    "fillrandsync,"
    "fillrandom,"
    "overwrite,"
    "readrandom,"
    "readseq,";

// Batch size for write operations. Default 1000
static int FLAGS_batch_size = 1000;

// Use writable MMAP
static bool FLAGS_writemap = false;

// MapStorage mmap size in GiB. Keep default behavior at 64 GiB.
static int FLAGS_map_size_gb = 64;

// don't explicitly sync meta data
static bool FLAGS_metasync = false;

// Use write-ahead logging
static bool FLAGS_use_wal = false;

// Use FileStorage instead of MapStorage
static bool FLAGS_use_file_storage = false;

// Use ReplicatingMapStorage (includes merkle hashing)
static bool FLAGS_use_replicating = false;

// Use ConfluenceDB with N concurrent writer threads (0 = disabled)
static int FLAGS_use_confluence = 0;

// Override merge_write_threshold (0 = leave default). Set huge to suppress
// auto-merge during the benchmark and isolate writer-path overhead.
static uint32_t FLAGS_merge_threshold = 0;

// When true and ConfluenceDB is active, flush all tributaries into the main DB
// after each write benchmark completes (not included in the benchmark timing).
static bool FLAGS_merge_after_write = false;

// When non-empty, append the exact key & value sequence used by each write
// phase to this file (binary format). Used by test_merger to replay the
// crashing benchmark workload deterministically in a single thread.
static const char* FLAGS_dump_workload = nullptr;

// Number of key/values to place in database
static int FLAGS_num = 1000000;

// Number of read operations to do.  If negative, do FLAGS_num reads.
static int FLAGS_reads = -1;

// Size of each value
static int FLAGS_value_size = 100;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Cache size. Default 4 MB
static int FLAGS_cache_size = 4194304;

// Page size. Default 1 KB
static int FLAGS_page_size = 1024;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// Compression flag. If true, compression is on. If false, compression
// is off.
static bool FLAGS_compression = true;

// Use the db with the following name.
static const char* FLAGS_db = nullptr;

#if 0
namespace leaves {
size_t dump_db(std::ostream& out, DB::db_ptr db);
uint64_t dump_info(std::ostream& out, DB::db_ptr db);
}  // namespace leaves

inline void dump_graph(const char* output, leaves::DB::db_ptr db) {
  std::ofstream out(output);
  leaves::dump_db(out, db);
}

inline uint64_t dump_info(leaves::DB::db_ptr db) {
  return leaves::dump_info(std::cout, db);
}
#endif

namespace leveldb {

// Helper for quickly generating random data.
namespace {
class RandomGenerator {
 private:
  std::vector<char> data_;
  int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.insert(data_.end(), piece.begin(), piece.end());
    }
    pos_ = 0;
  }

  leaves::Slice Generate(int len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return leaves::Slice(data_.data() + pos_ - len, len);
  }
};

static Slice TrimSpace(Slice s) {
  int start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  int limit = s.size();
  while (limit > start && isspace(s[limit - 1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}

}  // namespace

class Benchmark {
 private:
  // Storage pointers - only one will be non-null based on configuration
  std::shared_ptr<leaves::FileStorage> file_storage_;
  std::shared_ptr<leaves::MapStorage> map_storage_;
  std::unique_ptr<leaves::MapStorage::ConfluenceDB> confluence_db_;
  bool using_file_storage_;
  bool using_replicating_;
  bool using_confluence_;
  int db_num_;
  int num_;
  int reads_;
  double start_;
  double last_op_finish_;
  int64_t bytes_;
  std::string message_;
  Histogram hist_;
  RandomGenerator gen_;
  Random rand_;
  std::vector<char> bench_keys_buf_;
  int bench_key_size_{0};
  // Pre-built generators for WriteImplConfluence threads (constructed before
  // Start() so their 1 MB init cost isn't counted in benchmark time).
  std::vector<RandomGenerator> conf_thread_gens_;

  // State kept for progress messages
  int done_;
  int next_report_;  // When to report next

  const char* storage_name() const {
    if (using_confluence_) return "MapStorage (ConfluenceDB)";
    if (using_replicating_) return "MapStorage (replicating)";
    if (using_file_storage_) return "FileStorage";
    return "MapStorage";
  }

  void PrintHeader() {
    const int kKeySize = FLAGS_binary_key ? 8 : 16;
    PrintEnvironment();
    std::fprintf(stdout, "Storage:     %s\n", storage_name());
    std::fprintf(stdout, "WAL:         %s\n", FLAGS_use_wal ? "enabled" : "disabled");
    std::fprintf(stdout, "Keys:        %d bytes each (%s)\n", kKeySize,
                 FLAGS_binary_key ? "binary uint64 big-endian" : "decimal string");
    std::fprintf(
        stdout, "Values:      %d bytes each (%d bytes after compression)\n",
        FLAGS_value_size,
        static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    std::fprintf(stdout, "Entries:     %d\n", num_);
    std::fprintf(stdout, "RawSize:     %.1f MB (estimated)\n",
                 ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_) /
                  1048576.0));
    std::fprintf(
        stdout, "FileSize:    %.1f MB (estimated)\n",
        (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num_) /
         1048576.0));
    PrintWarnings();
    std::fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    std::fprintf(
        stdout,
        "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
    std::fprintf(
        stdout,
        "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
  }

  void PrintEnvironment() {
    // std::fprintf(stderr, "leaves:    version %d.%d\n", leaves::kMajorVersion,
    //              leaves::kMinorVersion);

#if defined(__linux)
    time_t now = time(nullptr);
    std::fprintf(stderr, "Date:           %s",
                 ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = std::fopen("/proc/cpuinfo", "r");
    if (cpuinfo != nullptr) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != nullptr) {
        const char* sep = strchr(line, ':');
        if (sep == nullptr) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      std::fclose(cpuinfo);
      std::fprintf(stderr, "CPU:            %d * %s\n", num_cpus,
                   cpu_type.c_str());
      std::fprintf(stderr, "CPUCache:       %s\n", cache_size.c_str());
    }
#endif
  }

  void Start() {
    // CPU warmup - prevent cold start variance
    volatile int warmup_sum = 0;
    for (int i = 0; i < 1000; ++i) {
      warmup_sum += i * i;
    }

    start_ = Env::Default()->NowMicros() * 1e-6;
    bytes_ = 0;
    message_.clear();
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
    next_report_ = 100;
  }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = Env::Default()->NowMicros() * 1e-6;
      double micros = (now - last_op_finish_) * 1e6;
      hist_.Add(micros);
      if (micros > 20000) {
        std::fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        std::fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if (next_report_ < 1000)
        next_report_ += 100;
      else if (next_report_ < 5000)
        next_report_ += 500;
      else if (next_report_ < 10000)
        next_report_ += 1000;
      else if (next_report_ < 50000)
        next_report_ += 5000;
      else if (next_report_ < 100000)
        next_report_ += 10000;
      else if (next_report_ < 500000)
        next_report_ += 50000;
      else
        next_report_ += 100000;
      std::fprintf(stderr, "... finished %d ops%30s\r", done_, "");
      std::fflush(stderr);
    }
  }

  void Stop(const leveldb::Slice& name) {
    double finish = Env::Default()->NowMicros() * 1e-6;

    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedSingleOp().
    if (done_ < 1) done_ = 1;

    if (bytes_ > 0) {
      char rate[100];
      std::snprintf(rate, sizeof(rate), "%6.1f MB/s",
                    (bytes_ / 1048576.0) / (finish - start_));
      if (!message_.empty()) {
        message_ = std::string(rate) + " " + message_;
      } else {
        message_ = rate;
      }
    }

    std::fprintf(stdout, "%-12s : %11.3f micros/op;%s%s\n",
                 name.ToString().c_str(), (finish - start_) * 1e6 / done_,
                 (message_.empty() ? "" : " "), message_.c_str());
    if (FLAGS_histogram) {
      std::fprintf(stdout, "Microseconds per op:\n%s\n",
                   hist_.ToString().c_str());
    }
    std::fflush(stdout);
  }

 public:
  enum Order { SEQUENTIAL, RANDOM };
  enum DBState { FRESH, EXISTING };

  void prepare_keys(Order order, int n) {
    bench_key_size_ = FLAGS_binary_key ? 8 : 16;
    bench_keys_buf_.resize(n * bench_key_size_ + 1);  // +1 for snprintf null terminator
    char* buf = bench_keys_buf_.data();
    if (FLAGS_binary_key) {
      for (int i = 0; i < n; i++) {
        const int k = (order == SEQUENTIAL) ? i : (rand_.Next() % n);
        uint64_t bk = native_to_big((uint64_t)(uint32_t)k);
        memcpy(buf + i * bench_key_size_, &bk, sizeof(uint64_t));
      }
    } else {
      for (int i = 0; i < n; i++) {
        const int k = (order == SEQUENTIAL) ? i : (rand_.Next() % n);
        snprintf(buf + i * bench_key_size_, bench_key_size_ + 1, "%016d", k);
      }
    }

    // Optional: dump the just-generated key block so test_merger can replay
    // the EXACT workload that crashes the benchmark.  Format (binary, LE):
    //   uint32_t tag = 'K' << 0 | 'E' << 8 | 'Y' << 16 | 'S' << 24
    //   uint32_t key_size
    //   uint32_t num_keys
    //   <key bytes>...
    if (FLAGS_dump_workload != nullptr) {
      FILE* f = std::fopen(FLAGS_dump_workload, "ab");
      if (f) {
        uint32_t tag = 0x5359454BU;  // 'K','E','Y','S' little-endian
        uint32_t ks  = (uint32_t)bench_key_size_;
        uint32_t nn  = (uint32_t)n;
        std::fwrite(&tag, 4, 1, f);
        std::fwrite(&ks,  4, 1, f);
        std::fwrite(&nn,  4, 1, f);
        std::fwrite(buf, bench_key_size_, n, f);
        std::fclose(f);
      }
    }
  }

  leaves::Slice bench_key(int i) const {
    return leaves::Slice(bench_keys_buf_.data() + i * bench_key_size_, bench_key_size_);
  }

  Benchmark()
      : file_storage_(nullptr),
        map_storage_(nullptr),
        using_file_storage_(FLAGS_use_file_storage),
        using_replicating_(FLAGS_use_replicating),
        using_confluence_(FLAGS_use_confluence > 0),
        num_(FLAGS_num),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        bytes_(0),
        rand_(301) {
    std::vector<std::string> files;
    std::string test_dir;
    leveldb::Env::Default()->GetTestDirectory(&test_dir);
    leveldb::Env::Default()->GetChildren(test_dir.c_str(), &files);
    if (!FLAGS_use_existing_db) {
      // Remove db directories from previous runs (they are directories, use rm -rf)
      char cmd[200];
      sprintf(cmd, "rm -rf %s/dbbench_mdb-*", test_dir.c_str());
      int cleanup_status = std::system(cmd);
      if (cleanup_status != 0) {
        std::fprintf(stderr,
                     "warning: failed to remove previous benchmark directories: %s (status=%d)\n",
                     cmd, cleanup_status);
      }
      for (int i = 0; i < files.size(); i++) {
        if (leveldb::Slice(files[i]).starts_with("dbbench_leaves")) {
          std::string file_name(test_dir);
          file_name += "/";
          file_name += files[i];
          leveldb::Env::Default()->RemoveFile(file_name.c_str());
        }
      }
    }
  }

  ~Benchmark() {
    // Destroy confluence_db before map_storage so its monitor thread stops first
    confluence_db_.reset();
    file_storage_.reset();
    map_storage_.reset();
  }

  void Run() {
    PrintHeader();
    Open(false);

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != nullptr) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == nullptr) {
        name = benchmarks;
        benchmarks = nullptr;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      // Pre-generate keys before timing starts for EXISTING writes and random reads.
      // (FRESH writes re-generate in Write() after Open(), before their own Start().)
      if (name == Slice("overwrite")) {
        prepare_keys(RANDOM, num_);
      } else if (name == Slice("readrandom")) {
        prepare_keys(RANDOM, reads_);
      } else if (name == Slice("readrand100K")) {
        prepare_keys(RANDOM, reads_ / 1000);
      }

      Start();

      bool known = true;
      bool write_sync = false;
      bool is_write = false;
      if (name == Slice("fillseq")) {
        is_write = true;
        Write(write_sync, SEQUENTIAL, FRESH, num_, FLAGS_value_size,
              FLAGS_batch_size);
      } else if (name == Slice("fillbatch")) {
        is_write = true;
        Write(write_sync, RANDOM, FRESH, num_, FLAGS_value_size,
              FLAGS_batch_size);
      } else if (name == Slice("fillrandom")) {
        is_write = true;
        Write(write_sync, RANDOM, FRESH, num_, FLAGS_value_size,
              FLAGS_batch_size);
      } else if (name == Slice("overwrite")) {
        is_write = true;
        Write(write_sync, RANDOM, EXISTING, num_, FLAGS_value_size,
              FLAGS_batch_size);
      } else if (name == Slice("fillrandsync")) {
        is_write = true;
        write_sync = true;
        Write(write_sync, RANDOM, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == Slice("fillseqsync")) {
        is_write = true;
        write_sync = true;
        Write(write_sync, SEQUENTIAL, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == Slice("fillrand100K")) {
        is_write = true;
        Write(write_sync, RANDOM, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == Slice("fillseq100K")) {
        is_write = true;
        Write(write_sync, SEQUENTIAL, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == Slice("readseq")) {
        ReadSequential();
      } else if (name == Slice("readrandom")) {
        ReadRandom();
      } else if (name == Slice("readrand100K")) {
        int n = reads_;
        reads_ /= 1000;
        ReadRandom();
        reads_ = n;
      } else if (name == Slice("readseq100K")) {
        int n = reads_;
        reads_ /= 1000;
        ReadSequential();
        reads_ = n;
      } else {
        known = false;
        if (name != Slice()) {  // No error message for empty name
          std::fprintf(stderr, "unknown benchmark '%s'\n",
                       name.ToString().c_str());
        }
      }
      if (known) {
        Stop(name);
        // Optionally flush all tributaries into the main DB after a write
        // benchmark (not timed, so it does not affect the reported rate).
        if (is_write && FLAGS_merge_after_write && confluence_db_) {
          confluence_db_->merge_all_now();
        }
#if 0  // old STATISTICS block - moved to WriteImpl
        if (using_replicating_ && map_storage_) {
          std::cout << "File size: "
                    << map_storage_->file_size() / (1024 * 1024) << " MB"
                    << std::endl;
        } else if (using_file_storage_ && file_storage_) {
          std::cout << "File size: "
                    << file_storage_->file_size() / (1024 * 1024) << " MB"
                    << std::endl;
        } else if (map_storage_) {
          std::cout << "File size: "
                    << map_storage_->file_size() / (1024 * 1024) << " MB"
                    << std::endl;
        }
        // leaves::_MemoryChecker<Storage>(*db_).check();
        auto txn = db_->txn();

        size_t size = 0;
        size_t counts = 0;
        std::cout << "GARBAGE" << std::endl;
        Storage::Statistics db_stat;
        db_->statistics(db_stat);
        for (auto slot : db_stat.garbage.slots) {
          std::cout << "Slot: " << slot.page_size << ": " << slot.count
                    << std::endl;
          size += slot.page_size * slot.count;
          counts += slot.count;
        }
        std::cout << "CHECK Garbage: " << size << "(" << counts << ")"
                  << std::endl;

        std::cout << "BRANCHES" << std::endl;
        size_t nsize = 0;
        for (auto slot : db_stat.branch.slots) {
          std::cout << "Slot: " << slot.page_size << ": " << slot.count
                    << " : " << slot.free << std::endl;
          nsize += slot.page_size * slot.count;
        }

        std::cout << "LEAVES" << std::endl;
        for (auto slot : db_stat.leaf.slots) {
          std::cout << "Slot: " << slot.page_size << ": " << slot.count
                    << " : " << slot.free << std::endl;
          nsize += slot.page_size * slot.count;
        }
        std::cout << "CHECK Nodes: " << nsize << std::endl;

        nsize = 0;
        std::cout << "TRANSACTION" << std::endl;
        for (auto slot : db_stat.transaction.slots) {
          std::cout << "Slot: " << slot.page_size << ": " << slot.count
                    << " : " << slot.free << std::endl;
          nsize += slot.page_size * slot.count;
        }
        std::cout << "CHECK Transactions: " << nsize << std::endl;

        size += nsize;

        std::cout << std::endl
                  << "file size: " << txn->file_size / (1024 * 1024) << " MB"
                  << std::endl;
#endif
#if 0 
        size_t spare = txn->garbage.end_area - txn->garbage.next_free +
                       txn->garbage.end4k - txn->garbage.next4k;

        std::cout << std::endl
                  << "file size: " << txn->file_size << " B" << std::endl
                  << "endarea: " << txn->garbage.end_area << " B" << std::endl
                  << "spare: " << spare << " B" << std::endl
                  << "calc size: " << size << " B" << std::endl
                  << "difference: "
                  << (int)size + (int)spare + 64 - (int)txn->file_size
                  << std::endl;
#endif
      }
    }
  }

 private:
  void Open(bool sync) {
    assert(file_storage_ == nullptr && map_storage_ == nullptr);

    // Initialize db_
    char file_name[100], cmd[200];
    db_num_++;
    std::string test_dir;
    leveldb::Env::Default()->GetTestDirectory(&test_dir);
    std::snprintf(file_name, sizeof(file_name), "%s/dbbench_mdb-%d",
                  test_dir.c_str(), db_num_);

    sprintf(cmd, "mkdir -p %s", file_name);
    int r = system(cmd);

    std::string test_fname(file_name);
    test_fname.append("/bench.lvs");

    const uint64_t map_size_bytes = static_cast<uint64_t>(FLAGS_map_size_gb) * leaves::G;

    if (using_replicating_) {
      map_storage_ = leaves::MapStorage::create(test_fname.c_str(), map_size_bytes);
    } else if (using_file_storage_) {
      file_storage_ = leaves::FileStorage::create(test_fname.c_str());
    } else {
      map_storage_ = leaves::MapStorage::create(test_fname.c_str(), map_size_bytes);
    }

    if (using_confluence_) {
      confluence_db_ =
          std::make_unique<leaves::MapStorage::ConfluenceDB>(
              map_storage_, "benchmark");
      if (FLAGS_merge_threshold)
        confluence_db_->set_merge_write_threshold(FLAGS_merge_threshold);
    }
  }

  void WriteImplConfluence(bool sync, int num_entries, int value_size,
                           int entries_per_batch) {
    int n_threads = FLAGS_use_confluence;
    int per_thread = (num_entries + n_threads - 1) / n_threads;

    std::vector<std::thread> threads;
    std::atomic<int64_t> total_bytes{0};

    for (int t = 0; t < n_threads; t++) {
      int start = t * per_thread;
      if (start >= num_entries) break;
      int end = std::min(start + per_thread, num_entries);

      threads.emplace_back([this, &total_bytes, start, end, value_size,
                            entries_per_batch, sync, t]() {
        auto cursor = confluence_db_->cursor();
        RandomGenerator& local_gen = conf_thread_gens_[t];
        int64_t my_bytes = 0;
        for (int i = start; i < end; i += entries_per_batch) {
          cursor.start_transaction();
          int batch_end = std::min(i + entries_per_batch, end);
          for (int j = i; j < batch_end; j++) {
            leaves::Slice key = bench_key(j);
            cursor.find(key);
            cursor.value(local_gen.Generate(value_size));
            my_bytes += value_size + key.size();
          }
          cursor.commit(sync);
        }
        total_bytes.fetch_add(my_bytes, std::memory_order_relaxed);
      });
    }

    for (auto& th : threads) th.join();

    std::cerr << "[confluence] tributaries allocated (high-water): "
              << confluence_db_->_internal()->_tributaries_count.load(
                     std::memory_order_acquire)
              << " (n_threads=" << n_threads << ")\n";

    bytes_ += total_bytes.load();
    done_ += num_entries;
  }

  // Template method for writing operations
  template <typename StorageType, template<typename> class DBClass = leaves::_DB>
  void WriteImpl(StorageType& storage, bool sync, Order order, int num_entries,
                 int value_size, int entries_per_batch) {
    leaves::Slice mkey, mval;

    auto db = open_bench_db<StorageType, DBClass>(storage);
    auto cursor = db.cursor();

    // Write to database
    for (int i = 0; i < num_entries; i += entries_per_batch) {
      cursor.start_transaction(false, FLAGS_use_wal);
      for (int j = 0; j < entries_per_batch; j++) {
        mkey = bench_key(i + j);
        int iter = i + j;

        bytes_ += value_size + mkey.size();
        mval = gen_.Generate(value_size);


        cursor.find(mkey);
        cursor.value(mval);

        FinishedSingleOp();
#if 0
        // Dump size_root and offset_root for debugging
        auto db_internal = db._internal();
        auto txn = db_internal->_wtxn;

        std::cout << "Iter " << iter
                  << ": free_bigmem_root=" << txn->bigmem.root._offset
                  << std::endl;

        if (iter >= 0 && iter < 240) {
          if (txn->bigmem.root) {
            char filename[256];
            snprintf(filename, sizeof(filename), "errors/dump_bigmen_%06d.yaml",
                     iter);
            std::ofstream of(filename);
            leaves::_Dumper(db, txn->bigmem.root, false).dump(of);
          }
          if (txn->root) {
            char filename[256];
            snprintf(filename, sizeof(filename), "errors/dump_data_%06d.yaml",
                     iter);
            std::ofstream of(filename);
            leaves::_Dumper(db, txn->root, false).dump(of);
          }
        }
#endif
      }
      cursor.commit(sync);
    }

#ifdef STATISTICS
    {
  auto db_for_stat = open_bench_db<StorageType, DBClass>(storage);
      auto* db_internal = db_for_stat._internal();
      using DB_type = std::remove_pointer_t<decltype(db_internal)>;
      typename DB_type::Statistics db_stat;
      db_internal->statistics(db_stat);
      std::cout << "\n--- Memory Statistics ---\n";
      std::cout << "GARBAGE:\n";
      for (auto& slot : db_stat.garbage.slots)
        if (slot.count > 0)
          std::cout << "  slot[" << slot.page_size << "]: " << slot.count << " pages = " << (slot.count * slot.page_size / 1024) << " KB\n";
      std::cout << "BRANCH NODES:\n";
      size_t total_branch = 0;
      for (auto& slot : db_stat.branch.slots)
        if (slot.count > 0) {
          std::cout << "  slot[" << slot.page_size << "]: " << slot.count << " pages = " << (slot.count * slot.page_size / 1024) << " KB\n";
          total_branch += slot.count * slot.page_size;
        }
      std::cout << "LEAF NODES:\n";
      size_t total_leaf = 0;
      for (auto& slot : db_stat.leaf.slots)
        if (slot.count > 0) {
          std::cout << "  slot[" << slot.page_size << "]: " << slot.count << " pages = " << (slot.count * slot.page_size / 1024) << " KB\n";
          total_leaf += slot.count * slot.page_size;
        }
      std::cout << "TRANSACTIONS:\n";
      size_t total_txn = 0;
      for (auto& slot : db_stat.transaction.slots)
        if (slot.count > 0) {
          std::cout << "  slot[" << slot.page_size << "]: " << slot.count << " pages = " << (slot.count * slot.page_size / 1024) << " KB\n";
          total_txn += slot.count * slot.page_size;
        }
      std::cout << "Branches=" << total_branch/1024 << " KB, Leaves=" << total_leaf/1024 << " KB, Txns=" << total_txn/1024 << " KB\n";
    }
#endif
  }

  void Write(bool sync, Order order, DBState state, int num_entries,
             int value_size, int entries_per_batch) {
    // Create new database if state == FRESH
    if (state == FRESH) {
      if (FLAGS_use_existing_db) {
        message_ = "skipping (--use_existing_db is true)";
        return;
      }

      // Clean up existing storage if any
      if (file_storage_ || map_storage_) {
        char cmd[200];
        sprintf(cmd, "rm -rf %s*", FLAGS_db);

        confluence_db_.reset();  // cancel monitor before releasing storage
        file_storage_.reset();
        map_storage_.reset();

        int r = system(cmd);
      }

      Open(sync);
      prepare_keys(order, num_entries);
      if (using_confluence_) {
        conf_thread_gens_.clear();
        conf_thread_gens_.resize(FLAGS_use_confluence);
      }
      Start();  // Do not count time taken to destroy/open
    }

    if (num_entries != num_) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_entries);
      message_ = msg;
    }

    // Call the appropriate template implementation based on storage type
    if (using_confluence_) {
      WriteImplConfluence(sync, num_entries, value_size, entries_per_batch);
    } else if (using_replicating_) {
      if (!map_storage_) {
        throw std::runtime_error("Map storage pointer is null");
      }
      WriteImpl<leaves::MapStorage, leaves::_ReplicationDB>(*map_storage_, sync, order, num_entries, value_size,
                entries_per_batch);
    } else if (using_file_storage_) {
      if (!file_storage_) {
        throw std::runtime_error("File storage pointer is null");
      }
      WriteImpl(*file_storage_, sync, order, num_entries, value_size,
                entries_per_batch);
    } else {
      if (!map_storage_) {
        throw std::runtime_error("Map storage pointer is null");
      }
      WriteImpl(*map_storage_, sync, order, num_entries, value_size,
                entries_per_batch);
    }
  }

  void ReadSequentialImplConfluence() {
    auto cursor = confluence_db_->cursor();
    for (cursor.first(); cursor.is_valid(); cursor.next()) {
      bytes_ += cursor.key().size() + cursor.value().size();
      FinishedSingleOp();
    }
  }

  // Template method for sequential reading
  template <typename StorageType, template<typename> class DBClass = leaves::_DB>
  void ReadSequentialImpl(StorageType& storage) {
    leaves::Slice key, value;

    auto db = open_bench_db<StorageType, DBClass>(storage);
    auto cursor = db.cursor();
    for (cursor.first(); cursor.is_valid(); cursor.next()) {
      key = cursor.key();
      value = cursor.value();
      bytes_ += key.size() + value.size();
      FinishedSingleOp();
    }
  }

  void ReadSequential() {
    // Call the appropriate template implementation based on storage type
    if (using_confluence_) {
      if (!map_storage_) throw std::runtime_error("Map storage pointer is null");
      ReadSequentialImplConfluence();
    } else if (using_replicating_) {
      if (!map_storage_) {
        throw std::runtime_error("Map storage pointer is null");
      }
      ReadSequentialImpl<leaves::MapStorage, leaves::_ReplicationDB>(*map_storage_);
    } else if (using_file_storage_) {
      if (!file_storage_) {
        throw std::runtime_error("File storage pointer is null");
      }
      ReadSequentialImpl(*file_storage_);
    } else {
      if (!map_storage_) {
        throw std::runtime_error("Map storage pointer is null");
      }
      ReadSequentialImpl(*map_storage_);
    }
  }

  void ReadRandomImplConfluence() {
    auto cursor = confluence_db_->cursor();
    for (int i = 0; i < reads_; i++) {
      leaves::Slice value_out;
      cursor.find(bench_key(i));
      if (cursor.is_valid()) {
        value_out = cursor.value();
        bytes_ += bench_key_size_ + value_out.size();
      }
      FinishedSingleOp();
    }
  }

  // Template method for random reading
  template <typename StorageType, template<typename> class DBClass = leaves::_DB>
  void ReadRandomImpl(StorageType& storage) {
    auto db = open_bench_db<StorageType, DBClass>(storage);
    auto cursor = db.cursor();
    for (int i = 0; i < reads_; i++) {
      cursor.find(bench_key(i));
      if (cursor.is_valid()) {
        bytes_ += cursor.key().size() + cursor.value().size();
      }
      FinishedSingleOp();
    }
  }

  template <typename StorageType, template <typename> class DBClass>
  auto open_bench_db(StorageType& storage) {
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<StorageType>>,
                                 leaves::MapStorage>) {
      if constexpr (std::is_same_v<
                        DBClass<leaves::MapStorage::StorageImpl>,
                        leaves::_ReplicationDB<leaves::MapStorage::StorageImpl>>) {
        return storage.template open<leaves::MapStorage::ReplicationDB>("benchmark");
      } else {
        return storage.template open<leaves::MapStorage::DB>("benchmark");
      }
    } else {
      return storage.template open<DBClass>("benchmark");
    }
  }

  void ReadRandom() {
    // Call the appropriate template implementation based on storage type
    if (using_confluence_) {
      if (!map_storage_) throw std::runtime_error("Map storage pointer is null");
      ReadRandomImplConfluence();
    } else if (using_replicating_) {
      if (!map_storage_) {
        throw std::runtime_error("Map storage pointer is null");
      }
      ReadRandomImpl<leaves::MapStorage, leaves::_ReplicationDB>(*map_storage_);
    } else if (using_file_storage_) {
      if (!file_storage_) {
        throw std::runtime_error("File storage pointer is null");
      }
      ReadRandomImpl(*file_storage_);
    } else {
      if (!map_storage_) {
        throw std::runtime_error("Map storage pointer is null");
      }
      ReadRandomImpl(*map_storage_);
    }
  }
};

}  // namespace leveldb

int main(int argc, char** argv) {
  std::string default_db_path;
  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--batch_size=%d%c", &n, &junk) == 1) {
      FLAGS_batch_size = n;
    } else if (sscanf(argv[i], "--cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_cache_size = n;
    } else if (sscanf(argv[i], "--page_size=%d%c", &n, &junk) == 1) {
      FLAGS_page_size = n;
    } else if (sscanf(argv[i], "--map_size_gb=%d%c", &n, &junk) == 1 && n > 0) {
      FLAGS_map_size_gb = n;
    } else if (sscanf(argv[i], "--use_file_storage=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_file_storage = (n == 1) ? true : false;
      std::fprintf(stderr, "Using %s for storage\n",
                   FLAGS_use_file_storage ? "FileStorage" : "MapStorage");
    } else if (sscanf(argv[i], "--use_replicating=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_replicating = (n == 1) ? true : false;
      if (FLAGS_use_replicating) {
        std::fprintf(stderr, "Using MapStorage with _ReplicationDB for storage\n");
      }
    } else if (sscanf(argv[i], "--use_confluence=%d%c", &n, &junk) == 1 &&
               n >= 0) {
      FLAGS_use_confluence = n;
      if (n > 0) {
        std::fprintf(stderr, "Using ConfluenceDB with %d threads\n", n);
      }
    } else if (sscanf(argv[i], "--merge_threshold=%d%c", &n, &junk) == 1 &&
               n >= 0) {
      FLAGS_merge_threshold = static_cast<uint32_t>(n);
    } else if (sscanf(argv[i], "--merge_after_write=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_merge_after_write = (n == 1) ? true : false;
    } else if (sscanf(argv[i], "--compression=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_compression = (n == 1) ? true : false;
    } else if (sscanf(argv[i], "--binary_key=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_binary_key = (n == 1);
      if (FLAGS_binary_key) {
        std::fprintf(stderr, "Using binary keys (big-endian uint64, 8 bytes)\n");
      }
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (sscanf(argv[i], "--use_wal=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_wal = (n == 1);
      std::fprintf(stderr, "WAL: %s\n", FLAGS_use_wal ? "enabled" : "disabled");
    } else if (strncmp(argv[i], "--dump_workload=", 16) == 0) {
      FLAGS_dump_workload = argv[i] + 16;
    } else {
      std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      std::exit(1);
    }
  }

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == nullptr) {
    leveldb::Env::Default()->GetTestDirectory(&default_db_path);
    default_db_path += "/dbbench";
    FLAGS_db = default_db_path.c_str();
  }

  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}
