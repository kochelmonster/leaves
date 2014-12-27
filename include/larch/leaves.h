#ifndef _LARCH_LEAVES_H
#define _LARCH_LEAVES_H

#include <vector>
#include <memory>
#include <string>

namespace larch_leaves {

struct Options {
  // if true Storage starts with multiprocess support
  bool multiprocess;
  
  // size of a map window
  size_t window_size;
  
  // the maximum size of key+value
  // must be smaller than window_size
  size_t max_leaf_size;
};


class Storage {
 public:
  // opens a storage in directory "path"
  // path must exists and end with a separator ("/" or "\")
  // is path is NULL a in memory database is build
  static std::shared_ptr<Storage> open(
      const char* path, const Options& options);

  // returns a cursor inside a namespace
  // if signed_compare is true the trie table is [-127, 127] instead of [0, 255]
  std::shared_ptr<Cursor> read_cursor(const Slice& namespace_);
  std::shared_ptr<Cursor> write_cursor(const Slice& namespace_);
  
  Storage() {}
  ~Storage();
  
private:
  std::unique_ptr<NodeMemoryManager> _node_memory_manager;
  std::unique_ptr<LeafMemoryManager> _leaf_memory_manager;
};


class Cursor {
 public:
 
  // returns true if changes have to be commited
  bool is_dirty() const;
  
  // returns true if cursor is on a valid position
  bool is_valid() const;
 
  // sets the cursor to key
  virtual void set(const Slice& key, int read_forward=100);
  
  // sets the cursor to first
  void first(int read_forward=100);
  void last(int read_forward=-100) throw Error;
  void next(int read_forward=100) throw Error;
  void prev(int read_forward=-100) throw Error;
  bool is_valid() const;

  void add(const Slice& key) throw Error;
  Slice key() const;
  Slice value() const;
  void set_value(const Slice& value);
  void start_commit() throw Error;
  bool is_comitting() const;
  void end_commit() throw Error;
  void commit() throw Error;

 protected:
  virtual byte_t trie_index(char key) = 0;
  
  Cursor(Storage& storage) : _storage(storage), _transaction(0) {}

 private:
  versiont_t _transaction;
  std::shared_ptr<Storage> _storage;
  std::vector<NodePtr> _search_trace;
  std::string _key;
  
  friend Storage;
};


} // namespace larch_leaves 
#endif // _LARCH_LEAVES_H
