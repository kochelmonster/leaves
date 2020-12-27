#define BOOST_TEST_MODULE StorageTest
//#define BOOST_TEST_NO_MAIN
#include <cstdio>
#include <boost/test/included/unit_test.hpp>


#define GENERATE

#ifndef CMPFILES
#define CMPFILES "."
#endif


#define AREA_COUNT 100
#include "../src/storage.hpp"

#include "../src/node.cpp"


#define TEST_FILE "test.lvs"

using namespace larch_leaves;


#define SEGMENT_SIZE 1024*16


struct Preparation {
  Preparation() {
    std::remove(TEST_FILE);
  }

  ~Preparation() {
    std::remove(TEST_FILE);
  }
};


const char* handler_names[] = {
  "kValue",
  "kNull",
  "kCompressed",
  "kTrie",
};


std::ostream& operator<<(std::ostream& out, segment_ptr ptr) {
  out << handler_names[ptr.type] << "-" << ptr.segment_id << "-" << (ptr.delta & 0xFFFFFFF0);
  return out;
}

struct DumpBase {
  virtual void dump(std::ofstream& out, segment_ptr ptr, Storage* storage) = 0;
};


struct NullDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage) {
    out << "id: " << ptr << std::endl;
  }
};

void dump_node(std::ofstream& out, segment_ptr ptr, Storage* storage);

struct ValueDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage) {
    ValueData *data = (ValueData*)ptr.resolve(storage);
    out << "id: " << ptr << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "children: " << std::endl;
    out << "  - " << data->next << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage) {
    CompressedData *data = (CompressedData*)ptr.resolve(storage);
    out << "id: " << ptr << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "keys: \"";
    for(int i = 0; i < data->size; i++) {
      out << "[" << std::hex << data->keys[i] << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << data->next << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
  }
};

struct TrieDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage) {
    TrieData *data = (TrieData*)ptr.resolve(storage);
    int size = popcount(data->bits);
    out << "id: " << ptr << std::endl;
    out << "size: " << pocount(data->bits) << std::endl;
    out << "bits: " << std::hex << data->bits << std::endl;
    out << "children: " << std::endl;
    for(int i = 0; i <= size; i++) {
        out << "  - " << data->children[i] << std::endl;
    }
    out << "---" << std::endl;
    for(int i = 0; i <= size; i++) {
      if (data->children[i]) {
        dump_node(out, data->children[i], storage);
      }
    }
  }
};

ValueDumper value_dumper;
NullDumper null_dumper;
CompressDumper compress_dumper;
TrieDumper trie_dumper;

DumpBase* dumpers[] = {
  &value_dumper,
  &null_dumper,
  &compress_dumper,
  &trie_dumper
};


void dump_node(std::ofstream& out, segment_ptr ptr, Storage* storage) {
  dumpers[ptr.type]->dump(out, ptr, storage);
}


void dump_graph(const char* output, Storage& storage) {
  std::ofstream out(output);
  dump_node(out, storage.start, &storage);
}


void check_graph(const char* name, Storage& storage) {
  std::string path(CMPFILES);
  path.append(name);
  path.append(".yaml");

#ifdef GENERATE
  dump_graph(path.c_str(), storage);
#else

#endif
}



void test_insert_first(Storage& storage) {
  Slice key("abcdefg");
  Transition trans(&storage.start, &storage);

  check_graph("empty", storage);

  segment_ptr *result = trans.find(key);
  BOOST_REQUIRE_EQUAL(result, (segment_ptr*)NULL);

  result = trans.insert(key, key);
  storage.start = *result;

  BOOST_REQUIRE_EQUAL(result->type, kCompressed);
  BOOST_REQUIRE_EQUAL(storage.start.type, kCompressed);

  CompressedData* comp = compressed.ptr(trans);
  BOOST_REQUIRE_EQUAL(comp->size, 7);
  BOOST_REQUIRE(! memcmp(comp->keys, key.data(), key.size()));
  BOOST_REQUIRE_EQUAL(comp->next.type, kValue);

  Transition vtrans(&comp->next, &storage);
  ValueData *value = value_handler.ptr(vtrans);
  BOOST_REQUIRE_EQUAL(value->size, 7);
  BOOST_REQUIRE(! memcmp(value->value, key.data(), key.size()));
  BOOST_REQUIRE_EQUAL(value->next.type, kNull);
  BOOST_REQUIRE(!value->next);

  check_graph("insert_first", storage);
  // std::cout << "new " << result->delta << ", " << result->segment_id << std::endl;
}


void test_divide_compressed(Storage& storage) {
  Slice key("abhij");
  Transition trans(&storage.start, &storage);

  segment_ptr *result = trans.find(key);
  BOOST_REQUIRE_EQUAL(result, (segment_ptr*)NULL);

  result = trans.insert(key, key);
  storage.start = *result;

  BOOST_REQUIRE_EQUAL(result->type, kCompressed);
  BOOST_REQUIRE_EQUAL(storage.start.type, kCompressed);

  CompressedData* comp = compressed.ptr(trans);
  BOOST_REQUIRE_EQUAL(comp->size, 2);
  BOOST_REQUIRE(! memcmp(comp->keys, key.data(), comp->size));
  BOOST_REQUIRE_EQUAL(comp->next.type, kTrie);

  check_graph("divide_compressed", storage);

  //std::cout << "bits " << dtrie->bits << std::endl;
}


BOOST_AUTO_TEST_SUITE(NullNode)

BOOST_AUTO_TEST_CASE(find_and_insert) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CompressedNode)

BOOST_AUTO_TEST_CASE(find_and_divide) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
}


BOOST_AUTO_TEST_SUITE_END()
