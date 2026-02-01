#ifndef _LEAVES_INSERTER_HPP
#define _LEAVES_INSERTER_HPP

#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <type_traits>

#include "../core/_node.hpp"
#include "_check.hpp"

namespace leaves {

template <typename Transition>
struct _Inserter {
  typedef _Inserter<Transition> Inserter;
  using Traits = typename Transition::Traits;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using page_ptr = typename Transition::page_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using offset_e = typename Transition::offset_e;

  Transition* back;
  const size_t value_size;

  _Inserter(Transition* back_, size_t size) : back(back_), value_size(size) {}

  template <typename T>
  offset_t resolve(T ptr) {
    return back->cursor->_db->resolve(ptr);
  }

  page_ptr resolve(const offset_t* offset_ptr) {
    return back->cursor->_db->resolve(offset_ptr);
  }

  page_ptr alloc(uint16_t size) { return back->cursor->alloc(size); }

  page_ptr alloc(uint16_t size, const offset_e* hint) {
    return back->cursor->alloc(size, hint);
  }

  // Allocate node with PageHeader prefix, return pointer to node
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc(node_size);
    return page + sizeof(PageHeader);
  }

  // Allocate node with locality hint
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size, const offset_e* hint) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc(node_size, hint);
    return page + sizeof(PageHeader);
  }

  // Free node by computing PageHeader pointer
  template <typename NodePtr>
  void free_node(NodePtr& node) {
    using PageHeader = typename Traits::PageHeader;
    static_assert(
        !std::is_same_v<NodePtr, page_ptr>,
        "free_node must be called with node pointers, not page pointers");
    page_ptr page = node - sizeof(PageHeader);
    free(page);
  }

  void free(page_ptr page) { back->cursor->_db->free(page); }

  void exec() {
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // insert the very first value
  void first_exec() {
    assert(back->is_root());

    const Slice& bkey = back->key();
    if (bkey.size() > 255) {
      // First insert, no children yet - no hint
      trie_ptr trie = alloc_node<trie_ptr>(TrieNode::size(255, 1));
      assert(trie);  // must always succeed
      *back->offset = resolve(trie);
      back->trie() = trie;
      fill_bigkey(*back);
      create_leaf();
      return;
    }

    assert(bkey.size() <= 255);
    back->leaf() = fill_leaf(bkey);
    assert(back->leaf());  // must always succeed
    *back->offset = resolve(back->leaf());
    back->cmp = 0;
    back->prefix = bkey.size();
    back->advance_key(back->prefix);
  }

  bool split_compressed() {
    if (back->is_trie() && back->prefix == back->trie()->len())
      return false;  // no split

    /*
    Operation:

      Before:
        parent -> [abcd] -> children

      Insert: [abef]

      After:
        parent -> [ab] -> [cd] -> children
                       -> [ef] -> table with new value
    */

    assert(back->prefix < back->trie()->len());

    trie_ptr otrie = back->trie();
    assert(otrie->count() < otrie->MAX_BRANCH_COUNT);

    // copy the original trie node with second part of compressed
    // to a new slot
    uint8_t suffix_len = otrie->len() - back->prefix;
    uint16_t trie_size = otrie->changed_len(suffix_len);
    // Use original trie's first child as hint (locality with existing children)
    const offset_e* child_hint = otrie->count() > 0 ? &otrie->array()[0] : nullptr;
    trie_ptr child_trie = alloc_node<trie_ptr>(trie_size, child_hint);
    child_trie->create(*otrie,
                       Slice(&otrie->compressed()[back->prefix], suffix_len));
    assert(child_trie->size() == trie_size);

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int key = back->key() ? (back->branch_key = (uint8_t)back->key()[0])
                          : TrieNode::NONE;
    int okey = otrie->compressed()[back->prefix];
    // Use child_trie offset as hint (trie near its child)
    offset_e child_trie_offset = resolve(child_trie);
    trie_ptr trie =
        alloc_node<trie_ptr>(TrieNode::size(back->prefix, okey, key), &child_trie_offset);

    back->trie() = trie;
    auto idxs =
        trie->create(Slice(otrie->compressed(), back->prefix), okey, key);

    back->link_idx = idxs.second;
    trie->array()[idxs.first] = resolve(child_trie);
    assert(trie->isset(okey));
    assert(trie->isset(key));

    free_node(otrie);
    back->update_trie_offset();
    create_leaf();
    return true;
  }

  void fill_bigkey(Transition& trans) {
    assert(trans.key().size() > 255);
    Slice prefix = trans.key().slice(255);
    trans.branch_key = trans.key()[prefix.size()];
    trans.link_idx = trans.trie()->create(prefix, trans.branch_key);
    trans.prefix = prefix.size();
    trans.cmp = 0;
    trans.advance_key(trans.prefix);
  }

  void create_bigkey() {
    Slice& key = back->key();
    while (key.size() > 255) {
      // Big key chain: no child hint available yet (chain built top-down)
      trie_ptr trie = alloc_node<trie_ptr>(TrieNode::size(255, 1));
      *back->link() = resolve(trie);
      Transition& bottom = back->push();
      fill_bigkey(bottom);
      back = &bottom;
    }
  }

  void create_leaf() {
    create_bigkey();
    const Slice& bkey = back->key();
    leaf_ptr leaf = fill_leaf(bkey);
    *back->link() = resolve(leaf);
    Transition& bottom = back->push();
    bottom.cmp = 0;
    bottom.prefix = bkey.size();
    bottom.advance_key(bottom.prefix);
  }

  leaf_ptr fill_leaf(const Slice& key) {
    // Get locality hint:
    // - If at leaf: use leaf's offset (replacing existing leaf)
    // - If at trie: use first sibling as neighbor hint
    const offset_e* hint = nullptr;
    if (back->is_leaf()) {
      hint = back->offset;
    } else if (back->is_trie() && back->trie()->count() > 0) {
      hint = &back->trie()->array()[0];
    }
    leaf_ptr leaf =
        alloc_node<leaf_ptr>(LeafNode::size(key.size(), value_size), hint);
    leaf->set(key, value_size);
    return leaf;
  }

  const uint16_t MAX_SIZE = TrieNode::MAX_SIZE;

  void add_to_array() {
    int key = back->key() ? (uint8_t)back->branch_key : TrieNode::NONE;
    trie_ptr otrie = back->trie();
    // Use existing child as hint for trie (locality with children)
    const offset_e* child_hint = otrie->count() > 0 ? &otrie->array()[0] : nullptr;
    trie_ptr new_trie = alloc_node<trie_ptr>(otrie->increment_size(key), child_hint);

    back->trie() = new_trie;
    back->link_idx = new_trie->create(*otrie, key);
    assert(new_trie->isset(key));

    free_node(otrie);
    back->update_trie_offset();
    back->cmp = 0;
    create_leaf();
  }

  // change the value of leaf
  void change_leaf() {
    assert(back->is_leaf());
    leaf_ptr oleaf = back->leaf();

    if (back->cmp == 0) {
      assert(back->prefix == back->leaf()->key_size);
      assert(back->key().empty());
      back->leaf() = fill_leaf(oleaf->key());
      free_node(oleaf);
      back->update_leaf_offset();
      return;
    }

    // replace the leaf with a trie node!

    // first: copy the leaf node and cut of the new rest key by prefix
    // if it is a big leaf just the reference to the big value is copied
    assert(back->prefix <= oleaf->key_size);

    leaf_ptr copy = copy_reduced_leaf(back->prefix, oleaf);

    int okey = !copy->key_size ? TrieNode::NONE : copy->data[0];
    int nkey = back->key() ? (uint8_t)back->key()[0] : TrieNode::NONE;
    back->branch_key = nkey;

    // Use the reduced leaf copy as hint (trie near its child)
    offset_e copy_offset = resolve(copy);
    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(back->prefix, okey, nkey), &copy_offset);
    back->trie() = new_trie;
    auto idxs = new_trie->create(Slice(oleaf->data, back->prefix), okey, nkey);
    assert(new_trie->isset(okey));
    assert(new_trie->isset(nkey));

    back->link_idx = idxs.second;
    new_trie->array()[idxs.first] = resolve(copy);

    back->cmp = 0;

    free_node(oleaf);
    back->update_trie_offset();
    create_leaf();
  }

  // copy the leaf node but reduce the key size by split_pos
  leaf_ptr copy_reduced_leaf(uint8_t split_pos, leaf_ptr& oleaf) {
    // Use original leaf as neighbor hint (same neighborhood)
    leaf_ptr copy = alloc_node<leaf_ptr>(
        LeafNode::size(oleaf->key_size - split_pos, oleaf->vsize()), back->offset);

    copy->key_size = oleaf->key_size - split_pos;
    copy->value_size = oleaf->value_size;
    memcpy(copy->data, oleaf->data + split_pos, copy->key_size + copy->vsize());

    return copy;
  }
};

// =============================================================================
// PageBuilder: Orchestrates multi-node page allocation with overflow handling
// =============================================================================
//
// PageBuilder collects node slots with creators, allocates pages, and executes
// node creation. Supports multi-page layouts for big key chains.
//
// Usage:
//   _PageBuilder<Traits> builder;
//   auto trie_slot = builder.add(trie_size, TRIE, [&](char* dst) { ... });
//   auto leaf_slot = builder.add(leaf_size, LEAF, [&](char* dst) { ... });
//   builder.build(alloc);
//   builder.set_link(trie->offset(key), leaf_slot, db);
//
// For big keys with paired nodes:
//   auto result_slot = builder.add_alias(first_trie_slot);  // size=0, shares
//   page
//
template <typename Traits>
struct _PageBuilder {
  using PageHeader = typename Traits::PageHeader;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using page_ptr = typename Traits::ptr;
  using trie_ptr = typename Traits::template Pointer<TrieNode>;
  using leaf_ptr = typename Traits::template Pointer<LeafNode, LEAF>;
  using offset_e = typename Traits::offset_e;

  static constexpr auto& PAGE_SIZES = Traits::PAGE_SIZES;
  static constexpr uint16_t PAGE_SIZES_COUNT = Traits::PAGE_SIZES_COUNT;
  static constexpr uint16_t MAX_PAGE_SIZE = PAGE_SIZES[PAGE_SIZES_COUNT - 1];

  // Node slot - stores page pointer after build
  struct NodeSlot {
    page_ptr page;    // Page this slot is on (set after build)
    uint16_t offset;  // Offset within page (after PageHeader)
    uint16_t size;    // Aligned size of this node (0 for place holders)
    NodeTypes type;   // TRIE or LEAF
    std::function<void(NodeSlot&)> creator;

    trie_ptr trie() {
      type = TRIE;
      return page + (uint64_t)offset;
    }
    leaf_ptr leaf() {
      type = LEAF;
      return page + (uint64_t)offset;
    }
  };

  static constexpr uint16_t MAX_NODES = 3;
  NodeSlot _slots[MAX_NODES];
  uint16_t _node_count;
  page_ptr _page;

  _PageBuilder() : _node_count(0), _page(nullptr) {}

  void reset() {
    _node_count = 0;
    _page = nullptr;
  }

  template <typename Resolver>
  offset_e page_link(Resolver& resolver) {
    assert(_node_count > 0);
    NodeSlot& slot = _slots[0];
    if (slot.type == LEAF) {
      leaf_ptr leaf = slot.leaf();
      return resolver.resolve(leaf);
    }
    trie_ptr trie = slot.trie();
    return resolver.resolve(trie);
  }

  // Add a node slot with creator lambda, returns slot index
  // WARNING: Lambda must fit in std::function's SBO to avoid heap allocation.
  // Use [&] capture - do NOT capture large objects by value.
  template <typename Creator>
  void add(uint16_t slot_id, uint16_t node_size, Creator&& creator) {
    // Typical SBO threshold is 24-32 bytes depending on implementation.
    // A [&] capture is just one pointer (8 bytes) - always safe.
    // Allow up to 32 bytes to accommodate lambdas with a few small value
    // captures.
    static_assert(
        sizeof(Creator) <= 24,
        "Lambda capture too large - will cause heap allocation in "
        "std::function. "
        "Use [&] reference capture instead of capturing objects by value.");
    assert(slot_id < MAX_NODES);

    uint16_t aligned_size = align(node_size);
    NodeSlot& slot = _slots[slot_id];
    slot.page = nullptr;
    slot.offset = 0;
    slot.size = aligned_size;
    slot.creator = std::forward<Creator>(creator);
    _node_count = std::max((uint16_t)(slot_id + 1), _node_count);
  }

  NodeSlot& root() {
    assert(_node_count > 0);
    return _slots[0];
  }

  offset_e* root_link(uint16_t idx) {
    NodeSlot& slot = _slots[0];
    assert(slot.type == TRIE);
    return slot.trie()->array() + idx;
  }

  // Build: allocate pages and run creators
  template <typename Allocator>
  void build(Allocator& alloc) {
    uint16_t page_size = 0;
    static constexpr uint16_t MAX_PAYLOAD =
        MAX_PAGE_SIZE - sizeof(PageHeader) - TrieNode::MAX_SIZE;

    // i == 0 is the root slot usually a trie, that will be for sure on the page
    size_t i = 1;
    for (; i < _node_count; ++i) {
      page_size += _slots[i].size;
      if (page_size > MAX_PAYLOAD) {
        // there must always be enough space for the maximum trie size
        page_size -= _slots[i].size;
        break;
      }
    }
    page_size += _slots[0].size;  // add root slot size
    assert(page_size + sizeof(PageHeader) <= MAX_PAGE_SIZE);

    _page = alloc.alloc(page_size);
    // fill the page
    uint16_t offset = sizeof(PageHeader);
    for (size_t j = 0; j < i; ++j) {
      if (_slots[j].size == 0) continue;  // skip place holders
      _slots[j].page = _page;
      _slots[j].offset = offset;
      offset += _slots[j].size;
    }
    // fallback for remaining slots
    for (; i < _node_count; ++i) {
      if (_slots[i].size == 0) continue;  // skip place holders
      _slots[i].page = alloc.alloc(_slots[i].size);
      _slots[i].offset = sizeof(PageHeader);
    }

    for (i = 0; i < _node_count; ++i) {
      _slots[i].creator(_slots[i]);
    }
  }

  // Set link: relative if same page, absolute if different page
  template <typename Resolver>
  void set_link(offset_e* offset_field, uint16_t slot_id, Resolver& resolver) {
    NodeSlot& target = _slots[slot_id];
    offset_field->type(target.type);

    char* target_addr = (char*)target.page + target.offset;
    if (target.page == _page) {
      // Same as first page - relative offset
      offset_field->set_relative(target_addr);
    } else {
      // Different page - absolute offset via resolve
      if (target.type == LEAF) {
        leaf_ptr leaf = (leaf_ptr)target_addr;
        *offset_field = resolver.resolve(leaf);
      } else {
        trie_ptr trie = (trie_ptr)target_addr;
        *offset_field = resolver.resolve(trie);
      }
    }
  }

  template <typename Resolver>
  void set_root_link(uint16_t idx, uint16_t slot_id, Resolver& resolver) {
    set_link(root_link(idx), slot_id, resolver);
  }
};

// =============================================================================
// Locality-Aware Inserter: Uses builders for multi-node pages
// =============================================================================
template <typename Transition>
struct _LocalityInserter {
  using Traits = typename Transition::Traits;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using page_ptr = typename Traits::ptr;
  using trie_ptr = typename Traits::template Pointer<TrieNode>;
  using leaf_ptr = typename Traits::template Pointer<LeafNode, LEAF>;
  using node_ptr = typename Traits::template Pointer<_Node>;
  using offset_e = typename Traits::offset_e;
  using PageHeader = typename Traits::PageHeader;
  typedef _PageBuilder<Traits> PageBuilder;
  using NodeSlot = typename PageBuilder::NodeSlot;

  // Core state
  Transition* back;
  page_ptr old_page;
  bool is_page_root;
  const size_t value_size;
  PageBuilder builder;

  // Lambda context - stored as members so lambdas capture only 'this'
  trie_ptr _ptrie;                      // parent/original trie
  leaf_ptr _oleaf;                      // original leaf
  uint16_t _pk_size;                    // page kids size
  uint16_t _link_idx;                   // link index in parent
  uint16_t _prefix;                     // prefix length
  uint8_t _suffix_len;                  // suffix length for split
  uint16_t _child_trie_size;            // child trie size for split
  uint16_t _idx;                        // result index from trie creation
  std::pair<uint16_t, uint16_t> _idxs;  // result indices pair
  int _nkey;              // new key byte (first byte of remaining key, or NONE)
  int _okey;              // old key byte (from existing node)
  int _chain_key;         // branch key for long key chains
  offset_e _next;         // next link for long keys
  Slice _key;             // current key slice
  Slice _trie_key;        // trie key slice for long keys
  Slice _leaf_key;        // leaf key slice for long keys
  Slice _pair1;           // pair slice 1 for long keys
  Slice _pair2;           // pair slice 2 for long keys
  int _end;               // end position for long keys
  page_ptr _root_page;    // stored root slot page
  uint16_t _root_offset;  // stored root slot offset
  NodeTypes _root_type;   // stored root slot type

  _LocalityInserter(Transition* back_, size_t size)
      : back(back_), value_size(size) {
    is_page_root = back->is_page_root();
    old_page =
        (is_page_root ? back->node : back->parent().node) - sizeof(PageHeader);
  }

  void exec() {
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // Create first node in empty tree
  void first_exec() {
    assert(back->is_root());

    create_leaf(0);
    builder.build(*this);
    *back->offset = builder.page_link(*this);
    back->node = resolve(back->offset);
    back->first();
  }

  template <typename T>
  offset_e resolve(T ptr) {
    return back->cursor->_db->resolve(ptr);
  }

  page_ptr resolve(offset_e* offset_ptr) {
    return back->cursor->_db->template resolve<page_ptr>(offset_ptr);
  }

  page_ptr alloc(uint16_t size) { return back->cursor->alloc(size); }

  Slice& key() { return back->key(); }

  /*
  Does all the cleanup work after exec
  - free the space of of the old node
  - connect the new node to the parent
  - copy the parent pages if needed
  - place the cursor on the new node
  */
  void clean_stack() {
    if (is_page_root) {
      back->cursor->_db->free(old_page);
      back->update_offset(builder.page_link(*this));
      back->node = resolve(back->offset);
      back->cursor->reset_key(back->keypos);
      back->find();
      assert(back->cursor->stack.back().success());
      return;
    }

    // Non-page-root case: copy the parent with siblings, link to new node
    auto& parent = back->parent();
    assert(parent.is_page_root());
    assert(parent.is_trie());

    _ptrie = parent.trie();
    _pk_size = page_kids(&parent);
    _link_idx = parent.link_idx;

    // Calculate size of old node being replaced
    uint16_t old_size =
        back->is_trie() ? back->trie()->size() : align(back->leaf()->size());
    assert(_pk_size >= old_size);
    _pk_size -= old_size;

    _root_page = builder.root().page;
    _root_offset = builder.root().offset;
    _root_type = builder.root().type;

    builder.reset();
    // Copy parent trie and siblings (excluding old node)
    builder.add(0, _ptrie->size(), [this](NodeSlot& dst) {
      memcpy((char*)dst.trie(), (char*)_ptrie, _ptrie->size());
      dst.trie()->array()[_link_idx] = 0;  // skip the old node from copying
    });
    add_slot_copy_page_kids();

    // Add placeholder for the new node from builder
    builder.add(2, 0, [this](NodeSlot& dst) {
      dst.page = _root_page;
      dst.offset = _root_offset;
      dst.type = _root_type;
    });
    builder.build(*this);
    builder.set_root_link(parent.link_idx, 2, *this);

    back->cursor->_db->free(old_page);
    parent.update_offset(builder.page_link(*this));
    parent.node = back->cursor->_db->template resolve<node_ptr>(parent.offset);
    parent.cursor->reset_key(parent.keypos);
    parent.cursor->stack.size--;
    parent.find();
    assert(parent.cursor->stack.back().success());
  }

  uint16_t page_kids(Transition* trans) {
    assert(trans->is_page_root());
    auto& trie = trans->trie();
    PageHeader* header = (PageHeader*)((char*)trie - sizeof(PageHeader));
    return header->used - trie->size();
  }

  uint16_t page_kids() { return is_page_root ? page_kids(back) : 0; }

  uint16_t copy_page_kids(trie_ptr from, trie_ptr to,
                          uint16_t skip_idx = 0xffff) {
    uint16_t count = to->count();
    assert(count == from->count() ||
           (count == from->count() + 1 && skip_idx >= 0));

    char* dest = (char*)to + to->size();
    auto farray = from->array();
    auto tarray = to->array();

    for (uint16_t i = 0, j = 0; i < count; ++i) {
      if (i == skip_idx) continue;

      if (tarray[i].is_relative()) {
        tarray[i].set_relative(dest);
        offset_e* foffset = &farray[j];
        char* src = (char*)foffset + foffset->as_signed();

        if (tarray[i].type() == LEAF) {
          LeafNode* leaf = (LeafNode*)src;
          memcpy(dest, leaf, leaf->size());
          dest += align(leaf->size());
        } else {
          TrieNode* trie = (TrieNode*)src;
          memcpy(dest, trie, trie->size());
          assert(align(trie->size()) == trie->size());
          dest += trie->size();
        }
      }
      ++j;
    }
    return dest - (char*)to;
  }

  /*
    copy the page kids in an extra slot for correct overflow handling
    (allways slot 1)
  */
  void add_slot_copy_page_kids(uint16_t* skip_idx = nullptr) {
    builder.add(1, _pk_size, [this, skip_idx](NodeSlot& dst) {
      if (_pk_size) {
        auto& root = this->builder.root();
        assert(root.page == dst.page);  // must be same page!
        trie_ptr trie = root.trie();
        [[maybe_unused]] auto filled =
            copy_page_kids(_ptrie, trie, skip_idx ? *skip_idx : 0xffff);
        assert(filled == _pk_size + trie->size());
      }
    });
  }

  bool split_compressed() {
    if (back->is_trie() && back->prefix == back->trie()->len())
      return false;  // no split

    /*
    Operation:

      Before:
        parent -> [abcd] -> children

      Insert: [abef]

      After:
        parent -> [ab] -> [cd] -> children
                       -> [ef] -> table with new value
    */

    assert(back->prefix < back->trie()->len());

    _ptrie = back->trie();
    assert(_ptrie->count() < _ptrie->MAX_BRANCH_COUNT);

    // copy the original trie node with second part of compressed
    // to a new slot
    _suffix_len = _ptrie->len() - back->prefix;
    _child_trie_size = _ptrie->changed_len(_suffix_len);
    _pk_size = page_kids();
    _prefix = back->prefix;

    if (_pk_size) {
      // root with relative children - put it in a extra page
      PageBuilder builder_;
      builder_.add(0, _child_trie_size + _pk_size, [this](NodeSlot& dst) {
        dst.trie()->create(*_ptrie,
                           Slice(&_ptrie->compressed()[_prefix], _suffix_len));
        [[maybe_unused]] auto filled = copy_page_kids(_ptrie, dst.trie());
        assert(filled == _pk_size + _child_trie_size);
      });
      builder_.build(*this);
      // Store root slot data before builder_ goes out of scope
      _root_page = builder_.root().page;
      _root_offset = builder_.root().offset;
      _root_type = builder_.root().type;
      builder.add(2, 0, [this](NodeSlot& dst) {
        dst.page = _root_page;
        dst.offset = _root_offset;
        dst.type = _root_type;
      });
    } else {
      builder.add(2, _child_trie_size, [this](NodeSlot& dst) {
        dst.trie()->create(*_ptrie,
                           Slice(&_ptrie->compressed()[_prefix], _suffix_len));
      });
    }

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    _nkey = key() ? (back->branch_key = (uint8_t)key()[0]) : TrieNode::NONE;
    _okey = _ptrie->compressed()[_prefix];

    builder.add(0, TrieNode::size(_prefix, _nkey, _okey),
                [this](NodeSlot& dst) {
                  _idxs = dst.trie()->create(
                      Slice(_ptrie->compressed(), _prefix), _nkey, _okey);
                });

    create_leaf(1);
    builder.build(*this);
    builder.set_root_link(_idxs.first, 1, *this);   // new branch → new leaf
    builder.set_root_link(_idxs.second, 2, *this);  // old branch → child_trie

    clean_stack();
    return true;
  }

  void add_to_array() {
    _ptrie = back->trie();
    _pk_size = page_kids();
    _idx = 0xffff;
    assert(back->prefix == _ptrie->len());

    _nkey = key() ? (back->branch_key = (uint8_t)key()[0]) : TrieNode::NONE;
    builder.add(0, _ptrie->increment_size(_nkey), [this](NodeSlot& dst) {
      _idx = dst.trie()->create(*_ptrie, _nkey);
    });
    add_slot_copy_page_kids(&_idx);

    create_leaf(2);
    builder.build(*this);
    assert(_idx != 0xffff);
    builder.set_root_link(_idx, 2, *this);
    clean_stack();
  }

  // change the value of leaf
  void change_leaf() {
    assert(back->is_leaf());
    _oleaf = back->leaf();

    if (back->cmp == 0) return replace_leaf();

    // replace the leaf with a trie node!

    // first: copy the leaf node and cut of the new rest key by prefix
    // if it is a big leaf just the reference to the big value is copied
    assert(back->prefix <= _oleaf->key_size);
    _prefix = back->prefix;
    // the orinal leaf with key reduced split_pos on
    builder.add(1, LeafNode::size(_oleaf->key_size - _prefix, _oleaf->vsize()),
                [this](NodeSlot& dst) {
                  auto copy = dst.leaf();
                  copy->key_size = _oleaf->key_size - _prefix;
                  copy->value_size = _oleaf->value_size;
                  memcpy(copy->data, _oleaf->data + _prefix,
                         copy->key_size + copy->vsize());
                });

    _okey = _prefix < _oleaf->key_size ? _oleaf->data[_prefix] : TrieNode::NONE;
    _nkey = key() ? (uint8_t)key()[0] : TrieNode::NONE;
    back->branch_key = _nkey;

    builder.add(
        0, TrieNode::size(_prefix, _okey, _nkey), [this](NodeSlot& dst) {
          _idxs =
              dst.trie()->create(Slice(_oleaf->data, _prefix), _okey, _nkey);
        });

    create_leaf(2);
    builder.build(*this);
    builder.set_root_link(_idxs.first, 1, *this);   // old branch → reduced leaf
    builder.set_root_link(_idxs.second, 2, *this);  // new branch → new leaf
    clean_stack();
  }

  void replace_leaf() {
    _oleaf = back->leaf();
    assert(back->prefix == _oleaf->key_size);
    assert(key().empty());
    assert(back->cursor->is_valid());

    if (is_page_root) {
      builder.add(0, LeafNode::size(_oleaf->key_size, value_size),
                  [this](NodeSlot& dst) {
                    dst.leaf()->set(_oleaf->key(), value_size);
                  });
      builder.build(*this);
      return clean_stack();
    }

    // copy the parent and the siblings into a new page
    auto& parent = back->parent();
    assert(parent.is_trie());
    _ptrie = parent.trie();
    _pk_size = page_kids(&parent);
    _link_idx = parent.link_idx;
    assert(_pk_size > align(_oleaf->size()));
    _pk_size -= align(_oleaf->size());

    builder.add(0, _ptrie->size(), [this](NodeSlot& dst) {
      memcpy((char*)dst.trie(), (char*)_ptrie, _ptrie->size());
      dst.trie()->array()[_link_idx] = 0;  // skip old_leaf from copying
    });
    add_slot_copy_page_kids();

    // add the new leaf
    builder.add(
        2, LeafNode::size(_oleaf->key_size, value_size),
        [this](NodeSlot& dst) { dst.leaf()->set(_oleaf->key(), value_size); });

    builder.build(*this);
    builder.set_root_link(parent.link_idx, 2, *this);
    parent.update_offset(builder.page_link(*this));
    parent.node = back->cursor->_db->template resolve<_Node>(parent.offset);
    back->offset = parent.trie()->array() + parent.link_idx;
    back->node = back->cursor->_db->template resolve<_Node>(back->offset);
    back->cursor->_db->free(old_page);
  }

  void create_leaf(uint16_t slot_id) {
    _key = back->key();

    if (_key.size() <= 255) return create_leaf_only(builder, slot_id, _key);

    // we have to reverse iterate the key

    // first create the leaf page
    size_t rest = _key.size() % 255;
    if (rest == 0) rest = 255;  // leaf can hold up to 255 bytes
    _end = _key.size() - rest;
    assert(_end >= 255);
    assert((_end % 255) == 0);  // must be multiple of 255
    _leaf_key = Slice(_key.data() + _end, rest);
    _end -= 255;
    assert(_end >= 0);
    _trie_key = Slice(_key.data() + _end, 255);

    PageBuilder builder_;
    _idx = 0;
    builder_.add(0, TrieNode::size(255, 1), [this](NodeSlot& dst) {
      _idx = dst.trie()->create(_trie_key, _leaf_key[0]);
    });
    create_leaf_only(builder_, 1, _leaf_key);
    builder_.build(*this);
    builder_.set_root_link(_idx, 1, *this);

    _next = builder_.page_link(*this);
    _chain_key = _trie_key[0];

    constexpr uint16_t PAIR_SIZE = 255 * 2;

    // create pages with trie_255 pairs
    while (_end >= PAIR_SIZE) {
      builder_.reset();
      _pair2 = Slice(_key.data() + _end, 255);
      _end -= 255;
      _pair1 = Slice(_key.data() + _end, 255);

      builder_.add(0, TrieNode::size(255, 1), [this](NodeSlot& dst) {
        _idx = dst.trie()->create(_pair1, _pair2[0]);
      });
      builder_.add(1, TrieNode::size(255, 1), [this](NodeSlot& dst) {
        auto trie = dst.trie();
        auto idx_ = trie->create(_pair2, _chain_key);
        trie->array()[idx_] = _next;
      });
      builder_.build(*this);
      builder_.set_root_link(_idx, 1, *this);
      _next = builder_.page_link(*this);
      _chain_key = _pair1[0];
    }

    // create the first trie if needed
    if (_end == 0) {
      // Store root slot data before builder_ goes out of scope
      _root_page = builder_.root().page;
      _root_offset = builder_.root().offset;
      _root_type = builder_.root().type;
      builder.add(slot_id, 0, [this](NodeSlot& dst) {
        dst.page = _root_page;
        dst.offset = _root_offset;
        dst.type = _root_type;
      });
      return;
    }

    builder.add(slot_id, TrieNode::size(_end, 1), [this](NodeSlot& dst) {
      Slice first_slice = _key.slice(_end);
      auto trie = dst.trie();
      auto idx_ = trie->create(first_slice, _chain_key);
      trie->array()[idx_] = _next;
    });
  }

  void create_leaf_only(PageBuilder& bldr, uint16_t slot_id,
                        const Slice& leaf_key) {
    _leaf_key = leaf_key;  // Store in member for lambda capture
    bldr.add(slot_id, LeafNode::size(leaf_key.size(), value_size),
             [this](NodeSlot& dst) { dst.leaf()->set(_leaf_key, value_size); });
  }
};

}  // namespace leaves
#endif  // _LEAVES_INSERTER_HPP
