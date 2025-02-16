#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE BitTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_bits.hpp"

using namespace leaves;
using namespace leaves::bits;

BOOST_AUTO_TEST_CASE(test_set_and_get) {
  _BitField bf;
  bf.init();
  bf.set(5);
  bf.set(70);
  bf.set(130);

  BOOST_CHECK(bf.get(5));
  BOOST_CHECK(bf.get(70));
  BOOST_CHECK(bf.get(130));
  BOOST_CHECK(!bf.get(6));
  BOOST_CHECK(!bf.get(71));
  BOOST_CHECK(!bf.get(131));
}

BOOST_AUTO_TEST_CASE(test_count) {
  _BitField bf;
  bf.init();
  bf.set(5);
  bf.set(70);
  bf.set(130);

  BOOST_CHECK_EQUAL(bf.count(), 3);
}

BOOST_AUTO_TEST_CASE(test_index) {
  _BitField bf;
  bf.init();
  bf.set(5);
  bf.set(70);
  bf.set(130);

  BOOST_CHECK_EQUAL(bf.index(5), 0);
  BOOST_CHECK_EQUAL(bf.index(70), 1);
  BOOST_CHECK_EQUAL(bf.index(130), 2);
}

BOOST_AUTO_TEST_CASE(test_first_and_last) {
  _BitField bf;
  bf.init();
  bf.set(5);
  bf.set(70);
  bf.set(130);

  BOOST_CHECK_EQUAL(bf.first(), 5);
  BOOST_CHECK_EQUAL(bf.last(), 130);

  bf.init();
  BOOST_CHECK_EQUAL(bf.first(), -1);
  BOOST_CHECK_EQUAL(bf.last(), -1);
}

BOOST_AUTO_TEST_CASE(test_next_and_prev) {
  _BitField bf;
  bf.init();
  bf.set(5);
  bf.set(70);
  bf.set(130);

  BOOST_CHECK_EQUAL(bf.next(4), 5);
  BOOST_CHECK_EQUAL(bf.next(5), 70);
  BOOST_CHECK_EQUAL(bf.next(60), 70);
  BOOST_CHECK_EQUAL(bf.next(70), 130);
  BOOST_CHECK_EQUAL(bf.next(129), 130);
  BOOST_CHECK_EQUAL(bf.next(130), -1);

  BOOST_CHECK_EQUAL(bf.prev(140), 130);
  BOOST_CHECK_EQUAL(bf.prev(130), 70);
  BOOST_CHECK_EQUAL(bf.prev(100), 70);
  BOOST_CHECK_EQUAL(bf.prev(70), 5);
  BOOST_CHECK_EQUAL(bf.prev(60), 5);
  BOOST_CHECK_EQUAL(bf.prev(5), -1);
  BOOST_CHECK_EQUAL(bf.prev(3), -1);
}

BOOST_AUTO_TEST_CASE(test_clear) {
  _BitField bf;
  bf.init();
  bf.set(5);
  bf.set(70);
  bf.set(130);

  bf.clear(70);
  BOOST_CHECK(bf.get(5));
  BOOST_CHECK(!bf.get(70));
  BOOST_CHECK(bf.get(130));
  BOOST_CHECK_EQUAL(bf.count(), 2);
}

typedef _SparseArray<int> SA;

BOOST_AUTO_TEST_CASE(sparse_test_set_and_get) {
  union {
    SA sa;
    char buffer[SA::space(3)];
  };

  sa.init();
  sa.set(5, 100);
  sa.set(70, 200);
  sa.set(130, 300);

  BOOST_CHECK_EQUAL(sa[5], 100);
  BOOST_CHECK_EQUAL(sa[70], 200);
  BOOST_CHECK_EQUAL(sa[130], 300);

  sa.set(130, 400);
  BOOST_CHECK_EQUAL(sa[130], 400);
}

BOOST_AUTO_TEST_CASE(sparse_test_iterator) {
  union {
    SA sa;
    char buffer[SA::space(3)];
  };
  sa.init();
  sa.set(5, 100);
  sa.set(70, 200);
  sa.set(130, 300);

  auto it = sa.begin();
  BOOST_CHECK_EQUAL(*it, 100);
  ++it;
  BOOST_CHECK_EQUAL(*it, 200);
  ++it;
  BOOST_CHECK_EQUAL(*it, 300);
  ++it;
  BOOST_CHECK(it == sa.end());
}

BOOST_AUTO_TEST_CASE(sparse_test_count) {
  union {
    SA sa;
    char buffer[SA::space(3)];
  };
  sa.init();
  sa.set(5, 100);
  sa.set(70, 200);
  sa.set(130, 300);

  BOOST_CHECK_EQUAL(sa.bits.count(), 3);
  BOOST_CHECK_EQUAL(sa[5], 100);
  BOOST_CHECK_EQUAL(sa[70], 200);
  BOOST_CHECK_EQUAL(sa[130], 300);
}

BOOST_AUTO_TEST_CASE(sparse_test_remove) {
  union {
    SA sa;
    char buffer[SA::space(3)];
  };
  sa.init();
  sa.set(5, 100);
  sa.set(70, 200);
  sa.set(130, 300);

  BOOST_CHECK_EQUAL(sa.bits.count(), 3);
  BOOST_CHECK_EQUAL(sa[5], 100);
  BOOST_CHECK_EQUAL(sa[70], 200);
  BOOST_CHECK_EQUAL(sa[130], 300);

  sa.remove(70);
  BOOST_CHECK_EQUAL(sa.bits.count(), 2);
  BOOST_CHECK_EQUAL(sa[5], 100);
  BOOST_CHECK_EQUAL(sa[130], 300);
  BOOST_CHECK(!sa.bits.get(70));
}

BOOST_AUTO_TEST_CASE(test_index_bit) {
  uint64_t test = 0b1001;

  BOOST_CHECK_EQUAL(index(test, 0), 0);
  BOOST_CHECK_EQUAL(index(test, 3), 1);
  BOOST_CHECK_EQUAL(index(test, 2), 1);
}

BOOST_AUTO_TEST_CASE(test_first_bit) {
  uint64_t test1 = 0b1000;  // Bit at position 3
  uint64_t test2 = 0b0;     // No bits set
  uint64_t test3 = 0b1;     // First bit set

  BOOST_CHECK_EQUAL(first(test1), 3);
  BOOST_CHECK_EQUAL(first(test2), -1);
  BOOST_CHECK_EQUAL(first(test3), 0);
}

BOOST_AUTO_TEST_CASE(test_last_bit) {
  uint64_t test1 = 0b1000;  // Bit at position 3
  uint64_t test2 = 0b0;     // No bits set
  uint64_t test3 = 0b1001;  // Bits at position 0 and 3

  BOOST_CHECK_EQUAL(last(test1), 3);
  BOOST_CHECK_EQUAL(last(test2), -1);
  BOOST_CHECK_EQUAL(last(test3), 3);
}

BOOST_AUTO_TEST_CASE(test_next_bit) {
  uint64_t test1 = 0b1011;  // Bits at positions 0, 1, and 3

  BOOST_CHECK_EQUAL(next(test1, 0), 1);   // Next after position 0
  BOOST_CHECK_EQUAL(next(test1, 1), 3);   // Next after position 1
  BOOST_CHECK_EQUAL(next(test1, 3), -1);  // No next bit

  uint64_t test2 = 0b0;  // No bits set
  BOOST_CHECK_EQUAL(next(test2, 0), -1);
}

BOOST_AUTO_TEST_CASE(test_prev_bit) {
  uint64_t test1 = 0b1011;  // Bits at positions 0, 1, and 3

  BOOST_CHECK_EQUAL(prev(test1, 4), 3);   // Previous from position 4
  BOOST_CHECK_EQUAL(prev(test1, 3), 1);   // Previous from position 3
  BOOST_CHECK_EQUAL(prev(test1, 1), 0);   // Previous from position 1
  BOOST_CHECK_EQUAL(prev(test1, 0), -1);  // No previous bit

  uint64_t test2 = 0b0;  // No bits set
  BOOST_CHECK_EQUAL(prev(test2, 4), -1);
}

// Test with different bit widths
BOOST_AUTO_TEST_CASE(test_different_bit_widths) {
  uint32_t test32 = 0b1000;
  uint16_t test16 = 0b1000;
  uint8_t test8 = 0b1000;

  BOOST_CHECK_EQUAL(first<uint32_t>(test32), 3);
  BOOST_CHECK_EQUAL(first<uint16_t>(test16), 3);
  BOOST_CHECK_EQUAL(first<uint8_t>(test8), 3);

  BOOST_CHECK_EQUAL(last<uint32_t>(test32), 3);
  BOOST_CHECK_EQUAL(last<uint16_t>(test16), 3);
  BOOST_CHECK_EQUAL(last<uint8_t>(test8), 3);
}

BOOST_AUTO_TEST_CASE(test_count_uint64) {
  // Test empty bits
  BOOST_CHECK_EQUAL(count<uint64_t>(0), 0);

  // Test single bit
  BOOST_CHECK_EQUAL(count<uint64_t>(1), 1);
  BOOST_CHECK_EQUAL(count<uint64_t>(0x8000000000000000), 1);

  // Test multiple bits
  BOOST_CHECK_EQUAL(count<uint64_t>(0x3), 2);  // 0b11
  BOOST_CHECK_EQUAL(count<uint64_t>(0xF), 4);  // 0b1111

  // Test alternating bits
  BOOST_CHECK_EQUAL(count<uint64_t>(0x5555555555555555), 32);  // 101010...
  BOOST_CHECK_EQUAL(count<uint64_t>(0xAAAAAAAAAAAAAAAA), 32);  // 010101...

  // Test all bits set
  BOOST_CHECK_EQUAL(count<uint64_t>(0xFFFFFFFFFFFFFFFF), 64);
}

BOOST_AUTO_TEST_CASE(test_count_uint32) {
  // Similar tests for 32-bit integers
  BOOST_CHECK_EQUAL(count<uint32_t>(0), 0);
  BOOST_CHECK_EQUAL(count<uint32_t>(1), 1);
  BOOST_CHECK_EQUAL(count<uint32_t>(0x80000000), 1);
  BOOST_CHECK_EQUAL(count<uint32_t>(0xFFFFFFFF), 32);
}

BOOST_AUTO_TEST_CASE(test_count_uint16) {
  // Tests for 16-bit integers
  BOOST_CHECK_EQUAL(count<uint16_t>(0), 0);
  BOOST_CHECK_EQUAL(count<uint16_t>(0xFFFF), 16);
}

BOOST_AUTO_TEST_CASE(test_count_uint8) {
  // Tests for 8-bit integers
  BOOST_CHECK_EQUAL(count<uint8_t>(0), 0);
  BOOST_CHECK_EQUAL(count<uint8_t>(0xFF), 8);
}
