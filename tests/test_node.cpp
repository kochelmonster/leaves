#define BOOST_TEST_MODULE ModifyTest
// #define GENERATE

#include <cstdio>
#include <boost/test/included/unit_test.hpp>
#include "test.hpp"

using std::string;

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

BOOST_AUTO_TEST_SUITE(ModifyNullNode)

BOOST_AUTO_TEST_CASE(insert) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(ModifyCompressedNode)

BOOST_AUTO_TEST_CASE(trie_divide) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);
  test_divide_compressed(storage);
}

BOOST_AUTO_TEST_CASE(value_divide) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);
  test_divide_compressed_value(storage);
}

BOOST_AUTO_TEST_CASE(very_big) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);

  std::string key;
  for(int i = 0; i < 20; i++) {
    key.append("abcdefghijklmn");
  }
  insert(storage, Slice(key), "very_big");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ModifyValueNode)

BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
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

  value = "abcdefghijklmn";
  value.resize(100);
  trace.set_value(value);  // use value pool

  BOOST_REQUIRE_EQUAL(storage.value_pools[0].pool->used_nodes, 1);

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  trace.set_value(key);

  BOOST_REQUIRE_EQUAL(storage.value_pools[0].pool->used_nodes, 0);
  BOOST_REQUIRE_EQUAL(storage.value_pools[0].pool->freed_nodes, 1);
}

BOOST_AUTO_TEST_CASE(remove_intermediate) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
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
  Storage storage(TEST_FILE, TEST_OPTIONS);
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

BOOST_AUTO_TEST_SUITE(ModifyTrieNode)

BOOST_AUTO_TEST_CASE(add_value_node) {
  // add a value to trie
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
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
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
}

BOOST_AUTO_TEST_CASE(insert_grow_lower) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
  test_insert_grow(storage);
}

BOOST_AUTO_TEST_CASE(insert_grow_upper) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  test_insert_first(storage);
  test_divide_compressed(storage);
  insert(storage, Slice("abp"), "insert_index_p");
  insert(storage, Slice("ab0"), "insert_index_0");
}

BOOST_AUTO_TEST_CASE(shrink_lower) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
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
  Storage storage(TEST_FILE, TEST_OPTIONS);
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
  Storage storage(TEST_FILE, TEST_OPTIONS);
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
  Storage storage(TEST_FILE, TEST_OPTIONS);
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


void fill_db(Storage& storage) {
  insert(storage, Slice("abc"), "fill_abc");
  insert(storage, Slice("abc123"), "fill_abc123");
  insert(storage, Slice("abc323"), "fill_abc323");
  insert(storage, Slice("abc523"), "fill_abc523");
  insert(storage, Slice("abc723"), "fill_abc723");
  insert(storage, Slice("abcA23"), "fill_abcA23");
  insert(storage, Slice("bcd1234"), "fill_bcd1234");
  insert(storage, Slice("bcd1235"), "fill_bcd1235");
}


BOOST_AUTO_TEST_SUITE(MoveForward)

BOOST_AUTO_TEST_CASE(move_forward_empty) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  Trace trace(storage);

  trace.first();
  BOOST_REQUIRE(!trace.valid());

  trace.next();
  BOOST_REQUIRE(!trace.valid());
}

BOOST_AUTO_TEST_CASE(move_forward) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  fill_db(storage);

  Trace trace(storage);

  trace.first();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc123"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc123"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc323"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc323"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc523"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc523"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc723"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc723"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abcA23"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abcA23"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1234"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1234"));

  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1235"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1235"));

  trace.next();
  BOOST_REQUIRE(!trace.valid());

  // jump into intermediate value
  trace.find(Slice("abc"));
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc123"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc123"));

  // jump into unknown trie value
  trace.find(Slice("abc2"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc2"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc323"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc323"));

  // jump into trievalue with empty key
  trace.find(Slice("bcd123"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd123"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1234"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1234"));

  // jump before compressed
  trace.find(Slice("aba"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("a"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc"));

  // jump after compressed
  trace.find(Slice("abd"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("a"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1234"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1234"));

  // jump before compressed
  trace.find(Slice("aa"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("a"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc"));

  // jump after compressed
  trace.find(Slice("ac"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("a"));
  trace.next();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1234"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1234"));
}


BOOST_AUTO_TEST_CASE(move_backward) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  fill_db(storage);

  Trace trace(storage);

  trace.last();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1235"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1235"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1234"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1234"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abcA23"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abcA23"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc723"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc723"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc523"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc523"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc323"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc323"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc123"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc123"));

  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc"));

  trace.prev();
  BOOST_REQUIRE(!trace.valid());

  // jump into intermediate value
  trace.find(Slice("abc"));
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc"));
  trace.prev();
  BOOST_REQUIRE(!trace.valid());

  // jump into unknown trie value
  trace.find(Slice("abc2"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc2"));
  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abc123"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abc123"));

  // jump into trievalue with empty key
  trace.find(Slice("bcd123"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd123"));
  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abcA23"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abcA23"));

  // jump before compressed
  trace.find(Slice("bcd122"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("b"));
  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abcA23"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abcA23"));

  // jump after compressed
  trace.find(Slice("bcd124"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("b"));
  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1235"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1235"));

  // jump before compressed
  trace.find(Slice("ba"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("b"));
  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("abcA23"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("abcA23"));

  // jump after compressed
  trace.find(Slice("bd"));
  BOOST_REQUIRE(!trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, string("b"));
  trace.prev();
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), string("bcd1235"));
  BOOST_REQUIRE_EQUAL(trace.current_key, string("bcd1235"));
}

BOOST_AUTO_TEST_CASE(move_backward_empty) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  Trace trace(storage);

  trace.last();
  BOOST_REQUIRE(!trace.valid());

  trace.prev();
  BOOST_REQUIRE(!trace.valid());
}

BOOST_AUTO_TEST_SUITE_END()
