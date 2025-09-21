#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBFileStoreTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_fstore.hpp"

using namespace leaves;

typedef _FileStore DBFileStore;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_fstore";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

void wrong_signature(const char* path) { DBFileStore db(path); }

BOOST_AUTO_TEST_CASE(test_init) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  {
    // Create a DBFileStore instance and initialize it
    DBFileStore db(dbFilePath.c_str());

    // Check if the database file is created
    BOOST_REQUIRE(std::filesystem::exists(dbFilePath));

    // Check if the header is properly initialized
    BOOST_REQUIRE(db._header != nullptr);
    BOOST_REQUIRE_EQUAL(std::string(db._header->signature), std::string(FSTORE_SIGNATURE));
    BOOST_REQUIRE_EQUAL(db._header->db_version, 0);
    BOOST_REQUIRE_EQUAL(db._header->db_count, 48);  // default db_count
    BOOST_REQUIRE_GE(db._header->file_size, db._header_size);
  }

  {
    // Test reopening existing file
    DBFileStore db(dbFilePath.c_str());
    BOOST_REQUIRE(db._header != nullptr);
    BOOST_REQUIRE_EQUAL(std::string(db._header->signature), std::string(FSTORE_SIGNATURE));
  }

  // Change the signature to test wrong filetype error
  std::fstream file(dbFilePath, std::ios::in | std::ios::out | std::ios::binary);
  if (file.is_open()) {
    file.seekp(0);
    file.put('X');  // Change first character of signature
    file.close();
  }

  BOOST_CHECK_THROW(wrong_signature(dbFilePath.c_str()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_db_count_validation) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  // Create a file with db_count = 10
  {
    DBFileStore db(dbFilePath.c_str(), 10);
    BOOST_REQUIRE_EQUAL(db._header->db_count, 10);
  }

  // Try to reopen with different db_count - should throw
  BOOST_CHECK_THROW(DBFileStore db(dbFilePath.c_str(), 20), WrongValue);
  
  // But opening with same db_count should work
  DBFileStore db(dbFilePath.c_str(), 10);
  BOOST_CHECK_EQUAL(db._header->db_count, 10);
}

BOOST_AUTO_TEST_CASE(test_file_operations) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBFileStore db(dbFilePath.c_str());

  // Test writing and reading data
  const char* test_data = "Hello, FileStore!";
  size_t test_size = strlen(test_data) + 1;
  
  // Write data at offset beyond header
  uint64_t write_offset = db._header_size;
  db.write(write_offset, test_data, test_size);
  
  // Read data back
  char read_buffer[100];
  db.read(write_offset, read_buffer, test_size);
  
  BOOST_CHECK_EQUAL(std::string(read_buffer), std::string(test_data));
}

BOOST_AUTO_TEST_CASE(test_sanitize) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBFileStore db(dbFilePath.c_str());

  // Artificially extend the file beyond what the header says
  uint64_t original_size = db._header->file_size;
  std::filesystem::resize_file(dbFilePath, original_size + 1000);
  
  // Verify file is actually larger
  BOOST_CHECK_GT(std::filesystem::file_size(dbFilePath), original_size);
  
  // Sanitize should resize it back
  db.sanitize();
  BOOST_CHECK_EQUAL(std::filesystem::file_size(dbFilePath), db._header->file_size);
}

BOOST_AUTO_TEST_CASE(test_signature_constants) {
  // Test that FSTORE_SIGNATURE constant is correct
  BOOST_CHECK_EQUAL(std::string(FSTORE_SIGNATURE), "larch-leaves");
  BOOST_CHECK_GT(FSTORE_SIGNATURE_SIZE, 0);
  BOOST_CHECK_GE(FSTORE_SIGNATURE_SIZE, sizeof(FSTORE_SIGNATURE));
}

BOOST_AUTO_TEST_CASE(test_file_header_structure) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBFileStore db(dbFilePath.c_str(), 10);  // Custom db_count

  // Verify FileHeader structure
  BOOST_CHECK_EQUAL(std::string(db._header->signature), std::string(FSTORE_SIGNATURE));
  BOOST_CHECK_EQUAL(db._header->db_version, 0);
  BOOST_CHECK_EQUAL(db._header->db_count, 10);
  BOOST_CHECK_GT(db._header->file_size, 0);
  
  // Verify header size calculation
  size_t expected_header_size = padding(sizeof(DBFileStore::FileHeader) + 
                                      sizeof(DBFileStore::DBEntry) * 10, 4 * K);
  BOOST_CHECK_EQUAL(db._header_size, expected_header_size);
}

BOOST_AUTO_TEST_CASE(test_concurrent_access) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  // Test that multiple instances can access the same file
  DBFileStore db1(dbFilePath.c_str());
  DBFileStore db2(dbFilePath.c_str());
  
  // Both should have valid headers
  BOOST_CHECK(db1._header != nullptr);
  BOOST_CHECK(db2._header != nullptr);
  
  // They should see the same signature and basic properties
  BOOST_CHECK_EQUAL(std::string(db1._header->signature), std::string(db2._header->signature));
  BOOST_CHECK_EQUAL(db1._header->db_count, db2._header->db_count);
}

BOOST_AUTO_TEST_CASE(test_dirty_processor_thread) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBFileStore db(dbFilePath.c_str());

  // Test that the dirty processor thread is running
  BOOST_CHECK(db._dirty_processor_thread.joinable());
  
  // Test basic cache functionality
  BOOST_CHECK_GT(db._cache._capacity, 0);
  BOOST_CHECK_EQUAL(db._cache._size, 0);  // Initially empty
}

BOOST_AUTO_TEST_CASE(test_area_slice_functionality) {
  // Test AreaSlice methods independently
  AreaSlice area(1000, 2048, 0);  // offset=1000, size=2048, ref=0
  
  // Test basic getters
  BOOST_CHECK_EQUAL(area.get_offset(), 1000);
  BOOST_CHECK_EQUAL(area.get_size(), 2048);
  BOOST_CHECK_EQUAL(area.get_ref(), 0);
  
  // Test dirty bit operations
  BOOST_CHECK(!area.is_dirty());
  area.set_dirty();
  BOOST_CHECK(area.is_dirty());
  BOOST_CHECK(area.clear_dirty());  // Should return true (was dirty)
  BOOST_CHECK(!area.is_dirty());
  BOOST_CHECK(!area.clear_dirty()); // Should return false (wasn't dirty)
  
  // Test reference counting
  BOOST_CHECK_EQUAL(area.inc_ref(), 1);
  BOOST_CHECK_EQUAL(area.get_ref(), 1);
  BOOST_CHECK_EQUAL(area.dec_ref(), 0);
  BOOST_CHECK_EQUAL(area.get_ref(), 0);
  
  // Test offset modification
  area.set_offset(2000);
  BOOST_CHECK_EQUAL(area.get_offset(), 2000);
  
  // Test size modification
  area.set_size(4096);
  BOOST_CHECK_EQUAL(area.get_size(), 4096);
  
  // Test operator bool and end()
  BOOST_CHECK(area);  // Should be true since size > 0
  BOOST_CHECK_EQUAL(area.end(), 2000 + 4096);
  
  // Test zero-size area
  AreaSlice empty_area(0, 0, 0);
  BOOST_CHECK(!empty_area);  // Should be false since size == 0
}

BOOST_AUTO_TEST_CASE(test_invalid_paths) {
  DirPreparation prep;
  
  // Test invalid file path
  std::filesystem::path invalidPath = prep.tempDir / "non_existent_dir" / "test.lvs";
  BOOST_CHECK_THROW(DBFileStore invalid_db(invalidPath.c_str()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_filename_method) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBFileStore db(dbFilePath.c_str());

  // Test that filename() returns the correct path
  BOOST_CHECK_EQUAL(db.filename(), dbFilePath.string());
}

BOOST_AUTO_TEST_CASE(test_get_area_alignment_and_growth) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "areas.lvs";
  DBFileStore db(dbFilePath.c_str());

  uint64_t initial = db._header->file_size;
  auto a1 = db.get_area(1024);
  BOOST_CHECK(a1);
  BOOST_CHECK_EQUAL(a1.get_offset() % DBFileStore::AREA_SIZE, 0);
  BOOST_CHECK_GE(a1.get_size(), 1024);
  BOOST_CHECK_GE(db._header->file_size, initial + 1024);

  auto a2 = db.get_area(2 * 1024);
  BOOST_CHECK(a2);
  BOOST_CHECK_EQUAL(a2.get_offset(), a1.end());
  BOOST_CHECK_EQUAL(a2.get_offset() % DBFileStore::AREA_SIZE, 0);
}

BOOST_AUTO_TEST_CASE(test_resolve_reads_back_data_and_caches) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "resolve.lvs";
  DBFileStore db(dbFilePath.c_str());

  // Allocate an area and write a header + payload into the file directly
  auto area = db.get_area(4096);
  const uint64_t base = area.get_offset();

  // Compose an area buffer: [AreaSlice header][payload...]
  std::vector<char> buf(area.get_size());
  std::memcpy(buf.data(), &area, sizeof(AreaSlice));
  uint32_t pattern = 0xDEADBEEF;
  size_t payload_off = sizeof(AreaSlice) + 128;
  std::memcpy(buf.data() + payload_off, &pattern, sizeof(pattern));

  db.write(base, buf.data(), buf.size());

  // Resolve a pointer inside the payload and verify
  auto ptr = db.resolve(base + payload_off, READ);
  BOOST_REQUIRE(ptr);
  uint32_t got = 0;
  std::memcpy(&got, (const void*)ptr, sizeof(got));
  BOOST_CHECK_EQUAL(got, pattern);

  // Second resolve should hit cache path without throws
  auto ptr2 = db.resolve(base + payload_off, READ);
  BOOST_REQUIRE(ptr2);
  std::memcpy(&got, (const void*)ptr2, sizeof(got));
  BOOST_CHECK_EQUAL(got, pattern);
}

BOOST_AUTO_TEST_CASE(test_make_dirty_pushes_and_flushes_once) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "dirty.lvs";
  DBFileStore db(dbFilePath.c_str());

  // Prepare a cached area in memory and mark it dirty twice; background loop will clear once
  auto area = db.get_area(4096);
  const uint64_t base = area.get_offset();

  // Write a valid area buffer so resolve can read it
  std::vector<char> buf(area.get_size());
  std::memcpy(buf.data(), &area, sizeof(AreaSlice));
  db.write(base, buf.data(), buf.size());

  // Resolve a location to get a block_ptr
  auto blk = db.resolve(base + sizeof(AreaSlice), WRITE);
  BOOST_REQUIRE(blk);

  // Mark dirty twice; internal queue should accept both, but clear_dirty ensures only first triggers write
  db.make_dirty(blk);
  db.make_dirty(blk);

  // Give background thread a moment to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // The dirty bit should be cleared now
  BOOST_CHECK(!blk.area()->is_dirty());
}
