#ifndef _LEAVES_INSERTER_HPP
#define _LEAVES_INSERTER_HPP

namespace leaves {

template <typename Transition>
struct _Inserter {
  typedef _Inserter<Transition> Inserter;
  using Traits = typename Transition::Traits;
  using TrieNode = typename Transition::TrieNode;
  using LeafNode = typename Transition::LeafNode;
  using BurstTable = typename Transition::BurstTable;
  using block_ptr = typename Transition::block_ptr;
  using trie_ptr = typename Transition::trie_ptr;
  using leaf_ptr = typename Transition::leaf_ptr;
  using burst_ptr = typename Transition::burst_ptr;
  using offset_e = typename Transition::offset_e;

  const Slice& value;
  Transition* back;

  _Inserter(Transition* back_, const Slice& value_)
      : value(value_), back(back_) {}

  _Inserter(Transition* back_, const Slice& value_, bool first)
      : value(value_), back(back_) {}

  tid_t txn_id() const { return back->cursor->txn_id(); }

  template <typename T>
  offset_t resolve(T ptr) {
    return back->cursor->storage.resolve(ptr);
  }
  block_ptr alloc(uint16_t size) { return back->cursor->storage.alloc(size); }
  block_ptr alloc_slot(uint16_t size) {
    return back->cursor->storage.alloc_slot(size);
  }

  void free(block_ptr& block) { back->cursor->storage.free(block); }

  void start() {
    if (Traits::BURST && back->is_burst()) add_to_burst();
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // insert the very first value
  void first() {
    if (Traits::BURST) {
      back->burst = alloc(BurstTable::META_SIZE);
      back->burst->init();
      back->burst->insert(*back, value);
      back->cmp = 0;
      back->offset = resolve(back->burst);
      back->cursor->set_root(back->offset);
      back->advance_key(back->prefix);
    } else {
      Slice bkey = back->key();
      back->prefix = std::min(bkey.size(), (size_t)10);
      if (back->prefix > 1) back->prefix--;  // keep one for trie

      back->trie = alloc(TrieNode::size(back->prefix, 1));
      back->offset = resolve(back->trie);
      back->link_offset = back->trie->create(
          Slice(bkey.data(), back->prefix),
          (bkey.size() > back->prefix ? (back->branch_key = bkey[back->prefix])
                                      : TrieNode::NONE));
      back->cursor->set_root(back->offset);
      back->advance_key(back->prefix);
      create_leaf();
    }
  }

  bool split_compressed() {
    if (back->is_trie() && back->prefix == back->trie->_compressed_len)
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

    assert(back->prefix < back->trie->_compressed_len);

    auto otrie = back->trie;

    // copy the original trie node with second part of compressed
    // to a new page
    uint8_t prefix_len = otrie->_compressed_len - back->prefix;
    trie_ptr child_trie = alloc(TrieNode::size(prefix_len, otrie->count()));
    child_trie->create(*otrie,
                       Slice(&otrie->compressed()[back->prefix], prefix_len));

    // replace the original trie node with a two branch trie node
    // and the first part of compressed
    int key =
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE;
    back->trie = alloc(TrieNode::size(back->prefix, 2));
    back->link_offset = back->trie->create(
        Slice(otrie->compressed(), back->prefix),
        otrie->compressed()[back->prefix], resolve(child_trie), key);
    free(otrie);
    back->replace(resolve(back->trie));
    create_leaf();
    return true;
  }

  void create_leaf() {
    assert(back->key().size() < 255);
    const Slice& bkey = back->key();
    leaf_ptr leaf = fill_leaf(bkey);
    Transition& bottom = back->push(resolve(leaf));
    bottom.cmp = 0;
    bottom.prefix = bkey.size();
    bottom.advance_key(bottom.prefix);
    *back->link() = bottom.offset;
  }

  leaf_ptr fill_leaf(const Slice& key) {
    leaf_ptr leaf = alloc(LeafNode::size(key, value));
    leaf->key_size = key.size();
    leaf->value_size = value.size();
    // TODO: big value handling
    memcpy(leaf->data, key.data(), key.size());
    memcpy(leaf->vdata(), value.data(), value.size());
    return leaf;
  }

  const uint16_t MAX_SIZE = TrieNode::MAX_SIZE;

  void add_to_array() {
    trie_ptr otrie = back->trie;
    back->trie = alloc(std::min(
        (uint16_t)(otrie->size() + 2 * sizeof(offset_e)), (uint16_t)MAX_SIZE));
    back->link_offset = back->trie->create(
        *otrie, back->key() ? back->branch_key : TrieNode::NONE);
    free(otrie);
    back->replace(resolve(back->trie));
    back->cmp = 0;
    if (Traits::BURST) {
      offset_e* link = back->link();
      *link = 0;
      offset_e* begin = back->trie->array();
      offset_e* end = begin + back->trie->count();
      for (offset_e* it = begin; it != end; ++it) {
        if (it->type() == BURST) {
          *link = *it;
          break;
        }
      }
      if (!link) {
        burst_ptr p = alloc_slot(BurstTable::SLOT_ID);
        *link = resolve(p);
      }
      auto bottom = back->push(*link);
      back = &bottom;
      add_to_burst();
    } else
      create_leaf();
  }

  void add_to_burst() {
    if (back->burst->can_insert(back->key(), value)) {
      if (back->burst->txn_id != txn_id()) {
        burst_ptr old = back->burst;
        back->burst = alloc_slot(BurstTable::SLOT_ID);
        old->copy_to(*back->burst);
        free(old);
        back->replace(resolve(back->burst));
      }

      back->burst->insert(*back, value);
      back->cmp = 0;
      back->advance_key(back->prefix);
      return;
    }

    // Split the burst table
    BurstTable& src = *back->burst;
    uint8_t upper = 0;
    offset_e offsets[257];
    memset(offsets, 0, sizeof(offsets));

    uint16_t i = 0;
    if (src.item(0)->key_size == 0) {
      // null leaf
      leaf_ptr leaf = alloc(LeafNode::size(0, value.size()));
      leaf->key_size = 0;
      leaf->value_size = value.size();
      // TODO: big value handling
      memcpy(leaf->vdata(), value.data(), value.size());
      offsets[0] = resolve(leaf);
      i = 1;
    }

    Slice prefix = src.prefix();
    assert(prefix.size() < 256);
    uint8_t psize = prefix.size();
    uint16_t count = count;
    uint8_t split_char = src.item(src.count / 2)->data[psize];
    int last_fchar = -1;
    
    {
      burst_ptr burst = alloc_slot(BurstTable::SLOT_ID);
      BurstTable& dst = *burst;
      offset_e odst = resolve(burst);
      for (uint16_t j = 0; i < count; i++, j++) {
        auto item = src.item(i);
        assert(item->key_size > psize);
        int fchar = item->data[psize];
        if (fchar > split_char) break;
        dst.add_item(j, src.item(i), psize);
        if (last_fchar != fchar) {
          last_fchar = fchar;
          offsets[fchar] = odst;
          upper |= 1 << TrieNode::ubit(fchar);
        }
      }
    }
    if (i < count) {
      burst_ptr burst = alloc_slot(BurstTable::SLOT_ID);
      BurstTable& dst = *burst;
      offset_e odst = resolve(burst);
      for (uint16_t j = 0; i < count; i++, j++) {
        auto item = src.item(i);
        assert(item->key_size > psize);
        int fchar = item->data[psize];
        dst.add_item(j, src.item(i), psize);
        if (last_fchar != fchar) {
          last_fchar = fchar;
          offsets[fchar] = odst;
          upper |= 1 << TrieNode::ubit(fchar);
        }
      }
    }

    free(back->burst);
    if (prefix.empty() && !back->is_root())
      add_burst_to_parent(upper, offsets);
    else
      create_burst_parent(prefix, upper, offsets);

    add_to_burst();
  }

  void add_burst_to_parent(uint8_t upper, offset_e offsets[257]) {
    Transition& parent = back->parent();
    TrieNode& trie = *parent.trie;
    upper |= trie._upper;
    if (offsets[0] == 0 && trie.has_null()) {
      offsets[0] = trie.array()[0];
    }
    int nchar = trie.next(TrieNode::NONE);
    while (nchar != TrieNode::OUT_OF_RANGE) {
      if (!offsets[nchar]) offsets[nchar] = *trie.offset(nchar);
      nchar = trie.next(nchar);
    }

    char buffer[TrieNode::MAX_SIZE];
    TrieNode& new_trie = *(TrieNode*)buffer;
    new_trie.create(Slice(trie.compressed(), trie._compressed_len), upper,
                    offsets);
    free(parent.trie);
    parent.trie = alloc(new_trie.size());
    memcpy(parent.trie, &new_trie, new_trie.size());
    parent.replace(resolve(parent.trie));
    parent.pop();
    parent.find();
    back = &back->cursor->stack.back();
  }

  void create_burst_parent(const Slice& prefix, uint8_t upper,
                           offset_e* offsets) {
    char buffer[TrieNode::MAX_SIZE];
    TrieNode& new_trie = *(TrieNode*)buffer;
    new_trie.create(prefix, upper, offsets);
    back->trie = alloc(new_trie.size());
    back->replace(resolve(back->trie));
    back->find();
    back = &back->cursor->stack.back();
  }

  // change the value of leaf
  void change_leaf() {
    assert(back->is_leaf());
    leaf_ptr oleaf = back->leaf;

    if (back->cmp == 0) {
      assert(back->prefix == back->leaf->key_size);
      assert(back->key().empty());
      back->leaf = fill_leaf(oleaf->key());
      free(oleaf);
      back->replace(resolve(back->leaf));
      return;
    }
    leaf_ptr copy =
        alloc(LeafNode::size(oleaf->key_size - back->prefix, value.size()));
    copy->key_size = oleaf->key_size - back->prefix;
    copy->value_size = oleaf->value_size;
    memcpy(copy->data, oleaf->data + back->prefix,
           copy->key_size + copy->vsize());

    int bkey = !copy->key_size ? TrieNode::NONE : copy->data[0];

    back->trie = alloc(TrieNode::size(back->prefix, 2));
    back->link_offset = back->trie->create(
        Slice(oleaf->data, back->prefix), bkey, resolve(copy),
        back->key() ? (back->branch_key = back->key()[0]) : TrieNode::NONE);

    free(oleaf);
    back->replace(resolve(back->trie));
    create_leaf();
  }
};

}  // namespace leaves
#endif  // _LEAVES_INSERTER_HPP