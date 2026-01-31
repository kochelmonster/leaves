// #define GENERATE

#include <algorithm>
#include <boost/test/included/unit_test.hpp>
#include <cstdio>
#include <iomanip>
#include <random>
#include <vector>

#include "../include/leaves/intern/db/_check.hpp"
#include "../include/leaves/mmap.hpp"

#ifndef CMPFILES
#define CMPFILES "./"
#endif

#define MOVEMENT

#define TEST_FILE "test.lvs"

using namespace leaves;

struct Preparation {
  Preparation() { std::remove(TEST_FILE); }

  ~Preparation() {
    std::remove(TEST_FILE);
    std::cout << "remove " << TEST_FILE << std::endl;
  }
};

// defined in node.cpp

template <typename T>
inline void dump_graph(const char* output, T& storage) {
  std::ofstream out(output);
  _Dumper<T>(storage, storage._internal()->txn()->root, false).dump(out);
}

template <typename T>
inline void compare_graph(const char* input, T& storage) {
  std::stringstream cstr;
  _Dumper<T>(storage, storage._internal()->txn()->root, false).dump(cstr);

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

template <typename T>
inline void check_graph(const char* name, T& storage, const char* dbname = "test") {
  std::string truncated_name(name);
  if (truncated_name.length() > 32) {
    truncated_name = truncated_name.substr(0, 32);
  }
  std::string path(CMPFILES);
  path.append(truncated_name);
  path.append(".yaml");
  std::replace(path.begin(), path.end(), ' ', '_');

  auto db = (*storage)[dbname];
#ifdef GENERATE
  std::cout << "generate graph " << name << std::endl;
  dump_graph(path.c_str(), db);
#else
  compare_graph(path.c_str(), db);
#endif
}

void cmp_value(const Slice& value, const std::string& find) {
  std::string value_str(value.data(), std::min(find.size(), value.size()));
  if (value_str != find) {
    std::cerr << "\n=== VALUE MISMATCH ===" << std::endl;
    std::cerr << "value.size()=" << value.size()
              << " find.size()=" << find.size()
              << " value_str.size()=" << value_str.size() << std::endl;
    // Find first difference
    size_t diff_pos = 0;
    while (diff_pos < value_str.size() && diff_pos < find.size() &&
           value_str[diff_pos] == find[diff_pos]) {
      diff_pos++;
    }
    std::cerr << "First difference at position " << diff_pos << std::endl;
    // Show 20 bytes around the difference
    size_t start = (diff_pos > 10) ? diff_pos - 10 : 0;
    size_t end =
        std::min(diff_pos + 10, std::min(value_str.size(), find.size()));
    std::cerr << "value_str[" << start << ".." << end << "]: ";
    for (size_t i = start; i < end && i < value_str.size(); i++) {
      std::cerr << std::hex << std::setw(2) << std::setfill('0')
                << (int)(unsigned char)value_str[i] << " ";
    }
    std::cerr << std::endl << "find[" << start << ".." << end << "]:      ";
    for (size_t i = start; i < end && i < find.size(); i++) {
      std::cerr << std::hex << std::setw(2) << std::setfill('0')
                << (int)(unsigned char)find[i] << " ";
    }
    std::cerr << std::dec << std::endl;
  }
  BOOST_REQUIRE_EQUAL(value_str, find);
}

template <typename T>
inline void insert(T& storage, const char* test_name, const Slice& key,
                   const Slice& value) {
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  // std::cout << "insert " << test_name << std::endl;
  cursor.find(key);
  BOOST_REQUIRE(!cursor.is_valid());

  cursor.value(value);
  BOOST_REQUIRE(cursor.is_valid());
  cursor.commit();
  cmp_value(cursor.value(), value.string());

  check_graph(test_name, storage);
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key(), key.string());
}

template <typename T>
inline void insert(T& storage, const char* test_name, const Slice& key) {
  insert(storage, test_name, key, key);
}

using std::string;

typedef std::vector<string> strings_t;
typedef std::vector<int> ints_t;

#ifdef MOVEMENT
template <typename T>
inline void test_movement(T& storage, strings_t& strings) {
  std::sort(strings.begin(), strings.end());

  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  std::cout << std::endl
            << "iter forward" << std::endl
            << "------------" << std::endl;
  cursor.first();
  strings_t::iterator i = strings.begin();
  for (; i != strings.end(); i++, cursor.next()) {
    std::cout << "find \"" << i->substr(0, 20) << "\"";
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_REQUIRE_EQUAL(cursor.key(), Slice(*i));
    cmp_value(cursor.value(), *i);
    std::cout << " ok" << std::endl;
  }
  BOOST_REQUIRE(i == strings.end());
  BOOST_REQUIRE(!cursor.is_valid());

  std::cout << std::endl
            << "iter backward" << std::endl
            << "-------------" << std::endl;
  cursor.last();
  for (strings_t::reverse_iterator i = strings.rbegin(); i != strings.rend();
       i++, cursor.prev()) {
    std::cout << "find \"" << i->substr(0, 20) << "\"";
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_REQUIRE_EQUAL(cursor.key(), *i);
    cmp_value(cursor.value(), *i);
    std::cout << " ok" << std::endl;
  }
  BOOST_REQUIRE(!cursor.is_valid());

  std::cout << std::endl << "find" << std::endl << "----" << std::endl;

  ints_t indexes;
  indexes.resize(strings.size());
  for (int i = 0; i < (int)strings.size(); i++) indexes[i] = i;
  shuffle(indexes.begin(), indexes.end(), std::default_random_engine(42));

  for (ints_t::iterator i = indexes.begin(); i != indexes.end(); i++) {
    std::string find(strings[*i]);

    std::cout << "find \"" << find.substr(0, 20) << "\"";

    cursor.find(find);
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_REQUIRE_EQUAL(cursor.key(), find);
    cmp_value(cursor.value(), find);

    if (*i > 0) {
      cursor.prev();
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.key(), strings[*i - 1]);
      cmp_value(cursor.value(), strings[*i - 1]);
    }

    if (*i < (int)strings.size() - 1) {
      cursor.find(find);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.key(), find);
      cmp_value(cursor.value(), find);

      cursor.next();
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.key(), strings[*i + 1]);
      cmp_value(cursor.value(), strings[*i + 1]);
    }

    std::cout << std::endl;
    // set cursor after find in a non valid position
    find.push_back('!');
    cursor.find(find);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.prev();
    find = strings[*i];
    std::string cmp1(cursor.key().data(), cursor.key().size());
    std::cout << "before cmp " << cmp1 << " == " << find.substr(0, 20)
              << std::endl;
    BOOST_REQUIRE_EQUAL(cursor.key(), find);
    cmp_value(cursor.value(), find);

    // set cursor before find in a non valid position
    find.back()--;
    find.push_back('!');
    cursor.find(find);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.next();
    find = strings[*i];
    cmp1.assign(cursor.key().data(), cursor.key().size());
    std::cout << "after cmp " << cmp1.substr(0, 20)
              << " == " << find.substr(0, 20) << std::endl;

    BOOST_REQUIRE_EQUAL(cursor.key(), find);
    cmp_value(cursor.value(), find);

    std::cout << "ok" << std::endl;
  }

  std::cout << std::endl << "missing" << std::endl << "-------" << std::endl;
  for (strings_t::iterator i = strings.begin(); i != strings.end(); i++) {
    std::string missing(*i);
    missing.append(".");
    std::cout << "find \"" << missing << "\"";
    cursor.find(missing);
    BOOST_REQUIRE(!cursor.is_valid());
    std::cout << " ok (not found)" << std::endl;
  }
  std::cout << std::endl << std::endl;
}
#endif

template <typename T>
inline void test_insertion(T& storage, const char* title, const char* keys[],
                           int value_fill = 0, int key_fill = 0) {
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
    std::string key(keys[i]);
    key.append(key_fill, '0');
    std::string value(key);
    value.append(value_fill, '-');
    insert(storage, test_name.c_str(), Slice(key), Slice(value));
    strings.push_back(key);
  }
#ifdef MOVEMENT
  test_movement(storage, strings);
#endif
}

template <typename T>
inline void test_remove(T& storage, const char* title, const char* keys[],
                        const char* to_remove[]) {
  strings_t strings;
  std::cout << "==========================================" << std::endl
            << "Test: " << title << std::endl
            << "==========================================" << std::endl;
  std::cout << "insert keys" << std::endl << "-----------" << std::endl;

  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  for (int i = 0; keys[i]; i++) {
    std::cout << "insert " << keys[i] << std::endl;
    cursor.find(keys[i]);
    BOOST_REQUIRE(!cursor.is_valid());
    cursor.value(keys[i]);
    strings.push_back(keys[i]);
  }
  cursor.commit();

  std::string name(title);
  name += "_begin";
  check_graph(name.c_str(), storage);

  for (int i = 0; to_remove[i]; i++) {
    std::cout << "remove " << to_remove[i] << std::endl;
    cursor.find(to_remove[i]);
    cursor.next();
    std::string next;
    if (cursor.is_valid()) next = cursor.key().string();

    cursor.find(to_remove[i]);
    BOOST_REQUIRE(cursor.is_valid());
    cursor.remove();
    cursor.commit();

    // the position is on the next item after the removed one
    if (next.size()) {
      BOOST_CHECK(cursor.is_valid());
      BOOST_CHECK_EQUAL(cursor.key().string(), next);
    } else
      BOOST_CHECK(!cursor.is_valid());

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
