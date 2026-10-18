// quicsrt_c implementation: a pure, exception-free wrapper over the C++
// façades. Handles own their object; status codes map one-to-one from
// the C++ enums; stats format to one key=value line (the same fields
// quicsrt-dump prints). The event loop and threading contracts are the
// façades' - this layer adds none of its own.

#include "quicsrt/quicsrt_c.h"

#include <cstdint>
#include <cstdio>
#include <new>
#include <string>

#include "quicsrt/publisher.hpp"
#include "quicsrt/subscriber.hpp"
#include "quicsrt/version.hpp"

// Handles are wrappers around the C++ objects. last_error is kept in a
// std::string the caller reads through a const char* that stays valid
// until the next call on the handle (the façades return by value).
struct quicsrt_publisher
{
    std::unique_ptr<quicsrt::Publisher> obj;
    std::string                        error;

    explicit quicsrt_publisher(std::unique_ptr<quicsrt::Publisher> o) : obj(std::move(o)) {}
};

struct quicsrt_subscriber
{
    std::unique_ptr<quicsrt::Subscriber> obj;
    std::string                         error;

    explicit quicsrt_subscriber(std::unique_ptr<quicsrt::Subscriber> o) : obj(std::move(o)) {}
};

namespace
{

// Start-parameter defaults (mirrors config.hpp; <= 0 selects these).
constexpr std::uint16_t kDefaultPort          = 443;
constexpr int           kDefaultLatencyMs     = 120;
constexpr int           kDefaultConnectMs     = 10000;

// MapStart's case labels are the façade enums' integer VALUES, not their
// names: a reorder of either enum would silently mistranslate. Pin every
// value the mapping assumes (both façade enums share the 0..7 ordering).
static_assert(quicsrt::kPublishOk == 0 && quicsrt::kStartOk == 0, "MapStart case 0 -> QUICSRT_OK");
static_assert(quicsrt::kPublishInvalidConfig == 1 && quicsrt::kStartInvalidConfig == 1,
              "MapStart case 1 -> QUICSRT_ECONFIG");
static_assert(quicsrt::kPublishAlreadyStarted == 2 && quicsrt::kStartAlreadyStarted == 2,
              "MapStart case 2 -> QUICSRT_ESTATE");
static_assert(quicsrt::kPublishInvalidTarget == 3 && quicsrt::kStartInvalidTarget == 3,
              "MapStart case 3 -> QUICSRT_ETARGET");
static_assert(quicsrt::kPublishRejected == 4 && quicsrt::kStartRejected == 4, "MapStart case 4 -> QUICSRT_EREJECTED");
static_assert(quicsrt::kPublishConnectFailed == 5 && quicsrt::kStartConnectFailed == 5,
              "MapStart case 5 -> QUICSRT_ECONNECT");
static_assert(quicsrt::kPublishTimedOut == 6 && quicsrt::kStartTimedOut == 6, "MapStart case 6 -> QUICSRT_ETIMEOUT");
static_assert(quicsrt::kPublishInternal == 7 && quicsrt::kStartInternal == 7,
              "MapStart default -> QUICSRT_EINTERNAL (kXInternal is 7)");

quicsrt_status_t MapStart(int status)
{
    switch (status)
    {
    case 0:  return QUICSRT_OK;        // kPublishOk / kStartOk
    case 1:  return QUICSRT_ECONFIG;   // kXInvalidConfig
    case 2:  return QUICSRT_ESTATE;    // kXAlreadyStarted
    case 3:  return QUICSRT_ETARGET;   // kXInvalidTarget
    case 4:  return QUICSRT_EREJECTED; // kXRejected
    case 5:  return QUICSRT_ECONNECT;  // kXConnectFailed
    case 6:  return QUICSRT_ETIMEOUT;  // kXTimedOut
    default: return QUICSRT_EINTERNAL; // kXInternal
    }
}

quicsrt_status_t FormatPublisherStats(quicsrt_publisher_t* handle, char* buf, std::size_t buflen)
{
    quicsrt::PublisherStats s;
    if (handle->obj->Stats(&s) != quicsrt::kStatsOk)
    {
        buf[0] = '\0';
        return QUICSRT_ESTATE;
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
    return QUICSRT_OK;
}

quicsrt_status_t FormatSubscriberStats(quicsrt_subscriber_t* handle, char* buf, std::size_t buflen)
{
    quicsrt::SubscriberStats s;
    if (handle->obj->Stats(&s) != quicsrt::kStatsOk)
    {
        buf[0] = '\0';
        return QUICSRT_ESTATE;
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
    return QUICSRT_OK;
}

} // namespace

const char* quicsrt_version(void)
{
    return quicsrt::version_string();
}

quicsrt_publisher_t* quicsrt_publisher_create(void)
{
    try
    {
        return new quicsrt_publisher_t(std::make_unique<quicsrt::Publisher>());
    }
    catch (...)
    {
        return nullptr;
    }
}

void quicsrt_publisher_destroy(quicsrt_publisher_t* pub)
{
    delete pub; // ~Publisher stops a live session
}

quicsrt_status_t quicsrt_publisher_start(
    quicsrt_publisher_t* pub, const char* host, int port, const char* stream,
    const char* cert_hash_hex, int latency_ms, int timeout_ms)
{
    if (pub == nullptr || pub->obj == nullptr)
    {
        return QUICSRT_EINVAL;
    }
    if (host == nullptr || stream == nullptr)
    {
        pub->error = "host and stream must be non-NULL";
        return QUICSRT_EINVAL;
    }
    try
    {
        quicsrt::PublisherConfig cfg;
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
        return QUICSRT_ENOMEM;
    }
    catch (...)
    {
        pub->error = "unknown failure";
        return QUICSRT_EINTERNAL;
    }
}

int quicsrt_publisher_write(quicsrt_publisher_t* pub, const void* data, size_t len, int timeout_ms)
{
    if (pub == nullptr || pub->obj == nullptr)
    {
        return QUICSRT_WRITE_INVALID;
    }
    return pub->obj->Write(data, len, timeout_ms);
}

quicsrt_status_t quicsrt_publisher_end(quicsrt_publisher_t* pub, int timeout_ms)
{
    if (pub == nullptr || pub->obj == nullptr)
    {
        return QUICSRT_EINVAL;
    }
    if (!pub->obj->End(timeout_ms))
    {
        pub->error = pub->obj->LastError();
        return pub->error.empty() ? QUICSRT_ETIMEOUT : QUICSRT_EINTERNAL;
    }
    pub->error.clear();
    return QUICSRT_OK;
}

void quicsrt_publisher_stop(quicsrt_publisher_t* pub)
{
    if (pub != nullptr && pub->obj != nullptr)
    {
        pub->obj->Stop();
    }
}

quicsrt_status_t quicsrt_publisher_stats(quicsrt_publisher_t* pub, char* buf, size_t buflen)
{
    if (pub == nullptr || pub->obj == nullptr || buf == nullptr || buflen == 0)
    {
        return QUICSRT_EINVAL;
    }
    return FormatPublisherStats(pub, buf, buflen);
}

const char* quicsrt_publisher_last_error(quicsrt_publisher_t* pub)
{
    if (pub == nullptr)
    {
        return "";
    }
    pub->error = pub->obj->LastError();
    return pub->error.c_str();
}

quicsrt_subscriber_t* quicsrt_subscriber_create(void)
{
    try
    {
        return new quicsrt_subscriber_t(std::make_unique<quicsrt::Subscriber>());
    }
    catch (...)
    {
        return nullptr;
    }
}

void quicsrt_subscriber_destroy(quicsrt_subscriber_t* sub)
{
    delete sub;
}

quicsrt_status_t quicsrt_subscriber_start(
    quicsrt_subscriber_t* sub, const char* host, int port, const char* stream,
    const char* cert_hash_hex, int latency_ms, int timeout_ms)
{
    if (sub == nullptr || sub->obj == nullptr)
    {
        return QUICSRT_EINVAL;
    }
    if (host == nullptr || stream == nullptr)
    {
        sub->error = "host and stream must be non-NULL";
        return QUICSRT_EINVAL;
    }
    try
    {
        quicsrt::SubscriberConfig cfg;
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
        return QUICSRT_ENOMEM;
    }
    catch (...)
    {
        sub->error = "unknown failure";
        return QUICSRT_EINTERNAL;
    }
}

int quicsrt_subscriber_read(quicsrt_subscriber_t* sub, void* buf, size_t len, int timeout_ms)
{
    if (sub == nullptr || sub->obj == nullptr)
    {
        return QUICSRT_READ_INVALID;
    }
    return sub->obj->Read(buf, len, timeout_ms);
}

quicsrt_status_t quicsrt_subscriber_stats(quicsrt_subscriber_t* sub, char* buf, size_t buflen)
{
    if (sub == nullptr || sub->obj == nullptr || buf == nullptr || buflen == 0)
    {
        return QUICSRT_EINVAL;
    }
    return FormatSubscriberStats(sub, buf, buflen);
}

void quicsrt_subscriber_stop(quicsrt_subscriber_t* sub)
{
    if (sub != nullptr && sub->obj != nullptr)
    {
        sub->obj->Stop();
    }
}

const char* quicsrt_subscriber_last_error(quicsrt_subscriber_t* sub)
{
    if (sub == nullptr)
    {
        return "";
    }
    sub->error = sub->obj->LastError();
    return sub->error.c_str();
}
