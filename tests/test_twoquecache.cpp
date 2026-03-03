#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE TwoQCacheTest

#include <boost/test/included/unit_test.hpp>

#include "../include/leaves/intern/memory/_twoquecache.hpp"
#include "../include/leaves/intern/core/_traits.hpp"
#include "../include/leaves/intern/core/_util.hpp"

using namespace leaves;

struct DummyPageHeader {
    uint32_t slot_id;
};

// A simple SmartPointer-like class for testing
class TestPtr {
public:
    TestPtr(AreaSlice* slice = nullptr) : _slice(slice) {
        if (_slice) _slice->inc_ref();
    }

    TestPtr(const TestPtr& other) : _slice(other._slice) {
        if (_slice) _slice->inc_ref();
    }

    ~TestPtr() {
        if (_slice && _slice->dec_ref() == 0) {
            ::operator delete(_slice);
        }
    }

    TestPtr& operator=(const TestPtr& other) {
        if (this != &other) {
            AreaSlice* old = _slice;
            _slice = other._slice;
            if (_slice) _slice->inc_ref();
            if (old && old->dec_ref() == 0) {
                ::operator delete(old);
            }
        }
        return *this;
    }

    // Required by TwoQCache
    AreaSlice* area() const { return _slice; }

    // For testing
    operator bool() const { return _slice != nullptr; }

private:
    AreaSlice* _slice;
};

// Helper to create a TestPtr with a given size and refcount starting at 0
// (the TestPtr constructor will inc_ref to 1)
static TestPtr make_item(size_t size, size_t offset = 0) {
    AreaSlice* slice = static_cast<AreaSlice*>(::operator new(size));
    slice->size(size);
    slice->offset(offset);
    slice->_ref.store(0);
    return TestPtr(slice);
}

static const size_t ITEM_SIZE = 1024;

BOOST_AUTO_TEST_CASE(test_basic) {
    const size_t CAPACITY = 100 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    for (uint64_t i = 0; i < 10; i++) {
        cache.put(i, make_item(ITEM_SIZE, i * ITEM_SIZE));
    }

    for (uint64_t i = 0; i < 10; i++) {
        auto* val = cache.get(i);
        BOOST_REQUIRE(val != nullptr);
    }

    BOOST_CHECK_EQUAL(cache.size(), 10u);
}

BOOST_AUTO_TEST_CASE(test_a1in_to_am_promotion) {
    const size_t CAPACITY = 100 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    for (uint64_t i = 0; i < 5; i++) {
        cache.put(i, make_item(ITEM_SIZE, i * ITEM_SIZE));
    }

    BOOST_CHECK_EQUAL(cache._a1in_map.size(), 5u);
    BOOST_CHECK_EQUAL(cache._am_map.size(), 0u);

    for (uint64_t i = 0; i < 5; i++) {
        auto* val = cache.get(i);
        BOOST_REQUIRE(val != nullptr);
    }

    BOOST_CHECK_EQUAL(cache._a1in_map.size(), 0u);
    BOOST_CHECK_EQUAL(cache._am_map.size(), 5u);
}

BOOST_AUTO_TEST_CASE(test_a1in_overflow_to_ghost) {
    // Small capacity: A1in limit = 25% of 4*ITEM_SIZE = 1 item
    const size_t CAPACITY = 4 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    for (uint64_t i = 0; i < 10; i++) {
        cache.put(i, make_item(ITEM_SIZE, i * ITEM_SIZE));
    }

    BOOST_CHECK_GT(cache._a1out_list.size(), 0u);
    BOOST_CHECK_LE(cache._a1in_current_size, cache._a1in_size_limit + ITEM_SIZE);
}

BOOST_AUTO_TEST_CASE(test_ghost_promotion_to_am) {
    // Small capacity forces eviction
    const size_t CAPACITY = 4 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    for (uint64_t i = 0; i < 10; i++) {
        cache.put(i, make_item(ITEM_SIZE, i * ITEM_SIZE));
    }

    // Find a key in the ghost list
    uint64_t ghost_key = UINT64_MAX;
    for (auto& [key, entry] : cache._a1out_map) {
        ghost_key = key;
        break;
    }
    BOOST_REQUIRE_NE(ghost_key, UINT64_MAX);

    size_t am_before = cache._am_map.size();
    cache.put(ghost_key, make_item(ITEM_SIZE, ghost_key * ITEM_SIZE));
    BOOST_CHECK_EQUAL(cache._am_map.size(), am_before + 1);
    BOOST_CHECK_EQUAL(cache._am_map.count(ghost_key), 1u);
}

BOOST_AUTO_TEST_CASE(test_am_eviction) {
    // Capacity = 4 items. A1in limit = 25% = 1 item.
    const size_t CAPACITY = 4 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    // Insert and immediately promote to Am one at a time
    for (uint64_t i = 0; i < 4; i++) {
        cache.put(i, make_item(ITEM_SIZE, i * ITEM_SIZE));
        cache.get(i);  // promote to Am
    }
    BOOST_CHECK_EQUAL(cache._am_map.size(), 4u);
    BOOST_CHECK_EQUAL(cache._a1in_map.size(), 0u);
    BOOST_CHECK_EQUAL(cache._size, 4 * ITEM_SIZE);

    // Insert one more — _size > capacity → Am eviction loop runs
    cache.put(100, make_item(ITEM_SIZE, 100 * ITEM_SIZE));

    BOOST_CHECK_EQUAL(cache._am_map.size(), 3u);
    BOOST_CHECK_LE(cache._size, CAPACITY);
}

BOOST_AUTO_TEST_CASE(test_am_eviction_blocked) {
    const size_t CAPACITY = 4 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    // Insert and promote to Am, but keep external refs (refcount=2)
    std::vector<TestPtr> refs;
    for (uint64_t i = 0; i < 4; i++) {
        auto item = make_item(ITEM_SIZE, i * ITEM_SIZE);
        refs.push_back(item);  // keep ref → refcount=2
        cache.put(i, item);
        cache.get(i);  // promote to Am
    }
    BOOST_CHECK_EQUAL(cache._am_map.size(), 4u);

    // Am eviction loop runs but can't evict (all refcount=2)
    cache.put(100, make_item(ITEM_SIZE, 100 * ITEM_SIZE));

    BOOST_CHECK_EQUAL(cache._am_map.size(), 4u);
    BOOST_CHECK_GT(cache._size, CAPACITY);
}

BOOST_AUTO_TEST_CASE(test_ghost_list_trimming) {
    // Small capacity so items get evicted quickly
    const size_t CAPACITY = 2 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    for (uint64_t i = 0; i < 100; i++) {
        cache.put(i, make_item(ITEM_SIZE, i * ITEM_SIZE));
    }

    BOOST_CHECK_LE(cache._a1out_list.size(), cache._a1out_size_limit);
}

BOOST_AUTO_TEST_CASE(test_duplicate_put) {
    const size_t CAPACITY = 100 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    auto item = make_item(ITEM_SIZE, 0);
    cache.put(0, item);
    size_t size_before = cache.size();

    // Putting same key again — should be ignored (in A1in)
    cache.put(0, item);
    BOOST_CHECK_EQUAL(cache.size(), size_before);

    // Promote to Am by accessing
    cache.get((uint64_t)0);
    // Putting same key again — should be ignored (in Am)
    cache.put(0, item);
    BOOST_CHECK_EQUAL(cache.size(), size_before);
}

BOOST_AUTO_TEST_CASE(test_intrusive_list) {
    IntrusiveList<uint64_t, TestPtr> list;
    BOOST_CHECK(list.empty());
    BOOST_CHECK_EQUAL(list.size(), 0u);

    auto item = make_item(64, 0);
    CacheEntry<uint64_t, TestPtr> e1(1, item);
    CacheEntry<uint64_t, TestPtr> e2(2, item);

    list.push_back(&e1);
    BOOST_CHECK(!list.empty());
    BOOST_CHECK_EQUAL(list.size(), 1u);

    list.push_back(&e2);
    BOOST_CHECK_EQUAL(list.size(), 2u);
    BOOST_CHECK_EQUAL(list.front(), &e1);
    BOOST_CHECK_EQUAL(list.back(), &e2);

    // Move front to back
    list.move_to_back(&e1);
    BOOST_CHECK_EQUAL(list.front(), &e2);
    BOOST_CHECK_EQUAL(list.back(), &e1);

    // Move back to back (no-op)
    list.move_to_back(&e1);
    BOOST_CHECK_EQUAL(list.back(), &e1);

    list.unlink(&e1);
    list.unlink(&e2);
    BOOST_CHECK(list.empty());
}

BOOST_AUTO_TEST_CASE(test_debug_info) {
    const size_t CAPACITY = 10 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    cache.put(0, make_item(ITEM_SIZE, 0));
    cache.put(1, make_item(ITEM_SIZE, ITEM_SIZE));
    BOOST_CHECK_NO_THROW(cache.debug_info());
}

BOOST_AUTO_TEST_CASE(test_legacy_get) {
    const size_t CAPACITY = 10 * ITEM_SIZE;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);

    cache.put(0, make_item(ITEM_SIZE, 0));

    TestPtr out;
    BOOST_CHECK(cache.get((uint64_t)0, out) == true);
    BOOST_CHECK(out);
    BOOST_CHECK(cache.get((uint64_t)999, out) == false);
}

BOOST_AUTO_TEST_CASE(test_move) {
    const size_t CAPACITY = 10 * ITEM_SIZE;

    TwoQCache<uint64_t, TestPtr> cache(CAPACITY, 0.25, 0.5, ITEM_SIZE);
    cache.put(0, make_item(ITEM_SIZE, 0));
    cache.put(1, make_item(ITEM_SIZE, ITEM_SIZE));

    // Move constructor
    TwoQCache<uint64_t, TestPtr> cache2(std::move(cache));
    BOOST_CHECK_EQUAL(cache2.size(), 2u);
    BOOST_CHECK_EQUAL(cache.size(), 0u);

    // Move assignment
    TwoQCache<uint64_t, TestPtr> cache3(CAPACITY, 0.25, 0.5, ITEM_SIZE);
    cache3 = std::move(cache2);
    BOOST_CHECK_EQUAL(cache3.size(), 2u);
    BOOST_CHECK_EQUAL(cache2.size(), 0u);
}