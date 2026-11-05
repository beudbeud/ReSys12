// ReSys12: Namco System 11 C76 board (M37702 + C352 + shared RAM + inputs), MAME namcos11.cpp c76_map. See s11_c76.h.
#include "s11_c76.h"

#include <string.h>

#include "../m37710/m37710.h"
#include "../sound/c352.h"

#define C76_CYCLES_PER_SAMPLE 96 // 8467200 CPU cycles/s / 88200 Hz
#define SND_MAX 4096             // stereo pairs buffered between two s11_c76_sound_frame calls

static struct {
	uint8_t iram[0x200]; // internal RAM 0x80-0x27f (0x82-0x83: MAME's speedup word lives here too)
	uint8_t shared[S11_C76_SHARED_SIZE];
	uint16_t in[4];      // 0x1000 PLAYER4, 0x1002 SWITCH, 0x1004 PLAYER1, 0x1006 PLAYER2
	uint32_t last_main;  // main CPU cycle count at the last s11_c76_run_until
	uint32_t frac;       // main cycles not yet turned into C76 cycles (4 main cycles per C76 cycle)
	int synced;          // 0 until the first s11_c76_run_until after a reset (it only latches the main count)
	uint64_t snd_base;   // sample index of s_snd[0]
	int snd_n;           // samples rendered into s_snd
} c76;

static uint8_t s_bios[0x4000];
static const uint8_t *s_sprog;
static size_t s_sprog_size;
static int16_t s_snd[SND_MAX * 2];

// ---- C352, rendered in step with the C76 (MAME m_stream->update() at each register access) ----
static void snd_render_to(uint64_t cycles)
{
	const uint64_t t = cycles / C76_CYCLES_PER_SAMPLE;
	while (t > c76.snd_base + (uint64_t)c76.snd_n) {
		if (c76.snd_n == SND_MAX) { // nobody fetched the samples: drop them (the chip still steps every sample)
			c76.snd_base += SND_MAX;
			c76.snd_n = 0;
		}
		uint64_t n = t - c76.snd_base - (uint64_t)c76.snd_n;
		if (n > (uint64_t)(SND_MAX - c76.snd_n))
			n = (uint64_t)(SND_MAX - c76.snd_n);
		c352_render(s_snd + c76.snd_n * 2, (int)n);
		c76.snd_n += (int)n;
	}
}

static uint16_t c352_r(uint32_t a)
{
	snd_render_to(m37710_total_cycles());
	return c352_read((a - 0x2000) >> 1);
}

static void c352_w(uint32_t a, uint16_t d)
{
	snd_render_to(m37710_total_cycles());
	c352_write((a - 0x2000) >> 1, d);
}

// MAME c76_speedup_r: the BIOS idle loop at 0xc151 (LDA $83 / BEQ) waits for the byte at 0x83
static void speedup(void)
{
	if (m37710_pc() == 0xc153 && c76.iram[0x83 - 0x80] == 0)
		m37710_spin_until_interrupt();
}

// ---- M37710 bus (24-bit; 0x00-0x7f are the on-chip registers, handled by the core) ----
uint8_t m37710_read8(uint32_t a)
{
	if (a < 0x280) {
		if (a == 0x82 || a == 0x83)
			speedup();
		return a >= 0x80 ? c76.iram[a - 0x80] : 0;
	}
	if (a >= 0xc000 && a <= 0xffff)
		return s_bios[a - 0xc000];
	if (a >= 0x4000 && a < 0xc000)
		return c76.shared[a - 0x4000];
	if (a >= 0x2000 && a < 0x3000)
		return (uint8_t)(c352_r(a & ~1u) >> ((a & 1) * 8));
	if (a >= 0x1000 && a < 0x1008)
		return (uint8_t)(c76.in[(a - 0x1000) >> 1] >> ((a & 1) * 8));
	if ((a >= 0x080000 && a < 0x100000) || (a >= 0x200000 && a < 0x300000)) {
		const uint32_t o = a & 0x7ffff; // 512 KB region "c76", the sprog at 0, zero filled
		return o < s_sprog_size ? s_sprog[o] : 0;
	}
	return 0;
}

uint16_t m37710_read16(uint32_t a)
{
	if (a >= 0x2000 && a < 0x3000)
		return c352_r(a);
	return (uint16_t)(m37710_read8(a) | m37710_read8(a + 1) << 8);
}

void m37710_write8(uint32_t a, uint8_t d)
{
	if (a >= 0x80 && a < 0x280)
		c76.iram[a - 0x80] = d;
	else if (a >= 0x4000 && a < 0xc000)
		c76.shared[a - 0x4000] = d;
	else if (a >= 0x2000 && a < 0x3000) { // byte write: merged into the register (MAME COMBINE_DATA)
		const uint16_t old = c352_r(a & ~1u);
		c352_w(a & ~1u, (a & 1) ? (uint16_t)((old & 0x00ff) | d << 8) : (uint16_t)((old & 0xff00) | d));
	}
	// 0x300000 / 0x301000: nopw, ROM and the rest: ignored
}

void m37710_write16(uint32_t a, uint16_t d)
{
	if (a >= 0x2000 && a < 0x3000)
		c352_w(a, d);
	else {
		m37710_write8(a, (uint8_t)d);
		m37710_write8(a + 1, (uint8_t)(d >> 8));
	}
}

// ---- API ----
int s11_c76_init(const uint8_t *bios, size_t bios_size, const uint8_t *sprog, size_t sprog_size,
		 const uint8_t *wave, size_t wave_size)
{
	memset(&c76, 0, sizeof(c76));
	memset(s_bios, 0, sizeof(s_bios));
	if (!bios || !sprog)
		return -1;
	memcpy(s_bios, bios, bios_size < sizeof(s_bios) ? bios_size : sizeof(s_bios));
	s_sprog = sprog;
	s_sprog_size = sprog_size < 0x80000 ? sprog_size : 0x80000;
	c352_init(wave, (uint32_t)wave_size, 25401600, 288);
	m37710_init();
	return 0;
}

void s11_c76_reset(void)
{
	m37710_reset();
	c352_reset();
	c76.synced = 0;
	c76.frac = 0;
	c76.snd_base = m37710_total_cycles() / C76_CYCLES_PER_SAMPLE;
	c76.snd_n = 0;
}

uint8_t *s11_c76_shared(void)
{
	return c76.shared;
}

void s11_c76_set_inputs(const S11C76Inputs *in)
{
	c76.in[0] = in->player4;
	c76.in[1] = in->sw;
	c76.in[2] = in->player1;
	c76.in[3] = in->player2;
	memcpy(m37710.adc_in, in->adc, sizeof(m37710.adc_in));
}

void s11_c76_run_until(uint32_t main_cycles)
{
	if (!c76.synced) {
		c76.last_main = main_cycles;
		c76.synced = 1;
		return;
	}
	const uint32_t t = (main_cycles - c76.last_main) + c76.frac;
	c76.last_main = main_cycles;
	c76.frac = t & 3;
	if (t >> 2)
		m37710_run((int)(t >> 2));
}

void s11_c76_vblank(void)
{
	// ponytail: MAME raises IRQ0 and IRQ2 from two free-running 60 Hz timers, here both at the main board's vblank
	m37710_set_irq_line(M37710_LINE_IRQ0, 1);
	m37710_set_irq_line(M37710_LINE_IRQ2, 1);
}

int s11_c76_sound_frame(int16_t *out, int max_pairs)
{
	snd_render_to(m37710_total_cycles());
	int n = c76.snd_n < max_pairs ? c76.snd_n : max_pairs;
	memcpy(out, s_snd, (size_t)n * 4);
	if (n < c76.snd_n)
		memmove(s_snd, s_snd + n * 2, (size_t)(c76.snd_n - n) * 4);
	c76.snd_base += (uint64_t)n;
	c76.snd_n -= n;
	return n;
}

// ---- save state: board + core + C352 (buffered samples are not saved) ----
size_t s11_c76_state_size(void)
{
	return sizeof(c76) + sizeof(m37710) + c352_state_size();
}

void s11_c76_state_save(void *dst)
{
	uint8_t *p = (uint8_t *)dst;
	memcpy(p, &c76, sizeof(c76));
	memcpy(p + sizeof(c76), &m37710, sizeof(m37710));
	c352_state_save(p + sizeof(c76) + sizeof(m37710));
}

void s11_c76_state_load(const void *src)
{
	const uint8_t *p = (const uint8_t *)src;
	memcpy(&c76, p, sizeof(c76));
	memcpy(&m37710, p + sizeof(c76), sizeof(m37710));
	c352_state_load(p + sizeof(c76) + sizeof(m37710));
	c76.snd_base = m37710_total_cycles() / C76_CYCLES_PER_SAMPLE;
	c76.snd_n = 0;
}
