//@+leo-ver=5-thin
//@+node:michael.20150101205559.18: * @file test_trie.cpp
//@@language cplusplus
//@@tabwidth -2
#define BOOST_TEST_NO_MAIN
#define GENERATE
//@+<< includes >>
//@+node:michael.20150101205559.22: ** << includes >>
#include <string.h>
#include <stdlib.h>
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
//@+node:michael.20150101205559.21: ** TestDatabase
struct TestDatabase {
  NodeStorageInHeap _nodes;
  Trace trace;
  std::string key_buffer;

  TestDatabase() : trace(_nodes, _nodes) {
      NodeRef root(_nodes.get_page(0), 0);
      trace.push(root);
      trace.add_node(TempTrie(), Slice());
      trace.reset();
    }
     
  bool is_valid() const {
      return trace.complete;
    }
  
  void find(const Slice& key) { 
      key_buffer.assign(key.data(), key.size());
      trace.find(key);
    }
    
  void first() { 
      trace.first();
    }
    
  void last() { 
      trace.last();
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

  void set_value(const Slice& value) {
      trace.set_leaf(key_buffer, TempLeaf(value));
    }
  
  void remove() { 
      trace.remove();
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

//@+node:michael.20150101205559.31: ** Test Utils
std::stringstream tp_output;

namespace larch_leaves {

void TESTPOINT(const char* str) {
  tp_output << "TESTPOINT: " << str << std::endl;
}

}

void prepare_testpoint_output() {
  tp_output.str("");
}

#ifdef GENERATE

void check_output(const char* fname, std::stringstream& strstr) {
  std::cout << strstr.str();
  std::ofstream out(fname);
  out << strstr.str();
}

void check_testpoints(size_t case_count, const char** testpoints) {
  std::cerr << "check case:" << tp_output.str().size() << std::endl;
  std::cerr << tp_output.str() << std::endl;
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


void check_testpoints(size_t case_count, const char** testpoints) {
  std::vector<int> done;
  done.resize(case_count);
  std::string output(tp_output.str());
  std::stringstream in(output);

  while(in)  {
    std::string sub;
    in >> sub;
    for(size_t i = 0; i < case_count; i++) {
      if (sub == testpoints[i])
        done[i] = 1;
    }
    
    //std::cerr << "sub: " << sub << std::endl;
  }
  
  for(size_t i = 0; i < case_count; i++) {
    if (! done[i]) {
      std::cerr << "missing case: " << testpoints[i] 
                << " in " << std::endl << output << std::endl;
      BOOST_REQUIRE(false);
      break;
    }
  }
  
  //std::cerr << tp_output.str() << std::endl;
}

#endif
//@+node:michael.20150101205559.39: ** TestBase
struct TestBase {
  TestDatabase *db;
  std::string input;
  
  void prepare(int size) {
      db = new TestDatabase;
      input.clear();
      for(int i = 0; i < size; i++)
        input.append(1, (char)i);
      db->find(input);
      db->set_value(Slice("value"));
      prepare_testpoint_output();
    }
    
  void end() {
      delete db;
    }

  void check(const char *fname, size_t case_count, const char** testpoints) {
      std::stringstream strstr;
      db->dump(strstr);
      check_output(fname, strstr);
      check_testpoints(case_count, testpoints);
    }
};
//@+node:michael.20150101205559.32: ** TestCompress
struct TestCompress : public TestBase {
  void test_CompressAdd0_CompressReinsert0() {
      // is not possible would be a compress node of size1
    }

  void test_CompressAddNew() {
      prepare(3);
      input.append(1, (char)3);
      input.append(1, (char)4);
      input.append(1, (char)5);
      db->find(input);
      db->set_value(Slice("value-5"));
      const char* testpoints[] = {"CompressAddNew"};
      check("CompressAddNew", 1, testpoints);
      end();
  }
  
  void test_CompressAdd0_CompressReinsert1() {
      prepare(3);
      input[1] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd0", "CompressReinsert1"};
      check("CompressAdd0_CompressReinsert1", 2, testpoints);
      end();
    }

  void test_CompressAdd0_CompressReinsert2() {
      prepare(10);
      input[1] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd0", "CompressReinsert2"};
      check("CompressAdd0_CompressReinsert2", 2, testpoints);
      end();
    }
    
  void test_CompressAdd1_CompressReinsert0() {
      prepare(3);
      input[2] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd1", "CompressReinsert0"};
      check("CompressAdd1_CompressReinsert0", 2, testpoints);
      end();
    }
    
  void test_CompressAdd1_CompressReinsert1() {
      prepare(4);
      input[2] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd1", "CompressReinsert1"};
      check("CompressAdd1_CompressReinsert1", 2, testpoints);
      end();
    }    
    
  void test_CompressAdd1_CompressReinsert2() {
      prepare(10);
      input[2] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd1", "CompressReinsert2"};
      check("CompressAdd1_CompressReinsert2", 2, testpoints);
      end();
    }
    
  void test_CompressAdd2_CompressReinsert0() {
      prepare(10);
      input[9] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd2", "CompressReinsert0"};
      check("CompressAdd2_CompressReinsert0", 2, testpoints);
      end();
    }
    
  void test_CompressAdd2_CompressReinsert1() {
      prepare(10);
      input[8] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd2", "CompressReinsert1"};
      check("CompressAdd2_CompressReinsert1", 2, testpoints);
      end();
    }
    
  void test_CompressAdd2_CompressReinsert2() {
      prepare(10);
      input[5] = 11;
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"CompressAdd2", "CompressReinsert2"};
      check("CompressAdd2_CompressReinsert2", 2, testpoints);
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
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"LeafAdd0"};
      check("LeafAdd0", 1, testpoints);
      end();
    }
    
  void test_LeafAdd1() {
      prepare(5);
      input.append(1, (char)5);
      input.append(1, (char)6);
      input.append(1, (char)7);
      
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"LeafAdd1"};
      check("LeafAdd1", 1, testpoints);
      end();
    }
    
    void test_LeafAdd2() {
      prepare(5);
      input.append(1, (char)5);
      db->find(input);
      db->set_value(Slice("value-1"));
      const char* testpoints[] = {"LeafAdd2"};
      check("LeafAdd2", 1, testpoints);
      end();
    }    
};

TestLeaf test_leaf;
//@+node:michael.20150101205559.40: ** TestTrie
struct TestTrie : public TestBase {
  void prepare(int count) {
      db = new TestDatabase;
      input.clear();
      input.assign(2, (char)0);
      
      for(int i = 0; i < count; i++) {
        std::stringstream f;
        f << "value-" << i;
        input[1] = i;
        db->find(input);
        db->set_value(Slice(f.str()));
      }
      prepare_testpoint_output();
    }

  //@+others
  //@+node:michael.20150101205559.46: *3* test_TrieBaseAdd0
  void test_TrieBaseAdd0() {
      prepare(1);
     
      input.resize(1);
      db->find(input);
      db->set_value(Slice("value-e"));
      
      const char* testpoints[] = {"TrieBaseAdd0"};
      check("TrieBaseAdd0", 1, testpoints);
      end();
    }
  //@+node:michael.20150101205559.47: *3* test_BitTrieAdd0
  void test_BitTrieAdd0() {
      prepare(56);
      input[1] = 57;
      
      db->find(input);
      db->set_value(Slice("value-57"));
            
      const char* testpoints[] = {"TrieBaseAdd1", "BitTrieAdd0"};
      check("BitTrieAdd0", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.48: *3* test_BitTrieAdd1
  void test_BitTrieAdd1() {
      prepare(8);
      input[1] = 11;
      
      db->find(input);
      db->set_value(Slice("value-11"));
            
      const char* testpoints[] = {"TrieBaseAdd1", "BitTrieAdd1", "BitTrieAdd2"};
      check("BitTrieAdd1", 3, testpoints);
      end();
    }
  //@+node:michael.20150101205559.49: *3* test_BitTrieAdd2
  void test_BitTrieAdd2() {
      prepare(1);
      input[1] = 11;
      
      db->find(input);
      db->set_value(Slice("value-11"));
            
      const char* testpoints[] = {"TrieBaseAdd1", "BitTrieAdd2"};
      check("BitTrieAdd2", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.50: *3* test_TrieBaseAdd2
  void test_TrieBaseAdd2() {
      prepare(1);
      input[1] = 1;
      input.append(1, (char)2);
      
      db->find(input);
      db->set_value(Slice("value-2"));
            
      const char* testpoints[] = {"TrieBaseAdd2", "BitTrieAdd2"};
      check("TrieBaseAdd2", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.51: *3* test_TrieBaseAdd3
  void test_TrieBaseAdd3() {
      prepare(1);
      input[1] = 1;
      input.append(1, (char)2);
      input.append(1, (char)3);
      
      db->find(input);
      db->set_value(Slice("value-3"));
            
      const char* testpoints[] = {"TrieBaseAdd3", "BitTrieAdd2"};
      check("TrieBaseAdd3", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.52: *3* test_TrieRemove
  void test_TrieRemove() {
      prepare(56);
      input[1] = 56;
      input.append(1, (char)2);
      input.append(1, (char)3);
      input.append(1, (char)4);
      input.append(1, (char)5);
      
      db->find(input);
      db->set_value(Slice("value-56"));
      
      db->remove();
            
      const char* testpoints[] = {"TrieRemove"};
      check("TrieRemove", 1, testpoints);
      end();
    }
  //@+node:michael.20150101205559.58: *3* test_BitTrieRemove0
  void test_BitTrieRemove0() {
      prepare(1);
      
      input.pop_back();
      db->find(input);
      db->set_value(Slice("value-E"));
      
      db->remove();
            
      const char* testpoints[] = {"BitTrieRemove0"};
      check("BitTrieRemove0", 1, testpoints);
      end();
    }
  //@+node:michael.20150101205559.60: *3* test_BitTrieRemove1
  void test_BitTrieRemove1() {
      prepare(1);
      
      input.pop_back();
      db->find(input);
      db->set_value(Slice("value-E"));
      
      input.append(1, (char)0);
      db->find(input);
      db->remove();
            
      const char* testpoints[] = {"BitTrieRemove1", "BitTrieChange2"};
      check("BitTrieRemove1", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.59: *3* test_BitTrieRemove2
  void test_BitTrieRemove2() {
      prepare(2);
      db->remove();
            
      const char* testpoints[] = {"BitTrieRemove2"};
      check("BitTrieRemove2", 1, testpoints);
      end();
    }
  //@+node:michael.20150101205559.56: *3* test_BitTrieRemove3_8
  void test_BitTrieRemove3_8() {
      prepare(9);
      
      input[1] = 0;
      db->find(input);
      db->remove();
            
      const char* testpoints[] = {"BitTrieRemove3", "DefragmentNodeMove"};
      check("BitTrieRemove3_8", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.55: *3* test_BitTrieRemove3_24
  void test_BitTrieRemove3_24() {
      prepare(25);
      
      input[1] = 0;
      db->find(input);
      db->remove();
            
      const char* testpoints[] = {"BitTrieRemove3", "DefragmentNodeMove"};
      check("BitTrieRemove3_24", 2, testpoints);
      end();
    }
  //@+node:michael.20150101205559.54: *3* test_BitTrieRemove3_40
  void test_BitTrieRemove3_40() {
      prepare(41);
      db->remove();
            
      const char* testpoints[] = {"BitTrieRemove3"};
      check("BitTrieRemove3_40", 1, testpoints);
      end();
    }
  //@+node:michael.20150101205559.61: *3* test_BitTrieChange0
  void test_BitTrieChange0() {
      prepare(1);
      input[1] = 1;
      input.append(1, (char)2);
      input.append(1, (char)3);
      input.append(1, (char)4);
      db->find(input);
      db->set_value(Slice("value-4"));
      
      input.resize(2);
      input[1] = 0;
      db->find(input);
      db->remove();
            
      const char* testpoints[] = {"BitTrieChange0"};
      check("BitTrieChange0", 1, testpoints);
      end();
    }
  //@-others
};

TestTrie test_trie;
//@+node:michael.20150101205559.62: ** TestPageManagement
struct TestPageManagement : public TestBase {
  void add(int number) {
      char buffer[20];
      sprintf(buffer, "%d", number);
      input.clear();
      for(size_t j = 0; j < strlen(buffer); j++) {
        input.push_back(buffer[j]-'0');
      }
      
      std::stringstream f;
      f << "value-" << number;
      
      for(int i = 0; i < 240; i++)
        f << "a";
      
      db->find(input);
      db->set_value(Slice(f.str()));
    }

  void prepare(int count) {
      db = new TestDatabase;
      for(int i = 0; i < count; i++)
        add(i);

      prepare_testpoint_output();
    }

  void test_TwoPages() {
      prepare(16);
      
      add(16);
      
      const char* testpoints[] = {"LeafAdd1"};
      check("TwoPages", 1, testpoints);
      end();
    }

};


TestPageManagement test_pm;
//@+node:michael.20150101205559.38: ** TestSuite
BOOST_AUTO_TEST_SUITE(trie_manipulations)

BOOST_AUTO_TEST_CASE(CompressAddNew) {
  test_compress.test_CompressAddNew();
}

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
  test_trie.test_BitTrieAdd2();
}

BOOST_AUTO_TEST_CASE(BitTrieAdd1) {
  test_trie.test_BitTrieAdd1();
}
  
BOOST_AUTO_TEST_CASE(BitTrieAdd0) {
  test_trie.test_BitTrieAdd0();
}
  
BOOST_AUTO_TEST_CASE(TrieBaseAdd0) {
  test_trie.test_TrieBaseAdd0();
}
  
BOOST_AUTO_TEST_CASE(TrieBaseAdd2) {
  test_trie.test_TrieBaseAdd2();
}
  
BOOST_AUTO_TEST_CASE(TrieBaseAdd3) {
  test_trie.test_TrieBaseAdd3();
}

BOOST_AUTO_TEST_CASE(TrieRemove) {
  test_trie.test_TrieRemove();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove0) {
  test_trie.test_BitTrieRemove0();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove1) {
  test_trie.test_BitTrieRemove1();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove2) {
  test_trie.test_BitTrieRemove2();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove3_8) {
  test_trie.test_BitTrieRemove3_8();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove3_24) {
  test_trie.test_BitTrieRemove3_24();
}

BOOST_AUTO_TEST_CASE(BitTrieRemove3_40) {
  test_trie.test_BitTrieRemove3_40();
}

BOOST_AUTO_TEST_CASE(test_BitTrieChange0) {
  test_trie.test_BitTrieChange0();
}

BOOST_AUTO_TEST_SUITE_END()

/*
BOOST_AUTO_TEST_SUITE(page_overflow)

BOOST_AUTO_TEST_CASE(test_TwoPages) {
  test_pm.test_TwoPages();
}


BOOST_AUTO_TEST_SUITE_END()

*/

#ifdef BOOST_TEST_NO_MAIN
int main(int argc, const char* argv[]) {

  //test_compress.test_CompressAddNew();
  //test_compress.test_CompressAdd0_CompressReinsert0();
  //test_compress.test_CompressAdd0_CompressReinsert1();
  //test_compress.test_CompressAdd0_CompressReinsert2();
  //test_compress.test_CompressAdd1_CompressReinsert0();
  //test_compress.test_CompressAdd1_CompressReinsert1();
  //test_compress.test_CompressAdd1_CompressReinsert2();
  //test_compress.test_CompressAdd2_CompressReinsert0();
  //test_compress.test_CompressAdd2_CompressReinsert1();
  //test_compress.test_CompressAdd2_CompressReinsert2();
  //test_leaf.test_LeafAdd0();
  //test_leaf.test_LeafAdd1();
  //test_leaf.test_LeafAdd2();
  //test_trie.test_BitTrieAdd2();
  //test_trie.test_BitTrieAdd1();
  //test_trie.test_BitTrieAdd0();
  //test_trie.test_TrieBaseAdd0();
  //test_trie.test_TrieBaseAdd2();
  //test_trie.test_TrieBaseAdd3();
  //test_trie.test_TrieRemove();
  //test_trie.test_BitTrieRemove0();
  //test_trie.test_BitTrieRemove1();
  //test_trie.test_BitTrieRemove2();
  //test_trie.test_BitTrieRemove3_8();
  //test_trie.test_BitTrieRemove3_24();
  //test_trie.test_BitTrieRemove3_40();
  //test_trie.test_BitTrieChange0();
  test_pm.test_TwoPages();
  return 0;
}


// set_terminate example
#include <iostream>       // std::cerr
#include <exception>      // std::set_terminate
#include <cstdlib>        // std::abort
#include <unistd.h>
#include <signal.h>


void segsev(int) {
  std::cerr << "segmentation fault: "<< getpid() << "\n";
  sleep(10000);
}

void myterminate () {
  std::cerr << "terminate handler called: "<< getpid() << "\n";
  abort();  // forces abnormal termination
}


struct SetTerminate {
  SetTerminate() {
    std::cerr << "terminate handler installed\n";
    //set_terminate(myterminate);
    signal(SIGSEGV, segsev);
  }
};

SetTerminate s;

#endif
//@-others

//@-leo
