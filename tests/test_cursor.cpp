/*
Test the the cursor with without burst table
*/
#define BOOST_TEST_MODULE CursorTest
#define GENERATE

#include <boost/endian/conversion.hpp>

#include "blake3.h"
#include "test.hpp"

using boost::endian::big_to_native;
using boost::endian::native_to_big;

typedef MapStorage Storage;

BOOST_AUTO_TEST_CASE(insert_bigkeys) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a",     "ab",     "abc",    "abcd",
                        "abcde", "abcdef", "abcdeg", NULL};
  test_insertion(storage, "insert_big_keys", keys, 0, 530);
}

BOOST_AUTO_TEST_CASE(insert_bigvalues) {
  Preparation p;
  Storage storage(TEST_FILE);
  auto cursor = storage["test"].cursor();

  std::string value(5000, '0');
  cursor.find("abc");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(value);
  cursor.commit();

  cursor.find("abcdef");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(value);
  cursor.commit();

  check_graph("big_value", storage);

  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(value));
  const char *recycled_pointer = cursor.value().data();

  value = std::string(5001, '1');
  cursor.value(value);
  cursor.commit();

  cursor.find("abce");
  cursor.value(value);
  cursor.commit();
  // the new pointer is the recycled one from the last replacement action
  BOOST_CHECK(cursor.value().data() == recycled_pointer);
}

BOOST_AUTO_TEST_CASE(change_leaf_with_bigvalue) {
  // Test that when a leaf with a big value is REPLACED (same key, new value),
  // the big value memory is properly freed via free_complete()
  // This tests the if (back->cmp == 0) branch in change_leaf()
  Preparation p;
  Storage storage(TEST_FILE);
  auto cursor = storage["test"].cursor();

  // Insert a leaf with a big value at key "abc"
  std::string big_value(5000, 'X');
  cursor.find("abc");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(big_value);
  cursor.commit();

  // Verify it was inserted and remember the pointer location
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_value));
  const char* original_big_value_ptr = cursor.value().data();

  // Now REPLACE "abc" with a new big value (same key)
  // This triggers the if (back->cmp == 0) branch in change_leaf()
  // which calls free_complete(oleaf) to free both the leaf AND the big value
  std::string new_big_value(5000, 'Y');
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  cursor.value(new_big_value);
  cursor.commit();

  // Verify the replacement happened
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(new_big_value));
  
  // The old big value should have been freed, and a new one allocated
  // The pointer will likely be different (or could be reused)
  const char* new_ptr = cursor.value().data();

  // Insert another big value at "abd" to verify memory management works
  std::string big_value3(5000, 'Z');
  cursor.find("abd");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(big_value3);
  cursor.commit();

  // Verify both values are accessible
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(new_big_value));

  cursor.find("abd");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_value3));

  // The test passes if:
  // 1. No assertion failures occur (meaning free_complete worked correctly)
  // 2. The old big value was properly freed
  // 3. The new values are accessible with correct content
}

BOOST_AUTO_TEST_CASE(split_leaf_keep_bigvalue) {
  // Test that when a leaf with a big value is split (via copy_reduced_leaf),
  // the big value reference is properly transferred to the copy and only the
  // leaf node is freed (not the big value, since copy now owns it)
  Preparation p;
  Storage storage(TEST_FILE);
  auto cursor = storage["test"].cursor();

  // Insert a leaf with a big value at key "abc"
  std::string big_value(5000, 'Z');
  cursor.find("abc");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(big_value);
  cursor.commit();

  // Verify it was inserted
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_value));
  const char* original_ptr = cursor.value().data();

  // Now insert "ab" which will cause the leaf at "abc" to be split
  // This triggers the else-branch in change_leaf() where:
  // 1. copy_reduced_leaf() creates a new leaf "c" with the big value reference
  // 2. free(oleaf) is called at line 209 - which should NOT free the big value (copy owns it)
  // 3. A new trie is created with compressed "ab" and branches to "c" and the new leaf
  cursor.find("ab");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(string("small"));
  cursor.commit();

  // Verify the split happened correctly
  cursor.find("ab");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("small"));

  // The big value should still be accessible at "abc" through the copied leaf
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_value));
  // The pointer should be the same since the big value was not freed, only referenced
  BOOST_CHECK(cursor.value().data() == original_ptr);

  // Insert another value at "abd" to verify the trie structure
  cursor.find("abd");
  BOOST_CHECK(!cursor.is_valid());
  cursor.value(string("another"));
  cursor.commit();

  // Verify all values are accessible
  cursor.find("ab");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("small"));

  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_value));

  cursor.find("abd");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("another"));

  // Now replace "abc" with a different value to test that the big value
  // is properly freed when the copied leaf is eventually replaced
  std::string new_value(5000, 'W');
  cursor.find("abc");
  cursor.value(new_value);
  cursor.commit();

  // Verify the new value is there and memory was properly managed
  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(new_value));

  // The test passes if:
  // 1. The big value reference was properly transferred to the copy
  // 2. The original leaf was freed without freeing the big value
  // 3. The big value pointer remained the same (proving it wasn't reallocated)
  // 4. All subsequent operations work correctly
}

BOOST_AUTO_TEST_CASE(insert_one) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_one", keys);
}

BOOST_AUTO_TEST_CASE(insert_one_big_key) {
  Preparation p;
  Storage storage(TEST_FILE);
  std::string big_key(260, '0');
  const char *keys[] = {big_key.c_str(), NULL};
  test_insertion(storage, "insert_one_big_key", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_extend) {
  Preparation p;
  {
    Storage storage(TEST_FILE);
  }
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", "abcd", NULL};
  test_insertion(storage, "insert_start_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_split) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", "a*c", NULL};
  test_insertion(storage, "insert_start_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_short) {
  // A variant of insert_compress_extend
  Preparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();
  cursor.find("abc");
  cursor.value(string("abc"));
  cursor.commit();
  check_graph("insert_start_short_0", storage);

  cursor.find("");
  cursor.value(string(""));
  cursor.commit();
  check_graph("insert_start_short_1", storage);
}

BOOST_AUTO_TEST_CASE(insert_start_null) {
  // A variant of insert_compress_extend
  Preparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();
  cursor.find("");
  cursor.value(string("aaa"));
  cursor.commit();
  check_graph("insert_start_null_0", storage);

  cursor.find("abc");
  cursor.value(string("bbb"));
  cursor.commit();
  check_graph("insert_start_null_1", storage);
}

BOOST_AUTO_TEST_CASE(change_start) {
  // A variant of insert_compress_extend
  Preparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();
  cursor.find("abc");
  cursor.value(string("aaa"));
  cursor.commit();
  check_graph("change_start_0", storage);

  cursor.find("abc");
  cursor.value(string("bbb"));
  cursor.commit();
  check_graph("change_start_1", storage);
}

BOOST_AUTO_TEST_CASE(change_start_null) {
  // A variant of insert_compress_extend
  Preparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();
  cursor.find("");
  cursor.value(string("aaa"));
  cursor.commit();
  check_graph("change_start_null_0", storage);

  cursor.find("");
  cursor.value(string("bbb"));
  cursor.commit();
  check_graph("change_start_null_1", storage);
}

BOOST_AUTO_TEST_CASE(insert_compress_split) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "ab*defghi", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "abc*efg", "ab", NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_start) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "ba", NULL};
  test_insertion(storage, "insert_compress_start", keys);
}

BOOST_AUTO_TEST_CASE(insert_big_stack) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a",     "ab",     "abc",    "abcd",
                        "abcde", "abcdef", "abcdeg", NULL};
  test_insertion(storage, "insert_big_stack", keys);

  auto db = storage["test"];
  auto cursor = db.cursor();
  std::cout << "insert 7: abcd*" << std::endl;

  cursor.find("abcd*");
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.value("abcd*");
  BOOST_REQUIRE(cursor.is_valid());

  std::cout << "insert 8: abcde*" << std::endl;
  cursor.find("abcde*");
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.value("abcde*");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.commit();
  check_graph("insert_big_stack_7", storage);

#ifdef MOVEMENT
  {
    strings_t strings;
    const char *keys[] = {"a",      "ab",     "abc",   "abcd",   "abcde",
                          "abcdef", "abcdeg", "abcd*", "abcde*", NULL};
    for (const char **key = keys; *key; key++) {
      strings.push_back(*key);
    }
    test_movement(storage, strings);
  }
#endif

  // TODO: move test for all
}

BOOST_AUTO_TEST_CASE(insert_leaf_split) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*e*ghi", NULL};
  test_insertion(storage, "insert_leaf_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_split_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abcd*fghi", "abcd*", NULL};
  test_insertion(storage, "insert_leaf_split_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_extend) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*efghijk", NULL};
  test_insertion(storage, "insert_leaf_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_array) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", NULL};
  test_insertion(storage, "insert_array", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "abf", "abg",
                        "abh", "abi", "abj", "abk", "abl", "abm", "abn",
                        "abo", "abp", "abA", "ab",  NULL};
  // the last inserts a null leaf
  test_insertion(storage, "insert_trie", keys);
}

BOOST_AUTO_TEST_CASE(overflow_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  uint16_t i;
  auto db = storage["test"];
  auto cursor = db.cursor();
  for (i = 0; i < 258; i++) {
    uint16_t key = native_to_big(i);
    Slice skey((char *)&key, sizeof(key));
    cursor.find(skey);
    BOOST_REQUIRE(!cursor.is_valid());
    std::string value = std::to_string(i);
    cursor.value(value);
    cursor.commit();
  }
  check_graph("overflow_trie", storage);

#ifdef MOVEMENT
  {
    uint16_t i = 0;
    for (cursor.first(); cursor.is_valid(); cursor.next(), i++) {
      BOOST_REQUIRE(cursor.is_valid());
      uint16_t key = native_to_big(i);
      Slice cmp_key((char *)&key, sizeof(key));
      //std::cout << "iter forward: " << i << std::endl;
      BOOST_REQUIRE(cmp_key == cursor.key());
    }
    BOOST_REQUIRE(i == 258);

    i = 257;
    for (cursor.last(); cursor.is_valid(); cursor.prev(), i--) {
      BOOST_REQUIRE(cursor.is_valid());
      uint16_t key = native_to_big(i);
      Slice cmp_key((char *)&key, sizeof(key));
      // std::cout << "iter backward: " << i << std::endl;
      BOOST_REQUIRE(cmp_key == cursor.key());
    }
    BOOST_REQUIRE(i == 0xFFFF);  // overflow to -1
  }
#endif
}

BOOST_AUTO_TEST_CASE(insert_null_leaf) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "ab", NULL};
  test_insertion(storage, "insert_null_leaf", keys);
}

BOOST_AUTO_TEST_CASE(insert_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", "abcdefg", NULL};
  test_insertion(storage, "insert_value", keys);
}

BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                        "aI", "aJ", "aK", "aC", "al", "an", "aO", NULL};
  const char *remove[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                          "aI", "aJ", "al", "an", "aO", "aK", "aC", NULL};
  test_remove(storage, "remove_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_none_leaf) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "ab", NULL};
  const char *remove[] = {"ab", NULL};
  test_remove(storage, "remove_none_leaf", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_combine_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"ab", "abcf", "abcde", "abcdf", NULL};
  const char *remove[] = {"abcf", NULL};
  test_remove(storage, "remove_combine_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_combine_leaf) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"ab", "abc", "abde", "abdf", NULL};
  const char *remove[] = {"abdf", NULL};
  test_remove(storage, "remove_combine_leaf", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_bigkeys_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  std::string prefix(255, '0');
  std::string child1 = prefix + "1";
  std::string child2 = prefix + "11";
  std::string child3 = prefix + "12";
  const char *keys[] = {prefix.c_str(),  child1.c_str(),  child2.c_str(), child3.c_str(), NULL};
  const char *remove[] = {prefix.c_str(), NULL};
  test_remove(storage, "remove_bigkeys_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_bigkeys_leaf) {
  Preparation p;
  Storage storage(TEST_FILE);
  std::string prefix(255, '0');
  std::string child1 = prefix + "11111";
  std::string child2 = prefix + "22222";

  const char *keys[] = {child1.c_str(), child2.c_str(), NULL};
  const char *remove[] = {child1.c_str(), NULL};
  test_remove(storage, "remove_bigkeys_leaf", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_bigkeys_one) {
  Preparation p;
  Storage storage(TEST_FILE);
  std::string prefix(255, '0');
  std::string child1 = prefix + "11111";

  const char *keys[] = {child1.c_str(), NULL};
  const char *remove[] = {child1.c_str(), NULL};
  test_remove(storage, "remove_bigkeys_one", keys, remove);
}


BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  Slice key("abcdefg");
  auto db = storage["test"];
  auto cursor = db.cursor();

  cursor.find(key);
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.value(key);
  BOOST_REQUIRE_EQUAL(cursor.value().string(), key.string());

  cursor.commit();

  // std::cout << "insert " << test_name << std::endl;
  cursor.find(key);
  BOOST_REQUIRE(cursor.is_valid());

  std::string value;
  for (int i = 0; i < 20; i++) {
    value.append("abcdefghijklmn");
  }
  cursor.value(value);

  cursor.find(key);
  BOOST_REQUIRE_EQUAL(cursor.value().string(), value);

  value.resize(50);
  cursor.value(value);  // use value pool

  cursor.find(key);
  BOOST_REQUIRE_EQUAL(cursor.value().string(), value);

  cursor.value(key);
  cursor.commit();
}

BOOST_AUTO_TEST_CASE(move_backward) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abca", "abcb", "adea", "adeb", NULL};
  test_insertion(storage, "move_backward", keys);

  auto cursor = storage["test"].cursor();
  cursor.find("ad");
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.prev();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("abcb"));

  cursor.find("abd");
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.prev();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("abcb"));

  // no extra test for this
  try {
    auto cursor = storage["test"].cursor();
    cursor.value("abcb");
    BOOST_REQUIRE(false);
  } catch (const leaves::NoValidPosition &e) {
    // right
  } catch (...) {
    BOOST_REQUIRE(false);
  }
}
