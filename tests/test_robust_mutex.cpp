#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE RobustMutexTest
#include <boost/test/included/unit_test.hpp>

#include <chrono>
#include <thread>
#include <atomic>
#include <vector>

#include "../include/leaves/intern/_port.hpp"

using namespace leaves;

BOOST_AUTO_TEST_CASE(test_basic_lock_unlock) {
  RobustMutex mutex("test_basic");
  
  // Basic lock/unlock should work
  mutex.lock();
  BOOST_CHECK(!mutex.owner_died());
  mutex.unlock();
}

BOOST_AUTO_TEST_CASE(test_try_lock) {
  RobustMutex mutex("test_try_lock");
  
  // try_lock should succeed when unlocked
  BOOST_CHECK(mutex.try_lock());
  BOOST_CHECK(!mutex.owner_died());
  
  // try_lock should fail when already locked
  BOOST_CHECK(!mutex.try_lock());
  
  mutex.unlock();
  
  // try_lock should succeed again after unlock
  BOOST_CHECK(mutex.try_lock());
  mutex.unlock();
}

BOOST_AUTO_TEST_CASE(test_try_lock_for) {
  RobustMutex mutex("test_try_lock_for");
  
  // try_lock_for should succeed when unlocked
  BOOST_CHECK(mutex.try_lock_for(std::chrono::milliseconds(10)));
  BOOST_CHECK(!mutex.owner_died());
  
  // Start a thread that will try to lock with timeout
  std::atomic<bool> thread_got_lock{false};
  std::thread worker([&]() {
    // This should timeout since the mutex is already locked
    bool success = mutex.try_lock_for(std::chrono::milliseconds(50));
    thread_got_lock.store(success);
    if (success) mutex.unlock();
  });
  
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  worker.join();
  
  // The worker thread should have timed out
  BOOST_CHECK(!thread_got_lock.load());
  
  mutex.unlock();
  
  // Now try_lock_for should succeed again
  BOOST_CHECK(mutex.try_lock_for(std::chrono::milliseconds(10)));
  mutex.unlock();
}

BOOST_AUTO_TEST_CASE(test_concurrent_access) {
  RobustMutex mutex("test_concurrent");
  std::atomic<int> counter{0};
  std::atomic<int> successful_locks{0};
  const int num_threads = 4;
  const int iterations_per_thread = 100;
  
  std::vector<std::thread> threads;
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&, i]() {
      for (int j = 0; j < iterations_per_thread; ++j) {
        if (mutex.try_lock()) {
          successful_locks.fetch_add(1);
          
          // Critical section - increment counter
          int old_val = counter.load();
          std::this_thread::sleep_for(std::chrono::microseconds(1));
          counter.store(old_val + 1);
          
          BOOST_CHECK(!mutex.owner_died());
          mutex.unlock();
        }
        
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  // Counter should equal the number of successful locks
  BOOST_CHECK_EQUAL(counter.load(), successful_locks.load());
  BOOST_CHECK_GT(successful_locks.load(), 0); // At least some locks should succeed
}

BOOST_AUTO_TEST_CASE(test_owner_died_flag) {
  RobustMutex mutex("test_owner_died");
  
  // Initially, owner should not have died
  BOOST_CHECK(!mutex.owner_died());
  
  // After normal lock/unlock, owner should not have died
  mutex.lock();
  BOOST_CHECK(!mutex.owner_died());
  mutex.unlock();
  BOOST_CHECK(!mutex.owner_died());
  
  // Clear the flag explicitly
  mutex.clear_owner_died();
  BOOST_CHECK(!mutex.owner_died());
}

BOOST_AUTO_TEST_CASE(test_recovery) {
  RobustMutex mutex("test_recovery");
  
  // Lock the mutex
  mutex.lock();
  BOOST_CHECK(!mutex.owner_died());
  
  // Simulate recovery scenario
  mutex.recover();
  
  // After recovery, we should be able to lock again
  mutex.lock();
  BOOST_CHECK(!mutex.owner_died());
  mutex.unlock();
}

BOOST_AUTO_TEST_CASE(test_named_mutex_isolation) {
  RobustMutex mutex1("test_isolation_1");
  RobustMutex mutex2("test_isolation_2");
  
  // Different named mutexes should be independent
  mutex1.lock();
  
  // mutex2 should still be lockable
  BOOST_CHECK(mutex2.try_lock());
  
  mutex1.unlock();
  mutex2.unlock();
}

BOOST_AUTO_TEST_CASE(test_multiple_lock_unlock_cycles) {
  RobustMutex mutex("test_cycles");
  
  // Test multiple lock/unlock cycles
  for (int i = 0; i < 100; ++i) {
    mutex.lock();
    BOOST_CHECK(!mutex.owner_died());
    mutex.unlock();
    
    BOOST_CHECK(mutex.try_lock());
    BOOST_CHECK(!mutex.owner_died());
    mutex.unlock();
    
    BOOST_CHECK(mutex.try_lock_for(std::chrono::milliseconds(1)));
    BOOST_CHECK(!mutex.owner_died());
    mutex.unlock();
  }
}