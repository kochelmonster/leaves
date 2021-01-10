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

void TableData::insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  if (key.empty()) {
    assert(val_ptr.node->type == kValue);
    self.cmp = 1;
    self.index = 0;
    val_ptr.value->next = *self.node_ptr;
    self.set(val_ptr);
    return;
  }

  if (count >= (bottom+1)/2) {
    split(self, key, val_ptr);
    return;
  }

  int index = self.index;
  memmove(data[index+1].bytes, data[index].bytes, sizeof(Item)*(count-index));

  int end = bottom-index;
  for(int i=bottom-count; i < end; i++)
    data[i].ptr = data[i+1].ptr;

  Item& item(data[index]);
  item.size = min_size(key.size());
  memcpy(item.fragment, key.data(), item.size);

  val_ptr = CompressedData::build(self.trace, val_ptr, key.advance(item.size), kCompressedLeaf);
  data[bottom-index].ptr = val_ptr;
  count++;
}

offset_ptr* TableData::find(Transition& self, ISlice& key, string& current_key) {
  offset_ptr* result = ifind(self, key);
  if (result) {
    Item& item(data[self.index]);
    key.iadvance(item.size);
    current_key.append(item.fragment, item.size);
    return result;
  }
  return NULL;
}

offset_ptr* TableData::ifind(Transition& self, const Slice& key) {
  int lo = 0, hi=count-1, pivot_ = 0, cmp = -1;

  self.cmp = compare_item(count-1, key);
  // optimation for append
  switch(self.cmp) {
    case -1:
      self.index = count;
      return NULL;
    case 0:
      self.index = count - 1;
      return &data[bottom-self.index].ptr;
  }

  while(lo < hi) {
    pivot_ = (lo + hi) / 2;
    cmp = sign(compare_item(pivot_, key));
    switch(cmp) {
      case -1: lo= pivot_ + 1; break;
      case 1: hi = pivot_; break;
      default: lo = hi = pivot_;
    }
  }
  if (cmp < 0)
    cmp = sign(compare_item(lo, key));

  self.cmp = cmp;
  self.index = lo;
  return cmp == 0 ? &data[bottom-self.index].ptr : NULL;
}

void TableData::split(Transition& self, const Slice& key, any_ptr val_ptr) {
  // first extract a common prefix
  size_t split_pos;
  bool go_on = true;

  // the fast normal branch
  for(split_pos = 0; split_pos < sizeof(Item::fragment) && go_on; split_pos++) {
    if (split_pos >= key.size()) {
      go_on = false;
      continue;
    }

    char cmp = key[split_pos];
    for(int j = 0; j < count; j++) {
      Item& item(data[j]);
      if (item.size <= split_pos || item.fragment[split_pos] != cmp) {
        go_on = false;
        break;
      }
    }
  }

  if (go_on) {
    // Still no split_pos found?
    // -> the slow complete branch
    Trace iter(self.trace->storage, self.node_ptr);
    for(; go_on; split_pos++) {
      if (split_pos >= key.size()) {
        go_on = false;
        continue;
      }

      char cmp = key[split_pos];
      for(iter.first(); iter.valid(); iter.next()) {
        if (iter.current_key.size() < split_pos || iter.current_key[split_pos] != cmp) {
          go_on = false;
          break;
        }
      }
    }
  }

  trie_split(self, --split_pos, key, val_ptr);
}

#ifdef DEBUG_SPLIT
void dump_node(std::ostream& out, any_ptr ptr, Storage* storage);
#endif

void TableData::trie_split(Transition& self, int split_pos, const Slice& key, any_ptr val_ptr) {
  /* move the values into a new subtree */

  val_ptr = CompressedData::build(self.trace, val_ptr, key.advance(split_pos+1), kCompressedTable);
  val_ptr = CompressedData::build(self.trace, val_ptr, key.slice(split_pos+1), kCompressedTrie);

  offset_ptr rroot(*self.node_ptr);
  offset_ptr iroot(val_ptr);
  Trace inserter(self.trace->storage, &iroot);
  Trace remover(self.trace->storage, &rroot);

#ifdef DEBUG_SPLIT
  int i = 0;
  std::stringstream cstr;
  cstr << "errors/error_" << i++ << ".yaml";
  std::ofstream out(cstr.str());
  dump_node(out, *inserter.root, &self.trace->storage);
#endif

  for(remover.first(); remover.valid(); remover.first()) {
    inserter.find(remover.current_key);
    inserter.iinsert(remover.ipop_value());
#ifdef DEBUG_SPLIT
    std::stringstream cstr;
    cstr << "errors/error_" << i++ << ".yaml";
    std::ofstream out(cstr.str());
    dump_node(out, *inserter.root, &self.trace->storage);
#endif
  }

  self.set(iroot);
}

bool TableData::remove(Transition& self) {
  int index = self.index;

  if (data[bottom-index].ptr)
    return false;

  if (count == 1) {
    // purge this
    assert(!data[bottom].ptr);
    *self.node_ptr = offset_ptr();
    self.trace->free(this);
    return true;
  }

  count--;
  memmove(data[index].bytes, data[index+1].bytes, sizeof(Item)*(count-index));

  int end = bottom - count;
  for(int i=bottom-index; i > end; i--)
    data[i].ptr = data[i-1].ptr;

  return true;
}

any_ptr TableData::build(Trace* trace, any_ptr val_ptr, const Slice& key) {
  assert(val_ptr.node->type == kValue);

  val_ptr = CompressedData::build(
      trace, val_ptr, key.advance(sizeof(Item::fragment)), kCompressedLeaf);

  TableData* table = trace->storage.pools[MAIN_POOL_COUNT-1].allocate().table;
  table->type = kTable;
  table->bottom = 2*trace->storage.table_count - 1;
  table->count = 1;

  Slice tkey(key.slice(sizeof(Item::fragment)));
  uint8_t size = table->data[0].size = tkey.size();
  memcpy(table->data[0].fragment, tkey.data(), size);
  table->data[table->bottom].ptr = val_ptr;
  return table;
}

} // namespace leaves
