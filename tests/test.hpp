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
  virtual void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) = 0;
};


struct NullDumper : public DumpBase {
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    out << "id: " << ptr << std::endl;
  }
};

void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1);

struct ValueDumper : public DumpBase {
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
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
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
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
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
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


void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage, int upper) {
  dumpers[ptr.type]->dump(out, ptr, storage, upper);
}


void dump_graph(const char* output, Storage& storage) {
  std::ofstream out(output);
  dump_node(out, storage.start, &storage);
}

void compare_graph(const char* input, Storage& storage) {
  std::stringstream cstr;
  dump_node(cstr, storage.start, &storage);

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


void check_graph(const char* name, Storage& storage) {
  std::string path(CMPFILES);
  path.append(name);
  path.append(".yaml");

#ifdef GENERATE
  dump_graph(path.c_str(), storage);
#else
  compare_graph(path.c_str(), storage);
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
