#include "node.hpp"
#include "trace.hpp"

namespace leaves {

struct FindResult {
  union {
    struct {
      int index : 16;
      int cmp : 16;
    };
    int val;
  };
};

INLINE bool find_table_link(Trace& cursor, Transition& trans) {
  if (cursor.rest_key.empty()) {
    trans.index = -1;
    trans.found = false;
    return false;
  }

  TableBlock& block = cursor.get_block(trans.node->link)->table;
  FindResult result;
  Slice& rest_key(cursor.rest_key);
  result.val = block.find(cursor.rest_key);
  trans.index = result.index;
  trans.found = result.cmp == 0;
  if (trans.found) {
    cursor.current_key.append(rest_key.data(), rest_key.size());
    rest_key.iadvance(rest_key.size());
  }
  return false;
}

INLINE bool advance_table_link(Trace& cursor, const Transition& trans) {
  TableBlock& block = cursor.get_block(trans.node->link)->table;
  if (trans.index < block.count) {
    TableBlock::Item* item = block.get_item(trans.index);
    Slice& rest_key = cursor.rest_key;
    if (rest_key.size() == item->key_size &&
        memcmp(rest_key.data(), item->key_data, item->key_size) == 0) {
      rest_key = Slice();
    }
    return true;
  }

  return false;
}


INLINE void set_value_table_link(Trace& cursor, Transition& trans, const Slice& value) {
  if (trans.index < 0) {
    offset_ptr link = trans.node->link;
    trans.block->trie.free(trans.pnode->offset, sizeof(link));
    ssize_t onode = trans.onode;
    cursor.stack.pop_back(); // remove this transition
    create_value(cursor, trans, onode, value);
    Transition& back = cursor.stack.back();
    Transition& nback = cursor.alloc(sizeof(link), 0, kTableLink);
    nback.node->link = link;

    // change trans to value
    // add 
  }
}

INLINE void create_table(Trace& cursor, Transition& trans, ssize_t onode,
                         const Slice& value) {
  Transition& back(cursor.alloc(sizeof(offset_ptr), onode, kTableLink));
  TableBlock& block = cursor.storage.alloc_cow_block(BURST_PAGE_SIZE)->table;
  back.node->link = block.offset;
  block.init(cursor.storage.active_transaction);
  block.offsets[0] = block.add_item(cursor, value);
  assert(block.offsets[0] > 0);
  block.count = 1;
}

INLINE ssize_t is_valid_table_link(const Transition& trans) {
  return trans.found;
}

INLINE TableBlock::Item* TableBlock::get_item(uint16_t index) const {
  return (TableBlock::Item*)&data[offsets[index]];
}

inline int TableBlock::Item::compare(const Slice& other) const {
  return Slice(key_data, key_size).compare(other);
}

INLINE int TableBlock::find(const Slice& key) const {
  int lo = 0, hi = count - 1, pivot_ = 0, cmp = -1;

  FindResult result;

  // optimization for append
  Item* item = get_item(hi);
  cmp = sign(item->compare(key));
  int index = 0;
  switch (cmp) {
    case -1:
      result.index = count;
      result.cmp = cmp;
      return result.val;
    case 0:
      result.index = count-1;
      result.cmp = cmp;
      return result.val;
  }

  while (lo < hi) {
    pivot_ = (lo + hi) / 2;
    item = get_item(pivot_);
    cmp = sign(item->compare(key));
    switch (cmp) {
      case -1:
        lo = pivot_ + 1;
        break;
      case 1:
        hi = pivot_;
        break;
      default:
        lo = hi = pivot_;
    }
  }
  
  result.index = index;
  result.cmp = cmp;
  return result.val;
}

INLINE ssize_t TableBlock::add_item(Trace& cursor, const Slice& value) {
  const char* value_data;
  offset_ptr value_link;
  ssize_t real_value_size;

  if (value.size() > Value::SMALL_SIZE) {
    value_link = cursor.storage.alloc_block(value.size());
    cursor.storage.write_value(value_link, value);
    value_data = (const char*)&value_link;
    real_value_size = sizeof(value_link);
  } else {
    real_value_size = value.size();
    value_data = value.data();
  }

  ssize_t item_size =
      real_value_size - cursor.rest_key.size() - sizeof(TableBlock::Item);

  // item_size + offset
  if (item_size + sizeof(ssize_t) > available_space()) return 0;

  item_start -= item_size;
  Item* item = (Item*)&data[item_start];
  item->key_size = cursor.rest_key.size();
  memcpy(item->key_data, cursor.rest_key.data(), item->key_size);
  item->data_size = value.size();
  memcpy(&item->key_data[item->key_size], value.data(), real_value_size);
  return item_start;
}

INLINE void TableBlock::insert(Trace& cursor, const Slice& value) {
  ssize_t offset_ = add_item(cursor, value);
  if (!offset_) {
    cursor.burst();
    cursor.set_value(value);
    return;
  }

  if (cursor.rest_key.empty()) {
    // replace 
  }

}

}  // namespace leaves