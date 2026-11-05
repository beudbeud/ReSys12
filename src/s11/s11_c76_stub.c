// ReSys12: placeholder C76 (s11_c76.h) until the M37710 one lands: shared RAM only, no CPU, silence.
// ponytail: delete this file (and its Makefile line) when src/s11/s11_c76.c is merged. GPL-2.0-or-later.
#include "s11_c76.h"

#include <string.h>

static uint8_t s_shared[S11_C76_SHARED_SIZE];

int s11_c76_init(const uint8_t *bios, size_t bios_size, const uint8_t *sprog, size_t sprog_size,
		 const uint8_t *wave, size_t wave_size)
{
	(void)bios; (void)bios_size; (void)sprog; (void)sprog_size; (void)wave; (void)wave_size;
	return 0;
}
void s11_c76_reset(void) { memset(s_shared, 0, sizeof(s_shared)); }
uint8_t *s11_c76_shared(void) { return s_shared; }
void s11_c76_set_inputs(const S11C76Inputs *in) { (void)in; }
void s11_c76_run_until(uint32_t main_cycles) { (void)main_cycles; }
void s11_c76_vblank(void) {}
int s11_c76_sound_frame(int16_t *out, int max_pairs)
{
	const int n = max_pairs < 1474 ? max_pairs : 1474; // 88200 Hz / 59.82 frames
	memset(out, 0, (size_t)n * 4);
	return n;
}
size_t s11_c76_state_size(void) { return sizeof(s_shared); }
void s11_c76_state_save(void *dst) { memcpy(dst, s_shared, sizeof(s_shared)); }
void s11_c76_state_load(const void *src) { memcpy(s_shared, src, sizeof(s_shared)); }
