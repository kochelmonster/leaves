// Benchmark: Page size analysis for different key scenarios
//
// Inserts N keys of various formats, collects MemStatistics, and reports
// per-slot usage including wasted space. Use the output to tune PAGE_SIZES.
//
// Build:  cmake --build build -j --target bench_page_sizes
// Run:    ./build/bench_page_sizes [--num=100000] [--vsize=100]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "leaves/mmap.hpp"

using namespace leaves;

static int FLAGS_num = 100000;
static int FLAGS_vsize = 100;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct TempDir {
  std::filesystem::path dir;
  TempDir(const char* name) {
    dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
  std::filesystem::path path(const char* file) const { return dir / file; }
};

// ---------------------------------------------------------------------------
// Key generators
// ---------------------------------------------------------------------------

// 1. Random binary: 8-byte random values (e.g. database row IDs)
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

// 2. Binary hash (32 bytes): SHA-256 / BLAKE3 style content-addressed keys
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

// 3. Hex strings: "a3f2b1..." (e.g. hex-encoded hashes, git commit IDs)
static std::vector<std::string> gen_hex_strings(int n) {
  static const char hex[] = "0123456789abcdef";
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 15);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k(40, '\0');  // 40 hex chars = 160 bits (SHA-1 length)
    for (auto& c : k) c = hex[dist(rng)];
    keys.push_back(std::move(k));
  }
  return keys;
}

// 4. Decimal integers: "0000000001" (e.g. auto-increment IDs, timestamps)
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

// 5. Path keys: "users/alice/settings/123" style hierarchical keys
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
  static const char* leaves[] = {
      "profile", "settings","avatar",  "email",   "token",  "session",
      "history", "prefs",   "keys",    "certs",   "roles",  "perms",
      "status",  "count",   "created", "updated", "active", "deleted",
  };
  constexpr int nseg = sizeof(segments) / sizeof(segments[0]);
  constexpr int nname = sizeof(names) / sizeof(names[0]);
  constexpr int nleaf = sizeof(leaves) / sizeof(leaves[0]);

  std::mt19937 rng(42);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k;
    k += segments[rng() % nseg];
    k += '/';
    k += names[rng() % nname];
    k += '/';
    k += leaves[rng() % nleaf];
    char buf[16];
    snprintf(buf, sizeof(buf), "/%d", i);
    k += buf;
    keys.push_back(std::move(k));
  }
  return keys;
}

// 6. UUIDs: "550e8400-e29b-41d4-a716-446655440000"
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

// 7. Sequential prefixed: "log:2024-01-15:event:000001"
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

// 8. Base64 keys: URL-safe base64 encoded data
static std::vector<std::string> gen_base64_keys(int n) {
  static const char b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 63);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; i++) {
    std::string k(22, '\0');  // 22 base64 chars ~ 128 bits
    for (auto& c : k) c = b64[dist(rng)];
    keys.push_back(std::move(k));
  }
  return keys;
}

// ---------------------------------------------------------------------------
// Benchmark runner
// ---------------------------------------------------------------------------

struct ScenarioResult {
  std::string name;
  double insert_time_ms;
  double lookup_time_ms;
  size_t total_branches;
  size_t total_leaves;
  size_t total_branch_bytes;
  size_t total_leaf_bytes;
  size_t total_branch_wasted;
  size_t total_leaf_wasted;

  struct SlotInfo {
    uint16_t page_size;
    size_t branch_count;
    size_t branch_free;
    size_t leaf_count;
    size_t leaf_free;
  };
  std::vector<SlotInfo> slots;
};

static ScenarioResult run_scenario(const char* name,
                                   const std::vector<std::string>& keys,
                                   int vsize) {
  TempDir tmp("bench_page_sizes");
  auto fpath = tmp.path((std::string(name) + ".lvs").c_str());

  auto storage = MapStorage::create(fpath.c_str());
  auto db = (*storage)["bench"];
  auto cursor = db.cursor();

  std::string val(vsize, 'x');

  double t0 = now_seconds();

  int batch = 1000;
  for (int i = 0; i < (int)keys.size(); i++) {
    cursor.find(Slice(keys[i]));
    cursor.value(Slice(val));
    if ((i + 1) % batch == 0 || i + 1 == (int)keys.size()) {
      cursor.commit();
    }
  }

  double t1 = now_seconds();

  // Lookup benchmark: traverse every key to measure read speed
  cursor = db.cursor();
  double t2 = now_seconds();
  for (int i = 0; i < (int)keys.size(); i++) {
    cursor.find(Slice(keys[i]));
    assert(cursor.is_valid());
  }
  double t3 = now_seconds();

  // Collect statistics
  auto* db_internal = db._internal();
  using DB_type = std::remove_pointer_t<decltype(db_internal)>;
  typename DB_type::Statistics stat;
  db_internal->statistics(stat);

  ScenarioResult result;
  result.name = name;
  result.insert_time_ms = (t1 - t0) * 1000.0;
  result.lookup_time_ms = (t3 - t2) * 1000.0;
  result.total_branches = 0;
  result.total_leaves = 0;
  result.total_branch_bytes = 0;
  result.total_leaf_bytes = 0;
  result.total_branch_wasted = 0;
  result.total_leaf_wasted = 0;

  for (int i = 0; i < DB_type::MemStatistics::COUNT; i++) {
    auto& bs = stat.branch.slots[i];
    auto& ls = stat.leaf.slots[i];
    uint16_t ps = DB_type::Traits::PAGE_SIZES[i];

    ScenarioResult::SlotInfo si;
    si.page_size = ps;
    si.branch_count = bs.count;
    si.branch_free = bs.free;
    si.leaf_count = ls.count;
    si.leaf_free = ls.free;
    result.slots.push_back(si);

    result.total_branches += bs.count;
    result.total_leaves += ls.count;
    result.total_branch_bytes += bs.count * ps;
    result.total_leaf_bytes += ls.count * ps;
    result.total_branch_wasted += bs.free;
    result.total_leaf_wasted += ls.free;
  }

  return result;
}

static void print_result(const ScenarioResult& r) {
  printf("\n");
  printf(
      "==============================================================="
      "====\n");
  printf("  Scenario: %-40s\n", r.name.c_str());
  printf(
      "==============================================================="
      "====\n");
  printf("  Insert time:  %.1f ms  (%.1f us/op)\n", r.insert_time_ms,
         r.insert_time_ms * 1000.0 / FLAGS_num);
  printf("  Lookup time:  %.1f ms  (%.1f us/op)\n", r.lookup_time_ms,
         r.lookup_time_ms * 1000.0 / FLAGS_num);
  printf("  Branches:     %zu nodes = %zu KB\n", r.total_branches,
         r.total_branch_bytes / 1024);
  printf("  Leaves:       %zu nodes = %zu KB\n", r.total_leaves,
         r.total_leaf_bytes / 1024);
  printf("  Branch waste: %zu KB (%.1f%%)\n", r.total_branch_wasted / 1024,
         r.total_branch_bytes > 0
             ? 100.0 * r.total_branch_wasted / r.total_branch_bytes
             : 0.0);
  printf("  Leaf waste:   %zu KB (%.1f%%)\n", r.total_leaf_wasted / 1024,
         r.total_leaf_bytes > 0
             ? 100.0 * r.total_leaf_wasted / r.total_leaf_bytes
             : 0.0);
  printf("\n");

  printf("  %-6s  %-10s  %-10s  %-16s  %-10s  %-16s\n", "Slot", "PageSize",
         "Branches", "BrWaste", "Leaves", "LfWaste");
  printf("  %-6s  %-10s  %-10s  %-16s  %-10s  %-16s\n", "------",
         "----------", "----------", "----------------", "----------",
         "----------------");
  for (size_t i = 0; i < r.slots.size(); i++) {
    auto& s = r.slots[i];
    if (s.branch_count == 0 && s.leaf_count == 0) continue;
    char bw[32] = "", lw[32] = "";
    if (s.branch_count > 0)
      snprintf(bw, sizeof(bw), "%zu (%.0f%%)", s.branch_free,
               100.0 * s.branch_free / (s.branch_count * s.page_size));
    if (s.leaf_count > 0)
      snprintf(lw, sizeof(lw), "%zu (%.0f%%)", s.leaf_free,
               100.0 * s.leaf_free / (s.leaf_count * s.page_size));
    printf("  [%zu]     %-10u  %-10zu  %-16s  %-10zu  %-16s\n", i,
           s.page_size, s.branch_count, bw, s.leaf_count, lw);
  }
}

static void print_summary(const std::vector<ScenarioResult>& results) {
  printf("\n\n");
  printf(
      "==============================================================="
      "====\n");
  printf("  CROSS-SCENARIO SUMMARY\n");
  printf(
      "==============================================================="
      "====\n\n");

  // Print current page sizes
  auto* db_internal_dummy =
      static_cast<MapStorage::StorageImpl::DB*>(nullptr);
  using DB_type = MapStorage::StorageImpl::DB;

  printf("  Current PAGE_SIZES:\n");
  for (int i = 0; i < DB_type::MemStatistics::COUNT; i++) {
    printf("    [%d] = %u bytes", i, DB_type::Traits::PAGE_SIZES[i]);
    const char* label;
    switch (i) {
      case 0:  label = "2 branches"; break;
      case 1:  label = "3 branches"; break;
      case 2:  label = "4 branches"; break;
      case 3:  label = "5-10 branches"; break;
      case 4:  label = "11-16 branches"; break;
      case 5:  label = "17-64 branches"; break;
      case 6:  label = "65-256 branches"; break;
      case 7:  label = "4K max"; break;
      default: label = ""; break;
    }
    printf("  ; %s\n", label);
  }
  (void)db_internal_dummy;

  // Per-slot usage across scenarios
  printf("\n  Per-slot usage across all scenarios:\n\n");
  printf("  %-6s  %-8s", "Slot", "Size");
  for (auto& r : results) {
    std::string short_name = r.name.substr(0, 14);
    printf("  %-16s", short_name.c_str());
  }
  printf("\n  %-6s  %-8s", "------", "--------");
  for (size_t i = 0; i < results.size(); i++)
    printf("  %-16s", "----------------");
  printf("\n");

  for (int slot = 0; slot < DB_type::MemStatistics::COUNT; slot++) {
    bool any = false;
    for (auto& r : results) {
      if (r.slots[slot].branch_count > 0 || r.slots[slot].leaf_count > 0) {
        any = true;
        break;
      }
    }
    if (!any) continue;

    printf("  [%d]     %-8u", slot, DB_type::Traits::PAGE_SIZES[slot]);
    for (auto& r : results) {
      auto& s = r.slots[slot];
      size_t total = s.branch_count + s.leaf_count;
      size_t total_bytes = total * s.page_size;
      size_t waste = s.branch_free + s.leaf_free;
      if (total > 0)
        printf("  %6zu (%4.1f%%w) ", total,
               100.0 * waste / total_bytes);
      else
        printf("  %16s", "-");
    }
    printf("\n");
  }

  // Per-scenario waste summary
  printf("\n  Overall waste per scenario:\n");
  for (auto& r : results) {
    size_t total_bytes = r.total_branch_bytes + r.total_leaf_bytes;
    size_t total_waste = r.total_branch_wasted + r.total_leaf_wasted;
    printf("    %-25s  %6zu KB used, %5zu KB wasted (%4.1f%%)\n",
           r.name.c_str(), total_bytes / 1024, total_waste / 1024,
           total_bytes > 0 ? 100.0 * total_waste / total_bytes : 0.0);
  }

  printf("\n  Recommendation:\n");
  printf(
      "  High waste%% in a heavily-used slot means the page is too large\n"
      "  for actual node sizes. Consider adding intermediate page sizes\n"
      "  or adjusting existing ones to better fit observed node sizes.\n"
      "  Slots with 0%% waste are perfectly utilized.\n");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (sscanf(argv[i], "--num=%d", &FLAGS_num) == 1) continue;
    if (sscanf(argv[i], "--vsize=%d", &FLAGS_vsize) == 1) continue;
    fprintf(stderr, "Usage: %s [--num=N] [--vsize=N]\n", argv[0]);
    return 1;
  }

  using DB_type = MapStorage::StorageImpl::DB;

  printf("Page Size Benchmark\n");
  printf("  Keys:       %d\n", FLAGS_num);
  printf("  Value size: %d bytes\n", FLAGS_vsize);
  printf("  PAGE_SIZES:");
  for (int i = 0; i < DB_type::MemStatistics::COUNT; i++)
    printf(" [%d]=%u", i, DB_type::Traits::PAGE_SIZES[i]);
  printf("\n");

  struct Scenario {
    const char* name;
    std::function<std::vector<std::string>(int)> gen;
  };

  Scenario scenarios[] = {
      {"random_binary_8B", gen_random_binary},
      {"binary_hash_32B", gen_binary_hash},
      {"hex_strings_40ch", gen_hex_strings},
      {"decimal_integers", gen_decimal_integers},
      {"path_keys", gen_path_keys},
      {"uuid_strings", gen_uuid_keys},
      {"seq_prefixed", gen_sequential_prefixed},
      {"base64_keys_22ch", gen_base64_keys},
  };

  std::vector<ScenarioResult> results;

  for (auto& sc : scenarios) {
    printf("\n>>> Generating keys: %s ...\n", sc.name);
    auto keys = sc.gen(FLAGS_num);

    // Print a sample key
    auto& sample = keys[0];
    bool printable = true;
    for (char c : sample)
      if (c < 32 || c > 126) {
        printable = false;
        break;
      }
    if (printable) {
      printf("    Sample: \"%s\" (%zu bytes)\n", sample.c_str(), sample.size());
    } else {
      printf("    Sample: [");
      for (unsigned char c : sample) printf("%02x", c);
      printf("] (%zu bytes)\n", sample.size());
    }

    auto result = run_scenario(sc.name, keys, FLAGS_vsize);
    print_result(result);
    results.push_back(std::move(result));
  }

  print_summary(results);
  return 0;
}
