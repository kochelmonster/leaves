#ifndef _LEAVES__REPLICATOR_HPP
#define _LEAVES__REPLICATOR_HPP

#include <boost/endian/arithmetic.hpp>

#include "_node.hpp"

namespace leaves {
namespace Replication {
namespace Replication {

template <typename Traits>
void calc_hash(typename Traits::offset_e offset, tid_t txn_id,
               typename Traits::Hasher& phasher) {
  typedef typename Traits::Hasher Hasher;

  if (root.type() == LEAF) {
    leaf_ptr leaf = _db.resolve(offset);
    if (leaf->txn_id == txn_id) {
      Hasher hasher;
      hasher.update(leaf->data, leaf->key_size);
      Slice value = leaf->value();
      hasher.update(value.data(), value.size());
      hasher.finalize(leaf->hash);
    }
    phasher.update(leaf->hash);
    return;
  }
  assert(root.type() == TRIE);
  trie_ptr trie = _db.resolve(offset);
  if (trie->txn_id == txn_id) {
    Hasher hasher;
    offset_e* begin = trie->array();
    offset_e* end = begin + trie->count();
    for (offset_e* iter = begin; iter != end; iter++) {
      calc_hash<Traits>(*iter, txn_id, hasher);
    }
    Slice compressed = trie->compressed();
    hasher.update(compressed.data(), compressed.size());
    hasher.finalize(trie->hash);
    phasher.update(trie->hash, sizeof(trie->hash));
  }
}

typedef std::vector<uint8_t> buffer_t;

template <typename Cursor_>
struct _QueryBuilder {
  struct Traits {
    struct BlockHeader {
      hash_t hash;
    };
    typedef char[0] offset_t;
  };

  typedef Cursor_ Cursor;
  using db_ptr = typename Cursor::db_ptr;
  using TrieNode = typename Cursor::Transition::TrieNode

      typedef _TrieNode<Traits>
          TTrieNode;

  struct TransferChunk {
    boost::endian::big_int16_t size;
    boost::endian::big_int16_t path_size;
    uint8_t data[];
    TTrieNode* trie() const { return (TTrieNode*)&data[path_size]; }
  }

  QueryBuilder(db_ptr db)
      : _cursor(db) {
  }

  void receive(buffer_t&& buffer) {
    size_t size = buffer.size();
    size_t pos = 0;
    while (pos < size) {
      TransferChunk* chunk = (TransferChunk*)&buffer[pos];
      assert(size >= pos + chunk->size);
      _cursor.find(Slice(chunk->data, chunk->path_size));
      assert(_cursor.stack.back().is_trie());
      auto mtrie = _cursor.stack.back().trie();
      auto ptrie = chunk->trie();

      Slice mprefix(mtrie->compressed(), mtrie->len()),
          pprefix(ptrie->compressed(), ptrie->len());

      auto prefix = get_prefix(mprefix.data(), pprefix.data(), mprefix.size(),
                               pprefix.size(), cmp);
      
      if (prefix)                               
    }
  }

  buffer_t _query_buffer;
  Cursor _cursor;
};

struct _MemoryMapReplicationTraits : public _MemoryMapReplicationTraits {
  typedef _ReplicationTraits::hash_t hash_t;
  typedef _ReplicationTraits::Hasher Hasher;
};

typedef std::vector<uint8_t> buffer_t;

typedef enum {
  START_SYNC,  // A SYNC Request from a peer
  SEND,        // send the content of send_buffer to the peer
  RECEIVE,     // receive a new message
  WAIT,        // Nothing to do -> receive a message with timeout
  INSERT,      // start inserting sync data (the sync data can be modified)
  ERROR        // an Error occured use error_desc()
} command_t;

typedef enum {
  MSG_START = 'S',   // start sync
  MSG_MERKLE = 'M',  // send part of merkle trie
  MSG_DATA = 'T',    // send part of a Trie
  MSG_BIG = 'B',     // send big data
  MSG_CANCEL = 'C'   // cancel synchronization
} message_t;

struct Message {
  char type;
  boost::endian::big_uint64_t size;  // message size without type
  boost::endian::big_uint16_t idx;
  boost::endian::big_uint64_t path;
  const char path_data[];
  const char* data() { return &path_data[path]; }
};

struct StartMessage {
  char type;
  boost::endian::big_uint64_t size;  // message size without type
  boost::endian::big_uint16_t idx;
  char _name[];
  Slice name() const { return Slice(_name, size - sizeof(idx)); }
};

struct CancelMessage {
  char type;
  boost::endian::big_uint64_t size;  // message size without type
  boost::endian::big_uint16_t idx;
};

// FSM to replicate on DB
template <typename DB>
struct _RDB {
  typedef DB::Storage Storage;
  typedef DB::Cursor Cursor;
  using db_ptr = typename Storage::db_ptr;

  typedef enum {
    WAITING,
    FAILED,
    RECEIVING,
  } state_t;

  std::vector<offset_e*> children_v;

  state_t _state;
  tid_t _last_tid;
  db_ptr _db;
  Cursor _cursor;
  boost::endian::big_uint16_t idx;

  std::vector<uint8_t> send_buffer;

  _RDB(db_ptr db, uint16_t idx_)
      : _db(db), _state(WAITING), _last_tid(0), _cursor(db), idx(idx_) {}

  Slice name() const { return _db.name(); }

  void start() { _tmp_cursor = _db->_storage->_make(_tmp_name)->cursor(); }

  void cancel() { _state = WAITING; }

  command_t execute() {
    _handle_receive();
    switch (_state) {
      case WAITING:
        if (_last_tid != _db->tid) {
          _last_tid = _db->tid;
          _state = RECEIVING;
          start();
          return _send_start_message();
        }
        break;

      case RECEIVING:
        return handle_receive_buffer();

      case ERROR:
        return HANDLE_ERROR;
    }
  }
    WAITING,
    FAILED,
    RECEIVING,
  } state_t;

  std::vector<offset_e*> children_v;

  state_t _state;
  tid_t _last_tid;
  db_ptr _db;
  Cursor _cursor;
  boost::endian::big_uint16_t idx;

  std::vector<uint8_t> send_buffer;

  _RDB(db_ptr db, uint16_t idx_)
      : _db(db), _state(WAITING), _last_tid(0), _cursor(db), idx(idx_) {}

  Slice name() const { return _db.name(); }

  void start() { _tmp_cursor = _db->_storage->_make(_tmp_name)->cursor(); }

  void cancel() { _state = WAITING; }

  command_t execute() {
    _handle_receive();
    switch (_state) {
      case WAITING:
        if (_last_tid != _db->tid) {
          _last_tid = _db->tid;
          _state = RECEIVING;
          start();
          return _send_start_message();
        }
        break;

      case RECEIVING:
        return handle_receive_buffer();

      case ERROR:
        return HANDLE_ERROR;
    }
  }

  void reset_error() { _state = WAITING; }

  command_t handle_receive_buffer() {
    if (receiver.execute()) {
      if (receiver.error) {
        send_reset();
        _state = IN_ERROR;
        return HANDLE_ERROR;
      }
      switch (receiver.type()) {
        case MERKLE:
          return compare_merkle();

        case DATA:
          return add_data();

        case BIG:
          return add_big_data();

        default:
          assert(0);
      }
    }
    return RECEIVE;
  }

  void receive(const Message* msg) {
    switch (msg->type) {
      case MSG_MERKLE:
        _handle_merke_msg(msg);
        break;

      case MSG_DATA:
        _handle_data_msg(msg);

      case MSG_BIG:
        _handle_big_msg(msg);
    }
  }
  void reset_error() { _state = WAITING; }

  command_t handle_receive_buffer() {
    if (receiver.execute()) {
      if (receiver.error) {
        send_reset();
        _state = IN_ERROR;
        return HANDLE_ERROR;
      }
      switch (receiver.type()) {
        case MERKLE:
          return compare_merkle();

        case DATA:
          return add_data();

        case BIG:
          return add_big_data();

        default:
          assert(0);
      }
    }
    return RECEIVE;
  }

  void receive(const Message* msg) {
    switch (msg->type) {
      case MSG_MERKLE:
        _handle_merke_msg(msg);
        break;

      case MSG_DATA:
        _handle_data_msg(msg);

      case MSG_BIG:
        _handle_big_msg(msg);
    }
  }

  void _send_start_message() {
    size_t size = send_buffer.size();
    Slice name_ = name();
    send_buffer.resize(size + sizeof(StartMessage) + name_.size());
    StartMessage* sm = (StartMessage*)send_buffer.data() + size;
    sm->type = MSG_START;
    sm->size = sizeof(StartMessage) + name_.size();
    memcpy(sm->name, name_.data(), name_.size());
    _send_merkle_trie(Slice());
  }

  void _send_merkle_trie(const Slice& path) {
    _cursor.find(path);
    assert(_cursor.stack.back().prefix == 0);

    size_t size = send_buffer.size();
    Message* msg = (Message*)_extend_send_buffer(sizeof(Message) + path.size());
    msg->type = MSG_MERKLE : msg->idx = idx;
    memcpy(msg->path, path.data(), path.size());
    auto bsize = _fill_merkle_buffer(_cursor->stack.back().offset, 64 * K,
                                     std::vector<offset_t> children);
    msg = (Message*)send_buffer.data() + size;
    msg->size = bsize;
  }

  char* _extend_send_buffer(size_t size) {
    size_t sb_size = send_buffer.size();
    send_buffer.resize(sb_size + size);
    return send_buffer.data();
  void _send_start_message() {
    size_t size = send_buffer.size();
    Slice name_ = name();
    send_buffer.resize(size + sizeof(StartMessage) + name_.size());
    StartMessage* sm = (StartMessage*)send_buffer.data() + size;
    sm->type = MSG_START;
    sm->size = sizeof(StartMessage) + name_.size();
    memcpy(sm->name, name_.data(), name_.size());
    _send_merkle_trie(Slice());
  }

  void _send_merkle_trie(const Slice& path) {
    _cursor.find(path);
    assert(_cursor.stack.back().prefix == 0);

    size_t size = send_buffer.size();
    Message* msg = (Message*)_extend_send_buffer(sizeof(Message) + path.size());
    msg->type = MSG_MERKLE : msg->idx = idx;
    memcpy(msg->path, path.data(), path.size());
    auto bsize = _fill_merkle_buffer(_cursor->stack.back().offset, 64 * K,
                                     std::vector<offset_t> children);
    msg = (Message*)send_buffer.data() + size;
    msg->size = bsize;
  }

  char* _extend_send_buffer(size_t size) {
    size_t sb_size = send_buffer.size();
    send_buffer.resize(sb_size + size);
    return send_buffer.data();
  }

  uint32_t _fill_merkle_buffer(offset_t offset, uint32_t space_left,
                               children_v children) {
  uint32_t _fill_merkle_buffer(offset_t offset, uint32_t space_left,
                               children_v children) {
    if (offset.type() == LEAF) {
      if (sizeof(LeafNode) > space_left) return 0;
      LeafNode* dst = _extend_send_buffer(sizeof(LeafNode));
      leaf_ptr src = _db->resolve(offset);
      LeafNode* dst = _extend_send_buffer(sizeof(LeafNode));
      leaf_ptr src = _db->resolve(offset);
      dst->key_size = 0;
      dst->value_size = 0;
      memcpy(dst->hash, src->hash, sizeof(src->hash));
      return sizeof(LeafNode);
    }


    assert(offset.type() == TRIE);
    trie_ptr src = _db->resolve(offset);
    uint16_t size = src->size();
    trie_ptr src = _db->resolve(offset);
    uint16_t size = src->size();
    if (size > space_left) return 0;
    TrieNode* dst = (TrieNode*)_extend_send_buffer(size);
    TrieNode* dst = (TrieNode*)_extend_send_buffer(size);
    memcpy(dst, (void*)src, size);

    offset_e* begin = dst->array();
    children.insert(children.begin(), begin, begin + dst->count());
    offset_e* begin = dst->array();
    children.insert(children.begin(), begin, begin + dst->count());

    buffer += size;
    space_left -= size;
    for (offset_e* iter = begin; iter != end; iter++, dst++) {
      uint16_t delta = fill_merkle_buffer(buffer, space_left, iter);
      if (!delta) return size;
      *dst = size;
      size += delta;
      buffer += delta;
      space_left -= delta;
    }

    return size;
  }
};

/*
  Replication od a Storage
*/
template <typename Storage_>
struct _Replicator {
  typedef Storage_ Storage;
  using Traits = typename Storage::Traits;
  typedef _TrieNode<Traits> TrieNode;
  typedef _LeafNode<Traits> LeafNode;
  using block_ptr = typename Traits::ptr;
  using offset_e = typename Traits::offset_e;
  using trie_ptr = typename Traits::Pointer<TrieNode>;
  using leaf_ptr = typename Traits::Pointer<LeafNode, LEAF>;
  typedef Storage::DB DB;
  typedef Storage::db_ptr db_ptr;
  typedef std::vector<db_ptr> db_v;
  typedef _RDB<DB> RDB;
  typedef std::shared_ptr<DB> rdb_ptr;
  typedef std::vector<rdb_ptr> rdb_v;
  typedef std::unordered_map<uint16_t, uint16_t> _peer_map;

  typedef enum { START, WORKING, CANCELD } state_t;

  Storage& _storage;
  state_t _state;
  rdb_v _rdbs;
  buffer_t _received_msg;
  buffer_t _send_buffer;
  rdb_ptr _rdb;

  _Replicator(const db_v& dbs) : _storage(dbs[0]->_storage), _state(START) {
    uint16_t idx = 0;
    for (const auto& db : dbs) {
      _rdbs.emplace_back(std::make_shared<RDB>(db, idx++));
    }
    _rdb = rdbs[0];
  }

  rdb_ptr rdb() { return _rdb; }

  void rdb(const Slice& name) {
    if (_state != START_SYNC) throw logic_error();
    const StartMessage* msg = (StartMessage*)_received_msg.data();
    int i = 0;
    for (auto& rdb_ : _rdbs) {
      if (name == rdb_->name()) {
        _peer_map[msg->idx] = i;
        return;
      }
      i++;
    }
    throw std::invalid_argument();
  }

  Slice peer() const {
    if (_state != START_SYNC) throw logic_error();
    const StartMessage* msg = (StartMessage*)_received_msg.data();
    assert(msg->type == MSG_START);
    return Slice(msg->name, msg->size - sizeof(msg->idx));
  }

  buffer_t& send_buffer() { return _send_buffer; }

  void start() {
    if (_state != START_SYNC) throw logic_error();
    const StartMessage* msg = (StartMessage*)_received_msg.data();
    assert(msg->type == MSG_START);
    auto peer_idx = msg->idx;
    _cut_recveive_msg();
    assert(_peer_map.find(peer_idx) != _peer_map.end());
    _rdbs[_peer_map[peer_idx]]->start();
    _handle_receive();
  }

  void cancel() {
    if (_state != START_SYNC) throw logic_error();
    const StartMessage* msg = (StartMessage*)_received_msg.data();

    buffer_t buffer;
    buffer.resize(sizeof(CancelMessage));
    CancelMessage* cm = (CancelMessage*)buffer->data();
    cm->type = MSG_CANCEL;
    cm->size = sizeof(CancelMessage);
    cm->idx = msg->idx;
    _add_to_send_buffer(std::move(buffer));

    assert(msg->type == MSG_START);
    _peer_map.erase(msg->idx);
    _cut_recveive_msg();
    _handle_receive();
  }

  command_t execute() {
    if (_state == START) return START_SYNC;
    return _handle_dbs();
  }

  template <typename Buffer>
  void receive(Buffer&& buffer) {
    if (!_received_msg.size())
      _received_msg = std::move(buffer);
    else
      _received_msg.append(buffer);
  }

  command_t _handle_dbs() {
    if (_send_buffer.size()) return SEND;

    command_t result = WAIT;
    for (int i = 0, size = _rdbs.size(); i < size; i++) {
      command_t command = _rdb->execute();
      switch (command) {
        case RECEIVE:
          result = RECEIVE;

        case WAIT:
          continue;

        case SEND:
          _add_to_send_buffer(std::move(rdb->send_buffer));

        case ERROR:
          return command;  // don't change rdb

        default:
          result = command;
      }
      _rdb = _rdbs[(_rdb->idx + 1) % size];
    }
    return result;
  }

  void _handle_receive() {
    while (_received_msg.size() > sizeof(Message::size) + 1) {
      const Message* msg = (const Message*)buffer.data();
      if (msg->size < _received_msg.size) return;
      switch (msg->type) {
        case MSG_START: {
          _state = START;
          const StartMessage* msg = (const StartMessage*)buffer.data();
          rdb(msg->name());
          return;
        }

        case MSG_CANCEL: {
          const CancelMessage* msg = (const CancelMessage*)buffer.data();
          _rdb[msg->idx]->cancel();
          _cut_recveive_msg();
        } break;

        default:
          auto piter = _peer_map.find(msg->idx);
          if (piter != _peer_map.end()) _rdbs[_peer_map[*piter]]->receive(msg);
          _cut_recveive_msg();
      }
    }
  }

  void _add_to_send_buffer(buffer_t&& buffer) {
    if (!_send_buffer.size())
      _send_buffer = std::move(buffer);
    else
      _send_buffer.append(buffer);
  }

  void _cut_recveive_msg() {
    const Message* msg = (const Message*)buffer.data();
    assert(msg->size >= _received_msg.size);
    _received_msg.erase(_received_msg.begin(),
                        _received_msg.begin() + msg->size);
  }
};

void repl() {
  Replicator r;
  Socket socket;

  switch (r.execute()) {
    case SEND:
      socket.write(r.send_buffer.data(), r.send_buffer.size());
      break;

    case START_SYNC:
      if (r.peer() == "eee") {
        r.rdb("uuu");  // change the rdb
        r.start();
      } else
        r.cancel();

      break;

    case INSERT: {
      auto cursor = r.rdb()->cursor;
      for (cursor.first(); cursor.is_valid();) {
        if (!pred(cursor))
          cursor.remove();
        else
          cursor.next();
      }
      r.rdb()->insert();
      break;
    }

    case ERROR:
      std::cout << "error sync" << r.error_desc.rdb->name << ", "
                << r.error_desc.peer << ", " << r.error_desc.type << std::endl;
      break;
  }

  buffer_t buffer;
  buffer.resize(64 * K);
  buffer.resize(socket.read(buffer.data(), buffer.size(), timeout = 1));
  if (buffer.size()) r.receive(std::move(buffer));
}

}  // namespace Replication
}  // namespace leaves

#endif  // _LEAVES__REPLICATOR_HPP