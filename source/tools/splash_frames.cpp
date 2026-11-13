// MIT. Renders the boot-splash source frames with the app's own ride renderer and HUD type (host build, see
// make_splash.py). Raw RGB565 320x172: 0 the scene at rest, 1 the scene at full whammy speed, 2 the bike and rider
// alone at speed on black, then the HUD on black at 20, 120 and 220 km/h (holding) and at 300 (WHAMMY +24).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "ride.h"
static uint16_t fb[320 * 172];
static void hud(int kmh, bool fast) {
 using namespace ride_art;
 const uint16_t ink = RESONA_RGB(255, 246, 222), cyan = RESONA_RGB(110, 236, 240), amber = RESONA_RGB(255, 196, 96);
 ride::type(fb, 320, title, "NIGHT RIDER", 7, 6, ink, false);
 ride::type(fb, 320, label, "SPORT", 313, 7, cyan, true);
 char n[6]; snprintf(n, sizeof n, "%d", kmh);
 ride::type(fb, 320, num, n, 7, 132, ink, false);
 ride::type(fb, 320, small, "KM/H", 9 + (ride::typeWidth10(num, n) + 5) / 10 + 3, 150, ink, false);
 ride::type(fb, 320, small, fast ? "WHAMMY +24" : "HOLD: +24", 313, 158, fast ? amber : ink, true);
}
int main(int argc, char **argv) {
 static ride::Ride r; ride::Shared *sh = (ride::Shared *)calloc(1, sizeof(ride::Shared)); ride::init(r, *sh, 0);
 FILE *f = fopen(argc > 1 ? argv[1] : "frames.raw", "wb");
 for (int t = 0; t < 1500; t += 44) ride::update(r, 44, 1, 0, false);
 ride::render(r, *sh, fb, 320, nullptr); fwrite(fb, 2, 320 * 172, f);                 // 0 rest
 static ride::Ride fast = r; for (int t = 0; t < 1400; t += 44) ride::update(fast, 44, 1, 0, true);
 ride::render(fast, *sh, fb, 320, nullptr); fwrite(fb, 2, 320 * 172, f);              // 1 full speed
 memset(fb, 0, sizeof fb); ride::drawBike(fast, *sh, fb, 320); ride::drawRider(fast, *sh, fb, 320);
 fwrite(fb, 2, 320 * 172, f);                                                         // 2 bike + rider on black
 for (int k : {20, 120, 220}) { memset(fb, 0, sizeof fb); hud(k, false); fwrite(fb, 2, 320 * 172, f); }   // 3-5
 memset(fb, 0, sizeof fb); hud(300, true); fwrite(fb, 2, 320 * 172, f);               // 6 the whammy at +24
 fclose(f);
}
