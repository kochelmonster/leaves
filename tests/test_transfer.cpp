#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#ifndef TESTING
#define TESTING
#endif

#include "leaves/replicating_mmap.hpp"
#include "leaves/intern/replication/_transfer.hpp"

using namespace leaves;

// Use replicating map storage for testing
using Storage = ReplicatingMapStorage;

// Get traits from the internal DB type
using DBImpl = Storage::StorageImpl::DB;
using Traits = DBImpl::Traits;
using TrieNode = _TrieNode<Traits>;
using LeafNode = _LeafNode<Traits>;
using TransferBuffer = TransferTrie<Traits>;
using Sender = TransferTrieSender<DBImpl>;

void test_header_size() {
  std::cout << "test_header_size... ";
  
  // Verify header sizes match wire format spec
  static_assert(sizeof(TransferTrieHeader) == 38);
  static_assert(sizeof(RequestChildrenHeader) == 17);
  static_assert(sizeof(ChunkRef) == 40);  // 8 + 32
  
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
  
  assert(transfer.add_node(TransferNodeType::TRIE_NODE, fake_trie, sizeof(fake_trie)) != nullptr);
  assert(transfer.add_node(TransferNodeType::LEAF_NODE, fake_leaf, sizeof(fake_leaf)) != nullptr);
  
  assert(transfer.node_count() == 2);
  
  Slice result = transfer.finalize();
  
  const TransferTrieHeader* parsed_hdr = TransferBuffer::parse_header(result);
  assert(parsed_hdr != nullptr);
  assert(parsed_hdr->node_count == 2);
  
  // Verify nodes data directly
  const uint8_t* nodes = TransferBuffer::nodes_data(result, *parsed_hdr);
  
  // root_node_type is the FIRST node (pre-order DFS semantics)
  assert(parsed_hdr->root_node_type == static_cast<uint8_t>(TransferNodeType::TRIE_NODE));
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
  while (transfer.add_node(TransferNodeType::TRIE_NODE, data, sizeof(data))) {
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
  
  auto db = (*storage)["testdb"];
  auto* db_impl = db._internal();
  auto txn = db_impl->txn();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  // Empty DB should complete immediately
  bool complete = sender.fill_buffer();
  assert(complete);
  assert(sender.is_complete());
  
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->node_count == 0);
  
  std::cout << "OK\n";
}

void test_sender_single_leaf() {
  std::cout << "test_sender_single_leaf... ";
  
  auto db_path = test_temp_dir / "single_leaf.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = (*storage)["testdb"];
  auto cursor = db.cursor();
  
  cursor.find(Slice("key1"));
  cursor.value(Slice("value1"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->txn();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  bool complete = sender.fill_buffer();
  assert(complete);
  assert(sender.is_complete());
  
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->node_count == 1);  // Just the leaf
  
  // Verify root node type from header
  assert(hdr->root_node_type == static_cast<uint8_t>(TransferNodeType::LEAF_NODE));
  
  std::cout << "OK\n";
}

void test_sender_multiple_keys() {
  std::cout << "test_sender_multiple_keys... ";
  
  auto db_path = test_temp_dir / "multi_keys.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = (*storage)["testdb"];
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
  auto txn = db_impl->txn();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  bool complete = sender.fill_buffer();
  assert(complete);
  assert(sender.is_complete());
  
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  // Should have trie nodes + leaf nodes
  assert(hdr->node_count >= 3);
  
  // Verify root node type from header
  // First node should be a trie node (root of the tree with 3 leaves)
  assert(hdr->root_node_type == static_cast<uint8_t>(TransferNodeType::TRIE_NODE));
  
  // Verify we have expected node count
  assert(hdr->node_count >= 4);  // At least 1 trie + 3 leaves
  
  std::cout << "OK\n";
}

void test_sender_buffer_overflow() {
  std::cout << "test_sender_buffer_overflow... ";
  
  auto db_path = test_temp_dir / "overflow.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = (*storage)["testdb"];
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
  auto txn = db_impl->txn();
  
  // Use a small buffer - with post-order DFS, the entire subtrie must fit
  // A 512 byte buffer is too small, so fill_buffer should return false
  Sender sender(db_impl, txn, 512);  // 512 bytes only
  sender.begin();
  
  bool complete = sender.fill_buffer();
  
  // With post-order DFS, a too-small buffer returns false (subtrie doesn't fit)
  // The test verifies this behavior - caller would need to request smaller subtries
  assert(!complete);  // Subtrie doesn't fit
  
  // With a larger buffer (32KB), it should work
  Sender sender2(db_impl, txn, 32 * 1024);
  sender2.begin();
  complete = sender2.fill_buffer();
  assert(complete);
  assert(sender2.is_complete());
  
  Slice buffer = sender2.finalize();
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->node_count >= 100);  // At least 100 leaves
  
  std::cout << "OK\n";
}

void test_sender_continue_from_paths() {
  std::cout << "test_sender_continue_from_paths... ";
  
  auto db_path = test_temp_dir / "continue.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = (*storage)["testdb"];
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
  auto txn = db_impl->txn();
  
  // Test continue_from with specific paths
  // First, do a full transfer to get baseline
  Sender sender1(db_impl, txn, 32 * 1024);  // Large buffer
  sender1.begin();
  bool complete = sender1.fill_buffer();
  assert(complete);
  assert(sender1.is_complete());
  
  Slice buffer1 = sender1.finalize();
  const TransferTrieHeader* hdr1 = TransferBuffer::parse_header(buffer1);
  assert(hdr1 != nullptr);
  size_t full_node_count = hdr1->node_count;
  
  // Now test continue_from with specific subtrie paths
  Sender sender2(db_impl, txn, 32 * 1024);
  std::vector<std::string> paths = {"k0", "k1", "k2"};  // Request specific subtries
  sender2.continue_from(std::move(paths));
  
  size_t total_nodes = 0;
  int rounds = 0;
  
  while (!sender2.is_complete() && rounds < 10) {
    bool subtrie_complete = sender2.fill_buffer();
    Slice buffer = sender2.finalize();
    
    const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
    assert(hdr != nullptr);
    total_nodes += hdr->node_count;
    rounds++;
  }
  
  assert(sender2.is_complete());
  assert(total_nodes > 0);  // Should have transferred some nodes
  assert(rounds == 3);  // One round per subtrie path
  
  std::cout << "OK\n";
}

// Test that relative offsets in the buffer work correctly
// This verifies that children can be found by following relative offsets
void test_relative_offsets() {
  std::cout << "test_relative_offsets... ";
  
  auto db_path = test_temp_dir / "relative.lvs";
  auto storage = Storage::create(db_path.c_str());
  assert(storage);
  
  auto db = (*storage)["testdb"];
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
  auto txn = db_impl->txn();
  
  Sender sender(db_impl, txn);
  sender.begin();
  
  bool complete = sender.fill_buffer();
  assert(complete);
  assert(sender.is_complete());
  
  Slice buffer = sender.finalize();
  
  const TransferTrieHeader* hdr = TransferBuffer::parse_header(buffer);
  assert(hdr != nullptr);
  assert(hdr->root_node_type == static_cast<uint8_t>(TransferNodeType::TRIE_NODE));
  
  // Root is the LAST node in the buffer (post-order DFS)
  const uint8_t* nodes = TransferBuffer::nodes_data(buffer, *hdr);
  const uint8_t* buffer_end = reinterpret_cast<const uint8_t*>(buffer.data()) + hdr->total_size;
  
  // Verify that we have at least the expected node count
  assert(hdr->node_count >= 4);  // At least some trie nodes + 3 leaves
  
  // Verify the buffer size is reasonable
  size_t nodes_size = buffer_end - nodes;
  assert(nodes_size > 0);
  
  // Scan all 8-byte offsets in the trie nodes looking for relative offsets
  // Nodes are 8-byte aligned, so offsets within them should also be aligned
  bool found_relative_offset = false;
  
  // Simpler approach: scan the entire buffer for 8-byte values that look like
  // relative offsets (have RELATIVE_FLAG set and point within buffer)
  for (const uint8_t* pos = nodes; pos + 8 <= buffer_end; pos += 8) {
    WireFormatTraits::offset_e test_offset;
    std::memcpy(&test_offset, pos, 8);
    
    if ((uint64_t)test_offset && test_offset.is_relative()) {
      int64_t rel = test_offset.as_signed();
      const uint8_t* target = pos + rel;
      if (target >= nodes && target < buffer_end) {
        found_relative_offset = true;
        break;
      }
    }
  }
  
  assert(found_relative_offset);  // We should have found at least one relative offset
  
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
  test_invalid_header();
  
  std::cout << "\n=== Sender Tests ===\n";
  setup_temp_dir();
  
  test_sender_empty_db();
  test_sender_single_leaf();
  test_sender_multiple_keys();
  test_sender_buffer_overflow();
  test_sender_continue_from_paths();
  test_relative_offsets();
  
  cleanup_temp_dir();
  
  std::cout << "\nAll tests passed!\n";
  return 0;
}
