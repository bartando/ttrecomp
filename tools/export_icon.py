#!/usr/bin/env python3
"""Export the vivid icon renders to the desktop PNG/ICO and the PS5 icon0.png.

Requires Pillow. The macOS packaging script creates ICNS from the exported PNG.
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def load_square(path):
    image = Image.open(path)
    if image.width != image.height or image.mode != "RGBA":
        raise ValueError(f"{path} must be square with RGBA transparency")
    return image


def main():
    directory = ROOT / "assets" / "icon"
    with load_square(directory / "tabletennis_icon_vivid_master.png") as source:
        icon = source.resize((1024, 1024), Image.Resampling.LANCZOS)
    png = directory / "tabletennis_icon_vivid.png"
    ico = png.with_suffix(".ico")
    icon.save(png, optimize=True)
    icon.save(ico, sizes=[(size, size) for size in (16, 24, 32, 48, 64, 128, 256)])
    # The PS5 home screen masks tiles to its own rounded shape; any margin in the
    # PNG shows up as padding, so this one is full-bleed and opaque.
    with load_square(directory / "tabletennis_icon_vivid_square.png") as source:
        tile = source.convert("RGB").resize((512, 512), Image.Resampling.LANCZOS)
    ps5 = ROOT / "ps5" / "game" / "icon0.png"
    tile.save(ps5, optimize=True)
    for path in (png, ico, ps5):
        print(path)


if __name__ == "__main__":
    main()
