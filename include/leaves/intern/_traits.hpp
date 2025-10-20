#ifndef _LEAVES__TRAITS_HPP
#define _LEAVES__TRAITS_HPP

#include <atomic>
#include <cstdint>

#include "_util.hpp"

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
  // A Pointer to a Block inside an area. Uses AreaSlice directly.
  struct ptr {
    static constexpr NodeTypes type = TRIE;
    ptr(AreaSlice* area = nullptr) : _iref(area) {
      if (_iref) _iref->inc_ref();
    }
    ptr(const ptr& src) : _iref(src._iref), _offset(src._offset) {
      if (_iref) _iref->inc_ref();
    }
    ptr(ptr&& other) noexcept : _iref(other._iref), _offset(other._offset) {
      other._iref = nullptr;
      other._offset = 0;
    }
    ptr& operator=(const ptr& other) {
      if (this == &other) return *this;
      AreaSlice* old = _iref;
      _iref = other._iref;
      _offset = other._offset;
      if (_iref) _iref->inc_ref();
      if (old && old->dec_ref() == 0) {
        ::operator delete((void*)old);
      }
      return *this;
    }
    ptr& operator=(ptr&& other) noexcept {
      if (this == &other) return *this;
      AreaSlice* old = _iref;
      _iref = other._iref;
      _offset = other._offset;
      other._iref = nullptr;
      other._offset = 0;
      if (old && old->dec_ref() == 0) {
        ::operator delete((void*)old);
      }
      return *this;
    }
    ~ptr() {
      AreaSlice* tmp = _iref;
      if (tmp && tmp->dec_ref() == 0) {
        ::operator delete((void*)tmp);
      }
    }
    operator char*() { return (char*)_iref + _offset; }
    operator const char*() const { return (const char*)_iref + _offset; }
    operator const uint8_t*() const { return (uint8_t*)_iref + _offset; }
    operator uint64_t() const { return (uint64_t)((char*)_iref + _offset); }
    operator uint64_t() { return (uint64_t)((char*)_iref + _offset); }
    operator bool() const { return _iref != nullptr; }
    operator bool() { return _iref != nullptr; }

    BlockHeader* operator->() {
      return reinterpret_cast<BlockHeader*>((char*)*this);
    }
    const BlockHeader* operator->() const {
      return reinterpret_cast<const BlockHeader*>((const char*)*this);
    }
    bool operator==(const ptr& other) const {
      return _iref == other._iref && _offset == other._offset;
    }
    bool operator!=(const ptr& other) const { return !(*this == other); }
    bool operator!=(const void* other) const { return (char*)*this != other; }
    void* link(uint16_t offset) { return (char*)_iref + _offset + offset; }
    void reset() {
      if (_iref) {
        if (_iref->dec_ref() == 0) {
          ::operator delete((void*)_iref);
        }
        _iref = nullptr;
      }
      _offset = 0;
    }
    AreaSlice* area() const { return _iref; }
    uint64_t offset() const { return _iref ? _iref->offset() + _offset : 0; }

    AreaSlice* _iref = nullptr;
    uint32_t _offset = 0;  // offset within area (from AreaSlice base)
  };

  template <typename T, NodeTypes t = TRIE>
  struct Pointer : public ptr {
    static constexpr NodeTypes type = t;
    Pointer(void* src = nullptr) : ptr(reinterpret_cast<AreaSlice*>(src)) {}
    Pointer(const ptr& src) : ptr(src) {}
    const ptr& operator=(const ptr& src) {
      if (ptr::_iref) ptr::_iref->dec_ref();
      ptr::_iref = src._iref;
      ptr::_offset = src._offset;
      if (ptr::_iref) ptr::_iref->inc_ref();
      return src;
    }

    T* operator->() { return reinterpret_cast<T*>((char*)*this); }
    const T* operator->() const {
      return reinterpret_cast<const T*>((const char*)*this);
    }
    T& operator*() { return *reinterpret_cast<T*>((char*)*this); }
    const T& operator*() const {
      return *reinterpret_cast<const T*>((const char*)*this);
    }
  };
};

}  // namespace leaves

#endif  // _LEAVES__TRAITS_HPP