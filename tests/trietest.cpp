//@+leo-ver=5-thin
//@+node:michael.20150101205559.18: * @file trietest.cpp
//@@language cplusplus
//@@tabwidth -2
//@+<< includes >>
//@+node:michael.20150101205559.22: ** << includes >>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#define BOOST_TEST_MODULE TrieTest
#include <boost/test/included/unit_test.hpp>
#include "larch/leaves.h"
#include "node.h"

//@-<< includes >>

using namespace larch_leaves;

//@+others
//@+node:michael.20150101205559.19: ** TestDatabase
/* A Storage to test the pure tie structures */
//@+others
//@+node:michael.20150101205559.20: *3* Cursor
struct ReadTestCursor {
  Trace trace;
  NodeRef root;
  std::string key_buffer;
  
  ReadTestCursor(NodeStorageInHeap& nodes, NodeRef root_) 
    : trace(nodes, nodes), root(root_) { 
      trace.push(root);
    }
    
  bool is_valid() const {
      return trace.complete;
    }
  
  void set(const Slice& key) { 
      key_buffer.assign(key.data(), key.size());
      if (trace.find(key))
        return;
      trace.reset();
      root.find(key, trace);
    }
    
  void first() { 
      root.first(trace);
    }
  void last() { 
      root.last(trace);
    }
  void next() { 
      trace.next();
    }
  void prev() {
      trace.prev();
    }
  
  Slice key() { 
      return Slice(trace.key);
    }
    
  Slice value() { 
      trace.check_complete();
      return Slice((char*)trace.current().node(), *trace.current().extra());
    }
  
  void set_value(const Slice& value) { throw NotImplemented(); }
  void remove() { throw NotImplemented(); }
};

struct WriteTestCursor : public ReadTestCursor {
  WriteTestCursor(NodeStorageInHeap& nodes, NodeRef root_)
    : ReadTestCursor(nodes, root_) { }
  
  void set_value(const Slice& value) {
      trace.set_leaf(key_buffer, TempLeaf(value));
    }
  
  void remove() { 
      trace.remove();
    }
};
//@+node:michael.20150101205559.21: *3* class TestDatabase
struct TestDatabase {
  NodeStorageInHeap _nodes;

  TestDatabase() {
      Trace trace(_nodes, _nodes);
      NodeRef root_ = root();
      trace.push(root_);
      trace.add_node(TempTrie(), Slice());
    }

  NodeRef root() {
    return NodeRef(_nodes.get_page(0), 0);
  }

  ReadTestCursor* reader() {
      return new ReadTestCursor(_nodes, root());
    }
  
  WriteTestCursor* writer() {
      return new WriteTestCursor(_nodes, root());
    }
    
#ifdef DEBUG
  void dump(std::ostream& out) {
      out << "state:" << std::endl;
      typedef std::vector<NodeStorageInHeap::_page_ptr>::iterator iter_t;
      iter_t i = _nodes._pages.begin();
      for(int j = 0; i != _nodes._pages.end(); i++, j++)
        PageRef(i->get(), j, j).dump(out);
      out << "---" << std::endl;
    }
#endif    
};

//@-others

//@+node:michael.20150101205559.31: ** Test Utils
std::stringstream case_output;

namespace larch_leaves {

void TESTCASE(const char* str) {
  case_output << "TESTCASE: " << str << std::endl;
}

}

void prepare_case_output() {
  case_output.str("");
}

void check_cases(size_t case_count, const char** cases) {
  std::vector<int> done;
  done.resize(case_count);
  std::string output(case_output.str());

  while(case_output)  {
    std::string sub;
    case_output >> sub;
    for(size_t i = 0; i < case_count; i++) {
      if (sub == cases[i])
        done[i] = 1;
    }
    
    //std::cerr << "sub: " << sub << std::endl;
  }
  
  for(size_t i = 0; i < case_count; i++) {
    if (! done[i]) {
      std::cerr << "missing case: " << cases[i] 
                << " in " << std::endl << output << std::endl;
      break;
    }
  }
  
  //std::cerr << case_output.str() << std::endl;
}

#ifdef GENERATE

void check_output(const char* fname, std::stringstream& strstr) {
  std::cout << strstr.str();
  std::ofstream out(fname);
  out << strstr.str();
}
 
#else
  
void check_output(const char* fname, std::stringstream& strstr) {
  std::string path("outputs/");
  path.append(fname);
  
  std::string compare;
  std::ifstream in(path.c_str(), std::ios_base::in|std::ios_base::binary);
  
  if (! in.is_open()) {
    std::cerr << "could not open " << path << std::endl;
    return;
  }
  
  in.seekg(0, std::ios::end);
  size_t size = in.tellg();
  //std::cerr << "--size: " << size << " in open? " << in.is_open() << std::endl;
  compare.assign(size, 0);
  in.seekg(0);
  in.read(&compare[0], size);
 
  BOOST_REQUIRE_EQUAL(strstr.str(), compare);
}


#endif
//@+node:michael.20150101205559.32: ** TestCompress
struct TestCompress {
  TestDatabase *db;
  WriteTestCursor *cursor;
  std::string input;
  
  void prepare(int size) {
      db = new TestDatabase;
      cursor = db->writer();
      input.clear();
      for(int i = 0; i < size; i++)
        input.append(1, (char)i);
      cursor->set(input);
      cursor->set_value(Slice("value"));
      prepare_case_output();
    }
    
  void end() {
      delete db;
      delete cursor;
    }

  void check(const char *fname, size_t case_count, const char** cases) {
      std::stringstream strstr;
      db->dump(strstr);
      check_output(fname, strstr);
      check_cases(case_count, cases);
    }

  void test_CompressAdd0_CompressReinsert0() {
      // is not possible would be a compress node of size1
    }

  void test_CompressAdd0_CompressReinsert1() {
      prepare(3);
      input[1] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd0", "CompressReinsert1"};
      check("CompressAdd0_CompressReinsert1", 2, cases);
      end();
    }

  void test_CompressAdd0_CompressReinsert2() {
      prepare(10);
      input[1] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd0", "CompressReinsert2"};
      check("CompressAdd0_CompressReinsert2", 2, cases);
      end();
    }
    
  void test_CompressAdd1_CompressReinsert0() {
      prepare(3);
      input[2] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd1", "CompressReinsert0"};
      check("CompressAdd1_CompressReinsert0", 2, cases);
      end();
    }
    
  void test_CompressAdd1_CompressReinsert1() {
      prepare(4);
      input[2] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd1", "CompressReinsert1"};
      check("CompressAdd1_CompressReinsert1", 2, cases);
      end();
    }    
    
  void test_CompressAdd1_CompressReinsert2() {
      prepare(10);
      input[2] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd1", "CompressReinsert2"};
      check("CompressAdd1_CompressReinsert2", 2, cases);
      end();
    }
    
  void test_CompressAdd2_CompressReinsert0() {
      prepare(10);
      input[9] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd2", "CompressReinsert0"};
      check("CompressAdd2_CompressReinsert0", 2, cases);
      end();
    }
    
  void test_CompressAdd2_CompressReinsert1() {
      prepare(10);
      input[8] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd2", "CompressReinsert1"};
      check("CompressAdd2_CompressReinsert1", 2, cases);
      end();
    }
    
  void test_CompressAdd2_CompressReinsert2() {
      prepare(10);
      input[5] = 11;
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"CompressAdd2", "CompressReinsert2"};
      check("CompressAdd2_CompressReinsert2", 2, cases);
      end();
    }
};

TestCompress test_compress;

//@+node:michael.20150101205559.34: ** TestLeaf
struct TestLeaf {
  TestDatabase *db;
  WriteTestCursor *cursor;
  std::string input;
  
  void prepare() {
      db = new TestDatabase;
      cursor = db->writer();
      input.assign((const char[]){ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }, 10);
      cursor->set(input);
      cursor->set_value(Slice("value"));
      std::cerr << "------------------" << std::endl;
    }
    
  void end() {
      delete db;
      delete cursor;
    }

  void test_LeafAdd2() {
      prepare();
      input.append(1, (char)11);
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      db->dump(std::cout);
      end();
    }
};
//@+node:michael.20150101205559.38: ** TestSuite
BOOST_AUTO_TEST_SUITE(trie_creation_suite)

BOOST_AUTO_TEST_CASE(CompressAdd0_CompressReinsert0) {
  test_compress.test_CompressAdd0_CompressReinsert0();
}

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



BOOST_AUTO_TEST_SUITE_END()
//@-others

//@-leo
