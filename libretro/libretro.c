// ReSys12: libretro frontend. Content = a MAME System 12, System 11 or System 10 zip (parent sets, namcoc76.zip next
// to the System 11 sets). GPL-2.0-or-later.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libretro.h"
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"
#include "plugins.h"
#include "../src/psx/frontend/plugin_lib.h"
#include "../src/psx/plugins/dfsound/out.h"
#include "../src/psx/plugins/dfsound/spu_config.h"
#include "s12.h"
#include "s12_rom.h"
#include "s12_state.h"
#include "s12_sound.h"
#include "s12_cdxa.h"
#include "s12_cdmix.h"
#include "s11.h"
#include "s11_c76.h"
#include "s10.h"
#include "s10_state.h"
#include "s10_exio.h"
#include "s10_memp3.h"
#include "new_dynarec/new_dynarec.h"

extern void rs12_psx_bind(int spu);
extern void (*rs12_GPUrearmedCallbacks)(const struct rearmed_cbs *);
extern void CALLBACK SPUirq(int);
extern void CALLBACK SPUschedule(unsigned int);

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

// gpu_async.c statistics (VRAM read rectangles); this frontend does not show them
volatile int rs12_rw_hit[5], rs12_rw_max[11], rs12_rw_full[6];

static S12Rom s_rom;
static int s_vertical; // the screen is turned a quarter: the picture is 3:4
static int s_loaded;
static S12SoundBatch *s_snd_prev;
static int16_t s_snd_buf[S12_SOUND_MAX_FRAMES * 2];

// ---- System 10 audio: dfsound's output driver; SPUasync (once a frame, at vblank) feeds 44100 Hz stereo ----
#define SND10_MAX (44100 / 30 * 2)
static int16_t s_snd10[SND10_MAX * 2];
static size_t s_snd10_n; // frames

static int snd_init(void) { return 0; }
static void snd_finish(void) {}
static int snd_busy(void) { return 0; }
static void snd_feed(void *buf, int bytes)
{
	size_t n = (size_t)bytes / 4;
	if (s_snd10_n + n > SND10_MAX)
		n = SND10_MAX - s_snd10_n;
	memcpy(s_snd10 + s_snd10_n * 2, buf, n * 4);
	s_snd10_n += n;
}
void out_register_libretro(struct out_driver *drv)
{
	drv->name = "libretro";
	drv->init = snd_init;
	drv->finish = snd_finish;
	drv->busy = snd_busy;
	drv->feed = snd_feed;
}

// ---- video: gpulib hands the displayed VRAM rectangle to pl_vout_flip (15bpp BGR555 or 24bpp BGR) ----
#define FB_W 1024
#define FB_H 512
static uint16_t s_fb[FB_W * FB_H];
static int s_fb_w = 640, s_fb_h = 480;

static int vout_open(void) { return 0; }
static void vout_close(void) {}
static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp) { (void)w; (void)h; (void)raw_w; (void)raw_h; (void)bpp; }

static void vout_flip(const void *vram, int vram_offset, int bgr24, int x, int y, int w, int h, int dims_changed)
{
	(void)x; (void)y; (void)dims_changed;
	if (w <= 0 || h <= 0 || w > FB_W || h > FB_H)
		return;
	s_fb_w = w;
	s_fb_h = h;
	if (!vram) {
		memset(s_fb, 0, sizeof(s_fb));
		return;
	}
	const uint8_t *src = (const uint8_t *)vram + vram_offset;
	for (int l = 0; l < h; l++, src += 2048) {
		uint16_t *d = s_fb + l * w;
		if (bgr24) {
			const uint8_t *s = src;
			for (int i = 0; i < w; i++, s += 3)
				d[i] = (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3);
		} else {
			const uint16_t *s = (const uint16_t *)src;
			for (int i = 0; i < w; i++) {
				const uint16_t p = s[i];
				d[i] = (uint16_t)((p & 0x1f) << 11 | (p & 0x3e0) << 1 | (p >> 10 & 0x1f));
			}
		}
	}
}

// ---- input: RetroPad -> JAMMA. Y X B A L R L2 R2 = buttons 1-8, Select = coin, L3 = test, R3 = service ----
static const struct { unsigned id; uint32_t bit; } k_map[] = {
	{ RETRO_DEVICE_ID_JOYPAD_UP, S12_IN_UP }, { RETRO_DEVICE_ID_JOYPAD_DOWN, S12_IN_DOWN },
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, S12_IN_LEFT }, { RETRO_DEVICE_ID_JOYPAD_RIGHT, S12_IN_RIGHT },
	{ RETRO_DEVICE_ID_JOYPAD_Y, S12_IN_B1 }, { RETRO_DEVICE_ID_JOYPAD_X, S12_IN_B2 },
	{ RETRO_DEVICE_ID_JOYPAD_B, S12_IN_B3 }, { RETRO_DEVICE_ID_JOYPAD_A, S12_IN_B4 },
	{ RETRO_DEVICE_ID_JOYPAD_L, S12_IN_B5 }, { RETRO_DEVICE_ID_JOYPAD_R, S12_IN_B6 },
	{ RETRO_DEVICE_ID_JOYPAD_L2, S12_IN_B7 }, { RETRO_DEVICE_ID_JOYPAD_R2, S12_IN_B8 },
	{ RETRO_DEVICE_ID_JOYPAD_START, S12_IN_START }, { RETRO_DEVICE_ID_JOYPAD_R3, S12_IN_SERVICE },
};

// gun games: a lightgun (or the mouse RetroArch shows as one) aims, the left stick aims where it points; the trigger
// is button 1 (Golgo 13: button 2, scope sensor = aux A). Wheel games: left stick X or d-pad steers, R2 gas, L2 brake.
// Lever games: left stick Y
static int16_t in_st(unsigned pl, unsigned dev, unsigned idx, unsigned id) { return input_state_cb(pl, dev, idx, id); }
static float stick(unsigned pl, unsigned id)
{
	const float a = in_st(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, id) / 32767.0f;
	return a > -0.08f && a < 0.08f ? 0.0f : a > 1.0f ? 1.0f : a;
}
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static float s_test_analog; // ponytail: one scripted axis (lever, wheel); gun games shoot the screen centre
static void analog_input(void)
{
	const int kind = s12_analog_kind(s12.game);
	if (kind == S12_ANALOG_GUN) {
		static float gx[2] = { 0.5f, 0.5f }, gy[2] = { 0.5f, 0.5f };
		const int golgo = s12.game->state == S12_S_GOLGO13;
		for (unsigned pl = 0; pl < 2; pl++) {
			const float sx = stick(pl, RETRO_DEVICE_ID_ANALOG_X), sy = stick(pl, RETRO_DEVICE_ID_ANALOG_Y);
			if (sx != 0.0f || sy != 0.0f) {
				gx[pl] = clamp01(0.5f + sx * 0.55f);
				gy[pl] = clamp01(0.5f + sy * 0.55f);
			} else if (!in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN)) {
				const int lx = in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
				const int ly = in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
				if (lx || ly) { // (0,0 = no gun input)
					gx[pl] = (lx + 32768) / 65535.0f;
					gy[pl] = (ly + 32768) / 65535.0f;
				}
			}
			s12_set_gun(pl, gx[pl], gy[pl]);
			if (in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER))
				s12.in.player[pl] |= golgo ? S12_IN_B2 : S12_IN_B1;
			if (golgo && in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A))
				s12.in.player[pl] |= S12_IN_B7;
			if (in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START))
				s12.in.player[pl] |= S12_IN_START;
			if (in_st(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SELECT))
				s12.in.system |= pl ? S12_SYS_COIN2 : S12_SYS_COIN1;
		}
	} else if (kind == S12_ANALOG_WHEEL) {
		// d-pad: full lock in 1/4 s, back to centre when released; the stick adds
		static float dsteer;
		const uint32_t p = s12.in.player[0];
		if (p & S12_IN_LEFT)
			dsteer = dsteer > -1.0f ? dsteer - 0.07f : -1.0f;
		else if (p & S12_IN_RIGHT)
			dsteer = dsteer < 1.0f ? dsteer + 0.07f : 1.0f;
		else
			dsteer = dsteer > 0.1f ? dsteer - 0.1f : dsteer < -0.1f ? dsteer + 0.1f : 0.0f;
		float steer = stick(0, RETRO_DEVICE_ID_ANALOG_X) + dsteer + s_test_analog;
		steer = steer < -1.0f ? -1.0f : steer > 1.0f ? 1.0f : steer;
		s12_set_wheel(steer, (p & S12_IN_B8) ? 1.0f : 0.0f, (p & S12_IN_B7) ? 1.0f : 0.0f);
	} else if (kind == S12_ANALOG_LEVER) {
		s12_set_lever(-stick(0, RETRO_DEVICE_ID_ANALOG_Y) + s_test_analog);
	}
}

// ---- cabinet Test / Service buttons, as the blueflash core: on the stick clicks (L3 / R3 of player 1), or for arcade
// panels and wheels with no stick to click, a chord held 5 s presses the button once for 0.2 s the moment the 5 s are
// reached (Start alone = Test, Start + B = Service; B during a Start hold turns it into the other chord). Start and B
// still reach the game during the hold. System 10's Test is a slide switch: the hold (or L3) flips it ----
static int s_test_hold, s_service_hold; // resys12_service_mode / resys12_service1: 1 = long hold, 0 = stick click
static struct hold { unsigned held, press; int fired; } s_hold_test, s_hold_service;

// 1 on the frame the 5 s are reached, then (press frames) while the press lasts
static int hold_update(struct hold *h, int held, unsigned press)
{
	const unsigned threshold = 299; // 5 s at 59.82 Hz
	if (!held)
		h->held = 0, h->fired = 0;
	else if (!h->fired && ++h->held >= threshold)
		h->fired = 1, h->press = press;
	if (!h->press)
		return 0;
	h->press--;
	return 1;
}

static void read_service_vars(void)
{
	struct retro_variable v = { "resys12_service_mode", NULL };
	if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
		s_test_hold = !strcmp(v.value, "start_hold");
	v.key = "resys12_service1", v.value = NULL;
	if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
		s_service_hold = !strcmp(v.value, "start_b_hold");
	memset(&s_hold_test, 0, sizeof(s_hold_test)); // (a hold started under the other setting must not fire)
	memset(&s_hold_service, 0, sizeof(s_hold_service));
}

static void poll_vars(void)
{
	static int vars_read;
	bool updated = false;
	if (!vars_read || (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)) {
		vars_read = 1;
		read_service_vars();
	}
}

static void poll_input(void)
{
	poll_vars();
	input_poll_cb();
	uint32_t sys = 0;
	for (int pl = 0; pl < 2; pl++) {
		uint32_t p = 0;
		for (unsigned i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
			if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, k_map[i].id))
				p |= k_map[i].bit;
		const int ax = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		const int ay = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
		if (ax < -12000) p |= S12_IN_LEFT;
		if (ax > 12000) p |= S12_IN_RIGHT;
		if (ay < -12000) p |= S12_IN_UP;
		if (ay > 12000) p |= S12_IN_DOWN;
		s12.in.player[pl] = p;
		if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
			sys |= pl ? S12_SYS_COIN2 : S12_SYS_COIN1;
	}
	const int start = in_st(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) != 0;
	const int b = in_st(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) != 0;
	int test = in_st(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) != 0;
	int service = (s12.in.player[0] & S12_IN_SERVICE) != 0;
	if (s_test_hold) // ponytail: the L3 / R3 descriptors still read Test / Service in this mode
		test = hold_update(&s_hold_test, start && !b, 12);
	if (s_service_hold)
		service = hold_update(&s_hold_service, start && b, 12);
	if (test)
		sys |= S12_SYS_TEST;
	s12.in.player[0] = (s12.in.player[0] & ~(uint32_t)S12_IN_SERVICE) | (service ? S12_IN_SERVICE : 0);
	s12.in.system = sys;
}

// ---- System 10 input: RetroPad -> JAMMA (MAME IN1). Y X B A L R = buttons 1-6, Select = coin, L3 = test switch (a
// slide switch: each press flips it), R3 = service ----
static const unsigned k_pad10[10] = { RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_L };
// IN1 bit of k_pad10[i] for player 1 (player 2: the same << 8 for 0-7, << 4 for buttons 4-6)
static const uint32_t k_bit10[10] = { S10_P1_RIGHT, S10_P1_LEFT, S10_P1_DOWN, S10_P1_UP, S10_P1_B1, S10_P1_B2, S10_P1_B3,
	S10_START1, S10_P1_B4, S10_P1_B5 };
static int s_test10;    // test switch position
static int s_trackball; // nflclsfb
static int s_taiko;     // the Taiko no Tatsujin sets (drums on the EXIO analog inputs)

static void poll_input_s10(void)
{
	poll_vars();
	input_poll_cb();
	uint32_t in = 0;
	if (s_trackball) { // nflclsfb, MAME's layout: players 1-4 Start, A / B = pass 1 / 2 (EXIO_IN1); player 1's Up / Down /
		// Y = the menu's select up / down / enter; L / R / X of players 1 and 3 = the left / right side's choose L / R, decide
		static const uint32_t start[4] = { 1u << 5, 1u << 6, 1u << 13, 1u << 14 };
		static const uint32_t side[2][3] = { { 1u << 1, 1u << 9, 1u << 7 }, { 1u << 0, 1u << 8, 1u << 15 } };
		static const unsigned side_pad[3] = { RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_X };
		uint32_t pass = 0;
		for (unsigned pl = 0; pl < 4; pl++) {
			if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
				in |= start[pl];
			if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
				pass |= 1u << (pl == 1 ? 2 : pl == 2 ? 1 : pl);
			if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
				pass |= 0x10u << (pl == 1 ? 2 : pl == 2 ? 1 : pl);
			if (!(pl & 1)) // (the left side is players 1-2, the right 3-4)
				for (unsigned i = 0; i < 3; i++)
					if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, side_pad[i]))
						in |= side[pl / 2][i];
			if (pl < 2 && input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
				in |= pl ? S10_COIN2 : S10_COIN1;
		}
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
			in |= 1u << 3;
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
			in |= 1u << 2;
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
			in |= 1u << 4;
		s10_exio_in1 = pass;
	}
	if (s_taiko) { // MAME's layout: player 1's Up / Down / Start = the menu's select up / down, enter; the drums below
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
			in |= 1u << 3;
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
			in |= 1u << 2;
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
			in |= 1u << 4;
		for (unsigned pl = 0; pl < 2; pl++)
			if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
				in |= pl ? S10_COIN2 : S10_COIN1;
	}
	for (unsigned pl = 0; pl < (s_trackball || s_taiko ? 0u : 2u); pl++) {
		for (unsigned i = 0; i < 10; i++)
			if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, k_pad10[i]))
				in |= i < 8 ? k_bit10[i] << (8 * pl) : k_bit10[i] << (4 * pl);
		if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
			in |= S10_P1_B6 << (4 * pl);
		const int ax = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		const int ay = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
		if (ax < -12000) in |= S10_P1_LEFT << (8 * pl);
		if (ax > 12000) in |= S10_P1_RIGHT << (8 * pl);
		if (ay < -12000) in |= S10_P1_UP << (8 * pl);
		if (ay > 12000) in |= S10_P1_DOWN << (8 * pl);
		if (input_state_cb(pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
			in |= pl ? S10_COIN2 : S10_COIN1;
	}
	const int start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) != 0;
	const int b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) != 0;
	static int l3_prev;
	const int l3 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) != 0;
	if (s_test_hold ? hold_update(&s_hold_test, start && !b, 1) : (l3 && !l3_prev))
		s_test10 ^= 1;
	l3_prev = l3;
	if (s_test10)
		in |= S10_TEST;
	if (s_service_hold ? hold_update(&s_hold_service, start && b, 12)
		: input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3) != 0)
		in |= S10_SERVICE;
	if (s10_has_gun()) { // the left stick aims while it is pushed, else the lightgun (or mouse); the trigger is button 1
		static float gx[2] = { 0.5f, 0.5f }, gy[2] = { 0.5f, 0.5f };
		for (unsigned pl = 0; pl < 2; pl++) {
			const int sx = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			const int sy = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
			if (sx < -2600 || sx > 2600 || sy < -2600 || sy > 2600) {
				gx[pl] = 0.5f + sx / 65536.0f;
				gy[pl] = 0.5f + sy / 65536.0f;
				in &= ~((S10_P1_RIGHT | S10_P1_LEFT | S10_P1_DOWN | S10_P1_UP) << (8 * pl));
			} else if (!input_state_cb(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN)) {
				const int lx = input_state_cb(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
				const int ly = input_state_cb(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
				if (lx || ly) { // (0,0 = no gun input)
					gx[pl] = (lx + 32768) / 65535.0f;
					gy[pl] = (ly + 32768) / 65535.0f;
				}
			}
			s10_set_gun((int)pl, gx[pl], gy[pl]);
			if (input_state_cb(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER)) // (g13jnr: button 2)
				in |= (s10_has_gun() == S10_GUN_P3 ? S10_P1_B2 : S10_P1_B1) << (8 * pl);
			if (input_state_cb(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START))
				in |= S10_START1 << (8 * pl);
			if (input_state_cb(pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SELECT))
				in |= pl ? S10_COIN2 : S10_COIN1;
		}
	}
	if (s_trackball) // nflclsfb: players 1-4's trackballs on the mouse, or the left stick (up to 16 counts a frame), as seen
		// on the rotated screen: X = the game's X, up = its Y
		for (unsigned pl = 0; pl < 4; pl++)
			for (unsigned axis = 0; axis < 2; axis++) {
				const int m = input_state_cb(pl, RETRO_DEVICE_MOUSE, 0, axis ? RETRO_DEVICE_ID_MOUSE_Y : RETRO_DEVICE_ID_MOUSE_X);
				const int a = input_state_cb(pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
					axis ? RETRO_DEVICE_ID_ANALOG_Y : RETRO_DEVICE_ID_ANALOG_X);
				s10_exio_trackball[pl * 2 + axis] += (uint8_t)(axis ? -(m + a / 2048) : m + a / 2048); // (the game's Y: up)
			}
	if (s_taiko) { // drum sensors (MAME EXIO_ANALOG1-8, per player: right rim, right surface, left surface, left rim), full
		// strength while the button is held: R, A, Y, L
		static const unsigned pad[4] = { RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
			RETRO_DEVICE_ID_JOYPAD_L };
		for (unsigned i = 0; i < 8; i++)
			s10_exio_analog[i] = input_state_cb(i / 4, RETRO_DEVICE_JOYPAD, 0, pad[i & 3]) ? 0x3ff : 0;
	} else if (s10_io_kind(s12.s10) == S10_IO_EXIO && !s10_has_gun()) { // EXIO analog: per player left stick, right stick (MAME
		// EXIO_ANALOG1-8: P1 left X/Y, P1 right X/Y, P2 ..., 10 bits; channels 2, 3, 6, 7 PORT_REVERSE)
		static const unsigned stk[2] = { RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_INDEX_ANALOG_RIGHT };
		for (unsigned i = 0; i < 8; i++) {
			const int v = input_state_cb(i / 4, RETRO_DEVICE_ANALOG, stk[(i / 2) & 1],
				(i & 1) ? RETRO_DEVICE_ID_ANALOG_Y : RETRO_DEVICE_ID_ANALOG_X);
			const unsigned a = (unsigned)(v + 32768) >> 6;
			s10_exio_analog[i] = (uint16_t)((i & 3) == 1 || (i & 3) == 2 ? 0x3ff - a : a);
		}
	}
	if (s10_has_gun() == S10_GUN_P3) { // g13jnr's scope sensor: a switch (as MAME's PORT_TOGGLE) on button 3 / aux A
		static int prev, on = 1;
		const int sc = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) ||
			input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A);
		if (sc && !prev)
			on ^= 1;
		prev = sc;
		in &= ~(uint32_t)S10_P1_B3;
		if (on)
			in |= S10_G13_SCOPE;
	}
	s10_in1 = in;
}

// ---- RetroArch input descriptors: the names its remap menu shows. One row per parent set (clones inherit), empty
// slots read "Button n". Button order = JAMMA buttons 1-8 = RetroPad Y X B A L R L2 R2 (k_map) ----
static const struct { const char *set, *b[8]; } k_names[] = {
	{ "tekken3", { "Left Punch", "Right Punch", "Left Kick", "Right Kick" } },
	{ "tektagt", { "Left Punch", "Right Punch", "Left Kick", "Right Kick", "Tag" } },
	{ "soulclbr", { "A (Horizontal Attack)", "B (Vertical Attack)", "K (Kick)", "G (Guard)" } },
	{ "ehrgeiz", { "High Attack", "Low Attack", "Guard", "Jump" } },
	{ "fgtlayer", { "Light Punch", "Heavy Punch", "Light Kick", "Heavy Kick" } },
	{ "toukon3", { "Punch", "Kick", "Grapple", "Run" } },
	{ "ptblank2", { "Trigger" } },
	{ "ghlpanic", { "Trigger" } },
	{ "golgo13", { NULL, "Trigger", NULL, NULL, NULL, NULL, "Scope" } },
	{ "g13knd", { NULL, "Trigger", NULL, NULL, NULL, NULL, "Scope" } },
	{ "technodr", { NULL, NULL, NULL, NULL, NULL, NULL, "Brake", "Gas" } },
	{ "truckk", { NULL, NULL, "Horn", NULL, NULL, NULL, "Brake", "Gas" } },
	{ "kartduel", { NULL, NULL, NULL, NULL, NULL, NULL, "Brake", "Gas" } },
	// System 11
	{ "tekken", { "Left Punch", "Right Punch", "Left Kick", "Right Kick" } },
	{ "tekken2", { "Left Punch", "Right Punch", "Left Kick", "Right Kick" } },
	{ "souledge", { "A (Horizontal Attack)", "B (Vertical Attack)", "K (Kick)", "G (Guard)" } },
	{ "pocketrc", { NULL, "View", NULL, NULL, NULL, NULL, NULL, "Gas" } },
};

static void set_descriptors(const S12Game *g)
{
	static const char *const k_button[8] = { "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8" };
	static const unsigned k_button_id[8] = { RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2 };
	static struct retro_input_descriptor desc[64];
	const char *const *b = NULL;
	const char *parent = g->parent[0] ? g->parent : g->name;
	for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++)
		if (!strcmp(k_names[i].set, parent))
			b = k_names[i].b;
	const int kind = s12_analog_kind(g);
	const char *ax = "Left Stick X", *ay = "Left Stick Y";
	if (kind == S12_ANALOG_GUN) ax = "Aim X", ay = "Aim Y";
	else if (kind == S12_ANALOG_WHEEL) ax = "Steering";
	else if (kind == S12_ANALOG_LEVER) ay = "Lever";
	unsigned n = 0;
	for (unsigned pl = 0; pl < 2; pl++) {
		static const struct { unsigned id; const char *name; } fixed[] = {
			{ RETRO_DEVICE_ID_JOYPAD_UP, "Up" }, { RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
			{ RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" }, { RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
			{ RETRO_DEVICE_ID_JOYPAD_START, "Start" }, { RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" },
			{ RETRO_DEVICE_ID_JOYPAD_L3, "Test" }, { RETRO_DEVICE_ID_JOYPAD_R3, "Service" },
		};
		for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, fixed[i].id, fixed[i].name };
		for (unsigned i = 0; i < 8; i++)
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, k_button_id[i], b && b[i] ? b[i] : k_button[i] };
		desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, ax };
		desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, ay };
		if (kind == S12_ANALOG_GUN) {
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Trigger" };
			if (g->state == S12_S_GOLGO13)
				desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A, "Scope" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START, "Start" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SELECT, "Coin" };
		}
	}
	desc[n] = (struct retro_input_descriptor){ 0, 0, 0, 0, NULL };
	env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static void set_descriptors_s10(void)
{
	static const char *const names[10] = { "Right", "Left", "Down", "Up", "Button 1", "Button 2", "Button 3", "Start",
		"Button 4", "Button 5" };
	static struct retro_input_descriptor desc[40];
	unsigned n = 0;
	if (s_taiko) {
		static const char *const drum[4] = { "Right rim (Ka)", "Right surface (Don)", "Left surface (Don)", "Left rim (Ka)" };
		static const unsigned pad[4] = { RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
			RETRO_DEVICE_ID_JOYPAD_L };
		for (unsigned pl = 0; pl < 2; pl++) {
			for (unsigned i = 0; i < 4; i++)
				desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, pad[i], drum[i] };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" };
		}
		desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Select up" };
		desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Select down" };
		desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Enter" };
	}
	if (s_trackball) {
		for (unsigned pl = 0; pl < 4; pl++) {
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Pass 1" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Pass 2" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
				RETRO_DEVICE_ID_ANALOG_X, "Trackball X (or the mouse)" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
				RETRO_DEVICE_ID_ANALOG_Y, "Trackball Y (or the mouse)" };
		}
		for (unsigned pl = 0; pl < 4; pl += 2) {
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, pl ? "Right side choose L" : "Left side choose L" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, pl ? "Right side choose R" : "Left side choose R" };
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, pl ? "Right side decide" : "Left side decide" };
			desc[n++] = (struct retro_input_descriptor){ pl / 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" };
		}
		desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Select up" };
		desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Select down" };
		desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Enter" };
	}
	for (unsigned pl = 0; pl < (s_trackball || s_taiko ? 0u : 2u); pl++) {
		for (unsigned i = 0; i < 10; i++)
			desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, k_pad10[i], names[i] };
		desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button 6" };
		desc[n++] = (struct retro_input_descriptor){ pl, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" };
	}
	desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Test (switch)" };
	desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Service" };
	desc[n] = (struct retro_input_descriptor){ 0, 0, 0, 0, NULL };
	env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static int s_link_id; // Kart Duel PCB ID to write into the EEPROM settings (0: leave them)

// ---- libretro ----
void retro_set_environment(retro_environment_t cb)
{
	env_cb = cb;
	bool no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
	static const struct retro_variable vars[] = {
		{ "resys12_link", "Cabinet link (Kart Duel, Libero Grande; udp port, content restart); disabled|22260|22270" },
		{ "resys12_link_id", "Cabinet PCB ID (Kart Duel, content restart); 1|2" },
		{ "resys12_service_mode", "Test button (operator menu): L3, or Start held 5 s; l3|start_hold" },
		{ "resys12_service1", "Service button: R3, or Start + B held 5 s; r3|start_b_hold" },
		{ NULL, NULL },
	};
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
	// ponytail: the lightgun is offered on every set (the game is not known yet); non-gun sets never read it
	static const struct retro_controller_description pads[] = {
		{ "RetroPad", RETRO_DEVICE_JOYPAD }, { "Lightgun", RETRO_DEVICE_LIGHTGUN },
	};
	static const struct retro_controller_info ports[] = { { pads, 2 }, { pads, 2 }, { NULL, 0 } };
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "ReSys12";
	info->library_version = "0.1";
	info->valid_extensions = "zip";
	info->need_fullpath = true;
	info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = 640;
	info->geometry.base_height = 480;
	info->geometry.max_width = FB_W;
	info->geometry.max_height = FB_H;
	info->geometry.aspect_ratio = s_vertical ? 3.0f / 4.0f : 4.0f / 3.0f;
	info->timing.fps = 53693175.0 / 263 / 3413; // PSX GPU NTSC, ~59.82 Hz (psxcounters.c FPS_FRACTIONAL_NTSC)
	info->timing.sample_rate = s12.s10 ? 44100 : S12_SOUND_RATE; // (the SPU, or the C352)
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

// System 10: s10_load loaded the set; the board, inputs and the SPU
static int load_s10(const char *dir, const char *name, const char *msg)
{
	s12_attach_s10(s10_game(s10_find_game(name)));
	s_trackball = !strcmp(s12.s10->name, "nflclsfb");
	s_taiko = !strncmp(s12.s10->name, "taiko", 5);
	s_test10 = 0;
	if (s_trackball) { // (a portrait monitor: the game draws its play screens a quarter turn; MAME says ROT0)
		unsigned rot = 1; // 90 degrees counter-clockwise
		env_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rot);
		s_vertical = 1;
	}
	s10_state_set_game(s12.s10->name);
	set_descriptors_s10();
	extern int rs12_vram_ymask, rs12_gpu_type1;
	rs12_vram_ymask = 1023; // CXD8561CQ with 2MB GPU RAM (MAME): 1024 VRAM lines
	rs12_gpu_type1 = 0;
	memset(&Config, 0, sizeof(Config));
	Config.FractionalFramerate = 1; // (frames at the GPU's ~59.82 Hz, the rate retro_get_system_av_info declares)
#if defined(__arm__) || defined(__aarch64__) || defined(LIGHTREC)
	Config.Cpu = getenv("RS12_INTERP") ? CPU_INTERPRETER : CPU_DYNAREC;
#else
	Config.Cpu = CPU_INTERPRETER;
#endif
	// MAME: CXD8606BQ at 101.4912 MHz, one instruction step per 4 clocks = 25.37M steps a second; pcsx_rearmed counts
	// 100 = one 33.8688 MHz cycle per instruction: 33.8688 / 25.3728 * 100 = 133
	Config.cycle_multiplier = 133;
	if (getenv("RS10_P3_CLOCK"))
		s10_p3_clock_pct = atoi(getenv("RS10_P3_CLOCK"));
	if (getenv("RS12_CYCLE_MULT"))
		Config.cycle_multiplier = atoi(getenv("RS12_CYCLE_MULT"));
	spu_config.iUseReverb = 1;
	spu_config.iUseInterpolation = 1;
	spu_config.iVolume = 512; // (0.5: the Taiko sets' drums over their music go 1.5x past full scale, and crackle)
	spu_config.iUseThread = 0;
	(void)dir; (void)msg;
	return 0;
}

bool retro_load_game(const struct retro_game_info *game)
{
	if (!game || !game->path)
		return false;
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		return false;

	// roms_dir/<set>.zip
	char dir[1024], name[64];
	const char *slash = strrchr(game->path, '/');
	const char *base = slash ? slash + 1 : game->path;
	snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - game->path) : 1, slash ? game->path : ".");
	snprintf(name, sizeof(name), "%s", base);
	char *dot = strrchr(name, '.');
	if (dot)
		*dot = 0;
	char msg[512] = "";
	s_vertical = 0;
	const int is_s10 = s12_find_game(name) < 0 && s10_find_game(name) >= 0;
	if (is_s10) {
		if (s10_load(dir, name, msg, sizeof(msg)) != 0) {
			fprintf(stderr, "ReSys12: load %s: %s\n", name, msg);
			return false;
		}
		load_s10(dir, name, msg);
	} else {
		if (s12_find_game(name) < 0) {
			fprintf(stderr, "ReSys12: unknown set %s\n", name);
			return false;
		}
		memset(&s_rom, 0, sizeof(s_rom));
		if (s12_rom_load(&s_rom, dir, name, msg, sizeof(msg)) != 0) {
			fprintf(stderr, "ReSys12: load %s: %s\n", name, msg);
			return false;
		}
		s12_attach(&s_rom);
		set_descriptors(s12.game);
		{ // MAME ROT90 / ROT270 (vertical screens); libretro counts quarter turns counter-clockwise
			const int rot = s12_game(s12_find_game(name))->rot;
			unsigned turns = rot == 90 ? 3 : rot == 180 ? 2 : rot == 270 ? 1 : 0;
			env_cb(RETRO_ENVIRONMENT_SET_ROTATION, &turns);
			s_vertical = turns & 1;
		}
		if (s12.cdxa && s12_cdxa_open_disc(dir, name) != 0)
			fprintf(stderr, "ReSys12: %s: disc image %s/%s/*.chd not found (MAME layout)\n", name, dir, name);
		if (getenv("RS12_RTC")) // comparisons: fixed RTC4543 time at reset (unix seconds, UTC)
			s12.rtc_base = strtoll(getenv("RS12_RTC"), NULL, 0);
		if (s12.link) { // network board link: core option, RS12_LINK_PORT for any other port
			extern int rs12_link_start(uint16_t port);
			struct retro_variable v = { "resys12_link", NULL };
			long port = 0;
			if (getenv("RS12_LINK_PORT"))
				port = strtol(getenv("RS12_LINK_PORT"), NULL, 10);
			else if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
				port = strtol(v.value, NULL, 10); // ("disabled" gives 0)
			if (port > 0) { // the lobby (sys12_comm.cfg next to the saves) pairs cabinets beyond the LAN
				extern void rs12_link_lobby_config(const char *save_dir, const char *game, const char *user);
				const char *save = NULL, *user = NULL;
				if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save) {
					env_cb(RETRO_ENVIRONMENT_GET_USERNAME, &user);
					rs12_link_lobby_config(save, name, user);
				}
			}
			if (port > 0 && port < 65536 - 8 && !rs12_link_start((uint16_t)port))
				fprintf(stderr, "ReSys12: link: no free udp port at %ld\n", port);
			struct retro_variable id = { "resys12_link_id", NULL };
			if (getenv("RS12_LINK_ID"))
				s_link_id = atoi(getenv("RS12_LINK_ID"));
			else if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &id) && id.value)
				s_link_id = atoi(id.value);
		}

		// gpulib VRAM lines. System 11 as MAME: 2MB GPU RAM (1024 lines) on both CPU boards; tekken2 (COH-110, CXD8561Q)
		// draws right only so. tekken / tekken2o sets (COH-100, CXD8538Q = MAME psxgpu type 1) have their own tpage and
		// draw area layout, gpulib rewrites it (rs12_gpu_type1); RS11_VRAM512 keeps the 512-line PS1 wrapping for comparisons
		extern int rs12_vram_ymask, rs12_gpu_type1;
		rs12_vram_ymask = s12.s11 && getenv("RS11_VRAM512") ? 511 : 1023;
		rs12_gpu_type1 = s12.s11 && (s12.game->flags & S12_G_GPU1);
		memset(&Config, 0, sizeof(Config));
#if defined(__arm__) || defined(__aarch64__) || defined(LIGHTREC)
		Config.Cpu = getenv("RS12_INTERP") ? CPU_INTERPRETER : CPU_DYNAREC;
#else
		Config.Cpu = CPU_INTERPRETER;
#endif
		// CD-XA sets keep the interpreter's rate: at 120 the library VSync timeout loop (32768 x 14 instructions) is shorter
		// than a frame, so full-frame waits time out and the boot handshake with the SH2 (768 VSyncs) expires too soon.
		// lightrec (x86_64) keeps it too: it times as the interpreter (the test pictures)
		Config.cycle_multiplier = Config.Cpu == CPU_DYNAREC && !s12.cdxa ? S12_CYCLE_MULTIPLIER_DYNAREC : S12_CYCLE_MULTIPLIER;
#ifdef LIGHTREC
		Config.cycle_multiplier = S12_CYCLE_MULTIPLIER;
#endif
		if (s12.s11) // System 11: the PS1's 33.8688 MHz CXD8530, pcsx_rearmed's PS1 rate
			Config.cycle_multiplier = CYCLE_MULT_DEFAULT;
		if (getenv("RS12_CYCLE_MULT")) // comparisons
			Config.cycle_multiplier = atoi(getenv("RS12_CYCLE_MULT"));
	}

	pl_rearmed_cbs.pl_vout_open = vout_open;
	pl_rearmed_cbs.pl_vout_close = vout_close;
	pl_rearmed_cbs.pl_vout_set_mode = vout_set_mode;
	pl_rearmed_cbs.pl_vout_flip = vout_flip;
	pl_rearmed_cbs.gpu_neon.allow_interlace = 2;
	pl_rearmed_cbs.thread_rendering = 0; // ponytail: rendering on the emulation thread; gpu_async if a Pi 5 needs it

	{ // comparisons (as the PC harness): RS12_H8BLK=0 runs the H8 instruction by instruction
		extern int h8_blk_enable;
		h8_blk_enable = getenv("RS12_H8BLK") ? atoi(getenv("RS12_H8BLK")) : 1;
	}
#if !defined(DRC_DISABLE) && !defined(LIGHTREC) && defined(RS12_HARNESS)
	{ // comparisons: ReSys12 recompiler switches, as the Vita's data files (ndrcalign, nokeepsame)
		extern int rs12_ndrc_align, rs12_ndrc_keep_same;
		if (getenv("RS12_NDRC_ALIGN")) rs12_ndrc_align = atoi(getenv("RS12_NDRC_ALIGN"));
		if (getenv("RS12_NOKEEPSAME")) rs12_ndrc_keep_same = 0;
	}
#endif
#if !defined(DRC_DISABLE) && !defined(LIGHTREC)
	{ // comparisons: recompiler option bits (NDHACK_*), as the Vita's ndhacks file
		extern struct ndrc_globals ndrc_g;
		if (getenv("RS12_NDHACKS"))
			ndrc_g.hacks = (int)strtol(getenv("RS12_NDHACKS"), NULL, 0);
	}
#endif
	rs12_psx_bind(is_s10);
	if (psxInit() != 0) {
		fprintf(stderr, "ReSys12: psxInit failed\n");
		return false;
	}
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs);
	GPU_init();
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs); // GPU_init clears the settings
	GPU_open(NULL, NULL, NULL);
	if (is_s10) {
		SPU_init();
		SPU_open();
		SPU_registerCallback(SPUirq);
		SPU_registerScheduleCb(SPUschedule);
	}
	psxReset();
	s_snd_prev = NULL;
	s_loaded = 1;
	fprintf(stderr, "ReSys12: %s loaded (%s), %s\n", name, msg, Config.Cpu != CPU_DYNAREC ? "interpreter" :
#ifdef LIGHTREC
		"lightrec dynarec");
#else
		"ari64 dynarec");
#endif
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
	(void)type; (void)info; (void)num;
	return false;
}

void retro_unload_game(void)
{
	if (!s_loaded)
		return;
	{
		extern void rs12_link_stop(void);
		rs12_link_stop();
	}
	if (getenv("RS12_UNMAPPED")) // diagnostics: distinct unhandled board addresses
		(s12.s10 ? s10_dump_unmapped : s12_dump_unmapped)();
	if (s12.s10) {
		SPU_close();
		SPU_shutdown();
	}
	GPU_close();
	psxShutdown();
	if (s12.s10)
		s10_unload();
	else
		s12_rom_free(&s_rom);
	s_loaded = 0;
}

void retro_reset(void)
{
	if (s_loaded)
		psxReset();
}

// ---- test hooks (headless runs): RS12_INPUT="frame:p1hex:syshex[:analog],..." as the Vita's input.txt (a P1 mask
// 0xfffff mashes buttons from then on; analog -100..100 holds the lever / the wheel; System 10: p1 = the MAME IN1 bits
// of s10.h, sys unused); RS12_DUMP_DIR + RS12_DUMP_EVERY=n write every n-th picture as a PPM ----
static void test_hooks(uint32_t frame)
{
	{ // tests: RS12_STATETEST=s,l saves a state at frame s and loads it back at frame l
		static unsigned st_s, st_l, st_init;
		static uint8_t *st_buf;
		static size_t st_n;
		if (!st_init) {
			st_init = 1;
			if (!getenv("RS12_STATETEST") || sscanf(getenv("RS12_STATETEST"), "%u,%u", &st_s, &st_l) != 2)
				st_s = st_l = 0;
		}
		if (st_l && frame == st_s)
			st_buf = s12.s10 ? s10_state_save_buf(&st_n) : s12_state_save(&st_n);
		if (st_l && frame == st_l && st_buf)
			fprintf(stderr, "ReSys12: state test: load %s\n",
				(s12.s10 ? s10_state_load_buf(st_buf, st_n) : s12_state_load(st_buf, st_n)) == 0 ? "ok" : "failed");
	}
	if (getenv("RS12_REALTIME")) { // tests: 60 frames a second of wall time (two linked cabinets in step)
		static struct timespec t0;
		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (!frame)
			t0 = t;
		const int64_t due = (int64_t)frame * 1000000000 / 60, now = (int64_t)(t.tv_sec - t0.tv_sec) * 1000000000 + (t.tv_nsec - t0.tv_nsec);
		if (due > now) {
			const struct timespec d = { (time_t)((due - now) / 1000000000), (long)((due - now) % 1000000000) };
			nanosleep(&d, NULL);
		}
	}
	static int init, in_n, in_i;
	static unsigned in_f[64], in_p1[64], in_sys[64];
	static int in_an[64], cur_an;
	static uint32_t cur_p1, cur_sys, mash_rng = 12345, mash_p1;
	static const char *dump_dir;
	static int dump_every;
	if (!init) {
		init = 1;
		const char *e = getenv("RS12_INPUT");
		while (e && in_n < 64 && sscanf(e, " %u:%x:%x:%d", &in_f[in_n], &in_p1[in_n], &in_sys[in_n], &in_an[in_n]) >= 3) {
			in_n++;
			e = strchr(e, ',');
			if (e)
				e++;
		}
		dump_dir = getenv("RS12_DUMP_DIR");
		dump_every = getenv("RS12_DUMP_EVERY") ? atoi(getenv("RS12_DUMP_EVERY")) : 0;
	}
	while (in_i < in_n && frame >= in_f[in_i]) {
		cur_p1 = in_p1[in_i];
		cur_sys = in_sys[in_i];
		cur_an = in_an[in_i];
		in_i++;
	}
	s_test_analog = cur_an / 100.0f;
	uint32_t p1 = cur_p1;
	if (cur_p1 == 0xfffff) {
		if (frame % 6 == 0) {
			mash_rng = mash_rng * 1103515245u + 12345u;
			mash_p1 = s12.s10 ? (mash_rng >> 8) & 0xff : (mash_rng >> 8) & 0x3ff & ~(uint32_t)0x3;
		}
		p1 = (frame % 6) < 4 ? mash_p1 : 0;
	}
	if (s12.s10)
		s10_in1 |= p1;
	else {
		s12.in.player[0] |= p1;
		s12.in.system |= cur_sys;
	}
#ifdef RS12_HARNESS
	{ // RS12_RAMDUMP=frame,frame,...: main RAM to <RS12_DUMP_DIR>/ram_<frame>.bin; RS12_WATCH=lo-hi[,from]: stores logged
		static const char *rd; static int rd_init;
		if (!rd_init) {
			rd_init = 1;
			rd = getenv("RS12_RAMDUMP");
			extern uint32_t rs12_watch_lo, rs12_watch_hi, rs12_watch_from;
			const char *w = getenv("RS12_WATCH");
			if (w && sscanf(w, "%x-%x,%u", &rs12_watch_lo, &rs12_watch_hi, &rs12_watch_from) < 2)
				rs12_watch_lo = 1, rs12_watch_hi = 0;
		}
		for (const char *e = rd; e && *e; ) {
			if ((uint32_t)strtoul(e, NULL, 10) == frame && dump_dir) {
				char path[1200];
				snprintf(path, sizeof(path), "%s/ram_%06u.bin", dump_dir, frame);
				FILE *f = fopen(path, "wb");
				if (f) { fwrite(psxRegs.ptrs.psxM, 1, PSX_RAM_SIZE, f); if (s12.cdxa) s12_cdxa_dump(f); fclose(f); }
			}
			e = strchr(e, ',');
			if (e) e++;
		}
	}
#endif
	if (dump_dir && dump_every && frame % dump_every == 0) {
		char path[1200];
		snprintf(path, sizeof(path), "%s/%06u.ppm", dump_dir, frame);
		FILE *f = fopen(path, "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", s_fb_w, s_fb_h);
			for (int i = 0; i < s_fb_w * s_fb_h; i++) {
				const uint16_t p = s_fb[i];
				const uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)((p >> 5 & 0x3f) << 2), (uint8_t)((p & 0x1f) << 3) };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
	}
}

void retro_run(void)
{
	static uint32_t frame;
	if (s12.s10) { // System 10: the SPU renders at vblank (SPU_async) into s_snd10, the MP3 board mixes over it
		poll_input_s10();
		test_hooks(frame++);
		s_snd10_n = 0;
		psxRegs.stop = 0;
		psxCpu->Execute(&psxRegs);
		s10_memp3_render(s_snd10, (int)s_snd10_n);
		if (s_snd10_n) {
			static FILE *wav;
			if (!wav && getenv("RS12_PCM")) // tests: the raw sound
				wav = fopen(getenv("RS12_PCM"), "wb");
			if (wav)
				fwrite(s_snd10, 4, s_snd10_n, wav);
			audio_batch_cb(s_snd10, s_snd10_n);
		}
		video_cb(s_fb, s_fb_w, s_fb_h, s_fb_w * 2);
		return;
	}
	extern int rs12_link_ring_id;
	extern int rs12_link_holding(void);
	if (!frame && rs12_link_holding()) { // (a black screen while the lobby pairs the cabinets)
		if (getenv("RS12_REALTIME")) // tests: (the frontend's vsync paces the others)
			nanosleep(&(struct timespec){ 0, 16666667 }, NULL);
		video_cb(s_fb, s_fb_w, s_fb_h, s_fb_w * 2);
		return;
	}
	// Kart Duel PCB ID, read by the game at boot: the lobby's cabinet number, else the core option (after the frontend
	// loaded the .srm); ponytail: a later change of number (the master left) applies at the next start
	static int ring_id;
	const int id = rs12_link_ring_id > 0 ? rs12_link_ring_id : s_link_id;
	if ((!frame || ring_id != rs12_link_ring_id) && id > 0 && s12.game->machine == S12_M_KARTDUEL && s12.eeprom[0x144] != id - 1) {
		s12.eeprom[0x15a] = (uint8_t)(s12.eeprom[0x15a] + s12.eeprom[0x144] - (id - 1)); // (settings check byte: NOR of a sum)
		s12.eeprom[0x144] = (uint8_t)(id - 1);
	}
	ring_id = rs12_link_ring_id;
	poll_input();
	test_hooks(frame++);
	analog_input(); // once a frame, after the scripted buttons (the d-pad wheel ramps per call)
	if (s12.s11)
		s11_update_inputs();
	psxRegs.stop = 0;
	psxCpu->Execute(&psxRegs);
	if (s12.s11) { // the C76 renders the C352 (same 88200 Hz rate as System 12's)
		const int n = s11_c76_sound_frame(s_snd_buf, S12_SOUND_MAX_FRAMES);
		if (n > 0)
			audio_batch_cb(s_snd_buf, (size_t)n);
		video_cb(s_fb, s_fb_w, s_fb_h, s_fb_w * 2);
		return;
	}
	S12SoundBatch *b = s12_sub_sound_frame(s_snd_prev);
	const int n = s12_sound_render(b, s_snd_buf);
	s_snd_prev = b;
	if (n > 0 && s12.cdxa) {
		s12_cdmix_pull();
		s12_cdmix_mix(s_snd_buf, n);
	}
	if (n > 0)
		audio_batch_cb(s_snd_buf, (size_t)n);
	video_cb(s_fb, s_fb_w, s_fb_h, s_fb_w * 2);
}

static uint8_t *state_save(size_t *n) { return s12.s10 ? s10_state_save_buf(n) : s12_state_save(n); }

size_t retro_serialize_size(void)
{
	size_t size = 0;
	uint8_t *buf = s_loaded ? state_save(&size) : NULL;
	free(buf);
	return size;
}

bool retro_serialize(void *data, size_t size)
{
	size_t n = 0;
	uint8_t *buf = s_loaded ? state_save(&n) : NULL;
	const bool ok = buf && n <= size;
	if (ok)
		memcpy(data, buf, n);
	free(buf);
	return ok;
}

bool retro_unserialize(const void *data, size_t size)
{
	if (!s_loaded)
		return false;
	return (s12.s10 ? s10_state_load_buf((const uint8_t *)data, size) : s12_state_load(data, size)) == 0;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// the save RAM: System 12 / 11's EEPROM, System 10's settings block (NAND 0 block 1, or the MEM(M) flash at 0x40000)
void *retro_get_memory_data(unsigned id)
{
	size_t n = 0;
	if (id != RETRO_MEMORY_SAVE_RAM || !s_loaded)
		return NULL;
	return s12.s10 ? s10_nvram(&n) : (void *)s12.eeprom;
}

size_t retro_get_memory_size(unsigned id)
{
	size_t n = 0;
	if (id != RETRO_MEMORY_SAVE_RAM || !s_loaded)
		return 0;
	if (s12.s10)
		s10_nvram(&n);
	else
		n = sizeof(s12.eeprom);
	return n;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
