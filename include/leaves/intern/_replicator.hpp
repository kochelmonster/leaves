#ifndef _LEAVES__REPLICATOR_HPP
#define _LEAVES__REPLICATOR_HPP

#include "_node.hpp"

namespace leaves {


template <typename Traits>
void calc_hash(typename Traits::offset_e offset, tid_t txn_id,
               typename Traits::Hasher& phasher) {
  typedef typename Traits::Hasher Hasher;

  if (root.type() == LEAF) {
    leaf_ptr leaf = _db.resolve(offset);
    if (leaf->txn_id == txn_id) {
      Hasher hasher;
      hasher.update(leaf->data, leaf->key_size);
      Slice value = leaf->value();
      hasher.update(value.data(), value.size());
      hasher.finalize(leaf->hash);
    }
    phasher.update(leaf->hash);
    return;
  }
  assert(root.type() == TRIE);
  trie_ptr trie = _db.resolve(offset);
  if (trie->txn_id == txn_id) {
    Hasher hasher;
    offset_e* begin = trie->array();
    offset_e* end = begin + trie->count();
    for (offset_e* iter = begin; iter != end; iter++) {
      calc_hash<Traits>(*iter, txn_id, hasher);
    }
    Slice compressed = trie->compressed();
    hasher.update(compressed.data(), compressed.size());
    hasher.finalize(trie->hash);
    phasher.update(trie->hash, sizeof(trie->hash));
  }
}

struct _MemoryMapReplicationTraits : public _MemoryMapReplicationTraits {
  typedef _ReplicationTraits::hash_t hash_t;
  typedef _ReplicationTraits::Hasher Hasher;
  
};




template <typename Storage_>
struct _Replicator {
  typedef Storage_ Storage;
  using Traits = typename Storage::Traits;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;

  typedef enum {
    START_SEND,
    SEND,
    START_RECEIVE,
    RECEIVE,
    WAIT,
  } command_t

      Storage& _storage;

  Replicator(Storage& storage) : _storage(storage) {}

  Slice ready_for_receiving() const {}
  Slice ready_for_sending() const {}

  void cancel() {}
  void continue_() {}

  Slice send();

  Slice receive(Slice data) {}

  command_t execute() {}

  void send_trie_part(offset_e offset) {
    char buffer[64 * K];
    uint32_t space_left = sizeof(buffer);
    uint32_t filled = fill_merkle_buffer(buffer, space_left, offset);
    return Slice(buffer, filled);
  }

  uint32_t fill_merkle_buffer(char* buffer, uint32_t space_left,
                              offset_e offset) {
    if (offset.type() == LEAF) {
      if (sizeof(LeafNode) > space_left) return 0;
      LeafNode* dst = (LeafNode*)buffer;
      leaf_ptr src = _db.resolve(offset);
      dst->key_size = 0;
      dst->value_size = 0;
      memcpy(dst->hash, src->hash, sizeof(src->hash));
      return sizeof(LeafNode);
    }
    assert(offset.type() == TRIE);
    trie_ptr src = _db.resolve(offset);
    uint16_t size = src.size();
    if (size > space_left) return 0;
    TrieNode* dst = (TrieNode*)buffer;
    memcpy(dst, (void*)src, size);

    offset_e* begin = src->array();
    offset_e* end = begin + src->count();
    offset_e* dst = dst->array();
    memset(dst, 0, sizeof(offset_e) * dst->count());

    buffer += size;
    space_left -= size;
    for (offset_e* iter = begin; iter != end; iter++, dst++) {
      uint16_t delta = fill_merkle_buffer(buffer, space_left, iter);
      if (!delta) return size;
      *dst = size;
      size += delta;
      buffer += delta;
      space_left -= delta;
    }

    return size;
  }
};

}  // namespace leaves

#endif  // _LEAVES__REPLICATOR_HPP