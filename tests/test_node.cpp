#define BOOST_TEST_MODULE GraphTest
#define GENERATE

#include <cstdio>
#include <vector>
#include <algorithm>
#include <random>

#include <boost/test/included/unit_test.hpp>

#include "test.hpp"

using std::string;

typedef std::vector<string> strings_t;
typedef std::vector<int> ints_t;


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

  ints_t indexes;
  indexes.resize(strings.size());
  for(int i = 0; i < (int)strings.size(); i++)
    indexes[i] = i;
  shuffle(indexes.begin(), indexes.end(), std::default_random_engine(42));

  for(ints_t::iterator i=indexes.begin(); i != indexes.end(); i++) {
    std::string& find(strings[*i]);

    std::cout << "find \"" << find << "\"";
    trace.find(find);
    BOOST_REQUIRE(trace.valid());
    BOOST_REQUIRE_EQUAL(trace.current_key, find);
    BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

    if (*i > 0) {
      trace.prev();
      BOOST_REQUIRE(trace.valid());
      BOOST_REQUIRE_EQUAL(trace.current_key, strings[*i-1]);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), strings[*i-1]);
    }

    if (*i < (int)strings.size()-1) {
      trace.find(find);
      BOOST_REQUIRE(trace.valid());
      BOOST_REQUIRE_EQUAL(trace.current_key, find);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

      trace.next();
      BOOST_REQUIRE(trace.valid());
      BOOST_REQUIRE_EQUAL(trace.current_key, strings[*i+1]);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), strings[*i+1]);
    }

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


void test_remove(Storage& storage, const char* title, const char* keys[], const char *to_remove) {
  strings_t strings;
  std::cout << "==========================================" << std::endl
            << "Test: " << title << std::endl
            << "==========================================" << std::endl;
  std::cout << "insert keys" << std::endl
            << "-----------" << std::endl;

  Trace trace(storage);

  for(int i = 0; keys[i]; i++) {
    trace.find(keys[i]);
    BOOST_REQUIRE(!trace.valid());
    trace.set_value(keys[i]);
    if (strcmp(keys[i], to_remove))
      strings.push_back(keys[i]);
  }

  std::string name(title);
  name += "_begin";

  check_graph(name.c_str(), storage);
  trace.find(to_remove);
  BOOST_REQUIRE(trace.valid());
  trace.remove();

  name = title;
  name += "_end";
  check_graph(name.c_str(), storage);

  test_movement(storage, strings);
}


BOOST_AUTO_TEST_SUITE(BasicCases)

BOOST_AUTO_TEST_CASE(insert_null) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_null", keys);
}


BOOST_AUTO_TEST_CASE(inser_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abc", NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_split) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abhij", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_table_split) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abhij", "abdef", "abklmn", NULL};
  test_insertion(storage, "insert_table_split", keys);
}

#if 0

BOOST_AUTO_TEST_CASE(insert_table_duplicates) {
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

BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abe", "abd", NULL};
  test_remove(storage, "remove_trie", keys, "abe");
}

BOOST_AUTO_TEST_CASE(remove_trie_purge_to_leaf) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abedefg", "abcdfgh", NULL};
  test_remove(storage, "remove_trie_purge_to_leaf", keys, "abedefg");
}

BOOST_AUTO_TEST_CASE(remove_trie_purge_to_compress) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abedefghi", "abcdfgh", NULL};
  test_remove(storage, "remove_trie_purge_to_compress", keys, "abedefghi");
}

BOOST_AUTO_TEST_CASE(remove_leaf_long) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abc", NULL};
  test_remove(storage, "remove_leaf_long", keys, "abcdefg");
}

BOOST_AUTO_TEST_CASE(remove_leaf_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abc", NULL};
  test_remove(storage, "remove_leaf_short", keys, "abc");
}

BOOST_AUTO_TEST_CASE(remove_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abcefgh", "ab", NULL};
  test_remove(storage, "remove_compress_short", keys, "ab");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(MemoryEdges)

BOOST_AUTO_TEST_CASE(big_compressed) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);

  std::string key1;
  std::string key2;
  for(int i = 0; i < 18; i++) {
    key1.append("abcdefghijklmn");
  }

  key2 = key1;
  key2.resize(170);
  key2.append("llllllllllllllllllllllllllllllllllllllllllllllllllllll");

  strings_t strings;
  strings.push_back(key1);
  strings.push_back(key2);

  Trace trace(storage);

  for(strings_t::iterator i=strings.begin(); i != strings.end(); i++) {
    trace.find(*i);
    BOOST_REQUIRE(!trace.valid());
    trace.set_value(*i);
  }

  check_graph("big_compressed", storage);
  test_movement(storage, strings);
}

BOOST_AUTO_TEST_CASE(insert_trie_grow) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"@", "M", "A", "B", "D", "E", "F", "G", "H",
                        "I", "J", "K", "C", "L", "N", "O", NULL};
  test_insertion(storage, "insert_trie_grow", keys);
}


BOOST_AUTO_TEST_CASE(remove_trie_shrink) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"@", "M", "A", "B", "D", "E", "F", "G", "H",
                        "I", "J", "K", "C", "L", "N", "O", NULL};

  Trace trace(storage);

  for(int i = 0; keys[i]; i++) {
    trace.find(keys[i]);
    BOOST_REQUIRE(!trace.valid());
    trace.set_value(keys[i]);
  }

  for(int i = 0; keys[i]; i++) {
    std::stringstream cstr;
    cstr << "remove_trie_shrink_" << i << "_" << keys[i];
    check_graph(cstr.str().c_str(), storage);
    trace.find(keys[i]);
    BOOST_REQUIRE(trace.valid());
    trace.remove();
  }
  check_graph("remove_trie_shrink_end", storage);
}


BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);

  Slice key("abcdefg");
  Trace trace(storage);

  trace.find(key);
  BOOST_REQUIRE(!trace.valid());
  trace.set_value(key);

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

  value.resize(MAX_POOL_SIZE-sizeof(ValueData));

  int pool = pool_index(value.size()+sizeof(ValueData));
  size_t used_count = storage.pools[pool].pool->used_nodes;
  size_t free_count = storage.pools[pool].pool->freed_nodes;

  trace.set_value(value);  // use value pool
  BOOST_REQUIRE_EQUAL(storage.pools[pool].pool->used_nodes, used_count+1);

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  trace.set_value(key);

  BOOST_REQUIRE_EQUAL(storage.pools[pool].pool->used_nodes, used_count);
  BOOST_REQUIRE_EQUAL(storage.pools[pool].pool->freed_nodes, free_count+1);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
