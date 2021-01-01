#ifndef _LARCH_LEAVES_H
#define _LARCH_LEAVES_H

#include <string.h>
#include <string>
#include <memory>
#include <exception>


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


Slice version();


class Cursor {
public:
  // returns true if cursor is on a valid position
  virtual bool valid() const = 0;

  // sets the cursor to key
  virtual void find(const Slice& key) = 0;

  virtual void first() = 0;
  virtual void last() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;

  virtual Slice key() const = 0;
  virtual Slice value() const = 0;

  // sets the value raise an exception if a read curor
  virtual void set_value(const Slice& value) = 0;
  virtual void remove() = 0;
};


struct ValuePools {
  /*
    Parameters to create segregated memory pools for Values opmizide ValueStorage
    start_size = 100
    increment = 32
    count = 3
    creates 3 memory pools for size 132, 164, 196 bytes
  */
  ValuePools(size_t start_size=100, size_t increment=64, size_t count=0) :
    start_size(start_size), increment(increment), count(count) { }

  size_t start_size;
  size_t increment;
  size_t count;
};


class DB {
public:
  typedef std::shared_ptr<Cursor> cursor_ptr;
  typedef std::shared_ptr<DB> ptr;

  virtual ~DB();
  virtual cursor_ptr create_cursor() = 0;
  virtual void flush() = 0;
  static ptr open(const char *path, size_t segment_size=1<<27, ValuePools pools=ValuePools());
};

} // namespace leaves


#endif // _LARCH_LEAVES_H
