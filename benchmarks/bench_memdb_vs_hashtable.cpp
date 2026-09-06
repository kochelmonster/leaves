// bench_memdb_vs_hashtable — Benchmark comparing leaves::_MemoryDB to
// std::unordered_map (Hash Table) and std::map (Sorted Red-Black Tree).
//
// Measures throughput (ops/s), latency (micros/op), data bandwidth (MB/s),
// and memory footprint across sequential/random insertions, lookups, scans,
// overwrites, and deletes.
//
// Build: cmake --build build --target bench_memdb_vs_hashtable -j
// Run:   ./build/bench_memdb_vs_hashtable [--num=1000000] [--key_size=16] [--val_size=100]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "leaves/intern/storage/_memstore.hpp"
#include "leaves/intern/core/_util.hpp"

namespace {

struct Flags {
  uint64_t num = 1'000'000;
  uint64_t reads = 1'000'000;
  uint64_t key_size = 32;  // 32-byte binary hash (e.g. SHA-256 / BLAKE3)
  uint64_t val_size = 100;
  uint64_t rounds = 3;
  std::string benchmarks = "fillseq,fillrandom,readrandom,readmissing,readseq,overwrite,erase";
};

uint64_t parse_u64(const char* s) { return std::strtoull(s, nullptr, 10); }

// High-resolution timer helper
inline double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Memory footprint helper (Resident Set Size in bytes)
size_t get_rss_bytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS info;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
    return static_cast<size_t>(info.WorkingSetSize);
  }
  return 0;
#elif defined(__linux__)
  FILE* fp = std::fopen("/proc/self/statm", "r");
  if (!fp) return 0;
  long rss_pages = 0;
  long dummy = 0;
  if (std::fscanf(fp, "%ld %ld", &dummy, &rss_pages) == 2) {
    std::fclose(fp);
    long page_size = sysconf(_SC_PAGESIZE);
    return static_cast<size_t>(rss_pages * page_size);
  }
  std::fclose(fp);
  return 0;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return static_cast<size_t>(usage.ru_maxrss * 1024);
  }
  return 0;
#endif
}

// Generate binary hash keys (e.g. SHA-256 / BLAKE3 style content-addressed binary hashes)
void generate_binary_hash_keys(std::vector<std::string>& out, uint64_t n, size_t key_size, uint64_t seed) {
  out.clear();
  out.reserve(n);
  std::mt19937_64 rng(seed);

  for (uint64_t i = 0; i < n; ++i) {
    std::string k(key_size, '\0');
    size_t written = 0;
    while (written < key_size) {
      uint64_t val = rng();
      size_t to_copy = std::min<size_t>(sizeof(val), key_size - written);
      std::memcpy(&k[written], &val, to_copy);
      written += to_copy;
    }
    out.push_back(std::move(k));
  }
}

// Uniform value payload generator
class ValueGenerator {
 private:
  std::string buffer_;
  size_t pos_{0};

 public:
  explicit ValueGenerator(size_t len) {
    buffer_.reserve(1024 * 1024);
    std::mt19937 rng(301);
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (size_t i = 0; i < 1024 * 1024; ++i) {
      buffer_.push_back(charset[rng() % (sizeof(charset) - 1)]);
    }
  }

  leaves::Slice generate(size_t len) {
    if (pos_ + len > buffer_.size()) {
      pos_ = 0;
    }
    const char* ptr = buffer_.data() + pos_;
    pos_ += len;
    return leaves::Slice(ptr, len);
  }

  std::string generate_str(size_t len) {
    leaves::Slice s = generate(len);
    return std::string(s.data(), s.size());
  }
};

struct OpResult {
  double seconds{0.0};
  uint64_t ops{0};
  uint64_t total_bytes{0};
  size_t mem_bytes{0};

  double micros_per_op() const {
    return ops > 0 ? (seconds * 1e6) / ops : 0.0;
  }

  double ops_per_sec() const {
    return seconds > 0.0 ? ops / seconds : 0.0;
  }

  double mb_per_sec() const {
    return seconds > 0.0 ? (total_bytes / (1024.0 * 1024.0)) / seconds : 0.0;
  }
};

// ============================================================================
// Target 1: leaves::_MemoryDB Adapter
// ============================================================================
class MemoryDBBench {
 private:
  std::unique_ptr<leaves::_MemoryStorage> storage_;
  leaves::_MemoryDB<leaves::_MemoryStorage>* db_{nullptr};

 public:
  void reset() {
    storage_ = std::make_unique<leaves::_MemoryStorage>();
    db_ = &storage_->db();
  }

  size_t memory_usage() const {
    return storage_ ? storage_->file_size() : 0;
  }

  OpResult fill(const std::vector<std::string>& keys, size_t val_size, ValueGenerator& gen) {
    reset();
    auto cursor = db_->create_cursor();
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      leaves::Slice v = gen.generate(val_size);
      total_bytes += k.size() + v.size();
      cursor->find(leaves::Slice(k));
      cursor->value(v);
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }

  OpResult overwrite(const std::vector<std::string>& keys, size_t val_size, ValueGenerator& gen) {
    auto cursor = db_->create_cursor();
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      leaves::Slice v = gen.generate(val_size);
      total_bytes += k.size() + v.size();
      cursor->find(leaves::Slice(k));
      cursor->value(v);
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }

  OpResult read_lookup(const std::vector<std::string>& lookup_keys) {
    auto cursor = db_->create_cursor();
    uint64_t total_bytes = 0;
    uint64_t found_count = 0;
    double t0 = now_seconds();

    for (const auto& k : lookup_keys) {
      cursor->find(leaves::Slice(k));
      if (cursor->is_valid()) {
        leaves::Slice val = cursor->value();
        total_bytes += k.size() + val.size();
        found_count++;
      }
    }

    double t1 = now_seconds();
    return {t1 - t0, lookup_keys.size(), total_bytes, memory_usage()};
  }

  OpResult read_seq(uint64_t expected_count) {
    auto cursor = db_->create_cursor();
    uint64_t total_bytes = 0;
    uint64_t ops = 0;
    double t0 = now_seconds();

    for (cursor->first(); cursor->is_valid(); cursor->next()) {
      leaves::Slice k = cursor->key();
      leaves::Slice v = cursor->value();
      total_bytes += k.size() + v.size();
      ops++;
    }

    double t1 = now_seconds();
    return {t1 - t0, ops, total_bytes, memory_usage()};
  }

  OpResult erase_keys(const std::vector<std::string>& keys) {
    auto cursor = db_->create_cursor();
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      cursor->find(leaves::Slice(k));
      if (cursor->is_valid()) {
        cursor->remove();
        total_bytes += k.size();
      }
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }
};

// ============================================================================
// Target 2: std::unordered_map Adapter
// ============================================================================
class UnorderedMapBench {
 private:
  std::unordered_map<std::string, std::string> map_;

 public:
  void reset() {
    map_.clear();
    map_.rehash(0);
  }

  size_t memory_usage() const {
    size_t mem = map_.bucket_count() * sizeof(void*);
    for (const auto& kv : map_) {
      // std::_Hash_node overhead: value_type + __hash_node_base (next pointer) + cached hash code
      mem += sizeof(std::pair<const std::string, std::string>) + 2 * sizeof(void*);
      if (kv.first.size() > 15) mem += kv.first.capacity() + 1;
      if (kv.second.size() > 15) mem += kv.second.capacity() + 1;
    }
    return mem;
  }

  OpResult fill(const std::vector<std::string>& keys, size_t val_size, ValueGenerator& gen) {
    reset();
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      leaves::Slice v = gen.generate(val_size);
      total_bytes += k.size() + v.size();
      map_[k] = std::string(v.data(), v.size());
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }

  OpResult overwrite(const std::vector<std::string>& keys, size_t val_size, ValueGenerator& gen) {
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      leaves::Slice v = gen.generate(val_size);
      total_bytes += k.size() + v.size();
      map_[k] = std::string(v.data(), v.size());
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }

  OpResult read_lookup(const std::vector<std::string>& lookup_keys) {
    uint64_t total_bytes = 0;
    uint64_t found_count = 0;
    double t0 = now_seconds();

    for (const auto& k : lookup_keys) {
      auto it = map_.find(k);
      if (it != map_.end()) {
        total_bytes += it->first.size() + it->second.size();
        found_count++;
      }
    }

    double t1 = now_seconds();
    return {t1 - t0, lookup_keys.size(), total_bytes, memory_usage()};
  }

  OpResult read_seq(uint64_t expected_count) {
    uint64_t total_bytes = 0;
    uint64_t ops = 0;
    double t0 = now_seconds();

    for (auto it = map_.begin(); it != map_.end(); ++it) {
      total_bytes += it->first.size() + it->second.size();
      ops++;
    }

    double t1 = now_seconds();
    return {t1 - t0, ops, total_bytes, memory_usage()};
  }

  OpResult erase_keys(const std::vector<std::string>& keys) {
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      auto it = map_.find(k);
      if (it != map_.end()) {
        total_bytes += k.size();
        map_.erase(it);
      }
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }
};

// ============================================================================
// Target 3: std::map (Sorted Red-Black Tree) Adapter
// ============================================================================
class StdMapBench {
 private:
  std::map<std::string, std::string> map_;

 public:
  void reset() {
    map_.clear();
  }

  size_t memory_usage() const {
    size_t mem = 0;
    for (const auto& kv : map_) {
      // std::_Rb_tree_node overhead: value_type + color + 3 pointers (parent, left, right)
      mem += sizeof(std::pair<const std::string, std::string>) + 4 * sizeof(void*);
      if (kv.first.size() > 15) mem += kv.first.capacity() + 1;
      if (kv.second.size() > 15) mem += kv.second.capacity() + 1;
    }
    return mem;
  }

  OpResult fill(const std::vector<std::string>& keys, size_t val_size, ValueGenerator& gen) {
    reset();
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      leaves::Slice v = gen.generate(val_size);
      total_bytes += k.size() + v.size();
      map_[k] = std::string(v.data(), v.size());
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }

  OpResult overwrite(const std::vector<std::string>& keys, size_t val_size, ValueGenerator& gen) {
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      leaves::Slice v = gen.generate(val_size);
      total_bytes += k.size() + v.size();
      map_[k] = std::string(v.data(), v.size());
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }

  OpResult read_lookup(const std::vector<std::string>& lookup_keys) {
    uint64_t total_bytes = 0;
    uint64_t found_count = 0;
    double t0 = now_seconds();

    for (const auto& k : lookup_keys) {
      auto it = map_.find(k);
      if (it != map_.end()) {
        total_bytes += it->first.size() + it->second.size();
        found_count++;
      }
    }

    double t1 = now_seconds();
    return {t1 - t0, lookup_keys.size(), total_bytes, memory_usage()};
  }

  OpResult read_seq(uint64_t expected_count) {
    uint64_t total_bytes = 0;
    uint64_t ops = 0;
    double t0 = now_seconds();

    for (auto it = map_.begin(); it != map_.end(); ++it) {
      total_bytes += it->first.size() + it->second.size();
      ops++;
    }

    double t1 = now_seconds();
    return {t1 - t0, ops, total_bytes, memory_usage()};
  }

  OpResult erase_keys(const std::vector<std::string>& keys) {
    uint64_t total_bytes = 0;
    double t0 = now_seconds();

    for (const auto& k : keys) {
      auto it = map_.find(k);
      if (it != map_.end()) {
        total_bytes += k.size();
        map_.erase(it);
      }
    }

    double t1 = now_seconds();
    return {t1 - t0, keys.size(), total_bytes, memory_usage()};
  }
};

// ============================================================================
// Benchmark Runner and Reporter
// ============================================================================
struct BenchSuite {
  Flags flags;
  std::vector<std::string> seq_keys;
  std::vector<std::string> rand_keys;
  std::vector<std::string> missing_keys;
  ValueGenerator gen{100};

  BenchSuite(const Flags& f) : flags(f), gen(f.val_size) {
    std::cout << "Generating " << flags.num << " binary hash keys ("
              << flags.key_size << "-byte binary hash, val_size="
              << flags.val_size << " bytes)..." << std::endl;

    // Generate random binary hash keys
    generate_binary_hash_keys(rand_keys, flags.num, flags.key_size, 42);

    // Sequential keys: sorted binary hashes
    seq_keys = rand_keys;
    std::sort(seq_keys.begin(), seq_keys.end());

    // Missing keys: binary hash keys generated with different seed
    generate_binary_hash_keys(missing_keys, flags.reads, flags.key_size, 999999);
  }

  bool is_enabled(const std::string& name) const {
    return flags.benchmarks.find(name) != std::string::npos;
  }

  void print_header() {
    std::cout << "\n===================================================================================================\n";
    std::cout << "      Leaves _MemoryDB vs Hash Table (std::unordered_map) & std::map [Binary Hash Keys]            \n";
    std::cout << "===================================================================================================\n";
    std::cout << "Entries: " << flags.num << " | Key: " << flags.key_size
              << "-byte binary hash | Value Size: " << flags.val_size
              << " bytes | Rounds: " << flags.rounds << "\n";
    std::cout << "---------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(15) << "Benchmark"
              << std::setw(22) << "Target"
              << std::right << std::setw(14) << "Time (s)"
              << std::setw(14) << "micros/op"
              << std::setw(18) << "Throughput (ops/s)"
              << std::setw(14) << "Rate (MB/s)"
              << std::setw(14) << "Memory (MB)"
              << "\n";
    std::cout << "---------------------------------------------------------------------------------------------------\n";
  }

  void report(const std::string& bench, const std::string& target, const OpResult& res) {
    double mem_mb = res.mem_bytes / (1024.0 * 1024.0);
    std::cout << std::left << std::setw(15) << bench
              << std::setw(22) << target
              << std::right << std::fixed << std::setprecision(4)
              << std::setw(14) << res.seconds
              << std::setprecision(3)
              << std::setw(14) << res.micros_per_op()
              << std::setprecision(0)
              << std::setw(18) << res.ops_per_sec()
              << std::setprecision(2)
              << std::setw(14) << res.mb_per_sec()
              << std::setprecision(1)
              << std::setw(14) << mem_mb
              << "\n";
  }

  void run() {
    print_header();

    MemoryDBBench memdb;
    UnorderedMapBench unord_map;
    StdMapBench std_map;

    auto run_rounds = [&](const std::string& bench_name, auto fn) -> OpResult {
      OpResult best;
      for (uint64_t r = 0; r < flags.rounds; ++r) {
        OpResult cur = fn();
        if (r == 0 || cur.seconds < best.seconds) {
          best = cur;
        }
      }
      return best;
    };

    // 1. Sequential Write (fillseq)
    if (is_enabled("fillseq")) {
      OpResult r_mem = run_rounds("fillseq", [&]() { return memdb.fill(seq_keys, flags.val_size, gen); });
      OpResult r_unord = run_rounds("fillseq", [&]() { return unord_map.fill(seq_keys, flags.val_size, gen); });
      OpResult r_std = run_rounds("fillseq", [&]() { return std_map.fill(seq_keys, flags.val_size, gen); });

      report("fillseq", "leaves::_MemoryDB", r_mem);
      report("fillseq", "std::unordered_map", r_unord);
      report("fillseq", "std::map", r_std);
      std::cout << "---------------------------------------------------------------------------------------------------\n";
    }

    // 2. Random Write (fillrandom)
    if (is_enabled("fillrandom")) {
      OpResult r_mem = run_rounds("fillrandom", [&]() { return memdb.fill(rand_keys, flags.val_size, gen); });
      OpResult r_unord = run_rounds("fillrandom", [&]() { return unord_map.fill(rand_keys, flags.val_size, gen); });
      OpResult r_std = run_rounds("fillrandom", [&]() { return std_map.fill(rand_keys, flags.val_size, gen); });

      report("fillrandom", "leaves::_MemoryDB", r_mem);
      report("fillrandom", "std::unordered_map", r_unord);
      report("fillrandom", "std::map", r_std);
      std::cout << "---------------------------------------------------------------------------------------------------\n";
    }

    // Prepare random state for read & update benchmarks
    memdb.fill(rand_keys, flags.val_size, gen);
    unord_map.fill(rand_keys, flags.val_size, gen);
    std_map.fill(rand_keys, flags.val_size, gen);

    // 3. Random Point Read (readrandom)
    if (is_enabled("readrandom")) {
      OpResult r_mem = run_rounds("readrandom", [&]() { return memdb.read_lookup(rand_keys); });
      OpResult r_unord = run_rounds("readrandom", [&]() { return unord_map.read_lookup(rand_keys); });
      OpResult r_std = run_rounds("readrandom", [&]() { return std_map.read_lookup(rand_keys); });

      report("readrandom", "leaves::_MemoryDB", r_mem);
      report("readrandom", "std::unordered_map", r_unord);
      report("readrandom", "std::map", r_std);
      std::cout << "---------------------------------------------------------------------------------------------------\n";
    }

    // 4. Missing Point Read (readmissing)
    if (is_enabled("readmissing")) {
      OpResult r_mem = run_rounds("readmissing", [&]() { return memdb.read_lookup(missing_keys); });
      OpResult r_unord = run_rounds("readmissing", [&]() { return unord_map.read_lookup(missing_keys); });
      OpResult r_std = run_rounds("readmissing", [&]() { return std_map.read_lookup(missing_keys); });

      report("readmissing", "leaves::_MemoryDB", r_mem);
      report("readmissing", "std::unordered_map", r_unord);
      report("readmissing", "std::map", r_std);
      std::cout << "---------------------------------------------------------------------------------------------------\n";
    }

    // 5. Sequential Scan / Iteration (readseq)
    if (is_enabled("readseq")) {
      OpResult r_mem = run_rounds("readseq", [&]() { return memdb.read_seq(flags.num); });
      OpResult r_unord = run_rounds("readseq", [&]() { return unord_map.read_seq(flags.num); });
      OpResult r_std = run_rounds("readseq", [&]() { return std_map.read_seq(flags.num); });

      report("readseq (ordered)", "leaves::_MemoryDB", r_mem);
      report("readseq (bucket)", "std::unordered_map", r_unord);
      report("readseq (ordered)", "std::map", r_std);
      std::cout << "---------------------------------------------------------------------------------------------------\n";
    }

    // 6. Overwrite (overwrite)
    if (is_enabled("overwrite")) {
      OpResult r_mem = run_rounds("overwrite", [&]() { return memdb.overwrite(rand_keys, flags.val_size, gen); });
      OpResult r_unord = run_rounds("overwrite", [&]() { return unord_map.overwrite(rand_keys, flags.val_size, gen); });
      OpResult r_std = run_rounds("overwrite", [&]() { return std_map.overwrite(rand_keys, flags.val_size, gen); });

      report("overwrite", "leaves::_MemoryDB", r_mem);
      report("overwrite", "std::unordered_map", r_unord);
      report("overwrite", "std::map", r_std);
      std::cout << "---------------------------------------------------------------------------------------------------\n";
    }

    // 7. Erase (erase)
    if (is_enabled("erase")) {
      OpResult r_mem = run_rounds("erase", [&]() {
        memdb.fill(rand_keys, flags.val_size, gen);
        return memdb.erase_keys(rand_keys);
      });
      OpResult r_unord = run_rounds("erase", [&]() {
        unord_map.fill(rand_keys, flags.val_size, gen);
        return unord_map.erase_keys(rand_keys);
      });
      OpResult r_std = run_rounds("erase", [&]() {
        std_map.fill(rand_keys, flags.val_size, gen);
        return std_map.erase_keys(rand_keys);
      });

      report("erase", "leaves::_MemoryDB", r_mem);
      report("erase", "std::unordered_map", r_unord);
      report("erase", "std::map", r_std);
      std::cout << "===================================================================================================\n";
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  Flags flags;

  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--num=", 6) == 0) {
      flags.num = parse_u64(argv[i] + 6);
      flags.reads = flags.num;
    } else if (std::strncmp(argv[i], "--reads=", 8) == 0) {
      flags.reads = parse_u64(argv[i] + 8);
    } else if (std::strncmp(argv[i], "--key_size=", 11) == 0) {
      flags.key_size = parse_u64(argv[i] + 11);
    } else if (std::strncmp(argv[i], "--val_size=", 11) == 0) {
      flags.val_size = parse_u64(argv[i] + 11);
    } else if (std::strncmp(argv[i], "--rounds=", 9) == 0) {
      flags.rounds = parse_u64(argv[i] + 9);
    } else if (std::strncmp(argv[i], "--benchmarks=", 13) == 0) {
      flags.benchmarks = argv[i] + 13;
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                << "Options:\n"
                << "  --num=N             Number of keys to insert/test (default: 1000000)\n"
                << "  --reads=N           Number of read lookups (default: same as --num)\n"
                << "  --key_size=N        Binary key size in bytes, e.g. 32 for SHA-256 (default: 32)\n"
                << "  --val_size=N        Value size in bytes (default: 100)\n"
                << "  --rounds=N          Number of rounds, reporting best (default: 3)\n"
                << "  --benchmarks=LIST   Comma-separated list of benchmarks to run (default: all)\n"
                << "                      Available: fillseq,fillrandom,readrandom,readmissing,readseq,overwrite,erase\n";
      return 0;
    } else {
      std::cerr << "Unknown flag: " << argv[i] << " (use --help for usage)\n";
      return 1;
    }
  }

  BenchSuite suite(flags);
  suite.run();
  return 0;
}
