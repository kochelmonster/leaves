#ifndef TESTPOINTS_HPP
#define TESTPOINTS_HPP

#include <iostream>
#include <boost/test/included/unit_test.hpp>
#include <filesystem>
#include "../src/memory.hpp"

#ifndef TESTING
#error "TESTING must be defined"
#endif

#define GENERATE
#ifdef GENERATE

inline void check_testpoints(const char* testpoints[]) {
  std::cerr << "check case:" << std::endl;
  for (const auto& item : leaves::TestPoints::tp_output) {
    std::cerr << item.first << "(" << item.second << ")" << std::endl;
  }
}
#else

inline void check_testpoints(const char* testpoints[]) {
  for (size_t i = 0; testpoints[i]; i++) {
    auto found = TestPoints::tp_output.find(testpoints[i]);
    if (found == TestPoints::tp_output.end()) {
        std::cerr << "missing testpoint: " << testpoints[i] << std::endl;
        std::cerr << "found testpoints" << std::endl;
        for (const auto& item : TestPoints::tp_output) {
         std::cerr << item.first << "(" << item.second << ")" << std::endl;
        }
        BOOST_REQUIRE(false);     
    }
  }
}
#endif


struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_db";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    std::filesystem::path dbFilePath = tempDir / "test.lvs";
  }

  ~DirPreparation() {
    std::filesystem::remove_all(tempDir);
  }

  std::filesystem::path tempDir;
};

#endif  // TESTPOINTS_HPP