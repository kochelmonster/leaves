//@+leo-ver=5-thin
//@+node:michael.20150101205559.18: * @file test_trie.cpp
//@@language cplusplus
//@@tabwidth -2
//@+<< includes >>
//@+node:michael.20150101205559.22: ** << includes >>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#define BOOST_TEST_MODULE TrieTest
//#define BOOST_TEST_NO_MAIN
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
      trace.find(key);
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


//#define GENERATE
#ifdef GENERATE

void check_output(const char* fname, std::stringstream& strstr) {
  std::cout << strstr.str();
  std::ofstream out(fname);
  out << strstr.str();
}

void check_cases(size_t case_count, const char** cases) {
  std::cerr << "check case:" << case_output.str().size() << std::endl;
  std::cerr << case_output.str() << std::endl;
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


void check_cases(size_t case_count, const char** cases) {
  std::vector<int> done;
  done.resize(case_count);
  std::string output(case_output.str());
  std::stringstream in(output);

  while(in)  {
    std::string sub;
    in >> sub;
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
      BOOST_REQUIRE(false);
      break;
    }
  }
  
  //std::cerr << case_output.str() << std::endl;
}

#endif
//@+node:michael.20150101205559.39: ** TestBase
struct TestBase {
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
};

//@+node:michael.20150101205559.32: ** TestCompress
struct TestCompress : public TestBase {
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
struct TestLeaf : public TestBase {
  void test_LeafAdd0() {
      prepare(5);
      input.append(1, (char)5);
      input.append(1, (char)6);
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"LeafAdd0"};
      check("LeafAdd0", 1, cases);
      end();
    }
    
  void test_LeafAdd1() {
      prepare(5);
      input.append(1, (char)5);
      input.append(1, (char)6);
      input.append(1, (char)7);
      
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"LeafAdd1"};
      check("LeafAdd1", 1, cases);
      end();
    }
    
    void test_LeafAdd2() {
      prepare(5);
      input.append(1, (char)5);
      cursor->set(input);
      cursor->set_value(Slice("value-1"));
      const char* cases[] = {"LeafAdd2"};
      check("LeafAdd2", 1, cases);
      end();
    }    
};

TestLeaf test_leaf;
//@+node:michael.20150101205559.40: ** TestBitTrie
struct TestBitTrie : public TestBase {

  void prepare(int count) {
      db = new TestDatabase;
      cursor = db->writer();
      input.clear();
      input.assign(2, (char)0);
      
      for(int i = 0; i < count; i++) {
        std::stringstream f;
        f << "value-" << i;
        input[1] = i;
        cursor->set(input);
        cursor->set_value(Slice(f.str()));
      }
      prepare_case_output();
    }


  void test_TrieBaseAdd0() {
      prepare(1);
     
      input.resize(1);
      cursor->set(input);
      cursor->set_value(Slice("value-e"));
      
      const char* cases[] = {"TrieBaseAdd0"};
      check("TrieBaseAdd0", 1, cases);
      end();
    }
    
  void test_BitTrieAdd0() {
      prepare(56);
      input[1] = 57;
      
      cursor->set(input);
      cursor->set_value(Slice("value-57"));
            
      const char* cases[] = {"TrieBaseAdd1", "BitTrieAdd0"};
      check("BitTrieAdd0", 2, cases);
      end();
    }    
    
  void test_BitTrieAdd1() {
      prepare(8);
      input[1] = 11;
      
      cursor->set(input);
      cursor->set_value(Slice("value-11"));
            
      const char* cases[] = {"TrieBaseAdd1", "BitTrieAdd1", "BitTrieAdd2"};
      check("BitTrieAdd1", 3, cases);
      end();
    }    
    
  void test_BitTrieAdd2() {
      prepare(1);
      input[1] = 11;
      
      cursor->set(input);
      cursor->set_value(Slice("value-11"));
            
      const char* cases[] = {"TrieBaseAdd1", "BitTrieAdd2"};
      check("BitTrieAdd2", 2, cases);
      end();
    }
    
    
  void test_TrieBaseAdd2() {
      prepare(1);
      input[1] = 1;
      input.append(1, (char)2);
      
      cursor->set(input);
      cursor->set_value(Slice("value-2"));
            
      const char* cases[] = {"TrieBaseAdd2", "BitTrieAdd2"};
      check("TrieBaseAdd2", 2, cases);
      end();
    }
    
    
  void test_TrieBaseAdd3() {
      prepare(1);
      input[1] = 1;
      input.append(1, (char)2);
      input.append(1, (char)3);
      
      cursor->set(input);
      cursor->set_value(Slice("value-3"));
            
      const char* cases[] = {"TrieBaseAdd3", "BitTrieAdd2"};
      check("TrieBaseAdd3", 2, cases);
      end();
    }        
};

TestBitTrie test_bitrie;
//@+node:michael.20150101205559.38: ** TestSuite
//BOOST_AUTO_TEST_SUITE(trie_creation_suite)

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


BOOST_AUTO_TEST_CASE(LeafAdd0) {
  test_leaf.test_LeafAdd0();
}

BOOST_AUTO_TEST_CASE(LeafAdd1) {
  test_leaf.test_LeafAdd1();
}

BOOST_AUTO_TEST_CASE(LeafAdd2) {
  test_leaf.test_LeafAdd2();
}

BOOST_AUTO_TEST_CASE(BitTrieAdd2) {
  test_bitrie.test_BitTrieAdd2();
}

BOOST_AUTO_TEST_CASE(BitTrieAdd1) {
  test_bitrie.test_BitTrieAdd1();
}
  
BOOST_AUTO_TEST_CASE(BitTrieAdd0) {
  test_bitrie.test_BitTrieAdd0();
}
  
BOOST_AUTO_TEST_CASE(TrieBaseAdd0) {
  test_bitrie.test_TrieBaseAdd0();
}
  
BOOST_AUTO_TEST_CASE(TrieBaseAdd2) {
  test_bitrie.test_TrieBaseAdd2();
}
  
BOOST_AUTO_TEST_CASE(TrieBaseAdd3) {
  test_bitrie.test_TrieBaseAdd3();
}

//BOOST_AUTO_TEST_SUITE_END()



#ifdef BOOST_TEST_NO_MAIN
int main(int argc, const char* argv[]) {
  test_bitrie.test_BitTrieAdd2();
  test_bitrie.test_BitTrieAdd1();
  test_bitrie.test_BitTrieAdd0();
  test_bitrie.test_TrieBaseAdd0();
  test_bitrie.test_TrieBaseAdd2();
  test_bitrie.test_TrieBaseAdd3();
  return 0;
}
#endif
//@-others

//@-leo
