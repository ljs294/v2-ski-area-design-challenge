"""Generate the immutable class-value COG-shaped fixture without test-process tag setup."""
from __future__ import annotations

import base64
import hashlib
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT, TILE = 19, 18, 16
CLASSES = (10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100)


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

    scale = reserve(struct.pack("<3d", 1 / 36000, 1 / 36000, 0.0), 8)
    tie = reserve(struct.pack("<6d", 0, 0, 0, -121.5, 47.0, 0), 8)
    keys = reserve(struct.pack("<12H", 1, 1, 0, 2, 1024, 0, 1, 2, 2048, 0, 1, 4326))
    nodata = reserve(b"0\0", 1)
    offsets_slot = reserve(bytes(16), 4)
    counts_slot = reserve(bytes(16), 4)
    tile_offsets: list[int] = []
    tile_counts: list[int] = []
    for tile_y in (0, 16):
        for tile_x in (0, 16):
            samples = bytearray()
            for y in range(tile_y, tile_y + TILE):
                for x in range(tile_x, tile_x + TILE):
                    samples.append(CLASSES[(x + y) % len(CLASSES)] if x < WIDTH and y < HEIGHT else 0)
            tile_offsets.append(reserve(bytes(samples), 4))
            tile_counts.append(len(samples))
    for index, (offset, data) in enumerate(blobs):
        if offset == offsets_slot:
            blobs[index] = (offset, struct.pack("<4I", *tile_offsets))
        elif offset == counts_slot:
            blobs[index] = (offset, struct.pack("<4I", *tile_counts))

    def short(value: int) -> bytes:
        return struct.pack("<H", value) + bytes(2)

    def long(value: int) -> bytes:
        return struct.pack("<I", value)

    entries = [
        (256, 4, 1, long(WIDTH)), (257, 4, 1, long(HEIGHT)), (258, 3, 1, short(8)),
        (259, 3, 1, short(1)), (262, 3, 1, short(1)), (274, 3, 1, short(1)),
        (277, 3, 1, short(1)), (284, 3, 1, short(1)), (322, 4, 1, long(TILE)),
        (323, 4, 1, long(TILE)), (324, 4, 4, long(offsets_slot)),
        (325, 4, 4, long(counts_slot)), (339, 3, 1, short(1)),
        (33550, 12, 3, long(scale)), (33922, 12, 6, long(tie)),
        (34735, 3, 16, long(keys)),
        # Two ASCII bytes fit in the entry, so TIFF stores them inline rather than by offset.
        (42113, 2, 2, b"0\x00" + bytes(2)),
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
    destination = root / "Content/P1Fixtures/worldcover-class-cog-synthetic.tif.base64"
    data = build_fixture()
    encoded = base64.b64encode(data).decode("ascii") + "\n"
    if "--write" in sys.argv:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(encoded, encoding="ascii", newline="\n")
    elif not destination.is_file() or destination.read_text(encoding="ascii") != encoded:
        raise SystemExit("fixture differs; run with --write and review the generated fixture")
    print(hashlib.sha256(data).hexdigest())
