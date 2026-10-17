#include "wt/connect_response.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace websrt
{

std::size_t ConnectResponseParser::Feed(const std::uint8_t* data, std::size_t size)
{
    if (done_ || overflow_)
    {
        return 0; // sticky: drop instead of buffer
    }
    buf_.insert(buf_.end(), data, data + size);
    if (buf_.size() > kMaxHeaderBytes)
    {
        overflow_ = true;
        buf_.clear();
        return 0;
    }
    constexpr std::string_view kTerm = "\r\n\r\n";
    const auto                 found = std::ranges::search(buf_, kTerm);
    if (found.empty())
    {
        return 0;
    }
    const std::size_t head_len   = static_cast<std::size_t>(found.begin() - buf_.begin()) + 4;
    const std::size_t from_input = head_len - (buf_.size() - size); // this input's share of the head
    ParseStatusLine();
    done_ = true;
    buf_.clear();
    return from_input;
}

void ConnectResponseParser::ParseStatusLine()
{
    const auto        line_end = std::ranges::find(buf_, '\r');
    char              line[32] = {};
    const std::size_t line_len =
        std::min<std::size_t>(static_cast<std::size_t>(line_end - buf_.begin()), sizeof line - 1);
    std::memcpy(line, buf_.data(), line_len);
    int  status      = -1;
    char version[16] = {};
    if (2 == std::sscanf(line, "%15s %d", version, &status))
    {
        status_ = status;
    }
    else
    {
        status_ = -1;
    }
    status_line_.assign(line, line + line_len);
}

bool ConnectResponseParser::done() const
{
    return done_;
}

bool ConnectResponseParser::overflow() const
{
    return overflow_;
}

std::size_t ConnectResponseParser::pending() const
{
    return buf_.size();
}

int ConnectResponseParser::status() const
{
    return status_;
}

const std::string& ConnectResponseParser::status_line() const
{
    return status_line_;
}

} // namespace websrt
