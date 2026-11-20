"""A character's hand-painted layers over the game's textures (build_characters.py, "overlay" in character.json): e.g.
Xavier's fur where Gene's gauntlet arm comes out of his stump, painted over the arm skin.

overlay(base_rgba, layer_rgba) -> the layer (an RGBA image, any size: scaled to the base) laid over the base by its alpha.
overlay_textures(game_texs, layers) -> {name: new tim3 bytes}: the game's named 16-colour textures with their layers
    on, requantized to 16 colours in the game's size and layout.
"""
import math
import struct

import numpy as np
from PIL import Image


def overlay(base_rgba, layer_rgba):
    h, w = base_rgba.shape[:2]
    layer = Image.fromarray(np.ascontiguousarray(layer_rgba), 'RGBA')
    if layer.size != (w, h):
        # premultiplied, so the transparent texels' colours do not bleed into the edge
        pre = np.asarray(layer).astype(np.float32)
        pre[..., :3] *= pre[..., 3:4] / 255.0
        small = np.stack([np.asarray(Image.fromarray(pre[..., c]).resize((w, h), Image.LANCZOS)) for c in range(4)], -1)
        a = np.clip(small[..., 3:4], 0, 255) / 255.0
        rgb = np.where(a > 1e-4, small[..., :3] / np.maximum(a, 1e-4), 0.0)
    else:
        arr = np.asarray(layer).astype(np.float32)
        a = arr[..., 3:4] / 255.0
        rgb = arr[..., :3]
    out = base_rgba.astype(np.float32)
    out[..., :3] = out[..., :3] * (1.0 - a) + np.clip(rgb, 0, 255) * a
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def quantize16(rgba):
    """16-colour indices and palette (RGBA, alpha 255) of an opaque image (at most 512x512): a median cut (the box with
    the largest squared error is split at its population's median along its widest channel, the first channel of
    R, G, B on ties; each box's colour is its rounded mean), then each pixel's nearest colour (the first on ties).
    src/kamite/character_build.cpp quantize16 does the same step for step (the game builds characters at boot), so the
    float steps here are exact (sums of squares below 2^53) and the ties are spelt out."""
    rgb = np.ascontiguousarray(rgba[..., :3])
    px = rgb.reshape(-1, 3).astype(np.int64)
    keys, counts = np.unique((px[:, 0] << 16) | (px[:, 1] << 8) | px[:, 2], return_counts=True)
    boxes = [[((int(k) >> 16) & 255, (int(k) >> 8) & 255, int(k) & 255, int(c)) for k, c in zip(keys, counts)]]
    while len(boxes) < 16:
        best, bi = -1.0, -1
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            n = sum(c[3] for c in b)
            s = 0.0
            for q in range(3):
                s1 = sum(c[q] * c[3] for c in b)
                s2 = sum(c[q] * c[q] * c[3] for c in b)
                s += float(s2) - float(s1 * s1) / float(n)
            if s > best:
                best, bi = s, i
        if bi < 0:
            break
        b = boxes[bi]
        n = sum(c[3] for c in b)
        rng = [max(c[q] for c in b) - min(c[q] for c in b) for q in range(3)]
        ch = rng.index(max(rng))
        b = sorted(b, key=lambda c: (c[ch], c[0], c[1], c[2]))
        cut, cum = len(b) - 1, 0
        for i in range(1, len(b)):
            cum += b[i - 1][3]
            if 2 * cum >= n:
                cut = i
                break
        boxes[bi] = b[:cut]
        boxes.append(b[cut:])
    pal = []
    for b in boxes:
        n = sum(c[3] for c in b)
        pal.append(tuple((sum(c[q] * c[3] for c in b) + n // 2) // n for q in range(3)) + (255,))
    while len(pal) < 16:
        pal.append(pal[-1])
    pal = np.array(pal, np.uint8)
    d = ((rgb[:, :, None, :].astype(int) - pal[None, None, :, :3].astype(int)) ** 2).sum(-1)
    return np.argmin(d, -1).astype(np.uint8), pal


def tim3_encode(idx, pal, w, h):
    """A 4-bit TIM3 in the game's layout (as encode_xavier.py writes them)."""
    from gametex import swizzle4
    idx = np.asarray(idx, np.uint8)
    if w >= 128 and h >= 128:
        data = swizzle4(idx, w, h)
        fid = 6
    else:
        f = idx.ravel()
        data = bytes((f[0::2] | (f[1::2] << 4)).astype(np.uint8))
        fid = 0
    clut = bytearray()
    for r, g, b, a in pal:
        clut += bytes((int(r), int(g), int(b), int(round(int(a) * 128 / 255))))
    tbw = max(2, w // 64)
    tw = int(math.log2(w))
    th = int(math.log2(h))
    tex0 = (tbw << 14) | (20 << 20) | (tw << 26) | (th << 30)
    pic = struct.pack('<3I2H4B2H2Q2I', 48 + len(clut) + len(data), len(clut), len(data), 48, 16, 0, 1, 3, 4, w, h, tex0, 0x260, 0, 0)
    return b'TIM3' + bytes((4, fid, 1, 0)) + bytes(8) + pic + data + bytes(clut)


def overlay_textures(game_texs, layers):
    from gametex import tim3_decode
    out = {}
    for name, t in game_texs:
        if name in layers:
            rgba, info = tim3_decode(t)
            idx, pal = quantize16(overlay(rgba, layers[name]))
            out[name] = tim3_encode(idx, pal, info['w'], info['h'])
    return out
