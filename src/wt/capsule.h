// WT capsule framing (RFC 9599) over a stream: QUIC varint (RFC 9000
// s.16) encoded type + length prefixes followed by the payload. Pure
// buffering logic - no sockets, no locking, no listener - which is what
// the unit tests drive. This is the sole implementation; WTClient and
// external consumers (the gateway) link against it.

#ifndef QUICSRT_WT_CAPSULE_H
#define QUICSRT_WT_CAPSULE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace quicsrt
{

// QUIC varint (RFC 9000 s.16): 2-bit length prefix, big-endian payload.
// VarintPut writes `v` to `dst` (at most 8 bytes) and returns the width.
// VarintGet decodes at most `n` bytes, returning the value and the width,
// or nullopt when the input is empty or the encoded varint is truncated.
std::size_t                                          VarintPut(std::uint8_t* dst, std::uint64_t v);
std::optional<std::pair<std::uint64_t, std::size_t>> VarintGet(const std::uint8_t* p, std::size_t n);

// Incremental capsule consumer: Feed appends stream bytes and consumes
// every whole capsule (type varint, length varint, payload). Unknown
// types are skipped via their length prefix - RFC 9599 makes everything
// skippable except CLOSE_WEBTRANSPORT_SESSION, the only capsule that
// terminates the session. An unconsumed partial tail (truncated varint or
// short payload) stays buffered until later Feeds complete it, but never
// without bound: a capsule whose header demands more than kMaxPendingBytes
// trips the sticky overflow flag instead (a malicious or broken peer).
// What to do with an overflowed stream - typically killing the session -
// is the caller's call. Pure buffering logic, the TsChunker precedent:
// no sockets, no locking.
class WtCapsuleParser
{
public:
    // CLOSE_WEBTRANSPORT_SESSION (RFC 9599 s.7): the only terminal capsule.
    static constexpr std::uint64_t kCapsuleCloseWtSession = 0x2843;

    // Pending-bytes ceiling (~1 MiB): an incomplete capsule demanding more
    // than this - the only way the partial tail can grow past it - trips
    // overflow() instead of buffering forever.
    static constexpr std::size_t kMaxPendingBytes = 1 << 20;

    // Appends `size` bytes to the internal buffer, then consumes every
    // whole capsule in it. Consuming a CLOSE_WT_SESSION capsule sets the
    // sticky closed flag; unknown types and PADDING are skipped by their
    // length prefix alone. Once overflow() is set the bytes are dropped
    // instead of buffered.
    void Feed(const std::uint8_t* data, std::size_t size);

    // True once a CLOSE capsule has been consumed; sticky.
    [[nodiscard]] bool closed() const;

    // True once a capsule header demanded more than kMaxPendingBytes;
    // sticky. The buffered tail is discarded and later Feeds drop their
    // bytes, so pending() stays bounded - the framing is untrustworthy
    // and the caller should tear the stream down.
    [[nodiscard]] bool overflow() const;

    // Buffered-but-unconsumed bytes (test observability: the partial tail
    // or garbage waiting for more bytes). Bounded by kMaxPendingBytes.
    [[nodiscard]] std::size_t pending() const;

private:
    std::vector<std::uint8_t> buf_;
    bool                      closed_   = false;
    bool                      overflow_ = false;
};

} // namespace quicsrt

#endif // QUICSRT_WT_CAPSULE_H
