# Serial Number Arithmetic (RFC 1982)

## Overview

The `serial_number<T>` type implements **Serial Number Arithmetic** as described in [RFC 1982](https://tools.ietf.org/html/rfc1982). This is a wrapping counter with overflow-aware ordering, commonly used for:

- **TCP sequence numbers** - wraparound at 2^32
- **DNS serial numbers** - version tracking with wraparound
- **Transaction IDs** - monotonically increasing IDs with wraparound
- **Circular buffers** - index management with wraparound

## Key Properties

### 1. All Valid Values are Non-Zero
- `0` is reserved to represent "uninitialized" or "invalid"
- Valid range: `[1, 0xFFFFFFFF]` for `serial32` (uint32_t)
- After `0xFFFFFFFF` comes `1`, skipping `0`

### 2. Overflow Wrapping
When addition would exceed the maximum value, it wraps around:

```cpp
serial32 s(0xFFFFFFFE);
s += 3;  // Result: 2 (not 0x100000001)

// Formula: result = ((s + n - 1) % MAX) + 1
```

This ensures we always skip `0` during wraparound.

### 3. Overflow-Aware Ordering

The most important property: **comparison accounts for wraparound**.

#### Standard Ordering (within half range)
```cpp
serial32 s1(100);
serial32 s2(200);
s1 < s2;  // true (normal comparison)
```

#### Wraparound Ordering
```cpp
serial32 s1(0xFFFFFFFF);  // Maximum value
serial32 s2(1);           // Minimum valid value
s1 < s2;  // true! (1 is "after" 0xFFFFFFFF in sequence)
```

#### The Comparison Window

Serial number comparison only works within a **comparison window** of half the range (`2^31` for `uint32_t`):

- If `s2 - s1 < 2^31`: normal ordering (`s1 < s2` if `s1` < `s2`)
- If `s2 - s1 > 2^31`: reversed ordering (wraparound occurred)

**Example:**
```cpp
serial32 s1(100);
serial32 s2(100 + 0x7FFFFFFF);  // Within window
s1 < s2;  // true

serial32 s3(100);
serial32 s4(100 + 0x80000001);  // Outside window (wrapped other way)
s3 > s4;  // true! (reversed because difference > half range)
```

## RFC 1982 Comparison Algorithm

Given two serial numbers `i1` and `i2`, `i1 < i2` if and only if:

```
(i1 < i2 && i2 - i1 < 2^(SERIAL_BITS - 1))
  OR
(i1 > i2 && i1 - i2 > 2^(SERIAL_BITS - 1))
```

For `uint32_t` (32 bits): half range = `2^31 = 0x80000000`

### Visual Representation

```
Circular number space (for uint32_t):

         0 (invalid)
         |
    0xFFFFFFFF ← ← ← wraps ← ← ← 1
         ↑                       ↓
         |                       |
         |    Comparison         |
    0x80000000              0x7FFFFFFF
         |     Window            |
         |   (half range)        |
         ↓                       ↑
         ← ← ← ← ← ← ← ← ← ← ← ←

Numbers can be compared if they're within 2^31 of each other.
```

## Usage Examples

### Transaction ID Management

```cpp
#include "leaves/intern/core/_serial.hpp"
using namespace leaves;

// Initialize transaction counter
serial32 current_txn(1);
serial32 oldest_active_txn(1);

// Allocate new transaction
void start_transaction() {
    ++current_txn;  // Wraps at 0xFFFFFFFF -> 1
    // Use current_txn.value() for storage
}

// Check if block can be recycled
bool can_recycle_block(uint32_t block_txn_id) {
    serial32 block_txn(block_txn_id);
    
    // Block can be recycled if its transaction is before oldest active
    return block_txn < oldest_active_txn;
}

// Update oldest active transaction
void update_oldest_active() {
    serial32 min_txn(0xFFFFFFFF);
    
    // Find minimum active transaction from all cursors
    for (auto& cursor : active_cursors) {
        serial32 cursor_txn(cursor.txn_id());
        if (cursor_txn < min_txn) {
            min_txn = cursor_txn;
        }
    }
    
    oldest_active_txn = min_txn;
}
```

### Sequence Number Management

```cpp
// Network packet sequencing
serial32 expected_seq(1000);
serial32 received_seq(packet.sequence_number);

if (received_seq < expected_seq) {
    // Old/duplicate packet
    discard_packet();
} else if (received_seq == expected_seq) {
    // Expected packet
    process_packet();
    ++expected_seq;
} else {
    // Future packet - buffer for reordering
    buffer_packet(packet);
}
```

### Wraparound Scenario

```cpp
// Start near maximum value
serial32 txn(0xFFFFFFF0);

for (int i = 0; i < 32; ++i) {
    std::cout << std::hex << txn.value() << std::endl;
    ++txn;
}

// Output:
// fffffff0, fffffff1, ..., fffffffe, ffffffff, 
// 1, 2, 3, ..., f, 10

// All consecutive values maintain proper ordering:
serial32 before_wrap(0xFFFFFFFE);
serial32 at_max(0xFFFFFFFF);
serial32 after_wrap(1);

before_wrap < at_max;     // true
at_max < after_wrap;      // true (wraparound!)
before_wrap < after_wrap; // true
```

## Implementation Details

### Addition Algorithm

```cpp
T add_with_wrap(T value, T increment) {
    // Handle zero specially
    if (value == 0) return increment;
    
    // Calculate sum in 64-bit to detect overflow
    uint64_t sum = static_cast<uint64_t>(value) + 
                   static_cast<uint64_t>(increment);
    
    // Wrap to [1, MAX]: ((sum - 1) % MAX) + 1
    return static_cast<T>((sum - 1) % MAX_VALUE) + 1;
}
```

### Comparison Algorithm

```cpp
bool less_than(serial_number a, serial_number b) {
    // Treat zero as minimum
    if (a == 0 || b == 0) return a < b;
    
    if (a < b) {
        // Normal case: check if within comparison window
        return (b - a) < HALF_RANGE;
    } else if (a > b) {
        // Potential wraparound: check if outside window
        return (a - b) > HALF_RANGE;
    }
    return false;  // Equal
}
```

## Mathematical Basis

The comparison works because we're dividing the circular number space into two halves:

1. **Forward half** (0 to 2^31-1 ahead): These are "greater than" current
2. **Backward half** (2^31 to 2^32-1 ahead = 2^32-2^31 to 0 behind): These are "less than" current

When we see a large jump forward (> 2^31), we interpret it as actually being a small jump backward (wraparound).

## Limitations

### Comparison Window

Serial numbers can only be compared if they're within the comparison window (half the range):

```cpp
serial32 s1(100);
serial32 s2(100 + 0x80000001);  // Too far apart!

// Comparison is unreliable - could go either way
// depending on whether wraparound occurred
```

**Solution:** Keep active transaction range < 2^31 values.

### Not Suitable for All Use Cases

Serial numbers are **not** suitable when you need:
- Exact total ordering across the entire range
- Precise difference calculation beyond half range
- Comparison of values that might be > 2^31 apart

**Use regular integers when:**
- Values don't wrap
- You have enough bits (e.g., 64-bit counter for billions of years)
- You need total ordering

## References

- [RFC 1982](https://tools.ietf.org/html/rfc1982) - Serial Number Arithmetic
- [TCP Sequence Numbers](https://www.rfc-editor.org/rfc/rfc793) - Original use case
- [DNS Serial Numbers](https://www.rfc-editor.org/rfc/rfc1034) - Zone version tracking

## Type Aliases

```cpp
using serial32 = serial_number<uint32_t>;  // 32-bit (0 to 2^32-1)
using serial64 = serial_number<uint64_t>;  // 64-bit (0 to 2^64-1)
using tid_serial = serial32;               // Transaction ID type
```

## See Also

- Tests: `tests/test_serial.cpp` - Comprehensive test suite
- Implementation: `include/leaves/intern/_serial.hpp`
