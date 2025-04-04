#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE BurstTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_burst.hpp"
#include "leaves/intern/_traits.hpp"

using namespace leaves;
using namespace leaves::bits;

struct TestTraits {
  typedef offset_t offset_e;
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;

  static constexpr uint16_t BLOCK_SIZES = { PAGE_SIZE };
  struct BlockHeader {
    typedef BlockHeader Base;
  };

  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

typedef _BurstTable<TestTraits> BurstTable;

BOOST_AUTO_TEST_CASE(test_insert) { BurstTable table; }
