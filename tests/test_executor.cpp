#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE ExecutorTest

#include <boost/test/included/unit_test.hpp>
#include <atomic>
#include <thread>
#include <vector>

#include "leaves/intern/util/_executor.hpp"
#include "leaves/intern/util/_task_group.hpp"
#include "leaves/intern/util/_threadpool.hpp"
#include "leaves/intern/memory/_memory.hpp"
#include "leaves/intern/core/_traits.hpp"

using namespace leaves;

// =========================================================================
// Phase 1: Executor tests
// =========================================================================

BOOST_AUTO_TEST_SUITE(inline_executor)

BOOST_AUTO_TEST_CASE(post_runs_synchronously) {
  _InlineExecutor exec;
  int value = 0;
  exec.post([&] { value = 42; });
  BOOST_TEST(value == 42);
}

BOOST_AUTO_TEST_CASE(concurrency_is_one) {
  _InlineExecutor exec;
  BOOST_TEST(exec.concurrency() == 1u);
  BOOST_TEST(exec.is_single_threaded());
}

BOOST_AUTO_TEST_SUITE_END()

#if LEAVES_HAS_THREADS

// Minimal storage that satisfies _ThreadPoolMixin CRTP
struct TestPool : _ThreadPoolMixin<TestPool> {
  TestPool(size_t n = 4) : _ThreadPoolMixin(n) {}
};

BOOST_AUTO_TEST_SUITE(pool_executor)

BOOST_AUTO_TEST_CASE(wraps_thread_pool_mixin) {
  TestPool pool(4);
  _PoolExecutor exec(pool);
  BOOST_TEST(exec.concurrency() == pool.pool_size());
  BOOST_TEST(!exec.is_single_threaded());
}

BOOST_AUTO_TEST_CASE(post_executes_on_worker) {
  TestPool pool(2);
  _PoolExecutor exec(pool);

  std::atomic<int> value{0};
  exec.post([&] { value.store(42, std::memory_order_release); });
  pool.wait_all();
  BOOST_TEST(value.load(std::memory_order_acquire) == 42);
}

BOOST_AUTO_TEST_SUITE_END()

// =========================================================================
// Phase 1: TaskGroup tests
// =========================================================================

BOOST_AUTO_TEST_SUITE(task_group_inline)

BOOST_AUTO_TEST_CASE(spawn_wait_basic) {
  _InlineExecutor exec;
  _TaskGroup<_InlineExecutor> tg(exec);

  int a = 0, b = 0;
  tg.spawn([&] { a = 1; });
  tg.spawn([&] { b = 2; });
  tg.wait();
  BOOST_TEST(a == 1);
  BOOST_TEST(b == 2);
}

BOOST_AUTO_TEST_CASE(exception_propagation) {
  _InlineExecutor exec;
  _TaskGroup<_InlineExecutor> tg(exec);

  tg.spawn([] { throw std::runtime_error("fail"); });
  tg.spawn([] {});  // should be skipped after exception

  BOOST_CHECK_THROW(tg.wait(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(wait_with_dispatcher_ignored) {
  _InlineExecutor exec;
  _TaskGroup<_InlineExecutor> tg(exec);

  int v = 0;
  tg.spawn([&] { v = 10; });
  tg.wait([](auto&&) {}); // dispatcher is ignored for inline
  BOOST_TEST(v == 10);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(task_group_pool)

BOOST_AUTO_TEST_CASE(collect_expand_dispatch) {
  TestPool pool(4);
  _PoolExecutor exec(pool);
  _TaskGroup<_PoolExecutor> tg(exec);

  std::atomic<int> sum{0};
  // Spawn 8 tasks that each add 1
  for (int i = 0; i < 8; i++) {
    tg.spawn([&] { sum.fetch_add(1, std::memory_order_relaxed); });
  }
  // Dispatch to pool
  tg.wait([&](auto&& task) { exec.post(std::move(task)); });
  BOOST_TEST(sum.load() == 8);
}

BOOST_AUTO_TEST_CASE(wait_without_dispatcher_runs_inline) {
  TestPool pool(4);
  _PoolExecutor exec(pool);
  _TaskGroup<_PoolExecutor> tg(exec);

  auto caller_id = std::this_thread::get_id();
  std::thread::id worker_id;

  tg.spawn([&] { worker_id = std::this_thread::get_id(); });
  tg.wait();  // no dispatcher → runs inline
  BOOST_TEST(worker_id == caller_id);
}

BOOST_AUTO_TEST_CASE(exception_from_dispatched_task) {
  TestPool pool(4);
  _PoolExecutor exec(pool);
  _TaskGroup<_PoolExecutor> tg(exec);

  for (int i = 0; i < 4; i++) {
    tg.spawn([i] {
      if (i == 2) throw std::runtime_error("dispatch_error");
    });
  }
  BOOST_CHECK_THROW(
      tg.wait([&](auto&& task) { exec.post(std::move(task)); }),
      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(nested_spawn_expansion) {
  // Simulate trie-like expansion: root spawns 2 children, each spawns 2.
  // With concurrency=4 the BFS expansion should run the first level inline,
  // which spawns 4 tasks, then those 4 get dispatched.
  TestPool pool(4);
  _PoolExecutor exec(pool);
  _TaskGroup<_PoolExecutor> tg(exec);

  std::atomic<int> leaf_count{0};

  // Root level: 2 branches
  tg.spawn([&] {
    // This runs during BFS expansion (inline), spawns children
    tg.spawn([&] { leaf_count.fetch_add(1); });
    tg.spawn([&] { leaf_count.fetch_add(1); });
  });
  tg.spawn([&] {
    tg.spawn([&] { leaf_count.fetch_add(1); });
    tg.spawn([&] { leaf_count.fetch_add(1); });
  });

  tg.wait([&](auto&& task) { exec.post(std::move(task)); });
  BOOST_TEST(leaf_count.load() == 4);
}

BOOST_AUTO_TEST_SUITE_END()

// =========================================================================
// Phase 2: MemManagerPool tests
// =========================================================================

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

// TestResolver acts like a DB for _PooledResolver / _MemManagerPool tests
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

  // Simulated _active_txn for _PooledResolver
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
    size_t old_size = memory.size();
    memory.resize(old_size + AREA_SIZE);
    Area* area = new Area();
    _owned_areas.push_back(area);
    area->offset(old_size);
    area->size(AREA_SIZE);
    area->_ref.store(0);
    return area_ptr(area);
  }

  page_ptr alloc_slot(uint16_t slot) {
    page_ptr result = _backing_mm.alloc(slot, *this);
    result->txn_id = _active_txn->txn_id;
    return result;
  }

  void free(page_ptr) {}
};

BOOST_AUTO_TEST_SUITE(mem_manager_pool)

BOOST_AUTO_TEST_CASE(single_thread_fast_path) {
  TestResolver resolver;
  _MemManagerPool<TestTraits> pool;
  pool.init(sizeof(void*), AREA_SIZE);

  // POOL_SIZE == 4 by default — all managers always active
  auto p1 = pool.alloc(0, resolver);
  BOOST_CHECK(p1 != nullptr);
  BOOST_CHECK(p1->slot_id == 0);

  auto p2 = pool.alloc(1, resolver);
  BOOST_CHECK(p2 != nullptr);
  BOOST_CHECK(p2->slot_id == 1);

  // Free and re-alloc
  pool.free(p1, resolver);
}

BOOST_AUTO_TEST_CASE(multiple_managers_alloc) {
  TestResolver resolver;
  _MemManagerPool<TestTraits> pool;
  pool.init(sizeof(void*), AREA_SIZE);

  // All POOL_SIZE managers are always active; managers 1..N-1 get areas lazily
  std::vector<TestTraits::ptr> pages;
  for (int i = 0; i < 20; i++) {
    auto p = pool.alloc(0, resolver);
    BOOST_CHECK(p != nullptr);
    pages.push_back(p);
  }

  // Free all
  for (auto& p : pages) {
    pool.free(p, resolver);
  }
}

BOOST_AUTO_TEST_CASE(concurrent_alloc_free) {
  TestResolver resolver;
  _MemManagerPool<TestTraits> pool;
  pool.init(sizeof(void*), AREA_SIZE);

  constexpr int N_THREADS = 8;
  constexpr int N_ALLOCS = 100;

  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < N_THREADS; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < N_ALLOCS; i++) {
        auto p = pool.alloc(0, resolver);
        if (p) {
          success_count.fetch_add(1, std::memory_order_relaxed);
          pool.free(p, resolver);
        }
      }
    });
  }

  for (auto& t : threads) t.join();
  BOOST_TEST(success_count.load() == N_THREADS * N_ALLOCS);
}

BOOST_AUTO_TEST_CASE(reinit_locks_after_clone) {
  TestResolver resolver;
  _MemManagerPool<TestTraits> pool;
  pool.init(sizeof(void*), AREA_SIZE);

  // Simulate memcpy + reinit (as clone() would do)
  _MemManagerPool<TestTraits> pool2;
  memcpy(&pool2, &pool, sizeof(pool));
  pool2.reinit_locks();

  // pool2 should be usable
  auto p = pool2.alloc(0, resolver);
  BOOST_CHECK(p != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// =========================================================================
// Phase 2: PooledResolver tests
// =========================================================================

BOOST_AUTO_TEST_SUITE(pooled_resolver)

BOOST_AUTO_TEST_CASE(routes_alloc_to_manager) {
  TestResolver resolver;
  _MemManager<TestTraits> mgr;
  mgr.init(sizeof(void*), AREA_SIZE);

  _PooledResolver<TestResolver, TestTraits> pr(resolver, mgr);

  auto p = pr.alloc_slot(0);
  BOOST_CHECK(p != nullptr);
  BOOST_CHECK(p->slot_id == 0);
  BOOST_CHECK(p->txn_id == resolver._active_txn->txn_id);
}

BOOST_AUTO_TEST_CASE(routes_free_to_manager) {
  TestResolver resolver;
  _MemManager<TestTraits> mgr;
  mgr.init(sizeof(void*), AREA_SIZE);

  _PooledResolver<TestResolver, TestTraits> pr(resolver, mgr);

  auto p = pr.alloc_slot(0);
  BOOST_CHECK(p != nullptr);
  // Should not crash
  pr.free(p);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // LEAVES_HAS_THREADS
