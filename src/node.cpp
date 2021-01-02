//  Handlers for all nodes
#include "node.hpp"
#include "port.hpp"
#include <algorithm>

namespace leaves {

typedef unsigned char uchar_t;


struct Value : public NodeHandler {
  static segment_ptr build(Storage* storage, segment_ptr next, const Slice& value) {
    resolved_ptr result(storage->allocate(value.size()+sizeof(ValueData)));
    result.value->size = value.size();
    memcpy(result.value->value, value.data(), value.size());
    result.value->next = next;
    return result.me;
  }

  ValueData* ptr(const Transition& self) const {
    return (ValueData*)self.node_ptr->resolve(self.storage);
  }

  bool valid() const { return true; }

  virtual Slice get_value(const Transition& self) const {
    return Slice(self.value->value, self.value->size);
  }

  segment_ptr* find(Transition& self, ISlice& key, string& current_key) {
    self.value = ptr(self);
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
    self.value = ptr(self);
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
    self.value = ptr(self);
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

  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key);

  bool remove(Transition& self, bool last) {
    if (last) {
      segment_ptr next = self.value->next;
      self.storage->free(
        resolved_ptr(*self.node_ptr, self.value, self.storage),
        self.value->size+sizeof(ValueData));
      *self.node_ptr = next;
      return true;
    }
    // intermediate value
    return false;
  };
};

namespace bit {
  uchar_t upper(uchar_t value) {
    return value >> 4;
  }

  uchar_t lower(uchar_t value) {
    return (value & 0x0F);
  }
}

#pragma pack(2)
struct TrieData {
  uint16_t bits;
  segment_ptr children[];

  int index_of(int bit) {
    return index_of_moved(1<<bit);
  }

  int index_of_moved(int moved_bit) {
    return popcount(bits & (moved_bit-1));
  }

  bool full(size_t count) {
    return (count & 3) == 0 || count == 2;
  }

  segment_ptr* find(int bit) {
    int moved_bit = 1 << bit;
    return (bits & moved_bit) ? &children[index_of_moved(moved_bit)] : NULL;
  }

  segment_ptr* next(uchar_t& bit) {
    uint16_t nbits = bits & (0xFFFF << (bit+1));
    if (nbits) {
      bit = ctz(nbits);
      return &children[index_of(bit)];
    }
    return NULL;
  }

  segment_ptr* first(uchar_t& bit) {
    bit = ctz(bits);
    return &children[index_of(bit)];
  }

  segment_ptr* prev(uchar_t& bit) {
    if (bit) {
      uint16_t nbits = bits & (0xFFFF >> (16-bit));
      if (nbits) {
        bit = 15 - (clz(nbits) & 0xf);
        return &children[index_of(bit)];
      }
    }
    return NULL;
  }

  segment_ptr* last(uchar_t& bit) {
    bit = 15 - (clz(bits) & 0xf);
    return &children[index_of(bit)];
  }

  void add(int bit, segment_ptr next) {
    int moved_bit = 1 << bit;
    assert(!(bits & moved_bit));
    bits |= moved_bit;
    int index = index_of_moved(moved_bit);
    for(int i = popcount(bits)-1; i > index; i--) {
      children[i] = children[i-1];
    }
    children[index] = next;
  }

  segment_ptr insert(Transition& self, segment_ptr* to_me, segment_ptr next, int bit) {
    size_t count = popcount(bits);
    if (full(count)) {
      // node must grow
      size_t size = count * sizeof(segment_ptr) + sizeof(TrieData);
      resolved_ptr new_ptr = self.storage->allocate(size+sizeof(segment_ptr));
      new_ptr.me.type = kTrie;
      memcpy((void*)new_ptr.trie, this, size);
      self.storage->free(resolved_ptr(*to_me, this, self.storage), size);
      *to_me = new_ptr;
      new_ptr.trie->add(bit, next);
      return new_ptr.me;
    }

    add(bit, next);
    return *to_me;
  }

  bool remove(Transition& self, segment_ptr* to_me, TrieData** dest, int bit) {
    assert(bits & (1<<bit));
    int index = index_of(bit);

    if (children[index]) {
      // the node is still active => remove of intermediate value
      return false;
    }

    bits &= ~(1<<bit);

    if (!bits) {
      // has been shrunken to pool 0 for shure
      self.storage->pools[0].free(resolved_ptr(*to_me, this, self.storage));
      *to_me = segment_ptr();
      return true;
    }

    size_t count = popcount(bits);
    for(size_t i = index; i < count; i++) {
      children[i] = children[i+1];
    }

    if (full(count)) {
      // shrink
      size_t size = count * sizeof(segment_ptr) + sizeof(TrieData);
      resolved_ptr new_ptr = self.storage->allocate(size);
      new_ptr.me.type = kTrie;
      memcpy((void*)new_ptr.trie, this, size);
      *dest = new_ptr.trie;
      self.storage->free(resolved_ptr(*to_me, this, self.storage), size+sizeof(segment_ptr));
      *to_me = new_ptr;
    }
    return false;
  }

};
#pragma pack(0)

struct Trie : public NodeHandler {
  TrieData* ptr1(Transition& self) {
    return (TrieData*)self.node_ptr->resolve(self.storage);
  }

  TrieData* ptr2(Transition& self) {
    return (TrieData*)self.resolve(*self.second_ptr);
  }

  segment_ptr* find(Transition& self, ISlice& key, string& current_key) {
    if (key.empty()) {
      self.cmp = 1;
      return NULL;
    }

    segment_ptr *result = ifind(self, key[0]);
    current_key.push_back(self.key);
    if (result)
      key.iadvance(1);

    return result;
  }

  segment_ptr* ifind(Transition& self, char key) {
    self.cmp = 0;
    char value = self.key = key;
    self.upper = ptr1(self);
    self.second_ptr = self.upper->find(bit::upper(value));
    if (self.second_ptr) {
      assert(self.second_ptr->type == kTrie);
      uchar_t lower = bit::lower(value);
      self.lower = ptr2(self);
      return self.lower->find(lower);
    }
    else
      self.lower = NULL;
    return NULL;
  }

  segment_ptr* next(Transition& self, string& current_key) {
    if (self.cmp == 1)
      return self.first(current_key);

    uchar_t upper(bit::upper(self.key)), lower;
    if (self.lower) {
      assert(self.second_ptr->type == kTrie);
      lower = bit::lower(self.key);
      segment_ptr* next = self.lower->next(lower);
      if (next) {
        current_key.back() = self.key = (upper << 4) | lower;
        return next;
      }
    }

    self.second_ptr = self.upper->next(upper);
    if (self.second_ptr) {
      self.lower = ptr2(self);
      segment_ptr* next = self.lower->first(lower);
      current_key.back() = self.key = (upper << 4) | lower;
      return next;
    }

    current_key.pop_back();
    return NULL;
  }

  segment_ptr* first(Transition& self, string& current_key) {
    uchar_t upper, lower;
    self.cmp = 0;
    self.upper = ptr1(self);
    self.second_ptr = self.upper->first(upper);
    self.lower = ptr2(self);
    segment_ptr *next = self.lower->first(lower);
    current_key.push_back(self.key = (upper << 4) | lower);
    return next;
  }

  segment_ptr* prev(Transition& self, string& current_key) {
    if (self.cmp == 1)
      return NULL;

    uchar_t upper(bit::upper(self.key)), lower;
    if (self.lower) {
      assert(self.second_ptr->type == kTrie);
      lower = bit::lower(self.key);
      segment_ptr* next = self.lower->prev(lower);
      if (next) {
        current_key.back() = self.key = (upper << 4) | lower;
        return next;
      }
    }

    self.second_ptr = self.upper->prev(upper);
    if (self.second_ptr) {
      self.lower = ptr2(self);
      segment_ptr* next = self.lower->last(lower);
      current_key.back() = self.key = (upper << 4) | lower;
      return next;
    }

    current_key.pop_back();
    return NULL;
  }

  segment_ptr* last(Transition& self, string& current_key) {
    uchar_t upper, lower;
    self.cmp = 0;
    self.upper = ptr1(self);
    self.second_ptr = self.upper->last(upper);
    self.lower = ptr2(self);
    segment_ptr *next = self.lower->last(lower);
    current_key.push_back(self.key = (upper << 4) | lower);
    return next;
  }

  int advance(Transition& self, ISlice& key) {
    if (key.size() && key[0] == self.key) {
      self.cmp = 0;
      key.iadvance(1);
      return 1;
    }
    return -1;
  }

  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key);
  bool remove(Transition& self, bool last);

  static segment_ptr create(Storage* storage, segment_ptr next, int bit) {
    resolved_ptr result = storage->pools[0].allocate();
    result.trie->bits = 1<<bit;
    result.trie->children[0] = next;
    result.me.type = kTrie;
    return result.me;
  }

  static segment_ptr build(Storage* storage, segment_ptr next, char key) {
    next = create(storage, next, bit::lower(key));
    next = create(storage, next, bit::upper(key));
    return next;
  }
};


#define MAX_COMPRESSED_LEN (MAX_POOL_SIZE - sizeof(CompressedData))


#pragma pack(1)
struct CompressedData {
  segment_ptr next;
  unsigned char size;
  char keys[];

  void free(Transition& self) {
    self.storage->free(
      resolved_ptr(*self.node_ptr, this, self.storage), size+sizeof(CompressedData));
  }
};
#pragma pack(0)

inline char sign(int x) {
  return (x>0)-(x<0);
}


struct Compressed : public NodeHandler {
  static CompressedData* ptr(Transition& self) {
    return (CompressedData*)self.node_ptr->resolve(self.storage);
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
        resolved_ptr nptr(self.storage->allocate(size+sizeof(CompressedData)));
        segment_ptr new_ptr = fill(
          self.storage, nptr, next_node->next, Slice(node->keys, node->size));

        CompressedData *new_node((CompressedData*)self.resolve(new_ptr));
        memcpy(new_node->keys+node->size, next_node->keys, next_node->size);
        new_node->size += next_node->size;

        next_node->free(next);
        node->free(self);

        *self.node_ptr = new_ptr;
      }
    }

    return true;
  }

  static segment_ptr fill(Storage* storage, resolved_ptr& node_ptr, segment_ptr next,
                          const Slice& key) {
    node_ptr.me.type = kCompressed;
    node_ptr.compressed->next = next;
    node_ptr.compressed->size = (unsigned char)key.size();
    memcpy(node_ptr.compressed->keys, key.data(), key.size());
    return node_ptr.me;
  }

  static segment_ptr build(Storage* storage, segment_ptr next, const Slice& key) {
    if (key.empty())
      return next;

    if (key.size() > MAX_COMPRESSED_LEN) {
      // divide key in multiple compressed
      Slice first(key.data(), MAX_COMPRESSED_LEN);
      Slice second(key.advance(MAX_COMPRESSED_LEN));
      return build(storage, build(storage, next, second), first);
    }

    resolved_ptr result(storage->allocate(key.size()+sizeof(CompressedData)));
    return fill(storage, result, next, key);
  }
};

struct Null : public NodeHandler {
  segment_ptr* find(Transition& self, ISlice& key, string& current_key) { return NULL; }
  segment_ptr* next(Transition& self, string& current_key) { return NULL; }
  segment_ptr* first(Transition& self, string& current_key) { return NULL; }
  segment_ptr* prev(Transition& self, string& current_key) { return NULL; }
  segment_ptr* last(Transition& self, string& current_key) { return NULL; }
  void insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
    segment_ptr value_ptr(Value::build(self.storage, segment_ptr(), value));
    *self.node_ptr = Compressed::build(self.storage, value_ptr, key);
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


void Trie::insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
  if (self.cmp == 1) {
    // key was empty at find -> insert value key before
    *self.node_ptr = Value::build(self.storage, *self.node_ptr, value);
    return;
  }

  current_key.pop_back();
  uchar_t upper = bit::upper(self.key);
  uchar_t lower = bit::lower(self.key);

  segment_ptr next(Value::build(self.storage, segment_ptr(), value));
  if (key.size() > 1) {
    Slice restkey(key.advance(1));
    next = Compressed::build(self.storage, next, restkey);
  }

  if (!self.lower) {
    segment_ptr lower_ptr = create(self.storage, next, lower);
    *self.node_ptr = self.upper->insert(self, self.node_ptr, lower_ptr, upper);
    return;
  }

  *self.second_ptr = self.lower->insert(self, self.second_ptr, next, lower);
}


bool Trie::remove(Transition& self, bool last) {
  if (self.lower->remove(self, self.second_ptr, &self.lower, bit::lower(self.key))) {
    self.upper->remove(self, self.node_ptr, &self.upper, bit::upper(self.key));
  }

  if (popcount(self.upper->bits) == 1 && popcount(self.lower->bits) == 1) {
    segment_ptr next = self.lower->children[0];
    char value = (ctz(self.upper->bits) << 4) | ctz(self.lower->bits);
    self.storage->pools[0].free(resolved_ptr(*self.second_ptr, self.lower, self.storage));
    self.storage->pools[0].free(resolved_ptr(*self.node_ptr, self.upper, self.storage));
    *self.node_ptr = Compressed::build(self.storage, next, Slice(&value, 1));
    self.remove(last);  // combines compressed if possible
  }
  return true;
}


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
      segment_ptr rest_ptr = build(self.storage, node->next, rest);
      segment_ptr trie_ptr = Trie::build(self.storage, rest_ptr, node->keys[i]);
      Transition trie(&trie_ptr, self.storage);
      trie_handler.ifind(trie, key[i]);

      ISlice rest_key(key.advance(i));
      current_key.push_back(trie.key); // will be popped in trie.insert
      trie.insert(rest_key, value, current_key);

      segment_ptr first_ptr(build(self.storage, trie_ptr, first));
      node->free(self);
      *self.node_ptr = first_ptr;
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
  segment_ptr rest_ptr = build(self.storage, node->next, rest);
  segment_ptr value_ptr = Value::build(self.storage, rest_ptr, value);
  segment_ptr first_ptr = build(self.storage, value_ptr, first);

  node->free(self);
  *self.node_ptr = first_ptr;
}


void Value::insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
  if (key.empty()) {
    segment_ptr new_ptr(build(self.storage, self.value->next, value));
    self.storage->free(
      resolved_ptr(*self.node_ptr, self.value, self.storage), self.value->size+sizeof(ValueData));
    *self.node_ptr = new_ptr;
    return;
  }

  assert(!self.value->next);
  segment_ptr next(build(self.storage, segment_ptr(), value));
  self.value->next = Compressed::build(self.storage, next, key);
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
    ValueData *data = (ValueData*)ptr.resolve(storage);
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
    CompressedData *data = (CompressedData*)ptr.resolve(storage);
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
    TrieData *data = (TrieData*)ptr.resolve(storage);
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
