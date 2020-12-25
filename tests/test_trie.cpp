#define BOOST_TEST_NO_MAIN
//#define GENERATE
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <vector>
#define BOOST_TEST_MODULE TrieTest
#include "../src/node.hpp"
#include <boost/test/included/unit_test.hpp>

using namespace larch_leaves;

#ifndef CMPFILES
#define CMPFILES "."
#endif

struct TestDatabase {
  NodeStorageInHeap nodes;
  Trace trace;

  TestDatabase() : trace(nodes, nodes) { reinit(); }

  void reinit() {
    assert(nodes._free_pages.size() == 0);
    assert(nodes._pages.empty());
    assert(trace.size() == 0);
    PageRef page(nodes.free_page(1));
    TempTrie root;
    NodeRef rroot(page, page.new_node(root.size()));
    memcpy(rroot.node, root.node(), rroot.size());
    page.page->entry_points[0] = 1;
    trace.push_root(rroot);
  }

  bool is_valid() const { return trace.is_valid(); }

  void find(const Slice &key) { trace.find(key); }

  void first() { trace.first(); }

  void last() { trace.last(); }

  void next() { trace.next(); }

  void prev() { trace.prev(); }

  Slice key() { return Slice(trace.key); }

  Slice value() {
    trace.check_valid();
    return trace.current().data();
  }

  void set_value(const Slice &value) { trace.set_leaf(TempLeaf(value)); }

  void remove() {
    if (trace.remove())
      reinit();
  }

  void dump(std::ostream &out) {
#ifdef DEBUG
    out << "state:" << std::endl;
    typedef std::vector<NodeStorageInHeap::_page_ptr>::iterator iter_t;
    iter_t i = nodes._pages.begin();
    for (int j = 0; i != nodes._pages.end(); i++, j++) {
      if (*i) {
        PageRef(i->get(), j, j).dump(out);
      }
    }
    out << "---" << std::endl;
#endif
  }
};

typedef std::unique_ptr<TestDatabase> db_t;
std::stringstream tp_output;

namespace larch_leaves {

void testpoint(const char *str) {
  tp_output << "TESTPOINT: " << str << std::endl;
}
}

void prepare_testpoint_output() { tp_output.str(""); }

#ifdef GENERATE

std::ostream &out(std::cout);

#undef BOOST_REQUIRE
#define BOOST_REQUIRE(x) assert(x)

void check_dump(const char *fname, db_t &db) {
  std::stringstream cstr;
  db->dump(cstr);
  std::cout << cstr.str();

  std::string path(CMPFILES);
  path.append(fname);
  std::ofstream out(path.c_str());
  out << cstr.str();
}

void check_testpoints(size_t case_count, const char *testpoints[]) {
  std::cerr << "check case:" << tp_output.str().size() << std::endl;
  std::cerr << tp_output.str() << std::endl;
}

#else

std::stringstream dumy;
std::ostream &out(dumy);

void check_dump(const char *fname, db_t &db) {
  std::stringstream cstr;
  db->dump(cstr);

  std::string path(CMPFILES);
  path.append(fname);

  std::string compare;
  std::ifstream in(path.c_str(), std::ios_base::in | std::ios_base::binary);

  if (!in.is_open()) {
    std::cerr << "could not open " << path << std::endl;
    return;
  }

  in.seekg(0, std::ios::end);
  size_t size = in.tellg();
  // std::cerr << "--size: " << size << " in open? " << in.is_open() <<
  // std::endl;
  compare.assign(size, 0);
  in.seekg(0);
  in.read(&compare[0], size);

  BOOST_REQUIRE_EQUAL(cstr.str(), compare);
}

void check_testpoints(size_t case_count, const char *testpoints[]) {
  std::vector<int> done;
  done.resize(case_count);
  std::string output(tp_output.str());
  std::stringstream in(output);

  while (in) {
    std::string sub;
    in >> sub;
    for (size_t i = 0; i < case_count; i++) {
      if (sub == testpoints[i])
        done[i] = 1;
    }

    // std::cerr << "sub: " << sub << std::endl;
  }

  for (size_t i = 0; i < case_count; i++) {
    if (!done[i]) {
      std::cerr << "missing case: " << testpoints[i] << " in " << std::endl
                << output << std::endl;
      BOOST_REQUIRE(false);
      break;
    }
  }

  // std::cerr << tp_output.str() << std::endl;
}

#endif

// Key Creators

std::string sequence(size_t size, size_t start = 0) {
  std::string result;
  for (size_t i = 0; i < size; i++)
    result.push_back((char)start++);

  return result;
}

std::string number(size_t number, size_t size = 0) {
  std::string result;
  std::stringstream f;
  f << std::setw(size) << std::setfill('0') << number;
  result = f.str();
  for (size_t i = 0; i < result.size(); i++)
    result[i] -= '0';

  return result;
}

template <typename content_t>
std::string value(content_t content, size_t size = 0) {
  std::stringstream f;
  f << "value-" << content;
  std::string result(f.str());
  while (result.size() < size)
    result.push_back('-');

  return result;
}
struct TestLeaf {
  void test_LeafAdd0() {
    db_t db(new TestDatabase);

    db->find(number(1));
    db->set_value(value(1));

    check_dump("LeafAdd0-1", db);
    prepare_testpoint_output();

    db->find(number(123));
    db->set_value(value(2));

    check_dump("LeafAdd0-2", db);

    const char *testpoints[] = {"LeafAdd0"};
    check_testpoints(1, testpoints);
  }

  void test_LeafAdd1() {
    db_t db(new TestDatabase);

    db->find(number(1));
    db->set_value(value(1));

    check_dump("LeafAdd1-1", db);
    prepare_testpoint_output();

    db->find(number(1234));
    db->set_value(value(2));

    check_dump("LeafAdd1-2", db);

    const char *testpoints[] = {"LeafAdd0"};
    check_testpoints(1, testpoints);
  }

  void test_LeafAdd2() {
    db_t db(new TestDatabase);

    db->find(number(1));
    db->set_value(value(1));

    check_dump("LeafAdd2-1", db);
    prepare_testpoint_output();

    db->find(number(12));
    db->set_value(value(2));

    check_dump("LeafAdd2-2", db);

    const char *testpoints[] = {"LeafAdd0"};
    check_testpoints(1, testpoints);
  }
};

TestLeaf test_leaf;
struct TestCompress {
  void test_CompressAddNew() {
    db_t db(new TestDatabase);

    db->find(number(1));
    db->set_value(value(1));

    check_dump("CompressAddNew-1", db);
    prepare_testpoint_output();

    db->find(number(12345));
    db->set_value(value(2));

    check_dump("CompressAddNew-2", db);

    const char *testpoints[] = {"CompressAddNew"};
    check_testpoints(1, testpoints);
  }
  void test_CompressAdd0_CompressReinsert1() {
    db_t db(new TestDatabase);

    db->find(number(123));
    db->set_value(value(1));

    check_dump("CompressAdd0_CompressReinsert1_1-1", db);
    prepare_testpoint_output();

    db->find(number(1345));
    db->set_value(value(2));

    check_dump("CompressAdd0_CompressReinsert1_1", db);

    const char *testpoints[] = {"CompressAdd0", "CompressReinsert1"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd0_CompressReinsert2() {
    db_t db(new TestDatabase);

    db->find(number(1234));
    db->set_value(value(1));

    check_dump("CompressAdd0_CompressReinsert2-1", db);
    prepare_testpoint_output();

    db->find(number(1345));
    db->set_value(value(2));

    check_dump("CompressAdd0_CompressReinsert2-2", db);

    const char *testpoints[] = {"CompressAdd0", "CompressReinsert2"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd1_CompressReinsert0() {
    db_t db(new TestDatabase);

    db->find(number(123));
    db->set_value(value(1));

    check_dump("CompressAdd1_CompressReinsert0-1", db);
    prepare_testpoint_output();

    db->find(number(1245));
    db->set_value(value(2));

    check_dump("CompressAdd1_CompressReinsert0-2", db);

    const char *testpoints[] = {"CompressAdd1", "CompressReinsert0"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd1_CompressReinsert1() {
    db_t db(new TestDatabase);

    db->find(number(1234));
    db->set_value(value(1));

    check_dump("CompressAdd1_CompressReinsert1-1", db);
    prepare_testpoint_output();

    db->find(number(1245));
    db->set_value(value(2));

    check_dump("CompressAdd1_CompressReinsert1-2", db);

    const char *testpoints[] = {"CompressAdd1", "CompressReinsert1"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd1_CompressReinsert2() {
    db_t db(new TestDatabase);

    db->find(number(12345));
    db->set_value(value(1));

    check_dump("CompressAdd1_CompressReinsert2-1", db);
    prepare_testpoint_output();

    db->find(number(1245));
    db->set_value(value(2));

    check_dump("CompressAdd1_CompressReinsert2-2", db);

    const char *testpoints[] = {"CompressAdd1", "CompressReinsert1"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd2_CompressReinsert0() {
    db_t db(new TestDatabase);

    db->find(number(1234));
    db->set_value(value(1));

    check_dump("CompressAdd2_CompressReinsert0-1", db);
    prepare_testpoint_output();

    db->find(number(1235));
    db->set_value(value(2));

    check_dump("CompressAdd2_CompressReinsert0-2", db);

    const char *testpoints[] = {"CompressAdd1", "CompressReinsert0"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd2_CompressReinsert1() {
    db_t db(new TestDatabase);

    db->find(number(12345));
    db->set_value(value(1));

    check_dump("CompressAdd2_CompressReinsert1-1", db);
    prepare_testpoint_output();

    db->find(number(1235));
    db->set_value(value(2));

    check_dump("CompressAdd2_CompressReinsert1-2", db);

    const char *testpoints[] = {"CompressAdd1", "CompressReinsert1"};
    check_testpoints(2, testpoints);
  }
  void test_CompressAdd2_CompressReinsert2() {
    db_t db(new TestDatabase);

    db->find(number(123456));
    db->set_value(value(1));

    check_dump("CompressAdd2_CompressReinsert2-1", db);
    prepare_testpoint_output();

    db->find(number(1235));
    db->set_value(value(2));

    check_dump("CompressAdd2_CompressReinsert2-2", db);

    const char *testpoints[] = {"CompressAdd1", "CompressReinsert1"};
    check_testpoints(2, testpoints);
  }

  std::string sep_number(size_t i) {
    std::string sep(5, 60);
    std::string n = number(i);
    std::string key(sep);
    for (size_t j = 0; j < n.size(); j++) {
      key.append(1, n[j]);
      key.append(sep);
    }
    return key;
  }

  void test_CompressedCondense1() {
    db_t db(new TestDatabase);

    db->find(sep_number(1));
    db->set_value(value(1));

    db->find(sep_number(10));
    db->set_value(value(10));

    check_dump("CompressedEatCompressed-1", db);

    prepare_testpoint_output();

    db->find(sep_number(1));
    db->remove();

    check_dump("CompressedEatCompressed-2", db);

    const char *testpoints[] = {"Condense1"};
    check_testpoints(1, testpoints);
  }

  void test_CompressedCondense2() {}

  void test_CompressedCondense3() {}

  void test_CompressedCondense4() {}

  void test_bigkey() {}
};

TestCompress test_compress;
struct TestTrie {
  void test_TrieBaseAdd0() {
    db_t db(new TestDatabase);

    check_dump("TrieBaseAdd0-1", db);
    prepare_testpoint_output();

    db->find(std::string());
    db->set_value(value(1));

    check_dump("TrieBaseAdd0-2", db);

    const char *testpoints[] = {"TrieBaseAdd0"};
    check_testpoints(1, testpoints);
  }
  void test_TrieBaseAdd1() {
    db_t db(new TestDatabase);

    check_dump("TrieBaseAdd1-1", db);
    prepare_testpoint_output();

    db->find(number(1));
    db->set_value(value(1));

    check_dump("TrieBaseAdd1-2", db);

    const char *testpoints[] = {"TrieBaseAdd1"};
    check_testpoints(1, testpoints);
  }
  void test_TrieBaseAdd2() {
    db_t db(new TestDatabase);

    check_dump("TrieBaseAdd2-1", db);
    prepare_testpoint_output();

    db->find(number(12));
    db->set_value(value(1));

    check_dump("TrieBaseAdd2-2", db);

    const char *testpoints[] = {"TrieBaseAdd2"};
    check_testpoints(1, testpoints);
  }
  void test_TrieBaseAdd3() {
    db_t db(new TestDatabase);

    check_dump("TrieBaseAdd3-1", db);
    prepare_testpoint_output();

    db->find(number(123));
    db->set_value(value(1));

    check_dump("TrieBaseAdd3-2", db);

    const char *testpoints[] = {"TrieBaseAdd2"};
    check_testpoints(1, testpoints);
  }
  void test_BitTrieAdd0() {
    db_t db(new TestDatabase);

    std::string key = sequence(1);
    for (char i = 0; i < 60; i++) {
      key[0] = i;
      db->find(key);
      db->set_value(value(i));
    }

    check_dump("BitTrieAdd0-1", db);
    prepare_testpoint_output();

    key[0] = 61;
    db->find(key);
    db->set_value(value(61));

    check_dump("BitTrieAdd0-2", db);

    const char *testpoints[] = {"BitTrieAdd0", "grow0"};
    check_testpoints(1, testpoints);
  }
  void test_BitTrieAdd1(char border) {
    db_t db(new TestDatabase);
    std::stringstream cstr;

    std::string key = sequence(1);
    for (char i = 0; i < border; i++) {
      key[0] = i;
      db->find(key);
      db->set_value(value(i));
    }

    cstr << "BitTrieAdd1_" << border << "-1";
    check_dump(cstr.str().c_str(), db);
    prepare_testpoint_output();

    key[0] = (char)border;
    db->find(key);
    db->set_value(value(1));

    cstr.str("");
    cstr << "BitTrieAdd1_" << border << "-2";
    check_dump(cstr.str().c_str(), db);
  }

  void test_BitTrieAdd1_grow0() {
    test_BitTrieAdd1(1);
    const char *testpoints[] = {"BitTrieAdd1", "grow0"};
    check_testpoints(1, testpoints);
  }

  void test_BitTrieAdd1_grow2() {
    test_BitTrieAdd1(2);
    const char *testpoints[] = {"BitTrieAdd1", "grow2"};
    check_testpoints(1, testpoints);
  }

  void test_TrieRemove() {
    db_t db(new TestDatabase);

    std::string key = sequence(1);
    for (char i = 0; i < 61; i++) {
      key[0] = i;
      db->find(key);
      db->set_value(value(i));
    }

    check_dump("TrieRemove-1", db);
    prepare_testpoint_output();

    key[0] = 1;
    db->find(key);
    db->remove();

    check_dump("TrieRemove-2", db);

    const char *testpoints[] = {"TrieRemove", "grow0"};
    check_testpoints(1, testpoints);
  }
  void test_BitTrieRemove0() {
    db_t db(new TestDatabase);

    db->find(number(1));
    db->set_value(value(1));

    db->find(number(11));
    db->set_value(value(11));

    check_dump("BitTrieRemove0-1", db);
    prepare_testpoint_output();

    db->find(number(1));
    db->remove();

    check_dump("BitTrieRemove0-2", db);

    const char *testpoints[] = {"BitTrieRemove0"};
    check_testpoints(1, testpoints);
  }
  void test_BitTrieRemove1(char border) {
    db_t db(new TestDatabase);
    std::stringstream cstr;

    std::string key = sequence(1);
    for (char i = 0; i < border + 1; i++) {
      key[0] = i;
      db->find(key);
      db->set_value(value(i));
    }

    cstr << "BitTrieRemove1_" << border << "-1";
    check_dump(cstr.str().c_str(), db);
    prepare_testpoint_output();

    key[0] = 1;
    db->find(key);
    db->remove();

    cstr.str("");
    cstr << "BitTrieRemove1_" << border << "-2";
    check_dump(cstr.str().c_str(), db);
  }

  void test_BitTrieRemove1_grow0() {
    test_BitTrieRemove1(4);
    const char *testpoints[] = {"BitTrieRemove1", "grow0"};
    check_testpoints(1, testpoints);
  }

  void test_BitTrieRemove1_grow2() {
    test_BitTrieRemove1(2);
    const char *testpoints[] = {"BitTrieRemove1", "grow2"};
    check_testpoints(1, testpoints);
  }

  void test_BitTrieRemove2() {
    db_t db(new TestDatabase);

    db->find(number(123456));
    db->set_value(value(1));

    db->find(number(123457));
    db->set_value(value(2));

    check_dump("BitTrieRemove2-1", db);
    prepare_testpoint_output();

    db->remove();

    check_dump("BitTrieRemove2-2", db);

    const char *testpoints[] = {"BitTrieRemove2"};
    check_testpoints(1, testpoints);
  }
  void test_NodeEatSingle() {
    db_t db(new TestDatabase);

    db->find(number(1));
    db->set_value(value(1));

    db->find(number(11));
    db->set_value(value(11));

    db->find(number(111));
    db->set_value(value(111));

    check_dump("NodeEatSingle-1", db);
    prepare_testpoint_output();

    db->find(number(111));
    db->remove();

    check_dump("NodeEatSingle-2", db);

    const char *testpoints[] = {"NodeEatSingle"};
    check_testpoints(1, testpoints);
  }
#if 0
  // some test to complete the test cover
  void test_TrieMisc() {
    TempTrie trie(60);

    nodeid_t *children = (nodeid_t *)trie.node();
    memset(children, 0, sizeof(nodeid_t) * 64);
    for (int i = 1; i < 64; i += 2)
      children[i] = i;

    trie.set_extra(101);

    NodeHandler *handler = NodeHandler::handlers[kTrie];
    NodePtr *ptr = trie.page.node_ptr;
    Node *node = (Node *)&trie.page.data[ptr->offset];

    nodeid_t cmps[65];
    size_t count = handler->get_children(ptr, node, cmps);
    BOOST_REQUIRE(count == 33);

    for (int i = 1, j = 0; j < (int)count - 1; i += 2, j++) {
      BOOST_REQUIRE(cmps[j] == i);
    }

    BOOST_REQUIRE(cmps[count - 1] == 101);
    out << "end: " << (int)cmps[count - 1] << std::endl;

    for (int i = 1, j = 0; j < (int)count - 1; i += 2, j++)
      cmps[j] = i + 1;

    cmps[count - 1] = 102;

    handler->replace_children(ptr, node, cmps);

    for (int i = 1; i < 64; i += 2) {
      BOOST_REQUIRE(children[i] == i + 1);
    }

    BOOST_REQUIRE(trie.extra() == 102);
  }
#endif
};

TestTrie test_trie;
struct TestPageManagement {
#define VALUE_PAD 300
#define TWO_PAGE_COUNT 26
  void test_TwoPages() {
    db_t db(new TestDatabase);

    for (size_t i = 0; i < TWO_PAGE_COUNT; i++) {
      db->find(number(i, 5));
      db->set_value(value(i, VALUE_PAD));
    }

    check_dump("TwoPages-1", db);

    db->find(number(TWO_PAGE_COUNT, 5));
    db->set_value(value(TWO_PAGE_COUNT, VALUE_PAD));

    check_dump("TwoPages-2", db);

    db->find(number(20, 5));
    BOOST_REQUIRE(db->is_valid());
    BOOST_REQUIRE(db->value() == value(20, VALUE_PAD));

    db->find(number(5, 5));
    BOOST_REQUIRE(db->is_valid());
    BOOST_REQUIRE(db->value() == value(5, VALUE_PAD));
  }

  void test_MergePages() {
    db_t db(new TestDatabase);

    for (size_t i = 0; i <= TWO_PAGE_COUNT; i++) {
      db->find(number(i));
      db->set_value(value(i, VALUE_PAD));
    }

    check_dump("MergePages-1", db);
    prepare_testpoint_output();

    db->find(number(20));
    db->remove();

    check_dump("MergePages-2", db);

    const char *testpoints[] = {"MergePages", "DefragmentNodeMove"};
    check_testpoints(2, testpoints);
  }

  void test_RemoveAll() {
    db_t db(new TestDatabase);

    for (size_t i = 0; i < 200; i++) {
      db->find(number(i));
      db->set_value(value(i, 200));
    }

    for (db->first(); db->is_valid(); db->next())
      db->remove();

    db->first();
    BOOST_REQUIRE(!db->is_valid());

    db->find(number(10));
    BOOST_REQUIRE(!db->is_valid());
    db->set_value(value(10));
    BOOST_REQUIRE(db->is_valid());
  }

  void test_NodesInHeap() {
    NodeStorageInHeap nodes;

    PageRef p1 = nodes.free_page(1);
    nodes.no_reserve(p1);
    PageRef p2 = nodes.free_page(1);
    nodes.no_reserve(p2);
    PageRef p3 = nodes.free_page(1);
    nodes.no_reserve(p3);
    PageRef p4 = nodes.free_page(1);
    nodes.no_reserve(p4);
    PageRef p5 = nodes.free_page(1);
    nodes.no_reserve(p5);

    BOOST_REQUIRE(p1.id == 0);
    BOOST_REQUIRE(p2.id == 1);
    BOOST_REQUIRE(p3.id == 2);
    BOOST_REQUIRE(p4.id == 3);
    BOOST_REQUIRE(p5.id == 4);

    nodes.with_reserve(p2);
    BOOST_REQUIRE(nodes._free_pages.size() == 1);

    p2 = nodes.free_page(1);
    nodes.no_reserve(p2);
    BOOST_REQUIRE(p2.id == 1);
    BOOST_REQUIRE(nodes._free_pages.size() == 0);

    nodes.with_reserve(p3);
    nodes.with_reserve(p4);
    BOOST_REQUIRE(nodes._free_pages.size() == 2);

    nodes.with_reserve(p5);
    BOOST_REQUIRE(nodes._free_pages.size() == 0);

    p3 = nodes.free_page(1);
    BOOST_REQUIRE(p3.id == 2);

    BOOST_REQUIRE(nodes._pages.size() == 3);
  }
};

TestPageManagement test_pm;

struct TestNavigation {
  void test_RandomFind() {
    db_t db(new TestDatabase);
    for (size_t i = 0; i < 5000; i += 2) {
      db->find(number(i, 4));
      db->set_value(value(i));
    }

    int numbers[] = {
        580,  2849, 2879, 4228, 3134, 461,  1576, 4889, 3731, 2307, 4811, 1004,
        4454, 3216, 2233, 318,  3358, 4378, 1482, 4612, 3662, 1531, 2371, 933,
        2795, 1065, 1945, 2576, 2154, 3791, 4505, 4548, 985,  3608, 1530, 2973,
        225,  4026, 3566, 4421, 3132, 863,  478,  4382, 3886, 1707, 3293, 4003,
        1235, 3292, 2324, 410,  2876, 4514, 4827, 3874, 2321, 3047, 1178, 1661,
        4960, 1040, 2291, 3462, 450,  4558, 2626, 3677, 1897, 4243, 2097, 2585,
        2570, 949,  4567, 1139, 862,  2804, 1187, 4748, 4132, 3261, 3088, 4439,
        2220, 2516, 2695, 543,  3633, 857,  3378, 4280, 2038, 1651, 3433, 1881,
        2038, 4387, 2823, 2064};
    const int count = sizeof(numbers) / sizeof(int);
    for (int i = 0; i < count; i++) {
      int n = numbers[i];
      std::string key(number(n, 4));
      db->find(key);
      if (n % 2 == 1) {

        out << "check " << n << ":" << !db->is_valid() << std::endl;
        BOOST_REQUIRE(!db->is_valid());

        db->next();
        std::string next_key = db->key().string();

        key = number(++n, 4);
      }

      out << "check " << n << ":" << db->is_valid() << ","
          << (db->key().string() == key) << std::endl;
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == key);
    }
  }
  void create_trie(db_t &db, bool hole, bool end_node) {
    std::string key(2, 0);

    for (int i = 0; i < 64; i++) {
      if (hole && i == 10)
        continue;

      key[1] = (char)i;
      db->find(key);
      db->set_value(value(i));
    }

    if (end_node) {
      key.pop_back();
      db->find(key);
      db->set_value("value");
    }
  }

  void test_IterTrie() {
    db_t db(new TestDatabase);
    std::string key(2, 0);
    create_trie(db, true, false);

    db->first();
    int i = 0;
    while (db->is_valid()) {
      key[1] = i;

      out << "check " << i << ":" << (db->key() == key) << std::endl;
      BOOST_REQUIRE(db->key() == key);

      db->next();
      i++;
      if (i == 10)
        i++;
    }
    BOOST_REQUIRE(i == 64);

    i = 63;
    db->last();
    while (db->is_valid()) {
      key[1] = i;

      out << "check " << i << ":" << (db->key() == key) << std::endl;
      BOOST_REQUIRE(db->key() == key);

      db->prev();
      i--;
      if (i == 10)
        i--;
    }
    BOOST_REQUIRE(i == -1);

    // and do some finding
    key.assign(1, (char)0);
    db->find(key);
    BOOST_REQUIRE(!db->is_valid());

    key.append(1, (char)0);
    db->find(key);
    BOOST_REQUIRE(db->is_valid());

    key[1] = 10;
    db->find(key);
    BOOST_REQUIRE(!db->is_valid());

    key[1] = 11;
    db->find(key);
    BOOST_REQUIRE(db->is_valid());
  }
  // iteration with end node
  void test_IterTrieEnd() {
    db_t db(new TestDatabase);
    std::string key(2, 0);
    create_trie(db, false, true);

    db->first();

    key.assign(1, (char)0);
    out << "check "
        << "end"
        << ":" << (db->key() == key) << std::endl;
    BOOST_REQUIRE(db->key() == key);
    db->next();

    key.append(1, (char)0);
    int i = 0;
    while (db->is_valid()) {
      key[1] = i;

      out << "check " << i << ":" << (db->key() == key) << std::endl;
      BOOST_REQUIRE(db->key() == key);

      db->next();
      i++;
    }
    BOOST_REQUIRE(i == 64);

    i = 63;
    db->last();
    while (db->is_valid() && i >= 0) {
      key[1] = i;

      out << "check " << i << ":" << (db->key() == key) << std::endl;
      BOOST_REQUIRE(db->key() == key);

      db->prev();
      i--;
    }
    BOOST_REQUIRE(i == -1);
    BOOST_REQUIRE(db->is_valid());

    key.assign(1, (char)0);
    out << "check "
        << "end"
        << ":" << (db->key() == key) << std::endl;
    BOOST_REQUIRE(db->key() == key);

    db->prev();
    BOOST_REQUIRE(!db->is_valid());

    // and do some finding
    key.assign(1, (char)0);
    db->find(key);
    BOOST_REQUIRE(db->is_valid());
  }
  void do_iter(size_t key_size) {
    db_t db(new TestDatabase);
    std::string last_key;
    for (size_t j = 0; j < 5000; j += 2) {
      db->find(number(j, key_size));
      db->set_value(value(j));
    }

    // check_dump("tst", db);

    int i = 0;
    db->first();

    while (db->is_valid()) {
      Slice key = db->key();
      out << "down " << i << ": " << db->value().string() << std::endl;
      BOOST_REQUIRE(last_key < key.string());

      last_key = key.string();
      i++;
      db->next();
    }
    BOOST_REQUIRE(i == 2500);

    // iter down
    last_key.assign(1, (char)99);
    db->last();
    i = 0;
    while (db->is_valid()) {
      Slice key = db->key();
      out << "up " << i << ": " << db->value().string() << std::endl;
      BOOST_REQUIRE(last_key > key.string());
      last_key = key.string();
      i++;
      db->prev();
    }
    BOOST_REQUIRE(i == 2500);
  }

  void test_Iter() { do_iter(0); }

  void test_IterCompressed() { do_iter(10); }
  void test_removeFromTop(size_t key_size) {
    db_t db(new TestDatabase);
    std::string last_key;
    for (size_t j = 0; j < 5000; j += 2) {
      db->find(number(j, key_size));
      db->set_value(value(j));
    }

    int i = 0;
    for (db->first(); db->is_valid(); db->next()) {
      out << "remove " << i << ": " << db->value().string() << std::endl;
      db->remove();
      i++;
    }
    out << "stopped remove at: " << i << std::endl;
    BOOST_REQUIRE(i == 2500);

    for (db->first(); db->is_valid(); db->next())
      BOOST_REQUIRE(0);

    db->next();
    BOOST_REQUIRE(!db->is_valid());
    db->prev();
    BOOST_REQUIRE(!db->is_valid());
    db->last();
    BOOST_REQUIRE(!db->is_valid());
  }

  void test_removeFromBottom(size_t key_size) {
    db_t db(new TestDatabase);
    std::string last_key;
    for (size_t j = 0; j < 5000; j += 2) {
      db->find(number(j, key_size));
      db->set_value(value(j));
    }

    int i = 0;
    for (db->last(); db->is_valid(); db->prev()) {
      out << "remove " << i << ": " << db->value().string() << std::endl;
      db->remove();
      i++;
    }
    out << "stopped remove at: " << i << std::endl;
    BOOST_REQUIRE(i == 2500);

    for (db->last(); db->is_valid(); db->prev())
      BOOST_REQUIRE(0);

    db->next();
    BOOST_REQUIRE(!db->is_valid());
    db->prev();
    BOOST_REQUIRE(!db->is_valid());
    db->last();
    BOOST_REQUIRE(!db->is_valid());
  }

  // test prev / next of compress nodes
  void test_compress() {
    db_t db(new TestDatabase);

    db->find(number(10, 5));
    db->set_value(value(10));

    // find before
    db->find(number(9, 5));
    BOOST_REQUIRE(!db->is_valid());
    db->next();
    BOOST_REQUIRE(db->is_valid());
    BOOST_REQUIRE(db->key() == number(10, 5));

    db->find(number(9, 5));
    BOOST_REQUIRE(!db->is_valid());
    db->prev();
    BOOST_REQUIRE(!db->is_valid());

    // find after
    db->find(number(11, 5));
    BOOST_REQUIRE(!db->is_valid());
    db->next();
    BOOST_REQUIRE(!db->is_valid());

    db->find(number(11, 5));
    BOOST_REQUIRE(!db->is_valid());
    db->prev();
    BOOST_REQUIRE(db->is_valid());
    BOOST_REQUIRE(db->key() == number(10, 5));

    // compelete testing all operations
    db->first();
    BOOST_REQUIRE(db->is_valid());
    BOOST_REQUIRE(db->key() == number(10, 5));

    db->last();
    BOOST_REQUIRE(db->is_valid());
    BOOST_REQUIRE(db->key() == number(10, 5));
  }
  void test_ChangeValue() {
    db_t db(new TestDatabase);

    for (int i = 0; i < 5; i++) {
      db->find(number(i, 5));
      db->set_value(value(i));
    }

    const char *cmp_new[] = {"new_value-0 ppppppppp", // make bigger
                             "new_value-1 ppppppppp", "new_value-2 ppppppppp",
                             "new_value-3 ppppppppp", "new_value-4 ppppppppp"};

    int i = 0;
    for (db->first(); db->is_valid(); db->next()) {
      out << db->value().string() << std::endl;
      db->set_value(cmp_new[i++]);
    }

    i = 0;
    for (db->first(); db->is_valid(); db->next()) {
      out << db->value().string() << std::endl;
      BOOST_REQUIRE(db->value().string() == cmp_new[i++]);
    }
  }
};

TestNavigation test_nav;
BOOST_AUTO_TEST_SUITE(trie_manipulations)

BOOST_AUTO_TEST_CASE(CompressAddNew) { test_compress.test_CompressAddNew(); }

BOOST_AUTO_TEST_CASE(CompressAdd0_CompressReinsert1) {
  test_compress.test_CompressAdd0_CompressReinsert1();
}

BOOST_AUTO_TEST_CASE(CompressAdd0_CompressReinsert2) {
  test_compress.test_CompressAdd0_CompressReinsert2();
}

BOOST_AUTO_TEST_CASE(CompressAdd1_CompressReinsert0) {
  test_compress.test_CompressAdd1_CompressReinsert0();
}

BOOST_AUTO_TEST_CASE(CompressAdd1_CompressReinsert1) {
  test_compress.test_CompressAdd1_CompressReinsert1();
}

BOOST_AUTO_TEST_CASE(CompressAdd1_CompressReinsert2) {
  test_compress.test_CompressAdd1_CompressReinsert2();
}

BOOST_AUTO_TEST_CASE(CompressAdd2_CompressReinsert0) {
  test_compress.test_CompressAdd2_CompressReinsert0();
}

BOOST_AUTO_TEST_CASE(CompressAdd2_CompressReinsert1) {
  test_compress.test_CompressAdd2_CompressReinsert1();
}

BOOST_AUTO_TEST_CASE(CompressAdd2_CompressReinsert2) {
  test_compress.test_CompressAdd2_CompressReinsert2();
}

BOOST_AUTO_TEST_CASE(Compresse_Condense1) {
  test_compress.test_CompressedCondense1();
}

BOOST_AUTO_TEST_CASE(Compresse_Condense2) {
  test_compress.test_CompressedCondense2();
}

BOOST_AUTO_TEST_CASE(Compresse_Condense3) {
  test_compress.test_CompressedCondense3();
}

BOOST_AUTO_TEST_CASE(Compresse_Condense4) {
  test_compress.test_CompressedCondense4();
}

BOOST_AUTO_TEST_CASE(Compresse_bigkey) { test_compress.test_bigkey(); }

BOOST_AUTO_TEST_CASE(LeafAdd0) { test_leaf.test_LeafAdd0(); }

BOOST_AUTO_TEST_CASE(LeafAdd1) { test_leaf.test_LeafAdd1(); }

BOOST_AUTO_TEST_CASE(LeafAdd2) { test_leaf.test_LeafAdd2(); }

BOOST_AUTO_TEST_CASE(TrieBaseAdd0) { test_trie.test_TrieBaseAdd0(); }

BOOST_AUTO_TEST_CASE(TrieBaseAdd1) { test_trie.test_TrieBaseAdd1(); }

BOOST_AUTO_TEST_CASE(TrieBaseAdd2) { test_trie.test_TrieBaseAdd2(); }

BOOST_AUTO_TEST_CASE(TrieBaseAdd3) { test_trie.test_TrieBaseAdd3(); }

BOOST_AUTO_TEST_CASE(BitTrieAdd0) { test_trie.test_BitTrieAdd0(); }

BOOST_AUTO_TEST_CASE(BitTrieAdd1) {
  test_trie.test_BitTrieAdd1_grow0();
  test_trie.test_BitTrieAdd1_grow2();
}

BOOST_AUTO_TEST_CASE(TrieRemove) { test_trie.test_TrieRemove(); }

BOOST_AUTO_TEST_CASE(BitTrieRemove0) { test_trie.test_BitTrieRemove0(); }

BOOST_AUTO_TEST_CASE(BitTrieRemove1) {
  test_trie.test_BitTrieRemove1_grow0();
  test_trie.test_BitTrieRemove1_grow2();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove2) { test_trie.test_BitTrieRemove2(); }

BOOST_AUTO_TEST_CASE(NodeEatSingle) { test_trie.test_NodeEatSingle(); }

// BOOST_AUTO_TEST_CASE(TrieMisc) { test_trie.test_TrieMisc(); }

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE(page_manipulations)

BOOST_AUTO_TEST_CASE(TwoPages) { test_pm.test_TwoPages(); }

BOOST_AUTO_TEST_CASE(MergePages) { test_pm.test_MergePages(); }

BOOST_AUTO_TEST_CASE(RemoveAll) { test_pm.test_RemoveAll(); }

BOOST_AUTO_TEST_CASE(NodesInHeap) { test_pm.test_NodesInHeap(); }

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE(navigation)

BOOST_AUTO_TEST_CASE(RandomFind) { test_nav.test_RandomFind(); }

BOOST_AUTO_TEST_CASE(IterTrie) { test_nav.test_IterTrie(); }

BOOST_AUTO_TEST_CASE(IterTrieEnd) { test_nav.test_IterTrieEnd(); }

BOOST_AUTO_TEST_CASE(Iter) { test_nav.test_Iter(); }

BOOST_AUTO_TEST_CASE(IterCompressed) { test_nav.test_IterCompressed(); }

BOOST_AUTO_TEST_CASE(RemoveFromTop) {
  test_nav.test_removeFromTop(0);
  test_nav.test_removeFromTop(10);
}

BOOST_AUTO_TEST_CASE(RemoveFromBottom) {
  test_nav.test_removeFromBottom(0);
  test_nav.test_removeFromBottom(10);
}

BOOST_AUTO_TEST_CASE(compress) { test_nav.test_compress(); }

BOOST_AUTO_TEST_CASE(ChangeValue) { test_nav.test_ChangeValue(); }

BOOST_AUTO_TEST_SUITE_END()

#ifdef BOOST_TEST_NO_MAIN
int main(int argc, const char *argv[]) {
  std::cerr << "start test" << std::endl;
  test_compress.test_CompressAddNew();
  // test_compress.test_CompressAdd0_CompressReinsert1();
  // test_compress.test_CompressAdd0_CompressReinsert2();
  // test_compress.test_CompressAdd1_CompressReinsert0();
  // test_compress.test_CompressAdd1_CompressReinsert1();
  // test_compress.test_CompressAdd1_CompressReinsert2();
  // test_compress.test_CompressAdd2_CompressReinsert0();
  // test_compress.test_CompressAdd2_CompressReinsert1();
  // test_compress.test_CompressAdd2_CompressReinsert2();
  // test_compress.test_CompressedCondense1();

  // test_compress.test_CompressedCondense2();
  // test_compress.test_CompressedCondense3();
  // test_compress.test_CompressedCondense4();
  // test_compress.test_bigkey();
  // test_leaf.test_LeafGrow1();

  // test_leaf.test_LeafAdd0();
  // test_leaf.test_LeafAdd1();
  // test_leaf.test_LeafAdd2();
  // test_trie.test_TrieBaseAdd0();
  // test_trie.test_TrieBaseAdd1();
  // test_trie.test_TrieBaseAdd2();
  // test_trie.test_TrieBaseAdd3();
  // test_trie.test_BitTrieAdd0();
  // test_trie.test_BitTrieAdd1_grow0();
  // test_trie.test_BitTrieAdd1_grow2();
  // test_trie.test_TrieRemove();
  // test_trie.test_BitTrieRemove0();
  // test_trie.test_BitTrieRemove1_grow0();
  // test_trie.test_BitTrieRemove1_grow2();
  // test_trie.test_BitTrieRemove2();
  // test_trie.test_NodeEatSingle();
  // test_trie.test_TrieMisc();
  // test_pm.test_TwoPages();
  // test_pm.test_MergePages();
  // test_pm.test_RemoveAll();
  // test_pm.test_NodesInHeap();
  // test_nav.test_RandomFind();
  // test_nav.test_IterTrie();
  // test_nav.test_IterTrieEnd();
  // test_nav.test_Iter();
  // test_nav.test_IterCompressed();
  // test_nav.test_removeFromTop(0);
  // test_nav.test_removeFromTop(10);
  // test_nav.test_removeFromBottom(0);
  // test_nav.test_removeFromBottom(10);
  // test_nav.test_compress();
  // test_nav.test_ChangeValue();
  std::cerr << "done test" << std::endl;
  return 0;
}

#endif
