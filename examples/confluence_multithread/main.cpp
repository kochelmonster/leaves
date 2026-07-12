#include <leaves/confluence.hpp>
#include <leaves/metrics.hpp>
#include <leaves/mmap.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace leaves;

namespace {

using DemoStorage = MapStorage_<MetricsMapTraits>;
using DemoConfluenceDB = DemoStorage::ConfluenceDB;

struct DemoConfig {
  const char* db_path = "./confluence_multithread.lvs";
  const char* db_name = "confluence_multithread";
  bool clean_start = true;

  uint32_t writers = 6;
  uint32_t ops_per_writer = 50000;

  // Add light pacing so writers stay active long enough to observe in-flight
  // merge progress on fast machines.
  uint32_t writer_pause_every_ops = 32;
  uint32_t writer_pause_us = 100;

  // Start a synchronous merge probe after enough writes; we then require that
  // at least one merge drain generation completed before all writers finished.
  uint64_t merge_probe_min_commits = 4096;
  uint64_t merge_probe_timeout_ms = 3000;

  // Keep thresholds low to make internal merge activity visible quickly.
  uint32_t merge_write_threshold = 8;
  uint64_t max_attached_age_ms = 5;
};

constexpr DemoConfig kConfig{};

std::string make_key(uint32_t writer_id, uint32_t op_id) {
  std::ostringstream os;
  os << "writer/" << writer_id << "/op/" << op_id;
  return os.str();
}

std::string make_value(uint32_t writer_id, uint32_t op_id) {
  std::ostringstream os;
  os << "value(" << writer_id << "," << op_id << ")";
  return os.str();
}

void check_merge_error(DemoConfluenceDB* db) {
  if (std::exception_ptr ep = db->get_merge_error()) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("merge error: ") + e.what());
    } catch (...) {
      throw std::runtime_error("merge error: unknown exception");
    }
  }
}

void print_metrics_snapshot(DemoConfluenceDB* db, const char* tag) {
  const auto& aspect = db->_internal_main().aspect();
  auto txn = aspect.txn_snapshot();
  auto merge = aspect.merge_snapshot();

  std::cout << "[metrics] " << tag
            << " user_commits=" << txn.user_txns_committed
            << " merge_commits=" << txn.merge_txns_committed
            << " merge_adds=" << merge.merge_adds
            << " merge_overwrites=" << merge.merge_overwrites
            << " merge_deletes=" << merge.merge_deletes << "\n";
}

bool validate_sample_keys(DemoConfluenceDB* db,
                          uint32_t writers,
                          uint32_t ops_per_writer) {
  auto cursor = db->cursor();

  for (uint32_t w = 0; w < writers; ++w) {
    std::vector<uint32_t> sample_ids;
    sample_ids.push_back(0);
    sample_ids.push_back(ops_per_writer / 2);
    sample_ids.push_back(ops_per_writer - 1);

    for (uint32_t op_id : sample_ids) {
      const std::string key = make_key(w, op_id);
      const std::string expected = make_value(w, op_id);
      cursor.find(Slice(key));
      if (!cursor.is_valid()) {
        std::cerr << "Missing expected key: " << key << "\n";
        return false;
      }
      const std::string actual = cursor.value().string();
      if (actual != expected) {
        std::cerr << "Value mismatch for key=" << key << " expected='"
                  << expected << "' got='" << actual << "'\n";
        return false;
      }
    }
  }
  return true;
}

bool validate_total_keys(DemoConfluenceDB* db, uint64_t expected_total) {
  auto cursor = db->cursor();
  uint64_t seen = 0;
  for (cursor.first(); cursor.is_valid(); cursor.next()) {
    ++seen;
  }
  if (seen != expected_total) {
    std::cerr << "Key count mismatch: expected=" << expected_total
              << " actual=" << seen << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (kConfig.clean_start) {
    std::error_code ec;
    std::filesystem::remove(kConfig.db_path, ec);
  }

  try {
    auto storage = DemoStorage::create(kConfig.db_path);
    auto cdb = storage->open<DemoConfluenceDB>(kConfig.db_name);

    cdb.set_merge_write_threshold(kConfig.merge_write_threshold);
    cdb.set_max_attached_age_ms(kConfig.max_attached_age_ms);

    std::atomic<bool> start{false};
    std::atomic<uint64_t> committed{0};
    std::atomic<uint32_t> writers_done{0};
    std::atomic<bool> printed_mid_snapshot{false};
    std::atomic<bool> merge_observed_while_active{false};
    std::atomic<bool> stop_merge_observer{false};
    std::mutex out_mu;

    std::cout << "Confluence multithread demo\n"
              << "db_path=" << kConfig.db_path << " db_name=" << kConfig.db_name
              << " writers=" << kConfig.writers
              << " ops_per_writer=" << kConfig.ops_per_writer << "\n";

    print_metrics_snapshot(&cdb, "startup");

    std::vector<std::thread> threads;
    threads.reserve(kConfig.writers);

    const auto t0 = std::chrono::steady_clock::now();
    auto* impl = cdb._internal();
    const uint64_t merge_done_at_start =
        impl->_merge_done.load(std::memory_order_acquire);

    std::thread merge_observer([&]() {
      uint64_t last_done = merge_done_at_start;
      while (!stop_merge_observer.load(std::memory_order_acquire)) {
        const uint64_t merge_done_now =
            impl->_merge_done.load(std::memory_order_acquire);
        if (merge_done_now > last_done) {
          last_done = merge_done_now;
          if (writers_done.load(std::memory_order_relaxed) < kConfig.writers &&
              !merge_observed_while_active.exchange(true,
                                                    std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(out_mu);
            std::cout << "merge observed while writers were active "
                      << "(merge_done=" << merge_done_now << ")\n";
            print_metrics_snapshot(&cdb, "merge-observed-while-active");
          }
        }
        if (writers_done.load(std::memory_order_relaxed) >= kConfig.writers) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });

    for (uint32_t writer_id = 0; writer_id < kConfig.writers; ++writer_id) {
      threads.emplace_back([&, writer_id]() {
        std::cout << "writer " << writer_id << " started" << std::endl;
        auto cursor = cdb.cursor();

        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        for (uint32_t op_id = 0; op_id < kConfig.ops_per_writer; ++op_id) {
          const std::string key = make_key(writer_id, op_id);
          const std::string value = make_value(writer_id, op_id);

          cursor.start_transaction();
          cursor.find(Slice(key));
          cursor.value(Slice(value));
          cursor.commit();

          committed.fetch_add(1, std::memory_order_relaxed);

          if (kConfig.writer_pause_every_ops > 0 &&
              ((op_id + 1) % kConfig.writer_pause_every_ops) == 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(kConfig.writer_pause_us));
          }

          if (writer_id == 0 &&
              op_id == (kConfig.ops_per_writer / 2) &&
              !printed_mid_snapshot.exchange(true, std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(out_mu);
            print_metrics_snapshot(&cdb, "mid-run");
          }
        }

        const uint32_t done =
            writers_done.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lk(out_mu);
        std::cout << "writer " << writer_id << " finished (" << done << "/"
                  << kConfig.writers << ")\n";
      });
    }

    start.store(true, std::memory_order_release);

    bool issued_merge_probe = false;
    const auto merge_probe_deadline =
        t0 + std::chrono::milliseconds(kConfig.merge_probe_timeout_ms);
    while (writers_done.load(std::memory_order_relaxed) < kConfig.writers &&
           std::chrono::steady_clock::now() < merge_probe_deadline &&
           !merge_observed_while_active.load(std::memory_order_relaxed)) {
      if (!issued_merge_probe &&
          committed.load(std::memory_order_relaxed) >=
              kConfig.merge_probe_min_commits) {
        cdb.merge_now();
        check_merge_error(&cdb);
        issued_merge_probe = true;
        std::lock_guard<std::mutex> lk(out_mu);
        print_metrics_snapshot(&cdb, "after-merge-probe");
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    for (auto& t : threads) {
      t.join();
    }

    stop_merge_observer.store(true, std::memory_order_release);
    if (merge_observer.joinable()) {
      merge_observer.join();
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    check_merge_error(&cdb);
    print_metrics_snapshot(&cdb, "after-writers");

    // Keep runtime autonomous (no merge worker), then force one final drain so
    // merge activity is visible in aspect counters.
    cdb.merge_all_now();
    check_merge_error(&cdb);
    print_metrics_snapshot(&cdb, "after-final-drain");

    if (!merge_observed_while_active.load(std::memory_order_relaxed)) {
      std::cerr
          << "Did not observe merge completion while writers were active. "
          << "Increase ops_per_writer or pacing and rerun.\n";
      return 1;
    }

    const uint64_t total_commits = committed.load(std::memory_order_relaxed);
    const double elapsed_s = static_cast<double>(elapsed_ms) / 1000.0;
    const double throughput = elapsed_s > 0.0
                                  ? static_cast<double>(total_commits) / elapsed_s
                                  : 0.0;

    std::cout << "commits=" << total_commits << " elapsed_ms=" << elapsed_ms
              << " ops_per_sec=" << static_cast<uint64_t>(throughput) << "\n";

    const uint64_t expected_total =
        static_cast<uint64_t>(kConfig.writers) * kConfig.ops_per_writer;

    if (!validate_total_keys(&cdb, expected_total)) {
      return 1;
    }
    if (!validate_sample_keys(&cdb, kConfig.writers, kConfig.ops_per_writer)) {
      return 1;
    }

    print_metrics_snapshot(&cdb, "validated");
    std::cout << "merge observed while writers active: yes\n";
    std::cout << "\nResult: SUCCESS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Result: FAILURE: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "Result: FAILURE: unknown exception\n";
    return 1;
  }
}
