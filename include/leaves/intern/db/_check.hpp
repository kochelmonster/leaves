#ifndef _LEAVES__CHECK_HPP
#define _LEAVES__CHECK_HPP

#include <sstream>
#include <string>

#include "../core/_node.hpp"

namespace leaves {

inline std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit) && bit != '"' && bit != '<' && bit != '>' && bit != ']' &&
      bit != '\\' && bit != '}' && bit != '{' && bit != '|') {
    cstr << bit;
  } else {
    cstr << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
  return cstr.str();
}

// Helper to detect if PageHeader has txn_id member
template <typename T, typename = void>
struct has_txn_id : std::false_type {};

template <typename T>
struct has_txn_id<T, std::void_t<decltype(std::declval<T>().txn_id)>>
    : std::true_type {};

// Helper to conditionally output txn_id if PageHeader has it
template <typename PageHeader>
void dump_txn_id_impl(std::ostream& out, PageHeader* header, std::true_type) {
  out << "txn: " << header->txn_id << std::endl;
}

// Fallback when PageHeader doesn't have txn_id
template <typename PageHeader>
void dump_txn_id_impl(std::ostream&, PageHeader*, std::false_type) {}

template <typename PageHeader>
void dump_txn_id(std::ostream& out, PageHeader* header) {
  dump_txn_id_impl(out, header, has_txn_id<PageHeader>{});
}

// Helper to extract the actual cursor type:
// - If Container::Cursor has cursor_ptr, use that->element_type
// - Otherwise, use Container::Cursor directly
template <typename T, typename = void>
struct ExtractInternalCursor {
  using type = T;  // No cursor_ptr, so T is the cursor itself
};

template <typename T>
struct ExtractInternalCursor<T, std::void_t<typename T::cursor_ptr>> {
  using type = typename T::cursor_ptr::element_type;
};

// Helper to detect if Cursor has BigMemory member
template <typename T, typename = void>
struct has_big_memory : std::false_type {};

template <typename T>
struct has_big_memory<T, std::void_t<typename T::BigMemory>> : std::true_type {
};

template <typename Container, bool with_headers = true>
struct _Dumper {
  using DB = typename Container::db_type;
  using Traits = typename DB::Traits;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using offset_e = typename Traits::offset_e;
  using uint16_e = typename Traits::uint16_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;
  using page_ptr = typename Traits::ptr;

  const DB& _db;
  int _id;
  offset_e* _root;
  bool _simple;

   _Dumper(const Container& container, offset_e* root, bool simple = false)
      : _db(*container._internal()), _id(0), _root(root), _simple(simple) {}

  void dump(std::ostream& out) {
    dump_link(out, _root, _root, _id++);
  }

  void dump_link(std::ostream& out, offset_e* link, offset_e* parent, int id) {
    if(!link || !*link) return;
    if (link->type() == TRIE)
      dump_trie(out, link, parent, id);
    else
      dump_leaf(out, link, parent, id);
  }

  uint64_t to_absolute(offset_e* offset) {
    if (!offset->is_relative()) return offset->_offset;
    if (offset->type() == TRIE) {
      trie_ptr node = _db.template resolve<TrieNode>(offset);
      return _db.resolve(node)._offset;
    } else {
      leaf_ptr node = _db.template resolve<LeafNode>(offset);
      return _db.resolve(node)._offset;
    }
  }

  template <typename InternalCursor>
  void dump_big_value_impl(std::ostream& out, leaf_ptr leaf, std::true_type) {
    using BigMemory = typename InternalCursor::BigMemory;
    using BigValue = typename BigMemory::BigValue;
    using FreeKey = typename BigMemory::FreeKey;
    using PageHeader = typename Traits::PageHeader;
    auto bv = (BigValue*)leaf->vdata();
    out << "valuesize: " << bv->value_size << std::endl;

    // Read chunk header to get size and has_successor flag
    offset_e header_offset = bv->chunk_offset;
    header_offset._offset -= sizeof(FreeKey);
    auto header_ptr =
        (FreeKey*)(char*)_db.template resolve<PageHeader>(&header_offset, READ);
    uint64_t chunk_size = header_ptr->size;
    bool has_successor = (header_ptr->offset & 1) != 0;

    out << "value: \"chunk_offset=" << bv->chunk_offset
        << " chunk_size=" << chunk_size
        << " has_successor=" << (has_successor ? "true" : "false") << "\""
        << std::endl;
  }

  template <typename InternalCursor>
  void dump_big_value_impl(std::ostream& out, leaf_ptr leaf, std::false_type) {
    out << "valuesize: (big value - type not supported)" << std::endl;
    out << "value: \"...\"" << std::endl;
  }

  void dump_leaf(std::ostream& out, offset_e* offset, offset_e* parent,
                 int id) {
    using PageHeader = typename Traits::PageHeader;
    leaf_ptr leaf = _db.template resolve<LeafNode>(offset);
    uint16_t size = leaf->size();
    out << "type: leaf" << std::endl;
    if (_simple) {
      out << "id: " << id << std::endl;
    } else {
      out << "id: " << to_absolute(offset) << std::endl;
      if constexpr (with_headers) {
        // If relative, use parent to find page; otherwise use offset itself
        offset_e* page_link = offset->is_relative() ? parent : offset;
        offset_e header_offset;
        header_offset._offset = page_link->_offset - sizeof(PageHeader);
        page_ptr header = _db.template resolve<page_ptr>(&header_offset, READ);
        out << "page: " << header_offset._offset << std::endl;
        out << "freespace: "
            << Traits::PAGE_SIZES[header->slot_id] - header->used << std::endl;
      }
    }
    out << "size: " << size << std::endl;
    if constexpr (with_headers) {
      // If relative, use parent to find page; otherwise use offset itself
      offset_e* page_link = offset->is_relative() ? parent : offset;
      offset_e header_offset;
      header_offset._offset = page_link->_offset - sizeof(PageHeader);
      page_ptr header = _db.template resolve<page_ptr>(&header_offset, READ);
      dump_txn_id(out, reinterpret_cast<PageHeader*>(&(*header)));
    }
    out << "keysize: " << (uint16_t)leaf->key_size << std::endl;
    out << "key: \"";
    for (int i = 0; i < leaf->key_size; i++) {
      out << "[" << bitstr(leaf->data[i]) << "]";
    }
    out << "\"" << std::endl;
    if (!leaf->is_big()) {
      out << "valuesize: " << leaf->value_size << std::endl;
      out << "value: \"";
      int delta = leaf->key_size;
      for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)30);
           i < end; i++) {
        // out << "[" << bitstr(leaf->data[i + delta]) << "]";
        out << bitstr(leaf->data[i + delta]);
      }
      out << "\"" << std::endl;
    } else {
      using InternalCursor =
          typename ExtractInternalCursor<typename Container::Cursor>::type;
      dump_big_value_impl<InternalCursor>(out, leaf,
                                          has_big_memory<InternalCursor>{});
    }

    out << "---" << std::endl;
  }

  void dump_trie(std::ostream& out, offset_e* offset, offset_e* parent,
                 int id) {
    using PageHeader = typename Traits::PageHeader;
    trie_ptr trie = _db.template resolve<TrieNode>(offset);
    uint16_t size = trie->size();
    out << "type: trie" << std::endl;
    if (_simple) {
      out << "id: " << id << std::endl;
    } else {
      out << "id: " << to_absolute(offset) << std::endl;
      if constexpr (with_headers) {
        // If relative, use parent to find page; otherwise use offset itself
        offset_e* page_link = offset->is_relative() ? parent : offset;
        offset_e header_offset;
        header_offset._offset = page_link->_offset - sizeof(PageHeader);
        page_ptr header = _db.template resolve<page_ptr>(&header_offset, READ);
        out << "page: " << header_offset._offset << std::endl;
        out << "freespace: "
            << Traits::PAGE_SIZES[header->slot_id] - header->used << std::endl;
      }
    }
    if constexpr (with_headers) {
      // If relative, use parent to find page; otherwise use offset itself
      offset_e* page_link = offset->is_relative() ? parent : offset;
      offset_e header_offset;
      header_offset._offset = page_link->_offset - sizeof(PageHeader);
      page_ptr header = _db.template resolve<page_ptr>(&header_offset, READ);
      dump_txn_id(out, reinterpret_cast<PageHeader*>(&(*header)));
    }
    out << "size: " << size << std::endl;
    out << "compressed: " << std::endl;
    out << "  size: " << (int)trie->len() << std::endl;
    out << "  key: \"";
    for (int i = 0; i < trie->len(); i++) {
      out << "[" << bitstr(trie->compressed()[i]) << "]";
    }
    out << "\"" << std::endl;

    offset_e* start = trie->array();
    offset_e* end = start + trie->count();

    assert(trie->count() > 0);
    assert(trie->count() <= 256);
    out << "branches: \"";
    for (int iter = trie->first(); iter != TrieNode::OUT_OF_RANGE;
         iter = trie->next(iter)) {
      if (iter != TrieNode::NONE)
        out << "[" << bitstr(iter) << "]";
      else
        out << "[]";
    }
    out << "\"" << std::endl;

    out << "children: " << std::endl;
    int id_start = _id;
    if (_simple) {
      for (offset_e* iter = start; iter < end; iter++) {
        out << "  - " << _id++ << std::endl;
      }
    } else {
      for (offset_e* iter = start; iter < end; iter++) {
        out << "  - " << to_absolute(iter) << std::endl;
        _id++;
      }
    }

    out << "---" << std::endl;
    int id_repeat = id_start;
    for (offset_e* iter = start; iter < end; iter++) {
      // Pass current offset as parent for children
      if (offset) dump_link(out, iter, offset, id_repeat++);
    }
  }
};
#if 0
template <typename Storage>
struct _MemoryChecker {
  using Traits = typename Storage::Traits;
  static constexpr auto& PAGE_SIZES = Storage::PAGE_SIZES;
  using offset_e = typename Storage::offset_e;
  using txn_ptr = typename Storage::txn_ptr;
  using page_ptr = typename Storage::page_ptr;
  static constexpr uint16_t COUNT =
      sizeof(PAGE_SIZES) / sizeof(PAGE_SIZES[0]);
  Storage& storage;
  std::vector<uint64_t> pages;

  static constexpr std::array<uint16_t, COUNT> generate_counts() {
    std::array<uint16_t, COUNT> result;
    for (int i = 0; i < COUNT; i++) {
      uint16_t bsize = PAGE_SIZES[i];
      uint16_t count = AREA_SIZE / bsize;
      uint16_t used = count * bsize;
      uint16_t collect = count;
      uint16_t rest = AREA_SIZE - used;
      for (int id = i - 1; id >= 0 && rest > PAGE_SIZES[0]; id--) {
        uint16_t bs = PAGE_SIZES[id];
        while (bs < rest) {
          collect++;
          rest -= bs;
        }
      }
      result[i] = collect;
    }
    return result;
  }

  static std::array<uint16_t, COUNT> PART_COUNT;

  _MemoryChecker(Storage& storage_) : storage(storage_) {}

  void check() {
    const uint64_t ALL = ~(uint64_t)0;

    txn_ptr txn_ = storage.txn();
    pages.resize(txn_->file_size / AREA_SIZE);
    pages[0] = ALL;

    for (uint64_t p = txn_->mem_manager.next_free;
         p < txn_->mem_manager.allocation_end; p += AREA_SIZE) {
      pages[p / AREA_SIZE] = ALL;
    }

    for (int i = 0; i < txn_->mem_manager.COUNT; i++) {
      auto& slot = txn_->mem_manager.slots[i];
      uint16_t size = PAGE_SIZES[i];
      uint64_t b = slot.next_free;
      while (true) {
        uint64_t pb = padding(b, AREA_SIZE);  // this is wrong
        if (b + size > pb) b = pb;
        if (b == slot.end_free) break;
        offset_t b_offset(b);
        storage.resolve(&b_offset, READ)->slot_id = i;
        mark_page(b);
        b += size;
      }
      // collect garbage container blocks
      offset_t o = slot.ostart;
      while (o) {
        typename Storage::MemManager::Slot::cont_ptr gc = storage.template resolve<PageContainer>(&o);
        mark_page(o);
        if (o == slot.oend) break;
        o = gc->next;
      }

      // collect garbage blocks
      slot.iter(storage, [this]<typename Block>(Block& block) {
        mark_page(block.link);
      });
    }

    mark_trie_memory(txn_->root);

    storage.iter_transactions([this](txn_ptr txn) -> bool {
      mark_page(storage.resolve(txn));
      return false;
    });

    for (int i = 0; i < pages.size(); i++) {
      uint64_t p = pages[i];
      if (p != ALL) {
        offset_t i_offset(i * AREA_SIZE);
        page_ptr ptr = storage.template resolve<PageHeader>(&i_offset);
        uint16_t s0 = PAGE_SIZES[0];
        uint16_t s1 = PAGE_SIZES[1];
        uint16_t size = PAGE_SIZES[ptr->slot_id];
        int collected = bits::count(p);
        int needed = PART_COUNT[ptr->slot_id];
        assert(collected == needed);
      }
    }
  }

  void mark_page(offset_t offset) {
    uint16_t slot_id = storage.resolve(&offset, READ)->slot_id;
    uint64_t addr = offset;
    uint16_t size = PAGE_SIZES[slot_id];
    uint64_t page = addr / AREA_SIZE;
    uint16_t poff = (addr % AREA_SIZE);
    uint16_t part = poff / size;
    if (poff % size) part += AREA_SIZE / size;

    assert(!(pages[page] & (1 << part)));
    pages[page] |= (1 << part);
  };

  void mark_trie_memory(offset_e offset) {
    typedef _TrieNode<Traits> TrieNode;
    using trie_ptr = typename Traits::Pointer<TrieNode>;
    mark_page(offset);

    if (offset.type() == TRIE) {
      trie_ptr branch = storage.template resolve<TrieNode>(&offset);
      auto count = branch->count();
      offset_e* array = branch->array();
      for (int i = 0; i < count; i++) {
        mark_trie_memory(array[i]);
      }
    }
  }
};

template <typename Storage>
std::array<uint16_t, _MemoryChecker<Storage>::COUNT>
    _MemoryChecker<Storage>::PART_COUNT =
        _MemoryChecker<Storage>::generate_counts();
#endif
}  // namespace leaves

#endif  // _LEAVES__CHECK_HPP