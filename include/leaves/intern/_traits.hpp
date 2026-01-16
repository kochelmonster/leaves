#ifndef _LEAVES__TRAITS_HPP
#define _LEAVES__TRAITS_HPP

#include <atomic>
#include <cstdint>

#include "_util.hpp"

namespace leaves {

template <typename T, NodeTypes t = TRIE>
struct SimplePointer {
  static constexpr NodeTypes type = t;

  SimplePointer(const SimplePointer& src) : p(src.p) {}
  SimplePointer(void* src = nullptr) : p((void*)src) {}
  SimplePointer(std::nullptr_t)
      : p(nullptr) {}  // Explicit nullptr_t constructor

  // Allow conversion from SimplePointer of different types
  template <typename U, NodeTypes u>
  SimplePointer(const SimplePointer<U, u>& src) : p(src.p) {}

  SimplePointer& operator=(const SimplePointer& src) {
    p = src.p;
    return *this;
  }

  // Allow assignment from SimplePointer of different types
  template <typename U, NodeTypes u>
  SimplePointer& operator=(const SimplePointer<U, u>& src) {
    p = src.p;
    return *this;
  }

  operator char*() { return (char*)p; }
  operator const char*() const { return (char*)p; }
  operator const uint8_t*() const { return (uint8_t*)p; }
  operator uint64_t() const { return (uint64_t)p; }
  operator uint64_t() { return (uint64_t)p; }
  operator bool() const { return p != nullptr; }
  operator bool() { return p != nullptr; }
  operator T*() { return (T*)p; }
  operator const T*() const { return (const T*)p; }
  T* operator->() { return (T*)p; }
  const T* operator->() const { return (const T*)p; }
  T& operator*() { return *static_cast<T*>(p); }
  const T& operator*() const { return *static_cast<const T*>(p); }

  SimplePointer operator-(uint64_t offset) const {
    return SimplePointer((char*)p - offset);
  }

  SimplePointer operator+(uint64_t offset) const {
    return SimplePointer((char*)p + offset);
  }

  bool operator==(const SimplePointer& other) const { return p == other.p; }
  bool operator!=(const SimplePointer& other) const { return p != other.p; }
  bool operator!=(const void* other) const { return p != other; }
  void reset() { p = nullptr; }

  // Make p accessible to other template instantiations
  template <typename, NodeTypes>
  friend struct SimplePointer;

  void* p = nullptr;
};

struct AreaSlice;

template <typename T, NodeTypes t = TRIE>
struct SmartPointer {
  // A Pointer to a Block inside an area. Uses AreaSlice directly.
  static constexpr NodeTypes type = t;

  SmartPointer(AreaSlice* area = nullptr) : _iref(area) {
    if (_iref) _iref->inc_ref();
  }
  SmartPointer(void* src) : SmartPointer(reinterpret_cast<AreaSlice*>(src)) {}
  SmartPointer(std::nullptr_t)
      : _iref(nullptr), _offset(0) {}  // Explicit nullptr_t constructor
  SmartPointer(const SmartPointer& src)
      : _iref(src._iref), _offset(src._offset) {
    if (_iref) _iref->inc_ref();
  }
  SmartPointer(SmartPointer&& other) noexcept
      : _iref(other._iref), _offset(other._offset) {
    other._iref = nullptr;
    other._offset = 0;
  }

  // Allow conversion from SmartPointer of different types
  template <typename U, NodeTypes u>
  SmartPointer(const SmartPointer<U, u>& src)
      : _iref(src._iref), _offset(src._offset) {
    if (_iref) _iref->inc_ref();
  }

  SmartPointer& operator=(const SmartPointer& other) {
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

  // Allow assignment from SmartPointer of different types
  template <typename U, NodeTypes u>
  SmartPointer& operator=(const SmartPointer<U, u>& other) {
    AreaSlice* old = _iref;
    _iref = other._iref;
    _offset = other._offset;
    if (_iref) _iref->inc_ref();
    if (old && old->dec_ref() == 0) {
      ::operator delete((void*)old);
    }
    return *this;
  }

  SmartPointer& operator=(SmartPointer&& other) noexcept {
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

  ~SmartPointer() {
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

  operator T*() { return (T*)((char*)*this); }
  operator const T*() const { return (const T*)((const char*)*this); }
  T* operator->() { return (T*)*this; }
  const T* operator->() const { return (const T*)*this; }
  T& operator*() { return *reinterpret_cast<T*>((char*)*this); }
  const T& operator*() const {
    return *reinterpret_cast<const T*>((const char*)*this);
  }

  SmartPointer operator-(uint64_t offset) {
    SmartPointer result = *this;
    assert(result._offset >= offset);
    result._offset -= offset;
    return result;
  }

  SmartPointer operator+(uint64_t offset) {
    SmartPointer result = *this;
    result._offset += offset;
    return result;
  }

  bool operator==(const SmartPointer& other) const {
    return _iref == other._iref && _offset == other._offset;
  }
  bool operator!=(const SmartPointer& other) const { return !(*this == other); }
  bool operator!=(const void* other) const { return (char*)*this != other; }

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

  // Make _iref and _offset accessible to other template instantiations
  template <typename, NodeTypes>
  friend struct SmartPointer;

  AreaSlice* _iref = nullptr;
  uint32_t _offset = 0;  // offset within area (from AreaSlice base)
};

}  // namespace leaves

#endif  // _LEAVES__TRAITS_HPP