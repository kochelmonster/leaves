#ifndef _LARCH_LEAVES_H
#define _LARCH_LEAVES_H

#include <string.h>
#include <string>
#include <memory>
#include <exception>
#ifdef DEBUG
#include <ostream>
#endif

namespace larch_leaves {

#ifndef _MSC_VER
  #define NOEXCEPT noexcept
#else
  #if _MSC_VER >= 1600
    #define NOEXCEPT noexcept
  #else
    #define NOEXCEPT
  #endif
#endif


class LeavesException : public std::exception {
};

class NoValidPosition : public LeavesException {
};

class NotImplemented : public LeavesException {
};

class WrongValue : public LeavesException {
 private:
  const char* _msg;

 public:
  WrongValue(const char* msg) : _msg(msg) {}

  const char* what() const NOEXCEPT {
    return _msg;
  }
};


class Slice {
 private:
  size_t _size;
  const char* _data;

 public:
  Slice(const char *data, size_t size) : _size(size), _data(data) { }

  Slice(const char* str) : _size(strlen(str)), _data(str) { }

  Slice(const std::string& src)
    : _size(src.size()), _data(src.data()) { }

  Slice()
    : _size(0), _data(NULL) { }


  Slice slice(size_t len) const {
      return Slice(data(), len);
    }

  std::string string() const {
      return std::string(data(), _size);
    }

  template <typename ot> bool operator==(const ot& other) const {
      return size() == other.size()
        && memcmp(data(), other.data(), size()) == 0;
    }

  char operator[](size_t index) const {
      return data()[index];
    }

  const char* data() const {
      return _data;
    }

  size_t size() const {
      return _size;
    }

  Slice advance(size_t size) const {
      return Slice(data()+size, _size-size);
    }

  bool empty() const {
      return _size == 0;
    }
};
class Cursor {
 public:
  // returns true if cursor is on a valid position
  virtual bool is_valid() const = 0;

  // sets the cursor to key
  virtual void set(const Slice& key, int read_forward=100) = 0;

  // sets the cursor to first
  virtual void first(int read_forward=100) = 0;
  virtual void last(int read_forward=-100) = 0;
  virtual void next(int read_forward=100) = 0;
  virtual void prev(int read_forward=-100) = 0;

  virtual Slice key() = 0;
  virtual Slice value() = 0;

  // sets the value raise an exception if a read curor
  virtual void set_value(const Slice& value) = 0;
  virtual void remove() = 0;
};

#define EMBED_BREAKPOINT \
    asm("0:"                              \
        ".pushsection embed-breakpoints;" \
        ".quad 0b;"                       \
        ".popsection;")


} // namespace larch_leaves

#endif // _LARCH_LEAVES_H
