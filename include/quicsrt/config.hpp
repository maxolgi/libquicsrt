// Session configuration (libquicsrt public API).
//
// SubscriberConfig and PublisherConfig describe one subscribe / publish
// session: which gateway to talk to, which stream, and the SRT/WT
// parameters the session negotiates. Fields are plain values; validate()
// checks every bound before the session starts, the way QuicSRT's Rust
// SrtConfig::validate does (descriptive message per violated constraint,
// first failure wins).

#ifndef QUICSRT_CONFIG_HPP
#define QUICSRT_CONFIG_HPP

#include <cstdint>
#include <string>

namespace quicsrt
{

struct SubscriberConfig
{
    // Gateway host: an IPv4 address literal (e.g. "127.0.0.1") or a DNS
    // name (e.g. "quicsrt.vyidd.com"). Names are resolved at Start() on
    // the calling thread - blocking, on the OS resolver's default
    // timeout budget - via the srt fork's own resolver; the name as
    // typed (never the resolved IP) is used for TLS SNI and the PKI
    // certificate name check. IPv4 results only for now.
    std::string host;

    // Gateway UDP port (WebTransport). Default: the QuicSRT gateway port.
    std::uint16_t port = 443;

    // Stream name to view; becomes the /wt?stream=<name> query. May only
    // contain the unreserved characters A-Z a-z 0-9 . _ ~ - (it is placed
    // verbatim into the request path).
    std::string stream_name;

    // Expected SHA-256 of the gateway certificate's DER encoding, as 64
    // hex characters (upper or lower case). Empty string means normal PKI
    // validation of the gateway certificate instead of pinning - the mode
    // a browser uses when the gateway serves a CA-signed certificate.
    std::string cert_sha256_hex;

    // TSBPD (timestamp-based packet delivery) latency in milliseconds.
    // The effective delivery delay is max(sender, receiver) latency, so
    // this is a floor. Default: 120 (the QuicSRT gateway ingest default).
    int tsbpd_latency_ms = 120;

    // SRT payload size in bytes. Must match the stream's TS framing: the
    // QuicSRT constant is 1128 (6 x 188-byte MPEG-TS packets), and the
    // handshake negotiates min(gateway, client), so the default is right
    // for QuicSRT gateways. Default: 1128.
    int payload_size = 1128;

    // Initial RTT estimate in milliseconds seeding SRT's retransmission
    // timers before the first live sample: 0 = derive it automatically
    // from the QUIC connection's smoothed RTT once measured; otherwise it
    // must fit the option's 1..30000 range and overrides the automatic
    // seed.
    int initial_rtt_ms = 0;

    // How long Start() may wait for the WebTransport session AND the SRT
    // handshake to complete before giving up (the session is torn down).
    // Default: 10000.
    int connect_timeout_ms = 10000;

    // Returns the empty string when every field is within its bounds;
    // otherwise a human-readable description of the first violated
    // constraint (e.g. "port must be in 1..65535, got 0").
    std::string validate() const;
};

struct PublisherConfig
{
    // Gateway host: an IPv4 address literal or a DNS name, resolved at
    // Start() (see SubscriberConfig::host).
    std::string host;

    // Gateway UDP port (WebTransport). Default: the QuicSRT gateway port.
    std::uint16_t port = 443;

    // Stream name to publish; becomes the /wt?publish=<name> query. May
    // only contain the unreserved characters A-Z a-z 0-9 . _ ~ - (it is
    // placed verbatim into the request path). Occupancy is first-wins:
    // the gateway answers HTTP 403 while a live source (WT publisher or
    // UDP ingest) holds the name, and Start surfaces kPublishRejected.
    std::string stream_name;

    // Expected SHA-256 of the gateway certificate's DER encoding, as 64
    // hex characters (upper or lower case). Empty string means normal PKI
    // validation of the gateway certificate instead of pinning.
    std::string cert_sha256_hex;

    // TSBPD (timestamp-based packet delivery) latency in milliseconds.
    // The effective delivery delay is max(sender, receiver) latency, so
    // this is a floor the receiver may raise. Default: 120 (the browser
    // publisher's srt-wasm default).
    int tsbpd_latency_ms = 120;

    // SRT payload size in bytes. The handshake negotiates
    // min(gateway, client) and the Write chunker frames to the negotiated
    // size, so this only caps it. The QuicSRT constant is 1128
    // (6 x 188-byte MPEG-TS packets). Default: 1128.
    int payload_size = 1128;

    // Initial RTT estimate in milliseconds seeding SRT's retransmission
    // timers before the first live sample: 0 = derive it automatically
    // from the QUIC connection's smoothed RTT once measured; otherwise it
    // must fit the option's 1..30000 range and overrides the seed.
    int initial_rtt_ms = 0;

    // How long Start() may wait for the WebTransport session AND the SRT
    // handshake to complete before giving up (the session is torn down).
    // Default: 10000.
    int connect_timeout_ms = 10000;

    // Returns the empty string when every field is within its bounds;
    // otherwise a human-readable description of the first violated
    // constraint. The bounds match SubscriberConfig's.
    std::string validate() const;
};

} // namespace quicsrt

#endif // QUICSRT_CONFIG_HPP
