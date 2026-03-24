#ifndef _LEAVES__REPLICATION_PROTOCOL_HPP
#define _LEAVES__REPLICATION_PROTOCOL_HPP

#include <cstdint>

#include "../core/_port.hpp"

namespace leaves {

// =============================================================================
// Replication Protocol — shared wire-format types
// =============================================================================

// Message types for replication protocol
enum class ReplicationMsgType : uint8_t {
  TRIE_DATA = 0x01,  // TransferTrie buffer (sender → receiver)
  SUBTRIE_ACK =
      0x02,  // ACK subtries at paths - sender can skip (receiver → sender)
  COMPLETE = 0x03,           // Replication complete (either side)
  ERROR = 0x04,              // Error occurred (either side)
  FRACTION_COMPLETE = 0x05,  // Fraction merged, restart from root
                             // (receiver → sender)
  BIG_VALUE_DATA = 0x06,     // Big value data transfer (sender → receiver)
  BIG_VALUE_ACK =
      0x07,  // Big value received acknowledgement (receiver → sender)
  BIG_VALUE_START = 0x08,  // Big value transfer start with total size
                           // (sender → receiver)
};

constexpr uint32_t REPLICATION_MSG_MAGIC = 0x4C565250;  // "LVRP"

// Message envelope wrapping all replication messages
constexpr uint8_t REPLICATION_PROTOCOL_VERSION = 1;

#pragma pack(push, 1)
struct ReplicationMsgHeader {
  _little_uint32_t magic;
  uint8_t msg_type;  // ReplicationMsgType
  _little_uint64_t session_id;
  _little_uint32_t payload_size;
  uint8_t version;      // Protocol version (REPLICATION_PROTOCOL_VERSION)
  uint8_t reserved[6];  // Padding to 24 bytes for 8-byte payload alignment
  // Followed by: payload bytes

  bool is_valid() const {
    return magic == REPLICATION_MSG_MAGIC &&
           version == REPLICATION_PROTOCOL_VERSION;
  }
};
#pragma pack(pop)

static_assert(sizeof(ReplicationMsgHeader) == 24,
              "ReplicationMsgHeader must be 24 bytes");

// Big value data header - sent in BIG_VALUE_DATA messages
// Multiple big values can be batched in one message
#pragma pack(push, 1)
struct BigValueDataHeader {
  _little_uint64_t wire_chunk_offset;          // Original offset (lookup key)
  _little_uint32_t value_size;                  // Size of value data
  // Followed by: value_size bytes of data
};
#pragma pack(pop)

static_assert(sizeof(BigValueDataHeader) == 12,
              "BigValueDataHeader must be 12 bytes");

// Big value start header - sent in BIG_VALUE_START messages
// Announces total count and aligned size so receiver can pre-allocate
#pragma pack(push, 1)
struct BigValueStartHeader {
  _little_uint32_t count;                       // Number of big values to follow
  _little_uint64_t total_aligned_size;            // Total size with MAX_PAGE_SIZE alignment
};
#pragma pack(pop)

static_assert(sizeof(BigValueStartHeader) == 12,
              "BigValueStartHeader must be 12 bytes");

// Error codes for ERROR messages
enum class ReplicationError : uint8_t {
  NONE = 0x00,
  SESSION_MISMATCH = 0x01,
  INVALID_MESSAGE = 0x02,
  INVALID_STATE = 0x03,
  INTERNAL_ERROR = 0x04,
  PAYLOAD_TOO_LARGE = 0x05,
  RESOURCE_LIMIT = 0x06,
  STORAGE_FULL = 0x07
};

}  // namespace leaves

#endif  // _LEAVES__REPLICATION_PROTOCOL_HPP
