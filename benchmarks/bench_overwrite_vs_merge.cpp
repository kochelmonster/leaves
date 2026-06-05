// bench_overwrite_vs_merge — precise apples-to-apples comparison of two ways
// to apply 100k overwrites to a 1M-key database:
//
//   Option 1: overwrite the 100k keys DIRECTLY in the main DB (one txn).
//   Option 2: write the 100k overwrites into a SEPARATE DB, then MERGE that
//             DB into the main DB (one merge / one txn). Only the merge is timed.
//
// Both options start from an identical, freshly built 1M-key main DB, so the
// destination state is the same in both cases. The 100k overwrite keys are a
// subset of the 1M keys already present (true overwrites, not inserts).
//
// Build: target bench_overwrite_vs_merge (see CMakeLists.txt).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "leaves/confluence.hpp"
#include "leaves/mmap.hpp"
#include "leaves/intern/util/_merger.hpp"

using namespace leaves;

namespace {

using Storage = MapStorage;
using InternalDB = _DB<Storage::StorageImpl>;

struct Flags {
  uint64_t num = 1'000'000;     // keys preloaded into main
  uint64_t overwrites = 100'000;  // keys to overwrite
  uint64_t key_size = 16;
  uint64_t value_size = 100;
  uint64_t rounds = 3;          // repeat; best (min) time reported
  bool random = true;           // random vs sequential overwrite-key selection
};

uint64_t parse_u64(const char* s) { return std::strtoull(s, nullptr, 10); }

void make_key(std::string& out, uint64_t n, size_t key_size) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%020llu", (unsigned long long)n);
  out.assign(buf, std::min<size_t>(key_size, 20));
  if (out.size() < key_size) out.append(key_size - out.size(), '0');
}

std::string temp_path(const char* tag) {
  auto dir = std::filesystem::temp_directory_path();
  auto p = dir / ("bench_ovm_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()) +
                  ".lvs");
  return p.string();
}

// Overwrite-key index i maps to main-key index in [0, num).
uint64_t overwrite_index(uint64_t i, const Flags& f) {
  if (!f.random) return i;  // first `overwrites` keys
  // Deterministic scattered subset across the whole 1M key space.
  return (i * 2654435761u + 0x9E3779B97F4A7C15ull) % f.num;
}

// Build a 1M-key main DB at `path`. Returns the storage (kept alive by caller).
std::shared_ptr<Storage> build_main(const std::string& path, const Flags& f) {
  auto storage = Storage::create(path.c_str(), 16 * G);
  auto db = storage->open("main");
  auto cursor = db.cursor();
  std::string key, val(f.value_size, 'A');
  const uint64_t batch = 1000;
  for (uint64_t i = 0; i < f.num; i += batch) {
    uint64_t end = std::min(i + batch, f.num);
    for (uint64_t j = i; j < end; ++j) {
      make_key(key, j, f.key_size);
      cursor.find(Slice(key));
      cursor.value(Slice(val));
    }
    cursor.commit();
  }
  return storage;
}

// Merge entire src DB into dst DB via _Merger, committing one txn. Returns
// seconds spent inside the merge+commit only.
double merge_src_into_dst(InternalDB& dst_db, InternalDB& src_db) {
  using DstCursorTraits = typename InternalDB::CursorTraits;
  using SrcCursorTraits = typename InternalDB::CursorTraits;
  using DstCursor = _TransactionalCursor<DstCursorTraits>;
  using SrcCursor = _Cursor<SrcCursorTraits>;

  auto src_root = &src_db.txn()->root;
  auto dst_root = &dst_db.txn()->root;
  uint64_t cursor_id = dst_db.new_cursor_id();

  DstCursor dst_cursor(&dst_db, dst_root);
  SrcCursor src_cursor(&src_db, src_root);
  src_cursor.clear();

  StandardMergePolicy handler;

  auto t0 = std::chrono::steady_clock::now();
  dst_cursor.start_transaction();
  _Merger<DstCursor, SrcCursor, StandardMergePolicy> merger(dst_cursor,
                                                            src_cursor, handler);
  merger.exec();
  dst_cursor.commit(cursor_id);
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(t1 - t0).count();
}

// Option 1: overwrite `overwrites` keys directly in a fresh main DB. Returns
// seconds spent inside the overwrite transaction only.
double run_option1_direct(const Flags& f) {
  std::string main_path = temp_path("o1");
  auto storage = build_main(main_path, f);
  auto db = storage->open("main");
  auto cursor = db.cursor();

  std::string key, val(f.value_size, 'B');  // different value => real overwrite

  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < f.overwrites; ++i) {
    make_key(key, overwrite_index(i, f), f.key_size);
    cursor.find(Slice(key));
    cursor.value(Slice(val));
  }
  cursor.commit();
  auto t1 = std::chrono::steady_clock::now();

  storage.reset();
  std::filesystem::remove(main_path);
  return std::chrono::duration<double>(t1 - t0).count();
}

// Option 2: write `overwrites` into a separate DB, then merge it into a fresh
// main DB. Returns seconds spent inside the MERGE only (fill not counted).
double run_option2_merge(const Flags& f) {
  std::string main_path = temp_path("o2main");
  std::string src_path = temp_path("o2src");
  auto main_storage = build_main(main_path, f);

  // Build the overwrite DB (NOT timed).
  auto src_storage = Storage::create(src_path.c_str(), 16 * G);
  {
    auto src_db = src_storage->open("main");
    auto cursor = src_db.cursor();
    std::string key, val(f.value_size, 'B');
    const uint64_t batch = 1000;
    for (uint64_t i = 0; i < f.overwrites; i += batch) {
      uint64_t end = std::min(i + batch, f.overwrites);
      for (uint64_t j = i; j < end; ++j) {
        make_key(key, overwrite_index(j, f), f.key_size);
        cursor.find(Slice(key));
        cursor.value(Slice(val));
      }
      cursor.commit();
    }
  }

  auto main_db = main_storage->open("main");
  auto src_db = src_storage->open("main");
  double secs = merge_src_into_dst(*main_db._internal(), *src_db._internal());

  // Verify the merge actually applied the overwrites (value 'B', not 'A').
  {
    auto vc = main_storage->open("main").cursor();
    std::string key;
    uint64_t checked = 0, wrong = 0;
    for (uint64_t i = 0; i < f.overwrites; i += (f.overwrites / 1000 + 1)) {
      make_key(key, overwrite_index(i, f), f.key_size);
      vc.find(Slice(key));
      if (!vc.is_valid() || vc.value().size() == 0 ||
          ((const char*)vc.value().data())[0] != 'B')
        ++wrong;
      ++checked;
    }
    if (wrong)
      std::fprintf(stderr,
                   "[WARN] merge verification: %llu/%llu sampled keys not "
                   "overwritten!\n",
                   (unsigned long long)wrong, (unsigned long long)checked);
  }

  main_storage.reset();
  src_storage.reset();
  std::filesystem::remove(main_path);
  std::filesystem::remove(src_path);
  return secs;
}

}  // namespace

int main(int argc, char** argv) {
  Flags f;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!std::strncmp(a, "--num=", 6)) f.num = parse_u64(a + 6);
    else if (!std::strncmp(a, "--overwrites=", 13)) f.overwrites = parse_u64(a + 13);
    else if (!std::strncmp(a, "--key_size=", 11)) f.key_size = parse_u64(a + 11);
    else if (!std::strncmp(a, "--value_size=", 13)) f.value_size = parse_u64(a + 13);
    else if (!std::strncmp(a, "--rounds=", 9)) f.rounds = parse_u64(a + 9);
    else if (!std::strncmp(a, "--random=", 9)) f.random = parse_u64(a + 9) != 0;
    else {
      std::fprintf(stderr, "unknown flag: %s\n", a);
      return 1;
    }
  }
  if (f.overwrites > f.num) f.overwrites = f.num;

  std::printf("config: num=%llu overwrites=%llu key_size=%llu value_size=%llu "
              "rounds=%llu random=%d\n",
              (unsigned long long)f.num, (unsigned long long)f.overwrites,
              (unsigned long long)f.key_size,
              (unsigned long long)f.value_size,
              (unsigned long long)f.rounds, (int)f.random);

  double best1 = 1e30, best2 = 1e30;
  for (uint64_t r = 0; r < f.rounds; ++r) {
    double t1 = run_option1_direct(f);
    double t2 = run_option2_merge(f);
    best1 = std::min(best1, t1);
    best2 = std::min(best2, t2);
    std::printf("round %llu: direct=%.3f ms  merge=%.3f ms\n",
                (unsigned long long)r, t1 * 1e3, t2 * 1e3);
  }

  double us_key1 = best1 * 1e6 / f.overwrites;
  double us_key2 = best2 * 1e6 / f.overwrites;
  std::printf("\n=== BEST (min over %llu rounds) ===\n",
              (unsigned long long)f.rounds);
  std::printf("Option 1 (direct overwrite): %.3f ms   (%.4f us/key)\n",
              best1 * 1e3, us_key1);
  std::printf("Option 2 (merge separate DB): %.3f ms   (%.4f us/key)\n",
              best2 * 1e3, us_key2);
  if (best1 < best2)
    std::printf("=> DIRECT is faster by %.2fx\n", best2 / best1);
  else
    std::printf("=> MERGE is faster by %.2fx\n", best1 / best2);
  return 0;
}
