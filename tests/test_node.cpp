#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE TrieNodeTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_node.hpp"

using namespace leaves;
using namespace leaves::bits;

struct TestTraits {
  typedef offset_t offset_e;
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;

  struct BlockHeader {
    typedef BlockHeader Base;
  };
};

typedef _TrieNode<TestTraits> TrieNode;
static const int OOR = TrieNode::OUT_OF_RANGE;

const uint16_t PAGE_SIZE = 4 * 1024;

void set_and_get(const Slice& prefix, uint16_t* sizes, uint16_t* offsets) {
  char buffer1[PAGE_SIZE], buffer2[PAGE_SIZE];
  TrieNode& trie1 = *(TrieNode*)buffer1;
  TrieNode& trie2 = *(TrieNode*)buffer2;
  uint16_t offset;

  offset = trie1.create(prefix, 130);
  *(offset_t*)((char*)&trie1 + offset) = 130;
  BOOST_CHECK(trie1.isset(130));
  BOOST_CHECK_EQUAL(*trie1.offset(130), 130);
  BOOST_CHECK_EQUAL(trie1.size(), *sizes++);
  BOOST_CHECK_EQUAL(offset, *offsets++);

  offset = trie2.create(trie1, 5);
  *(offset_t*)((char*)&trie2 + offset) = 5;
  BOOST_CHECK(trie2.isset(130));
  BOOST_CHECK(trie2.isset(5));
  BOOST_CHECK_EQUAL(*trie2.offset(5), 5);
  BOOST_CHECK_EQUAL(trie2.size(), *sizes++);
  BOOST_CHECK_EQUAL(offset, *offsets++);

  offset = trie1.create(trie2, 70);
  *(offset_t*)((char*)&trie1 + offset) = 70;
  BOOST_CHECK(trie1.isset(130));
  BOOST_CHECK(trie1.isset(5));
  BOOST_CHECK(trie1.isset(70));
  BOOST_CHECK_EQUAL(*trie1.offset(70), 70);
  BOOST_CHECK_EQUAL(trie1.size(), *sizes++);
  BOOST_CHECK_EQUAL(offset, *offsets++);

  offset = trie2.create(trie1, 7);
  *(offset_t*)((char*)&trie2 + offset) = 7;
  BOOST_CHECK(trie2.isset(130));
  BOOST_CHECK(trie2.isset(5));
  BOOST_CHECK(trie2.isset(70));
  BOOST_CHECK(trie2.isset(7));
  BOOST_CHECK_EQUAL(*trie2.offset(7), 7);
  BOOST_CHECK_EQUAL(trie2.size(), *sizes++);
  BOOST_CHECK_EQUAL(offset, *offsets++);

  BOOST_CHECK_EQUAL(trie2.count(), 4);

  BOOST_CHECK_EQUAL(trie2.offset(TrieNode::NONE), nullptr);

  offset = trie1.create(trie2, TrieNode::NONE);
  *(offset_t*)((char*)&trie1 + offset) = 0;
  BOOST_CHECK(trie1.isset(130));
  BOOST_CHECK(trie1.isset(5));
  BOOST_CHECK(trie1.isset(70));
  BOOST_CHECK(trie1.isset(7));
  BOOST_CHECK_EQUAL(*trie1.offset(TrieNode::NONE), 0);
  BOOST_CHECK_EQUAL(trie1.size(), *sizes++);
  BOOST_CHECK_EQUAL(offset, *offsets++);
  BOOST_CHECK_EQUAL(trie1.count(), 5);

  BOOST_CHECK(trie1.has_none());
  BOOST_CHECK(trie1.isset(5));
  BOOST_CHECK(trie1.isset(70));
  BOOST_CHECK(trie1.isset(130));
  BOOST_CHECK(!trie1.isset(6));
  BOOST_CHECK(!trie1.isset(71));
  BOOST_CHECK(!trie1.isset(131));
  BOOST_CHECK_EQUAL(trie1.offset(250), nullptr);

  offset_t* trie_offsets = trie1.array();
  BOOST_CHECK_EQUAL(*trie_offsets, 0);
  BOOST_CHECK_EQUAL(*(trie_offsets + 1), 5);
  BOOST_CHECK_EQUAL(*(trie_offsets + 2), 7);
  BOOST_CHECK_EQUAL(*(trie_offsets + 3), 70);
  BOOST_CHECK_EQUAL(*(trie_offsets + 4), 130);
  BOOST_CHECK(*trie1.offset(5) == 5);
  BOOST_CHECK(*trie1.offset(70) == 70);
  BOOST_CHECK(*trie1.offset(130) == 130);

  offset = trie2.create(trie1, 250);
  *(offset_t*)((char*)&trie2 + offset) = 250;
  BOOST_CHECK_EQUAL(*trie2.offset(250), 250);
  BOOST_CHECK_EQUAL(trie2.size(), *sizes++);
  BOOST_CHECK_EQUAL(offset, *offsets++);
  BOOST_CHECK_EQUAL(trie2.count(), 6);
}

BOOST_AUTO_TEST_CASE(test_set_and_get) {
  //                     130, 5, 70, 7, NONE, 240
  uint16_t sizes1[] = {24, 32, 48, 56, 64, 72};
  uint16_t offsets1[] = {16, 16, 32, 32, 24, 64};
  set_and_get(Slice(), sizes1, offsets1);

  uint16_t sizes2[] = {24, 40, 48, 56, 64, 80};
  uint16_t offsets2[] = {16, 24, 32, 32, 24, 72};
  set_and_get(Slice("1234"), sizes2, offsets2);
}

BOOST_AUTO_TEST_CASE(test_create) {
  char buffer[PAGE_SIZE];
  TrieNode& trie = *(TrieNode*)buffer;
  Slice prefix("123456");

  uint16_t offset = trie.create(prefix, 130);
  *(offset_t*)((char*)&trie + offset) = 130;
  BOOST_CHECK_EQUAL(*trie.offset(130), 130);
  BOOST_CHECK_EQUAL(trie.size(), 24);

  offset = trie.create(prefix, TrieNode::NONE);
  *(offset_t*)((char*)&trie + offset) = 0;
  BOOST_CHECK_EQUAL(*trie.offset(TrieNode::NONE), 0);
  BOOST_CHECK_EQUAL(trie.size(), 24);

  offset = trie.create(prefix, 5, 5, TrieNode::NONE);
  *(offset_t*)((char*)&trie + offset) = 0;
  BOOST_CHECK_EQUAL(*trie.offset(TrieNode::NONE), 0);
  BOOST_CHECK_EQUAL(*trie.offset(5), 5);
  BOOST_CHECK_EQUAL(trie.size(), 32);
  BOOST_CHECK_EQUAL(offset, 16);

  offset = trie.create(prefix, TrieNode::NONE, 0, 5);
  *(offset_t*)((char*)&trie + offset) = 5;
  BOOST_CHECK_EQUAL(*trie.offset(TrieNode::NONE), 0);
  BOOST_CHECK_EQUAL(*trie.offset(5), 5);
  BOOST_CHECK_EQUAL(trie.size(), 32);
  BOOST_CHECK_EQUAL(offset, 24);

  char buffer1[PAGE_SIZE];
  TrieNode& trie1 = *(TrieNode*)buffer1;
  trie1.create(trie, Slice("123"));
  BOOST_CHECK(Slice(trie1.compressed(), trie1._compressed_len) == Slice("123"));
  BOOST_CHECK_EQUAL(*trie1.offset(TrieNode::NONE), 0);
  BOOST_CHECK_EQUAL(*trie1.offset(5), 5);
}

void create_trie(TrieNode* fill, int size, int* values) {
  char buffer[PAGE_SIZE];
  TrieNode* tmp = (TrieNode*)buffer;
  Slice prefix;

  uint16_t offset = fill->create(prefix, values[0]);
  *(offset_t*)((char*)fill + offset) = 0;
  for (int i = 1; i < size; i++) {
    memcpy(tmp, fill, fill->size());
    offset = fill->create(*tmp, values[i]);
    *(offset_t*)((char*)fill + offset) = 0;
  }
}

BOOST_AUTO_TEST_CASE(test_prev) {
  char buffer[PAGE_SIZE];
  TrieNode& trie = *(TrieNode*)buffer;
  int values[] = {5, 70, 130};
  create_trie(&trie, 3, values);

  BOOST_CHECK_EQUAL(trie.next(TrieNode::NONE), 5);
  BOOST_CHECK_EQUAL(trie.next(1), 5);
  BOOST_CHECK_EQUAL(trie.next(4), 5);
  BOOST_CHECK_EQUAL(trie.next(5), 70);
  BOOST_CHECK_EQUAL(trie.next(6), 70);
  BOOST_CHECK_EQUAL(trie.next(69), 70);
  BOOST_CHECK_EQUAL(trie.next(70), 130);
  BOOST_CHECK_EQUAL(trie.next(71), 130);
  BOOST_CHECK_EQUAL(trie.next(129), 130);
  BOOST_CHECK_EQUAL(trie.next(130), OOR);
  BOOST_CHECK_EQUAL(trie.next(131), OOR);
}

BOOST_AUTO_TEST_CASE(test_next) {
  char buffer[PAGE_SIZE];
  TrieNode& trie = *(TrieNode*)buffer;
  int values[] = {5, 70, 130};
  create_trie(&trie, 3, values);

  BOOST_CHECK_EQUAL(trie.prev(TrieNode::NONE), OOR);
  BOOST_CHECK_EQUAL(trie.prev(1), OOR);
  BOOST_CHECK_EQUAL(trie.prev(4), OOR);
  BOOST_CHECK_EQUAL(trie.prev(5), OOR);
  BOOST_CHECK_EQUAL(trie.prev(6), 5);
  BOOST_CHECK_EQUAL(trie.prev(69), 5);
  BOOST_CHECK_EQUAL(trie.prev(70), 5);
  BOOST_CHECK_EQUAL(trie.prev(71), 70);
  BOOST_CHECK_EQUAL(trie.prev(129), 70);
  BOOST_CHECK_EQUAL(trie.prev(130), 70);
  BOOST_CHECK_EQUAL(trie.prev(131), 130);
  BOOST_CHECK_EQUAL(trie.prev(255), 130);
}

BOOST_AUTO_TEST_CASE(test_count) {
  char buffer[PAGE_SIZE];
  TrieNode& trie = *(TrieNode*)buffer;
  int values[] = {5, 70, 130};
  create_trie(&trie, 3, values);
  BOOST_CHECK_EQUAL(trie.count(), 3);
}

void test_add(TrieNode& trie, uint8_t branch) {
  char buffer[PAGE_SIZE];
  TrieNode* tmp = (TrieNode*)buffer;
  uint16_t offset = tmp->create(trie, branch);
  *(offset_t*)((char*)tmp + offset) = branch;
  memcpy(&trie, tmp, tmp->size());
  BOOST_CHECK_EQUAL(*trie.offset(branch), branch);
}

BOOST_AUTO_TEST_CASE(test_many_branches) {
  char buffer[PAGE_SIZE];
  TrieNode& trie = *(TrieNode*)buffer;
  Slice prefix;
  uint16_t offset = trie.create(prefix, 'a');
  *(offset_t*)((char*)&trie + offset) = 'a';
  test_add(trie, 'b');
  test_add(trie, 'c');
  test_add(trie, 'd');
  test_add(trie, 'e');
  test_add(trie, 'f');
  test_add(trie, 'g');
  test_add(trie, 'h');
  test_add(trie, 'i');
  test_add(trie, 'j');
  test_add(trie, 'k');
  test_add(trie, 'l');
  test_add(trie, 'm');
  test_add(trie, 'n');
  test_add(trie, 'o');
  test_add(trie, 'p');
  test_add(trie, 'A');
}

#if 0
BOOST_AUTO_TEST_CASE(test_clear) {
  char buffer[PAGE_SIZE];
  TrieNode& trie = *(TrieNode*)buffer;

  trie.init();
  *trie.add(5) = 5;
  *trie.add(70) = 70;
  *trie.add(130) = 130;

  BOOST_CHECK_EQUAL(trie.count(), 3);
  trie.remove(70);
  BOOST_CHECK(trie.isset(5));
  BOOST_CHECK(!trie.isset(70));
  BOOST_CHECK(trie.isset(130));
  BOOST_CHECK_EQUAL(trie.count(), 2);
  BOOST_CHECK_EQUAL(*trie.offset(5), 5);
  BOOST_CHECK_EQUAL(*trie.offset(130), 130);

  *trie.add(TrieNode::NONE) = 0;
  BOOST_CHECK_EQUAL(*trie.offset(TrieNode::NONE), 0);
  trie.remove(TrieNode::NONE);
  BOOST_CHECK_EQUAL(trie.offset(TrieNode::NONE), nullptr);

  BOOST_CHECK_EQUAL(trie.size(), 32);
  trie.remove(250);
  BOOST_CHECK_EQUAL(trie.size(), 32);
  trie.remove(6);
  BOOST_CHECK_EQUAL(trie.size(), 32);

  trie.remove(5);
  BOOST_CHECK(!trie.isset(5));
  BOOST_CHECK_EQUAL(trie.offset(5), nullptr);
  BOOST_CHECK_EQUAL(trie.count(), 1);
  BOOST_CHECK_EQUAL(trie.size(), 24);
}
#endif

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
