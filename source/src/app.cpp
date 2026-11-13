// Night Rider — a native Box App using only the public platform SDK. MIT.
//
// The illustration rides, and the bike's speed is the guitar's pitch: at rest it rolls at 20 km/h with no
// shift; holding the big knob is a whammy that accelerates to top speed (the WHAMMY range, +24 semitones by
// default) and stays there while held, and letting go glides back down. Turning the big knob sets the range;
// playing shakes the bike. The Rider Engine pack (created by the App in its own effect graph, Guitar -> engine
// -> Output) runs the same glide, so the km/h on screen reads the pitch you hear.
//
// The Web, Mobile and MIDI controllers (controller/, CCs on channel 1, see README.md) reach every setting the
// knobs reach, plus MIX, LEVEL, a THROTTLE that holds the whammy at any interval, and four hold pads: BEND
// (the big knob's whammy hold), +12 (whatever the range), NITRO (TURBO at +24) and DRY.
#include <resona/platform.h>
#include <resona/ui.h>
#include "demo_capture.h"
#include <resona/startup_helpers.h>
#include "splash_data.h"
#include "ride.h"
#include "own_audio.h"
#include "trip_guard.h"
#include "controller_link.h"
namespace {
const resona_platform *P;
const char *const packId = "org.resona.demo.night-rider.engine";  // dsp/resona.project.json
own_audio::Graph g_audio;
const char *const styles[] = {"CRUISE", "SPORT", "TURBO"};
enum { SPEED, BOOST, STYLE, MIX, LEVEL, WHAMMY, PARAMS };
struct SavedV2 { uint32_t magic, version; int32_t range, style; };
struct Saved { uint32_t magic, version; int32_t range, style, mix, level; };   // version 3
// The controllers' MIDI map: CC on channel 1 (controller/shared/profile.js says the same).
enum { CC_RANGE = 20, CC_STYLE, CC_THROTTLE, CC_DRY, CC_MIX, CC_LEVEL, CC_SPEED, CC_WHAMMY = 30, CC_OCTAVE, CC_NITRO, CC_HOLD_DRY };
struct State {
 int range, style, compare, keyDown[2], boostHeld, ready, error, sentSpeed, sentBoost;
 uint32_t now, nextSetup, saveAt, drawAt, drawEvery, downAt, hudKey; float activity;
 ride::Ride ride; ride::Shared *sh;
 uint32_t stDraws, stDrawSum, stDrawMax, stSlow, stFrames, stFrameMax, lastFrameAt, stResetAt;   // bench stats
 trip_guard::Guard guard;   // back-off after the OS bypasses the engine for CPU
 int mix, level, throttle, kmhRead, padWhammy, padOctave, padNitro, padDry;   // controller settings and held pads
 int latched, sentWhammy, sentStyle, sentMix, sentLevel; float glide;   // the WHAMMY range the engine glides on
 ctl::Link link;
};
constexpr int kBinds = 11;
ctl::Bind kBind[kBinds];
void bind(State *s) {
 const ctl::Bind b[kBinds] = {
  {CC_RANGE, ctl::RANGE, 1, 24, &s->range}, {CC_STYLE, ctl::STEPS, 0, 2, &s->style}, {CC_THROTTLE, ctl::RANGE, 0, 24, &s->throttle},
  {CC_DRY, ctl::STEPS, 0, 1, &s->compare}, {CC_MIX, ctl::RANGE, 0, 100, &s->mix}, {CC_LEVEL, ctl::RANGE, 0, 100, &s->level},
  {CC_SPEED, ctl::READ_RANGE, 20, 300, &s->kmhRead},
  {CC_WHAMMY, ctl::TRIGGER, 0, 1, nullptr}, {CC_OCTAVE, ctl::TRIGGER, 0, 1, nullptr}, {CC_NITRO, ctl::TRIGGER, 0, 1, nullptr}, {CC_HOLD_DRY, ctl::TRIGGER, 0, 1, nullptr}};
 for (int i = 0; i < kBinds; ++i) kBind[i] = b[i];
}
int clamp(int x, int a, int b) { return x < a ? a : (x > b ? b : x); }
void checked(State *s, int result) { if (result < 0) s->error = result; }   // an audio failure, shown until audio is fine again

// The whammy is engaged by the big knob's hold, a pad or the controller's THROTTLE; the pads override while held.
int engaged(const State *s) { return s->boostHeld || s->padWhammy || s->padOctave || s->padNitro || s->throttle > 0; }
int rangeNow(const State *s) { return s->padNitro ? 24 : s->padOctave ? 12 : (s->boostHeld || s->padWhammy || !s->throttle) ? s->range : s->throttle; }
int styleNow(const State *s) { return s->padNitro ? 2 : s->style; }
int dryNow(const State *s) { return s->compare || s->padDry; }
// The engine's pitch is its glide x WHAMMY, so the range only follows a new setting at rest or while engaged:
// a release glides back down on the range it went up with.
void retarget(State *s) { if (engaged(s)) s->latched = rangeNow(s); else if (s->glide <= 0) s->latched = s->range; }
void send(State *s, int param, int value, int &sent, bool all) {
 if (all || value != sent) { checked(s, P->effect_param_set(g_audio.h[0], static_cast<uint32_t>(param), static_cast<float>(value))); sent = value; }
}
void parameters(State *s, bool all) {
 if (!s->ready) return;
 retarget(s);
 const int speed = static_cast<int>(s->ride.speed * 100.f + .5f), boost = engaged(s) ? 1 : 0;
 send(s, SPEED, speed, s->sentSpeed, all);
 send(s, BOOST, boost, s->sentBoost, all);
 send(s, STYLE, styleNow(s), s->sentStyle, all);
 send(s, MIX, dryNow(s) ? 0 : s->mix, s->sentMix, all);
 send(s, LEVEL, s->level, s->sentLevel, all);
 send(s, WHAMMY, s->latched, s->sentWhammy, all);
}
void save(State *s) {
 Saved v = {0x4e525431u, 3, s->range, s->style, s->mix, s->level};
 if (P->app_store_write(0, &v, sizeof v) < 0) P->log("Night Rider: settings not saved");   // not an audio error
 s->saveAt = 0;
}
// The OS bypassed the engine for CPU (API 1 reports it as a processor fault): show CPU LIMIT, drop the graph and
// rebuild it after a wait that grows with each trip (trip_guard.h).
void trip(State *s) { s->ready = 0; s->error = RESONA_ERR_BUDGET; s->nextSetup = s->now + trip_guard::tripped(s->guard, s->now); own_audio::stop(P, g_audio); }
// The App owns audio (own_audio.h); until the engine is installed, retry once a second.
void setup(State *s) {
 if (s->now < s->nextSetup) return;
 s->nextSetup = s->now + 1000;
 if (s->ready && trip_guard::faulted(P, g_audio.h[0])) { trip(s); return; }   // polled once a second while audio runs
 int rc = own_audio::start(P, g_audio, packId);
 if (rc < 0) { s->ready = 0; s->error = rc; return; }
 resona_effect_info fi = {}; rc = P->effect_info(g_audio.h[0], &fi);
 if (rc != RESONA_OK || fi.param_count != PARAMS) { s->ready = 0; s->error = rc < 0 ? rc : -20; return; }
 if (!s->ready) { rc = P->effect_enable(g_audio.h[0], 1); if (rc < 0) { s->ready = 0; s->error = rc; return; } s->ready = 1; s->error = 0; parameters(s, true); }
 else if (s->error) { s->error = 0; parameters(s, true); }   // a stale failure: resend everything, keep only a fresh one
}
void input(State *s, uint32_t kind, uint32_t which, int value) {
 s->drawAt = s->now;
 if (which == RESONA_INPUT_SELECTOR) {
  if (kind == RESONA_EVENT_TURN) { s->style = (s->style + value % 3 + 3) % 3; s->saveAt = s->now + 1500; parameters(s, false); }
  else if (kind == RESONA_EVENT_CLICK) { s->compare = !s->compare; parameters(s, false); }
  else if (kind == RESONA_EVENT_LONG) save(s);
 } else {
  if (kind == RESONA_EVENT_TURN) {
   s->range = clamp(s->range + value, 1, 24); s->saveAt = s->now + 1500;
   parameters(s, false);
  }
  else if (kind == RESONA_EVENT_LONG) { if (!s->keyDown[1]) { s->boostHeld = 1; s->downAt = s->now; } }   // synthetic long: a 1 s whammy
 }
}
void key(State *s, int which, int down) {
 s->drawAt = s->now; s->keyDown[which] = down;
 if (which == 1) { s->boostHeld = down ? 1 : 0; s->downAt = s->now; }   // the whammy follows the raw key at once
}

// A controller changed a value or pressed a pad: do what the knobs do for that setting.
void control(State *s, const ctl::Msg &m) {
 s->drawAt = s->now;
 if (m.cc == CC_WHAMMY) s->padWhammy = m.down;
 else if (m.cc == CC_OCTAVE) s->padOctave = m.down;
 else if (m.cc == CC_NITRO) s->padNitro = m.down;
 else if (m.cc == CC_HOLD_DRY) s->padDry = m.down;
 else if (m.cc == CC_RANGE || m.cc == CC_STYLE || m.cc == CC_MIX || m.cc == CC_LEVEL) s->saveAt = s->now + 1500;
 parameters(s, false);
}
const uint16_t kInk = RESONA_RGB(255, 246, 222), kCyan = RESONA_RGB(110, 236, 240), kAmber = RESONA_RGB(255, 196, 96);
void draw(State *s) {
 uint16_t *px = nullptr; uint32_t stride = 0; resona_ui *ui = P->screen_begin(&px, &stride); if (!ui || !px) return;
 if (s->sh) ride::render(s->ride, *s->sh, px, stride, P->millis);
 else ui->clear(ui->ctx, RESONA_RGB(10, 18, 40));
 using namespace ride_art;
 ride::type(px, stride, title, "NIGHT RIDER", 7, 6, kInk, false);
 ride::type(px, stride, label, dryNow(s) ? "DRY" : styles[styleNow(s)], 313, 7, kCyan, true);
 char n[6]; P->format(n, sizeof n, "%d", ride::kmh(s->ride));
 ride::type(px, stride, num, n, 7, 132, kInk, false);
 ride::type(px, stride, small, "KM/H", 9 + (ride::typeWidth10(num, n) + 5) / 10 + 3, 150, kInk, false);
 char hint[16]; const int semis = static_cast<int>(s->ride.speed * 24 + .5f);
 if (dryNow(s)) P->format(hint, sizeof hint, "DRY GUITAR");
 else if (engaged(s) || semis > 0) P->format(hint, sizeof hint, "WHAMMY +%d", semis);
 else P->format(hint, sizeof hint, "HOLD: +%d", s->range);
 ride::type(px, stride, small, hint, 313, 158, engaged(s) || semis > 0 ? kAmber : kInk, true);
#if !defined(RESONA_LAB_HOST)
 if (s->error) {
  ui->rect(ui->ctx, 0, 164, 320, 8, RESONA_RGB(10, 18, 40)); char line[48];
  if (s->error == RESONA_ERR_PACK_ID) P->format(line, sizeof line, "INSTALL RIDER ENGINE IN LAB");
  else if (s->error == RESONA_ERR_BUDGET) P->format(line, sizeof line, "CPU LIMIT"); else P->format(line, sizeof line, "ENGINE STATUS %d", s->error);
  ui->text(ui->ctx, 6, 165, 1, RESONA_RGB(255, 195, 128), line);
 }
#endif
 if (!s->sh) ui->text(ui->ctx, 6, 90, 1, RESONA_RGB(255, 195, 128), "NOT ENOUGH APP MEMORY");
 demo_capture::remember(px, stride); P->screen_present(0, 0, 0, 0);
}
#ifdef RESONA_DEMO_BENCH
// Bench stats on the capture channel: F0 7D 52 42 58 53 03 token F7 reads, 04 resets (as in CREATURE).
bool stats(State *s, const resona_app_event *e) {
 if (e->kind != RESONA_APP_EV_MIDI || e->u.midi.len < 9) return false;
 const uint8_t *m = e->u.midi.data;
 if (m[0] != 0xf0 || m[1] != 0x7d || m[2] != 0x52 || m[3] != 0x42 || m[4] != 0x58 || m[5] != 0x53 || (m[6] != 3 && m[6] != 4)) return false;
 if (m[6] == 4) { s->stDraws = s->stDrawSum = s->stDrawMax = s->stSlow = s->stFrames = s->stFrameMax = 0; s->stResetAt = P->millis(); if (s->sh) s->sh->msBg = s->sh->msBike = 0; }
 const uint32_t v[11] = {s->stDraws, s->stDrawSum, s->stDrawMax, s->stSlow, s->stFrames, s->stFrameMax, s->drawEvery, s->now,
                         s->sh ? s->sh->msBg : 0, s->sh ? s->sh->msBike : 0, P->millis() - s->stResetAt};
 uint8_t out[60] = {0xf0, 0x7d, 0x52, 0x42, 0x58, 0x53, 0x40, m[6], m[7]}; int k = 9;
 for (int i = 0; i < 11; ++i) for (int b = 0; b < 4; ++b) out[k++] = static_cast<uint8_t>((v[i] >> (7 * b)) & 127);
 out[k++] = 0xf7; (void)P->midi_send(RESONA_MIDI_USB, out, static_cast<uint32_t>(k)); return true;
}
#endif
int init(void *state, const resona_platform *platform) {
 State *s = static_cast<State *>(state); P = platform; s->range = 24; s->style = 1; s->mix = 100; s->level = 80; s->latched = 24;
 s->sentSpeed = s->sentBoost = s->sentStyle = s->sentMix = s->sentLevel = s->sentWhammy = -1;
 Saved v = {}; const int rc = P->app_store_read(0, &v, sizeof v);
 if (rc != RESONA_OK || v.magic != 0x4e525431u || v.version != 3) {   // 0.2 settings: range and style only
  SavedV2 v2 = {}; v = {};
  if (P->app_store_read(0, &v2, sizeof v2) == RESONA_OK && v2.magic == 0x4e525431u && v2.version == 2) v = {v2.magic, 3, v2.range, v2.style, 100, 80};
 }
 if (v.magic == 0x4e525431u && v.version == 3 && v.range >= 1 && v.range <= 24 && v.style >= 0 && v.style < 3) {
  s->range = v.range; s->style = v.style; s->mix = clamp(v.mix, 0, 100); s->level = clamp(v.level, 0, 100); s->latched = s->range;
 }
 bind(s);
 P->input_config(RESONA_INPUT_SELECTOR, 0); P->input_config(RESONA_INPUT_CUSTOMIZE, 0);
 // the boot splash (tools/make_splash.py): written only when it differs; it plays from the next power-up
 { const int rc = resona_startup_splash_apply(P, kSplash, sizeof kSplash);
   if (rc == RESONA_STARTUP_REBOOT_REQUIRED) P->log("Night Rider: boot splash updated; it plays from the next power-up");
   else if (rc < 0 && rc != RESONA_ERR_UNSUPPORTED) P->log("Night Rider: boot splash not applied"); }
 s->sh = static_cast<ride::Shared *>(P->alloc(sizeof(ride::Shared), 4));
 if (s->sh) ride::init(s->ride, *s->sh, 0); else P->log("Night Rider: app heap allocation failed");
 P->log("Night Rider: native ride UI, owns audio: Guitar -> Rider Engine"); return 0;
}
void event(void *state, const resona_app_event *e) {
 State *s = static_cast<State *>(state);
#ifdef RESONA_DEMO_BENCH
 if (stats(s, e)) return;
#endif
 if (demo_capture::event(P, e)) return;
 ctl::Msg m;
 if (ctl::receive(P, s->link, kBind, kBinds, e, m)) { if (m.changed) control(s, m); return; }
 if (e->kind == RESONA_APP_EV_INPUT) input(s, e->u.input.kind, e->u.input.input, e->u.input.value);
 else if (e->kind == RESONA_APP_EV_KEY && e->u.key.key < 2) key(s, static_cast<int>(e->u.key.key), e->u.key.down != 0);
#ifdef RESONA_DEMO_BENCH
 // bench only: channel-16 CC20..27 play the encoders and keys
 else if (e->kind == RESONA_APP_EV_MIDI && e->u.midi.len >= 3) {
  const uint8_t *m = e->u.midi.data; if (m[0] != 0xbf) return;
  if (m[1] == 20 || m[1] == 21) input(s, RESONA_EVENT_TURN, m[1] - 20, static_cast<int>(m[2]) - 64);
  else if (m[1] == 22 || m[1] == 23) key(s, m[1] - 22, m[2] >= 64);
  else if (m[1] >= 24 && m[1] <= 27 && m[2]) input(s, m[1] >= 26 ? RESONA_EVENT_LONG : RESONA_EVENT_CLICK, (m[1] - 24) % 2, 1);
 }
#endif
 // audio restarted or the engine was bypassed for CPU (API 1: tripped_role is -1, the fault shows in effect_info)
 else if (e->kind == RESONA_APP_EV_AUDIO) { if (e->u.audio.tripped_role >= 0 || (s->ready && trip_guard::faulted(P, g_audio.h[0]))) trip(s); }
 else if (e->kind == RESONA_APP_EV_CHANGED && e->u.changed.what == RESONA_CHANGED_MODELS && !s->ready && s->error != RESONA_ERR_BUDGET) s->nextSetup = s->now;
}
void frame(void *state, uint32_t ms) {
 if (demo_capture::paused(P)) return;
 State *s = static_cast<State *>(state); s->now += ms; setup(s);
 { const uint32_t t = P->millis(); if (s->lastFrameAt) { const uint32_t gap = t - s->lastFrameAt; if (gap > s->stFrameMax) s->stFrameMax = gap; } s->lastFrameAt = t; ++s->stFrames; }
 // the guitar: a fast attack, slow release envelope of the input meter shakes the bike (never the pitch)
 uint8_t in = 0, out = 0; P->meters(&in, &out, nullptr);
 const float target = static_cast<float>(in) / 9;
 s->activity += (target - s->activity) * (static_cast<float>(ms) / (static_cast<float>(ms) + (target > s->activity ? 60.f : 600.f)));
 // a synthetic long press (MIDI CC) holds the whammy for a second
 if (s->boostHeld && !s->keyDown[1] && s->now - s->downAt >= 1000) s->boostHeld = 0;
 if (s->saveAt && s->now >= s->saveAt) save(s);
 // the engine's glide, mirrored: up to the range in 1 s, back down in 0.6 s
 { const float dt = static_cast<float>(ms) / 1000.f, g = s->glide + (engaged(s) ? dt : -1.6f * dt); s->glide = g < 0 ? 0 : (g > 1 ? 1 : g); }
 retarget(s);
 if (s->sh) {
  ride::update(s->ride, ms, static_cast<float>(s->latched) / 24.f, s->activity, engaged(s) != 0);
  // at the top of the glide the engine follows a new range in about 35 ms; so does the bike
  if (engaged(s) && s->glide >= 1) { const float k = static_cast<float>(ms) / 35.f; s->ride.speed += (static_cast<float>(s->latched) / 24.f - s->ride.speed) * (k > 1 ? 1 : k); }
  s->kmhRead = ride::kmh(s->ride);
 }
 parameters(s, false);
 ctl::sync(P, s->link, kBind, kBinds, s->now);
 if (s->now >= s->drawAt) {
  const uint32_t t0 = P->millis(); draw(s); const uint32_t took = P->millis() - t0;
  ++s->stDraws; s->stDrawSum += took; if (took > s->stDrawMax) s->stDrawMax = took; if (took >= 20) ++s->stSlow;
  // draw every frame unless a draw takes more than half of one; then back off
  s->drawEvery = took * 2 > 33 ? (took * 2 > 100 ? 100 : took * 2) : 33; s->drawAt = s->now + s->drawEvery;
 }
}
const resona_app app = {RESONA_PLATFORM_VERSION, sizeof(resona_app), RESONA_APP_ID, RESONA_APP_NAME, RESONA_APP_VERSION,
                        sizeof(State), sizeof(ride::Shared) + 4096, init, event, frame};
}
extern "C" const resona_app *resona_app_get() { return &app; }
