#ifndef _LEAVES__MEMSTORE_HPP
#define _LEAVES__MEMSTORE_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "../db/_db.hpp"
#include "../core/_exception.hpp"
#include "../memory/_memory.hpp"
#include "../core/_node.hpp"
#include "../core/_port.hpp"
#include "../core/_traits.hpp"

namespace leaves {

// Memory storage traits - similar to _StoreTraits but simpler
struct _MemoryTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  struct PageHeader {
    typedef PageHeader Base;
    uint16_e used;  // used bytes in page
    uint8_t slot_id;
    template <typename DB>
    bool needs_cow(const DB* db) const {
      return false;
    }
  };

  static constexpr size_t MAX_KEY_SIZE = 1 * M;
  static constexpr size_t AREA_SIZE = 128 * K;  // Same as file store
  static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t PAGE_SIZES[] = {                      // Page sizes (header + node)
      sizeof(PageHeader) + _TrieNode<_MemoryTraits>::size(1, 10),   // digits 0-9
      sizeof(PageHeader) + _TrieNode<_MemoryTraits>::size(1, 16),   // hex 0-9A-F
      sizeof(PageHeader) + _TrieNode<_MemoryTraits>::size(1, 64),   // base64
      sizeof(PageHeader) + _TrieNode<_MemoryTraits>::size(1, 127),  // utf-8
      sizeof(PageHeader) + _TrieNode<_MemoryTraits>::size(1, 256),  // binary
      4 * K};
  static constexpr uint16_t PAGE_SIZES_COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);
  using ptr = SimplePointer<PageHeader, TRIE>;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = SimplePointer<T, type>;
};

// Non-transactional DB that derives from _DB but removes transaction handling
template <typename Storage_>
struct _MemoryDB {
  typedef Storage_ Storage;
  using Traits = typename Storage::Traits;
  using area_ptr = typename Storage::area_ptr;
  using page_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;

  typedef _MemoryDB<Storage> DB;
  typedef DB db_type;

  // Value traits for non-transactional operations
  struct CursorTraits : public Storage::Traits {
    typedef db_type DB;
  };

  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto& PAGE_SIZES = Traits::PAGE_SIZES;
  static constexpr uint16_t PAGE_SIZES_COUNT = Traits::PAGE_SIZES_COUNT;
  static constexpr uint16_t MIN_PAGE_SIZE = PAGE_SIZES[0];
  static constexpr uint16_t MAX_PAGE_SIZE = PAGE_SIZES[PAGE_SIZES_COUNT - 1];

  typedef _Cursor<CursorTraits> Cursor;
  typedef _MemManager<Traits> MemManager;
  typedef std::unique_ptr<Cursor> cursor_ptr;

  offset_e _root;
  Storage& _storage;
  MemManager _mem_manager;

  _MemoryDB(Storage& storage) : _storage(storage) { init(); }

  void init() {
    auto area_ptr = _storage.alloc_single_area();
    _mem_manager.init(area_ptr->content_offset(), area_ptr->end());
  }

  const db_type* _internal() const { return this; }  // for _Dumper

  // Area management
  area_ptr alloc_single_area() { return _storage.alloc_single_area(); }

  // Block allocation - using memory manager properly
  // Raw slot allocation (allocate slot for 'space' total bytes)
  page_ptr alloc(uint16_t space) {
    uint8_t slot = _mem_manager.assign_slot(space);
    return alloc_slot(slot);
  }

  // Allocate a page for content of 'space' bytes (PageHeader added internally).
  // Returns page_ptr pointing at the PageHeader.
  page_ptr alloc_page(uint16_t space) {
    using PageHeader = typename Traits::PageHeader;
    uint8_t slot = _mem_manager.assign_slot(space + sizeof(PageHeader));
    page_ptr result = alloc_slot(slot);
    result->used = space;
    return result;
  }

  // Allocate a node of 'node_size' bytes, returning a pointer past PageHeader.
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc_page(node_size);
    return page + sizeof(PageHeader);
  }

  void free(page_ptr p) { _mem_manager.free(p, *this); }

  // Memory manager interface
  page_ptr alloc_slot(uint8_t slot_id) {
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
  page_ptr resolve(const offset_t* offset_ptr, Access /*access*/ = READ) const {
    offset_t offset = *offset_ptr;
    // memstore doesn't support relative offsets - all offsets are absolute
    // pointers
    return page_ptr(reinterpret_cast<void*>((uint64_t)offset));
  }

  template <typename T>
  typename Traits::Pointer<T> resolve(const offset_t* offset_ptr,
                                      Access access = READ) const {
    offset_t offset = *offset_ptr;
    // memstore doesn't support relative offsets - all offsets are absolute
    // pointers
    return
        typename Traits::Pointer<T>(reinterpret_cast<void*>((uint64_t)offset));
  }

  // Non-template overload for page_ptr to avoid implicit conversion to
  // uint64_t
  offset_t resolve(const page_ptr& p) const {
    return offset_t((uint64_t)p).type(page_ptr::type);
  }

  // Template for typed pointers - disabled for integral types and raw pointers
  template <typename Pointer, typename = typename std::enable_if<
                                  !std::is_integral<Pointer>::value &&
                                  !std::is_pointer<Pointer>::value>::type>
  offset_t resolve(const Pointer& p) const {
    return offset_t((uint64_t)p).type(p.type);
  }

  template <typename PtrType>
  void make_dirty(PtrType /*block*/) {}
  void prefetch(const offset_t& offset) const { prefetch(&offset); }
  void prefetch(const offset_t* /*offset_ptr*/,
                Access /*access*/ = READ) const {}
  void prefetch(void* /*mem*/, Access /*access*/ = READ) const {}

  void flush(bool /*sync*/ = false, bool /*force*/ = false) {}

  // Clone a block (copy-on-write) - never called since needs_cow() returns
  // false
  template <typename ptr>
  ptr clone(const ptr& src) {
    return src;
  }

  // Big allocation methods - throw exceptions since memory storage doesn't
  // support them
  AreaSlice alloc_big(uint64_t /*size*/) {
    throw std::runtime_error("Big allocation not supported in memory storage");
  }

  void free_big(offset_e /*offset*/, size_t /*size*/) {
    throw std::runtime_error(
        "Big deallocation not supported in memory storage");
  }

  // Cursor factory methods
  cursor_ptr create_cursor() { return std::make_unique<Cursor>(this, &_root); }
};

// Memory storage implementation
struct _MemoryStorage {
  typedef _MemoryTraits Traits;
  using page_ptr = typename Traits::ptr;
  using area_ptr = typename Traits::template Pointer<Area>;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  typedef _MemoryDB<_MemoryStorage> DB;

  // Memory-based storage using vectors (single-threaded)
  std::vector<std::unique_ptr<char[]>> _areas;
  size_t _total_size = 0;
  DB _db;  // Single DB instance
  _MemoryStorage() : _total_size(0), _db(*this) {}

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
  DB& db() { return _db; }

  DB::cursor_ptr create_cursor() { return _db.create_cursor(); }

  // Compatibility methods
  void flush(bool /*sync*/ = false, bool /*force*/ = false) {}

  const char* filename() const { return "memory"; }
  size_t file_size() const { return _total_size; }
};

}  // namespace leaves

#endif  // _LEAVES__MEMSTORE_HPP