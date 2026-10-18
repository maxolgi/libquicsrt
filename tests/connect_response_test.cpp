// ConnectResponseParser units: the bounded extended-CONNECT response-
// head accumulation the WT client feeds from its session-stream read
// path. Covered: whole and split heads, capsule bytes riding the
// completing chunk, the 64 KiB ceiling (an at-limit head succeeds fed at
// the client's read granularity; one byte over - terminated or not -
// trips the sticky overflow flag and stays bounded), and the status-line
// variants behind the client's close reasons (HTTP status N, the
// unparseable line). Plain checks (not assert) so Release stays
// meaningful; pure parsing - no sockets, no threads.

#include "wt/connect_response.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

using quicsrt::ConnectResponseParser;

constexpr std::size_t kReadChunk = 16384; // the WT client's lsquic read granularity

int  g_checks = 0;
bool g_ok     = true;

bool Check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond)
    {
        g_ok = false;
        std::fprintf(stderr, "connect_response_test: FAIL: %s\n", what);
    }
    return cond;
}

std::vector<std::uint8_t> Bytes(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// A well-formed response head of exactly `total` bytes (>= 20): status
// line, filler header lines at a 64-byte stride (the last one padded),
// blank line.
std::vector<std::uint8_t> MakeHead(std::size_t total)
{
    const std::string kStatus = "HTTP/1.1 200\r\n";
    const std::string kLine   = "x-pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; // 62 bytes + CRLF
    std::string       head    = kStatus;
    while (head.size() + kLine.size() + 2 + 4 <= total - 2)
    {
        head += kLine + "\r\n";
    }
    const std::size_t room = total - 2 - head.size(); // >= 4: the last line + its CRLF
    std::string       last = kLine;
    last.resize(room - 2, 'a');
    head += last + "\r\n";
    head += "\r\n";
    return Bytes(head);
}

bool TestSimple200()
{
    const auto wire = Bytes("HTTP/1.1 200 OK\r\nserver: quicsrt-gateway\r\n\r\n");

    ConnectResponseParser parser;
    const std::size_t     consumed = parser.Feed(wire.data(), wire.size());
    if (!Check(consumed == wire.size(), "a whole head in one Feed is consumed entirely"))
    {
        return false;
    }
    if (!Check(parser.done(), "a complete head sets done"))
    {
        return false;
    }
    if (!Check(parser.status() == 200, "status line parses to 200"))
    {
        return false;
    }
    if (!Check(parser.status_line() == "HTTP/1.1 200 OK", "status line text is kept"))
    {
        return false;
    }
    if (!Check(parser.pending() == 0, "a complete head leaves nothing pending"))
    {
        return false;
    }
    return Check(!parser.overflow(), "a complete in-limit head does not overflow");
}

bool TestSplitHead()
{
    const auto wire = Bytes("HTTP/1.1 200 OK\r\nx-a: 1\r\nx-b: 2\r\n\r\n");

    ConnectResponseParser parser;
    const std::size_t     first = parser.Feed(wire.data(), 10);
    if (!Check(first == 0, "an incomplete head consumes nothing"))
    {
        return false;
    }
    if (!Check(!parser.done(), "an incomplete head is not done"))
    {
        return false;
    }
    if (!Check(parser.pending() == 10, "an incomplete head buffers what arrived"))
    {
        return false;
    }
    const std::size_t second = parser.Feed(wire.data() + 10, wire.size() - 10);
    if (!Check(second == wire.size() - 10, "the completing Feed consumes its whole input"))
    {
        return false;
    }
    if (!Check(parser.done() && parser.status() == 200, "a split head completes with status 200"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "a completed head leaves nothing pending");
}

bool TestCapsuleBytesAfterHead()
{
    // The completing chunk carries two capsule-stream bytes past the
    // terminator: Feed must count them out of the head.
    const auto                head = Bytes("HTTP/1.1 200\r\n\r\n");
    std::vector<std::uint8_t> wire = head;
    wire.push_back(0xab);
    wire.push_back(0xcd);

    ConnectResponseParser parser;
    const std::size_t     consumed = parser.Feed(wire.data(), wire.size());
    if (!Check(consumed == head.size(), "consumed stops at the head terminator"))
    {
        return false;
    }
    if (!Check(parser.done() && parser.status() == 200, "head plus trailing bytes completes the head"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "trailing capsule bytes stay with the caller");
}

bool TestAtLimitHeadSucceeds()
{
    // A realistically large head - 64 KiB of 64-byte filler header lines
    // - fed at the client's read granularity: every chunk before the
    // last is retained without tripping, and a head landing exactly on
    // the ceiling still parses.
    const auto wire = MakeHead(ConnectResponseParser::kMaxHeaderBytes);
    Check(wire.size() == ConnectResponseParser::kMaxHeaderBytes, "at-limit head is exactly the ceiling");

    ConnectResponseParser parser;
    std::size_t           fed = 0;
    while (fed < wire.size())
    {
        const std::size_t take     = std::min(kReadChunk, wire.size() - fed);
        const std::size_t consumed = parser.Feed(wire.data() + fed, take);
        fed += take;
        if (fed < wire.size())
        {
            if (!Check(consumed == 0, "pre-completion chunks consume nothing"))
            {
                return false;
            }
            if (!Check(!parser.overflow(), "an under-ceiling partial head does not overflow"))
            {
                return false;
            }
        }
        else if (!Check(consumed == take, "the completing chunk is consumed entirely"))
        {
            return false;
        }
        if (!Check(parser.pending() <= ConnectResponseParser::kMaxHeaderBytes, "pending stays at/below the ceiling"))
        {
            return false;
        }
    }
    if (!Check(parser.done(), "an at-limit head completes"))
    {
        return false;
    }
    if (!Check(parser.status() == 200, "an at-limit head parses the status line"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "an at-limit head leaves nothing pending");
}

bool TestOneByteOverFails()
{
    // One byte over the ceiling, terminator and all: the bound is
    // checked before the terminator search, so a complete-but-oversized
    // head is dead too.
    const auto wire = MakeHead(ConnectResponseParser::kMaxHeaderBytes + 1);
    Check(wire.size() == ConnectResponseParser::kMaxHeaderBytes + 1, "over-limit head is one byte past the ceiling");

    ConnectResponseParser parser;
    const std::size_t     consumed = parser.Feed(wire.data(), wire.size());
    if (!Check(parser.overflow(), "a head over the ceiling sets overflow"))
    {
        return false;
    }
    if (!Check(consumed == 0 && !parser.done(), "an overflowed head consumes nothing and never completes"))
    {
        return false;
    }
    if (!Check(parser.status() == -1, "an overflowed head never parses a status"))
    {
        return false;
    }
    if (!Check(parser.pending() == 0, "an overflowed head discards its buffer"))
    {
        return false;
    }

    const std::vector<std::uint8_t> more(kReadChunk, 'x');
    parser.Feed(more.data(), more.size());
    if (!Check(parser.overflow(), "overflow is sticky after further feeds"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "further feeds after overflow buffer nothing");
}

bool TestUnterminatedOverLimitFails()
{
    // The OOM shape: a status line then filler with no blank line, ever.
    // Fed at read granularity, the accumulation trips the ceiling
    // mid-stream instead of growing without bound.
    std::vector<std::uint8_t> wire = Bytes("HTTP/1.1 200\r\nx-endless: ");
    wire.resize(ConnectResponseParser::kMaxHeaderBytes + 1, 'x');

    ConnectResponseParser parser;
    bool                  tripped = false;
    std::size_t           fed     = 0;
    while (fed < wire.size())
    {
        const std::size_t take     = std::min(kReadChunk, wire.size() - fed);
        const std::size_t consumed = parser.Feed(wire.data() + fed, take);
        fed += take;
        if (parser.overflow())
        {
            tripped = true;
            if (!Check(consumed == 0, "an overflowing feed consumes nothing"))
            {
                return false;
            }
        }
        else
        {
            if (!Check(consumed == 0, "an unterminated head never completes"))
            {
                return false;
            }
            if (!Check(parser.pending() == fed, "an unterminated head buffers everything fed"))
            {
                return false;
            }
        }
        if (!Check(parser.pending() <= ConnectResponseParser::kMaxHeaderBytes, "pending stays at/below the ceiling"))
        {
            return false;
        }
        if (!Check(!parser.done(), "an unterminated head never completes done"))
        {
            return false;
        }
    }
    if (!Check(tripped, "an unterminated head over the ceiling trips overflow"))
    {
        return false;
    }
    return Check(parser.status() == -1, "no status is ever parsed from an unterminated head");
}

bool TestStatusLineVariants()
{
    // The two non-200 close reasons the WT client builds from the parse:
    // "HTTP status N" and the unparseable-line failure with the kept
    // (truncated) text.
    const auto            not_found = Bytes("HTTP/1.1 404 Not Found\r\n\r\n");
    ConnectResponseParser parser;
    parser.Feed(not_found.data(), not_found.size());
    if (!Check(parser.done() && parser.status() == 404, "a non-200 status parses"))
    {
        return false;
    }

    const auto            garbage = Bytes("QUICSRT-NOT-HTTP\r\n\r\n");
    ConnectResponseParser garbage_parser;
    garbage_parser.Feed(garbage.data(), garbage.size());
    if (!Check(garbage_parser.done(), "a head with an unparseable line still completes"))
    {
        return false;
    }
    if (!Check(garbage_parser.status() == -1, "an unparseable status line yields -1"))
    {
        return false;
    }
    if (!Check(garbage_parser.status_line() == "QUICSRT-NOT-HTTP", "the unparseable line text is kept"))
    {
        return false;
    }

    // Status lines truncate to 31 bytes, matching the close reason.
    const std::string     long_line(48, 'z');
    const auto            long_wire = Bytes(long_line + "\r\n\r\n");
    ConnectResponseParser long_parser;
    long_parser.Feed(long_wire.data(), long_wire.size());
    return Check(long_parser.status_line().size() == 31, "a long status line is truncated to 31 bytes");
}

} // namespace

int main()
{
    struct Section
    {
        const char* name;
        bool (*fn)();
    };

    const Section sections[] = {
        {.name = "simple-200", .fn = TestSimple200},
        {.name = "split-head", .fn = TestSplitHead},
        {.name = "capsule-bytes-after-head", .fn = TestCapsuleBytesAfterHead},
        {.name = "at-limit-head-succeeds", .fn = TestAtLimitHeadSucceeds},
        {.name = "one-byte-over-fails", .fn = TestOneByteOverFails},
        {.name = "unterminated-over-limit-fails", .fn = TestUnterminatedOverLimitFails},
        {.name = "status-line-variants", .fn = TestStatusLineVariants},
    };

    bool ok = true;
    for (const Section& section : sections)
    {
        ok = section.fn() && ok;
    }
    if (!ok)
    {
        std::fprintf(stderr, "connect_response_test: FAILED\n");
        return 1;
    }
    std::printf("connect_response_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
