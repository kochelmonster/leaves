#ifndef _LEAVES_INTERN_DB_TMPDB_HPP
#define _LEAVES_INTERN_DB_TMPDB_HPP

#include <memory>
#include <vector>

#include "../core/_node.hpp"
#include "../core/_traits.hpp"
#include "../core/_util.hpp"
#include "../memory/_memory.hpp"
#include "_cursor.hpp"

namespace leaves {

// Minimal traits for TmpDB — same layout as _MemoryTraits but zero-cost
// PageHeader (needs_cow always false, no txn_id needed for semantics).
struct _TmpTraits {
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
        bool needs_cow(const DB*) const { return false; }
    };

    static constexpr size_t MAX_KEY_SIZE = 1 * M;
    static constexpr size_t AREA_SIZE = 512 * K;
    static constexpr size_t PAGE_CONTAINER_SIZE = 4 * K;
    static constexpr uint16_t PAGE_SIZES[] = {
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 2),
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 3),
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 4),
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 10),
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 16),
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 64),
        sizeof(PageHeader) + _TrieNode<_TmpTraits>::size(1, 256),
        4 * K};
    static constexpr uint16_t PAGE_SIZES_COUNT =
        sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);

    using ptr = SimplePointer<PageHeader, TRIE>;
    template <typename T, NodeTypes type = TRIE>
    using Pointer = SimplePointer<T, type>;
};

// Lightweight in-memory trie database. Single-writer, no transactions, no COW,
// no BigValue. Borrows areas from a Storage backend and returns them on
// destruction. On crash, storage-level recover_areas() reclaims orphans.
template <typename Storage>
struct _TmpDB {
    using Traits = _TmpTraits;
    using page_ptr = typename Traits::ptr;
    using offset_e = typename Traits::offset_e;
    using area_ptr = typename Traits::template Pointer<Area>;
    using storage_area_ptr = typename Storage::area_ptr;

    typedef _TmpDB DB;
    typedef DB db_type;

    struct CursorTraits : public Traits {
        typedef db_type DB;
    };

    static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
    static constexpr auto& PAGE_SIZES = Traits::PAGE_SIZES;
    static constexpr uint16_t PAGE_SIZES_COUNT = Traits::PAGE_SIZES_COUNT;
    static constexpr uint16_t MIN_PAGE_SIZE = PAGE_SIZES[0];
    static constexpr uint16_t MAX_PAGE_SIZE = PAGE_SIZES[PAGE_SIZES_COUNT - 1];

    static_assert(sizeof(typename Traits::PageHeader) % 8 == 0,
                  "PageHeader size must be a multiple of 8 for offset alignment");

    typedef _Cursor<CursorTraits> Cursor;
    typedef _MemManager<Traits> MemManager;

    struct _AreaRecord {
        storage_area_ptr ptr;    // keeps SmartPointer ref alive for CacheStore
        offset_t file_offset;   // original storage offset before init()
    };

    offset_e _root;
    MemManager _mem_manager;
    Storage& _storage;
    std::vector<_AreaRecord> _area_records;

    _TmpDB(Storage& storage) : _storage(storage) {
        _root = offset_e();
        auto area = alloc_single_area();
        _mem_manager.init(area->content_offset(), area->end());
    }

    ~_TmpDB() {
        if (_area_records.empty()) return;

        // Restore storage offsets and rebuild linked list
        for (size_t i = 0; i < _area_records.size(); i++) {
            Area* area = reinterpret_cast<Area*>(&*_area_records[i].ptr);
            area->_offset.store((uint64_t)_area_records[i].file_offset,
                                std::memory_order_relaxed);
            area->_size = AREA_SIZE;
            if (i + 1 < _area_records.size()) {
                area->next = _area_records[i + 1].file_offset;
            } else {
                area->next = 0;
            }
        }

        _storage.return_single_areas(
            _area_records.front().file_offset,
            _area_records.back().file_offset);
    }

    _TmpDB(const _TmpDB&) = delete;
    _TmpDB& operator=(const _TmpDB&) = delete;

    area_ptr alloc_single_area() {
        auto sptr = _storage.alloc_single_area();
        Area* area = &*sptr;
        offset_t file_offset = area->offset();
        _area_records.push_back({std::move(sptr), file_offset});

        // Repurpose the area for pointer-as-offset resolution
        offset_t area_offset(reinterpret_cast<uintptr_t>(area));
        area->init(area_offset, AREA_SIZE, 0);
        area->_ref.store(0);
        return area_ptr(area);
    }

    // --- Allocation ---

    page_ptr alloc_page(uint16_t space) {
        using PageHeader = typename Traits::PageHeader;
        uint8_t slot = _mem_manager.assign_slot(space + sizeof(PageHeader));
        page_ptr result = alloc_slot(slot);
        result->used = space;
        return result;
    }

    template <typename NodePtr>
    NodePtr alloc_node(uint16_t node_size) {
        using PageHeader = typename Traits::PageHeader;
        page_ptr page = alloc_page(node_size);
        return page + sizeof(PageHeader);
    }

    page_ptr alloc_slot(uint8_t slot_id) {
        return _mem_manager.alloc(slot_id, *this);
    }

    void free(page_ptr p) { _mem_manager.free(p, *this); }

    // --- Recycling (no-op, no transactions) ---

    template <typename BlockType>
    void mark_for_recycle(const BlockType&) {}

    template <typename BlockType>
    bool may_recycle(const BlockType&) const { return true; }

    // --- Resolution (raw pointer ↔ offset, like _MemoryStore) ---

    page_ptr resolve(const offset_t* offset_ptr, Access = READ) const {
        offset_t offset = *offset_ptr;
        return page_ptr(reinterpret_cast<void*>(offset.raw() & ~offset_t::TYPE_MASK));
    }

    template <typename T>
    typename Traits::template Pointer<T> resolve(const offset_t* offset_ptr,
                                                 Access = READ) const {
        offset_t offset = *offset_ptr;
        return typename Traits::template Pointer<T>(
            reinterpret_cast<void*>(offset.raw() & ~offset_t::TYPE_MASK));
    }

    offset_t resolve(const page_ptr& p) const {
        return offset_t((uint64_t)p).type(page_ptr::type);
    }

    template <typename Pointer, typename = typename std::enable_if<
                                    !std::is_integral<Pointer>::value &&
                                    !std::is_pointer<Pointer>::value>::type>
    offset_t resolve(const Pointer& p) const {
        return offset_t((uint64_t)p).type(p.type);
    }

    // --- No-ops ---

    template <typename PtrType>
    void make_dirty(PtrType) {}

    void prefetch(const offset_t&) const {}
    void prefetch(const offset_t*, Access = READ) const {}
    void prefetch(void*, Access = READ) const {}

    void flush(bool = false, bool = false) {}

    tid_t transaction_active() const { return tid_t(1); }

    // Clone - never called since needs_cow() returns false
    template <typename ptr>
    ptr clone(const ptr& src) { return src; }

    // --- Cursor for reading results ---

    Cursor cursor() { return Cursor(this, &_root); }
};

}  // namespace leaves

#endif  // _LEAVES_INTERN_DB_TMPDB_HPP
