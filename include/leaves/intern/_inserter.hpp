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
    if (Traits::BURST && back->is_burst()) return add_to_burst();
    if (back->is_leaf()) return change_leaf();
    if (split_compressed()) return;
    add_to_array();
  }

  // insert the very first value
  void first() {
    if (Traits::BURST) {
      back->burst = alloc_slot(BurstTable::SLOT_ID);
      back->burst->init();
      back->burst->insert(*back, value);
      back->cmp = 0;
      back->offset = resolve(back->burst);
      back->cursor->set_root(back->offset);
      back->advance_key(back->key().size());
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
    if (Traits::BURST && back->key()) {
      offset_e* link = back->link();
      offset_e* begin = back->trie->array();
      offset_e* end = begin + back->trie->count();
      assert(begin <= link);
      assert(link < end);
      if (link > begin && (link - 1)->type() == BURST)
        *link = *(link - 1);
      else if ((link + 1) < end && (link + 1)->type() == BURST)
        *link = *(link + 1);
      else {
        burst_ptr p = alloc_slot(BurstTable::SLOT_ID);
        p->init();
        *link = resolve(p);
        Transition& bottom = back->push(*link);
        back = &bottom;
        back->cmp = -1;
        back->index = 0;
        add_to_burst();
        return;
      }
      Transition& bottom = back->push(*link);
      back = &bottom;
      back->find();
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
        offset_e old_offset = back->offset;
        back->replace(resolve(back->burst));
        if (!back->is_root()) {
          // replace also all other links to the burst table
          Transition& parent = back->parent();
          assert(parent.is_trie());
          offset_e* link = parent.link();
          offset_e* begin = parent.trie->array();
          offset_e* end = begin + parent.trie->count();
          assert(begin <= link);
          assert(link < end);
          offset_e* iter;
          for (iter = link - 1; iter >= begin && *iter == old_offset; iter--) {
            *iter = *link;
          }
          for (iter = link + 1; iter < end && *iter == old_offset; iter++) {
            *iter = *link;
          }
        }
      }

      back->burst->insert(*back, value);
      back->cmp = 0;
      back->advance_key(back->key().size());
      return;
    }

    if (back->cmp == 0) {
      // drive back rest_key and current_key
      assert(back->cursor->rest_key.empty());
      int delta = back->cursor->current_key.size() - back->keypos;
      back->cursor->current_key.resize(back->keypos);
      back->cursor->rest_key =
          Slice(back->cursor->rest_key.data() - delta, delta);
    }

    // hybrid burst (see https://tessil.github.io/2017/06/22/hat-trie.html)
    BurstTable& src = *back->burst;
    uint8_t upper = 0;
    offset_e buffer[257];  // in 256 is the none leaf
    memset(buffer, 0, sizeof(buffer));
    offset_e* offsets = &buffer[1];  // offset[Trie::NONE(==-1)] is awailable

    Slice prefix = src.prefix();
    assert(prefix.size() < 256);
    uint8_t psize = prefix.size();
    uint16_t count = src.count;
    uint16_t i = 0;
    int split_char;
    int last_fchar = -1;

    int branch_key = back->key().size() > psize ? (uint8_t)back->key()[psize]
                                                : TrieNode::NONE;

    {
      burst_ptr burst = alloc_slot(BurstTable::SLOT_ID);
      BurstTable& dst = *burst;
      dst.init();
      offset_e odst = resolve(burst);

      if (src.item(0)->key_size == psize) {
        // none leaf
        leaf_ptr leaf = alloc(LeafNode::size(0, value.size()));
        leaf->key_size = 0;
        leaf->value_size = value.size();
        // TODO: big value handling
        memcpy(leaf->vdata(), value.data(), value.size());
        offsets[TrieNode::NONE] = resolve(leaf);
        i = 1;
      }

      split_char = (uint8_t)((src.item(i)->data[psize] +
                              src.item(count - 1)->data[psize]) /
                             2);

      for (uint16_t j = 0; i < count; i++, j++) {
        auto item = src.item(i);
        assert(item->key_size > psize);
        int fchar = (uint8_t)item->data[psize];
        if (fchar > split_char) break;
        dst.add_item(j, src.item(i), psize);
        if (last_fchar != fchar) {
          last_fchar = fchar;
          offsets[fchar] = odst;
          upper |= 1 << TrieNode::ubit(fchar);
        }
      }
      dst.check();
      if (branch_key <= split_char) {
        offsets[branch_key] = odst;
        upper |= 1 << TrieNode::ubit(branch_key);
      }
    }
    assert(i < count);
    {
      burst_ptr burst = alloc_slot(BurstTable::SLOT_ID);
      BurstTable& dst = *burst;
      dst.init();
      offset_e odst = resolve(burst);
      for (uint16_t j = 0; i < count; i++, j++) {
        auto item = src.item(i);
        assert(item->key_size > psize);
        int fchar = (uint8_t)item->data[psize];
        dst.add_item(j, src.item(i), psize);
        if (last_fchar != fchar) {
          last_fchar = fchar;
          offsets[fchar] = odst;
          upper |= 1 << TrieNode::ubit(fchar);
        }
      }
      if (branch_key > split_char) {
        offsets[branch_key] = odst;
        upper |= 1 << TrieNode::ubit(branch_key);
      }
      dst.check();
    }

    free(back->burst);
    if (prefix.empty() && !back->is_root()) {
      add_burst_to_parent(upper, offsets);
    } else
      create_burst_parent(prefix, upper, offsets);

    start();
  }

  void add_burst_to_parent(uint8_t upper, offset_e* offsets) {
    Transition& parent = back->parent();
    TrieNode& trie = *parent.trie;

    upper |= trie._upper;
    int nchar = trie.first();
    while (nchar != TrieNode::OUT_OF_RANGE) {
      if (!offsets[nchar]) offsets[nchar] = *trie.offset(nchar);
      nchar = trie.next(nchar);
    }

    char buffer[TrieNode::MAX_SIZE];
    TrieNode& new_trie = *(TrieNode*)buffer;
    new_trie.create(Slice(trie.compressed(), trie._compressed_len), upper,
                    offsets);
    assert(new_trie.count() == trie.count());
    free(parent.trie);
    parent.trie = alloc(new_trie.size());
    copy(*parent.trie, new_trie);
    parent.replace(resolve(parent.trie));
    back->offset = *parent.link();
    back->block = back->resolve(back->offset);
    back->find();
  }

  void create_burst_parent(const Slice& prefix, uint8_t upper,
                           offset_e* offsets) {
    char buffer[TrieNode::MAX_SIZE];
    TrieNode& new_trie = *(TrieNode*)buffer;
    new_trie.create(prefix, upper, offsets);
    back->trie = alloc(new_trie.size());
    copy(*back->trie, new_trie);
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