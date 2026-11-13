// Whammy — an upward pitch shifter for one mono guitar, MIT.
//
// One read pointer runs through a delay line `ratio` times faster than the guitar is written, so the note rises
// and the timbre moves with it (the classic whammy sound, not a formant-kept voice). When the pointer gets close
// to the write head it jumps back by a whole number of the note's periods and the two positions are cross-faded
// briefly; because the jump is period-aligned, the waveform carries on in phase and the splice is inaudible on a
// held note. The period comes from a running autocorrelation tracker on a 6 kHz copy of the input (McLeod-style
// peak pick, at 3 kHz, cheap enough to run every sample with no bursts); the jump is then refined against the
// waveform itself (a short correlation, sub-sample accurate). Chords without a period get a wider search.
// At ratio 1 the pointer settles within 1.4 ms of the write head, so the effect at rest is transparent.
//
// Everything per sample is a few dozen multiply-adds; the only burst is the jump refinement (at most ~1,000
// multiply-adds, coarse then fine, a few hundred times a second at +24).
#pragma once
#include <stdint.h>
#include <resona/math.h>

namespace whammy {
constexpr uint32_t kBuf = 4096, kMask = kBuf - 1;
constexpr uint32_t kDecim = 16, kLagMin = 3, kLagMax = 80;     // tracker at 3 kHz: 37.5 Hz .. 1 kHz
constexpr uint32_t kHist = 128;   // (a power chord's common period is ~40 Hz)
constexpr float kMinDelay = 4, kVoicedJump = 480, kUnvoicedJump = 1200;   // 10 ms minimum / 25 ms without a period
constexpr float kRestWindow = kMinDelay + 64;   // where the head may sit at ratio 1 (<= 1.4 ms)

inline float wabs(float x) { return x < 0 ? -x : x; }
inline float wclip(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

struct Lp {   // trapezoidal state-variable low-pass (one 2-pole section)
 float s1, s2, a1, a2, a3;
 void set(float hz, float q) {
  const float x = 3.141592654f * wclip(hz, 40, 21000) / 48000, g = resona_sinf(x) / resona_cosf(x);
  const float k = 1 / q; a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2;
 }
 float run(float x) {
  const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
  s1 = 2 * v1 - s1; s2 = 2 * v2 - s2;
  if (wabs(s1) < 1e-20f) s1 = 0; if (wabs(s2) < 1e-20f) s2 = 0;
  return v2;
 }
};

struct Tracker {
 float hist[2 * kHist];   // written twice so every lag reads contiguously
 float r[kLagMax + 2], e, pre1, pre2;
 uint32_t hw, count;
 float period, confidence;   // full-rate samples; 0..1
};

struct Shifter {
 float buf[kBuf]; uint32_t w;
 Lp aa; float aaHz;
 Tracker tr;
 float ratio, d, dOld, fade, fadeInc, restP; int fading;
};

inline void reset(Shifter &s) {
 for (uint32_t i = 0; i < kBuf; ++i) s.buf[i] = 0;
 s.w = 0; s.aa = Lp{}; s.aaHz = 0; s.ratio = 1;
 Tracker &t = s.tr;
 for (uint32_t i = 0; i < 2 * kHist; ++i) t.hist[i] = 0;
 for (uint32_t i = 0; i < kLagMax + 2; ++i) t.r[i] = 0;
 t.e = t.pre1 = t.pre2 = 0; t.hw = t.count = 0; t.period = 0; t.confidence = 0;
 s.d = kMinDelay; s.dOld = 0; s.fade = 0; s.fadeInc = 0; s.fading = 0; s.restP = 0;
}

// Ratio 1..4. The anti-alias low-pass (2-pole at 19 kHz / ratio) keeps what would fold back out of the delay
// line (a guitar has little above 5 kHz); it is retuned only when the ratio moves noticeably.
inline void setRatio(Shifter &s, float ratio) {
 s.ratio = wclip(ratio, 1, 4);
 const float hz = 19000 / s.ratio;
 if (wabs(hz - s.aaHz) > 30) { s.aaHz = hz; s.aa.set(hz, .7071f); }
}

// The tracker's period pick, run once per 128 samples: the first autocorrelation lobe that reaches 85 % of the
// strongest one (so octave-low picks are avoided), refined by a parabola.
inline void pick(Tracker &t) {
 const float norm = 1 / (t.e + 1e-12f);
 float best = 0; uint32_t lobeLag[12]; float lobeVal[12]; uint32_t lobes = 0;
 uint32_t tau = kLagMin;
 while (tau <= kLagMax && t.r[tau] > 0) ++tau;          // leave the lobe around lag 0
 float curVal = 0; uint32_t curLag = 0;
 for (; tau <= kLagMax; ++tau) {
  const float v = t.r[tau] * norm;
  if (v > 0) { if (v > curVal) { curVal = v; curLag = tau; } }
  else if (curLag) {
   if (lobes < 12) { lobeLag[lobes] = curLag; lobeVal[lobes] = curVal; ++lobes; }
   if (curVal > best) best = curVal; curVal = 0; curLag = 0;
  }
 }
 if (curLag && lobes < 12 && curLag < kLagMax) { lobeLag[lobes] = curLag; lobeVal[lobes] = curVal; ++lobes; if (curVal > best) best = curVal; }
 if (best < .6f || t.e < 1e-7f) { t.confidence = 0; return; }   // chords without a clear period, noise, silence
 for (uint32_t i = 0; i < lobes; ++i) if (lobeVal[i] >= .85f * best) {
  const uint32_t L = lobeLag[i];
  const float a = t.r[L - 1], b = t.r[L], c = t.r[L + 1], den = a - 2 * b + c;
  const float off = den < 0 ? wclip(.5f * (a - c) / den, -.5f, .5f) : 0;
  t.period = (static_cast<float>(L) + off) * kDecim; t.confidence = lobeVal[i];
  return;
 }
}

inline void track(Tracker &t, float x) {
 // two one-poles at ~750 Hz, then every 16th sample feeds the 3 kHz analysis
 t.pre1 += .095f * (x - t.pre1); t.pre2 += .095f * (t.pre1 - t.pre2);
 if (++t.count < kDecim) return;
 t.count = 0;
 const float v = t.pre2; constexpr float a = 1.f / 75;        // ~25 ms memory
 t.hist[t.hw] = v; t.hist[t.hw + kHist] = v;
 const float *h = &t.hist[t.hw + kHist];
 t.e += a * (v * v - t.e);
 for (uint32_t tau = kLagMin - 1; tau <= kLagMax + 1; ++tau) t.r[tau] += a * (v * h[-static_cast<int32_t>(tau)] - t.r[tau]);
 t.hw = (t.hw + 1) & (kHist - 1);
 if ((t.hw & 7) == 0) pick(t);
}

// 4-point Hermite read at a fractional delay behind the write head (delay >= 3).
inline float read(const Shifter &s, float delay) {
 const float p = static_cast<float>(s.w + kBuf) - delay; const uint32_t i = static_cast<uint32_t>(p);
 const float f = p - static_cast<float>(i);
 const float y0 = s.buf[(i - 1) & kMask], y1 = s.buf[i & kMask], y2 = s.buf[(i + 1) & kMask], y3 = s.buf[(i + 2) & kMask];
 const float c1 = .5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - .5f * y3, c3 = .5f * (y3 - y0) + 1.5f * (y1 - y2);
 return ((c3 * f + c2) * f + c1) * f + y1;
}

// How well the waveform behind the read head continues `jump` samples further back (positive) or forward
// (negative): 2xy / (xx + yy) over the last 8 x `points` samples. 1 = identical.
inline float similarity(const Shifter &s, uint32_t head, int32_t jump, uint32_t points) {
 const uint32_t b = head - static_cast<uint32_t>(jump);
 float xy = 0, e = 1e-12f;
 for (uint32_t k = 0; k < points; ++k) {
  const float u = s.buf[(head - 8 * k) & kMask], v = s.buf[(b - 8 * k) & kMask]; xy += u * v; e += u * u + v * v;
 }
 return 2 * xy / e;
}
// The jump (in samples, sub-sample accurate) nearest `target` that best continues the waveform: a coarse pass
// (+-span in `step`s, 24 points), the best one +-step/2 by 1 (32 points), then a parabola through its neighbours.
inline float refine(const Shifter &s, float delay, float target, int span, int step, float lo, float hi) {
 const uint32_t head = s.w + 2 * kBuf - static_cast<uint32_t>(delay);   // the read head's whole-sample position
 const int32_t t0 = static_cast<int32_t>(target + .5f);
 auto ok = [&](int32_t j) { const float d = delay + static_cast<float>(j); return d >= lo && d <= hi; };
 int32_t best = t0; float score = -2;
 if (step > 1) {
  for (int32_t o = -span; o <= span; o += step) { const int32_t j = t0 + o; if (!ok(j)) continue; const float v = similarity(s, head, j, 24); if (v > score) { score = v; best = j; } }
  const int32_t c = best; score = -2;
  for (int32_t o = -step / 2; o <= step / 2; ++o) { const int32_t j = c + o; if (!ok(j)) continue; const float v = similarity(s, head, j, 32); if (v > score) { score = v; best = j; } }
 } else {
  for (int32_t o = -span; o <= span; ++o) { const int32_t j = t0 + o; if (!ok(j)) continue; const float v = similarity(s, head, j, 32); if (v > score) { score = v; best = j; } }
 }
 float frac = 0;
 if (ok(best - 1) && ok(best + 1)) {
  const float l = similarity(s, head, best - 1, 32), r = similarity(s, head, best + 1, 32), den = l - 2 * score + r;
  if (den < 0) frac = wclip(.5f * (l - r) / den, -.5f, .5f);
 }
 return delay + static_cast<float>(best) + frac;
}

inline void startFade(Shifter &s, float newDelay, float length) {
 s.dOld = s.d; s.d = newDelay; s.fade = 0; s.fadeInc = 1 / (length < 32 ? 32 : length); s.fading = 1;
}

inline float process(Shifter &s, float x) {
 track(s.tr, x);
 s.buf[s.w] = s.aa.run(x);
 const float step = s.ratio - 1;
 const bool voiced = s.tr.confidence > 0;
 const float P = s.tr.period;
 if (!s.fading) {
  s.d -= step;
  if (step > 0) {
   // jump back by whole periods once the pointer would get too close to the write head during a fade
   const float J0 = voiced ? P * static_cast<float>(static_cast<uint32_t>((kVoicedJump + P - 1) / P)) : kUnvoicedJump;
   const float fadeLen = J0 / s.ratio;
   if (s.d < step * (fadeLen + 16) + kMinDelay + 1) {     // +16: room for the refinement to lengthen the jump
    const float hi = static_cast<float>(kBuf - 8);
    // a clear note: lock to its period; a looser one: search wider; no period: the best match near 25 ms
    const float nd = !voiced ? refine(s, s.d, J0, 96, 8, s.d + 64, hi)
                   : s.tr.confidence > .85f ? refine(s, s.d, J0, 4, 1, s.d + 64, hi) : refine(s, s.d, J0, 24, 4, s.d + 64, hi);
    startFade(s, nd, (nd - s.d) / s.ratio);
   }
  } else if (s.d > kRestWindow) {
   // back at rest. A held note first steps forward by whole periods (seamless), which can leave the head up to
   // one period behind; the rest of the way is taken where it cannot be heard: in silence, on a sound without a
   // period, or when the note changes. Otherwise the head goes straight into the first 1.4 ms.
   const bool held = voiced && s.tr.e > 1e-6f;
   if (held && s.restP == 0) {
    if (s.d - kMinDelay >= P) {
     const float n = static_cast<float>(static_cast<uint32_t>((s.d - kMinDelay) / P));
     startFade(s, refine(s, s.d, -n * P, 4, 1, kMinDelay, s.d - 1), 512);
    }
    s.restP = P;                                  // less than a period behind now; wait for a hidden moment
   } else if (!held || s.restP == 0 || wabs(P - s.restP) > .03f * s.restP) {
    startFade(s, refine(s, s.d, kMinDelay + 32 - s.d, 32, 4, kMinDelay, kRestWindow), 512); s.restP = 0;
   }
  }
  if (step > 0) s.restP = 0;
 } else {
  s.d -= step; s.dOld -= step;
  if (s.dOld < 3) s.dOld = 3;                    // a sudden ratio rise mid-fade: hold the old head at the edge
 }
 float y;
 if (s.fading) {
  s.fade += s.fadeInc;
  if (s.fade >= 1) { s.fading = 0; y = read(s, s.d); }
  else {
   const float g = s.fade * s.fade * (3 - 2 * s.fade);   // smoothstep; the two gains sum to one
   const float a = read(s, s.dOld); y = a + g * (read(s, s.d) - a);
  }
 } else y = read(s, s.d);
 s.w = (s.w + 1) & kMask;
 return y;
}
}  // namespace whammy
