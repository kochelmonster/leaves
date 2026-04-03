#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE RangeFilterTest

#include <boost/test/included/unit_test.hpp>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "leaves/filter.hpp"
#include "leaves/intern/storage/_memstore.hpp"
#include "leaves/keycodec.hpp"

using namespace leaves;

// Helper: insert key→value into a MemoryStorage
static void insert(_MemoryStorage& storage, const std::string& key,
                   const std::string& value) {
    auto cursor = storage.create_cursor();
    cursor->find(Slice(key));
    cursor->value(Slice(value));
}

// Helper: collect all keys from a TmpDB cursor into a sorted set
template <typename CursorType>
static std::set<std::string> collect(CursorType& cursor) {
    std::set<std::string> result;
    cursor.first();
    while (cursor.is_valid()) {
        result.insert(std::string(cursor.key().data(), cursor.key().size()));
        cursor.next();
    }
    return result;
}

// Build an index: encoded uint32 value → doc_key string.
// Inserts N entries where the encoded value = i, and doc_key = "doc_<i>".
struct IndexFixture {
    _MemoryStorage storage;

    void build(uint32_t start, uint32_t end) {
        auto cursor = storage.create_cursor();
        for (uint32_t i = start; i <= end; ++i) {
            KeyBuilder kb;
            kb.append_uint32(i);
            std::string doc_key = "doc_" + std::to_string(i);
            cursor->find(Slice(kb));
            cursor->value(Slice(doc_key));
        }
    }
};

// ---- Tests ----

BOOST_AUTO_TEST_CASE(test_tmpdb_basic) {
    _MemoryStorage storage;
    _TmpDB<_MemoryStorage> db(storage);
    auto cursor = db.cursor();

    // Insert some keys
    cursor.find(Slice("hello"));
    cursor.value(Slice());

    cursor.find(Slice("world"));
    cursor.value(Slice());

    // Verify
    cursor.find(Slice("hello"));
    BOOST_CHECK(cursor.is_valid());

    cursor.find(Slice("world"));
    BOOST_CHECK(cursor.is_valid());

    cursor.find(Slice("missing"));
    BOOST_CHECK(!cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(test_tmpdb_iteration) {
    _MemoryStorage storage;
    _TmpDB<_MemoryStorage> db(storage);
    auto cursor = db.cursor();

    std::vector<std::string> keys = {"alpha", "beta", "gamma", "delta"};
    for (auto& k : keys) {
        cursor.find(Slice(k));
        cursor.value(Slice());
    }

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 4);
    BOOST_CHECK(result.count("alpha"));
    BOOST_CHECK(result.count("beta"));
    BOOST_CHECK(result.count("gamma"));
    BOOST_CHECK(result.count("delta"));
}

BOOST_AUTO_TEST_CASE(test_range_inclusive_both) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range [5, 10] inclusive
    KeyBuilder lb, ub;
    lb.append_uint32(5);
    ub.append_uint32(10);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(lb), true),
                              _RangeBound(Slice(ub), true));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 6);
    for (uint32_t i = 5; i <= 10; ++i) {
        BOOST_CHECK_MESSAGE(result.count("doc_" + std::to_string(i)),
                            "Missing doc_" + std::to_string(i));
    }
}

BOOST_AUTO_TEST_CASE(test_range_exclusive_both) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range (5, 10) exclusive
    KeyBuilder lb, ub;
    lb.append_uint32(5);
    ub.append_uint32(10);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(lb), false),
                              _RangeBound(Slice(ub), false));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 4);
    for (uint32_t i = 6; i <= 9; ++i) {
        BOOST_CHECK_MESSAGE(result.count("doc_" + std::to_string(i)),
                            "Missing doc_" + std::to_string(i));
    }
}

BOOST_AUTO_TEST_CASE(test_range_mixed_bounds) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range [5, 10) — inclusive lower, exclusive upper
    KeyBuilder lb, ub;
    lb.append_uint32(5);
    ub.append_uint32(10);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(lb), true),
                              _RangeBound(Slice(ub), false));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 5);
    for (uint32_t i = 5; i <= 9; ++i) {
        BOOST_CHECK_MESSAGE(result.count("doc_" + std::to_string(i)),
                            "Missing doc_" + std::to_string(i));
    }
}

BOOST_AUTO_TEST_CASE(test_range_empty_result) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range [100, 200] — no keys in this range
    KeyBuilder lb, ub;
    lb.append_uint32(100);
    ub.append_uint32(200);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(lb), true),
                              _RangeBound(Slice(ub), true));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 0);
}

BOOST_AUTO_TEST_CASE(test_range_all_keys) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // No bounds — scan everything
    auto cursor = filter.scan(&db._root, _RangeBound(), _RangeBound());

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 21);
}

BOOST_AUTO_TEST_CASE(test_range_single_element) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range [7, 7] — single element
    KeyBuilder kb;
    kb.append_uint32(7);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(kb), true),
                              _RangeBound(Slice(kb), true));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 1);
    BOOST_CHECK(result.count("doc_7"));
}

BOOST_AUTO_TEST_CASE(test_range_lower_only) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range [15, ∞) — lower bound only
    KeyBuilder lb;
    lb.append_uint32(15);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(lb), true),
                              _RangeBound());

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 6);
    for (uint32_t i = 15; i <= 20; ++i) {
        BOOST_CHECK_MESSAGE(result.count("doc_" + std::to_string(i)),
                            "Missing doc_" + std::to_string(i));
    }
}

BOOST_AUTO_TEST_CASE(test_range_upper_only) {
    IndexFixture idx;
    idx.build(0, 20);
    auto& db = idx.storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range (-∞, 5] — upper bound only
    KeyBuilder ub;
    ub.append_uint32(5);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(),
                              _RangeBound(Slice(ub), true));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 6);
    for (uint32_t i = 0; i <= 5; ++i) {
        BOOST_CHECK_MESSAGE(result.count("doc_" + std::to_string(i)),
                            "Missing doc_" + std::to_string(i));
    }
}

BOOST_AUTO_TEST_CASE(test_range_empty_db) {
    _MemoryStorage storage;
    auto& db = storage.db();

    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    KeyBuilder lb, ub;
    lb.append_uint32(0);
    ub.append_uint32(100);
    auto cursor = filter.scan(&db._root,
                              _RangeBound(Slice(lb), true),
                              _RangeBound(Slice(ub), true));

    auto result = collect(cursor);
    BOOST_CHECK_EQUAL(result.size(), 0);
}

BOOST_AUTO_TEST_CASE(test_range_string_keys) {
    // Use plain string keys (not encoded) as the index
    _MemoryStorage storage;
    auto cursor = storage.create_cursor();

    // Insert: "apple" → "doc1", "banana" → "doc2", "cherry" → "doc3",
    //         "date" → "doc4", "elderberry" → "doc5"
    std::vector<std::pair<std::string, std::string>> entries = {
        {"apple", "doc1"},
        {"banana", "doc2"},
        {"cherry", "doc3"},
        {"date", "doc4"},
        {"elderberry", "doc5"},
    };
    for (auto& [k, v] : entries) {
        cursor->find(Slice(k));
        cursor->value(Slice(v));
    }

    auto& db = storage.db();
    _InlineExecutor exec;
    _TaskGroup<_InlineExecutor> tg(exec);
    _RangeFilter<std::remove_reference_t<decltype(db)>, _InlineExecutor> filter(&db, &tg);

    // Range ["banana", "date"] inclusive
    auto result_cursor = filter.scan(
        &db._root,
        _RangeBound(Slice("banana"), true),
        _RangeBound(Slice("date"), true));

    auto result = collect(result_cursor);
    BOOST_CHECK_EQUAL(result.size(), 3);
    BOOST_CHECK(result.count("doc2"));  // banana
    BOOST_CHECK(result.count("doc3"));  // cherry
    BOOST_CHECK(result.count("doc4"));  // date
}
