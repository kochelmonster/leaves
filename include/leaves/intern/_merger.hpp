#ifndef _LEAVES_MERGER_HPP
#define _LEAVES_MERGER_HPP

#include "_bits.hpp"
#include "_cursor.hpp"
#include "_node.hpp"

namespace leaves {

// _NodeIterator is now defined in _cursor.hpp

/**
 * @brief Merger for combining two tries
 *
 * Merges trie A into trie B,
 */
template <typename CursorDst, typename CursorSrc>
struct _Merger {
  typedef _Merger<CursorDst, CursorSrc> Merger;
  using Traits = typename CursorDst::Traits;
  using TrieNode = typename CursorDst::TrieNode;
  using LeafNode = typename CursorDst::LeafNode;
  using block_ptr = typename CursorDst::block_ptr;
  using trie_ptr = typename CursorDst::trie_ptr;
  using leaf_ptr = typename CursorDst::leaf_ptr;
  using offset_e = typename CursorDst::offset_e;

  CursorDst& dst_cursor;
  CursorSrc& src_cursor;

  _Merger(CursorDst& dest, CursorSrc& src)
      : dst_cursor(dest), src_cursor(src) {}

  // Helper methods for memory management
  block_ptr alloc(uint16_t size) { return dst_cursor._db->alloc(size); }
  
  block_ptr resolve_src(offset_t offset) { return src_cursor._db->resolve(offset); }
  
  block_ptr resolve_dest(offset_t offset) { return dst_cursor._db->resolve(offset); }
  
  template <typename T>
  offset_t resolve_offset(T ptr) { return dst_cursor._db->resolve(ptr); }

  void exec() {
    src_cursor._prepare_move();
    if (!src_cursor.stack.size) return;  // Empty source trie
    merge_node();
  }

  void merge_node() {
    dst_cursor->find(src_cursor->current_key());

    auto& src = src_cursor->stack.back();
    if (src.is_leaf()) {
      Slice value = src.value();
      dst_cursor->value(value);
      src.pop();
      return;
    }

    auto& strie = src.trie();
    auto dest_key = dst_cursor->current_key();
    auto src_key = src_cursor->current_key();

    if (dest_key == src_key) {
      // found a corrsponding node
      auto& dest = dst_cursor->stack.back();

      if (dest.is_trie()) {
        auto& dtrie = dest.trie();
        assert(dest.prefix <= dtrie.len());
        if (dest.prefix < dtrie.len()) {
          // split dest trans line in inserter + deep copy all subtries of src
          split_and_add(dest, src);
        } else {
          assert(dest.prefix == dtrie.len());
          merge_children(dest, src, 0);
        }
      }
      src.pop();
      return;
    }

    assert(dest_key.size() < src_key().size());
    merge_children(dst_cursor.stack.back(), src, src_key().size() - dest_key().size());
    src.pop();
  }

  // Method to merge children from src to dest
  void merge_children(typename CursorDst::Transition& dest, typename CursorSrc::Transition& src, int key_offset) {
    // TODO: Implement proper children merging logic
    // This is a placeholder that would need to handle merging src children into dest
    // taking into account the key_offset for proper positioning
  }

  void split_and_add(typename CursorDst::Transition& dest, typename CursorSrc::Transition& src) {
    /* split the dest node in "an upper node" that has the same compressed
    prefix as src and a lower node that contains the remaining branches. The
    upper node has one child: the lower node. Merge the children of src that are not
    common with the the upper node to the upper node and do a deep copy of the the src children
    to dest. call replace on dest to cow update the dest trie.
    finally if lower node is also a child of src (identified by its key) walk down 
    the correspoinding child in src and continue merging from there by calling merge_node.
     */
    
    trie_ptr dtrie = dest.trie();
    trie_ptr strie = src.trie();
    
    assert(dest.prefix < dtrie->len());
    
    // Step 1: Create lower node with remaining compressed prefix
    uint8_t prefix_len = dtrie->len() - dest.prefix;
    trie_ptr child_trie = dst_cursor._db->alloc(TrieNode::size(prefix_len, dtrie->count()));
    child_trie->create(*dtrie, Slice(&dtrie->compressed()[dest.prefix], prefix_len));
    
    // Get the key byte at the split point (identifies the lower node)
    int lower_key = dtrie->compressed()[dest.prefix];
    offset_t child_offset = dst_cursor._db->resolve(child_trie);
    
    // Step 2: Analyze src children and find if lower_key exists in src
    int src_count = strie->count();
    offset_e* src_array = strie->array();
    
    bool lower_in_src = false;
    int lower_idx_in_src = -1;
    
    // Check if lower_key is among src children by iterating through src trie structure
    // We need to examine the keys that src trie branches on
    for (int i = 0; i < src_count; i++) {
      // TODO: This is simplified - in practice would need to traverse src structure
      // to determine which key each child corresponds to
    }
    
    // Step 3: Create upper node in stack buffer first
    int upper_branch_count = 1 + src_count;  // 1 for lower + all src children
    if (lower_in_src) upper_branch_count--;   // avoid double counting
    
    // Use stack buffer for building the upper node
    uint8_t buffer[TrieNode::MAX_SIZE];
    trie_ptr upper_buffer = reinterpret_cast<trie_ptr>(buffer);
    
    // Start building upper node - first add lower_key
    if (src_count > 0) {
      // Get first src key for two-key create
      int first_src_key = 0;  // Simplified - would need proper key extraction
      
      upper_buffer->create(
          Slice(dtrie->compressed(), dest.prefix),
          lower_key, child_offset, first_src_key);
    } else {
      // Only the lower node
      upper_buffer->create(
          Slice(dtrie->compressed(), dest.prefix),
          lower_key);
      upper_buffer->array()[0] = child_offset;
    }
    
    // Step 4: Add src children to the buffer node
    offset_e* upper_array = upper_buffer->array();
    int dest_idx = lower_in_src ? 0 : 1;  // If lower in src, it's already placed
    
    for (int i = 0; i < src_count; i++) {
      if (lower_in_src && i == lower_idx_in_src) {
        // Skip this one, it's the lower_key child
        continue;
      }
      
      offset_t src_child_offset = src_array[i];
      
      // Deep copy the src child to dest database
      offset_t copied_offset;
      if (src_child_offset.type() == LEAF) {
        // Deep copy leaf
        block_ptr src_block = src_cursor._db->resolve(src_child_offset);
        leaf_ptr src_leaf(src_block);
        uint16_t leaf_size = LeafNode::size(src_leaf->key_size, src_leaf->vsize());
        leaf_ptr dest_leaf = dst_cursor._db->alloc(leaf_size);
        
        memcpy((void*)dest_leaf, (void*)src_leaf, leaf_size);
        
        // Handle big values
        if (src_leaf->is_big()) {
          auto src_bv = src_leaf->big();
          block_ptr src_big_value = src_cursor._db->resolve(src_bv->offset);
          block_ptr dest_big_value = dst_cursor._db->alloc(src_bv->size());
          memcpy((void*)dest_big_value, (void*)src_big_value, src_bv->value_size);
          
          auto dest_bv = dest_leaf->big();
          dest_bv->offset = dst_cursor._db->resolve(dest_big_value);
        }
        
        copied_offset = dst_cursor._db->resolve(dest_leaf);
      } else {
        // Deep copy trie recursively
        copied_offset = deep_copy_trie_recursive(src_child_offset);
      }
      
      upper_array[dest_idx++] = copied_offset;
    }
    
    // Step 5: Allocate final upper trie and copy from buffer
    uint16_t final_size = upper_buffer->size();
    trie_ptr final_upper = dst_cursor._db->alloc(final_size);
    memcpy((void*)final_upper, (void*)upper_buffer, final_size);
    
    // Step 6: COW update - replace old dest trie with new upper trie
    dst_cursor._db->free(dtrie);
    offset_t upper_offset = dst_cursor._db->resolve(final_upper);
    dest.replace(upper_offset);
    
    // Step 7: If lower_key is also a child of src, recursively merge
    if (lower_in_src) {
      // Push down to the lower node and corresponding src child
      auto& lower_trans = dst_cursor.push(child_offset);
      auto& src_child_trans = src_cursor.push(src_array[lower_idx_in_src]);
      merge_node();
      dst_cursor.pop();
      src_cursor.pop();
    }
  }
  
  // Helper method for recursive deep copying of trie subtrees
  offset_t deep_copy_trie_recursive(offset_t src_offset) {
    block_ptr src_block = src_cursor._db->resolve(src_offset);
    trie_ptr src_trie(src_block);
    
    uint16_t trie_size = src_trie->size();
    trie_ptr dest_trie = dst_cursor._db->alloc(trie_size);
    
    // Copy the trie structure
    memcpy((void*)dest_trie, (void*)src_trie, trie_size);
    
    // Recursively copy all children and update their offsets
    offset_e* dest_array = dest_trie->array();
    for (int i = 0; i < dest_trie->count(); i++) {
      offset_t child_offset = src_trie->array()[i];
      
      if (child_offset.type() == LEAF) {
        // Deep copy leaf
        block_ptr child_src = src_cursor._db->resolve(child_offset);
        leaf_ptr child_src_leaf(child_src);
        uint16_t child_size = LeafNode::size(child_src_leaf->key_size, child_src_leaf->vsize());
        leaf_ptr child_dest_leaf = dst_cursor._db->alloc(child_size);
        memcpy((void*)child_dest_leaf, (void*)child_src_leaf, child_size);
        
        // Handle big values in child
        if (child_src_leaf->is_big()) {
          auto bv_src = child_src_leaf->big();
          block_ptr big_src = src_cursor._db->resolve(bv_src->offset);
          block_ptr big_dest = dst_cursor._db->alloc(bv_src->size());
          memcpy((void*)big_dest, (void*)big_src, bv_src->value_size);
          
          auto bv_dest = child_dest_leaf->big();
          bv_dest->offset = dst_cursor._db->resolve(big_dest);
        }
        
        dest_array[i] = dst_cursor._db->resolve(child_dest_leaf);
      } else {
        // Recursive trie copy
        dest_array[i] = deep_copy_trie_recursive(child_offset);
      }
    }
    
    return dst_cursor._db->resolve(dest_trie);
  }

  /**
   * @brief Deep copy entire subtree from source to destination
   */
  void deep_copy_subtree(offset_t src_offset) {
    block_ptr src_block = resolve_src(src_offset);

    if (src_offset.type() == LEAF) {
      deep_copy_leaf(src_block);
    } else {
      deep_copy_trie(src_block);
    }
  }

  /**
   * @brief Deep copy a leaf node from source to destination
   */
  void deep_copy_leaf(block_ptr src_block) {
    leaf_ptr src_leaf(src_block);
    uint16_t leaf_size = LeafNode::size(src_leaf->key_size, src_leaf->vsize());
    leaf_ptr dest_leaf = alloc(leaf_size);

    memcpy((void*)dest_leaf, (void*)src_leaf, leaf_size);

    // Handle big values
    if (src_leaf->is_big()) {
      auto src_bv = src_leaf->big();
      block_ptr src_big_value = resolve_src(src_bv->offset);
      block_ptr dest_big_value = alloc(src_bv->size());
      memcpy((void*)dest_big_value, (void*)src_big_value, src_bv->value_size);

      auto dest_bv = dest_leaf->big();
      dest_bv->offset = resolve_offset(dest_big_value);
    }

    offset_t leaf_offset = resolve_offset(dest_leaf);
    dst_cursor->set_root(leaf_offset);
  }

  /**
   * @brief Deep copy a trie node and its subtree from source to destination
   */
  void deep_copy_trie(block_ptr src_block) {
    trie_ptr src_trie(src_block);
    uint16_t trie_size = src_trie->size();
    trie_ptr dest_trie = alloc(trie_size);

    memcpy((void*)dest_trie, (void*)src_trie, trie_size);

    // Recursively copy all children and update offsets
    offset_e* dest_array = dest_trie->array();
    for (int i = 0; i < dest_trie->count(); i++) {
      offset_t child_offset = dest_trie->array()[i];

      // Recursively copy child
      block_ptr child_src = resolve_src(child_offset);
      offset_t child_dest_offset;

      if (child_offset.type() == LEAF) {
        leaf_ptr child_src_leaf(child_src);
        uint16_t child_size =
            LeafNode::size(child_src_leaf->key_size, child_src_leaf->vsize());
        leaf_ptr child_dest_leaf = alloc(child_size);
        memcpy((void*)child_dest_leaf, (void*)child_src_leaf, child_size);

        // Handle big values in child
        if (child_src_leaf->is_big()) {
          auto bv_src = child_src_leaf->big();
          block_ptr big_src = resolve_src(bv_src->offset);
          block_ptr big_dest = alloc(bv_src->size());
          memcpy((void*)big_dest, (void*)big_src, bv_src->value_size);

          auto bv_dest = child_dest_leaf->big();
          bv_dest->offset = resolve_offset(big_dest);
        }

        child_dest_offset = resolve_offset(child_dest_leaf);
      } else {
        // TODO: Recursive trie copy not yet fully implemented
        // Would need to recursively deep copy the trie structure
        child_dest_offset = child_offset;  // placeholder
      }

      dest_array[i] = child_dest_offset;
    }

    offset_t trie_offset = resolve_offset(dest_trie);
    dst_cursor->set_root(trie_offset);
  }
};

}  // namespace leaves

#endif  // _LEAVES_MERGER_HPP
