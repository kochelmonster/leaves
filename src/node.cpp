//  Handlers for all nodes
#include "node.hpp"
#include "port.hpp"
#include <algorithm>
#ifdef DEBUG
#include <ctype.h>
#endif

namespace leaves {

#define MAX_POOL_SIZE (4 + (POOL_COUNT-1) * NODE_INCREMENT)

#define MAX_VAL_SIZE  (MAX_POOL_SIZE - sizeof(ValueData))
#define VDELTA 29


#pragma pack(2)
struct ValueData {
  uint32_t size;
  segment_ptr next;
  char value[];

  static int get_pool_index(size_t size) {
    return size <= 6 ? 0 : (size+VDELTA)/NODE_INCREMENT;
  }

  void free(Transition& self) {
    if (size > MAX_VAL_SIZE)
      self.storage->free(*self.node_ptr);
    else
      self.storage->pools[get_pool_index(size)].free(*self.node_ptr);
  }
};
#pragma pack(0)

struct Value : public NodeHandler {
  ValueData* ptr(const Transition& self) const {
    return (ValueData*)self.node_ptr->resolve(self.storage);
  }

  bool valid() const { return true; }
  virtual Slice get_value(const Transition& self) const {
    ValueData* node(ptr(self));
    return Slice(node->value, node->size);
  }

  segment_ptr* find(Transition& self, Slice& key, string& current_key) {
    ValueData *node(ptr(self));
    if (key.size() && node->next) {
      self.cmp = 1;
      return &node->next;
    }
    return NULL;
  }

  segment_ptr* next(Transition& self, string& current_key) {
    if (self.cmp == 0) {
      ValueData *node(ptr(self));
      self.cmp = 1;
      return node->next ? &node->next : NULL;
    }
    return NULL;
  }

  segment_ptr* first(Transition& self, string& key) {
    self.cmp = 0;
    return NULL;
  }

  segment_ptr* prev(Transition& self, string& current_key) {
    if (self.cmp == 1) {
      ValueData *node(ptr(self));
      self.cmp = 0;
      /* a hack (see trace.imove)
        if we come from node->next this value has to be returned
      */
      return node->next ? self.node_ptr : NULL;
    }
    return NULL;
  }

  segment_ptr* last(Transition& self, string& current_key) {
    ValueData *node(ptr(self));
    self.cmp = 1;
    return node->next ? &node->next : NULL;
  }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value, string& current_key);

  bool remove(Transition& self, bool last) {
    if (last) {
      ValueData* node(ptr(self));
      segment_ptr next = node->next;
      node->free(self);
      *self.node_ptr = next;
      return true;
    }
    // intermediate value
    return false;
  };

  static segment_ptr build(Storage* storage, segment_ptr next, const Slice& value) {
    segment_ptr result;
    size_t size = value.size();
    if (size > MAX_VAL_SIZE)
      result = storage->allocate(size+sizeof(ValueData));
    else
      result = storage->pools[ValueData::get_pool_index(size)].allocate();

    ValueData *node = (ValueData*)result.resolve(storage);
    node->size = value.size();
    memcpy(node->value, value.data(), node->size);
    node->next = next;
    result.type = kValue;
    return result;
  }
};

namespace bit {
  char upper(char value) {
    return value >> 4;
  }

  char lower(char value) {
    return (value & 0x0F);
  }
}

#pragma pack(2)
struct TrieData {
  uint16_t bits;
  segment_ptr children[];

  size_t get_pool_index() {
    size_t count(popcount(bits));
    return count > 2 ? (count+3) / 4 : 0;
  }

  int index_of(int bit) {
    return popcount(bits & ((1<<bit)-1));
  }

  bool full() {
    size_t count(popcount(bits));
    return (count & 3) == 0 || count == 2;
  }

  segment_ptr* find(int bit) {
    return (bits & (1<<bit)) ? &children[index_of(bit)] : NULL;
  }

  segment_ptr* next(char& bit) {
    uint16_t nbits = bits & (0xFFFF << (bit+1));
    if (nbits) {
      bit = ctz(nbits);
      return &children[index_of(bit)];
    }
    return NULL;
  }

  segment_ptr* first(char& bit) {
    bit = ctz(bits);
    return &children[index_of(bit)];
  }

  segment_ptr* prev(char& bit) {
    if (bit) {
      uint16_t nbits = bits & (0xFFFF >> (16-bit));
      if (nbits) {
        bit = 15 - (clz(nbits) & 0xf);
        return &children[index_of(bit)];
      }
    }
    return NULL;
  }

  segment_ptr* last(char& bit) {
    bit = 15 - (clz(bits) & 0xf);
    return &children[index_of(bit)];
  }

  void add(int bit, segment_ptr next) {
    assert(!(bits & 1<<bit));
    bits |= 1 << bit;
    int index = index_of(bit);
    for(int i = popcount(bits)-1; i > index; i--) {
      children[i] = children[i-1];
    }
    children[index] = next;
  }

  segment_ptr insert(Transition& self, segment_ptr* to_me, segment_ptr next, int bit) {
    if (full()) {
      // node must grow
      size_t index = get_pool_index();
      segment_ptr new_ptr = self.storage->pools[index+1].allocate();
      new_ptr.type = kTrie;
      TrieData *new_node((TrieData*)self.resolve(new_ptr));
      memcpy((void*)new_node, this, sizeof(TrieData)+popcount(bits)*sizeof(segment_ptr));
      self.storage->pools[index].free(*to_me);
      *to_me = new_ptr;
      new_node->add(bit, next);
      return new_ptr;
    }

    add(bit, next);
    return *to_me;
  }

  bool remove(Transition& self, segment_ptr* to_me, int bit) {
    assert(bits & (1<<bit));
    size_t pool_index = get_pool_index();
    int index = index_of(bit);

    if (children[index]) {
      // the node is still active => remove of intermediate value
      return false;
    }

    bits &= ~(1<<bit);

    if (!bits) {
      self.storage->pools[pool_index].free(*to_me);
      *to_me = segment_ptr();
      return true;
    }

    int size = popcount(bits);
    for(int i = index; i < size; i++) {
      children[i] = children[i+1];
    }
    if (full()) {
      // we are the upper edge of the next smaller pool
      segment_ptr new_ptr = self.storage->pools[pool_index-1].allocate();
      new_ptr.type = kTrie;
      TrieData *new_node((TrieData*)self.resolve(new_ptr));
      memcpy((void*)new_node, this, sizeof(TrieData)+size*sizeof(segment_ptr));
      self.storage->pools[pool_index].free(*to_me);
      *to_me = new_ptr;
    }
    return false;
  }

};
#pragma pack(0)

struct Trie : public NodeHandler {
  TrieData* ptr1(Transition& self) {
    return (TrieData*)self.resolve(*self.node_ptr);
  }

  TrieData* ptr2(Transition& self) {
    return (TrieData*)self.resolve(*self.second_ptr);
  }

  segment_ptr* find(Transition& self, Slice& key, string& current_key) {
    if (key.empty()) {
      self.cmp = 1;
      return NULL;
    }

    self.cmp = 0;
    char value = self.value = key[0];
    current_key.push_back(value);

    self.second_ptr = ptr1(self)->find(bit::upper(value));
    if (self.second_ptr) {
      assert(self.second_ptr->type == kTrie);
      char lower = bit::lower(value);
      segment_ptr *next(ptr2(self)->find(lower));
      if (next) {
        key = key.advance(1);
        return next;
      }
    }

    return NULL;
  }

  segment_ptr* next(Transition& self, string& current_key) {
    if (self.cmp == 1)
      return self.first(current_key);

    TrieData* node(ptr1(self));
    char upper(bit::upper(self.value)), lower;
    self.second_ptr = node->find(upper);
    if (self.second_ptr) {
      assert(self.second_ptr->type == kTrie);
      lower = bit::lower(self.value);
      segment_ptr* next = ptr2(self)->next(lower);
      if (next) {
        current_key.back() = self.value = (upper << 4) | lower;
        return next;
      }
    }

    self.second_ptr = node->next(upper);
    if (self.second_ptr) {
      segment_ptr* next = ptr2(self)->first(lower);
      current_key.back() = self.value = (upper << 4) | lower;
      return next;
    }

    current_key.pop_back();
    return NULL;
  }

  segment_ptr* first(Transition& self, string& current_key) {
    char upper, lower;
    self.cmp = 0;
    self.second_ptr = ptr1(self)->first(upper);
    segment_ptr *next = ptr2(self)->first(lower);
    current_key.push_back(self.value = (upper << 4) | lower);
    return next;
  }

  segment_ptr* prev(Transition& self, string& current_key) {
    if (self.cmp == 1)
      return NULL;

    TrieData* node(ptr1(self));
    char upper(bit::upper(self.value)), lower;
    self.second_ptr = node->find(upper);
    if (self.second_ptr) {
      assert(self.second_ptr->type == kTrie);
      lower = bit::lower(self.value);
      segment_ptr* next = ptr2(self)->prev(lower);
      if (next) {
        current_key.back() = self.value = (upper << 4) | lower;
        return next;
      }
    }

    self.second_ptr = node->prev(upper);
    if (self.second_ptr) {
      segment_ptr* next = ptr2(self)->last(lower);
      current_key.back() = self.value = (upper << 4) | lower;
      return next;
    }

    current_key.pop_back();
    return NULL;
  }

  segment_ptr* last(Transition& self, string& current_key) {
    char upper, lower;
    self.cmp = 0;
    self.second_ptr = ptr1(self)->last(upper);
    segment_ptr *next = ptr2(self)->last(lower);
    current_key.push_back(self.value = (upper << 4) | lower);
    return next;
  }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value, string& current_key);
  bool remove(Transition& self, bool last);

  static segment_ptr create(Storage* storage, segment_ptr next, int bit) {
    segment_ptr result = storage->pools[0].allocate();
    TrieData* node = (TrieData*)result.resolve(storage);
    node->bits = 1<<bit;
    node->children[0] = next;
    result.type = kTrie;
    return result;
  }

  static segment_ptr build(Storage* storage, segment_ptr next, char key) {
    next = create(storage, next, bit::lower(key));
    next = create(storage, next, bit::upper(key));
    return next;
  }
};


#define MAX_COMPRESSED_LEN (MAX_POOL_SIZE - sizeof(CompressedData))
#define CDELTA  26

#pragma pack(1)
struct CompressedData {
  segment_ptr next;
  unsigned char size;
  char keys[];

  static size_t get_pool_index(size_t size) {
    return size <= 9 ? 0 : (size+CDELTA)/NODE_INCREMENT;
  }

  void free(Transition& self) {
    self.storage->pools[get_pool_index(size)].free(*self.node_ptr);
  }
};
#pragma pack(0)

inline char sign(int x) {
  return (x>0)-(x<0);
}


struct Compressed : public NodeHandler {
  static CompressedData* ptr(Transition& self) {
    return (CompressedData*)self.resolve(*self.node_ptr);
  }

  segment_ptr* find(Transition& self, Slice& key, string& current_key) {
    CompressedData* node = ptr(self);
    size_t size = std::min(key.size(), (size_t)node->size);
    if (!(self.cmp=sign(memcmp(key.data(), node->keys, size))) && size == node->size) {
      key = key.advance(node->size);
      current_key.append(node->keys, node->size);
      return &node->next;
    }
    return NULL;
  }

  segment_ptr* move(Transition& self, string& current_key, bool do_it) {
    if (do_it) {
      self.cmp = 0;
      CompressedData* node = ptr(self);
      current_key.append(node->keys, node->size);
      return &node->next;
    }
    if (self.cmp == 0) {
      self.cmp = 1;
      CompressedData* node = ptr(self);
      current_key.resize(current_key.size()-node->size);
    }
    return NULL;
  }

  segment_ptr* next(Transition& self, string& current_key) {
    return move(self, current_key, self.cmp < 0);
  }

  segment_ptr* first(Transition& self, string& current_key) {
    self.cmp = -1;
    return self.next(current_key);
  }

  segment_ptr* prev(Transition& self, string& current_key) {
    return move(self, current_key, self.cmp > 0);
  }

  segment_ptr* last(Transition& self, string& current_key) {
    self.cmp = 1;
    return self.prev(current_key);
  }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value, string& current_key) {
    CompressedData* node = ptr(self);

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
        Transition trie = Transition(&trie_ptr, self.storage);
        trie.value = key[i];
        Slice rest_key(key.advance(i));
        current_key.push_back(trie.value); // will be popped in trie.insert
        trie_ptr = *trie.insert(rest_key, value, current_key);

        segment_ptr first_ptr(build(self.storage, trie_ptr, first));
        node->free(self);
        *self.node_ptr = first_ptr;
        return self.node_ptr;
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
    return self.node_ptr;
  }

  bool remove(Transition& self, bool last) {
    CompressedData *node(ptr(self));
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
        int index = CompressedData::get_pool_index(size);
        segment_ptr new_ptr(self.storage->pools[index].allocate());
        new_ptr = fill(self.storage, new_ptr, next_node->next, Slice(node->keys, node->size));

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

  static segment_ptr fill(Storage* storage, segment_ptr node_ptr, segment_ptr next,
                          const Slice& key) {
    node_ptr.type = kCompressed;
    CompressedData *node = (CompressedData*)node_ptr.resolve(storage);
    node->next = next;
    node->size = (unsigned char)key.size();
    memcpy(node->keys, key.data(), node->size);
    return node_ptr;
  }

  static segment_ptr build(Storage* storage, segment_ptr next, const Slice& key) {
    if (key.empty())
      return next;

    int index = CompressedData::get_pool_index(key.size());
    if (index > POOL_COUNT) {
      // divide key in multiple compressed
      Slice first(key.data(), MAX_COMPRESSED_LEN);
      Slice second(key.advance(MAX_COMPRESSED_LEN));
      return build(storage, build(storage, next, second), first);
    }

    segment_ptr result(storage->pools[index].allocate());
    return fill(storage, result, next, key);
  }
};

struct Null : public NodeHandler {
  segment_ptr* find(Transition& self, Slice& key, string& current_key) { return NULL; }
  segment_ptr* next(Transition& self, string& current_key) { return NULL; }
  segment_ptr* first(Transition& self, string& current_key) { return NULL; }
  segment_ptr* prev(Transition& self, string& current_key) { return NULL; }
  segment_ptr* last(Transition& self, string& current_key) { return NULL; }
  segment_ptr* insert(Transition& self, Slice& key, const Slice& value, string& current_key) {
    segment_ptr value_ptr(Value::build(self.storage, segment_ptr(), value));
    *self.node_ptr = Compressed::build(self.storage, value_ptr, key);
    return self.node_ptr;
  }
  bool remove(Transition& self, bool last) { return false; } // never
};


static Value value_handler;
static Null null;
static Compressed compressed;
static Trie trie;

NodeHandler* Transition::handlers[] = { &value_handler, &null, &compressed, &trie };


segment_ptr* Trie::insert(Transition& self, Slice& key, const Slice& value, string& current_key) {
  if (self.cmp == 1) {
    // key was empty at find -> insert value key before
    *self.node_ptr = Value::build(self.storage, *self.node_ptr, value);
    return self.node_ptr;
  }

  current_key.pop_back();
  char upper = bit::upper(self.value);
  char lower = bit::lower(self.value);

  segment_ptr next(Value::build(self.storage, segment_ptr(), value));
  if (key.size() > 1) {
    Slice restkey(key.advance(1));
    next = Compressed::build(self.storage, next, restkey);
  }

  TrieData *node(ptr1(self));
  self.second_ptr = node->find(upper);
  if (!self.second_ptr) {
    segment_ptr lower_ptr = create(self.storage, next, lower);
    *self.node_ptr = node->insert(self, self.node_ptr, lower_ptr, upper);
    return self.node_ptr;
  }

  node = ptr2(self);
  *self.second_ptr = node->insert(self, self.second_ptr, next, lower);
  return self.node_ptr;
}


bool Trie::remove(Transition& self, bool last) {
  TrieData *lower(ptr2(self)), *upper;

  if (lower->remove(self, self.second_ptr, bit::lower(self.value))) {
    upper = ptr1(self);
    upper->remove(self, self.node_ptr, bit::upper(self.value));
  }

  lower = ptr2(self);
  upper = ptr1(self);

  if (popcount(upper->bits) == 1 && popcount(lower->bits) == 1) {
    segment_ptr next = lower->children[0];
    char value = (ctz(upper->bits) << 4) | ctz(lower->bits);
    self.storage->pools[lower->get_pool_index()].free(*self.second_ptr);
    self.storage->pools[upper->get_pool_index()].free(*self.node_ptr);
    *self.node_ptr = Compressed::build(self.storage, next, Slice(&value, 1));
    self.remove(last);  // combines compressed if possible
  }
  return true;
}

segment_ptr* Value::insert(Transition& self, Slice& key, const Slice& value, string& current_key) {
  ValueData* node(ptr(self));
  if (key.empty()) {
    segment_ptr new_ptr(build(self.storage, node->next, value));
    node->free(self);
    *self.node_ptr = new_ptr;
    return self.node_ptr;
  }

  assert(!node->next);
  segment_ptr next(build(self.storage, segment_ptr(), value));
  node->next = Compressed::build(self.storage, next, key);
  return self.node_ptr;
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
