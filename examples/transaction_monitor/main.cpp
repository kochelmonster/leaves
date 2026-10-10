#include "transaction_monitor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Database = TransactionMonitor::Database;

const std::filesystem::path kDatabasePath =
    std::filesystem::temp_directory_path() /
    "leaves_transaction_monitor.lvs";

void write_transaction(Database::Cursor& accounts_cursor,
                       Database::Cursor& audit_cursor) {
  accounts_cursor.find(leaves::Slice("customer:42"));
  accounts_cursor.value(leaves::Slice("balance=100"));
  audit_cursor.find(leaves::Slice("transfer:1001"));
  audit_cursor.value(leaves::Slice("completed"));
}

void verify_record(Database& database, const std::string& key,
                   const std::string& expected) {
  auto cursor = database.cursor();
  cursor.find(leaves::Slice(key));
  if (!cursor.is_valid() || cursor.value().string() != expected) {
    throw std::runtime_error("committed record did not verify: " + key);
  }
}

void verify_committed(Database& accounts, Database& audit) {
  verify_record(accounts, "customer:42", "balance=100");
  verify_record(audit, "transfer:1001", "completed");
}

void verify_absent(Database& database, const std::string& key) {
  auto cursor = database.cursor();
  cursor.find(leaves::Slice(key));
  if (cursor.is_valid()) {
    throw std::runtime_error("rolled back record is visible: " + key);
  }
}

void verify_aborted(Database& accounts, Database& audit) {
  verify_absent(accounts, "customer:42");
  verify_absent(audit, "transfer:1001");
}

int run(bool crash_after_decision, bool crash_during_prepare, bool recover,
        bool rollback_requested) {
  if (!recover) {
    std::error_code error;
    std::filesystem::remove(kDatabasePath, error);
  }

  {
    auto storage = leaves::MapStorage::create(kDatabasePath);
    auto accounts = storage->open("accounts");
    auto audit = storage->open("audit");
    auto accounts_cursor = accounts.cursor();
    auto audit_cursor = audit.cursor();

    TransactionMonitor::PersistHook persist_hook;
    if (crash_after_decision) {
      persist_hook = [](TransactionMonitor::State state) {
        if (state == TransactionMonitor::State::committing) {
          std::cout << "Commit decision persisted; simulating crash.\n"
                    << std::flush;
          std::_Exit(42);
        }
      };
    } else if (crash_during_prepare) {
      persist_hook = [persist_count = 0](TransactionMonitor::State state) mutable {
        if (state == TransactionMonitor::State::preparing &&
            ++persist_count == 2) {
          std::cout << "Preparing state persisted; simulating crash.\n"
                    << std::flush;
          std::_Exit(43);
        }
      };
    }

    TransactionMonitor monitor(storage, "transfer-1001",
                               "_transaction_monitor",
                               std::move(persist_hook));
    monitor.add_participant("accounts", accounts, accounts_cursor);
    monitor.add_participant("audit", audit, audit_cursor);

    if (!recover) {
      if (!monitor.start()) {
        throw std::runtime_error("could not start all participant transactions");
      }
      write_transaction(accounts_cursor, audit_cursor);
    }

    auto progress = rollback_requested ? monitor.rollback() : monitor.commit();
    while (progress == TransactionMonitor::Progress::pending) {
      progress = rollback_requested ? monitor.rollback() : monitor.commit();
    }

    if (monitor.state() == TransactionMonitor::State::aborted) {
      verify_aborted(accounts, audit);
      if (rollback_requested) {
        std::cout << "Transaction rolled back; neither record is visible.\n";
      } else {
        std::cout << "Recovered and aborted transaction.\n";
      }
    } else {
      if (monitor.state() != TransactionMonitor::State::committed) {
        throw std::runtime_error("transaction did not commit");
      }
      verify_committed(accounts, audit);
      std::cout << (recover ? "Recovered and committed transaction.\n"
                            : "Transaction committed across accounts and audit.\n")
                << "Both records verified.\n";
    }
  }

  std::error_code error;
  std::filesystem::remove(kDatabasePath, error);
  return 0;
}

}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  const bool crash_after_decision = mode == "--crash-after-decision";
  const bool crash_during_prepare = mode == "--crash-during-prepare";
  const bool recover = mode == "--recover";
  const bool rollback_requested = mode == "--rollback";
  if (argc > 2 ||
      (!mode.empty() && !crash_after_decision && !crash_during_prepare &&
       !recover && !rollback_requested)) {
    std::cerr << "Usage: " << argv[0]
              << " [--crash-after-decision|--crash-during-prepare|--recover|--rollback]\n";
    return 2;
  }

  try {
    return run(crash_after_decision, crash_during_prepare, recover,
               rollback_requested);
  } catch (const std::exception& error) {
    std::cerr << "Transaction monitor example failed: " << error.what()
              << '\n';
    return 1;
  }
}
