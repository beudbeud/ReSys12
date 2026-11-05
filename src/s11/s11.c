// ReSys12: Namco System 11 main board: shared RAM with the C76, KEYCUS protection, EEPROM, ROM bank windows.
// Behaviour follows MAME src/mame/namco/namcos11.cpp and ns11prot.cpp (BSD-3-Clause, copyright smf and
// contributors). GPL-2.0-or-later.
#include "s11.h"
#include "s11_c76.h"

#include <stdlib.h>
#include <string.h>

#include "psxcommon.h"
#include "psxmem.h"
#include "psxevents.h"
#include "r3000a.h"
#include "s12.h"

#define BANK_SIZE 0x100000u // eight 1MB windows at 0x1f000000-0x1f7fffff

enum { L_NAMCOS11, L_TEKKEN, L_SOULEDGE, L_MYANGEL3, L_POCKETRC }; // MAME input port sets

static struct {
	uint8_t bank[8];     // 1MB ROM entry of each window
	uint32_t bankoffset; // rom8_64_upper_w
	uint16_t p1, p2, p3; // KEYCUS registers
	uint32_t rng;        // KEYCUS reads MAME answers with machine().rand()
} s11;

static int s_keycus, s_map, s_layout; // 406 = C406 ..., S12_S_S11_*, L_*
static uint32_t s_entries;             // 1MB entries in the bank region
static uint8_t *s_shared;              // C76 shared RAM (little-endian)

void s11_attach(S12Rom *rom)
{
	const S12Game *g = rom->game;
	s_keycus = g->keycus[0] == 'C' ? atoi(g->keycus + 1) : 0;
	s_map = g->state;
	s_layout = !strcmp(g->inputs, "tekken") ? L_TEKKEN : !strcmp(g->inputs, "souledge") ? L_SOULEDGE :
		!strcmp(g->inputs, "myangel3") ? L_MYANGEL3 : !strcmp(g->inputs, "pocketrc") ? L_POCKETRC : L_NAMCOS11;
	s_entries = rom->rgn_size[S12_RGN_BANK] / BANK_SIZE;
	s11_c76_init(rom->rgn[S12_RGN_IOCPU], rom->rgn_size[S12_RGN_IOCPU], rom->rgn[S12_RGN_SUB], rom->rgn_size[S12_RGN_SUB],
		rom->rgn[S12_RGN_C352], rom->rgn_size[S12_RGN_C352]);
	s_shared = s11_c76_shared();
	memset(&s11, 0, sizeof(s11));
	s11.rng = 0x1234567;
}

// ---- ROM bank windows (MAME rom8_w / rom8_64_w), through the core read LUT ----
static void map_window(int i)
{
	uint8_t *base = s12.rom->rgn[S12_RGN_BANK];
	if (!base || !s_entries || s_map == S12_S_S11_NOBANK)
		return;
	const uint32_t o = (s11.bank[i] % s_entries) * BANK_SIZE; // ponytail: MAME asserts on an entry past the ROM
	if (!s12_rom_all_ready)
		s12_rom_wait(S12_RGN_BANK, o, BANK_SIZE);
	psxMemMapRead(0x1f000000u + i * BANK_SIZE, BANK_SIZE, base + o);
}

void s11_mem_reset(void)
{
	memset(s11.bank, 0, sizeof(s11.bank));
	s11.bankoffset = 0;
	for (int i = 0; i < 8; i++)
		map_window(i);
}

void s11_cpu_reset(void)
{
	s11.p1 = s11.p2 = s11.p3 = 0;
	s11_c76_reset(); // (psxReset: the main CPU cycle count is 0)
	set_event(PSXINT_S12, s12_sub_quantum);
}

// ---- KEYCUS (ns11prot.cpp): offset = 16-bit word in 0x1fa20000-0x1fa2001f ----
static uint16_t digits(uint32_t v, int lo, int hi) // decimal digit 10^lo in the low byte, 10^hi in the high one
{
	static const uint32_t p10[6] = { 1, 10, 100, 1000, 10000, 100000 };
	return (uint16_t)((v / p10[lo]) % 10 | (hi >= 0 ? ((v / p10[hi]) % 10) << 8 : 0));
}

static uint16_t keycus_r(int off)
{
	const uint16_t p1 = s11.p1, p2 = s11.p2, p3 = s11.p3;
	uint32_t v;
	switch (s_keycus) {
	case 406:
		if (off == 0 && p1 == 0x1234 && p2 == 0x5678 && p3 == 0x000f)
			return 0x3256;
		break;
	case 409:
		if (off == 7) {
			const int a2 = (p1 - 0x01) & 0x1f, a3 = (0x20 - p1) & 0x1f;
			const int r = (((p2 & 0x1f) * a2) + ((p3 & 0x1f) * a3)) / 0x1f;
			const int g = ((((p2 >> 5) & 0x1f) * a2) + (((p3 >> 5) & 0x1f) * a3)) / 0x1f;
			const int b = ((((p2 >> 10) & 0x1f) * a2) + (((p3 >> 10) & 0x1f) * a3)) / 0x1f;
			return (uint16_t)(r | (g << 5) | (b << 10));
		}
		break;
	case 410:
		if (p2 == 0) {
			v = p1 == 0xfffe ? 410 : p1;
			if (off == 1) return digits(v, 0, -1);
			if (off == 2) return digits(v, 2, 3);
			if (off == 3) return digits(v, 4, 1);
		}
		break;
	case 411:
		if (p2 == 0 && (((p1 == 0x0000 || p1 == 0x100) && p3 == 0xff7f) || p1 == 0x7256)) {
			v = p1 != 0x7256 ? 411 : p3;
			if (off == 0) return digits(v, 0, 1);
			if (off == 2) return digits(v, 2, 3);
			if (off == 8) return digits(v, 4, -1);
		}
		break;
	case 430:
		if (p2 == 0 && ((p1 == 0xbfff && p3 == 0) || p3 == 0xe296)) {
			v = p3 != 0xe296 ? 430 : p1;
			if (off == 1) return digits(v, 4, -1);
			if (off == 4) return digits(v, 2, 3);
			if (off == 5) return digits(v, 0, 1);
		}
		break;
	case 431:
		if (p2 == 0 && (((p1 == 0x0000 || p1 == 0xab50) && p3 == 0x7fff) || p1 == 0x9e61)) {
			v = p1 != 0x9e61 ? 431 : p3;
			if (off == 0) return digits(v, 0, 1);
			if (off == 4) return digits(v, 2, 3);
			if (off == 8) return digits(v, 4, -1);
		}
		break;
	case 432:
		if (p1 == 0 && (((p3 == 0x0000 || p3 == 0x00dc) && p2 == 0xefff) || p3 == 0x2f15)) {
			v = p3 != 0x2f15 ? 432 : p2;
			if (off == 2) return digits(v, 0, 1);
			if (off == 4) return digits(v, 2, 3);
			if (off == 6) return digits(v, 4, 5);
		}
		break;
	case 442:
		if (off == 0 && p1 == 0x0020 && (p2 == 0x0000 || p2 == 0x0021))
			return 0;
		if (off == 1 && p1 == 0x0020 && (p2 == 0x0020 || p2 == 0x3af2))
			return 0;
		if (off == 1 && p1 == 0x0020 && p2 == 0x0021)
			return 0xc442;
		break;
	case 443:
		if (off == 0 && p1 == 0x0020 && (p2 == 0x0000 || p2 == 0xffff || p2 == 0xffe0))
			return 0x0020;
		if (off == 1 && p1 == 0x0020 && p2 == 0xffdf)
			return 0;
		if (off == 1 && p1 == 0x0020 && (p2 == 0xffff || p2 == 0xffe0))
			return 0xc443;
		break;
	}
	s11.rng = s11.rng * 1103515245u + 12345u; // (fixed seed: headless runs repeat)
	return (uint16_t)(s11.rng >> 16);
}

static void keycus_w(int off, uint16_t d)
{
	// write offsets of p1, p2, p3 per device
	static const struct { int type; int8_t o1, o2, o3; } k[] = {
		{ 406, 1, 2, 3 }, { 409, 1, 3, 7 }, { 410, 0, 2, -1 }, { 411, 2, 8, 10 }, { 430, 0, 1, 4 },
		{ 431, 0, 4, 12 }, { 432, 0, 2, 6 }, { 442, 0, 1, -1 }, { 443, 0, 1, -1 },
	};
	for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++)
		if (k[i].type == s_keycus) {
			if (off == k[i].o1) s11.p1 = d;
			else if (off == k[i].o2) s11.p2 = d;
			else if (off == k[i].o3) s11.p3 = d;
		}
}

// ---- 16-bit bus (MAME namcos11_map, rom8_map, rom8_64_map) ----
int s11_r16(uint32_t a, uint16_t *v)
{
	if (a - 0x1fa04000u < 0xc000u) { // C76 shared RAM (its 0x4000-0xbfff; MAME maps 0x1fa04000-0x1fa0ffff)
		const uint32_t o = a - 0x1fa04000u;
		*v = o < S11_C76_SHARED_SIZE ? (uint16_t)(s_shared[o] | s_shared[o + 1] << 8) : 0;
		return 1;
	}
	if (a - 0x1fa20000u < 0x20u) {
		*v = keycus_r((int)(a - 0x1fa20000u) >> 1);
		return 1;
	}
	if (a - 0x1fa30000u < 0x1000u) { // AT28C16 on byte lanes 0 and 2 (umask32 0x00ff00ff)
		*v = s12.eeprom[((a - 0x1fa30000u) >> 1) & 0x7ff];
		return 1;
	}
	if (a < 0x1f800000u && s_entries && s_map != S12_S_S11_NOBANK) { // (the read LUT maps them: other paths)
		const uint8_t *p = s12.rom->rgn[S12_RGN_BANK] + (s11.bank[(a >> 20) & 7] % s_entries) * BANK_SIZE + (a & (BANK_SIZE - 2));
		*v = (uint16_t)(p[0] | p[1] << 8);
		return 1;
	}
	if (a >= 0x1fc00000u) {
		const uint8_t *r = psxRegs.ptrs.psxR + ((a - 0x1fc00000u) & PSX_ROM_MASK & ~1u);
		*v = (uint16_t)(r[0] | (r[1] << 8));
		return 1;
	}
	if (s_map == S12_S_S11_ROM8_64 && a - 0x1fa10020u < 0x10u) {
		*v = 0;
		return 1;
	}
	return 0;
}

int s11_w16(uint32_t a, uint16_t d)
{
	if (a - 0x1fa04000u < 0xc000u) {
		const uint32_t o = a - 0x1fa04000u;
		if (o < S11_C76_SHARED_SIZE) {
			s_shared[o] = (uint8_t)d;
			s_shared[o + 1] = (uint8_t)(d >> 8);
		}
		return 1;
	}
	if (a - 0x1fa20000u < 0x20u) {
		keycus_w((int)(a - 0x1fa20000u) >> 1, d);
		return 1;
	}
	if (a - 0x1fa30000u < 0x1000u) {
		const uint32_t o = ((a - 0x1fa30000u) >> 1) & 0x7ff;
		if (s12.eeprom[o] != (uint8_t)d) {
			s12.eeprom[o] = (uint8_t)d;
			s12.eeprom_dirty = 1;
		}
		return 1;
	}
	if (a - 0x1fa10020u < 0x10u && s_map != S12_S_S11_NOBANK) {
		const int i = (int)(a - 0x1fa10020u) >> 1;
		s11.bank[i] = (uint8_t)(s_map == S12_S_S11_ROM8_64 ? ((((d & 0xc0) >> 3) + (d & 0x07)) ^ s11.bankoffset)
			: (((d & 0xc0) >> 4) + (d & 0x03)));
		map_window(i);
		return 1;
	}
	if (s_map == S12_S_S11_ROM8_64 && a - 0x1f080000u < 4u) {
		s11.bankoffset = ((a - 0x1f080000u) >> 1) * 16;
		return 1;
	}
	// MAME nopw: 0x1fb00000, 0x1fbf6000, and the program flash
	return (a & ~3u) == 0x1fb00000u || (a & ~3u) == 0x1fbf6000u || a >= 0x1fc00000u;
}

int s11_w8(uint32_t a, uint8_t d)
{
	if (a - 0x1fa04000u < S11_C76_SHARED_SIZE) {
		s_shared[a - 0x1fa04000u] = d;
		return 1;
	}
	return s11_w16(a & ~1u, (uint16_t)((a & 1) ? d << 8 : d)); // (the byte in its lane)
}

// ---- C76 timing: caught up every S12 sub quantum and at VBLANK (MAME: IRQ0 / IRQ2 timers at 60 Hz) ----
void s11_event(void)
{
	s11_c76_run_until(psxRegs.cycle);
	set_event(PSXINT_S12, s12_sub_quantum);
}

void s11_vblank(int state)
{
	if (!state)
		return;
	s11_c76_run_until(psxRegs.cycle);
	s11_c76_vblank();
}

// ---- inputs: s12.in (frontend, active high) -> the C76 ports as MAME's namcos11 / tekken / souledge / myangel3 /
// pocketrc input ports read them (active low, all eight bits of each port declared, upper byte 0) ----
static uint16_t joy(uint32_t p, int b3)
{
	const uint32_t on = ((p & S12_IN_RIGHT) ? 0x01 : 0) | ((p & S12_IN_LEFT) ? 0x02 : 0) | ((p & S12_IN_DOWN) ? 0x04 : 0) |
		((p & S12_IN_UP) ? 0x08 : 0) | ((p & S12_IN_B1) ? 0x10 : 0) | ((p & S12_IN_B2) ? 0x20 : 0) |
		((b3 && (p & S12_IN_B3)) ? 0x40 : 0) | ((p & S12_IN_START) ? 0x80 : 0);
	return (uint16_t)(0xff & ~on);
}

void s11_update_inputs(void)
{
	const uint32_t p1 = s12.in.player[0], p2 = s12.in.player[1], sys = s12.in.system;
	S11C76Inputs in;
	memset(in.adc, 0xff, sizeof(in.adc));
	in.player4 = 0xff;
	in.sw = (uint16_t)(0xff & ~(((p1 & S12_IN_SERVICE) ? 0x80 : 0) | ((sys & S12_SYS_TEST) ? 0x40 : 0) |
		((sys & S12_SYS_COIN1) ? 0x20 : 0) | ((sys & S12_SYS_COIN2) ? 0x10 : 0)));
	in.player1 = joy(p1, s_layout != L_TEKKEN);
	in.player2 = joy(p2, s_layout != L_TEKKEN);
	switch (s_layout) {
	case L_TEKKEN: // P1 buttons 3 / 4 on ADC2 / ADC1, P2's on PLAYER4
		in.adc[2] = (p1 & S12_IN_B3) ? 0 : 0xff;
		in.adc[1] = (p1 & S12_IN_B4) ? 0 : 0xff;
		in.player4 = (uint16_t)(0xff & ~(((p2 & S12_IN_B3) ? 0x10 : 0) | ((p2 & S12_IN_B4) ? 0x20 : 0)));
		break;
	case L_SOULEDGE: // button 4: P1 on ADC2, P2 on PLAYER4
		in.adc[2] = (p1 & S12_IN_B4) ? 0 : 0xff;
		in.player4 = (p2 & S12_IN_B4) ? 0xef : 0xff;
		break;
	case L_MYANGEL3: { // four answer buttons, no stick
		const uint32_t p[2] = { p1, p2 };
		uint16_t *w[2] = { &in.player1, &in.player2 };
		for (int i = 0; i < 2; i++)
			*w[i] = (uint16_t)(0xff & ~(((p[i] & S12_IN_B4) ? 0x01 : 0) | ((p[i] & S12_IN_B3) ? 0x02 : 0) |
				((p[i] & S12_IN_B2) ? 0x04 : 0) | ((p[i] & S12_IN_B1) ? 0x08 : 0) | ((p[i] & S12_IN_START) ? 0x80 : 0)));
		break;
	}
	case L_POCKETRC: // wheel ADC0, gas ADC1 (s12_set_wheel), view button 2 on PLAYER4
		in.player1 = in.player2 = 0xff;
		in.player4 = (p1 & S12_IN_B2) ? 0xf7 : 0xff;
		in.adc[0] = (uint8_t)s12.in.adc[0];
		in.adc[1] = (uint8_t)s12.in.adc[1];
		break;
	}
	s11_c76_set_inputs(&in);
}

// ---- save state: board registers + the C76 (the shared RAM is the C76's) ----
uint32_t s11_state_size(void)
{
	return (uint32_t)(sizeof(s11) + s11_c76_state_size());
}

void s11_state_save(void *dst)
{
	memcpy(dst, &s11, sizeof(s11));
	s11_c76_state_save((uint8_t *)dst + sizeof(s11));
}

void s11_state_load(const void *src)
{
	memcpy(&s11, src, sizeof(s11));
	s11_c76_state_load((const uint8_t *)src + sizeof(s11));
	for (int i = 0; i < 8; i++)
		map_window(i);
}
