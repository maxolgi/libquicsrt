// websrt_c implementation: a pure, exception-free wrapper over the C++
// façades. Handles own their object; status codes map one-to-one from
// the C++ enums; stats format to one key=value line (the same fields
// websrt-dump prints). The event loop and threading contracts are the
// façades' - this layer adds none of its own.

#include "websrt/websrt_c.h"

#include <cstdint>
#include <cstdio>
#include <new>
#include <string>

#include "websrt/publisher.hpp"
#include "websrt/subscriber.hpp"
#include "websrt/version.hpp"

// Handles are wrappers around the C++ objects. last_error is kept in a
// std::string the caller reads through a const char* that stays valid
// until the next call on the handle (the façades return by value).
struct websrt_publisher
{
    std::unique_ptr<websrt::Publisher> obj;
    std::string                        error;

    explicit websrt_publisher(std::unique_ptr<websrt::Publisher> o) : obj(std::move(o)) {}
};

struct websrt_subscriber
{
    std::unique_ptr<websrt::Subscriber> obj;
    std::string                         error;

    explicit websrt_subscriber(std::unique_ptr<websrt::Subscriber> o) : obj(std::move(o)) {}
};

namespace
{

// Start-parameter defaults (mirrors config.hpp; <= 0 selects these).
constexpr std::uint16_t kDefaultPort          = 443;
constexpr int           kDefaultLatencyMs     = 120;
constexpr int           kDefaultConnectMs     = 10000;

websrt_status_t MapStart(int status)
{
    switch (status)
    {
    case 0:  return WEBSRT_OK;        // kPublishOk / kStartOk
    case 1:  return WEBSRT_ECONFIG;   // kXInvalidConfig
    case 2:  return WEBSRT_ESTATE;    // kXAlreadyStarted
    case 3:  return WEBSRT_ETARGET;   // kXInvalidTarget
    case 4:  return WEBSRT_EREJECTED; // kXRejected
    case 5:  return WEBSRT_ECONNECT;  // kXConnectFailed
    case 6:  return WEBSRT_ETIMEOUT;  // kXTimedOut
    default: return WEBSRT_EINTERNAL; // kXInternal
    }
}

websrt_status_t FormatPublisherStats(websrt_publisher_t* handle, char* buf, std::size_t buflen)
{
    websrt::PublisherStats s;
    if (handle->obj->Stats(&s) != websrt::kStatsOk)
    {
        buf[0] = '\0';
        return WEBSRT_ESTATE;
    }
    std::snprintf(buf,
                  buflen,
                  "ms=%lld pkts_sent=%lld bytes_sent=%llu rtx=%lld naks_recv=%lld"
                  " acks_recv=%lld rtt=%.1f send_rate=%.2f bandwidth=%.2f"
                  " flight=%d buffered=%d tsbpd_ms=%d",
                  static_cast<long long>(s.ms_elapsed),
                  static_cast<long long>(s.packets_sent),
                  static_cast<unsigned long long>(s.bytes_sent),
                  static_cast<long long>(s.packets_retransmitted),
                  static_cast<long long>(s.naks_received),
                  static_cast<long long>(s.acks_received),
                  s.rtt_ms,
                  s.send_rate_mbps,
                  s.bandwidth_mbps,
                  s.flight_size,
                  s.packets_buffered,
                  s.ms_tsbpd_delay);
    return WEBSRT_OK;
}

websrt_status_t FormatSubscriberStats(websrt_subscriber_t* handle, char* buf, std::size_t buflen)
{
    websrt::SubscriberStats s;
    if (handle->obj->Stats(&s) != websrt::kStatsOk)
    {
        buf[0] = '\0';
        return WEBSRT_ESTATE;
    }
    std::snprintf(buf,
                  buflen,
                  "ms=%lld pkts_recv=%lld bytes_recv=%llu lost=%lld rtx=%lld"
                  " dropped=%lld belated=%lld naks_sent=%lld rtt=%.1f recv_rate=%.2f"
                  " buffered=%d buffered_ms=%d tsbpd_ms=%d",
                  static_cast<long long>(s.ms_elapsed),
                  static_cast<long long>(s.packets_received),
                  static_cast<unsigned long long>(s.bytes_received),
                  static_cast<long long>(s.packets_lost),
                  static_cast<long long>(s.packets_retransmitted),
                  static_cast<long long>(s.packets_dropped),
                  static_cast<long long>(s.packets_belated),
                  static_cast<long long>(s.naks_sent),
                  s.rtt_ms,
                  s.recv_rate_mbps,
                  s.packets_buffered,
                  s.ms_buffered,
                  s.ms_tsbpd_delay);
    return WEBSRT_OK;
}

} // namespace

const char* websrt_version(void)
{
    return websrt::version_string();
}

websrt_publisher_t* websrt_publisher_create(void)
{
    try
    {
        return new websrt_publisher_t(std::make_unique<websrt::Publisher>());
    }
    catch (...)
    {
        return nullptr;
    }
}

void websrt_publisher_destroy(websrt_publisher_t* pub)
{
    delete pub; // ~Publisher stops a live session
}

websrt_status_t websrt_publisher_start(
    websrt_publisher_t* pub, const char* host, int port, const char* stream,
    const char* cert_hash_hex, int latency_ms, int timeout_ms)
{
    if (pub == nullptr || pub->obj == nullptr)
    {
        return WEBSRT_EINVAL;
    }
    if (host == nullptr || stream == nullptr)
    {
        pub->error = "host and stream must be non-NULL";
        return WEBSRT_EINVAL;
    }
    try
    {
        websrt::PublisherConfig cfg;
        cfg.host            = host;
        cfg.port            = port > 0 && port <= 65535 ? static_cast<std::uint16_t>(port) : kDefaultPort;
        cfg.stream_name     = stream;
        cfg.cert_sha256_hex = cert_hash_hex != nullptr ? cert_hash_hex : "";
        cfg.tsbpd_latency_ms   = latency_ms > 0 ? latency_ms : kDefaultLatencyMs;
        cfg.connect_timeout_ms = timeout_ms > 0 ? timeout_ms : kDefaultConnectMs;
        const int status = pub->obj->Start(cfg);
        pub->error       = pub->obj->LastError();
        return MapStart(status);
    }
    catch (const std::bad_alloc&)
    {
        pub->error = "out of memory";
        return WEBSRT_ENOMEM;
    }
    catch (...)
    {
        pub->error = "unknown failure";
        return WEBSRT_EINTERNAL;
    }
}

int websrt_publisher_write(websrt_publisher_t* pub, const void* data, size_t len, int timeout_ms)
{
    if (pub == nullptr || pub->obj == nullptr)
    {
        return WEBSRT_WRITE_INVALID;
    }
    return pub->obj->Write(data, len, timeout_ms);
}

websrt_status_t websrt_publisher_end(websrt_publisher_t* pub, int timeout_ms)
{
    if (pub == nullptr || pub->obj == nullptr)
    {
        return WEBSRT_EINVAL;
    }
    if (!pub->obj->End(timeout_ms))
    {
        pub->error = pub->obj->LastError();
        return pub->error.empty() ? WEBSRT_ETIMEOUT : WEBSRT_EINTERNAL;
    }
    pub->error.clear();
    return WEBSRT_OK;
}

void websrt_publisher_stop(websrt_publisher_t* pub)
{
    if (pub != nullptr && pub->obj != nullptr)
    {
        pub->obj->Stop();
    }
}

websrt_status_t websrt_publisher_stats(websrt_publisher_t* pub, char* buf, size_t buflen)
{
    if (pub == nullptr || pub->obj == nullptr || buf == nullptr || buflen == 0)
    {
        return WEBSRT_EINVAL;
    }
    return FormatPublisherStats(pub, buf, buflen);
}

const char* websrt_publisher_last_error(websrt_publisher_t* pub)
{
    if (pub == nullptr)
    {
        return "";
    }
    pub->error = pub->obj->LastError();
    return pub->error.c_str();
}

websrt_subscriber_t* websrt_subscriber_create(void)
{
    try
    {
        return new websrt_subscriber_t(std::make_unique<websrt::Subscriber>());
    }
    catch (...)
    {
        return nullptr;
    }
}

void websrt_subscriber_destroy(websrt_subscriber_t* sub)
{
    delete sub;
}

websrt_status_t websrt_subscriber_start(
    websrt_subscriber_t* sub, const char* host, int port, const char* stream,
    const char* cert_hash_hex, int latency_ms, int timeout_ms)
{
    if (sub == nullptr || sub->obj == nullptr)
    {
        return WEBSRT_EINVAL;
    }
    if (host == nullptr || stream == nullptr)
    {
        sub->error = "host and stream must be non-NULL";
        return WEBSRT_EINVAL;
    }
    try
    {
        websrt::SubscriberConfig cfg;
        cfg.host            = host;
        cfg.port            = port > 0 && port <= 65535 ? static_cast<std::uint16_t>(port) : kDefaultPort;
        cfg.stream_name     = stream;
        cfg.cert_sha256_hex = cert_hash_hex != nullptr ? cert_hash_hex : "";
        cfg.tsbpd_latency_ms   = latency_ms > 0 ? latency_ms : kDefaultLatencyMs;
        cfg.connect_timeout_ms = timeout_ms > 0 ? timeout_ms : kDefaultConnectMs;
        const int status = sub->obj->Start(cfg);
        sub->error       = sub->obj->LastError();
        return MapStart(status);
    }
    catch (const std::bad_alloc&)
    {
        sub->error = "out of memory";
        return WEBSRT_ENOMEM;
    }
    catch (...)
    {
        sub->error = "unknown failure";
        return WEBSRT_EINTERNAL;
    }
}

int websrt_subscriber_read(websrt_subscriber_t* sub, void* buf, size_t len, int timeout_ms)
{
    if (sub == nullptr || sub->obj == nullptr)
    {
        return WEBSRT_READ_INVALID;
    }
    return sub->obj->Read(buf, len, timeout_ms);
}

websrt_status_t websrt_subscriber_stats(websrt_subscriber_t* sub, char* buf, size_t buflen)
{
    if (sub == nullptr || sub->obj == nullptr || buf == nullptr || buflen == 0)
    {
        return WEBSRT_EINVAL;
    }
    return FormatSubscriberStats(sub, buf, buflen);
}

void websrt_subscriber_stop(websrt_subscriber_t* sub)
{
    if (sub != nullptr && sub->obj != nullptr)
    {
        sub->obj->Stop();
    }
}

const char* websrt_subscriber_last_error(websrt_subscriber_t* sub)
{
    if (sub == nullptr)
    {
        return "";
    }
    sub->error = sub->obj->LastError();
    return sub->error.c_str();
}
