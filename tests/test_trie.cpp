/*
Test the trie nodes without bursting.
*/
#define BOOST_TEST_MODULE TrieTest
#define GENERATE

#include "test.hpp"

BOOST_AUTO_TEST_CASE(insert_start_extend) {
  Preparation p;
  { DBMemory storage(TEST_FILE); }
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", "abcd", NULL};
  test_insertion(storage, "insert_start_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_split) {
  Preparation p;
  { DBMemory storage(TEST_FILE); }
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", "a*c", NULL};
  test_insertion(storage, "insert_start_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_short) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("abc");
  trace.set_value(string("abc"));
  trace.commit();
  check_graph("insert_start_short_0", storage);

  trace.find("");
  trace.set_value(string(""));
  trace.commit();
  check_graph("insert_start_short_1", storage);
}

BOOST_AUTO_TEST_CASE(insert_start_null) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("insert_start_null_0", storage);

  trace.find("abc");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("insert_start_null_1", storage);
}

BOOST_AUTO_TEST_CASE(change_start) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("abc");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("change_start_0", storage);

  trace.find("abc");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("change_start_1", storage);
}

BOOST_AUTO_TEST_CASE(change_start_null) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("change_start_null_0", storage);

  trace.find("");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("change_start_null_1", storage);
}

BOOST_AUTO_TEST_CASE(insert_compress_split) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "ab*defghi", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_short) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "ab", NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_start) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "ba", NULL};
  test_insertion(storage, "insert_compress_start", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_split) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*e*ghi", NULL};
  test_insertion(storage, "insert_leaf_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_split_short) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abcd*fghi", "abcd*", NULL};
  test_insertion(storage, "insert_leaf_split_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_extend) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*efghijk", NULL};
  test_insertion(storage, "insert_leaf_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_array) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", NULL};
  test_insertion(storage, "insert_array", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "abf",
                        "abg", "abh", "abi", "abj", "abk", "abl",
                        "abm", "abn", "abo", "abp", "abA", NULL};
  test_insertion(storage, "insert_trie", keys);
}

BOOST_AUTO_TEST_CASE(insert_null_leaf) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "ab", NULL};
  test_insertion(storage, "insert_null_leaf", keys);
}


BOOST_AUTO_TEST_CASE(insert_value) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", "abcdefg", NULL};
  test_insertion(storage, "insert_value", keys);
}


#ifdef WITH_REMOVE
BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                        "aI", "aJ", "aK", "aC", "aL", "aN", "aO", NULL};
  const char *remove[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                          "aI", "aJ", "aK", "aC", "aL", "aN", "aO", NULL};
  test_remove(storage, "remove_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_intermediate_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "ab", NULL};
  const char *remove[] = {"ab", NULL};
  test_remove(storage, "remove_intermediate_value", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "abcefgh", "ab", NULL};
  const char *remove[] = {"ab", NULL};
  test_remove(storage, "remove_compress_short", keys, remove);
}
#endif

BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  Slice key("abcdefg");
  Trace trace(storage);

  trace.find(key);
  BOOST_REQUIRE(!trace.is_valid());
  trace.set_value(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), key.string());

  trace.commit();

  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(trace.is_valid());

  std::string value;
  for (int i = 0; i < 20; i++) {
    value.append("abcdefghijklmn");
  }
  trace.set_value(value);

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  value.resize(50);
  trace.set_value(value);  // use value pool

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  trace.set_value(key);
  trace.commit();
}

/*
BOOST_AUTO_TEST_CASE(page_split_compressed) {
  Preparation p;
  Storage storage(TEST_FILE);

  std::string key1(3950, 'a');

  const char *keys[] = {key1.c_str(), "abcdefghaijklmnopqrstxyz", NULL};
  test_insertion(storage, "page_split_compressed", keys);

  const char *testpoints[] = {"PageSplitAtNode", "PageSplit", NULL};
  check_testpoints(testpoints);
}


BOOST_AUTO_TEST_CASE(page_split_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  Trace trace(storage);
  for (int i = 0; i < 1000; i++) {
    //std::cout << "insert " << i << std::endl;
    std::string key = std::to_string(i);
    trace.find(key);
    BOOST_REQUIRE(!trace.isvalid());
    trace.set_value(key);
    trace.commit();
  }
  check_graph("page_split_trie", storage);

  const char *testpoints[] = {"PageSplitAtTrie", "PageSplit", "PageMerge",
NULL}; check_testpoints(testpoints);
}


*/