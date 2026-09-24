#!/usr/bin/env python3
"""Verify the ControlStream.c packet type tables stay aligned.

src/ControlStream.c indexes five parallel `packetTypesGen*` arrays with the same
set of IDX_* constants. If one array is shorter than the others, a lookup for a
high index silently reads the wrong packet type or past the end of the array.
This has already happened once in this file's history, so it is checked in CI
rather than by eye.

Asserts:
  1. All five packetTypesGen* arrays exist and have identical length.
  2. Every IDX_* constant is in bounds for that length.
  3. No packet type number appears twice within one array.
  4. No packet type number is one that Sunshine or Apollo use for a message
     this library does not implement (FOREIGN_PACKET_TYPES). Our own protocol
     extensions (0x3003-0x3005) share the host's number space, so a collision
     would make one host message be parsed as another.

Exits non-zero on failure.
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO_ROOT, "src", "ControlStream.c")

EXPECTED_ARRAYS = [
    "packetTypesGen3",
    "packetTypesGen4",
    "packetTypesGen5",
    "packetTypesGen7",
    "packetTypesGen7Enc",
]

# Control stream packet types that Sunshine (LizardByte/Sunshine src/stream.cpp)
# or Apollo (ClassicOldSong/Apollo src/stream.cpp) define but that have no entry
# in our tables. Types both sides already share (0x5500-0x5503, 0x3000-0x3002)
# are in the tables themselves and covered by the uniqueness check. Re-check
# both hosts' packetTypes arrays before adding a new extension number.
FOREIGN_PACKET_TYPES = {
    0x5504: "Sunshine: set player indicator LEDs",
}

ARRAY_RE = re.compile(
    r"static\s+const\s+short\s+(packetTypesGen\w*)\s*\[\s*\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
IDX_RE = re.compile(r"^#define\s+(IDX_\w+)\s+(\d+)\s*$", re.MULTILINE)


def count_entries(body):
    # Strip // comments, then count comma-separated non-empty entries.
    body = re.sub(r"//[^\n]*", "", body)
    return len([e for e in body.split(",") if e.strip()])


def entry_values(body):
    # Integer values of the entries, in order (-1 for unused slots).
    body = re.sub(r"//[^\n]*", "", body)
    return [int(e.strip(), 0) for e in body.split(",") if e.strip()]


def main():
    source = open(SOURCE, encoding="utf-8").read()

    bodies = dict(ARRAY_RE.findall(source))
    arrays = {name: count_entries(body) for name, body in bodies.items()}
    indices = {name: int(value) for name, value in IDX_RE.findall(source)}

    failures = []

    print("packet type arrays in %s:" % os.path.relpath(SOURCE, REPO_ROOT))
    for name in EXPECTED_ARRAYS:
        if name not in arrays:
            failures.append("array %s not found" % name)
            print("  %-20s MISSING" % name)
        else:
            print("  %-20s %d entries" % (name, arrays[name]))

    for name in sorted(set(arrays) - set(EXPECTED_ARRAYS)):
        failures.append("unexpected extra array %s (update EXPECTED_ARRAYS)" % name)
        print("  %-20s %d entries  UNEXPECTED" % (name, arrays[name]))

    lengths = set(arrays.values())
    if len(lengths) > 1:
        failures.append("arrays have differing lengths: %s" % sorted(lengths))
    print("\nall arrays same length: %s" % ("YES" if len(lengths) == 1 else "NO"))

    if not indices:
        failures.append("no IDX_* constants found")

    bound = min(lengths) if lengths else 0
    print("\nIDX_* constants (bound = %d):" % bound)
    for name, value in sorted(indices.items(), key=lambda kv: (kv[1], kv[0])):
        ok = 0 <= value < bound
        if not ok:
            failures.append("%s = %d is out of bounds (0..%d)" % (name, value, bound - 1))
        print("  %-36s %2d  %s" % (name, value, "ok" if ok else "OUT OF BOUNDS"))

    print("\npacket type numbers:")
    for name in sorted(bodies):
        values = [v for v in entry_values(bodies[name]) if v != -1]
        dupes = sorted({v for v in values if values.count(v) > 1})
        foreign = sorted(v for v in values if v in FOREIGN_PACKET_TYPES)
        for v in dupes:
            failures.append("%s uses 0x%04x more than once" % (name, v))
        for v in foreign:
            failures.append("%s uses 0x%04x, which is taken (%s)"
                            % (name, v, FOREIGN_PACKET_TYPES[v]))
        print("  %-20s %s" % (name, "ok" if not dupes and not foreign else "COLLISION"))

    print("")
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        return 1

    print("PASS: %d arrays x %d entries, %d IDX_* constants all in bounds"
          % (len(arrays), bound, len(indices)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
