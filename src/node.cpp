//  Handlers for all nodes
#include <algorithm>
#ifdef DEBUG
#include <sstream>
#endif

#include "node.hpp"
#include "trie.hpp"
#ifndef PURE_TRIE
#include "table.hpp"
#endif
#include "trace.hpp"

namespace leaves {


struct Value : public NodeHandler {
  bool valid() const { return true; }

  offset_ptr* find(Transition& self, ISlice& key, KeyString& current_key) {
    if (key.size()) {
      self.cmp = 1;
      if (self.value->next)
        return &self.value->next;
    }
    else
      self.cmp = 0;
    return NULL;
  }

  offset_ptr* first(Transition& self, KeyString& current_key) {
    self.cmp = 0;
    return NULL;
  }

  offset_ptr* next(Transition& self, KeyString& current_key) {
    if (self.cmp == 0) {
      self.cmp = 1;
      return self.value->next ? &self.value->next : NULL;
    }
    return NULL;
  }

  offset_ptr* prev(Transition& self, KeyString& current_key) {
    if (self.cmp == 1) {
      self.cmp = 0;
      return self.node_ptr;  // see trace.imove
    }
    return NULL;
  }

  offset_ptr* last(Transition& self, KeyString& current_key) {
    if (self.value->next) {
      self.cmp = 1;
      return &self.value->next;
    }
    self.cmp = 0;
    return NULL;
  }

  int advance(Transition& self, const Slice& key) {
    if (key.empty()) {
      self.cmp = 0;
      return -1;
    }
    return 0;
  }

  void insert(Transition& self, const Slice& key, any_ptr val_ptr) {
    if (key.empty()) {
      val_ptr.value->next = self.value->next;
      self.trace->free(self.value);
      self.set(val_ptr);
      return;
    }

    assert(!self.value->next);
#ifdef PURE_TRIE
    self.value->next = CompressedData::build(self.trace, val_ptr, key);
#else
    self.value->next = TableData::build(self.trace, val_ptr, key);
#endif
  }

  bool remove(Transition& self) { assert(0); return false; };

  void report(offset_ptr* node, Stats& stats) {
    ValueData* value = node->resolve().value;
    if (value->next) {
      stats.intermediate_nodes++;
      Transition::handlers[value->next.resolve().node->type]->report(&value->next, stats);
    }
    else
      stats.end_nodes++;
  }
};

struct Compressed : public NodeHandler {
  offset_ptr* move(Transition& self, KeyString& current_key, bool do_it) {
    CompressedData* node = self.compressed;
    if (do_it) {
      self.cmp = 0;
      current_key.append(node->keys, node->size);
      return &node->next;
    }
    if (self.cmp == 0) {
      self.cmp = 1;
      current_key.resize(current_key.size()-node->size);
    }
    return NULL;
  }

  offset_ptr* next(Transition& self, KeyString& current_key) {
    return move(self, current_key, self.cmp < 0);
  }

  offset_ptr* first(Transition& self, KeyString& current_key) {
    self.cmp = -1;
    return self.next(current_key);
  }

  offset_ptr* prev(Transition& self, KeyString& current_key) {
    return move(self, current_key, self.cmp > 0);
  }

  offset_ptr* last(Transition& self, KeyString& current_key) {
    self.cmp = 1;
    return self.prev(current_key);
  }

  bool remove(Transition& self) {
    CompressedData *node = self.compressed;
    if (!node->next) {
      self.trace->free(node);
      *self.node_ptr = offset_ptr();
      return true;
    }
    node->eat_child(self);
    return true;
  }

  offset_ptr* find(Transition& self, ISlice& key, KeyString& current_key) {
    if (!(self.cmp = self.compressed->find(key))) {
      key.iadvance(self.compressed->size);
      current_key.append(self.compressed->keys, self.compressed->size);
      return &self.compressed->next;
    }
    return NULL;
  }

  int advance(Transition& self, const Slice& key) {
    CompressedData* node = self.compressed;
    if (node->size <= key.size() && !memcmp(node->keys, key.data(), node->size)) {
      self.cmp = 0;
      return node->size;
    }
    return -1;
  }

  void insert(Transition& self, const Slice& key, any_ptr val_ptr) {
    assert(val_ptr.node->type == kValue);
    CompressedData *node = self.compressed;
    size_t size = std::min((size_t)node->size, key.size());
    char* keys = node->keys;
    Slice rest(keys, node->size);

    for(size_t i = 0; i < size; i++) {
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
        any_ptr rest_ptr = CompressedData::build(self.trace, node->next, rest.advance(i+1));
        any_ptr trie_ptr = TrieData::build(self.trace, rest_ptr, keys[i]);

        self.set(trie_ptr);
        trie_handler.ifind(self, key[i]);
        self.insert(key.advance(i), val_ptr);

        self.set(CompressedData::build(self.trace, trie_ptr, first));
        self.trace->free(node);
        return;
      }
    }

    /* insert_compressed_short: (key is a substring of node)
       node = [abcdefg]
       key = [abhij]
                     first  Leaf   rest
       [abcdefg] ==> [ab] -> [] -> [cefg]
    */
    assert(key.size() < node->size);
    val_ptr.value->next = CompressedData::build(self.trace, node->next, rest.advance(key.size()));
    val_ptr = CompressedData::build(self.trace, val_ptr, key);
    self.set(val_ptr);
    self.trace->free(node);
  }

  void report(offset_ptr* node, Stats& stats) {
    CompressedData* value = node->resolve().compressed;
    stats.intermediate_nodes++;
    Transition::handlers[value->next.resolve().node->type]->report(&value->next, stats);
    stats.compressed_nodes++;
  }
};


struct Null : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key, KeyString& current_key) { return NULL; }
  offset_ptr* next(Transition& self, KeyString& current_key) { return NULL; }
  offset_ptr* first(Transition& self, KeyString& current_key) { return NULL; }
  offset_ptr* prev(Transition& self, KeyString& current_key) { return NULL; }
  offset_ptr* last(Transition& self, KeyString& current_key) { return NULL; }
  void insert(Transition& self, const Slice& key, any_ptr val_ptr) {
#ifdef PURE_TRIE
    self.set(CompressedData::build(self.trace, val_ptr, key));
#else
    self.set(TableData::build(self.trace, val_ptr, key));
#endif
  }
  int advance(Transition& self, const Slice& key) { assert(0); return -1; }
  bool remove(Transition& self) { assert(0); return false; }
};


static Value value_handler;
static Null null_handler;
static Compressed compressed_handler;


NodeHandler* Transition::handlers[] = {
  &value_handler, &null_handler, &compressed_handler, &trie_handler,
#ifndef PURE_TRIE
  &table_handler
#endif
};


any_ptr ValueData::build(Trace* trace, const Slice& value) {
  any_ptr result(trace->allocate(value.size()+sizeof(ValueData)));
  result.value->type = kValue;
  result.value->size = value.size();
  memcpy(result.value->value, value.data(), value.size());
  result.value->next = offset_ptr();
  return result;
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
  CompressedData *child = next.resolve().compressed;
  if (child->type == kCompressed) {
    string tmp;
    tmp.reserve(size + child->size);
    tmp.append(keys, size);
    tmp.append(child->keys, child->size);
    any_ptr result = CompressedData::build(self.trace, child->next, tmp);
    self.trace->free(child);
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
    if (data->next) {
      out << "children: " << std::endl;
      out << "  - " << dump_id(data->next, storage) << std::endl;
      out << "---" << std::endl;
      dump_node(out, data->next, storage);
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
    out << "  - " << dump_id(data->next, storage) << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
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

#ifndef PURE_TRIE
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
#ifndef PURE_TRIE
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
