/*
Write-ahead log support for durable storage updates and recovery replay.
*/
#ifndef _LEAVES__WAL_HPP
#define _LEAVES__WAL_HPP
#ifndef __EMSCRIPTEN__

// Write-Ahead Log (logical operation log) for leaves.
//
// Design: next_log / active_log ping-pong state machine with two fixed files
// per DB.  A transaction writes only to the file selected at start_transaction
// (active_log = next_log) and that selection never changes underneath it, so a
// transaction's records + sentinels always land in ONE file.  
//
// Per-file format:
//   [magic: 8 bytes "LVSWAL01"]
//   record*, where each record is one of:
//     [BEGIN   0x01][txn_id: u32 LE]
//     [PUT     0x02][ksz: u32 LE][vsz: u32 LE][key bytes][val bytes]
//     [DELETE  0x03][ksz: u32 LE][key bytes]
//     [PREPARE 0x04]
//     [COMMIT  0x05]
// A transaction is replayable on recovery only if it contains, in order,
// BEGIN ... PREPARE COMMIT.  Any trailing unprepared transaction is discarded

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/_serial.hpp"
#include "../core/_util.hpp"
#include "../core/_exception.hpp"

namespace leaves {

// ---------------------------------------------------------------------------
// Record tags
// ---------------------------------------------------------------------------
enum class _WalOp : uint8_t {
  BEGIN = 0x01,
  PUT = 0x02,
  DEL = 0x03,
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
  uint32_t txn_id{0};
  std::vector<_WalOpRecord> ops;
};

// ---------------------------------------------------------------------------
// WAL shared state that must reside in the DB Header (mmap'd) so that all
// processes sharing a MemoryMappedFile see the same ping-pong state.
// ---------------------------------------------------------------------------
// Under Emscripten (browser / WSM mode) there are no WAL files, so WalState
// is an empty stub — the compiler can eliminate it via [[no_unique_address]].
struct WalState {
  uint64_t write_off[2];                  // current write position per file
  std::atomic<int> next_log{0};           // file index for next transaction
  std::atomic<int> active_log{0};         // file index of active transaction
  std::atomic<uint64_t> last_commit[2];   // last committed txn_id per file
  std::atomic<bool> is_open{false};       // true when WAL files are opened
};

// ---------------------------------------------------------------------------
// Cross-platform raw file helpers
// ---------------------------------------------------------------------------
#ifdef _WIN32
using _wal_fd_t = HANDLE;
static const _wal_fd_t WAL_INVALID_FD = INVALID_HANDLE_VALUE;

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
// wal_parse — read one WAL file, return its replayable (BEGIN+PREPARE+COMMIT)
// transactions.  Incomplete trailing data is silently discarded.
// If a dangling (prepared but not committed) transaction is present at end,
// and dangling is non-null, it is written there.
// ---------------------------------------------------------------------------
inline void wal_parse(const std::string& path, std::vector<_WalTxn>& result,
                      _WalTxn* dangling = nullptr) {
  _wal_fd_t fd = _wal_open(path);
  if (fd == WAL_INVALID_FD) return;

  uint64_t size = _wal_size(fd);
  if (size < WAL_HEADER_SIZE) {
    _wal_close(fd);
    return;
  }

  std::vector<uint8_t> data(size);
  if (!_wal_pread(fd, 0, data.data(), size)) {
    _wal_close(fd);
    return;
  }
  _wal_close(fd);

  if (std::memcmp(data.data(), WAL_MAGIC, sizeof(WAL_MAGIC)) != 0)
    return;

  size_t pos = WAL_HEADER_SIZE;
  const size_t n = data.size();

  bool in_txn = false;
  bool prepared = false;
  _WalTxn cur;

  auto need = [&](size_t bytes) -> bool { return pos + bytes <= n; };

  while (pos < n) {
    uint8_t tag = data[pos];
    if (tag == static_cast<uint8_t>(_WalOp::BEGIN)) {
      if (!need(1 + 4)) break;
      cur = _WalTxn{};
      cur.txn_id = _wal_read_u32(&data[pos + 1]);
      pos += 1 + 4;
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
    } else if (tag == static_cast<uint8_t>(_WalOp::DEL)) {
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

  // If we ended with a prepared but uncommitted transaction, it's the dangling
  // one — return it if the caller asked for it.
  if (in_txn && prepared && dangling) {
    *dangling = std::move(cur);
  }
}

// ---------------------------------------------------------------------------
// _WalWriter — one per DB, owns the two-file pair and the ping-pong state.
// State that must be shared across processes (_write_off, _next_log,
// _active_log, _last_commit, is_open) lives in a WalState struct in the
// DB Header.  fd/path/buf are per-process.
// ---------------------------------------------------------------------------
struct _WalWriter {
  std::string _path[2];
  _wal_fd_t _fd[2]{WAL_INVALID_FD, WAL_INVALID_FD};

  // Pointer to shared state in the DB Header (mmap'd).
  WalState* _state{nullptr};

  // Per-transaction record buffer (BEGIN + PUT/DELETE).  Accumulated during the
  // transaction; written to file[active_log] at prepare().  Safe because the
  // active file is fixed for the whole transaction.
  std::vector<uint8_t> _buf;

  bool _prepared{false};

  bool is_open() const { return _state && _state->is_open.load(); }

  // Ensure file[idx] has a valid magic header; reset write offset.
  void _init_file(int idx) {
    uint64_t size = _wal_size(_fd[idx]);
    if (size < WAL_HEADER_SIZE) {
      _wal_truncate(_fd[idx], 0);
      _wal_pwrite(_fd[idx], 0, WAL_MAGIC, sizeof(WAL_MAGIC));
      _wal_sync(_fd[idx]);
      _state->write_off[idx] = WAL_HEADER_SIZE;
    } else {
      _state->write_off[idx] = size;
    }
  }

  void parse(const std::string& base_path, std::vector<_WalTxn>& out,
             _WalTxn* dangling = nullptr) {
    wal_parse(base_path + ".wal.0", out, dangling);
    wal_parse(base_path + ".wal.1", out, dangling);
    std::sort(out.begin(), out.end(), [](const _WalTxn& a, const _WalTxn& b) {
      return tid_t(static_cast<uint32_t>(a.txn_id)) <
             tid_t(static_cast<uint32_t>(b.txn_id));
    });
  }

  // Open both files; create if missing.  base_path is e.g. "/path/bench.lvs.name".
  // state is a pointer to the WalState in the DB Header (mmap'd).
  // Returns false on failure.
  bool open(const std::string& base_path, WalState* state) {
    _state = state;
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
        _state = nullptr;
        return false;
      }
      _init_file(i);
    }
    _state->next_log.store(0);
    _state->active_log.store(0);
    _state->last_commit[0].store(0);
    _state->last_commit[1].store(0);
    _state->is_open.store(true);
    return true;
  }

  uint64_t active_data_size() const {
    int idx = _state->active_log.load();
    return _state->write_off[idx] - WAL_HEADER_SIZE;
  }

  // Begin a transaction: pin the active file and start the record buffer.
  void begin(uint32_t txn_id) {
    int idx = _state->next_log.load();
    _state->last_commit[idx].store(txn_id);
    _state->active_log.store(idx);
    _buf.clear();
    _buf.push_back(static_cast<uint8_t>(_WalOp::BEGIN));
    _wal_put_u32(_buf, txn_id);
    _prepared = false;
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
    _buf.push_back(static_cast<uint8_t>(_WalOp::DEL));
    _wal_put_u32(_buf, static_cast<uint32_t>(key.size()));
    const uint8_t* kp = reinterpret_cast<const uint8_t*>(key.data());
    _buf.insert(_buf.end(), kp, kp + key.size());
  }

  // Append PREPARE, flush the buffered records to the active file, fdatasync.
  // Throws leaves::WalError on I/O failure.
  void prepare(bool skip_sync = false) {
    if (_prepared) return;  // idempotent
    _buf.push_back(static_cast<uint8_t>(_WalOp::PREPARE));
    int idx = _state->active_log.load();
    if (!_wal_pwrite(_fd[idx], _state->write_off[idx], _buf.data(), _buf.size()))
      throw WalError("WAL prepare: pwrite failed");
    _state->write_off[idx] += _buf.size();
    if (!skip_sync) _wal_sync(_fd[idx]);
    _buf.clear();
    _prepared = true;
  }

  // Append COMMIT, fdatasync, publish last_commit.
  // Throws leaves::WalError on I/O failure.
  void commit() {
    int idx = _state->active_log.load();
    uint8_t rec = static_cast<uint8_t>(_WalOp::COMMIT);
    if (!_wal_pwrite(_fd[idx], _state->write_off[idx], &rec, 1))
      throw WalError("WAL commit: pwrite failed");
    _state->write_off[idx] += 1;
    _wal_sync(_fd[idx]);
  }

  // Abort the current transaction (rollback): drop the in-memory buffer.
  // Nothing was written to disk yet (records are written at prepare()).
  void abort() { _buf.clear(); }

  // Physically clear file[idx] back to just the magic header.
  void truncate(int idx) {
    _wal_truncate(_fd[idx], 0);
    _wal_pwrite(_fd[idx], 0, WAL_MAGIC, sizeof(WAL_MAGIC));
    _wal_sync(_fd[idx]);
    _state->write_off[idx] = WAL_HEADER_SIZE;
  }

  // Remove both WAL files from disk.  Called during recovery *before* open(),
  // so _state and _fd may not be set — we work from the base_path.
  void reset(const std::string& base_path) {
    auto remove_file = [](const std::string& p) {
      // best-effort removal
      std::error_code ec;
      std::filesystem::remove(p, ec);
    };
    remove_file(base_path + ".wal.0");
    remove_file(base_path + ".wal.1");
  }

  void close() {
    if (!is_open()) return;
    for (int i = 0; i < 2; i++) {
      _wal_close(_fd[i]);
      _fd[i] = WAL_INVALID_FD;
    }
    _state->is_open.store(false);
    _state = nullptr;
  }

  void flushed(uint32_t txn_id) {
    int idx = 1 - _state->active_log.load();
    if (tid_t(_state->last_commit[idx].load()) <= tid_t(txn_id)) {
      _wal_truncate(_fd[idx], 0);
      _wal_pwrite(_fd[idx], 0, WAL_MAGIC, sizeof(WAL_MAGIC));
      _wal_sync(_fd[idx]);
      _state->write_off[idx] = WAL_HEADER_SIZE;
      _state->last_commit[idx].store(0);
      _state->next_log.store(idx);
    }
  }
};
}  // namespace leaves
#else // _EMSCRIPTEN__
namespace leaves {
struct WalState {};
}
#endif
#endif  // _LEAVES__WAL_HPP