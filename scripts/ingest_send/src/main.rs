//! ingest_send: publish a file's raw bytes over SRT to the WebSRT
//! gateway's UDP ingest, exactly once, in WebSRT-sized (6 x 188 = 1128
//! byte) TS chunks.
//!
//! Why this exists (scripts/interop.sh needs a byte-exact publisher with a
//! streamid, and neither stock tool provides one):
//!   - ffmpeg re-muxes even with `-c copy -f mpegts`, so what goes on the
//!     wire is NOT the fixture's bytes (verified: differs from byte ~5088);
//!   - WebSRT's mock_obs and the srt-rs fork's srt-transmit never set a
//!     streamid, and the ingest rejects streamid-less callers.
//!
//! Behavior: connect (caller mode) to <dst> with <streamid>, wait 2 s so
//! `websrt-dump --wait-stream` subscribers can attach (the gateway's
//! broadcast channel has no replay for receivers created later), then
//! pace the file over 10 s (fixture duration, like mock_obs) and close
//! cleanly. Exit 0.
//!
//! Usage: ingest_send <dst host:port> <streamid> <file>

use std::env;
use std::error::Error;
use std::fs;
use std::time::{Duration, Instant};

use bytes::Bytes;
use futures::SinkExt;
use srt_tokio::SrtSocket;

const TS_PACKET: usize = 188;
/// WebSRT's SRT payload size (6 x 188; crates/websrt/src/srt_sender.rs).
const PAYLOAD_BYTES: usize = 1128;
const GRACE: Duration = Duration::from_secs(2);
const STREAM_SECONDS: f64 = 10.0;

fn usage() -> String {
    "usage: ingest_send <dst host:port> <streamid> <file>".to_string()
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let mut args = env::args().skip(1);
    let dst = args.next().ok_or_else(usage)?;
    let streamid = args.next().ok_or_else(usage)?;
    let path = args.next().ok_or_else(usage)?;

    let data = fs::read(&path)?;
    if data.is_empty() || data.len() % TS_PACKET != 0 {
        return Err(format!("{}: not a 188-aligned TS file", path).into());
    }
    let chunk = PAYLOAD_BYTES - PAYLOAD_BYTES % TS_PACKET;

    let dst: srt_protocol::options::SocketAddress = dst.as_str().try_into()?;
    let mut socket = SrtSocket::builder().call(dst, Some(&streamid)).await?;
    println!("connected (streamid '{streamid}')");

    // Grace period: the gateway delivers a broadcast message only to
    // receivers that already exist, so data sent now would be
    // unrecoverable for a subscriber still in its --wait-stream retry.
    tokio::time::sleep(GRACE).await;

    let bytes_per_sec = data.len() as f64 / STREAM_SECONDS;
    let start = Instant::now();
    let mut emitted = 0usize;
    let mut cursor = 0usize;
    while cursor + chunk <= data.len() {
        let target = emitted as f64 / bytes_per_sec;
        let actual = start.elapsed().as_secs_f64();
        if target > actual {
            tokio::time::sleep(Duration::from_secs_f64(target - actual)).await;
        }
        let msg = (
            Instant::now(),
            Bytes::copy_from_slice(&data[cursor..cursor + chunk]),
        );
        socket.send(msg).await?;
        cursor += chunk;
        emitted += chunk;
    }
    let tail = &data[cursor..];
    if !tail.is_empty() {
        socket
            .send((Instant::now(), Bytes::copy_from_slice(tail)))
            .await?;
        emitted += tail.len();
    }
    socket.close_and_finish().await?;
    println!("sent {emitted} bytes of {} once; closed", data.len());
    Ok(())
}
