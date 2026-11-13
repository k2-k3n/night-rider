#!/usr/bin/env python3
"""MIT. Night Rider's boot splash: the night road, speed streaks as the km/h climbs, then the bike at 300 with the
whammy wide open.

The bike and rider are rendered by the app's own ride.h (tools/splash_frames.cpp, host clang++) and posterised to a
palette picked from their own pixels; the night behind them is redrawn flat from the painting's colours (sky,
skyline, elevated highway with its cyan lights, asphalt), because flat spans are what the 24 KiB RSP1 budget can
afford. The HUD is drawn back crisp. Writes src/splash_data.h and build/splash.gif (decoded from the bytes).

  /opt/homebrew/bin/python3.11 tools/make_splash.py
"""
import os, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from scipy import ndimage
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build'   # previews; created when the script runs

import splash_rsp as S                      # noqa: E402
from splash_rsp import to_lab               # noqa: E402

SDK = Path(os.environ['RESONA_SDK'])
INK, CYAN, AMBER = (255, 246, 222), (110, 236, 240), (255, 196, 96)
H, W = 172, 320


def render():
    tmp = Path(tempfile.mkdtemp()); exe = tmp / 'frames'
    subprocess.run(['clang++', '-std=c++17', '-O2', '-DRESONA_LAB_HOST', f'-I{SDK}', f'-I{ROOT / "src"}', '-include', 'cmath',
                    str(ROOT / 'tools' / 'splash_frames.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(tmp / 'f.raw')], check=True)
    return [S.rgb888(f) for f in np.fromfile(tmp / 'f.raw', np.uint16).reshape(-1, H, W)]


def band_colour(img, y0, y1):
    return tuple(int(v) for v in np.median(img[y0:y1].reshape(-1, 3), 0))


def night(rest):
    """The night redrawn flat: sky, a skyline silhouette traced from the far city, the elevated highway with a
    cyan light line, the barrier and the asphalt."""
    img = np.zeros((H, W, 3), np.uint8)
    sky_top, sky = band_colour(rest, 0, 6), band_colour(rest, 6, 20)
    city = tuple(int(v * .62) for v in band_colour(rest, 20, 46))
    hwy, road = band_colour(rest, 50, 74), band_colour(rest, 110, 150)
    img[:] = sky; img[:8] = sky_top
    # skyline: per 12 px block, the first row (in the far-city band) that is clearly darker than the sky
    lum = rest[..., :3].astype(int).sum(-1); sky_l = sum(sky)
    for x0 in range(0, W, 12):
        col = lum[8:47, x0:x0 + 12].mean(1); hit = np.flatnonzero(col < sky_l * .8)
        top = 8 + (hit[0] if len(hit) else 30); top = int(round(top / 4) * 4)
        img[top:48, x0:x0 + 12] = city
    img[48:77] = hwy; img[56:58] = tuple(int(v * .6 + c * .4) for v, c in zip(hwy, CYAN)); img[74:77] = tuple(int(v * .7) for v in hwy)
    img[77:108] = tuple(int(v * .85) for v in road); img[108:] = road
    return img


def streaks(img, seed, n, speed):
    out = img.copy(); rng = np.random.default_rng(seed)
    cols = [(220, 60, 50), AMBER, INK, CYAN, (150, 160, 190)]
    for _ in range(n):
        # the road band, clear of the HUD
        y = int(rng.integers(84, 127)); x = int(rng.integers(-60, 300)); ln = int(rng.integers(30, 90) * speed); th = int(rng.integers(1, 3))
        out[y:y + th, max(0, x):min(W, x + ln)] = cols[int(rng.integers(0, len(cols)))]
    return out


def bike_layer(bike, k=8):
    """The bike and rider (rendered on black) posterised to k colours from their own pixels; returns (mask, rgb)."""
    mask = bike.astype(int).sum(-1) > 12
    mask = ndimage.binary_opening(ndimage.binary_closing(mask, iterations=2), iterations=1)
    px = bike[mask].astype(np.float32); lab = to_lab(px.reshape(-1, 1, 3)).reshape(-1, 3)
    rng = np.random.default_rng(5); cent = lab[rng.choice(len(lab), k, replace=False)]
    for _ in range(25):
        d = ((lab[::3, None] - cent[None]) ** 2).sum(-1).argmin(1)
        for j in range(k):
            mm = d == j
            if mm.any(): cent[j] = lab[::3][mm].mean(0)
    d = ((lab[:, None] - cent[None]) ** 2).sum(-1).argmin(1)
    pal = np.array([px[d == j].mean(0) if (d == j).any() else (0, 0, 0) for j in range(k)]).astype(np.uint8)
    idx = np.full(mask.shape, -1); idx[mask] = d
    for _ in range(2):
        votes = np.stack([ndimage.uniform_filter((idx == j).astype(np.float32), 5) for j in range(k)])
        best = votes.argmax(0); idx = np.where(mask, best, -1)
    out = np.zeros_like(bike); out[mask] = pal[idx[mask]]
    return mask, out


def hud(img, hud_rgb, number=None):
    out = img.copy(); lum = hud_rgb.astype(int).sum(-1)
    is_cyan = (hud_rgb[..., 2].astype(int) > hud_rgb[..., 0].astype(int) + 40)
    is_amber = (hud_rgb[..., 0].astype(int) > hud_rgb[..., 2].astype(int) + 60)
    out[(lum > 300) & ~is_cyan & ~is_amber] = INK; out[(lum > 230) & is_cyan] = CYAN; out[(lum > 230) & is_amber] = AMBER
    return out


def main():
    rest, fast, bike, hud20, hud120, hud220, hud300 = render()
    base = night(rest)
    mask, layer = bike_layer(bike)
    final = base.copy(); final = streaks(final, 7, 26, 1.6); final[mask] = layer[mask]
    final = hud(final, hud300)
    road = lambda seed, n, sp, h: S.rgb565(hud(streaks(base, seed, n, sp), h))
    frames = [
        (320, S.rgb565(hud(base, hud20))),                # the night at 20 km/h
        (220, road(2, 14, 1.0, hud220)),                  # streaks rush in as the km/h climbs
        (1000, S.rgb565(final)),                          # the bike at 300, WHAMMY +24
    ]
    for hold, f in frames: print(f'  frame {hold:4d} ms: {S.frame_bytes(f):6d} bytes')
    data = S.encode(frames, duration_ms=sum(h for h, _ in frames))
    S.write_header(data, ROOT / 'src' / 'splash_data.h')
    OUT.mkdir(exist_ok=True)
    S.preview(S.decode(data), OUT / 'splash.gif', scale=2)
    from PIL import Image
    Image.fromarray(S.rgb888(frames[-1][1])).resize((640, 344), Image.NEAREST).save(OUT / 'splash-final.png')
    print(f'{len(data)} bytes of {S.MAX_BYTES}')


if __name__ == '__main__':
    main()
