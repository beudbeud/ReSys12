// ReSys12: libretro frontend. Content = a MAME System 12 zip (parent sets next to it). GPL-2.0-or-later.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "psxcommon.h"
#include "r3000a.h"
#include "plugins.h"
#include "../src/psx/frontend/plugin_lib.h"
#include "s12.h"
#include "s12_rom.h"
#include "s12_state.h"
#include "s12_sound.h"

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
}

// ---- libretro ----
void retro_set_environment(retro_environment_t cb)
{
	env_cb = cb;
	bool no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
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
	info->geometry.aspect_ratio = 4.0f / 3.0f;
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

	memset(&Config, 0, sizeof(Config));
#if defined(__arm__) || defined(__aarch64__)
	Config.Cpu = getenv("RS12_INTERP") ? CPU_INTERPRETER : CPU_DYNAREC;
#else
	Config.Cpu = CPU_INTERPRETER;
#endif
	Config.cycle_multiplier = Config.Cpu == CPU_DYNAREC ? S12_CYCLE_MULTIPLIER_DYNAREC : S12_CYCLE_MULTIPLIER;

	pl_rearmed_cbs.pl_vout_open = vout_open;
	pl_rearmed_cbs.pl_vout_close = vout_close;
	pl_rearmed_cbs.pl_vout_set_mode = vout_set_mode;
	pl_rearmed_cbs.pl_vout_flip = vout_flip;
	pl_rearmed_cbs.gpu_neon.allow_interlace = 2;
	pl_rearmed_cbs.thread_rendering = 0; // ponytail: rendering on the emulation thread; gpu_async if a Pi 5 needs it

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
	GPU_close();
	psxShutdown();
	s12_rom_free(&s_rom);
	s_loaded = 0;
}

void retro_reset(void)
{
	if (s_loaded)
		psxReset();
}

void retro_run(void)
{
	poll_input();
	psxRegs.stop = 0;
	psxCpu->Execute(&psxRegs);
	S12SoundBatch *b = s12_sub_sound_frame(s_snd_prev);
	const int n = s12_sound_render(b, s_snd_buf);
	s_snd_prev = b;
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
