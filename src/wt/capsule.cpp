#include "wt/capsule.h"

namespace websrt
{

// The bodies are moved semantics-identical from src/wt/client.cpp (whose
// anonymous-namespace originals keep internal linkage there).

std::size_t VarintPut(std::uint8_t* dst, std::uint64_t v)
{
    if (v < 0x40)
    {
        dst[0] = static_cast<std::uint8_t>(v);
        return 1;
    }
    if (v < 0x4000)
    {
        dst[0] = static_cast<std::uint8_t>(0x40 | v >> 8);
        dst[1] = static_cast<std::uint8_t>(v);
        return 2;
    }
    if (v < 0x40000000)
    {
        dst[0] = static_cast<std::uint8_t>(0x80 | v >> 24);
        dst[1] = static_cast<std::uint8_t>(v >> 16);
        dst[2] = static_cast<std::uint8_t>(v >> 8);
        dst[3] = static_cast<std::uint8_t>(v);
        return 4;
    }
    dst[0] = static_cast<std::uint8_t>(0xc0 | v >> 56);
    dst[1] = static_cast<std::uint8_t>(v >> 48);
    dst[2] = static_cast<std::uint8_t>(v >> 40);
    dst[3] = static_cast<std::uint8_t>(v >> 32);
    dst[4] = static_cast<std::uint8_t>(v >> 24);
    dst[5] = static_cast<std::uint8_t>(v >> 16);
    dst[6] = static_cast<std::uint8_t>(v >> 8);
    dst[7] = static_cast<std::uint8_t>(v);
    return 8;
}

std::optional<std::pair<std::uint64_t, std::size_t>> VarintGet(const std::uint8_t* p, std::size_t n)
{
    if (n < 1)
    {
        return std::nullopt;
    }
    const std::uint8_t first = p[0];
    const std::size_t  len   = 1U << (first >> 6);
    if (n < len)
    {
        return std::nullopt;
    }
    std::uint64_t v = first & 0x3f;
    for (std::size_t i = 1; i < len; ++i)
    {
        v = v << 8 | p[i];
    }
    return std::make_pair(v, len);
}

void WtCapsuleParser::Feed(const std::uint8_t* data, std::size_t size)
{
    buf_.insert(buf_.end(), data, data + size);
    std::size_t off = 0;
    for (;;)
    {
        const auto type = VarintGet(buf_.data() + off, buf_.size() - off);
        if (!type)
        {
            break;
        }
        const auto length = VarintGet(buf_.data() + off + type->second, buf_.size() - off - type->second);
        if (!length)
        {
            break;
        }
        if (buf_.size() - off - type->second - length->second < length->first)
        {
            break;
        }
        if (type->first == kCapsuleCloseWtSession)
        {
            closed_ = true;
        }
        off += type->second + length->second + length->first;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off));
}

bool WtCapsuleParser::closed() const
{
    return closed_;
}

std::size_t WtCapsuleParser::pending() const
{
    return buf_.size();
}

} // namespace websrt
