#define BOOST_TEST_MODULE ModifyTest
#define GENERATE

#include <cstdio>
#include <vector>
#include <algorithm>
#include <random>

#include <boost/test/included/unit_test.hpp>

#include "test.hpp"

using std::string;

typedef std::vector<string> strings_t;


void test_insert_null(Storage& storage) {
  check_graph("empty", storage);
  insert(storage, Slice("abcdefg"), "insert_null");
}

void test_insert_leaf_split(Storage& storage) {
  insert(storage, Slice("abhij"), "insert_leaf_split");
}

void test_insert_leaf_short(Storage& storage) {
  insert(storage, Slice("ab"), "insert_leaf_short");
}

void test_insert_leaf_long(Storage& storage) {
  insert(storage, Slice("abcdefghi"), "insert_leaf_long");
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


void test_movement(Storage& storage, strings_t& strings) {
  std::sort(strings.begin(), strings.end());

  Trace trace(storage);

  std::cout << std::endl
            << "iter forward" << std::endl
            << "------------" << std::endl;
  trace.first();
  for(strings_t::iterator i=strings.begin(); i != strings.end(); i++, trace.next()) {
    std::cout << "find \"" << *i << "\"";
    BOOST_REQUIRE(trace.valid());
    BOOST_REQUIRE_EQUAL(trace.current_key, *i);
    std::cout << " ok" << std::endl;
  }
  BOOST_REQUIRE(!trace.valid());

  std::cout << std::endl
            << "iter backward" << std::endl
            << "-------------" << std::endl;
  trace.last();
  for(strings_t::reverse_iterator i=strings.rbegin(); i != strings.rend(); i++, trace.prev()) {
    std::cout << "find \"" << *i << "\"";
    BOOST_REQUIRE(trace.valid());
    BOOST_REQUIRE_EQUAL(trace.current_key, *i);
    std::cout << " ok" << std::endl;
  }
  BOOST_REQUIRE(!trace.valid());

  std::cout << std::endl
            << "find" << std::endl
            << "----" << std::endl;
  shuffle(strings.begin(), strings.end(), std::default_random_engine(42));
  for(strings_t::iterator i=strings.begin(); i != strings.end(); i++) {
    std::cout << "find \"" << *i << "\"";
    trace.find(*i);
    BOOST_REQUIRE(trace.valid());
    BOOST_REQUIRE_EQUAL(trace.current_key, *i);
    BOOST_REQUIRE_EQUAL(trace.get_value().string(), *i);
    std::cout << " ok" << std::endl;
  }

  std::cout << std::endl
            << "missing" << std::endl
            << "-------" << std::endl;
  for(strings_t::iterator i=strings.begin(); i != strings.end(); i++) {
    std::string missing(*i);
    missing.append(".");
    std::cout << "find \"" << missing << "\"";
    trace.find(missing);
    BOOST_REQUIRE(!trace.valid());
    std::cout << " ok (not found)" << std::endl;
  }
  std::cout << std::endl << std::endl;
}

void test_insertion(Storage& storage, const char* title, const char* keys[]) {
  strings_t strings;

  std::cout << "==========================================" << std::endl
            << "Test: " << title << std::endl
            << "==========================================" << std::endl;
  std::cout << "insert keys" << std::endl
            << "-----------" << std::endl;
  for(int i = 0; keys[i]; i++) {
    std::stringstream cstr;
    cstr << title << "_" << i << "_" << keys[i];
    std::cout << "insert " << keys[i] << std::endl;
    insert(storage, keys[i], cstr.str().c_str());
    strings.push_back(keys[i]);
  }
  test_movement(storage, strings);
}

BOOST_AUTO_TEST_SUITE(BasicCases)

BOOST_AUTO_TEST_CASE(insert_null) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_null", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_split) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abhij", NULL};
  test_insertion(storage, "insert_leaf_split", keys);
}

BOOST_AUTO_TEST_CASE(inser_leaf_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abc", NULL};
  test_insertion(storage, "insert_leaf_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_long) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abcdefg", NULL};
  test_insertion(storage, "insert_leaf_long", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_split) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abcefgh", "abdefgh", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abcefgh", "ab", "b", NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_extend) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"ab", "abcdefg", "abcefgh", NULL};
  test_insertion(storage, "insert_leaf_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_split_a) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abd", "abe", NULL};
  test_insertion(storage, "insert_trie_split_a", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_split_b) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abe", "abd", NULL};
  test_insertion(storage, "insert_trie_split_b", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abe", "ab", NULL};
  test_insertion(storage, "insert_trie_short", keys);
}

/*
BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abe", "abd", NULL};
  test_insertion(storage, "insert_trie_split_b", keys);
}
*/



BOOST_AUTO_TEST_SUITE_END()

#if 0


BOOST_AUTO_TEST_SUITE(ModifyCompressedNode)


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
  Trace trace(storage, storage.master);
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

  BOOST_REQUIRE_EQUAL(storage.pools[MAIN_POOL_COUNT].pool->used_nodes, 1);

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  trace.set_value(key);

  BOOST_REQUIRE_EQUAL(storage.pools[MAIN_POOL_COUNT].pool->used_nodes, 0);
  BOOST_REQUIRE_EQUAL(storage.pools[MAIN_POOL_COUNT].pool->freed_nodes, 1);
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
  Trace trace(storage, storage.master);

  trace.first();
  BOOST_REQUIRE(!trace.valid());

  trace.next();
  BOOST_REQUIRE(!trace.valid());
}

BOOST_AUTO_TEST_CASE(move_forward) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  fill_db(storage);

  Trace trace(storage, storage.master);

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
#endif
