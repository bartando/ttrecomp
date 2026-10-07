#!/usr/bin/env python3
"""Draws the app icon: a paddle and ball on a table-green tile.

Original artwork, deliberately not based on the game's own icon. Writes
assets/icon/tabletennis_icon.png (1024x1024) and the Windows
assets/icon/tabletennis_icon.ico; needs Pillow.
"""

import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
SCALE = 4  # Supersampling factor for smooth edges.
OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "icon" / "tabletennis_icon.png"


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def vertical_gradient(size, top, bottom):
    image = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        draw.line([(0, y), (size, y)], fill=lerp(top, bottom, y / (size - 1)) + (255,))
    return image


def rotated_polygon(points, center, degrees):
    angle = math.radians(degrees)
    cx, cy = center
    c, s = math.cos(angle), math.sin(angle)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in points]


def shadow_layer(size, draw_shape, blur, offset, opacity):
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw_shape(ImageDraw.Draw(layer), (0, 0, 0, opacity))
    layer = layer.filter(ImageFilter.GaussianBlur(blur))
    shifted = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shifted.paste(layer, offset, layer)
    return shifted


def main():
    n = SIZE * SCALE
    u = n / 1024  # One unit in 1024-space.

    # macOS-style rounded tile with a margin, so the icon sits like system ones.
    margin, radius = 100 * u, 185 * u
    tile_box = [margin, margin, n - margin, n - margin]
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(tile_box, radius=radius, fill=255)

    tile = vertical_gradient(n, (34, 139, 92), (14, 78, 54))
    lines = ImageDraw.Draw(tile)
    line_width = 14 * u
    inset = margin + 46 * u
    lines.rounded_rectangle([inset, inset, n - inset, n - inset], radius=radius - 46 * u,
                            outline=(236, 246, 240, 235), width=round(line_width))
    lines.line([(n / 2, inset), (n / 2, n - inset)], fill=(236, 246, 240, 200),
               width=round(line_width * 0.7))

    icon = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    icon.paste(tile, (0, 0), mask)

    # Paddle: a rubber face on a wooden handle, tilted.
    face_center = (440 * u, 480 * u)
    face_radius = 200 * u
    tilt = -38
    handle = rotated_polygon(
        [(face_center[0] - 46 * u, face_center[1] + face_radius - 30 * u),
         (face_center[0] + 46 * u, face_center[1] + face_radius - 30 * u),
         (face_center[0] + 40 * u, face_center[1] + face_radius + 215 * u),
         (face_center[0] - 40 * u, face_center[1] + face_radius + 215 * u)],
        face_center, tilt)

    def draw_paddle(draw, color):
        draw.polygon(handle, fill=color)
        draw.ellipse([face_center[0] - face_radius, face_center[1] - face_radius,
                      face_center[0] + face_radius, face_center[1] + face_radius], fill=color)

    icon.alpha_composite(shadow_layer(n, draw_paddle, 28 * u, (round(18 * u), round(26 * u)), 110))
    paddle = ImageDraw.Draw(icon)
    paddle.polygon(handle, fill=(196, 140, 84))
    grip = rotated_polygon(
        [(face_center[0] - 40 * u, face_center[1] + face_radius + 60 * u),
         (face_center[0] + 40 * u, face_center[1] + face_radius + 60 * u),
         (face_center[0] + 40 * u, face_center[1] + face_radius + 215 * u),
         (face_center[0] - 40 * u, face_center[1] + face_radius + 215 * u)],
        face_center, tilt)
    paddle.polygon(grip, fill=(150, 98, 52))
    rim = face_radius + 12 * u
    paddle.ellipse([face_center[0] - rim, face_center[1] - rim,
                    face_center[0] + rim, face_center[1] + rim], fill=(120, 24, 28))
    face = vertical_gradient(n, (236, 66, 58), (176, 28, 34))
    face_mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(face_mask).ellipse([face_center[0] - face_radius, face_center[1] - face_radius,
                                       face_center[0] + face_radius, face_center[1] + face_radius],
                                      fill=255)
    icon.paste(face, (0, 0), face_mask)
    highlight = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(highlight).ellipse(
        [face_center[0] - face_radius * 0.72, face_center[1] - face_radius * 0.82,
         face_center[0] + face_radius * 0.18, face_center[1] - face_radius * 0.05],
        fill=(255, 255, 255, 70))
    icon.alpha_composite(highlight.filter(ImageFilter.GaussianBlur(30 * u)))

    # Ball.
    ball_center, ball_radius = (735 * u, 300 * u), 84 * u

    def draw_ball(draw, color):
        draw.ellipse([ball_center[0] - ball_radius, ball_center[1] - ball_radius,
                      ball_center[0] + ball_radius, ball_center[1] + ball_radius], fill=color)

    icon.alpha_composite(shadow_layer(n, draw_ball, 22 * u, (round(14 * u), round(22 * u)), 120))
    ball = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ball_draw = ImageDraw.Draw(ball)
    steps = 48
    for i in range(steps):
        t = i / (steps - 1)
        r = ball_radius * (1 - 0.82 * t)
        cx = ball_center[0] - ball_radius * 0.32 * t
        cy = ball_center[1] - ball_radius * 0.36 * t
        ball_draw.ellipse([cx - r, cy - r, cx + r, cy + r],
                          fill=lerp((250, 168, 64), (255, 236, 196), t) + (255,))
    icon.alpha_composite(ball)

    icon = icon.resize((SIZE, SIZE), Image.LANCZOS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    icon.save(OUT)
    ico = OUT.with_suffix(".ico")
    icon.save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print(OUT)
    print(ico)


if __name__ == "__main__":
    sys.exit(main())
