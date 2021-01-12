#include <cstdio>
#include <vector>
#include <algorithm>
#include <random>

#include <boost/test/included/unit_test.hpp>

#ifndef CMPFILES
#define CMPFILES "."
#endif


#define AREA_COUNT 100
#include "../src/trace.hpp"
#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;


struct Preparation {
  Preparation() {
    std::remove(TEST_FILE);
  }

  ~Preparation() {
    std::remove(TEST_FILE);
    std::cout << "remove " << TEST_FILE << std::endl;
  }
};

namespace leaves {
// defined in node.cpp
void dump_node(std::ostream& out, any_ptr ptr, Storage* storage);
}


inline void dump_graph(const char* output, Storage& storage) {
  std::ofstream out(output);
  dump_node(out, storage.header->root, &storage);
}


inline void compare_graph(const char* input, Storage& storage) {
  std::stringstream cstr;
  dump_node(cstr, storage.header->root, &storage);

  std::ifstream in(input, std::ios_base::in | std::ios_base::binary);
  std::string cmp((std::istreambuf_iterator<char>(in)), (std::istreambuf_iterator<char>()));

  std::cout << "check graph " << input << std::endl;
  if (cmp != cstr.str()) {
    std::cerr << "error " << input << std::endl;
    std::cerr << "===========================" << std::endl;
    std::cerr << cmp << std::endl;
    std::cerr << "---------------------------" << std::endl;
    std::cerr << cstr.str() << std::endl;
    std::cerr << "===========================" << std::endl;
    dump_graph("error.yaml", storage);
    BOOST_REQUIRE(false);
  }
}


inline void check_graph(const char* name, Storage& storage) {
  std::string path(CMPFILES);
  path.append(name);
  path.append(".yaml");

#ifdef GENERATE
  std::cout << "generate graph " << name << std::endl;
  dump_graph(path.c_str(), storage);
#else
  compare_graph(path.c_str(), storage);
#endif
}


inline void insert(Storage& storage, const Slice& key, const char* test_name) {
  Trace trace(storage);
  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(!trace.valid());

  trace.set_value(key);
  check_graph(test_name, storage);
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), key.string());
}


using std::string;

typedef std::vector<string> strings_t;
typedef std::vector<int> ints_t;


inline void test_movement(Storage& storage, strings_t& strings) {
  std::sort(strings.begin(), strings.end());

  Trace trace(storage);

  std::cout << std::endl
            << "iter forward" << std::endl
            << "------------" << std::endl;
  trace.first();
  for(strings_t::iterator i=strings.begin(); i != strings.end(); i++, trace.next()) {
    std::cout << "find \"" << *i << "\"";
    BOOST_REQUIRE(trace.valid());
    BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), *i);
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
    BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), *i);
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
    BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), find);
    BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

    if (*i > 0) {
      trace.prev();
      BOOST_REQUIRE(trace.valid());
      BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), strings[*i-1]);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), strings[*i-1]);
    }

    if (*i < (int)strings.size()-1) {
      trace.find(find);
      BOOST_REQUIRE(trace.valid());
      BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), find);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

      trace.next();
      BOOST_REQUIRE(trace.valid());
      BOOST_REQUIRE_EQUAL(trace.current_key.slice().string(), strings[*i+1]);
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


inline void test_insertion(Storage& storage, const char* title, const char* keys[]) {
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
    std::string test_name(cstr.str());
    test_name.resize(30);
    insert(storage, keys[i], test_name.c_str());
    strings.push_back(keys[i]);
  }
  test_movement(storage, strings);
}


inline void test_remove(
    Storage& storage, const char* title, const char* keys[], const char* to_remove[]) {
  strings_t strings;
  std::cout << "==========================================" << std::endl
            << "Test: " << title << std::endl
            << "==========================================" << std::endl;
  std::cout << "insert keys" << std::endl
            << "-----------" << std::endl;

  Trace trace(storage);

  for(int i = 0; keys[i]; i++) {
    std::cout << "insert " << keys[i] << std::endl;
    trace.find(keys[i]);
    BOOST_REQUIRE(!trace.valid());
    trace.set_value(keys[i]);
    strings.push_back(keys[i]);
  }

  std::string name(title);
  name += "_begin";
  check_graph(name.c_str(), storage);

  for(int i = 0; to_remove[i]; i++) {
    std::cout << "remove " << to_remove[i] << std::endl;
    trace.find(to_remove[i]);
    BOOST_REQUIRE(trace.valid());
    trace.remove();

    std::stringstream cstr;
    cstr << title << "_remove_" << i << "_" << to_remove[i];
    check_graph(cstr.str().c_str(), storage);

    for(strings_t::iterator iter=strings.begin(); iter != strings.end(); iter++) {
      if (*iter == to_remove[i]) {
        strings.erase(iter);
        break;
      }
    }
  }

  test_movement(storage, strings);
}


Options TEST_OPTIONS(1024*32, 1);
