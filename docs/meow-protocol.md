# Meow control stream extensions

These messages exist only on the `meow` branch. They are opt-in in both
directions: a stock Moonlight client or Sunshine/Apollo host never sends them,
and a peer that does not implement one simply ignores it.

All of them:

- are opt-in: a meow client sends SUBSCRIBE / REPORT only when it chooses to,
  and a meow host sends POSITION / APPLIED only in answer to them. Nothing
  announces support in advance, so the first client message is itself the
  probe; stock Sunshine/Apollo hosts log unknown control types at debug level
  and otherwise ignore them. A client that wants stock hosts never to see them
  gates on a proven meow host (a viewport echo) first;
- ride the **existing encrypted ENet control stream** (the same channel and the
  same AES-GCM encryption as every other control message). No new ports,
  sockets or listeners, so nothing changes for firewalls, Tailscale ACLs or NAT;
- exist only in the encrypted Gen 7 packet-type table (`packetTypesGen7Enc`).
  Older generations have `-1` in those slots and the send functions return `-3`;
- are **little endian**;
- are tiny (at most 24 bytes of payload), far below the 1280-byte tailnet MTU.

Every length is validated on receive. Short, oversize and unknown-version
payloads are dropped, never read past. The wire codecs live in
[`src/MeowProtocol.h`](../src/MeowProtocol.h) as pure functions and are tested
by [`tests/meow/`](../tests/meow) (`make -C tests/meow`).

| Type     | Name            | Direction            | Payload |
| -------- | --------------- | -------------------- | ------- |
| `0x3003` | VIEWPORT        | client -> host       | 10 bytes (request) |
|          |                 | host -> client       | 10, 14 or 18 bytes (echo) |
| `0x3004` | CURSOR          | client -> host       | 2 bytes (SUBSCRIBE) |
|          |                 | host -> client       | 8 bytes (POSITION) |
| `0x3005` | RECEIVER REPORT | client -> host       | 24 bytes (REPORT) |
|          |                 | host -> client       | 8 bytes (APPLIED) |

Numbers were checked against the packet types of Apollo (`0x3000`-`0x3002`)
and Sunshine (`0x5500`-`0x5504`); `tools/check-packet-type-tables.py` fails on
a duplicate or on a known foreign number.

## 0x3003 VIEWPORT

Coordinates are in the **negotiated stream resolution, uncropped** (including
the host's aspect-ratio padding), not host desktop pixels.

Request (client -> host), 10 bytes, unchanged:

| Offset | Type  | Field   |
| ------ | ----- | ------- |
| 0      | u8    | version = 1 |
| 1      | u8    | flags = 0 |
| 2      | u16   | x |
| 4      | u16   | y |
| 6      | u16   | width |
| 8      | u16   | height |

Echo (host -> client): the request layout, plus fields guarded by flag bits:

| Offset | Type  | Field | Present when |
| ------ | ----- | ----- | ------------ |
| 10     | u16   | desktop_width  | flag bit0 `desktop_extent` |
| 12     | u16   | desktop_height | flag bit0 `desktop_extent` |
| 14     | u32   | frame_index    | flag bit1 `frame_index` (echo v2) |

`frame_index` is the host video frame number (the same number the client sees
in `DECODE_UNIT.frameNumber`) of the **first** encoded frame produced with the
applied rectangle. The host must send the echo from the encode path only after
that frame has been produced (not from the control thread on receipt). A
revocation (applied = full frame) is echoed the same way. Clients that ignore
bit1 keep working.

Receive rules (client):

- version must be 1; width and height must be non-zero;
- bit1 without bit0 is malformed (frame_index is at a fixed offset 14);
- the payload must be exactly 10, 14 or 18 bytes as the flags describe.
  Only when the echo also carries flag bits this build does not know are
  trailing bytes tolerated (and ignored) — that is how the format grows without
  a version bump. Optional fields sit at fixed offsets in flag-bit order, so
  a host that sets flag bit N must also set every lower bit and send their
  fields; a future bit2 field goes at offset 18;
- a zero desktop extent is reported as "unknown" (0/0).

Client API: the echo is delivered through `ConnListenerSetViewportV2(x, y,
width, height, desktopWidth, desktopHeight, frameIndex)`, with `frameIndex = 0`
when the host did not send bit1. Callers that leave `setViewportV2` NULL keep
receiving the echo through the original `ConnListenerSetViewport` (the default
forwards to it), so existing callers compile and behave unchanged.

## 0x3004 CURSOR

SUBSCRIBE (client -> host), 2 bytes — `LiSendCursorSubscribe(bool)`:

| Offset | Type | Field |
| ------ | ---- | ----- |
| 0      | u8   | version = 1 |
| 1      | u8   | flags: bit0 subscribe (1) / unsubscribe (0) |

POSITION (host -> client), 8 bytes — `ConnListenerCursorPosition(x, y,
visible, seq)`:

| Offset | Type | Field |
| ------ | ---- | ----- |
| 0      | u8   | version = 1 |
| 1      | u8   | flags: bit0 visible |
| 2      | u16  | seq |
| 4      | u16  | x |
| 6      | u16  | y |

x, y = cursor **hotspot** in the uncropped reference frame (negotiated stream
resolution with Sunshine's aspect padding — the same space as 0x3003, via the
host's `to_reference()`), clamped to the frame. Sent on change, coalesced to
<= 60 Hz, plus once immediately on subscribe and on visibility change. `seq`
increments per send (wraps). The host never sends POSITION to a client that did
not subscribe.

Receive rules (client): exactly 8 bytes, version 1; unknown flag bits are
ignored. The library keeps only the latest position while a callback is
outstanding, so under load `seq` can skip.

## 0x3005 RECEIVER REPORT

REPORT (client -> host), every 1000 ms while streaming, 24 bytes —
`LiSendReceiverReport(const MEOW_RECEIVER_REPORT*)`:

| Offset | Type | Field |
| ------ | ---- | ----- |
| 0      | u8   | version = 1 |
| 1      | u8   | flags: bit0 auto_bitrate |
| 2      | u16  | interval_ms |
| 4      | u32  | received_kbps (video goodput incl. FEC) |
| 8      | u16  | loss_permille (network-lost video packets / expected, pre-FEC; clamped to 1000) |
| 10     | u16  | rtt_ms |
| 12     | u16  | rtt_var_ms |
| 14     | u16  | decode_queue_frames |
| 16     | u16  | avg_decode_ms |
| 18     | u16  | reserved = 0 |
| 20     | u32  | max_kbps (client ceiling; 0 = negotiated bitrate is the ceiling) |

APPLIED (host -> client), 8 bytes — `ConnListenerBitrateApplied(kbps)`:

| Offset | Type | Field |
| ------ | ---- | ----- |
| 0      | u8   | version = 1 |
| 1      | u8   | flags = 0 |
| 2      | u16  | reserved |
| 4      | u32  | applied_kbps |

Sent whenever the host's controller changes the encoder bitrate (and once after
the first report). Receive rules (client): exactly 8 bytes, version 1,
`applied_kbps` non-zero.

### Client statistics

`received_kbps` and `loss_permille` need network-level counters that only the
library sees. `LiGetMeowVideoNetworkStats(PMEOW_VIDEO_NETWORK_STATS)` returns
free-running (mod 2^32) counts of video packets received, packets expected (from
the advance of authenticated RTP sequence numbers, data + FEC parity) and bytes
received. The expected count survives sequence-number wraparound after long
outages (see `meowTrackSequenceNumber()`), and the received counts include
late FEC shards dropped before decryption. Take the difference between two
snapshots one interval apart:

```
expected = now.packetsExpected - last.packetsExpected      // uint32 arithmetic
received = now.packetsReceived - last.packetsReceived
lost     = max(0, (int32_t)(expected - received))
loss_permille = expected ? lost * 1000 / expected : 0
received_kbps = (now.bytesReceived - last.bytesReceived) * 8 / interval_ms
```

RTT comes from `LiGetEstimatedRttInfo()` and the decode queue from
`LiGetPendingVideoFrames()`; decode time is measured by the client's decoder.

## Callback ABI

`setViewportV2`, `cursorPosition` and `bitrateApplied` are appended to the end
of `CONNECTION_LISTENER_CALLBACKS`. Callers using designated initializers
compile unchanged; `fixupMissingCallbacks()` fills NULL members with no-ops (and
`setViewportV2` with the forwarder to `setViewport`). The struct is not ABI
stable across library versions — rebuild callers against the matching header.
