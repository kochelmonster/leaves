#ifndef _LEAVES__UTIL_HPP
#define _LEAVES__UTIL_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace leaves {

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
    return cmp;
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
    cmp = str1[i] > str2[i] ? 1 : -1;
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
void copy(Block& dst, const Block& src, size_t space = 0) {
  size_t base_size = sizeof(typename Block::Base), src_size = sizeof(Block);
  memcpy((char*)&dst + base_size, (char*)&src + base_size,
         space + src_size - base_size);
  dst.size = src.size;
}

}  // namespace leaves

#endif  // _LEAVES__UTIL_HPP
