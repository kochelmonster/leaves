#ifndef _LEAVES__CHECK_HPP
#define _LEAVES__CHECK_HPP

#include <set>
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

  void dump(std::ostream& out) { dump_link(out, _root, _root, _id++); }

  void dump_link(std::ostream& out, offset_e* link, offset_e* parent, int id) {
    if (!link || !*link) return;
    if (link->type() == TRIE)
      dump_trie(out, link, parent, id);
    else
      dump_leaf(out, link, parent, id);
  }

  uint64_t to_id(offset_e* offset) {
    if (offset->is_relative())
      return *offset ? (uint64_t)offset->template resolve<char>() : 0;

    return offset->_offset;
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
    offset_e header_offset((uint64_t)bv->chunk_offset);
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
      out << "id: " << to_id(offset) << std::endl;
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
      out << "id: " << to_id(offset) << std::endl;
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
    assert(trie->count() <= TrieNode::MAX_BRANCH_COUNT);
    out << "branches: \"";
    trie->for_each_branch([&](int iter, auto*) {
      if (iter != TrieNode::NONE)
        out << "[" << bitstr(iter) << "]";
      else
        out << "[]";
    });
    out << "\"" << std::endl;

    out << "children: " << std::endl;
    int id_start = _id;
    if (_simple) {
      for (offset_e* iter = start; iter < end; iter++) {
        out << "  - " << _id++ << std::endl;
      }
    } else {
      for (offset_e* iter = start; iter < end; iter++) {
        out << "  - " << to_id(iter) << std::endl;
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

/**
 * Memory integrity checker — verifies that every allocated page is
 * accounted for exactly once across: free space, garbage slots,
 * trie nodes (branch + leaf), and transaction headers.
 *
 * Usage:
 *   _MemoryChecker<db_type>(*db._internal()).check();
 */
template <typename DB>
struct _MemoryChecker {
  using Traits = typename DB::Traits;
  using MemManager = typename DB::MemManager;
  using PageHeader = typename Traits::PageHeader;
  using PageContainer = typename MemManager::PageContainer;
  using offset_e = typename Traits::offset_e;
  using txn_ptr = typename DB::txn_ptr;
  using page_ptr = typename Traits::ptr;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using trie_ptr = typename Traits::template Pointer<TrieNode>;
  using leaf_ptr = typename Traits::template Pointer<LeafNode, LEAF>;

  static constexpr auto& PAGE_SIZES = Traits::PAGE_SIZES;
  static constexpr uint16_t COUNT = Traits::PAGE_SIZES_COUNT;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;

  DB& db;
  std::set<uint64_t> marked_pages;
  size_t total_pages = 0;

  _MemoryChecker(DB& db_) : db(db_) {}

  struct CheckError : std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  void mark_page(uint64_t addr) {
    if (!marked_pages.insert(addr).second) {
      throw CheckError("double allocation at offset " + std::to_string(addr));
    }
    total_pages++;
  }

  void mark_page(offset_t offset) { mark_page(uint64_t(offset)); }

  void check() {
    txn_ptr txn_ = db.txn();
    auto& mm = txn_->mem_manager;

    // 1. Mark free allocation space (not yet allocated pages)
    //    These are tracked as ranges, not individual pages, so we
    //    don't mark them — they're simply unallocated.

    // 2. Mark garbage container blocks and freed pages
    for (int i = 0; i < MemManager::COUNT; i++) {
      auto& slot = mm.slots_at(i);

      // Mark garbage container blocks (PageContainer linked list)
      if (slot.ostart) {
        offset_t o = slot.ostart;
        while (true) {
          mark_page(o);
          typename MemManager::Slot::cont_ptr gc =
              db.template resolve<PageContainer>(&o);
          if (o == slot.oend) break;
          o = gc->next;
        }
      }

      // Mark freed pages tracked inside the garbage containers
      if (slot.count) {
        slot.iter(db, [this](auto& block) { mark_page(block.link); });
      }
    }

    // 3. Mark all trie nodes (branches and leaves)
    if (txn_->root) {
      mark_trie_memory(txn_->root);
    }

    // 4. Mark transaction pages
    db.iter_transactions([this](txn_ptr txn) -> bool {
      mark_page(db.resolve(txn));
      return false;
    });
  }

  void mark_trie_memory(offset_e offset) {
    mark_page(offset);

    if (offset.type() == TRIE) {
      trie_ptr branch = db.template resolve<TrieNode>(&offset);
      auto count = branch->count();
      offset_e* array = branch->array();
      for (int i = 0; i < count; i++) {
        mark_trie_memory(array[i]);
      }
    }
    // LEAF nodes are already marked above, no children to recurse into
  }
};
}  // namespace leaves

#endif  // _LEAVES__CHECK_HPP