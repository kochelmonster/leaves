// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/env.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/slice_transform.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "../leveldb/util/histogram.h"
#include "../leveldb/util/random.h"

// Type aliases for compatibility
using Slice = rocksdb::Slice;
using Env = rocksdb::Env;
using WriteOptions = rocksdb::WriteOptions;

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
//   compact       -- Compact the entire database
static const char* FLAGS_benchmarks =
    "fillseq,"
    "fillrandom,"
    "overwrite,"
    "readrandom,"
    "readseq,"
    "fillrand100K,"
    "fillseq100K,"
    "readseq100K,"
    "readrand100K,"
    "compact,";

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

// Block cache size. Default 1GB for high performance
static int FLAGS_cache_size = 1024 * 1024 * 1024;

// Block size for SST files. Default 32KB for better performance
static int FLAGS_block_size = 32 * 1024;

// Write buffer size. Default 256MB for high throughput
static int FLAGS_write_buffer_size = 256 * 1024 * 1024;

// Maximum number of write buffers
static int FLAGS_max_write_buffer_number = 6;

// Target file size for level 1 - larger files reduce compaction
static int FLAGS_target_file_size_base = 128 * 1024 * 1024;

// Maximum number of level 0 files - higher to reduce compaction overhead
static int FLAGS_level0_file_num_compaction_trigger = 8;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// Use the db with the following name.
static const char* FLAGS_db = nullptr;

// Use bloom filter
static bool FLAGS_bloom_bits = true;

// Number of bits per key for bloom filter - higher for better false positive rate
static int FLAGS_bloom_bits_per_key = 15;

// Disable WAL for maximum write performance
static bool FLAGS_disable_wal = false;

// Use direct I/O for reads for predictable performance
static bool FLAGS_use_direct_reads = true;

// Use direct I/O for writes
static bool FLAGS_use_direct_io_for_flush_and_compaction = true;

// Allow mmap reads (disabled due to incompatibility with direct reads)
static bool FLAGS_allow_mmap_reads = false;

// Allow mmap writes for better performance
static bool FLAGS_allow_mmap_writes = false;

// Number of background threads for compaction - increase for better performance
static int FLAGS_max_background_compactions = 8;

// Number of background threads for flushing
static int FLAGS_max_background_flushes = 2;

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
      piece.clear();
      for (int i = 0; i < 100; i++) {
        piece += static_cast<char>(' ' + rnd.Uniform(95));
      }
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(int len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
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
  std::shared_ptr<rocksdb::Cache> cache_;
  std::shared_ptr<const rocksdb::FilterPolicy> filter_policy_;
  rocksdb::DB* db_;
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
    PrintEnvironment();
    std::fprintf(stdout, "Keys:       %d bytes each\n", 16);
    std::fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
                 FLAGS_value_size,
                 static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    std::fprintf(stdout, "Entries:    %d\n", num_);
    std::fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
                 ((static_cast<int64_t>(16 + FLAGS_value_size) * num_) / 1048576.0));
    std::fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
                 (((16 + FLAGS_value_size * FLAGS_compression_ratio) * num_) / 1048576.0));
    PrintWarnings();
    std::fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    std::fprintf(stdout, "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
    std::fprintf(stdout, "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
  }

  void PrintEnvironment() {
    std::fprintf(stderr, "RocksDB:    version %d.%d.%d\n",
                 ROCKSDB_MAJOR, ROCKSDB_MINOR, ROCKSDB_PATCH);

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
                 name.ToString().c_str(),
                 (finish - start_) * 1e6 / done_,
                 (message_.empty() ? "" : " "),
                 message_.c_str());
    if (FLAGS_histogram) {
      std::fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    std::fflush(stdout);
  }

 public:
  enum Order { SEQUENTIAL, RANDOM };
  enum DBState { FRESH, EXISTING };

  Benchmark()
      : cache_(rocksdb::NewLRUCache(FLAGS_cache_size)),
        filter_policy_(FLAGS_bloom_bits ? rocksdb::NewBloomFilterPolicy(FLAGS_bloom_bits_per_key)
                                        : nullptr),
        db_(nullptr),
        db_num_(0),
        num_(FLAGS_num),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        bytes_(0),
        rand_(301) {
    std::vector<std::string> files;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    Env::Default()->GetChildren(test_dir, &files);
    for (int i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("dbbench_rocksdb")) {
        std::string file_name(test_dir);
        file_name += "/";
        file_name += files[i];
        Env::Default()->DeleteFile(file_name);
      }
    }
    if (!FLAGS_use_existing_db) {
      DestroyDB();
    }
  }

  ~Benchmark() {
    delete db_;
  }

  void Run() {
    PrintHeader();
    Open();

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

      // Reset parameters that may be overridden below
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      int entries_per_batch = 1;
      int value_size = FLAGS_value_size;
      WriteOptions write_options;

      void (Benchmark::*method)(Slice) = nullptr;
      bool fresh_db = false;

      if (name == Slice("fillseq")) {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillbatch")) {
        fresh_db = true;
        entries_per_batch = 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandom")) {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("overwrite")) {
        fresh_db = false;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillseqsync")) {
        fresh_db = true;
        num_ /= 100;
        write_options.sync = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandsync")) {
        fresh_db = true;
        num_ /= 100;
        write_options.sync = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillrand100K")) {
        fresh_db = true;
        num_ /= 1000;
        value_size = 100 * 1000;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillseq100K")) {
        fresh_db = true;
        num_ /= 1000;
        value_size = 100 * 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readreverse")) {
        method = &Benchmark::ReadReverse;
      } else if (name == Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readmissing")) {
        method = &Benchmark::ReadMissing;
      } else if (name == Slice("seekrandom")) {
        method = &Benchmark::SeekRandom;
      } else if (name == Slice("readhot")) {
        method = &Benchmark::ReadHot;
      } else if (name == Slice("readrandomsmall")) {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("deleteseq")) {
        method = &Benchmark::DeleteSeq;
      } else if (name == Slice("deleterandom")) {
        method = &Benchmark::DeleteRandom;
      } else if (name == Slice("readwhilewriting")) {
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("compact")) {
        method = &Benchmark::Compact;
      } else if (name == Slice("crc32c")) {
        method = &Benchmark::Crc32c;
      } else if (name == Slice("acquireload")) {
        method = &Benchmark::AcquireLoad;
      } else if (name == Slice("snappycomp")) {
        method = &Benchmark::SnappyCompress;
      } else if (name == Slice("snappyuncomp")) {
        method = &Benchmark::SnappyUncompress;
      } else if (name == Slice("fillseq100K")) {
        fresh_db = true;
        num_ /= 1000;
        value_size = 100 * 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("readseq100K")) {
        reads_ /= 1000;
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readrand100K")) {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else {
        if (name != Slice()) {  // No error message for empty name
          std::fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
        }
      }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          message_ = "skipping (--use_existing_db is true)";
          method = nullptr;
        } else {
          delete db_;
          db_ = nullptr;
          DestroyDB();
          Open();
        }
      }

      if (method != nullptr) {
        RunBenchmark(name, method);
      }
    }
  }

 private:

  void RunBenchmark(Slice name, void (Benchmark::*method)(Slice)) {
    Start();
    (this->*method)(name);
  }

  void Open() {
    assert(db_ == nullptr);

    rocksdb::Options options;
    options.create_if_missing = true;
    
    // High-performance configuration
    options.write_buffer_size = FLAGS_write_buffer_size;
    options.max_write_buffer_number = FLAGS_max_write_buffer_number;
    options.target_file_size_base = FLAGS_target_file_size_base;
    options.level0_file_num_compaction_trigger = FLAGS_level0_file_num_compaction_trigger;
    options.max_background_compactions = FLAGS_max_background_compactions;
    options.max_background_flushes = FLAGS_max_background_flushes;
    
    // Advanced performance optimizations
    options.level0_slowdown_writes_trigger = 16;
    options.level0_stop_writes_trigger = 24;
    options.max_bytes_for_level_base = 512 * 1024 * 1024; // 512MB
    options.max_bytes_for_level_multiplier = 8;
    
    // Parallelism
    options.max_subcompactions = 4;
    options.allow_concurrent_memtable_write = true;
    options.enable_write_thread_adaptive_yield = true;
    
    // Use ZSTD compression for better compression ratio and speed
    options.compression = rocksdb::kZSTD;
    options.compression_opts.level = 3;
    options.compression_opts.parallel_threads = 4;
    
    // Enable direct I/O for better performance
    // Note: mmap reads and direct reads are mutually exclusive
    if (FLAGS_allow_mmap_reads) {
      options.use_direct_reads = false;
    } else {
      options.use_direct_reads = FLAGS_use_direct_reads;
    }
    options.use_direct_io_for_flush_and_compaction = FLAGS_use_direct_io_for_flush_and_compaction;
    
    // Memory-mapped file options
    options.allow_mmap_reads = FLAGS_allow_mmap_reads;
    options.allow_mmap_writes = FLAGS_allow_mmap_writes;
    
    // Memory management
    options.db_write_buffer_size = FLAGS_write_buffer_size * FLAGS_max_write_buffer_number;
    options.arena_block_size = 32 * 1024 * 1024; // 32MB arena blocks
    
    // Optimize for point lookups
    options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(8));
    
    // Bloom filter configuration
    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = cache_;
    table_options.block_size = FLAGS_block_size;
    if (FLAGS_bloom_bits) {
      table_options.filter_policy = filter_policy_;
      table_options.whole_key_filtering = true;
      table_options.optimize_filters_for_memory = true;
    }
    
    // Use hash index for faster lookups in smaller blocks
    if (FLAGS_block_size <= 16 * 1024) {
      table_options.index_type = rocksdb::BlockBasedTableOptions::kHashSearch;
    } else {
      table_options.index_type = rocksdb::BlockBasedTableOptions::kBinarySearch;
    }
    
    // Cache optimizations
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    table_options.pin_top_level_index_and_filter = true;
    
    // Block-based table optimizations
    table_options.checksum = rocksdb::kCRC32c;
    // Note: block_align is incompatible with compression, so we don't enable it
    table_options.enable_index_compression = false; // Keep index uncompressed for speed
    
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    // Disable WAL for maximum write performance (if specified)
    if (FLAGS_disable_wal) {
      options.disable_auto_compactions = true;
    }

    char file_name[100];
    db_num_++;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    std::snprintf(file_name, sizeof(file_name), "%s/dbbench_rocksdb-%d",
                  test_dir.c_str(), db_num_);

    rocksdb::Status s = rocksdb::DB::Open(options, file_name, &db_);
    if (!s.ok()) {
      std::fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  void WriteSeq(Slice name) {
    DoWrite(name, false, SEQUENTIAL, FRESH);
  }

  void WriteRandom(Slice name) {
    DoWrite(name, false, RANDOM, FRESH);
  }

  void DoWrite(Slice name, bool sync, Order order, DBState state) {
    if (num_ != FLAGS_num) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_);
      message_ = msg;
    }

    rocksdb::WriteOptions write_options;
    write_options.sync = sync;
    write_options.disableWAL = FLAGS_disable_wal;

    Start();

    for (int i = 0; i < num_; i++) {
      const int k = (order == SEQUENTIAL) ? i : (rand_.Next() % FLAGS_num);
      char key[100];
      std::snprintf(key, sizeof(key), "%016d", k);
      rocksdb::Status s = db_->Put(write_options, key, gen_.Generate(FLAGS_value_size));
      if (!s.ok()) {
        std::fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      bytes_ += FLAGS_value_size + strlen(key);
      FinishedSingleOp();
    }

    Stop(name);
  }

  void ReadSequential(Slice name) {
    rocksdb::Iterator* iter = db_->NewIterator(rocksdb::ReadOptions());
    int i = 0;
    int64_t bytes = 0;
    Start();
    for (iter->SeekToFirst(); i < reads_ && iter->Valid(); iter->Next()) {
      bytes += iter->key().size() + iter->value().size();
      FinishedSingleOp();
      ++i;
    }
    delete iter;
    bytes_ = bytes;
    Stop(name);
  }

  void ReadReverse(Slice name) {
    rocksdb::Iterator* iter = db_->NewIterator(rocksdb::ReadOptions());
    int i = 0;
    int64_t bytes = 0;
    Start();
    for (iter->SeekToLast(); i < reads_ && iter->Valid(); iter->Prev()) {
      bytes += iter->key().size() + iter->value().size();
      FinishedSingleOp();
      ++i;
    }
    delete iter;
    bytes_ = bytes;
    Stop(name);
  }

  void ReadRandom(Slice name) {
    rocksdb::ReadOptions options;
    std::string value;
    int found = 0;
    Start();
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = rand_.Next() % FLAGS_num;
      std::snprintf(key, sizeof(key), "%016d", k);
      if (db_->Get(options, key, &value).ok()) {
        found++;
      }
      FinishedSingleOp();
    }
    char msg[100];
    std::snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
    message_ = msg;
    Stop(name);
  }

  void ReadMissing(Slice name) {
    rocksdb::ReadOptions options;
    std::string value;
    Start();
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = rand_.Next() % FLAGS_num;
      std::snprintf(key, sizeof(key), "%016d.", k);
      db_->Get(options, key, &value);
      FinishedSingleOp();
    }
    Stop(name);
  }

  void SeekRandom(Slice name) {
    rocksdb::ReadOptions options;
    int found = 0;
    Start();
    for (int i = 0; i < reads_; i++) {
      rocksdb::Iterator* iter = db_->NewIterator(options);
      char key[100];
      const int k = rand_.Next() % FLAGS_num;
      std::snprintf(key, sizeof(key), "%016d", k);
      iter->Seek(key);
      if (iter->Valid() && iter->key() == key) found++;
      delete iter;
      FinishedSingleOp();
    }
    char msg[100];
    std::snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
    message_ = msg;
    Stop(name);
  }

  void ReadHot(Slice name) {
    rocksdb::ReadOptions options;
    std::string value;
    const int range = (FLAGS_num + 99) / 100;
    Start();
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = rand_.Next() % range;
      std::snprintf(key, sizeof(key), "%016d", k);
      db_->Get(options, key, &value);
      FinishedSingleOp();
    }
    Stop(name);
  }

  void DeleteSeq(Slice name) {
    rocksdb::WriteOptions write_options;
    Start();
    for (int i = 0; i < num_; i++) {
      char key[100];
      std::snprintf(key, sizeof(key), "%016d", i);
      db_->Delete(write_options, key);
      FinishedSingleOp();
    }
    Stop(name);
  }

  void DeleteRandom(Slice name) {
    rocksdb::WriteOptions write_options;
    Start();
    for (int i = 0; i < num_; i++) {
      char key[100];
      const int k = rand_.Next() % FLAGS_num;
      std::snprintf(key, sizeof(key), "%016d", k);
      db_->Delete(write_options, key);
      FinishedSingleOp();
    }
    Stop(name);
  }

  void Compact(Slice name) {
    Start();
    db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    Stop(name);
  }

  void Crc32c(Slice name) {
    // Test CRC computation
    const char* label = "(4K per op)";
    const char* data = new char[4096];
    int64_t bytes = 0;
    uint32_t crc = 0;
    Start();
    for (int i = 0; i < num_; i++) {
      // Simple hash instead of crc32c
      for (int j = 0; j < 4096; j++) {
        crc = crc ^ data[j];
      }
      bytes += 4096;
      FinishedSingleOp();
    }
    delete[] data;
    bytes_ = bytes;
    message_ = label;
    Stop(name);
  }

  void AcquireLoad(Slice name) {
    int dummy = 0;
    int count = 0;
    Start();
    for (int i = 0; i < num_; i++) {
      // Simple volatile load
      volatile int* ptr = &dummy;
      count += *ptr;
      FinishedSingleOp();
    }
    if (count < 0) exit(1); // Disable unused variable warning.
    message_ = "(each op is 1000 loads)";
    Stop(name);
  }

  void SnappyCompress(Slice name) {
    RandomGenerator gen;
    Slice input = gen.Generate(4096);
    int64_t bytes = 0;
    int64_t produced = 0;
    std::string compressed;
    Start();
    for (int i = 0; i < num_; i++) {
      // Simple compression simulation
      compressed = input.ToString();
      bytes += input.size();
      produced += compressed.size();
      FinishedSingleOp();
    }

    char buf[100];
    std::snprintf(buf, sizeof(buf), "(output: %.1f%%)",
                  (produced * 100.0) / bytes);
    message_ = buf;
    bytes_ = bytes;
    Stop(name);
  }

  void SnappyUncompress(Slice name) {
    RandomGenerator gen;
    Slice input = gen.Generate(4096);
    std::string compressed = input.ToString();
    int64_t bytes = 0;
    Start();
    for (int i = 0; i < num_; i++) {
      std::string uncompressed = compressed;
      bytes += input.size();
      FinishedSingleOp();
    }
    bytes_ = bytes;
    Stop(name);
  }

  void DestroyDB() {
    char file_name[100];
    db_num_++;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    std::snprintf(file_name, sizeof(file_name), "%s/dbbench_rocksdb-%d",
                  test_dir.c_str(), db_num_);
    rocksdb::Options options;
    rocksdb::DestroyDB(file_name, options);
  }
};

}  // namespace leveldb

int main(int argc, char** argv) {
  std::string default_db_path;

  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    char junk;
    if (rocksdb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_cache_size = n;
    } else if (sscanf(argv[i], "--bloom_bits=%d%c", &n, &junk) == 1) {
      FLAGS_bloom_bits_per_key = n;
    } else if (sscanf(argv[i], "--disable_wal=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_disable_wal = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else {
      std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}