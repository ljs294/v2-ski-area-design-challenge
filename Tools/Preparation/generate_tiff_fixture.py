"""Generate the immutable synthetic tiled GeoTIFF regression fixture as base64."""
from __future__ import annotations

import base64
import hashlib
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT, TILE = 19, 18, 16


def build_fixture() -> bytes:
    blobs: list[tuple[int, bytes]] = []
    cursor = 8 + 2 + 17 * 12 + 4

    def reserve(data: bytes, alignment: int = 2) -> int:
        nonlocal cursor
        cursor += (-cursor) % alignment
        offset = cursor
        cursor += len(data)
        blobs.append((offset, data))
        return offset

    scale = reserve(struct.pack("<3d", 0.001, 0.001, 0.0), 8)
    tie = reserve(struct.pack("<6d", 0, 0, 0, -121.5, 47.0, 0), 8)
    keys = reserve(struct.pack("<12H", 1, 1, 0, 2, 1024, 0, 1, 2, 2048, 0, 1, 4326))
    nodata = reserve(b"-9999\0", 1)
    offsets_slot = reserve(bytes(16), 4)
    counts_slot = reserve(bytes(16), 4)
    tile_offsets: list[int] = []
    for tile_y in (0, 16):
        for tile_x in (0, 16):
            samples = [float(y * 1000 + x) if x < WIDTH and y < HEIGHT else -9999.0
                       for y in range(tile_y, tile_y + TILE)
                       for x in range(tile_x, tile_x + TILE)]
            tile_offsets.append(reserve(struct.pack("<256f", *samples), 4))
    for index, (offset, data) in enumerate(blobs):
        if offset == offsets_slot:
            blobs[index] = (offset, struct.pack("<4I", *tile_offsets))
        elif offset == counts_slot:
            blobs[index] = (offset, struct.pack("<4I", *([1024] * 4)))

    def short(value: int) -> bytes:
        return struct.pack("<H", value) + bytes(2)

    def long(value: int) -> bytes:
        return struct.pack("<I", value)

    entries = [
        (256, 4, 1, long(WIDTH)), (257, 4, 1, long(HEIGHT)), (258, 3, 1, short(32)),
        (259, 3, 1, short(1)), (262, 3, 1, short(1)), (274, 3, 1, short(1)),
        (277, 3, 1, short(1)), (284, 3, 1, short(1)), (322, 4, 1, long(TILE)),
        (323, 4, 1, long(TILE)), (324, 4, 4, long(offsets_slot)),
        (325, 4, 4, long(counts_slot)), (339, 3, 1, short(3)),
        (33550, 12, 3, long(scale)), (33922, 12, 6, long(tie)),
        (34735, 3, 12, long(keys)), (42113, 2, 6, long(nodata)),
    ]
    entries.sort()
    output = bytearray(cursor)
    output[:8] = b"II" + struct.pack("<HI", 42, 8)
    output[8:10] = struct.pack("<H", len(entries))
    position = 10
    for tag, kind, count, value in entries:
        output[position:position + 12] = struct.pack("<HHI", tag, kind, count) + value
        position += 12
    for offset, data in blobs:
        output[offset:offset + len(data)] = data
    return bytes(output)


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[2]
    destination = root / "Content/P1Fixtures/usgs-tiled-nodata-synthetic.tif.base64"
    data = build_fixture()
    encoded = base64.b64encode(data).decode("ascii") + "\n"
    if "--write" in sys.argv:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(encoded, encoding="ascii", newline="\n")
    elif not destination.is_file() or destination.read_text(encoding="ascii") != encoded:
        raise SystemExit("fixture differs; run with --write and review the generated fixture")
    print(hashlib.sha256(data).hexdigest())
    print(encoded, end="")
