/* handling table node
*/
#include "table.hpp"
#include "trie.hpp"
#include "trace.hpp"

#include <fstream>

namespace leaves {

offset_ptr* Table::find(Transition& self, ISlice& key, string& current_key) {
  return self.table->find(self, key, current_key);
}

offset_ptr* Table::next(Transition& self, string& current_key) {
  return self.table->next(self, current_key);
}

offset_ptr* Table::first(Transition& self, string& current_key) {
  return self.table->first(self, current_key);
}

offset_ptr* Table::prev(Transition& self, string& current_key) {
  return self.table->prev(self, current_key);
}

offset_ptr* Table::last(Transition& self, string& current_key) {
  return self.table->last(self, current_key);
}

int Table::advance(Transition& self, const Slice& key) {
  return self.table->advance(key, self.index);
}

void Table::insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  return self.table->insert(self, key, val_ptr);
}

bool Table::remove(Transition& self) {
  return self.table->remove(self);
}

Table table_handler;

/* Table Data
---------------------------------------------------------------------------------
*/

offset_ptr* TableData::find(Transition& self, ISlice& key, string& current_key) {
  offset_ptr* result = ifind(self, key);
  if (result) {
    DataItem* item = get_item(self.index);
    key.iadvance(item->key_size);
    current_key.append(item->key_data, item->key_size);
    return result;
  }
  return NULL;
}

offset_ptr* TableData::ifind(Transition& self, const Slice& key) {
  int lo = 0, hi=count-1, pivot_ = 0, cmp = -1;

  DataItem *item = get_item(count-1);
  self.cmp = sign(item->compare(key));
  // optimation for append
  switch(self.cmp) {
    case -1:
      self.index = count;
      return NULL;
    case 0:
      self.index = count - 1;
      return &item->value;
  }

  while(lo < hi) {
    pivot_ = (lo + hi) / 2;
    item = get_item(pivot_);
    cmp = sign(item->compare(key));
    switch(cmp) {
      case -1: lo= pivot_ + 1; break;
      case 1: hi = pivot_; break;
      default: lo = hi = pivot_;
    }
  }
  if (cmp < 0) {
    item = get_item(lo);
    cmp = sign(item->compare(key));
  }

  self.cmp = cmp;
  self.index = lo;
  return cmp == 0 ? &item->value : NULL;
}


void TableData::insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  if (key.empty()) {
    assert(val_ptr.node->type == kValue);
    self.cmp = 1;
    self.index = 0;
    val_ptr.value->next = *self.node_ptr;
    self.set(val_ptr);
    return;
  }

#ifdef SPLIT_COUNT
  if (count >= SPLIT_COUNT) {
    split(self, key, val_ptr);
    return;
  }
#endif

  size_t item_size = DataItem::calc_size(key.size());
  if ((size_t)(ptrs + count) + item_size > ((size_t)this)+data_top) {
    split(self, key, val_ptr);
    return;
  }

  int index = self.index;
  memmove(&ptrs[index+1], &ptrs[index], sizeof(uint16_t)*(count-index));
  prepare_item(index, key.size())->set(key, val_ptr);
  count++;
}

void TableData::split(Transition& self, const Slice& key, any_ptr val_ptr) {
  // first extract a common prefix
  size_t split_pos;
  bool go_on = true;

  DataItem *item0 = get_item(0);

  for(split_pos = 0; ; split_pos++) {
    if (item0->key_size <= split_pos)
      break;

    char cmp = item0->key_data[split_pos];
    for(int j = 1; j < count; j++) {
      DataItem *item = get_item(j);
      if (item->key_data[split_pos] != cmp) {
        go_on = false;
        break;
      }
    }
    if (!go_on)
      break;
  }

  trie_split(self, split_pos);

  Trace inserter(self.trace->storage, self.node_ptr);
  inserter.find(key);
  inserter.iinsert(val_ptr);
}

void TableData::insert_to_trie(Transition& self, const Slice& key, any_ptr next) {
  if (self.node->type == kTrie) {
    offset_ptr *ptr = trie_handler.ifind(self, key[0]);
    if (ptr) {
      // a short is already here
      assert(ptr->resolve().node->type == kValue);
      ptr->resolve().value->next = next;
    }
    else
      self.insert(key, next);
  }
  else
      self.set(TrieData::build(self.trace, next, key[0]));
}

void TableData::trie_split(Transition& self, int split_pos) {
  /* move the values into a new subtree */
  assert(count > 1);
  DataItem *item(get_item(0));
  int start = item->key_size <= split_pos ? 1 : 0; // item0 is short

  // Distribute items to split table
  int offset = split_pos+1;
  char cmp = get_item(start)->key_data[split_pos];
  Slice trie_key(&cmp, 1);

  // 1. insert one letter keys -> append a value
  for(int i = start; i < count; i++) {
      item = get_item(i);
      cmp = item->key_data[split_pos];
      if (item->key_size <= offset)
        insert_to_trie(self, trie_key, item->value);
  }

  // 2. insert multi letter keys -> append a table
  TableData *table = alloc(self.trace);
  int last_index = start;
  cmp = get_item(start)->key_data[split_pos];
  for(int i = start; i < count; i++) {
      char ncmp = get_item(i)->key_data[split_pos];
      if (ncmp != cmp) {
        if (copy_to_split(self, table, last_index, i, offset))
          insert_to_trie(self, trie_key, table);

        cmp = ncmp;
        last_index = i;
        table = alloc(self.trace);
      }
  }
  if (copy_to_split(self, table, last_index, count, offset))
    insert_to_trie(self, trie_key, table);

  assert(self.node->type == kTrie);
  trie_handler.ifind(self, cmp); // for one_branch test
  if (trie_handler.one_branch(self)) {
    // compress the trie
    any_ptr next = self.lower->children[0].resolve();
    cmp = self.key;
    self.trace->free(self.node);
    self.set(CompressedData::build(self.trace, next, trie_key));
  }

  item = get_item(0);
  if (start == 1) {
    // item[0] is short -> insert its value before
    any_ptr next = item->value;
    next.value->next = *self.node_ptr;
    self.set(next);
  }

  Slice key(item->key_data, split_pos);
  self.set(CompressedData::build(self.trace, *self.node_ptr, key));
  if (self.node->type == kCompressed)
    self.compressed->eat_child(self);  // if trie node was one_branch

  self.trace->free(this);
}

bool TableData::copy_to_split(Transition& self, TableData* dest, int start, int end, int offset) {
  int dindex = 0;
  for(int i = start; i < end; i++) {
    DataItem* item = get_item(i);
    if (item->key_size > offset) {
      DataItem* item = get_item(i);
      dest->prepare_item(dindex++, item->key_size-offset)->set(item, offset);
      dest->count++;
    }
  }
  if (!dest->count) {
    self.trace->free(dest);
    return false;
  }
  return true;
}


bool TableData::remove(Transition& self) {
  if (count == 1) {
    // purge this
    *self.node_ptr = offset_ptr();
    self.trace->free(this);
    return true;
  }

  // faster to copy items to new table
  TableData *dest = alloc(self.trace);
  int dindex = 0;
  for(int i = 0; i < count; i++) {
    if (i != self.index) {
      DataItem* item = get_item(i);
      dest->prepare_item(dindex++, item->key_size)->set(item, 0);
      dest->count++;
    }
  }
  self.set(dest);
  self.trace->free(this);
  return true;
}

TableData* TableData::alloc(Trace* trace) {
  size_t table_size = trace->storage.burst_size * PAGE_SIZE;
  TableData* table = trace->storage.allocate(table_size).table;
  table->type = kTable;
  table->data_top = table_size;
  table->count = 0;
  return table;
}

any_ptr TableData::build(Trace* trace, any_ptr val_ptr, const Slice& key) {
  assert(val_ptr.node->type == kValue);
  TableData* table = alloc(trace);
  table->type = kTable;
  table->prepare_item(0, key.size())->set(key, val_ptr);
  table->count = 1;
  return table;
}

} // namespace leaves
