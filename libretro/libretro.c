// ReSys12: libretro frontend. Content = a MAME System 12 zip (parent sets next to it). GPL-2.0-or-later.
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
#include "s12.h"
#include "s12_rom.h"
#include "s12_state.h"
#include "s12_sound.h"
#include "s12_cdxa.h"
#include "new_dynarec/new_dynarec.h"

extern void rs12_psx_bind(void);
extern void (*rs12_GPUrearmedCallbacks)(const struct rearmed_cbs *);

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

// gpu_async.c statistics the Vita frontend shows (VRAM read rectangles)
volatile int rs12_rw_hit[5], rs12_rw_max[11], rs12_rw_full[6];

static S12Rom s_rom;
static int s_vertical; // the screen is turned a quarter: the picture is 3:4
static void cd_fifo_clear(void);
static int s_loaded;
static S12SoundBatch *s_snd_prev;
static int16_t s_snd_buf[S12_SOUND_MAX_FRAMES * 2];

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
		float steer = stick(0, RETRO_DEVICE_ID_ANALOG_X) + dsteer;
		steer = steer < -1.0f ? -1.0f : steer > 1.0f ? 1.0f : steer;
		s12_set_wheel(steer, (p & S12_IN_B8) ? 1.0f : 0.0f, (p & S12_IN_B7) ? 1.0f : 0.0f);
	} else if (kind == S12_ANALOG_LEVER) {
		s12_set_lever(-stick(0, RETRO_DEVICE_ID_ANALOG_Y));
	}
}

static void poll_input(void)
{
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
	if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3))
		sys |= S12_SYS_TEST;
	s12.in.system = sys;
	analog_input();
}

// ---- libretro ----
void retro_set_environment(retro_environment_t cb)
{
	env_cb = cb;
	bool no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
	static const struct retro_variable vars[] = {
		{ "resys12_link", "Cabinet link (Kart Duel, udp port, content restart); disabled|22260|22270" },
		{ NULL, NULL },
	};
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
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
	info->timing.sample_rate = S12_SOUND_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

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
	if (s12_find_game(name) < 0) {
		fprintf(stderr, "ReSys12: unknown set %s\n", name);
		return false;
	}
	char msg[256] = "";
	memset(&s_rom, 0, sizeof(s_rom));
	if (s12_rom_load(&s_rom, dir, name, msg, sizeof(msg)) != 0) {
		fprintf(stderr, "ReSys12: load %s: %s\n", name, msg);
		return false;
	}
	s12_attach(&s_rom);
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
	if (s12.game->machine == S12_M_KARTDUEL) { // network board link: core option, RS12_LINK_PORT for any other port
		extern int rs12_link_start(uint16_t port);
		struct retro_variable v = { "resys12_link", NULL };
		long port = 0;
		if (getenv("RS12_LINK_PORT"))
			port = strtol(getenv("RS12_LINK_PORT"), NULL, 10);
		else if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
			port = strtol(v.value, NULL, 10); // ("disabled" gives 0)
		if (port > 0 && port < 65536 - 8 && !rs12_link_start((uint16_t)port))
			fprintf(stderr, "ReSys12: link: no free udp port at %ld\n", port);
	}

	memset(&Config, 0, sizeof(Config));
#if defined(__arm__) || defined(__aarch64__)
	Config.Cpu = getenv("RS12_INTERP") ? CPU_INTERPRETER : CPU_DYNAREC;
#else
	Config.Cpu = CPU_INTERPRETER;
#endif
	// CD-XA sets keep the interpreter's rate: at 120 the library VSync timeout loop (32768 x 14 instructions) is shorter
	// than a frame, so full-frame waits time out and the boot handshake with the SH2 (768 VSyncs) expires too soon
	Config.cycle_multiplier = Config.Cpu == CPU_DYNAREC && !s12.cdxa ? S12_CYCLE_MULTIPLIER_DYNAREC : S12_CYCLE_MULTIPLIER;
	if (getenv("RS12_CYCLE_MULT")) // comparisons
		Config.cycle_multiplier = atoi(getenv("RS12_CYCLE_MULT"));

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
#if !defined(DRC_DISABLE) && defined(RS12_HARNESS)
	{ // comparisons: ReSys12 recompiler switches, as the Vita's data files (ndrcalign, nokeepsame)
		extern int rs12_ndrc_align, rs12_ndrc_keep_same;
		if (getenv("RS12_NDRC_ALIGN")) rs12_ndrc_align = atoi(getenv("RS12_NDRC_ALIGN"));
		if (getenv("RS12_NOKEEPSAME")) rs12_ndrc_keep_same = 0;
	}
#endif
#ifndef DRC_DISABLE
	{ // comparisons: recompiler option bits (NDHACK_*), as the Vita's ndhacks file
		extern struct ndrc_globals ndrc_g;
		if (getenv("RS12_NDHACKS"))
			ndrc_g.hacks = (int)strtol(getenv("RS12_NDHACKS"), NULL, 0);
	}
#endif
	rs12_psx_bind();
	if (psxInit() != 0) {
		fprintf(stderr, "ReSys12: psxInit failed\n");
		return false;
	}
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs);
	GPU_init();
	rs12_GPUrearmedCallbacks(&pl_rearmed_cbs); // GPU_init clears the settings
	GPU_open(NULL, NULL, NULL);
	psxReset();
	s_snd_prev = NULL;
	cd_fifo_clear();
	s_loaded = 1;
	fprintf(stderr, "ReSys12: %s loaded (%s), %s\n", name, msg, Config.Cpu == CPU_DYNAREC ? "ari64 dynarec" : "interpreter");
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
#ifdef RS12_HARNESS
	if (getenv("RS12_UNMAPPED")) // diagnostics: distinct unhandled board addresses
		s12_dump_unmapped();
#endif
	GPU_close();
	psxShutdown();
	s12_rom_free(&s_rom);
	s_loaded = 0;
}

void retro_reset(void)
{
	cd_fifo_clear();
	if (s_loaded)
		psxReset();
}

// ---- test hooks (headless runs): RS12_INPUT="frame:p1hex:syshex,..." as the Vita's input.txt (a P1 mask 0xfffff
// mashes buttons from then on); RS12_DUMP_DIR + RS12_DUMP_EVERY=n write every n-th picture as a PPM ----
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
			st_buf = s12_state_save(&st_n);
		if (st_l && frame == st_l && st_buf)
			fprintf(stderr, "ReSys12: state test: load %s\n", s12_state_load(st_buf, st_n) == 0 ? "ok" : "failed");
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
	static uint32_t cur_p1, cur_sys, mash_rng = 12345, mash_p1;
	static const char *dump_dir;
	static int dump_every;
	if (!init) {
		init = 1;
		const char *e = getenv("RS12_INPUT");
		while (e && in_n < 64 && sscanf(e, " %u:%x:%x", &in_f[in_n], &in_p1[in_n], &in_sys[in_n]) == 3) {
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
		in_i++;
	}
	if (cur_p1 == 0xfffff) {
		if (frame % 6 == 0) {
			mash_rng = mash_rng * 1103515245u + 12345u;
			mash_p1 = (mash_rng >> 8) & 0x3ff & ~(uint32_t)0x3;
		}
		s12.in.player[0] |= (frame % 6) < 4 ? mash_p1 : 0;
	} else
		s12.in.player[0] |= cur_p1;
	s12.in.system |= cur_sys;
	if (in_n)
		analog_input(); // (pedals and wheel from the scripted buttons)
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

// CDXA board audio (37800 Hz) mixed into the C352 output at 0.30 as MAME's routing. A FIFO and a fixed-rate polyphase
// windowed-sinc resampler whose position carries over frames (the chip's DAC oversamples with a digital filter: linear
// interpolation left images at -35 dB, heard as hiss); the step follows the FIFO level so frame jitter cannot drift
#define CD_FIFO 8192
#define CD_TARGET 1024 // sample pairs kept ahead: about 1.6 frames at 37800 Hz
#define CD_TAPS 16
#define CD_HIST (CD_TAPS / 2 - 1) // past samples the filter still reads
#define CD_PHASES 256
static int16_t s_cd_fifo[CD_FIFO * 2];
static int s_cd_n, s_cd_primed;
static uint32_t s_cd_frac; // position after s_cd_fifo[CD_HIST], 16.16
static float s_cd_fir[CD_PHASES + 1][CD_TAPS];

static void cd_fifo_clear(void)
{
	memset(s_cd_fifo, 0, (size_t)CD_HIST * 4);
	s_cd_n = CD_HIST;
	s_cd_primed = 0;
	s_cd_frac = 0;
	if (s_cd_fir[0][CD_HIST + 1] != 0.0f)
		return;
	const double fc = 0.9 * 0.5; // cutoff at 90 % of the source Nyquist, in cycles per source sample
	for (int ph = 0; ph <= CD_PHASES; ph++) {
		double sum = 0;
		for (int t = 0; t < CD_TAPS; t++) {
			const double x = (double)(t - CD_HIST) - (double)ph / CD_PHASES; // distance to the output point
			const double sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
			const double wpos = (x + CD_TAPS / 2.0) / CD_TAPS; // Blackman window over the taps
			const double win = 0.42 - 0.5 * cos(2 * M_PI * wpos) + 0.08 * cos(4 * M_PI * wpos);
			s_cd_fir[ph][t] = (float)(sinc * win);
			sum += sinc * win;
		}
		for (int t = 0; t < CD_TAPS; t++) // unity gain at every phase
			s_cd_fir[ph][t] = (float)(s_cd_fir[ph][t] / sum);
	}
}

static void mix_cd_audio(int n)
{
	int16_t *cdbuf = s_cd_fifo + s_cd_n * 2;
	const int m = s12_cdxa_audio(cdbuf, CD_FIFO - s_cd_n);
	if (m > 0)
		s_cd_n += m;
#ifdef RS12_HARNESS
	static FILE *raw, *out;
	static int dbg_init;
	if (!dbg_init) { // diagnostics: RS12_CDRAW / RS12_CDOUT get the CD audio alone, at 37800 Hz / resampled
		dbg_init = 1;
		if (getenv("RS12_CDRAW"))
			raw = fopen(getenv("RS12_CDRAW"), "wb");
		if (getenv("RS12_CDOUT"))
			out = fopen(getenv("RS12_CDOUT"), "wb");
	}
	if (raw && m > 0)
		fwrite(cdbuf, 4, (size_t)m, raw);
	if (getenv("RS12_CDRATE")) { // diagnostics: CD sample pairs per frame over each 600 frames, FIFO level
		static int frames, sum;
		sum += m > 0 ? m : 0;
		if (++frames % 600 == 0) {
			fprintf(stderr, "cdrate f%d %.2f pairs/frame fifo %d\n", frames, sum / 600.0, s_cd_n - CD_HIST);
			sum = 0;
		}
	}
#endif
	const int avail = s_cd_n - CD_HIST;
	if (!s_cd_primed && avail < CD_TARGET)
		return;
	s_cd_primed = 1;
	// proportional and tiny (256 pairs off target = 0.024 %), so the pitch never steps audibly
	const int64_t base = ((int64_t)S12_CDXA_RATE << 16) / S12_SOUND_RATE;
	const uint32_t step = (uint32_t)(base + base * (avail - CD_TARGET) / 1048576);
	uint32_t pos = s_cd_frac;
	for (int i = 0; i < n; i++, pos += step) {
		const int j = CD_HIST + (int)(pos >> 16); // the output point lies between j and j + 1
		if (j + CD_TAPS / 2 >= s_cd_n) // underrun: stop mixing until the FIFO refills
			break;
		const float *fir = s_cd_fir[(pos >> 8) & 255];
		const int16_t *src = s_cd_fifo + (j - CD_HIST) * 2;
		float l = 0, r = 0;
		for (int t = 0; t < CD_TAPS; t++) {
			l += fir[t] * src[t * 2];
			r += fir[t] * src[t * 2 + 1];
		}
		const int cd[2] = { (int)lrintf(l), (int)lrintf(r) };
		for (int c = 0; c < 2; c++) {
#ifdef RS12_HARNESS
			if (out) { const int16_t o = (int16_t)(cd[c] > 32767 ? 32767 : cd[c] < -32768 ? -32768 : cd[c]); fwrite(&o, 2, 1, out); }
#endif
			const int v = s_snd_buf[i * 2 + c] + cd[c] * 3 / 10;
			s_snd_buf[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
	int used = (int)(pos >> 16);
	if (used > s_cd_n - CD_TAPS)
		used = s_cd_n - CD_TAPS > 0 ? s_cd_n - CD_TAPS : 0;
	s_cd_frac = used == (int)(pos >> 16) ? (pos & 0xffff) : 0;
	memmove(s_cd_fifo, s_cd_fifo + used * 2, (size_t)(s_cd_n - used) * 4);
	s_cd_n -= used;
}

void retro_run(void)
{
	static uint32_t frame;
	poll_input();
	test_hooks(frame++);
	psxRegs.stop = 0;
	psxCpu->Execute(&psxRegs);
	S12SoundBatch *b = s12_sub_sound_frame(s_snd_prev);
	const int n = s12_sound_render(b, s_snd_buf);
	s_snd_prev = b;
	if (n > 0 && s12.cdxa)
		mix_cd_audio(n);
	if (n > 0)
		audio_batch_cb(s_snd_buf, (size_t)n);
	video_cb(s_fb, s_fb_w, s_fb_h, s_fb_w * 2);
}

size_t retro_serialize_size(void)
{
	size_t size = 0;
	uint8_t *buf = s_loaded ? s12_state_save(&size) : NULL;
	free(buf);
	return size;
}

bool retro_serialize(void *data, size_t size)
{
	size_t n = 0;
	uint8_t *buf = s_loaded ? s12_state_save(&n) : NULL;
	const bool ok = buf && n <= size;
	if (ok)
		memcpy(data, buf, n);
	free(buf);
	return ok;
}

bool retro_unserialize(const void *data, size_t size)
{
	cd_fifo_clear();
	return s_loaded && s12_state_load(data, size) == 0;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
	return id == RETRO_MEMORY_SAVE_RAM && s_loaded ? s12.eeprom : NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	return id == RETRO_MEMORY_SAVE_RAM && s_loaded ? sizeof(s12.eeprom) : 0;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
