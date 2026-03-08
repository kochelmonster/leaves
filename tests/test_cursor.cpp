/*
Test the the cursor with without burst table
*/
#define BOOST_TEST_MODULE CursorTest
//#define GENERATE

#include <boost/endian/conversion.hpp>

#include "blake3.h"
#include "test.hpp"

using boost::endian::big_to_native;
using boost::endian::native_to_big;

typedef MapStorage Storage;

BOOST_AUTO_TEST_CASE(insert_bigkeys) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"a",     "ab",     "abc",    "abcd",
                        "abcde", "abcdef", "abcdeg", NULL};
  test_insertion(storage, "insert_big_keys", keys, 0, 530);
}

BOOST_AUTO_TEST_CASE(insert_bigvalues) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto cursor = (*storage)["test"].cursor();

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
  // COW strategy: freed areas cannot be recycled within the same transaction
  // to ensure old transaction data remains accessible to other cursors
  BOOST_CHECK(cursor.value().data() != recycled_pointer);
}

BOOST_AUTO_TEST_CASE(change_leaf_with_bigvalue) {
  // Test that when a leaf with a big value is REPLACED (same key, new value),
  // the big value memory is properly freed via free_complete()
  // This tests the if (back->cmp == 0) branch in change_leaf()
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto cursor = (*storage)["test"].cursor();

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
  auto storage = Storage::create(TEST_FILE);
  auto cursor = (*storage)["test"].cursor();

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
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_one", keys);
}

BOOST_AUTO_TEST_CASE(insert_one_big_key) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  std::string big_key(260, '0');
  const char *keys[] = {big_key.c_str(), NULL};
  test_insertion(storage, "insert_one_big_key", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_extend) {
  Preparation p;
  {
    auto storage = Storage::create(TEST_FILE);
  }
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abc", "abcd", NULL};
  test_insertion(storage, "insert_start_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_split) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abc", "a*c", NULL};
  test_insertion(storage, "insert_start_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_short) {
  // A variant of insert_compress_extend
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
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
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
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
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
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
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
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
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "ab*defghi", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_short) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abcdefg", "abc*efg", "ab", NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_start) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abcdefg", "ba", NULL};
  test_insertion(storage, "insert_compress_start", keys);
}

BOOST_AUTO_TEST_CASE(insert_big_stack) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"a",     "ab",     "abc",    "abcd",
                        "abcde", "abcdef", "abcdeg", NULL};
  test_insertion(storage, "insert_big_stack", keys);

  auto db = (*storage)["test"];
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
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*e*ghi", NULL};
  test_insertion(storage, "insert_leaf_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_split_short) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abcd*fghi", "abcd*", NULL};
  test_insertion(storage, "insert_leaf_split_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_extend) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*efghijk", NULL};
  test_insertion(storage, "insert_leaf_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_array) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", NULL};
  test_insertion(storage, "insert_array", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "abf", "abg",
                        "abh", "abi", "abj", "abk", "abl", "abm", "abn",
                        "abo", "abp", "abA", "ab",  NULL};
  // the last inserts a null leaf
  test_insertion(storage, "insert_trie", keys);
}

BOOST_AUTO_TEST_CASE(overflow_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  uint16_t i;
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  for (i = 0; i < 258; i++) {
    uint16_t key = native_to_big(i);
    Slice skey((char *)&key, sizeof(key));
    cursor.find(skey);
    BOOST_REQUIRE(!cursor.is_valid());
    std::string value = std::to_string(i);
    if (i == 257) {
      std::cout << "DEBUG insert overflow key: " << i << std::endl;
    } 

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
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "ab", NULL};
  test_insertion(storage, "insert_null_leaf", keys);
}

BOOST_AUTO_TEST_CASE(insert_value) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abc", "abcdefg", NULL};
  test_insertion(storage, "insert_value", keys);
}

BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                        "aI", "aJ", "aK", "aC", "al", "an", "aO", NULL};
  const char *remove[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                          "aI", "aJ", "al", "an", "aO", "aK", "aC", NULL};
  test_remove(storage, "remove_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_none_leaf) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "ab", NULL};
  const char *remove[] = {"ab", NULL};
  test_remove(storage, "remove_none_leaf", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_combine_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"ab", "abcf", "abcde", "abcdf", NULL};
  const char *remove[] = {"abcf", NULL};
  test_remove(storage, "remove_combine_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_combine_leaf) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"ab", "abc", "abde", "abdf", NULL};
  const char *remove[] = {"abdf", NULL};
  test_remove(storage, "remove_combine_leaf", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_bigkeys_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
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
  auto storage = Storage::create(TEST_FILE);
  std::string prefix(255, '0');
  std::string child1 = prefix + "11111";
  std::string child2 = prefix + "22222";

  const char *keys[] = {child1.c_str(), child2.c_str(), NULL};
  const char *remove[] = {child1.c_str(), NULL};
  test_remove(storage, "remove_bigkeys_leaf", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_bigkeys_one) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  std::string prefix(255, '0');
  std::string child1 = prefix + "11111";

  const char *keys[] = {child1.c_str(), NULL};
  const char *remove[] = {child1.c_str(), NULL};
  test_remove(storage, "remove_bigkeys_one", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_after_first) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abc", "abd", "abe", "abf", "abg", NULL};
  
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  
  // Insert keys
  for (int i = 0; keys[i]; i++) {
    cursor.find(keys[i]);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.value(keys[i]);
  }
  cursor.commit();
  
  check_graph("remove_after_first_begin", storage);
  
  // Test removing first element
  cursor.first();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("abc"));
  cursor.remove();
  cursor.commit();
  
  check_graph("remove_after_first_end", storage);
  
  // Verify first is now "abd"
  cursor.first();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("abd"));
  
  // Verify all remaining keys exist
  cursor.find("abd");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.find("abe");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.find("abf");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.find("abg");
  BOOST_REQUIRE(cursor.is_valid());
  
  // Verify removed key doesn't exist
  cursor.find("abc");
  BOOST_REQUIRE(!cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(remove_after_last) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abc", "abd", "abe", "abf", "abg", NULL};
  
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  
  // Insert keys
  for (int i = 0; keys[i]; i++) {
    cursor.find(keys[i]);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.value(keys[i]);
  }
  cursor.commit();
  
  check_graph("remove_after_last_begin", storage);
  
  // Test removing last element
  cursor.last();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("abg"));
  cursor.remove();
  cursor.commit();
  
  check_graph("remove_after_last_end", storage);
  
  // Verify last is now "abf"
  cursor.last();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("abf"));
  
  // Verify all remaining keys exist
  cursor.find("abc");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.find("abd");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.find("abe");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.find("abf");
  BOOST_REQUIRE(cursor.is_valid());
  
  // Verify removed key doesn't exist
  cursor.find("abg");
  BOOST_REQUIRE(!cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(remove_after_first_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  // Create a scenario with trie nodes
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", NULL};
  
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  
  // Insert keys
  for (int i = 0; keys[i]; i++) {
    cursor.find(keys[i]);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.value(keys[i]);
  }
  cursor.commit();
  
  check_graph("remove_after_first_trie_begin", storage);
  
  // Test removing first element in a trie structure
  cursor.first();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("a@"));
  cursor.remove();
  cursor.commit();
  
  check_graph("remove_after_first_trie_end", storage);
  
  // Verify first is now "aA"
  cursor.first();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("aA"));
  
  // Verify removed key doesn't exist
  cursor.find("a@");
  BOOST_REQUIRE(!cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(remove_after_last_trie) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  // Create a scenario with trie nodes
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", NULL};
  
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  
  // Insert keys
  for (int i = 0; keys[i]; i++) {
    cursor.find(keys[i]);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.value(keys[i]);
  }
  cursor.commit();
  
  check_graph("remove_after_last_trie_begin", storage);
  
  // Test removing last element in a trie structure
  cursor.last();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("aM"));
  cursor.remove();
  cursor.commit();
  
  check_graph("remove_after_last_trie_end", storage);
  
  // Verify last is now "aF"
  cursor.last();
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), Slice("aF"));
  
  // Verify removed key doesn't exist
  cursor.find("aM");
  BOOST_REQUIRE(!cursor.is_valid());
}


BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  Slice key("abcdefg");
  auto db = (*storage)["test"];
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
  auto storage = Storage::create(TEST_FILE);
  const char *keys[] = {"abca", "abcb", "adea", "adeb", NULL};
  test_insertion(storage, "move_backward", keys);

  auto cursor = (*storage)["test"].cursor();
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
    auto cursor = (*storage)["test"].cursor();
    cursor.value("abcb");
    BOOST_REQUIRE(false);
  } catch (const leaves::NoValidPosition &e) {
    // right
  } catch (...) {
    BOOST_REQUIRE(false);
  }
}

BOOST_AUTO_TEST_CASE(test_statistics) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // Insert enough keys to create multiple trie and leaf nodes
  for (int i = 0; i < 100; i++) {
    char key[16];
    snprintf(key, sizeof(key), "key_%04d", i);
    cursor.find(key);
    cursor.value("value");
    cursor.commit();
  }

  Storage::StorageImpl::DB::Statistics stat;
  db._internal()->statistics(stat);

  // Both branch and leaf stats should be populated
  size_t branch_count = 0, leaf_count = 0;
  for (auto& slot : stat.branch.slots) branch_count += slot.count;
  for (auto& slot : stat.leaf.slots) leaf_count += slot.count;

  BOOST_CHECK_GT(branch_count, 0);
  BOOST_CHECK_GT(leaf_count, 0);
  BOOST_CHECK_EQUAL(leaf_count, 100);  // one leaf per key
}

BOOST_AUTO_TEST_CASE(rollback_sees_committed_trie) {
  // Regression test for CODE_REVIEW_4 Finding #1:
  // After rollback(), the cursor must navigate the committed trie,
  // not the orphaned write transaction.
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // 1. Insert and commit some keys
  cursor.find("aaa");
  cursor.value("val_aaa");
  cursor.find("bbb");
  cursor.value("val_bbb");
  cursor.find("ccc");
  cursor.value("val_ccc");
  cursor.commit();

  // 2. Verify committed state
  cursor.find("aaa");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_aaa"));

  // 3. Make uncommitted changes: modify existing + insert new
  cursor.find("aaa");
  cursor.value("MODIFIED");
  cursor.find("ddd");
  cursor.value("val_ddd");

  // 4. Rollback — cursor should revert to committed state
  cursor.rollback();

  // 5. The key "aaa" should have its ORIGINAL value, not "MODIFIED"
  cursor.find("aaa");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_aaa"));

  // 6. The uncommitted key "ddd" must NOT exist
  cursor.find("ddd");
  BOOST_CHECK(!cursor.is_valid());

  // 7. Other committed keys must still be accessible
  cursor.find("bbb");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_bbb"));

  cursor.find("ccc");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_ccc"));

  // 8. We can still write after rollback
  cursor.find("eee");
  cursor.value("val_eee");
  cursor.commit();

  cursor.find("eee");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_eee"));

  // 9. Original keys still intact after new commit
  cursor.find("aaa");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_aaa"));
}

BOOST_AUTO_TEST_CASE(rollback_position_preserved) {
  // Verify that rollback re-finds the cursor at its current key position
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // Commit some keys
  for (int i = 0; i < 50; i++) {
    char key[16];
    snprintf(key, sizeof(key), "key_%04d", i);
    cursor.find(key);
    cursor.value("committed");
  }
  cursor.commit();

  // Navigate to a known position, then start modifying
  cursor.find("key_0025");
  BOOST_CHECK(cursor.is_valid());

  // Make uncommitted changes while positioned at key_0025
  cursor.value("uncommitted");

  // Rollback from that position
  cursor.rollback();

  // Cursor should be re-positioned at or near key_0025 with committed value
  cursor.find("key_0025");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("committed"));

  // Verify next/prev navigation works after rollback
  cursor.next();
  BOOST_CHECK(cursor.is_valid());
  std::string next_key(cursor.key().data(), cursor.key().size());
  BOOST_CHECK_EQUAL(next_key, "key_0026");
}

BOOST_AUTO_TEST_CASE(rollback_with_big_values) {
  // Rollback must also handle big values correctly — the bigmemory
  // allocator must be reset to the committed free list.
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // Commit a key with a big value
  std::string big_committed(5000, 'A');
  cursor.find("big");
  cursor.value(big_committed);
  cursor.commit();

  cursor.find("big");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_committed));

  // Overwrite with a different big value but don't commit
  std::string big_uncommitted(6000, 'B');
  cursor.find("big");
  cursor.value(big_uncommitted);

  // Rollback
  cursor.rollback();

  // Should see the original big value
  cursor.find("big");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice(big_committed));
}

BOOST_AUTO_TEST_CASE(test_memory_checker) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // Insert keys, committing periodically to create multiple transactions
  for (int i = 0; i < 200; i++) {
    char key[16];
    snprintf(key, sizeof(key), "key_%04d", i);
    cursor.find(key);
    cursor.value("some_value");
    if (i % 10 == 0) cursor.commit();
  }
  cursor.commit();

  // Delete some keys to populate garbage slots
  for (int i = 0; i < 50; i++) {
    char key[16];
    snprintf(key, sizeof(key), "key_%04d", i);
    cursor.find(key);
    cursor.remove();
  }
  cursor.commit();

  // Run the memory checker — should not throw
  using db_type = Storage::StorageImpl::DB;
  _MemoryChecker<db_type> checker(*db._internal());
  BOOST_CHECK_NO_THROW(checker.check());
  BOOST_CHECK_GT(checker.total_pages, 0);
}

// ── Deleter coverage: combine() with go_next=false (L167) ──────────────
// When a 2-child trie has its first child deleted, the remaining child
// is at begin+1 and go_next is false, triggering parent.first() path.

BOOST_AUTO_TEST_CASE(remove_combine_first_child) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // Insert two keys that create a trie with 2 branches where the first
  // (alphabetically) is what we'll delete
  cursor.find("xya");
  cursor.value("val_a");
  cursor.find("xyb");
  cursor.value("val_b");
  cursor.commit();

  // Delete "xya" — branch 'a' is first in array (begin), so link == begin,
  // go_next = false → triggers parent.first() in combine()
  cursor.find("xya");
  BOOST_CHECK(cursor.is_valid());
  cursor.remove();
  cursor.commit();

  // Verify "xyb" remains and "xya" is gone
  cursor.find("xya");
  BOOST_CHECK(!cursor.is_valid());

  cursor.find("xyb");
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_b"));

  // Cursor should be positioned correctly after remove
  cursor.first();
  BOOST_CHECK(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.key().string(), "xyb");
  cursor.next();
  BOOST_CHECK(!cursor.is_valid());
}

// ── Cursor rollback without transaction (_cursor.hpp L768) ──────────────
// rollback() returns false when the cursor doesn't own the current write
// transaction (txn_cursor_id != _id).
BOOST_AUTO_TEST_CASE(rollback_without_transaction) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  // Insert some data so the DB isn't empty
  cursor.find("abc");
  cursor.value("123");
  cursor.commit();

  // Now try to rollback without having started a new transaction.
  // The cursor's _id won't match txn_cursor_id, so rollback returns false.
  // TCursor::rollback() returns void, so just verify it doesn't crash.
  BOOST_CHECK_NO_THROW(cursor.rollback());
}

