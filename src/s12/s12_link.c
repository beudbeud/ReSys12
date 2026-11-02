// ReSys12: kartduel link board CPU (MAME kartduel_state: H8/3002 at 14.7456 MHz, linkcpu_program_map). It has no ROM:
// the main CPU uploads its program into the 64KB shared RAM (0x1f780000 on its side, 0x000000 here) and releases its
// reset line. This is the second H8 build (src/h8 compiled again with h8_link_names.h).
#include "h8_link_names.h"
#include "h8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r3000a.h"
#include "psxmem.h"
#include "s12_link.h"

extern int h8_blk_enable, h8_jit_enable; // (src/h8/h8.c)

uint16_t s12_link_ram[0x8000]; // shared RAM, host-order words (the H8 RAM fast path layout)

static uint8_t s_ext_ram[0x10000];  // 0x010000-0x01ffff
static uint8_t s_net_ram[0x2000];   // 0x200000-0x201fff: the C422's frame RAM (13-bit indexes)
static uint8_t s_ext_misc[0x40];    // 0x400000-07 (mirrored at 0x408000), 0x410000-0f, 0x418002-07
static uint16_t s_rx_index, s_tx_index; // C422 receive / transmit positions in s_net_ram
static int s_irq;
static int s_rx_ready; // the firmware has set the receive index since the reset
static struct { uint8_t data[256]; int count; } s_rxq[32];
static int s_rxq_head, s_rxq_n;
static void deliver(void);
void (*s12_link_send_hook)(const uint8_t *data, int count);
void (*s12_link_poll_hook)(void);
static int s_running;               // reset line released
static uint32_t s_last_cycle;       // main CPU cycle the link CPU has been run to
static uint64_t s_frac;             // main cycles not yet turned into link states (x 14745600)
static int s_log = -1;

static void note(const char *what, uint32_t a, uint32_t d)
{
	if (s_log < 0)
		s_log = getenv("RS12_LINKLOG") != NULL;
	if (s_log)
		fprintf(stderr, "linkcpu %s %06x %04x pc %06x\n", what, a, d, h8.PPC);
}

// main CPU IRQ10, the network board's interrupt to the main CPU: what raises it is not known yet (harness experiment
// RS12_IRQ10=rx,tx,ctl: frame received, frame sent, 0x400002 bit 1)
static void main_irq10(const char *why)
{
#ifndef RS12_HARNESS
	(void)why;
#else
	static const char *mode = (const char *)1;
	if (mode == (const char *)1)
		mode = getenv("RS12_IRQ10");
	if (mode && strstr(mode, why))
		psxHu32ref(0x1070) |= SWAPu32(0x400);
#endif
}


static uint8_t *misc(uint32_t a)
{
	if ((a & ~0x8000u) >= 0x400000 && (a & ~0x8000u) < 0x400008) // (0x408000: the same registers, A15 not decoded)
		return &s_ext_misc[(a & ~0x8000u) - 0x400000];
	if (a >= 0x410000 && a < 0x410010)
		return &s_ext_misc[0x10 + (a - 0x410000)];
	if (a >= 0x418002 && a < 0x418008)
		return &s_ext_misc[0x20 + (a - 0x418000)];
	return NULL;
}

// C422 pointer registers hold a 13-bit index with the bytes swapped: (lo & 0x1f) << 8 | hi
static uint16_t index_of(const uint8_t *r) { return (uint16_t)((r[1] & 0x1f) << 8 | r[0]); }
static void index_put(uint8_t *r, uint16_t i) { r[0] = (uint8_t)i; r[1] = (uint8_t)(i >> 8); }


static void transmit(void)
{
	uint8_t *count = misc(0x410004);
	uint8_t frame[256];
	const int n = count[0];
	s_tx_index = index_of(misc(0x410006));
	for (int i = 0; i < n; i++)
		frame[i] = s_net_ram[(s_tx_index + i) & 0x1fff];
	count[0] = 0; // (the firmware waits for it before the next frame)
	note("tx", s_tx_index, (uint32_t)n);
	if (s_log > 0 && n >= 6)
		fprintf(stderr, "linkframe tx %02x %02x %02x %02x %02x %02x\n", frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
	if (n && s12_link_send_hook)
		s12_link_send_hook(frame, n);
	main_irq10("tx");
}

void s12_link_receive(const uint8_t *data, int count)
{
	if (!s_running || !s_rx_ready || count <= 0 || count > 256)
		return;
	s12_link_sync();
	if (s_rxq_n == 32) {
		note("rx overflow", 0, (uint32_t)count);
		return;
	}
	const int t = (s_rxq_head + s_rxq_n) % 32;
	memcpy(s_rxq[t].data, data, (size_t)count);
	s_rxq[t].count = count;
	s_rxq_n++;
	deliver();
}

uint8_t h8_ext_read8(uint32_t a)
{
	if (a >= 0x010000 && a < 0x020000)
		return s_ext_ram[a - 0x010000];
	if (a >= 0x200000 && a < 0x202000)
		return s_net_ram[a - 0x200000];
	uint8_t *p = misc(a);
	if (p)
		return *p;
	note("r8 unmapped", a, 0xff);
	return 0xff;
}

uint16_t h8_ext_read16(uint32_t a)
{
	return (uint16_t)(h8_ext_read8(a) << 8 | h8_ext_read8(a + 1));
}

void h8_ext_write8(uint32_t a, uint8_t d)
{
	if (a >= 0x010000 && a < 0x020000) {
		s_ext_ram[a - 0x010000] = d;
		return;
	}
	if (a >= 0x200000 && a < 0x202000) {
		s_net_ram[a - 0x200000] = d;
		return;
	}
	uint8_t *p = misc(a);
	if (!p) {
		note("w8 unmapped", a, d);
		return;
	}
	if ((a & ~0x8000u) == 0x400002 && (d & 2) && !(*p & 2))
		main_irq10("ctl");
	*p = d;
	if ((a & ~0x8000u) == 0x400007) { // receive index set by the firmware (low byte last)
		s_rx_index = index_of(misc(0x400006));
		s_rx_ready = 1;
		deliver();
	}
	else if (((a & ~0x8001u) == 0x400000 || a == 0x410000) && s_irq) { // IRQ0 acknowledge / status clear: next frame
		s_irq = 0; // (the next frame comes with the next slice: the handler acks first, then reads the index)
		h8_set_irq_line(0, 0);
	} else if (a == 0x410005)
		transmit();
}

void h8_ext_write16(uint32_t a, uint16_t d)
{
	h8_ext_write8(a, (uint8_t)(d >> 8));
	h8_ext_write8(a + 1, (uint8_t)d);
}

uint8_t h8_port_in(int port) { (void)port; return 0xff; }
void h8_port_out(int port, uint8_t data, uint8_t ddr) { note("port", (uint32_t)port, (uint32_t)(data | ddr << 8)); }
uint16_t h8_adc_in(int ch) { (void)ch; return 0; }
void h8_sci_tx(int sci, int state) { (void)sci; (void)state; }
void h8_sci_clk(int sci, int state) { (void)sci; (void)state; }
void h8_sci_tx_byte(int sci, uint8_t data) { note("sci tx", (uint32_t)sci, data); }

void s12_link_reset(void)
{
	memset(s12_link_ram, 0, sizeof(s12_link_ram));
	memset(s_ext_ram, 0, sizeof(s_ext_ram));
	memset(s_ext_misc, 0, sizeof(s_ext_misc));
	memset(s_net_ram, 0, sizeof(s_net_ram));
	s_rx_index = s_tx_index = 0;
	s_irq = 0;
	s_running = 0;
	h8_set_rom(NULL, 0);
	h8_set_ram(s12_link_ram, 0x000000, sizeof(s12_link_ram));
	h8_blk_enable = 0; // the program is in RAM: no pre-decoded blocks
	h8_idle_enable = 0;
	h8_jit_enable = 0;
}

// frames from the other cabinet, one at a time: the firmware handles one frame per IRQ0 (the wire spaces them)

static void deliver(void)
{
	if (s_irq || !s_rxq_n || !s_rx_ready)
		return;
	const uint8_t *data = s_rxq[s_rxq_head].data;
	const int count = s_rxq[s_rxq_head].count;
	s_rxq_head = (s_rxq_head + 1) % 32;
	s_rxq_n--;
	for (int i = 0; i < count; i++) {
		s_net_ram[s_rx_index] = data[i];
		s_rx_index = (s_rx_index + 1) & 0x1fff;
	}
	index_put(misc(0x400006), s_rx_index);
	note("rx", s_rx_index, (uint32_t)count);
	s_irq = 1;
	h8_set_irq_line(0, 1);
	main_irq10("rx");
}

void s12_link_sync(void)
{
	const uint32_t now = psxRegs.cycle;
	const uint32_t d = now - s_last_cycle;
	s_last_cycle = now;
	if (!s_running)
		return;
	if (s12_link_poll_hook)
		s12_link_poll_hook();
	s_frac += (uint64_t)d * 14745600u;
	const uint64_t states = s_frac / 33868800u;
	s_frac -= states * 33868800u;
	if (states)
		h8_run((int)states);
	deliver();
}

void s12_link_reset_line(int release)
{
#ifdef RS12_HARNESS
	if (getenv("RS12_NOLINKCPU")) // comparisons: the link board without its CPU, as before
		release = 0;
#endif
	s12_link_sync();
	note(release ? "reset released" : "reset asserted", 0, 0);
	if (release && !s_running) {
		s_irq = 0;
		s_rx_ready = 0;
		s_rxq_n = 0;
		h8_set_irq_line(0, 0);
		h8_reset();
		s_frac = 0;
	}
	s_running = release;
}

// save states (s12_state.c, kartduel only): the link CPU, its memories and the C422 (frames in the queue are dropped)
typedef struct {
	int running, irq, rx_ready;
	uint32_t last_cycle;
	uint64_t frac;
	uint16_t rx_index, tx_index;
} LinkState;

uint32_t s12_link_state_size(void)
{
	return (uint32_t)(sizeof(LinkState) + sizeof(h8) + sizeof(s12_link_ram) + sizeof(s_ext_ram) + sizeof(s_net_ram) +
		sizeof(s_ext_misc));
}

void s12_link_state_save(void *dst)
{
	const LinkState ls = { s_running, s_irq, s_rx_ready, s_last_cycle, s_frac, s_rx_index, s_tx_index };
	uint8_t *d = (uint8_t *)dst;
	memcpy(d, &ls, sizeof(ls)), d += sizeof(ls);
	memcpy(d, &h8, sizeof(h8)), d += sizeof(h8);
	memcpy(d, s12_link_ram, sizeof(s12_link_ram)), d += sizeof(s12_link_ram);
	memcpy(d, s_ext_ram, sizeof(s_ext_ram)), d += sizeof(s_ext_ram);
	memcpy(d, s_net_ram, sizeof(s_net_ram)), d += sizeof(s_net_ram);
	memcpy(d, s_ext_misc, sizeof(s_ext_misc));
}

void s12_link_state_load(const void *src)
{
	const uint8_t *s = (const uint8_t *)src;
	LinkState ls;
	uint16_t *ram = h8.ram;
	const uint32_t ram_base = h8.ram_base, ram_size = h8.ram_size;
	memcpy(&ls, s, sizeof(ls)), s += sizeof(ls);
	memcpy(&h8, s, sizeof(h8)), s += sizeof(h8);
	h8.rom = NULL; // (no ROM: the program runs from the shared RAM)
	h8.rom_size = 0;
	h8.ram = ram;
	h8.ram_base = ram_base;
	h8.ram_size = ram_size;
	memcpy(s12_link_ram, s, sizeof(s12_link_ram)), s += sizeof(s12_link_ram);
	memcpy(s_ext_ram, s, sizeof(s_ext_ram)), s += sizeof(s_ext_ram);
	memcpy(s_net_ram, s, sizeof(s_net_ram)), s += sizeof(s_net_ram);
	memcpy(s_ext_misc, s, sizeof(s_ext_misc));
	s_running = ls.running, s_irq = ls.irq, s_rx_ready = ls.rx_ready;
	s_last_cycle = ls.last_cycle, s_frac = ls.frac;
	s_rx_index = ls.rx_index, s_tx_index = ls.tx_index;
	s_rxq_n = 0;
}
