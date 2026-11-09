// ReSys10: System 10 EXIO board (MAME src/mame/namco/namcos10_exio.cpp, BSD-3-Clause, windyfairy): a TMP95C061
// (MAME's TLCS-900/H core, src/tlcs900) running the program the main CPU uploads into the shared RAM, reading the
// analog inputs. The main CPU's side (0x1fe00000-0x1fffffff) is in s10.cpp; it calls in here.
#include "s10_exio.h"

#include <cstring>

#include "s10_tmp95.h"

static constexpr uint32_t PSXCLK = 33868800; // the main CPU's event clock (pcsx_rearmed psxRegs.cycle)

uint16_t s10_exio_analog[8];
uint8_t s10_exio_trackball[8];

static struct {
	Tmp95Cpu *cpu;
	uint8_t ram100[0x2f00];  // 0x000100-0x002fff (stack and such)
	uint16_t ram[0x2800];     // 0x003000-0x007fff mirror 0xff8000, shared with the main CPU (exio_ram)
	// (saved in states from here)
	uint8_t is_active, analog_idx, running;
	uint16_t bus_req, ctrl;
	uint32_t synced_cycle;    // psxRegs.cycle the EXIO CPU has caught up to
	int32_t debt;             // EXIO cycles run past the last target
	uint8_t tb_req[10], tb_req_len, tb_reply[10], tb_reply_len, tb_reply_pos; // the trackball board (serial channel 0)
	int32_t tb_wait;          // EXIO cycles until its next reply byte
} x;

// nflclsfb's trackball board on the EXIO's serial channel 0, worked out from the game (no MAME reference): the main
// CPU sends 10 bytes every 2 frames (0xb1, two 7-segment digits, lamps, a checksum) and takes bytes 1-8 of the 10 it
// gets back as the four trackballs' X / Y counters (8 bits, wrapping; the game uses their deltas, Y reversed); header
// and checksum are not checked. Its I/O test sends a0 00 .. 00 a0 instead and wants the same 10 bytes back.
static constexpr int32_t TB_BYTE_CYCLES = 22118400 / 3840; // (one byte at 38400 bauds, a guess: nothing checks it)

static void trackball_rx(uint8_t d) // a byte the EXIO program sent
{
	x.tb_req[x.tb_req_len++] = d;
	if (x.tb_req_len < sizeof(x.tb_req))
		return;
	x.tb_req_len = 0;
	memcpy(x.tb_reply, x.tb_req, sizeof(x.tb_reply)); // (another command, as the connection check's 0xa0: echoed)
	if (x.tb_req[0] == 0xb1) {
		uint8_t sum = x.tb_reply[0];
		for (int i = 0; i < 8; i++)
			sum += x.tb_reply[1 + i] = s10_exio_trackball[i];
		x.tb_reply[9] = sum;
	}
	x.tb_reply_len = sizeof(x.tb_reply);
	x.tb_reply_pos = 0;
	x.tb_wait = TB_BYTE_CYCLES;
}

static uint8_t ext_r(offs_t a)
{
	if (a >= 0x100 && a < 0x3000)
		return x.ram100[a - 0x100];
	const offs_t m = a & 0x7fff;
	if (m >= 0x3000) {
		const uint16_t w = x.ram[(m - 0x3000) >> 1];
		return (m & 1) ? uint8_t(w >> 8) : uint8_t(w);
	}
	return 0;
}

static void ext_w(offs_t a, uint8_t d)
{
	if (a >= 0x100 && a < 0x3000) {
		x.ram100[a - 0x100] = d;
		return;
	}
	const offs_t m = a & 0x7fff;
	if (m >= 0x3000) {
		uint16_t &w = x.ram[(m - 0x3000) >> 1];
		w = (m & 1) ? uint16_t((w & 0x00ff) | (d << 8)) : uint16_t((w & 0xff00) | d);
	}
}

void s10_exio_attach(int on)
{
	delete x.cpu;
	memset((void *)&x, 0, sizeof(x));
	if (!on)
		return;
	x.cpu = new Tmp95Cpu(22118400); // MAME: XTAL(22'118'400)
	x.cpu->m_space.ext_r = ext_r;
	x.cpu->m_space.ext_w = ext_w;
	// straight to memory: 0x1000-0x2fff of the private RAM, the shared RAM at 0x3000-0x7fff of every 32KB (mirror
	// 0xff8000); the first page (internal registers at 0x00-0x7f) stays on the callbacks
	x.cpu->m_space.map_ram(0x1000, 0x2fff, x.ram100 + 0xf00);
	for (offs_t m = 0; m < 0x1000000; m += 0x8000)
		x.cpu->m_space.map_ram(m + 0x3000, m + 0x7fff, (uint8_t *)x.ram);
	// MAME namcos10_exio_device::device_add_mconfig port callbacks
	x.cpu->port7_write().set([] (uint8_t data) { x.analog_idx = data; }); // the analog channel pair the program reads
	x.cpu->port8_write().set([] (uint8_t data) { x.is_active |= data != 0; }); // MAME HACK: the CPU is alive
	x.cpu->an_read<0>().set([] () { return uint16_t(s10_exio_analog[(x.analog_idx & 3) * 2]); });
	x.cpu->an_read<1>().set([] () { return uint16_t(s10_exio_analog[(x.analog_idx & 3) * 2 + 1]); });
	x.cpu->tx_byte<0>().set(trackball_rx);
	x.cpu->start();
}

void s10_exio_reset(uint32_t now)
{
	if (!x.cpu)
		return;
	x.is_active = x.analog_idx = x.running = 0;
	x.bus_req = x.ctrl = 0;
	x.synced_cycle = now;
	x.debt = 0;
	x.tb_req_len = x.tb_reply_len = 0;
	memset(x.ram, 0, sizeof(x.ram));
	memset(x.ram100, 0, sizeof(x.ram100));
	x.cpu->reset(); // (held in reset until the main CPU starts it)
}

// catch the EXIO CPU up with the main CPU (22.1184 MHz against the 33.8688 MHz event clock)
void s10_exio_sync(uint32_t now)
{
	if (!x.cpu)
		return;
	const uint32_t d = now - x.synced_cycle;
	x.synced_cycle = now;
	if (!x.running || d > PSXCLK) // (a stopped CPU, or a gap from a load)
		return;
	const int32_t want = int32_t(uint64_t(d) * 22118400u / PSXCLK) - x.debt;
	if (want <= 0) {
		x.debt = -want;
		return;
	}
	int32_t left = want;
	while (left > 0) { // (in slices while the trackball board has reply bytes due: one RX interrupt per byte)
		const int32_t slice = x.tb_reply_len && x.tb_wait < left ? (x.tb_wait > 0 ? x.tb_wait : 1) : left;
		x.cpu->run(slice);
		const int32_t ran = slice - x.cpu->m_icount; // (the last instruction ran past the target)
		left -= ran;
		if (x.tb_reply_len && (x.tb_wait -= ran) <= 0) {
			x.cpu->rx_byte<0>(x.tb_reply[x.tb_reply_pos++]);
			x.tb_wait = TB_BYTE_CYCLES;
			if (x.tb_reply_pos == x.tb_reply_len)
				x.tb_reply_len = 0;
		}
	}
	x.debt = -left;
}

int s10_exio_active(void) { return x.cpu != nullptr; }

void s10_exio_dump(FILE *f) // diagnostics (libretro.c RS10_RAMDUMP): 0x000000-0x007fff as the EXIO CPU sees it
{
	uint8_t img[0x8000] = {};
	memcpy(img + 0x100, x.ram100, sizeof(x.ram100));
	memcpy(img + 0x3000, x.ram, sizeof(x.ram));
	fwrite(img, 1, sizeof(img), f);
}

uint16_t s10_exio_r16(uint32_t o, uint32_t now)
{
	s10_exio_sync(now);
	if (o >= 0x6000 && o < 0x10000) { // ram_r: a byte per halfword, odd = the high byte of the CPU's word
		const uint32_t i = (o - 0x6000) >> 1;
		return (i & 1) ? x.ram[i / 2] >> 8 : x.ram[i / 2] & 0xff;
	}
	switch (o & ~2u) {
	case 0x10000: return x.ctrl;
	case 0x18000: return x.bus_req == 1 ? 2 : 0;
	case 0x28000: return x.is_active ? 0x0101 : 0; // cpu_status_r
	}
	return 0;
}

void s10_exio_w16(uint32_t o, uint16_t d, uint32_t now)
{
	s10_exio_sync(now);
	if (o >= 0x6000 && o < 0x10000) {
		const uint32_t i = (o - 0x6000) >> 1;
		uint16_t &w = x.ram[i / 2];
		w = (i & 1) ? uint16_t(((d & 0xff) << 8) | (w & 0xff)) : uint16_t((w & 0xff00) | (d & 0xff));
		x.cpu->code_written((const uint8_t *)&w); // (a program the EXIO CPU runs, rewritten)
		return;
	}
	switch (o & ~2u) {
	case 0x10000: // ctrl_w: bit 0 rising resets (starts) the CPU, falling halts it
		if ((d & 1) && !(x.ctrl & 1)) {
			x.cpu->reset();
			x.running = 1;
			x.debt = 0;
		} else if (!(d & 1) && (x.ctrl & 1))
			x.running = 0;
		x.ctrl = d;
		break;
	case 0x18000:
		x.bus_req = d;
		break;
	}
}

size_t s10_exio_state_size(void)
{
	if (!x.cpu)
		return 0;
	return x.cpu->m_save.size() + sizeof(x.ram100) + sizeof(x.ram) + sizeof(x) - offsetof(decltype(x), is_active);
}

void s10_exio_state_save(void *dst)
{
	if (!x.cpu)
		return;
	uint8_t *p = (uint8_t *)dst;
	x.cpu->m_save.save(p);
	p += x.cpu->m_save.size();
	memcpy(p, x.ram100, sizeof(x.ram100));
	p += sizeof(x.ram100);
	memcpy(p, x.ram, sizeof(x.ram));
	p += sizeof(x.ram);
	memcpy(p, &x.is_active, sizeof(x) - offsetof(decltype(x), is_active));
}

void s10_exio_state_load(const void *src, uint32_t now)
{
	if (!x.cpu)
		return;
	const uint8_t *p = (const uint8_t *)src;
	x.cpu->m_save.load(p);
	p += x.cpu->m_save.size();
	memcpy(x.ram100, p, sizeof(x.ram100));
	p += sizeof(x.ram100);
	memcpy(x.ram, p, sizeof(x.ram));
	p += sizeof(x.ram);
	memcpy(&x.is_active, p, sizeof(x) - offsetof(decltype(x), is_active));
	x.synced_cycle = now;
	x.cpu->drop_code(); // (its RAM came back from the state: decoded blocks may be stale)
}
