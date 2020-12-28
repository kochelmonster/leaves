#define BOOST_TEST_MODULE StorageTest
//#define BOOST_TEST_NO_MAIN
#include <cstdio>
#include <boost/test/included/unit_test.hpp>


#define GENERATE

#ifndef CMPFILES
#define CMPFILES "."
#endif


#define AREA_COUNT 100
#include "../src/trace.hpp"
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
  out << handler_names[ptr.type] << "-" << ptr.segment_id << "-" << (ptr.delta & 0xFFFFFFFC);
  return out;
}

struct DumpBase {
  virtual void dump(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper=-1) = 0;
};


struct NullDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    out << "id: " << ptr << std::endl;
  }
};

void dump_node(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper=-1);

struct ValueDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    ValueData *data = (ValueData*)ptr.resolve(storage);
    out << "id: " << ptr << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "value: \"";
    for(size_t i = 0; i < data->size; i++) {
      out << "[" << std::hex << data->value[i] << std::dec << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << data->next << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    CompressedData *data = (CompressedData*)ptr.resolve(storage);
    out << "id: " << ptr << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "keys: \"";
    for(int i = 0; i < data->size; i++) {
      out << "[" << std::hex << data->keys[i] << std::dec << "]";
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
  void dump(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    TrieData *data = (TrieData*)ptr.resolve(storage);
    int size = popcount(data->bits);
    out << "id: " << ptr << std::endl;
    out << "size: " << popcount(data->bits) << std::endl;
    out << "bits: " << std::hex << data->bits << std::dec << std::endl;

    int indizes[17];
    out << "bitindex: [";
    unsigned int bits = data->bits;
    int index = 0, i = 0;
    while(bits){
      index = ctz(bits);
      out << index;
      indizes[i++] = index;
      bits &= ~(1 << index);
      if (bits) {
        out << ", ";
      }
    }
    out << "]" << std::endl;

    if (upper >= 0) {
      upper <<= 4;
      out << "byteindex: [";
      unsigned int bits = data->bits;
      int index = 0;
      while(bits){
        index = ctz(bits);
        out << '"' << (char)(upper | index) << '"';
        bits &= ~(1 << index);
        if (bits) {
          out << ", ";
        }
      }
      out << "]" << std::endl;
    }

    out << "children: " << std::endl;
    for(int i = 0; i < size; i++) {
        out << "  - " << data->children[i] << std::endl;
    }
    out << "---" << std::endl;
    for(int i = 0; i < size; i++) {
      dump_node(out, data->children[i], storage, indizes[i]);
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


void dump_node(std::ofstream& out, segment_ptr ptr, Storage* storage, int upper) {
  dumpers[ptr.type]->dump(out, ptr, storage, upper);
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


void insert(Storage& storage, const Slice& key, const char* test_name) {
  Trace trace(storage);
  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(!trace.valid());

  trace.set_value(key);
  check_graph(test_name, storage);
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.current_key, key.string());
}

void test_insert_first(Storage& storage) {
  check_graph("empty", storage);
  insert(storage, Slice("abcdefg"), "first");
}

void test_divide_compressed(Storage& storage) {
  insert(storage, Slice("abhij"), "divide_compressed");
}

void test_add_value_node(Storage& storage) {
  insert(storage, Slice("ab"), "value_to_trie");
}

void test_insert_index(Storage& storage) {
  insert(storage, "abd", "insert_index_abd");
}

void test_insert_grow(Storage& storage) {
  insert(storage, "aba", "insert_index_a");
  insert(storage, "abb", "insert_index_b");
  insert(storage, "abe", "insert_index_e");
  insert(storage, "abf", "insert_index_f");
  insert(storage, "abg", "insert_index_g");
  insert(storage, "abi", "insert_index_i");
  insert(storage, "abj", "insert_index_j");
  insert(storage, "abk", "insert_index_k");
  insert(storage, "abl", "insert_index_l");
  insert(storage, "abm", "insert_index_m");
  insert(storage, "abn", "insert_index_n");
  insert(storage, "abo", "insert_index_o");
  insert(storage, "ab`", "insert_index_60");
}

BOOST_AUTO_TEST_SUITE(NullNode)

BOOST_AUTO_TEST_CASE(insert) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(CompressedNode)

BOOST_AUTO_TEST_CASE(trie_divide) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
}

BOOST_AUTO_TEST_CASE(value_divide) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  insert(storage, Slice("ab"), "divide_compressed_value");
}

BOOST_AUTO_TEST_CASE(very_big) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);

  std::string key;
  for(int i = 0; i < 20; i++) {
    key.append("abcdefghijklmn");
  }
  insert(storage, Slice(key), "very_big");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ValueNode)

BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);

  Slice key("abcdefg");
  Trace trace(storage);
  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(trace.valid());

  std::string value;
  for(int i = 0; i < 20; i++) {
    value.append("abcdefghijklmn");
  }
  trace.set_value(value);

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  trace.set_value(key);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TriedNode)

BOOST_AUTO_TEST_CASE(add_value_node) {
  // add a value to trie
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_add_value_node(storage);

  Trace trace(storage);
  Slice key("abhij");
  trace.find(key);
  BOOST_REQUIRE(trace.valid());
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), key.string());
}

BOOST_AUTO_TEST_CASE(insert_index) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
}

BOOST_AUTO_TEST_CASE(insert_grow_lower) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  test_insert_index(storage);
  test_insert_grow(storage);
}


BOOST_AUTO_TEST_CASE(insert_grow_upper) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
  test_divide_compressed(storage);
  insert(storage, Slice("abp"), "insert_index_p");
  insert(storage, Slice("ab0"), "insert_index_0");
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(Error)

BOOST_AUTO_TEST_SUITE_END()
