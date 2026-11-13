// Rider Engine — original mono guitar processor for Night Rider, MIT.
//
// The guitar is the motorcycle. SPEED (set by the app as the bike moves) opens the whole engine:
//   - an idle throb: faint amplitude modulation at an engine-like rate that rises with speed and fades out
//   - drive: asymmetric soft clipping whose gain grows with the square of speed
//   - tone: a state-variable low-pass that opens from 2.4 to 7 kHz with speed (resonant in SPORT, like a wah)
//   - air rush: a short flanger whose sweep speeds up and deepens as the road blurs
// BOOST is the whammy: while it is held the pitch glides up to WHAMMY semitones (default +24) in one second
// and stays there; on release it glides back down in about 0.6 s. The shifter (whammy.h) sits first in the
// chain, like a whammy pedal before the drive: one read head runs through a delay line faster than the guitar
// is written and jumps back by whole periods of the note (tracked continuously), so held notes and power chords
// stay smooth. STYLE picks the engine: CRUISE (clean and warm), SPORT (crunch, resonant), TURBO (fuzz).
// STYLE and WHAMMY are smoothed like the other controls: a style turn morphs the drive, tone and throb, and a
// range turn while held glides the pitch. A measured make-up per style and speed keeps the engine at the dry
// guitar's loudness; at MIX 0 the guitar passes at unity (LEVEL scales only the wet; 80, the default, is unity).
// Every transcendental is outside the per-sample path.
#include <resona/module.h>
#include <resona/parameters.h>
#include <resona/math.h>
#include "whammy.h"
namespace {
enum { SPEED, BOOST, STYLE, MIX, LEVEL, WHAMMY, COUNT };
const resona_param params[] = {
 {"SPEED", 0, 100, 30, "%"}, {"BOOST", 0, 1, 0, ""}, {"STYLE", 0, 2, 1, ""},
 {"MIX", 0, 100, 100, "%"}, {"LEVEL", 0, 100, 80, "%"}, {"WHAMMY", 0, 24, 24, "st"},
};
const char *const styles[] = {"CRUISE", "SPORT", "TURBO"};
const resona_param_ext ext[] = {
 {}, {RESONA_PARAM_TOGGLE, 1, 0, nullptr}, {RESONA_PARAM_CHOICE | RESONA_PARAM_STEPPED, 1, 3, styles}, {}, {},
 {RESONA_PARAM_STEPPED, 1, 0, nullptr}
};
constexpr uint32_t kFlange = 512;                       // 10.7 ms at 48 kHz
constexpr float kRise = 1.f / 48000, kFall = 1.6f / 48000;   // full sweep up in 1 s, back down in 0.6 s
float absf(float x) { return x < 0 ? -x : x; }
float clip(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
float flush(float x) { return absf(x) < 1e-20f ? 0 : x; }
float soft(float x) { return x / (1 + absf(x)); }
// output limiter: linear to 0.8, then a soft knee toward 0.99 (no corner)
float limit(float x) { const float a = absf(x); if (a <= .8f) return x; const float t = (a - .8f) * 5, y = .8f + .19f * t / (1 + t); return x < 0 ? -y : y; }
// make-up per style at speed 0, .25, .5, .75, 1: the reciprocal of the DI's RMS gain through the engine (no shift)
constexpr float kMakeup[3][5] = {
 {1.1327f, 1.0180f, .8025f, .6410f, .5433f}, {1.0725f, .7954f, .5327f, .4319f, .4007f}, {1.1061f, .6285f, .4211f, .3852f, .4051f}};

struct Svf {   // trapezoidal state-variable filter, low-pass output
 float s1, s2, g, k, a1, a2, a3;
 void coeff(float hz, float q) {
  const float x = 3.141592654f * clip(hz, 40, 12000) / 48000, x2 = x * x;
  g = x * (1 + x2 * (0.333333333f + x2 * 0.133333333f)); k = 1 / clip(q, .5f, 8);
  a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2;
 }
 float lp(float x) { const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3; s1 = flush(2 * v1 - s1); s2 = flush(2 * v2 - s2); return v2; }
 void reset() { s1 = s2 = 0; }
};
struct State {
 resona_parameter_inbox inbox; float desired[COUNT], p[COUNT], sm[COUNT];
 Svf tone; float throbPh, throbInc, throbDepth, lfoPh, lfoInc, drive, flangeMix, fb, makeup;
 float dcX, dcY, flange[kFlange]; uint32_t fw;
 float glide; whammy::Shifter wham;
};

// per sub-block: everything that depends on the smoothed controls
void coefficients(State &s) {
 // everything follows the speed, which the app glides with the whammy, so a press never jumps
 const float v = clip(s.sm[SPEED] * .01f, 0, 1);
 // the style glides too: interpolate between the two nearest engines
 const float st = clip(s.sm[STYLE], 0, 2); const int a = st >= 2 ? 1 : static_cast<int>(st); const float f = st - static_cast<float>(a);
 const float driveMax[] = {3.f, 11.f, 26.f}, q[] = {.75f, 2.4f, 1.05f}, throb[] = {.06f, .04f, .04f};
 const auto mixf = [f, a](const float *t) { return t[a] + f * (t[a + 1] - t[a]); };
 s.drive = 1.1f + mixf(driveMax) * v * v;
 s.tone.coeff(2400 + 4700 * v, mixf(q));                  // a normal guitar at rest, opening with speed
 s.throbInc = (7 + 38 * v) / 48000.f;                      // the engine's firing rate rises with speed
 s.throbDepth = mixf(throb) * (1 - v) * (1 - v);           // a faint idle, gone once moving
 const float x = v * 4; const int k = x >= 4 ? 3 : static_cast<int>(x); const float u = x - static_cast<float>(k);
 const float m0 = kMakeup[a][k] + u * (kMakeup[a][k + 1] - kMakeup[a][k]), m1 = kMakeup[a + 1][k] + u * (kMakeup[a + 1][k + 1] - kMakeup[a + 1][k]);
 s.makeup = m0 + f * (m1 - m0);
 s.lfoInc = (.12f + 1.7f * v) / 48000.f;                   // air rush sweeps faster as the road blurs
 s.flangeMix = .08f + .14f * v; s.fb = .3f + .2f * v;       // a light air rush: it must not blur the glide
 whammy::setRatio(s.wham, resona_exp2f(s.glide * s.sm[WHAMMY] * (1.f / 12)));
}
void reset(void *p) {
 State &s = *static_cast<State *>(p);
 s.throbPh = s.lfoPh = s.dcX = s.dcY = 0; s.fw = 0;
 s.tone.reset();
 for (uint32_t i = 0; i < kFlange; ++i) s.flange[i] = 0;
 whammy::reset(s.wham); s.glide = 0;
 for (uint32_t n = 0; n < COUNT; ++n) s.sm[n] = s.p[n];
 coefficients(s);
}
void init(void *p, const resona_host *) {
 State &s = *static_cast<State *>(p); s.inbox = {};
 for (uint32_t n = 0; n < COUNT; ++n) s.p[n] = s.desired[n] = params[n].def;
 reset(p);
}
void set(void *p, uint32_t n, float value) {
 if (n >= COUNT) return; State &s = *static_cast<State *>(p);
 value = clip(value, params[n].min, params[n].max);
 if (n == STYLE || n == BOOST || n == WHAMMY) value = static_cast<float>(static_cast<uint32_t>(value + .5f));
 s.desired[n] = value; resona_parameter_publish(&s.inbox, n, value);
}
float get(const void *p, uint32_t n) { return n < COUNT ? static_cast<const State *>(p)->desired[n] : 0; }
// sine from a 0..1 phase without a transcendental: a parabola per half cycle
float sine01(float ph) { const float x = ph < .5f ? ph * 4 - 1 : 3 - ph * 4; return x * (2 - absf(x)) * -1; }

void process(void *p, const float *const *in, float *const *out, uint32_t frames) {
 State &s = *static_cast<State *>(p); float pending[12];
 const uint32_t bits = resona_parameter_consume(&s.inbox, pending);
 for (uint32_t n = 0; n < COUNT; ++n) if (bits & (1u << n)) s.p[n] = pending[n];
 for (uint32_t i = 0; i < frames; ++i) {
  if ((i & 15u) == 0) coefficients(s);
  for (uint32_t n = 0; n < COUNT; ++n) s.sm[n] += .0015f * (s.p[n] - s.sm[n]);
  const float dry = in[0][i];
  // whammy glide, then the shifter (at rest it sits within 1.4 ms of the input, uncoloured)
  s.glide = clip(s.glide + (s.p[BOOST] > .5f ? kRise : -kFall), 0, 1);
  const float x = whammy::process(s.wham, dry);
  // drive into the tone filter
  const float driven = soft(x * s.drive + .08f * x * x * s.drive) / (1 + .04f * s.drive);
  float y = s.tone.lp(driven);
  // idle throb
  s.throbPh += s.throbInc; if (s.throbPh >= 1) s.throbPh -= 1;
  y *= 1 - s.throbDepth * (.5f + .5f * sine01(s.throbPh));
  // air rush: a short flanger with feedback
  s.lfoPh += s.lfoInc; if (s.lfoPh >= 1) s.lfoPh -= 1;
  const float tri = s.lfoPh < .5f ? s.lfoPh * 2 : 2 - s.lfoPh * 2;
  const float d = 24 + 168 * tri, rp = static_cast<float>(s.fw) - d + kFlange;
  const uint32_t r0 = static_cast<uint32_t>(rp); const float fr = rp - static_cast<float>(r0);
  const float tap = s.flange[r0 % kFlange] * (1 - fr) + s.flange[(r0 + 1) % kFlange] * fr;
  s.flange[s.fw] = flush(y + tap * s.fb); s.fw = (s.fw + 1) % kFlange;
  y = y * (1 - s.flangeMix * .5f) + tap * s.flangeMix;
  // mix, level, DC block, soft limit; MIX 0 is the untouched guitar at unity
  const float mix = s.sm[MIX] * .01f;
  const float combined = dry * (1 - mix) + soft(y * 1.2f) * s.makeup * s.sm[LEVEL] * (1.f / 80) * mix;
  const float dc = combined - s.dcX + .9985f * s.dcY; s.dcX = combined; s.dcY = flush(dc);
  out[0][i] = limit(dc);
 }
}
void trial(void *p, uint32_t phase) {
 set(p, SPEED, 100); set(p, STYLE, 2); set(p, WHAMMY, 24); set(p, BOOST, phase == RESONA_CPU_TRIAL_RELEASE ? 0 : 1);
 if (phase != RESONA_CPU_TRIAL_RELEASE) static_cast<State *>(p)->glide = 1;   // measure the full +24 sweep at once
}
const resona_cpu_profile cpu = {RESONA_CPU_PROFILE_VERSION, sizeof(resona_cpu_profile), 0, trial};
const resona_module module = {RESONA_MODULE_ABI, sizeof(resona_module),
 "org.resona.demo.night-rider.engine", "Rider Engine", "0.2.1",
 RESONA_LAYOUT_MONO, RESONA_RATE_48000, sizeof(State), COUNT, params, ext,
 init, reset, set, process, nullptr, nullptr, get, &cpu, nullptr, RESONA_MEMORY_FAST, nullptr};
}
extern "C" RESONA_EXPORT const resona_module *resona_module_get() { return &module; }
