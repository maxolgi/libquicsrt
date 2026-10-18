// CONNECT response-head accumulation for the WT client (the
// WtCapsuleParser precedent: pure buffering logic - no sockets, no
// locking, no listener - which is what the unit tests drive). With
// lsquic's built-in (HTTP/1.x) header interface the extended-CONNECT
// response arrives as a status line, headers, and a blank line; anything
// after that terminator is the session stream's capsule stream. The head
// itself is bounded: a gateway that never terminates its header block
// cannot grow the client's buffer without limit.

#ifndef QUICSRT_WT_CONNECT_RESPONSE_H
#define QUICSRT_WT_CONNECT_RESPONSE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quicsrt
{

class ConnectResponseParser
{
public:
    // Header-block ceiling (64 KiB): accumulating more than this without
    // the terminator - the only way the buffer can grow without bound -
    // trips the sticky overflow flag instead (a malicious or broken
    // gateway).
    static constexpr std::size_t kMaxHeaderBytes = 64 << 10;

    // Appends `size` bytes, then consumes the response head once its
    // terminator ("\r\n\r\n") arrives. Returns how many leading bytes of
    // THIS input belong to the head, terminator included - the rest is
    // capsule-stream bytes for the caller to route onward - or 0 while
    // the head is still incomplete. Once done() or overflow() is set the
    // bytes are dropped instead of buffered.
    std::size_t Feed(const std::uint8_t* data, std::size_t size);

    // True once the terminator has been consumed; sticky.
    [[nodiscard]] bool done() const;

    // True once the accumulation exceeded kMaxHeaderBytes before the
    // terminator; sticky. The buffered head is discarded and later Feeds
    // drop their bytes, so pending() stays bounded - the response is
    // untrustworthy and the caller should tear the session down.
    [[nodiscard]] bool overflow() const;

    // Buffered-but-unconsumed bytes (test observability). Bounded by
    // kMaxHeaderBytes.
    [[nodiscard]] std::size_t pending() const;

    // HTTP status code from the status line; -1 until the head is done,
    // and -1 when the status line did not parse.
    [[nodiscard]] int status() const;

    // The status line as received, truncated to 31 bytes: diagnostics,
    // and the unparseable-line close reason.
    [[nodiscard]] const std::string& status_line() const;

private:
    // Parses the status line (up to the first CR) from buf_; ported
    // verbatim from the WTClient response parsing it replaces.
    void ParseStatusLine();

    std::vector<std::uint8_t> buf_;
    std::string               status_line_;
    bool                      done_     = false;
    bool                      overflow_ = false;
    int                       status_   = -1;
};

} // namespace quicsrt

#endif // QUICSRT_WT_CONNECT_RESPONSE_H
