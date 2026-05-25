// Emscripten variant of bench_page_sizes — collects the raw allocator-size
// histogram for the BrowserStorage backend.
//
// Build:
//   source /home/michael/src/emsdk/emsdk_env.sh
//   emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release -G Ninja
//   cmake --build build-wasm -j --target bench_page_sizes_browser
//
// Run (uses fake-indexeddb under node):
//   npm install --no-save fake-indexeddb
//   node --require fake-indexeddb/auto \
//        build-wasm/bench_page_sizes_browser.js [--num=N] [--vsize=V] \
//      | tee /tmp/bench_browser.out
//   grep '^HIST,' /tmp/bench_browser.out | sed 's/^HIST,//' \
//      > /tmp/leaves_browser_hist.csv
//   python3 tools/page_sizes_solver.py /tmp/leaves_browser_hist.csv \
//      --current=40,56,72,120,152,568,1024,1048,1232,4096
//
// The bench skips commit()/lookup() so it never touches IndexedDB; only the
// in-memory _DB::alloc_page hook (LEAVES_PAGE_HIST) is exercised, which is
// all that's needed to drive the page-sizing solver.

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <typeinfo>
#include <vector>

#include "leaves/browserstore.hpp"
#include "leaves/intern/util/_page_hist.hpp"

using namespace leaves;

// ── Flags ──────────────────────────────────────────────────────
static int FLAGS_num = 10000;
static int FLAGS_vsize = 100;
static const char* FLAGS_vsize_sweep = "8,100,1000";
static int FLAGS_scenario = -1;  // -1 = all, otherwise index
static int FLAGS_only_vsize = -1;

// ── Key generators (identical to native bench_page_sizes.cpp) ───
static std::vector<std::string> gen_random_binary(int n) {
  std::mt19937_64 rng(42);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    uint64_t v = rng();
    keys.emplace_back(reinterpret_cast<const char*>(&v), sizeof(v));
  }
  return keys;
}

static std::vector<std::string> gen_binary_hash(int n) {
  std::mt19937_64 rng(42);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k(32, '\0');
    for (int j = 0; j < 4; j++) {
      uint64_t v = rng();
      memcpy(&k[j * 8], &v, 8);
    }
    keys.push_back(std::move(k));
  }
  return keys;
}

static std::vector<std::string> gen_hex_strings(int n) {
  static const char hex[] = "0123456789abcdef";
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 15);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k(40, '\0');
    for (auto& c : k) c = hex[dist(rng)];
    keys.push_back(std::move(k));
  }
  return keys;
}

static std::vector<std::string> gen_decimal_integers(int n) {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%010d", i);
    keys.emplace_back(buf);
  }
  return keys;
}

static std::vector<std::string> gen_path_keys(int n) {
  static const char* segments[] = {
      "users",   "posts",   "config",  "data",    "cache",  "logs",
      "auth",    "api",     "web",     "service", "admin",  "public",
      "db",      "storage", "backup",  "temp",    "queue",  "events",
      "tasks",   "metrics", "alerts",  "reports", "billing","search",
  };
  static const char* names[] = {
      "alice",   "bob",     "charlie", "diana",   "eve",    "frank",
      "grace",   "henry",   "iris",   "jack",    "kate",   "leo",
      "maria",   "nick",    "olive",  "peter",   "quinn",  "rose",
      "sam",     "tina",    "ursula", "victor",  "wendy",  "xavier",
  };
  static const char* leaves_[] = {
      "profile", "settings","avatar",  "email",   "token",  "session",
      "history", "prefs",   "keys",    "certs",   "roles",  "perms",
      "status",  "count",   "created", "updated", "active", "deleted",
  };
  constexpr int nseg = sizeof(segments) / sizeof(segments[0]);
  constexpr int nname = sizeof(names) / sizeof(names[0]);
  constexpr int nleaf = sizeof(leaves_) / sizeof(leaves_[0]);
  std::mt19937 rng(42);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k;
    k += segments[rng() % nseg]; k += '/';
    k += names[rng() % nname];   k += '/';
    k += leaves_[rng() % nleaf];
    char buf[16];
    snprintf(buf, sizeof(buf), "/%d", i);
    k += buf;
    keys.push_back(std::move(k));
  }
  return keys;
}

static std::vector<std::string> gen_uuid_keys(int n) {
  static const char hex[] = "0123456789abcdef";
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 15);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    char buf[37];
    int pos = 0;
    for (int j = 0; j < 36; j++) {
      if (j == 8 || j == 13 || j == 18 || j == 23)
        buf[pos++] = '-';
      else
        buf[pos++] = hex[dist(rng)];
    }
    buf[36] = '\0';
    keys.emplace_back(buf, 36);
  }
  return keys;
}

static std::vector<std::string> gen_sequential_prefixed(int n) {
  std::vector<std::string> keys;
  keys.reserve(n);
  static const char* prefixes[] = {
      "log:2024-01-15:event:", "log:2024-01-16:event:",
      "log:2024-01-17:event:", "log:2024-01-18:event:",
      "metric:cpu:host001:",   "metric:mem:host001:",
      "metric:cpu:host002:",   "metric:mem:host002:",
  };
  constexpr int npfx = sizeof(prefixes) / sizeof(prefixes[0]);
  for (int i = 0; i < n; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%06d", prefixes[i % npfx], i / npfx);
    keys.emplace_back(buf);
  }
  return keys;
}

static std::vector<std::string> gen_base64_keys(int n) {
  static const char b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 63);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k(22, '\0');
    for (auto& c : k) c = b64[dist(rng)];
    keys.push_back(std::move(k));
  }
  return keys;
}

// ── Histogram → stdout dump (avoids needing host filesystem) ────
static void dump_hist_stdout(const char* tag) {
  for (size_t s = 0; s < _PAGE_HIST_MAX; s++) {
    uint64_t c = _page_hist_bins[s].load(std::memory_order_relaxed);
    if (c) {
      std::printf("HIST,%s,%zu,%llu\n", tag, s, (unsigned long long)c);
    }
  }
  std::fflush(stdout);
}

// ── Run one scenario (no commit, no lookup — only allocations) ──
static void run_scenario(const char* name,
                         const std::vector<std::string>& keys,
                         int vsize) {
  _page_hist_reset();

  // IDB db names must be < 21 chars (DBEntry::name limit). Use scenario
  // index + vsize encoded compactly.
  static int sc_idx = 0;
  char db_name[24];
  std::snprintf(db_name, sizeof(db_name), "b%02d_v%d",
                ++sc_idx, vsize);
  auto storage = BrowserStorage::create(db_name,
                                        /*capacity=*/256 * M);
  auto db = storage->open("bench");
  auto cursor = db.cursor();

  std::string val((size_t)vsize, 'x');
  int batch = 1000;
  for (int i = 0; i < (int)keys.size(); i++) {
    cursor.find(Slice(keys[i]));
    cursor.value(Slice(val));
    if ((i + 1) % batch == 0 || i + 1 == (int)keys.size()) {
      cursor.commit();
    }
  }

  std::string tag = std::string(name) + "|v" + std::to_string(vsize);
  dump_hist_stdout(tag.c_str());
  std::printf("# scenario %-22s vsize=%-5d keys=%d\n", name, vsize,
              (int)keys.size());
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (sscanf(argv[i], "--num=%d", &FLAGS_num) == 1) continue;
    if (sscanf(argv[i], "--vsize=%d", &FLAGS_vsize) == 1) continue;
    if (sscanf(argv[i], "--scenario=%d", &FLAGS_scenario) == 1) continue;
    if (sscanf(argv[i], "--only_vsize=%d", &FLAGS_only_vsize) == 1) continue;
    if (strncmp(argv[i], "--vsize_sweep=", 14) == 0) {
      FLAGS_vsize_sweep = argv[i] + 14;
      continue;
    }
    std::fprintf(stderr,
                 "Usage: %s [--num=N] [--vsize=V] "
                 "[--vsize_sweep=8,100,1000]\n",
                 argv[0]);
    return 1;
  }

  std::vector<int> vsizes;
  if (FLAGS_vsize_sweep && FLAGS_vsize_sweep[0]) {
    const char* p = FLAGS_vsize_sweep;
    while (*p) {
      char* end = nullptr;
      long v = strtol(p, &end, 10);
      if (end == p) break;
      if (v > 0) vsizes.push_back((int)v);
      p = end;
      while (*p == ',' || *p == ' ') p++;
    }
  }
  if (vsizes.empty()) vsizes.push_back(FLAGS_vsize);

  std::printf("# bench_page_sizes_browser  num=%d  vsizes=", FLAGS_num);
  for (int v : vsizes) std::printf("%d,", v);
  std::printf("\n");
  std::printf("# Browser PAGE_SIZES:");
  for (int i = 0; i < _BrowserStoreTraits::PAGE_SIZES_COUNT; i++)
    std::printf(" [%d]=%u", i, _BrowserStoreTraits::PAGE_SIZES[i]);
  std::printf("\n");
  std::printf("# Format: HIST,<tag>,<request_size_bytes>,<count>\n");

  struct Scenario {
    const char* name;
    std::function<std::vector<std::string>(int)> gen;
  };
  Scenario scenarios[] = {
      {"random_binary_8B", gen_random_binary},
      {"binary_hash_32B",  gen_binary_hash},
      {"hex_strings_40ch", gen_hex_strings},
      {"decimal_integers", gen_decimal_integers},
      {"path_keys",        gen_path_keys},
      {"uuid_strings",     gen_uuid_keys},
      {"seq_prefixed",     gen_sequential_prefixed},
      {"base64_keys_22ch", gen_base64_keys},
  };

  for (size_t si = 0; si < sizeof(scenarios)/sizeof(scenarios[0]); si++) {
    if (FLAGS_scenario >= 0 && (int)si != FLAGS_scenario) continue;
    auto& sc = scenarios[si];
    std::fprintf(stderr, ">>> [%zu] %s\n", si, sc.name);
    auto keys = sc.gen(FLAGS_num);
    for (int v : vsizes) {
      if (FLAGS_only_vsize > 0 && v != FLAGS_only_vsize) continue;
      std::fprintf(stderr, "    vsize=%d ...\n", v);
      try {
        run_scenario(sc.name, keys, v);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "    !! %s: type=%s what=%s\n",
                     sc.name, typeid(e).name(), e.what());
      } catch (...) {
        std::fprintf(stderr, "    !! %s: unknown exception\n", sc.name);
      }
    }
  }

  std::printf("# done\n");
  return 0;
}

#else  // !__EMSCRIPTEN__
int main() { return 0; }
#endif
