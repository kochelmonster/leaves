#ifndef _LEAVES_INSERTER_HPP
#define _LEAVES_INSERTER_HPP

#include <array>
#include <cstring>
#include <functional>

#include "_node.hpp"

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

  // Allocate node with PageHeader prefix, return pointer to node
  template <typename NodePtr>
  NodePtr alloc_node(uint16_t node_size) {
    using PageHeader = typename Traits::PageHeader;
    page_ptr page = alloc(node_size);
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
    trie_ptr child_trie =
        alloc_node<trie_ptr>(TrieNode::size(suffix_len, otrie->count()));
    child_trie->create(*otrie,
                       Slice(&otrie->compressed()[back->prefix], suffix_len));

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int key =
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE;
    trie_ptr trie = alloc_node<trie_ptr>(TrieNode::size(back->prefix, 2));

    back->trie() = trie;
    auto idxs = trie->create(Slice(otrie->compressed(), back->prefix),
                             otrie->compressed()[back->prefix], key);

    back->link_idx = idxs.second;
    trie->array()[idxs.first] = resolve(child_trie);

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
    leaf_ptr leaf =
        alloc_node<leaf_ptr>(LeafNode::size(key.size(), value_size));
    leaf->set(key, value_size);
    return leaf;
  }

  const uint16_t MAX_SIZE = TrieNode::MAX_SIZE;

  void add_to_array() {
    trie_ptr otrie = back->trie();
    trie_ptr new_trie =
        alloc_node<trie_ptr>(TrieNode::size(back->prefix, otrie->count() + 1));

    back->trie() = new_trie;
    back->link_idx = new_trie->create(
        *otrie, back->key() ? back->branch_key : TrieNode::NONE);

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

    int bkey = !copy->key_size ? TrieNode::NONE : copy->data[0];

    trie_ptr new_trie = alloc_node<trie_ptr>(TrieNode::size(back->prefix, 2));
    back->trie() = new_trie;
    auto idxs = new_trie->create(
        Slice(oleaf->data, back->prefix), bkey,
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE);

    back->link_idx = idxs.second;
    new_trie->array()[idxs.first] = resolve(copy);

    back->cmp = 0;

    free_node(oleaf);
    back->update_trie_offset();
    create_leaf();
  }

  // copy the leaf node but reduce the key size by split_pos
  leaf_ptr copy_reduced_leaf(uint8_t split_pos, leaf_ptr& oleaf) {
    leaf_ptr copy = alloc_node<leaf_ptr>(
        LeafNode::size(oleaf->key_size - split_pos, oleaf->vsize()));

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

    const NodeSlot& operator=(const NodeSlot& src) {
      // set placeholder
      assert(size == 0);
      page = src.page;
      offset = src.offset;
      type = src.type;
      return *this;
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
      leaf_ptr leaf = slot.page + (uint64_t)slot.offset;
      return resolver.resolve(leaf);
    }
    trie_ptr trie = slot.page + (uint64_t)slot.offset;
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
    assert(_node_count < MAX_NODES);

    uint16_t aligned_size = align(node_size);
    NodeSlot& slot = _slots[_node_count];
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
    static constexpr uint16_t MAX_PAYLOAD = MAX_PAGE_SIZE - sizeof(PageHeader);

    size_t i = 0;
    for (; i < _node_count; ++i) {
      page_size += _slots[i].size;
      if (page_size > MAX_PAYLOAD) {
        page_size -= _slots[i].size;
        break;
      }
    }

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
  using offset_e = typename Traits::offset_e;
  using PageHeader = typename Traits::PageHeader;
  typedef _PageBuilder<Traits> PageBuilder;
  using NodeSlot = typename PageBuilder::NodeSlot;

  Transition* back;
  page_ptr old_page;
  bool is_page_root;
  const size_t value_size;
  PageBuilder builder;

  _LocalityInserter(Transition* back_, size_t size)
      : back(back_), value_size(size) {
    is_page_root = back->is_page_root();
    old_page =
        (is_page_root ? back->node : back->parent().node) - sizeof(PageHeader);
  }

  /*
  Does all the cleanup work after exec
  - free the space of of the old node
  - connect the new node to the parent
  - copy the parent pages if needed
  - place the cursor on the new node
  */
  void clean_stack() {
    back->cursor->reset_key(back->keypos);
    if (is_page_root) {
      back->cursor->_db->free(old_page);
      back->update_offset(builder.page_link(*this));
      back->node = resolve(back->offset);
      back->find();
      assert(back->cursor->stack.back().success());
      return;
    }

    // Non-page-root case: copy the parent with siblings, link to new node
    PageBuilder _builder;
    auto& parent = back->parent();
    assert(parent.is_page_root());
    assert(parent.is_trie());

    trie_ptr ptrie = parent.trie();
    uint16_t pk_size = page_kids(&parent);

    // Calculate size of old node being replaced
    uint16_t old_size =
        back->is_trie() ? back->trie()->size() : back->leaf()->size();
    assert(pk_size >= old_size);
    pk_size -= old_size;

    // Copy parent trie and siblings (excluding old node)
    struct Ctx {
      trie_ptr ptrie;
      uint16_t link_idx;
      uint16_t pk_size;
      _LocalityInserter* self;
    };
    Ctx ctx{ptrie, parent.link_idx, pk_size, this};
    _builder.add(0, ptrie->size() + pk_size, [&ctx](NodeSlot& dst) {
      memcpy((char*)dst.trie(), (char*)ctx.ptrie, ctx.ptrie->size());
      dst.trie()->array()[ctx.link_idx] = 0;  // skip the old node from copying
      [[maybe_unused]] auto filled =
          ctx.self->copy_page_kids(ctx.ptrie, dst.trie());
      assert(filled == ctx.pk_size + ctx.ptrie->size());
    });

    // Add placeholder for the new node from builder
    _builder.add(1, 0, [&](NodeSlot& dst) { dst = builder.root(); });

    _builder.build(*this);
    _builder.set_root_link(parent.link_idx, 1, *this);

    back->cursor->_db->free(old_page);
    parent.update_offset(_builder.page_link(*this));
    parent.node = back->cursor->_db->template resolve<_Node>(parent.offset);
    back->find();
    assert(back->cursor->stack.back().success());
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

  uint16_t page_kids(Transition* trans) {
    assert(trans->is_page_root());
    auto trie = trans->trie();
    PageHeader* header = (PageHeader*)((char*)trie - sizeof(PageHeader));
    return header->used - trie->size();
  }

  uint16_t page_kids() { return is_page_root ? page_kids(back) : 0; }

  uint16_t copy_page_kids(trie_ptr from, trie_ptr to) {
    uint16_t count = to->count();
    assert(count == from->count());

    char* dest = (char*)to + to->size();
    auto farray = from->array();
    auto tarray = to->array();

    for (uint16_t i = 0; i < count; ++i) {
      // Check tarray (destination) - allows caller to zero an entry to skip it
      if (tarray[i].is_relative()) {
        offset_e* foffset = &farray[i];
        char* src = (char*)foffset + foffset->as_signed();
        tarray[i].set_relative(dest);

        if (tarray[i].type() == LEAF) {
          LeafNode* leaf = (LeafNode*)src;
          memcpy(dest, leaf, leaf->size());
          dest += leaf->size();
        } else {
          TrieNode* trie = (TrieNode*)src;
          memcpy(dest, trie, trie->size());
          dest += trie->size();
        }
      }
    }
    return dest - (char*)to;
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
    uint16_t child_trie_size = TrieNode::size(suffix_len, otrie->count());
    uint16_t pk_size = page_kids();

    if (pk_size) {
      // root with relative children - put it in a extra page
      PageBuilder builder_;
      struct Ctx {
        trie_ptr otrie;
        uint8_t suffix_len;
        uint16_t pk_size;
        uint16_t child_trie_size;
        uint16_t prefix;
        _LocalityInserter* self;
      };
      Ctx ctx{otrie, suffix_len, pk_size, child_trie_size, back->prefix, this};
      builder_.add(0, child_trie_size + pk_size, [&ctx](NodeSlot& dst) {
        dst.trie()->create(
            *ctx.otrie,
            Slice(&ctx.otrie->compressed()[ctx.prefix], ctx.suffix_len));
        [[maybe_unused]] auto filled =
            ctx.self->copy_page_kids(ctx.otrie, dst.trie());
        assert(filled == ctx.pk_size + ctx.child_trie_size);
      });
      builder_.build(*this);
      builder.add(2, 0, [&builder_](NodeSlot& dst) { dst = builder_.root(); });
    } else {
      struct Ctx2 {
        trie_ptr otrie;
        uint8_t suffix_len;
        uint16_t prefix;
      };
      Ctx2 ctx{otrie, suffix_len, back->prefix};
      builder.add(2, child_trie_size, [&ctx](NodeSlot& dst) {
        dst.trie()->create(
            *ctx.otrie,
            Slice(&ctx.otrie->compressed()[ctx.prefix], ctx.suffix_len));
      });
    }

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int new_key = key() ? (back->branch_key = key()[0]) : TrieNode::NONE;

    std::pair<uint16_t, uint16_t> idx;
    struct IdxCtx {
      std::pair<uint16_t, uint16_t>* idx;
      trie_ptr otrie;
      int new_key;
      uint16_t prefix;
    };
    IdxCtx ictx{&idx, otrie, new_key, back->prefix};
    builder.add(0, TrieNode::size(back->prefix, 2), [&ictx](NodeSlot& dst) {
      *ictx.idx = dst.trie()->create(
          Slice(ictx.otrie->compressed(), ictx.prefix), ictx.new_key,
          ictx.otrie->compressed()[ictx.prefix]);
    });

    create_leaf(1);
    builder.build(*this);
    builder.set_root_link(idx.first, 1, *this);   // new branch → new leaf
    builder.set_root_link(idx.second, 2, *this);  // old branch → child_trie

    clean_stack();
    return true;
  }

  void add_to_array() {
    trie_ptr otrie = back->trie();

    uint16_t pk_size = page_kids();
    uint16_t idx = 0xffff;

    struct Ctx {
      uint16_t* idx;
      trie_ptr otrie;
      uint16_t pk_size;
      _LocalityInserter* self;
    };
    Ctx ctx{&idx, otrie, pk_size, this};
    builder.add(0, TrieNode::size(back->prefix, otrie->count() + 1) + pk_size,
                [&ctx](NodeSlot& dst) {
                  *ctx.idx = dst.trie()->create(
                      *ctx.otrie,
                      ctx.self->key()
                          ? (ctx.self->back->branch_key = ctx.self->key()[0])
                          : TrieNode::NONE);
                  if (ctx.pk_size) {
                    [[maybe_unused]] auto filled =
                        ctx.self->copy_page_kids(ctx.otrie, dst.trie());
                    assert(filled == ctx.pk_size + dst.trie()->size());
                  }
                });
    create_leaf(1);
    builder.build(*this);
    assert(idx != 0xffff);
    builder.set_root_link(idx, 1, *this);
    clean_stack();
  }

  // change the value of leaf
  void change_leaf() {
    assert(back->is_leaf());
    leaf_ptr oleaf = back->leaf();

    if (back->cmp == 0) return replace_leaf();

    // replace the leaf with a trie node!

    // first: copy the leaf node and cut of the new rest key by prefix
    // if it is a big leaf just the reference to the big value is copied
    assert(back->prefix <= oleaf->key_size);
    std::pair<uint16_t, uint16_t> idxs;
    auto split_pos = back->prefix;
    // the orinal leaf with key reduced split_pos on
    struct LeafCtx {
      leaf_ptr oleaf;
      uint16_t split_pos;
    };
    LeafCtx lctx{oleaf, split_pos};
    builder.add(1, LeafNode::size(oleaf->key_size - split_pos, oleaf->vsize()),
                [&lctx](NodeSlot& dst) {
                  auto copy = dst.leaf();
                  copy->key_size = lctx.oleaf->key_size - lctx.split_pos;
                  copy->value_size = lctx.oleaf->value_size;
                  memcpy(copy->data, lctx.oleaf->data + lctx.split_pos,
                         copy->key_size + copy->vsize());
                });

    int okey =
        split_pos < oleaf->key_size ? oleaf->data[split_pos] : TrieNode::NONE;
    int bkey = split_pos < key().size() ? (back->branch_key = key()[split_pos])
                                        : TrieNode::NONE;

    struct TrieCtx {
      std::pair<uint16_t, uint16_t>* idxs;
      leaf_ptr oleaf;
      uint16_t split_pos;
      int okey;
      int bkey;
    };
    TrieCtx tctx{&idxs, oleaf, split_pos, okey, bkey};
    builder.add(0, TrieNode::size(split_pos, 2), [&tctx](NodeSlot& dst) {
      *tctx.idxs = dst.trie()->create(Slice(tctx.oleaf->data, tctx.split_pos),
                                      tctx.okey, tctx.bkey);
    });

    create_leaf(2);
    builder.build(*this);
    builder.set_root_link(idxs.first, 1, *this);   // old branch → reduced leaf
    builder.set_root_link(idxs.second, 2, *this);  // new branch → new leaf
    clean_stack();
  }

  void replace_leaf() {
    auto oleaf = back->leaf();
    assert(back->prefix == oleaf->key_size);
    assert(key().empty());
    assert(back->cursor->is_valid());

    if (is_page_root) {
      struct Ctx {
        leaf_ptr oleaf;
        size_t value_size;
      };
      Ctx ctx{oleaf, value_size};
      builder.add(0, LeafNode::size(oleaf->key_size, value_size),
                  [&ctx](NodeSlot& dst) {
                    dst.leaf()->set(ctx.oleaf->key(), ctx.value_size);
                  });
      builder.build(*this);
      return clean_stack();
    }

    // copy the parent and the siblings into a new page
    auto& parent = back->parent();
    assert(parent.is_trie());
    trie_ptr ptrie = parent.trie();
    uint16_t pk_size = page_kids(&parent);
    assert(pk_size > oleaf->size());
    pk_size -= oleaf->size();
    struct Ctx {
      trie_ptr ptrie;
      uint16_t link_idx;
      uint16_t pk_size;
      _LocalityInserter* self;
    };
    Ctx ctx{ptrie, parent.link_idx, pk_size, this};
    builder.add(0, ptrie->size() + pk_size, [&ctx](NodeSlot& dst) {
      memcpy((char*)dst.trie(), (char*)ctx.ptrie, ctx.ptrie->size());
      dst.trie()->array()[ctx.link_idx] = 0;  // skip old_leaf from copying
      [[maybe_unused]] auto filled =
          ctx.self->copy_page_kids(ctx.ptrie, dst.trie());
      assert(filled == ctx.pk_size + ctx.ptrie->size());
    });
    // add the new leaf
    struct LeafCtx {
      leaf_ptr oleaf;
      size_t value_size;
    };
    LeafCtx lctx{oleaf, value_size};
    builder.add(1, LeafNode::size(oleaf->key_size, value_size),
                [&lctx](NodeSlot& dst) {
                  dst.leaf()->set(lctx.oleaf->key(), lctx.value_size);
                });

    builder.build(*this);
    builder.set_root_link(parent.link_idx, 1, *this);
    parent.update_offset(builder.page_link(*this));
    parent.node = back->cursor->_db->template resolve<_Node>(parent.offset);
    back->offset = parent.trie()->array() + parent.link_idx;
    back->node = back->cursor->_db->template resolve<_Node>(back->offset);
    back->cursor->_db->free(old_page);
  }

  void create_leaf(uint16_t slot_id) {
    Slice key = back->key();

    if (key.size() <= 255) return create_leaf_only(builder, slot_id, key);

    // we have to reverse iterate the key

    // first create the leaf page
    size_t rest = key.size() % 255;
    if (rest == 0) rest = 255;  // leaf can hold up to 255 bytes
    int end = key.size() - rest;
    assert(end >= 255);
    assert((end % 255) == 0);  // must be multiple of 255
    Slice leaf_key = Slice(key.data() + end, rest);
    end -= 255;
    assert(end >= 0);
    Slice trie_key = Slice(key.data() + end, 255);

    PageBuilder builder_;
    uint16_t idx = 0;
    struct Ctx1 {
      uint16_t* idx;
      const Slice* trie_key;
      const Slice* leaf_key;
    };
    Ctx1 ctx1{&idx, &trie_key, &leaf_key};
    builder_.add(0, TrieNode::size(255, 1), [&ctx1](NodeSlot& dst) {
      *ctx1.idx = dst.trie()->create(*ctx1.trie_key, (*ctx1.leaf_key)[0]);
    });
    create_leaf_only(builder_, 1, leaf_key);
    builder_.build(*this);
    builder_.set_root_link(idx, 1, *this);

    offset_e next = builder_.page_link(*this);
    int branch_key = trie_key[0];

    constexpr uint16_t PAIR_SIZE = 255 * 2;

    // create pages with trie_255 pairs
    while (end >= PAIR_SIZE) {
      builder_.reset();
      Slice pair2(key.data() + end, 255);
      end -= 255;
      Slice pair1(key.data() + end, 255);

      struct CtxPair1 {
        uint16_t* idx;
        const Slice* pair1;
        const Slice* pair2;
      };
      CtxPair1 ctxp1{&idx, &pair1, &pair2};
      builder_.add(0, TrieNode::size(255, 1), [&ctxp1](NodeSlot& dst) {
        *ctxp1.idx = dst.trie()->create(*ctxp1.pair1, (*ctxp1.pair2)[0]);
      });
      struct CtxPair2 {
        const Slice* pair2;
        int branch_key;
        offset_e next;
      };
      CtxPair2 ctxp2{&pair2, branch_key, next};
      builder_.add(1, TrieNode::size(255, 1), [&ctxp2](NodeSlot& dst) {
        auto trie = dst.trie();
        auto idx_ = trie->create(*ctxp2.pair2, ctxp2.branch_key);
        trie->array()[idx_] = ctxp2.next;
      });
      builder_.build(*this);
      builder_.set_root_link(idx, 1, *this);
      next = builder_.page_link(*this);
      branch_key = pair1[0];
    }

    // create the first trie if needed
    if (end == 0) {
      builder.add(slot_id, 0, [&builder_](NodeSlot& dst) {
        dst = builder_.root();  // set placeholder
      });
      return;
    }

    struct CtxFirst {
      const Slice* key;
      int end;
      int branch_key;
      offset_e next;
    };
    CtxFirst ctxf{&key, end, branch_key, next};
    builder.add(slot_id, TrieNode::size(end, 1), [&ctxf](NodeSlot& dst) {
      Slice first_slice = ctxf.key->slice(ctxf.end);
      auto trie = dst.trie();
      auto idx_ = trie->create(first_slice, ctxf.branch_key);
      trie->array()[idx_] = ctxf.next;
    });
  }

  void create_leaf_only(PageBuilder& builder, uint16_t slot_id,
                        const Slice& key) {
    struct Ctx {
      const Slice* key;
      size_t value_size;
    };
    Ctx ctx{&key, value_size};
    builder.add(
        slot_id, LeafNode::size(key.size(), value_size),
        [&ctx](NodeSlot& dst) { dst.leaf()->set(*ctx.key, ctx.value_size); });
  }
};

}  // namespace leaves
#endif  // _LEAVES_INSERTER_HPP
