#ifndef _LEAVES__HASH_HPP
#define _LEAVES__HASH_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "_node.hpp"
#include "_util.hpp"

namespace leaves {

// Hash size for replication - 32 bytes (256 bits)
static constexpr size_t HASH_SIZE = 32;

struct NullHasher {
  typedef uint8_t hash_t[0];

  NullHasher() {}
  void update(const void* /*data*/, size_t /*size*/) {}
  void finalize(hash_t /*hash*/) {}
};

// compute_hashes for NullHasher - does nothing
template <typename DB>
void compute_hashes(NullHasher, DB* /*db*/, typename DB::txn_ptr /*txn*/) {
  // No-op for NullHasher
}

#if defined(BLAKE3_API)

struct Blake3Hasher {
  typedef uint8_t hash_t[BLAKE3_KEY_LEN];

  blake3_hasher _hasher;

  Blake3Hasher() { blake3_hasher_init(&_hasher); }

  void update(const void* data, size_t size) {
    blake3_hasher_update(&_hasher, data, size);
  }

  void finalize(hash_t hash) { blake3_hasher_finalize(&_hasher, hash, BLAKE3_OUT_LEN); }
};

namespace detail {

// Type trait to detect if DB has BigMemory support
template <typename T, typename = void>
struct has_big_memory : std::false_type {};

template <typename T>
struct has_big_memory<T, std::void_t<typename T::BigMemory>> : std::true_type {};

// Helper to hash big value data when BigMemory is available
template <typename DB, typename LeafPtr, typename Hasher>
void hash_leaf_value_impl(DB* db, LeafPtr leaf, Hasher& hasher, std::true_type /*has_big_memory*/) {
  if (leaf->is_big()) {
    using BigValue = typename DB::BigMemory::BigValue;
    BigValue* bvalue = (BigValue*)leaf->vdata();
    auto data_ptr = bvalue->data(db);
    hasher.update(data_ptr, bvalue->value_size);
  } else {
    hasher.update(leaf->vdata(), leaf->vsize());
  }
}

// Helper when BigMemory is not available - just hash inline value
template <typename DB, typename LeafPtr, typename Hasher>
void hash_leaf_value_impl(DB* /*db*/, LeafPtr leaf, Hasher& hasher, std::false_type /*has_big_memory*/) {
  hasher.update(leaf->vdata(), leaf->vsize());
}

template <typename DB>
void compute_node_hash(DB* db, typename DB::offset_e offset, tid_t current_txn_id,
                       std::string& key_path) {
  using Traits = typename DB::Traits;
  using CursorTraits = typename DB::CursorTraits;
  using TrieNode = _TrieNode<CursorTraits>;
  using LeafNode = _LeafNode<CursorTraits>;
  using PageHeader = typename Traits::PageHeader;
  using trie_ptr = typename Traits::template Pointer<TrieNode>;
  using leaf_ptr = typename Traits::template Pointer<LeafNode>;
  using page_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;

  if (offset.type() == LEAF) {
    leaf_ptr leaf = db->template resolve<LeafNode>(&offset);
    // Access PageHeader before the node
    page_ptr page_header = leaf - sizeof(PageHeader);
    if (page_header->txn_id == current_txn_id) {
      // Hash full key (path + leaf's key suffix) and value
      // Note: Blake3Hasher uses stack-only memory (~2KB), safe to create per-call
      Blake3Hasher hasher;
      hasher.update(key_path.data(), key_path.size());
      hasher.update(leaf->data, leaf->key_size);
      
      // Hash the value - uses BigMemory if available
      hash_leaf_value_impl(db, leaf, hasher, has_big_memory<DB>{});
      
      hasher.finalize(leaf->hash);
      db->make_dirty(leaf);
    }
    return;
  }

  assert(offset.type() == TRIE);
  trie_ptr trie = db->template resolve<TrieNode>(&offset);
  
  // Access PageHeader before the node
  page_ptr page_header = trie - sizeof(PageHeader);
  
  // If this node wasn't modified in current transaction, skip entire subtree
  if (page_header->txn_id != current_txn_id) {
    return;
  }
  
  // Save current path length to restore later
  size_t path_len = key_path.size();
  
  // Add this trie's compressed path to the key path
  key_path.append((const char*)trie->compressed(), trie->len());

  // Recursively hash all children
  offset_e* children = trie->array();
  uint16_t child_count = trie->count();
  for (uint16_t i = 0; i < child_count; ++i) {
    compute_node_hash(db, children[i], current_txn_id, key_path);
  }
  
  // Restore original path length
  key_path.resize(path_len);

  // Now compute this node's hash
  Blake3Hasher hasher;

  // Hash the compressed path
  hasher.update(trie->compressed(), trie->len());

  // Hash all child hashes in array order
  for (uint16_t i = 0; i < child_count; ++i) {
    if (children[i].type() == LEAF) {
      leaf_ptr child = db->template resolve<LeafNode>(&children[i]);
      hasher.update(child->hash, sizeof(child->hash));
    } else {
      trie_ptr child = db->template resolve<TrieNode>(&children[i]);
      hasher.update(child->hash, sizeof(child->hash));
    }
  }

  hasher.finalize(trie->hash);
  db->make_dirty(trie);
}

}  // namespace detail

// compute_hashes for Blake3Hasher - computes merkle hashes for all modified nodes
template <typename DB>
void compute_hashes(Blake3Hasher, DB* db, typename DB::txn_ptr txn) {
  if (!txn->root) return;
  std::string key_path;
  key_path.reserve(255);  // reasonable value
  detail::compute_node_hash(db, txn->root, txn->txn_id, key_path);
}

#endif  // BLAKE3_API

#if defined(BOOST_HASH)

#include <boost/hash2/sha3.hpp>

typedef boost::hash2::sha3_256 BoostHasher;

#endif

}  // namespace leaves

#endif  // _LEAVES__HASH_HPP