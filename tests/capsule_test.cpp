// WtCapsuleParser units: QUIC varint roundtrips across all four widths
// and truncation, CLOSE detection (whole, split across Feeds, sticky,
// repeated), skippable unknown/PADDING capsules, partial-tail retention,
// empty feeds, and garbage-prefix retention. Plain checks (not assert) so
// Release stays meaningful; pure parsing - no sockets, no threads.

#include "wt/capsule.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{

using websrt::VarintGet;
using websrt::VarintPut;
using websrt::WtCapsuleParser;

int  g_checks = 0;
bool g_ok     = true;

bool Check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond)
    {
        g_ok = false;
        std::fprintf(stderr, "capsule_test: FAIL: %s\n", what);
    }
    return cond;
}

// Frames one capsule (type varint + length varint + payload) onto `out`.
void AppendCapsule(std::vector<std::uint8_t>& out, std::uint64_t type, const std::vector<std::uint8_t>& payload)
{
    std::uint8_t      v[8];
    const std::size_t tn = VarintPut(v, type);
    out.insert(out.end(), v, v + tn);
    const std::size_t ln = VarintPut(v, static_cast<std::uint64_t>(payload.size()));
    out.insert(out.end(), v, v + ln);
    out.insert(out.end(), payload.begin(), payload.end());
}

bool TestVarintRoundtrips()
{
    const struct
    {
        std::uint64_t value;
        std::size_t   width;
    } cases[] = {
        {0x3f, 1},
        {0x40, 2},
        {0x3fff, 2},
        {0x4000, 4},
        {0x3fffffff, 4},
        {0x40000000, 8},
        {0x0000deadbeefcafe, 8},
        {0x3fffffffffffffff, 8}, // maximum representable QUIC varint (62 bits)
    };
    for (const auto& c : cases)
    {
        std::uint8_t buf[8] = {};
        const bool   put_ok = Check(VarintPut(buf, c.value) == c.width, "VarintPut width");
        const auto   got    = VarintGet(buf, sizeof buf);
        if (!put_ok || !Check(got.has_value(), "VarintGet decodes a full varint"))
        {
            return false;
        }
        if (!Check(got->first == c.value, "VarintPut/VarintGet roundtrip value"))
        {
            return false;
        }
        if (!Check(got->second == c.width, "VarintGet reports the encoded width"))
        {
            return false;
        }
    }
    return true;
}

bool TestVarintTruncated()
{
    // 0x1234 encodes as a 2-byte varint; every proper prefix must fail.
    std::uint8_t buf[8] = {};
    Check(VarintPut(buf, 0x1234) == 2, "0x1234 encodes as a 2-byte varint");
    Check(!VarintGet(buf, 0).has_value(), "VarintGet on zero bytes is nullopt");
    Check(!VarintGet(buf, 1).has_value(), "VarintGet on the first byte of a 2-byte varint is nullopt");
    return true;
}

bool TestCloseAlone()
{
    std::vector<std::uint8_t> wire;
    AppendCapsule(wire, WtCapsuleParser::kCapsuleCloseWtSession, {});

    WtCapsuleParser parser;
    parser.Feed(wire.data(), wire.size());
    if (!Check(parser.closed(), "CLOSE alone in one Feed sets closed"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "CLOSE alone is fully consumed");
}

bool TestCloseSplitAcrossFeeds()
{
    std::vector<std::uint8_t> wire;
    AppendCapsule(wire, WtCapsuleParser::kCapsuleCloseWtSession, {});
    Check(wire.size() == 3, "CLOSE capsule is three bytes on the wire");

    WtCapsuleParser parser;
    for (std::size_t i = 0; i + 1 < wire.size(); ++i)
    {
        parser.Feed(wire.data() + i, 1);
        if (!Check(!parser.closed(), "split CLOSE stays open before the last byte"))
        {
            return false;
        }
        if (!Check(parser.pending() == i + 1, "split CLOSE retains the partial bytes"))
        {
            return false;
        }
    }
    parser.Feed(wire.data() + wire.size() - 1, 1);
    if (!Check(parser.closed(), "split CLOSE closes after the last byte"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "split CLOSE is fully consumed");
}

bool TestUnknownThenClose()
{
    // Unknown type with a 200-byte payload: the length prefix (200 needs
    // the 2-byte varint form) must carry the parser past the payload.
    std::vector<std::uint8_t> unknown_wire;
    AppendCapsule(unknown_wire, 0x3f, std::vector<std::uint8_t>(200, 0xaa));
    Check(unknown_wire.size() == 1 + 2 + 200, "unknown capsule framing is type + 2-byte length + payload");

    WtCapsuleParser parser;
    parser.Feed(unknown_wire.data(), unknown_wire.size());
    if (!Check(!parser.closed(), "unknown capsule does not close"))
    {
        return false;
    }
    if (!Check(parser.pending() == 0, "unknown capsule with payload is skipped via the length prefix"))
    {
        return false;
    }

    std::vector<std::uint8_t> close_wire;
    AppendCapsule(close_wire, WtCapsuleParser::kCapsuleCloseWtSession, {});
    parser.Feed(close_wire.data(), close_wire.size());
    if (!Check(parser.closed(), "CLOSE after an unknown capsule closes"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "unknown capsule and CLOSE both fully consumed");
}

bool TestPaddingThenClose()
{
    // PADDING (type 0x00, RFC 9599): payload of filler bytes, skippable.
    std::vector<std::uint8_t> wire;
    AppendCapsule(wire, 0x00, std::vector<std::uint8_t>(16, 0x00));
    AppendCapsule(wire, WtCapsuleParser::kCapsuleCloseWtSession, {});

    WtCapsuleParser parser;
    parser.Feed(wire.data(), wire.size());
    if (!Check(parser.closed(), "PADDING then CLOSE closes"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "PADDING then CLOSE is fully consumed");
}

bool TestTruncatedTail()
{
    // PADDING with length 0x41: the length takes the 2-byte varint form.
    // Feed the type byte and the FIRST length byte; completion follows.
    std::uint8_t head[2] = {};
    Check(VarintPut(&head[0], 0x00) == 1, "PADDING type is a 1-byte varint");
    Check(VarintPut(&head[1], 0x41) == 2, "length 0x41 encodes as a 2-byte varint");

    WtCapsuleParser parser;
    parser.Feed(head, 2);
    if (!Check(!parser.closed(), "truncated length varint does not close"))
    {
        return false;
    }
    if (!Check(parser.pending() == 2, "truncated length varint is retained"))
    {
        return false;
    }

    std::uint8_t second[2] = {};
    Check(VarintPut(second, 0x41) == 2, "length 0x41 re-encodes as a 2-byte varint");
    std::vector<std::uint8_t> rest(second + 1, second + 2); // the completing length byte
    rest.resize(1 + 0x41, 0x00);                            // plus the 0x41 payload bytes
    parser.Feed(rest.data(), rest.size());
    if (!Check(!parser.closed(), "completed PADDING capsule does not close"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "completed capsule is fully consumed");
}

bool TestEmptyFeed()
{
    WtCapsuleParser parser;
    parser.Feed(nullptr, 0);
    if (!Check(parser.pending() == 0, "empty Feed buffers nothing"))
    {
        return false;
    }
    if (!Check(!parser.closed(), "empty Feed does not close"))
    {
        return false;
    }

    std::vector<std::uint8_t> wire;
    AppendCapsule(wire, WtCapsuleParser::kCapsuleCloseWtSession, {});
    parser.Feed(wire.data(), wire.size());
    if (!Check(parser.closed(), "CLOSE still works after an empty Feed"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "CLOSE after an empty Feed is fully consumed");
}

bool TestCloseStickyAndRepeated()
{
    std::vector<std::uint8_t> close_wire;
    AppendCapsule(close_wire, WtCapsuleParser::kCapsuleCloseWtSession, {});

    WtCapsuleParser parser;
    parser.Feed(close_wire.data(), close_wire.size());
    if (!Check(parser.closed(), "first CLOSE closes"))
    {
        return false;
    }

    // A second CLOSE with a 4-byte error-code payload: consumed silently,
    // the flag stays set.
    std::vector<std::uint8_t> second_wire;
    AppendCapsule(second_wire, WtCapsuleParser::kCapsuleCloseWtSession, {0x00, 0x00, 0x00, 0x00});
    parser.Feed(second_wire.data(), second_wire.size());
    if (!Check(parser.closed(), "closed is sticky after a second CLOSE"))
    {
        return false;
    }
    return Check(parser.pending() == 0, "second CLOSE is fully consumed");
}

bool TestGarbagePrefix()
{
    // Stream bytes that are not capsule framing (an HTTP/1.1 status line).
    // The first bytes decode as varints demanding a payload that never
    // arrives, so everything stays buffered waiting for more bytes.
    static const std::uint8_t kGarbage[] = "HTTP/1.1 200\r\n\r\n";

    WtCapsuleParser parser;
    parser.Feed(kGarbage, sizeof kGarbage - 1);
    if (!Check(!parser.closed(), "garbage prefix does not close"))
    {
        return false;
    }
    if (!Check(parser.pending() == sizeof kGarbage - 1, "garbage prefix is retained unconsumed"))
    {
        return false;
    }
    // Still a functioning parser: a following real CLOSE is only consumed
    // if it completes the garbage's pending length - here it merely joins
    // the retained tail, proving the parser waits rather than desyncs.
    std::vector<std::uint8_t> close_wire;
    AppendCapsule(close_wire, WtCapsuleParser::kCapsuleCloseWtSession, {});
    parser.Feed(close_wire.data(), close_wire.size());
    if (!Check(!parser.closed(), "bytes after retained garbage do not spuriously close"))
    {
        return false;
    }
    return Check(parser.pending() == sizeof kGarbage - 1 + close_wire.size(), "garbage plus CLOSE stays pending");
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
        {.name = "varint-roundtrips", .fn = TestVarintRoundtrips},
        {.name = "varint-truncated", .fn = TestVarintTruncated},
        {.name = "close-alone", .fn = TestCloseAlone},
        {.name = "close-split", .fn = TestCloseSplitAcrossFeeds},
        {.name = "unknown-then-close", .fn = TestUnknownThenClose},
        {.name = "padding-then-close", .fn = TestPaddingThenClose},
        {.name = "truncated-tail", .fn = TestTruncatedTail},
        {.name = "empty-feed", .fn = TestEmptyFeed},
        {.name = "close-sticky", .fn = TestCloseStickyAndRepeated},
        {.name = "garbage-prefix", .fn = TestGarbagePrefix},
    };

    bool ok = true;
    for (const Section& section : sections)
    {
        ok = section.fn() && ok;
    }
    if (!ok)
    {
        std::fprintf(stderr, "capsule_test: FAILED\n");
        return 1;
    }
    std::printf("capsule_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
