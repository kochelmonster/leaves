#ifndef _LEAVES__CHECK_HPP
#define _LEAVES__CHECK_HPP

#include "_node.hpp"

namespace leaves {

inline std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit) && bit != '"' && bit != '<' && bit != '>' && bit != ']' &&
      bit != '\\' && bit != '}' && bit != '{') {
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

  static void dump_link(std::ostream& out, offset_t link, Storage* storage) {
    switch (link.type()) {
      case LEAF:
        dump_leaf(out, link, storage);
        break;
      case TRIE:
        dump_trie(out, link, storage);
        break;
      default:
        out << "Unknown type: " << link.type() << std::endl;
        break;
    }
  }

  static void dump_leaf(std::ostream& out, offset_e offset, Storage* storage) {
    leaf_ptr leaf = storage->resolve(offset);
    uint16_t size = leaf->size();
    out << "type: leaf" << std::endl;
    out << "id: " << (offset._offset) << std::endl;
    out << "page: " << offset.page() << std::endl;
    out << "freespace: " << BLOCK_SIZES[leaf->slot_id] - size << std::endl;
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

  static void dump_trie(std::ostream& out, offset_t offset, Storage* storage) {
    trie_ptr trie = storage->resolve(offset);
    uint16_t size = trie->size();
    out << "type: trie" << std::endl;
    out << "id: " << offset._offset << std::endl;
    out << "page: " << offset.page() << std::endl;
    out << "txn: " << trie->txn_id << std::endl;
    out << "size: " << size << std::endl;
    out << "freespace: " << BLOCK_SIZES[trie->slot_id] - size << std::endl;
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
    for (offset_e* iter = start; iter < end; iter++) {
      out << "  - " << iter->_offset << std::endl;
    }

    out << "---" << std::endl;
    offset_e last;
    for (offset_e* iter = start; iter < end; iter++) {
      if (*iter != last) {
        last = *iter;
        dump_link(out, *iter, storage);
      }
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES__CHECK_HPP