#ifndef _LEAVES__TRAITS_HPP
#define _LEAVES__MMAP_HPP

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

template <typename BlockHeader>
struct SmartPointer {
  struct _IPtr {
    void* p;
    uint64_t offset;
    uint32_t size;
    uint32_t ref;
  };

  struct ptr {
    static constexpr NodeTypes type = TRIE;
    ptr(const ptr& src) : _iref(src._iref) { if (_iref) _iref->ref++; }
    ~ptr() {
      if (_iref != nullptr) {
        _iref->ref--;
      }
    }
    operator char*() { return (char*)_iref->p; }
    operator const char*() const { return (char*)_iref->p; }
    operator const uint8_t*() const { return (uint8_t*)_iref->p; }
    operator uint64_t() const { return (uint64_t)_iref->p; }
    operator uint64_t() { return (uint64_t)_iref->p; }
    operator bool() const { return _iref != nullptr; }
    operator bool() { return _iref != nullptr; }

    BlockHeader* operator->() { return (BlockHeader*)_iref->p; }
    const BlockHeader* operator->() const {
      return (const BlockHeader*)_iref->p;
    }

    bool operator==(const ptr& other) const {
      return _iref->p == other._iref->p;
    }
    bool operator!=(const ptr& other) const {
      return _iref->p != other._iref->p;
    }
    bool operator!=(const void* other) const { return _iref->p != other; }
    void* link(uint16_t offset) { return (char*)_iref->p + offset; }
    void reset() {
      _iref->ref--;
      _iref = nullptr;
    }

    _IPtr* _iref;
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

    T* operator->() { return static_cast<T*>(ptr::_iref->p); }
    const T* operator->() const { return static_cast<const T*>(ptr::_iref->p); }
    T& operator*() { return *static_cast<T*>(ptr::_iref->p); }
    const T& operator*() const { return *static_cast<T*>(ptr::_iref->p); }
  };
};

}  // namespace leaves

#endif  // _LEAVES__TRAITS_HPP