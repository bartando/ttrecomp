#!/usr/bin/env python3
"""Report skin brightness from a repro.sh character crop, plus a reference.

face  - the character's face; the bug makes this ~0 while the rest is fine.
shirt - the (correctly rendered) jersey. If this is also ~0 the run never
        reached the character-select screen, so `face` means nothing.
"""
import sys
from PIL import Image


def mean(im, box):
    px = list(im.crop(box).getdata())
    n = len(px)
    return tuple(sum(q[i] for q in px) / n for i in range(3))


for p in sys.argv[1:]:
    im = Image.open(p).convert("RGB")
    W, H = im.size
    f = mean(im, (int(W * .14), int(H * .25), int(W * .29), int(H * .38)))
    s = mean(im, (int(W * .22), int(H * .45), int(W * .34), int(H * .60)))
    ok = "" if sum(s) / 3 > 12 else "   <-- screen not reached, ignore"
    print(f"{p.split('/')[-1]:30} face={sum(f)/3:6.1f}  shirt={sum(s)/3:6.1f}{ok}")
