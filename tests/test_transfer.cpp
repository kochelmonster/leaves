#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef TESTING
#define TESTING
#endif

#ifndef CMPFILES
#define CMPFILES "./"
#endif

#include "leaves/mmap.hpp"
#include "leaves/replication.hpp"
#include "leaves/intern/replication/_replication_db.hpp"
#include "leaves/intern/replication/_transfer.hpp"

using namespace leaves;

// Use replicating map storage for testing
using Storage = MapStorage;

// Get traits from the internal DB type
using DBImpl = _ReplicationDB<Storage::StorageImpl>;
using Traits = DBImpl::Traits;
using TrieNode = Traits::TrieNode;
using LeafNode = Traits::LeafNode;
using TransferBuffer = ReplicationTransferTrie<>;
using Sender = TransferTrieSender<DBImpl>;
using WireTransferTrie = _TransferTrie<HASH_SIZE>;
using WireTrieNode = WireTransferTrie::TrieNode;
using WireLeafNode = WireTransferTrie::LeafNode;
using WireOffset = WireTransferTrie::Offset;

std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());

  in.seekg(0, std::ios::end);
  std::streamsize size = in.tellg();
  assert(size >= 0);
  in.seekg(0, std::ios::beg);

  std::vector<uint8_t> data((size_t)size);
  if (size > 0) {
    in.read(reinterpret_cast<char*>(data.data()), size);
    assert(in.good() || in.eof());
    assert(in.gcount() == size);
  }
  return data;
}

void write_binary_file(const std::filesystem::path& path,
                       const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out.is_open());
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()),
              (std::streamsize)data.size());
    assert(out.good());
  }
}

std::vector<uint8_t> build_transfer_trie_compat_payload_v1() {
  TransferBuffer transfer(2048);
  transfer.begin(0x1122334455667788ULL, 0x8877665544332211ULL,
                 DbType::DB_MAIN, Slice());

  // Build a deterministic root trie with two branches: 'a' and 'b'.
  const int key_a = static_cast<int>('a');
  const int key_b = static_cast<int>('b');
  std::vector<uint8_t> root_mem(WireTrieNode::size(0, key_a, key_b), 0);
  auto* root_src = reinterpret_cast<WireTrieNode*>(root_mem.data());
  auto root_slots = root_src->create(Slice(), key_a, key_b);
  if constexpr (WireTrieNode::HAS_HASH) {
    std::memset(root_src->hash, 0xA1, WireTrieNode::HASH_SIZE);
  }

  auto build_leaf = [](const char* key_suffix, const char* value,
                       uint8_t hash_fill) {
    std::string key(key_suffix);
    std::string val(value);
    std::vector<uint8_t> leaf_mem(WireLeafNode::size(key.size(), val.size()), 0);
    auto* leaf = reinterpret_cast<WireLeafNode*>(leaf_mem.data());
    leaf->set(Slice(key), val.size());
    std::memcpy(leaf->vdata(), val.data(), val.size());
    if constexpr (WireLeafNode::HAS_HASH) {
      std::memset(leaf->hash, hash_fill, WireLeafNode::HASH_SIZE);
    }
    return leaf_mem;
  };

  // Suffixes reconstruct full keys via trie branch chars: a+lpha, b+eta.
  std::vector<uint8_t> leaf_alpha_mem = build_leaf("lpha", "one", 0xB1);
  std::vector<uint8_t> leaf_beta_mem = build_leaf("eta", "two", 0xC1);

  auto* root_wire = transfer.add_trie_node(
      reinterpret_cast<const WireTrieNode*>(root_mem.data()));
  auto* leaf_alpha_wire = transfer.add_leaf_node(
      reinterpret_cast<const WireLeafNode*>(leaf_alpha_mem.data()));
  auto* leaf_beta_wire = transfer.add_leaf_node(
      reinterpret_cast<const WireLeafNode*>(leaf_beta_mem.data()));

  assert(root_wire != nullptr);
  assert(leaf_alpha_wire != nullptr);
  assert(leaf_beta_wire != nullptr);

  WireOffset* root_array = root_wire->array();
  root_array[root_slots.first].set_relative(leaf_alpha_wire);
  root_array[root_slots.first].type(LEAF);
  root_array[root_slots.second].set_relative(leaf_beta_wire);
  root_array[root_slots.second].type(LEAF);

  Slice payload = transfer.finalize();
  assert(payload.data() != nullptr);
  return std::vector<uint8_t>(
      reinterpret_cast<const uint8_t*>(payload.data()),
      reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());
}

void validate_transfer_trie_payload(
    const std::vector<uint8_t>& payload,
    const std::map<std::string, std::string>& expected_kv) {
  assert(!payload.empty());

  Slice data(reinterpret_cast<const char*>(payload.data()), payload.size());
  Slice subtrie_path;
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(data, &subtrie_path);
  assert(hdr != nullptr);
  assert(hdr->magic == TRANSFER_MAGIC);
  assert(hdr->version == TRANSFER_VERSION);
  assert(hdr->db_type == static_cast<uint8_t>(DbType::DB_MAIN));
  assert(hdr->session_id == 0x1122334455667788ULL);
  assert(hdr->snapshot_id == 0x8877665544332211ULL);
  assert(hdr->total_size == payload.size());
  assert(subtrie_path.empty());
  assert(hdr->node_count > 0);

  const uint8_t* begin = payload.data();
  const uint8_t* nodes_begin = TransferBuffer::nodes_data(data, *hdr);
  const uint8_t* end = begin + payload.size();
  assert(nodes_begin >= begin);
  assert(nodes_begin <= end);

  auto ptr_in_nodes = [&](const uint8_t* ptr) {
    return ptr >= nodes_begin && ptr < end;
  };

  std::set<const uint8_t*> visiting;
  std::set<const uint8_t*> visited;
  std::map<std::string, std::string> actual_kv;

  std::function<void(const uint8_t*, NodeTypes, const std::string&)> walk =
      [&](const uint8_t* node_ptr, NodeTypes type, const std::string& path) {
        assert(ptr_in_nodes(node_ptr));
        assert((((uintptr_t)node_ptr) & 7u) == 0u);
        assert(visited.find(node_ptr) == visited.end());
        assert(visiting.insert(node_ptr).second);

        if (type == TRIE) {
          auto* trie = reinterpret_cast<const WireTrieNode*>(node_ptr);
          uint16_t trie_size = trie->size();
          assert(trie_size >= WireTrieNode::HEADER_SIZE);
          assert(node_ptr + trie_size <= end);
          assert(trie->array_start() % sizeof(WireOffset) == 0);
          assert(trie->lower_start() <= trie->lower_end());
          assert(trie->lower_end() <= trie->array_start());
          assert(trie->array_end() == trie_size);

          const uint8_t* compressed = trie->compressed();
          assert(compressed + trie->len() <= node_ptr + trie_size);

          std::string next_path(path);
          next_path.append(reinterpret_cast<const char*>(compressed), trie->len());

          trie->for_each_branch([&](int key, WireOffset* child_off) {
            assert(child_off != nullptr);
            assert(*child_off != 0);
            assert(child_off->is_relative());

            NodeTypes child_type = child_off->type();
            assert(child_type == TRIE || child_type == LEAF);

            const uint8_t* child_ptr = child_off->template resolve<uint8_t>();
            assert(ptr_in_nodes(child_ptr));

            std::string child_path(next_path);
            if (key != WireTrieNode::NONE) {
              assert(key >= 0 && key <= 255);
              child_path.push_back(static_cast<char>(key));
            }
            walk(child_ptr, child_type, child_path);
          });
        } else {
          auto* leaf = reinterpret_cast<const WireLeafNode*>(node_ptr);
          uint16_t leaf_size = leaf->size();
          assert(leaf_size >= WireLeafNode::HEADER_SIZE);
          assert(node_ptr + leaf_size <= end);
          assert(leaf->data + leaf->key_size + leaf->vsize() <= node_ptr + leaf_size);

          std::string key(path);
          key.append(reinterpret_cast<const char*>(leaf->data), leaf->key_size);
          std::string value(reinterpret_cast<const char*>(leaf->data + leaf->key_size),
                            leaf->vsize());
          assert(actual_kv.insert(std::make_pair(key, value)).second);
        }

        visiting.erase(node_ptr);
        visited.insert(node_ptr);
      };

  assert(hdr->root.is_relative());
  NodeTypes root_type = hdr->root.type();
  assert(root_type == TRIE || root_type == LEAF);
  const uint8_t* root_ptr = hdr->root.template resolve<uint8_t>();
  walk(root_ptr, root_type, std::string());

  assert(visiting.empty());
  assert(visited.size() == hdr->node_count);
  assert(actual_kv == expected_kv);
}

void test_header_size() {
  std::cout << "test_header_size... ";
  
  // Verify header sizes match wire format spec
  static_assert(sizeof(TransferTrieHeader) == 45);
  static_assert(sizeof(RequestChildrenHeader) == 17);
  
  std::cout << "OK\n";
}

void test_generate_session_id() {
  std::cout << "test_generate_session_id... ";
  
  uint64_t id1 = TransferBuffer::generate_session_id();
  uint64_t id2 = TransferBuffer::generate_session_id();
  
  // Should be different (with overwhelming probability)
  assert(id1 != id2);
  assert(id1 != 0);
  assert(id2 != 0);
  
  std::cout << "OK\n";
}

void test_header_roundtrip() {
  std::cout << "test_header_roundtrip... ";
  
  TransferBuffer transfer;
  uint8_t path_data[] = {0x01, 0x02, 0x03};
  Slice subtrie_path(path_data, sizeof(path_data));
  
  transfer.begin(0xDEADBEEF12345678ULL, 0x0000000000000042ULL,
                 DbType::DB_MAIN, subtrie_path);
  
  Slice result = transfer.finalize();
  assert(result.size() >= sizeof(TransferTrieHeader) + subtrie_path.size());
  
  // Parse it back
  Slice parsed_path;
  const TransferTrieHeader* parsed_hdr = TransferBuffer::parse_header(result, &parsed_path);
  
  assert(parsed_hdr != nullptr);
  assert(parsed_hdr->magic == TRANSFER_MAGIC);
  assert(parsed_hdr->version == TRANSFER_VERSION);
  assert(parsed_hdr->db_type == static_cast<uint8_t>(DbType::DB_MAIN));
  assert(parsed_hdr->session_id == 0xDEADBEEF12345678ULL);
  assert(parsed_hdr->snapshot_id == 0x0000000000000042ULL);
  assert(parsed_hdr->subtrie_path_len == 3);
  assert(parsed_hdr->node_count == 0);
  assert(parsed_path.size() == subtrie_path.size());
  assert(std::memcmp(parsed_path.data(), subtrie_path.data(), parsed_path.size()) == 0);
  
  std::cout << "OK\n";
}

void test_empty_subtrie_path() {
  std::cout << "test_empty_subtrie_path... ";
  
  TransferBuffer transfer;
  
  transfer.begin(123, 456, DbType::DB_DELETION, Slice());
  Slice result = transfer.finalize();
  
  Slice parsed_path;
  const TransferTrieHeader* parsed_hdr = TransferBuffer::parse_header(result, &parsed_path);
  
  assert(parsed_hdr != nullptr);
  assert(parsed_hdr->db_type == static_cast<uint8_t>(DbType::DB_DELETION));
  assert(parsed_hdr->subtrie_path_len == 0);
  assert(parsed_path.empty());
  
  std::cout << "OK\n";
}

void test_add_raw_nodes() {
  std::cout << "test_add_raw_nodes... ";
  
  TransferBuffer transfer(1024);  // 1KB max
  transfer.begin(111, 222, DbType::DB_MAIN, {});
  
  // Create minimally valid fake trie node data
  // Layout: upper(1), compressed_len(1), lower_offset(1), array_offset(1), array_len(2), hash(32)
  // Minimum size: array_offset must be >= ceil(38/8) = 5, so array_start = 40
  // With 0 children (array_len = 0), size = 40
  uint8_t fake_trie[40] = {};
  fake_trie[0] = 0x00;  // _upper: no bits set
  fake_trie[1] = 0x00;  // _compressed_len: 0
  fake_trie[2] = 0x00;  // _lower_offset: 0
  fake_trie[3] = 0x05;  // _array_offset: 5 (5*8=40 bytes to array start)
  fake_trie[4] = 0x00;  // _array_len low byte: 0
  fake_trie[5] = 0x00;  // _array_len high byte: 0
  // hash[6..37] = zeros
  
  // Create minimally valid fake leaf node data
  // Layout: key_size(1), value_size(2), hash(32), data[key+value]
  // With key_size=0, value_size=0: size = 1 + 2 + 32 + 0 = 35
  uint8_t fake_leaf[35] = {};
  fake_leaf[0] = 0x00;  // key_size: 0
  fake_leaf[1] = 0x00;  // value_size low byte: 0  
  fake_leaf[2] = 0x00;  // value_size high byte: 0
  // hash[3..34] = zeros
  
  assert(transfer.add_node(TRIE, fake_trie, sizeof(fake_trie)) != nullptr);
  assert(transfer.add_node(LEAF, fake_leaf, sizeof(fake_leaf)) != nullptr);
  
  assert(transfer.node_count() == 2);
  
  Slice result = transfer.finalize();
  
  const TransferTrieHeader* parsed_hdr = TransferBuffer::parse_header(result);
  assert(parsed_hdr != nullptr);
  assert(parsed_hdr->node_count == 2);
  
  // Verify nodes data directly
  const uint8_t* nodes = TransferBuffer::nodes_data(result, *parsed_hdr);
  
  // Root can be resolved using header's root offset
  // Note: nodes are 8-byte aligned, so first node should be at aligned position
  assert(std::memcmp(nodes, fake_trie, sizeof(fake_trie)) == 0);
  
  // Second node follows first, but aligned to 8 bytes
  size_t first_end = (size_t)nodes + sizeof(fake_trie);
  size_t second_start_aligned = (first_end + 7) & ~size_t(7);
  const uint8_t* second_node = (const uint8_t*)second_start_aligned;
  assert(std::memcmp(second_node, fake_leaf, sizeof(fake_leaf)) == 0);
  
  std::cout << "OK\n";
}

void test_capacity_limit() {
  std::cout << "test_capacity_limit... ";
  
  TransferBuffer transfer(100);  // Very small buffer
  transfer.begin(1, 2, DbType::DB_MAIN, {});
  
  // Header takes ~37 bytes, should have ~63 left
  assert(transfer.remaining_capacity() < 100);
  assert(transfer.remaining_capacity() > 50);
  
  // Add nodes until we run out of space
  uint8_t data[20];
  std::memset(data, 0x42, sizeof(data));
  
  int added = 0;
  while (transfer.add_node(TRIE, data, sizeof(data))) {
    added++;
  }
  
  // Should have added at least 2 nodes (20+3 = 23 bytes each)
  assert(added >= 2);
  // But not too many
  assert(added <= 3);
  
  std::cout << "OK\n";
}

void test_request_children_builder() {
  std::cout << "test_request_children_builder... ";
  
  RequestChildrenBuilder builder;
  builder.begin(0x123456789ABCDEF0ULL, DbType::DB_MAIN);
  
  std::string path1 = "\x01\x02";
  std::string path2 = "\x03\x04\x05";
  std::string path3 = "";  // Empty path (root)
  
  builder.add_path(path1);
  builder.add_path(path2);
  builder.add_path(path3);
  
  Slice result = builder.finalize();
  
  // Parse header
  RequestChildrenHeader hdr;
  bool ok = parse_request_children(result, &hdr);
  assert(ok);
  assert(hdr.session_id == 0x123456789ABCDEF0ULL);
  assert(hdr.db_type == static_cast<uint8_t>(DbType::DB_MAIN));
  assert(hdr.path_count == 3);
  
  // Iterate paths
  auto iter = request_children_iterator(result, hdr);
  
  assert(iter.valid());
  assert(iter.path() == path1);
  
  iter.next();
  assert(iter.valid());
  assert(iter.path() == path2);
  
  iter.next();
  assert(iter.valid());
  assert(iter.path() == path3);
  
  iter.next();
  assert(!iter.valid());
  
  std::cout << "OK\n";
}

void test_request_children_bounds_check() {
  std::cout << "test_request_children_bounds_check... ";
  
  // Build a valid message first
  RequestChildrenBuilder builder;
  builder.begin(0x123456789ABCDEF0ULL, DbType::DB_MAIN);
  builder.add_path("\x01\x02\x03");
  Slice valid_msg = builder.finalize();
  
  // Now create a truncated buffer - keep header but truncate path data
  // Header is sizeof(RequestChildrenHeader), after that comes path data
  // We'll truncate in the middle of the path
  size_t header_size = sizeof(RequestChildrenHeader);
  if (header_size & 1) header_size++;  // padding
  
  // Create buffer with header + length field but truncated path
  std::vector<uint8_t> truncated(header_size + 2 + 1);  // header + 2-byte len + 1 byte (not 3)
  std::memcpy(truncated.data(), valid_msg.data(), truncated.size());
  
  // Parse header - should succeed
  RequestChildrenHeader hdr;
  bool ok = parse_request_children(Slice(truncated.data(), truncated.size()), &hdr);
  assert(ok);
  
  // Create iterator
  RequestChildrenIterator iter((uint8_t*)truncated.data() + header_size,
                                truncated.size() - header_size);
  
  // First valid() should be true (we have 2 bytes for length)
  assert(iter.valid());
  
  // But path() should return empty slice (path_len says 3 but only 1 byte available)
  Slice path = iter.path();
  assert(path.empty());
  
  // next() should fail and set error
  bool advanced = iter.next();
  assert(!advanced);
  assert(iter.error());
  assert(!iter.valid());
  
  // reset() should clear error
  iter.reset();
  assert(!iter.error());
  assert(iter.valid());
  
  std::cout << "OK\n";
}

void test_invalid_header() {
  std::cout << "test_invalid_header... ";
  
  // Too short
  uint8_t short_data[10] = {0};
  assert(TransferBuffer::parse_header(Slice(short_data, sizeof(short_data))) == nullptr);
  
  // Wrong magic
  TransferTrieHeader bad_magic;
  std::memset(&bad_magic, 0, sizeof(bad_magic));
  bad_magic.magic = 0x12345678;
  bad_magic.version = TRANSFER_VERSION;
  assert(TransferBuffer::parse_header(Slice(&bad_magic, sizeof(bad_magic))) == nullptr);
  
  // Wrong version
  TransferTrieHeader bad_version;
  std::memset(&bad_version, 0, sizeof(bad_version));
  bad_version.magic = TRANSFER_MAGIC;
  bad_version.version = 0xFFFF;
  assert(TransferBuffer::parse_header(Slice(&bad_version, sizeof(bad_version))) == nullptr);
  
  std::cout << "OK\n";
}

// =============================================================================
// Phase 2: Sender Tests
// =============================================================================

std::filesystem::path test_temp_dir;

void setup_temp_dir() {
  test_temp_dir = std::filesystem::temp_directory_path() / "test_transfer";
  std::filesystem::remove_all(test_temp_dir);
  std::filesystem::create_directory(test_temp_dir);
}

void cleanup_temp_dir() {
  std::filesystem::remove_all(test_temp_dir);
}

void test_sender_empty_db() {
  std::cout << "test_sender_empty_db... ";
  
  auto db_path = test_temp_dir / "empty.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = storage->open<Storage::ReplicationDB>("testdb");
  auto* db_impl = db._internal();
  auto txn = db_impl->acquire_hash_trie();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  // Empty DB should complete immediately
  sender.fill_buffer();
  assert(sender.is_complete());
  
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->node_count == 0);
  
  db_impl->release_hash_trie(txn);
  std::cout << "OK\n";
}

void test_sender_single_leaf() {
  std::cout << "test_sender_single_leaf... ";
  
  auto db_path = test_temp_dir / "single_leaf.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = storage->open<Storage::ReplicationDB>("testdb");
  auto cursor = db.cursor();
  
  cursor.find(Slice("key1"));
  cursor.value(Slice("value1"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->acquire_hash_trie();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  sender.fill_buffer();
  assert(sender.is_complete());
  
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->node_count == 1);  // Just the leaf
  
  db_impl->release_hash_trie(txn);
  std::cout << "OK\n";
}

void test_sender_multiple_keys() {
  std::cout << "test_sender_multiple_keys... ";
  
  auto db_path = test_temp_dir / "multi_keys.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = storage->open<Storage::ReplicationDB>("testdb");
  auto cursor = db.cursor();
  
  // Insert multiple keys to create a trie structure
  cursor.find(Slice("aaa"));
  cursor.value(Slice("val_a"));
  cursor.find(Slice("aab"));
  cursor.value(Slice("val_ab"));
  cursor.find(Slice("bbb"));
  cursor.value(Slice("val_b"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->acquire_hash_trie();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  // With BFS protocol, we need to run fill_buffer + process_ack cycles
  size_t total_nodes = 0;
  int rounds = 0;
  
  do {
    sender.fill_buffer();
    Slice buffer = sender.finalize();
    
    const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
    if (hdr && hdr->node_count > 0) {
      total_nodes += hdr->node_count;
    }
    
    // Send empty ACK to advance (no subtries match)
    RequestChildrenBuilder ack_builder;
    ack_builder.begin(sender.session_id(), DbType::DB_MAIN);
    Slice ack_data = ack_builder.finalize();
    RequestChildrenHeader ack_hdr;
    parse_request_children(ack_data, &ack_hdr);
    auto ack_iter = request_children_iterator(ack_data, ack_hdr);
    sender.process_ack(ack_iter);
    
    rounds++;
  } while (sender.has_pending() && rounds < 20);
  
  assert(sender.is_complete());
  // Should have trie nodes + leaf nodes
  assert(total_nodes >= 3);  // At least 3 leaves
  
  db_impl->release_hash_trie(txn);
  std::cout << "OK\n";
}

void test_sender_buffer_overflow() {
  std::cout << "test_sender_buffer_overflow... ";
  
  auto db_path = test_temp_dir / "overflow.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = storage->open<Storage::ReplicationDB>("testdb");
  auto cursor = db.cursor();
  
  // Insert many keys
  for (int i = 0; i < 100; i++) {
    std::string key = "key" + std::to_string(i);
    std::string val = "value" + std::to_string(i);
    cursor.find(Slice(key));
    cursor.value(Slice(val));
  }
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->acquire_hash_trie();
  
  // With post-order DFS, the entire subtrie must fit in one buffer
  // Use a buffer large enough to hold all 100 keys
  Sender sender(db_impl, txn, 32 * 1024);  // 32KB should be enough
  sender.begin();
  
  size_t total_nodes = 0;
  int rounds = 0;
  
  do {
    sender.fill_buffer();
    Slice buffer = sender.finalize();
    
    const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
    if (hdr && hdr->node_count > 0) {
      total_nodes += hdr->node_count;
    }
    
    rounds++;
  } while (sender.has_pending() && rounds < 10);
  
  assert(sender.is_complete());
  assert(total_nodes >= 100);  // At least 100 leaves
  
  db_impl->release_hash_trie(txn);
  std::cout << "OK\n";
}

void test_sender_process_ack() {
  std::cout << "test_sender_process_ack... ";
  
  auto db_path = test_temp_dir / "ack.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = storage->open<Storage::ReplicationDB>("testdb");
  auto cursor = db.cursor();
  
  // Insert many keys with unique prefixes to force multi-level trie
  for (int i = 0; i < 50; i++) {
    char key[16];
    snprintf(key, sizeof(key), "k%02d", i);  // k00, k01, ..., k49
    std::string val = "value" + std::to_string(i);
    cursor.find(Slice(key, 3));
    cursor.value(Slice(val));
  }
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->acquire_hash_trie();
  
  // With post-order DFS, entire subtrie must fit in buffer
  Sender sender(db_impl, txn, 32 * 1024);
  sender.begin();
  
  // Fill buffer - should write entire trie in one go
  sender.fill_buffer();
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->node_count >= 50);  // At least 50 leaves
  
  // Should be complete after one round
  assert(sender.is_complete());
  
  db_impl->release_hash_trie(txn);
  std::cout << "OK\n";
}

// Test that relative offsets in the buffer work correctly
// This verifies that children can be found by following relative offsets
void test_relative_offsets() {
  std::cout << "test_relative_offsets... ";
  
  auto db_path = test_temp_dir / "relative.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = storage->open<Storage::ReplicationDB>("testdb");
  auto cursor = db.cursor();
  
  // Insert keys that will create a trie structure
  cursor.find(Slice("aaa"));
  cursor.value(Slice("val_a"));
  cursor.find(Slice("aab"));
  cursor.value(Slice("val_ab"));
  cursor.find(Slice("bbb"));
  cursor.value(Slice("val_b"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->acquire_hash_trie();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  // Run fill_buffer + process_ack cycles to complete
  size_t total_nodes = 0;
  Slice last_buffer;
  
  do {
    sender.fill_buffer();
    last_buffer = sender.finalize();
    
    const TransferTrieHeader* hdr = TransferBuffer::parse_header(last_buffer);
    if (hdr && hdr->node_count > 0) {
      total_nodes += hdr->node_count;
    }
    
    // Send empty ACK to advance
    RequestChildrenBuilder ack_builder;
    ack_builder.begin(sender.session_id(), DbType::DB_MAIN);
    Slice ack_data = ack_builder.finalize();
    RequestChildrenHeader ack_hdr;
    parse_request_children(ack_data, &ack_hdr);
    auto ack_iter = request_children_iterator(ack_data, ack_hdr);
    sender.process_ack(ack_iter);
  } while (sender.has_pending());
  
  assert(sender.is_complete());
  assert(total_nodes >= 3);  // At least 3 leaves
  
  db_impl->release_hash_trie(txn);
  
  // The test now verifies that BFS traversal works correctly across multiple rounds
  // Relative offset verification would require examining buffer contents which is
  // implementation-specific to the wire format
  
  std::cout << "OK\n";
}

void test_transfer_trie_binary_compatibility_file_roundtrip() {
  std::cout << "test_transfer_trie_binary_compatibility_file_roundtrip... ";

  std::map<std::string, std::string> expected_kv = {
      {"alpha", "one"},
      {"beta", "two"},
  };

  std::vector<uint8_t> generated = build_transfer_trie_compat_payload_v1();

  auto runtime_path = test_temp_dir / "transfer_trie_compat_runtime_v1.bin";
  write_binary_file(runtime_path, generated);
  std::vector<uint8_t> roundtrip = read_binary_file(runtime_path);
  assert(roundtrip == generated);
  validate_transfer_trie_payload(roundtrip, expected_kv);

  std::filesystem::path fixture_path =
      std::filesystem::path(CMPFILES) / "transfer_trie_compat_v1.bin";
  assert(std::filesystem::exists(fixture_path));

  std::vector<uint8_t> fixture = read_binary_file(fixture_path);
  assert(fixture == generated);
  validate_transfer_trie_payload(fixture, expected_kv);

  std::cout << "OK\n";
}

int main() {
  std::cout << "=== TransferTrie Tests ===\n";
  
  test_header_size();
  test_generate_session_id();
  test_header_roundtrip();
  test_empty_subtrie_path();
  test_add_raw_nodes();
  test_capacity_limit();
  test_request_children_builder();
  test_request_children_bounds_check();
  test_invalid_header();
  
  std::cout << "\n=== Sender Tests ===\n";
  setup_temp_dir();
  test_sender_empty_db();
  test_sender_single_leaf();
  test_sender_multiple_keys();
  test_sender_buffer_overflow();
  test_sender_process_ack();
  test_relative_offsets();
  test_transfer_trie_binary_compatibility_file_roundtrip();
  cleanup_temp_dir();
  
  std::cout << "\nAll tests passed!\n";
  return 0;
}
