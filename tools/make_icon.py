#!/usr/bin/env python3
"""Turn the artwork in ico/ into the multi-size colaimdation.ico.

Windows picks a different icon size for every place it draws one (16 px in the
title bar and the taskbar, 32 px in Alt+Tab, 48 px in Explorer's medium view,
256 px in the extra-large view), so a single-size .ico looks blurry or scaled
somewhere. This writes one file holding all of them.

Every entry is a classic BMP/DIB image. PNG-compressed entries are smaller, but
only the Vista+ shell reads them: `LoadImage` and several libraries (including
System.Drawing, which is what quick inspection scripts use) skip them, and the
extra-large icon then silently falls back to 128 px.

Usage:  python tools/make_icon.py [source.png] [output.ico]
"""
import os
import struct
import sys

from PIL import Image

SIZES = [16, 24, 32, 48, 64, 128, 256]


def ico_entries(path):
    """Reads an .ico and returns [(width, height, blob)] for each image."""
    with open(path, "rb") as handle:
        data = handle.read()
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1:
        raise ValueError("%s is not an icon file" % path)
    entries = []
    for index in range(count):
        offset = 6 + index * 16
        width, height, _colors, _reserved, _planes, _bpp, size, at = struct.unpack_from(
            "<BBBBHHII", data, offset)
        entries.append((width or 256, height or 256, data[at:at + size]))
    return entries


def write_ico(path, entries):
    header = struct.pack("<HHH", 0, 1, len(entries))
    directory = b""
    offset = 6 + 16 * len(entries)
    for width, height, blob in entries:
        directory += struct.pack(
            "<BBBBHHII",
            width if width < 256 else 0,
            height if height < 256 else 0,
            0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as handle:
        handle.write(header + directory + b"".join(blob for _w, _h, blob in entries))


def default_source(root):
    """The artwork to convert: the usual name first, else any PNG in ico/."""
    icons = os.path.join(root, "ico")
    preferred = os.path.join(icons, "无标题.png")
    if os.path.exists(preferred):
        return preferred
    candidates = sorted(name for name in os.listdir(icons)
                        if name.lower().endswith(".png"))
    if not candidates:
        raise SystemExit("no .png artwork found in %s" % icons)
    return os.path.join(icons, candidates[0])


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source = sys.argv[1] if len(sys.argv) > 1 else default_source(root)
    target = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "ico", "colaimdation.ico")
    if not os.path.exists(source):
        raise SystemExit("source image not found: %s" % source)

    image = Image.open(source).convert("RGBA")
    # Only sizes the artwork can actually fill: upscaling a small source would just
    # hand Windows a blurry icon in the sizes that matter most.
    wanted = [(size, size) for size in SIZES
              if size <= max(image.width, image.height)]
    if not wanted:
        raise SystemExit("source image is smaller than 16 px: %s" % source)

    temporary = target + ".part.ico"
    image.save(temporary, format="ICO", sizes=wanted, bitmap_format="bmp")
    entries = ico_entries(temporary)
    os.remove(temporary)

    write_ico(target, entries)

    print("%s -> %s" % (os.path.relpath(source, root), os.path.relpath(target, root)))
    print("  sizes: %s" % ", ".join("%dx%d" % (w, h) for w, h, _blob in entries))
    print("  bytes: %d" % os.path.getsize(target))


if __name__ == "__main__":
    main()
