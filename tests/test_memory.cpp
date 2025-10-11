#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MemManagerTest

#include <boost/test/included/unit_test.hpp>
#include <map>
#include <vector>

#include "leaves/intern/_memory.hpp"
#include "leaves/intern/_traits.hpp"

using namespace leaves;

struct TestTraits {
  typedef offset_t offset_e;
  typedef uint16_t uint16_e;
  typedef uint32_t uint32_e;
  typedef uint64_t uint64_e;

  static constexpr size_t BLOCK_SIZE = 4 * K;

  // size of newly allocated areas
  static constexpr size_t AREA_SIZE = 4 * BLOCK_SIZE;
  static constexpr uint16_t BLOCK_SIZES[] = {104, 160, 568, 1056, 2088, 4 * K};
  static constexpr uint16_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

  struct BlockHeader {
    tid_t txn_id;
    uint8_t slot_id;
    uint8_t free_idx;
  };
  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

constexpr size_t AREA_SIZE = TestTraits::AREA_SIZE;

struct TestStorage {
  typedef TestTraits Traits;
  using BlockHeader = typename TestTraits::BlockHeader;
  using block_ptr = typename TestTraits::ptr;
  using offset_e = typename TestTraits::offset_e;
  using uint32_e = typename TestTraits::uint32_e;
  using uint16_e = typename TestTraits::uint16_e;
  using MemManager = typename leaves::_MemManager<TestTraits>;

  using area_ptr = typename TestTraits::template Pointer<Area>;
  std::vector<char> memory;
  AreaList single_areas;
  AreaList multi_areas;

  TestStorage() {
    accept_tid = mark_tid = 0;
    memory.reserve(1024 * 1024);
    memory.resize(AREA_SIZE);
    single_areas.init();
    multi_areas.init();
    mm.init(sizeof(void*), AREA_SIZE);
    accept_tid = mark_tid = 1;
  }

  MemManager mm;
  tid_t accept_tid;
  tid_t mark_tid;

  block_ptr resolve(offset_t offset, Access access = READ) {
    return block_ptr(&memory[offset]);
  }

  offset_t resolve(const block_ptr& p) {
    return offset_t((const char*)p - (char*)&memory[0]);
  }

  block_ptr alloc(uint16_t space) { return alloc_slot(mm.assign_slot(space)); }

  block_ptr alloc_slot(uint8_t slot) {
    block_ptr result = mm.alloc(slot, *this);
    result->txn_id = mark_tid;
    return result;
  }

  void free(block_ptr p) { mm.free(p, *this); }

  area_ptr alloc_single_area() {
    auto result = single_areas.pop(*this);
    if (!result) {
      size_t old_size = memory.size();
      memory.resize(old_size + AREA_SIZE);

      // Create a new Area and initialize it
      Area* area = new Area();
      area->offset(old_size);
      area->size(AREA_SIZE);
      area->_ref.store(0);

      // Wrap in area_ptr
      return area_ptr(area);
    }

    return result;
  }

  area_ptr alloc_multi_area(uint64_t size) {
    size = padding(size, AREA_SIZE);
    auto result = multi_areas.find_and_remove(size, *this);
    if (!result) {
      size_t old_size = memory.size();
      memory.resize(old_size + size);

      // Create a new Area and initialize it
      Area* area = new Area();
      area->offset(old_size);
      area->size(size);
      area->_ref.store(0);

      // Wrap in area_ptr
      return area_ptr(area);
    }

    return result;
  }

  template <typename T>
  bool may_recycle(const T& free_block) {
    return free_block.txn_id <= accept_tid;
  }
  template <typename T>
  void mark_for_recycle(T& free_block) {
    free_block.txn_id = mark_tid;
  }

  void make_dirty(block_ptr& block) {}
  void flush(bool sync = false, bool force = false) {}
};

struct TestTraitsBig : public TestTraits {
  static constexpr size_t AREA_SIZE = 1 * M;
};

struct TestStorageBig : public TestStorage {
  typedef TestTraitsBig Traits;
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
      storage.mm.assign_slot(MemManager::BlockContainer::SIZE);

  const uint16_t SPACE = 200;

  std::vector<offset_t> offsets;
  const int COUNT = MemManager::BlockContainer::COUNT;
  int count = 1 + COUNT;
  for (int i = 0; i < count; ++i) {
    auto result = storage.alloc(SPACE);
    offsets.push_back(storage.resolve(result));
    BOOST_CHECK((bool)result);
    BOOST_CHECK_EQUAL(result->slot_id, 2);
  }

  for (auto offset : offsets) {
    auto p = storage.resolve(offset);
    storage.free(p);
  }

  int sid = storage.mm.assign_slot(SPACE);
  BOOST_CHECK_EQUAL(storage.mm.slots[sid].count, count);
  BOOST_CHECK(storage.mm.slots[sid].ostart != storage.mm.slots[sid].oend);
  int ccount = storage.mm.slots[BC].count;

  offset_t last_offset = offsets.back();
  offsets.pop_back();

  for (auto offset : offsets) {
    auto result = storage.alloc(SPACE);
    BOOST_CHECK_EQUAL(storage.resolve(result), offset);
  }

  BOOST_CHECK_EQUAL(storage.mm.slots[sid].count, 1);
  BOOST_CHECK(storage.mm.slots[sid].ostart == storage.mm.slots[sid].oend);
  BOOST_CHECK_EQUAL(storage.mm.slots[BC].count, ccount + 1);

  auto result = storage.alloc(SPACE);
  BOOST_CHECK_EQUAL(storage.resolve(result), last_offset);
  BOOST_CHECK_EQUAL(storage.mm.slots[BC].count, ccount + 2);
  BOOST_CHECK_EQUAL(storage.mm.slots[sid].count, 0);
}

constexpr auto BLOCK_SIZE = TestTraits::BLOCK_SIZE;

BOOST_AUTO_TEST_CASE(test_page_border) {
  TestStorage storage;
  constexpr auto& BLOCK_SIZES = TestTraits::BLOCK_SIZES;

  uint16_t bsize = BLOCK_SIZES[3];
  offset_t delta = storage.resolve(storage.alloc_slot(3));
  delta += bsize;

  BOOST_CHECK_EQUAL(storage.mm.left_over_start, storage.mm.left_over_end);

  while (delta + bsize < storage.mm.allocation_end) {
    auto result = storage.resolve(storage.alloc_slot(3));
    BOOST_CHECK(result == delta);
    delta += bsize;
  }

  auto result = storage.resolve(storage.alloc_slot(3));
  // A new page is allocated for slots and
  // the rest is used with min blocks
  BOOST_CHECK(result != delta);
  BOOST_CHECK(storage.mm.left_over_start < storage.mm.left_over_end);

  auto old = storage.mm.left_over_start;
  result = storage.resolve(storage.alloc_slot(1));
  BOOST_CHECK(result == old);
  BOOST_CHECK(result < storage.mm.left_over_start);
}

BOOST_AUTO_TEST_CASE(test_arealist) {
  TestStorage storage;
  AreaList areas;
  areas.init();

  // Test empty list
  BOOST_CHECK_EQUAL(areas.get_head(), 0);
  auto result = areas.pop(storage);
  BOOST_CHECK(!result);

  // Test push and pop single area
  auto area1 = storage.alloc_single_area();
  areas.push(*area1, storage);
  BOOST_CHECK_EQUAL(areas.get_head(), area1->offset());

  auto popped = areas.pop(storage);
  BOOST_CHECK(popped);
  BOOST_CHECK_EQUAL(popped->offset(), area1->offset());
  BOOST_CHECK_EQUAL(popped->size(), area1->size());
  BOOST_CHECK_EQUAL(areas.get_head(), offset_t(0));

  // Test multiple areas
  auto area2 = storage.alloc_single_area();
  auto area3 = storage.alloc_single_area();

  areas.push(*area2, storage);
  areas.push(*area3, storage);

  // Should pop in LIFO order
  auto pop1 = areas.pop(storage);
  BOOST_CHECK_EQUAL(pop1->offset(), area3->offset());

  auto pop2 = areas.pop(storage);
  BOOST_CHECK_EQUAL(pop2->offset(), area2->offset());

  BOOST_CHECK_EQUAL(areas.get_head(), offset_t(0));

  // Test find_and_remove with different sizes
  Area small_area;
  small_area.offset(1000);
  small_area.size(AREA_SIZE);

  Area big_area;
  big_area.offset(2000);
  big_area.size(2 * AREA_SIZE);

  areas.push(small_area, storage);
  areas.push(big_area, storage);

  // Find exact size match
  auto found = areas.find_and_remove(2 * AREA_SIZE, storage);
  BOOST_CHECK(found);
  BOOST_CHECK_EQUAL(found->offset(), big_area.offset());

  // Should still have small area
  auto remaining = areas.pop(storage);
  BOOST_CHECK(remaining);
  BOOST_CHECK_EQUAL(remaining->offset(), small_area.offset());
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