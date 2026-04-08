// Benchmark: Leaves _BrowserStore vs bare IndexedDB
//
// Compares the leaves trie-based storage engine (over IndexedDB) against
// raw emscripten_idb_store/emscripten_idb_load calls.
//
// Build: emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release -G Ninja
//        cmake --build build-wasm -j
// Run:   node --require fake-indexeddb/auto build-wasm/db_bench_browser.js
//
// Based on the benchmark structure from db_bench_leaves.cpp (LevelDB authors).

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <leaves/intern/storage/_browserstore.hpp>

// ── Configuration flags ──────────────────────────────────────────

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
    "idb_fillseq,"
    "idb_fillrandom,"
    "idb_overwrite,"
    "idb_readrandom,"
    "idb_readseq,"
    "idb_fillrand100K,"
    "idb_fillseq100K,"
    "idb_readseq100K,"
    "idb_readrand100K,";

static int FLAGS_num = 10000;
static int FLAGS_reads = -1;
static int FLAGS_value_size = 100;
static int FLAGS_batch_size = 1000;

// ── Minimal PRNG (LCG, same seed as db_bench_leaves) ────────────

class Random {
  uint32_t seed_;

 public:
  explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
    if (seed_ == 0 || seed_ == 2147483647L) seed_ = 1;
  }
  uint32_t Next() {
    static const uint32_t M = 2147483647L;  // 2^31-1
    static const uint64_t A = 16807;
    uint64_t product = seed_ * A;
    seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
    if (seed_ > M) seed_ -= M;
    return seed_;
  }
};

// ── Random data generator ────────────────────────────────────────

class RandomGenerator {
  std::vector<char> data_;
  int pos_;

 public:
  RandomGenerator() : pos_(0) {
    Random rnd(301);
    // Generate 1 MB of pseudo-random data
    data_.resize(1048576);
    for (size_t i = 0; i < data_.size(); i++) {
      data_[i] = static_cast<char>(rnd.Next() & 0xff);
    }
  }

  leaves::Slice Generate(int len) {
    if (pos_ + len > static_cast<int>(data_.size())) {
      pos_ = 0;
    }
    pos_ += len;
    return leaves::Slice(data_.data() + pos_ - len, len);
  }
};

// ── High-resolution timer ────────────────────────────────────────

static double NowSeconds() {
  auto now = std::chrono::high_resolution_clock::now();
  auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
  return us.count() * 1e-6;
}

// ── Raw IndexedDB helpers ────────────────────────────────────────

static const char* RAW_IDB_NAME = "bench_raw_idb";

static void raw_idb_put(const char* key, const void* data, size_t size) {
  int error = 0;
  emscripten_idb_store(RAW_IDB_NAME, key, const_cast<void*>(data), size,
                       &error);
  if (error) {
    std::fprintf(stderr, "raw_idb_put failed for key: %s\n", key);
  }
}

static bool raw_idb_get(const char* key, void* buf, size_t buf_size) {
  void* loaded = nullptr;
  int loaded_size = 0;
  int error = 0;
  emscripten_idb_load(RAW_IDB_NAME, key, &loaded, &loaded_size, &error);
  if (error || !loaded) return false;
  size_t copy = std::min(buf_size, static_cast<size_t>(loaded_size));
  std::memcpy(buf, loaded, copy);
  free(loaded);
  return true;
}

static void raw_idb_clear() {
  // Delete the entire IDB database so next benchmark starts fresh
  int error = 0;
  emscripten_idb_delete(RAW_IDB_NAME, "header", &error);
  // Best-effort: delete known keys; IDB has no "drop database" via emscripten_idb_*
  // We'll just create a new DB name per fresh run instead
}

// ── Benchmark class ──────────────────────────────────────────────

class Benchmark {
  enum Order { SEQUENTIAL, RANDOM_ORDER };
  enum DBState { FRESH, EXISTING };

  int num_;
  int reads_;
  double start_;
  int64_t bytes_;
  std::string message_;
  RandomGenerator gen_;
  Random rand_;
  int done_;

  // Leaves storage — lazily created
  leaves::_BrowserStore* store_;
  int db_num_;
  // Raw IDB counter for fresh databases
  int idb_num_;
  // Unique per-run ID to avoid IDB name collisions across repeated runs
  int run_id_;

  void PrintHeader() {
    const int kKeySize = 16;
    std::fprintf(stdout, "Engine:      Leaves _BrowserStore vs bare IndexedDB\n");
    std::fprintf(stdout, "Keys:        %d bytes each\n", kKeySize);
    std::fprintf(stdout, "Values:      %d bytes each\n", FLAGS_value_size);
    std::fprintf(stdout, "Entries:     %d\n", num_);
    std::fprintf(stdout, "RawSize:     %.1f MB (estimated)\n",
                 ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_) /
                  1048576.0));
#ifndef NDEBUG
    std::fprintf(stdout,
                 "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
    std::fprintf(stdout, "------------------------------------------------\n");
  }

  void Start() {
    start_ = NowSeconds();
    bytes_ = 0;
    message_.clear();
    done_ = 0;
  }

  void FinishedSingleOp() {
    done_++;
    if (done_ % 10000 == 0) {
      std::fprintf(stderr, "... finished %d ops\r", done_);
      std::fflush(stderr);
    }
  }

  void Stop(const char* name) {
    double finish = NowSeconds();
    if (done_ < 1) done_ = 1;

    if (bytes_ > 0) {
      char rate[100];
      std::snprintf(rate, sizeof(rate), "%6.1f MB/s",
                    (bytes_ / 1048576.0) / (finish - start_));
      if (!message_.empty())
        message_ = std::string(rate) + " " + message_;
      else
        message_ = rate;
    }

    std::fprintf(stdout, "%-16s : %11.3f micros/op;%s%s\n",
                 name, (finish - start_) * 1e6 / done_,
                 (message_.empty() ? "" : " "), message_.c_str());
    std::fflush(stdout);
  }

  // ── Leaves _BrowserStore operations ────────────────────────────

  void OpenLeaves() {
    delete store_;
    store_ = nullptr;
    db_num_++;
    char name[64];
    std::snprintf(name, sizeof(name), "bl_%d_%d", run_id_, db_num_);
    store_ = new leaves::_BrowserStore(name, 10 * leaves::M, 0);
  }

  void LeavesWrite(bool sync, Order order, DBState state, int num_entries,
                   int value_size, int entries_per_batch) {
    if (state == FRESH) {
      OpenLeaves();
      Start();
    }
    if (num_entries != num_) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_entries);
      message_ = msg;
    }

    char key[100];
    auto* db = (*store_)["benchmark"];
    auto cursor = db->create_cursor();

    for (int i = 0; i < num_entries; i += entries_per_batch) {
      for (int j = 0; j < entries_per_batch; j++) {
        const int k =
            (order == SEQUENTIAL) ? i + j : (rand_.Next() % num_entries);
        std::snprintf(key, sizeof(key), "%016d", k);
        leaves::Slice mkey(key);
        leaves::Slice mval = gen_.Generate(value_size);
        bytes_ += value_size + mkey.size();

        cursor->find(mkey);
        cursor->value(mval);
        FinishedSingleOp();
      }
      cursor->commit(sync);
    }
  }

  void LeavesReadSequential() {
    auto* db = (*store_)["benchmark"];
    auto cursor = db->create_cursor();
    for (cursor->first(); cursor->is_valid(); cursor->next()) {
      leaves::Slice k = cursor->key();
      leaves::Slice v = cursor->value();
      bytes_ += k.size() + v.size();
      FinishedSingleOp();
    }
  }

  void LeavesReadRandom() {
    char key[100];
    auto* db = (*store_)["benchmark"];
    auto cursor = db->create_cursor();
    for (int i = 0; i < reads_; i++) {
      const int k = rand_.Next() % reads_;
      std::snprintf(key, sizeof(key), "%016d", k);
      cursor->find(leaves::Slice(key));
      if (cursor->is_valid()) {
        bytes_ += cursor->key().size() + cursor->value().size();
      }
      FinishedSingleOp();
    }
  }

  // ── Raw IndexedDB operations ───────────────────────────────────

  std::string raw_db_name_;

  void OpenRawIDB() {
    idb_num_++;
    char name[64];
    std::snprintf(name, sizeof(name), "br_%d_%d", run_id_, idb_num_);
    raw_db_name_ = name;
  }

  const char* raw_db() const { return raw_db_name_.c_str(); }

  void RawIDBWrite(bool /*sync*/, Order order, DBState state, int num_entries,
                   int value_size, int entries_per_batch) {
    if (state == FRESH) {
      OpenRawIDB();
      Start();
    }
    if (num_entries != num_) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_entries);
      message_ = msg;
    }

    char key[100];
    for (int i = 0; i < num_entries; i += entries_per_batch) {
      for (int j = 0; j < entries_per_batch; j++) {
        const int k =
            (order == SEQUENTIAL) ? i + j : (rand_.Next() % num_entries);
        std::snprintf(key, sizeof(key), "%016d", k);
        leaves::Slice mval = gen_.Generate(value_size);
        bytes_ += value_size + static_cast<int>(strlen(key));

        int error = 0;
        emscripten_idb_store(raw_db(), key, const_cast<char*>(mval.data()),
                             mval.size(), &error);
        FinishedSingleOp();
      }
      // Raw IDB has no batch/commit concept — each store is immediate
    }
  }

  void RawIDBReadSequential() {
    // emscripten_idb_* has no cursor API; iterate by regenerating keys 0..N-1
    char key[100];
    std::vector<char> buf(FLAGS_value_size + 1024);
    for (int i = 0; i < num_; i++) {
      std::snprintf(key, sizeof(key), "%016d", i);
      void* loaded = nullptr;
      int loaded_size = 0;
      int error = 0;
      emscripten_idb_load(raw_db(), key, &loaded, &loaded_size, &error);
      if (!error && loaded) {
        bytes_ += static_cast<int>(strlen(key)) + loaded_size;
        free(loaded);
      }
      FinishedSingleOp();
    }
  }

  void RawIDBReadRandom() {
    char key[100];
    for (int i = 0; i < reads_; i++) {
      const int k = rand_.Next() % reads_;
      std::snprintf(key, sizeof(key), "%016d", k);
      void* loaded = nullptr;
      int loaded_size = 0;
      int error = 0;
      emscripten_idb_load(raw_db(), key, &loaded, &loaded_size, &error);
      if (!error && loaded) {
        bytes_ += static_cast<int>(strlen(key)) + loaded_size;
        free(loaded);
      }
      FinishedSingleOp();
    }
  }

 public:
  Benchmark()
      : num_(FLAGS_num),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        start_(0),
        bytes_(0),
        rand_(301),
        done_(0),
        store_(nullptr),
        db_num_(0),
        idb_num_(0),
        run_id_(static_cast<int>(NowSeconds() * 1000) & 0xFFFFFFF) {}

  ~Benchmark() { delete store_; }

  void Run() {
    PrintHeader();

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != nullptr) {
      const char* sep = std::strchr(benchmarks, ',');
      std::string name;
      if (sep == nullptr) {
        name = benchmarks;
        benchmarks = nullptr;
      } else {
        name = std::string(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }
      if (name.empty()) continue;

      Start();
      bool known = true;
      bool write_sync = false;

      // ── Leaves benchmarks ──────────────────────────────────────
      if (name == "fillseq") {
        LeavesWrite(false, SEQUENTIAL, FRESH, num_, FLAGS_value_size,
                    FLAGS_batch_size);
      } else if (name == "fillrandom") {
        LeavesWrite(false, RANDOM_ORDER, FRESH, num_, FLAGS_value_size,
                    FLAGS_batch_size);
      } else if (name == "overwrite") {
        LeavesWrite(false, RANDOM_ORDER, EXISTING, num_, FLAGS_value_size,
                    FLAGS_batch_size);
      } else if (name == "fillseqsync") {
        LeavesWrite(true, SEQUENTIAL, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == "fillrandsync") {
        LeavesWrite(true, RANDOM_ORDER, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == "fillseq100K") {
        LeavesWrite(false, SEQUENTIAL, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == "fillrand100K") {
        LeavesWrite(false, RANDOM_ORDER, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == "readseq") {
        LeavesReadSequential();
      } else if (name == "readrandom") {
        LeavesReadRandom();
      } else if (name == "readseq100K") {
        int repeats = std::max(1, num_ / 1000);
        for (int r = 0; r < repeats; r++) LeavesReadSequential();
      } else if (name == "readrand100K") {
        int n = reads_;
        reads_ = std::max(100, num_ / 1000);
        LeavesReadRandom();
        reads_ = n;
      }
      // ── Raw IndexedDB benchmarks ──────────────────────────────
      else if (name == "idb_fillseq") {
        RawIDBWrite(false, SEQUENTIAL, FRESH, num_, FLAGS_value_size,
                    FLAGS_batch_size);
      } else if (name == "idb_fillrandom") {
        RawIDBWrite(false, RANDOM_ORDER, FRESH, num_, FLAGS_value_size,
                    FLAGS_batch_size);
      } else if (name == "idb_overwrite") {
        RawIDBWrite(false, RANDOM_ORDER, EXISTING, num_, FLAGS_value_size,
                    FLAGS_batch_size);
      } else if (name == "idb_fillseqsync") {
        RawIDBWrite(true, SEQUENTIAL, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == "idb_fillrandsync") {
        RawIDBWrite(true, RANDOM_ORDER, FRESH, num_ / 100, FLAGS_value_size, 1);
      } else if (name == "idb_fillseq100K") {
        RawIDBWrite(false, SEQUENTIAL, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == "idb_fillrand100K") {
        RawIDBWrite(false, RANDOM_ORDER, FRESH, num_ / 1000, 100 * 1000, 1);
      } else if (name == "idb_readseq") {
        RawIDBReadSequential();
      } else if (name == "idb_readrandom") {
        RawIDBReadRandom();
      } else if (name == "idb_readseq100K") {
        int n = num_;
        num_ /= 1000;
        int repeats = std::max(1, n / 1000);
        for (int r = 0; r < repeats; r++) RawIDBReadSequential();
        num_ = n;
      } else if (name == "idb_readrand100K") {
        int n = reads_;
        reads_ = std::max(100, num_ / 1000);
        RawIDBReadRandom();
        reads_ = n;
      } else {
        known = false;
        if (!name.empty()) {
          std::fprintf(stderr, "unknown benchmark '%s'\n", name.c_str());
        }
      }

      if (known) {
        Stop(name.c_str());
      }
    }
  }
};

// ── main ─────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    int n;
    char junk;
    if (strncmp(argv[i], "--benchmarks=", 13) == 0) {
      FLAGS_benchmarks = argv[i] + 13;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--batch_size=%d%c", &n, &junk) == 1) {
      FLAGS_batch_size = n;
    } else {
      std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      return 1;
    }
  }

  Benchmark benchmark;
  benchmark.Run();
  return 0;
}

#else
int main() {
  std::fprintf(stderr, "db_bench_browser requires Emscripten compilation\n");
  return 1;
}
#endif
