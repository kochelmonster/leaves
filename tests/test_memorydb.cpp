//@+leo-ver=5-thin
//@+node:michael.20150106224503.8: * @file test_memorydb.cpp
//@@language cplusplus
//@@tabwidth -2
#define BOOST_TEST_NO_MAIN
#define GENERATE
//@+<< includes >>
//@+node:michael.20150106224503.9: ** << includes >>
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
//@+node:michael.20150106224503.20: ** Test Utils
namespace larch_leaves {

void testpoint(const char* str) {
}

}

#ifdef GENERATE

std::ostream& out(std::cout);

#undef BOOST_REQUIRE
#define BOOST_REQUIRE(x) assert(x)
 
#else

std::stringstream dumy;
std::ostream& out(dumy);
  
#endif
//@+node:michael.20150106224503.19: ** TestMemoryDB
struct TestMemoryDB {
  MemoryDatabase *db;
  
  std::string number(int number, size_t size=0) {
      std::stringstream f;
      f << std::setw(size) << std::setfill('0') << number;
      return f.str();
    }
    
  void test_Access()  {
      int i;
      std::unique_ptr<MemoryDatabase> db(MemoryDatabase::create());
      
      for(i = 0; i < 10000; i+=2) {
        std::string n = number(i, 6);
        db->find(n);
        db->set_value(n);
      }
      out << "generated! " << db->count() << std::endl;
      BOOST_REQUIRE(db->count() == 5000);

      for(db->first(), i = 0; db->is_valid(); db->next(), i+=2) {
        std::string n = number(i, 6);
        BOOST_REQUIRE(db->key() == n);
        BOOST_REQUIRE(db->value() == n);
      }
      
      BOOST_REQUIRE(i == 10000);
      out << "forward iteration passed" << std::endl;
      
      for(db->last(), i = 10000-2; db->is_valid(); db->prev(), i-=2) {
        std::string n = number(i, 6);
        BOOST_REQUIRE(db->key() == n);
        BOOST_REQUIRE(db->value() == n);
      }
      
      BOOST_REQUIRE(i == -2);
      out << "backward iteration passed" << std::endl;
            
      std::string n = number(1000, 6);
      db->find(n);
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == n);
      BOOST_REQUIRE(db->value() == n);
      
      n = number(1001, 6);
      db->find(n);
      BOOST_REQUIRE(!db->is_valid());
      db->next();
      n = number(1002, 6);
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == n);
      BOOST_REQUIRE(db->value() == n);
     
      n = number(1001, 6);
      db->find(n);
      BOOST_REQUIRE(!db->is_valid());
      db->prev();
      n = number(1000, 6);
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == n);
      BOOST_REQUIRE(db->value() == n);
      out << "find passed" << std::endl;
      
      for(db->first(); db->is_valid(); db->next())
        db->remove();
      
      out << "removed passed" << db->count() << std::endl;
      for(db->first(); db->is_valid(); db->next())
        BOOST_REQUIRE(0);
        
      out << "removed passed" << db->count() << std::endl;
      BOOST_REQUIRE(db->count() == 0);
    }

};


TestMemoryDB test_mdb;
//@+node:michael.20150106224503.18: ** TestSuite

BOOST_AUTO_TEST_CASE(Access) {
  test_mdb.test_Access();
}


#ifdef BOOST_TEST_NO_MAIN
int main(int argc, const char* argv[]) {
  test_mdb.test_Access();
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
    std::set_terminate(myterminate);
    signal(SIGSEGV, segsev);
  }
};

SetTerminate s;

#endif
//@-others

//@-leo
