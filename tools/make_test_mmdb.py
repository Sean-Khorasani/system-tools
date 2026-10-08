#!/usr/bin/env python3
"""Build a minimal, VALID MaxMind DB (.mmdb) so WinTCP's GeoIP path can be
tested end to end with a database the app actually accepts.

No real MaxMind database can be shipped with WinTCP (licensing), so the
country/geoip code could otherwise only ever be exercised against its
"cannot load database" rejection. This writes a real file - search tree,
16-byte separator, data section, "\xab\xcd\xefMaxMind.com" marker, metadata -
that maps a few /16s to country records.

Geometry (matches what wintcp/src/GeoIp.cpp derives from the metadata):
  record_size 24 -> 3 bytes per record, 6 per node
  node_count    -> a record < node_count is the next node,
                    == node_count means "no data",
                    > node_count + 16 is a data pointer (offset = record -
                    node_count - 16), and hitting one ENDS the walk.
  A record is therefore resolved at the depth we choose, so a 2^16-node heap
  (393 KB) is enough for /16 granularity - no need for a full 2^32 tree.
"""
import struct
import sys

RECORD_BYTES = 3
NODE_BYTES = 6
DEPTH = 16
# A complete binary tree of DEPTH levels holds 2**DEPTH - 1 nodes, and in heap
# order node i's children are 2i+1 / 2i+2. Using 2**DEPTH instead leaves the
# last level's children out of range, so every lookup stops one level short.
NODE_COUNT = (1 << DEPTH) - 1      # also the "no data" record value


def mm_string(s):
    b = s.encode("utf-8")
    assert len(b) < 29
    return bytes([0x40 | len(b)]) + b


def mm_uint(v, ctrl_type):
    """A uint16/uint32/uint64 whose SIZE IS ITS PAYLOAD WIDTH.

    The size is the low 5 bits of the control byte, so an integer is never
    29 bytes long and never uses the 29/30/31 escape. Widths above 8 bytes
    are not something these tests need.
    """
    b = v.to_bytes(max(1, (v.bit_length() + 7) // 8), "big")
    assert len(b) <= 8
    return bytes([ctrl_type | len(b)]) + b


def mm_uint16(v):
    return mm_uint(v, 0xA0)


def mm_uint32(v):
    return mm_uint(v, 0xC0)


def mm_uint64(v):
    # uint64 is an extended type (9), so the control byte carries the SIZE and
    # the following byte carries the type (9 - 7 = 2).
    b = v.to_bytes(max(1, (v.bit_length() + 7) // 8), "big")
    return bytes([len(b), 0x02]) + b


def mm_map(pairs):
    """pairs: list of (key_string, encoded_value_bytes)."""
    n = len(pairs)
    assert n < 29
    out = bytes([0xE0 | n])
    for k, v in pairs:
        out += mm_string(k) + v
    return out


def country_record(iso, name):
    """{country: {iso_code, names: {en}}} - a plain nested map."""
    return mm_map([
        ("country", mm_map([
            ("iso_code", mm_string(iso)),
            ("names", mm_map([("en", mm_string(name))])),
        ])),
    ])


def build(entries, out_path):
    # ---- data section ------------------------------------------------
    # Record 0 cannot sit at data-section offset 0: a pointer to it would be
    # exactly node_count + 16, which the format reserves as "no data" (those
    # values exist only so a pointer CAN name the section's first byte). One
    # pad byte keeps every record strictly inside the section.
    data = b"\x00"
    offsets = {}
    for _, _, iso, name in entries:
        if iso in offsets:
            continue
        offsets[iso] = len(data)
        data += country_record(iso, name)

    def data_pointer(off):
        return NODE_COUNT + 16 + off

    # ---- tree: every record "no data" unless a /16 claims it ---------
    tree = bytearray(NODE_COUNT * NODE_BYTES)
    for i in range(NODE_COUNT):
        for b in (0, 1):
            child = 2 * i + 1 + b
            rec = child if child < NODE_COUNT else NODE_COUNT
            tree[i * NODE_BYTES + b * RECORD_BYTES + 0] = (rec >> 16) & 0xFF
            tree[i * NODE_BYTES + b * RECORD_BYTES + 1] = (rec >> 8) & 0xFF
            tree[i * NODE_BYTES + b * RECORD_BYTES + 2] = rec & 0xFF

    for ip, _, iso, _ in entries:
        node = 0
        for depth in range(DEPTH):
            bit = (ip >> (31 - depth)) & 1
            child = 2 * node + 1 + bit
            if depth == DEPTH - 1:
                # Leaf of our depth: both halves resolve to the data record,
                # so the walk ends here whatever the last bit was.
                ptr = data_pointer(offsets[iso])
                for b in (0, 1):
                    tree[node * NODE_BYTES + b * RECORD_BYTES + 0] = (ptr >> 16) & 0xFF
                    tree[node * NODE_BYTES + b * RECORD_BYTES + 1] = (ptr >> 8) & 0xFF
                    tree[node * NODE_BYTES + b * RECORD_BYTES + 2] = ptr & 0xFF
                break
            node = child

    # ---- metadata ----------------------------------------------------
    meta = mm_map([
        ("binary_format_major_version", mm_uint16(2)),
        ("binary_format_minor_version", mm_uint16(0)),
        ("build_epoch", mm_uint64(1700000000)),
        ("database_type", mm_string("Country")),
        ("description", mm_string("WinTCP test GeoIP")),
        ("ip_version", mm_uint16(4)),
        ("languages", mm_map([("en", mm_string("en"))])),
        ("node_count", mm_uint32(NODE_COUNT)),
        ("record_size", mm_uint16(24)),
    ])

    with open(out_path, "wb") as f:
        f.write(bytes(tree))
        f.write(b"\x00" * 8 + struct.pack(">Q", len(tree) + 16))
        f.write(data)
        f.write(b"\xab\xcd\xefMaxMind.com")
        f.write(meta)
    print("wrote %s: %d nodes, tree %d bytes, data %d bytes"
          % (out_path, NODE_COUNT, len(tree), len(data)))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "test-country.mmdb"
    def p(a, b, c, d):
        return (a << 24) | (b << 16) | (c << 8) | d
    build([
        # 8.8.8.8 (Google DNS), 13.248.151.210, 52.188.247.144 -> US
        (p(8, 8, 0, 0), p(8, 8, 0, 0), "US", "United States"),
        (p(13, 248, 0, 0), p(13, 248, 0, 0), "US", "United States"),
        (p(52, 188, 0, 0), p(52, 188, 0, 0), "US", "United States"),
        (p(1, 1, 0, 0), p(1, 1, 0, 0), "AU", "Australia"),
        (p(5, 9, 0, 0), p(5, 9, 0, 0), "DE", "Germany"),
    ], out)

