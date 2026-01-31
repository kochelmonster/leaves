#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE test_serial

#include <boost/test/included/unit_test.hpp>
#include "../include/leaves/intern/core/_serial.hpp"

using namespace leaves;

BOOST_AUTO_TEST_CASE(test_serial_initialization) {
  serial32 s1;
  BOOST_CHECK_EQUAL(s1.value(), 0u);
  BOOST_CHECK(!s1.is_valid());
  BOOST_CHECK(!s1);
  
  serial32 s2(1);
  BOOST_CHECK_EQUAL(s2.value(), 1u);
  BOOST_CHECK(s2.is_valid());
  BOOST_CHECK(s2);
  
  serial32 s3(0xFFFFFFFF);
  BOOST_CHECK_EQUAL(s3.value(), 0xFFFFFFFFu);
  BOOST_CHECK(s3.is_valid());
}

BOOST_AUTO_TEST_CASE(test_serial_increment) {
  serial32 s(1);
  ++s;
  BOOST_CHECK_EQUAL(s.value(), 2u);
  
  s = serial32(0xFFFFFFFF);
  ++s;
  BOOST_CHECK_EQUAL(s.value(), 1u);  // Wraps to 1, skipping 0
  
  s = serial32(0xFFFFFFFE);
  s++;
  BOOST_CHECK_EQUAL(s.value(), 0xFFFFFFFFu);
  s++;
  BOOST_CHECK_EQUAL(s.value(), 1u);
}

BOOST_AUTO_TEST_CASE(test_serial_decrement) {
  serial32 s(10);
  --s;
  BOOST_CHECK_EQUAL(s.value(), 9u);
  
  s = serial32(1);
  --s;
  BOOST_CHECK_EQUAL(s.value(), 0xFFFFFFFFu);  // Wraps to MAX, skipping 0
  
  s = serial32(2);
  s--;
  BOOST_CHECK_EQUAL(s.value(), 1u);
  s--;
  BOOST_CHECK_EQUAL(s.value(), 0xFFFFFFFFu);
}

BOOST_AUTO_TEST_CASE(test_serial_addition) {
  serial32 s(10);
  s += 5;
  BOOST_CHECK_EQUAL(s.value(), 15u);
  
  // Test overflow wrapping
  s = serial32(0xFFFFFFFE);
  s += 1;
  BOOST_CHECK_EQUAL(s.value(), 0xFFFFFFFFu);
  
  s = serial32(0xFFFFFFFE);
  s += 2;
  BOOST_CHECK_EQUAL(s.value(), 1u);  // Wraps, skips 0
  
  s = serial32(0xFFFFFFFE);
  s += 3;
  BOOST_CHECK_EQUAL(s.value(), 2u);  // 0xFFFFFFFE + 3 = 1 (wraps and skips 0)
  
  // Test the formula: (a + b - 1) % MAX + 1
  s = serial32(0xFFFFFFFF);
  s += 1;
  BOOST_CHECK_EQUAL(s.value(), 1u);
  
  s = serial32(1);
  serial32 s2 = s + 0xFFFFFFFF;
  BOOST_CHECK_EQUAL(s2.value(), 1u);  // Full wrap
}

BOOST_AUTO_TEST_CASE(test_serial_overflow_ordering_basic) {
  // Normal ordering
  serial32 s1(1);
  serial32 s2(2);
  BOOST_CHECK(s1 < s2);
  BOOST_CHECK(s2 > s1);
  BOOST_CHECK(s1 <= s2);
  BOOST_CHECK(s2 >= s1);
  
  serial32 s3(100);
  serial32 s4(200);
  BOOST_CHECK(s3 < s4);
  BOOST_CHECK(s4 > s3);
}

BOOST_AUTO_TEST_CASE(test_serial_overflow_ordering_wraparound) {
  // Key test: 0xFFFFFFFF < 1 (wraparound case)
  serial32 s1(0xFFFFFFFF);
  serial32 s2(1);
  BOOST_CHECK(s1 < s2);
  BOOST_CHECK(s2 > s1);
  
  // More wraparound cases
  serial32 s3(0xFFFFFFFE);
  serial32 s4(2);
  BOOST_CHECK(s3 < s4);
  
  serial32 s5(0xFFFFFFF0);
  serial32 s6(0x10);
  BOOST_CHECK(s5 < s6);
}

BOOST_AUTO_TEST_CASE(test_serial_ordering_window) {
  // Within the comparison window (< 2^31 apart)
  serial32 s1(100);
  serial32 s2(100 + 1000);
  BOOST_CHECK(s1 < s2);
  
  // At the edge of the window
  serial32 s3(100);
  serial32 s4(100 + 0x7FFFFFFFu);  // Exactly at half range
  BOOST_CHECK(s3 < s4);
  
  // Just past the window - ordering reverses!
  // If s1 is 100 and s2 is 100 + 0x80000001, then
  // s2 - s1 > 2^31, so we interpret s2 as being "before" s1
  serial32 s5(100);
  serial32 s6(100 + 0x80000001);
  BOOST_CHECK(s5 > s6);  // Reversed because difference > half range
}

BOOST_AUTO_TEST_CASE(test_serial_equality) {
  serial32 s1(42);
  serial32 s2(42);
  serial32 s3(43);
  
  BOOST_CHECK(s1 == s2);
  BOOST_CHECK(s1 != s3);
  BOOST_CHECK_EQUAL(s1, s2);
  
  // Test with raw values
  BOOST_CHECK(s1 == 42u);
  BOOST_CHECK(s1 != 43u);
}

BOOST_AUTO_TEST_CASE(test_serial_difference) {
  serial32 s1(10);
  serial32 s2(20);
  BOOST_CHECK_EQUAL(s2 - s1, 10);
  BOOST_CHECK_EQUAL(s1 - s2, -10);
  
  // Difference across wraparound
  serial32 s3(0xFFFFFFFE);
  serial32 s4(2);
  BOOST_CHECK_EQUAL(s4 - s3, 4);  // From 0xFFFFFFFE: +1 -> 0xFFFFFFFF, +2 -> 1, +3 -> 2
  BOOST_CHECK_EQUAL(s3 - s4, -4);
  
  serial32 s5(1);
  serial32 s6(0xFFFFFFFF);
  BOOST_CHECK_EQUAL(s5 - s6, 2);  // 1 is 2 steps after 0xFFFFFFFF (wrapping)
}

BOOST_AUTO_TEST_CASE(test_serial_sequence) {
  // Simulate a sequence of transaction IDs
  serial32 txn_id(0xFFFFFFF0);
  
  for (int i = 0; i < 32; ++i) {
    serial32 old_id = txn_id;
    ++txn_id;
    
    // Each new ID should be greater than the previous
    BOOST_CHECK(txn_id > old_id);
    BOOST_CHECK(old_id < txn_id);
  }
  
  // After 32 increments from 0xFFFFFFF0, we should be at 0x11 (17)
  // (0xFFFFFFF0 + 16 = 0xFFFFFFFF + 1 = 1 (skip 0) + 15 = 17 = 0x11)
  BOOST_CHECK_EQUAL(txn_id.value(), 0x11u);
  
  // And it should still be > the starting value (in serial number terms)
  serial32 start(0xFFFFFFF0);
  BOOST_CHECK(txn_id > start);
}

BOOST_AUTO_TEST_CASE(test_serial_zero_handling) {
  // Zero is treated as uninitialized/invalid
  serial32 s1(0);
  serial32 s2(1);
  
  BOOST_CHECK(!s1.is_valid());
  BOOST_CHECK(s2.is_valid());
  
  // Zero should be less than any valid value
  BOOST_CHECK(s1 < s2);
  BOOST_CHECK_EQUAL(s1.value(), 0u);
  
  // Adding to zero initializes it
  s1 += 5;
  BOOST_CHECK_EQUAL(s1.value(), 5u);
  BOOST_CHECK(s1.is_valid());
}

BOOST_AUTO_TEST_CASE(test_serial_max_value_wrap) {
  serial32 s(0xFFFFFFFF);
  
  // Adding 1 to MAX_VALUE wraps to 1
  serial32 s2 = s + 1u;
  BOOST_CHECK_EQUAL(s2.value(), 1u);
  
  // Adding 2 to MAX_VALUE wraps to 2
  serial32 s3 = s + 2u;
  BOOST_CHECK_EQUAL(s3.value(), 2u);
  
  // Adding MAX_VALUE to any number gives the same number (full cycle)
  serial32 s4(42);
  serial32 s5 = s4 + 0xFFFFFFFF;
  BOOST_CHECK_EQUAL(s5.value(), 42u);
}

BOOST_AUTO_TEST_CASE(test_serial_typical_usage) {
  // Simulate typical transaction ID usage
  serial32 current_txn(1);
  serial32 start_txn(1);
  
  // Allocate 1000 transactions
  for (int i = 0; i < 1000; ++i) {
    ++current_txn;
  }
  
  BOOST_CHECK_EQUAL(current_txn.value(), 1001u);
  
  // Check if a transaction is recyclable
  // (txn_id < start_txn means it's old enough to recycle)
  serial32 old_txn(500);
  serial32 recent_txn(900);
  
  start_txn = serial32(600);  // Everything before 600 can be recycled
  
  BOOST_CHECK(old_txn < start_txn);     // Can recycle
  BOOST_CHECK(!(recent_txn < start_txn)); // Cannot recycle (too recent)
}

BOOST_AUTO_TEST_CASE(test_serial_wraparound_scenario) {
  // Start near the end of the range
  serial32 txn(0xFFFFFFF0);
  
  std::vector<uint32_t> sequence;
  for (int i = 0; i < 32; ++i) {
    sequence.push_back(txn.value());
    ++txn;
  }
  
  // Verify sequence wraps correctly: ..., 0xFFFFFFFE, 0xFFFFFFFF, 1, 2, ...
  BOOST_CHECK_EQUAL(sequence[14], 0xFFFFFFFEu);
  BOOST_CHECK_EQUAL(sequence[15], 0xFFFFFFFFu);
  BOOST_CHECK_EQUAL(sequence[16], 1u);
  BOOST_CHECK_EQUAL(sequence[17], 2u);
  
  // All consecutive values should maintain ordering
  for (size_t i = 1; i < sequence.size(); ++i) {
    serial32 prev(sequence[i - 1]);
    serial32 curr(sequence[i]);
    BOOST_CHECK(prev < curr);
  }
}

BOOST_AUTO_TEST_CASE(test_serial_addition_formula) {
  // Test the formula: if txn1 + txn2 > 0xFFFFFFFF
  // then result = ((txn1 + txn2) % 0xFFFFFFFF) + 1
  
  // This is actually: ((txn1 + txn2 - 1) % 0xFFFFFFFF) + 1
  // to ensure we skip 0
  
  serial32 s1(0xFFFFFFFE);
  serial32 s2 = s1 + 3u;
  
  // Manual calculation:
  // sum = 0xFFFFFFFE + 3 = 0x100000001 (> 0xFFFFFFFF)
  // result = ((0x100000001 - 1) % 0xFFFFFFFF) + 1
  //        = (0x100000000 % 0xFFFFFFFF) + 1
  //        = 1 + 1 = 2
  BOOST_CHECK_EQUAL(s2.value(), 2u);
  
  // More examples
  serial32 s3(0xFFFFFFFF);
  serial32 s4 = s3 + 1u;
  BOOST_CHECK_EQUAL(s4.value(), 1u);
  
  serial32 s5(0xFFFFFFFF);
  serial32 s6 = s5 + 0xFFFFFFFF;
  // This should complete a full cycle and return to the same value
  BOOST_CHECK_EQUAL(s6.value(), 0xFFFFFFFFu);
}

BOOST_AUTO_TEST_CASE(test_serial_comparison_with_raw_values) {
  serial32 s(42);
  
  BOOST_CHECK(s < 100u);
  BOOST_CHECK(s > 10u);
  BOOST_CHECK(s == 42u);
  BOOST_CHECK(s != 43u);
  
  // Wraparound comparison with raw values
  serial32 s2(0xFFFFFFFF);
  BOOST_CHECK(s2 < 1u);  // 0xFFFFFFFF < 1 in serial arithmetic
}
