#!/usr/bin/env python3
"""Generate the square brand icons served by the API.

Source artwork: ``app/static/app/logo.png`` (600x600 RGBA). The output is
written next to it, inside the tree served by the ``/app`` static mount, so the
files are baked into the Docker image and reachable both as ``/app/<name>`` and
from the root aliases registered in :mod:`app.main`:

    favicon.ico            multi-size 16/32/48 ICO (tabs, bookmarks, crawlers)
    icon-48.png            48x48
    icon-96.png            96x96
    icon-192.png           192x192 (web manifest)
    icon-512.png           512x512 (web manifest, Organization JSON-LD)
    apple-touch-icon.png   180x180 (iOS home screen)

Downscaling goes through Pillow's premultiplied ``RGBa`` mode: resizing RGBA
channels independently would blend the transparent corners against black and
leave a dark fringe on a LANCZOS reduction.

``apple-touch-icon.png`` is flattened onto the site background instead of
keeping alpha: iOS composites transparency against black anyway, and the
flattened version matches ``<meta name="theme-color">`` exactly.

The application never imports this module. Run it on the host (Pillow is a
build-time tool here, not a runtime dependency) and commit the binaries:

    python scripts/build_brand_icons.py

The output is deterministic, so re-running on unchanged artwork is a no-op.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

SERVER_ROOT = Path(__file__).resolve().parents[1]
APP_STATIC_DIR = SERVER_ROOT / "app" / "static" / "app"
SOURCE = APP_STATIC_DIR / "logo.png"

# The site background from ``<meta name="theme-color">`` in every page head.
BACKGROUND = (7, 11, 16, 255)

ICO_FILENAME = "favicon.ico"
ICO_SIZES = (16, 32, 48)
PNG_ICONS = {
    "icon-48.png": 48,
    "icon-96.png": 96,
    "icon-192.png": 192,
    "icon-512.png": 512,
}
APPLE_TOUCH_ICON = ("apple-touch-icon.png", 180)


def _resize(image: Image.Image, size: int) -> Image.Image:
    """Square LANCZOS reduction that keeps edges free of a dark fringe."""
    if image.size == (size, size):
        return image.copy()
    return (
        image.convert("RGBa")
        .resize((size, size), Image.Resampling.LANCZOS)
        .convert("RGBA")
    )


def _flatten(image: Image.Image) -> Image.Image:
    """Composite RGBA onto the opaque site background."""
    canvas = Image.new("RGBA", image.size, BACKGROUND)
    canvas.alpha_composite(image)
    return canvas.convert("RGB")


def build(source: Path = SOURCE, output_dir: Path = APP_STATIC_DIR) -> list[Path]:
    """(Re)generate every icon from ``source``; return the paths written."""
    if not source.is_file():
        raise SystemExit(f"source artwork is missing: {source}")

    with Image.open(source) as opened:
        opened.load()
        if opened.width != opened.height:
            raise SystemExit(
                f"source artwork must be square, got {opened.width}x{opened.height}"
            )
        original = opened.convert("RGBA")

    written: list[Path] = []

    # ICO: build each frame from the source at its native size. Pillow only
    # downsamples when the requested size is smaller than the image, so the
    # largest frame is generated at exactly 48px and 16/32 are reduced from it.
    ico_base = _resize(original, max(ICO_SIZES))
    ico_path = output_dir / ICO_FILENAME
    ico_base.save(ico_path, format="ICO", sizes=[(size, size) for size in ICO_SIZES])
    written.append(ico_path)

    for filename, size in PNG_ICONS.items():
        path = output_dir / filename
        _resize(original, size).save(path, format="PNG", optimize=True)
        written.append(path)

    filename, size = APPLE_TOUCH_ICON
    path = output_dir / filename
    _flatten(_resize(original, size)).save(path, format="PNG", optimize=True)
    written.append(path)

    return written


if __name__ == "__main__":
    try:
        for written_path in build():
            print(f"{written_path.relative_to(SERVER_ROOT)}")
    except SystemExit as error:  # pragma: no cover - CLI path
        print(error, file=sys.stderr)
        raise
