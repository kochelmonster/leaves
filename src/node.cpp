//  Handlers for all nodes
#include <algorithm>

#include "node.hpp"
#include "trie.hpp"
#include "trace.hpp"


namespace leaves {

static Trie trie_handler;


struct Value : public NodeHandler {
  bool valid() const { return true; }

  virtual Slice get_value(const Transition& self) const {
    return Slice(self.value->value, self.value->size);
  }

  offset_ptr* find(Transition& self, ISlice& key, string& current_key) {
    if (key.size() && self.value->next) {
      self.cmp = 1;
      return &self.value->next;
    }
    return NULL;
  }

  offset_ptr* next(Transition& self, string& current_key) {
    if (self.cmp == 0) {
      self.cmp = 1;
      return self.value->next ? &self.value->next : NULL;
    }
    return NULL;
  }

  offset_ptr* first(Transition& self, string& key) {
    self.cmp = 0;
    return NULL;
  }

  offset_ptr* prev(Transition& self, string& current_key) {
    if (self.cmp == 1) {
      self.cmp = 0;
      /* a hack (see trace.imove)
        if we come from node->next this value has to be returned
      */
      return self.value->next ? self.node_ptr : NULL;
    }
    return NULL;
  }

  offset_ptr* last(Transition& self, string& current_key) {
    self.cmp = 1;
    return self.value->next ? &self.value->next : NULL;
  }

  int advance(Transition& self, ISlice& key) {
    if ((key.size() && self.value->next) || (!key.size() && ! self.value->next)) {
      self.cmp = 0;
      return 0;
    }
    return -1;
  }

  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
    if (key.empty()) {
      any_ptr new_ptr(ValueData::build(self.trace, self.value->next, value));
      self.trace->free(self.value);
      self.set(new_ptr);
      return;
    }

    assert(!self.value->next);
    any_ptr next(ValueData::build(self.trace, offset_ptr(), value));
    self.value->next = CompressedData::build(self.trace, next, key);
  }

  bool remove(Transition& self, bool last) {
    if (last) {
      *self.node_ptr = self.value->next;
      self.trace->free(self.value);
      return true;
    }
    // intermediate value
    return false;
  };
};

struct Compressed : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key, string& current_key) {
    if (!(self.cmp = self.compressed->find(key, current_key)))
      return &self.compressed->next;
    return NULL;
  }

  offset_ptr* move(Transition& self, string& current_key, bool do_it) {
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

  offset_ptr* next(Transition& self, string& current_key) {
    return move(self, current_key, self.cmp < 0);
  }

  offset_ptr* first(Transition& self, string& current_key) {
    self.cmp = -1;
    return self.next(current_key);
  }

  offset_ptr* prev(Transition& self, string& current_key) {
    return move(self, current_key, self.cmp > 0);
  }

  offset_ptr* last(Transition& self, string& current_key) {
    self.cmp = 1;
    return self.prev(current_key);
  }

  int advance(Transition& self, ISlice& key) {
    CompressedData* node = self.compressed;
    if (node->size <= key.size() && !memcmp(node->keys, key.data(), node->size)) {
        self.cmp = 0;
        key.iadvance(node->size);
        return node->size;
    }
    return -1;
  }

  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
    CompressedData* node = self.compressed;

    int size = std::min((size_t)node->size, key.size());
    for(int i = 0; i < size; i++) {
      if (node->keys[i] != key[i]) {
        /* divide compressed:
           node = [abcdefg]
           key = [abhij]
                         first   trie    rest
           [abcdefg] ==> [ab] -> |c| -> [efg]
                                 |h| -> [ij]
        */
        Slice first(key.data(), i);
        Slice rest(node->keys+i+1, node->size-i-1);

        // build trie node
        any_ptr rest_ptr = CompressedData::build(self.trace, node->next, rest);
        any_ptr trie_ptr = TrieData::build(self.trace, rest_ptr, node->keys[i]);
        self.trace->free(node);

        self.set(trie_ptr);
        trie_handler.ifind(self, key[i]);
        ISlice rest_key(key.advance(i));
        current_key.push_back(self.key); // will be popped in trie.insert
        self.insert(rest_key, value, current_key);

        self.set(CompressedData::build(self.trace, trie_ptr, first));
        return;
      }
    }

    /* key is a substring of node
       node = [abcdefg]
       key = [abhij]
                     first  value   rest
       [abcdefg] ==> [ab] -> [] -> [cefg]
    */
    Slice first(key.data(), size);
    Slice rest(node->keys+size, node->size-size);
    any_ptr rest_ptr = CompressedData::build(self.trace, node->next.resolve(), rest);
    any_ptr value_ptr = ValueData::build(self.trace, rest_ptr, value);
    any_ptr first_ptr = CompressedData::build(self.trace, value_ptr, first);

    self.trace->free(node);
    self.set(first_ptr);
  }

  bool remove(Transition& self, bool last) {
    CompressedData *node = self.compressed;
    if (!node->next) {
      self.trace->free(node);
      *self.node_ptr = offset_ptr();
      return true;
    }

    CompressedData *child = node->next.resolve().compressed;
    if (child->type == kCompressed) {
      std::string tmp;
      tmp.reserve(node->size + child->size);
      tmp.append(node->keys, node->size);
      tmp.append(child->keys, child->size);
      any_ptr result = CompressedData::build(self.trace, child->next, tmp);
      self.trace->free(node);
      self.trace->free(child);
      self.set(result);
    }

    return true;
  }
};

struct Null : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key, string& current_key) { return NULL; }
  offset_ptr* next(Transition& self, string& current_key) { return NULL; }
  offset_ptr* first(Transition& self, string& current_key) { return NULL; }
  offset_ptr* prev(Transition& self, string& current_key) { return NULL; }
  offset_ptr* last(Transition& self, string& current_key) { return NULL; }
  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
    offset_ptr value_ptr(ValueData::build(self.trace, offset_ptr(), value));
    self.set(CompressedData::build(self.trace, value_ptr, key));
  }
  int advance(Transition& self, ISlice& key) { return -1; } // never
  bool remove(Transition& self, bool last) { return false; } // never
};


static Value value_handler;
static Null null_handler;
static Compressed compressed_handler;

NodeHandler* Transition::handlers[] = {
  &value_handler, &null_handler, &compressed_handler, &trie_handler };


any_ptr ValueData::build(Trace* trace, const offset_ptr& next, const Slice& value) {
  any_ptr result(trace->allocate(value.size()+sizeof(ValueData)).type(kValue));
  result.value->size = value.size();
  memcpy(result.value->value, value.data(), value.size());
  result.value->next = next;
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

  any_ptr result(trace->allocate(key.size()+sizeof(CompressedData)).type(kCompressed));
  result.compressed->fill(next, key);
  return result;
}




#ifdef DEBUG

const char* handler_names[] = {
  "kValue",
  "kNull",
  "kCompressed",
  "kTrie",
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

ValueDumper value_dumper;
NullDumper null_dumper;
CompressDumper compress_dumper;
TrieDumper trie_dumper;

DumpBase* dumpers[] = {
  &value_dumper,
  &null_dumper,
  &compress_dumper,
  &trie_dumper
};

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage, int upper) {
  dumpers[ptr.node->type]->dump(out, ptr, storage, upper);
}

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage) {
  dump_node(out, ptr, storage, -1);
}

#endif


} // namespace leaves
