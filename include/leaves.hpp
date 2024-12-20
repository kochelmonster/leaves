#ifndef _LEAVES_HPP
#define _LEAVES_HPP

#include <string.h>

#include <cassert>
#include <exception>
#include <memory>
#include <string>

namespace leaves {

#ifndef _MSC_VER
#define NOEXCEPT noexcept
#else
#if _MSC_VER >= 1600
#define NOEXCEPT noexcept
#else
#define NOEXCEPT
#endif
#endif

constexpr size_t padding(size_t a, size_t b) {
    return ((a + b - 1) / b) * b; // Round up to the next multiple of b
}


const char SIGNATURE[] = "larch-leaves";
const size_t SIGNATURE_SIZE = padding(sizeof(SIGNATURE), 8);
static const int kMajorVersion = 3;
static const int kMinorVersion = 0;

class LeavesException : public std::exception {};

class TransactionActive : public LeavesException {};

class NoTransactionFree : public LeavesException {};

class NoValidPosition : public LeavesException {};

class NotImplemented : public LeavesException {};

class KeyToBig : public LeavesException {};

class WrongValue : public LeavesException {
 private:
  const char* _msg;

 public:
  WrongValue(const char* msg) : _msg(msg) {}

  const char* what() const NOEXCEPT { return _msg; }
};

class Slice {
 private:
  size_t _size;
  const char* _data;

 public:
  Slice(const void* data, size_t size)
      : _size(size), _data((const char*)data) {}

  Slice(const char* str) : _size(strlen(str)), _data(str) {}

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

class Cursor {
 public:
  // returns true if cursor is on a valid position
  virtual bool is_valid() const = 0;

  // sets the cursor to key
  virtual void find(const Slice& key) = 0;

  virtual void first() = 0;
  virtual void last() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;

  virtual Slice get_key() const = 0;
  virtual Slice get_value() = 0;

  // sets the value raise an exception if a read cursor
  virtual void set_value(const Slice& value) = 0;
  virtual void remove() = 0;
  virtual void commit() = 0;
};

class DB {
 public:
  typedef std::shared_ptr<Cursor> cursor_ptr;
  typedef std::shared_ptr<DB> db_ptr;
  virtual ~DB();
  virtual cursor_ptr create_cursor() = 0;
  static db_ptr open(const char* path);
};

}  // namespace leaves

#endif  // _LEAVES_HPP
