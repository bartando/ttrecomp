#!/usr/bin/env python3
"""Typeset two clean curved lines for the matte ball print."""

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets/icon/variants/recomp-ball"
FONT = Path("/System/Library/Fonts/Supplemental/Futura.ttc")


def curved_text(image, text, font_size, radius, start, end, bottom=False):
    font = ImageFont.truetype(str(FONT), font_size, index=4)  # Condensed ExtraBold
    ink = (8, 11, 12, 255)
    widths = [font.getlength(letter) for letter in text]
    tracking = 18
    total = sum(widths) + tracking * (len(text) - 1)
    offset = 0
    for letter, width in zip(text, widths):
        fraction = (offset + width / 2) / total
        angle = start + fraction * (end - start)
        offset += width + tracking
        if letter == " ":
            continue
        tile = Image.new("RGBA", (512, 512))
        draw = ImageDraw.Draw(tile)
        bounds = draw.textbbox((0, 0), letter, font=font)
        draw.text(((512 - (bounds[2] - bounds[0])) / 2 - bounds[0],
                   (512 - (bounds[3] - bounds[1])) / 2 - bounds[1]),
                  letter, font=font, fill=ink)
        rotation = 90 - angle if bottom else 270 - angle
        tile = tile.rotate(rotation, Image.Resampling.BICUBIC)
        x = 1024 + radius * math.cos(math.radians(angle))
        y = 1024 + radius * math.sin(math.radians(angle))
        image.alpha_composite(tile, (round(x - 256), round(y - 256)))


def main():
    image = Image.new("RGBA", (2048, 2048))
    curved_text(image, "TABLE TENNIS", 250, 720, 204, 336)
    curved_text(image, "RECOMP", 330, 680, 145, 35, bottom=True)
    OUT.mkdir(parents=True, exist_ok=True)
    image.save(OUT / "recomp_ball_print.png", optimize=True)
    print(OUT / "recomp_ball_print.png")


if __name__ == "__main__":
    main()
