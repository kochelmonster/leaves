#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "leaves/intern/storage/_memstore.hpp"
#include "leaves/intern/util/_merger.hpp"
#include "leaves/intern/util/_set_operations.hpp"

namespace {

using MemoryCursor = leaves::_MemoryStorage::DB::Cursor;
using Clock = std::chrono::steady_clock;

struct Flags {
  size_t num = 100000;
  size_t key_size = 16;
  size_t rounds = 3;
  size_t overlap = 50;
  uint64_t seed = 301;
  bool random = true;
  bool iterative = false;
};

struct Inputs {
  std::vector<std::string> left;
  std::vector<std::string> right;
  std::vector<std::string> union_keys;
  std::vector<std::string> intersection_keys;
};

struct Timing {
  std::vector<double> samples;

  template <typename Operation>
  void measure(Operation&& operation) {
    const auto start = Clock::now();
    operation();
    samples.push_back(std::chrono::duration<double, std::milli>(
                          Clock::now() - start).count());
  }

  void print(const char* backend, const char* operation, size_t count) const {
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    const double median = sorted.size() % 2 ? sorted[middle]
        : (sorted[middle - 1] + sorted[middle]) / 2;
    std::cout << std::left << std::setw(20) << backend << std::setw(14) << operation
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(14) << median << std::setw(14) << sorted.front()
              << std::setw(16) << count << '\n';
  }
};

struct MemoryMergeCursor : MemoryCursor {
  using MemoryCursor::MemoryCursor;
  using BigMemory = void;
  void get_bigmemory() {}
};

struct EmptyValueMergePolicy : leaves::StandardMergePolicy {
  template <typename Leaf, typename Cursor>
  void free_big(Leaf&, Cursor&) {
    throw std::logic_error("empty-valued benchmark encountered a big value");
  }

  template <typename Leaf, typename Source, typename Destination>
  leaves::MigratedValue migrate_big_value(Leaf&, Source&, Destination&) {
    throw std::logic_error("empty-valued benchmark encountered a big value");
  }
};

uint64_t parse_number(std::string_view text) {
  uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid unsigned integer: " + std::string(text));
  return result;
}

Flags parse_flags(int argc, char** argv) {
  Flags flags;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    const auto equal = argument.find('=');
    if (equal == std::string_view::npos)
      throw std::invalid_argument("expected --name=value: " + std::string(argument));
    const auto name = argument.substr(0, equal);
    const auto text = argument.substr(equal + 1);
    if (name == "--layout") {
      if (text != "random" && text != "sequential")
        throw std::invalid_argument("layout must be random or sequential");
      flags.random = text == "random";
      continue;
    }
    const uint64_t value = parse_number(text);
    if (name == "--seed") {
      flags.seed = value;
      continue;
    }
    if (value > SIZE_MAX) throw std::invalid_argument("value exceeds size_t");
    if (name == "--num") flags.num = static_cast<size_t>(value);
    else if (name == "--key_size") flags.key_size = static_cast<size_t>(value);
    else if (name == "--rounds") flags.rounds = static_cast<size_t>(value);
    else if (name == "--overlap") flags.overlap = static_cast<size_t>(value);
    else if (name == "--iterative" && value <= 1) flags.iterative = value != 0;
    else throw std::invalid_argument("unknown flag: " + std::string(name));
  }
  if (!flags.rounds || flags.overlap > 100 || flags.num > SIZE_MAX / 2 ||
      flags.key_size < 8 || flags.key_size > leaves::_MemoryTraits::MAX_KEY_SIZE)
    throw std::invalid_argument("require rounds > 0, overlap in [0,100], "
                                "num <= SIZE_MAX/2 and key_size in [8,MAX_KEY_SIZE]");
  return flags;
}

uint64_t permute(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

std::string make_key(uint64_t id, const Flags& flags) {
  const uint64_t encoded = flags.random ? permute(id + flags.seed) : id;
  std::string result(flags.key_size, '\0');
  for (size_t index = 0; index < result.size(); ++index) {
    const uint64_t word = index < 8 ? encoded : permute(encoded + index / 8);
    result[index] = static_cast<char>(word >> ((7 - index % 8) * 8));
  }
  return result;
}

Inputs make_inputs(const Flags& flags) {
  Inputs inputs;
  const size_t shared = flags.num / 100 * flags.overlap +
                        flags.num % 100 * flags.overlap / 100;
  const size_t right_start = flags.num - shared;
  const size_t total = flags.num + right_start;
  inputs.left.reserve(flags.num);
  inputs.right.reserve(flags.num);
  inputs.union_keys.reserve(total);
  inputs.intersection_keys.reserve(shared);
  for (size_t id = 0; id < total; ++id) {
    auto key = make_key(id, flags);
    if (id < flags.num) inputs.left.push_back(key);
    if (id >= right_start) inputs.right.push_back(key);
    if (id >= right_start && id < flags.num)
      inputs.intersection_keys.push_back(key);
    inputs.union_keys.push_back(std::move(key));
  }
  std::sort(inputs.union_keys.begin(), inputs.union_keys.end());
  std::sort(inputs.intersection_keys.begin(), inputs.intersection_keys.end());
  std::mt19937_64 random(flags.seed);
  std::shuffle(inputs.left.begin(), inputs.left.end(), random);
  std::shuffle(inputs.right.begin(), inputs.right.end(), random);
  return inputs;
}

void fill_memory(leaves::_MemoryStorage& storage,
                 const std::vector<std::string>& keys) {
  auto cursor = storage.create_cursor();
  for (const auto& key : keys) {
    cursor->find(leaves::Slice(key));
    cursor->value(leaves::Slice(""));
  }
}

void verify_memory(leaves::_MemoryStorage& storage,
                   const std::vector<std::string>& expected) {
  auto cursor = storage.create_cursor();
  size_t index = 0;
  for (cursor->first(); cursor->is_valid(); cursor->next(), ++index) {
    if (index == expected.size() || cursor->key() != leaves::Slice(expected[index]) ||
        !cursor->value().empty())
      throw std::runtime_error("Leaves result verification failed");
  }
  if (index != expected.size())
    throw std::runtime_error("Leaves result cardinality mismatch");
}

void bench_memory(const Flags& flags, const Inputs& inputs) {
  leaves::_MemoryStorage left_storage, right_storage;
  fill_memory(left_storage, inputs.left);
  fill_memory(right_storage, inputs.right);
  auto left = left_storage.create_cursor();
  auto right = right_storage.create_cursor();
  Timing union_time, intersection_time, iterative_time;
  for (size_t round = 0; round < flags.rounds; ++round) {
    {
      leaves::_MemoryStorage result;
      fill_memory(result, inputs.left);
      MemoryMergeCursor destination(&result.db(), &result.db()._root);
      right->clear();
      EmptyValueMergePolicy policy;
      leaves::_Merger<MemoryMergeCursor, MemoryCursor, EmptyValueMergePolicy> merger(
          destination, *right, policy);
      union_time.measure([&] { merger.exec(); });
      verify_memory(result, inputs.union_keys);
    }
    {
      leaves::_MemoryStorage result;
      auto destination = result.create_cursor();
      leaves::_SetOperationObserver observer;
      leaves::_Intersector<MemoryCursor, MemoryCursor, MemoryCursor,
                           leaves::_SetOperationObserver> intersection(
          *destination, *left, *right, 0, observer);
      intersection_time.measure([&] { intersection.exec(); });
      verify_memory(result, inputs.intersection_keys);
    }
    if (flags.iterative) {
      leaves::_MemoryStorage result;
      fill_memory(result, inputs.left);
      auto destination = result.create_cursor();
      iterative_time.measure([&] {
        for (right->first(); right->is_valid(); right->next()) {
          destination->find(right->key());
          destination->value(leaves::Slice(""));
        }
      });
      verify_memory(result, inputs.union_keys);
    }
  }
  auto sorted_left = inputs.left;
  auto sorted_right = inputs.right;
  std::sort(sorted_left.begin(), sorted_left.end());
  std::sort(sorted_right.begin(), sorted_right.end());
  verify_memory(left_storage, sorted_left);
  verify_memory(right_storage, sorted_right);
  union_time.print("Leaves _MemoryDB", "union", inputs.union_keys.size());
  intersection_time.print("Leaves _MemoryDB", "intersection", inputs.intersection_keys.size());
  if (flags.iterative)
    iterative_time.print("Leaves _MemoryDB", "union_iter", inputs.union_keys.size());
}

void bench_ordered(const Flags& flags, const Inputs& inputs) {
  const std::set<std::string> left(inputs.left.begin(), inputs.left.end());
  const std::set<std::string> right(inputs.right.begin(), inputs.right.end());
  Timing union_time, intersection_time;
  for (size_t round = 0; round < flags.rounds; ++round) {
    {
      auto result = left;
      union_time.measure([&] { result.insert(right.begin(), right.end()); });
      if (!std::equal(result.begin(), result.end(),
                      inputs.union_keys.begin(), inputs.union_keys.end()))
        throw std::runtime_error("std::set union verification failed");
    }
    {
      std::set<std::string> result;
      intersection_time.measure([&] {
        std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                              std::inserter(result, result.end()));
      });
      if (!std::equal(result.begin(), result.end(),
                      inputs.intersection_keys.begin(), inputs.intersection_keys.end()))
        throw std::runtime_error("std::set intersection verification failed");
    }
  }
  union_time.print("std::set", "union", inputs.union_keys.size());
  intersection_time.print("std::set", "intersection", inputs.intersection_keys.size());
}

void verify_unordered(const std::unordered_set<std::string>& result,
                      const std::vector<std::string>& expected) {
  if (result.size() != expected.size())
    throw std::runtime_error("std::unordered_set cardinality mismatch");
  for (const auto& key : expected) {
    if (result.find(key) == result.end())
      throw std::runtime_error("std::unordered_set verification failed");
  }
}

void bench_unordered(const Flags& flags, const Inputs& inputs) {
  const std::unordered_set<std::string> left(inputs.left.begin(), inputs.left.end());
  const std::unordered_set<std::string> right(inputs.right.begin(), inputs.right.end());
  Timing union_time, intersection_time;
  for (size_t round = 0; round < flags.rounds; ++round) {
    {
      auto result = left;
      union_time.measure([&] { result.insert(right.begin(), right.end()); });
      verify_unordered(result, inputs.union_keys);
    }
    {
      std::unordered_set<std::string> result;
      intersection_time.measure([&] {
        for (const auto& key : left) {
          if (right.find(key) != right.end()) result.insert(key);
        }
      });
      verify_unordered(result, inputs.intersection_keys);
    }
  }
  union_time.print("std::unordered_set", "union", inputs.union_keys.size());
  intersection_time.print("std::unordered_set", "intersection", inputs.intersection_keys.size());
}

}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "bench_set_operations [--num=100000] [--key_size=16] [--rounds=3]\n"
                   "  [--overlap=50] [--layout=random|sequential] [--seed=301] [--iterative=0|1]\n";
      return 0;
    }
    const auto flags = parse_flags(argc, argv);
    const auto inputs = make_inputs(flags);
    std::cout << "num/side=" << flags.num << " key_size=" << flags.key_size
              << " overlap=" << flags.overlap << "% layout="
              << (flags.random ? "random" : "sequential") << " seed=" << flags.seed
              << " rounds=" << flags.rounds << '\n'
              << "Operation only: setup, validation and destruction excluded; "
                 "result growth included. Exact-key intersection.\n"
              << std::left << std::setw(20) << "backend" << std::setw(14) << "operation"
              << std::right << std::setw(14) << "median ms" << std::setw(14) << "best ms"
              << std::setw(16) << "result keys" << '\n';
    bench_memory(flags, inputs);
    bench_ordered(flags, inputs);
    bench_unordered(flags, inputs);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "bench_set_operations: " << error.what() << '\n';
    return 1;
  }
}