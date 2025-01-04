// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <leaves.hpp>

#include "util/histogram.h"
#include "util/random.h"
#include "util/testutil.h"

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
static const char* FLAGS_benchmarks1 =
    "fillseq,"
    "fillseqsync,"
    "fillrandsync,"
    "fillrandom,"
    //"overwrite,"
    "readrandom,"
    "readseq,"
    "fillrand100K,"
    "fillseq100K,"
    "readseq100K,"
    "readrand100K,";

static const char* FLAGS_benchmarks =
    "fillseq,"
    "fillrandom,"
    "overwrite,"
    "readrandom,";
;

// Use writable MMAP
static bool FLAGS_writemap = false;

// don't explicitly sync meta data
static bool FLAGS_metasync = false;

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


namespace leaves {
extern size_t _grow_leaf;
extern size_t _grow_branch;
}

#ifdef DEBUG
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
#else
uint64_t dump_info(leaves::DB::db_ptr db) { return 0; }
#endif

namespace leveldb {

// Helper for quickly generating random data.
namespace {
class RandomGenerator {
 private:
  std::string data_;
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
      data_.append(piece);
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
  leaves::DB::db_ptr db_;
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

  // State kept for progress messages
  int done_;
  int next_report_;  // When to report next

  void PrintHeader() {
    const int kKeySize = 16;
    PrintEnvironment();
    std::fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    std::fprintf(
        stdout, "Values:     %d bytes each (%d bytes after compression)\n",
        FLAGS_value_size,
        static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    std::fprintf(stdout, "Entries:    %d\n", num_);
    std::fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
                 ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_) /
                  1048576.0));
    std::fprintf(
        stdout, "FileSize:   %.1f MB (estimated)\n",
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
    std::fprintf(stderr, "leaves:    version %d.%d\n", leaves::kMajorVersion,
                 leaves::kMinorVersion);

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

  void Stop(const Slice& name) {
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

  Benchmark()
      : db_(nullptr),
        num_(FLAGS_num),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        bytes_(0),
        rand_(301) {
    std::vector<std::string> files;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    Env::Default()->GetChildren(test_dir.c_str(), &files);
    if (!FLAGS_use_existing_db) {
      for (int i = 0; i < files.size(); i++) {
        if (Slice(files[i]).starts_with("dbbench_leaves")) {
          std::string file_name(test_dir);
          file_name += "/";
          file_name += files[i];
          Env::Default()->RemoveFile(file_name.c_str());
        }
      }
    }
  }

  ~Benchmark() { db_ = NULL; }

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

      Start();

      bool known = true;
      bool write_sync = false;
      if (name == Slice("fillseq")) {
        Write(write_sync, SEQUENTIAL, FRESH, num_, FLAGS_value_size, 100);
      } else if (name == Slice("fillbatch")) {
        Write(write_sync, RANDOM, FRESH, num_, FLAGS_value_size, 100);
      } else if (name == Slice("fillrandom")) {
        Write(write_sync, RANDOM, FRESH, num_, FLAGS_value_size, 100);
      } else if (name == Slice("overwrite")) {
        Write(write_sync, RANDOM, EXISTING, num_, FLAGS_value_size, 1);
      } else if (name == Slice("fillrandsync")) {
        write_sync = true;
        Write(write_sync, RANDOM, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == Slice("fillseqsync")) {
        write_sync = true;
        Write(write_sync, SEQUENTIAL, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == Slice("fillrand100K")) {
        Write(write_sync, RANDOM, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == Slice("fillseq100K")) {
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
      }
    }
  }

 private:
  void Open(bool sync) {
    assert(db_ == nullptr);

    // Initialize db_
    char file_name[100], cmd[200];
    db_num_++;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    std::snprintf(file_name, sizeof(file_name), "%s/dbbench_mdb-%d",
                  test_dir.c_str(), db_num_);

    sprintf(cmd, "mkdir -p %s", file_name);
    system(cmd);

    std::string test_fname(file_name);
    test_fname.append("/bench.lvs");
    db_ = leaves::DB::open(test_fname.c_str());
  }

  void Write(bool sync, Order order, DBState state, int num_entries,
             int value_size, int entries_per_batch) {
    // Create new database if state == FRESH
    if (state == FRESH) {
      if (FLAGS_use_existing_db) {
        message_ = "skipping (--use_existing_db is true)";
        return;
      }
      if (db_) {
        char cmd[200];
        sprintf(cmd, "rm -rf %s*", FLAGS_db);
        db_ = NULL;
        system(cmd);
      }
      Open(sync);
      Start();  // Do not count time taken to destroy/open
    }

    if (num_entries != num_) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_entries);
      message_ = msg;
    }

    std::ostream null_stream(nullptr);

    leaves::Slice mkey, mval;
    char key[100];
    int flag = 0, rc;

    leaves::_grow_branch = 0;
    leaves::_grow_leaf = 0;

    leaves::DB::cursor_ptr cursor = db_->create_cursor();
    // Write to database
    for (int i = 0; i < num_entries; i += entries_per_batch) {
      for (int j = 0; j < entries_per_batch; j++) {
        const int k =
            (order == SEQUENTIAL) ? i + j : (rand_.Next() % num_entries);
        snprintf(key, sizeof(key), "%016d", k);
        mkey = leaves::Slice(key);
        // printf("insert %i = (%i+%i) = %s\n", k, i, j, key);

        bytes_ += value_size + mkey.size();
        mval = gen_.Generate(value_size);

        cursor->find(mkey);
        cursor->set_value(mval);

        FinishedSingleOp();
      }
      cursor->commit();
    }

#ifdef DEBUG
    leaves::Statistics stats;
    db_->statistics(stats, "all", true);
    std::cout << std::endl;

    std::cout << "size: " << stats.size << std::endl;
    std::cout << "branches: " << stats.branches << std::endl;
    std::cout << "leaves: " << stats.leaves << std::endl;
    for (size_t i = 0; i < leaves::Statistics::POOL_COUNT; i++) {
      std::cout << "pool " << i << ": " << stats.pools[i].used << " used, "
                << stats.pools[i].freed << " freed, " << stats.pools[i].frag
                << " frag, " << stats.pools[i].cused << " cused" << std::endl;
    }

    std::cout << "grow branch: " << leaves::_grow_branch << std::endl;
    std::cout << "grow leaf: " << leaves::_grow_leaf << std::endl;

    std::cout << std::endl;
    std::cout << std::endl;
#endif
  }

  void ReadSequential() {
    leaves::Slice key, value;
    leaves::DB::cursor_ptr cursor = db_->create_cursor();
    for (cursor->first(); cursor->is_valid(); cursor->next()) {
      key = cursor->get_key();
      value = cursor->get_value();
      bytes_ += key.size() + value.size();
      FinishedSingleOp();
    }
  }

  void ReadRandom() {
    leaves::Slice key;
    leaves::DB::cursor_ptr cursor = db_->create_cursor();
    char ckey[100];
    for (int i = 0; i < reads_; i++) {
      const int k = rand_.Next() % reads_;
      snprintf(ckey, sizeof(ckey), "%016d", k);

      cursor->find(ckey);
      if (cursor->is_valid()) {
        bytes_ += key.size() + cursor->get_value().size();
      }
      FinishedSingleOp();
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
    } else if (sscanf(argv[i], "--cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_cache_size = n;
    } else if (sscanf(argv[i], "--page_size=%d%c", &n, &junk) == 1) {
      FLAGS_page_size = n;
    } else if (sscanf(argv[i], "--compression=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_compression = (n == 1) ? true : false;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
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
