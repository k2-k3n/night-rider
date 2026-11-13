#!/usr/bin/env python3
"""Cut the Night Rider illustration into ride layers for the 320 x 172 screen: writes src/ride_art.h.

From assets/original.png (the illustration), assets/background.png (the clean environment plate),
assets/bike-underlay.png (the bike with the rider removed, used only where the rider can move) and
assets/rig-data.json (outlines, anchors, wheel ellipses and the parts in front of each wheel).

  - background: one tileable 288 px period (ends cross-faded), 256-colour palette
  - bike: the illustration inside the foreground outline, the rider replaced by the underlay bike,
    wheel faces cut out except the fork, caliper, swingarm and exhaust in front of them
  - rider and scarf: the illustration inside their outlines, with a head mask for rigid head motion
  - rotors: each wheel face resampled in polar form from the sectors where nothing is in front
  - type: Montserrat Bold (SIL OFL, assets/Montserrat-OFL.txt), as in CREATURE
Development tool only (Python 3, numpy, scipy, Pillow); builds use the generated header.
  python3 tools/encode_ride.py      (from the project folder)
"""
import json, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

REF_W, REF_H = 1672, 941
SW, SH = 320, 172
K = SW / REF_W                      # reference px -> screen px
OY = (REF_H * K - SH) / 2           # rows trimmed top and bottom
P, L = 288, 32                      # background period and cross-fade
rig = json.load(open('assets/rig-data.json'))
A = np.asarray(Image.open('assets/original.png').convert('RGB')).astype(np.float32)
BG = np.asarray(Image.open('assets/background.png').convert('RGB')).astype(np.float32)
UND = np.asarray(Image.open('assets/bike-underlay.png').convert('RGB')).astype(np.float32)

def poly_mask(points, feather=1.25, ss=1):
    im = Image.new('L', (REF_W, REF_H), 0); ImageDraw.Draw(im).polygon([tuple(p) for p in points], fill=255)
    m = np.asarray(im).astype(np.float32) / 255
    return ndimage.gaussian_filter(m, feather) if feather else m

def to_screen(arr):
    """reference-size array -> screen grid (320 x 172), area-averaged"""
    h = int(round(REF_H * K))
    if arr.ndim == 2:
        im = Image.fromarray(np.clip(arr * 255, 0, 255).astype(np.uint8), 'L').resize((SW, h), Image.LANCZOS)
        out = np.asarray(im).astype(np.float32) / 255
    else:
        im = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), 'RGB').resize((SW, h), Image.LANCZOS)
        out = np.asarray(im).astype(np.float32)
    o = int(round(OY)); return out[o:o + SH]

def rgb565(rgb):
    q = np.clip(rgb + .5, 0, 255).astype(np.uint16)
    return (q[..., 0] >> 3 << 11) | (q[..., 1] >> 2 << 5) | (q[..., 2] >> 3)

def quantize(rgb_list, colors=256):
    """one palette for several screen-size RGB arrays; returns (palette565, [index arrays])"""
    strip = np.concatenate([r.reshape(-1, 3) for r in rgb_list], 0)
    w = 512; h = (len(strip) + w - 1) // w
    pad = np.zeros((w * h, 3), np.float32); pad[:len(strip)] = strip
    im = Image.fromarray(pad.reshape(h, w, 3).astype(np.uint8), 'RGB').quantize(colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(im.getpalette()[:colors * 3], np.float32).reshape(-1, 3)
    # remap with nearest palette colour (stable, no dither noise on the moving layers)
    idx = []
    for r in rgb_list:
        flat = r.reshape(-1, 3)
        best = np.zeros(len(flat), np.uint8)
        for s in range(0, len(flat), 8192):
            d = ((flat[s:s + 8192, None, :] - pal[None, :, :]) ** 2).sum(-1); best[s:s + 8192] = d.argmin(1)
        idx.append(best.reshape(r.shape[:2]))
    return rgb565(pal), idx

# ---- background: a tileable period ------------------------------------------------------------------
bg = to_screen(BG)
tile = bg[:, :P].copy()
for x in range(L):
    t = x / L; tile[:, x] = bg[:, x + P] * (1 - t) + bg[:, x] * t
bg_pal, (bg_idx,) = quantize([tile])

# ---- masks -------------------------------------------------------------------------------------------
fg = poly_mask(rig['masks']['foreground'])
for hole in rig.get('foregroundHoles', []): fg = np.clip(fg - poly_mask(hole), 0, 1)
human = poly_mask(rig['masks']['human'])
scarf = poly_mask(rig['masks']['scarf'])
head = poly_mask(rig['regions']['head']['points'], 3)
# where the underlay shows bike (not city) behind the rider
cover = np.clip(human + scarf, 0, 1)
R, G, B = UND[..., 0], UND[..., 1], UND[..., 2]
city = (B > R + 8) | ((R > 140) & (G > 100) & (B < 100) & (np.abs(R - G) < 90)) | ((G > 140) & (B > 140))
cand = (cover > .02) & ~city & (np.sqrt(((UND - BG) ** 2).sum(-1)) > 25)
# keep only what connects to the visible bike body outside the rider
body = (fg > .5) & (cover < .02)
lab, _ = ndimage.label(ndimage.binary_closing(cand | body, iterations=2))
und_bike = (lab == lab[535, 816]) & (cover > .02)
und_bike = ndimage.binary_opening(und_bike, iterations=2)
und_bike = ndimage.gaussian_filter(und_bike.astype(np.float32), 1.0)
bike_rgb = A * (1 - cover[..., None]) + UND * cover[..., None]
bike_a = np.clip(fg * (1 - cover) + und_bike * cover * fg, 0, 1)
# wheel faces: cut out, except the parts in front of them
wheel_face = np.zeros((REF_H, REF_W), np.float32)
yy, xx = np.mgrid[0:REF_H, 0:REF_W]
def ellipse_norm(w, x, y):
    e = w['spinEllipse']; cx, cy = e['center']; rx, ry = e['radius']; a = math.radians(e['rotation'])
    dx, dy = x - cx, y - cy; u = (dx * math.cos(a) + dy * math.sin(a)) / rx; v = (-dx * math.sin(a) + dy * math.cos(a)) / ry
    return u, v
for name, w in rig['wheels'].items():
    u, v = ellipse_norm(w, xx, yy); wheel_face = np.maximum(wheel_face, np.clip((1.0 - np.sqrt(u * u + v * v)) * 40, 0, 1))
occ = np.zeros((REF_H, REF_W), np.float32)
for k, pts in rig['occluders'].items():
    if k != 'windshield': occ = np.maximum(occ, poly_mask(pts, .8))
bike_a = bike_a * (1 - wheel_face * (1 - occ))

# ---- rotors: polar resampling from the clear sectors --------------------------------------------------
NR, NA = 16, 96
rotors = {}
for name, w in rig['wheels'].items():
    e = w['spinEllipse']; cx, cy = e['center']; rx, ry = e['radius']; a = math.radians(e['rotation'])
    sectors = w['safeSourceAngles']; total = sum(b - s for s, b in sectors)
    strip = np.zeros((NR, NA, 3), np.float32)
    for j in range(NA):
        # ping-pong through the clear sectors so the repeated texture has no seam
        t = (j / NA) * 2; t = t if t <= 1 else 2 - t; deg = t * total
        for s, b in sectors:
            if deg <= b - s: ang = math.radians(s + deg); break
            deg -= b - s
        for i in range(NR):
            r = (i + .5) / NR * .97
            u, v = r * math.cos(ang), r * math.sin(ang)
            X = cx + u * rx * math.cos(a) - v * ry * math.sin(a); Y = cy + u * rx * math.sin(a) + v * ry * math.cos(a)
            x0, y0 = int(X), int(Y); fx, fy = X - x0, Y - y0
            strip[i, j] = (A[y0, x0] * (1 - fx) * (1 - fy) + A[y0, x0 + 1] * fx * (1 - fy) + A[y0 + 1, x0] * (1 - fx) * fy + A[y0 + 1, x0 + 1] * fx * fy)
    rotors[name] = strip

# ---- to the screen grid ------------------------------------------------------------------------------
bA = to_screen(bike_a); bRGB = to_screen(bike_rgb * bike_a[..., None]) / np.maximum(bA[..., None], 1 / 255)
rider = np.clip(np.maximum(human, scarf), 0, 1)
hA = to_screen(np.clip(human, 0, 1)); rA = to_screen(rider); hRGB = to_screen(A * rider[..., None]) / np.maximum(rA[..., None], 1 / 255)
sA = to_screen(scarf); sRGB = to_screen(A * scarf[..., None]) / np.maximum(sA[..., None], 1 / 255)
hdM = to_screen(head)
def bbox(a, thr=.02):
    ys, xs = np.nonzero(a > thr); return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
BX0, BY0, BX1, BY1 = bbox(bA); HX0, HY0, HX1, HY1 = bbox(np.maximum(hA, sA))
fg_pal, (bike_i, hum_i, rot_r, rot_f) = quantize([bRGB[BY0:BY1, BX0:BX1], hRGB[HY0:HY1, HX0:HX1], rotors['rear'], rotors['front']])
def nib(a): q = np.clip(np.round(a * 15), 0, 15).astype(np.uint8).ravel(); q = np.append(q, 0) if len(q) % 2 else q; return (q[0::2] | (q[1::2] << 4))

# ---- anchors on the screen ------------------------------------------------------------------------------
def scr(p): return (p[0] * K, p[1] * K - int(round(OY)))
an = rig['anchors']
wheels = {}
for name, w in rig['wheels'].items():
    e = w['spinEllipse']; c = scr(e['center'])
    wheels[name] = (c[0], c[1], e['radius'][0] * K, e['radius'][1] * K, math.radians(e['rotation']))

# ---- type ------------------------------------------------------------------------------------------------
def bake(cap, chars, track, ss=8, stretch=1.10, font='assets/Montserrat-Bold.ttf'):
    size = cap * ss * 1.4
    for _ in range(8):
        f = ImageFont.truetype(font, int(size)); b = f.getbbox('H', stroke_width=ss // 2); size *= cap * ss / (b[3] - b[1])
    f = ImageFont.truetype(font, int(size)); hb = f.getbbox('H', stroke_width=ss // 2); out = []
    for ch in chars:
        if ch == ' ': out.append((0, round(cap * .32), b'')); continue
        im = Image.new('L', (int(size * 2), int(size * 2))); ImageDraw.Draw(im).text((ss * 2, 0), ch, font=f, fill=255, stroke_width=ss // 2, stroke_fill=255)
        bb = im.getbbox(); w = max(1, round((bb[2] - bb[0]) / ss * stretch))
        out.append((w, w + track, np.asarray(im.crop((bb[0], hb[1], bb[2], hb[3])).resize((w, cap), Image.LANCZOS), np.uint8).tobytes()))
    return out
FONTS = {'title': (11, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ ', 1.0), 'num': (26, '0123456789', 1.0),
         'label': (9, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ:/! ', .7), 'small': (8, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ:/! +0123456789', .6)}

# ---- header -------------------------------------------------------------------------------------------------
def arr(ctype, name, values, per=32):
    vals = [str(int(v)) for v in np.asarray(values).ravel()]
    return 'alignas(4) static const %s %s[%d]={\n%s};\n' % (ctype, name, len(vals), ',\n'.join(','.join(vals[i:i + per]) for i in range(0, len(vals), per)))
o = ['// Generated by tools/encode_ride.py from the Night Rider illustration and assets/Montserrat-Bold.ttf\n'
     '// (SIL OFL 1.1, see assets/Montserrat-OFL.txt). Do not edit by hand.\n#pragma once\n#include <stdint.h>\nnamespace ride_art {\n']
o.append('constexpr int kPeriod=%d;\n' % P)
o.append(arr('uint16_t', 'bg_pal', bg_pal)); o.append(arr('uint8_t', 'bg_idx', bg_idx))
o.append(arr('uint16_t', 'fg_pal', fg_pal))
o.append('constexpr int kBikeX=%d,kBikeY=%d,kBikeW=%d,kBikeH=%d;\n' % (BX0, BY0, BX1 - BX0, BY1 - BY0))
o.append(arr('uint8_t', 'bike_i', bike_i)); o.append(arr('uint8_t', 'bike_a4', nib(bA[BY0:BY1, BX0:BX1])))
o.append('constexpr int kHumX=%d,kHumY=%d,kHumW=%d,kHumH=%d;\n' % (HX0, HY0, HX1 - HX0, HY1 - HY0))
o.append(arr('uint8_t', 'hum_i', hum_i)); o.append(arr('uint8_t', 'hum_a4', nib(hA[HY0:HY1, HX0:HX1])))
o.append(arr('uint8_t', 'scarf_a4', nib(sA[HY0:HY1, HX0:HX1]))); o.append(arr('uint8_t', 'head_a4', nib(hdM[HY0:HY1, HX0:HX1])))
o.append('constexpr int kRotorR=%d,kRotorA=%d;\n' % (NR, NA))
o.append(arr('uint8_t', 'rotor_rear', rot_r)); o.append(arr('uint8_t', 'rotor_front', rot_f))
for name, (cx, cy, rx, ry, a) in wheels.items():
    o.append('static const float kWheel%s[5]={%.3ff,%.3ff,%.3ff,%.3ff,%.4ff};   // centre (screen), radii, tilt\n' % (name.capitalize(), cx, cy, rx, ry, a))
for k in ['groundPivot', 'hip', 'headCenter', 'nearGlove', 'farGlove', 'boot', 'scarfRoot', 'scarfTip', 'rearContact', 'frontContact']:
    x, y = scr(an[k]); o.append('constexpr float k%s[2]={%.2ff,%.2ff};\n' % (k[0].upper() + k[1:], x, y))
x, y = scr((1222, 405)); o.append('constexpr float kHeadlight[2]={%.2ff,%.2ff};\n' % (x, y))
x, y = scr((268, 358)); o.append('constexpr float kTaillight[2]={%.2ff,%.2ff};\n' % (x, y))
o.append('struct Font{int h;const char *chars;const uint8_t *w;const uint16_t *adv10;const uint16_t *off;const uint8_t *alpha;};\n')
for name, (cap, chars, track) in FONTS.items():
    gl = bake(cap, chars, track); offs, blob = [], b''
    for w, adv, a in gl: offs.append(len(blob)); blob += a
    o.append(arr('uint8_t', name + '_w', [g[0] for g in gl])); o.append(arr('uint16_t', name + '_adv10', [round(g[1] * 10) for g in gl]))
    o.append(arr('uint16_t', name + '_off', offs)); o.append(arr('uint8_t', name + '_alpha', list(blob)))
    o.append('static const Font %s={%d,"%s",%s_w,%s_adv10,%s_off,%s_alpha};\n' % (name, cap, chars, name, name, name, name))
o.append('}\n')
open('src/ride_art.h', 'w').write(''.join(o))
sizes = dict(bg=bg_idx.size, bike=bike_i.size + (bike_i.size + 1) // 2, rider=hum_i.size * 2, rotors=rot_r.size + rot_f.size)
print('wrote src/ride_art.h; bike box', (BX0, BY0, BX1 - BX0, BY1 - BY0), 'rider box', (HX0, HY0, HX1 - HX0, HY1 - HY0), 'bytes', sizes)

# preview of the layers, for checking the cut by eye
pv = Image.new('RGB', (SW * 2, SH * 2))
def show(idx, pal, a, ox, oy, canvas):
    rgb = np.stack([(pal >> 11) * 255 // 31, ((pal >> 5) & 63) * 255 // 63, (pal & 31) * 255 // 31], -1)[idx]
    c = np.asarray(canvas).copy(); h, w = idx.shape; reg = c[oy:oy + h, ox:ox + w].astype(np.float32)
    c[oy:oy + h, ox:ox + w] = (reg * (1 - a[..., None]) + rgb * a[..., None]).astype(np.uint8); return Image.fromarray(c)
base = Image.fromarray(np.stack([(bg_pal >> 11) * 255 // 31, ((bg_pal >> 5) & 63) * 255 // 63, (bg_pal & 31) * 255 // 31], -1)[bg_idx].astype(np.uint8))
full = Image.new('RGB', (SW, SH)); full.paste(base, (0, 0)); full.paste(base.crop((0, 0, SW - P, SH)), (P, 0))
comp = show(bike_i, fg_pal, bA[BY0:BY1, BX0:BX1], BX0, BY0, full)
comp = show(hum_i, fg_pal, hA[HY0:HY1, HX0:HX1], HX0, HY0, comp)
pv.paste(comp, (0, 0)); pv.paste(full, (SW, 0))
bikeonly = show(bike_i, fg_pal, bA[BY0:BY1, BX0:BX1], BX0, BY0, Image.new('RGB', (SW, SH), (40, 0, 40)))
pv.paste(bikeonly, (0, SH))
rot = np.stack([(fg_pal >> 11) * 255 // 31, ((fg_pal >> 5) & 63) * 255 // 63, (fg_pal & 31) * 255 // 31], -1)
pv.paste(Image.fromarray(rot[rot_r].astype(np.uint8)).resize((NA * 2, NR * 2), Image.NEAREST), (SW + 4, SH + 4))
pv.paste(Image.fromarray(rot[rot_f].astype(np.uint8)).resize((NA * 2, NR * 2), Image.NEAREST), (SW + 4, SH + 44))
pv.resize((SW * 4, SH * 4), Image.NEAREST).save('build/layers-preview.png')
