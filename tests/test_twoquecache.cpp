#include <chrono>
#include <iostream>

#include "../include/leaves/intern/_twoquecache.hpp"
#include "../include/leaves/intern/_traits.hpp"
#include "../include/leaves/intern/_util.hpp"

using namespace leaves;

struct DummyBlockHeader {
    uint32_t slot_id;
    uint32_t free_idx;
};

// A simple SmartPointer-like class for testing
class TestPtr {
public:
    TestPtr(AreaSlice* slice = nullptr) : _slice(slice) {
        if (_slice) _slice->inc_ref();
    }
    
    TestPtr(const TestPtr& other) : _slice(other._slice) {
        if (_slice) _slice->inc_ref();
    }
    
    ~TestPtr() {
        if (_slice && _slice->dec_ref() == 0) {
            ::operator delete(_slice);
        }
    }
    
    TestPtr& operator=(const TestPtr& other) {
        if (this != &other) {
            AreaSlice* old = _slice;
            _slice = other._slice;
            if (_slice) _slice->inc_ref();
            if (old && old->dec_ref() == 0) {
                ::operator delete(old);
            }
        }
        return *this;
    }
    
    // Required by TwoQCache
    AreaSlice* area() const { return _slice; }
    
    // For testing
    operator bool() const { return _slice != nullptr; }
    
private:
    AreaSlice* _slice;
};

int main() {
    std::cout << "Testing TwoQCache implementation..." << std::endl;
    
    // Create cache with 1MB capacity
    const size_t CAPACITY = 1024 * 1024;
    TwoQCache<uint64_t, TestPtr> cache(CAPACITY);
    
    // Create some test objects
    std::vector<TestPtr> items;
    std::vector<uint64_t> keys;
    
    const size_t NUM_ITEMS = 100;
    const size_t ITEM_SIZE = 10 * 1024; // 10KB per item
    
    std::cout << "Creating " << NUM_ITEMS << " test objects, each " << ITEM_SIZE << " bytes..." << std::endl;
    
    for (size_t i = 0; i < NUM_ITEMS; i++) {
        // Create an AreaSlice with the specified size
        AreaSlice* slice = static_cast<AreaSlice*>(::operator new(ITEM_SIZE));
        slice->size(ITEM_SIZE);
        slice->offset(i * ITEM_SIZE);
        slice->_ref.store(0);
        
        // Create a TestPtr that holds this slice
        TestPtr ptr(slice);
        items.push_back(ptr);
        keys.push_back(i);
    }
    
    std::cout << "Adding items to cache..." << std::endl;
    
    // Add items to cache
    for (size_t i = 0; i < NUM_ITEMS; i++) {
        cache.put(keys[i], items[i]);
    }
    
    // Print cache stats
    cache.debug_info();
    
    std::cout << "Testing retrieval..." << std::endl;
    
    // Test retrieving items (should promote from A1in to Am)
    size_t num_retrieved = 0;
    for (size_t i = 0; i < NUM_ITEMS; i += 2) {
        TestPtr retrieved;
        if (cache.get(keys[i], retrieved)) {
            num_retrieved++;
        }
    }
    
    std::cout << "Retrieved " << num_retrieved << " items" << std::endl;
    
    // Print cache stats again to see the effect of retrievals
    cache.debug_info();
    
    std::cout << "Adding more items to test eviction..." << std::endl;
    
    // Add more items to force eviction
    for (size_t i = NUM_ITEMS; i < NUM_ITEMS * 2; i++) {
        // Create an AreaSlice with the specified size
        AreaSlice* slice = static_cast<AreaSlice*>(::operator new(ITEM_SIZE));
        slice->size(ITEM_SIZE);
        slice->offset(i * ITEM_SIZE);
        slice->_ref.store(0);
        
        TestPtr ptr(slice);
        cache.put(i, ptr);
    }
    
    // Print final cache stats
    cache.debug_info();
    
    std::cout << "Test completed successfully!" << std::endl;
    return 0;
}