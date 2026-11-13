#!/usr/bin/env python3
"""Draw the Scratchivision skin graphics in the style of images/logo.png.

Look: near-black pages; panels with a thin black inner stroke and an orange -> hot pink ->
purple outer ring (the logo's border) plus a faint glow; chrome (white -> steel blue) fills for
anything lit (the logo's letters); slanted Orbitron capitals for baked-in text. Borders are
fractional-pixel widths: masks are drawn at 4x and scaled down. The MPC cannot draw
gradients or load fonts, so all of it is baked into PNGs here.

Output (all committed, so building needs no Pillow):
  plugin/skins/*.png            backgrounds, logo, waveforms, playhead, buttons, scope dots
  docs/images/*-preview.png     mock-ups of both pages, for checking the look on a computer

Needs Pillow:
  python3 -m venv build/venv && build/venv/bin/pip install pillow
  build/venv/bin/python scripts/make_skin_art.py
Font: Orbitron (SIL OFL 1.1, assets/fonts/OFL.txt).
"""
import array
import math
import os
import sys
import wave

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skin_layout as L  # noqa: E402

SKINS = os.path.join(L.ROOT, 'plugin', 'skins')
DOCS = os.path.join(L.ROOT, 'docs', 'images')
FONT = os.path.join(L.ROOT, 'assets', 'fonts', 'Orbitron[wght].ttf')
os.makedirs(SKINS, exist_ok=True)
os.makedirs(DOCS, exist_ok=True)


# ---- helpers ------------------------------------------------------------------------------
def lut(stops, n=256):
    out = []
    for i in range(n):
        t = i / (n - 1) * (len(stops) - 1)
        k = min(int(t), len(stops) - 2)
        f = t - k
        a, b = stops[k], stops[k + 1]
        out.append(tuple(int(round(a[c] + (b[c] - a[c]) * f)) for c in range(3)))
    return out


def gradient(w, h, stops, direction='diag'):
    """RGBA gradient. direction: 'diag' (top-left -> bottom-right), 'vert', 'horiz'."""
    if direction == 'horiz':
        t = Image.linear_gradient('L').rotate(90, expand=True).transpose(Image.FLIP_LEFT_RIGHT).resize((w, h))
    elif direction == 'vert':
        t = Image.linear_gradient('L').resize((w, h))
    else:
        hx = Image.linear_gradient('L').rotate(90, expand=True).transpose(Image.FLIP_LEFT_RIGHT).resize((w, h)).point(lambda v: v // 2)
        vy = Image.linear_gradient('L').resize((w, h)).point(lambda v: v // 2)
        t = ImageChops.add(hx, vy)
    table = lut(stops)
    r = t.point([c[0] for c in table])
    g = t.point([c[1] for c in table])
    b = t.point([c[2] for c in table])
    return Image.merge('RGBA', (r, g, b, Image.new('L', (w, h), 255)))


def rr_mask(w, h, rect, radius, fill=255, ss=4):
    """Rounded-rectangle mask; rect and radius may be fractional (drawn at ss x, then reduced)."""
    m = Image.new('L', (w * ss, h * ss), 0)
    x, y, rw, rh = rect
    ImageDraw.Draw(m).rounded_rectangle([x * ss, y * ss, (x + rw) * ss - 1, (y + rh) * ss - 1],
                                        radius=max(0, radius) * ss, fill=fill)
    return m.resize((w, h), Image.BOX)


def paste_masked(dst, src, mask, at=(0, 0)):
    layer = Image.new('RGBA', dst.size, (0, 0, 0, 0))
    layer.paste(src, at)
    a = ImageChops.multiply(layer.getchannel('A'), mask)
    layer.putalpha(a)
    dst.alpha_composite(layer)


def font(size, weight='Bold'):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_name(weight)
    except Exception:
        pass
    return f


SHEAR = 0.18   # the logo's forward slant


def text_layer(text, size, fill_stops, stroke=3, weight='Black', fill_dir='vert', outer=0, outer_stops=None):
    """Slanted text: gradient fill, black stroke, optional gradient outer stroke (like the logo)."""
    f = font(size, weight)
    pad = stroke + outer + 6
    l, t, r, b = f.getbbox(text, stroke_width=stroke + outer)
    w, h = r - l + 2 * pad + int(size * SHEAR) + 4, b - t + 2 * pad
    org = (pad - l + int(size * SHEAR * 0.5), pad - t)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    if outer:
        m = Image.new('L', (w, h), 0)
        ImageDraw.Draw(m).text(org, text, font=f, fill=255, stroke_width=stroke + outer, stroke_fill=255)
        paste_masked(out, gradient(w, h, outer_stops or L.BORDER_STOPS, 'diag'), m)
    m = Image.new('L', (w, h), 0)
    ImageDraw.Draw(m).text(org, text, font=f, fill=255, stroke_width=stroke, stroke_fill=255)
    black = Image.new('RGBA', (w, h), L.BLACK + (255,))
    paste_masked(out, black, m)
    m = Image.new('L', (w, h), 0)
    ImageDraw.Draw(m).text(org, text, font=f, fill=255)
    paste_masked(out, gradient(w, h, fill_stops, fill_dir), m)
    # Slant: x' = x + SHEAR * (h - y)
    return out.transform(out.size, Image.AFFINE, (1, SHEAR, -SHEAR * h, 0, 1, 0), resample=Image.BICUBIC)


def place_text(img, layer, rect, align='center'):
    x, y, w, h = rect
    lw, lh = layer.size
    if align == 'left':
        px = x
    else:
        px = x + (w - lw) // 2
    img.alpha_composite(layer, (int(px), int(y + (h - lh) // 2)))


def panel(img, rect, radius=18, border=L.BORDER, glow=True, fill=L.PANEL):
    """Gradient-bordered panel drawn into img (RGBA, page size)."""
    W, H = img.size
    x, y, w, h = rect
    if glow:
        g = rr_mask(W, H, rect, radius, ss=1)
        g = ImageChops.subtract(g.filter(ImageFilter.GaussianBlur(9)), rr_mask(W, H, (x + 3, y + 3, w - 6, h - 6), radius, ss=1))
        g = g.point(lambda v: v * 60 // 255)
        paste_masked(img, gradient(W, H, L.BORDER_STOPS, 'diag'), g)
    outer = rr_mask(W, H, rect, radius)
    inner = rr_mask(W, H, (x + border, y + border, w - 2 * border, h - 2 * border), radius - border)
    paste_masked(img, gradient(W, H, L.BORDER_STOPS, 'diag'), ImageChops.subtract(outer, inner))
    # thin black stroke inside the ring, then the fill
    b2 = border + L.INNER_STROKE
    inner2 = rr_mask(W, H, (x + b2, y + b2, w - 2 * b2, h - 2 * b2), radius - b2)
    paste_masked(img, Image.new('RGBA', (W, H), L.BLACK + (255,)), ImageChops.subtract(inner, inner2))
    paste_masked(img, gradient(W, H, [L.PANEL_HI, fill], 'vert'), inner2)


def page():
    img = gradient(L.TAB_W, L.TAB_H, [(16, 18, 40), L.BG, (8, 9, 18)], 'vert')
    # faint horizon grid, retro-futurist
    d = ImageDraw.Draw(img)
    for yy in range(0, L.TAB_H, 32):
        d.line([(0, yy), (L.TAB_W, yy)], fill=(30, 34, 70, 255), width=1)
    for xx in range(0, L.TAB_W, 32):
        d.line([(xx, 0), (xx, L.TAB_H)], fill=(22, 25, 52, 255), width=1)
    return img


def title(img, text, rect, size=22):
    layer = text_layer(text, size, [L.ORANGE, L.PINK, L.MAGENTA], stroke=2, fill_dir='horiz')
    place_text(img, layer, rect, 'left')


def caption(img, text, rect, size=16):
    layer = text_layer(text, size, [L.CORAL, L.PINK], stroke=0, weight='Bold', fill_dir='horiz')
    place_text(img, layer, rect, 'left')


# ---- buttons ------------------------------------------------------------------------------
def icon_layer(kind, w, h, stops):
    """A slanted play-style triangle pointing left or right, black-edged, gradient-filled."""
    m = Image.new('L', (w, h), 0)
    cx, cy, r = w / 2, h / 2, min(w, h) * 0.26
    pts = [(cx + r, cy), (cx - r * 0.8, cy - r), (cx - r * 0.8, cy + r)] if kind == 'right' else \
          [(cx - r, cy), (cx + r * 0.8, cy - r), (cx + r * 0.8, cy + r)]
    ImageDraw.Draw(m).polygon(pts, fill=255)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    paste_masked(out, Image.new('RGBA', (w, h), L.BLACK + (255,)), m.filter(ImageFilter.MaxFilter(5)))
    paste_masked(out, gradient(w, h, stops, 'vert'), m)
    return out


def button(name, label, w, h, size=None, icon=None):
    size = size or min(26, int(h * 0.48))
    for on in (True, False):
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        r = min(14, h // 3)
        a, b = L.BUTTON_RING, L.BUTTON_RING + L.INNER_STROKE
        outer = rr_mask(w, h, (0, 0, w, h), r)
        inner = rr_mask(w, h, (a, a, w - 2 * a, h - 2 * a), r - a)
        stops = L.BORDER_STOPS if on else [(130, 80, 50), (120, 40, 70), (60, 25, 80)]
        paste_masked(img, gradient(w, h, stops, 'diag'), ImageChops.subtract(outer, inner))
        inner2 = rr_mask(w, h, (b, b, w - 2 * b, h - 2 * b), r - b)
        paste_masked(img, Image.new('RGBA', (w, h), L.BLACK + (255,)), ImageChops.subtract(inner, inner2))
        if on:
            paste_masked(img, gradient(w, h, [L.CYAN, (190, 206, 236), L.BLUE, (205, 215, 238)], 'vert'), inner2)
            # gloss
            gl = rr_mask(w, h, (5, 5, w - 10, (h - 10) // 2), r - 5).point(lambda v: v * 70 // 255)
            paste_masked(img, Image.new('RGBA', (w, h), (255, 255, 255, 255)), gl)
            lab = icon_layer(icon, w, h, [(16, 18, 40), (40, 30, 80)]) if icon else text_layer(label, size, [(16, 18, 40), (16, 18, 40)], stroke=0)
        else:
            paste_masked(img, gradient(w, h, [(30, 33, 60), (16, 18, 34)], 'vert'), inner2)
            lab = icon_layer(icon, w, h, [L.CYAN, L.BLUE]) if icon else text_layer(label, size, [L.CYAN, L.BLUE], stroke=0)
        place_text(img, lab, (0, 0, w, h))
        img.save(os.path.join(SKINS, '%s_%s.png' % (name, 'on' if on else 'off')))


def scale_rgb(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)


def pad(name, label, w, h, colour, filled_off, size=20):
    """A cue-style pad: ring in `colour`; filled_off=True gives a filled pad (cue set / CLEAR on)."""
    for on in (True, False):
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        r = min(12, h // 3)
        a, b = L.BUTTON_RING + (0.5 if filled_off else 0), L.BUTTON_RING + L.INNER_STROKE + (0.5 if filled_off else 0)
        outer = rr_mask(w, h, (0, 0, w, h), r)
        inner = rr_mask(w, h, (a, a, w - 2 * a, h - 2 * a), r - a)
        ring = colour if (filled_off or on) else scale_rgb(colour, 0.55)
        paste_masked(img, gradient(w, h, [scale_rgb(ring, 1.15), ring, scale_rgb(ring, 0.7)], 'diag'), ImageChops.subtract(outer, inner))
        inner2 = rr_mask(w, h, (b, b, w - 2 * b, h - 2 * b), r - b)
        paste_masked(img, Image.new('RGBA', (w, h), L.BLACK + (255,)), ImageChops.subtract(inner, inner2))
        if filled_off:
            top = scale_rgb(colour, 1.25 if on else 1.05)
            paste_masked(img, gradient(w, h, [top, colour, scale_rgb(colour, 0.6)], 'vert'), inner2)
            gl = rr_mask(w, h, (5, 4, w - 10, (h - 8) // 2), r - 5).point(lambda v: v * 60 // 255)
            paste_masked(img, Image.new('RGBA', (w, h), (255, 255, 255, 255)), gl)
            lab = text_layer(label, size, [(14, 10, 24), (30, 16, 40)], stroke=0)
        else:
            paste_masked(img, gradient(w, h, [(34, 28, 56) if on else (28, 23, 46), (16, 13, 28)], 'vert'), inner2)
            lab = text_layer(label, size, [scale_rgb(colour, 1.2), colour], stroke=0)
        place_text(img, lab, (0, 0, w, h))
        img.save(os.path.join(SKINS, '%s_%s.png' % (name, 'on' if on else 'off')))


def cue_marker(n, colour):
    """A marker for cue n: a thin line the height of the waveform with a numbered tab at the top."""
    w, h = L.CUE_MARKER_W, L.WAVE_VIEW[3]
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    cx = w // 2
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    d.rectangle([cx - 1, 20, cx + 1, h - 1], fill=255)
    d.rounded_rectangle([2, 0, w - 3, 22], radius=5, fill=255)
    paste_masked(img, Image.new('RGBA', (w, h), L.BLACK + (255,)), m.filter(ImageFilter.MaxFilter(3)))
    paste_masked(img, gradient(w, h, [scale_rgb(colour, 1.2), colour, scale_rgb(colour, 0.8)], 'vert'), m)
    f = font(15, 'Black')
    ImageDraw.Draw(img).text((cx, 11), str(n), font=f, fill=(14, 10, 24, 255), anchor='mm')
    img.save(os.path.join(SKINS, 'cue_marker_%d.png' % n))
    return img


# ---- waveform -----------------------------------------------------------------------------
def read_peaks(path, columns):
    with wave.open(path, 'rb') as w:
        ch, sw, n = w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        a = array.array('h'); a.frombytes(raw); scale = 32768.0
    elif sw == 1:
        a = array.array('B', raw); a = array.array('h', [(v - 128) * 256 for v in a]); scale = 32768.0
    elif sw == 4:
        a = array.array('i'); a.frombytes(raw); scale = 2147483648.0
    else:   # 24-bit
        a = array.array('i', [int.from_bytes(raw[i:i + 3], 'little', signed=True) for i in range(0, len(raw), 3)]); scale = 8388608.0
    frames = len(a) // ch
    peaks = []
    for c in range(columns):
        s0, s1 = c * frames // columns, max(c * frames // columns + 1, (c + 1) * frames // columns)
        lo = hi = 0.0
        for i in range(s0, s1):
            v = sum(a[i * ch + k] for k in range(ch)) / (ch * scale)
            lo, hi = min(lo, v), max(hi, v)
        peaks.append((lo, hi))
    return peaks


def bar_strips():
    """The live waveform's film strip, bar.png: 2 x BAR_STEPS frames of a mirrored bar. Frames
    0..23 are bars ahead of the playhead (chrome), 24..47 bars already played (hot pink -> purple);
    frame 0 / 24 = empty. One frame is a bar slot of the waveform window (10 px wide, scaled to ~9.5)."""
    w, h, n = 10, L.WAVE_VIEW[3], L.BAR_STEPS
    strip = Image.new('RGBA', (w, h * 2 * n), (0, 0, 0, 0))
    for half, stops in enumerate(([L.BLUE, L.CYAN, L.BLUE], [L.PURPLE, L.PINK, L.PURPLE])):
        for f in range(n):
            frame = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            hh = 1 + (h / 2 - 14) * f / (n - 1)
            m = Image.new('L', (w, h), 0)
            ImageDraw.Draw(m).rounded_rectangle([2, h / 2 - hh, w - 3, h / 2 + hh], radius=2, fill=255)
            paste_masked(frame, Image.new('RGBA', (w, h), L.BLACK + (255,)), m.filter(ImageFilter.MaxFilter(3)))
            paste_masked(frame, gradient(w, h, stops, 'vert'), m)
            strip.alpha_composite(frame, (0, (half * n + f) * h))
    strip.save(os.path.join(SKINS, 'bar.png'))
    return strip


def playhead():
    w, h = L.PLAYHEAD_W, L.WAVE_VIEW[3]
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    cx = w // 2
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    d.rectangle([cx - 2, 10, cx + 2, h - 11], fill=255)
    d.polygon([(cx - 11, 0), (cx + 11, 0), (cx, 14)], fill=255)
    d.polygon([(cx - 11, h - 1), (cx + 11, h - 1), (cx, h - 15)], fill=255)
    paste_masked(img, Image.new('RGBA', (w, h), L.BLACK + (255,)), m.filter(ImageFilter.MaxFilter(5)))
    paste_masked(img, gradient(w, h, L.BORDER_STOPS, 'vert'), m)
    img.save(os.path.join(SKINS, 'playhead.png'))
    return img


def dots():
    out = []
    for k, alpha in enumerate((255, 190, 120, 70)):
        s = L.DOT
        m = Image.new('L', (s, s), 0)
        ImageDraw.Draw(m).ellipse([5, 5, s - 6, s - 6], fill=255)
        glow = m.filter(ImageFilter.GaussianBlur(2.5)).point(lambda v: min(255, v * 2))
        img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
        paste_masked(img, Image.new('RGBA', (s, s), L.BLUE + (255,)), glow.point(lambda v: v * alpha // 255))
        paste_masked(img, Image.new('RGBA', (s, s), L.CYAN + (255,)), m.point(lambda v: v * alpha // 255))
        img.save(os.path.join(SKINS, 'dot_%d.png' % k))
        out.append(img)
    return out


# ---- pages --------------------------------------------------------------------------------
def deck_background():
    img = page()
    panel(img, L.WAVE_PANEL, radius=22, border=L.BORDER_BIG)
    # inside the waveform window: centre line and time ticks, faint
    d = ImageDraw.Draw(img)
    vx, vy, vw, vh = L.WAVE_VIEW
    d.line([(vx, vy + vh // 2), (vx + vw, vy + vh // 2)], fill=(52, 60, 110, 255), width=1)
    for k in range(1, 12):
        xx = vx + vw * k // 12
        d.line([(xx, vy + 6), (xx, vy + 16)], fill=(60, 66, 120, 255), width=1)
        d.line([(xx, vy + vh - 17), (xx, vy + vh - 7)], fill=(60, 66, 120, 255), width=1)
    for rect, label in ((L.P_SAMPLE, 'SAMPLE'), (L.P_DECK, 'DECK'), (L.P_READ, 'STATUS')):
        panel(img, rect)
        title(img, label, (rect[0] + 18, rect[1] + 12, rect[2] - 36, 34))
    img.save(os.path.join(SKINS, 'bg_deck.png'))
    return img


def scope_background():
    img = page()
    panel(img, L.RING_PANEL, radius=24, border=L.BORDER_BIG)
    d = ImageDraw.Draw(img)
    x, y, w, h = L.RING_VIEW
    cx, cy = x + w // 2, y + h // 2
    for r, col in ((int(w * 0.46), (70, 60, 120, 255)), (int(w * 0.23), (40, 44, 86, 255))):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=col, width=2)
    d.line([(x + 10, cy), (x + w - 10, cy)], fill=(40, 44, 86, 255), width=1)
    d.line([(cx, y + 10), (cx, y + h - 10)], fill=(40, 44, 86, 255), width=1)
    title(img, 'TIMECODE', (L.RING_PANEL[0] + 22, L.RING_PANEL[1] + 14, 300, 34))
    caption(img, 'L', (x + w - 30, cy - 30, 30, 24))
    caption(img, 'R', (cx + 10, y + 4, 30, 24))
    for rect, label in ((L.PLOT_SPEED_PANEL, 'SPEED'), (L.PLOT_LEVEL_PANEL, 'LEVEL'), (L.SCOPE_READ_PANEL, 'STATUS')):
        panel(img, rect)
        title(img, label, (rect[0] + 18, rect[1] + 10, 300, 32))
    px, py, pw, ph = L.PLOT_SPEED_VIEW
    d.line([(px, py + ph // 2), (px + pw, py + ph // 2)], fill=(70, 60, 120, 255), width=1)
    caption(img, '+3', (px + pw - 40, py - 2, 40, 20), 13)
    caption(img, '-3', (px + pw - 40, py + ph - 18, 40, 20), 13)
    for key, (cap, _) in L.SCOPE_READOUTS.items():
        caption(img, {'speed': 'PITCH', 'dir': 'DIRECTION', 'lock': 'LOCK', 'pos': 'POSITION'}[key], cap)
    img.save(os.path.join(SKINS, 'bg_scope.png'))
    return img


def logo():
    src = Image.open(os.path.join(L.ROOT, 'images', 'logo.png')).convert('RGBA')
    src = src.crop(src.getbbox())
    # The logo's motion arcs are black, which vanishes on the dark page. They are the only shapes
    # not joined to the lettering: flood-fill the lettering from a point on it, and whatever
    # opaque pixels remain unfilled are the arcs; paint those in the letters' cyan -> blue.
    a = src.getchannel('A').point(lambda v: 255 if v > 40 else 0)
    cy = src.height // 2
    seed = next(x for x in range(src.width // 3, src.width) if a.getpixel((x, cy)) == 255)
    ImageDraw.floodfill(a, (seed, cy), 128)
    arcs = a.point(lambda v: 255 if v == 255 else 0).filter(ImageFilter.MaxFilter(3))
    arcs = ImageChops.multiply(arcs, src.getchannel('A'))
    tinted = gradient(src.width, src.height, [L.CYAN, L.BLUE], 'vert')
    tinted.putalpha(arcs)
    src.alpha_composite(tinted)
    w, h = L.LOGO[2], L.LOGO[3]
    s = min(w / src.width, h / src.height)
    small = src.resize((int(src.width * s), int(src.height * s)), Image.LANCZOS)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.alpha_composite(small, ((w - small.width) // 2, (h - small.height) // 2))
    out.save(os.path.join(SKINS, 'logo.png'))
    return out


# ---- previews (mock-ups; on the MPC the text readouts are drawn by the device in Nexa) -----
def preview_text(img, text, rect, size, colour):
    f = font(size, 'Bold')
    d = ImageDraw.Draw(img)
    d.text((rect[0] + 4, rect[1] + (rect[3] - size) // 2 - 2), text, font=f, fill=colour + (255,))


def screen(tab_img, tabs, active):
    """Full 1280x800 mock-up: header strip, the tab, and function-key tabs along the bottom."""
    scr = Image.new('RGBA', (1280, 800), (24, 24, 28, 255))
    d = ImageDraw.Draw(scr)
    d.rectangle([0, 0, 1280, 109], fill=(36, 36, 42, 255))
    d.text((24, 36), 'Slow Ops: Scratchivision', font=font(26, 'Bold'), fill=(220, 220, 225, 255))
    scr.alpha_composite(tab_img, (0, 110))
    for i, name in enumerate(tabs):
        x = 10 + i * 210
        d.rectangle([x, 748, x + 196, 794], fill=(200, 200, 205, 255) if i == active else (60, 60, 66, 255))
        d.text((x + 70, 758), name, font=font(20, 'Bold'), fill=(20, 20, 24, 255) if i == active else (220, 220, 225, 255))
    return scr


def preview_centred(img, text, rect, size, colour):
    f = font(size, 'Bold')
    d = ImageDraw.Draw(img)
    tw = d.textlength(text, font=f)
    d.text((rect[0] + (rect[2] - tw) / 2, rect[1] + (rect[3] - size) // 2 - 2), text, font=f, fill=colour + (255,))


def main():
    for old in os.listdir(SKINS):   # images from earlier layouts (sample buttons, PNG waveforms)
        if old.startswith(('wave_', 'sample_', 'cue', 'bar_')) and old.endswith('.png'):
            os.remove(os.path.join(SKINS, old))
    lg = logo()
    bar_strip = bar_strips()
    ph = playhead()
    dd = dots()
    deck = deck_background()
    scope = scope_background()

    button('prev', '', L.SAMPLE_PREV[2], L.SAMPLE_PREV[3], icon='left')
    button('next', '', L.SAMPLE_NEXT[2], L.SAMPLE_NEXT[3], icon='right')
    button('rescan', 'RESCAN', L.SAMPLE_RESCAN[2], L.SAMPLE_RESCAN[3], 15)
    button('zoomout', '-', L.ZOOM_OUT[2], L.ZOOM_OUT[3], 30)
    button('zoomin', '+', L.ZOOM_IN[2], L.ZOOM_IN[3], 30)
    labels = {'cue': 'CUE', 'rpm33': '33', 'rpm45': '45', 'rel': 'REL', 'abs': 'ABS', 'loop': 'LOOP', 'thru': 'THRU'}
    for k, rect in L.DECK_BUTTONS.items():
        button(k, labels[k], rect[2], rect[3], 34 if k == 'cue' else None)
    for k, rect in L.SCOPE_BUTTONS.items():
        button(k, {'swap': 'SWAP', 'test': 'TEST'}[k], rect[2], rect[3])
    marks = []
    for c, rect in enumerate(L.CUE_PADS):
        pad('cue%d_empty' % (c + 1), 'CUE %d' % (c + 1), rect[2], rect[3], L.CUE_COLOURS[c], False)
        pad('cue%d_set' % (c + 1), 'CUE %d' % (c + 1), rect[2], rect[3], L.CUE_COLOURS[c], True)
        marks.append(cue_marker(c + 1, L.CUE_COLOURS[c]))
    pad('clear', 'CLEAR', L.CUE_CLEAR[2], L.CUE_CLEAR[3], (235, 50, 70), False)
    # CLEAR's "on" image is the filled one: render the filled pair and keep its on image.
    pad('clear_filled', 'CLEAR', L.CUE_CLEAR[2], L.CUE_CLEAR[3], (235, 50, 70), True)
    os.replace(os.path.join(SKINS, 'clear_filled_on.png'), os.path.join(SKINS, 'clear_on.png'))
    os.remove(os.path.join(SKINS, 'clear_filled_off.png'))

    # DECK preview: the first bundled sample 0.35 s in, at 1 s zoom, drawn with the real bar strips.
    p = deck.copy()
    p.alpha_composite(lg, L.LOGO[:2])
    vx, vy, vw, vh = L.WAVE_VIEW
    samples = L.list_samples()
    if samples:
        name, f, dur = samples[0]
        cols = max(1, int(round(dur * L.NUM_BARS)))   # one column = one bar at 1 s zoom
        peaks = [max(-lo, hi) for lo, hi in read_peaks(os.path.join(L.WAV_DIR, f), cols)]
        top = max(1e-6, max(peaks))
        start = int(round(0.35 * L.NUM_BARS)) - L.NUM_BARS // 2
        bw = vw / float(L.NUM_BARS)
        for k in range(L.NUM_BARS):
            c = start + k
            v = peaks[c] / top if 0 <= c < len(peaks) else 0.0
            fr = int(round(v * (L.BAR_STEPS - 1))) + (L.BAR_STEPS if k < L.NUM_BARS // 2 else 0)
            frame = bar_strip.crop((0, fr * vh, 10, (fr + 1) * vh)).resize((max(1, int(round(bw))), vh))
            p.alpha_composite(frame, (int(round(vx + k * bw)), vy))
        preview_centred(p, name, L.SAMPLE_NAME, 28, L.CYAN)
        preview_centred(p, '1/%d  %.1f s' % (len(samples), dur), L.SAMPLE_INFO, 16, L.PINK)
    for c, t in ((0, 0.20), (2, 0.62)):
        x = vx + vw * (0.5 + (t - 0.35)) - L.CUE_MARKER_W / 2
        p.alpha_composite(marks[c], (int(round(x)), vy))
    for c, rect in enumerate(L.CUE_PADS):
        p.alpha_composite(Image.open(os.path.join(SKINS, 'cue%d_%s_off.png' % (c + 1, 'set' if c in (0, 2) else 'empty'))), rect[:2])
    p.alpha_composite(Image.open(os.path.join(SKINS, 'clear_off.png')), L.CUE_CLEAR[:2])
    p.alpha_composite(ph, (vx + vw // 2 - L.PLAYHEAD_W // 2, vy))
    for k, rect in (('prev', L.SAMPLE_PREV), ('next', L.SAMPLE_NEXT), ('rescan', L.SAMPLE_RESCAN)):
        p.alpha_composite(Image.open(os.path.join(SKINS, '%s_off.png' % k)), rect[:2])
    for k, rect in L.DECK_BUTTONS.items():
        p.alpha_composite(Image.open(os.path.join(SKINS, '%s_%s.png' % (k, 'on' if k in ('rpm33', 'rel') else 'off'))), rect[:2])
    for k, rect in (('zoomout', L.ZOOM_OUT), ('zoomin', L.ZOOM_IN)):
        p.alpha_composite(Image.open(os.path.join(SKINS, '%s_off.png' % k)), rect[:2])
    preview_centred(p, '1 s', L.ZOOM_LABEL, 26, L.CYAN)
    preview_text(p, 'Locked', L.DECK_READOUTS['lock'], 24, L.PINK)
    screen(p, ['DECK', 'SCOPE'], 0).save(os.path.join(DOCS, 'deck-preview.png'))

    # SCOPE preview: a ring of dots, a speed trace, readouts.
    s = scope.copy()
    x, y, w, h = L.RING_VIEW
    for k in range(L.NUM_DOTS):
        a = 2 * math.pi * k / L.NUM_DOTS * 2.9
        rr = 0.46 * (0.8 if (k // 4) % 3 == 0 else 1.0)
        dx, dy = x + w * (0.5 + rr * math.sin(a)), y + h * (0.5 - rr * math.cos(a))
        s.alpha_composite(dd[min(3, (L.NUM_DOTS - 1 - k) // 8)], (int(dx - L.DOT / 2), int(dy - L.DOT / 2)))
    d = ImageDraw.Draw(s)
    px, py, pw, phh = L.PLOT_SPEED_VIEW
    pts = [(px + i, py + phh / 2 - phh / 2 * 0.5 * math.sin(i / 38.0) * (1 if i < 400 else 0.3)) for i in range(pw)]
    d.line(pts, fill=L.CYAN + (255,), width=3)
    lx, ly, lw, lh = L.PLOT_LEVEL_VIEW
    d.line([(lx + i, ly + lh - lh * (0.6 + 0.05 * math.sin(i / 9.0))) for i in range(lw)], fill=L.MAGENTA + (255,), width=3)
    vals = {'speed': '+1.02x', 'dir': 'Fwd', 'lock': 'Locked', 'pos': '2:13.4'}
    for key, (_, val) in L.SCOPE_READOUTS.items():
        preview_text(s, vals[key], val, 30, L.CYAN)
    for k, rect in L.SCOPE_BUTTONS.items():
        s.alpha_composite(Image.open(os.path.join(SKINS, '%s_off.png' % k)), rect[:2])
    screen(s, ['DECK', 'SCOPE'], 1).save(os.path.join(DOCS, 'scope-preview.png'))
    print('art written to', os.path.relpath(SKINS, L.ROOT), 'and', os.path.relpath(DOCS, L.ROOT))


if __name__ == '__main__':
    main()
