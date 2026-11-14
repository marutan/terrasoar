#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds terrasoar.ico (the Windows program and installer icon) from the
PNG renders in png/ (16 to 256 px, PNG-compressed entries, as Windows
Vista and later read them). Run after render_icons.py; commit the result.

Usage: make_ico.py
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 24, 32, 48, 64, 128, 256]


def main():
    images = []
    for size in SIZES:
        with open(os.path.join(HERE, "png", f"terrasoar-{size}.png"),
                  "rb") as stream:
            images.append((size, stream.read()))
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    for size, data in images:
        # Width and height 0 mean 256.
        dimension = size if size < 256 else 0
        entries += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32,
                               len(data), offset)
        offset += len(data)
    path = os.path.join(HERE, "terrasoar.ico")
    with open(path, "wb") as stream:
        stream.write(header + entries + b"".join(data for _, data in images))
    print(path)


if __name__ == "__main__":
    main()
