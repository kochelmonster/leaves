#ifndef _LEAVES___PORT_HPP
#define _LEAVES___PORT_HPP

#include <atomic>
#include <chrono>
#include <cstring>

#include "_util.hpp"
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <string>
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

// ============================================================================
// RobustMutex: Cross-platform robust mutex wrapper
// ============================================================================
// - Linux/Unix: pthread robust mutex with PROCESS_SHARED and EOWNERDEAD support
// - Windows: Named mutex with WAIT_ABANDONED detection
// - Detects when previous owner died while holding the lock
// ============================================================================

struct RobustMutex {
  static constexpr size_t NAME_CAP = 192;

  // Common interface
  RobustMutex() = delete;
  explicit RobustMutex(const char* name);
  ~RobustMutex();
  
  RobustMutex(const RobustMutex&) = delete;
  RobustMutex& operator=(const RobustMutex&) = delete;

  void lock();
  bool try_lock();
  template <class Rep, class Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period>& d);
  void unlock();
  
  void recover();
  bool owner_died() const { return died_.load(std::memory_order_acquire); }
  void clear_owner_died() { died_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> died_{false};
  char name_[NAME_CAP]{};
  
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
  
  void init_with_name(const char* name);
  void destroy();
  bool wait_and_check(DWORD timeout_ms);
#else
  pthread_mutex_t mutex_;
  bool inited_ = false;
  
  void init_with_name(const char* name);
  void destroy();
#endif
};

// ============================================================================
// Implementation
// ============================================================================

inline RobustMutex::RobustMutex(const char* name) {
  init_with_name(name);
}

inline RobustMutex::~RobustMutex() {
  destroy();
}

inline void RobustMutex::recover() {
  init_with_name(name_);
}

#if defined(_WIN32)
// ========== Windows Implementation ==========

inline void RobustMutex::init_with_name(const char* name) {
  // Store name for diagnostics and recovery
  size_t n = 0;
  if (name) {
    while (name[n] && n + 1 < NAME_CAP) {
      name_[n] = name[n];
      ++n;
    }
  }
  name_[n] = '\0';
  
  destroy();
  
  // Convert to wide string for Windows API
  std::wstring wname;
  if (n > 0) {
    wname.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(name_[i])));
    }
  }
  
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  handle_ = CreateMutexW(&sa, FALSE, wname.empty() ? nullptr : wname.c_str());
}

inline void RobustMutex::destroy() {
  if (handle_) {
    CloseHandle(handle_);
    handle_ = nullptr;
  }
}

inline bool RobustMutex::wait_and_check(DWORD timeout_ms) {
  if (!handle_) return false;
  
  DWORD rc = WaitForSingleObject(handle_, timeout_ms);
  if (rc == WAIT_OBJECT_0) {
    died_.store(false, std::memory_order_release);
    return true;
  }
  if (rc == WAIT_ABANDONED) {
    died_.store(true, std::memory_order_release);
    return true;
  }
  return false;
}

inline void RobustMutex::lock() {
  if (!handle_) recover();
  DWORD rc = WaitForSingleObject(handle_, INFINITE);
  died_.store(rc == WAIT_ABANDONED, std::memory_order_release);
}

inline bool RobustMutex::try_lock() {
  if (!handle_) recover();
  return wait_and_check(0);
}

template <class Rep, class Period>
inline bool RobustMutex::try_lock_for(const std::chrono::duration<Rep, Period>& d) {
  if (!handle_) recover();
  DWORD ms = static_cast<DWORD>(
      std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
  return wait_and_check(ms);
}

inline void RobustMutex::unlock() {
  if (handle_) ReleaseMutex(handle_);
}

#else
// ========== POSIX Implementation ==========

inline void RobustMutex::init_with_name(const char* name) {
  // Store name for diagnostics
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
    inited_ = false;
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
  
  int rc = pthread_mutex_init(&mutex_, &attr);
  pthread_mutexattr_destroy(&attr);
  inited_ = (rc == 0);
}

inline void RobustMutex::destroy() {
  if (inited_) {
    pthread_mutex_destroy(&mutex_);
    inited_ = false;
  }
}

inline void RobustMutex::lock() {
  if (!inited_) recover();
  
  [[maybe_unused]] int rc = pthread_mutex_lock(&mutex_);
#ifdef PTHREAD_MUTEX_ROBUST
  if (rc == EOWNERDEAD) {
    died_.store(true, std::memory_order_release);
    pthread_mutex_consistent(&mutex_);
    return;
  }
#endif
  died_.store(false, std::memory_order_release);
}

inline bool RobustMutex::try_lock() {
  if (!inited_) recover();
  
  int rc = pthread_mutex_trylock(&mutex_);
#ifdef PTHREAD_MUTEX_ROBUST
  if (rc == EOWNERDEAD) {
    died_.store(true, std::memory_order_release);
    pthread_mutex_consistent(&mutex_);
    return true;
  }
#endif
  if (rc == 0) {
    died_.store(false, std::memory_order_release);
    return true;
  }
  return false;
}

template <class Rep, class Period>
inline bool RobustMutex::try_lock_for(const std::chrono::duration<Rep, Period>& d) {
  if (!inited_) recover();
  
#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS >= 0)
  using namespace std::chrono;
  auto abs = system_clock::now() + d;
  auto secs = duration_cast<seconds>(abs.time_since_epoch()).count();
  auto nsec = (duration_cast<nanoseconds>(abs.time_since_epoch()).count() -
               secs * 1000000000LL);
  
  timespec ts;
  ts.tv_sec = static_cast<time_t>(secs);
  ts.tv_nsec = static_cast<long>(nsec);
  
  int rc = pthread_mutex_timedlock(&mutex_, &ts);
#ifdef PTHREAD_MUTEX_ROBUST
  if (rc == EOWNERDEAD) {
    died_.store(true, std::memory_order_release);
    pthread_mutex_consistent(&mutex_);
    return true;
  }
#endif
  if (rc == 0) {
    died_.store(false, std::memory_order_release);
    return true;
  }
  return (rc != ETIMEDOUT);
#else
  // Fallback: emulate with polling
  auto deadline = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < deadline) {
    if (try_lock()) return true;
    timespec ts = {0, 1000000};  // 1ms sleep
    nanosleep(&ts, nullptr);
  }
  return false;
#endif
}

inline void RobustMutex::unlock() {
  if (inited_) {
    pthread_mutex_unlock(&mutex_);
  }
}

#endif  // _WIN32

}  // namespace leaves


#endif  // _LEAVES___PORT_HPP