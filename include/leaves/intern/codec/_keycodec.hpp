#ifndef _LEAVES_INTERN_CODEC_KEYCODEC_HPP
#define _LEAVES_INTERN_CODEC_KEYCODEC_HPP

#include <cassert>
#include <cstdint>
#include <cstring>

namespace leaves {

struct _KeyCodec {
    // --- byte-swap helpers (self-contained, no _port.hpp dependency) ---

#ifdef _MSC_VER
    static uint16_t _bswap16(uint16_t v) { return _byteswap_ushort(v); }
    static uint32_t _bswap32(uint32_t v) { return _byteswap_ulong(v); }
    static uint64_t _bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
    static uint16_t _bswap16(uint16_t v) { return __builtin_bswap16(v); }
    static uint32_t _bswap32(uint32_t v) { return __builtin_bswap32(v); }
    static uint64_t _bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

    // --- unsigned integers: big-endian encoding ---

    static void encode_uint8(uint8_t val, char* out) {
        out[0] = static_cast<char>(val);
    }

    static uint8_t decode_uint8(const char* in) {
        return static_cast<uint8_t>(in[0]);
    }

    static void encode_uint16(uint16_t val, char* out) {
        uint16_t be = _bswap16(val);
        std::memcpy(out, &be, sizeof(be));
    }

    static uint16_t decode_uint16(const char* in) {
        uint16_t be;
        std::memcpy(&be, in, sizeof(be));
        return _bswap16(be);
    }

    static void encode_uint32(uint32_t val, char* out) {
        uint32_t be = _bswap32(val);
        std::memcpy(out, &be, sizeof(be));
    }

    static uint32_t decode_uint32(const char* in) {
        uint32_t be;
        std::memcpy(&be, in, sizeof(be));
        return _bswap32(be);
    }

    static void encode_uint64(uint64_t val, char* out) {
        uint64_t be = _bswap64(val);
        std::memcpy(out, &be, sizeof(be));
    }

    static uint64_t decode_uint64(const char* in) {
        uint64_t be;
        std::memcpy(&be, in, sizeof(be));
        return _bswap64(be);
    }

    // --- signed integers: XOR sign bit + big-endian ---
    // XOR with the sign bit flips the ordering so that negative values
    // sort before positive values under unsigned (memcmp) comparison.

    static void encode_int8(int8_t val, char* out) {
        uint8_t u;
        std::memcpy(&u, &val, 1);
        u ^= 0x80u;
        out[0] = static_cast<char>(u);
    }

    static int8_t decode_int8(const char* in) {
        uint8_t u = static_cast<uint8_t>(in[0]);
        u ^= 0x80u;
        int8_t val;
        std::memcpy(&val, &u, 1);
        return val;
    }

    static void encode_int16(int16_t val, char* out) {
        uint16_t u;
        std::memcpy(&u, &val, sizeof(u));
        u ^= 0x8000u;
        u = _bswap16(u);
        std::memcpy(out, &u, sizeof(u));
    }

    static int16_t decode_int16(const char* in) {
        uint16_t u;
        std::memcpy(&u, in, sizeof(u));
        u = _bswap16(u);
        u ^= 0x8000u;
        int16_t val;
        std::memcpy(&val, &u, sizeof(val));
        return val;
    }

    static void encode_int32(int32_t val, char* out) {
        uint32_t u;
        std::memcpy(&u, &val, sizeof(u));
        u ^= 0x80000000u;
        u = _bswap32(u);
        std::memcpy(out, &u, sizeof(u));
    }

    static int32_t decode_int32(const char* in) {
        uint32_t u;
        std::memcpy(&u, in, sizeof(u));
        u = _bswap32(u);
        u ^= 0x80000000u;
        int32_t val;
        std::memcpy(&val, &u, sizeof(val));
        return val;
    }

    static void encode_int64(int64_t val, char* out) {
        uint64_t u;
        std::memcpy(&u, &val, sizeof(u));
        u ^= 0x8000000000000000ull;
        u = _bswap64(u);
        std::memcpy(out, &u, sizeof(u));
    }

    static int64_t decode_int64(const char* in) {
        uint64_t u;
        std::memcpy(&u, in, sizeof(u));
        u = _bswap64(u);
        u ^= 0x8000000000000000ull;
        int64_t val;
        std::memcpy(&val, &u, sizeof(val));
        return val;
    }

    // --- IEEE 754 floats: order-preserving encoding ---
    // Positive floats: XOR sign bit → big-endian (already ordered)
    // Negative floats: XOR all bits → big-endian (reverses order)

    static void encode_float(float val, char* out) {
        uint32_t u;
        std::memcpy(&u, &val, sizeof(u));
        if (u & 0x80000000u) {
            u = ~u;  // negative: flip all bits
        } else {
            u ^= 0x80000000u;  // positive: flip sign bit
        }
        u = _bswap32(u);
        std::memcpy(out, &u, sizeof(u));
    }

    static float decode_float(const char* in) {
        uint32_t u;
        std::memcpy(&u, in, sizeof(u));
        u = _bswap32(u);
        if (u & 0x80000000u) {
            u ^= 0x80000000u;  // was positive: flip sign bit back
        } else {
            u = ~u;  // was negative: flip all bits back
        }
        float val;
        std::memcpy(&val, &u, sizeof(val));
        return val;
    }

    static void encode_double(double val, char* out) {
        uint64_t u;
        std::memcpy(&u, &val, sizeof(u));
        if (u & 0x8000000000000000ull) {
            u = ~u;
        } else {
            u ^= 0x8000000000000000ull;
        }
        u = _bswap64(u);
        std::memcpy(out, &u, sizeof(u));
    }

    static double decode_double(const char* in) {
        uint64_t u;
        std::memcpy(&u, in, sizeof(u));
        u = _bswap64(u);
        if (u & 0x8000000000000000ull) {
            u ^= 0x8000000000000000ull;
        } else {
            u = ~u;
        }
        double val;
        std::memcpy(&val, &u, sizeof(val));
        return val;
    }

    // --- encoded sizes ---

    static constexpr size_t encoded_size_uint8  = 1;
    static constexpr size_t encoded_size_uint16 = 2;
    static constexpr size_t encoded_size_uint32 = 4;
    static constexpr size_t encoded_size_uint64 = 8;
    static constexpr size_t encoded_size_int8   = 1;
    static constexpr size_t encoded_size_int16  = 2;
    static constexpr size_t encoded_size_int32  = 4;
    static constexpr size_t encoded_size_int64  = 8;
    static constexpr size_t encoded_size_float  = 4;
    static constexpr size_t encoded_size_double = 8;
};

}  // namespace leaves

#endif  // _LEAVES_INTERN_CODEC_KEYCODEC_HPP
