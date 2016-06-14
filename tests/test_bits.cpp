#define BOOST_TEST_MODULE TrieTest
//#define BOOST_TEST_NO_MAIN
#include <boost/test/included/unit_test.hpp>
#include "../src/bittrie.hpp"

using namespace larch_leaves;

namespace larch_leaves {

void testpoint(const char* str) {
}

}

typedef unsigned char trieindex_t;


BOOST_AUTO_TEST_CASE(add_bits) {
  BitTrie bits;
  int settings[8] = { 3, 15, 20, 26, 32, 45, 50, 60 };
  Page::ptr children[65];

  bits.bits = 0;
  memset(children, 0, sizeof(children));

  BOOST_REQUIRE_EQUAL(bits.last_bit(), -1);
  BOOST_REQUIRE_EQUAL(bits.first_bit(), -1);

  for(int i = 7; i >= 0; i--)
    bits.add(settings[i], i+1, children);

  int b = bits.first_bit();
  int i = 0;
  while(b >= 0) {
    int ci = bits.get_child_index(b);
    BOOST_REQUIRE_EQUAL(settings[i], b);
    BOOST_REQUIRE_EQUAL(ci, i+1);
    BOOST_REQUIRE_EQUAL(children[ci], i+1);
    b = bits.next_bit(b);
    i++;
  }

  b = bits.last_bit();
  i = 7;
  while(b >= 0) {
    int ci = bits.get_child_index(b);
    BOOST_REQUIRE_EQUAL(settings[i], b);
    BOOST_REQUIRE_EQUAL(ci, i+1);
    BOOST_REQUIRE_EQUAL(children[ci], i+1);
    b = bits.prev_bit(b);
    i--;
  }

  for(int i = 0; i < 8; i++) {
    int b = settings[i];
    int ci = bits.get_child_index(b);
    BOOST_REQUIRE_EQUAL(ci, i+1);
    BOOST_REQUIRE_EQUAL(children[ci], i+1);
    BOOST_REQUIRE_EQUAL(bits.get_child_index(b-1), 0);
    BOOST_REQUIRE_EQUAL(bits.get_child_index(b+1), 0);
  }

  for(int i = 0; i < 8; i++)
    bits.remove(settings[i], children);

  BOOST_REQUIRE_EQUAL(bits.bits, 0);
}
