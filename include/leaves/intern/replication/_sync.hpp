#ifndef _LEAVES__SYNC_HPP
#define _LEAVES__SYNC_HPP

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../core/_hash.hpp"
#include "../core/_node.hpp"
#include "../core/_util.hpp"

namespace leaves {

// Result of overwrite policy
enum class OverwriteAction {
  KEEP_LOCAL,  // Reject remote value
  USE_REMOTE,  // Accept remote value
  USE_MERGED,  // Use merged_value from context
  ABORT        // Abort the entire sync
};

// Context passed to overwrite policy
struct OverwriteContext {
  Slice key;
  Slice local_value;
  Slice remote_value;
  uint64_t local_txn_id;
  uint64_t remote_txn_id;
  std::string merged_value;  // Set by policy if returning USE_MERGED
};

// Policy interface for conflict resolution
struct OverwritePolicy {
  virtual ~OverwritePolicy() = default;
  virtual OverwriteAction on_conflict(OverwriteContext& ctx) = 0;
};

// Default policy: always accept remote
struct AlwaysAcceptRemote : OverwritePolicy {
  OverwriteAction on_conflict(OverwriteContext&) override {
    return OverwriteAction::USE_REMOTE;
  }
};

// Last-write-wins based on transaction ID
struct LastWriteWins : OverwritePolicy {
  OverwriteAction on_conflict(OverwriteContext& ctx) override {
    return ctx.remote_txn_id > ctx.local_txn_id ? OverwriteAction::USE_REMOTE
                                                : OverwriteAction::KEEP_LOCAL;
  }
};

// Child hash info for tree traversal
struct ChildHash {
  uint8_t hash[HASH_SIZE];
  uint8_t child_key;       // The byte value (0-255) or 0xFF for null branch
  uint8_t compressed_len;  // Length of compressed path at this node

  static constexpr uint8_t NULL_BRANCH = 0xFF;
};

// Path in the trie (sequence of child keys taken)
using TriePath = std::vector<uint8_t>;

// Leaf data returned when reaching a leaf
struct LeafData {
  std::string key;
  std::string value;
  uint8_t hash[HASH_SIZE];
};

// Diff entry representing a change
struct DiffEntry {
  std::string key;
  std::string value;
  bool is_deletion;  // true if this key was deleted (value empty)
};

// Merkle tree sync operations
// Template parameter DB should be a database type with cursor access
template <typename DB>
struct _MerkleSync {
  using Traits = typename DB::Traits;
  using Cursor = typename DB::Cursor;
  using Transition = typename Cursor::Transition;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using offset_e = typename Traits::offset_e;

  // Get root hash of database
  static bool get_root_hash(DB* db, typename DB::txn_ptr txn,
                            uint8_t* out_hash) {
    if (!txn->root) {
      std::memset(out_hash, 0, HASH_SIZE);
      return false;
    }

    if (txn->root.type() == LEAF) {
      auto leaf = db->template resolve<LeafNode>(&txn->root);
      std::memcpy(out_hash, leaf->hash, HASH_SIZE);
    } else {
      auto trie = db->template resolve<TrieNode>(&txn->root);
      std::memcpy(out_hash, trie->hash, HASH_SIZE);
    }
    return true;
  }

  // Get child hashes at a given path in the trie
  // Returns empty vector if path leads to a leaf
  static std::vector<ChildHash> get_child_hashes(DB* db,
                                                 typename DB::txn_ptr txn,
                                                 const TriePath& path) {
    std::vector<ChildHash> result;

    if (!txn->root) return result;

    // Navigate to the node at path
    offset_e* current = &txn->root;

    for (uint8_t child_key : path) {
      if (current->type() == LEAF) {
        return result;  // Path leads past a leaf
      }

      auto trie = db->template resolve<TrieNode>(current);

      // Find the child with this key
      offset_e* child_offset = trie->offset(child_key);
      if (!child_offset) {
        return result;  // Path doesn't exist
      }
      current = child_offset;
    }

    // Now at target node - collect its children
    if (current->type() == LEAF) {
      // Leaf node has no children
      return result;
    }

    auto trie = db->template resolve<TrieNode>(current);

    // Check for null branch
    if (trie->has_none()) {
      ChildHash ch;
      offset_e* null_offset = trie->offset(TrieNode::NONE);
      if (null_offset) {
        if (null_offset->type() == LEAF) {
          auto leaf = db->template resolve<LeafNode>(null_offset);
          std::memcpy(ch.hash, leaf->hash, HASH_SIZE);
        } else {
          auto child_trie = db->template resolve<TrieNode>(null_offset);
          std::memcpy(ch.hash, child_trie->hash, HASH_SIZE);
        }
        ch.child_key = ChildHash::NULL_BRANCH;
        ch.compressed_len = 0;
        result.push_back(ch);
      }
    }

    // Iterate through all possible byte values
    int nchar = trie->next(TrieNode::NONE);
    while (nchar != TrieNode::OUT_OF_RANGE) {
      offset_e* child_offset = trie->offset(nchar);
      if (child_offset) {
        ChildHash ch;
        if (child_offset->type() == LEAF) {
          auto leaf = db->template resolve<LeafNode>(child_offset);
          std::memcpy(ch.hash, leaf->hash, HASH_SIZE);
          ch.compressed_len = leaf->key_size;
        } else {
          auto child_trie = db->template resolve<TrieNode>(child_offset);
          std::memcpy(ch.hash, child_trie->hash, HASH_SIZE);
          ch.compressed_len = child_trie->len();
        }
        ch.child_key = static_cast<uint8_t>(nchar);
        result.push_back(ch);
      }
      nchar = trie->next(nchar);
    }

    return result;
  }

  // Get leaf data at a given path
  // Returns nullopt if path doesn't lead to a leaf
  static bool get_leaf(DB* db, typename DB::txn_ptr txn, const TriePath& path,
                       LeafData& out) {
    if (!txn->root) return false;

    // Use cursor to navigate to path and get full key
    Cursor cursor(db, txn);

    // Navigate down the path
    offset_e* current = &txn->root;
    std::string key_so_far;

    for (uint8_t child_key : path) {
      if (current->type() == LEAF) {
        return false;  // Path leads past a leaf
      }

      auto trie = db->template resolve<TrieNode>(current);

      // Add compressed portion to key
      key_so_far.append(reinterpret_cast<const char*>(trie->compressed()),
                        trie->len());

      // Find the child
      if (child_key == ChildHash::NULL_BRANCH) {
        current = trie->offset(TrieNode::NONE);
      } else {
        key_so_far.push_back(static_cast<char>(child_key));
        current = trie->offset(child_key);
      }

      if (!current) return false;
    }

    // Should be at a leaf now
    if (current->type() != LEAF) {
      return false;  // Not a leaf
    }

    auto leaf = db->template resolve<LeafNode>(current);
    key_so_far.append(reinterpret_cast<const char*>(leaf->key()), leaf->key_size);

    out.key = std::move(key_so_far);
    out.value = std::string(reinterpret_cast<const char*>(leaf->value()),
                            leaf->value_size);
    std::memcpy(out.hash, leaf->hash, HASH_SIZE);
    return true;
  }

  // Compare two hashes
  static bool hashes_equal(const uint8_t* h1, const uint8_t* h2) {
    return std::memcmp(h1, h2, HASH_SIZE) == 0;
  }

  // Find differing paths between local and remote by comparing child hashes
  // Returns paths that need to be explored further
  static std::vector<TriePath> find_diff_paths(
      const std::vector<ChildHash>& local_children,
      const std::vector<ChildHash>& remote_children, const TriePath& prefix) {
    std::vector<TriePath> diff_paths;

    // Build maps for quick lookup
    std::vector<const ChildHash*> local_by_key(256, nullptr);
    std::vector<const ChildHash*> remote_by_key(256, nullptr);
    const ChildHash* local_null = nullptr;
    const ChildHash* remote_null = nullptr;

    for (const auto& ch : local_children) {
      if (ch.child_key == ChildHash::NULL_BRANCH) {
        local_null = &ch;
      } else {
        local_by_key[ch.child_key] = &ch;
      }
    }

    for (const auto& ch : remote_children) {
      if (ch.child_key == ChildHash::NULL_BRANCH) {
        remote_null = &ch;
      } else {
        remote_by_key[ch.child_key] = &ch;
      }
    }

    // Check null branch
    if (local_null || remote_null) {
      if (!local_null || !remote_null ||
          !hashes_equal(local_null->hash, remote_null->hash)) {
        TriePath path = prefix;
        path.push_back(ChildHash::NULL_BRANCH);
        diff_paths.push_back(path);
      }
    }

    // Check all byte values
    for (int i = 0; i < 256; ++i) {
      const ChildHash* local = local_by_key[i];
      const ChildHash* remote = remote_by_key[i];

      if (local || remote) {
        if (!local || !remote || !hashes_equal(local->hash, remote->hash)) {
          TriePath path = prefix;
          path.push_back(static_cast<uint8_t>(i));
          diff_paths.push_back(path);
        }
      }
    }

    return diff_paths;
  }

  // Callback types for sync operations
  using GetRemoteRootHash = std::function<bool(uint8_t* out_hash)>;
  using GetRemoteChildren =
      std::function<std::vector<ChildHash>(const TriePath& path)>;
  using GetRemoteLeaf = std::function<bool(const TriePath& path, LeafData& out)>;

  // Synchronize local database with remote
  // Returns number of entries synchronized, or -1 on error
  template <typename WriteTxn>
  static int sync_from_remote(DB* db, WriteTxn& write_txn,
                              typename DB::txn_ptr read_txn,
                              GetRemoteRootHash get_remote_root,
                              GetRemoteChildren get_remote_children,
                              GetRemoteLeaf get_remote_leaf,
                              OverwritePolicy& policy) {
    // Get root hashes
    uint8_t local_root[HASH_SIZE];
    uint8_t remote_root[HASH_SIZE];

    get_root_hash(db, read_txn, local_root);
    if (!get_remote_root(remote_root)) {
      return 0;  // Remote is empty
    }

    if (hashes_equal(local_root, remote_root)) {
      return 0;  // Already in sync
    }

    // BFS to find all differing leaves
    std::vector<TriePath> to_explore;
    to_explore.push_back({});  // Start at root

    std::vector<DiffEntry> diffs;

    while (!to_explore.empty()) {
      TriePath path = std::move(to_explore.back());
      to_explore.pop_back();

      auto local_children = get_child_hashes(db, read_txn, path);
      auto remote_children = get_remote_children(path);

      // If remote has no children at this path, it's a leaf
      if (remote_children.empty()) {
        LeafData remote_leaf;
        if (get_remote_leaf(path, remote_leaf)) {
          DiffEntry diff;
          diff.key = remote_leaf.key;
          diff.value = remote_leaf.value;
          diff.is_deletion = false;
          diffs.push_back(std::move(diff));
        }
        continue;
      }

      // Find differing paths
      auto diff_paths = find_diff_paths(local_children, remote_children, path);
      for (auto& dp : diff_paths) {
        to_explore.push_back(std::move(dp));
      }
    }

    // Apply diffs
    int count = 0;
    for (const auto& diff : diffs) {
      if (diff.is_deletion) {
        write_txn.remove(Slice(diff.key));
        ++count;
      } else {
        // Check for conflict
        std::string local_value;
        if (db->get(read_txn, Slice(diff.key), local_value)) {
          if (local_value != diff.value) {
            OverwriteContext ctx;
            ctx.key = Slice(diff.key);
            ctx.local_value = Slice(local_value);
            ctx.remote_value = Slice(diff.value);
            ctx.local_txn_id = read_txn->txn_id;
            ctx.remote_txn_id = 0;  // Unknown

            OverwriteAction action = policy.on_conflict(ctx);
            switch (action) {
              case OverwriteAction::KEEP_LOCAL:
                continue;
              case OverwriteAction::USE_REMOTE:
                write_txn.put(Slice(diff.key), Slice(diff.value));
                ++count;
                break;
              case OverwriteAction::USE_MERGED:
                write_txn.put(Slice(diff.key), Slice(ctx.merged_value));
                ++count;
                break;
              case OverwriteAction::ABORT:
                return -1;
            }
          }
        } else {
          // No local value, just insert
          write_txn.put(Slice(diff.key), Slice(diff.value));
          ++count;
        }
      }
    }

    return count;
  }
};

}  // namespace leaves

#endif  // _LEAVES__SYNC_HPP
