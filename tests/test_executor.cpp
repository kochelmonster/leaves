#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE ExecutorTest

#include <boost/test/included/unit_test.hpp>
#include <atomic>
#include <thread>
#include <vector>

#include "leaves/intern/util/_threadpool.hpp"
#include "leaves/intern/memory/_memory.hpp"
#include "leaves/intern/core/_traits.hpp"

using namespace leaves;

#if LEAVES_HAS_THREADS

// Minimal storage that satisfies _ThreadPoolMixin CRTP
struct TestPool : _ThreadPoolMixin<TestPool> {
  TestPool(size_t n = 4) : _ThreadPoolMixin(n) {}
};

BOOST_AUTO_TEST_SUITE(pool_executor)

BOOST_AUTO_TEST_CASE(wraps_thread_pool_mixin) {
  TestPool pool(4);
  BOOST_TEST(pool.pool_size() == 4u);
  BOOST_TEST(!pool.is_single_threaded());
}

BOOST_AUTO_TEST_CASE(post_executes_on_worker) {
  TestPool pool(2);

  std::atomic<int> value{0};
  pool.submit_task([&] { value.store(42, std::memory_order_release); });
  pool.wait_idle();
  BOOST_TEST(value.load(std::memory_order_acquire) == 42);
}

BOOST_AUTO_TEST_SUITE_END()

struct TestTraits {
  typedef offset_t offset_e;
  typedef uint16_t uint16_e;
  typedef uint32_t uint32_e;
  typedef uint64_t uint64_e;

  static constexpr size_t PAGE_SIZE = 4 * K;
  static constexpr size_t AREA_SIZE = 4 * PAGE_SIZE;
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t PAGE_SIZES[] = {104, 160, 568, 1056, 2088, 4 * K};
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);

  struct PageHeader {
    tid_t txn_id;
    uint8_t slot_id;
  };
  using ptr = SimplePointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SimplePointer<T, type>;
};

constexpr size_t AREA_SIZE = TestTraits::AREA_SIZE;

// TestResolver acts like a DB for _MemManager tests
struct TestResolver {
  typedef TestTraits Traits;
  using PageHeader = typename TestTraits::PageHeader;
  using page_ptr = typename TestTraits::ptr;
  using offset_e = typename TestTraits::offset_e;
  using area_ptr = typename TestTraits::template Pointer<Area>;

  std::vector<char> memory;
  std::vector<Area*> _owned_areas;
  _MemManager<TestTraits> _backing_mm;  // for alloc_slot
  tid_t mark_tid{1};
  tid_t accept_tid{1};
  SpinLock _area_mutex;  // protects memory + _owned_areas

  // Simulated _active_txn for container txn_id stamping
  struct FakeTxn { tid_t txn_id{1}; };
  FakeTxn _fake_txn;
  FakeTxn* _active_txn = &_fake_txn;

  TestResolver() {
    memory.reserve(1024 * 1024);
    memory.resize(2 * AREA_SIZE);
    // _backing_mm uses the second area exclusively (for PageContainer allocs)
    _backing_mm.init(AREA_SIZE, 2 * AREA_SIZE);
  }

  ~TestResolver() {
    for (auto* a : _owned_areas) delete a;
  }

  template <typename T>
  typename Traits::Pointer<T> resolve(const offset_e* offset_ptr,
                                      Access = READ) const {
    return typename Traits::Pointer<T>(
        const_cast<char*>(&memory[*offset_ptr]));
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return offset_t((const char*)p - &memory[0]);
  }

  template <typename T>
  bool may_recycle(const T& block) const {
    return block.txn_id <= accept_tid;
  }

  template <typename T>
  void mark_for_recycle(T& block) const {
    block.txn_id = mark_tid;
  }

  template <typename PtrType>
  void make_dirty(PtrType) {}

  area_ptr alloc_single_area() {
    _area_mutex.lock();
    size_t old_size = memory.size();
    memory.resize(old_size + AREA_SIZE);
    Area* area = new Area();
    _owned_areas.push_back(area);
    area->offset(old_size);
    area->size(AREA_SIZE);
    area->_ref.store(0);
    _area_mutex.unlock();
    return area_ptr(area);
  }

  page_ptr alloc_slot(uint16_t slot) {
    _area_mutex.lock();
    page_ptr result = _backing_mm.alloc(slot, *this);
    result->txn_id = _active_txn->txn_id;
    _area_mutex.unlock();
    return result;
  }

  void free(page_ptr) {}
};

#endif  // LEAVES_HAS_THREADS
