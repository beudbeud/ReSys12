// ReSys12: Namco System 12 CDXA PCB. See s12_cdxa.h. GPL-2.0-or-later.
#include "s12_cdxa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psxcommon.h"
#include "psxhw.h"
#include "r3000a.h"
#include "sh2.h"
#include "s12_atapi.h"

#define SH2_CLOCK 14745600.0
#define PSX_CLOCK 33868800.0

static struct {
	uint8_t cram[0x40000];  // C448 SRAM, shared with the main CPU (big-endian SH2 view)
	uint8_t sram[0x10000];  // SRAM behind the C448
	uint8_t onchip[0x800];  // SH7014 on-chip registers 0xffff8000-0xffff87ff
	uint8_t cache[0x1000];  // SH7014 cache space used as RAM, 0xfffff000-0xffffffff
	int sram_enabled, ide_sh2, ide_ps1, int10_busy, sh2_running;
	uint32_t last_cycle;
	double sh2_frac;
	SH2 cpu;
	uint32_t pending[8];    // INTC: requested vectors (bit per vector)
	uint8_t sci_ssr;        // SCI0 status (TDRE, TEND...)
	int tx_busy;            // SCI0 shift register holds a byte
	uint8_t tx_byte;
	int64_t tx_done;        // SH2 cycle when the byte in the shift register is out
	int64_t tx_end_fp;      // exact end of that byte, 16.16 SH2 cycles (the next queued byte starts there)
	int tx_chain;           // TDR was refilled during the byte: start at tx_end_fp, not at the late check
	int dac_bits, dac_lr;   // LC78836M: bytes of the current word, channel (first word: left)
	uint16_t dac_word;
	int16_t dac_l;
	int mute, dma_writing;
	int64_t mtu_base[3];    // MTU: SH2 cycle of the counter value below
	uint32_t mtu_cnt[3];
	uint8_t mtu_tsr[3];
	int16_t audio[8192 * 2];
	int audio_n;
} cd;

static int s_log = -1;
static void logf_once(const char *what, uint32_t a, uint32_t v)
{
	static uint32_t seen[512];
	static int n;
	if (s_log < 0)
		s_log = getenv("RS12_CDXALOG") != NULL;
	if (!s_log)
		return;
	const uint32_t key = a ^ (uint32_t)what[0] << 24;
	for (int i = 0; i < n; i++)
		if (seen[i] == key)
			return;
	if (n < 512)
		seen[n++] = key;
	fprintf(stderr, "cdxa %s %08x = %08x (sh2 pc %08x, cycle %u)\n", what, a, v, cd.cpu.pc, psxRegs.cycle);
}

// ---- ATAPI CD-ROM ----
static uint16_t ata_r(int reg) { return s12_atapi_r(reg); }
static void dma_check(int ch);
static void ata_w(int reg, uint16_t v)
{
	s12_atapi_w(reg, v);
	dma_check(0); // a command may raise DREQ for an armed channel
	dma_check(1);
}

// ---- SH7014 on-chip peripherals (MAME sh7014_intc/dmac/sci.cpp), only what the CDXA firmware uses ----
enum { V_DMA0 = 72, V_DMA1 = 76, V_TXI0 = 130, V_TEI0 = 131 };
#define REG8(a) cd.onchip[(a) & 0x7ff]
#define REG16(a) ((uint16_t)(REG8(a) << 8 | REG8((a) + 1)))
#define REG32(a) ((uint32_t)REG16(a) << 16 | REG16((a) + 2))
#define SCR0 REG8(0xffff81a2u)
#define SCR_TIE 0x80
#define SCR_TE 0x20
#define SCR_TEIE 0x04
#define SSR_TDRE 0x80
#define SSR_TEND 0x04
#define DMA_BASE(ch) (0xffff86c0u + (uint32_t)(ch) * 0x10)

static void set_reg32(uint32_t a, uint32_t v)
{
	REG8(a) = (uint8_t)(v >> 24);
	REG8(a + 1) = (uint8_t)(v >> 16);
	REG8(a + 2) = (uint8_t)(v >> 8);
	REG8(a + 3) = (uint8_t)v;
}

static int vector_level(int v)
{
	switch (v) {
	case V_DMA0: return REG8(0xffff834cu) >> 4; // IPRC
	case V_DMA1: return REG8(0xffff834cu) & 15;
	case V_TXI0: case V_TEI0: return REG8(0xffff8353u) >> 4; // IPRF
	case 88: case 89: return REG8(0xffff834eu) >> 4; // IPRD
	case 96: case 97: return REG8(0xffff834fu) >> 4;
	case 104: case 105: return REG8(0xffff8350u) >> 4; // IPRE
	}
	return 0;
}

static void intc_update(void)
{
	int best = -1, level = 0;
	for (int v = 0; v < 256; v++)
		if ((cd.pending[v >> 5] >> (v & 31)) & 1) {
			const int l = vector_level(v);
			if (l > level) {
				level = l;
				best = v;
			}
		}
	cd.cpu.irq_level = best < 0 ? 0 : level;
	cd.cpu.irq_vector = best;
}

static void set_irq(int v, int on)
{
	if (on)
		cd.pending[v >> 5] |= 1u << (v & 31);
	else
		cd.pending[v >> 5] &= ~(1u << (v & 31));
	intc_update();
}

static void irq_ack(int v)
{
	cd.pending[v >> 5] &= ~(1u << (v & 31));
	intc_update();
}

static int dma_allowed(void) { return (REG16(0xffff86b0u) & 7) == 1; } // DME, no NMIF / AE

// channel ch running with SCI0 TXI as its request (CHCR RS = 12, DE set, TE clear)
static int dma_txi(int ch)
{
	const uint32_t chcr = REG32(DMA_BASE(ch) + 12);
	return dma_allowed() && (chcr & 3) == 1 && ((chcr >> 8) & 15) == 12;
}

static void sci_tdr_loaded(void);

// one DMA unit for channel ch; the last one sets TE and requests the end interrupt
static void dma_unit(int ch)
{
	const uint32_t b = DMA_BASE(ch);
	uint32_t chcr = REG32(b + 12), sar = REG32(b), dar = REG32(b + 4), tcr = REG32(b + 8);
	const uint32_t size = 1u << ((chcr >> 3) & 3);
	const int sm = (chcr >> 12) & 3, dm = (chcr >> 14) & 3;
	if (sm == 2) sar -= size;
	if (dm == 2) dar -= size;
	cd.dma_writing = 1;
	switch (size) {
	case 1: cd.cpu.bus.wr8(dar, cd.cpu.bus.rd8(sar)); break;
	case 2: cd.cpu.bus.wr16(dar, cd.cpu.bus.rd16(sar)); break;
	default: cd.cpu.bus.wr32(dar, cd.cpu.bus.rd32(sar)); break;
	}
	cd.dma_writing = 0;
	if (sm == 1) sar += size;
	if (dm == 1) dar += size;
	tcr = tcr ? tcr - 1 : 0xffff; // a count of 0 means 65536
	set_reg32(b, sar);
	set_reg32(b + 4, dar);
	set_reg32(b + 8, tcr);
	if (tcr == 0) {
		chcr |= 2; // TE
		set_reg32(b + 12, chcr);
		if (chcr & 4)
			set_irq(ch ? V_DMA1 : V_DMA0, 1);
	}
}

// a channel may start: auto requests complete at once, external ones as far as the drive has data (ponytail: no
// cycle stealing, add if a game races it)
static void sci_update(void);
static void dma_check(int ch)
{
	const uint32_t chcr = REG32(DMA_BASE(ch) + 12);
	if (!dma_allowed() || (chcr & 3) != 1)
		return;
	const uint32_t rs = (chcr >> 8) & 15;
	if (rs == 4) { // auto request
		while (!(REG32(DMA_BASE(ch) + 12) & 2) && dma_allowed())
			dma_unit(ch);
	} else if (rs == 0 || rs == 2 || rs == 3) { // external request: the CD drive's DREQ, while it has data
		while (!(REG32(DMA_BASE(ch) + 12) & 2) && dma_allowed() && s12_atapi_drq())
			dma_unit(ch);
	} else if (((chcr >> 8) & 15) == 12)
		sci_update();
}

// SCI0 TXI: a DMA channel waiting for it takes it, otherwise it is an interrupt request
static void sci_update(void)
{
	if ((SCR0 & SCR_TE) && (cd.sci_ssr & SSR_TDRE) && (SCR0 & SCR_TIE)) {
		for (int ch = 0; ch < 2; ch++)
			if (dma_txi(ch)) {
				set_irq(V_TXI0, 0);
				dma_unit(ch); // writes TDR, which starts the byte
				return;
			}
	}
	set_irq(V_TXI0, (SCR0 & SCR_TIE) && (cd.sci_ssr & SSR_TDRE));
	set_irq(V_TEI0, (SCR0 & SCR_TEIE) && (cd.sci_ssr & SSR_TEND));
}

// TDR holds a byte (TDRE fell): the shift register takes it when it is free
static void sci_tdr_loaded(void)
{
	if (!(SCR0 & SCR_TE))
		return;
	if (!cd.tx_busy && !(cd.sci_ssr & SSR_TDRE)) {
		// 8 of the 32 bits of a stereo sample at 37800 Hz: 97.52 SH2 cycles, kept exact so the rate does not drift
		const int64_t period = (int64_t)(SH2_CLOCK * 65536.0 / (37800.0 * 32.0 / 8.0) + 0.5);
		const int64_t now = cd.cpu.cycles << 16;
		const int64_t start = cd.tx_chain && cd.tx_end_fp > 0 && cd.tx_end_fp < now ? cd.tx_end_fp : now;
		cd.tx_busy = 1;
		cd.tx_byte = REG8(0xffff81a3u);
		cd.tx_end_fp = start + period;
		cd.tx_done = (cd.tx_end_fp + 0xffff) >> 16;
		cd.sci_ssr = (uint8_t)((cd.sci_ssr & ~SSR_TEND) | SSR_TDRE);
	}
	sci_update();
}

// LC78836M: 16 bits per channel, LSB first (the SCI sends LSB first), left word first
static void dac_byte(uint8_t b)
{
	cd.dac_word |= (uint16_t)(b << (cd.dac_bits * 8));
	if (++cd.dac_bits < 2)
		return;
	const int16_t v = cd.mute ? 0 : (int16_t)cd.dac_word;
	cd.dac_bits = 0;
	cd.dac_word = 0;
	if (!cd.dac_lr) {
		cd.dac_l = v;
	} else if (cd.audio_n < 8192) {
		cd.audio[cd.audio_n * 2] = cd.dac_l;
		cd.audio[cd.audio_n * 2 + 1] = v;
		cd.audio_n++;
	}
	cd.dac_lr ^= 1;
}

static void sci_tx_event(void)
{
	cd.tx_busy = 0;
	dac_byte(cd.tx_byte);
	if (!(cd.sci_ssr & SSR_TDRE)) {
		cd.tx_chain = 1;
		sci_tdr_loaded();
		cd.tx_chain = 0;
	} else {
		cd.sci_ssr |= SSR_TEND;
		sci_update();
	}
}

// MTU (MAME sh7014_mtu.cpp): internal clocks, TGRA/TGRB compare match with clearing, TGIxA/B interrupts.
// ponytail: no overflow/underflow interrupts, external clocks or output pins (the CDXA firmware uses none)
#define MTU_BASE(ch) (0xffff8260u + (uint32_t)(ch) * 0x20)
static const int mtu_vec[3][2] = { { 88, 89 }, { 96, 97 }, { 104, 105 } };

static int mtu_running(int ch) { return (REG8(0xffff8240u) >> ch) & 1; }

// log2 of the prescaler, -1 for an external clock (MAME tcr_w dividers)
static int mtu_div(int ch)
{
	static const int div[3][8] = { { 0, 2, 4, 6, -1, -1, -1, -1 }, { 0, 2, 4, 6, -1, -1, 8, -1 }, { 0, 2, 4, 6, -1, -1, -1, 10 } };
	return div[ch][REG8(MTU_BASE(ch)) & 7];
}

// the TGR that clears the counter (0 = none)
static uint32_t mtu_cycle(int ch)
{
	const int cclr = (REG8(MTU_BASE(ch)) >> 5) & (ch ? 3 : 7);
	if (cclr == 1 || cclr == 2) {
		const uint32_t tgr = REG16(MTU_BASE(ch) + 8 + (uint32_t)(cclr - 1) * 2);
		return tgr ? tgr : 0x10000;
	}
	return 0x10000;
}

// counter value now, without wrapping
static int64_t mtu_raw(int ch, int64_t now)
{
	const int d = mtu_div(ch);
	if (!mtu_running(ch) || d < 0)
		return cd.mtu_cnt[ch];
	return cd.mtu_cnt[ch] + ((now - cd.mtu_base[ch]) >> d);
}

// folds the elapsed ticks into the counter (keeps the sub-tick remainder)
static void mtu_sync(int ch, int64_t now)
{
	const int d = mtu_div(ch);
	if (mtu_running(ch) && d >= 0) {
		const int64_t ticks = (now - cd.mtu_base[ch]) >> d;
		cd.mtu_cnt[ch] = (uint32_t)((cd.mtu_cnt[ch] + ticks) % mtu_cycle(ch));
		cd.mtu_base[ch] += ticks << d;
	} else {
		cd.mtu_base[ch] = now;
	}
	REG8(MTU_BASE(ch) + 6) = (uint8_t)(cd.mtu_cnt[ch] >> 8);
	REG8(MTU_BASE(ch) + 7) = (uint8_t)cd.mtu_cnt[ch];
}

// compare matches up to now set TSR flags and request the interrupts
static void mtu_update(int64_t now)
{
	for (int ch = 0; ch < 3; ch++) {
		if (!mtu_running(ch) || mtu_div(ch) < 0)
			continue;
		const int64_t tt = mtu_raw(ch, now);
		const uint8_t tier = REG8(MTU_BASE(ch) + 4);
		for (int i = 0; i < 2; i++)
			if ((tier >> i) & 1 && !((cd.mtu_tsr[ch] >> i) & 1) && tt >= REG16(MTU_BASE(ch) + 8 + (uint32_t)i * 2)) {
				cd.mtu_tsr[ch] |= (uint8_t)(1 << i);
				set_irq(mtu_vec[ch][i], 1);
			}
		mtu_sync(ch, now);
	}
}

// SH2 cycle of the next compare match interrupt, or INT64_MAX
static int64_t mtu_next(void)
{
	int64_t next = INT64_MAX;
	for (int ch = 0; ch < 3; ch++) {
		const int d = mtu_div(ch);
		if (!mtu_running(ch) || d < 0)
			continue;
		const uint8_t tier = REG8(MTU_BASE(ch) + 4);
		for (int i = 0; i < 2; i++) {
			const uint32_t tgr = REG16(MTU_BASE(ch) + 8 + (uint32_t)i * 2);
			if (!((tier >> i) & 1) || ((cd.mtu_tsr[ch] >> i) & 1) || tgr <= cd.mtu_cnt[ch])
				continue;
			const int64_t t = cd.mtu_base[ch] + ((int64_t)(tgr - cd.mtu_cnt[ch]) << d);
			if (t < next)
				next = t;
		}
	}
	return next;
}

static uint8_t onchip_r8(uint32_t a)
{
	if (a == 0xffff81a4u)
		return cd.sci_ssr;
	if (a >= 0xffff8260u && a < 0xffff82c0u) {
		const int ch = (int)((a - 0xffff8260u) >> 5);
		const uint32_t r = (a - 0xffff8260u) & 0x1f;
		if (r == 5)
			return (uint8_t)(cd.mtu_tsr[ch] | 0x40 | (ch ? 0x80 : 0));
		if (r == 6 || r == 7)
			mtu_update(cd.cpu.cycles);
	}
	const uint8_t v = cd.onchip[a & 0x7ff];
	logf_once("chip r", a, v);
	return v;
}

static void onchip_w8(uint32_t a, uint8_t v)
{
	logf_once("chip w", a, v);
	if (a >= 0xffff8240u && a < 0xffff82c0u)
		mtu_update(cd.cpu.cycles); // counters run with the old settings up to now
	const uint8_t old = cd.onchip[a & 0x7ff];
	cd.onchip[a & 0x7ff] = v;
	if (a >= 0xffff8260u && a < 0xffff82c0u) {
		const int ch = (int)((a - 0xffff8260u) >> 5);
		const uint32_t r = (a - 0xffff8260u) & 0x1f;
		if (r == 5) // TSR: the flags only fall where a 0 is written
			cd.mtu_tsr[ch] &= v;
		else if (r == 6 || r == 7)
			cd.mtu_cnt[ch] = REG16(MTU_BASE(ch) + 6);
	}
	switch (a) {
	case 0xffff81a2u: // SCR0
		if ((old & SCR_TE) && !(v & SCR_TE))
			cd.sci_ssr |= SSR_TEND | SSR_TDRE;
		sci_update();
		break;
	case 0xffff81a3u: // TDR0: a DMA write starts the byte (MAME tdr_w); a CPU write waits for its SSR write
		if (cd.dma_writing && (SCR0 & SCR_TE)) {
			cd.sci_ssr &= (uint8_t)~(SSR_TEND | SSR_TDRE);
			sci_tdr_loaded();
		}
		break;
	case 0xffff81a4u: { // SSR0: TDRE RDRF ORER FER PER only fall where a 0 is written
		const uint8_t w = 0xf8, prev = cd.sci_ssr;
		cd.sci_ssr = (uint8_t)((prev & ~(w | 1)) | (prev & v & w) | (v & 1));
		if (!(SCR0 & SCR_TE))
			cd.sci_ssr |= SSR_TEND | SSR_TDRE;
		if ((prev & SSR_TDRE) && !(cd.sci_ssr & SSR_TDRE))
			sci_tdr_loaded();
		else
			sci_update();
		break;
	}
	case 0xffff86b1u: // DMAOR
		dma_check(0);
		dma_check(1);
		break;
	case 0xffff86cfu: case 0xffff86dfu: { // CHCR low byte (DE, TE, IE)
		const int ch = a == 0xffff86dfu;
		if ((v & 2) && !(old & 2))
			cd.onchip[a & 0x7ff] = (uint8_t)(v & ~2); // TE only falls by software
		if (!(cd.onchip[a & 0x7ff] & 2))
			set_irq(ch ? V_DMA1 : V_DMA0, 0);
		dma_check(ch);
		break;
	}
	case 0xffff834cu: case 0xffff834du: case 0xffff834eu: case 0xffff834fu: case 0xffff8350u: case 0xffff8352u: case 0xffff8353u:
		intc_update();
		break;
	case 0xffff8391u: // PBDR: bit 6 mutes the DAC (MAME portb_w)
		cd.mute = (v >> 6) & 1;
		break;
	}
}

// ---- SH2 bus (big-endian) ----
static uint8_t *ext_ptr(uint32_t a)
{
	a &= 0x0fffffffu;
	if (a < 0x40000u)
		return &cd.cram[a];
	if (a >= 0x400000u && a < 0x410000u)
		return &cd.sram[a - 0x400000u];
	return NULL;
}

static uint16_t ext_r16(uint32_t a)
{
	a &= 0x0fffffffu;
	if ((a & ~3u) == 0x41c000u)
		return cd.int10_busy ? 0 : 0x800; // C448 status: 0x800 = the main CPU took the last IRQ10
	if (a >= 0xc00000u && a < 0xc20000u)
		return cd.ide_sh2 ? ata_r((int)((a >> 14) & 7)) : 0;
	if (a >= 0xc20000u && a < 0xc40000u)
		return 0;
	logf_once("ext r", a, 0);
	return 0;
}

static void ext_w16(uint32_t a, uint16_t v)
{
	a &= 0x0fffffffu;
	if ((a & ~3u) == 0x41601cu) { // C448: IRQ10 to the main CPU
		cd.int10_busy = 1;
		psxHu32ref(0x1070) |= SWAPu32(0x400);
		return;
	}
	if (a >= 0xc00000u && a < 0xc20000u) {
		if (cd.ide_sh2)
			ata_w((int)((a >> 14) & 7), v);
		return;
	}
	if (a >= 0xc20000u && a < 0xc40000u)
		return; // ATA control block: MAME leaves it unconnected (see namcos12_cdxa.cpp)
	logf_once("ext w", a, v);
}

static uint8_t rd8(uint32_t a)
{
	if (a >= 0xfffff000u)
		return cd.cache[a & 0xfff];
	if (a >= 0xffff8000u)
		return onchip_r8(a);
	const uint8_t *p = ext_ptr(a);
	if (p)
		return *p;
	const uint16_t w = ext_r16(a & ~1u);
	return (uint8_t)((a & 1) ? w : w >> 8);
}

static uint16_t rd16(uint32_t a)
{
	a &= ~1u;
	if (a >= 0xffff8000u)
		return (uint16_t)(rd8(a) << 8 | rd8(a + 1));
	const uint8_t *p = ext_ptr(a);
	return p ? (uint16_t)(p[0] << 8 | p[1]) : ext_r16(a);
}

static uint32_t rd32(uint32_t a)
{
	a &= ~3u;
	return (uint32_t)rd16(a) << 16 | rd16(a + 2);
}

static void wr8(uint32_t a, uint8_t v)
{
	if (a >= 0xfffff000u) {
		cd.cache[a & 0xfff] = v;
		return;
	}
	if (a >= 0xffff8000u) {
		onchip_w8(a, v);
		return;
	}
	uint8_t *p = ext_ptr(a);
	if (p) {
		*p = v;
		return;
	}
	ext_w16(a & ~1u, (a & 1) ? v : (uint16_t)(v << 8));
}

static void wr16(uint32_t a, uint16_t v)
{
	a &= ~1u;
	if (a >= 0xffff8000u) {
		wr8(a, (uint8_t)(v >> 8));
		wr8(a + 1, (uint8_t)v);
		return;
	}
	uint8_t *p = ext_ptr(a);
	if (p) {
		p[0] = (uint8_t)(v >> 8);
		p[1] = (uint8_t)v;
		return;
	}
	ext_w16(a, v);
}

static void wr32(uint32_t a, uint32_t v)
{
	a &= ~3u;
	wr16(a, (uint16_t)(v >> 16));
	wr16(a + 2, (uint16_t)v);
}

void s12_cdxa_reset(void)
{
	memset(&cd, 0, sizeof(cd));
	cd.cpu.bus = (SH2Bus){ rd8, rd16, rd32, wr8, wr16, wr32 };
	cd.cpu.irq_ack = irq_ack;
	cd.sci_ssr = SSR_TDRE | SSR_TEND;
	s12_atapi_reset();
	cd.last_cycle = psxRegs.cycle;
}

uint32_t s12_cdxa_state_size(void)
{
	return (uint32_t)sizeof(cd) + s12_atapi_state_size();
}

void s12_cdxa_state_save(void *dst)
{
	memcpy(dst, &cd, sizeof(cd));
	s12_atapi_state_save((uint8_t *)dst + sizeof(cd));
}

void s12_cdxa_state_load(const void *src)
{
	const SH2Bus bus = cd.cpu.bus;
	void (*ack)(int) = cd.cpu.irq_ack;
	memcpy(&cd, src, sizeof(cd));
	cd.cpu.bus = bus;
	cd.cpu.irq_ack = ack;
	s12_atapi_state_load((const uint8_t *)src + sizeof(cd));
}

void s12_cdxa_run(uint32_t main_cycles)
{
	const uint32_t d = main_cycles - cd.last_cycle;
	cd.last_cycle = main_cycles;
	if (!cd.sh2_running)
		return;
	cd.sh2_frac += d * (SH2_CLOCK / PSX_CLOCK);
	const int64_t n = (int64_t)cd.sh2_frac;
	cd.sh2_frac -= (double)n;
	const int64_t target = cd.cpu.cycles + n;
	while (cd.cpu.cycles < target) {
		int64_t stop = cd.tx_busy && cd.tx_done < target ? cd.tx_done : target;
		const int64_t m = mtu_next();
		if (m < stop)
			stop = m > cd.cpu.cycles ? m : cd.cpu.cycles + 1;
		sh2_run(&cd.cpu, stop);
		if (cd.tx_busy && cd.cpu.cycles >= cd.tx_done)
			sci_tx_event();
		if (m <= cd.cpu.cycles)
			mtu_update(cd.cpu.cycles);
	}
}

#ifdef RS12_HARNESS
// comparisons: board RAMs (C448 SRAM then the SRAM behind it) next to the main RAM dumps
void s12_cdxa_dump(FILE *f)
{
	fwrite(cd.cram, 1, sizeof(cd.cram), f);
	fwrite(cd.sram, 1, sizeof(cd.sram), f);
	fwrite(&cd.cpu, 1, sizeof(cd.cpu), f); // r0-r15, pc, pr, ... as sh2.h
}
#endif

int s12_cdxa_audio(int16_t *out, int max_frames)
{
	const int n = cd.audio_n < max_frames ? cd.audio_n : max_frames;
	memcpy(out, cd.audio, (size_t)n * 4);
	cd.audio_n = 0;
	return n;
}

// ---- main CPU side (MAME namcos12_cdxa_device::amap) ----
int s12_cdxa_r16(uint32_t a, uint16_t *v)
{
	if (a >= 0x1f7c0000u && a < 0x1f7d0000u) {
		const uint8_t *r = cd.sram_enabled ? cd.sram : cd.cram;
		const uint32_t o = a - 0x1f7c0000u;
		*v = (uint16_t)(r[o] << 8 | r[o + 1]);
		return 1;
	}
	if (a == 0x1f7d800au) {
		*v = 0x20; // MAME: the code waits for it before writing 0x1f7d8000
		return 1;
	}
	if (a >= 0x1f7e0000u && a < 0x1f7e8010u) {
		*v = cd.ide_ps1 ? ata_r((int)((a >> 1) & 7)) : 0;
		return 1;
	}
	return 0;
}

void s12_cdxa_w8(uint32_t a, uint8_t d)
{
	const uint32_t o = (a - 0x1f7c0000u) ^ 1; // little-endian main CPU byte -> big-endian SH2 byte
	if ((a & 0xff00) == 0x9000)
		logf_once("ps1 b", a, d);
	if (!cd.sram_enabled)
		cd.cram[o] = d;
	cd.sram[o] = d;
}

int s12_cdxa_w16(uint32_t a, uint16_t d)
{
	if (a >= 0x1f7d0000u || (a & 0xfff) == 0 || (a & 0xff00) == 0x9000)
		logf_once("ps1 w", a, d);
	if (a >= 0x1f7c0000u && a < 0x1f7d0000u) {
		const uint32_t o = a - 0x1f7c0000u;
		if (!cd.sram_enabled) {
			cd.cram[o] = (uint8_t)(d >> 8);
			cd.cram[o + 1] = (uint8_t)d;
		}
		cd.sram[o] = (uint8_t)(d >> 8);
		cd.sram[o + 1] = (uint8_t)d;
		return 1;
	}
	switch (a) {
	case 0x1f7d6002u: // SH2 reset pulse
		if (d == 1) {
			sh2_reset(&cd.cpu);
			cd.sh2_running = 1;
			logf_once("reset", cd.cpu.pc, cd.cpu.r[15]);
		}
		return 1;
	case 0x1f7d6004u: case 0x1f7d6006u: case 0x1f7d6008u: case 0x1f7d600au:
		return 1; // ICD2061A clock generator: the sample rate is fixed at 37800 Hz here
	case 0x1f7d6010u: cd.ide_sh2 = d != 0; return 1;
	case 0x1f7d6012u: cd.ide_ps1 = d != 0; return 1;
	case 0x1f7d601au: cd.sram_enabled = d != 0; return 1;
	case 0x1f7d601eu: cd.int10_busy = 0; return 1;
	case 0x1f7d6000u: case 0x1f7d6018u: case 0x1f7d7000u:
		return 1; // written at boot, unknown (MAME leaves them unmapped)
	}
	if (a >= 0x1f7e0000u && a < 0x1f7e8010u) {
		if (cd.ide_ps1)
			ata_w((int)((a >> 1) & 7), d);
		return 1;
	}
	if (a >= 0x1f7f8000u && a < 0x1f7f8100u)
		return 1; // MB87078 volume: ponytail: fixed gain, the data is coded in the address
	return 0;
}

// MAME DISK_REGION "cdxa_pcb:ata:0:cdrom": <roms_dir>/<set>/<disc>.chd
int s12_cdxa_open_disc(const char *roms_dir, const char *set)
{
	static const char *const discs[][2] = { { "truckk", "tkk2-a" }, { "ujlnow", "ul1-a" }, { "questff", "qf1-a" } };
	for (unsigned i = 0; i < sizeof(discs) / sizeof(discs[0]); i++)
		if (!strcmp(set, discs[i][0])) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/%s/%s.chd", roms_dir, set, discs[i][1]);
			return s12_atapi_open(path);
		}
	return -1;
}
