#ifndef _LEAVES__FSTORE_HPP
#define _LEAVES__FSTORE_HPP

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "_cachestore.hpp"
#include "../db/_db.hpp"
#include "../core/_exception.hpp"
#include "../memory/_memory.hpp"  // for AreaSlice, SmartPointer
#include "../core/_node.hpp"    // for _TrieNode
#include "../core/_port.hpp"
#include "../core/_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access

// Removed interprocess mutex and process ID references since they're no longer
// needed

namespace leaves {

static const char FSTORE_SIGNATURE[] = "larch-leaves-fstore";
static const size_t FSTORE_SIGNATURE_SIZE =
    padding(sizeof(FSTORE_SIGNATURE), 8);

// definition of all headers and data types
struct _StoreTraits {
  using Aspect = DefaultAspect;
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  struct PageHeader {
    typedef PageHeader Base;
    tid_t txn_id;
    uint16_e used;
    uint8_t slot_id;

    template <typename DB>
    bool needs_cow(const DB* db) const {
      return txn_id != db->transaction_active();
    }
  };

  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 128 * K;  // not OS AREA_SIZE
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t MEM_MANAGER_POOL_SIZE = 1;
  static constexpr uint16_t PAGE_SIZES[] = {                    // Page sizes (header + node)
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 2),    // 2 branches
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 3),    // 3 branches
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 4),    // 4 branches
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 10),   // 5-10 branches
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 16),   // hex 0-9A-F
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 64),   // base64
      sizeof(PageHeader) + _TrieNode<_StoreTraits>::size(1, 256),  // binary
      4 * K};
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);
  using ptr = SmartPointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SmartPointer<T, type>;
};

struct _FileOperations : _CacheBase {
  // Placeholder struct kept in FileHeader for layout compatibility.
  // The actual lock used by file_lock() is _file_mutex below.
  struct Mutex {
    template <typename Time = std::chrono::seconds>
    void lock(Time /*t*/ = Time(10)) {}
    bool try_lock() { return true; }
    void unlock() {}
  };

  struct CtxMutex {
    void lock() {}
    void unlock() {}
  };

  struct CtxCondVar {
    template <typename L> void wait(L&) {}
    void notify_one() {}
  };

  struct FileHeader {
    char signature[FSTORE_SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;     // Kept for compatibility but no longer needed
    AreaPool area_pool;  // pool for both single and multi areas
    uint32_t sanitize_generation;  // incremented on each storage open
    uint16_t db_entry_count;  // entries used in first directory page
    offset_t db_next_page;    // link to overflow directory page (0 = none)
    DBEntry dbs[];            // flexible array fills to 4K boundary

    FileHeader()
        : signature{},
          db_version(0),
          file_size(0),
          file_lock{},
          area_pool{},
          sanitize_generation(0),
          db_entry_count(0),
          db_next_page(0) {
      std::memset(signature, 0, sizeof(signature));
      std::strcpy(signature, FSTORE_SIGNATURE);
      area_pool.init();
      uint16_t cap = _DBDirectoryPage::capacity_for(4 * K - sizeof(FileHeader));
      std::memset((void*)dbs, 0, sizeof(DBEntry) * cap);
    }
  };

  std::string _filepath;
#ifdef _WIN32
  HANDLE _fd = INVALID_HANDLE_VALUE;
#else
  int _fd = -1;
#endif
  FileHeader* _header;
  // Real lock for area allocation serialization (returned by file_lock())
  mutable std::mutex _file_mutex;

  size_t file_size() const { return _header->file_size; }

  size_t calc_header_size() const {
    return 4 * K;
  }

  void open(const char* path) {
    _filepath = path;
#ifdef _WIN32
    _fd = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (_fd == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Failed to open file: error " +
                               std::to_string(GetLastError()));
    }
#else
    _fd = ::open(path, O_RDWR | O_CREAT, 0644);
    if (_fd < 0) {
      throw std::runtime_error("Failed to open file: " +
                               std::string(std::strerror(errno)));
    }
#endif
  }

  void close() {
#ifdef _WIN32
    if (_fd != INVALID_HANDLE_VALUE) {
      CloseHandle(_fd);
      _fd = INVALID_HANDLE_VALUE;
    }
#else
    if (_fd >= 0) {
      ::close(_fd);
      _fd = -1;
    }
#endif
  }

  void write(offset_t offset, const void* ptr, size_t size) const {
    if (size == 0) return;
    auto* src = static_cast<const char*>(ptr);
    uint64_t file_offset = static_cast<uint64_t>(offset);
    size_t written = 0;
    while (written < size) {
#ifdef _WIN32
      OVERLAPPED ov = {};
      uint64_t pos = file_offset + written;
      ov.Offset = static_cast<DWORD>(pos);
      ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
      DWORD n = 0;
      DWORD to_write = static_cast<DWORD>(
          std::min<size_t>(size - written, MAXDWORD));
      if (!WriteFile(_fd, src + written, to_write, &n, &ov) || n == 0) {
        throw std::runtime_error("Failed to write data: error " +
                                 std::to_string(GetLastError()));
      }
#else
      ssize_t n = ::pwrite(_fd, src + written, size - written,
                           static_cast<off_t>(file_offset + written));
      if (n <= 0) {
        throw std::runtime_error("Failed to write data: " +
                                 std::string(std::strerror(errno)));
      }
#endif
      written += n;
    }
  }

  void read(offset_t offset, void* ptr, size_t size) const {
    auto* dst = static_cast<char*>(ptr);
    uint64_t file_offset = static_cast<uint64_t>(offset);
    size_t total = 0;
    while (total < size) {
#ifdef _WIN32
      OVERLAPPED ov = {};
      uint64_t pos = file_offset + total;
      ov.Offset = static_cast<DWORD>(pos);
      ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
      DWORD n = 0;
      DWORD to_read = static_cast<DWORD>(
          std::min<size_t>(size - total, MAXDWORD));
      if (!ReadFile(_fd, dst + total, to_read, &n, &ov) || n == 0) {
        throw std::runtime_error("Failed to read data: error " +
                                 std::to_string(GetLastError()));
      }
#else
      ssize_t n = ::pread(_fd, dst + total, size - total,
                          static_cast<off_t>(file_offset + total));
      if (n <= 0) {
        throw std::runtime_error("Failed to read data: " +
                                 std::string(std::strerror(errno)));
      }
#endif
      total += n;
    }
  }

  void resize(size_t new_size) const {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(new_size);
    if (!SetFilePointerEx(_fd, li, NULL, FILE_BEGIN) ||
        !SetEndOfFile(_fd)) {
      throw std::runtime_error("Failed to resize file: error " +
                               std::to_string(GetLastError()));
    }
#else
    if (::ftruncate(_fd, static_cast<off_t>(new_size)) != 0) {
      throw std::runtime_error("Failed to resize file: " +
                               std::string(std::strerror(errno)));
    }
#endif
  }

  template <typename BlockVector>
  void write_batch(BlockVector& blocks_to_write, bool write_header = false) {
    std::sort(blocks_to_write.begin(), blocks_to_write.end(),
              [](const auto& a, const auto& b) {
                return a.area()->offset() < b.area()->offset();
              });

    // Process sorted blocks with batch writing for contiguous regions
    size_t i = 0;
    while (i < blocks_to_write.size()) {
      const auto& start_block = blocks_to_write[i];
      const auto& start_area = start_block.area();
      uint64_t start_offset = start_area->offset();
      uint64_t current_size = start_area->size();
      size_t batch_end = i;

      // Look for contiguous blocks
      for (size_t j = i + 1; j < blocks_to_write.size(); j++) {
        const auto& next_area = blocks_to_write[j].area();

        // Check if this block is contiguous with the previous ones
        if (next_area->offset() == start_offset + current_size) {
          current_size += next_area->size();
          batch_end = j;
        } else {
          // Not contiguous, stop here
          break;
        }
      }

      if (batch_end == i) {
        // No contiguous blocks found, write the single block
        write(start_offset, start_area, start_area->size());
        i++;
      } else {
        // We have contiguous blocks, allocate a temporary buffer and copy data
        std::vector<char> buffer(current_size);
        char* dest = buffer.data();

        // Copy all contiguous blocks to the buffer
        for (size_t j = i; j <= batch_end; j++) {
          const auto& area = blocks_to_write[j].area();
          std::memcpy(dest, area, area->size());
          dest += area->size();
        }

        // Write the entire batch at once
        write(start_offset, buffer.data(), current_size);

        // Move to the next non-contiguous block
        i = batch_end + 1;
      }
    }

    if (write_header) {
      write(0, _header, calc_header_size());
    }
  }

  // No-op: native pwrite/WriteFile calls are synchronous
  void sync_writes() {}
  bool has_pending_writes() const { return false; }

  const char* filename() const { return _filepath.c_str(); }

  std::mutex& file_lock() { return _file_mutex; }
};

template <typename Traits_ = _StoreTraits>
struct _FileStore : _CacheStore<Traits_, _FileOperations, _FileStore<Traits_>> {
  typedef _CacheStore<Traits_, _FileOperations, _FileStore<Traits_>> base_t;

  _FileStore(const char* path,
             size_t capacity = 500 * M, size_t pool_threads = 1)
      : base_t(capacity, pool_threads, Traits_::AREA_SIZE) {
    init_dbfile(path);
    // Thread pool already started by base constructor
  }

  ~_FileStore() {
    this->destroy();
    delete[] (char*)this->_header;
  }

  void init_dbfile(const char* path) {
    using FileHeader = typename _FileOperations::FileHeader;
    size_t header_size = 4 * K;
    std::unique_ptr<char[]> buffer(new char[header_size]);
    if (!std::filesystem::is_regular_file(path)) {
      _FileOperations::open(path);
      this->_header = new (buffer.get()) FileHeader();
      // Align initial file_size to AREA_SIZE so areas are AREA_SIZE-aligned
      this->_header->file_size = leaves::padding(header_size, Traits_::AREA_SIZE);
      this->resize(this->_header->file_size);
      // Write header
      this->write(0, buffer.get(), header_size);
    } else {
      _FileOperations::open(path);
      this->read(0, buffer.get(), header_size);
      this->_header = (FileHeader*)buffer.get();

      if (strcmp(this->_header->signature, FSTORE_SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }
    }

    assert(((uint64_t)this->_header & 7) == 0);
    buffer.release();  // ownership transferred to _header (freed in ~_FileStore)
    sanitize();
  }

  void sanitize() {
    // No locking needed since we're single-process
    recover_areas();
    ++this->_header->sanitize_generation;
    if (std::filesystem::file_size(this->filename()) != this->_header->file_size)
      std::filesystem::resize_file(this->filename(), this->_header->file_size);
  }

  // Rebuild the free area pool by scanning the file.
  void recover_areas() {
    auto* self = this;
    _recover_areas<_DBHeader<base_t>, _TxnContext<base_t>, Traits_::AREA_SIZE>(
        this->_header->area_pool,
        [self](auto fn) { self->_for_each_db_entry([&](auto& e) { if (e.offset) fn(e.offset); }); },
        this->_header->file_size,
        leaves::padding(this->calc_header_size(), Traits_::AREA_SIZE),
        [self](uint64_t pos, void* buf, size_t size) {
          self->read(pos, buf, size);
        },
        [self](uint64_t pos, const void* buf, size_t size) {
          self->write(pos, buf, size);
        });
  }

  // Compatibility method for tests
  AreaSlice get_area(size_t size) {
    auto area_ptr = this->alloc_multi_area(size);
    return *area_ptr;  // Convert Area* to AreaSlice
  }
};

}  // namespace leaves

#endif  // _LEAVES__FSTORE_HPP