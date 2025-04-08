#ifndef _LEAVES__CHECK_HPP
#define _LEAVES__CHECK_HPP

#include "_node.hpp"

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

template <typename Storage>
struct _Dumper {
  using Traits = typename Storage::Traits;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using offset_e = typename Traits::offset_e;
  using uint16_e = typename Traits::uint16_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;

  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;

  Storage& _storage;
  int _id;
  bool _simple;

  _Dumper(Storage& storage, bool simple = false)
      : _storage(storage), _id(0), _simple(simple) {}

  void dump(std::ostream& out) {
    auto root = _storage.txn()->root;
    dump_link(out, root, _id++);
  }

  void dump_link(std::ostream& out, offset_t link, int id) {
    if (link.type() == TRIE)
      dump_trie(out, link, id);
    else
      dump_leaf(out, link, id);
  }

  void dump_leaf(std::ostream& out, offset_e offset, int id) {
    leaf_ptr leaf = _storage.resolve(offset);
    uint16_t size = leaf->size();
    out << "type: leaf" << std::endl;
    if (_simple) {
      out << "id: " << id << std::endl;
    } else {
      out << "id: " << (offset._offset) << std::endl;
      out << "page: " << offset.page() << std::endl;
      out << "freespace: " << BLOCK_SIZES[leaf->slot_id] - size << std::endl;
    }
    out << "size: " << size << std::endl;
    out << "txn: " << leaf->txn_id << std::endl;
    out << "keysize: " << (uint16_t)leaf->key_size << std::endl;
    out << "key: \"";
    for (int i = 0; i < leaf->key_size; i++) {
      out << "[" << bitstr(leaf->data[i]) << "]";
    }
    out << "\"" << std::endl;
    out << "valuesize: " << leaf->value_size << std::endl;
    out << "value: \"";
    int delta = leaf->key_size;
    for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)10);
         i < end; i++) {
      out << "[" << bitstr(leaf->data[i + delta]) << "]";
    }
    out << "\"" << std::endl;
    out << "---" << std::endl;
  }

  void dump_trie(std::ostream& out, offset_t offset, int id) {
    trie_ptr trie = _storage.resolve(offset);
    uint16_t size = trie->size();
    out << "type: trie" << std::endl;
    if (_simple) {
      out << "id: " << id << std::endl;
    } else {
      out << "id: " << (offset._offset) << std::endl;
      out << "page: " << offset.page() << std::endl;
      out << "freespace: " << BLOCK_SIZES[trie->slot_id] - size << std::endl;
    }
    out << "txn: " << trie->txn_id << std::endl;
    out << "size: " << size << std::endl;
    out << "compressed: " << std::endl;
    out << "  size: " << (int)trie->_compressed_len << std::endl;
    out << "  key: \"";
    for (int i = 0; i < trie->_compressed_len; i++) {
      out << "[" << bitstr(trie->compressed()[i]) << "]";
    }
    out << "\"" << std::endl;

    offset_e* start = trie->array();
    offset_e* end = start + trie->count();

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
    }
    else {
      for (offset_e* iter = start; iter < end; iter++) {
        out << "  - " << iter->_offset << std::endl;
        _id++;
      }
    }

    out << "---" << std::endl;
    offset_e last;
    int id_repeat = id_start;
    for (offset_e* iter = start; iter < end; iter++) {
      if (*iter != last) {
        last = *iter;
        dump_link(out, *iter, id_repeat++);
      }
    }
  }
};

template <typename Storage>
struct _MemoryChecker {
  using Traits = typename Storage::Traits;
  static constexpr auto& BLOCK_SIZES = Storage::BLOCK_SIZES;
  using offset_e = typename Storage::offset_e;
  using txn_ptr = typename Storage::txn_ptr;
  using block_ptr = typename Storage::block_ptr;
  static constexpr uint16_t COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);
  Storage& storage;
  std::vector<uint64_t> pages;

  static constexpr std::array<uint16_t, COUNT> generate_counts() {
    std::array<uint16_t, COUNT> result;
    for (int i = 0; i < COUNT; i++) {
      uint16_t bsize = BLOCK_SIZES[i];
      uint16_t count = PAGE_SIZE / bsize;
      uint16_t used = count * bsize;
      uint16_t collect = count;
      uint16_t rest = PAGE_SIZE - used;
      for (int id = i - 1; id >= 0 && rest > BLOCK_SIZES[0]; id--) {
        uint16_t bs = BLOCK_SIZES[id];
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
    pages.resize(txn_->file_size / PAGE_SIZE);
    pages[0] = ALL;

    for (uint64_t p = txn_->mem_manager.next_free;
         p < txn_->mem_manager.allocation_end; p += PAGE_SIZE) {
      pages[p / PAGE_SIZE] = ALL;
    }

    for (int i = 0; i < txn_->mem_manager.COUNT; i++) {
      auto& slot = txn_->mem_manager.slots[i];
      uint16_t size = BLOCK_SIZES[i];
      uint64_t b = slot.next_free;
      while (true) {
        uint64_t pb = padding(b, PAGE_SIZE);
        if (b + size > pb) b = pb;
        if (b == slot.end_free) break;
        storage.resolve(offset_t(b))->slot_id = i;
        mark_page(b);
        b += size;
      }
      // collect garbage container blocks
      offset_t o = slot.ostart;
      while (o) {
        typename Storage::MemManager::Slot::garb_ptr gc = storage.resolve(o);
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
        block_ptr ptr = storage.resolve(offset_t(i * PAGE_SIZE));
        uint16_t s0 = BLOCK_SIZES[0];
        uint16_t s1 = BLOCK_SIZES[1];
        uint16_t size = BLOCK_SIZES[ptr->slot_id];
        int collected = bits::count(p);
        int needed = PART_COUNT[ptr->slot_id];
        assert(collected == needed);
      }
    }
  }

  void mark_page(offset_t offset) {
    uint16_t slot_id = storage.resolve(offset)->slot_id;
    uint64_t addr = offset;
    uint16_t size = BLOCK_SIZES[slot_id];
    uint64_t page = addr / PAGE_SIZE;
    uint16_t poff = (addr % PAGE_SIZE);
    uint16_t part = poff / size;
    if (poff % size) part += PAGE_SIZE / size;

    assert(!(pages[page] & (1 << part)));
    pages[page] |= (1 << part);
  };

  void mark_trie_memory(offset_e offset) {
    typedef _TrieNode<Traits> TrieNode;
    using trie_ptr = typename Traits::Pointer<TrieNode>;
    mark_page(offset);

    if (offset.type() == TRIE) {
      trie_ptr branch = storage.resolve(offset);
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

}  // namespace leaves

#endif  // _LEAVES__CHECK_HPP