#define BOOST_TEST_MODULE ModifyTest
//#define GENERATE

#include <cstdio>
#include <boost/test/included/unit_test.hpp>
#include "test.hpp"


void test_insert_first(Storage& storage) {
  check_graph("empty", storage);
  insert(storage, Slice("abcdefg"), "first");
}

void test_divide_compressed(Storage& storage) {
  insert(storage, Slice("abhij"), "divide_compressed");
}

void test_divide_compressed_value(Storage& storage) {
  insert(storage, Slice("ab"), "divide_compressed_value");
}

void test_add_value_node(Storage& storage) {
  insert(storage, Slice("ab"), "value_to_trie");
}

void test_insert_index(Storage& storage) {
  insert(storage, "abd", "insert_index_abd");
}

void test_insert_grow(Storage& storage) {
  insert(storage, "aba", "insert_index_a");
  insert(storage, "abb", "insert_index_b");
  insert(storage, "abe", "insert_index_e");
  insert(storage, "abf", "insert_index_f");
  insert(storage, "abg", "insert_index_g");
  insert(storage, "abi", "insert_index_i");
  insert(storage, "abj", "insert_index_j");
  insert(storage, "abk", "insert_index_k");
  insert(storage, "abl", "insert_index_l");
  insert(storage, "abm", "insert_index_m");
  insert(storage, "abn", "insert_index_n");
  insert(storage, "abo", "insert_index_o");
  insert(storage, "ab`", "insert_index_60");
}

BOOST_AUTO_TEST_SUITE(NullNode)

BOOST_AUTO_TEST_CASE(insert) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(CompressedNode)

BOOST_AUTO_TEST_CASE(trie_divide) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
}

BOOST_AUTO_TEST_CASE(value_divide) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed_value(storage);
}

BOOST_AUTO_TEST_CASE(very_big) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);

  std::string key;
  for(int i = 0; i < 20; i++) {
    key.append("abcdefghijklmn");
  }
  insert(storage, Slice(key), "very_big");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ValueNode)

BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);

  Slice key("abcdefg");
  Trace trace(storage);
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

  trace.set_value(key);
}

BOOST_AUTO_TEST_CASE(remove_intermediate) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed_value(storage);

  Trace trace(storage);
  Slice key("ab");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  trace.remove();
  check_graph("remove_intermediate", storage);
}

BOOST_AUTO_TEST_CASE(remove_until_intermediate) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed_value(storage);

  Trace trace(storage);
  Slice key("abcdefg");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  trace.remove();
  check_graph("remove_until_intermediate", storage);
}


BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TrieNode)

BOOST_AUTO_TEST_CASE(add_value_node) {
  // add a value to trie
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_add_value_node(storage);

  Trace trace(storage);
  Slice key("abhij");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), key.string());
}

BOOST_AUTO_TEST_CASE(insert_index) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
}

BOOST_AUTO_TEST_CASE(insert_grow_lower) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
  test_insert_grow(storage);
}

BOOST_AUTO_TEST_CASE(insert_grow_upper) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  insert(storage, Slice("abp"), "insert_index_p");
  insert(storage, Slice("ab0"), "insert_index_0");
}

BOOST_AUTO_TEST_CASE(shrink_lower) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
  insert(storage, "aba", "insert_index_a");
  insert(storage, "abb", "insert_index_b");

  Trace trace(storage);
  Slice key("abcdefg");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  trace.remove();
  check_graph("removed_abcdefg", storage);
}

BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);

  Trace trace(storage);
  Slice key("abcdefg");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  trace.remove();
  check_graph("removed_trie", storage);
}

BOOST_AUTO_TEST_CASE(remove_lower) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  insert(storage, Slice("abp"), "insert_index_p");
  insert(storage, Slice("ab0"), "insert_index_0");

  Trace trace(storage);
  Slice key("abp");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  trace.remove();
  check_graph("removed_abp", storage);
}

BOOST_AUTO_TEST_CASE(keep_lower) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  insert(storage, Slice("abp"), "insert_index_p");
  insert(storage, Slice("ab0"), "insert_index_0");
  insert(storage, Slice("abpqrs"), "insert_index_abpqrs");

  Trace trace(storage);
  Slice key("abp");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  trace.remove();
  check_graph("removed_abp_intermediate", storage);
}

BOOST_AUTO_TEST_SUITE_END()
