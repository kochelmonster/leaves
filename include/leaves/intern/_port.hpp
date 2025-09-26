#ifndef _LEAVES___PORT_HPP
#define _LEAVES___PORT_HPP

#include <atomic>
#include <chrono>

#include "_util.hpp"
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#if defined(_MSC_VER)
#include <xmmintrin.h>  // For _mm_prefetch and _MM_HINT_T0 on MSVC
#endif

namespace leaves {

// Cross-platform prefetch function
inline void prefetch(const void* ptr, Access access = READ) {
#if defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
  if (access == READ)
    __builtin_prefetch(ptr, 0);
  else
    __builtin_prefetch(ptr, 1);
#elif defined(_MSC_VER)
  // On MSVC, use the equivalent prefetch intrinsic
  _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T0);
#elif defined(__SUNPRO_CC)
  // For Oracle Developer Studio (formerly Sun Studio)
  if (access == READ)
    __builtin_prefetch(ptr, 0);
  else
    __builtin_prefetch(ptr, 1);
#else
  // Fallback: do nothing if no prefetch support
  (void)ptr;
#endif
}

}  // namespace leaves

// ---------------- Additional robust synchronization primitives
// ----------------
#ifndef _LEAVES__PORT_ROBUST_MUTEX_HPP
#define _LEAVES__PORT_ROBUST_MUTEX_HPP

namespace leaves {

// RobustMutex: header-only, cross-platform wrapper
// - Linux/Unix: uses pthread robust mutex if available; supports process-shared
// option
// - Windows: uses (named) mutex; WAIT_ABANDONED indicates owner death
// - Others: falls back to non-robust mutex; OwnerDied will never be reported
struct RobustMutex {
  static constexpr size_t NAME_CAP = 192;
#if defined(_WIN32)
  // Windows implementation
  bool named = false;
  std::atomic<bool> died{false};
  char name_[NAME_CAP]{};

  RobustMutex() = delete;
  explicit RobustMutex(const char* name) { init_with_name(name); }
  ~RobustMutex() { destroy(); }

  void recover() { init_with_name(name_); }

  void destroy() {
    if (h) {
      CloseHandle(h);
      h = nullptr;
    }
  }

  void lock() {
    HANDLE h = get_handle();
    DWORD rc = WaitForSingleObject(h, INFINITE);
    died.store(rc == WAIT_ABANDONED, std::memory_order_release);
  }

  template <class Rep, class Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period>& d) {
    HANDLE h = get_handle();
    DWORD ms = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    DWORD rc = WaitForSingleObject(h, ms);
    if (rc == WAIT_OBJECT_0) {
      died.store(false, std::memory_order_release);
      return true;
    }
    if (rc == WAIT_ABANDONED) {
      died.store(true, std::memory_order_release);
      return true;
    }
    if (rc == WAIT_TIMEOUT) return false;
    return false;
  }

  bool try_lock() {
    HANDLE h = get_handle();
    DWORD rc = WaitForSingleObject(h, 0);
    if (rc == WAIT_OBJECT_0) {
      died.store(false, std::memory_order_release);
      return true;
    }
    if (rc == WAIT_ABANDONED) {
      died.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  void unlock() {
    HANDLE h = get_handle();
    if (h) ReleaseMutex(h);
  }

  bool owner_died() const { return died.load(std::memory_order_acquire); }
  void clear_owner_died() { died.store(false, std::memory_order_release); }

 private:
  void init_with_name(const char* name) {
    // Store ASCII name locally (truncate if necessary)
    size_t n = 0;
    if (name) {
      while (name[n] && n + 1 < NAME_CAP) {
        name_[n] = name[n];
        ++n;
      }
    }
    name_[n] = '\0';
    // Ensure a handle exists in the per-process cache
    (void)get_handle();
    named = (n > 0);
  }

  // Per-process handle cache
  HANDLE get_handle() const {
    // Cache by name string to get the same handle per process
    static std::mutex mtx;
    static std::unordered_map<std::string, HANDLE> cache;
    std::lock_guard<std::mutex> lk(mtx);
    std::string key(name_);
    auto it = cache.find(key);
    if (it != cache.end() && it->second) return it->second;
    // Create or open
    std::wstring wname;
    wname.reserve(key.size());
    for (char c : key) wname.push_back(static_cast<wchar_t>(c));
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    HANDLE h =
        CreateMutexW(&sa, FALSE, wname.empty() ? nullptr : wname.c_str());
    cache[key] = h;
    return h;
  }

#else
  // POSIX implementation (Linux robust if available)
  pthread_mutex_t m;
  bool inited = false;
  std::atomic<bool> died{false};
  char name_[NAME_CAP]{};  // stored for parity and potential diagnostics

  RobustMutex() = delete;
  explicit RobustMutex(const char* name) { init_with_name(name); }
  ~RobustMutex() { destroy(); }
  void recover() { init_with_name(name_); }

  void destroy() {
    if (inited) {
      pthread_mutex_destroy(&m);
      inited = false;
    }
  }

  void lock() {
    if (!inited) recover();
    int rc = pthread_mutex_lock(&m);
#ifdef PTHREAD_MUTEX_ROBUST
    if (rc == EOWNERDEAD) {
      died.store(true, std::memory_order_release);
      pthread_mutex_consistent(&m);
      return;
    }
#endif
    (void)rc;
    died.store(false, std::memory_order_release);
  }

  bool try_lock() {
    if (!inited) recover();
    int rc = pthread_mutex_trylock(&m);
#ifdef PTHREAD_MUTEX_ROBUST
    if (rc == EOWNERDEAD) {
      died.store(true, std::memory_order_release);
      pthread_mutex_consistent(&m);
      return true;
    }
#endif
    if (rc == 0) {
      died.store(false, std::memory_order_release);
      return true;
    }
    return false;
  }

  template <class Rep, class Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period>& d) {
    if (!inited) recover();
#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS >= 0)
    using namespace std::chrono;
    auto now = system_clock::now();
    auto abs = now + d;
    timespec ts;
    auto secs = time_t(duration_cast<seconds>(abs.time_since_epoch()).count());
    auto nsec =
        long(duration_cast<nanoseconds>(abs.time_since_epoch()).count() -
             secs * 1000000000LL);
    ts.tv_sec = secs;
    ts.tv_nsec = nsec;
    int rc = pthread_mutex_timedlock(&m, &ts);
#ifdef PTHREAD_MUTEX_ROBUST
    if (rc == EOWNERDEAD) {
      died.store(true, std::memory_order_release);
      pthread_mutex_consistent(&m);
      return true;
    }
#endif
    if (rc == 0) {
      died.store(false, std::memory_order_release);
      return true;
    }
    if (rc == ETIMEDOUT) return false;
    return false;
#else
    // Fallback: emulate with polling
    auto deadline = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < deadline) {
      if (try_lock()) return true;
      // tiny sleep to avoid busy-waiting
      timespec ts = {0, 1000000};
      nanosleep(&ts, nullptr);
    }
    return false;
#endif
  }

  void unlock() {
    if (inited) pthread_mutex_unlock(&m);
  }

  bool owner_died() const { return died.load(std::memory_order_acquire); }
  void clear_owner_died() { died.store(false, std::memory_order_release); }

 private:
  void init_with_name(const char* name) {
    // Persist name for potential diagnostics; configure robust, process-shared
    // mutex
    size_t n = 0;
    if (name) {
      while (name[n] && n + 1 < NAME_CAP) {
        name_[n] = name[n];
        ++n;
      }
    }
    name_[n] = '\0';
    destroy();
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
      inited = false;
      return;
    }
#ifdef PTHREAD_PROCESS_SHARED
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#endif
#ifdef PTHREAD_MUTEX_ROBUST
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
#ifdef __linux__
#ifdef PTHREAD_MUTEX_ADAPTIVE_NP
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
#endif
#endif
    int rc = pthread_mutex_init(&m, &attr);
    pthread_mutexattr_destroy(&attr);
    inited = (rc == 0);
  }
#endif
};

}  // namespace leaves

#endif  // _LEAVES__PORT_ROBUST_MUTEX_HPP
#endif  // _LEAVES___PORT_HPP