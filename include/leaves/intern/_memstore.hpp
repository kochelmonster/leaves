#ifndef _LEAVES__MEMSTORE_HPP
#define _LEAVES__MEMSTORE_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "_db.hpp"
#include "_exception.hpp"
#include "_memory.hpp"
#include "_node.hpp"
#include "_port.hpp"
#include "_traits.hpp"

namespace leaves {

// Memory storage traits - similar to _StoreTraits but simpler
struct _MemoryTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  struct BlockHeader {
    typedef BlockHeader Base;
    tid_t txn_id;
    uint8_t slot_id;
  };

  static constexpr bool TRANSACTIONAL = false;
  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 128 * K;  // Same as file store
  static constexpr uint16_t BLOCK_SIZES[] = {   // Typical node sizes
      _TrieNode<_MemoryTraits>::size(1, 10),    // digits 0-9
      _TrieNode<_MemoryTraits>::size(1, 16),    // hex 0-9A-F
      _TrieNode<_MemoryTraits>::size(1, 64),    // base64
      _TrieNode<_MemoryTraits>::size(1, 127),   // utf-8
      _TrieNode<_MemoryTraits>::size(1, 256),   // binary
      4 * K};
  static constexpr uint16_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);
  typedef SimplePointer<BlockHeader> Pointers;
  using ptr = typename Pointers::ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename Pointers::template Pointer<T, type>;
};

// Non-transactional DB that derives from _DB but removes transaction handling
template <typename Storage_>
struct _MemoryDB {
  typedef Storage_ Storage;
  using Traits = typename Storage::Traits;
  using area_ptr = typename Storage::area_ptr;
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;

  typedef _MemoryDB<Storage> DB;

  // Transaction methods become no-ops
  struct NullTransaction {
    tid_t txn_id = tid_t(1);
    offset_t root{0};
    std::atomic<uint32_t> refs{0};
    uint64_t branch_count[32] = {0};
  };

  typedef NullTransaction* txn_ptr;

  // Value traits for non-transactional operations
  struct ValueTraits : public Storage::Traits {
    typedef std::shared_ptr<DB> db_ptr;
    typedef ::NullHasher Hasher;  // No hashing needed for memory-only
    typedef uint8_t hash_t[0];
    constexpr static bool TRANSACTIONAL = false;
    constexpr static bool TRACKED = false;
    static void set_root(txn_ptr txn, offset_t offset) { txn->root = offset; }
    static offset_t get_root(txn_ptr txn) { return txn->root; }
  };

  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static constexpr uint16_t BLOCK_SIZES_COUNT = Traits::BLOCK_SIZES_COUNT;
  static constexpr uint16_t MIN_BLOCK_SIZE = BLOCK_SIZES[0];
  static constexpr uint16_t MAX_BLOCK_SIZE = BLOCK_SIZES[BLOCK_SIZES_COUNT - 1];

  typedef _Cursor<DB, ValueTraits> Cursor;
  typedef _MemManager<Traits> MemManager;
  typedef DB db_type;

  Storage& _storage;
  MemManager _mem_manager;
  NullTransaction _null_txn;

  _MemoryDB(Storage& storage) : _storage(storage) { init(); }

  void init() {
    auto area_ptr = _storage.alloc_single_area();
    _mem_manager.init(area_ptr->content_offset(), area_ptr->end());
  }

  txn_ptr txn() { return &_null_txn; }

  // Area management
  area_ptr alloc_single_area() { return _storage.alloc_single_area(); }

  // Block allocation - using memory manager properly
  block_ptr alloc(uint16_t space) {
    uint8_t slot = _mem_manager.assign_slot(space);
    return alloc_slot(slot);
  }

  void free(block_ptr p) { _mem_manager.free(p, *this); }

  // Memory manager interface
  block_ptr alloc_slot(uint8_t slot_id) {
    return _mem_manager.alloc(slot_id, *this);
  }

  // Methods required by _MemManager
  template <typename BlockType>
  void mark_for_recycle(const BlockType& /*block*/) {}

  template <typename BlockType>
  bool may_recycle(const BlockType& /*block*/) const {
    return true;
  }

  // Direct pointer/offset resolution - no storage delegation needed
  block_ptr resolve(offset_t offset, Access /*access*/ = READ) const {
    return block_ptr(reinterpret_cast<void*>((uint64_t)offset));
  }
  
  // Non-template overload for block_ptr to avoid implicit conversion to uint64_t
  offset_t resolve(const block_ptr& p) const {
    return offset_t((uint64_t)p).type(block_ptr::type);
  }
  
  // Template for typed pointers - disabled for integral types
  template <typename Pointer, 
            typename = typename std::enable_if<!std::is_integral<Pointer>::value>::type>
  offset_t resolve(const Pointer& p) const {
    return offset_t((uint64_t)p).type(Pointer::type);
  }

  void make_dirty(block_ptr& /*block*/) {}
  void prefetch(offset_t /*offset*/, Access /*access*/ = READ) const {}
  void prefetch(void* /*mem*/, Access /*access*/ = READ) const {}
  
  void flush(bool /*sync*/ = false, bool /*force*/ = false) {}

  // Big allocation methods - throw exceptions since memory storage doesn't
  // support them
  AreaSlice alloc_big(uint64_t /*size*/) {
    throw std::runtime_error("Big allocation not supported in memory storage");
  }

  void free_big(offset_e /*offset*/, size_t /*size*/) {
    throw std::runtime_error(
        "Big deallocation not supported in memory storage");
  }

  // Copy-on-write - just return the same pointer for memory storage
  template <typename ptr>
  ptr cow(ptr& src) {
    return src;
  }

  // Transaction ID access
  tid_t txn_id() const { return _null_txn.txn_id; }

  // Transaction active check
  tid_t transaction_active() const {
    return tid_t(0);  // Memory storage doesn't use transactions
  }

  // Write transaction access for compatibility
  NullTransaction& _wtxn = _null_txn;

  // Cursor factory methods
  Cursor cursor() {
    // Get the proper shared_ptr from storage to ensure correct lifetime
    // management
    auto self = _storage.db();
    return Cursor(self);
  }
};

// Memory storage implementation
struct _MemoryStorage {
  typedef _MemoryTraits Traits;
  using block_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  typedef _MemoryDB<_MemoryStorage> DB;
  typedef std::shared_ptr<DB> db_ptr;

  // Simple mutex for memory storage (no-op for single-threaded)
  struct Mutex {
    template <typename Time = std::chrono::seconds>
    void lock(Time t = Time(10)) {}
    bool try_lock() { return true; }
    void unlock() {}
  };

  // Memory-based storage using vectors (single-threaded)
  std::vector<std::unique_ptr<char[]>> _areas;
  size_t _total_size = 0;
  db_ptr _db;  // Single DB instance
  _MemoryStorage() : _total_size(0) { _db = std::make_shared<DB>(*this); }

  // Area allocation - creates new memory areas
  area_ptr alloc_single_area() {
    // Allocate memory for a single area
    auto memory = std::make_unique<char[]>(AREA_SIZE);
    Area* area = reinterpret_cast<Area*>(memory.get());

    // Use pointer value as offset - no mapping needed
    offset_t area_offset(reinterpret_cast<uintptr_t>(area));
    area->init(area_offset, AREA_SIZE, 0);
    area->_ref.store(0);

    _total_size += AREA_SIZE;
    _areas.push_back(std::move(memory));

    return area_ptr(area);
  }

  area_ptr alloc_multi_area(uint64_t /*size*/) {
    throw std::runtime_error(
        "Multi-area allocation not supported in memory storage - use single "
        "areas only");
  }

  // Single DB access
  db_ptr db() { return _db; }

  // Compatibility methods
  void flush(bool /*sync*/ = false, bool /*force*/ = false) {}
  Mutex& file_lock() {
    static Mutex m;
    return m;
  }
  const char* filename() const { return "memory"; }
  size_t file_size() const { return _total_size; }
};

}  // namespace leaves

#endif  // _LEAVES__MEMSTORE_HPP