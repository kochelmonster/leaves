#ifndef _LEAVES_INTERN_CODEC_KEYBUILDER_HPP
#define _LEAVES_INTERN_CODEC_KEYBUILDER_HPP

#include <cassert>
#include <string>

#include "_keycodec.hpp"

namespace leaves {

// Accumulates order-preserving encoded fields into a composite key.
struct _KeyBuilder {
    std::string _buf;

    void append_uint8(uint8_t val) {
        char tmp[_KeyCodec::encoded_size_uint8];
        _KeyCodec::encode_uint8(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_uint16(uint16_t val) {
        char tmp[_KeyCodec::encoded_size_uint16];
        _KeyCodec::encode_uint16(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_uint32(uint32_t val) {
        char tmp[_KeyCodec::encoded_size_uint32];
        _KeyCodec::encode_uint32(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_uint64(uint64_t val) {
        char tmp[_KeyCodec::encoded_size_uint64];
        _KeyCodec::encode_uint64(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_int8(int8_t val) {
        char tmp[_KeyCodec::encoded_size_int8];
        _KeyCodec::encode_int8(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_int16(int16_t val) {
        char tmp[_KeyCodec::encoded_size_int16];
        _KeyCodec::encode_int16(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_int32(int32_t val) {
        char tmp[_KeyCodec::encoded_size_int32];
        _KeyCodec::encode_int32(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_int64(int64_t val) {
        char tmp[_KeyCodec::encoded_size_int64];
        _KeyCodec::encode_int64(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_float(float val) {
        char tmp[_KeyCodec::encoded_size_float];
        _KeyCodec::encode_float(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    void append_double(double val) {
        char tmp[_KeyCodec::encoded_size_double];
        _KeyCodec::encode_double(val, tmp);
        _buf.append(tmp, sizeof(tmp));
    }

    // Append a null-terminated string. The 0x00 terminator is included so
    // the reader can find the boundary. Strings must not contain 0x00 bytes.
    void append_string(const char* s, size_t len) {
        _buf.append(s, len);
        _buf.push_back('\0');
    }

    // Append raw bytes without a terminator. The caller must know the
    // length at decode time (e.g. fixed-size or last field in the key).
    void append_bytes(const char* data, size_t len) {
        _buf.append(data, len);
    }

    const char* data() const { return _buf.data(); }
    size_t size() const { return _buf.size(); }
    void clear() { _buf.clear(); }
};

// Reads order-preserving encoded fields from a buffer.
struct _KeyReader {
    const char* _data;
    size_t _size;
    size_t _pos;

    _KeyReader(const char* data, size_t size)
        : _data(data), _size(size), _pos(0) {}

    size_t remaining() const { return _size - _pos; }

    uint8_t read_uint8() {
        assert(_pos + _KeyCodec::encoded_size_uint8 <= _size);
        uint8_t val = _KeyCodec::decode_uint8(_data + _pos);
        _pos += _KeyCodec::encoded_size_uint8;
        return val;
    }

    uint16_t read_uint16() {
        assert(_pos + _KeyCodec::encoded_size_uint16 <= _size);
        uint16_t val = _KeyCodec::decode_uint16(_data + _pos);
        _pos += _KeyCodec::encoded_size_uint16;
        return val;
    }

    uint32_t read_uint32() {
        assert(_pos + _KeyCodec::encoded_size_uint32 <= _size);
        uint32_t val = _KeyCodec::decode_uint32(_data + _pos);
        _pos += _KeyCodec::encoded_size_uint32;
        return val;
    }

    uint64_t read_uint64() {
        assert(_pos + _KeyCodec::encoded_size_uint64 <= _size);
        uint64_t val = _KeyCodec::decode_uint64(_data + _pos);
        _pos += _KeyCodec::encoded_size_uint64;
        return val;
    }

    int8_t read_int8() {
        assert(_pos + _KeyCodec::encoded_size_int8 <= _size);
        int8_t val = _KeyCodec::decode_int8(_data + _pos);
        _pos += _KeyCodec::encoded_size_int8;
        return val;
    }

    int16_t read_int16() {
        assert(_pos + _KeyCodec::encoded_size_int16 <= _size);
        int16_t val = _KeyCodec::decode_int16(_data + _pos);
        _pos += _KeyCodec::encoded_size_int16;
        return val;
    }

    int32_t read_int32() {
        assert(_pos + _KeyCodec::encoded_size_int32 <= _size);
        int32_t val = _KeyCodec::decode_int32(_data + _pos);
        _pos += _KeyCodec::encoded_size_int32;
        return val;
    }

    int64_t read_int64() {
        assert(_pos + _KeyCodec::encoded_size_int64 <= _size);
        int64_t val = _KeyCodec::decode_int64(_data + _pos);
        _pos += _KeyCodec::encoded_size_int64;
        return val;
    }

    float read_float() {
        assert(_pos + _KeyCodec::encoded_size_float <= _size);
        float val = _KeyCodec::decode_float(_data + _pos);
        _pos += _KeyCodec::encoded_size_float;
        return val;
    }

    double read_double() {
        assert(_pos + _KeyCodec::encoded_size_double <= _size);
        double val = _KeyCodec::decode_double(_data + _pos);
        _pos += _KeyCodec::encoded_size_double;
        return val;
    }

    // Read a null-terminated string. Returns {pointer, length} excluding
    // the terminator. Advances past the 0x00.
    std::pair<const char*, size_t> read_string() {
        const char* start = _data + _pos;
        size_t i = _pos;
        while (i < _size && _data[i] != '\0') {
            ++i;
        }
        assert(i < _size);  // must find terminator
        size_t len = i - _pos;
        _pos = i + 1;  // skip past the 0x00
        return {start, len};
    }

    // Read a fixed number of raw bytes.
    const char* read_bytes(size_t len) {
        assert(_pos + len <= _size);
        const char* p = _data + _pos;
        _pos += len;
        return p;
    }
};

}  // namespace leaves

#endif  // _LEAVES_INTERN_CODEC_KEYBUILDER_HPP
