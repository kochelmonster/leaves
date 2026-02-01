#ifndef _LEAVES__TWOQUECACHE_HPP
#define _LEAVES__TWOQUECACHE_HPP

#include <iostream>
#include <vector>
#include "../third_party/unordered_dense.h"

namespace leaves {

/**
 * Intrusive doubly-linked list node for cache entries.
 * Avoids heap allocations per node that std::list incurs.
 */
template<typename Key, typename Value>
struct CacheEntry {
    Key key;
    Value value;
    CacheEntry* prev = nullptr;
    CacheEntry* next = nullptr;
    
    CacheEntry() = default;
    CacheEntry(Key k, Value v) : key(k), value(std::move(v)) {}
};

/**
 * Intrusive doubly-linked list with O(1) operations.
 * Nodes are allocated externally and linked/unlinked here.
 */
template<typename Key, typename Value>
struct IntrusiveList {
    using Entry = CacheEntry<Key, Value>;
    
    Entry* head = nullptr;
    Entry* tail = nullptr;
    size_t _size = 0;
    
    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }
    
    Entry* front() { return head; }
    Entry* back() { return tail; }
    
    void push_back(Entry* node) {
        node->prev = tail;
        node->next = nullptr;
        if (tail) tail->next = node;
        else head = node;
        tail = node;
        ++_size;
    }
    
    void unlink(Entry* node) {
        if (node->prev) node->prev->next = node->next;
        else head = node->next;
        if (node->next) node->next->prev = node->prev;
        else tail = node->prev;
        node->prev = node->next = nullptr;
        --_size;
    }
    
    // Move node to end (MRU position)
    void move_to_back(Entry* node) {
        if (node == tail) return;  // Already at back
        unlink(node);
        push_back(node);
        // Adjust size since unlink decremented it
    }
    
    // Transfer node from another list to end of this list
    void splice_back(IntrusiveList& from, Entry* node) {
        from.unlink(node);
        push_back(node);
    }
};

/**
 * Implementation of the 2Q cache algorithm as described in the paper:
 * "2Q: A Low Overhead High Performance Buffer Management Replacement Algorithm"
 * by Johnson and Shasha.
 * 
 * This implementation uses a short-term (A1in) queue for one-time visitors,
 * a ghost (A1out) queue to track recently evicted items from A1in,
 * and a long-term (Am) queue for frequently accessed items.
 * 
 * Uses intrusive linked lists to avoid per-node heap allocations.
 */
template<typename Key, typename Value>
struct TwoQCache {
    using Entry = CacheEntry<Key, Value>;
    using GhostEntry = CacheEntry<Key, char>;  // Ghost entries only need key
    using List = IntrusiveList<Key, Value>;
    using GhostList = IntrusiveList<Key, char>;
    
    // Constructor with configurable size ratios
    TwoQCache(size_t capacity = 0, float kin_ratio = 0.25, float kout_ratio = 0.5)
        : _capacity(capacity),
          _size(0),
          _a1in_size_limit(capacity * kin_ratio),
          _a1out_size_limit(capacity * kout_ratio) {
        // Pre-allocate hash maps to avoid rehashing during warmup
        // Estimate ~64KB average area size
        constexpr size_t avg_item_size = 64 * 1024;
        size_t estimated_items = capacity / avg_item_size;
        if (estimated_items > 0) {
            _a1in_map.reserve(estimated_items / 4);   // 25% in A1in
            _a1out_map.reserve(estimated_items / 2);  // 50% in A1out
            _am_map.reserve(estimated_items);         // rest in Am
        }
    }
    
    ~TwoQCache() {
        // Clean up all entries
        clear_list(_a1in_list);
        clear_list(_am_list);
        clear_ghost_list(_a1out_list);
    }
    
    // Disable copy (entries are owned)
    TwoQCache(const TwoQCache&) = delete;
    TwoQCache& operator=(const TwoQCache&) = delete;
    
    // Move constructor
    TwoQCache(TwoQCache&& other) noexcept
        : _capacity(other._capacity),
          _size(other._size),
          _a1in_size_limit(other._a1in_size_limit),
          _a1out_size_limit(other._a1out_size_limit),
          _a1in_list(other._a1in_list),
          _a1in_map(std::move(other._a1in_map)),
          _a1out_list(other._a1out_list),
          _a1out_map(std::move(other._a1out_map)),
          _am_list(other._am_list),
          _am_map(std::move(other._am_map)) {
        other._a1in_list = {};
        other._a1out_list = {};
        other._am_list = {};
        other._size = 0;
    }
    
    // Move assignment
    TwoQCache& operator=(TwoQCache&& other) noexcept {
        if (this != &other) {
            clear_list(_a1in_list);
            clear_list(_am_list);
            clear_ghost_list(_a1out_list);
            
            _capacity = other._capacity;
            _size = other._size;
            _a1in_size_limit = other._a1in_size_limit;
            _a1out_size_limit = other._a1out_size_limit;
            _a1in_list = other._a1in_list;
            _a1in_map = std::move(other._a1in_map);
            _a1out_list = other._a1out_list;
            _a1out_map = std::move(other._a1out_map);
            _am_list = other._am_list;
            _am_map = std::move(other._am_map);
            
            other._a1in_list = {};
            other._a1out_list = {};
            other._am_list = {};
            other._size = 0;
        }
        return *this;
    }

    // Get an item from the cache - returns pointer to cached value, nullptr if not found
    Value* get(uint64_t key) {
        // Check if in A1in queue (recent items)
        auto a1in_it = _a1in_map.find(key);
        if (a1in_it != _a1in_map.end()) {
            // Found in A1in queue - promote to Am queue (frequently used items)
            Entry* entry = a1in_it->second;
            
            // Move from A1in to Am
            _am_list.splice_back(_a1in_list, entry);
            _am_map[key] = entry;
            _a1in_map.erase(a1in_it);
            
            return &entry->value;
        }

        // Check if in Am queue (frequently used items)
        auto am_it = _am_map.find(key);
        if (am_it != _am_map.end()) {
            // Move to the back of Am (MRU position)
            Entry* entry = am_it->second;
            _am_list.move_to_back(entry);
            return &entry->value;
        }

        // Not found in cache
        return nullptr;
    }

    // Legacy get interface for compatibility
    bool get(uint64_t key, Value& out) {
        Value* ptr = get(key);
        if (ptr) {
            out = *ptr;
            return true;
        }
        return false;
    }

    // Put an item into the cache
    void put(uint64_t key, const Value& value) {
        // Don't add if already in A1in or Am queue
        if (_a1in_map.find(key) != _a1in_map.end() || 
            _am_map.find(key) != _am_map.end()) {
            return;
        }

        // Update total size
        _size += get_item_size(value);

        // Check if key is in A1out (was recently in cache)
        auto a1out_it = _a1out_map.find(key);
        if (a1out_it != _a1out_map.end()) {
            // This is a recurring item, put directly in Am
            GhostEntry* ghost = a1out_it->second;
            _a1out_list.unlink(ghost);
            delete ghost;
            _a1out_map.erase(a1out_it);
            
            Entry* entry = new Entry(key, value);
            _am_list.push_back(entry);
            _am_map[key] = entry;
        }
        else {
            // New item, put in A1in
            Entry* entry = new Entry(key, value);
            _a1in_list.push_back(entry);
            _a1in_map[key] = entry;
        }

        // Maintain size limits
        prune();
    }

    // Get size of value - default implementation for page_ptr with area() method
    size_t get_item_size(const Value& value) const {
        auto* slice = value.area();
        if (!slice) return 0;
        return slice->size();
    }

    // Prune the cache to keep it within size limits
    void prune() {
        // First, make sure A1in is within its size limit
        maintain_a1in_size();
        
        // Then ensure A1out stays within its size limit
        maintain_a1out_size();
        
        // Finally, if still over capacity, evict from Am
        while (_size > _capacity && !_am_list.empty()) {
            // Try to evict only items with reference count = 1
            bool evicted = false;
            
            for (Entry* entry = _am_list.front(); entry != nullptr; ) {
                Entry* next = entry->next;
                // Check if this item can be evicted (refcount = 1)
                if (can_evict(entry->value)) {
                    _size -= get_item_size(entry->value);
                    _am_map.erase(entry->key);
                    _am_list.unlink(entry);
                    delete entry;
                    evicted = true;
                    break;
                }
                entry = next;
            }
            
            if (!evicted) break;  // Everything has ref > 1, can't evict more
        }
    }

    // Returns true if item can be evicted (has refcount = 1)
    bool can_evict(const Value& value) const {
        // Get the reference count of the area and check if == 1
        if (!value) return false;
        auto* slice = value.area();
        if (!slice) return false;
        return (slice->get_ref() == 1);
    }

    // Make A1in queue respect size limit
    void maintain_a1in_size() {
        size_t a1in_current_size = 0;
        for (Entry* e = _a1in_list.front(); e != nullptr; e = e->next) {
            a1in_current_size += get_item_size(e->value);
        }
        
        if (a1in_current_size <= _a1in_size_limit) return;
        
        // Collect all evictable entries in one pass
        std::vector<Entry*> to_evict;
        to_evict.reserve(16);  // Pre-allocate for common case
        
        for (Entry* e = _a1in_list.front(); e != nullptr && a1in_current_size > _a1in_size_limit; e = e->next) {
            if (can_evict(e->value)) {
                size_t item_size = get_item_size(e->value);
                a1in_current_size -= item_size;
                _size -= item_size;
                to_evict.push_back(e);
            }
        }
        
        // Bulk evict: move keys to A1out and delete from A1in
        for (Entry* entry : to_evict) {
            Key key = entry->key;
            
            // Add to ghost list
            GhostEntry* ghost = new GhostEntry(key, 0);
            _a1out_list.push_back(ghost);
            _a1out_map[key] = ghost;
            
            // Remove from A1in
            _a1in_map.erase(key);
            _a1in_list.unlink(entry);
            delete entry;
        }
    }

    // Make A1out queue respect size limit
    void maintain_a1out_size() {
        while (_a1out_list.size() > _a1out_size_limit && !_a1out_list.empty()) {
            GhostEntry* ghost = _a1out_list.front();
            _a1out_map.erase(ghost->key);
            _a1out_list.unlink(ghost);
            delete ghost;
        }
    }
    
    // Helper to clear a list and delete all entries
    void clear_list(List& list) {
        Entry* e = list.front();
        while (e) {
            Entry* next = e->next;
            delete e;
            e = next;
        }
        list = {};
    }
    
    void clear_ghost_list(GhostList& list) {
        GhostEntry* e = list.front();
        while (e) {
            GhostEntry* next = e->next;
            delete e;
            e = next;
        }
        list = {};
    }

    // Debug information
    void debug_info() const {
        std::cout << "TwoQCache stats:\n"
                  << "  Total size: " << _size << " / " << _capacity << " bytes\n"
                  << "  A1in items: " << _a1in_list.size() << "\n"
                  << "  A1out items: " << _a1out_list.size() << "\n"
                  << "  Am items: " << _am_list.size() << std::endl;
    }

    // Get cache size (number of entries)
    size_t size() const { 
        return _am_map.size() + _a1in_map.size(); 
    }

    // Maximum capacity of the cache in bytes
    size_t _capacity;
    
    // Current size in bytes
    size_t _size;
    
    // Size limits for the different queues
    size_t _a1in_size_limit;  // A1in should use about 25% of cache
    size_t _a1out_size_limit; // A1out should track about 50% of items
    
    // A1in queue for recent items (first access)
    List _a1in_list;
    ankerl::unordered_dense::map<Key, Entry*> _a1in_map;
    
    // A1out queue for recently evicted items (ghost list - only keys)
    GhostList _a1out_list;
    ankerl::unordered_dense::map<Key, GhostEntry*> _a1out_map;
    
    // Am queue for frequently accessed items
    List _am_list;
    ankerl::unordered_dense::map<Key, Entry*> _am_map;
};

} // namespace leaves

#endif // _LEAVES__TWOQUECACHE_HPP