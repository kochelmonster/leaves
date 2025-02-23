#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MemManagerTest

#include <boost/test/included/unit_test.hpp>
#include <map>
#include <vector>

#include "leaves/intern/_memory.hpp"

using namespace leaves;

struct TestBlockHeader {
  using offset_t = uint64_t;
  using bsize_t = uint32_t;
  typedef uint64_t tid_t;

  struct ptr {
    ptr(const ptr& src) : p(src.p) {}
    ptr(void* p_ = nullptr) : p((TestBlockHeader*)p_) {}
    TestBlockHeader* operator->() { return p; }
    const TestBlockHeader* operator->() const { return p; }
    operator const TestBlockHeader*() const { return p; }
    operator TestBlockHeader*() { return p; }
    operator char*() { return (char*)p; }
    operator bool() { return p != nullptr; }
    TestBlockHeader* p;
  };

  template <typename T>
  struct Pointer : public ptr {
    static_assert(std::is_base_of<TestBlockHeader, T>::value,
                  "T must derive from TestBlockHeader");

    Pointer(void* src = nullptr) : ptr(src) {}
    Pointer(const ptr& src) : ptr(src.p) {}
    Pointer(const Pointer<T>& src) : ptr(src.p) {}
    T* operator->() { return static_cast<T*>(p); }
    T& operator*() { return *static_cast<T*>(p); }
  };

  tid_t tid;
  bsize_t block_size;
};

struct TestStorage {
  using block_ptr = typename TestBlockHeader::ptr;
  using offset_t = typename TestBlockHeader::offset_t;
  using bsize_t = typename TestBlockHeader::bsize_t;
  using tid_t = typename TestBlockHeader::tid_t;
  using MemManager = leaves::_MemManager<TestBlockHeader>;
  std::vector<char> memory;

  typedef std::map<offset_t, bsize_t> blocks_t;
  blocks_t _debug_collect_block;

  TestStorage() {
    mm.init(1);
    memory.reserve(1024 * 1024);
    memory.resize(AREA_SIZE);
    mm.next_free = 4096;
    accept_tid = mark_tid = 1;
  }

  union {
    MemManager mm;
    char buffer[MemManager::EXTRA_SPACE + sizeof(MemManager)];
  };
  tid_t accept_tid;
  tid_t mark_tid;

  block_ptr resolve(offset_t offset) {
    return block_ptr((TestBlockHeader*)&memory[offset]);
  }

  offset_t resolve(const block_ptr& p) {
    return (offset_t)(const TestBlockHeader*)p - (offset_t)&memory[0];
  }

  block_ptr alloc(bsize_t space) {
    block_ptr result = mm.alloc(space, *this);
    result->tid = mark_tid;
    return result;
  }

  bool free(const block_ptr& p) { return mm.free(p, *this); }

  void extend_file(size_t size) { memory.resize(size); }

  template <typename T>
  bool may_recycle(const T& free_block) {
    return free_block.tid <= accept_tid;
  }
  template <typename T>
  void mark_for_recycle(T& free_block) {
    free_block.tid = mark_tid;
  }
};

BOOST_AUTO_TEST_CASE(test_block_assignment) {
  BOOST_CHECK_EQUAL(assign_block(10), 0);
  BOOST_CHECK_EQUAL(assign_block(65), 1);
  BOOST_CHECK_EQUAL(assign_block(80), 1);
  BOOST_CHECK_EQUAL(assign_block(81), 2);
  BOOST_CHECK_EQUAL(assign_block(128), 2);
  BOOST_CHECK_EQUAL(assign_block(129), 3);
  BOOST_CHECK_EQUAL(assign_block(129), 3);
  BOOST_CHECK_EQUAL(assign_block(288), 3);
  BOOST_CHECK_EQUAL(assign_block(289), 4);
  BOOST_CHECK_EQUAL(assign_block(512), 4);
  BOOST_CHECK_EQUAL(assign_block(513), 5);
  BOOST_CHECK_EQUAL(assign_block(1024), 5);
  BOOST_CHECK_EQUAL(assign_block(1025), 6);
  BOOST_CHECK_EQUAL(assign_block(1535), 6);
  BOOST_CHECK_EQUAL(assign_block(1536), 6);
  BOOST_CHECK_EQUAL(assign_block(1537), 7);
  BOOST_CHECK_EQUAL(assign_block(7679), 18);
  BOOST_CHECK_EQUAL(assign_block(7680), 18);
  BOOST_CHECK_EQUAL(assign_block(8191), 19);
  BOOST_CHECK_EQUAL(assign_block(8192), 19);
  BOOST_CHECK_EQUAL(assign_block(65536), 33);
}

using block_ptr = TestBlockHeader::ptr;
using MemManager = TestStorage::MemManager;


BOOST_AUTO_TEST_CASE(test_free_overflow) {
  TestStorage storage;

  static const int BC =
      assign_block(MemManager::GarbageContainer::SIZE);

  using offset_t = TestBlockHeader::offset_t;
  std::vector<offset_t> offsets;
  int count = 1 + MemManager::GarbageContainer::BLOCK_COUNT;
  for (int i = 0; i < count; ++i) {
    auto result = storage.alloc(252);
    offsets.push_back(storage.resolve(result));
    BOOST_CHECK((bool)result);
    BOOST_CHECK_EQUAL(result->block_size, 288);
  }

  for (auto offset : offsets) {
    storage.free(storage.resolve(offset));
  }

  int b288 = assign_block(256);
  BOOST_CHECK_EQUAL(storage.mm.slots[b288].count, count);
  BOOST_CHECK(storage.mm.slots[b288].ostart != storage.mm.slots[b288].oend);
  int ccount = storage.mm.slots[BC].count;

  offset_t last_offset = offsets.back();
  offsets.pop_back();

  for (auto offset : offsets) {
    auto result = storage.alloc(252);
    BOOST_CHECK_EQUAL(storage.resolve(result), offset);
  }

  BOOST_CHECK_EQUAL(storage.mm.slots[b288].count, 1);
  BOOST_CHECK(storage.mm.slots[b288].ostart == storage.mm.slots[b288].oend);
  BOOST_CHECK_EQUAL(storage.mm.slots[BC].count, ccount + 1);

  auto result = storage.alloc(252);
  BOOST_CHECK_EQUAL(storage.resolve(result), last_offset);
  BOOST_CHECK_EQUAL(storage.mm.slots[BC].count, ccount + 2);
  BOOST_CHECK_EQUAL(storage.mm.slots[b288].count, 0);
}

#if 0

BOOST_AUTO_TEST_CASE(test_small_overflow) {
  TestStorage storage;
  size_t old_area = storage.mm.slots[0].end_free;
  storage.mm.next_free = storage.mm.end_area;
  storage.mm.next4k = storage.mm.end4k;
  storage.alloc(2048);
  BOOST_CHECK(old_area < storage.mm.end_area);
}

BOOST_AUTO_TEST_CASE(test_alloc_big) {
  TestStorage storage;
  block_ptr result1 = storage.alloc(1000);
  BOOST_CHECK((bool)result1);
  BOOST_CHECK_EQUAL(result1->block_size, 1024);

  block_ptr result2 = storage.alloc(2000);
  BOOST_CHECK((bool)result2);
  BOOST_CHECK_EQUAL(result2->block_size, 2048);

  block_ptr result3 = storage.alloc(6000);
  BOOST_CHECK((bool)result3);
  BOOST_CHECK_EQUAL(result3->block_size, 6144);

  size_t old_area = storage.mm.end_area;
  storage.mm.next_free = storage.mm.end_area;
  storage.alloc(4096);
  BOOST_CHECK(old_area < storage.mm.end_area);
}
#endif