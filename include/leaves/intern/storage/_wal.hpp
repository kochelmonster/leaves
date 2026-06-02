#ifndef _LEAVES__WAL_HPP
#define _LEAVES__WAL_HPP

// Write-Ahead Log (logical operation log) for leaves.
//
// Design: next_log / active_log ping-pong state machine with two fixed files
// per DB.  A transaction writes only to the file selected at start_transaction
// (active_log = next_log) and that selection never changes underneath it, so a
// transaction's records + sentinels always land in ONE file.  A storage-wide
// background thread (_WalManager) periodically flushes the main DB and switches
// next_log to the other (truncated) file.  No swap mutex needed — only atomics.
//
// Per-file format:
//   [magic: 8 bytes "LVSWAL01"]
//   record*, where each record is one of:
//     [BEGIN   0x01][txn_id: u64 LE]
//     [PUT     0x02][ksz: u32 LE][vsz: u32 LE][key bytes][val bytes]
//     [DELETE  0x03][ksz: u32 LE][key bytes]
//     [PREPARE 0x04]
//     [COMMIT  0x05]
// A transaction is replayable on recovery only if it contains, in order,
// BEGIN ... PREPARE COMMIT.  Any trailing incomplete transaction is discarded.

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/_util.hpp"  // Slice, tid_t

namespace leaves {

// ---------------------------------------------------------------------------
// Record tags
// ---------------------------------------------------------------------------
enum class _WalOp : uint8_t {
  BEGIN = 0x01,
  PUT = 0x02,
  DELETE = 0x03,
  PREPARE = 0x04,
  COMMIT = 0x05,
};

static constexpr char WAL_MAGIC[8] = {'L', 'V', 'S', 'W', 'A', 'L', '0', '1'};
static constexpr uint64_t WAL_HEADER_SIZE = sizeof(WAL_MAGIC);

// ---------------------------------------------------------------------------
// Parsed transaction (recovery)
// ---------------------------------------------------------------------------
struct _WalOpRecord {
  bool is_delete{false};
  std::string key;
  std::string val;
};

struct _WalTxn {
  uint64_t txn_id{0};
  std::vector<_WalOpRecord> ops;
};

// ---------------------------------------------------------------------------
// Cross-platform raw file helpers
// ---------------------------------------------------------------------------
#ifdef _WIN32
using _wal_fd_t = HANDLE;
static constexpr _wal_fd_t WAL_INVALID_FD = INVALID_HANDLE_VALUE;

inline _wal_fd_t _wal_open(const std::string& path) {
  return CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
}
inline void _wal_close(_wal_fd_t fd) {
  if (fd != WAL_INVALID_FD) CloseHandle(fd);
}
inline uint64_t _wal_size(_wal_fd_t fd) {
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(fd, &sz)) return 0;
  return static_cast<uint64_t>(sz.QuadPart);
}
inline bool _wal_pwrite(_wal_fd_t fd, uint64_t off, const void* data,
                        size_t size) {
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>(off >> 32);
  DWORD written = 0;
  return WriteFile(fd, data, static_cast<DWORD>(size), &written, &ov) &&
         written == size;
}
inline bool _wal_pread(_wal_fd_t fd, uint64_t off, void* data, size_t size) {
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>(off >> 32);
  DWORD got = 0;
  return ReadFile(fd, data, static_cast<DWORD>(size), &got, &ov) &&
         got == size;
}
inline void _wal_sync(_wal_fd_t fd) { FlushFileBuffers(fd); }
inline void _wal_truncate(_wal_fd_t fd, uint64_t size) {
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(size);
  SetFilePointerEx(fd, li, nullptr, FILE_BEGIN);
  SetEndOfFile(fd);
}
#else
using _wal_fd_t = int;
static constexpr _wal_fd_t WAL_INVALID_FD = -1;

inline _wal_fd_t _wal_open(const std::string& path) {
  return ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
}
inline void _wal_close(_wal_fd_t fd) {
  if (fd != WAL_INVALID_FD) ::close(fd);
}
inline uint64_t _wal_size(_wal_fd_t fd) {
  off_t end = ::lseek(fd, 0, SEEK_END);
  return end < 0 ? 0 : static_cast<uint64_t>(end);
}
inline bool _wal_pwrite(_wal_fd_t fd, uint64_t off, const void* data,
                        size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    ssize_t n = ::pwrite(fd, p, size, static_cast<off_t>(off));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    p += n;
    off += static_cast<uint64_t>(n);
    size -= static_cast<size_t>(n);
  }
  return true;
}
inline bool _wal_pread(_wal_fd_t fd, uint64_t off, void* data, size_t size) {
  char* p = static_cast<char*>(data);
  while (size > 0) {
    ssize_t n = ::pread(fd, p, size, static_cast<off_t>(off));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    p += n;
    off += static_cast<uint64_t>(n);
    size -= static_cast<size_t>(n);
  }
  return true;
}
inline void _wal_sync(_wal_fd_t fd) {
#if defined(__APPLE__)
  ::fsync(fd);
#else
  ::fdatasync(fd);
#endif
}
inline void _wal_truncate(_wal_fd_t fd, uint64_t size) {
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    // best effort
  }
}
#endif

// ---------------------------------------------------------------------------
// Little-endian serialization helpers
// ---------------------------------------------------------------------------
inline void _wal_put_u32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline void _wal_put_u64(std::vector<uint8_t>& buf, uint64_t v) {
  for (int i = 0; i < 8; i++)
    buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
inline uint32_t _wal_read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t _wal_read_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  return v;
}

// ---------------------------------------------------------------------------
// _WalWriter — one per DB, owns the two-file pair and the ping-pong state.
// ---------------------------------------------------------------------------
struct _WalWriter {
  std::string _path[2];
  _wal_fd_t _fd[2]{WAL_INVALID_FD, WAL_INVALID_FD};
  uint64_t _write_off[2]{WAL_HEADER_SIZE, WAL_HEADER_SIZE};

  // Ping-pong state (see file header comment).
  std::atomic<int> _next_log{0};    // changed only by WAL thread
  std::atomic<int> _active_log{0};  // changed only by start_transaction
  std::atomic<uint64_t> _last_commit{0};   // changed only by commit
  std::atomic<uint64_t> _flushed_upto{0};  // changed only by WAL thread

  // Per-transaction record buffer (BEGIN + PUT/DELETE).  Accumulated during the
  // transaction; written to file[active_log] at prepare().  Safe because the
  // active file is fixed for the whole transaction.
  std::vector<uint8_t> _buf;

  bool _open{false};

  // Ensure file[idx] has a valid magic header; reset write offset.
  void _init_file(int idx) {
    uint64_t size = _wal_size(_fd[idx]);
    if (size < WAL_HEADER_SIZE) {
      _wal_truncate(_fd[idx], 0);
      _wal_pwrite(_fd[idx], 0, WAL_MAGIC, sizeof(WAL_MAGIC));
      _wal_sync(_fd[idx]);
      _write_off[idx] = WAL_HEADER_SIZE;
    } else {
      _write_off[idx] = size;
    }
  }

  // Open both files; create if missing.  base_path is e.g. "/path/bench.lvs.name".
  // Returns false on failure.
  bool open(const std::string& base_path) {
    _path[0] = base_path + ".wal.0";
    _path[1] = base_path + ".wal.1";
    for (int i = 0; i < 2; i++) {
      _fd[i] = _wal_open(_path[i]);
      if (_fd[i] == WAL_INVALID_FD) {
        // rollback partial open
        for (int j = 0; j < i; j++) {
          _wal_close(_fd[j]);
          _fd[j] = WAL_INVALID_FD;
        }
        return false;
      }
      _init_file(i);
    }
    _next_log.store(0);
    _active_log.store(0);
    _open = true;
    return true;
  }

  bool is_open() const { return _open; }

  // Begin a transaction: pin the active file and start the record buffer.
  void begin(uint64_t txn_id) {
    int idx = _next_log.load();
    _active_log.store(idx);
    _buf.clear();
    _buf.push_back(static_cast<uint8_t>(_WalOp::BEGIN));
    _wal_put_u64(_buf, txn_id);
  }

  void put(const Slice& key, const Slice& val) {
    _buf.push_back(static_cast<uint8_t>(_WalOp::PUT));
    _wal_put_u32(_buf, static_cast<uint32_t>(key.size()));
    _wal_put_u32(_buf, static_cast<uint32_t>(val.size()));
    const uint8_t* kp = reinterpret_cast<const uint8_t*>(key.data());
    const uint8_t* vp = reinterpret_cast<const uint8_t*>(val.data());
    _buf.insert(_buf.end(), kp, kp + key.size());
    _buf.insert(_buf.end(), vp, vp + val.size());
  }

  void del(const Slice& key) {
    _buf.push_back(static_cast<uint8_t>(_WalOp::DELETE));
    _wal_put_u32(_buf, static_cast<uint32_t>(key.size()));
    const uint8_t* kp = reinterpret_cast<const uint8_t*>(key.data());
    _buf.insert(_buf.end(), kp, kp + key.size());
  }

  // Append PREPARE, flush the buffered records to the active file, fdatasync.
  void prepare() {
    _buf.push_back(static_cast<uint8_t>(_WalOp::PREPARE));
    int idx = _active_log.load();
    _wal_pwrite(_fd[idx], _write_off[idx], _buf.data(), _buf.size());
    _write_off[idx] += _buf.size();
    _wal_sync(_fd[idx]);
    _buf.clear();
  }

  // Append COMMIT, fdatasync, publish last_commit.
  void commit(uint64_t txn_id) {
    int idx = _active_log.load();
    uint8_t rec = static_cast<uint8_t>(_WalOp::COMMIT);
    _wal_pwrite(_fd[idx], _write_off[idx], &rec, 1);
    _write_off[idx] += 1;
    _wal_sync(_fd[idx]);
    _last_commit.store(txn_id);
  }

  // Abort the current transaction (rollback): drop the in-memory buffer.
  // Nothing was written to disk yet (records are written at prepare()).
  void abort() { _buf.clear(); }

  // Physically clear file[idx] back to just the magic header.
  void truncate(int idx) {
    _wal_truncate(_fd[idx], 0);
    _wal_pwrite(_fd[idx], 0, WAL_MAGIC, sizeof(WAL_MAGIC));
    _wal_sync(_fd[idx]);
    _write_off[idx] = WAL_HEADER_SIZE;
  }

  void close() {
    if (!_open) return;
    for (int i = 0; i < 2; i++) {
      _wal_close(_fd[i]);
      _fd[i] = WAL_INVALID_FD;
    }
    _open = false;
  }
};

// ---------------------------------------------------------------------------
// wal_parse — read one WAL file, return its replayable (BEGIN+PREPARE+COMMIT)
// transactions.  Incomplete trailing data is silently discarded.
// ---------------------------------------------------------------------------
inline std::vector<_WalTxn> wal_parse(const std::string& path) {
  std::vector<_WalTxn> result;
  _wal_fd_t fd = _wal_open(path);
  if (fd == WAL_INVALID_FD) return result;

  uint64_t size = _wal_size(fd);
  if (size < WAL_HEADER_SIZE) {
    _wal_close(fd);
    return result;
  }

  std::vector<uint8_t> data(size);
  if (!_wal_pread(fd, 0, data.data(), size)) {
    _wal_close(fd);
    return result;
  }
  _wal_close(fd);

  if (std::memcmp(data.data(), WAL_MAGIC, sizeof(WAL_MAGIC)) != 0)
    return result;

  size_t pos = WAL_HEADER_SIZE;
  const size_t n = data.size();

  bool in_txn = false;
  bool prepared = false;
  _WalTxn cur;

  auto need = [&](size_t bytes) -> bool { return pos + bytes <= n; };

  while (pos < n) {
    uint8_t tag = data[pos];
    if (tag == static_cast<uint8_t>(_WalOp::BEGIN)) {
      if (!need(1 + 8)) break;
      cur = _WalTxn{};
      cur.txn_id = _wal_read_u64(&data[pos + 1]);
      pos += 1 + 8;
      in_txn = true;
      prepared = false;
    } else if (tag == static_cast<uint8_t>(_WalOp::PUT)) {
      if (!in_txn || !need(1 + 8)) break;
      uint32_t ksz = _wal_read_u32(&data[pos + 1]);
      uint32_t vsz = _wal_read_u32(&data[pos + 5]);
      if (!need(1 + 8 + static_cast<size_t>(ksz) + vsz)) break;
      _WalOpRecord op;
      op.is_delete = false;
      op.key.assign(reinterpret_cast<const char*>(&data[pos + 9]), ksz);
      op.val.assign(reinterpret_cast<const char*>(&data[pos + 9 + ksz]), vsz);
      cur.ops.push_back(std::move(op));
      pos += 1 + 8 + ksz + vsz;
    } else if (tag == static_cast<uint8_t>(_WalOp::DELETE)) {
      if (!in_txn || !need(1 + 4)) break;
      uint32_t ksz = _wal_read_u32(&data[pos + 1]);
      if (!need(1 + 4 + static_cast<size_t>(ksz))) break;
      _WalOpRecord op;
      op.is_delete = true;
      op.key.assign(reinterpret_cast<const char*>(&data[pos + 5]), ksz);
      cur.ops.push_back(std::move(op));
      pos += 1 + 4 + ksz;
    } else if (tag == static_cast<uint8_t>(_WalOp::PREPARE)) {
      if (!in_txn) break;
      prepared = true;
      pos += 1;
    } else if (tag == static_cast<uint8_t>(_WalOp::COMMIT)) {
      if (!in_txn) break;
      pos += 1;
      if (prepared) result.push_back(std::move(cur));
      in_txn = false;
      prepared = false;
    } else {
      // Unknown/garbage tag — stop parsing (truncated/corrupt tail).
      break;
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// _WalManager — storage-wide background checkpoint thread.  Manages the
// registered set of _WalWriter pairs (one per WAL-enabled DB).
// ---------------------------------------------------------------------------
struct _WalManager {
  std::thread _thread;
  std::mutex _mutex;
  std::condition_variable _cv;
  std::vector<_WalWriter*> _wals;
  bool _stop{false};
  bool _running{false};
  uint32_t _interval_ms{200};
  std::function<void()> _flush_fn;

  ~_WalManager() { stop(); }

  // Register a WAL writer.  Lazily starts the background thread on first
  // registration so non-WAL workloads pay nothing.
  void register_wal(_WalWriter* w, std::function<void()> flush_fn) {
    std::lock_guard<std::mutex> lk(_mutex);
    _wals.push_back(w);
    if (!_running) {
      _flush_fn = std::move(flush_fn);
      _stop = false;
      _running = true;
      _thread = std::thread([this] { run(); });
    }
  }

  void unregister_wal(_WalWriter* w) {
    std::lock_guard<std::mutex> lk(_mutex);
    _wals.erase(std::remove(_wals.begin(), _wals.end(), w), _wals.end());
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(_mutex);
      if (!_running) return;
      _stop = true;
    }
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();
    _running = false;
  }

  void run() {
    std::unique_lock<std::mutex> lk(_mutex);
    while (!_stop) {
      _cv.wait_for(lk, std::chrono::milliseconds(_interval_ms),
                   [this] { return _stop; });
      if (_stop) break;
      checkpoint_all(lk);
    }
  }

  // Called with _mutex held.  Keeps the lock through the flush so a
  // concurrently destructing DB (unregister_wal) cannot free a _WalWriter
  // mid-checkpoint.
  void checkpoint_all(std::unique_lock<std::mutex>& /*lk*/) {
    bool any_pending = false;
    for (auto* w : _wals) {
      if (w->_last_commit.load() != w->_flushed_upto.load()) {
        any_pending = true;
        break;
      }
    }
    if (!any_pending) return;

    // Snapshot last_commit BEFORE the flush; the flush persists at least these.
    std::vector<uint64_t> snap(_wals.size());
    for (size_t i = 0; i < _wals.size(); i++)
      snap[i] = _wals[i]->_last_commit.load();

    if (_flush_fn) _flush_fn();  // storage.flush(sync=true): persist + fsync DB

    for (size_t i = 0; i < _wals.size(); i++) {
      _WalWriter* w = _wals[i];
      w->_flushed_upto.store(snap[i]);
      // Switch only when the active file equals next_log (no in-flight txn is
      // bound to the other file): truncate the other and hand it over.
      int next = w->_next_log.load();
      if (next == w->_active_log.load()) {
        int other = 1 - next;
        w->truncate(other);
        w->_next_log.store(other);
      }
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES__WAL_HPP
