#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MemManagerTest

#include <boost/test/included/unit_test.hpp>
#include <map>
#include <vector>

#include "leaves/intern/_traits.hpp"
#include "leaves/intern/_memory.hpp"

using namespace leaves;

struct TestTraits {
  typedef offset_t offset_e;
  typedef tid_t tid_e;
  typedef uint16_t uint16_e;
  typedef uint32_t uint32_e;
  typedef uint64_t uint64_e;

  static constexpr uint16_t BLOCK_SIZES[] = {104,  160,  568,
                                             1056, 2088, PAGE_SIZE};

  struct BlockHeader {
    tid_e txn_id;
    uint8_t slot_id;
    uint8_t free_idx;
  };

  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type=TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

struct TestStorage {
  using BlockHeader = typename TestTraits::BlockHeader;
  using block_ptr = typename TestTraits::ptr;
  using offset_e = typename TestTraits::offset_e;
  using uint32_e = typename TestTraits::uint32_e;
  using uint16_e = typename TestTraits::uint16_e;
  using tid_e = typename TestTraits::tid_e;
  using MemManager = typename leaves::_MemManager<TestTraits>;

  std::vector<char> memory;

  typedef std::map<offset_t, uint32_e> blocks_t;
  blocks_t _debug_collect_block;

  TestStorage() {
    mm.init(1);
    memory.reserve(1024 * 1024);
    memory.resize(AREA_SIZE);
    mm.next_free = 4096;
    accept_tid = mark_tid = 1;
  }

  MemManager mm;
  tid_t accept_tid;
  tid_t mark_tid;

  block_ptr resolve(offset_t offset) { return block_ptr(&memory[offset]); }

  offset_t resolve(const block_ptr& p) {
    return offset_t((const char*)(const void*)p - (char*)&memory[0]);
  }

  block_ptr alloc(uint16_t space) { return alloc_slot(mm.assign_slot(space)); }

  block_ptr alloc_slot(uint8_t slot) {
    block_ptr result = mm.alloc(slot, *this);
    result->txn_id = mark_tid;
    return result;
  }

  bool free(block_ptr p) { return mm.free(p, *this); }

  void extend_file(size_t size) { memory.resize(size); }

  template <typename T>
  bool may_recycle(const T& free_block) {
    return free_block.txn_id <= accept_tid;
  }
  template <typename T>
  void mark_for_recycle(T& free_block) {
    free_block.txn_id = mark_tid;
  }
};

BOOST_AUTO_TEST_CASE(test_block_assignment) {
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(10), 0);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(104), 0);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(128), 1);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(160), 1);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(169), 2);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(564), 2);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(1024), 3);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(1056), 3);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(1535), 4);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(2088), 4);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(2537), 5);
  BOOST_CHECK_EQUAL(TestStorage::MemManager::assign_slot(4096), 5);
}

using BlockHeader = TestTraits::BlockHeader;
using block_ptr = TestTraits::ptr;
using MemManager = TestStorage::MemManager;

BOOST_AUTO_TEST_CASE(test_free_overflow) {
  TestStorage storage;

  static const int BC =
      storage.mm.assign_slot(MemManager::GarbageContainer::SIZE);

  std::vector<offset_t> offsets;
  int count = 1 + MemManager::GarbageContainer::COUNT;
  for (int i = 0; i < count; ++i) {
    auto result = storage.alloc(252);
    offsets.push_back(storage.resolve(result));
    BOOST_CHECK((bool)result);
    BOOST_CHECK_EQUAL(result->slot_id, 2);
  }

  for (auto offset : offsets) {
    auto p = storage.resolve(offset);
    storage.free(p);
  }

  int b288 = storage.mm.assign_slot(256);
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

BOOST_AUTO_TEST_CASE(test_page_transform) {
  TestStorage storage;
  std::vector<offset_t> offsets;
  for (int i = 0; i < 20; i++) {
    auto result = storage.alloc(PAGE_SIZE);
    offsets.push_back(storage.resolve(result));
  }

  for (auto offset : offsets) {
    auto p(storage.resolve(offset));
    storage.free(p);
  }

  block_ptr p(storage.alloc_slot(0));
  offset_t moffset = storage.resolve(p);
  BOOST_CHECK_EQUAL(moffset, offsets[0]);
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