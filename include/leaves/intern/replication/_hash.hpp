/*
Hash helpers used by the replication protocol and transfer comparison logic.
*/
#ifndef _LEAVES__HASH_HPP
#define _LEAVES__HASH_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <blake3.h>

#include "../core/_node.hpp"
#include "../core/_util.hpp"

namespace leaves {

// Forward declaration — _Cursor is defined in ../db/_cursor.hpp.
// Full include would create a circular dependency via _bigmemory.hpp.
template <typename Traits_>
struct _Cursor;

// Hash size for replication - 32 bytes (256 bits)
static constexpr size_t HASH_SIZE = 32;

#if defined(BLAKE3_API)

struct Blake3Hasher {
  typedef uint8_t hash_t[BLAKE3_KEY_LEN];

  blake3_hasher _hasher;

  Blake3Hasher() { blake3_hasher_init(&_hasher); }

  void update(const void* data, size_t size) {
    blake3_hasher_update(&_hasher, data, size);
  }

  void finalize(hash_t hash) {
    blake3_hasher_finalize(&_hasher, hash, BLAKE3_OUT_LEN);
  }
};

namespace detail {

// Type trait to detect if DB has BigMemory support
template <typename T, typename = void>
struct has_big_memory : std::false_type {};

template <typename T>
struct has_big_memory<T, std::void_t<typename T::BigMemory>> : std::true_type {
};

// Helper to hash big value data when BigMemory is available
template <typename DB, typename LeafPtr, typename Hasher>
void hash_leaf_value_impl(DB* db, LeafPtr leaf, Hasher& hasher,
                          std::true_type /*has_big_memory*/) {
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
void hash_leaf_value_impl(DB* /*db*/, LeafPtr leaf, Hasher& hasher,
                          std::false_type /*has_big_memory*/) {
  hasher.update(leaf->vdata(), leaf->vsize());
}

}  // namespace detail

// Hash Trie Traits

/**
 * @brief Traits for hash trie nodes.
 *
 * Hash trie nodes store only the 32-byte hash of the corresponding data node.
 * They mirror the data trie structure but contain no values.
 *
 * Uses the same underlying node types (_TrieNode, _LeafNode) but with
 * hash storage enabled. Hash "leaves" have key_size=0, value_size=0.
 */
template <typename BaseTraits>
struct HashTrieTraits : BaseTraits {
  // Enable hash storage in nodes
  using hash_t = uint8_t[HASH_SIZE];

  // Minimum hash leaf size (NONE-branch: key_size=0, value_size=0).
  // Non-NONE branch leaves have key_size=1 (data[0] = branch char) and are
  // allocated as sizeof(HashLeafNode) + 1 bytes.
  static constexpr uint16_t HASH_LEAF_SIZE =
      sizeof(_LeafNode<HashTrieTraits>);  // NONE-branch minimum
};

// Hash Updater — data/hash trie sync

/**
 * @brief Updates a separate hash trie to mirror a data trie.
 *
 * Algorithm: Recursive walk of data and hash tries.
 * - Compare txn_id first: if equal, skip (COW guarantee - subtree unchanged)
 * - Handle prefix alignment by walking into branches as needed
 * - At matching positions: prune hash-only branches, add data-only branches,
 *   recurse into common branches
 *
 * Template parameters:
 * @tparam DataDB The data database type (with data trie nodes)
 * @tparam HashDB The hash database type (with hash trie nodes)
 */
template <typename DataDB, typename HashDB>
struct _HashUpdater {
  using DataTraits = typename DataDB::Traits;
  using DataCursorTraits = typename DataDB::CursorTraits;
  using HashTraits = HashTrieTraits<typename HashDB::Traits>;

  using DataTrieNode = _TrieNode<DataCursorTraits>;
  using DataLeafNode = _LeafNode<DataCursorTraits>;
  using HashTrieNode = _TrieNode<HashTraits>;
  using HashLeafNode = _LeafNode<HashTraits>;

  using data_offset_e = typename DataTraits::offset_e;
  using hash_offset_e = typename HashTraits::offset_e;
  using data_trie_ptr =
      typename DataTraits::template Pointer<DataTrieNode, TRIE>;
  using data_leaf_ptr =
      typename DataTraits::template Pointer<DataLeafNode, LEAF>;
  using hash_trie_ptr =
      typename HashTraits::template Pointer<HashTrieNode, TRIE>;
  using hash_leaf_ptr =
      typename HashTraits::template Pointer<HashLeafNode, LEAF>;
  using DataPageHeader = typename DataTraits::PageHeader;
  using HashPageHeader = typename HashTraits::PageHeader;
  using data_page_ptr = typename DataTraits::ptr;
  using hash_page_ptr = typename HashTraits::ptr;

  DataDB* _data_db;
  HashDB* _hash_db;
#ifdef __EMSCRIPTEN__
  uint32_t _yield_counter = 0;
#endif

  _HashUpdater(DataDB* data_db, HashDB* hash_db)
      : _data_db(data_db), _hash_db(hash_db) {}

  // ── txn_id helpers ─────────────────────────────────────────────────────

  tid_t get_data_txn_id(data_offset_e offset) {
    if (offset.type() == LEAF) {
      auto leaf = _data_db->template resolve<DataLeafNode>(&offset);
      data_page_ptr page = leaf - sizeof(DataPageHeader);
      return page->txn_id;
    } else {
      auto trie = _data_db->template resolve<DataTrieNode>(&offset);
      data_page_ptr page = trie - sizeof(DataPageHeader);
      return page->txn_id;
    }
  }

  tid_t get_hash_txn_id(hash_offset_e offset) {
    if (offset.type() == LEAF) {
      auto leaf = _hash_db->template resolve<HashLeafNode>(&offset);
      hash_page_ptr page = leaf - sizeof(HashPageHeader);
      return page->txn_id;
    } else {
      auto trie = _hash_db->template resolve<HashTrieNode>(&offset);
      hash_page_ptr page = trie - sizeof(HashPageHeader);
      return page->txn_id;
    }
  }

  // ── Core sync logic ────────────────────────────────────────────────────

  /**
   * @brief Sync hash node to match data node.
   *
  * Main entry point for recursive walk. Handles all cases:
   * - Both empty, one empty, both present
   * - txn_id comparison to skip unchanged subtrees
   *
   * @param hash_prefix_skip Number of bytes to skip in hash node's prefix.
   *        Used when hash prefix is longer and we're syncing against data
   * child.
   * @param data_prefix_skip Number of bytes to skip in data node's prefix.
   *        Used when data prefix is longer and we're syncing against hash
   * child.
   */
  void sync_nodes(std::string& key_path, data_offset_e data_offset,
                  hash_offset_e* hash_offset_ptr, uint8_t hash_prefix_skip = 0,
                  uint8_t data_prefix_skip = 0) {
#ifdef __EMSCRIPTEN__
    if (++_yield_counter % 100 == 0) emscripten_sleep(0);
#endif
    // Case: data is empty
    if (!data_offset) {
      if (*hash_offset_ptr) {
        free_hash_subtree(*hash_offset_ptr);
        *hash_offset_ptr = hash_offset_e();
      }
      return;
    }

    // Case: hash is empty - deep copy entire data subtree
    if (!*hash_offset_ptr) {
      deep_copy_data_to_hash(key_path, data_offset, hash_offset_ptr);
      return;
    }

    // Both exist - check txn_id (only if no prefix skip, otherwise structure
    // differs)
    if (hash_prefix_skip == 0 && data_prefix_skip == 0) {
      tid_t data_txn = get_data_txn_id(data_offset);
      tid_t hash_txn = get_hash_txn_id(*hash_offset_ptr);

      if (data_txn == hash_txn) {
        // COW guarantee: unchanged subtree, skip
        return;
      }
    }

    // txn_id differs - need to sync structure
    if (data_offset.type() == LEAF) {
      sync_data_leaf(key_path, data_offset, hash_offset_ptr, data_prefix_skip);
    } else {
      sync_data_trie(key_path, data_offset, hash_offset_ptr, hash_prefix_skip,
                     data_prefix_skip);
    }
  }

  /**
   * @brief Sync when data is a leaf.
   */
  void sync_data_leaf(std::string& key_path, data_offset_e data_offset,
                      hash_offset_e* hash_offset_ptr,
                      uint8_t data_prefix_skip = 0) {
    auto data_leaf = _data_db->template resolve<DataLeafNode>(&data_offset);

    // Save key path state
    size_t saved_len = key_path.size();
    key_path.append((const char*)data_leaf->data + data_prefix_skip,
                    data_leaf->key_size - data_prefix_skip);

    // Replace whatever is in hash with a new hash leaf
    if (*hash_offset_ptr) {
      free_hash_subtree(*hash_offset_ptr);
    }

    // Create hash leaf with computed hash
    hash_leaf_ptr hash_leaf = create_leaf_hash(key_path, data_leaf);
    *hash_offset_ptr = _hash_db->resolve(hash_leaf);

    key_path.resize(saved_len);
  }

  /**
   * @brief Sync when data is a trie.
   *
   * @param hash_prefix_skip Bytes to skip in hash node's prefix (for prefix
   * alignment)
   * @param data_prefix_skip Bytes to skip in data node's prefix (for prefix
   * alignment)
   */
  void sync_data_trie(std::string& key_path, data_offset_e data_offset,
                      hash_offset_e* hash_offset_ptr,
                      uint8_t hash_prefix_skip = 0,
                      uint8_t data_prefix_skip = 0) {
    auto data_trie = _data_db->template resolve<DataTrieNode>(&data_offset);

    // If hash is a leaf, replace entirely
    if (hash_offset_ptr->type() == LEAF) {
      free_hash_subtree(*hash_offset_ptr);
      *hash_offset_ptr = hash_offset_e();
      deep_copy_data_to_hash(key_path, data_offset, hash_offset_ptr);
      return;
    }

    auto hash_trie = _hash_db->template resolve<HashTrieNode>(hash_offset_ptr);

    // Check prefix alignment (applying skip to both prefixes)
    uint8_t data_len = data_trie->len();
    uint8_t hash_len = hash_trie->len();
    uint8_t effective_data_len = data_len - data_prefix_skip;
    uint8_t effective_hash_len = hash_len - hash_prefix_skip;
    const uint8_t* data_prefix = data_trie->compressed() + data_prefix_skip;
    const uint8_t* hash_prefix = hash_trie->compressed() + hash_prefix_skip;

    // Find common prefix length
    uint8_t common = 0;
    while (common < effective_data_len && common < effective_hash_len &&
           data_prefix[common] == hash_prefix[common]) {
      common++;
    }

    if (common < effective_data_len && common < effective_hash_len) {
      // Prefixes diverge at position 'common' - replace hash entirely
      free_hash_subtree(*hash_offset_ptr);
      *hash_offset_ptr = hash_offset_e();
      deep_copy_data_to_hash(key_path, data_offset, hash_offset_ptr);
      return;
    }

    if (common < effective_hash_len) {
      // Hash prefix is longer - e.g., data effective prefix="ab", hash
      // effective prefix="abcd" Hash embeds bytes that data has as branches.
      // diverge_byte is the next byte in the hash prefix after common
      uint8_t diverge_byte = hash_prefix[common];

      if (!(data_trie->isset)(diverge_byte)) {
        // Data doesn't have a branch matching hash's next prefix byte
        // Hash is completely stale - replace entirely
        free_hash_subtree(*hash_offset_ptr);
        *hash_offset_ptr = hash_offset_e();
        deep_copy_data_to_hash(key_path, data_offset, hash_offset_ptr);
        return;
      }

      // Build new hash trie mirroring data's structure
      size_t saved_len = key_path.size();
      key_path.append((const char*)data_prefix, effective_data_len);

      hash_offset_e offsets_raw[HashTrieNode::MAX_BRANCH_COUNT] = {};
      hash_offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 0;
      uint8_t upper = 0;
      bool hash_consumed = false;

      data_trie->for_each_branch([&](int k, auto* data_off) {
        data_offset_e data_child = *data_off;

        hash_offset_e hash_child{};
        if (k == (int)diverge_byte && !hash_consumed) {
          // This data branch aligns with hash's embedded prefix.
          // The child's compressed[0] is the branch char; don't skip it
          // so the recursive call naturally appends it to key_path.
          // Keep the matching byte in hash's prefix too (no +1).
          hash_child = *hash_offset_ptr;
          uint8_t new_hash_skip = hash_prefix_skip + common;
          uint8_t new_data_skip = 0;
          sync_nodes(key_path, data_child, &hash_child, new_hash_skip,
                     new_data_skip);
          hash_consumed = true;
        } else {
          // Non-matching branch: child appends full compressed (no skip),
          // so branch char is included naturally — no push needed.
          deep_copy_data_to_hash(key_path, data_child, &hash_child);
        }

        offsets_buf[k] = hash_child;
        branch_count++;
        if (k != DataTrieNode::NONE) upper |= (1u << HashTrieNode::ubit(k));
      });

      if (!hash_consumed) {
        free_hash_subtree(*hash_offset_ptr);
      }

      // Create new hash trie mirroring data's full compressed prefix.
      // Must use the FULL compressed (not effective/skipped), because
      // compute_trie_hash hashes trie->compressed() and it must match
      // what compute_node_hash would produce for this data trie node.
      Slice prefix((const char*)data_trie->compressed(), data_trie->len());
      data_page_ptr data_page = data_trie - sizeof(DataPageHeader);
      hash_trie_ptr new_hash_trie =
          alloc_hash_trie(prefix.size(), branch_count, data_page->txn_id);
      new_hash_trie->create(prefix, offsets_buf, upper);
      compute_trie_hash(new_hash_trie);
      *hash_offset_ptr = _hash_db->resolve(new_hash_trie);

      key_path.resize(saved_len);
      return;
    }

    if (common < effective_data_len) {
      // Data prefix is longer - e.g., data effective prefix="abcd", hash
      // effective prefix="ab" Hash has branches where data has embedded prefix
      // bytes. Only the hash branch matching data's next byte is relevant.
      uint8_t next_byte = data_prefix[common];

      // Free hash branches that don't match
      hash_trie->for_each_branch([&](int k, auto* off) {
        if (k != (int)next_byte) {
          free_hash_subtree(*off);
        }
      });

      if ((hash_trie->isset)(next_byte)) {
        // Sync data with hash's matching branch child.
        // Don't skip the branch byte on either side — the recursive call
        // will match it in the common prefix and append it to key_path.
        hash_offset_e hash_child = *hash_trie->offset(next_byte);
        size_t saved_len = key_path.size();
        key_path.append((const char*)data_prefix, common);

        uint8_t new_data_skip = data_prefix_skip + common;
        uint8_t new_hash_skip = 0;
        sync_nodes(key_path, data_offset, &hash_child, new_hash_skip,
                   new_data_skip);

        // Replace the hash trie with the synced result
        free_hash_node(hash_trie);
        *hash_offset_ptr = hash_child;

        key_path.resize(saved_len);
      } else {
        // Hash doesn't have matching branch - deep copy data
        free_hash_node(hash_trie);
        *hash_offset_ptr = hash_offset_e();
        deep_copy_data_to_hash(key_path, data_offset, hash_offset_ptr);
      }
      return;
    }

    // Prefixes match exactly - sync branches
    size_t saved_len = key_path.size();
    key_path.append((const char*)data_prefix, effective_data_len);

    sync_matching_tries(key_path, data_trie, hash_offset_ptr, data_prefix_skip);

    key_path.resize(saved_len);
  }

  /**
   * @brief Sync when data and hash tries have matching prefixes.
   *
   * - Remove branches only in hash
   * - Add branches only in data
   * - Recurse into common branches
   *
   * @param data_prefix_skip Bytes to skip in data prefix (for virtual
   * alignment)
   */
  void sync_matching_tries(std::string& key_path, data_trie_ptr data_trie,
                           hash_offset_e* hash_offset_ptr,
                           uint8_t data_prefix_skip = 0) {
    auto hash_trie = _hash_db->template resolve<HashTrieNode>(hash_offset_ptr);

    // Build offset array for new hash trie
    hash_offset_e offsets_raw[HashTrieNode::MAX_BRANCH_COUNT] = {};
    hash_offset_e* offsets_buf = &offsets_raw[1];  // -1..255 indexing
    int branch_count = 0;
    uint8_t upper = 0;

    // Track which hash branches exist
    bool hash_has[257] = {};
    hash_trie->for_each_branch([&](int k, auto*) {
      hash_has[k + 1] = true;  // +1 because NONE is -1
    });

    data_trie->for_each_branch([&](int k, auto* data_off) {
      data_offset_e data_child = *data_off;
      bool has_hash = hash_has[k + 1];
      hash_offset_e hash_child_init =
          has_hash ? *hash_trie->offset(k) : hash_offset_e{};

      auto do_branch = [this, k, data_child, has_hash, hash_child_init,
                        offsets_buf](std::string& kp) {
        hash_offset_e hash_child = hash_child_init;
        if (has_hash) {
          sync_nodes(kp, data_child, &hash_child);
        } else {
          deep_copy_data_to_hash(kp, data_child, &hash_child);
        }
        offsets_buf[k] = hash_child;
      };
      do_branch(key_path);
      branch_count++;
      if (k != DataTrieNode::NONE) upper |= (1u << HashTrieNode::ubit(k));
    });

    // Hash-only branches are implicitly pruned (not in offsets_buf)
    // Free them
    hash_trie->for_each_branch([&](int k, auto* off) {
      if (!(data_trie->isset)(k)) {
        free_hash_subtree(*off);
      }
    });

    // Create new hash trie mirroring data's full compressed prefix.
    Slice prefix((const char*)data_trie->compressed(), data_trie->len());
    data_page_ptr data_page = data_trie - sizeof(DataPageHeader);
    hash_trie_ptr new_hash_trie =
        alloc_hash_trie(prefix.size(), branch_count, data_page->txn_id);
    new_hash_trie->create(prefix, offsets_buf, upper);
    compute_trie_hash(new_hash_trie);

    // Free old hash trie node and update pointer
    free_hash_node(hash_trie);
    *hash_offset_ptr = _hash_db->resolve(new_hash_trie);
  }

  // ── Deep copy operations ───────────────────────────────────────────────

  /**
   * @brief Deep copy data subtree to hash trie, computing hashes.
   */
  void deep_copy_data_to_hash(std::string& key_path, data_offset_e data_offset,
                              hash_offset_e* hash_offset_ptr) {
    if (!data_offset) {
      *hash_offset_ptr = hash_offset_e();
      return;
    }

    if (data_offset.type() == LEAF) {
      auto data_leaf = _data_db->template resolve<DataLeafNode>(&data_offset);
      size_t saved_len = key_path.size();
      key_path.append((const char*)data_leaf->data, data_leaf->key_size);

      hash_leaf_ptr hash_leaf = create_leaf_hash(key_path, data_leaf);
      *hash_offset_ptr = _hash_db->resolve(hash_leaf);

      key_path.resize(saved_len);
    } else {
      auto data_trie = _data_db->template resolve<DataTrieNode>(&data_offset);
      size_t saved_len = key_path.size();
      key_path.append((const char*)data_trie->compressed(), data_trie->len());

      hash_offset_e offsets_raw[HashTrieNode::MAX_BRANCH_COUNT] = {};
      hash_offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 0;
      uint8_t upper = 0;

      data_trie->for_each_branch([&](int k, auto* data_off) {
        data_offset_e data_child = *data_off;

        auto do_branch = [this, k, data_child, offsets_buf](std::string& kp) {
          hash_offset_e result{};
          deep_copy_data_to_hash(kp, data_child, &result);
          offsets_buf[k] = result;
        };
        do_branch(key_path);
        branch_count++;
        if (k != DataTrieNode::NONE) upper |= (1u << HashTrieNode::ubit(k));
      });

      // Create hash trie
      Slice prefix((const char*)data_trie->compressed(), data_trie->len());
      data_page_ptr data_page = data_trie - sizeof(DataPageHeader);
      hash_trie_ptr hash_trie =
          alloc_hash_trie(prefix.size(), branch_count, data_page->txn_id);
      hash_trie->create(prefix, offsets_buf, upper);

      // Compute trie hash
      compute_trie_hash(hash_trie);

      *hash_offset_ptr = _hash_db->resolve(hash_trie);
      key_path.resize(saved_len);
    }
  }

  // ── Hash computation ───────────────────────────────────────────────────

  /**
   * @brief Create hash leaf from data leaf.
   */
  hash_leaf_ptr create_leaf_hash(const std::string& key_path,
                                 data_leaf_ptr data_leaf) {
    // Derive branch key from data leaf's key: data[0] is the branch char,
    // or NONE if key_size == 0 (exact-match / NONE-branch leaf).
    int branch_key =
        (data_leaf->key_size > 0)
            ? static_cast<int>(static_cast<uint8_t>(data_leaf->data[0]))
            : HashTrieNode::NONE;

    // Get data leaf's txn_id from its page header
    data_page_ptr data_page = data_leaf - sizeof(DataPageHeader);
    tid_t txn_id = data_page->txn_id;

    // Compute leaf hash: Blake3(full_key || value)
    Blake3Hasher hasher;
    hasher.update(key_path.data(), key_path.size());

    // Hash the value - handle big values if supported
    if constexpr (detail::has_big_memory<DataDB>::value) {
      detail::hash_leaf_value_impl(_data_db, data_leaf, hasher,
                                   std::true_type{});
    } else {
      hasher.update(data_leaf->vdata(), data_leaf->vsize());
    }

    hash_leaf_ptr hash_leaf = alloc_hash_leaf(txn_id, branch_key);
    hasher.finalize(hash_leaf->hash);
    return hash_leaf;
  }

  /**
   * @brief Compute hash for trie node from children's hashes.
   */
  void compute_trie_hash(hash_trie_ptr trie) {
    Blake3Hasher hasher;
    hasher.update(trie->compressed(), trie->len());

    // Hash all child hashes in array order
    hash_offset_e* children = trie->array();
    for (int i = 0; i < trie->count(); ++i) {
      if (children[i].type() == LEAF) {
        auto child = _hash_db->template resolve<HashLeafNode>(&children[i]);
        hasher.update(child->hash, sizeof(child->hash));
      } else {
        auto child = _hash_db->template resolve<HashTrieNode>(&children[i]);
        hasher.update(child->hash, sizeof(child->hash));
      }
    }
    hasher.finalize(trie->hash);
  }

  // ── Allocation helpers ─────────────────────────────────────────────────

  hash_leaf_ptr alloc_hash_leaf(tid_t txn_id,
                                int branch_key = HashTrieNode::NONE) {
    // NONE-branch leaf: key_size=0.  Non-NONE branch: data[0]=branch_char.
    uint16_t size = static_cast<uint16_t>(
        sizeof(HashLeafNode) + (branch_key == HashTrieNode::NONE ? 0 : 1));
    hash_leaf_ptr leaf = _hash_db->template alloc_node<hash_leaf_ptr>(size);
    if (branch_key == HashTrieNode::NONE) {
      leaf->key_size = 0;
    } else {
      leaf->key_size = 1;
      leaf->data[0] = static_cast<uint8_t>(branch_key);
    }
    leaf->value_size = 0;
    // Copy txn_id to hash page header
    hash_page_ptr page = leaf - sizeof(HashPageHeader);
    page->txn_id = txn_id;
    return leaf;
  }

  hash_trie_ptr alloc_hash_trie(uint8_t prefix_len, int branch_count,
                                tid_t txn_id) {
    uint16_t size = HashTrieNode::size(prefix_len, branch_count);
    hash_trie_ptr trie = _hash_db->template alloc_node<hash_trie_ptr>(size);
    // Copy txn_id to hash page header
    hash_page_ptr page = trie - sizeof(HashPageHeader);
    page->txn_id = txn_id;
    return trie;
  }

  // ── Free helpers ───────────────────────────────────────────────────────

  template <typename NodePtr>
  void free_hash_node(NodePtr node) {
    hash_page_ptr page = node - sizeof(HashPageHeader);
    _hash_db->free(page);
  }

  void free_hash_subtree(hash_offset_e offset) {
    if (!offset) return;

    if (offset.type() == LEAF) {
      auto leaf = _hash_db->template resolve<HashLeafNode>(&offset);
      free_hash_node(leaf);
    } else {
      auto trie = _hash_db->template resolve<HashTrieNode>(&offset);
      trie->for_each_branch([&](int k, auto* off) {
        free_hash_subtree(*off);
      });
      free_hash_node(trie);
    }
  }
};

/**
 * @brief Update hash trie for a data trie.
 *
 * Convenience function to update the hash trie to mirror the data trie.
 * Uses txn_id comparison to skip unchanged subtrees (COW guarantee).
 *
 * @param data_db Data database
 * @param hash_db Hash database (may be same as data_db)
 * @param data_root Root of data trie
 * @param hash_root_ptr Pointer to hash trie root (modified in place)
 */
template <typename DataDB, typename HashDB>
void update_hash_trie(DataDB* data_db, HashDB* hash_db,
                      typename DataDB::offset_e data_root,
                      typename HashDB::offset_e* hash_root_ptr) {
  std::string key_path;
  key_path.reserve(255);
  _HashUpdater<DataDB, HashDB> updater(data_db, hash_db);
  updater.sync_nodes(key_path, data_root, hash_root_ptr);
}

// Hash Lookup — cursor-based hash trie navigation

/**
 * @brief Encapsulates cursor-based hash trie lookup.
 *
 * Maintains its own cursor and root pointer for navigating the hash trie.
 * Correctly handles hash tries whose compressed-path structure differs
 * from the data/wire trie.
 *
 * @tparam DB The database type (provides CursorTraits, Traits)
 */
template <typename DB>
struct _HashLookup {
  using HashCursorTraits_ = HashTrieTraits<typename DB::CursorTraits>;
  using HashCursor_ = _Cursor<HashCursorTraits_>;
  using offset_e = typename DB::Traits::offset_e;

  HashCursor_ _cursor;
  offset_e* _root;

  _HashLookup(DB* db, offset_e* root) : _cursor(db, root), _root(root) {}

  void set_root(offset_e* root) {
    _root = root;
    _cursor.set_root(root);
    // Hash trie is non-COW: interior nodes may be freed/replaced
    // without the root offset changing. Always invalidate the stack.
    _cursor.clear();
  }

  /**
   * @brief Look up the hash for the node at `path` in the hash trie.
   *
   * @param path The key path to look up
   * @param expected_type LEAF or TRIE — determines which hash-trie node we
   * expect
   * @return Pointer to the 32-byte hash, or nullptr if not found
   */
  const uint8_t* find(const std::string& path, uint8_t expected_type) {
    if (!*_root) return nullptr;

    // Switch cursor root if it was changed externally
    if (_cursor._root != _root) {
      _cursor.set_root(_root);
    }

    _cursor.find(Slice(path));
    if (_cursor.stack.size == 0) return nullptr;

    auto& back = _cursor.stack.back();

    if (expected_type == LEAF) {
      // Hash leaves store only the branch char (key_size=1, data[0]=char)
      // or nothing for NONE-branch (key_size=0).  Since key_size < search
      // key length for non-NONE leaves, _Transition::success() (cmp==0
      // && is_leaf()) would wrongly return false.  is_leaf() suffices:
      // the trie navigation already reaches the correct node, and the
      // final BLAKE3 comparison catches mismatches.
      if (back.is_leaf()) return back.leaf()->hash;
      return nullptr;
    }

    // expected_type == TRIE: return the deepest trie node hash the cursor
    // landed on.  If the path overshoots or undershoots, the hash simply
    // won't match the wire hash, so the caller treats it as "hashes differ"
    // — no incorrect pruning can occur (would require a BLAKE3 collision).
    if (back.is_trie()) return back.trie()->hash;

    // Cursor followed the NONE-branch into a leaf — the trie we want is
    // the parent frame.
    if (_cursor.stack.size >= 2) {
      auto& parent = _cursor.stack.data[_cursor.stack.size - 2];
      if (parent.is_trie()) return parent.trie()->hash;
    }

    return nullptr;
  }
};

#endif  // BLAKE3_API

}  // namespace leaves

#endif  // _LEAVES__HASH_HPP