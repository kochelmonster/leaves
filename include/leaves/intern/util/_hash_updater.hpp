#ifndef _LEAVES_HASH_UPDATER_HPP
#define _LEAVES_HASH_UPDATER_HPP

#include "../core/_hash.hpp"
#include "../core/_node.hpp"

namespace leaves {

#if defined(BLAKE3_API)

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

/**
 * @brief Updates a separate hash trie to mirror a data trie.
 *
 * Algorithm: Parallel recursive walk of data and hash tries.
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
  std::string _key_path;

  _HashUpdater(DataDB* data_db, HashDB* hash_db)
      : _data_db(data_db), _hash_db(hash_db) {
    _key_path.reserve(255);
  }

  /**
   * @brief Update hash trie to match data trie.
   *
   * @param data_root Root offset of data trie
   * @param hash_root_ptr Pointer to hash trie root offset (modified in place)
   */
  void exec(data_offset_e data_root, hash_offset_e* hash_root_ptr) {
    sync_nodes(data_root, hash_root_ptr);
  }

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
   * Main entry point for parallel walk. Handles all cases:
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
  void sync_nodes(data_offset_e data_offset, hash_offset_e* hash_offset_ptr,
                  uint8_t hash_prefix_skip = 0, uint8_t data_prefix_skip = 0) {
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
      deep_copy_data_to_hash(data_offset, hash_offset_ptr);
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
      sync_data_leaf(data_offset, hash_offset_ptr, data_prefix_skip);
    } else {
      sync_data_trie(data_offset, hash_offset_ptr, hash_prefix_skip,
                     data_prefix_skip);
    }
  }

  /**
   * @brief Sync when data is a leaf.
   */
  void sync_data_leaf(data_offset_e data_offset, hash_offset_e* hash_offset_ptr,
                      uint8_t data_prefix_skip = 0) {
    auto data_leaf = _data_db->template resolve<DataLeafNode>(&data_offset);

    // Save key path state
    size_t saved_len = _key_path.size();
    _key_path.append((const char*)data_leaf->data + data_prefix_skip,
                     data_leaf->key_size - data_prefix_skip);

    // Replace whatever is in hash with a new hash leaf
    if (*hash_offset_ptr) {
      free_hash_subtree(*hash_offset_ptr);
    }

    // Create hash leaf with computed hash
    hash_leaf_ptr hash_leaf = create_leaf_hash(data_leaf);
    *hash_offset_ptr = _hash_db->resolve(hash_leaf);

    _key_path.resize(saved_len);
  }

  /**
   * @brief Sync when data is a trie.
   *
   * @param hash_prefix_skip Bytes to skip in hash node's prefix (for prefix
   * alignment)
   * @param data_prefix_skip Bytes to skip in data node's prefix (for prefix
   * alignment)
   */
  void sync_data_trie(data_offset_e data_offset, hash_offset_e* hash_offset_ptr,
                      uint8_t hash_prefix_skip = 0,
                      uint8_t data_prefix_skip = 0) {
    auto data_trie = _data_db->template resolve<DataTrieNode>(&data_offset);

    // If hash is a leaf, replace entirely
    if (hash_offset_ptr->type() == LEAF) {
      free_hash_subtree(*hash_offset_ptr);
      *hash_offset_ptr = hash_offset_e();
      deep_copy_data_to_hash(data_offset, hash_offset_ptr);
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
      deep_copy_data_to_hash(data_offset, hash_offset_ptr);
      return;
    }

    if (common < effective_hash_len) {
      // Hash prefix is longer - e.g., data effective prefix="ab", hash
      // effective prefix="abcd" Hash embeds bytes that data has as branches.
      // diverge_byte is the next byte in the hash prefix after common
      uint8_t diverge_byte = hash_prefix[common];

      if (!data_trie->isset(diverge_byte)) {
        // Data doesn't have a branch matching hash's next prefix byte
        // Hash is completely stale - replace entirely
        free_hash_subtree(*hash_offset_ptr);
        *hash_offset_ptr = hash_offset_e();
        deep_copy_data_to_hash(data_offset, hash_offset_ptr);
        return;
      }

      // Build new hash trie mirroring data's structure
      size_t saved_len = _key_path.size();
      _key_path.append((const char*)data_prefix, effective_data_len);

      hash_offset_e offsets_raw[HashTrieNode::MAX_BRANCH_COUNT] = {};
      hash_offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 0;
      bool hash_consumed = false;

      for (int k = data_trie->first(); k != DataTrieNode::OUT_OF_RANGE;
           k = data_trie->next(k)) {
        data_offset_e data_child = *data_trie->offset(k);

        hash_offset_e hash_child{};
        if (k == (int)diverge_byte && !hash_consumed) {
          // This data branch aligns with hash's embedded prefix.
          // The child's compressed[0] is the branch char, which will be
          // skipped via data_prefix_skip=1.  We must include it in
          // _key_path so the hash covers the full key.
          hash_child = *hash_offset_ptr;
          if (k != DataTrieNode::NONE) _key_path.push_back((char)k);
          uint8_t new_hash_skip = hash_prefix_skip + common + 1;
          uint8_t new_data_skip =
              1;  // Skip the branch byte embedded in child's prefix
          sync_nodes(data_child, &hash_child, new_hash_skip, new_data_skip);
          if (k != DataTrieNode::NONE) _key_path.pop_back();
          hash_consumed = true;
        } else {
          // Non-matching branch: child appends full compressed (no skip),
          // so branch char is included naturally — no push needed.
          deep_copy_data_to_hash(data_child, &hash_child);
        }

        offsets_buf[k] = hash_child;
        branch_count++;
      }

      if (!hash_consumed) {
        free_hash_subtree(*hash_offset_ptr);
      }

      // Create new hash trie mirroring data's full compressed prefix.
      // Must use the FULL compressed (not effective/skipped), because
      // compute_trie_hash hashes trie->compressed() and it must match
      // what compute_node_hash would produce for this data trie node.
      Slice prefix((const char*)data_trie->compressed(), data_trie->len());
      data_page_ptr data_page((char*)&*data_trie - sizeof(DataPageHeader));
      hash_trie_ptr new_hash_trie =
          alloc_hash_trie(prefix.size(), branch_count, data_page->txn_id);
      new_hash_trie->create(prefix, offsets_buf);
      compute_trie_hash(new_hash_trie);
      *hash_offset_ptr = _hash_db->resolve(new_hash_trie);

      _key_path.resize(saved_len);
      return;
    }

    if (common < effective_data_len) {
      // Data prefix is longer - e.g., data effective prefix="abcd", hash
      // effective prefix="ab" Hash has branches where data has embedded prefix
      // bytes. Only the hash branch matching data's next byte is relevant.
      uint8_t next_byte = data_prefix[common];

      // Free hash branches that don't match
      for (int k = hash_trie->first(); k != HashTrieNode::OUT_OF_RANGE;
           k = hash_trie->next(k)) {
        if (k != (int)next_byte) {
          free_hash_subtree(*hash_trie->offset(k));
        }
      }

      if (hash_trie->isset(next_byte)) {
        // Sync data with hash's matching branch child
        // Need to skip the consumed data prefix: data_prefix_skip + common + 1
        // Hash child's first byte is the branch key, so skip 1 byte in hash too
        hash_offset_e hash_child = *hash_trie->offset(next_byte);
        size_t saved_len = _key_path.size();
        _key_path.append((const char*)data_prefix, common);
        _key_path.push_back((char)next_byte);

        uint8_t new_data_skip = data_prefix_skip + common + 1;
        uint8_t new_hash_skip =
            1;  // Skip the branch byte embedded in hash child's prefix
        sync_nodes(data_offset, &hash_child, new_hash_skip, new_data_skip);

        // Replace the hash trie with the synced result
        free_hash_node(hash_trie);
        *hash_offset_ptr = hash_child;

        _key_path.resize(saved_len);
      } else {
        // Hash doesn't have matching branch - deep copy data
        free_hash_node(hash_trie);
        *hash_offset_ptr = hash_offset_e();
        deep_copy_data_to_hash(data_offset, hash_offset_ptr);
      }
      return;
    }

    // Prefixes match exactly - sync branches
    size_t saved_len = _key_path.size();
    _key_path.append((const char*)data_prefix, effective_data_len);

    sync_matching_tries(data_trie, hash_offset_ptr, data_prefix_skip);

    _key_path.resize(saved_len);
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
  void sync_matching_tries(data_trie_ptr data_trie,
                           hash_offset_e* hash_offset_ptr,
                           uint8_t data_prefix_skip = 0) {
    auto hash_trie = _hash_db->template resolve<HashTrieNode>(hash_offset_ptr);

    // Build offset array for new hash trie
    hash_offset_e offsets_raw[HashTrieNode::MAX_BRANCH_COUNT] = {};
    hash_offset_e* offsets_buf = &offsets_raw[1];  // -1..255 indexing
    int branch_count = 0;

    // Track which hash branches exist
    bool hash_has[257] = {};
    for (int k = hash_trie->first(); k != HashTrieNode::OUT_OF_RANGE;
         k = hash_trie->next(k)) {
      hash_has[k + 1] = true;  // +1 because NONE is -1
    }

    // Process all data branches
    for (int k = data_trie->first(); k != DataTrieNode::OUT_OF_RANGE;
         k = data_trie->next(k)) {
      data_offset_e data_child = *data_trie->offset(k);

      if (hash_has[k + 1]) {
        // Common branch - recurse
        hash_offset_e hash_child = *hash_trie->offset(k);
        sync_nodes(data_child, &hash_child);
        offsets_buf[k] = hash_child;
      } else {
        // Data-only branch - deep copy
        hash_offset_e hash_child{};
        deep_copy_data_to_hash(data_child, &hash_child);
        offsets_buf[k] = hash_child;
      }

      branch_count++;
    }

    // Hash-only branches are implicitly pruned (not in offsets_buf)
    // Free them
    for (int k = hash_trie->first(); k != HashTrieNode::OUT_OF_RANGE;
         k = hash_trie->next(k)) {
      if (!data_trie->isset(k)) {
        free_hash_subtree(*hash_trie->offset(k));
      }
    }

    // Create new hash trie mirroring data's full compressed prefix.
    // Must use the FULL compressed (not effective/skipped), because
    // compute_trie_hash hashes trie->compressed() and it must match
    // what compute_node_hash would produce for this data trie node.
    Slice prefix((const char*)data_trie->compressed(), data_trie->len());
    data_page_ptr data_page((char*)&*data_trie - sizeof(DataPageHeader));
    hash_trie_ptr new_hash_trie =
        alloc_hash_trie(prefix.size(), branch_count, data_page->txn_id);
    new_hash_trie->create(prefix, offsets_buf);

    // Compute trie hash
    compute_trie_hash(new_hash_trie);

    // Free old hash trie node and update pointer
    free_hash_node(hash_trie);
    *hash_offset_ptr = _hash_db->resolve(new_hash_trie);
  }

  // ── Deep copy operations ───────────────────────────────────────────────

  /**
   * @brief Deep copy data subtree to hash trie, computing hashes.
   */
  void deep_copy_data_to_hash(data_offset_e data_offset,
                              hash_offset_e* hash_offset_ptr) {
    if (!data_offset) {
      *hash_offset_ptr = hash_offset_e();
      return;
    }

    if (data_offset.type() == LEAF) {
      auto data_leaf = _data_db->template resolve<DataLeafNode>(&data_offset);
      size_t saved_len = _key_path.size();
      _key_path.append((const char*)data_leaf->data, data_leaf->key_size);

      hash_leaf_ptr hash_leaf = create_leaf_hash(data_leaf);
      *hash_offset_ptr = _hash_db->resolve(hash_leaf);

      _key_path.resize(saved_len);
    } else {
      auto data_trie = _data_db->template resolve<DataTrieNode>(&data_offset);
      size_t saved_len = _key_path.size();
      _key_path.append((const char*)data_trie->compressed(), data_trie->len());

      // Collect children
      hash_offset_e offsets_raw[HashTrieNode::MAX_BRANCH_COUNT] = {};
      hash_offset_e* offsets_buf = &offsets_raw[1];
      int branch_count = 0;

      for (int k = data_trie->first(); k != DataTrieNode::OUT_OF_RANGE;
           k = data_trie->next(k)) {
        hash_offset_e child{};
        deep_copy_data_to_hash(*data_trie->offset(k), &child);
        offsets_buf[k] = child;
        branch_count++;
      }

      // Create hash trie
      Slice prefix((const char*)data_trie->compressed(), data_trie->len());
      data_page_ptr data_page((char*)&*data_trie - sizeof(DataPageHeader));
      hash_trie_ptr hash_trie =
          alloc_hash_trie(prefix.size(), branch_count, data_page->txn_id);
      hash_trie->create(prefix, offsets_buf);

      // Compute trie hash
      compute_trie_hash(hash_trie);

      *hash_offset_ptr = _hash_db->resolve(hash_trie);
      _key_path.resize(saved_len);
    }
  }

  // ── Hash computation ───────────────────────────────────────────────────

  /**
   * @brief Create hash leaf from data leaf.
   */
  hash_leaf_ptr create_leaf_hash(data_leaf_ptr data_leaf) {
    // Derive branch key from data leaf's key: data[0] is the branch char,
    // or NONE if key_size == 0 (exact-match / NONE-branch leaf).
    int branch_key = (data_leaf->key_size > 0)
        ? static_cast<int>(static_cast<uint8_t>(data_leaf->data[0]))
        : HashTrieNode::NONE;

    // Get data leaf's txn_id from its page header
    data_page_ptr data_page((char*)&*data_leaf - sizeof(DataPageHeader));
    tid_t txn_id = data_page->txn_id;

    // Compute leaf hash: Blake3(full_key || value)
    Blake3Hasher hasher;
    hasher.update(_key_path.data(), _key_path.size());

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

  hash_leaf_ptr alloc_hash_leaf(tid_t txn_id, int branch_key = HashTrieNode::NONE) {
    // NONE-branch leaf: key_size=0.  Non-NONE branch: data[0]=branch_char.
    uint16_t size = static_cast<uint16_t>(sizeof(HashLeafNode) +
                                          (branch_key == HashTrieNode::NONE ? 0 : 1));
    hash_leaf_ptr leaf = _hash_db->template alloc_node<hash_leaf_ptr>(size);
    if (branch_key == HashTrieNode::NONE) {
      leaf->key_size = 0;
    } else {
      leaf->key_size = 1;
      leaf->data[0] = static_cast<uint8_t>(branch_key);
    }
    leaf->value_size = 0;
    // Copy txn_id to hash page header
    hash_page_ptr page((char*)&*leaf - sizeof(HashPageHeader));
    page->txn_id = txn_id;
    return leaf;
  }

  hash_trie_ptr alloc_hash_trie(uint8_t prefix_len, int branch_count,
                                tid_t txn_id) {
    uint16_t size = HashTrieNode::size(prefix_len, branch_count);
    hash_trie_ptr trie = _hash_db->template alloc_node<hash_trie_ptr>(size);
    // Copy txn_id to hash page header
    hash_page_ptr page((char*)&*trie - sizeof(HashPageHeader));
    page->txn_id = txn_id;
    return trie;
  }

  // ── Free helpers ───────────────────────────────────────────────────────

  template <typename NodePtr>
  void free_hash_node(NodePtr node) {
    hash_page_ptr page((char*)node - sizeof(HashPageHeader));
    _hash_db->free(page);
  }

  void free_hash_subtree(hash_offset_e offset) {
    if (!offset) return;

    if (offset.type() == LEAF) {
      auto leaf = _hash_db->template resolve<HashLeafNode>(&offset);
      free_hash_node(leaf);
    } else {
      auto trie = _hash_db->template resolve<HashTrieNode>(&offset);
      for (int k = trie->first(); k != HashTrieNode::OUT_OF_RANGE;
           k = trie->next(k)) {
        free_hash_subtree(*trie->offset(k));
      }
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
  _HashUpdater<DataDB, HashDB> updater(data_db, hash_db);
  updater.exec(data_root, hash_root_ptr);
}

#endif  // BLAKE3_API

}  // namespace leaves

#endif  // _LEAVES_HASH_UPDATER_HPP
