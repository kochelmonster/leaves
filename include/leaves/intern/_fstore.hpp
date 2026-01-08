#ifndef _LEAVES__FSTORE_HPP
#define _LEAVES__FSTORE_HPP

#include <fcntl.h>

#include <algorithm>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "_cachestore.hpp"
#include "_db.hpp"
#include "_exception.hpp"
#include "_memory.hpp"  // for AreaSlice, SmartPointer
#include "_node.hpp"    // for _TrieNode
#include "_port.hpp"
#include "_traits.hpp"  // for NodeTypes, offset_t, tid_t, K, M, padding, Access

// Removed interprocess mutex and process ID references since they're no longer
// needed

namespace leaves {

static const char FSTORE_SIGNATURE[] = "larch-leaves-fstore";
static const size_t FSTORE_SIGNATURE_SIZE =
    padding(sizeof(FSTORE_SIGNATURE), 8);

// definition of all headers and data types
struct _StoreTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  struct BlockHeader {
    typedef BlockHeader Base;
    tid_t txn_id;
    uint8_t slot_id;
    bool needs_cow(const BlockHeader& other) const {
      return txn_id != other.txn_id;
    }
  };

  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 128 * K;  // not OS AREA_SIZE
  static constexpr size_t BLOCK_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t BLOCK_SIZES[] = {   // Typical node sizes
      _TrieNode<_StoreTraits>::size(1, 10),     // digits 0-9
      _TrieNode<_StoreTraits>::size(1, 16),     // hex 0-9A-F
      _TrieNode<_StoreTraits>::size(1, 64),     // base64
      _TrieNode<_StoreTraits>::size(1, 127),    // utf-8
      _TrieNode<_StoreTraits>::size(1, 256),    // binary
      4 * K};
  static constexpr uint16_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);
  using ptr = SmartPointer<BlockHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SmartPointer<T, type>;
};

struct _FileOperations : _CacheBase {
  // Simple placeholder for compatibility, no actual locking
  struct Mutex {
    // No mutex needed for single-process use
    template <typename Time = std::chrono::seconds>
    void lock(Time /*t*/ = Time(10)) {}
    bool try_lock() { return true; }
    void unlock() {}
  };

  struct FileHeader {
    char signature[FSTORE_SIGNATURE_SIZE];
    uint16_t db_version;
    size_t file_size;
    Mutex file_lock;     // Kept for compatibility but no longer needed
    AreaPool area_pool;  // pool for both single and multi areas
    uint16_t db_count;
    DBEntry dbs[0];

    FileHeader(uint16_t db_count_)
        : signature{},
          db_version(0),
          file_size(0),
          file_lock{},
          area_pool{},
          db_count(db_count_) {
      std::memset(signature, 0, sizeof(signature));
      std::strcpy(signature, FSTORE_SIGNATURE);
      area_pool.init();
      std::memset((void*)dbs, 0, sizeof(DBEntry) * db_count);
    }
  };

  std::string _filepath;
  mutable std::fstream _file;
  FileHeader* _header;
  // Protect concurrent read/write/resize operations on the same fstream
  mutable std::mutex _io_mutex;

  size_t file_size() const { return _header->file_size; }

  size_t calc_header_size() const {
    return leaves::padding(
        sizeof(FileHeader) + sizeof(DBEntry) * _header->db_count, 4 * K);
  }

  void open(const char* path) {
    std::lock_guard<std::mutex> lock(_io_mutex);
    _filepath = path;
    _file.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!_file.is_open()) {
      // Try to create the file if it doesn't exist
      _file.open(path, std::ios::in | std::ios::out | std::ios::binary |
                           std::ios::trunc);
      if (!_file.is_open()) {
        throw std::runtime_error("Failed to open file");
      }
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (_file.is_open()) {
      _file.close();
    }
  }

  void write(offset_t offset, const void* ptr, size_t size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
    _file.clear();
    _file.seekp(static_cast<std::streampos>(offset));
    if (_file.fail()) {
      throw std::runtime_error("Failed to seek to offset");
    }
    _file.write(static_cast<const char*>(ptr), size);
    if (_file.fail()) {
      throw std::runtime_error("Failed to write data");
    }
    _file.flush();
  }

  void read(offset_t offset, void* ptr, size_t size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
    _file.clear();
    _file.seekg(static_cast<std::streampos>(offset));
    if (_file.fail()) {
      throw std::runtime_error("Failed to seek to offset");
    }
    _file.read(static_cast<char*>(ptr), size);
    if (_file.fail() || _file.gcount() != static_cast<std::streamsize>(size)) {
      throw std::runtime_error("Failed to read data");
    }
  }

  void resize(size_t new_size) const {
    std::lock_guard<std::mutex> lock(_io_mutex);
    if (!_file.is_open()) {
      throw std::runtime_error("File not open");
    }
    _file.close();
    std::filesystem::resize_file(_filepath, new_size);
    _file.open(_filepath, std::ios::in | std::ios::out | std::ios::binary);
    if (!_file.is_open()) {
      throw std::runtime_error("Failed to reopen file after resize");
    }
  }

  template <typename BlockVector>
  void write_batch(BlockVector& blocks_to_write, size_t header_size) {
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
        write(header_size + start_offset, start_area, start_area->size());
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
        write(header_size + start_offset, buffer.data(), current_size);

        // Move to the next non-contiguous block
        i = batch_end + 1;
      }
    }
  }

  const char* filename() const { return _filepath.c_str(); }

  Mutex& file_lock() { return _header->file_lock; }
};

struct _FileStore : _CacheStore<_StoreTraits, _FileOperations> {
  typedef _CacheStore<_StoreTraits, _FileOperations> base_t;
  using DB = base_t::DB;

  _FileStore(const char* path, uint16_t db_count = 48,
             size_t capacity = 500 * M)
      : base_t(db_count, capacity) {
    init_dbfile(path, db_count);
    start_write_back_thread();
  }

  ~_FileStore() {
    destroy();
    delete[] (char*)_header;
  }

  void init_dbfile(const char* path, uint16_t db_count) {
    // Compute header size based on requested db_count first
    size_t header_size =
        leaves::padding(sizeof(FileHeader) + sizeof(DBEntry) * db_count, 4 * K);
    char* buffer = new char[header_size];
    if (!std::filesystem::is_regular_file(path)) {
      open(path);
      _header = new (buffer) FileHeader(db_count);
      // Ensure enough space on disk for first area aligned to AREA_SIZE
      // boundary
      _header->file_size = header_size;
      resize(_header->file_size);
      // Write header
      write(0, buffer, header_size);
    } else {
      open(path);
      read(0, buffer, header_size);
      _header = (FileHeader*)buffer;

      if (strcmp(_header->signature, FSTORE_SIGNATURE)) {
        throw std::runtime_error("wrong filetype");
      }
      if (_header->db_count != db_count)
        throw WrongValue("db_count may not be changed.");
    }

    assert(((uint64_t)_header & 7) == 0);
    sanitize();
  }

  void sanitize() {
    // No locking needed since we're single-process
    sanitize_dbs();
    if (std::filesystem::file_size(filename()) != _header->file_size)
      std::filesystem::resize_file(filename(), _header->file_size);
  }

  void sanitize_dbs() {
    for (uint16_t i = 0; i < _header->db_count; i++) {
      if (_header->dbs[i].offset) {
        assert(!_dbs[i]);
        _DB(*this, _header->dbs[i].offset, i).sanitize();
      }
    }
  }

  // Compatibility method for tests
  AreaSlice get_area(size_t size) {
    auto area_ptr = alloc_multi_area(size);
    return *area_ptr;  // Convert Area* to AreaSlice
  }
};

}  // namespace leaves

#endif  // _LEAVES__FSTORE_HPP