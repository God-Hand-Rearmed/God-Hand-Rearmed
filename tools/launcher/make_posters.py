"""The first-run wanted posters (K, Nov 16; claude/presets-design.md): one per preset, baked once into
resources/presets for the launcher.

python3 tools/launcher/make_posters.py

Per preset N (0 Classic .. 3 Sleazy Mode): poster_N.png (procedural weathered paper with a ragged burned edge, the
title in Wendyville, a rule, the photo's soft shadow, the tagline and what the preset adds in Dominican (the splash
font's atlas) as patchy letterpress ink, tape on the photo's four corners); poster_N_tape.png (the tape over the photo
box only, drawn over the photo); poster_N_shadow.png (the poster's drop shadow, padded by SHADOW_PAD). stamp.png: the
red SELECTED stamp. posters.txt: per poster its photo box and the stamp's centre, in the poster's pixels. The photos
are the player's disc's (presets.cpp decodes them at run time) and never shipped. Fixed seeds: the same posters each run.
Fonts: Wendyville (Sharkshock, docs/licenses/Wendyville-readme.txt) from K's wendyville.zip (WENDYVILLE=path to the
.ttf), Holtwood One SC (OFL) for the stamp (HOLTWOOD=path to the .ttf); text from src/kamite/presets.cpp.
"""
import os, re
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(R, 'resources/presets')
INK = (46, 30, 20)
W, H = 600, 960            # 1.5 x the 400 x 640 the launcher draws at 1080p
PHOTO_ASPECT = 295 / 203
SHADOW_PAD = 40
SEEDS = (10, 11, 12, 13)


def value_noise(w, h, cell, rng):
    gw, gh = w // cell + 3, h // cell + 3
    g = rng.random((gh, gw)).astype(np.float32)
    im = Image.fromarray((g * 255).astype(np.uint8)).resize((gw * cell, gh * cell), Image.BICUBIC)
    return np.asarray(im, np.float32)[:h, :w] / 255.0


def fractal(w, h, rng, base=256, octaves=6):
    out = np.zeros((h, w), np.float32); amp = 1.0; tot = 0.0; cell = base
    for _ in range(octaves):
        out += amp * value_noise(w, h, max(cell, 1), rng); tot += amp; amp *= 0.5; cell //= 2
    return out / tot


def paper(w, h, seed):
    rng = np.random.default_rng(seed)
    base = np.array([224, 203, 158], np.float32)
    blot = fractal(w, h, rng, 160) - 0.5            # broad mottling
    grain = rng.normal(0, 1, (h, w)).astype(np.float32)
    grain = np.asarray(Image.fromarray(np.clip(grain * 40 + 128, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6)), np.float32) / 255 - 0.5
    shade = 1.0 + 0.22 * blot + 0.08 * grain
    # stains: a few soft rings / blotches
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    for _ in range(rng.integers(2, 5)):
        cx, cy, r = rng.random() * w, rng.random() * h, (0.08 + rng.random() * 0.22) * min(w, h)
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
        ring = np.exp(-((d - 1.0) ** 2) / 0.01) * 0.035 + np.exp(-(d ** 2) / 0.6) * 0.06
        shade -= ring * (0.5 + rng.random())
    # fibres
    fib = Image.new('L', (w, h), 0); d = ImageDraw.Draw(fib)
    for _ in range(int(w * h / 900)):
        x, y = rng.random() * w, rng.random() * h; a = rng.random() * np.pi; l = 4 + rng.random() * 14
        d.line((x, y, x + np.cos(a) * l, y + np.sin(a) * l), fill=int(30 + rng.random() * 50), width=1)
    shade -= np.asarray(fib.filter(ImageFilter.GaussianBlur(0.4)), np.float32) / 255 * 0.12
    # a fold or two
    for _ in range(rng.integers(0, 2)):
        if rng.random() < 0.5:
            y0 = h * (0.3 + rng.random() * 0.4); dist = np.abs(yy - y0)
        else:
            x0 = w * (0.3 + rng.random() * 0.4); dist = np.abs(xx - x0)
        shade -= np.exp(-(dist ** 2) / 4.0) * 0.10; shade += np.exp(-((dist - 2.5) ** 2) / 3.0) * 0.04
    # edges age darker
    ex = np.minimum(xx, w - 1 - xx) / (0.18 * w); ey = np.minimum(yy, h - 1 - yy) / (0.18 * h)
    shade *= 0.82 + 0.18 * np.clip(np.minimum(ex, ey), 0, 1) ** 0.7
    rgb = np.clip(base[None, None, :] * shade[..., None], 0, 255)
    return rgb, rng


def burn_edge(rgb, rng, rag=14, burn=18):
    h, w, _ = rgb.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dist = np.minimum(np.minimum(xx, w - 1 - xx), np.minimum(yy, h - 1 - yy))
    n = fractal(w, h, rng, 48, 5)
    edge = rag * (0.25 + 1.1 * n)                     # how far in the paper is gone
    inside = dist - edge
    alpha = np.clip(inside / 1.2, 0, 1)
    # burned band: from the torn edge inward, black -> dark brown -> paper
    n2 = fractal(w, h, rng, 24, 4)
    t = np.clip(inside / (burn * (0.5 + n2)), 0, 1)
    char = np.array([18, 10, 6], np.float32); scorch = np.array([110, 62, 28], np.float32)
    mix1 = np.clip(t * 2.2, 0, 1)[..., None]; mix2 = np.clip(t * 1.4 - 0.4, 0, 1)[..., None]
    col = char * (1 - mix1) + scorch * mix1
    out = col * (1 - mix2) + rgb * mix2
    return np.dstack([out, alpha * 255]).astype(np.uint8)


def ink_texture(mask_img, rng):
    """Letterpress: the ink a little patchy."""
    m = np.asarray(mask_img, np.float32) / 255
    w, h = mask_img.size
    patch = 0.78 + 0.22 * fractal(w, h, rng, 12, 3)
    speck = (rng.random((h, w)) > 0.012).astype(np.float32)
    return Image.fromarray(np.clip(m * patch * speck * 255, 0, 255).astype(np.uint8))


class BMFont:
    def __init__(self, path):
        self.chars = {}
        for line in open(path):
            if line.startswith('char '):
                kv = dict(p.split('=') for p in line.split()[1:])
                self.chars[int(kv['id'])] = {k: int(v) for k, v in kv.items() if k != 'id'}
            elif line.startswith('common'):
                kv = dict(p.split('=') for p in line.split()[1:] if '=' in p)
                self.line, self.base = int(kv['lineHeight']), int(kv['base'])
        self.atlas = Image.open(path.rsplit('/', 1)[0] + '/splash.png').convert('RGBA').getchannel('A')
        self.size = 128

    def width(self, text, px):
        s = px / self.size
        return sum(self.chars.get(ord(c), self.chars[63])['xadvance'] for c in text.replace('\u00a0', ' ')) * s

    def draw(self, mask, x, y, text, px):
        """Into an L mask at (x, top y), cap ~ px."""
        s = px / self.size
        for c in text:
            g = self.chars.get(ord(c), self.chars[63])
            if g['width'] and g['height']:
                gl = self.atlas.crop((g['x'], g['y'], g['x'] + g['width'], g['y'] + g['height']))
                gl = gl.resize((max(1, round(g['width'] * s)), max(1, round(g['height'] * s))), Image.LANCZOS)
                mask.paste(Image.fromarray(np.maximum(np.asarray(mask.crop((round(x + g['xoffset'] * s), round(y + g['yoffset'] * s), round(x + g['xoffset'] * s) + gl.width, round(y + g['yoffset'] * s) + gl.height))), np.asarray(gl))),
                           (round(x + g['xoffset'] * s), round(y + g['yoffset'] * s)))
            x += g['xadvance'] * s

    def wrap(self, text, px, width):
        """As few lines as a greedy wrap needs, the words shared out so the lines are as even as possible (K, Nov 16:
        no word left alone on a line). '&' and ' - ' stay with the words either side ("Misqu\u00e9 & Risqu\u00e9")."""
        words = text.replace(' & ', '\u00a0&\u00a0').replace(' - ', '\u00a0-\u00a0').split(' ')
        count, line = 1, ''
        for wd in words:
            t = (line + ' ' + wd).strip()
            if self.width(t, px) > width and line:
                count += 1; line = wd
            else:
                line = t
        n = len(words)
        span = lambda i, j: self.width(' '.join(words[i:j]), px)
        best = {(n, 0): (0.0, [])}  # (from word, lines left) -> (the widest line, the cut points)

        def solve(i, left):
            if (i, left) in best:
                return best[(i, left)]
            out = (float('inf'), [])
            if left > 0:
                for j in range(i + 1, n - left + 2):
                    w = span(i, j)
                    if w > width and j > i + 1:
                        break
                    rest = solve(j, left - 1)
                    if max(w, rest[0]) < out[0]:
                        out = (max(w, rest[0]), [j] + rest[1])
            best[(i, left)] = out
            return out

        cuts = solve(0, count)[1]
        lines, i = [], 0
        for j in cuts:
            lines.append(' '.join(words[i:j]).replace('\u00a0', ' ')); i = j
        return lines




def tape_strip(length, width, rng, angle):
    t = Image.new('RGBA', (length, width), (0, 0, 0, 0))
    a = np.zeros((width, length), np.float32) + 0.72
    a += (fractal(length, width, rng, 8, 3) - 0.5) * 0.15
    # torn ends: jagged
    for x0, dirn in ((0, 1), (length - 1, -1)):
        for yy in range(width):
            cut = int(rng.integers(0, max(2, width // 5)))
            for k in range(cut):
                a[yy, x0 + dirn * k] = 0
    col = np.zeros((width, length, 4), np.uint8)
    col[..., 0], col[..., 1], col[..., 2] = 232, 214, 160
    # a faint sheen across the strip
    sheen = np.linspace(0, 1, width)[:, None]
    col[..., :3] = np.clip(col[..., :3] + (np.exp(-((sheen - 0.3) ** 2) / 0.01) * 25)[..., None], 0, 255).astype(np.uint8)
    col[..., 3] = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    t = Image.fromarray(col, 'RGBA')
    return t.rotate(angle, expand=True, resample=Image.BICUBIC)




DOM = BMFont(os.path.join(R, 'resources/splash/splash.fnt'))


def preset_text():
    """K's title, tagline and band for each preset, read from src/kamite/presets.cpp."""
    src = open(os.path.join(R, 'src/kamite/presets.cpp'), encoding='utf-8').read()
    block = src[src.index('const Text kText[kCount] = {'):src.index('const Text kCustomText')]
    out = []
    for entry in re.findall(r'\{("(?:[^{}])*?),\s*\d+\s*,?\s*\}', block, re.S):
        strings = [bytes(s, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')
                   for s in re.findall(r'"((?:[^"\\]|\\.)*)"', entry)]
        # adjacent literals belong to one string: title, name, tagline, then the band's pieces
        out.append((strings[0], strings[2], ''.join(strings[3:])))
    assert len(out) == 4, out
    return out


def layers(title, tagline, band, seed, title_font):
    """The poster's base, its tape over the photo box, the photo box and the stamp's centre."""
    w, h = W, H
    rgb, rng = paper(w, h, seed)
    img = Image.fromarray(rgb.astype(np.uint8))
    ink = Image.new('L', (w, h), 0); d = ImageDraw.Draw(ink)
    m = round(w * 0.075)
    size = round(w * 0.2)
    f = ImageFont.truetype(title_font, size)
    while d.textlength(title, font=f) > w - 2 * m and size > 8:
        size -= 1; f = ImageFont.truetype(title_font, size)
    bb = d.textbbox((0, 0), title, font=f)
    ty = m + round(w * 0.02)
    d.text(((w - (bb[2] - bb[0])) / 2 - bb[0], ty - bb[1]), title, font=f, fill=255)
    y = ty + (bb[3] - bb[1]) + round(w * 0.045)
    d.rectangle((m, y, w - m, y + max(1, w // 160)), fill=255); y += round(w * 0.035)
    pw = w - 2 * m; ph = round(pw / PHOTO_ASPECT)
    shm = Image.new('L', (w, h), 0); ImageDraw.Draw(shm).rectangle((m + 4, y + 6, m + pw + 4, y + ph + 6), fill=150)
    img = Image.composite(Image.new('RGB', (w, h), (40, 28, 18)), img, shm.filter(ImageFilter.GaussianBlur(6)))
    box = (m, y, m + pw, y + ph)
    y += ph + round(w * 0.06)
    bpx = w * 21 / 400
    for line in DOM.wrap(tagline, bpx * 1.15, w - 2 * m):
        lw = DOM.width(line, bpx * 1.15); DOM.draw(ink, (w - lw) / 2, y, line, bpx * 1.15); y += round(bpx * 1.15 * 1.25)
    y += round(bpx * 0.5)
    # the band: three full lines (K, Nov 28): the largest size up to the tagline's that three lines hold (K's line break
    # is for the game's and the launcher's two lines; here one paragraph)
    band = band.replace('\n', ' ')
    px = bpx * 1.15
    while len(DOM.wrap(band, px, w - 2 * m)) > 3 and px > bpx * 0.7:
        px -= 0.25
    for line in DOM.wrap(band, px, w - 2 * m):
        if y > h - m - px:
            raise SystemExit('%s: the band does not fit the poster' % title)
        lw = DOM.width(line, px); DOM.draw(ink, (w - lw) / 2, y, line, px); y += round(px * 1.3)
    img = Image.composite(Image.new('RGB', (w, h), INK), img, ink_texture(ink, rng))
    # tape on all four corners of the photo, with its own soft shadow
    tape = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    L, Wd = int(w * 0.24), int(w * 0.075)
    x0, y0, x1, y1 = box
    for cx, cy, ang in ((x0, y0, 38), (x1, y0, -38), (x0, y1, -38), (x1, y1, 38)):
        t = tape_strip(L, Wd, rng, ang + rng.uniform(-6, 6))
        sh = Image.new('RGBA', t.size, (0, 0, 0, 0)); sh.putalpha(t.getchannel('A').point(lambda v: int(v * 0.25)))
        tape.alpha_composite(sh.filter(ImageFilter.GaussianBlur(3)), (int(cx - t.width / 2) + 3, int(cy - t.height / 2) + 4))
        tape.alpha_composite(t, (int(cx - t.width / 2), int(cy - t.height / 2)))
    base = img.convert('RGBA'); base.alpha_composite(tape)
    out = burn_edge(np.asarray(base.convert('RGB'), np.float32), rng, rag=max(6, w // 30), burn=max(8, w // 22))
    base = Image.fromarray(out, 'RGBA')
    over = Image.new('RGBA', (w, h), (0, 0, 0, 0)); over.paste(tape.crop(box), box[:2])
    return base, over, box, (w * 0.5, h - w * 0.2)


def stamp(text, font_path, seed):
    """The red stamp (rotated 8 degrees, patchy ink), trimmed to its pixels."""
    w = W
    rng = np.random.default_rng(seed)
    f = ImageFont.truetype(font_path, round(w * 0.11))
    d = ImageDraw.Draw(Image.new('L', (8, 8)))
    tw = d.textlength(text, font=f); bb = d.textbbox((0, 0), text, font=f)
    th = bb[3] - bb[1]; pad = round(w * 0.03)
    box = Image.new('L', (int(tw + 4 * pad), int(th + 4 * pad)), 0); bd = ImageDraw.Draw(box)
    bd.rectangle((2, 2, box.width - 3, box.height - 3), outline=255, width=max(3, w // 70))
    bd.text((2 * pad - bb[0], 2 * pad - bb[1]), text, font=f, fill=255)
    box = box.rotate(8, expand=True, resample=Image.BICUBIC)
    bw, bh = box.size
    m = np.asarray(box, np.float32) / 255 * (0.55 + 0.45 * fractal(bw, bh, rng, 10, 3)) * 0.85
    m *= (rng.random((bh, bw)) > 0.05)
    out = np.zeros((bh, bw, 4), np.uint8); out[..., 0], out[..., 1], out[..., 2] = 168, 30, 24
    out[..., 3] = np.clip(m * 255, 0, 255).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')


def shadow(base):
    a = base.getchannel('A')
    pad = Image.new('L', (a.width + 2 * SHADOW_PAD, a.height + 2 * SHADOW_PAD), 0); pad.paste(a, (SHADOW_PAD, SHADOW_PAD))
    blurred = pad.filter(ImageFilter.GaussianBlur(14))
    out = Image.new('RGBA', pad.size, (0, 0, 0, 0)); out.putalpha(blurred)
    return out


def main():
    title_font = os.environ.get('WENDYVILLE', '/tmp/wv/Wendyville.ttf')
    stamp_font = os.environ.get('HOLTWOOD', '/tmp/wp/fonts/HoltwoodOneSC-Regular.ttf')
    os.makedirs(OUT, exist_ok=True)
    lines = ['# The first-run posters (tools/launcher/make_posters.py): poster size, then per poster N its photo box',
             '# (left top right bottom) and the stamp centre (x y), in the poster\'s pixels.',
             'size %d %d %d' % (W, H, SHADOW_PAD)]
    for n, ((title, tagline, band), seed) in enumerate(zip(preset_text(), SEEDS)):
        base, over, box, centre = layers(title, tagline, band, seed, title_font)
        base.save(os.path.join(OUT, 'poster_%d.png' % n), optimize=True)
        over.crop(box).save(os.path.join(OUT, 'poster_%d_tape.png' % n), optimize=True)
        shadow(base).save(os.path.join(OUT, 'poster_%d_shadow.png' % n), optimize=True)
        lines.append('poster %d %d %d %d %d %d %d' % ((n,) + box + (round(centre[0]), round(centre[1]))))
    stamp('SELECTED', stamp_font, 7).save(os.path.join(OUT, 'stamp.png'), optimize=True)
    open(os.path.join(OUT, 'posters.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
