// bench_merge — isolates and times ONLY the confluence merge path
// (tributary -> main DB). Auto-merge is suppressed during the fill phase by
// setting the write threshold and attach-age to their maxima, so the only
// merges that happen are the ones triggered by the timed merge_all_now() call.
//
// The per-merge fixed cost (cursor construction etc.) is amortized over the
// keys in a tributary, so the "many small tributaries" configuration
// (--tribs large, few keys each) is the one that exposes per-merge overhead;
// "few large tributaries" is the contrast case.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "leaves/confluence.hpp"
#include "leaves/mmap.hpp"

namespace {

// MAX_TRIBUTARIES is 128; keeping more than that many write cursors alive at
// once would exhaust the slot pool (no merge can run while their cursors live).
// So a "wave" of at most kMaxWave live cursors is filled, then merged, before
// the next wave starts. Merge time is summed across waves.
constexpr uint64_t kMaxWave = 100;

struct Flags {
  uint64_t num = 1'000'000;   // total keys across all tributaries
  uint64_t tribs = 100;       // number of tributaries (fill cursors)
  uint64_t key_size = 16;     // key length in bytes
  uint64_t value_size = 100;  // value length in bytes
  uint64_t rounds = 3;        // repeat for stable timing (best is reported)
  uint64_t prefill = 0;       // keys preloaded into main before timing
  double del_frac = 0.0;      // fraction of tributary ops that are deletes
  bool random = true;         // random vs sequential keys
};

uint64_t parse_u64(const char* s) { return std::strtoull(s, nullptr, 10); }

void make_key(std::string& out, uint64_t n, size_t key_size, bool random) {
  uint64_t v = random ? (n * 2654435761u + 0x9E3779B97F4A7C15ull) : n;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%020llu", (unsigned long long)v);
  out.assign(buf, std::min<size_t>(key_size, 20));
  if (out.size() < key_size) out.append(key_size - out.size(), '0');
}

std::string temp_path() {
  auto dir = std::filesystem::temp_directory_path();
  auto p = dir / ("bench_merge_" + std::to_string(::getpid()) + "_" +
                  std::to_string(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()) +
                  ".lvs");
  return p.string();
}

struct Result {
  double merge_seconds = 0;
  uint64_t keys_merged = 0;
  uint64_t tribs_merged = 0;  // high-water tributary count
};

Result run_round(const Flags& f) {
  using namespace leaves;

  std::string path = temp_path();
  std::filesystem::remove(path);

  Result r;
  {
    auto storage = MapStorage::create(path.c_str(), 16 * G);
    MapConfluenceDB db(storage, "bench_merge");
    // Suppress every auto-merge trigger so the only merges are ours.
    db.set_merge_write_threshold(UINT32_MAX);
    db.set_max_attached_age_ms(UINT64_MAX);

    std::string key;
    std::string value(f.value_size, 'v');

    // Optional: preload the main DB so merges hit a non-empty target (the
    // realistic, slower path). Untimed.
    if (f.prefill) {
      auto c = db.cursor();
      c.start_transaction();
      for (uint64_t i = 0; i < f.prefill; ++i) {
        make_key(key, i, f.key_size, f.random);
        c.find(Slice(key));
        c.value(Slice(value));
      }
      c.commit();
      db.merge_all_now();
    }

    // Fill phase: spread `num` keys across `tribs` tributaries. At most
    // kMaxWave cursors are alive at once (slot pool cap); each wave is filled
    // with cursors kept alive (so no slot auto-merges early), then merged via
    // a timed merge_all_now(). Times are summed across waves.
    uint64_t per_trib = f.num / std::max<uint64_t>(f.tribs, 1);
    uint64_t total = 0;
    uint64_t high_water = 0;
    double merge_seconds = 0;

    uint64_t done_tribs = 0;
    while (done_tribs < f.tribs) {
      uint64_t wave = std::min<uint64_t>(kMaxWave, f.tribs - done_tribs);
      std::vector<MapConfluenceDB::Cursor> cursors;
      cursors.reserve(wave);
      for (uint64_t w = 0; w < wave; ++w) {
        uint64_t t = done_tribs + w;
        cursors.push_back(db.cursor());
        auto& c = cursors.back();
        c.start_transaction();
        for (uint64_t i = 0; i < per_trib; ++i) {
          uint64_t n = f.prefill + t * per_trib + i;
          make_key(key, n, f.key_size, f.random);
          c.find(Slice(key));
          if (f.del_frac > 0.0 &&
              (double)(i % 100) / 100.0 < f.del_frac && f.prefill) {
            // delete an already-present (prefilled) key to exercise Pass 1
            make_key(key, n % f.prefill, f.key_size, f.random);
            c.find(Slice(key));
            if (c.is_valid()) c.remove();
          } else {
            c.value(Slice(value));
          }
          ++total;
        }
        c.commit();  // slot -> ATTACHED; cursor kept alive so no auto-merge
      }

      // Timed: merge this wave's tributaries into main.
      auto t0 = std::chrono::steady_clock::now();
      db.merge_all_now();
      auto t1 = std::chrono::steady_clock::now();
      merge_seconds += std::chrono::duration<double>(t1 - t0).count();
      high_water = std::max<uint64_t>(
          high_water, db._internal()->_tributaries_count.load(
                          std::memory_order_acquire));
      done_tribs += wave;
      // cursors destroyed here (already merged -> cheap)
    }

    r.keys_merged = total;
    r.merge_seconds = merge_seconds;
    r.tribs_merged = high_water;

    // Sanity: a key written through a tributary must now be readable from main.
    {
      auto c = db.cursor();
      make_key(key, f.prefill, f.key_size, f.random);
      c.find(Slice(key));
      if (!c.is_valid() && f.del_frac == 0.0) {
        std::fprintf(stderr, "WARNING: sanity check failed (key not found)\n");
      }
    }
  }
  std::filesystem::remove(path);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  Flags f;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto eq = std::strchr(a, '=');
    if (!eq) continue;
    std::string name(a, eq);
    const char* val = eq + 1;
    if (name == "--num") f.num = parse_u64(val);
    else if (name == "--tribs") f.tribs = parse_u64(val);
    else if (name == "--key_size") f.key_size = parse_u64(val);
    else if (name == "--value_size") f.value_size = parse_u64(val);
    else if (name == "--rounds") f.rounds = parse_u64(val);
    else if (name == "--prefill") f.prefill = parse_u64(val);
    else if (name == "--del_frac") f.del_frac = std::strtod(val, nullptr);
    else if (name == "--random") f.random = parse_u64(val) != 0;
    else {
      std::fprintf(stderr, "unknown flag: %s\n", a);
      return 2;
    }
  }

  std::printf(
      "bench_merge: num=%llu tribs=%llu key_size=%llu value_size=%llu "
      "prefill=%llu del_frac=%.2f random=%d rounds=%llu\n",
      (unsigned long long)f.num, (unsigned long long)f.tribs,
      (unsigned long long)f.key_size, (unsigned long long)f.value_size,
      (unsigned long long)f.prefill, f.del_frac, (int)f.random,
      (unsigned long long)f.rounds);

  Result best;
  best.merge_seconds = 1e300;
  for (uint64_t round = 0; round < f.rounds; ++round) {
    Result r = run_round(f);
    double us_per_key = r.merge_seconds * 1e6 / std::max<uint64_t>(r.keys_merged, 1);
    double keys_per_sec = r.keys_merged / std::max(r.merge_seconds, 1e-12);
    double mb_per_sec =
        (double)r.keys_merged * (f.key_size + f.value_size) /
        std::max(r.merge_seconds, 1e-12) / (1024.0 * 1024.0);
    double us_per_trib =
        r.merge_seconds * 1e6 / std::max<uint64_t>(r.tribs_merged, 1);
    std::printf(
        "  round %llu: merge %8.3f ms | %7.4f us/key | %8.0f keys/s | "
        "%7.1f MB/s | tribs=%llu | %7.2f us/trib\n",
        (unsigned long long)round, r.merge_seconds * 1e3, us_per_key,
        keys_per_sec, mb_per_sec, (unsigned long long)r.tribs_merged,
        us_per_trib);
    if (r.merge_seconds < best.merge_seconds) best = r;
  }

  double us_per_key =
      best.merge_seconds * 1e6 / std::max<uint64_t>(best.keys_merged, 1);
  double keys_per_sec = best.keys_merged / std::max(best.merge_seconds, 1e-12);
  double mb_per_sec = (double)best.keys_merged * (f.key_size + f.value_size) /
                      std::max(best.merge_seconds, 1e-12) / (1024.0 * 1024.0);
  std::printf(
      "BEST: merge %8.3f ms | %7.4f us/key | %8.0f keys/s | %7.1f MB/s | "
      "tribs=%llu\n",
      best.merge_seconds * 1e3, us_per_key, keys_per_sec, mb_per_sec,
      (unsigned long long)best.tribs_merged);
  return 0;
}
