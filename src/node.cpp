//  Handlers for all nodes
#include <algorithm>

#include "node.hpp"
#include "trie.hpp"


namespace leaves {

struct Value : public NodeHandler {
  bool valid() const { return true; }

  virtual Slice get_value(const Transition& self) const {
    return Slice(self.value->value, self.value->size);
  }

  segment_ptr* find(Transition& self, ISlice& key, string& current_key) {
    if (key.size() && self.value->next) {
      self.cmp = 1;
      return &self.value->next;
    }
    return NULL;
  }

  segment_ptr* next(Transition& self, string& current_key) {
    if (self.cmp == 0) {
      self.cmp = 1;
      return self.value->next ? &self.value->next : NULL;
    }
    return NULL;
  }

  segment_ptr* first(Transition& self, string& key) {
    self.cmp = 0;
    return NULL;
  }

  segment_ptr* prev(Transition& self, string& current_key) {
    if (self.cmp == 1) {
      self.cmp = 0;
      /* a hack (see trace.imove)
        if we come from node->next this value has to be returned
      */
      return self.value->next ? self.node_ptr : NULL;
    }
    return NULL;
  }

  segment_ptr* last(Transition& self, string& current_key) {
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
      resolved_ptr new_ptr(ValueData::build(self.storage, self.value->next, value));
      self.storage->free(resolved_ptr(*self.node_ptr, self.value), self.value->size_of());
      self.set(new_ptr);
      return;
    }

    assert(!self.value->next);
    resolved_ptr next(ValueData::build(self.storage, segment_ptr(), value));
    self.value->next = CompressedData::build(self.storage, next, key);
  }

  bool remove(Transition& self, bool last) {
    if (last) {
      segment_ptr next = self.value->next;
      self.storage->free(resolved_ptr(*self.node_ptr, self.value),
        self.value->size+sizeof(ValueData));
      self.set(next);
      return true;
    }
    // intermediate value
    return false;
  };
};


inline char sign(int x) {
  return (x>0)-(x<0);
}


struct Compressed : public NodeHandler {
  static CompressedData* ptr(Transition& self) {
    return self.node_ptr->resolve(self.storage).compressed;
  }

  segment_ptr* find(Transition& self, ISlice& key, string& current_key) {
    CompressedData* node = self.compressed = ptr(self);
    size_t size = std::min(key.size(), (size_t)node->size);
    if (!(self.cmp=sign(memcmp(key.data(), node->keys, size))) && size == node->size) {
      key.iadvance(node->size);
      current_key.append(node->keys, node->size);
      return &node->next;
    }
    return NULL;
  }

  segment_ptr* move(Transition& self, string& current_key, bool do_it) {
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

  segment_ptr* next(Transition& self, string& current_key) {
    return move(self, current_key, self.cmp < 0);
  }

  segment_ptr* first(Transition& self, string& current_key) {
    self.cmp = -1;
    self.compressed = ptr(self);
    return self.next(current_key);
  }

  segment_ptr* prev(Transition& self, string& current_key) {
    return move(self, current_key, self.cmp > 0);
  }

  segment_ptr* last(Transition& self, string& current_key) {
    self.cmp = 1;
    self.compressed = ptr(self);
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

  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key);

  bool remove(Transition& self, bool last) {
    CompressedData *node = self.compressed;
    if (!node->next) {
      node->free(self);
      *self.node_ptr = segment_ptr();
      return true;
    }

    if (node->next.type == kCompressed) {
      Transition next(&node->next, self.storage);
      CompressedData *next_node(ptr(next));
      size_t size = (size_t)node->size + (size_t)next_node->size;

      if (size < MAX_COMPRESSED_LEN) {
        resolved_ptr nptr(self.storage->allocate(size+sizeof(CompressedData)).type(kCompressed));

        CompressedData *new_node(nptr.compressed);
        new_node->fill(next_node->next, Slice(node->keys, node->size));
        memcpy(new_node->keys+node->size, next_node->keys, next_node->size);
        new_node->size += next_node->size;

        next_node->free(next);
        node->free(self);

        self.set(nptr);
      }
    }

    return true;
  }
};

struct Null : public NodeHandler {
  segment_ptr* find(Transition& self, ISlice& key, string& current_key) { return NULL; }
  segment_ptr* next(Transition& self, string& current_key) { return NULL; }
  segment_ptr* first(Transition& self, string& current_key) { return NULL; }
  segment_ptr* prev(Transition& self, string& current_key) { return NULL; }
  segment_ptr* last(Transition& self, string& current_key) { return NULL; }
  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
    resolved_ptr value_ptr(ValueData::build(self.storage, segment_ptr(), value));
    self.set(CompressedData::build(self.storage, value_ptr, key));
  }
  int advance(Transition& self, ISlice& key) { return -1; } // never
  bool remove(Transition& self, bool last) { return false; } // never
};


static Value value_handler;
static Null null_handler;
static Compressed compressed_handler;
static Trie trie_handler;

NodeHandler* Transition::handlers[] = {
  &value_handler, &null_handler, &compressed_handler, &trie_handler };



void Compressed::insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
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
      resolved_ptr rest_ptr = CompressedData::build(self.storage, self.resolve(node->next), rest);
      resolved_ptr trie_ptr = TrieData::build(self.storage, rest_ptr.me, node->keys[i]);
      Transition trie(&trie_ptr, self.storage);
      trie_handler.ifind(trie, key[i]);

      ISlice rest_key(key.advance(i));
      current_key.push_back(trie.key); // will be popped in trie.insert
      trie.insert(rest_key, value, current_key);

      resolved_ptr first_ptr(CompressedData::build(self.storage, trie_ptr, first));
      node->free(self);
      self.set(first_ptr);
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
  resolved_ptr rest_ptr = CompressedData::build(self.storage, self.resolve(node->next), rest);
  resolved_ptr value_ptr = ValueData::build(self.storage, rest_ptr.me, value);
  resolved_ptr first_ptr = CompressedData::build(self.storage, value_ptr, first);

  node->free(self);
  self.set(first_ptr);
}



#ifdef DEBUG

const char* handler_names[] = {
  "kValue",
  "kNull",
  "kCompressed",
  "kTrie",
};


std::ostream& operator<<(std::ostream& out, segment_ptr ptr) {
  out << handler_names[ptr.type] << "-" << ptr.segment_id << "-" << ptr.delta;
  return out;
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
  virtual void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) = 0;
};


struct NullDumper : public DumpBase {
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    out << "id: " << ptr << std::endl;
  }
};

void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage, int upper);
void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage);

struct ValueDumper : public DumpBase {
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    ValueData *data = ptr.resolve(storage).value;
    out << "id: " << ptr << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "value: \"";
    for(size_t i = 0; i < data->size; i++) {
      out << "[";
      dump_char(out, data->value[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << data->next << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    CompressedData *data = ptr.resolve(storage).compressed;
    out << "id: " << ptr << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "keys: \"";
    for(int i = 0; i < data->size; i++) {
      out << "[";
      dump_char(out, data->keys[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << data->next << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
  }
};

struct TrieDumper : public DumpBase {
  void dump(std::ostream& out, segment_ptr ptr, Storage* storage, int upper=-1) {
    TrieData *data = ptr.resolve(storage).trie;
    int size = popcount(data->bits);
    out << "id: " << ptr << std::endl;
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
        out << "  - " << data->children[i] << std::endl;
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

void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage, int upper) {
  dumpers[ptr.type]->dump(out, ptr, storage, upper);
}

void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage) {
  dump_node(out, ptr, storage, -1);
}

#endif


} // namespace leaves
