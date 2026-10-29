#ifndef _LEAVES_INTERNAL_SET_OPERATIONS_HPP
#define _LEAVES_INTERNAL_SET_OPERATIONS_HPP

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

namespace leaves {

// Default observer for set operations. Tests or other callers can provide an
// observer with the same hooks to count node visits, allocations, and frees.
struct _SetOperationObserver {
  // Hooks called while reading the first input, second input, validating keys,
  // and allocating or freeing output nodes; the default implementation ignores
  // them.
  void on_first() {}
  void on_second() {}
  void on_validation() {}
  void on_allocate() {}
  void on_free() noexcept {}
};

template <typename DestinationCursor, typename FirstCursor, typename SecondCursor,
          typename Observer>
struct _Intersector {
  using Traits = typename DestinationCursor::Traits;
  using offset_e = typename Traits::offset_e;
  using FirstOffset = typename FirstCursor::Traits::offset_e;
  using SecondOffset = typename SecondCursor::Traits::offset_e;
  using Transition = typename DestinationCursor::Transition;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using FirstTrieNode = typename FirstCursor::Transition::TrieNode;
  using FirstLeafNode = typename FirstCursor::Transition::LeafNode;
  using SecondTrieNode = typename SecondCursor::Transition::TrieNode;
  using SecondLeafNode = typename SecondCursor::Transition::LeafNode;

  DestinationCursor& _destination;
  FirstCursor& _first;
  SecondCursor& _second;
  size_t _key_size;
  Observer& _observer;
  std::array<offset_e, 257> _output_offsets{};

  // Owns one newly allocated destination subtree until release() transfers
  // ownership to a parent node or the destination root.
  struct _OwnedNode {
    _Intersector* _owner = nullptr;
    offset_e _offset{};

    _OwnedNode() = default;
    // Construct an owner for `offset`; `owner` performs cleanup on destruction.
    _OwnedNode(_Intersector& owner, const offset_e& offset)
        : _owner(&owner), _offset(offset) {}
    _OwnedNode(const _OwnedNode&) = delete;
    _OwnedNode& operator=(const _OwnedNode&) = delete;
    // Move ownership from `other`, leaving it empty.
    _OwnedNode(_OwnedNode&& other) noexcept
        : _owner(other._owner), _offset(other.release()) {}
    // Free the current subtree, then take ownership from `other`.
    _OwnedNode& operator=(_OwnedNode&& other) noexcept {
      if (this != &other) {
        // Discard anything this wrapper currently owns before taking over.
        reset();
        _owner = other._owner;
        _offset = other.release();
      }
      return *this;
    }
    // Free the owned subtree unless ownership was released or moved away.
    ~_OwnedNode() { reset(); }

    // Return the stored offset and relinquish ownership without freeing it.
    offset_e release() noexcept {
      auto result = _offset;
      // A zero offset marks this wrapper as no longer owning the node.
      _offset = offset_e();
      return result;
    }
    // Free the owned subtree, if any, and leave this wrapper empty.
    void reset() noexcept {
      // Destruction of an untransferred result must reclaim the whole subtree.
      if (_offset != 0) _owner->_free_subtree(release());
    }
  };

  // Accumulates only matching child subtrees while a destination trie node is
  // being built. Its destructor cleans up children not transferred to a trie.
  struct _Children {
    _Intersector& _owner;
    std::array<uint64_t, 257> _offsets;
    std::array<uint32_t, 8> _bitmap{};
    offset_e _none{};
    uint16_t _count = 0;

    // Associate this temporary child collection with the intersector used to
    // allocate and free its child subtrees.
    explicit _Children(_Intersector& owner) : _owner(owner) {}
    _Children(const _Children&) = delete;
    _Children& operator=(const _Children&) = delete;
    // Free all child subtrees that were not transferred to a trie node.
    ~_Children() {
      for_each_child([&](int, const offset_e& offset) { _owner._free_subtree(offset); });
    }

    // Add a matched child under `key`, transferring ownership from `node`.
    // `key == -1` represents a key ending at this trie node.
    void add(int key, _OwnedNode node) noexcept {
      if (node._offset == 0) return;
      // Transfer ownership from the temporary result into this child list.
      if (key < 0) _none = node.release();
      else {
        _offsets[key + 1] = node.release().raw();
        _bitmap[key >> 5] |= uint32_t(1) << (key & 31);
      }
      ++_count;
    }

    // Invoke `function(key, offset)` for each child currently held here.
    template <typename Function>
    void for_each_child(Function&& function) const {
      if (_none != 0) function(-1, _none);
      for (size_t group = 0; group < _bitmap.size(); ++group) {
        uint32_t remaining = _bitmap[group];
        while (remaining) {
          const int key = static_cast<int>(group * 32 + bits::first(remaining));
          remaining &= remaining - 1;
          function(key, offset_e(_offsets[key + 1]));
        }
      }
    }

    // Relinquish all child ownership after a parent trie takes over the offsets.
    void release() noexcept {
      // The offsets are now owned by the newly-created trie node.
      _none = offset_e();
      _bitmap.fill(0);
      _count = 0;
    }
  };

  struct _PathBuffer {
    std::array<char, 255> _inline;
    std::string _overflow;
    size_t _size = 0;

    void append(const char* bytes, size_t size) {
      if (_overflow.empty() && size <= _inline.size() - _size) {
        if (size) std::memcpy(_inline.data() + _size, bytes, size);
      } else {
        if (_overflow.empty()) {
          _overflow.reserve(_size + size);
          _overflow.append(_inline.data(), _size);
        }
        _overflow.append(bytes, size);
      }
      _size += size;
    }

    const char* data() const {
      return _overflow.empty() ? _inline.data() : _overflow.data();
    }
    size_t size() const { return _size; }
  };

  // Bind the destination, both read-only sources, matching prefix length, and
  // observer used by this intersection operation.
  _Intersector(DestinationCursor& destination, FirstCursor& first,
               SecondCursor& second, size_t key_size, Observer& observer)
      : _destination(destination), _first(first), _second(second),
        _key_size(key_size), _observer(observer) {}

  template <typename Cursor>
  Slice _node_prefix(Cursor& cursor,
                     const typename Cursor::Traits::offset_e& offset) {
    if (offset.type() == LEAF) {
      auto leaf = cursor._db->template resolve<
          typename Cursor::Transition::LeafNode>(&offset);
      return Slice(leaf->data, leaf->key_size);
    }
    auto trie = cursor._db->template resolve<
        typename Cursor::Transition::TrieNode>(&offset);
    return Slice(trie->prefix(), trie->prefix_len());
  }

  template <typename Cursor>
  typename Cursor::Traits::offset_e _child(
      Cursor& cursor, const typename Cursor::Traits::offset_e& offset,
      int key) {
    auto trie = cursor._db->template resolve<
        typename Cursor::Transition::TrieNode>(&offset);
    auto child = trie->branch_offset(key);
    return child ? *child : typename Cursor::Traits::offset_e();
  }

  // Check whether every key below `offset` in `cursor` has at least
  // `key_size` bytes. `depth` is the number of bytes already consumed.
  template <typename Cursor>
  bool valid_keys(Cursor& cursor, const typename Cursor::Traits::offset_e& offset,
                  size_t depth = 0) {
    // Empty subtrees and paths already at the requested prefix length need no
    // further validation.
    if (offset == 0 || depth >= _key_size) return true;
    _observer.on_validation();
    if (offset.type() == LEAF) {
      auto leaf = cursor._db->template resolve<
          typename Cursor::Transition::LeafNode>(&offset);
      return depth + leaf->key_size >= _key_size;
    }
    auto trie = cursor._db->template resolve<
        typename Cursor::Transition::TrieNode>(&offset);
    depth += trie->prefix_len();
    if (depth >= _key_size) return true;
    // Before reaching key_size, a leaf or end-of-key branch means a key is too
    // short to participate in prefix-based matching.
    if (trie->has_none()) return false;
    bool valid = true;
    trie->for_each_branch([&](int, const auto* child) {
      if (valid && *child != 0) valid = valid_keys(cursor, *child, depth);
    });
    return valid;
  }

  // Intersect the two source roots and install the newly allocated result.
  void exec() {
    // Compare the two roots and transfer the resulting owned subtree to the
    // (required to be empty) destination root.
    auto result = _match(*_first._root, 0, *_second._root, 0, 0);
    *_destination._root = result.release();
  }

  // Recursively free the destination subtree rooted at `offset`.
  void _free_subtree(const offset_e& offset) noexcept {
    using PageHeader = typename Traits::PageHeader;
    if (offset.type() == LEAF) {
      // Leaves have no children, so their allocation can be freed directly.
      auto node = _destination._db->template resolve<LeafNode>(&offset);
      _destination._db->free(node - sizeof(PageHeader));
    } else {
      // Save child offsets before freeing the parent node that stores them.
      std::array<uint64_t, 257> children;
      size_t count = 0;
      auto node = _destination._db->template resolve<TrieNode>(&offset);
      node->for_each_branch([&](int, const auto* child) {
        if (*child != 0) children[count++] = child->raw();
      });
      _destination._db->free(node - sizeof(PageHeader));
      // Recursively release every descendant after the parent is gone.
      for (size_t index = 0; index < count; ++index)
        _free_subtree(offset_e(children[index]));
    }
    _observer.on_free();
  }

  // Allocate and return an owned destination leaf for the complete key in
  // `prefix`; the leaf's value is empty.
  _OwnedNode _leaf(const Slice& prefix) {
    // Allocate an output leaf containing a key but no value: intersection is
    // about key membership, and values from either input are not copied.
    _observer.on_allocate();
    const auto size = LeafNode::size(prefix.size(), 0);
    auto leaf = _destination._db->template alloc_node<typename Transition::leaf_ptr>(size);
    std::memset(static_cast<char*>(leaf), 0, LeafNode::HEADER_SIZE);
    leaf->set(prefix, 0);
    offset_e offset;
    offset = _destination._db->resolve(leaf);
    return _OwnedNode(*this, offset);
  }

  // Build an owned destination trie with compressed `prefix` and the matched
  // children collected in `children`; return empty if there are no children.
  _OwnedNode _trie(const Slice& prefix, _Children& children) {
    // An empty set of matching children means this branch contributes nothing.
    if (!children._count) return {};
    // The trie format has an 8-bit mask selecting which of the eight 32-bit
    // branch-bitmap groups are present.
    uint8_t groups = 0;
    for (size_t group = 0; group < children._bitmap.size(); ++group)
      if (children._bitmap[group]) groups |= uint8_t(1u << group);
    _observer.on_allocate();
    const auto size = TrieNode::size(prefix.size(), children._count);
    auto trie = _destination._db->template alloc_node<typename Transition::trie_ptr>(size);
    std::memset(static_cast<char*>(trie), 0, TrieNode::HEADER_SIZE);
    // Arrange child offsets in the database node's expected key-indexed layout.
    _output_offsets[0] = children._none;
    children.for_each_child([&](int key, const offset_e& offset) {
      _output_offsets[key + 1] = offset;
    });
    trie->create(prefix, _output_offsets.data() + 1,
                 children._bitmap.data(), groups);
    offset_e offset;
    offset = _destination._db->resolve(trie);
    // The new trie now owns these children; prevent _Children from freeing them.
    children.release();
    return _OwnedNode(*this, offset);
  }

  // Store a complete key, splitting it into trie-sized compressed chunks.
  _OwnedNode _leaf_key(const Slice& key) {
    if (key.size() <= 255) return _leaf(key);
    size_t end = ((key.size() - 1) / 255) * 255;
    auto result = _leaf(Slice(key.data() + end, key.size() - end));
    while (end) {
      const size_t start = end > 255 ? end - 255 : 0;
      _Children children(*this);
      children.add(static_cast<uint8_t>(key[end]), std::move(result));
      result = _trie(Slice(key.data() + start, end - start), children);
      end = start;
    }
    return result;
  }

  // Build the aligned result node with its full common prefix. Prefixes longer
  // than a trie node's limit are split at child-prefix boundaries.
  _OwnedNode _trie_with_prefix(const Slice& prefix, _Children& children) {
    if (prefix.size() <= 255)
      return _trie(Slice(prefix.data(), prefix.size()), children);
    size_t end = ((prefix.size() - 1) / 255) * 255;
    auto result = _trie(Slice(prefix.data() + end, prefix.size() - end), children);
    while (end) {
      const size_t start = end > 255 ? end - 255 : 0;
      _Children parent(*this);
      parent.add(static_cast<uint8_t>(prefix[end]), std::move(result));
      result = _trie(Slice(prefix.data() + start, end - start), parent);
      end = start;
    }
    return result;
  }

  // Deep-copy the subtree at `offset` from the first source into destination
  // storage, returning empty for an absent source node.
  _OwnedNode _clone(const FirstOffset& offset,
                    _PathBuffer* leading_prefix = nullptr) {
    const FirstOffset stable_offset = offset;
    if (stable_offset == 0) return {};
    _observer.on_first();
    // Clone a whole qualifying subtree from the first input into destination
    // storage; never reuse source offsets in a different database.
    if (stable_offset.type() == LEAF) {
      auto leaf = _first._db->template resolve<FirstLeafNode>(&stable_offset);
      if (leading_prefix) {
        leading_prefix->append(reinterpret_cast<const char*>(leaf->data),
                               leaf->key_size);
        return _leaf_key(Slice(leading_prefix->data(), leading_prefix->size()));
      }
      return _leaf(Slice(leaf->data, leaf->key_size));
    }
    auto trie = _first._db->template resolve<FirstTrieNode>(&stable_offset);
    _Children children(*this);
    trie->for_each_branch([&](int key, const auto* child) {
      if (*child != 0) children.add(key, _clone(*child));
    });
    const auto node_prefix = Slice(trie->prefix(), trie->prefix_len());
    if (!leading_prefix) return _trie(node_prefix, children);
    leading_prefix->append(node_prefix.data(), node_prefix.size());
    return _trie_with_prefix(Slice(leading_prefix->data(), leading_prefix->size()),
                             children);
  }

  // Match source nodes at their current prefix positions. `first_pos` and
  // `second_pos` are offsets within compressed prefixes; `depth` counts the
  // key bytes already matched. Return an owned result subtree or empty.
  _OwnedNode _match(const FirstOffset& first_offset, size_t first_pos,
                    const SecondOffset& second_offset, size_t second_pos,
                    size_t depth, _PathBuffer* first_path = nullptr) {
    const FirstOffset stable_first_offset = first_offset;
    const SecondOffset stable_second_offset = second_offset;
    // A missing side cannot contribute a key to the intersection.
    if (stable_first_offset == 0 || stable_second_offset == 0) return {};
    if (stable_first_offset.type() == LEAF && stable_second_offset.type() == LEAF)
      return _match_leaves(stable_first_offset, first_pos, stable_second_offset,
                           second_pos, depth, first_path);
    _observer.on_first();
    const Slice first_prefix = _node_prefix(_first, stable_first_offset);
    _observer.on_second();
    const Slice second_prefix = _node_prefix(_second, stable_second_offset);
    // Compare as many compressed-prefix bytes as both nodes have in common,
    // stopping at key_size when prefix matching was requested.
    size_t length = std::min(first_prefix.size() - first_pos,
                             second_prefix.size() - second_pos);
    if (_key_size) length = std::min(length, _key_size - depth);
    if (length && std::memcmp(first_prefix.data() + first_pos,
                              second_prefix.data() + second_pos, length))
      return {};
    first_pos += length;
    second_pos += length;
    depth += length;
    // Every first-input key below this point shares the requested prefix, so
    // copy that subtree without comparing its remaining branches.
    if (_key_size && depth == _key_size)
      return _clone(stable_first_offset, first_path);

    if (first_pos < first_prefix.size()) {
      // The first node still has compressed bytes to consume. The second
      // trie must have a branch for the next byte; a leaf cannot continue.
      if (stable_second_offset.type() == LEAF) return {};
      const auto key = static_cast<uint8_t>(first_prefix[first_pos]);
      return _match(stable_first_offset, first_pos,
        _child(_second, stable_second_offset, key), 0, depth, first_path);
    }

    if (second_pos < second_prefix.size()) {
      // The second node still has compressed bytes, so its next byte selects
      // the only first-side branch that can match. Defer output construction
      // until the prefixes align and the matching children are known.
      if (stable_first_offset.type() == LEAF) return {};
      const auto key = static_cast<uint8_t>(second_prefix[second_pos]);
      if (first_path) {
        first_path->append(first_prefix.data(), first_prefix.size());
        return _match(_child(_first, stable_first_offset, key), 0,
                      stable_second_offset, second_pos, depth, first_path);
      }
      _PathBuffer path;
      path.append(first_prefix.data(), first_prefix.size());
      return _match(_child(_first, stable_first_offset, key), 0,
                    stable_second_offset, second_pos, depth, &path);
    }

    // Both prefixes are aligned: recurse only through their common branches.
    return _match_equal_prefixes(stable_first_offset, first_pos,
                                 stable_second_offset, second_pos, depth,
                                 first_path);
  }

  // Both node prefixes are fully consumed; only shared branches can
  // contribute keys to the intersection.
  _OwnedNode _match_equal_prefixes(const FirstOffset& first_offset,
                                   size_t first_pos,
                                   const SecondOffset& second_offset,
                                   size_t second_pos, size_t depth,
                                   _PathBuffer* first_path = nullptr) {
    const FirstOffset stable_first_offset = first_offset;
    const SecondOffset stable_second_offset = second_offset;
    const Slice first_prefix = _node_prefix(_first, stable_first_offset);
    const bool first_leaf = stable_first_offset.type() == LEAF;
    const bool second_leaf = stable_second_offset.type() == LEAF;
    if (first_leaf) {
      if (second_leaf) {
        if (!first_path) return _leaf(first_prefix);
        first_path->append(first_prefix.data(), first_prefix.size());
        return _leaf_key(Slice(first_path->data(), first_path->size()));
      }
      return _match(first_offset, first_pos,
                    _child(_second, stable_second_offset, -1), 0, depth,
                    first_path);
    }

    _Children children(*this);
    if (second_leaf) {
      // The second key ends here: only the first trie’s end branch can match.
      children.add(-1,
                   _match(_child(_first, stable_first_offset, -1), 0,
                          stable_second_offset, second_pos, depth));
    } else {
      auto first_trie =
          _first._db->template resolve<FirstTrieNode>(&stable_first_offset);
      auto second_trie =
          _second._db->template resolve<SecondTrieNode>(&stable_second_offset);
      first_trie->for_each_branch([&](int key, const auto* first_child) {
        if (*first_child == 0) return;
        auto second_child = second_trie->branch_offset(key);
        if (second_child && *second_child != 0)
          children.add(key, _match(*first_child, 0, *second_child, 0, depth));
      });
    }
    if (!first_path) return _trie(first_prefix, children);
    first_path->append(first_prefix.data(), first_prefix.size());
    return _trie_with_prefix(Slice(first_path->data(), first_path->size()),
                             children);
  }

  // Compare two leaf keys from their current positions and return a copied
  // first-input key when it satisfies exact or `key_size`-prefix matching.
  _OwnedNode _match_leaves(const FirstOffset& first_offset, size_t first_pos,
                           const SecondOffset& second_offset, size_t second_pos,
                           size_t depth, _PathBuffer* first_path) {
    // Compare the unconsumed portions of two complete keys. With key_size == 0
    // the complete keys must be equal; otherwise only the requested prefix
    // must match (valid_keys has already checked key lengths).
    _observer.on_first();
    auto first = _first._db->template resolve<FirstLeafNode>(&first_offset);
    _observer.on_second();
    auto second = _second._db->template resolve<SecondLeafNode>(&second_offset);
    const size_t first_length = first->key_size - first_pos;
    const size_t second_length = second->key_size - second_pos;
    const size_t length = _key_size ? _key_size - depth : first_length;
    if (first_length < length || second_length < length ||
        (!_key_size && first_length != second_length) ||
        (length && std::memcmp(first->data + first_pos, second->data + second_pos, length)))
      return {};
    // Keep the first input's complete key in the result, even when matching
    // was based only on its prefix.
    if (first_path) {
      first_path->append(reinterpret_cast<const char*>(first->data),
                         first->key_size);
      return _leaf_key(Slice(first_path->data(), first_path->size()));
    }
    return _leaf(Slice(first->data, first->key_size));
  }
};

struct _SetOperations {
  // Require a cursor backed by the ordinary database implementation.
  template <typename Cursor>
  static void _check_cursor() {
    // Set operations rely on the ordinary DB cursor's transaction and root
    // layout, rather than a specialized cursor implementation.
    static_assert(std::is_same_v<typename Cursor::DBImpl,
                                _DB<typename Cursor::StorageImpl>>,
                  "Set operations require ordinary DB cursors.");
  }

  // Validate cursor/transaction requirements, optionally validate minimum key
  // lengths, then build the intersection in the empty destination.
  // Return false when the cursors or database state are unsupported.
  template <typename DestinationCursor, typename FirstCursor, typename SecondCursor,
            typename Observer>
  static bool intersect(const DestinationCursor& destination_cursor,
                        const FirstCursor& first_cursor,
                        const SecondCursor& second_cursor, size_t key_size,
                        Observer& observer) {
    // Reject unsupported cursor types at compile time, then invalid or
    // uninitialized cursor instances at runtime.
    _check_cursor<DestinationCursor>();
    _check_cursor<FirstCursor>();
    _check_cursor<SecondCursor>();
    if (!destination_cursor._cursor || !first_cursor._cursor || !second_cursor._cursor)
      return false;
    auto& destination = *destination_cursor._cursor;
    auto& first = *first_cursor._cursor;
    auto& second = *second_cursor._cursor;
    // The destination must be a separate, empty database in an active
    // non-WAL transaction. Sources must be stable (no active transactions).
    if (!destination.is_transaction_active() || destination._use_wal ||
        first.is_transaction_active() || second.is_transaction_active() ||
        static_cast<const void*>(destination._db) == static_cast<const void*>(first._db) ||
        static_cast<const void*>(destination._db) == static_cast<const void*>(second._db) ||
        *destination._root != 0 ||
        destination._db->_header->prepared_txn != destination._db->_header->read_txn)
      return false;
    using Dst = typename DestinationCursor::cursor_ptr::element_type;
    using First = typename FirstCursor::cursor_ptr::element_type;
    using Second = typename SecondCursor::cursor_ptr::element_type;
    _Intersector<Dst, First, Second, Observer> operation(
        destination, first, second, key_size, observer);
    // Prefix matching needs all source keys to be at least key_size bytes.
    if (key_size && (!operation.valid_keys(first, *first._root) ||
                     !operation.valid_keys(second, *second._root)))
      return false;
    // The destination cursor may have stale traversal state from earlier use;
    // clear it on every exit after validation, including if execution throws.
    struct _Reset {
      Dst& _cursor;
      ~_Reset() {
        _cursor.stack.clear();
        _cursor.rest_key.reset();
        _cursor.current_key.clear();
      }
    } reset{destination};
    // Build and install the result; an empty intersection leaves a zero root.
    operation.exec();
    return true;
  }
};

}

#endif