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


#define MAIN_POOL_COUNT 4
#define MAX_VALUE_POOL_COUNT 20
#define MAX_DB_SIZE (((size_t)1)<<47)



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
 protected:
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


struct Options {
  Options(
    size_t grow_size=1<<26,
    size_t area_count=2000,
    size_t value_pool_count=5,
    size_t value_pool_start_size=128,
    size_t value_pool_increment=128,
    size_t max_db_size=((size_t)1)<<37) // 128 Giga)
    : max_db_size(max_db_size),
      grow_size(grow_size),
      area_count(area_count),
      value_pool_count(value_pool_count),
      value_pool_start_size(value_pool_start_size),
      value_pool_increment(value_pool_increment) {}

  /* the maximum size of the database

    can be changed after creation.
  */
  size_t max_db_size;

  /* a database grows on multiples of grow_size

    can be changed after creation.
  */
  size_t grow_size;

  /* the count of items in a segregated memory pool chunk

     cannot be changed after creation.
  */
  size_t area_count;

  /*
    Parameters to create segregated memory pools for Values opmizide ValueStorage
    value_pool_start_size = 100
    value_pool_increment = 32
    value_pool_count = 3
    creates 3 memory pools for size 132, 164, 196 bytes

    cannot be changed after creation.
  */
  size_t value_pool_count;
  size_t value_pool_start_size;
  size_t value_pool_increment;
};


struct Stats : public Options {
  // count of used nodes in the standard pools
  size_t used_nodes[MAIN_POOL_COUNT+MAX_VALUE_POOL_COUNT];

  // count of freed nodes in the standard pools
  size_t freed_nodes[MAIN_POOL_COUNT+MAX_VALUE_POOL_COUNT];
};


class DB {
public:
  typedef std::shared_ptr<Cursor> cursor_ptr;
  typedef std::shared_ptr<DB> ptr;

  virtual ~DB();
  virtual cursor_ptr create_cursor() = 0;
  virtual void flush(bool async=true) = 0;
  virtual void get_stats(Stats& stats) = 0;

  static ptr open(const char *path,  const Options& options);
};


class TxnCursor: public Cursor {
public:
  virtual void abort() = 0;
  virtual void commit() = 0;
};


class TxnMonitor {
public:
  typedef std::shared_ptr<TxnMonitor> ptr;
  typedef std::shared_ptr<TxnCursor> cursor_ptr;

  /* returns a transactional cursor */
  virtual cursor_ptr create_cursor(DB::ptr db) = 0;
  static ptr create(const char *name);
};


} // namespace leaves


#endif // _LARCH_LEAVES_H
