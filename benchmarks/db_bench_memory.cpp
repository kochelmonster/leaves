// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <boost/endian/conversion.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "leaves/intern/db/_check.hpp"
#include "leaves/intern/storage/_memstore.hpp"
#include "leaves/leaves.hpp"
#include "util/histogram.h"
#include "util/random.h"
#include "util/testutil.h"

using boost::endian::big_to_native;
using boost::endian::native_to_big;

//#define BINARY_KEY

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//
//   fillseq       -- write N values in sequential key order
//   fillrandom    -- write N values in random key order
//   overwrite     -- overwrite N values in random key order
//   readseq       -- read N times sequentially
//   readrandom    -- read N times in random order
static const char* FLAGS_benchmarks =
    "fillseq,"
    "fillrandom,"
    "overwrite,"
    "readrandom,"
    "readseq,";

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
    if (pos_ + len > (int)data_.size()) {
      pos_ = 0;
      assert(len < (int)data_.size());
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

class MemoryBenchmark {
 private:
  std::unique_ptr<leaves::_MemoryStorage> storage_;
  leaves::_MemoryStorage::DB* db_;
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
    std::fprintf(stdout, "Storage:     MemoryStorage\n");
    std::fprintf(stdout, "Keys:        %d bytes each\n", kKeySize);
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

  MemoryBenchmark()
      : num_(FLAGS_num),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        bytes_(0),
        rand_(301) {
    // Initialize memory storage
    Open();
  }

  ~MemoryBenchmark() = default;

  void Run() {
    PrintHeader();

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
      if (name == Slice("fillseq")) {
        Write(SEQUENTIAL, FRESH, num_, FLAGS_value_size, 100);
      } else if (name == Slice("fillbatch")) {
        Write(RANDOM, FRESH, num_, FLAGS_value_size, 1000);
      } else if (name == Slice("fillrandom")) {
        Write(RANDOM, FRESH, num_, FLAGS_value_size, 100);
      } else if (name == Slice("overwrite")) {
        Write(RANDOM, EXISTING, num_, FLAGS_value_size, 1);
      } else if (name == Slice("readseq")) {
        ReadSequential();
      } else if (name == Slice("readrandom")) {
        ReadRandom();
      } else {
        known = false;
        if (name != Slice()) {  // No error message for empty name
          std::fprintf(stderr, "unknown benchmark '%s'\n",
                       name.ToString().c_str());
        }
      }
      if (known) {
        Stop(name);
        
        // Print memory usage statistics
        std::cout << "Memory usage: " << storage_->file_size() / (1024 * 1024)
                  << " MB" << std::endl;
      }
    }
  }

 private:
  void Open() {
    storage_ = std::make_unique<leaves::_MemoryStorage>();
    db_ = &storage_->db();
  }

  void Write(Order order, DBState state, int num_entries,
             int value_size, int /*entries_per_batch*/) {
    // For memory storage, FRESH means create new storage, EXISTING reuses current
    if (state == FRESH) {
      storage_ = std::make_unique<leaves::_MemoryStorage>();
      db_ = &storage_->db();
      Start();  // Do not count time taken to recreate storage
    }

    if (num_entries != num_) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_entries);
      message_ = msg;
    }

    leaves::Slice mkey, mval;
    char key[100];
    
    auto cursor = db_->create_cursor();
    
    // Write to database - memory storage doesn't need transactions
    for (int i = 0; i < num_entries; i++) {
      const int k = (order == SEQUENTIAL) ? i : (rand_.Next() % num_entries);

#ifdef BINARY_KEY
      *(uint64_t*)key = native_to_big(k);
      mkey = leaves::Slice(key, sizeof(uint64_t));
#else
      snprintf(key, sizeof(key), "%016d", k);
      mkey = leaves::Slice(key);
#endif
      
      bytes_ += value_size + mkey.size();
      mval = gen_.Generate(value_size);

      if (k == 129032) {
        // Debugging
        std::cout << "Debugging" << std::endl;
      }

      cursor->find(mkey);
      cursor->value(mval);
      FinishedSingleOp();
    }
  }

  void ReadSequential() {
    leaves::Slice key, value;
    
    auto cursor = db_->create_cursor();
    for (cursor->first(); cursor->is_valid(); cursor->next()) {
      key = cursor->key();
      value = cursor->value();
      bytes_ += key.size() + value.size();
      FinishedSingleOp();
    }
  }

  void ReadRandom() {
    leaves::Slice key;
    char ckey[100];
    
    auto cursor = db_->create_cursor();
    for (int i = 0; i < reads_; i++) {
      const int k = rand_.Next() % reads_;
      snprintf(ckey, sizeof(ckey), "%016d", k);

      cursor->find(ckey);
      if (cursor->is_valid()) {
        bytes_ += cursor->key().size() + cursor->value().size();
      }
      FinishedSingleOp();
    }
  }
};

}  // namespace leveldb

int main(int argc, char** argv) {
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
    } else {
      std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      std::exit(1);
    }
  }

  leveldb::MemoryBenchmark benchmark;
  benchmark.Run();
  return 0;
}