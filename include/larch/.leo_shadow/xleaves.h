//@+leo-ver=4-thin
//@+node:michael.20141215222649.17:@shadow leaves.h
//@@language cplusplus
//@@tabwidth -2
#ifndef _LARCH_LEAVES_H
#define _LARCH_LEAVES_H

//@<< includes >>
//@+node:michael.20141217010530.9:<< includes >>
#include <vector>
#include <memory>
#include <string>
//@nonl
//@-node:michael.20141217010530.9:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141215222649.38:struct Options
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
    node_window_size(4096*4096*8) {}
};


//@-node:michael.20141215222649.38:struct Options
//@+node:michael.20141230111914.5:struct Slice
class Slice {
 private:
  union {
    const char* _data;
    char _chars[];
  };
  size_t _size;

 public:
  Slice(const char *data, size_t size) : _size(size) {
      if (size < sizeof(_data))
        memcpy(_chars, data, size);
      else
        _data = data;
    }
    
  Slice(char one) : _size(1), _data(0) {
      _chars[0] = one;
    }
    
  Slice(const string& src)
    : Slice(src.data(), src.size()) { }

  Slice()
    : _size(0), _data(NULL) { }
    

  std::string string() const {
      return std::string(data(), _size);
    }

  const char* data() const {
      if (size < sizeof(_data))
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
};
//@nonl
//@-node:michael.20141230111914.5:struct Slice
//@+node:michael.20141215222649.27:class Storage (Declaration)
extern Slice main_name_space;

class Storage {
 public:
  virtual std::shared_ptr<Cursor> read_cursor(const Slice& namespace_=main_name_space) = 0;
  virtual std::shared_ptr<Cursor> write_cursor(const Slice& namespace_=main_name_space) = 0;
};


class MemoryStorage : public Storage {
public:
  MemoryStorage();
  virtual std::shared_ptr<Cursor> read_cursor(const Slice& namespace_=main_name_space);
  virtual std::shared_ptr<Cursor> write_cursor(const Slice& namespace_=main_name_space);

  NodeRef root() const {
    return NodeRef(_nodes._get_page(0), 0);
  }
  
private:
  NodeStorageInHeap _nodes;
  LeafStorageInHeap _leafs;
};


class PersistentStorage : public Storage {
 public:
  // opens a storage in directory "path"
  // path must exists and end with a separator ("/" or "\")
  // is path is NULL a in memory database is build
  void open(const char* path, const Options& options);
};


class CopyOnWriteStorage : public PersistentStorage {


};
//@nonl
//@-node:michael.20141215222649.27:class Storage (Declaration)
//@+node:michael.20141215222649.23:class Cursor (Declaration)
class Cursor {
 public:
  // returns true if cursor is on a valid position
  virtual bool is_valid() const = 0;
 
  // returns true if changes have to be commited
  virtual bool is_dirty() const = 0;
 
  // sets the cursor to key
  virtual void set(const Slice& key, int read_forward=100) = 0;
  
  // sets the cursor to first
  virtual void first(int read_forward=100) = 0;
  virtual void last(int read_forward=-100) = 0;
  virtual void next(int read_forward=100) = 0;
  virtual void prev(int read_forward=-100) = 0;
  
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
  
  // sets the value raise an exception if a read curor
  virtual void set_value(const Slice& value) = 0;
};


//@-node:michael.20141215222649.23:class Cursor (Declaration)
//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_H
//@nonl
//@-node:michael.20141215222649.17:@shadow leaves.h
//@-leo
