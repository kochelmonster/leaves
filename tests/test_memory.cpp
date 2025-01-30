#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MemManagerTest

#include <boost/test/included/unit_test.hpp>
#include <vector>

#include "leaves/intern/_memory.hpp"

using namespace leaves;

struct TestBlockHeader {
  typedef uint32_t bsize_t;
  typedef TestBlockHeader* ptr;
  typedef uint64_t offset_t;
  bsize_t block_size;
  offset_t next_free;

  template <typename T>
  struct Pointer {
    ptr p;
    T* operator->() { return (T*)(void*)p; }
    operator T*() { return (T*)(void*)p; }
    Pointer(ptr p_) : p(p_) {}
  };
};

using block_ptr = TestBlockHeader::ptr;
using MemManager = leaves::_MemManager<TestBlockHeader>;

BOOST_AUTO_TEST_CASE(test_initialization) {
  MemManager mem_manager;
  mem_manager.b128.reset();
  mem_manager.b512.reset();

  BOOST_CHECK_EQUAL(mem_manager.b128.end_current_area, 0);
  BOOST_CHECK_EQUAL(mem_manager.b128.next_free, 0);
  BOOST_CHECK_EQUAL(mem_manager.b512.end_current_area, 0);
  BOOST_CHECK_EQUAL(mem_manager.b512.next_free, 0);
}

BOOST_AUTO_TEST_CASE(test_clone) {
  MemManager mem_manager1;
  mem_manager1.b128.end_current_area = 100;
  mem_manager1.b128.next_free = 200;

  MemManager mem_manager2;
  mem_manager2.b128.copy(mem_manager1.b128);

  BOOST_CHECK_EQUAL(mem_manager2.b128.end_current_area, 100);
  BOOST_CHECK_EQUAL(mem_manager2.b128.next_free, 200);
}

BOOST_AUTO_TEST_CASE(test_reset) {
  MemManager mem_manager;
  mem_manager.b128.end_current_area = 100;
  mem_manager.b128.next_free = 200;

  mem_manager.b128.reset();

  BOOST_CHECK_EQUAL(mem_manager.b128.end_current_area, 0);
  BOOST_CHECK_EQUAL(mem_manager.b128.next_free, 0);
}

BOOST_AUTO_TEST_CASE(test_space) {
  MemManager mem_manager;
  BOOST_CHECK_EQUAL(mem_manager.space(), 2 * sizeof(MemManager::FreeBlock) +
                                             mem_manager.dumps.space());
}

struct Storage {
  typedef TestBlockHeader::ptr block_ptr;
  typedef TestBlockHeader::offset_t offset_t;

  std::vector<void*> areas;

  ~Storage() {
    for (auto ptr : areas) {
      free(ptr);
    }
  }

  offset_t alloc_area(size_t size) {
    void* ptr = malloc(size);
    areas.push_back(ptr);
    return (offset_t)ptr;
  }

 
  block_ptr resolve(offset_t offset) {
    return (block_ptr)offset;
  }

  offset_t resolve(block_ptr p) {
    return (offset_t)p;
  }
};

BOOST_AUTO_TEST_CASE(test_alloc) {
  union {
    MemManager mem_manager;
    char buffer[MemManager::FULL_SIZE];
  };
  Storage storage;
  mem_manager.reset();

  // Test allocation
  block_ptr ptr = mem_manager.alloc(128, storage);
  BOOST_CHECK(ptr != nullptr);

  // Test allocation with insufficient space
  auto ptr2 = mem_manager.alloc(1024 * 1024, storage);  // Large allocation
  BOOST_CHECK(ptr2 == nullptr);

  auto optr = storage.resolve(ptr);
  size_t bs = 128;
  auto start =
      storage.resolve((TestBlockHeader*)storage.areas.front());
  for (int i = 0; i < 63; i++) {
    auto ptr = mem_manager.alloc(4 * K, storage);
    auto offset = storage.resolve(ptr);
    // std::cout << "test: " << i << "  " << offset - start << std::endl;
    BOOST_CHECK(ptr != nullptr);
    BOOST_CHECK_EQUAL(optr + bs, offset);
    bs = 4 * K;
    optr = offset;
  }

  BOOST_CHECK(mem_manager.dumps.bits.bits[0] == 0);
  ptr = mem_manager.alloc(4 * K, storage);
  auto offset = storage.resolve(ptr);
  BOOST_CHECK(optr + bs != offset);
  BOOST_CHECK(mem_manager.dumps.bits.bits[0]);
}

BOOST_AUTO_TEST_CASE(test_free) {
  union {
    MemManager mem_manager;
    char buffer[MemManager::FULL_SIZE];
  };
  Storage storage;

  mem_manager.reset();

  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], 0);

  // Allocate and then free
  auto ptr = mem_manager.alloc(400, storage);
  BOOST_CHECK(ptr != nullptr);
  mem_manager.free(ptr, storage);

  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], 1 << 3);

  auto ptr1 = mem_manager.alloc(400, storage);
  BOOST_CHECK(ptr1 == ptr);
  mem_manager.free(ptr1, storage);
  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], 1 << 3);

  ptr1 = mem_manager.alloc(100, storage);
  BOOST_CHECK(ptr1 == ptr);
  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], (1 << 2));

  mem_manager.free(ptr1, storage);
  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], (1 << 2) | 1);

  ptr1 = mem_manager.alloc(100, storage);
  BOOST_CHECK(ptr1 == ptr);
  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], (1 << 2));

  auto ptr2 = mem_manager.alloc(100, storage);
  BOOST_CHECK((char*)ptr2 == (char*)ptr1 + 128);  // from former split
  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], 2);

  mem_manager.free(ptr1, storage);
  BOOST_CHECK_EQUAL(mem_manager.dumps.bits.bits[0], 2 | 1);

  mem_manager.free(ptr2, storage);
  BOOST_CHECK_EQUAL(mem_manager.dumps[0].free, 2);
}

BOOST_AUTO_TEST_CASE(test_calc_dump_index) {
  BOOST_CHECK_EQUAL(calc_dump_index(127), 0);
  BOOST_CHECK_EQUAL(calc_dump_index(128), 0);
  BOOST_CHECK_EQUAL(calc_dump_index(129), 1);
  BOOST_CHECK_EQUAL(calc_dump_index(4 * K + 1), 32);
  BOOST_CHECK_EQUAL(calc_dump_index(16 * K + 1), 56);
}

BOOST_AUTO_TEST_CASE(test_fit) {
  MemManager mem_manager;
  BOOST_CHECK(fit<Slot128Trait>(128));
  BOOST_CHECK(fit<Slot512Trait>(4 * K + 1));
  BOOST_CHECK(fit<Slot1024Trait>(16 * K + 1));
}