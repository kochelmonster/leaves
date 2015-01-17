//@+leo-ver=5-thin
//@+node:michael.20150116155028.13: * @file test_base64.cpp
#define BOOST_TEST_MODULE Base64
#include <boost/test/included/unit_test.hpp>
#include <string>
#include <iostream>
#include "larch/leaves.h"


namespace larch_leaves {
void encode(const Slice& input, std::string& output);
void decode(const std::string& input, std::string& output);
}

using namespace larch_leaves;

BOOST_AUTO_TEST_SUITE(encode_decode_suite)

inline void do_test(const char* in) {
  std::string input(in);
  std::string tmp;
  std::string output;
  encode(input, tmp);
  decode(tmp, output);
  std::cout << input << "==" << output << std::endl;
  BOOST_CHECK(input == output);
}

BOOST_AUTO_TEST_CASE(encode_decode)
{
  do_test("A");
  do_test("AB");
  do_test("ABC");
  do_test("ABCD");
  do_test("ABCDE");
  do_test("ABCDEF");
  do_test("ABCDEFG");
  do_test("ABCDEFGH");
  do_test("ABCDEFHIJKLMOPQRSTUVWXYZabcdefghiklmnopqrstuvwxyzÄ");
}

BOOST_AUTO_TEST_SUITE_END()

//@-leo
