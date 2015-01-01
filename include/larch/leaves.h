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
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20141230111914.147: ** Exceptions
class LeavesException : public std::exception {
};

class NoValidPosition : public LeavesException {
};

class NotImplemented : public LeavesException {
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
  
  Options()
    : multiprocess(true), node_window_size(4096*4096*8), 
    leaf_window_size(4096*4096*8) {}
};


//@+node:michael.20141230111914.5: ** class Slice
typedef unsigned char trieindex_t;

class Slice {
 private:
  size_t _size;
  union {
    const char* _data;
    char _chars[];
  };
  

 public:
 Slice(const char *data, size_t size) : _size(size), _data(NULL) {
      if (size < sizeof(const char*))
        memcpy(_chars, data, size);
      else
        _data = data;
    }
    
  Slice(const char* str) : Slice(str, strlen(str)) {
    }
    
  Slice(char one) : _size(1), _data(0) {
      _chars[0] = one;
    }
    
  Slice(const std::string& src)
    : Slice(src.data(), src.size()) { }

  Slice()
    : _size(0), _data(NULL) { }
    

  Slice slice(size_t len) const {
      return Slice(data(), len);
    }

  std::string string() const {
      return std::string(data(), _size);
    }

  const char* data() const {
      if (_size < sizeof(const char*))
        return _chars;
        
      return _data;
    }
    
  size_t size() const {
      return _size;
    }

  trieindex_t trieindex() const {
      return data()[0];
    }
    
  Slice advance(size_t size) const {
      return Slice(data()+1, _size-1);
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


typedef std::shared_ptr<Cursor> cursor_ptr;
//@+node:michael.20141215222649.27: ** class Storage (Declaration)
extern Slice main_name_space;

class Storage {
 public:
  virtual cursor_ptr read_cursor(const Slice& namespace_=main_name_space) = 0;
  virtual cursor_ptr write_cursor(const Slice& namespace_=main_name_space) = 0;
  virtual ~Storage() {}
};


class MemoryStorage : public Storage {
public:
  static std::shared_ptr<MemoryStorage> create();
};


class PersistentStorage : public Storage {
 public:
  // opens a storage in directory "path"
  // path must exists and end with a separator ("/" or "\")
  // is path is NULL a in memory database is build
  void open(const char* path, const Options& options);
};


class CopyOnWriteStorage : public PersistentStorage {
  // returns true if changes have to be commited
  virtual bool is_dirty() const = 0;

};

 
//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_H
//@-leo
