/*
Test the trie nodes without bursting.
*/
#define BOOST_TEST_MODULE TrieTest

#include "test.hpp"

BOOST_AUTO_TEST_CASE(insert_null) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_null", keys);
}

BOOST_AUTO_TEST_CASE(insert_compresstrie_split) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abddefg", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compresstrie_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "ab",  NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", "abcdefg",  NULL};
  test_insertion(storage, "insert_value", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "ab", NULL};
  test_insertion(storage, "insert_trie_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_lower_grow) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                        "aI", "aJ", "aK", "aC", "aL", "aN", "aO", NULL};
  test_insertion(storage, "insert_trie_lower_grow", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_upper_grow) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a ", "a0", "a@", "aP", "ap", NULL};
  test_insertion(storage, "insert_trie_upper_grow", keys);
}

BOOST_AUTO_TEST_CASE(big_compressed) {
  Preparation p;
  Storage storage(TEST_FILE);

  std::string key1(4010, 'a');

  const char *keys[] = {key1.c_str(), NULL};
  test_insertion(storage, "big_compressed", keys);
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
  const char *remove[] = {"ab", NULL };
  test_remove(storage, "remove_intermediate_value", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "abcefgh", "ab", NULL};
  const char *remove[] = {"ab", NULL };
  test_remove(storage, "remove_compress_short", keys, remove);
}
#endif

BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  Slice key("abcdefg");
  Trace trace(storage);

  trace.find(key);
  BOOST_REQUIRE(!trace.valid());
  trace.set_value(key);
  trace.commit();

  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(trace.valid());

  std::string value;
  for(int i = 0; i < 20; i++) {
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

BOOST_AUTO_TEST_CASE(insert_at_empty) {
  Preparation p;
  Storage storage(TEST_FILE);
  Trace trace(storage);
  trace.find("");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("insert_empty_0", storage);

  trace.find("abc");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("insert_empty_1", storage);
}

BOOST_AUTO_TEST_CASE(page_split_compressed) {
  Preparation p;
  Storage storage(TEST_FILE);

  std::string key1(3950, 'a');

  const char *keys[] = {key1.c_str(), "abcdefghaijklmnopqrstxyz", NULL};
  test_insertion(storage, "page_split_compressed", keys);

  const char *testpoints[] = {"PageSplitAtNode", "PageSplit"};
  check_testpoints(2, testpoints);
}


BOOST_AUTO_TEST_CASE(page_split_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  Trace trace(storage);
  for (int i = 0; i < 1000; i++) {
    //std::cout << "insert " << i << std::endl;
    std::string key = std::to_string(i);
    trace.find(key);
    BOOST_REQUIRE(!trace.valid());
    trace.set_value(key);
    trace.commit();
  }
  check_graph("page_split_trie", storage);

  const char *testpoints[] = {"PageSplitAtTrie", "PageSplit", "PageMerge"};
  check_testpoints(3, testpoints);
}


