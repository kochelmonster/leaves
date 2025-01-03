/*
g++ -o linear-vs-sorted -O3 linear-vs-sorted.cpp
*/

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>

// Common variables and constants
const int COUNT = 100000;
const int ARRAY_SIZE = 15;

typedef unsigned short test_t;

constexpr ::std::array<test_t, ARRAY_SIZE> generate_chars() {
  ::std::array<test_t, ARRAY_SIZE> result{0};
  int i = 0;
  for (i = 0; i < ARRAY_SIZE; i++) {
    result[i] = (test_t)i;
  };
  return result;
}

auto chars = generate_chars();
const test_t FIND = chars[ARRAY_SIZE / 2];

test_t tst_array[ARRAY_SIZE];
test_t size = 0;

// Insert function for sorted array
inline void insert_sorted(test_t* array, test_t character) {
  auto pos = std::lower_bound(array, array + size, character);
  std::move_backward(pos, array + size, array + size + 1);
  *pos = character;
  size++;
}

// Find function for sorted array
inline test_t find_sorted(test_t* array, test_t character) {
  return std::lower_bound(array, array + size, character) - array;
}

// Find function for linear array
inline test_t find_linear(test_t* array, test_t character) {
  for (test_t i = 0; i < size; i++) {
    if (array[i] == character) {
      return i;
    }
  }
  return size;
}

// Insert function for linear array
inline void insert_linear(test_t* array, test_t character) {
  find_linear(array, character);
  array[size++] = character;
}


int main() {
  // Benchmark for sorted insert and find
  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < COUNT; i++) {
    size = 0;
    for (auto c : chars) {
      insert_sorted(tst_array, c);
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::micro> duration = end - start;
  std::cout << "Time taken for sorted insert: " << duration.count() << " ms"
            << std::endl;

  start = std::chrono::high_resolution_clock::now();
  test_t count = 0;
  for (int i = 0; i < COUNT; i++) {
    count = find_sorted(tst_array, FIND);
  }
  end = std::chrono::high_resolution_clock::now();
  duration = end - start;
  std::cout << "Time taken for sorted find: " << duration.count() << " ms ("
            << count << ")" << std::endl;

  // Benchmark for linear insert and find
  start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < COUNT; i++) {
    size = 0;
    for (auto c : chars) {
      insert_linear(tst_array, c);
    }
  }
  end = std::chrono::high_resolution_clock::now();
  duration = end - start;
  std::cout << "Time taken for linear insert: " << duration.count() << " ms"
            << std::endl;

  start = std::chrono::high_resolution_clock::now();
  count = 0;
  for (int i = 0; i < COUNT; i++) {
    count = find_linear(tst_array, FIND);
  }
  end = std::chrono::high_resolution_clock::now();
  duration = end - start;
  std::cout << "Time taken for linear find: " << duration.count() << " ms ("
            << count << ")" << std::endl;

  return 0;
}