#ifndef _LEAVES_INTERN_UTIL_MPSC_QUEUE_HPP
#define _LEAVES_INTERN_UTIL_MPSC_QUEUE_HPP

#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace leaves {

// Lock-free multi-producer single-consumer byte ring buffer.
// Each entry is a 2-byte length header followed by variable-length
// payload, padded to 2-byte alignment.  If an entry does not fit in
// the remaining buffer space, a SKIP marker (0xFFFF) is written and
// the producer wraps to the beginning.
//
// No per-entry heap allocation.
//
// Memory ordering:
//   - push: acquire on _tail load (fullness), release on state store
//   - pop:  release on _tail store (free space), acquire on state load
//   - done: release/acquire on _done flag

struct _MPSCQueue {
    static constexpr size_t HEADER = 2;
    static constexpr size_t DEFAULT_CAPACITY = 256 * 1024;
    static constexpr uint16_t SKIP = 0xFFFF;

    static_assert(sizeof(std::atomic<uint16_t>) == sizeof(uint16_t));
    static_assert(std::atomic_ref<uint16_t>::required_alignment <= 2);

    std::unique_ptr<char[]> _buf;
    size_t _capacity;
    size_t _mask;

    alignas(64) std::atomic<size_t> _head{0};
    alignas(64) std::atomic<size_t> _tail{0};
    size_t _free_phys{0};
    size_t _free_esize{0};
    std::atomic<bool> _done{false};

    static size_t _esize(size_t len) {
        return (HEADER + len + 1) & ~size_t(1);
    }

    std::atomic_ref<uint16_t> _state_at(size_t phys) const {
        return std::atomic_ref<uint16_t>(
            *reinterpret_cast<uint16_t*>(_buf.get() + phys));
    }

    _MPSCQueue(size_t capacity = DEFAULT_CAPACITY)
        : _buf(new char[capacity]())
        , _capacity(capacity)
        , _mask(capacity - 1) {
        assert(capacity >= 4 && (capacity & (capacity - 1)) == 0);
    }

    _MPSCQueue(const _MPSCQueue&) = delete;
    _MPSCQueue& operator=(const _MPSCQueue&) = delete;

    void push(const char* data, size_t len) {
        size_t esize = _esize(len);
        assert(esize <= _capacity);

        while (true) {
            size_t pos = _head.load(std::memory_order_relaxed);
            size_t phys = pos & _mask;
            size_t remaining = _capacity - phys;

            if (esize > remaining) {
                size_t tail = _tail.load(std::memory_order_acquire);
                if (pos + remaining - tail > _capacity) {
                    std::this_thread::yield();
                    continue;
                }
                if (_head.compare_exchange_weak(pos, pos + remaining,
                        std::memory_order_acq_rel)) {
                    _state_at(phys).store(SKIP, std::memory_order_release);
                }
                continue;
            }

            size_t tail = _tail.load(std::memory_order_acquire);
            if (pos + esize - tail > _capacity) {
                std::this_thread::yield();
                continue;
            }

            if (_head.compare_exchange_weak(pos, pos + esize,
                    std::memory_order_acq_rel)) {
                std::memcpy(_buf.get() + phys + HEADER, data, len);
                _state_at(phys).store(static_cast<uint16_t>(len + 1),
                                      std::memory_order_release);
                return;
            }
        }
    }

    void push(const std::string& s) { push(s.data(), s.size()); }

    bool pop(const char*& data, size_t& len) {
        if (_free_esize) {
            _state_at(_free_phys).store(0, std::memory_order_relaxed);
            size_t t = _tail.load(std::memory_order_relaxed) + _free_esize;
            _tail.store(t, std::memory_order_release);
            _free_esize = 0;
        }

        size_t tail = _tail.load(std::memory_order_relaxed);
        while (true) {
            size_t phys = tail & _mask;
            uint16_t s = _state_at(phys).load(std::memory_order_acquire);

            if (s == 0) return false;

            if (s == SKIP) {
                size_t remaining = _capacity - phys;
                _state_at(phys).store(0, std::memory_order_relaxed);
                tail += remaining;
                _tail.store(tail, std::memory_order_release);
                continue;
            }

            size_t dlen = s - 1;
            data = _buf.get() + phys + HEADER;
            len = dlen;
            _free_phys = phys;
            _free_esize = _esize(dlen);
            return true;
        }
    }

    void finish() {
        _done.store(true, std::memory_order_release);
    }

    bool is_finished() const {
        if (!_done.load(std::memory_order_acquire)) return false;
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t phys = tail & _mask;
        return _state_at(phys).load(std::memory_order_acquire) == 0;
    }

    bool is_done() const {
        return _done.load(std::memory_order_acquire);
    }
};

}  // namespace leaves

#endif  // _LEAVES_INTERN_UTIL_MPSC_QUEUE_HPP
