#include "test_multiprocess_shared.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int parse_int(const char* value) {
  return std::atoi(value);
}

void print_usage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " <action> [args...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 2;
  }

  std::string action = argv[1];

  try {
    std::fprintf(stderr, "[diag][helper] action=%s\n", action.c_str());
    std::fflush(stderr);
    std::fprintf(stderr, "[diag][helper] argc=%d\n", argc);
    std::fflush(stderr);
    if (action == "write-range" && argc == 5) {
      return mp_test::child_write_range(parse_int(argv[2]), parse_int(argv[3]),
                                        parse_int(argv[4]) != 0)
                 ? 0
                 : 1;
    }
    if (action == "writer-batches" && argc == 4) {
      return mp_test::writer_batches(parse_int(argv[2]), parse_int(argv[3])) ? 0 : 1;
    }
    if (action == "merger-loops" && argc == 3) {
      return mp_test::merger_loops(parse_int(argv[2])) ? 0 : 1;
    }
    if (action == "contention-worker" && argc == 5) {
      return mp_test::contention_worker(parse_int(argv[2]), parse_int(argv[3]),
                                        parse_int(argv[4]))
                 ? 0
                 : 1;
    }
    if (action == "crash-during-merge" && argc == 3) {
      mp_test::crash_during_merge(parse_int(argv[2]));
    }
    if (action == "crash-during-transaction" && argc == 4) {
      mp_test::crash_during_transaction(parse_int(argv[2]), parse_int(argv[3]));
    }
    if (action == "recover-lost-areas-crash" && argc == 4) {
      mp_test::recover_lost_areas_crash(parse_int(argv[2]), parse_int(argv[3]));
    }
    if (action == "two-phase-prepare-crash" && argc == 3) {
      mp_test::two_phase_prepare_crash(parse_int(argv[2]));
    }
    if (action == "wal-prepare-crash" && argc == 3) {
      mp_test::wal_prepare_crash(parse_int(argv[2]));
    }
  } catch (...) {
    return 1;
  }

  print_usage(argv[0]);
  return 2;
}