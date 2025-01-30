#include <chrono>
#include <fstream>
#include <iostream>
#include "leaves/intern/_mmap.hpp"

#define TEST_FILE "test.lvs"

using namespace leaves;

const size_t COUNT = 1000000;


struct Preparation {
  Preparation() { std::remove(TEST_FILE); }

  ~Preparation() {
    std::remove(TEST_FILE);
    std::cout << "remove " << TEST_FILE << std::endl;
  }
};

void create() {
  DBMMap storage(TEST_FILE);
  Cursor cursor(storage);

  std::string val = std::string(100, 1);
  leaves::Slice mkey, mval(val);
  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < COUNT; i++) {
    char key[100];
    snprintf(key, sizeof(key), "%016d", (int)i);
    mkey = leaves::Slice(key);
    cursor.find(mkey);
    val[0] = (char)i;
    cursor.value(mval);
    if (i % 1000 == 0 && i > 0) {
      cursor.commit();
    }
  }
  cursor.commit();

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> duration = end - start;
  std::cout << "Inserted: " << COUNT << " items in " << duration.count()
            << " ms" << std::endl;

#ifdef DEBUG
// dump_graph("debug.yaml", db);
#endif
}

void read() {
  DBMMap storage(TEST_FILE);
  Cursor cursor(storage);

  std::string val = std::string(100, 1);
  leaves::Slice mkey, mval(val);
  auto start = std::chrono::high_resolution_clock::now();

  for(int j = 0; j < 10; j++) {
    size_t i = 0;
    for (cursor.first(); cursor.is_valid(); cursor.next(), i++) {
      Slice val = cursor.value();
      if (val.size() != 100) {
        std::cout << "wrong" << std::endl;
      }
      // if (i % 1000 == 0 && i > 0) {
      // }
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> duration = end - start;
  std::cout << "Read: " << COUNT << " items in " << duration.count() << " ms"
            << std::endl;

#ifdef DEBUG
// dump_graph("debug.yaml", db);
#endif
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " -c (create) | -r (read)"
              << std::endl;
    return 1;
  }

  std::string option = argv[1];

  if (option == "-c") {
    create();
  } else if (option == "-r") {
    read();
  } else {
    std::cerr << "Invalid option. Use -c to create or -r to read." << std::endl;
    return 1;
  }

  return 0;
}