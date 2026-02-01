#ifndef _LEAVES__TWOQUECACHE_HPP
#define _LEAVES__TWOQUECACHE_HPP

#include <iostream>
#include <list>
#include "../third_party/unordered_dense.h"

namespace leaves {

/**
 * Implementation of the 2Q cache algorithm as described in the paper:
 * "2Q: A Low Overhead High Performance Buffer Management Replacement Algorithm"
 * by Johnson and Shasha.
 * 
 * This implementation uses a short-term (A1in) queue for one-time visitors,
 * a ghost (A1out) queue to track recently evicted items from A1in,
 * and a long-term (Am) queue for frequently accessed items.
 */
template<typename Key, typename Value>
struct TwoQCache {
    // Constructor with configurable size ratios
    TwoQCache(size_t capacity = 0, float kin_ratio = 0.25, float kout_ratio = 0.5)
        : _capacity(capacity),
          _size(0),
          _a1in_size_limit(capacity * kin_ratio),
          _a1out_size_limit(capacity * kout_ratio) {}
          
    // Copy constructor
    TwoQCache(const TwoQCache&) = default;
    
    // Move constructor
    TwoQCache(TwoQCache&&) = default;
    
    // Copy assignment
    TwoQCache& operator=(const TwoQCache&) = default;
    
    // Move assignment
    TwoQCache& operator=(TwoQCache&&) = default;

    // Get an item from the cache
    bool get(uint64_t key, Value& out) {
        // Check if in A1in queue (recent items)
        auto a1in_it = _a1in_map.find(key);
        if (a1in_it != _a1in_map.end()) {
            // Found in A1in queue - promote to Am queue (frequently used items)
            // Use splice to move the node without copying the Value
            auto list_it = a1in_it->second;
            out = list_it->second;  // Single copy to output
            
            // Splice the node from A1in to Am (no Value copy)
            _am_list.splice(_am_list.end(), _a1in_list, list_it);
            _am_map[key] = list_it;
            _a1in_map.erase(a1in_it);
            
            return true;
        }

        // Check if in Am queue (frequently used items)
        auto am_it = _am_map.find(key);
        if (am_it != _am_map.end()) {
            // Move to the front of Am (MRU position)
            _am_list.splice(_am_list.end(), _am_list, am_it->second);
            out = am_it->second->second;
            return true;
        }

        // Not found in cache
        return false;
    }

    // Put an item into the cache
    void put(uint64_t key, const Value& value) {
        // Don't add if already in A1in or Am queue
        if (_a1in_map.count(key) > 0 || _am_map.count(key) > 0) {
            return;
        }

        // Update total size
        _size += get_item_size(value);

        // Check if key is in A1out (was recently in cache)
        auto a1out_it = _a1out_map.find(key);
        if (a1out_it != _a1out_map.end()) {
            // This is a recurring item, put directly in Am
            _a1out_list.erase(a1out_it->second);
            _a1out_map.erase(a1out_it);
            
            _am_list.push_back(std::make_pair(key, value));
            _am_map[key] = std::prev(_am_list.end());
        }
        else {
            // New item, put in A1in
            _a1in_list.push_back(std::make_pair(key, value));
            _a1in_map[key] = std::prev(_a1in_list.end());
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
            
            for (auto it = _am_list.begin(); it != _am_list.end(); ++it) {
                // Check if this item can be evicted (refcount = 1)
                if (can_evict(it->second)) {
                    _size -= get_item_size(it->second);
                    _am_map.erase(it->first);
                    _am_list.erase(it);
                    evicted = true;
                    break;
                }
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
        for (const auto& item : _a1in_list) {
            a1in_current_size += get_item_size(item.second);
        }
        
        while (a1in_current_size > _a1in_size_limit && !_a1in_list.empty()) {
            // Try to find an item that can be evicted
            bool evicted = false;
            
            for (auto it = _a1in_list.begin(); it != _a1in_list.end(); ++it) {
                // Check if this item can be evicted (refcount = 1)
                if (can_evict(it->second)) {
                    auto key = it->first;
                    size_t item_size = get_item_size(it->second);
                    
                    // Move key to A1out (ghost list)
                    _a1out_list.push_back(key);
                    _a1out_map[key] = std::prev(_a1out_list.end());
                    
                    // Remove from A1in
                    _a1in_map.erase(key);
                    _a1in_list.erase(it);
                    
                    // Update sizes
                    a1in_current_size -= item_size;
                    _size -= item_size;
                    
                    evicted = true;
                    break;
                }
            }
            
            if (!evicted) break;  // Everything has ref > 1, can't evict more
        }
    }

    // Make A1out queue respect size limit
    void maintain_a1out_size() {
        while (_a1out_list.size() > _a1out_size_limit && !_a1out_list.empty()) {
            auto key = _a1out_list.front();
            _a1out_map.erase(key);
            _a1out_list.pop_front();
        }
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
    std::list<std::pair<Key, Value>> _a1in_list;
    ankerl::unordered_dense::map<Key, typename std::list<std::pair<Key, Value>>::iterator> _a1in_map;
    
    // A1out queue for recently evicted items (ghost list - only keys)
    std::list<Key> _a1out_list;
    ankerl::unordered_dense::map<Key, typename std::list<Key>::iterator> _a1out_map;
    
    // Am queue for frequently accessed items
    std::list<std::pair<Key, Value>> _am_list;
    ankerl::unordered_dense::map<Key, typename std::list<std::pair<Key, Value>>::iterator> _am_map;
};

} // namespace leaves

#endif // _LEAVES__TWOQUECACHE_HPP