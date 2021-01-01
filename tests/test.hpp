#ifndef CMPFILES
#define CMPFILES "."
#endif


#define AREA_COUNT 100
#include "../src/trace.hpp"
#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;


#define SEGMENT_SIZE 1024*16


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
void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage);
}


inline void dump_graph(const char* output, Storage& storage) {
  std::ofstream out(output);
  dump_node(out, *storage.start, &storage);
}


inline void compare_graph(const char* input, Storage& storage) {
  std::stringstream cstr;
  dump_node(cstr, *storage.start, &storage);

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
  BOOST_REQUIRE_EQUAL(trace.current_key, key.string());
}
