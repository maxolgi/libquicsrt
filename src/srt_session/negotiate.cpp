#include "srt_session/negotiate.h"

#include <algorithm>

#include "proto/constants.h"

namespace websrt
{

int NegotiatePayload(SRTSOCKET accepted, int configured)
{
    int payload = configured;
    int optlen  = static_cast<int>(sizeof payload);
    if (srt_getsockopt(accepted, 0, SRTO_PAYLOADSIZE, &payload, &optlen) == SRT_ERROR || payload < kTsPacketSize)
    {
        payload = configured;
    }
    int mss = 1500;
    optlen  = static_cast<int>(sizeof mss);
    if (srt_getsockopt(accepted, 0, SRTO_MSS, &mss, &optlen) != SRT_ERROR)
    {
        const int cap = mss - (28 + 16);
        if (cap >= kTsPacketSize)
        {
            payload = std::min(payload, cap / kTsPacketSize * kTsPacketSize);
        }
    }
    return payload;
}

} // namespace websrt
