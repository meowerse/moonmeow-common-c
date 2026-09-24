//
// Wire encoding and decoding for the meow control stream extensions
// (0x3003 VIEWPORT echo, 0x3004 CURSOR, 0x3005 RECEIVER REPORT).
//
// Everything in here is a pure function over a byte buffer: no globals, no
// allocation, no locking and no dependency on the rest of the library beyond the
// public types in Limelight.h. That is deliberate. It keeps the length and
// version validation of host-supplied bytes in one place that the standalone
// tests under tests/meow/ can exercise directly, and it keeps the edits to
// ControlStream.c down to calls into this header.
//
// Every multi-byte field is little endian, like every other control stream
// payload. The functions assemble and split values byte by byte, so they are
// correct regardless of host byte order and never perform unaligned loads.
//
// The full wire format is documented in docs/meow-protocol.md.
//

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "Limelight.h"

// Results of the meowParse* functions. Anything other than MEOW_PARSE_OK means
// the payload must be dropped without acting on any part of it.
#define MEOW_PARSE_OK        0
#define MEOW_PARSE_SHORT    -1 // Fewer bytes than the header/flags require
#define MEOW_PARSE_OVERSIZE -2 // More bytes than the header/flags allow
#define MEOW_PARSE_VERSION  -3 // Unsupported version byte
#define MEOW_PARSE_INVALID  -4 // Well-formed, but a field has an impossible value

// -----------------------------------------------------------------------------
// 0x3003 VIEWPORT
// -----------------------------------------------------------------------------

// There is no negotiation of this version: a receiver that doesn't recognise it
// discards the message entirely rather than parsing it field by field. Prefer
// the flags byte for additive changes.
#define MEOW_VIEWPORT_VERSION 1

// version, flags, x, y, width, height
#define MEOW_VIEWPORT_REQUEST_LENGTH 10

// Echo only: uint16 desktopWidth, uint16 desktopHeight at offset 10.
#define MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT 0x01

// Echo only: uint32 frameIndex at offset 14, the host video frame number of the
// first frame encoded with the applied rectangle. Offset 14 means it is only
// well-formed together with MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT.
#define MEOW_VIEWPORT_FLAG_FRAME_INDEX 0x02

// All flag bits this version knows how to act on. Bits outside this mask belong
// to fields added after this build; their bytes follow the known fields and are
// ignored, which is how the format grows without a version bump.
#define MEOW_VIEWPORT_KNOWN_FLAGS (MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT | MEOW_VIEWPORT_FLAG_FRAME_INDEX)

typedef struct _MEOW_VIEWPORT_ECHO {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;

    // Captured desktop size, or 0/0 when the host did not report it (or
    // reported a meaningless zero extent).
    uint16_t desktopWidth;
    uint16_t desktopHeight;

    // First host frame number carrying the applied rectangle, or 0 when the
    // host did not report it. Real frame numbers start at 1.
    uint32_t frameIndex;

    // Flag bits outside MEOW_VIEWPORT_KNOWN_FLAGS, for logging only.
    uint8_t unknownFlags;
} MEOW_VIEWPORT_ECHO, *PMEOW_VIEWPORT_ECHO;

// -----------------------------------------------------------------------------
// 0x3004 CURSOR
// -----------------------------------------------------------------------------

#define MEOW_CURSOR_VERSION 1

// client -> host SUBSCRIBE: version, flags
#define MEOW_CURSOR_SUBSCRIBE_LENGTH 2
#define MEOW_CURSOR_FLAG_SUBSCRIBE 0x01

// host -> client POSITION: version, flags, seq, x, y
#define MEOW_CURSOR_POSITION_LENGTH 8
#define MEOW_CURSOR_FLAG_VISIBLE 0x01

typedef struct _MEOW_CURSOR_POSITION {
    uint16_t x;
    uint16_t y;
    uint16_t seq;
    bool visible;
} MEOW_CURSOR_POSITION, *PMEOW_CURSOR_POSITION;

// -----------------------------------------------------------------------------
// 0x3005 RECEIVER REPORT
// -----------------------------------------------------------------------------

#define MEOW_RECEIVER_REPORT_VERSION 1

// client -> host REPORT, see MEOW_RECEIVER_REPORT in Limelight.h
#define MEOW_RECEIVER_REPORT_LENGTH 24
#define MEOW_RECEIVER_REPORT_FLAG_AUTO_BITRATE 0x01

// host -> client APPLIED: version, flags, reserved, appliedKbps
#define MEOW_BITRATE_APPLIED_LENGTH 8

// -----------------------------------------------------------------------------
// Little-endian helpers
// -----------------------------------------------------------------------------

static inline uint16_t meowGetLe16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t meowGetLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void meowPutLe16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void meowPutLe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// -----------------------------------------------------------------------------
// Decoders (host -> client). None of them reads outside [payload, payload+length).
// -----------------------------------------------------------------------------

// Parses a 0x3003 viewport echo.
//
// The payload must be exactly the length its known flags describe. The only
// exception is a payload that also carries unknown flag bits: those announce
// fields from a newer host, so trailing bytes are allowed (and ignored) there.
static inline int meowParseViewportEcho(const uint8_t* payload, size_t length, PMEOW_VIEWPORT_ECHO echo) {
    size_t expectedLength;
    uint8_t flags;

    if (length < MEOW_VIEWPORT_REQUEST_LENGTH) {
        return MEOW_PARSE_SHORT;
    }

    if (payload[0] != MEOW_VIEWPORT_VERSION) {
        return MEOW_PARSE_VERSION;
    }

    flags = payload[1];

    // The frame index lives at a fixed offset behind the desktop extent
    if ((flags & MEOW_VIEWPORT_FLAG_FRAME_INDEX) && !(flags & MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT)) {
        return MEOW_PARSE_INVALID;
    }

    expectedLength = MEOW_VIEWPORT_REQUEST_LENGTH;
    if (flags & MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT) {
        expectedLength += 4;
    }
    if (flags & MEOW_VIEWPORT_FLAG_FRAME_INDEX) {
        expectedLength += 4;
    }

    if (length < expectedLength) {
        return MEOW_PARSE_SHORT;
    }
    if (length > expectedLength && !(flags & ~MEOW_VIEWPORT_KNOWN_FLAGS)) {
        return MEOW_PARSE_OVERSIZE;
    }

    echo->x = meowGetLe16(&payload[2]);
    echo->y = meowGetLe16(&payload[4]);
    echo->width = meowGetLe16(&payload[6]);
    echo->height = meowGetLe16(&payload[8]);

    // An empty rectangle is meaningless and would make the caller divide by zero
    if (echo->width == 0 || echo->height == 0) {
        return MEOW_PARSE_INVALID;
    }

    echo->desktopWidth = 0;
    echo->desktopHeight = 0;
    if (flags & MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT) {
        echo->desktopWidth = meowGetLe16(&payload[10]);
        echo->desktopHeight = meowGetLe16(&payload[12]);

        // A zero extent means "unknown", not a malformed rectangle
        if (echo->desktopWidth == 0 || echo->desktopHeight == 0) {
            echo->desktopWidth = 0;
            echo->desktopHeight = 0;
        }
    }

    echo->frameIndex = 0;
    if (flags & MEOW_VIEWPORT_FLAG_FRAME_INDEX) {
        echo->frameIndex = meowGetLe32(&payload[14]);
    }

    echo->unknownFlags = flags & (uint8_t)~MEOW_VIEWPORT_KNOWN_FLAGS;
    return MEOW_PARSE_OK;
}

// Parses a 0x3004 cursor POSITION. Unknown flag bits are ignored; the length is
// fixed for version 1.
static inline int meowParseCursorPosition(const uint8_t* payload, size_t length, PMEOW_CURSOR_POSITION position) {
    if (length < MEOW_CURSOR_POSITION_LENGTH) {
        return MEOW_PARSE_SHORT;
    }
    if (length > MEOW_CURSOR_POSITION_LENGTH) {
        return MEOW_PARSE_OVERSIZE;
    }
    if (payload[0] != MEOW_CURSOR_VERSION) {
        return MEOW_PARSE_VERSION;
    }

    position->visible = (payload[1] & MEOW_CURSOR_FLAG_VISIBLE) != 0;
    position->seq = meowGetLe16(&payload[2]);
    position->x = meowGetLe16(&payload[4]);
    position->y = meowGetLe16(&payload[6]);
    return MEOW_PARSE_OK;
}

// Parses a 0x3005 bitrate APPLIED message. A zero bitrate is impossible for a
// running encoder and is rejected.
static inline int meowParseBitrateApplied(const uint8_t* payload, size_t length, uint32_t* appliedKbps) {
    if (length < MEOW_BITRATE_APPLIED_LENGTH) {
        return MEOW_PARSE_SHORT;
    }
    if (length > MEOW_BITRATE_APPLIED_LENGTH) {
        return MEOW_PARSE_OVERSIZE;
    }
    if (payload[0] != MEOW_RECEIVER_REPORT_VERSION) {
        return MEOW_PARSE_VERSION;
    }

    *appliedKbps = meowGetLe32(&payload[4]);
    if (*appliedKbps == 0) {
        return MEOW_PARSE_INVALID;
    }

    return MEOW_PARSE_OK;
}

// -----------------------------------------------------------------------------
// Encoders (client -> host). Each writes exactly its *_LENGTH bytes.
// -----------------------------------------------------------------------------

static inline void meowEncodeCursorSubscribe(uint8_t out[MEOW_CURSOR_SUBSCRIBE_LENGTH], bool subscribe) {
    out[0] = MEOW_CURSOR_VERSION;
    out[1] = subscribe ? MEOW_CURSOR_FLAG_SUBSCRIBE : 0;
}

// loss_permille is clamped to 1000 so a caller's arithmetic slip can't put an
// impossible ratio on the wire.
static inline void meowEncodeReceiverReport(uint8_t out[MEOW_RECEIVER_REPORT_LENGTH], const MEOW_RECEIVER_REPORT* report) {
    out[0] = MEOW_RECEIVER_REPORT_VERSION;
    out[1] = report->autoBitrate ? MEOW_RECEIVER_REPORT_FLAG_AUTO_BITRATE : 0;
    meowPutLe16(&out[2], report->intervalMs);
    meowPutLe32(&out[4], report->receivedKbps);
    meowPutLe16(&out[8], report->lossPermille > 1000 ? 1000 : report->lossPermille);
    meowPutLe16(&out[10], report->rttMs);
    meowPutLe16(&out[12], report->rttVarianceMs);
    meowPutLe16(&out[14], report->decodeQueueFrames);
    meowPutLe16(&out[16], report->avgDecodeMs);
    meowPutLe16(&out[18], 0); // Reserved
    meowPutLe32(&out[20], report->maxKbps);
}

// -----------------------------------------------------------------------------
// Video network counters
// -----------------------------------------------------------------------------

// Tracks how many video packets the host has sent, from the 16-bit RTP sequence
// numbers of authenticated packets. The host numbers every video packet (data
// and FEC parity) consecutively, so the advance of the highest sequence number
// seen is the number of packets sent, whether or not they arrived. Reordered and
// duplicate packets (behind the highest seen) don't advance it.
typedef struct _MEOW_SEQUENCE_TRACKER {
    bool started;
    uint16_t highestSequenceNumber;
} MEOW_SEQUENCE_TRACKER, *PMEOW_SEQUENCE_TRACKER;

// Returns how many packets sequenceNumber adds to the expected count.
static inline uint32_t meowTrackSequenceNumber(PMEOW_SEQUENCE_TRACKER tracker, uint16_t sequenceNumber) {
    uint16_t advance;

    if (!tracker->started) {
        tracker->started = true;
        tracker->highestSequenceNumber = sequenceNumber;
        return 1;
    }

    advance = (uint16_t)(sequenceNumber - tracker->highestSequenceNumber);
    if (advance == 0 || advance >= 0x8000) {
        // Duplicate, or behind the highest sequence number seen
        return 0;
    }

    tracker->highestSequenceNumber = sequenceNumber;
    return advance;
}
