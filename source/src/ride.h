// MIT. Night Rider: the illustration as a ride. Every pixel comes from ride_art.h.
//
// Motion is a handful of continuous controls, so any change of throttle, boost or style blends in:
//   speed      0..1, chased with bike-like inertia (quicker to brake than to accelerate)
//   travel     how far the world has moved; each background band scrolls at its own parallax rate
//   blur       each band blends between pre-blurred copies as it gets fast
//   shake      road vibration (random each frame, larger with speed), plus suspension bumps on a spring
//   pitch      nose up under acceleration, down when braking (a spring)
//   tuck       the rider folds into the wind with speed; the head stays rigid; the scarf streams
//   wheels     each wheel face spins (resampled from its clear sectors) under the fork and swingarm
#pragma once
#include <stdint.h>
#include <resona/platform.h>
#include <resona/math.h>
#include "ride_art.h"

namespace ride {
using namespace ride_art;
constexpr int W = 320, H = 172, kBlurRow = 48;     // rows above kBlurRow (far city) are never blurred
constexpr int kLevels = 3;

struct Spring { float x, v; };
struct Ride {
 float speed, target, boostFx, accel, travel, wheelR, wheelF, tuck, bob, scarfPh, vibX, vibY, flare;
 Spring bump, pitch, head;
 uint32_t t, nextBump, seed;
 int boost;
 float streakY[12], streakX[12], streakL[12];
};
struct Shared {
 uint16_t bg[H * kPeriod];                              // sharp
 uint16_t blur[kLevels - 1][(H - kBlurRow) * kPeriod];  // medium and heavy horizontal blur (wrapped)
 uint32_t bike[kBikeW * kBikeH], rider[kHumW * kHumH];  // premultiplied RGB565 low, alpha 0..32 above
 uint8_t riderW[kHumW * kHumH];                         // 0 body, 1..15 head weight, 16+ scarf weight
 uint16_t rotor[2][kLevels][kRotorR * kRotorA];         // each wheel face, sharp to angularly blurred
 uint8_t wheelMap[2][64 * 64][2];                       // per bike pixel near a wheel: rotor radius, angle (255 = outside)
 int wheelX[2], wheelY[2];
 uint32_t msBg, msBike;
};

inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
inline uint32_t rnd(uint32_t &s) { s = s * 1664525u + 1013904223u; return s >> 8; }
inline float rndf(uint32_t &s) { return static_cast<float>(rnd(s) & 0xffff) / 65536.f; }
inline void spring(Spring &s, float to, float hz, float zeta, float dt) {
 const float w = 6.2831853f * hz, h = dt / 4.f;
 for (int i = 0; i < 4; ++i) { const float a = w * w * (to - s.x) - 2 * zeta * w * s.v; s.v += a * h; s.x += s.v * h; }
}

/* ---- colour helpers (RGB565 spread for one-multiply blends) ------------------------------------- */
constexpr uint32_t kSpread = 0x07E0F81Fu;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2")
#endif
#define RIDE_HOT inline __attribute__((always_inline))
RIDE_HOT uint32_t spread(uint32_t c) { return ((c & 0xFFFFu) | (c << 16)) & kSpread; }
RIDE_HOT uint16_t gather(uint32_t e) { return static_cast<uint16_t>((e & 0xF81Fu) | ((e >> 16) & 0x07E0u)); }
RIDE_HOT uint32_t lerp5(uint32_t a, uint32_t b, uint32_t w) { return ((a * (32 - w) + b * w) >> 5) & kSpread; }
inline uint32_t premul(uint16_t c, uint32_t a32) {
 const uint32_t r = ((c >> 11) * a32 + 16) >> 5, g = (((c >> 5) & 63) * a32 + 16) >> 5, b = ((c & 31) * a32 + 16) >> 5;
 return (a32 << 16) | (r << 11) | (g << 5) | b;
}
inline uint32_t nib(const uint8_t *a4, int i) { const uint32_t v = (a4[i >> 1] >> ((i & 1) * 4)) & 15; return (v * 32 + 7) / 15; }
// additive light: add a spread colour scaled by k (0..32), saturating each channel
RIDE_HOT uint16_t addLight(uint16_t dst, uint32_t lightSpread, uint32_t k) {
 uint32_t r = (dst >> 11) + ((((lightSpread >> 11) & 31) * k) >> 5), g = ((dst >> 5) & 63) + ((((lightSpread >> 21) & 63) * k) >> 5), b = (dst & 31) + (((lightSpread & 31) * k) >> 5);
 return static_cast<uint16_t>(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (b > 31 ? 31 : b));
}

/* ---- one-time preparation ---------------------------------------------------------------------- */
inline void boxBlurRow(const uint16_t *src, uint16_t *dst, int radius) {
 // a wrapped running box blur in spread form (the period is the tile width)
 uint32_t r = 0, g = 0, b = 0; const int n = 2 * radius + 1;
 for (int k = -radius; k <= radius; ++k) { const uint16_t c = src[(k + kPeriod) % kPeriod]; r += c >> 11; g += (c >> 5) & 63; b += c & 31; }
 for (int x = 0; x < kPeriod; ++x) {
  dst[x] = static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
  const uint16_t out = src[(x - radius + kPeriod) % kPeriod], in = src[(x + radius + 1) % kPeriod];
  r += (in >> 11) - (out >> 11); g += ((in >> 5) & 63) - ((out >> 5) & 63); b += (in & 31) - (out & 31);
 }
}
inline void init(Ride &r, Shared &sh, float speed0) {
 r = Ride{}; r.speed = r.target = speed0; r.seed = 0x7a11u; r.nextBump = 900;
 for (int i = 0; i < H * kPeriod; ++i) sh.bg[i] = bg_pal[bg_idx[i]];
 for (int y = kBlurRow; y < H; ++y) {
  boxBlurRow(sh.bg + y * kPeriod, sh.blur[0] + (y - kBlurRow) * kPeriod, 5);
  boxBlurRow(sh.bg + y * kPeriod, sh.blur[1] + (y - kBlurRow) * kPeriod, 16);
 }
 for (int i = 0; i < kBikeW * kBikeH; ++i) sh.bike[i] = premul(fg_pal[bike_i[i]], nib(bike_a4, i));
 for (int i = 0; i < kHumW * kHumH; ++i) {
  const uint32_t h = nib(hum_a4, i), s = nib(scarf_a4, i), hd = nib(head_a4, i);
  sh.rider[i] = premul(fg_pal[hum_i[i]], h > s ? h : s);
  sh.riderW[i] = static_cast<uint8_t>(s > h ? 16 + (s >> 1) : (hd >> 1));
 }
 // wheel faces: sharp, then blurred around the circle
 const uint8_t *rot[2] = {rotor_rear, rotor_front};
 for (int w = 0; w < 2; ++w) {
  for (int i = 0; i < kRotorR * kRotorA; ++i) sh.rotor[w][0][i] = fg_pal[rot[w][i]];
  for (int lv = 1; lv < kLevels; ++lv) {
   const int rad = lv == 1 ? 4 : 14, n = 2 * rad + 1;
   for (int ri = 0; ri < kRotorR; ++ri) for (int a = 0; a < kRotorA; ++a) {
    uint32_t R = 0, G = 0, B = 0;
    for (int k = -rad; k <= rad; ++k) { const uint16_t c = sh.rotor[w][0][ri * kRotorA + (a + k + kRotorA) % kRotorA]; R += c >> 11; G += (c >> 5) & 63; B += c & 31; }
    sh.rotor[w][lv][ri * kRotorA + a] = static_cast<uint16_t>(((R / n) << 11) | ((G / n) << 5) | (B / n));
   }
  }
  // which bike pixels show the wheel face, and where on it
  const float *E = w ? kWheelFront : kWheelRear;
  const float cx = E[0] - kBikeX, cy = E[1] - kBikeY, rx = E[2], ry = E[3], ca = resona_cosf(E[4]), sa = resona_sinf(E[4]);
  sh.wheelX[w] = static_cast<int>(cx) - 32; sh.wheelY[w] = static_cast<int>(cy) - 32;
  for (int j = 0; j < 64; ++j) for (int i = 0; i < 64; ++i) {
   const float dx = sh.wheelX[w] + i + .5f - cx, dy = sh.wheelY[w] + j + .5f - cy;
   const float u = (dx * ca + dy * sa) / rx, v = (-dx * sa + dy * ca) / ry, rr = resona_sqrtf(u * u + v * v);
   uint8_t *m = sh.wheelMap[w][j * 64 + i];
   if (rr >= 1.f) { m[0] = 255; m[1] = 0; continue; }
   // angle in 0..1 without atan2: octant-reduced rational approximation
   const float ax = resona_fabsf(u), ay = resona_fabsf(v), mn = ax < ay ? ax : ay, mx = ax < ay ? ay : ax, q = mn / (mx + 1e-6f);
   float ang = q * (.785398f + .273f * (1 - q)); if (ay > ax) ang = 1.570796f - ang; if (u < 0) ang = 3.141593f - ang; if (v < 0) ang = 6.283185f - ang;
   m[0] = static_cast<uint8_t>(rr * .97f * kRotorR > kRotorR - 1 ? kRotorR - 1 : rr * .97f * kRotorR);
   m[1] = static_cast<uint8_t>(ang / 6.283185f * kRotorA) % kRotorA;
  }
 }
}

/* ---- motion ------------------------------------------------------------------------------------- */
// speed 0..1 is the whammy position (x 24 semitones): held, it rises to `top` in one second and stays; released,
// it falls back in 0.6 s. The Rider Engine runs the same linear glide, so speed, km/h and pitch move together.
// activity 0..1 (guitar envelope) only shakes the bike.
inline void update(Ride &r, uint32_t ms, float top, float activity, bool boost) {
 const float dt = ms / 1000.f; r.t += ms; r.boost = boost ? 1 : 0;
 r.target = boost ? top : 0.f;
 const float rate = r.target > r.speed ? top : 1.6f * top, prev = r.speed;
 r.speed += clampf(r.target - r.speed, -rate * dt, rate * dt);
 r.accel += ((r.speed - prev) / (dt > 0 ? dt : 1) - r.accel) * .2f;
 r.boostFx += ((boost ? 1.f : 0.f) - r.boostFx) * (1 - resona_expf(-dt / .18f));
 const float v = r.speed, vis = .03f + resona_powf(v, 1.7f) + .45f * r.boostFx;
 r.travel += vis * 900.f * dt;                    // road pixels per second at parallax 1
 if (r.travel > 1.0e6f) r.travel -= 288.f * 3000.f;
 r.wheelR += vis * 900.f * dt / 18.f * kRotorA / 6.283185f;  // rolls with the road (radius ~18 px)
 r.wheelF += vis * 900.f * dt / 24.f * kRotorA / 6.283185f;
 while (r.wheelR >= kRotorA) r.wheelR -= kRotorA; while (r.wheelF >= kRotorA) r.wheelF -= kRotorA;
 // road vibration, bumps on the suspension, pitch with acceleration
 const float amp = .15f + .9f * v + .5f * r.boostFx + .6f * clampf(activity, 0, 1);
 r.vibX = (rndf(r.seed) - .5f) * amp * .5f; r.vibY = (rndf(r.seed) - .5f) * amp;
 if (r.nextBump > ms) r.nextBump -= ms;
 else { r.nextBump = 500 + static_cast<uint32_t>(rndf(r.seed) * (1800 - 1200 * v)); if (v > .15f) r.bump.v += (1.2f + 2.5f * v) * (rndf(r.seed) > .5f ? 1 : .6f); }
 spring(r.bump, 0, 3.2f, .32f, dt);
 spring(r.pitch, clampf(-r.accel * .09f, -.03f, .03f), 1.6f, .55f, dt);
 // rider
 r.tuck += (clampf(v * .9f + r.boostFx * .3f, 0, 1) - r.tuck) * (1 - resona_expf(-dt / .35f));
 spring(r.head, r.tuck + r.vibY * .25f, 2.4f, .45f, dt);
 r.scarfPh += dt * (1.5f + 7.f * v + 3.f * r.boostFx);
 if (r.scarfPh > 6283.f) r.scarfPh -= 6283.185f;
 r.flare = .55f + .25f * v + .35f * r.boostFx + (rndf(r.seed) - .5f) * .08f;
 // boost speed lines
 for (int i = 0; i < 12; ++i) {
  r.streakX[i] -= (500.f + 900.f * vis) * dt * (1 + (i % 6) * .15f);
  if (r.streakX[i] + r.streakL[i] < 0) { r.streakX[i] = W + rndf(r.seed) * 120; r.streakY[i] = 78 + rndf(r.seed) * 90; r.streakL[i] = 30 + rndf(r.seed) * 70; }
 }
}
inline int kmh(const Ride &r) { return static_cast<int>(20 + r.speed * 280 + .5f); }   // 20 = no shift, 300 = +24

/* ---- drawing ------------------------------------------------------------------------------------ */
inline float parallax(int y) {
 if (y < 48) return .05f;          // far city
 if (y < 77) return .16f;          // elevated highway
 if (y < 108) return 1.f;          // barrier
 return 1.15f + .85f * (y - 108) / 63.f;   // road, faster as it nears
}
inline void drawBackground(const Ride &r, const Shared &sh, uint16_t *px, uint32_t stride) {
 const float vis = .03f + resona_powf(r.speed, 1.7f) + .45f * r.boostFx;
 for (int y = 0; y < H; ++y) {
  const float f = parallax(y);
  const int off = static_cast<int>(r.travel * f) % kPeriod;
  // blur grows with each band's own apparent speed
  float lv = y < kBlurRow ? 0.f : clampf((vis * f - .18f) * 2.2f, 0.f, 2.f);
  const int i0 = lv >= 1.f ? 1 : 0; const uint32_t t = static_cast<uint32_t>((lv - i0) * 32.f);
  const uint16_t *a = i0 == 0 ? sh.bg + y * kPeriod : sh.blur[0] + (y - kBlurRow) * kPeriod;
  const uint16_t *b = y < kBlurRow ? a : (i0 == 0 ? sh.blur[0] : sh.blur[1]) + (y - kBlurRow) * kPeriod;
  uint16_t *row = px + y * stride;
  int sx = off;
  if (t == 0 || y < kBlurRow) {
   for (int x = 0; x < W; ++x) { row[x] = a[sx]; if (++sx == kPeriod) sx = 0; }
  } else {
   for (int x = 0; x < W; ++x) { row[x] = gather(lerp5(spread(a[sx]), spread(b[sx]), t)); if (++sx == kPeriod) sx = 0; }
  }
 }
}
// bilinear read of a packed premultiplied texture, 16.16 coordinates, bounds checked by the caller
RIDE_HOT uint32_t sample(const uint32_t *tex, int w, int32_t sx, int32_t sy, uint32_t &a) {
 const uint32_t fx = (sx >> 11) & 31, fy = (sy >> 11) & 31; const uint32_t *p = tex + (sy >> 16) * w + (sx >> 16);
 const uint32_t w00 = p[0], w10 = p[1], w01 = p[w], w11 = p[w + 1];
 a = (((w00 >> 16) * (32 - fx) + (w10 >> 16) * fx) * (32 - fy) + ((w01 >> 16) * (32 - fx) + (w11 >> 16) * fx) * fy) >> 10;
 if (!a) return 0;
 return lerp5(lerp5(spread(w00), spread(w10), fx), lerp5(spread(w01), spread(w11), fx), fy);
}
struct Pose { float px, py, ca, sa, ox, oy; };     // bike: screen = pivot + R(p) * (src - pivot) + offset
inline Pose bikePose(const Ride &r) {
 Pose q; q.px = kGroundPivot[0]; q.py = kGroundPivot[1];
 const float ang = r.pitch.x; q.ca = resona_cosf(ang); q.sa = resona_sinf(ang);
 q.ox = r.vibX; q.oy = r.vibY - r.bump.x * 1.6f;
 return q;
}
// screen -> bike-art coordinates (screen pixels of the art at rest)
inline void toArt(const Pose &q, float x, float y, float &u, float &v) {
 const float dx = x - q.ox - q.px, dy = y - q.oy - q.py;
 u = q.px + q.ca * dx + q.sa * dy; v = q.py - q.sa * dx + q.ca * dy;
}
inline void drawBike(const Ride &r, Shared &sh, uint16_t *px, uint32_t stride) {
 const Pose q = bikePose(r);
 const int lvR = clampf((r.speed + r.boostFx) * 2.2f, 0, 2.f) >= 1.6f ? 2 : (r.speed > .18f ? 1 : 0);
 const int x0 = kBikeX - 4 < 0 ? 0 : kBikeX - 4, x1 = kBikeX + kBikeW + 4 > W ? W : kBikeX + kBikeW + 4;
 const int y0 = kBikeY - 6 < 0 ? 0 : kBikeY - 6, y1 = kBikeY + kBikeH + 4 > H ? H : kBikeY + kBikeH + 4;
 // the inverse map is affine: step it along each row in 16.16
 const int32_t dudx = static_cast<int32_t>(q.ca * 65536.f), dvdx = static_cast<int32_t>(-q.sa * 65536.f);
 const int rotA[2] = {static_cast<int>(r.wheelR), static_cast<int>(r.wheelF)};
 for (int y = y0; y < y1; ++y) {
  float u, v; toArt(q, x0 + .5f, y + .5f, u, v);
  int32_t su = static_cast<int32_t>((u - kBikeX - .5f) * 65536.f), sv = static_cast<int32_t>((v - kBikeY - .5f) * 65536.f);
  uint16_t *row = px + y * stride;
  for (int x = x0; x < x1; ++x, su += dudx, sv += dvdx) {
   const int iu = su >> 16, iv = sv >> 16;
   if (static_cast<uint32_t>(iu) >= static_cast<uint32_t>(kBikeW - 1) || static_cast<uint32_t>(iv) >= static_cast<uint32_t>(kBikeH - 1)) continue;
   uint32_t a; uint32_t c = sample(sh.bike, kBikeW, su, sv, a);
   // a spinning wheel face shows where the bike art is open
   for (int w = 0; w < 2; ++w) {
    const int mi = iu - sh.wheelX[w], mj = iv - sh.wheelY[w];
    if (static_cast<uint32_t>(mi) >= 64u || static_cast<uint32_t>(mj) >= 64u) continue;
    const uint8_t *m = sh.wheelMap[w][mj * 64 + mi]; if (m[0] == 255) continue;
    const uint16_t rc = sh.rotor[w][lvR][m[0] * kRotorA + (m[1] + rotA[w]) % kRotorA];
    c = c + ((spread(rc) * (32 - a)) >> 5 & kSpread); a = 32; break;
   }
   if (!a) continue;
   row[x] = a >= 32 ? gather(c) : gather(c + ((spread(row[x]) * (32 - a)) >> 5 & kSpread));
  }
 }
}
inline void drawRider(const Ride &r, const Shared &sh, uint16_t *px, uint32_t stride) {
 const Pose q = bikePose(r);
 // tuck: shoulders move forward/down by up to (2.3, 3.4) px, weighted up from the hip; head rigid
 const float tx = r.tuck * 2.3f, ty = r.tuck * 3.4f, hx = r.head.x * 2.6f, hy = r.head.x * 3.6f;
 const float hipY = kHip[1], topY = kHeadCenter[1] - 12.f;
 const float scarfAmp = .6f + 1.4f * r.speed + .8f * r.boostFx;
 const int x0 = kHumX - 6, x1 = kHumX + kHumW + 6, y0 = kHumY - 6 < 0 ? 0 : kHumY - 6, y1 = kHumY + kHumH + 4;
 for (int y = y0; y < y1; ++y) {
  uint16_t *row = px + y * stride;
  for (int x = x0; x < x1; ++x) {
   float u, v; toArt(q, x + .5f, y + .5f, u, v);
   // body weight from the hip up, pinned at the gloves and boot
   float wgt = clampf((hipY - v) / (hipY - topY), 0, 1);
   const float dgx = u - kNearGlove[0], dgy = v - kNearGlove[1], dbx = u - kBoot[0], dby = v - kBoot[1];
   const float pin = clampf((dgx * dgx + dgy * dgy) / 40.f, 0, 1) * clampf((dbx * dbx + dby * dby) / 60.f, 0, 1);
   wgt *= pin;
   float du = tx * wgt, dv = ty * wgt;
   // the head and the scarf: weights baked per art pixel (nearest)
   const int ai = static_cast<int>(u) - kHumX, aj = static_cast<int>(v) - kHumY;
   if (ai >= 0 && aj >= 0 && ai < kHumW && aj < kHumH) {
    const uint8_t wb = sh.riderW[aj * kHumW + ai];
    if (wb >= 16) {   // scarf: a wave travelling to the tip, growing along it
     const float along = clampf((kScarfRoot[0] - u) / (kScarfRoot[0] - kScarfTip[0]), 0, 1.4f);
     dv += scarfAmp * along * resona_sinf(r.scarfPh * 6.2831853f - along * 5.5f); du += hx * .6f;
    } else if (wb) { const float h = wb / 16.f; du = du * (1 - h) + hx * h; dv = dv * (1 - h) + hy * h; }
   }
   const int32_t su = static_cast<int32_t>((u - du - kHumX - .5f) * 65536.f), sv = static_cast<int32_t>((v - dv - kHumY - .5f) * 65536.f);
   if (static_cast<uint32_t>(su >> 16) >= static_cast<uint32_t>(kHumW - 1) || static_cast<uint32_t>(sv >> 16) >= static_cast<uint32_t>(kHumH - 1)) continue;
   uint32_t a; const uint32_t c = sample(sh.rider, kHumW, su, sv, a); if (!a) continue;
   row[x] = a >= 32 ? gather(c) : gather(c + ((spread(row[x]) * (32 - a)) >> 5 & kSpread));
  }
 }
}
inline void glow(uint16_t *px, uint32_t stride, float cx, float cy, float rad, uint16_t color, float k) {
 const uint32_t sc = spread(color);
 for (int y = static_cast<int>(cy - rad); y <= static_cast<int>(cy + rad); ++y) {
  if (y < 0 || y >= H) continue;
  for (int x = static_cast<int>(cx - rad); x <= static_cast<int>(cx + rad); ++x) {
   if (x < 0 || x >= W) continue;
   const float d = ((x + .5f - cx) * (x + .5f - cx) + (y + .5f - cy) * (y + .5f - cy)) / (rad * rad); if (d >= 1) continue;
   const uint32_t kk = static_cast<uint32_t>((1 - d) * (1 - d) * k * 32.f); if (kk) px[y * stride + x] = addLight(px[y * stride + x], sc, kk > 32 ? 32 : kk);
  }
 }
}
inline void drawEffects(const Ride &r, uint16_t *px, uint32_t stride) {
 const Pose q = bikePose(r);
 const float hx = kHeadlight[0] + q.ox, hy = kHeadlight[1] + q.oy, tx = kTaillight[0] + q.ox, ty = kTaillight[1] + q.oy;
 // tail-light trail: a red streak behind the bike that lengthens with speed
 const int len = static_cast<int>(6 + 70 * r.speed * r.speed + 40 * r.boostFx);
 const uint32_t red = spread(0xF8A2u);
 for (int i = 0; i < len; ++i) {
  const int x = static_cast<int>(tx) - i; if (x < 0) break;
  const uint32_t k = static_cast<uint32_t>(26.f * (1 - static_cast<float>(i) / len));
  for (int dy = -1; dy <= 1; ++dy) { const int y = static_cast<int>(ty) + dy; if (y >= 0 && y < H) px[y * stride + x] = addLight(px[y * stride + x], red, dy ? k / 3 : k); }
 }
 glow(px, stride, tx, ty, 4.f + 3.f * r.speed, 0xF8A2u, .6f);
 glow(px, stride, hx, hy, 8.f + 6.f * r.flare + 10.f * r.boostFx, 0xFFF0u, r.flare * .9f);
 if (r.boostFx > .05f) glow(px, stride, hx + 6, hy, 26.f * r.boostFx, 0xFF80u, .35f * r.boostFx);   // the beam blooms on boost
 // boost: speed lines across the barrier and road
 if (r.boostFx > .05f) {
  const uint32_t white = spread(0xFFFFu);
  for (int i = 0; i < 12; ++i) {
   const int y = static_cast<int>(r.streakY[i]), thick = (i % 3 == 0) ? 2 : 1; if (y < 0 || y + thick > H) continue;
   for (int k = 0; k < static_cast<int>(r.streakL[i]); ++k) {
    const int x = static_cast<int>(r.streakX[i]) + k; if (x < 0 || x >= W) continue;
    const uint32_t a = static_cast<uint32_t>(30.f * r.boostFx * (1 - static_cast<float>(k) / r.streakL[i]));
    for (int t = 0; t < thick; ++t) px[(y + t) * stride + x] = addLight(px[(y + t) * stride + x], white, t ? a / 2 : a);
   }
  }
 }
}
inline void render(const Ride &r, Shared &sh, uint16_t *px, uint32_t stride, uint32_t (*clock)()) {
 const uint32_t t0 = clock ? clock() : 0;
 drawBackground(r, sh, px, stride);
 const uint32_t t1 = clock ? clock() : 0;
 drawBike(r, sh, px, stride);
 drawRider(r, sh, px, stride);
 drawEffects(r, px, stride);
 if (clock) { const uint32_t t2 = clock(); sh.msBg += t1 - t0; sh.msBike += t2 - t1; }
}

/* ---- type, with a soft shadow so it reads over the city ------------------------------------------- */
RIDE_HOT uint16_t blend565(uint16_t dst, uint16_t src, int a) { const uint32_t w = static_cast<uint32_t>(a + 4) >> 3; return w >= 32 ? src : gather(lerp5(spread(dst), spread(src), w)); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
inline void alphaBlit(uint16_t *px, uint32_t stride, const uint8_t *a, int w, int h, int x, int y, uint16_t color) {
 for (int v = 0; v < h; ++v) for (int u = 0; u < w; ++u) {
  const int X = x + u, Y = y + v; if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
  const int al = a[v * w + u]; if (al) px[Y * stride + X] = blend565(px[Y * stride + X], color, al);
 }
}
inline int glyphIndex(const Font &f, char ch) { for (int i = 0; f.chars[i]; ++i) if (f.chars[i] == ch) return i; return -1; }
inline int typeWidth10(const Font &f, const char *text) {
 int w = 0, last = -1; for (int n = 0; text[n]; ++n) { const int g = glyphIndex(f, text[n]); if (g < 0) continue; w += f.adv10[g]; last = g; }
 return last >= 0 ? w - (f.adv10[last] - f.w[last] * 10) : 0;
}
inline void type(uint16_t *px, uint32_t stride, const Font &f, const char *text, int x, int y, uint16_t color, bool right) {
 int cx10 = right ? x * 10 - typeWidth10(f, text) : x * 10;
 for (int pass = 0; pass < 2; ++pass) {
  int c10 = cx10;
  for (int n = 0; text[n]; ++n) {
   const int g = glyphIndex(f, text[n]); if (g < 0) continue;
   if (f.w[g]) {
    if (pass == 0) { // shadow
     const uint8_t *a = f.alpha + f.off[g];
     for (int v = 0; v < f.h; ++v) for (int u = 0; u < f.w[g]; ++u) {
      const int X = (c10 + 5) / 10 + u + 1, Y = y + v + 1; if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
      const int al = a[v * f.w[g] + u] * 3 / 5; if (al) px[Y * stride + X] = blend565(px[Y * stride + X], 0x0841u, al);
     }
    } else alphaBlit(px, stride, f.alpha + f.off[g], f.w[g], f.h, (c10 + 5) / 10, y, color);
   }
   c10 += f.adv10[g];
  }
 }
}
}
