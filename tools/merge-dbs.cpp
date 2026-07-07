#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <memory>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <map>

#include "../include/leaves/mmap.hpp"
#include "../include/leaves/intern/util/_merger.hpp"
#include "../include/leaves/intern/db/_check.hpp"

using namespace leaves;

// ── Simple YAML parser for the _Dumper output format ─────────────────────
//
// The _Dumper YAML format is a multi-document stream separated by "---".
// Each document is either a trie or leaf node.  We track the trie path
// (compressed prefixes + branch positions) to reconstruct full keys.
//
// Key encoding in the dump: individual bytes are shown as [byte] or [0xNN].
// Empty keys are "".  We decode these back to raw key strings.

struct YamlNode {
  std::string type;    // "trie" or "leaf"
  std::string key;     // for leaf: the key suffix; for trie: the compressed prefix
  std::string value;   // for leaf: the value
  bool is_big = false;
  std::vector<std::string> children; // child IDs for trie nodes
  std::vector<std::string> branches; // branch keys for trie nodes
  std::string id;
};

// Decode a bracket-encoded string from a dump yaml line.
// Input:  ""          -> empty string
// Input:  "[h][i]"    -> "hi"
// Input:  "[0xc3][0xb6]" -> "\xc3\xb6"
static std::string decode_bracketed(const std::string& raw) {
  if (raw.empty()) return "";
  std::string result;
  size_t i = 0;
  while (i < raw.size()) {
    if (raw[i] == '[') {
      size_t end = raw.find(']', i);
      if (end == std::string::npos) break;
      std::string inner = raw.substr(i + 1, end - i - 1);
      i = end + 1;
      if (inner.empty()) {
        // NONE branch - represented as empty string, no byte added
        continue;
      }
      if (inner.size() > 2 && inner[0] == '0' && inner[1] == 'x') {
        char* endp = nullptr;
        long val = strtol(inner.c_str(), &endp, 16);
        if (endp && *endp == '\0' && val >= 0 && val <= 255) {
          result.push_back((char)val);
        }
      } else if (inner.size() == 1) {
        result.push_back(inner[0]);
      } else {
        result += inner;
      }
    } else {
      result.push_back(raw[i]);
      i++;
    }
  }
  return result;
}

// Trim whitespace from both ends
static std::string trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Extract a quoted value from a YAML line
static std::string extract_quoted(const std::string& line) {
  size_t eq = line.find(':');
  if (eq == std::string::npos) return "";
  std::string val = trim(line.substr(eq + 1));
  if (val.size() >= 2 && val[0] == '"' && val[val.size()-1] == '"') {
    return val.substr(1, val.size() - 2);
  }
  return val;
}

// Extract an unquoted scalar value from a YAML line
static std::string extract_unquoted(const std::string& line) {
  size_t eq = line.find(':');
  if (eq == std::string::npos) return "";
  return trim(line.substr(eq + 1));
}

// Parse a YAML dump file into a vector of YamlNode
static std::vector<YamlNode> parse_yaml_dump(const std::string& path) {
  std::vector<YamlNode> nodes;
  std::ifstream file(path);
  if (!file.is_open()) return nodes;

  YamlNode current;
  bool in_node = false;
  int child_index = -1;
  std::string line;

  auto flush = [&]() {
    if (in_node) {
      nodes.push_back(std::move(current));
      current = YamlNode();
      child_index = -1;
      in_node = false;
    }
  };

  while (std::getline(file, line)) {
    if (line.rfind("---", 0) == 0) { flush(); continue; }

    std::string tline = trim(line);

    if (tline.rfind("type:", 0) == 0) {
      flush();
      current.type = trim(tline.substr(5));
      in_node = true;
      continue;
    }
    if (!in_node) continue;

    if (tline.rfind("id:", 0) == 0) {
      current.id = extract_unquoted(tline);
      continue;
    }
    if (tline.rfind("key:", 0) == 0) {
      current.key = decode_bracketed(extract_quoted(line));
      continue;
    }
    if (tline.rfind("valuesize:", 0) == 0) {
      // skip, we capture value by "value:" field
      continue;
    }
    // Big value
    if (tline.rfind("value:", 0) == 0 && tline.find("chunk_offset=") != std::string::npos) {
      current.is_big = true;
      continue;
    }
    if (tline.rfind("value:", 0) == 0) {
      current.value = extract_quoted(line);
      continue;
    }
    if (tline.rfind("branches:", 0) == 0) {
      // Parse branches line: branches: "[] [v]"
      std::string raw = extract_quoted(line);
      // Split by "][" or "[" or "]"
      size_t pos = 0;
      while (pos < raw.size()) {
        if (raw[pos] == '[') {
          size_t end = raw.find(']', pos);
          if (end == std::string::npos) break;
          std::string inner = raw.substr(pos + 1, end - pos - 1);
          current.branches.push_back(inner.empty() ? "" : inner);
          pos = end + 1;
        } else {
          pos++;
        }
      }
      continue;
    }
    // Child IDs under "children:"
    if (tline.rfind("children:", 0) == 0) {
      continue; // the list items follow in next lines
    }
    if (tline.rfind("- ", 0) == 0) {
      current.children.push_back(trim(tline.substr(2)));
      continue;
    }
  }
  flush();

  return nodes;
}

// Recursively walk the trie, building full keys and inserting leaves
// into the database.
static void walk_and_insert(
    const std::vector<YamlNode>& nodes,
    std::map<std::string, const YamlNode*>& by_id,
    const std::string& node_id,
    const std::string& prefix,
    leaves::TDB<leaves::MapStorage_<>>& db) {

  auto it = by_id.find(node_id);
  if (it == by_id.end()) return;
  const YamlNode& node = *it->second;

  if (node.type == "leaf") {
    std::string full_key = prefix + node.key;
    if (!node.is_big) {
      auto cursor = db.cursor();
      cursor.find(leaves::Slice(full_key));
      cursor.value(leaves::Slice(node.value));
      cursor.commit();
    }
    return;
  }

  // Trie node: append compressed prefix and recurse into children
  std::string new_prefix = prefix + node.key;
  for (size_t i = 0; i < node.children.size() && i < node.branches.size(); i++) {
    const std::string& child_id = node.children[i];
    if (child_id.empty() || child_id == "0") continue;

    // The child's stored key (trie compressed prefix or leaf key suffix)
    // ALREADY starts with the branch byte, so we just pass new_prefix unchanged.
    walk_and_insert(nodes, by_id, child_id, new_prefix, db);
  }
}

// Load all leaf key-value pairs from a _Dumper YAML file into a database.
static int load_yaml_into_db(const std::string& path,
                             MapStorage::storage_ptr storage) {
  std::vector<YamlNode> nodes = parse_yaml_dump(path);
  if (nodes.empty()) {
    std::cerr << "Error: No nodes found in " << path << std::endl;
    return -1;
  }

  // Build lookup by ID
  std::map<std::string, const YamlNode*> by_id;
  std::string root_id;

  for (const auto& n : nodes) {
    by_id[n.id] = &n;
  }

  // Find root (first trie with compressed size 0 / empty key, or first node)
  for (const auto& n : nodes) {
    if (n.type == "trie") {
      root_id = n.id;
      break;
    }
  }
  if (root_id.empty() && !nodes.empty()) {
    root_id = nodes[0].id;
  }

  int count_before = 0;
  {
    auto db = storage->open("test");
    auto cursor = db.cursor();
    cursor.first();
    while (cursor.is_valid()) {
      count_before++;
      cursor.next();
    }
  }

  // Reopen db for writing
  auto db = storage->open("test");
  walk_and_insert(nodes, by_id, root_id, "", db);

  // Count inserted
  int count_after = 0;
  {
    auto db2 = storage->open("test");
    auto cursor2 = db2.cursor();
    cursor2.first();
    while (cursor2.is_valid()) {
      count_after++;
      cursor2.next();
    }
  }

  int inserted = count_after - count_before;
  std::cout << "Loaded " << inserted << " key-value pairs from " << path
            << std::endl;
  return inserted;
}

// ── Merge policy: always overwrite, always add ─────────────────────────────
struct OverwritePolicy {
  bool may_overwrite(const std::string&, const Slice&, const Slice&, bool, bool) { return true; }
  bool may_add_leaf(const std::string&, const Slice&, bool) { return true; }
  bool may_add_trie(const std::string&) { return true; }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {
    _BigValue* bvalue = (_BigValue*)leaf->vdata();
    dst_cursor.get_bigmemory().free(bvalue);
  }

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor,
                                  DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData { char data; };
    auto src_data = src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;
    _BigValue* dst_bvalue = &_big_value_storage;
    dst_cursor.get_bigmemory().alloc(value_size, dst_bvalue);
    offset_t dst_offset(dst_bvalue->chunk_offset);
    auto dst_data = dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    memcpy((char*)dst_data, (char*)src_data, value_size);
    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr&, DBType*) {}
};

// ── Merge executor ────────────────────────────────────────────────────────
template <typename DBDst, typename DBSrc, typename MergePolicy>
void exec_merger(DBDst& dst_db, DBSrc& src_db, MergePolicy& handler) {
  using DstCursorTraits = typename DBDst::CursorTraits;
  using SrcCursorTraits = typename DBSrc::CursorTraits;
  using DstCursor = _TransactionalCursor<DstCursorTraits>;
  using SrcCursor = _Cursor<SrcCursorTraits>;

  auto src_root = &src_db.txn()->root;
  auto dst_root = &dst_db.txn()->root;
  uint64_t cursor_id = dst_db.new_cursor_id();

  DstCursor dst_cursor(&dst_db, dst_root);
  SrcCursor src_cursor(&src_db, src_root);
  src_cursor.clear();
  dst_cursor.start_transaction();

  _Merger<DstCursor, SrcCursor, MergePolicy> merger(dst_cursor, src_cursor, handler);
  merger.exec();
  dst_cursor.commit(cursor_id);
}

// ── Dump the result ──────────────────────────────────────────────────────
template <typename DB>
static void dump_result(DB& db, const std::string& path) {
  std::ofstream out(path);
  if (!out.is_open()) {
    std::cerr << "Error: Cannot open output file: " << path << std::endl;
    return;
  }
  _Dumper(db, &db.txn()->root, false).dump(out);
  out.close();
  std::cout << "Merged result written to: " << path << std::endl;
}

// ── Main ──────────────────────────────────────────────────────────────────
void print_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " <dst.yaml> <src.yaml> [result.yaml]\n";
  std::cerr << "\n";
  std::cerr << "Reads two _Dumper-format YAML files into temporary databases,\n";
  std::cerr << "merges the source into the destination, and writes the result\n";
  std::cerr << "as a _Dumper YAML to result.yaml (default: result.yaml).\n";
}

int main(int argc, char* argv[]) {
  if (argc < 3 || argc > 4) {
    print_usage(argv[0]);
    return 1;
  }

  std::string dst_path = argv[1];
  std::string src_path = argv[2];
  std::string result_path = (argc >= 4) ? argv[3] : "result.yaml";

  std::string tmp_dst = ".merge_dst_tmp.lvs";
  std::string tmp_src = ".merge_src_tmp.lvs";
  std::remove(tmp_dst.c_str());
  std::remove(tmp_src.c_str());

  try {
    auto dst_storage = MapStorage::create(tmp_dst.c_str());
    auto src_storage = MapStorage::create(tmp_src.c_str());

    std::cout << "Loading destination YAML: " << dst_path << std::endl;
    int dst_count = load_yaml_into_db(dst_path, dst_storage);
    if (dst_count < 0) return 1;

    std::cout << "Loading source YAML: " << src_path << std::endl;
    int src_count = load_yaml_into_db(src_path, src_storage);
    if (src_count < 0) return 1;

    auto dst_db = dst_storage->open("test");
    auto src_db = src_storage->open("test");
    auto dst_internal = dst_db._internal();
    auto src_internal = src_db._internal();

    std::cout << "Merging source into destination..." << std::endl;
    OverwritePolicy handler;
    exec_merger(*dst_internal, *src_internal, handler);

    dump_result(*dst_internal, result_path);

    std::cout << "Merge complete: " << dst_count << " dst + " << src_count
              << " src keys -> result written to " << result_path << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    std::remove(tmp_dst.c_str());
    std::remove(tmp_src.c_str());
    return 1;
  }

  std::remove(tmp_dst.c_str());
  std::remove(tmp_src.c_str());
  return 0;
}