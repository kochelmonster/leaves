#ifndef _LEAVES_BURST_HPP
#define _LEAVES_BURST_HPP

#include "_node.hpp"

namespace leaves {

/*
  The Table layout is
  +--------------------------------+
  | item_index[0]                  |
  +--------------------------------+
  | item_index[1]                  |
  +--------------------------------+
  |                                |
  |       free_space               |
  |                                |
  +--------------------------------+
  | DataItem[1]                    |
  +--------------------------------+
  | DataItem[0]                    \
  +--------------------------------+

key_fragments grow from top to bottom
node_pts grow from bottom to top
*/

#pragma pack(2)
template <typename Traits>
struct _BurstTable : public Traits::BlockHeader {
  using Base = typename Traits::BlockHeader;
  typedef _BurstTable<Traits> BurstTable;
  using uint16_e = typename Traits::uint16_e;
  using burst_ptr = typename Traits::Pointer<BurstTable, BURST>;

  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static const uint16_t SLOT_ID =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]) - 1;
  static const uint16_t SIZE = BLOCK_SIZES[SLOT_ID];

  struct BurstTraits : public Traits {
    struct BlockHeader {};
  };

  typedef _LeafNode<BurstTraits> ItemBase;
  struct Item : public ItemBase {
    int compare(const Slice other) const {
      return Slice(ItemBase::data, ItemBase::key_size).compare(other);
    }
  };

  uint16_e last_item;  // the lower border of free memory
  uint16_e count;
  uint16_e offsets[];

  const static size_t META_SIZE = sizeof(BurstTable);

  void init() {
    last_item = SIZE;
    count = 0;
  }

  uint16_t freespace() const {
    return last_item - sizeof(uint16_t) * count - META_SIZE;
  }

  void* resolve(uint16_t offset) { return (((uint8_t*)this) + offset); }
  const void* resolve(uint16_t offset) const {
    return (((uint8_t*)this) + offset);
  }

  const Item* item(uint16_t index) const {
    return (const Item*)(((uint8_t*)this) + offsets[index]);
  }

  template <typename Transition>
  void append_key(Transition& back) const {
    const Item* item_ = item(back.index);
    back.append_key(item_->data, item_->key_size);
    back.cmp = 0;
  }

  template <typename Transition>
  void first(Transition& back) const {
    back.index = 0;
    append_key(back);
  }

  template <typename Transition>
  void last(Transition& back) const {
    back.index = count - 1;
    append_key(back);
  }

  template <typename Transition>
  bool next(Transition& back) const {
    assert(back.index >= 0);
    assert(back.index <= count);
    if (back.cmp >= 0) {
      if (++back.index >= count) return false;
    }
    append_key(back);
    return true;
  }

  template <typename Transition>
  bool prev(Transition& back) const {
    assert(back.index >= 0);
    assert(back.index <= count);
    if (back.cmp <= 0) {
      if (back.index-- == 0) return false;
    }
    append_key(back);
    return true;
  }

  template <typename Transition>
  bool find(Transition& back) const {
    // optimation for append
    const Item* item_ = item(count - 1);
    const Slice& key = back.key();
    back.cmp = item_->compare(key);
    switch (back.cmp) {
      case -1:
        back.index = count;
        return false;
      case 0:
        back.index = count - 1;
        back.advance_key(item_->key_size);
        return false;
    }

    int lo = 0, hi = count - 2, cmp = -1;
    while (lo <= hi) {
      int pivot_ = (lo + hi) / 2;
      item_ = item(pivot_);
      cmp = item_->compare(key);
      if (cmp < 0) {
        lo = pivot_ + 1;
      } else if (cmp > 0) {
        hi = pivot_ - 1;
      } else {
        lo = hi = pivot_;
        back.advance_key(item_->key_size);
        break;
      }
    }

    back.cmp = cmp;
    back.index = lo;
    return false;
  }

  bool can_insert(const Slice& key, const Slice& value) const {
    return freespace() >= sizeof(uint16_t) + Item::size(key, value);
  }

  template <typename Transition>
  void insert(Transition& back, const Slice& value) {
    assert(can_insert(back.key(), value));
    assert(back.index >= 0);
    assert(back.index <= count);
    if (back.cmp == 0) {
      // remove old item
      uint16_t offset = offsets[back.index];
      uint8_t* src = (uint8_t*)resolve(offset);
      uint8_t* dst = src + ((Item*)src)->size();
      memmove(dst, src, offset - last_item);
      uint16_t delta = dst - src;
      for (uint16_t i = 0; i < count; i++) {
        if (offsets[i] < offset) offsets[i] += delta;
      }
    } else {
      // back.index is the next index > the key to insert
      memmove(&offsets[back.index + 1], &offsets[back.index],
              sizeof(uint16_t) * (count - back.index));
    }
    uint16_t size = Item::size(back.key(), value);
    last_item -= size;
    offsets[back.index] = last_item;
    Item* item_ = (Item*)resolve(last_item);
    // TODO: Big value handling
    item_->key_size = back.key().size();
    item_->value_size = value.size();
    memcpy(item_->data, back.key().data(), item_->key_size);
    memcpy(item_->vdata(), value.data(), value.size());
  }

  // copy whole page without blockheader
  template <typename page_ptr>
  void copy_to(page_ptr& dst) const {
    memcpy(dst.resolve(sizeof(Base)), resolve(sizeof(Base)),
           META_SIZE - sizeof(Base) + sizeof(uint16_e) * count);
    memcpy(dst.resolve(last_item), resolve(last_item), SIZE - last_item);
  }

  Slice prefix() const {
    const Item *first = item(0), *last = item(count - 1);
    int cmp;
    size_t prefix =
        get_prefix((const char*)first->data, (const char*)last->data,
                   first->key_size, last->key_size, cmp);
    return Slice(first->data, prefix);
  }
#if 0
  // hybrid burst (see https://tessil.github.io/2017/06/22/hat-trie.html)
  // prefix has to be calculated before
  template <typename Inserter>
  uint8_t burst(Inserter& inserter, size_t prefix) {
    burst_ptr burst1 = inserter.alloc(PAGE_SIZE);
    burst_ptr burst2 = inserter.alloc(PAGE_SIZE);

    offset_t oburst1 = inserter.resolve(burst1),
             oburst2 = inserter.resolve(burst2);

    uint8_t split_char = item(count / 2)->key_value[prefix];
    uint8_t last_fchar = 0xff;

    const Item* first = item(0);
    if (first->key_size > prefix) {
      last_fchar = first->key_value[prefix];
      burst1->add_item(0, first, prefix);
      node->add_array();
      array = node->array();
      *array->add(last_fchar) = oburst1;
    } else {
      // null key
      node->add_null_leaf(Slice(first->vdata(), first->value_size));
      node->add_array();
      array = node->array();
    }

    psize_t i = 1;
    for (; i < count; i++) {
      const Item* j = item(i);
      uint8_t fchar = j->key_value[prefix];
      if (fchar > split_char) break;
      burst1->add_item(i, j, prefix);
      if (last_fchar != fchar) {
        last_fchar = fchar;
        *array->add(fchar) = oburst1;
      }
    }
    psize_t offset = i;
    for (; i < count; i++) {
      const Item* j = item(i);
      uint8_t fchar = j->key_value[prefix];
      burst2->add_item(i - offset, j, prefix);
      if (last_fchar != fchar) {
        last_fchar = fchar;
        *array->add(fchar) = oburst2;
      }
    }

    return split_char;
  }
#endif
  void add_item(uint16_t index, const Item* src, uint8_t prefix) {
    assert(prefix <= src->key_size);
    uint16_t size = Item::size(src->key_size - prefix, src->vsize());
    last_item -= size;
    count = index + 1;
    offsets[index] = last_item;
    Item* item = (Item*)resolve(last_item);
    item->key_size = src->key_size - prefix;
    item->value_size = src->value_size;
    memcpy(item->data, src->data, item->key_size);
    memcpy(item->vdata(), src->vdata(), item->vsize());
  }
};
#pragma pack(0)

}  // namespace leaves
#endif  // _LEAVES_BURST_HPP