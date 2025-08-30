#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBMMapTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_fstore.hpp"
