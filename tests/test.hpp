#define GENERATE

#include <algorithm>
#include <boost/test/included/unit_test.hpp>
#include <cstdio>
#include <random>
#include <vector>

#include "../src/memory.hpp"
#include "../src/trace.hpp"
#include "testpoints.hpp"

#ifndef CMPFILES
#define CMPFILES "./"
#endif

#define TEST_FILE "test.lvs"

using namespace leaves;

struct Preparation {
  Preparation() { std::remove(TEST_FILE); }

  ~Preparation() {
    std::remove(TEST_FILE);
    std::cout << "remove " << TEST_FILE << std::endl;
  }
};

namespace leaves {
// defined in node.cpp

void dump_branch(std::ostream& out, offset_ptr offset,  DBMemory* storage);
}  // namespace leaves

inline void dump_graph(const char* output, DBMemory& storage) {
  std::ofstream out(output);
  offset_ptr root = storage.active_txn()->root;
  dump_branch(out, root, &storage);
}

inline void compare_graph(const char* input, DBMemory& storage) {
  std::stringstream cstr;
  offset_ptr root = storage.active_txn()->root;
  dump_branch(cstr, root, &storage);

  std::ifstream in(input, std::ios_base::in | std::ios_base::binary);
  std::string cmp((std::istreambuf_iterator<char>(in)),
                  (std::istreambuf_iterator<char>()));

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

inline void check_graph(const char* name, DBMemory& storage) {
  std::string path(CMPFILES);
  path.append(name);
  path.append(".yaml");
  std::replace(path.begin(), path.end(), ' ', '_');

#ifdef GENERATE
  std::cout << "generate graph " << name << std::endl;
  dump_graph(path.c_str(), storage);
#else
  compare_graph(path.c_str(), storage);
#endif
}

inline void insert(DBMemory& storage, const char* test_name, const Slice& key,
                   const Slice& value) {
  Trace trace(storage);
  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(!trace.is_valid());

  trace.set_value(value);
  BOOST_REQUIRE(trace.is_valid());
  trace.commit();
  check_graph(test_name, storage);
  BOOST_REQUIRE(trace.is_valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, key.string());
}

inline void insert(DBMemory& storage, const char* test_name, const Slice& key) {
  insert(storage, test_name, key, key);
}

using std::string;

typedef std::vector<string> strings_t;
typedef std::vector<int> ints_t;

#ifdef MOVEMENT
inline void test_movement(Storage& storage, strings_t& strings) {
  std::sort(strings.begin(), strings.end());

  Trace trace(storage);

  std::cout << std::endl
            << "iter forward" << std::endl
            << "------------" << std::endl;
  trace.first();
  for (strings_t::iterator i = strings.begin(); i != strings.end();
       i++, trace.next()) {
    std::cout << "find \"" << *i << "\"";
    BOOST_REQUIRE(trace.isvalid());
    BOOST_REQUIRE_EQUAL(trace.current_key, *i);
    std::cout << " ok" << std::endl;
  }
  BOOST_REQUIRE(!trace.isvalid());

  std::cout << std::endl
            << "iter backward" << std::endl
            << "-------------" << std::endl;
  trace.last();
  for (strings_t::reverse_iterator i = strings.rbegin(); i != strings.rend();
       i++, trace.prev()) {
    std::cout << "find \"" << *i << "\"";
    BOOST_REQUIRE(trace.isvalid());
    BOOST_REQUIRE_EQUAL(trace.current_key, *i);
    std::cout << " ok" << std::endl;
  }
  BOOST_REQUIRE(!trace.isvalid());

  std::cout << std::endl << "find" << std::endl << "----" << std::endl;

  ints_t indexes;
  indexes.resize(strings.size());
  for (int i = 0; i < (int)strings.size(); i++) indexes[i] = i;
  shuffle(indexes.begin(), indexes.end(), std::default_random_engine(42));

  for (ints_t::iterator i = indexes.begin(); i != indexes.end(); i++) {
    std::string find(strings[*i]);

    std::cout << "find \"" << find << "\"";
    trace.find(find);
    BOOST_REQUIRE(trace.isvalid());
    BOOST_REQUIRE_EQUAL(trace.current_key, find);
    BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

    if (*i > 0) {
      trace.prev();
      BOOST_REQUIRE(trace.isvalid());
      BOOST_REQUIRE_EQUAL(trace.current_key, strings[*i - 1]);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), strings[*i - 1]);
    }

    if (*i < (int)strings.size() - 1) {
      trace.find(find);
      BOOST_REQUIRE(trace.isvalid());
      BOOST_REQUIRE_EQUAL(trace.current_key, find);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

      trace.next();
      BOOST_REQUIRE(trace.isvalid());
      BOOST_REQUIRE_EQUAL(trace.current_key, strings[*i + 1]);
      BOOST_REQUIRE_EQUAL(trace.get_value().string(), strings[*i + 1]);
    }

    std::cout << std::endl;
    // set cursor after find in a non valid position
    find.push_back('!');
    trace.find(find);
    BOOST_REQUIRE(!trace.isvalid());
    trace.prev();
    find = strings[*i];
    std::string cmp1 = trace.current_key;
    std::cout << "before cmp " << cmp1 << " == " << find << std::endl;
    BOOST_REQUIRE_EQUAL(trace.current_key, find);
    BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

    // set cursor before find in a non valid position
    find.back()--;
    find.push_back('!');
    trace.find(find);
    BOOST_REQUIRE(!trace.isvalid());
    trace.next();
    find = strings[*i];
    cmp1 = trace.current_key;
    std::cout << "after cmp " << cmp1 << " == " << find << std::endl;

    BOOST_REQUIRE_EQUAL(trace.current_key, find);
    BOOST_REQUIRE_EQUAL(trace.get_value().string(), find);

    std::cout << " ok" << std::endl;
  }

  std::cout << std::endl << "missing" << std::endl << "-------" << std::endl;
  for (strings_t::iterator i = strings.begin(); i != strings.end(); i++) {
    std::string missing(*i);
    missing.append(".");
    std::cout << "find \"" << missing << "\"";
    trace.find(missing);
    BOOST_REQUIRE(!trace.isvalid());
    std::cout << " ok (not found)" << std::endl;
  }
  std::cout << std::endl << std::endl;
}
#endif

inline void test_insertion(DBMemory& storage, const char* title,
                           const char* keys[]) {
  strings_t strings;
  std::cout << "==========================================" << std::endl
            << "Test: " << title << std::endl
            << "==========================================" << std::endl;
  std::cout << "insert keys" << std::endl << "-----------" << std::endl;
  for (int i = 0; keys[i]; i++) {
    std::stringstream cstr;
    cstr << title << "_" << i << "_" << keys[i];
    std::cout << "insert " << i << ": " << keys[i] << std::endl;
    std::string test_name(cstr.str());
    test_name.resize(30);
    insert(storage, test_name.c_str(), keys[i]);
    strings.push_back(keys[i]);
  }
#ifdef MOVEMENT
  test_movement(storage, strings);
#endif
}

inline void test_remove(DBMemory& storage, const char* title, const char* keys[],
                        const char* to_remove[]) {
  strings_t strings;
  std::cout << "==========================================" << std::endl
            << "Test: " << title << std::endl
            << "==========================================" << std::endl;
  std::cout << "insert keys" << std::endl << "-----------" << std::endl;

  Trace trace(storage);

  for (int i = 0; keys[i]; i++) {
    std::cout << "insert " << keys[i] << std::endl;
    trace.find(keys[i]);
    BOOST_REQUIRE(!trace.is_valid());
    trace.set_value(keys[i]);
    strings.push_back(keys[i]);
  }

  std::string name(title);
  name += "_begin";
  check_graph(name.c_str(), storage);

  for (int i = 0; to_remove[i]; i++) {
    std::cout << "remove " << to_remove[i] << std::endl;
    trace.find(to_remove[i]);
    BOOST_REQUIRE(trace.is_valid());
    trace.remove();

    std::stringstream cstr;
    cstr << title << "_remove_" << i << "_" << to_remove[i];
    check_graph(cstr.str().c_str(), storage);

    for (strings_t::iterator iter = strings.begin(); iter != strings.end();
         iter++) {
      if (*iter == to_remove[i]) {
        strings.erase(iter);
        break;
      }
    }
  }
#ifdef MOVEMENT
  test_movement(storage, strings);
#endif
}
