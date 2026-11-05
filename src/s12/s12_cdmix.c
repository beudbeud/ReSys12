// ReSys12: CDXA board audio mixer, shared by the frontends. GPL-2.0-or-later.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s12_cdmix.h"
#include "s12_cdxa.h"
#include "s12_sound.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

void s12_cdmix_clear(void)
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

#ifdef RS12_HARNESS
static FILE *raw, *out;
#endif

void s12_cdmix_pull(void)
{
	int16_t *cdbuf = s_cd_fifo + s_cd_n * 2;
	const int m = s12_cdxa_audio(cdbuf, CD_FIFO - s_cd_n);
	if (m > 0)
		s_cd_n += m;
#ifdef RS12_HARNESS
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
}

void s12_cdmix_mix(int16_t *buf, int n)
{
	const int out_rate = s12_sound_half_rate ? S12_SOUND_RATE / 2 : S12_SOUND_RATE;
	const int avail = s_cd_n - CD_HIST;
	if (!s_cd_primed && avail < CD_TARGET)
		return;
	s_cd_primed = 1;
	// proportional and tiny (256 pairs off target = 0.024 %), so the pitch never steps audibly
	const int64_t base = ((int64_t)S12_CDXA_RATE << 16) / out_rate;
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
			const int v = buf[i * 2 + c] + cd[c] * 3 / 10;
			buf[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
	int used = (int)(pos >> 16);
	if (used > s_cd_n - CD_TAPS)
		used = s_cd_n - CD_TAPS > 0 ? s_cd_n - CD_TAPS : 0;
	s_cd_frac = used == (int)(pos >> 16) ? (pos & 0xffff) : 0;
	memmove(s_cd_fifo, s_cd_fifo + used * 2, (size_t)(s_cd_n - used) * 4);
	s_cd_n -= used;
}
