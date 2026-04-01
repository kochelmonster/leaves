#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE KeyCodecTest

#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "leaves/intern/codec/_keycodec.hpp"
#include "leaves/intern/codec/_keybuilder.hpp"

using namespace leaves;

// Helper: compare two encoded buffers via memcmp
static int cmp(const char* a, const char* b, size_t len) {
    return std::memcmp(a, b, len);
}

// ============================================================
// Unsigned integer roundtrip
// ============================================================

BOOST_AUTO_TEST_CASE(roundtrip_uint8) {
    for (uint8_t v : {uint8_t(0), uint8_t(1), uint8_t(127), uint8_t(128), uint8_t(255)}) {
        char buf[1];
        _KeyCodec::encode_uint8(v, buf);
        BOOST_TEST(_KeyCodec::decode_uint8(buf) == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_uint16) {
    for (uint16_t v : {uint16_t(0), uint16_t(1), uint16_t(255), uint16_t(256), uint16_t(65535)}) {
        char buf[2];
        _KeyCodec::encode_uint16(v, buf);
        BOOST_TEST(_KeyCodec::decode_uint16(buf) == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_uint32) {
    for (uint32_t v : {0u, 1u, 255u, 65536u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        char buf[4];
        _KeyCodec::encode_uint32(v, buf);
        BOOST_TEST(_KeyCodec::decode_uint32(buf) == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_uint64) {
    for (uint64_t v : {0ull, 1ull, 0xFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull}) {
        char buf[8];
        _KeyCodec::encode_uint64(v, buf);
        BOOST_TEST(_KeyCodec::decode_uint64(buf) == v);
    }
}

// ============================================================
// Signed integer roundtrip
// ============================================================

BOOST_AUTO_TEST_CASE(roundtrip_int8) {
    for (int8_t v : {int8_t(-128), int8_t(-1), int8_t(0), int8_t(1), int8_t(127)}) {
        char buf[1];
        _KeyCodec::encode_int8(v, buf);
        BOOST_TEST(_KeyCodec::decode_int8(buf) == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_int16) {
    for (int16_t v : {int16_t(-32768), int16_t(-1), int16_t(0), int16_t(1), int16_t(32767)}) {
        char buf[2];
        _KeyCodec::encode_int16(v, buf);
        BOOST_TEST(_KeyCodec::decode_int16(buf) == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_int32) {
    for (int32_t v : {INT32_MIN, -1, 0, 1, INT32_MAX}) {
        char buf[4];
        _KeyCodec::encode_int32(v, buf);
        BOOST_TEST(_KeyCodec::decode_int32(buf) == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_int64) {
    for (int64_t v : {INT64_MIN, int64_t(-1), int64_t(0), int64_t(1), INT64_MAX}) {
        char buf[8];
        _KeyCodec::encode_int64(v, buf);
        BOOST_TEST(_KeyCodec::decode_int64(buf) == v);
    }
}

// ============================================================
// Float/double roundtrip
// ============================================================

BOOST_AUTO_TEST_CASE(roundtrip_float) {
    for (float v : {-INFINITY, -1.0e30f, -1.0f, -0.0f, 0.0f, 1.0f, 1.0e30f, INFINITY}) {
        char buf[4];
        _KeyCodec::encode_float(v, buf);
        float decoded = _KeyCodec::decode_float(buf);
        // -0.0 and +0.0 compare equal
        BOOST_TEST(decoded == v);
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_double) {
    for (double v : {(double)-INFINITY, -1.0e300, -1.0, -0.0, 0.0, 1.0, 1.0e300, (double)INFINITY}) {
        char buf[8];
        _KeyCodec::encode_double(v, buf);
        double decoded = _KeyCodec::decode_double(buf);
        BOOST_TEST(decoded == v);
    }
}

// ============================================================
// Sort order tests: memcmp on encoded keys matches natural order
// ============================================================

BOOST_AUTO_TEST_CASE(sort_order_uint32) {
    std::vector<uint32_t> vals = {0, 1, 100, 1000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF};
    std::vector<std::string> encoded;
    for (auto v : vals) {
        char buf[4];
        _KeyCodec::encode_uint32(v, buf);
        encoded.emplace_back(buf, 4);
    }
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        BOOST_TEST(cmp(encoded[i].data(), encoded[i + 1].data(), 4) < 0);
    }
}

BOOST_AUTO_TEST_CASE(sort_order_uint64) {
    std::vector<uint64_t> vals = {0, 1, 0xFFFFFFFF, 0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF};
    std::vector<std::string> encoded;
    for (auto v : vals) {
        char buf[8];
        _KeyCodec::encode_uint64(v, buf);
        encoded.emplace_back(buf, 8);
    }
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        BOOST_TEST(cmp(encoded[i].data(), encoded[i + 1].data(), 8) < 0);
    }
}

BOOST_AUTO_TEST_CASE(sort_order_int32) {
    std::vector<int32_t> vals = {INT32_MIN, -1000, -1, 0, 1, 1000, INT32_MAX};
    std::vector<std::string> encoded;
    for (auto v : vals) {
        char buf[4];
        _KeyCodec::encode_int32(v, buf);
        encoded.emplace_back(buf, 4);
    }
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        BOOST_TEST(cmp(encoded[i].data(), encoded[i + 1].data(), 4) < 0);
    }
}

BOOST_AUTO_TEST_CASE(sort_order_int64) {
    std::vector<int64_t> vals = {INT64_MIN, -1000, -1, 0, 1, 1000, INT64_MAX};
    std::vector<std::string> encoded;
    for (auto v : vals) {
        char buf[8];
        _KeyCodec::encode_int64(v, buf);
        encoded.emplace_back(buf, 8);
    }
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        BOOST_TEST(cmp(encoded[i].data(), encoded[i + 1].data(), 8) < 0);
    }
}

BOOST_AUTO_TEST_CASE(sort_order_float) {
    // -0.0 and +0.0 have distinct IEEE 754 representations;
    // the encoding preserves this: -0.0 sorts just before +0.0.
    std::vector<float> vals = {-INFINITY, -1.0e10f, -1.0f, -0.0f, 0.0f, 1.0f, 1.0e10f, INFINITY};
    std::vector<std::string> encoded;
    for (auto v : vals) {
        char buf[4];
        _KeyCodec::encode_float(v, buf);
        encoded.emplace_back(buf, 4);
    }
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        BOOST_TEST(cmp(encoded[i].data(), encoded[i + 1].data(), 4) < 0);
    }
}

BOOST_AUTO_TEST_CASE(sort_order_double) {
    std::vector<double> vals = {(double)-INFINITY, -1.0e100, -1.0, -0.0, 0.0, 1.0, 1.0e100, (double)INFINITY};
    std::vector<std::string> encoded;
    for (auto v : vals) {
        char buf[8];
        _KeyCodec::encode_double(v, buf);
        encoded.emplace_back(buf, 8);
    }
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        BOOST_TEST(cmp(encoded[i].data(), encoded[i + 1].data(), 8) < 0);
    }
}

// ============================================================
// KeyBuilder / KeyReader roundtrip
// ============================================================

BOOST_AUTO_TEST_CASE(builder_reader_numeric) {
    _KeyBuilder b;
    b.append_uint32(42);
    b.append_int64(-100);
    b.append_double(3.14);

    _KeyReader r(b.data(), b.size());
    BOOST_TEST(r.read_uint32() == 42u);
    BOOST_TEST(r.read_int64() == -100);
    BOOST_TEST(r.read_double() == 3.14);
    BOOST_TEST(r.remaining() == 0u);
}

BOOST_AUTO_TEST_CASE(builder_reader_string) {
    _KeyBuilder b;
    b.append_string("hello", 5);
    b.append_uint16(999);
    b.append_string("world", 5);

    _KeyReader r(b.data(), b.size());
    auto [s1, l1] = r.read_string();
    BOOST_TEST(l1 == 5u);
    BOOST_TEST(std::string(s1, l1) == "hello");
    BOOST_TEST(r.read_uint16() == 999u);
    auto [s2, l2] = r.read_string();
    BOOST_TEST(l2 == 5u);
    BOOST_TEST(std::string(s2, l2) == "world");
    BOOST_TEST(r.remaining() == 0u);
}

BOOST_AUTO_TEST_CASE(builder_reader_bytes) {
    _KeyBuilder b;
    b.append_uint8(7);
    b.append_bytes("ABCDEF", 6);

    _KeyReader r(b.data(), b.size());
    BOOST_TEST(r.read_uint8() == 7u);
    const char* raw = r.read_bytes(6);
    BOOST_TEST(std::string(raw, 6) == "ABCDEF");
    BOOST_TEST(r.remaining() == 0u);
}

// ============================================================
// Composite key sort order
// ============================================================

BOOST_AUTO_TEST_CASE(composite_key_sort) {
    // Build keys: (string prefix, uint32 id)
    // "apple"/10 < "apple"/20 < "banana"/1
    auto make_key = [](const char* s, uint32_t id) {
        _KeyBuilder b;
        b.append_string(s, std::strlen(s));
        b.append_uint32(id);
        return std::string(b.data(), b.size());
    };

    std::string k1 = make_key("apple", 10);
    std::string k2 = make_key("apple", 20);
    std::string k3 = make_key("banana", 1);

    BOOST_TEST(k1 < k2);
    BOOST_TEST(k2 < k3);
    BOOST_TEST(k1 < k3);
}

BOOST_AUTO_TEST_CASE(builder_clear) {
    _KeyBuilder b;
    b.append_uint32(42);
    BOOST_TEST(b.size() == 4u);
    b.clear();
    BOOST_TEST(b.size() == 0u);
    b.append_uint16(1);
    BOOST_TEST(b.size() == 2u);
}
