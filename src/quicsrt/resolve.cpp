#include "quicsrt/resolve.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include "apputil.hpp" // the srt fork's resolver (CreateAddr)

namespace quicsrt
{

std::string ResolveHostToIpv4(const std::string& host, std::string* error)
{
    // CreateAddr is srt's own address helper: inet_pton fast path for
    // literals, then getaddrinfo. AF_INET keeps the client on the IPv4 UDP
    // path it has always dialed (IPv6 results are not requested; the WT
    // socket is AF_INET). On failure it reports nothing - the empty
    // (AF_UNSPEC) result - so the error text is ours.
    const srt::sockaddr_any addr                    = CreateAddr(host, 0, AF_INET);
    char                    dotted[INET_ADDRSTRLEN] = {};
    if (addr.family() != AF_INET || inet_ntop(AF_INET, addr.get_addr(), dotted, sizeof dotted) == nullptr)
    {
        if (error != nullptr)
        {
            *error = "failed to resolve host \"" + host + "\" to an IPv4 address";
        }
        return {};
    }
    return dotted;
}

} // namespace quicsrt
