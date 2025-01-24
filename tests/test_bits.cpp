#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE BitTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_bits.hpp"

using namespace leaves;

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
