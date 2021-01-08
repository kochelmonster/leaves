/* handling table node
*/
#include "table.hpp"
#include "trie.hpp"
#include "trace.hpp"


namespace leaves {

offset_ptr* Table::find(Transition& self, ISlice& key, string& current_key) {
  TableData *table = self.table;
  if (!(self.cmp = table->find(key, &self.index))) {
    return table->get_ptr(self.index);
  }
  return NULL;
}

offset_ptr* Table::next(Transition& self, string& current_key) {
  return self.table->next(self);
}

offset_ptr* Table::first(Transition& self, string& current_key) {
  return self.table->first(self);
}

offset_ptr* Table::prev(Transition& self, string& current_key) {
  return self.table->prev(self);
}

offset_ptr* Table::last(Transition& self, string& current_key) {
  return self.table->last(self);
}

int Table::advance(Transition& self, ISlice& key) {
  return self.table->advance(key, self.index);
}

void Table::insert(Transition& self, ISlice& key, any_ptr val_ptr) {
  return self.table->insert(self, key, val_ptr);
}

bool Table::remove(Transition& self) {
  return self.table->remove(self);
}


void TableData::insert_item(Transition& self, ISlice& key, any_ptr val_ptr, int index) {
  memmove(data[index+1].fragment, data[index].fragment, sizeof(Item)*(count-index));

  int end = bottom - count;
  for(int i=bottom-index; i > end; i--)
    data[i-1].ptr = data[i].ptr;

  count++;
  memset(data[index].fragment, 0, sizeof(Item));
  memcpy(data[index].fragment, key.data(), std::min(key.size(), sizeof(Item)));
  data[bottom-index].ptr = CompressedData::build(self.trace, val_ptr, key);
}

void TableData::insert(Transition& self, ISlice& key, any_ptr val_ptr) {
  if (!key.size()) {
    assert(val_ptr.node->type == kValue);
    self.cmp = 1;
    self.index = 0;
    val_ptr.value->next = *self.node_ptr;
    self.set(val_ptr);
  }

  if (count >= self.trace->storage.table_count) {
    split(self, key, val_ptr);
    return;
  }

  size_t size_ = std::min(sizeof(Item), key.size());
  if (memcmp(&data[count-1], key.data(), size_) < 0) {
    // optimation for append
    memset(data[count].fragment, 0, sizeof(Item));
    memcpy(data[count].fragment, key.data(), size_);
    data[bottom-count].ptr = CompressedData::build(self.trace, val_ptr, key);
    count++;
    return;
  }

  int pivot;
  switch(find(key, &pivot, count-2)) {
    case -1: insert_item(self, key, val_ptr, pivot+1); break; // pivot < key
    case 1: insert_item(self, key, val_ptr, pivot); break; // pivot > key
    default: assert(0);  // we moved further
  }
}

int TableData::find(ISlice& key, int* pivot, int bottom) {
  size_t size_ = std::min(sizeof(Item), key.size());
  int top = 0, pivot_ = 0, cmp = -1;
  while(top <= bottom) {
    pivot_ = (bottom + top) / 2;
    cmp = sign(memcmp(data[pivot_].fragment, key.data(), size_));
    switch(cmp) {
      case -1: top = pivot_ + 1; break;
      case 1: bottom = pivot_ - 1; break;
      default: top = 1; bottom = 0;
    }
  }
  *pivot = pivot_;
  return cmp;
}

void TableData::split(Transition& self, ISlice& key, any_ptr val_ptr) {
  char prefix[sizeof(Item)];
  // first extract a common prefix

  for(int i = 0; i < (int)sizeof(prefix); i++) {
    char cmp = prefix[i] = data[0].fragment[i];
    for(int j = 1; j < count; j++) {
      if (data[j].fragment[i] != cmp) {
        trie_split(self, i, key, val_ptr);
        self.trace->free(this);
        return;
      }
    }
  }
  assert(0);
}

void TableData::trie_split(Transition& self, int split_pos, ISlice& key, any_ptr val_ptr) {
  Trace remover(self.trace->storage, self.node_ptr);
  remover.first();
  any_ptr to_insert = remover.ipop_value();

  // insert the first node in a newly created stuctur
  Slice first(Slice(remover.current_key).advance(split_pos));
  to_insert = CompressedData::build(self.trace, to_insert, first.advance(1));
  any_ptr root = TrieData::build(self.trace, to_insert, first[0]);

  if (split_pos) {
    Slice prefix(Slice(remover.current_key).slice(split_pos));
    root = CompressedData::build(self.trace, root, prefix);
  }
  self.set(root);

  Trace inserter(self.trace->storage, self.node_ptr);

  // Now remove all edges and add to new trie
  remover.first();
  for(; remover.valid(); remover.first()) {
    inserter.find(remover.current_key);
    inserter.iinsert(remover.ipop_value());
  }

  // insert the new item
  inserter.find(key);
  inserter.iinsert(val_ptr);
}

bool TableData::remove(Transition& self) {
  int index = self.index;

  if (data[bottom-index].ptr)
    return false;

  if (count == 2) {
    // purge this
    self.set(self.index == 0 ? data[bottom-1].ptr : data[bottom].ptr);
    self.trace->free(this);
    return true;
  }

  count--;
  memmove(data[index].fragment, &data[index+1], sizeof(Item)*(count-index));

  int end = bottom - count;
  for(int i=bottom-index; i > end; i--)
    data[i].ptr = data[i-1].ptr;
  return true;
}

any_ptr TableData::build(Trace* trace, any_ptr next1, any_ptr next2) {
  assert(next1.node->type == kCompressed);
  assert(next2.node->type == kCompressed);

  TableData* table = trace->storage.pools[MAIN_POOL_COUNT-1].allocate().table;
  table->type = kTable;
  table->bottom = 2*trace->storage.table_count - 1;
  table->count = 2;

  if (next1.compressed->keys[0] > next2.compressed->keys[0]) {
    any_ptr tmp = next1;
    next1 = next2;
    next2 = tmp;
  }
  memset(table->data[0].fragment, 0, sizeof(Item)*2);
  memcpy(table->data[0].fragment, next1.compressed->keys, min_size(next1.compressed->size));
  memcpy(table->data[1].fragment, next2.compressed->keys, min_size(next2.compressed->size));
  table->data[table->bottom].ptr = next1;
  table->data[table->bottom-1].ptr = next2;
  return table;
}

} // namespace leaves
