//  Handlers for all nodes
#include "node.hpp"
#include "port.hpp"
#include <algorithm>


namespace larch_leaves {

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

  segment_ptr* find(Transition& self, Slice& key) {
    ValueData *node(ptr(self));
    if (key.size() && node->next)
      return &node->next;
    return NULL;
  }

  segment_ptr* last(Transition& self) {
    ValueData *node(ptr(self));
    if (node->next)
      return &node->next;
    return self.node_ptr;
  }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value);

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
    return (popcount(bits)+3) / 4;
  }

  int index_of(int bit) {
    return popcount(bits & ((1<<bit)-1));
  }

  bool full() {
    return (popcount(bits) & 3) == 0;
  }

  segment_ptr* find(int bit) {
    return (bits & (1<<bit)) ? &children[index_of(bit)] : NULL;
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
      return *to_me;
    }

    add(bit, next);
    return *to_me;
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

  void append_to(Transition& self, std::string& key) { key.push_back(self.value); }

  segment_ptr* find(Transition& self, Slice& key) {
    segment_ptr* result = ifind(self, key);
    if (result) {
      self.value = key[0];
      key = key.advance(1);
    }
    else
      self.value = 0;
    return result;
  }

  segment_ptr* ifind(Transition& self, Slice& key) {
    if (key.empty()) {
      return NULL;
    }

    char value = self.value = key[0];
    char lower = bit::lower(value);
    self.second_ptr = ptr1(self)->find(bit::upper(value));
    if (self.second_ptr) {
      assert(self.second_ptr->type == kTrie);
      return ptr2(self)->find(lower);
    }

    return NULL;
  }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value);

  static segment_ptr create(Storage* storage, segment_ptr next, int bit) {
    segment_ptr result = storage->pools[1].allocate();
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
    self.storage->pools[(size+CDELTA)/NODE_INCREMENT].free(*self.node_ptr);
  }
};
#pragma pack(0)

struct Compressed : public NodeHandler {
  static CompressedData* ptr(Transition& self) {
    return (CompressedData*)self.resolve(*self.node_ptr);
  }

  void append_to(Transition& self, std::string& key) {
    CompressedData* node = ptr(self);
    key.append(node->keys, (size_t)node->size);
  }

  segment_ptr* find(Transition& self, Slice& key) {
    CompressedData* node = ptr(self);
    if (key.size() >= node->size && ! memcmp(node->keys, key.data(), node->size)) {
      key = key.advance(node->size);
      return &node->next;
    }
    return NULL;
  }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value) {
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
        Slice rest_key(key.advance(i));
        trie_ptr = *trie.insert(rest_key, value);
        segment_ptr first_ptr = build(self.storage, trie_ptr, first);

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

  segment_ptr* first(Transition& self) { return &ptr(self)->next; }
  segment_ptr* last(Transition& self) { return &ptr(self)->next; }
  segment_ptr* next(Transition& self) { return NULL; }

  static segment_ptr fill(Storage* storage, segment_ptr node_ptr, segment_ptr next, Slice& key) {
    node_ptr.type = kCompressed;
    CompressedData *node = (CompressedData*)node_ptr.resolve(storage);
    node->next = next;
    node->size = (unsigned char)key.size();
    memcpy(node->keys, key.data(), node->size);
    assert(!memcmp(node->keys, key.data(), node->size));
    return node_ptr;
  }

  static segment_ptr build(Storage* storage, segment_ptr next, Slice& key) {
    int index = (key.size() + CDELTA)/NODE_INCREMENT;
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
  segment_ptr* find(Transition& self, Slice& key) { return NULL; }

  segment_ptr* insert(Transition& self, Slice& key, const Slice& value) {
    segment_ptr value_ptr(Value::build(self.storage, segment_ptr(), value));
    *self.node_ptr = Compressed::build(self.storage, value_ptr, key);
    return self.node_ptr;
  }
};


static Value value_handler;
static Null null;
static Compressed compressed;
static Trie trie;

NodeHandler* Transition::handlers[] = { &value_handler, &null, &compressed, &trie };


segment_ptr* Trie::insert(Transition& self, Slice& key, const Slice& value) {
  if (key.empty()) {
    // insert value key before
    *self.node_ptr = Value::build(self.storage, *self.node_ptr, value);
    return self.node_ptr;
  }

  char upper = bit::upper(key[0]);
  char lower = bit::lower(key[0]);

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


segment_ptr* Value::insert(Transition& self, Slice& key, const Slice& value) {
  if (key.empty()) {
    ValueData* old(ptr(self));
    segment_ptr new_ptr(build(self.storage, old->next, value));
    old->free(self);
    *self.node_ptr = new_ptr;
    return self.node_ptr;
  }

  segment_ptr next(build(self.storage, segment_ptr(), value));
  *self.node_ptr = Compressed::build(self.storage, next, key);
  return self.node_ptr;
}



} // namespace larch_leaves
