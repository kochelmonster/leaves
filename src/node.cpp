//  Handlers for all nodes
#include <algorithm>
#ifdef DEBUG
#include <sstream>
#endif

#include "node.hpp"
#include "trie.hpp"
#ifdef BURST
#include "table.hpp"
#endif
#include "trace.hpp"

namespace leaves {

void value_find(Transition& self, ISlice& key, KeyString& current_key) {
  self.value->find(self, key, current_key);
}
void null_find(Transition& self, ISlice& key, KeyString& current_key) {
}
void compressed_find(Transition& self, ISlice& key, KeyString& current_key) {
  self.compressed->find(self, key, current_key);
}
void trie_find(Transition& self, ISlice& key, KeyString& current_key) {
  trie::find(self, key, current_key);
}
#ifdef BURST
void table_find(Transition& self, ISlice& key, KeyString& current_key) {
  self.table->find(self, key, current_key);
}
#endif

find_f Transition::find_handlers[kNodeCount] = {
  value_find,
  null_find,
  compressed_find,
  trie_find,
#ifdef BURST
  table_find
#endif
};


void value_next(Transition& self, KeyString& current_key) {
  self.value->next(self, current_key);
}
void null_next(Transition& self, KeyString& current_key) {
}
void compressed_next(Transition& self, KeyString& current_key) {
  self.compressed->next(self, current_key);
}
void trie_next(Transition& self, KeyString& current_key) {
  trie::next(self, current_key);
}
#ifdef BURST
void table_next(Transition& self, KeyString& current_key) {
  self.table->next(self, current_key);
}
#endif

move_f Transition::next_handlers[kNodeCount] = {
  value_next,
  null_next,
  compressed_next,
  trie_next,
#ifdef BURST
  table_next
#endif
};


void value_first(Transition& self, KeyString& current_key) {
  self.value->first(self, current_key);
}
void null_first(Transition& self, KeyString& current_key) {
}
void compressed_first(Transition& self, KeyString& current_key) {
  self.compressed->first(self, current_key);
}
void trie_first(Transition& self, KeyString& current_key) {
  trie::first(self, current_key);
}
#ifdef BURST
void table_first(Transition& self, KeyString& current_key) {
  self.table->first(self, current_key);
}
#endif

move_f Transition::first_handlers[kNodeCount] = {
  value_first,
  null_first,
  compressed_first,
  trie_first,
#ifdef BURST
  table_first
#endif
};


void value_last(Transition& self, KeyString& current_key) {
  self.value->last(self, current_key);
}
void null_last(Transition& self, KeyString& current_key) {
}
void compressed_last(Transition& self, KeyString& current_key) {
  self.compressed->last(self, current_key);
}
void trie_last(Transition& self, KeyString& current_key) {
  trie::last(self, current_key);
}
#ifdef BURST
void table_last(Transition& self, KeyString& current_key) {
  self.table->last(self, current_key);
}
#endif

move_f Transition::last_handlers[kNodeCount] = {
  value_last,
  null_last,
  compressed_last,
  trie_last,
#ifdef BURST
  table_last
#endif
};


void value_prev(Transition& self, KeyString& current_key) {
  self.value->prev(self, current_key);
}
void null_prev(Transition& self, KeyString& current_key) {
}
void compressed_prev(Transition& self, KeyString& current_key) {
  self.compressed->prev(self, current_key);
}
void trie_prev(Transition& self, KeyString& current_key) {
  trie::prev(self, current_key);
}
#ifdef BURST
void table_prev(Transition& self, KeyString& current_key) {
  self.table->prev(self, current_key);
}
#endif

move_f Transition::prev_handlers[kNodeCount] = {
  value_prev,
  null_prev,
  compressed_prev,
  trie_prev,
#ifdef BURST
  table_prev
#endif
};


void value_insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  self.value->insert(self, key, val_ptr);
}
void null_insert(Transition& self, const Slice& key, any_ptr val_ptr) {
#ifdef BURST
  self.set(TableData::build(self.trace, val_ptr, key));
#else
  self.set(CompressedData::build(self.trace, val_ptr, key));
#endif
}
void compressed_insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  self.compressed->insert(self, key, val_ptr);
}
void trie_insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  trie::insert(self, key, val_ptr);
}
#ifdef BURST
void table_insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  self.table->insert(self, key, val_ptr);
}
#endif

insert_f Transition::insert_handlers[kNodeCount] = {
  value_insert,
  null_insert,
  compressed_insert,
  trie_insert,
#ifdef BURST
  table_insert
#endif
};


bool value_remove(Transition& self) {
  assert(0);
  return false;
}
bool null_remove(Transition& self) {
  return false;
}
bool compressed_remove(Transition& self) {
  return self.compressed->remove(self);
}
bool trie_remove(Transition& self) {
  return trie::remove(self);
}
#ifdef BURST
bool table_remove(Transition& self) {
  return self.table->remove(self);
}
#endif

remove_f Transition::remove_handlers[kNodeCount] = {
  value_remove,
  null_remove,
  compressed_remove,
  trie_remove,
#ifdef BURST
  table_remove
#endif
};


int value_advance(Transition& self, const Slice& key) {
  return self.value->advance(self, key);
}
int null_advance(Transition& self, const Slice& key) {
  return 0;
}
int compressed_advance(Transition& self, const Slice& key) {
  return self.compressed->advance(self, key);
}
int trie_advance(Transition& self, const Slice& key) {
  return trie::advance(self, key);
}
#ifdef BURST
int table_advance(Transition& self, const Slice& key) {
  return self.table->advance(self, key);
}
#endif

advance_f Transition::advance_handlers[kNodeCount] = {
  value_advance,
  null_advance,
  compressed_advance,
  trie_advance,
#ifdef BURST
  table_advance
#endif
};

void value_report(any_ptr node, Stats& stats, size_t depth) {
  node.value->report(stats, depth);
}
void null_report(any_ptr node, Stats& stats, size_t depth) {
}
void compressed_report(any_ptr node, Stats& stats, size_t depth) {
  node.compressed->report(stats, depth);
}
void trie_report(any_ptr node, Stats& stats, size_t depth) {
  trie::report(node, stats, depth);
}
#ifdef BURST
void table_report(any_ptr node, Stats& stats, size_t depth) {
  node.table->report(stats, depth);
}
#endif

report_f Transition::report_handlers[kNodeCount] = {
  value_report,
  null_report,
  compressed_report,
  trie_report,
#ifdef BURST
  table_report
#endif
};


any_ptr ValueData::build(Trace* trace, const Slice& value) {
  any_ptr result(trace->allocate(value.size()+sizeof(ValueData)));
  result.value->type = kValue;
  result.value->size = value.size();
  memcpy(result.value->value, value.data(), value.size());
  result.value->child = offset_ptr();
  return result;
}

void ValueData::insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  if (key.empty()) {
    val_ptr.value->child = self.value->child;
    self.trace->free(self.value);
    self.set(val_ptr);
    return;
  }

  assert(!self.value->child);
#ifdef BURST
  self.value->child = TableData::build(self.trace, val_ptr, key);
#else
  self.value->child = CompressedData::build(self.trace, val_ptr, key);
#endif
}


void CompressedData::insert(Transition& self, const Slice& key, any_ptr val_ptr) {
  assert(val_ptr.node->type == kValue);
  size_t size_ = std::min((size_t)size, key.size());
  Slice rest(keys, size);

  for(size_t i = 0; i < size_; i++) {
    if (keys[i] != key[i]) {
      /* insert_compressed_split:
         node = [abcdefg]
         key = [abhij]
                       first   trie    rest
         [abcdefg] ==> [ab] -> |c| -> [efg]
                               |h| -> [ij]
      */
      Slice first(key.data(), i);

      // build trie node
      any_ptr rest_ptr = build(self.trace, child, rest.advance(i+1));
      any_ptr trie_ptr = TrieData::build(self.trace, rest_ptr, keys[i]);

      self.set(trie_ptr);
      trie::ifind(self, key[i]);
      self.insert(key.advance(i), val_ptr);

      self.set(build(self.trace, trie_ptr, first));
      self.trace->free(this);
      return;
    }
  }

  /* insert_compressed_short: (key is a substring of node)
     node = [abcdefg]
     key = [abhij]
                   first  Leaf   rest
     [abcdefg] ==> [ab] -> [] -> [cefg]
  */
  assert(key.size() < size);
  val_ptr.value->child = build(self.trace, child, rest.advance(key.size()));
  val_ptr = build(self.trace, val_ptr, key);
  self.set(val_ptr);
  self.trace->free(this);
}

offset_ptr* CompressedData::move(Transition& self, KeyString& current_key, bool do_it) {
  if (do_it) {
    self.cmp = 0;
    current_key.append(keys, size);
    return &child;
  }
  if (self.cmp == 0) {
    self.cmp = 1;
    current_key.resize(current_key.size()-size);
  }
  return NULL;
}

bool CompressedData::remove(Transition& self) {
  if (!child) {
    self.trace->free(this);
    *self.node_ptr = offset_ptr();
    return true;
  }
  eat_child(self);
  return true;
}

any_ptr CompressedData::build(Trace* trace, any_ptr next, const Slice& key) {
  if (key.empty())
    return next;

  if (key.size() > MAX_COMPRESSED_LEN) {
    // divide key in multiple compressed
    Slice first(key.data(), MAX_COMPRESSED_LEN);
    Slice second(key.advance(MAX_COMPRESSED_LEN));
    return build(trace, build(trace, next, second), first);
  }

  any_ptr result(trace->allocate(key.size()+sizeof(CompressedData)));
  result.compressed->type = kCompressed;
  result.compressed->fill(next, key);
  return result;
}

void CompressedData::eat_child(Transition& self) {
  CompressedData *child_ = child.resolve().compressed;
  if (child_->type == kCompressed) {
    string tmp;
    tmp.reserve(size + child_->size);
    tmp.append(keys, size);
    tmp.append(child_->keys, child_->size);
    any_ptr result = CompressedData::build(self.trace, child_->child, tmp);
    self.trace->free(child_);
    self.trace->free(this);
    self.set(result);
  }
}


#ifdef DEBUG

const char* handler_names[] = {
  "kValue",
  "kNull",
  "kCompressed",
  "kTrie",
  "kTable"
};


string dump_id(any_ptr ptr, Storage* storage) {
  std::stringstream cstr;
  if (ptr.as_int) {
    cstr << handler_names[ptr.node->type] << "-"
        << (ptr.as_int - (uint64_t)storage->region.get_address());
  }
  else {
    cstr << "NULL-0";
  }
  return cstr.str();
}


void dump_char(std::ostream& out, char bit) {
  if (isprint(bit)) {
    out << bit;
  }
  else {
    out << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
}


struct DumpBase {
  virtual void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) = 0;
};


struct NullDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    out << "id: " << dump_id(ptr, storage) << std::endl;
  }
};

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage, int upper);
void dump_node(std::ostream& out, any_ptr ptr, Storage* storage);

struct ValueDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    ValueData *data = ptr.value;
    out << "id: " << dump_id(ptr, storage) << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "value: \"";
    for(size_t i = 0; i < data->size; i++) {
      out << "[";
      dump_char(out, data->value[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    if (data->child) {
      out << "children: " << std::endl;
      out << "  - " << dump_id(data->child, storage) << std::endl;
      out << "---" << std::endl;
      dump_node(out, data->child, storage);
    }
    else {
      out << "children: []" << std::endl;
      out << "---" << std::endl;
    }
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    CompressedData *data = ptr.compressed;
    out << "id: " << dump_id(ptr, storage) << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "keys: \"";
    for(int i = 0; i < data->size; i++) {
      out << "[";
      dump_char(out, data->keys[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << dump_id(data->child, storage) << std::endl;
    out << "---" << std::endl;
    if (data->child)
      dump_node(out, data->child, storage);
  }
};

struct TrieDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    TrieData *data = ptr.trie;
    int size = popcount(data->bits);
    out << "id: " << dump_id(ptr, storage) << std::endl;
    out << "size: " << popcount(data->bits) << std::endl;
    out << "bits: " << std::hex << data->bits << std::dec << std::endl;

    int indizes[17];
    out << "bitindex: [";
    unsigned int bits = data->bits;
    int index = 0, i = 0;
    while(bits){
      index = ctz(bits);
      out << index;
      indizes[i++] = index;
      bits &= ~(1 << index);
      if (bits) {
        out << ", ";
      }
    }
    out << "]" << std::endl;

    if (upper >= 0) {
      upper <<= 4;
      out << "byteindex: [";
      unsigned int bits = data->bits;
      int index = 0;
      while(bits){
        index = ctz(bits);
        out << '"';
        dump_char(out, (char)(upper | index));
        out << '"';
        bits &= ~(1 << index);
        if (bits) {
          out << ", ";
        }
      }
      out << "]" << std::endl;
    }

    out << "children: " << std::endl;
    for(int i = 0; i < size; i++) {
        out << "  - " << dump_id(data->children[i], storage) << std::endl;
    }
    out << "---" << std::endl;
    for(int i = 0; i < size; i++) {
      dump_node(out, data->children[i], storage, indizes[i]);
    }
  }
};

#ifdef BURST
struct TableDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    TableData *data = ptr.table;
    out << "id: " << dump_id(ptr, storage) << std::endl;
    out << "count: " << data->count << std::endl;

    out << "values:" << std::endl;
    std::string val;
    for(int i = 0; i < data->count; i++) {
      DataItem *item = data->get_item(i);
      val.assign(item->key_data, item->key_size);
      out << "  - " << val.c_str() << std::endl;
    }

    out << "children: " << std::endl;
    for(int i = 0; i < data->count; i++) {
        out << "  - " << dump_id(data->get_item(i)->value.resolve(), storage) << std::endl;
    }
    out << "---" << std::endl;
    for(int i = 0; i < data->count; i++) {
      dump_node(out, data->get_item(i)->value.resolve(), storage);
    }
  }
};

TableDumper table_dumper;
#endif

ValueDumper value_dumper;
NullDumper null_dumper;
CompressDumper compress_dumper;
TrieDumper trie_dumper;

DumpBase* dumpers[] = {
  &value_dumper,
  &null_dumper,
  &compress_dumper,
  &trie_dumper,
#ifdef BURST
  &table_dumper
#endif
};

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage, int upper) {
  dumpers[ptr.node->type]->dump(out, ptr, storage, upper);
}

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage) {
  dump_node(out, ptr, storage, -1);
}

#endif


} // namespace leaves
