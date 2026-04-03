#ifndef _LEAVES_INTERN_FILTER_RANGE_FILTER_HPP
#define _LEAVES_INTERN_FILTER_RANGE_FILTER_HPP

#include <string>
#include <thread>

#include "../core/_node.hpp"
#include "../core/_util.hpp"
#include "../db/_cursor.hpp"
#include "../db/_tmpdb.hpp"
#include "../util/_mpsc_queue.hpp"
#include "../util/_task_group.hpp"

namespace leaves {

// Boundary specification for a range scan.
struct _RangeBound {
    const char* data;
    size_t size;
    bool inclusive;

    _RangeBound() : data(nullptr), size(0), inclusive(true) {}
    _RangeBound(const char* d, size_t s, bool incl)
        : data(d), size(s), inclusive(incl) {}
    _RangeBound(Slice s, bool incl = true)
        : data(s.data()), size(s.size()), inclusive(incl) {}

    bool empty() const { return data == nullptr; }
};

// Range filter that walks an index trie, prunes branches outside
// [lower, upper], and collects matching document keys into a TmpDB.
//
// Template parameters:
//   DB       — the index database type (e.g., _DB<FileTraits>)
//   Executor — _InlineExecutor or _PoolExecutor
//
// The index trie maps encoded_value_key → document_key.
// The filter walks the trie, comparing accumulated prefixes against
// the range bounds. Branches whose byte values fall entirely outside
// the range are pruned. Matching leaf values (document keys) are
// pushed to a lock-free MPSC queue and a background consumer thread
// inserts them into the result TmpDB.
template <typename DB, typename Executor>
struct _RangeFilter {
    using Traits = typename DB::Traits;
    using StorageType = typename DB::Storage;
    using TrieNode = _TrieNode<Traits>;
    using LeafNode = _LeafNode<Traits>;
    using page_ptr = typename Traits::ptr;
    using offset_e = typename Traits::offset_e;
    template <typename T, NodeTypes type = TRIE>
    using Pointer = typename Traits::template Pointer<T, type>;
    using trie_ptr = Pointer<TrieNode>;
    using leaf_ptr = Pointer<LeafNode, LEAF>;

    DB* _db;
    _TaskGroup<Executor>* _tg;
    _MPSCQueue _queue;
    _TmpDB<StorageType> _result;

    _RangeBound _lower;
    _RangeBound _upper;

    _RangeFilter(DB* db, _TaskGroup<Executor>* tg)
        : _db(db), _tg(tg), _result(db->_storage) {}

    // Run the range scan on the given root offset.
    // Returns a cursor over the result TmpDB.
    typename _TmpDB<StorageType>::Cursor scan(offset_e* root, _RangeBound lower, _RangeBound upper) {
        _lower = lower;
        _upper = upper;

        if (!*root) return _result.cursor();

        // Start consumer thread
        std::thread consumer([this]() { _consume(); });

        // Walk the trie
        std::string key_path;
        _scan_node(root, key_path, !_lower.empty(), !_upper.empty());

        if constexpr (!std::is_same_v<Executor, _InlineExecutor>) {
            _tg->wait();
        }

        // Signal done and wait for consumer
        _queue.finish();
        consumer.join();

        return _result.cursor();
    }

private:
    // Consumer thread: pops doc keys from the queue and inserts into TmpDB.
    void _consume() {
        const char* data;
        size_t len;
        auto cursor = _result.cursor();

        while (true) {
            if (_queue.pop(data, len)) {
                Slice doc_key(data, len);
                cursor.find(doc_key);
                cursor.value(Slice());
            } else if (_queue.is_finished()) {
                break;
            } else {
                // Spin briefly — in practice items arrive quickly
                std::this_thread::yield();
            }
        }
    }

    // Dispatch to trie or leaf handler based on offset type.
    void _scan_node(offset_e* offset_ptr, std::string& key_path,
                    bool cl, bool cu) {
        offset_e offset = *offset_ptr;
        if (!offset) return;

        if (offset.type() == TRIE) {
            auto trie = _db->template resolve<TrieNode>(offset_ptr);
            _scan_trie(trie, offset_ptr, key_path, cl, cu);
        } else {
            auto leaf = _db->template resolve<LeafNode>(offset_ptr);
            _scan_leaf(leaf, key_path, cl, cu);
        }
    }

    // Handle a leaf node: check if the full key is within bounds.
    void _scan_leaf(leaf_ptr leaf, std::string& key_path,
                    bool cl, bool cu) {
        Slice leaf_key = leaf->key();

        // Build full key: key_path + leaf's remaining key
        // Compare against bounds
        if (cl) {
            int r = _cmp_with_bound(key_path, leaf_key, _lower);
            if (r < 0) return;  // full key < lower
            if (r == 0 && !_lower.inclusive) return;
        }
        if (cu) {
            int r = _cmp_with_bound(key_path, leaf_key, _upper);
            if (r > 0) return;  // full key > upper
            if (r == 0 && !_upper.inclusive) return;
        }

        // Key is in range — push the leaf's value (doc key) to the queue
        Slice doc_key = leaf->value();
        _queue.push(doc_key.data(), doc_key.size());
    }

    // Compare (key_path + leaf_key) against bound, lexicographically.
    // Returns < 0 if full_key < bound, 0 if equal, > 0 if full_key > bound.
    int _cmp_with_bound(const std::string& key_path, Slice leaf_key,
                        const _RangeBound& bound) {
        size_t depth = key_path.size();
        size_t full_len = depth + leaf_key.size();
        size_t bound_len = bound.size;
        size_t min_len = std::min(full_len, bound_len);

        // Compare byte by byte. key_path covers [0, depth), leaf_key covers
        // [depth, full_len). Bound covers [0, bound_len).
        // Since key_path was already matched to the bound up to depth (when
        // cl/cu are true), we can start comparison from depth.
        for (size_t i = depth; i < min_len; ++i) {
            uint8_t kb = (uint8_t)leaf_key.data()[i - depth];
            uint8_t bb = (uint8_t)bound.data[i];
            if (kb < bb) return -1;
            if (kb > bb) return 1;
        }
        if (full_len < bound_len) return -1;
        if (full_len > bound_len) return 1;
        return 0;
    }

    // Handle a trie node: compare compressed prefix, prune branches.
    void _scan_trie(trie_ptr trie, offset_e* offset_ptr,
                    std::string& key_path, bool cl, bool cu) {
        // Compare compressed prefix against bounds
        const uint8_t* prefix = trie->compressed();
        uint16_t prefix_len = trie->len();
        size_t depth = key_path.size();

        for (uint16_t i = 0; i < prefix_len; ++i) {
            uint8_t byte = prefix[i];
            size_t pos = depth + i;

            if (cl) {
                if (pos < _lower.size) {
                    uint8_t lb = (uint8_t)_lower.data[pos];
                    if (byte > lb) cl = false;       // past lower
                    else if (byte < lb) return;       // below lower
                    // byte == lb: still constrained
                } else {
                    // pos >= lower.size: key extends beyond lower → past lower
                    cl = false;
                }
            }
            if (cu) {
                if (pos < _upper.size) {
                    uint8_t ub = (uint8_t)_upper.data[pos];
                    if (byte < ub) cu = false;       // before upper
                    else if (byte > ub) return;       // above upper
                    // byte == ub: still constrained
                } else {
                    // pos >= upper.size: key extends beyond upper → above upper
                    return;
                }
            }
        }

        key_path.append((const char*)prefix, prefix_len);
        size_t branch_depth = key_path.size();

        // Determine valid branch range
        int first_branch = TrieNode::NONE;
        int last_branch = 256;  // sentinel past max byte

        if (cl && branch_depth < _lower.size) {
            first_branch = (uint8_t)_lower.data[branch_depth];
        }
        if (cu && branch_depth < _upper.size) {
            last_branch = (uint8_t)_upper.data[branch_depth];
        }

        // Handle NONE branch: no branch byte added, route through _scan_node
        // so both trie and leaf children are handled correctly.
        if (trie->has_none()) {
            offset_e* none_off = trie->offset(TrieNode::NONE);
            // NONE doesn't add a byte. Propagate cl/cu unchanged.
            _scan_node(none_off, key_path, cl, cu);
        }

        // When cu && branch_depth >= upper.size: the prefix already matches
        // upper exactly and any branch byte would make the key > upper.
        // Only the NONE branch (handled above) could be valid. Skip all
        // regular branches.
        bool skip_regular = cu && branch_depth >= _upper.size;

        if (!skip_regular) {
            // Iterate branches in the valid range
            trie->for_each_branch([&](int k, offset_e* child_off) {
                if (k == TrieNode::NONE) return;  // handled above

                // Branch pruning
                if (cl && branch_depth < _lower.size && k < first_branch)
                    return;
                if (cu && branch_depth < _upper.size && k > last_branch)
                    return;

                // Determine child constraint state
                bool child_cl =
                    cl && (branch_depth < _lower.size) && (k == first_branch);
                bool child_cu =
                    cu && (branch_depth < _upper.size) && (k == last_branch);

                auto do_branch = [this, child_off, child_cl,
                                  child_cu](std::string& kp) {
                    _scan_node(child_off, kp, child_cl, child_cu);
                };

                if constexpr (std::is_same_v<Executor, _InlineExecutor>) {
                    do_branch(key_path);
                }
#if LEAVES_HAS_THREADS
                else if (_in_worker) {
                    do_branch(key_path);
                } else {
                    _tg->spawn([do_branch, key = key_path]() mutable {
                        do_branch(key);
                    });
                }
#endif
            });

            if constexpr (!std::is_same_v<Executor, _InlineExecutor>) {
                _tg->wait();
            }
        }

        key_path.resize(depth);
    }
};

}  // namespace leaves

#endif  // _LEAVES_INTERN_FILTER_RANGE_FILTER_HPP
