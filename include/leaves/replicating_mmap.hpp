#ifndef _LEAVES_REPLICATING_MMAP_HPP
#define _LEAVES_REPLICATING_MMAP_HPP

#include <blake3.h>
#include <memory>

#include "db.hpp"
#include "intern/storage/_mmap.hpp"

namespace leaves {

// Replication-enabled traits mixin for memory-mapped storage
// Inherits from base traits and enables 32-byte hash storage
template <typename BaseTraits>
struct _ReplicationTraits : public BaseTraits {
  // Enable 32-byte hash storage in nodes
  typedef uint8_t hash_t[HASH_SIZE];
  
  // Use Blake3Hasher for replication
  typedef Blake3Hasher ReplicationHasher;
};

// Replication-enabled memory-mapped storage traits
typedef _ReplicationTraits<_MemoryMapTraits> _ReplicatingMemoryMapTraits;

class ReplicatingMapStorage
    : public std::enable_shared_from_this<ReplicatingMapStorage> {
 public:
  typedef _MemoryMapFile<_ReplicatingMemoryMapTraits> StorageImpl;
  typedef TDB<ReplicatingMapStorage> DB;
  typedef std::shared_ptr<ReplicatingMapStorage> storage_ptr;

  ReplicatingMapStorage(const char* path, size_t map_size = 4 * G,
                        uint16_t db_count = 48)
      : _storage(std::make_unique<StorageImpl>(path, map_size, db_count)) {}

  DB operator[](const char* name) { return DB(shared_from_this(), name); }

  void remove_db(const char* name) { _storage->remove_db(name); }

  void list_dbs(std::vector<std::string>& result) {
    return _storage->list_dbs(result);
  }

  Slice filename() const { return Slice(_storage->filename()); }

  size_t file_size() const { return _storage->file_size(); }

  static storage_ptr create(const char* path, size_t map_size = 4 * G,
                            uint16_t db_count = 48) {
    return std::make_shared<ReplicatingMapStorage>(path, map_size, db_count);
  }

 private:
  friend class TDB<ReplicatingMapStorage>;
  std::unique_ptr<StorageImpl> _storage;
};

}  // namespace leaves

#endif  // _LEAVES_REPLICATING_MMAP_HPP
