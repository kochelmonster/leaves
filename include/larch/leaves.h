//@+leo-ver=5-thin
//@+node:michael.20141215222649.17: * @file leaves.h
//@@language cplusplus
//@@tabwidth -2
#ifndef _LARCH_LEAVES_H
#define _LARCH_LEAVES_H

//@+<< includes >>
//@+node:michael.20141217010530.9: ** << includes >>
#include <string.h>
#include <string>
#include <memory>
#include <exception>
#ifdef DEBUG
#include <ostream>
#endif
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20141230111914.147: ** Exceptions
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
//@+node:michael.20141215222649.38: ** struct Options
struct Options {
  // if true Storage starts with multiprocess support
  bool multiprocess;
  
  // size of a map window for node storage must be a multiple of 4096
  size_t node_window_size;
  
  // size of a map window for leaf storage
  // value cannot be bigger than leaf_window_size
  size_t leaf_window_size;
  
  size_t max_leaf_size_in_node_storage; 
                  // if size of leaf is greater the leaf
                  // will be stored in leaf storage
  
  Options()
    : multiprocess(true), node_window_size(4096*4096*8), 
    leaf_window_size(4096*4096*8),
    max_leaf_size_in_node_storage(32) {}
};


//@+node:michael.20141230111914.5: ** class Slice
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
//@+node:michael.20141215222649.23: ** class Cursor (Declaration)
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

//@+node:michael.20141215222649.27: ** Database (Declaration)
#define EMBED_BREAKPOINT \
    asm("0:"                              \
        ".pushsection embed-breakpoints;" \
        ".quad 0b;"                       \
        ".popsection;")

class MemoryDatabase {
public:
  virtual bool is_valid() const = 0;
  virtual size_t count() const = 0;
  virtual size_t pages() const = 0;
  
  // sets the cursor to key
  virtual void find(const Slice& key) = 0;
  
  // sets the cursor to first
  virtual void first() = 0;
  virtual void last() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;
  
  virtual Slice key() = 0;
  virtual Slice value() = 0;
  
  // sets the value raise an exception if a read curor
  virtual void set_value(const Slice& value) = 0;
  virtual void remove() = 0;

#ifdef DEBUG
  virtual void dump(std::ostream& out) = 0;
#endif

  static MemoryDatabase* create();
};


/*

extern Slice main_name_space;

class PersistentDatabase : public Database {
 public:
  // opens a storage in directory "path"
  // path must exists and end with a separator ("/" or "\")
  // is path is NULL a in memory database is build
  void open(const char* path, const Options& options);
};


class CopyOnWriteDatabase : public PersistentDatabase {
  // returns true if changes have to be commited
  virtual bool is_dirty() const = 0;

};

 
*/
//@-others

} // namespace larch_leaves 

#endif // _LARCH_LEAVES_H
//@-leo
