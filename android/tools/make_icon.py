#!/usr/bin/env python3
"""Draws the PhoneCam launcher icon (a lens on a rounded green square)."""
import sys
from PIL import Image, ImageDraw

S = 4  # supersample
N = 192
img = Image.new("RGBA", (N * S, N * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([8 * S, 8 * S, (N - 8) * S, (N - 8) * S], radius=44 * S, fill=(22, 26, 34, 255))
c = N * S // 2
for r, col in ((62, (76, 201, 140)), (50, (22, 26, 34)), (38, (40, 120, 200)), (20, (12, 16, 24))):
    d.ellipse([c - r * S, c - r * S, c + r * S, c + r * S], fill=col + (255,))
d.ellipse([c - 30 * S, c - 30 * S, c - 16 * S, c - 16 * S], fill=(255, 255, 255, 200))
d.ellipse([(N - 52) * S, 24 * S, (N - 36) * S, 40 * S], fill=(235, 80, 80, 255))
img.resize((N, N), Image.LANCZOS).save(sys.argv[1])
