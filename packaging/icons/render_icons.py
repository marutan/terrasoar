#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Renders packaging/icons/terrasoar.svg to the PNG sizes the program and
the packages use (png/terrasoar-<size>.png), with librsvg and cairo.

Run it after changing the SVG, and commit the PNGs: builds don't need
librsvg. Usage: render_icons.py
"""

import ctypes
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 24, 32, 48, 64, 128, 256, 512]


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]


def main():
    rsvg = ctypes.CDLL("librsvg-2.so.2")
    cairo = ctypes.CDLL("libcairo.so.2")
    rsvg.rsvg_handle_new_from_file.restype = ctypes.c_void_p
    rsvg.rsvg_handle_new_from_file.argtypes = [ctypes.c_char_p,
                                               ctypes.c_void_p]
    rsvg.rsvg_handle_render_document.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(Rect),
        ctypes.c_void_p]
    cairo.cairo_image_surface_create.restype = ctypes.c_void_p
    cairo.cairo_create.restype = ctypes.c_void_p
    cairo.cairo_create.argtypes = [ctypes.c_void_p]
    cairo.cairo_surface_write_to_png.argtypes = [ctypes.c_void_p,
                                                 ctypes.c_char_p]
    cairo.cairo_destroy.argtypes = [ctypes.c_void_p]
    cairo.cairo_surface_destroy.argtypes = [ctypes.c_void_p]

    svg = os.path.join(HERE, "terrasoar.svg")
    handle = rsvg.rsvg_handle_new_from_file(svg.encode(), None)
    if not handle:
        raise SystemExit(f"Cannot read {svg}")
    out = os.path.join(HERE, "png")
    os.makedirs(out, exist_ok=True)
    for size in SIZES:
        surface = cairo.cairo_image_surface_create(0, size, size)  # ARGB32
        context = cairo.cairo_create(surface)
        viewport = Rect(0, 0, size, size)
        if not rsvg.rsvg_handle_render_document(handle, context,
                                                ctypes.byref(viewport), None):
            raise SystemExit(f"Cannot render {svg}")
        path = os.path.join(out, f"terrasoar-{size}.png")
        cairo.cairo_surface_write_to_png(surface, path.encode())
        cairo.cairo_destroy(context)
        cairo.cairo_surface_destroy(surface)
        print(path)


if __name__ == "__main__":
    main()
