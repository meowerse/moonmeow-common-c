// Standalone tests for the meow control stream extensions in src/MeowProtocol.h.
//
// Build and run with `make -C tests/meow`. Nothing here links against the
// library: the codecs are header-only pure functions, which is what makes them
// testable without an ENet peer or a host.
//
// Every decoder input is copied into a heap buffer of exactly the length under
// test, so under the default AddressSanitizer build a read even one byte past
// the payload aborts the run.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MeowProtocol.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
    long long a_ = (long long)(actual), e_ = (long long)(expected); \
    checks++; \
    if (a_ != e_) { \
        failures++; \
        fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, expected %lld\n", \
                __FILE__, __LINE__, #actual, a_, e_); \
    } \
} while (0)

// Copies len bytes into an exactly-sized heap allocation (at least 1 byte so
// malloc(0) can't return NULL) so ASan catches any over-read.
static uint8_t* exact(const uint8_t* bytes, size_t len) {
    uint8_t* p = malloc(len ? len : 1);
    if (p == NULL) {
        abort();
    }
    if (len) {
        memcpy(p, bytes, len);
    }
    return p;
}

static int parseEcho(const uint8_t* bytes, size_t len, MEOW_VIEWPORT_ECHO* echo) {
    uint8_t* p = exact(bytes, len);
    int ret = meowParseViewportEcho(p, len, echo);
    free(p);
    return ret;
}

static int parseCursor(const uint8_t* bytes, size_t len, MEOW_CURSOR_POSITION* pos) {
    uint8_t* p = exact(bytes, len);
    int ret = meowParseCursorPosition(p, len, pos);
    free(p);
    return ret;
}

static int parseApplied(const uint8_t* bytes, size_t len, uint32_t* kbps) {
    uint8_t* p = exact(bytes, len);
    int ret = meowParseBitrateApplied(p, len, kbps);
    free(p);
    return ret;
}

// A v2 echo: x=0x0102 y=0x0304 w=0x0506 h=0x0708, desktop 5360x1440,
// frame 0xA1B2C3D4. Asymmetric bytes catch any byte-order slip.
static const uint8_t ECHO_V2[18] = {
    0x01, 0x03,
    0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07,
    0xF0, 0x14, 0xA0, 0x05,
    0xD4, 0xC3, 0xB2, 0xA1,
};

static void testViewportEcho(void) {
    MEOW_VIEWPORT_ECHO echo;
    uint8_t buf[32];
    size_t len;

    // v1, no optional fields (10 bytes)
    memcpy(buf, ECHO_V2, 10);
    buf[1] = 0x00;
    memset(&echo, 0xEE, sizeof(echo));
    CHECK_EQ(parseEcho(buf, 10, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.x, 0x0102);
    CHECK_EQ(echo.y, 0x0304);
    CHECK_EQ(echo.width, 0x0506);
    CHECK_EQ(echo.height, 0x0708);
    CHECK_EQ(echo.desktopWidth, 0);
    CHECK_EQ(echo.desktopHeight, 0);
    CHECK_EQ(echo.frameIndex, 0);
    CHECK_EQ(echo.unknownFlags, 0);

    // v1 with desktop extent (14 bytes) -- what current sunmeow hosts send
    memcpy(buf, ECHO_V2, 14);
    buf[1] = MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT;
    CHECK_EQ(parseEcho(buf, 14, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.desktopWidth, 5360);
    CHECK_EQ(echo.desktopHeight, 1440);
    CHECK_EQ(echo.frameIndex, 0);

    // v2: desktop extent + frame index (18 bytes)
    CHECK_EQ(parseEcho(ECHO_V2, 18, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.x, 0x0102);
    CHECK_EQ(echo.height, 0x0708);
    CHECK_EQ(echo.desktopWidth, 5360);
    CHECK_EQ(echo.desktopHeight, 1440);
    CHECK_EQ(echo.frameIndex, 0xA1B2C3D4u);

    // Every truncation of a v2 echo is short, never a partial parse
    for (len = 0; len < sizeof(ECHO_V2); len++) {
        CHECK_EQ(parseEcho(ECHO_V2, len, &echo), MEOW_PARSE_SHORT);
    }

    // Flag says a field is there but the bytes aren't
    memcpy(buf, ECHO_V2, 10);
    buf[1] = MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT;
    CHECK_EQ(parseEcho(buf, 10, &echo), MEOW_PARSE_SHORT);
    CHECK_EQ(parseEcho(buf, 13, &echo), MEOW_PARSE_SHORT);

    // Oversize without unknown flags is dropped
    memset(buf, 0, sizeof(buf));
    memcpy(buf, ECHO_V2, 18);
    CHECK_EQ(parseEcho(buf, 19, &echo), MEOW_PARSE_OVERSIZE);
    buf[1] = 0x00;
    CHECK_EQ(parseEcho(buf, 11, &echo), MEOW_PARSE_OVERSIZE);
    buf[1] = MEOW_VIEWPORT_FLAG_DESKTOP_EXTENT;
    CHECK_EQ(parseEcho(buf, 15, &echo), MEOW_PARSE_OVERSIZE);
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_OVERSIZE);

    // Frame index lives at offset 14, so it is malformed without the extent
    memcpy(buf, ECHO_V2, 18);
    buf[1] = MEOW_VIEWPORT_FLAG_FRAME_INDEX;
    CHECK_EQ(parseEcho(buf, 14, &echo), MEOW_PARSE_INVALID);
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_INVALID);

    // Unknown flag bits announce newer fields: trailing bytes are tolerated,
    // known fields are still required
    memset(buf, 0x5A, sizeof(buf));
    memcpy(buf, ECHO_V2, 18);
    buf[1] = 0x03 | 0x04;
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.unknownFlags, 0x04);
    CHECK_EQ(echo.frameIndex, 0xA1B2C3D4u);
    CHECK_EQ(parseEcho(buf, 30, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.frameIndex, 0xA1B2C3D4u);
    CHECK_EQ(parseEcho(buf, 17, &echo), MEOW_PARSE_SHORT);
    buf[1] = 0x80;
    CHECK_EQ(parseEcho(buf, 12, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.unknownFlags, 0x80);
    CHECK_EQ(echo.desktopWidth, 0);

    // Wrong version, at every otherwise-valid length
    memcpy(buf, ECHO_V2, 18);
    buf[0] = 0;
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_VERSION);
    buf[0] = 2;
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_VERSION);
    buf[1] = 0;
    CHECK_EQ(parseEcho(buf, 10, &echo), MEOW_PARSE_VERSION);

    // Empty rectangle
    memcpy(buf, ECHO_V2, 18);
    buf[6] = buf[7] = 0;
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_INVALID);
    memcpy(buf, ECHO_V2, 18);
    buf[8] = buf[9] = 0;
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_INVALID);

    // A zero desktop extent means "unknown", not a malformed echo
    memcpy(buf, ECHO_V2, 18);
    buf[12] = buf[13] = 0;
    CHECK_EQ(parseEcho(buf, 18, &echo), MEOW_PARSE_OK);
    CHECK_EQ(echo.desktopWidth, 0);
    CHECK_EQ(echo.desktopHeight, 0);
    CHECK_EQ(echo.frameIndex, 0xA1B2C3D4u);
}

static void testCursorPosition(void) {
    // version 1, visible, seq 0xBEEF, x 5359, y 1439
    static const uint8_t POS[8] = { 0x01, 0x01, 0xEF, 0xBE, 0xEF, 0x14, 0x9F, 0x05 };
    MEOW_CURSOR_POSITION pos;
    uint8_t buf[16];
    size_t len;

    CHECK_EQ(parseCursor(POS, 8, &pos), MEOW_PARSE_OK);
    CHECK(pos.visible);
    CHECK_EQ(pos.seq, 0xBEEF);
    CHECK_EQ(pos.x, 5359);
    CHECK_EQ(pos.y, 1439);

    // Hidden
    memcpy(buf, POS, 8);
    buf[1] = 0x00;
    CHECK_EQ(parseCursor(buf, 8, &pos), MEOW_PARSE_OK);
    CHECK(!pos.visible);

    // Unknown flag bits don't change the fixed layout
    buf[1] = 0xFE;
    CHECK_EQ(parseCursor(buf, 8, &pos), MEOW_PARSE_OK);
    CHECK(!pos.visible);
    CHECK_EQ(pos.x, 5359);

    for (len = 0; len < 8; len++) {
        CHECK_EQ(parseCursor(POS, len, &pos), MEOW_PARSE_SHORT);
    }

    memset(buf, 0, sizeof(buf));
    memcpy(buf, POS, 8);
    CHECK_EQ(parseCursor(buf, 9, &pos), MEOW_PARSE_OVERSIZE);
    CHECK_EQ(parseCursor(buf, 16, &pos), MEOW_PARSE_OVERSIZE);

    buf[0] = 0;
    CHECK_EQ(parseCursor(buf, 8, &pos), MEOW_PARSE_VERSION);
    buf[0] = 2;
    CHECK_EQ(parseCursor(buf, 8, &pos), MEOW_PARSE_VERSION);
}

static void testBitrateApplied(void) {
    // version 1, flags 0, reserved 0, 25000 kbps
    static const uint8_t APPLIED[8] = { 0x01, 0x00, 0x00, 0x00, 0xA8, 0x61, 0x00, 0x00 };
    uint8_t buf[16];
    uint32_t kbps;
    size_t len;

    CHECK_EQ(parseApplied(APPLIED, 8, &kbps), MEOW_PARSE_OK);
    CHECK_EQ(kbps, 25000);

    memcpy(buf, APPLIED, 8);
    buf[4] = 0x04; buf[5] = 0x03; buf[6] = 0x02; buf[7] = 0x01;
    CHECK_EQ(parseApplied(buf, 8, &kbps), MEOW_PARSE_OK);
    CHECK_EQ(kbps, 0x01020304);

    for (len = 0; len < 8; len++) {
        CHECK_EQ(parseApplied(APPLIED, len, &kbps), MEOW_PARSE_SHORT);
    }

    memset(buf, 0, sizeof(buf));
    memcpy(buf, APPLIED, 8);
    CHECK_EQ(parseApplied(buf, 9, &kbps), MEOW_PARSE_OVERSIZE);

    buf[0] = 2;
    CHECK_EQ(parseApplied(buf, 8, &kbps), MEOW_PARSE_VERSION);

    memcpy(buf, APPLIED, 8);
    buf[4] = buf[5] = 0;
    CHECK_EQ(parseApplied(buf, 8, &kbps), MEOW_PARSE_INVALID);
}

static void testEncoders(void) {
    uint8_t sub[MEOW_CURSOR_SUBSCRIBE_LENGTH];
    uint8_t report[MEOW_RECEIVER_REPORT_LENGTH + 1];
    MEOW_RECEIVER_REPORT r;

    CHECK_EQ(MEOW_CURSOR_SUBSCRIBE_LENGTH, 2);
    meowEncodeCursorSubscribe(sub, true);
    CHECK_EQ(sub[0], 0x01);
    CHECK_EQ(sub[1], 0x01);
    meowEncodeCursorSubscribe(sub, false);
    CHECK_EQ(sub[0], 0x01);
    CHECK_EQ(sub[1], 0x00);

    memset(&r, 0, sizeof(r));
    r.autoBitrate = true;
    r.intervalMs = 1000;            // e8 03
    r.receivedKbps = 0x00012345;    // 45 23 01 00
    r.lossPermille = 37;            // 25 00
    r.rttMs = 0x0102;               // 02 01
    r.rttVarianceMs = 0x0304;       // 04 03
    r.decodeQueueFrames = 2;        // 02 00
    r.avgDecodeMs = 0x0506;         // 06 05
    r.maxKbps = 150000;             // f0 49 02 00

    static const uint8_t EXPECTED[MEOW_RECEIVER_REPORT_LENGTH] = {
        0x01, 0x01, 0xE8, 0x03,
        0x45, 0x23, 0x01, 0x00,
        0x25, 0x00, 0x02, 0x01,
        0x04, 0x03, 0x02, 0x00,
        0x06, 0x05, 0x00, 0x00,
        0xF0, 0x49, 0x02, 0x00,
    };

    // Canary byte after the payload proves the encoder writes exactly 24 bytes
    memset(report, 0xCC, sizeof(report));
    meowEncodeReceiverReport(report, &r);
    CHECK(memcmp(report, EXPECTED, MEOW_RECEIVER_REPORT_LENGTH) == 0);
    CHECK_EQ(report[MEOW_RECEIVER_REPORT_LENGTH], 0xCC);

    // Flags off, loss clamped to 1000, max 0 = negotiated ceiling
    r.autoBitrate = false;
    r.lossPermille = 4000;
    r.maxKbps = 0;
    meowEncodeReceiverReport(report, &r);
    CHECK_EQ(report[1], 0x00);
    CHECK_EQ(report[8], 0xE8);
    CHECK_EQ(report[9], 0x03);
    CHECK_EQ(report[18], 0x00);
    CHECK_EQ(report[19], 0x00);
    CHECK_EQ(report[20] | report[21] | report[22] | report[23], 0);
}

static void testSequenceTracker(void) {
    MEOW_SEQUENCE_TRACKER t;
    uint32_t expected = 0;

    memset(&t, 0, sizeof(t));
    expected += meowTrackSequenceNumber(&t, 100);   // first packet
    CHECK_EQ(expected, 1);
    expected += meowTrackSequenceNumber(&t, 101);
    expected += meowTrackSequenceNumber(&t, 102);
    CHECK_EQ(expected, 3);
    expected += meowTrackSequenceNumber(&t, 106);   // 103..105 lost
    CHECK_EQ(expected, 7);
    expected += meowTrackSequenceNumber(&t, 104);   // reordered, already counted
    expected += meowTrackSequenceNumber(&t, 106);   // duplicate
    CHECK_EQ(expected, 7);

    // Wraparound
    memset(&t, 0, sizeof(t));
    expected = meowTrackSequenceNumber(&t, 0xFFFE);
    expected += meowTrackSequenceNumber(&t, 0xFFFF);
    expected += meowTrackSequenceNumber(&t, 0x0001); // 0x0000 lost
    CHECK_EQ(expected, 4);
    expected += meowTrackSequenceNumber(&t, 0xFFFF); // old
    CHECK_EQ(expected, 4);

    // A 40000-packet outage lands more than half the space ahead. The first
    // packet after it is ambiguous; the next consecutive one confirms the jump
    // and the whole gap is counted, rather than stalling until the wrap.
    memset(&t, 0, sizeof(t));
    expected = meowTrackSequenceNumber(&t, 100);
    expected += meowTrackSequenceNumber(&t, 101);
    expected += meowTrackSequenceNumber(&t, 40101);
    CHECK_EQ(expected, 2);
    expected += meowTrackSequenceNumber(&t, 40102);
    CHECK_EQ(expected, 40003);
    expected += meowTrackSequenceNumber(&t, 40103);
    CHECK_EQ(expected, 40004);

    // Same outage, but the confirming packet was lost: the probe restarts on
    // the next packet and the jump is still counted in full
    memset(&t, 0, sizeof(t));
    expected = meowTrackSequenceNumber(&t, 100);
    expected += meowTrackSequenceNumber(&t, 40101);
    expected += meowTrackSequenceNumber(&t, 40103);
    CHECK_EQ(expected, 1);
    expected += meowTrackSequenceNumber(&t, 40104);
    CHECK_EQ(expected, 40005);

    // Reordering within MEOW_SEQUENCE_MAX_MISORDER never counts, even twice
    memset(&t, 0, sizeof(t));
    expected = meowTrackSequenceNumber(&t, 10000);
    expected += meowTrackSequenceNumber(&t, 10000 - MEOW_SEQUENCE_MAX_MISORDER);
    expected += meowTrackSequenceNumber(&t, 10001 - MEOW_SEQUENCE_MAX_MISORDER);
    CHECK_EQ(expected, 1);

    // A single stray packet far outside the window is ignored, and the
    // stream carries on counting normally
    memset(&t, 0, sizeof(t));
    expected = meowTrackSequenceNumber(&t, 10000);
    expected += meowTrackSequenceNumber(&t, 50000);
    expected += meowTrackSequenceNumber(&t, 10001);
    expected += meowTrackSequenceNumber(&t, 10002);
    CHECK_EQ(expected, 3);
}

// Random payloads of random lengths: nothing crashes or over-reads (ASan), and
// anything accepted satisfies the documented invariants.
static void testRandomPayloads(void) {
    uint8_t buf[64];
    unsigned int seed = 12345;

    for (int i = 0; i < 200000; i++) {
        size_t len;
        MEOW_VIEWPORT_ECHO echo;
        MEOW_CURSOR_POSITION pos;
        uint32_t kbps;

        for (size_t j = 0; j < sizeof(buf); j++) {
            seed = seed * 1103515245u + 12345u;
            buf[j] = (uint8_t)(seed >> 16);
        }
        // Bias toward valid versions and plausible lengths
        if (i & 1) {
            buf[0] = 1;
        }
        len = (seed >> 8) % 24;

        if (parseEcho(buf, len, &echo) == MEOW_PARSE_OK) {
            CHECK(len >= 10);
            CHECK(echo.width != 0 && echo.height != 0);
            CHECK((echo.desktopWidth == 0) == (echo.desktopHeight == 0));
        }
        if (parseCursor(buf, len, &pos) == MEOW_PARSE_OK) {
            CHECK_EQ(len, 8);
        }
        if (parseApplied(buf, len, &kbps) == MEOW_PARSE_OK) {
            CHECK_EQ(len, 8);
            CHECK(kbps != 0);
        }
    }
}

int main(void) {
    testViewportEcho();
    testCursorPosition();
    testBitrateApplied();
    testEncoders();
    testSequenceTracker();
    testRandomPayloads();

    if (failures) {
        printf("FAIL: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("PASS: %d checks\n", checks);
    return 0;
}
