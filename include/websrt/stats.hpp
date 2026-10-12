// Session statistics (libwebsrt public API).
//
// Stable, documented subsets of the underlying SRT socket statistics
// (srt_bstats on the accepted data socket, lifetime totals, no clearing).
// Byte counters follow SRT's convention: they include the SRT (16) +
// UDP (8) + IP (20) headers of every counted packet, so a 1128-byte
// payload counts as 1172 bytes. Rate and window fields are the
// instantaneous values at the moment of the snapshot.

#ifndef WEBSRT_STATS_HPP
#define WEBSRT_STATS_HPP

#include <cstdint>

namespace websrt
{

// Outcome of Subscriber::Stats() / Publisher::Stats().
enum StatsStatus
{
    kStatsOk = 0,       // out filled
    kStatsNotConnected, // no live SRT connection (not started / ended); out zeroed
};

struct SubscriberStats
{
    // Milliseconds since the SRT connection was established.
    std::int64_t ms_elapsed = 0;

    // Lifetime totals.
    std::int64_t  packets_received      = 0; // unique data packets delivered by the app
    std::uint64_t bytes_received        = 0; // payload bytes incl. per-packet headers (see above)
    std::int64_t  packets_sent          = 0; // unique data packets sent (control excluded; ~0 for a viewer)
    std::uint64_t bytes_sent            = 0;
    std::int64_t  packets_lost          = 0; // receiver-side detected loss, retransmit-recovered or not
    std::int64_t  packets_retransmitted = 0; // sender-side retransmissions (the gateway's)
    std::int64_t  packets_dropped       = 0; // too-late-to-play drops (live-stream holes)
    std::int64_t  packets_belated       = 0; // received but ignored (arrived past the ACK)
    std::int64_t  acks_sent             = 0;
    std::int64_t  acks_received         = 0;
    std::int64_t  naks_sent             = 0; // loss reports this receiver issued
    std::int64_t  naks_received         = 0;

    // Instantaneous values at snapshot time.
    double       rtt_ms            = 0.0; // smoothed RTT
    double       recv_rate_mbps    = 0.0;
    double       send_rate_mbps    = 0.0;
    double       bandwidth_mbps    = 0.0; // estimated path bandwidth
    std::int32_t flow_window       = 0;   // flow-control window, packets
    std::int32_t congestion_window = 0;   // congestion window, packets
    std::int32_t flight_size       = 0;   // packets in flight
    std::int32_t packets_buffered  = 0;   // received, awaiting TSBPD release
    std::int32_t bytes_buffered    = 0;
    std::int32_t ms_buffered       = 0; // TSBPD timespan currently held
    std::int32_t ms_tsbpd_delay    = 0; // negotiated delivery delay in effect
};

// The sender-side view of the same srt_bstats snapshot: what a Publisher
// (and the FFmpeg websrt:// output) needs to judge a stream's health.
struct PublisherStats
{
    // Milliseconds since the SRT connection was established.
    std::int64_t ms_elapsed = 0;

    // Lifetime totals.
    std::int64_t  packets_sent          = 0; // unique data packets sent
    std::uint64_t bytes_sent            = 0; // payload bytes incl. per-packet headers (see above)
    std::int64_t  packets_retransmitted = 0; // our retransmissions
    std::int64_t  packets_received      = 0; // ~0 for a sender (control packets are not counted)
    std::uint64_t bytes_received        = 0;
    std::int64_t  acks_received         = 0;
    std::int64_t  naks_received         = 0; // loss reports from the gateway; each drove retransmits

    // Instantaneous values at snapshot time.
    double       rtt_ms            = 0.0; // smoothed RTT
    double       send_rate_mbps    = 0.0;
    double       bandwidth_mbps    = 0.0; // estimated path bandwidth
    std::int32_t flow_window       = 0;   // flow-control window, packets
    std::int32_t congestion_window = 0;   // congestion window, packets
    std::int32_t flight_size       = 0;   // packets in flight
    std::int32_t packets_buffered  = 0;   // sent, not yet ACKed (End waits for 0)
    std::int32_t bytes_buffered    = 0;
    std::int32_t ms_tsbpd_delay    = 0; // negotiated delivery delay in effect
};

} // namespace websrt

#endif // WEBSRT_STATS_HPP
