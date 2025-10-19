#ifndef _LEAVES__UTIL_HPP
#define _LEAVES__UTIL_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

namespace leaves {

typedef uint64_t tid_t;
typedef enum { TRIE = 0, LEAF = 1 } NodeTypes;
typedef enum { READ = 0, WRITE = 1 } Access;

class Slice {
 private:
  size_t _size;
  const char* _data;

 public:
  Slice(const void* data, size_t size)
      : _size(size), _data((const char*)data) {}

  Slice(const char* str) : _size(std::strlen(str)), _data(str) {}

  Slice(const std::string& src) : _size(src.size()), _data(src.data()) {}

  Slice() : _size(0), _data(NULL) {}

  Slice slice(size_t len) const { return Slice(data(), len); }

  std::string string() const { return std::string(data(), _size); }

  const Slice& operator=(const Slice& src) {
    _size = src.size();
    _data = src.data();
    return *this;
  }

  template <typename ot>
  bool operator==(const ot& other) const {
    return size() == other.size() && memcmp(data(), other.data(), size()) == 0;
  }

  template <typename ot>
  int compare(const ot& other) const {
    int cmp = 0, size_ = size() < other.size() ? size() : other.size();
    (cmp = memcmp(data(), other.data(), size_)) ||
        (cmp = size() - other.size());
    return (0 < cmp) - (cmp < 0);
  }

  char operator[](size_t index) const { return data()[index]; }

  const char* data() const { return _data; }

  size_t size() const { return _size; }

  Slice advance(size_t size) const {
    return Slice(data() + size, _size - size);
  }

  void iadvance(size_t size) {
    assert(size <= _size);
    _data += size;
    _size -= size;
  }

  bool empty() const { return _size == 0; }

  operator bool() const { return _size != 0; }
  void reset() {
    _size = 0;
    _data = nullptr;
  }
};

template <typename BaseType>
struct _Offset {
  typedef _Offset<BaseType> OffsetType;
  BaseType _offset;

  static const uint64_t TYPE_MASK = 0x3;

  constexpr _Offset(uint64_t src = 0) : _offset(src) {}

  template <typename T>
  bool operator==(T other) const {
    return _offset == other;
  }
  template <typename T>
  bool operator!=(T other) const {
    return _offset != other;
  }

  template <typename T>
  bool operator<=(T other) const {
    return _offset <= other;
  }

  template <typename T>
  bool operator<(T other) const {
    return _offset < other;
  }

  template <typename T>
  bool operator>=(T other) const {
    return _offset >= other;
  }

  bool operator==(const _Offset& src) const { return _offset == src._offset; }
  bool operator!=(const _Offset& src) const { return _offset != src._offset; }
  bool operator<(const _Offset& src) const { return _offset < src._offset; }
  bool operator>(const _Offset& src) const { return _offset > src._offset; }
  bool operator<=(const _Offset& src) const { return _offset <= src._offset; }
  bool operator>=(const _Offset& src) const { return _offset >= src._offset; }

  const _Offset& operator=(const _Offset& src) {
    _offset = src._offset;
    return *this;
  }

  template <typename T>
  const _Offset& operator=(T src) {
    _offset = src;
    return *this;
  }

  template <typename T>
  const _Offset& operator+=(T add) {
    _offset += add;
    return *this;
  }

  template <typename T>
  _Offset operator+(T src) {
    return _Offset(_offset + src);
  }

  template <typename T>
  _Offset operator-(T src) {
    return _Offset(_offset - src);
  }

  operator uint64_t() const { return _offset & ~TYPE_MASK; }
  NodeTypes type() const { return (NodeTypes)(_offset & TYPE_MASK); }
  const _Offset& type(NodeTypes type) {
    _offset &= ~TYPE_MASK;
    _offset |= type;
    return *this;
  }
};

typedef _Offset<uint64_t> offset_t;

struct AreaSlice {
  std::atomic<uint64_t> _offset;
  uint32_t _size;
  std::atomic<uint32_t> _ref;

  // Copy assignment operator needed because std::atomic isn't copyable by
  // default
  AreaSlice& operator=(const AreaSlice& other) {
    _offset.store(other._offset.load(std::memory_order_acquire),
                  std::memory_order_release);
    _size = other._size;
    _ref.store(other._ref.load(std::memory_order_acquire),
               std::memory_order_release);
    return *this;
  }

  AreaSlice(uint64_t offset_ = 0, uint32_t size_ = 0, uint32_t ref_ = 0)
      : _offset(offset_), _size(size_), _ref(ref_) {}
  AreaSlice(const AreaSlice& other)
      : _offset(other._offset.load(std::memory_order_acquire)),
        _size(other._size),
        _ref(other._ref.load(std::memory_order_acquire)) {}

  // No more dirty bit methods - removed

  uint64_t offset() const { return _offset.load(std::memory_order_acquire); }

  void offset(uint64_t new_offset) {
    _offset.store(new_offset, std::memory_order_release);
  }

  uint32_t size() const { return _size; }

  void size(uint32_t new_size) { _size = new_size; }

  // Reference counting methods - atomic
  uint32_t inc_ref() {
    return _ref.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  uint32_t dec_ref() {
    return _ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
  }

  uint32_t get_ref() const { return _ref.load(std::memory_order_acquire); }

  operator bool() const { return size(); }
  uint64_t end() const { return offset() + size(); }
};

inline size_t get_prefix(const char* str1, const char* str2, size_t size1,
                         size_t size2, int& cmp) {
  size_t i = 0;
  size_t limit = std::min(size1, size2) / sizeof(uint64_t);
  const uint64_t* wstr1 = reinterpret_cast<const uint64_t*>(str1);
  const uint64_t* wstr2 = reinterpret_cast<const uint64_t*>(str2);

  while (i < limit && wstr1[i] == wstr2[i]) {
    i++;
  }
  i *= sizeof(uint64_t);

  limit = std::min(size1, size2);
  while (i < limit && str1[i] == str2[i]) {
    i++;
  }

  if (i < limit) {
    cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
  } else if (size1 > limit)
    cmp = 1;
  else if (size2 > limit)
    cmp = -1;
  else
    cmp = 0;
  return i;
}

constexpr size_t padding(size_t a, size_t b) {
  return ((a + b - 1) / b) * b;  // Round up to the next multiple of b
}

const static uint32_t ALIGN = sizeof(void*);
constexpr uint32_t align(uint32_t s) { return (s + ALIGN - 1) & ~(ALIGN - 1); }

template <typename Block>
void copy(Block& dst, const Block& src) {
  uint16_t base_size = sizeof(typename Block::Base), src_size = src.size();
  memcpy((char*)&dst + base_size, (char*)&src + base_size,
         src_size - base_size);
}

}  // namespace leaves

#endif  // _LEAVES__UTIL_HPP
