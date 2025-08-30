#ifndef _LEAVES__TRAITS_HPP
#define _LEAVES__MMAP_HPP

#include <atomic>

#include "./_util.hpp"

namespace leaves {

template <typename BlockHeader>
struct SimplePointer {
  struct ptr {
    static constexpr NodeTypes type = TRIE;
    ptr(const ptr& src) : p(src.p) {}
    ptr(void* src = nullptr) : p((void*)src) {}

    operator char*() { return (char*)p; }
    operator const char*() const { return (char*)p; }
    operator const uint8_t*() const { return (uint8_t*)p; }
    operator uint64_t() const { return (uint64_t)p; }
    operator uint64_t() { return (uint64_t)p; }
    operator bool() const { return p != nullptr; }
    operator bool() { return p != nullptr; }

    BlockHeader* operator->() { return (BlockHeader*)p; }
    const BlockHeader* operator->() const { return (const BlockHeader*)p; }

    bool operator==(const ptr& other) const { return p == other.p; }
    bool operator!=(const ptr& other) const { return p != other.p; }
    bool operator!=(const void* other) const { return p != other; }
    void* link(uint16_t offset) { return (char*)p + offset; }
    void reset() { p = nullptr; }

    void* p;
  };

  template <typename T, NodeTypes t = TRIE>
  struct Pointer : public ptr {
    static constexpr NodeTypes type = t;
    Pointer(void* src = nullptr) : ptr(src) {}
    Pointer(const ptr& src) : ptr(src) {}
    const ptr& operator=(const ptr& src) {
      ptr::p = src.p;
      return src;
    }

    T* operator->() { return static_cast<T*>(ptr::p); }
    const T* operator->() const { return static_cast<const T*>(ptr::p); }
    T& operator*() { return *static_cast<T*>(ptr::p); }
    const T& operator*() const { return *static_cast<T*>(ptr::p); }
  };
};

struct AreaSlice;

template <typename BlockHeader>
struct SmartPointer {
  struct _AreaPtr {
    // the definition of area the pointer points to
    std::atomic<uint32_t> ref;
    uint64_t _offset;
    uint32_t _size;
    char p[];
  };

  // A Pointer to a Block inside an area.
  struct ptr {
    static constexpr NodeTypes type = TRIE;
    ptr(_AreaPtr* first) : _iref(first) {
      assert(_iref);
      _iref->ref++;
    }
    ptr(const ptr& src) : _iref(src._iref), _offset(src._offset) {
      assert(_iref); 
      _iref->ref++;
    }
    ~ptr() {
      if (_iref != nullptr) {
        if (--_iref->ref) {
          delete _iref;
        }
      }
    }
    operator char*() { return (char*)_iref->p + _offset; }
    operator const char*() const { return (const char*)_iref->p + _offset; }
    operator const uint8_t*() const { return (uint8_t*)_iref->p + _offset; }
    operator uint64_t() const { return (uint64_t)_iref->p + _offset; }
    operator uint64_t() { return (uint64_t)_iref->p + _offset; }
    operator bool() const { return _iref != nullptr; }
    operator bool() { return _iref != nullptr; }

    BlockHeader* operator->() { return (BlockHeader*)(char*)*this; }
    const BlockHeader* operator->() const {
      return (const BlockHeader*)(const char*)*this;;
    }
    bool operator==(const ptr& other) const {
      return _iref->p == other._iref->p && _offset == other._offset;
    }
    bool operator!=(const ptr& other) const { return !(_iref == other); }
    bool operator!=(const void* other) const { return (char*)*this != other; }
    void* link(uint16_t offset) { return (char*)_iref->p + _offset + offset; }
    void reset() {
      _iref->ref--;
      _iref = nullptr;
    }
    AreaSlice* area() { return (AreaSlice*)_iref->p; }
    uint64_t offset() const { return _iref->_offset; }
    uint32_t size() const { return _iref->_size; }

    _AreaPtr* _iref;
    uint32_t _offset = 0;  // offset in the area pointing to the BlockHeader
  };

  template <typename T, NodeTypes t = TRIE>
  struct Pointer : public ptr {
    static constexpr NodeTypes type = t;
    Pointer(void* src = nullptr) : ptr(src) {}
    Pointer(const ptr& src) : ptr(src) {}
    const ptr& operator=(const ptr& src) {
      if (ptr::_iref) ptr::_iref->ref--;
      ptr::_iref = src._iref;
      ptr::_iref->ref++;
      return src;
    }

    T* operator->() { return static_cast<T*>((char*)*this); }
    const T* operator->() const { return static_cast<const T*>((const char*)*this); }
    T& operator*() { return *static_cast<T*>((char*)*this); }
    const T& operator*() const { return *static_cast<const T*>((const char*)*this); }
  };
};

}  // namespace leaves

#endif  // _LEAVES__TRAITS_HPP