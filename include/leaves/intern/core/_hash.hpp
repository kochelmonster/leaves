#ifndef _LEAVES__HASH_HPP
#define _LEAVES__HASH_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>

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

template <typename DB>
void compute_node_hash(DB* db, typename DB::offset_e offset, tid_t current_txn_id) {
  using Traits = typename DB::Traits;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using trie_ptr = typename Traits::template Pointer<TrieNode>;
  using leaf_ptr = typename Traits::template Pointer<LeafNode>;
  using offset_e = typename Traits::offset_e;

  if (offset.type() == LEAF) {
    leaf_ptr leaf = db->template resolve<LeafNode>(&offset);
    if (leaf->txn_id == current_txn_id) {
      // Hash key and value
      Blake3Hasher hasher;
      hasher.update(leaf->data, leaf->key_size);
      Slice value = leaf->value();
      hasher.update(value.data(), value.size());
      hasher.finalize(leaf->hash);
      db->make_dirty(leaf);
    }
    return;
  }

  assert(offset.type() == TRIE);
  trie_ptr trie = db->template resolve<TrieNode>(&offset);

  // First recursively hash children
  offset_e* begin = trie->array();
  offset_e* end = begin + trie->count();
  for (offset_e* iter = begin; iter != end; iter++) {
    compute_node_hash(db, *iter, current_txn_id);
  }

  // Then hash this node if it was modified
  if (trie->txn_id == current_txn_id) {
    Blake3Hasher hasher;

    // Hash the compressed path
    hasher.update(trie->compressed(), trie->len());

    // Hash all child hashes
    for (offset_e* iter = begin; iter != end; iter++) {
      if (iter->type() == LEAF) {
        leaf_ptr child = db->template resolve<LeafNode>(iter);
        hasher.update(child->hash, sizeof(child->hash));
      } else {
        trie_ptr child = db->template resolve<TrieNode>(iter);
        hasher.update(child->hash, sizeof(child->hash));
      }
    }

    hasher.finalize(trie->hash);
    db->make_dirty(trie);
  }
}

}  // namespace detail

// compute_hashes for Blake3Hasher - computes merkle hashes for all modified nodes
template <typename DB>
void compute_hashes(Blake3Hasher, DB* db, typename DB::txn_ptr txn) {
  if (!txn->root) return;
  detail::compute_node_hash(db, txn->root, txn->txn_id);
}

#endif  // BLAKE3_API

#if defined(BOOST_HASH)

#include <boost/hash2/sha3.hpp>

typedef boost::hash2::sha3_256 BoostHasher;

#endif

}  // namespace leaves

#endif  // _LEAVES__HASH_HPP