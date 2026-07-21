// PCM-only SPSC ring buffer for the Scream → Diretta pipeline.
//
// Producer: receiver thread (rcv_network) calling push().
// Consumer: Diretta SDK send thread, via Sync::getNewStream() callback,
//           which calls pop() / popSilence().
//
// Differences vs. slim2Diretta's DirettaRingBuffer:
//   * PCM only. Scream packets are already PCM with a small header, so we
//     never need DSD bit-reversal, byte-swap, 16→32 / 16→24 upsampling, or
//     similar codec outputs. Those paths in slim2Diretta exist because
//     LMS/SlimProto can deliver compressed audio (FLAC/MP3/AAC/Ogg) and
//     DSD-over-PCM, none of which Scream produces.
//   * 24-bit repack is performed by the producer (diretta.cpp) before the
//     bytes reach the ring: the ring only stores the format the SDK expects.
//   * No SIMD shuffles. Steady-state path is a pair of memcpy()s.
//   * No staging buffers. The slim2Diretta ring carries 192KB of staging
//     buffers (24-bit pack / 16→32 / DSD); we don't need any of them.
//   * No S24-pack auto-detection state machine. Scream tags 24-bit content
//     explicitly via `sample_size` and `wire_layout` in the receiver_format_t
//     header.
//
// Steady-state hot path: push() and pop() both do at most two memcpy()s
// against a preallocated, frame-aligned buffer. No heap allocations.
//
// Ring-full policy
// ----------------
// Real-time Scream is a live source: there is no upstream that can be told
// to slow down, and stalling the receiver thread on a full ring would only
// delay packets the producer has already received and inflate end-to-end
// latency. The policy is therefore **drop-newest, frame-aligned, never
// block**:
//
//   * push() and pushFrames() are non-blocking. If the ring does not have
//     enough free space for the requested bytes, the *new* bytes are
//     dropped (counted in m_dropBytes / m_dropFrames). The producer
//     continues with the next packet.
//   * pushFrames() additionally guarantees that drops happen on whole-frame
//     boundaries -- if a partial frame would otherwise be dropped, the
//     entire frame is dropped instead. This keeps the ring's contents
//     frame-aligned at all times.
//   * dropMs() converts dropped frames to milliseconds using the configured
//     (sample rate * channels * bytesPerSample) for human-readable stats.
//
// Underrun is the dual of drop: when the consumer asks for more bytes than
// are available, popOrSilence() fills the gap with the configured silence
// byte and bumps m_underrunCount. Both counters are atomic and lock-free.

#ifndef SCREAM_DIRETTA_RING_H
#define SCREAM_DIRETTA_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scream_diretta {

class PcmRing {
public:
    PcmRing() = default;

    // Resize and reset state. silenceByte is what pop()-on-empty emits.
    // Called from the control path only (open / format-change reconfigure),
    // never from the audio hot path.
    void resize(size_t bytes, uint8_t silenceByte) {
        size_t pow2 = roundUpPow2(bytes);
        m_size = pow2;
        m_mask = pow2 - 1;
        m_buffer.assign(pow2, silenceByte);
        m_silence.store(silenceByte, std::memory_order_release);
        clear();
    }

    void clear() {
        m_writePos.store(0, std::memory_order_release);
        m_readPos.store(0, std::memory_order_release);
        m_dropBytes.store(0, std::memory_order_release);
        m_dropFrames.store(0, std::memory_order_release);
        m_pushedBytes.store(0, std::memory_order_release);
        m_pushedFrames.store(0, std::memory_order_release);
        m_underrunCount.store(0, std::memory_order_release);
    }

    size_t capacity() const { return m_size; }
    uint8_t silenceByte() const { return m_silence.load(std::memory_order_acquire); }

    size_t available() const {
        if (m_size == 0) return 0;
        size_t wp = m_writePos.load(std::memory_order_acquire);
        size_t rp = m_readPos.load(std::memory_order_acquire);
        return (wp - rp) & m_mask;
    }

    size_t freeSpace() const {
        if (m_size == 0) return 0;
        size_t wp = m_writePos.load(std::memory_order_acquire);
        size_t rp = m_readPos.load(std::memory_order_acquire);
        return (rp - wp - 1) & m_mask;
    }

    // Producer side, byte-granular. Caller is responsible for frame
    // alignment of `len`. Drop-newest: whatever doesn't fit is counted in
    // m_dropBytes and discarded. Non-blocking.
    size_t push(const uint8_t* data, size_t len) {
        if (m_size == 0 || len == 0) return 0;
        size_t free = freeSpace();
        if (len > free) {
            m_dropBytes.fetch_add(len - free, std::memory_order_relaxed);
            len = free;
            if (len == 0) return 0;
        }
        size_t wp = m_writePos.load(std::memory_order_relaxed);
        size_t first = (m_size - wp < len) ? (m_size - wp) : len;
        std::memcpy(m_buffer.data() + wp, data, first);
        if (first < len) {
            std::memcpy(m_buffer.data(), data + first, len - first);
        }
        m_writePos.store((wp + len) & m_mask, std::memory_order_release);
        m_pushedBytes.fetch_add(len, std::memory_order_relaxed);
        return len;
    }

    // Producer side, frame-granular. `bytesPerFrame` must be > 0 and
    // `len` must already be a whole multiple of bytesPerFrame -- the caller
    // (diretta.cpp) carries the partial-frame tail in a fixed-size buffer.
    //
    // Drop-newest, frame-aligned: if free space cannot hold `len` bytes, we
    // drop down to the largest whole-frame multiple that does fit. The
    // remainder is counted in m_dropBytes / m_dropFrames. Non-blocking.
    //
    // Returns the number of bytes actually written (always a multiple of
    // bytesPerFrame).
    size_t pushFrames(const uint8_t* data, size_t len, size_t bytesPerFrame) {
        if (m_size == 0 || len == 0 || bytesPerFrame == 0) return 0;
        // Caller guarantees frame alignment, but be defensive: round down.
        size_t alignedLen = (len / bytesPerFrame) * bytesPerFrame;
        size_t free = freeSpace();
        if (alignedLen > free) {
            // Truncate the write to whole frames that fit.
            size_t fitFrames = free / bytesPerFrame;
            size_t fitBytes  = fitFrames * bytesPerFrame;
            size_t dropped   = alignedLen - fitBytes;
            m_dropBytes.fetch_add(dropped, std::memory_order_relaxed);
            m_dropFrames.fetch_add(dropped / bytesPerFrame, std::memory_order_relaxed);
            alignedLen = fitBytes;
            if (alignedLen == 0) return 0;
        }
        size_t wp = m_writePos.load(std::memory_order_relaxed);
        size_t first = (m_size - wp < alignedLen) ? (m_size - wp) : alignedLen;
        std::memcpy(m_buffer.data() + wp, data, first);
        if (first < alignedLen) {
            std::memcpy(m_buffer.data(), data + first, alignedLen - first);
        }
        m_writePos.store((wp + alignedLen) & m_mask, std::memory_order_release);
        m_pushedBytes.fetch_add(alignedLen, std::memory_order_relaxed);
        m_pushedFrames.fetch_add(alignedLen / bytesPerFrame, std::memory_order_relaxed);
        return alignedLen;
    }

    // Consumer side. If fewer than `len` bytes are available, fills the
    // remainder with silenceByte and increments the underrun counter.
    // Designed to be called from the SDK send thread inside getNewStream().
    void popOrSilence(uint8_t* dest, size_t len) {
        if (m_size == 0 || len == 0) {
            if (dest && len) std::memset(dest, m_silence.load(std::memory_order_acquire), len);
            return;
        }
        size_t avail = available();
        size_t take = (avail < len) ? avail : len;
        if (take > 0) {
            size_t rp = m_readPos.load(std::memory_order_relaxed);
            size_t first = (m_size - rp < take) ? (m_size - rp) : take;
            std::memcpy(dest, m_buffer.data() + rp, first);
            if (first < take) {
                std::memcpy(dest + first, m_buffer.data(), take - first);
            }
            m_readPos.store((rp + take) & m_mask, std::memory_order_release);
        }
        if (take < len) {
            std::memset(dest + take, m_silence.load(std::memory_order_acquire), len - take);
            m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Stats accessors (read by anybody, atomic-safe).
    uint64_t underrunCount() const { return m_underrunCount.load(std::memory_order_relaxed); }
    uint64_t dropBytes()     const { return m_dropBytes.load(std::memory_order_relaxed); }
    uint64_t dropFrames()    const { return m_dropFrames.load(std::memory_order_relaxed); }
    uint64_t pushedBytes()   const { return m_pushedBytes.load(std::memory_order_relaxed); }
    uint64_t pushedFrames()  const { return m_pushedFrames.load(std::memory_order_relaxed); }

private:
    static size_t roundUpPow2(size_t v) {
        if (v < 2) return 2;
        size_t r = 1;
        while (r < v) {
            if (r > (SIZE_MAX / 2)) break; // prevent overflow → infinite loop
            r <<= 1;
        }
        return r;
    }

    std::vector<uint8_t> m_buffer;
    size_t m_size = 0;
    size_t m_mask = 0;

    // Producer-only hot path — each variable on its own cache line to prevent
    // false sharing with the consumer thread.
    alignas(64) std::atomic<size_t>  m_writePos{0};
    alignas(64) std::atomic<uint64_t> m_pushedBytes{0};
    alignas(64) std::atomic<uint64_t> m_pushedFrames{0};
    alignas(64) std::atomic<uint64_t> m_dropBytes{0};
    alignas(64) std::atomic<uint64_t> m_dropFrames{0};

    // Consumer-only hot path — each variable on its own cache line.
    alignas(64) std::atomic<size_t>  m_readPos{0};
    alignas(64) std::atomic<uint64_t> m_underrunCount{0};

    // Shared, low-frequency read (silence byte, set once at resize).
    alignas(64) std::atomic<uint8_t>  m_silence{0};
};

} // namespace scream_diretta

#endif // SCREAM_DIRETTA_RING_H
