// The HTTP-status seam shared by the Subscriber and Publisher Start
// paths: which CONNECT responses are the gateway REFUSING the session
// versus the gateway failing. 403 = the stream name is held by a live
// source (first-wins), 404 = no such stream, 429 = session/viewer caps.
// Every other status - 5xx, redirects, and the unparseable -1 - is a
// transport-level failure, not a refusal: callers keying off the typed
// status (kXRejected / WEBSRT_EREJECTED) must be able to tell a wrong
// stream name from a gateway on fire.

#ifndef WEBSRT_START_STATUS_H
#define WEBSRT_START_STATUS_H

namespace websrt
{

constexpr bool HttpStatusIsRejected(int status)
{
    return status == 403 || status == 404 || status == 429;
}

} // namespace websrt

#endif // WEBSRT_START_STATUS_H
