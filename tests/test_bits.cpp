#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE BitTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_bits.hpp"

using namespace leaves;
using namespace leaves::bits;

BOOST_AUTO_TEST_CASE(test_isset_and_set) {
  uint64_t test = 0;
  
  BOOST_CHECK(!isset(test, 5));
  
  set(test, 5);
  BOOST_CHECK(isset(test, 5));
  BOOST_CHECK(!isset(test, 6));
  
  set(test, 0);
  BOOST_CHECK(isset(test, 0));
  BOOST_CHECK(isset(test, 5));
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
