//  Handlers for all nodes
#include <algorithm>

#include "node.hpp"
#include "trie.hpp"


namespace leaves {

static Trie trie_handler;


template <typename compressed_type>
void iinsert(compressed_type* node, Transition& self, ISlice& key, const Slice& value,
             TrieNavigation* next_leaf, size_t offset=0) {
  assert(node->size >= offset);
  size_t size = std::min((size_t)node->size-offset, key.size());
  char* chars = node->chars + offset;
  for(size_t i = 0; i < size; i++) {
    if (chars[i] != key[i]) {
      /* insert_compressed_split/insert_leaf_split:
         node = [abcdefg]
         key = [abhij]
                       first   trie    rest
         [abcdefg] ==> [ab] -> |c| -> [efg]
                               |h| -> [ij]
      */
      Slice first(key.data(), i);
      Slice rest(node->chars, node->size);

      // build trie node
      any_ptr rest_ptr = node->transform(self.storage, rest.advance(i+offset+1));
      any_ptr trie_ptr = TrieData::build(self.storage, rest_ptr, chars[i]);

      self.set(trie_ptr);
      self.cmp = -1;
      trie_handler.ifind(self, key[i]);
      self.insert(key, value, next_leaf);

      self.set(CompressedData::build(self.storage, trie_ptr, first));
      return;
    }
  }

  any_ptr new_leaf = LeafData::build(self.storage, key, value, next_leaf);

  if (key.size() > size) {
    /* insert_leaf_long:

       node = [abc]
       key = [abcdef]

       Leaf      Leaf     Leaf
       [abc] ==> [abc] -> [def]
    */
    new_leaf.leaf->next = node->next;
    node->next = new_leaf;
    return;
  }

  /* insert_compressed_short/insert_leaf_short: (key is a substring of node)

     node = [abcdefg]
     key = [abhij]
                   first  Leaf   rest
     [abcdefg] ==> [ab] -> [] -> [cefg]
  */
  new_leaf.leaf->next = node->transform(self.storage, Slice(node->chars+size, node->size-size));
  self.set(new_leaf);
}


template <typename compressed_type>
int iadvance(compressed_type* node, Transition& self, ISlice& key, size_t offset=0) {
  assert(node->size >= offset);

  size_t size_ = node->size - offset;
  if (size_ <= key.size() && !memcmp(node->chars+offset, key.data(), size_)) {
      self.cmp = 0;
      key.iadvance(size_);
      return size_;
  }
  return -1;
}

template <typename compressed_type>
any_ptr combine(compressed_type* parent, CompressedData* child, Storage* storage) {
  std::string tmp;
  tmp.reserve(parent->size + child->size);
  tmp.append(parent->chars, parent->size);
  tmp.append(child->chars, child->size);
  any_ptr result = CompressedData::build(storage, child->next, tmp);
  storage->free(parent);
  storage->free(child);
  return result;
}


int compare(char* chars, size_t size, const ISlice& key) {
  /* compare chars with key
    returns 0 if key == chars and key.size() >= size
    returns -1 if key < chars or key == chars and key.size() < size
    return 1 if key > chars
  */

  size_t size_ = std::min(key.size(), (size_t)size);
  int cmp = sign(memcmp(key.data(), chars, size_));
  if (cmp == 0)
    return size <= size_ ? 0 : -1;
  return cmp;
}

struct Compressed : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key) {
    CompressedData* node = self.compressed;
    self.cmp = compare(node->chars, node->size, key);
    if (!self.cmp) {
      key.iadvance(node->size);
      return &self.compressed->next;
    }
    return NULL;
  }

  TrieNavigation* next(Transition& self) {
    return self.cmp >= 0 ? NULL : rfirst(self.compressed->next);
  }

  TrieNavigation* first(any_ptr node) {
    assert(node.node->type == kCompressed);
    return rfirst(node.compressed->next);
  }

  int advance(Transition& self, ISlice& key) {
    return iadvance(self.compressed, self, key);
  }

  void insert(Transition& self, ISlice& key, const Slice& value, TrieNavigation* next_leaf) {
    iinsert(self.compressed, self, key,value, next_leaf);
  }

  bool remove(Transition& self, bool end_node) {
    CompressedData *node = self.compressed;
    if (!node->next) {
      self.storage->free(node);
      *self.node_ptr = offset_ptr();
      return true;
    }

    any_ptr node_next = node->next.resolve();
    if (node_next.node->type == kCompressed) {
      self.set(combine(node, node_next.compressed, self.storage));
    }
    else if (node_next.node->type == kLeaf) {
      self.set(node_next);
      self.storage->free(node);
    }
    return true;
  }
};

struct Leaf : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key) {
    LeafData* node = self.leaf;
    assert(node->size >= key.offset);
    self.cmp = compare(node->chars+key.offset, node->size-key.offset, key);
    if (!self.cmp) {
      key.iadvance(node->size-key.offset);
      if (key.size()) {
        if (node->next)
          return &node->next;
        else
          self.cmp = 1;
      }
    }
    return NULL;
  }

  TrieNavigation* next(Transition& self) {
    return self.cmp >= 0 ? NULL : self.leaf;
  }

  TrieNavigation* first(any_ptr node) {
    assert(node.node->type == kLeaf);
    return node.navigation;
  }

  int advance(Transition& self, ISlice& key) {
    int cmp = iadvance(self.leaf, self, key, key.offset);
    if (cmp >= 0 && key.empty()) {
      return -1;  // stop advancing use this node.
    }
    return cmp;
  }

  void insert(Transition& self, ISlice& key, const Slice& value, TrieNavigation* next_leaf) {
    if (!next_leaf) {
      // replace value
      self.storage->free(self.leaf->value.resolve());
      self.leaf->value = ValueData::build(self.storage, value);
    }
    else
      iinsert(self.leaf, self, key,value, next_leaf, key.offset);
  }

  bool remove(Transition& self, bool end_node) {
    LeafData *node = self.leaf;
    if (!end_node)
      return false;

    TrieNavigation* next = node->next_leaf.resolve().navigation;
    TrieNavigation* prev = node->prev_leaf.resolve().navigation;
    prev->next_leaf = next;
    next->prev_leaf = prev;

    if (node->next) {
      any_ptr node_next = node->next.resolve();
      if (node_next.node->type == kCompressed) {
        self.set(combine(node, node_next.compressed, self.storage));
        return true;
      }
      else
        *self.node_ptr = node->next;
    }
    else
      *self.node_ptr = offset_ptr();

    self.storage->free(node);
    return true;
  }
};


struct Null : public NodeHandler {
  offset_ptr* find(Transition& self, ISlice& key) { return NULL; }
  TrieNavigation* next(Transition& self) { return NULL; }
  TrieNavigation* first(any_ptr node) { return NULL; }
  void insert(Transition& self, ISlice& key, const Slice& value, TrieNavigation* next_leaf) {
    self.set(LeafData::build(self.storage, key, value, next_leaf));
  }
  int advance(Transition& self, ISlice& key) { return -1; } // never
  bool remove(Transition& self, bool end_node) { return false; } // never
};


static Leaf leaf_handler;
static Null null_handler;
static Compressed compressed_handler;

NodeHandler* Transition::handlers[] = {
  &null_handler, &compressed_handler, &trie_handler, &leaf_handler };


#ifdef DEBUG

const char* handler_names[] = {
  "kNull",
  "kCompressed",
  "kTrie",
  "kLeaf",
  "kValue"
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
    out << "children: []" << std::endl;
    out << "---" << std::endl;
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    CompressedData *data = ptr.compressed;
    out << "id: " << dump_id(ptr, storage) << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "chars: \"";
    for(int i = 0; i < data->size; i++) {
      out << "[";
      dump_char(out, data->chars[i]);
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


struct LeafDumper : public DumpBase {
  void dump(std::ostream& out, any_ptr ptr, Storage* storage, int upper=-1) {
    LeafData *data = ptr.leaf;
    out << "id: " << dump_id(ptr, storage) << std::endl;
    out << "size: " << (int)data->size << std::endl;
    out << "chars: \"";
    for(int i = 0; i < data->size; i++) {
      out << "[";
      dump_char(out, data->chars[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << dump_id(data->prev_leaf, storage) << std::endl;
    if (data->next)
      out << "  - " << dump_id(data->next, storage) << std::endl;
    out << "  - " << dump_id(data->next_leaf, storage) << std::endl;
    out << "---" << std::endl;
    if (data->next)
      dump_node(out, data->next, storage);
  }
};

ValueDumper value_dumper;
NullDumper null_dumper;
CompressDumper compress_dumper;
TrieDumper trie_dumper;
LeafDumper leaf_dumper;


DumpBase* dumpers[] = {
  &null_dumper,
  &compress_dumper,
  &trie_dumper,
  &leaf_dumper,
  &value_dumper,
};

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage, int upper) {
  dumpers[ptr.node->type]->dump(out, ptr, storage, upper);
}

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage) {
  dump_node(out, ptr, storage, -1);
}

#endif


} // namespace leaves
