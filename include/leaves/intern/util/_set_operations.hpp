#ifndef _LEAVES_INTERNAL_SET_OPERATIONS_HPP
#define _LEAVES_INTERNAL_SET_OPERATIONS_HPP

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "../db/_cursor.hpp"

namespace leaves {

struct _SetOperationObserver {
  void on_first() {}
  void on_second() {}
  void on_validation() {}
  void on_allocate() {}
  void on_free() noexcept {}
};

template <typename Cursor>
struct _SetNodeView {
  using offset_e = typename Cursor::Traits::offset_e;
  using LeafNode = typename Cursor::Transition::LeafNode;
  using TrieNode = typename Cursor::Transition::TrieNode;

  bool _leaf;
  std::string _prefix;
  std::array<uint64_t, 257> _children;
  std::array<uint32_t, 8> _bitmap{};
  bool _has_none = false;

  _SetNodeView(typename Cursor::DB* db, const offset_e& offset)
      : _leaf(offset.type() == LEAF) {
    if (_leaf) {
      auto leaf = db->template resolve<LeafNode>(&offset);
      _prefix.assign(reinterpret_cast<const char*>(leaf->data), leaf->key_size);
    } else {
      auto trie = db->template resolve<TrieNode>(&offset);
      _prefix.assign(reinterpret_cast<const char*>(trie->prefix()),
                     trie->prefix_len());
      trie->for_each_branch([&](int key, const auto* child) {
        if (*child == 0) return;
        _children[key + 1] = child->raw();
        if (key < 0) _has_none = true;
        else _bitmap[key >> 5] |= uint32_t(1) << (key & 31);
      });
    }
  }

  offset_e child(int key) const {
    const bool present = key < 0 ? _has_none
        : (_bitmap[key >> 5] & (uint32_t(1) << (key & 31))) != 0;
    return present ? offset_e(_children[key + 1]) : offset_e();
  }

  template <typename Function>
  void for_each_child(Function&& function) const {
    if (_has_none) function(-1, child(-1));
    for (size_t group = 0; group < _bitmap.size(); ++group) {
      uint32_t remaining = _bitmap[group];
      while (remaining) {
        const int key = static_cast<int>(group * 32 + bits::first(remaining));
        remaining &= remaining - 1;
        function(key, offset_e(_children[key + 1]));
      }
    }
  }
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
  using FirstView = _SetNodeView<FirstCursor>;
  using SecondView = _SetNodeView<SecondCursor>;

  DestinationCursor& _destination;
  FirstCursor& _first;
  SecondCursor& _second;
  size_t _key_size;
  Observer& _observer;
  std::array<offset_e, 257> _output_offsets{};

  struct _OwnedNode {
    _Intersector* _owner = nullptr;
    offset_e _offset{};

    _OwnedNode() = default;
    _OwnedNode(_Intersector& owner, const offset_e& offset)
        : _owner(&owner), _offset(offset) {}
    _OwnedNode(const _OwnedNode&) = delete;
    _OwnedNode& operator=(const _OwnedNode&) = delete;
    _OwnedNode(_OwnedNode&& other) noexcept
        : _owner(other._owner), _offset(other.release()) {}
    _OwnedNode& operator=(_OwnedNode&& other) noexcept {
      if (this != &other) {
        reset();
        _owner = other._owner;
        _offset = other.release();
      }
      return *this;
    }
    ~_OwnedNode() { reset(); }

    offset_e release() noexcept {
      auto result = _offset;
      _offset = offset_e();
      return result;
    }
    void reset() noexcept {
      if (_offset != 0) _owner->_free_subtree(release());
    }
  };

  struct _Children {
    _Intersector& _owner;
    std::array<uint64_t, 257> _offsets;
    std::array<uint32_t, 8> _bitmap{};
    offset_e _none{};
    uint16_t _count = 0;

    explicit _Children(_Intersector& owner) : _owner(owner) {}
    _Children(const _Children&) = delete;
    _Children& operator=(const _Children&) = delete;
    ~_Children() {
      for_each_child([&](int, const offset_e& offset) { _owner._free_subtree(offset); });
    }

    void add(int key, _OwnedNode node) noexcept {
      if (node._offset == 0) return;
      if (key < 0) _none = node.release();
      else {
        _offsets[key + 1] = node.release().raw();
        _bitmap[key >> 5] |= uint32_t(1) << (key & 31);
      }
      ++_count;
    }

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

    void release() noexcept {
      _none = offset_e();
      _bitmap.fill(0);
      _count = 0;
    }
  };

  _Intersector(DestinationCursor& destination, FirstCursor& first,
               SecondCursor& second, size_t key_size, Observer& observer)
      : _destination(destination), _first(first), _second(second),
        _key_size(key_size), _observer(observer) {}

  template <typename Cursor>
  bool valid_keys(Cursor& cursor, const typename Cursor::Traits::offset_e& offset,
                  size_t depth = 0) {
    if (offset == 0 || depth >= _key_size) return true;
    _observer.on_validation();
    _SetNodeView<Cursor> node(cursor._db, offset);
    depth += node._prefix.size();
    if (depth >= _key_size) return true;
    if (node._leaf || node._has_none) return false;
    bool valid = true;
    node.for_each_child([&](int, const auto& child) {
      if (valid) valid = valid_keys(cursor, child, depth);
    });
    return valid;
  }

  void exec() {
    auto result = _match(*_first._root, 0, *_second._root, 0, 0);
    *_destination._root = result.release();
  }

  void _free_subtree(const offset_e& offset) noexcept {
    using PageHeader = typename Traits::PageHeader;
    if (offset.type() == LEAF) {
      auto node = _destination._db->template resolve<LeafNode>(&offset);
      _destination._db->free(node - sizeof(PageHeader));
    } else {
      std::array<uint64_t, 257> children;
      size_t count = 0;
      auto node = _destination._db->template resolve<TrieNode>(&offset);
      node->for_each_branch([&](int, const auto* child) {
        if (*child != 0) children[count++] = child->raw();
      });
      _destination._db->free(node - sizeof(PageHeader));
      for (size_t index = 0; index < count; ++index)
        _free_subtree(offset_e(children[index]));
    }
    _observer.on_free();
  }

  _OwnedNode _leaf(const Slice& prefix) {
    std::array<char, 255> bytes;
    if (prefix.size()) std::memcpy(bytes.data(), prefix.data(), prefix.size());
    _observer.on_allocate();
    const auto size = LeafNode::size(prefix.size(), 0);
    auto leaf = _destination._db->template alloc_node<typename Transition::leaf_ptr>(size);
    std::memset(static_cast<char*>(leaf), 0, LeafNode::HEADER_SIZE);
    leaf->set(Slice(bytes.data(), prefix.size()), 0);
    offset_e offset;
    offset = _destination._db->resolve(leaf);
    return _OwnedNode(*this, offset);
  }

  _OwnedNode _trie(const std::string& prefix, _Children& children) {
    if (!children._count) return {};
    uint8_t groups = 0;
    for (size_t group = 0; group < children._bitmap.size(); ++group)
      if (children._bitmap[group]) groups |= uint8_t(1u << group);
    _observer.on_allocate();
    const auto size = TrieNode::size(prefix.size(), children._count);
    auto trie = _destination._db->template alloc_node<typename Transition::trie_ptr>(size);
    std::memset(static_cast<char*>(trie), 0, TrieNode::HEADER_SIZE);
    _output_offsets[0] = children._none;
    children.for_each_child([&](int key, const offset_e& offset) {
      _output_offsets[key + 1] = offset;
    });
    trie->create(Slice(prefix), _output_offsets.data() + 1,
                 children._bitmap.data(), groups);
    offset_e offset;
    offset = _destination._db->resolve(trie);
    children.release();
    return _OwnedNode(*this, offset);
  }

  _OwnedNode _clone(const FirstOffset& offset) {
    if (offset == 0) return {};
    _observer.on_first();
    FirstView node(_first._db, offset);
    if (node._leaf) return _leaf(Slice(node._prefix));
    _Children children(*this);
    node.for_each_child([&](int key, const auto& child) {
      children.add(key, _clone(child));
    });
    return _trie(node._prefix, children);
  }

  _OwnedNode _match(const FirstOffset& first_offset, size_t first_pos,
                    const SecondOffset& second_offset, size_t second_pos,
                    size_t depth) {
    if (first_offset == 0 || second_offset == 0) return {};
    if (first_offset.type() == LEAF && second_offset.type() == LEAF)
      return _match_leaves(first_offset, first_pos, second_offset, second_pos, depth);
    _observer.on_first();
    FirstView first(_first._db, first_offset);
    _observer.on_second();
    SecondView second(_second._db, second_offset);
    size_t length = std::min(first._prefix.size() - first_pos,
                             second._prefix.size() - second_pos);
    if (_key_size) length = std::min(length, _key_size - depth);
    if (length && std::memcmp(first._prefix.data() + first_pos,
                              second._prefix.data() + second_pos, length))
      return {};
    first_pos += length;
    second_pos += length;
    depth += length;
    if (_key_size && depth == _key_size) return _clone(first_offset);

    if (first_pos < first._prefix.size()) {
      if (second._leaf) return {};
      const auto key = static_cast<uint8_t>(first._prefix[first_pos]);
      return _match(first_offset, first_pos, second.child(key), 0, depth);
    }

    _Children children(*this);
    if (second_pos < second._prefix.size()) {
      if (first._leaf) return {};
      const auto key = static_cast<uint8_t>(second._prefix[second_pos]);
      children.add(key, _match(first.child(key), 0, second_offset, second_pos, depth));
    } else if (first._leaf) {
      if (second._leaf) return _leaf(Slice(first._prefix));
      return _match(first_offset, first_pos, second.child(-1), 0, depth);
    } else if (second._leaf) {
      children.add(-1, _match(first.child(-1), 0, second_offset, second_pos, depth));
    } else {
      children.add(-1, _match(first.child(-1), 0, second.child(-1), 0, depth));
      for (size_t group = 0; group < first._bitmap.size(); ++group) {
        uint32_t shared = first._bitmap[group] & second._bitmap[group];
        while (shared) {
          const size_t key = group * 32 + bits::first(shared);
          shared &= shared - 1;
          children.add(static_cast<int>(key),
                       _match(first.child(key), 0, second.child(key), 0, depth));
        }
      }
    }
    return _trie(first._prefix, children);
  }

  _OwnedNode _match_leaves(const FirstOffset& first_offset, size_t first_pos,
                           const SecondOffset& second_offset, size_t second_pos,
                           size_t depth) {
    _observer.on_first();
    auto first = _first._db->template resolve<typename FirstView::LeafNode>(&first_offset);
    _observer.on_second();
    auto second = _second._db->template resolve<typename SecondView::LeafNode>(&second_offset);
    const size_t first_length = first->key_size - first_pos;
    const size_t second_length = second->key_size - second_pos;
    const size_t length = _key_size ? _key_size - depth : first_length;
    if (first_length < length || second_length < length ||
        (!_key_size && first_length != second_length) ||
        (length && std::memcmp(first->data + first_pos, second->data + second_pos, length)))
      return {};
    return _leaf(Slice(first->data, first->key_size));
  }
};

struct _SetOperations {
  template <typename Cursor>
  static void _check_cursor() {
    static_assert(std::is_same_v<typename Cursor::DBImpl,
                                _DB<typename Cursor::StorageImpl>>,
                  "Set operations require ordinary DB cursors.");
  }

  template <typename DestinationCursor, typename FirstCursor, typename SecondCursor,
            typename Observer>
  static bool intersect(const DestinationCursor& destination_cursor,
                        const FirstCursor& first_cursor,
                        const SecondCursor& second_cursor, size_t key_size,
                        Observer& observer) {
    _check_cursor<DestinationCursor>();
    _check_cursor<FirstCursor>();
    _check_cursor<SecondCursor>();
    if (!destination_cursor._cursor || !first_cursor._cursor || !second_cursor._cursor)
      return false;
    auto& destination = *destination_cursor._cursor;
    auto& first = *first_cursor._cursor;
    auto& second = *second_cursor._cursor;
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
    if (key_size && (!operation.valid_keys(first, *first._root) ||
                     !operation.valid_keys(second, *second._root)))
      return false;
    struct _Reset {
      Dst& _cursor;
      ~_Reset() {
        _cursor.stack.clear();
        _cursor.rest_key.reset();
        _cursor.current_key.clear();
      }
    } reset{destination};
    operation.exec();
    return true;
  }
};

}

#endif