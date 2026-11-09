// ReSys10: System 10 MEM(P3) board (MAME src/mame/namco/namcos10.cpp namcos10_memp3_state, BSD-3-Clause, windyfairy):
// a TMP95C061 (src/tlcs900) running the firmware the main CPU uploads into its RAM, feeding a Sanyo LC82310 MP3
// decoder (MAME src/devices/sound/lc82310.cpp, windyfairy; minimp3 behind src/sound/mp3_audio.cpp) and reading the
// analog inputs (g13jnr's gun). The NAND side of the board is MEM(N) (s10.cpp).
#include "s10_memp3.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>

#include "s10_tmp95.h"
#include "mp3_audio.h"

static constexpr uint32_t PSXCLK = 33868800; // the main CPU's event clock (pcsx_rearmed psxRegs.cycle)
static constexpr uint32_t MCU_CLOCK = 101491200 / 2; // MAME: XTAL(101'491'200) / 2, "Measured"

uint16_t s10_p3_analog[4];
uint16_t s10_p3_dipsw = 0xffff;
int s10_p3_clock_pct = 100; // MEMP3_DIPSW (active low; bit 3 = debug mode)

// ---- LC82310 (lc82310.cpp): the control port is a serial command interface, the data port takes the MP3 stream ----
namespace {
struct Lc82310
{
	enum : uint8_t { ACCEPTING_CMD, ACCEPTING_PARAM };
	enum : uint8_t { CMD_UNK13_VOL = 0x13, CMD_UNK15_VOL = 0x15 };

	struct Regs { // (saved in states, with the decoder's own state)
		std::array<uint8_t, 0x1000> mp3data;
		std::array<short, 1152 * 2> samples;
		uint32_t mp3data_count;
		int32_t sample_count, samples_idx, frame_channels;
		uint32_t frame_sample_rate;
		float output_gain[2];
		uint8_t csctl, ckctl, dictl, doctl, ctl_state, ctl_cmd, ctl_bits, ctl_byte, ctl_out_byte;
		uint32_t frac; // resampling position (16.16) from frame_sample_rate to 44100 Hz
	} r;
	std::unique_ptr<mp3_audio> mp3dec;
	device_t host = device_t(machine_config(), "lc82310", "", nullptr, 0); // (the decoder's save_item registry)

	Lc82310()
	{
		memset((void *)&r, 0, sizeof(r));
		mp3dec = std::make_unique<mp3_audio>(&r.mp3data[0]);
		mp3dec->register_save(host);
		r.frame_sample_rate = 44100;
	}
	void reset()
	{
		r.output_gain[0] = r.output_gain[1] = 1.0f; // (MAME's stream gain until a volume command)
		r.csctl = r.ckctl = r.dictl = r.doctl = 0;
		r.ctl_bits = r.ctl_byte = r.ctl_out_byte = 0;
		r.ctl_state = ACCEPTING_CMD;
		reset_playback();
	}
	void reset_playback()
	{
		r.mp3data.fill(0);
		r.samples.fill(0);
		mp3dec->clear();
		r.mp3data_count = 0;
		r.sample_count = 0;
		r.samples_idx = 0;
		r.frame_channels = 2;
	}
	void zcsctl_w(int state) { r.csctl = uint8_t(state); }
	void ckctl_w(int state)
	{
		if (r.csctl == 0 && r.ckctl == 0 && state == 1) {
			r.ctl_byte |= r.dictl << r.ctl_bits;
			r.ctl_bits++;
			if (r.ctl_bits > 7) {
				if (r.ctl_state == ACCEPTING_CMD) {
					r.ctl_out_byte = 0;
					r.ctl_cmd = r.ctl_byte;
					r.ctl_state = ACCEPTING_PARAM;
				} else {
					handle_command(r.ctl_cmd, r.ctl_byte);
					r.ctl_state = ACCEPTING_CMD;
				}
				r.ctl_byte = 0;
				r.ctl_bits = 0;
			}
			r.doctl = r.ctl_out_byte & 1;
			r.ctl_out_byte >>= 1;
		}
		r.ckctl = uint8_t(state);
	}
	void dictl_w(int state) { r.dictl = uint8_t(state); }
	int doctl_r() const { return r.doctl; }
	int demand_r() const { return r.mp3data_count < r.mp3data.size(); }
	void dimpg_w(uint8_t data)
	{
		if (r.mp3data_count >= r.mp3data.size()) { // drop a byte if the buffer is full and it's still sending
			std::copy(r.mp3data.begin() + 1, r.mp3data.end(), r.mp3data.begin());
			r.mp3data_count--;
		}
		r.mp3data[r.mp3data_count++] = data;
	}
	void handle_command(uint8_t cmd, uint8_t param)
	{
		if (cmd != CMD_UNK13_VOL && cmd != CMD_UNK15_VOL)
			return;
		// (lc82310.cpp gain_table: from the in-game settings to what is sent to the decoder)
		static const float gain_table[] = {
			1.0f, 30.0f / 31, 29.0f / 31, 28.0f / 31, 27.0f / 31, 26.0f / 31, 25.0f / 31, 24.0f / 31, 23.0f / 31,
			22.0f / 31, 21.0f / 31, 20.0f / 31, 19.0f / 31, 18.0f / 31, 17.0f / 31, 16.0f / 31, 15.0f / 31, 14.0f / 31,
			13.0f / 31, 12.0f / 31, 11.0f / 31, 10.0f / 31, 9.0f / 31, 8.5f / 31, 8.0f / 31, 7.0f / 31, 6.0f / 31,
			5.5f / 31, 5.0f / 31, 4.5f / 31, 4.0f / 31, 3.5f / 31, 3.0f / 31, 2.75f / 31, 2.5f / 31, 2.25f / 31,
			2.0f / 31, (1.0f + 5.0f / 6) / 31, (1.0f + 4.0f / 6) / 31, (1.0f + 3.0f / 6) / 31, (1.0f + 2.0f / 6) / 31,
			(1.0f + 1.0f / 6) / 31, 1.0f / 31,
			33.0f / 34 / 31, 32.0f / 34 / 31, 31.0f / 34 / 31, 30.0f / 34 / 31, 29.0f / 34 / 31, 28.0f / 34 / 31,
			27.0f / 34 / 31, 26.0f / 34 / 31, 25.0f / 34 / 31, 24.0f / 34 / 31, 23.0f / 34 / 31, 22.0f / 34 / 31,
			21.0f / 34 / 31, 20.0f / 34 / 31, 19.0f / 34 / 31, 18.0f / 34 / 31, 17.0f / 34 / 31, 16.0f / 34 / 31,
			15.0f / 34 / 31, 14.0f / 34 / 31, 13.0f / 34 / 31, 12.0f / 34 / 31, 11.0f / 34 / 31, 10.0f / 34 / 31,
			9.0f / 34 / 31, 8.0f / 34 / 31, 7.0f / 34 / 31, 6.0f / 34 / 31, 5.0f / 34 / 31, 4.0f / 34 / 31,
			3.0f / 34 / 31, 2.0f / 34 / 31, 1.0f / 34 / 31, 0.0f,
		};
		r.output_gain[cmd == CMD_UNK15_VOL ? 1 : 0] = gain_table[std::min<uint8_t>(param, 0x4c)];
	}
	// lc82310.cpp fill_buffer: decode the next frame; false = nothing decoded
	bool fill_buffer()
	{
		int pos = 0, rate = 0;
		const bool ok = mp3dec->decode_buffer(pos, int(r.mp3data_count), &r.samples[0], r.sample_count, rate, r.frame_channels);
		r.samples_idx = 0;
		if (!ok || r.sample_count <= 0) {
			if (r.mp3data_count >= r.mp3data.size()) {
				std::copy(r.mp3data.begin() + 1, r.mp3data.end(), r.mp3data.begin());
				r.mp3data_count--;
			}
			r.sample_count = 0;
			return false;
		}
		std::copy(r.mp3data.begin() + pos, r.mp3data.end(), r.mp3data.begin());
		r.mp3data_count -= pos;
		if (rate > 0)
			r.frame_sample_rate = uint32_t(rate);
		if (getenv("RS10_P3LOG")) {
			static unsigned n;
			if (!(++n % 500))
				fprintf(stderr, "lc82310: %u MP3 frames, %u Hz, %d ch\n", n, r.frame_sample_rate, r.frame_channels);
		}
		return true;
	}
	// n stereo frames at 44100 Hz mixed into out (MAME changes the stream's rate to the MP3's; this steps through
	// the decoded samples at that rate)
	void render(int16_t *out, int n)
	{
		const int wps = std::min(r.frame_channels, 2);
		const uint32_t step = uint32_t((uint64_t(r.frame_sample_rate) << 16) / 44100);
		for (int i = 0; i < n; i++) {
			if (r.sample_count <= 0 && !fill_buffer())
				return; // (no stream: silence)
			const short *s = &r.samples[r.samples_idx * wps];
			for (int c = 0; c < 2; c++) {
				const int v = out[i * 2 + c] + int(s[c ? wps >> 1 : 0] * r.output_gain[c]);
				out[i * 2 + c] = int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
			}
			r.frac += step;
			r.samples_idx += r.frac >> 16;
			r.frac &= 0xffff;
			if (r.samples_idx >= r.sample_count)
				r.sample_count = 0;
		}
	}
};
} // namespace

static double s_prof_s;
static uint64_t s_prof_cycles;

static struct {
	Tmp95Cpu *cpu;
	Lc82310 *mp3;
	uint16_t *ram; // mcu_ram: 0x800000-0xffffff of the sub CPU (8MB, little-endian words), banked to the main CPU
	// (saved in states from here)
	uint16_t ram_bank, unk_val, unk2_val, unk3_val;
	uint8_t port7_data, porta_data, stream_available, received_byte, subcomm_busy, running;
	uint32_t synced_cycle;
	int32_t debt;
} p;

// ---- sub CPU bus (MAME namcos10_memp3_state::mcu_map) ----
static uint16_t mcu_r16(offs_t a)
{
	if (a < 0x800000 && getenv("RS10_P3IO"))
		fprintf(stderr, "p3 r16 %06x\n", a);
	if (a >= 0x800000)
		return p.ram[((a & 0x7fffff) >> 1)];
	switch (a & ~1u) {
	case 0x100000: return p.unk_val;
	case 0x100002: return s10_p3_dipsw;
	case 0x100004: return p.unk2_val;
	case 0x10000e: return 1; // DECODE PLD version
	case 0x100010: return p.unk3_val;
	case 0x100032: return p.stream_available;
	}
	if (a >= 0x10001e && a < 0x100030)
		return 1; // DRAM-C / NAND-C PLD versions
	return 0;
}

static void mcu_w16(offs_t a, uint16_t d)
{
	if (a >= 0x800000) {
		p.ram[(a & 0x7fffff) >> 1] = d;
		return;
	}
	switch (a & ~1u) {
	case 0x100000: p.unk_val = d; break;
	case 0x100004: p.unk2_val = d; break;
	case 0x100008: // subcomm_ack_w
		p.subcomm_busy = 0;
		p.cpu->set_input(TLCS900_INT5, CLEAR_LINE);
		break;
	case 0x100010: p.unk3_val = d; break;
	case 0x100030: // mp3_data_w
		if (getenv("RS10_P3LOG")) {
			static unsigned n;
			if (!(++n % 1000))
				fprintf(stderr, "p3: %u MP3 bytes, buffered %u\n", n, p.mp3->r.mp3data_count);
		}
		p.mp3->dimpg_w(uint8_t(d));
		p.received_byte = 1;
		break;
	case 0x100032: { // mp3_stream_status_w: 0 then 1 at every new stream
		const bool avail = d & 1;
		if (getenv("RS10_P3LOG"))
			fprintf(stderr, "p3: stream status %d\n", avail);
		if (p.stream_available && !avail)
			p.mp3->reset_playback();
		p.stream_available = avail;
		break;
	}
	}
}

static uint8_t mcu_r8(offs_t a)
{
	const uint16_t w = mcu_r16(a & ~1u);
	return (a & 1) ? uint8_t(w >> 8) : uint8_t(w);
}

static void mcu_w8(offs_t a, uint8_t d)
{
	if (a >= 0x800000) {
		uint16_t &w = p.ram[(a & 0x7fffff) >> 1];
		w = (a & 1) ? uint16_t((w & 0x00ff) | (d << 8)) : uint16_t((w & 0xff00) | d);
		return;
	}
	mcu_w16(a & ~1u, (a & 1) ? uint16_t(d << 8) : d); // (a byte in its lane, as a 16-bit device sees it)
}

extern "C" void s10_memp3_attach(int on)
{
	if (s_prof_cycles)
		fprintf(stderr, "p3 prof: %.2f s for %.1f M cycles (%.1f ns/cycle)\n", s_prof_s, s_prof_cycles / 1e6,
			s_prof_s * 1e9 / s_prof_cycles);
	s_prof_s = 0, s_prof_cycles = 0;
	delete p.cpu;
	delete p.mp3;
	free(p.ram);
	memset((void *)&p, 0, sizeof(p));
	if (!on)
		return;
	p.ram = (uint16_t *)calloc(1, 0x800000);
	p.mp3 = new Lc82310();
	p.cpu = new Tmp95Cpu(MCU_CLOCK);
	p.cpu->m_space.ext_r = mcu_r8;
	p.cpu->m_space.ext_w = mcu_w8;
	p.cpu->m_space.ext_r16 = mcu_r16;
	p.cpu->m_space.ext_w16 = mcu_w16;
	p.cpu->m_space.map_ram(0x800000, 0xffffff, (uint8_t *)p.ram); // (mcu_ram: little-endian words = bytes in order)
	// MAME namcos10_memp3_base port callbacks
	p.cpu->port7_write().set([] (uint8_t data) {
		p.mp3->zcsctl_w(BIT(data, 3));
		p.mp3->dictl_w(BIT(data, 1));
		p.mp3->ckctl_w(BIT(data, 2));
		p.port7_data = data;
	});
	p.cpu->porta_write().set([] (uint8_t data) { p.porta_data = data; });
	p.cpu->port7_read().set([] () { return uint8_t((p.port7_data & ~1) | p.mp3->doctl_r()); });
	p.cpu->porta_read().set([] () { return p.porta_data; });
	p.cpu->portb_read().set([] () {
		const uint8_t v = !p.received_byte && p.mp3->demand_r();
		p.received_byte = 0;
		return v;
	});
	p.cpu->an_read<0>().set([] () { return s10_p3_analog[0]; });
	p.cpu->an_read<1>().set([] () { return s10_p3_analog[1]; });
	p.cpu->an_read<2>().set([] () { return s10_p3_analog[2]; });
	p.cpu->an_read<3>().set([] () { return s10_p3_analog[3]; });
	p.cpu->start();
	p.cpu->m_space.trace_io = getenv("RS10_P3IO") != nullptr;
}

int s10_memp3_active(void) { return p.cpu != nullptr; }

void s10_memp3_reset(uint32_t now)
{
	if (!p.cpu)
		return;
	memset(p.ram, 0, 0x800000);
	p.ram_bank = p.unk_val = p.unk3_val = 0;
	p.unk2_val = 0xffff;
	p.port7_data = p.porta_data = 0;
	p.stream_available = p.received_byte = 0;
	p.subcomm_busy = 0;
	p.running = 0; // (MAME: suspended until the main CPU writes the firmware start)
	p.synced_cycle = now;
	p.debt = 0;
	p.cpu->reset();
	p.mp3->reset();
}

void s10_memp3_sync(uint32_t now)
{
	if (!p.cpu)
		return;
	const uint32_t d = now - p.synced_cycle;
	p.synced_cycle = now;
	if (!p.running || d > PSXCLK)
		return;
	const int32_t want = int32_t(uint64_t(d) * MCU_CLOCK / 100 * s10_p3_clock_pct / PSXCLK) - p.debt;
	if (want <= 0) {
		p.debt = -want;
		return;
	}
	static const bool prof = getenv("RS10_P3PROF") != nullptr;
	struct timespec t0, t1;
	if (prof)
		clock_gettime(CLOCK_MONOTONIC, &t0);
	p.cpu->run(want);
	p.debt = -p.cpu->m_icount;
	if (prof) { // diagnostics: host time in the sub CPU (printed at unload)
		clock_gettime(CLOCK_MONOTONIC, &t1);
		s_prof_s += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
		s_prof_cycles += uint64_t(want);
	}
	if (getenv("RS10_P3LOG")) // diagnostics: where the sub CPU is
		fprintf(stderr, "p3 pc %06x halted %d busy %d ran %d\n", p.cpu->m_pc.d, p.cpu->m_halted, p.subcomm_busy, want);
}

// ---- main CPU side (MAME namcos10_memp3_map_inner), offset from 0x1f300000 ----
int s10_memp3_r16(uint32_t o, uint32_t now, uint16_t *v)
{
	s10_memp3_sync(now);
	if (o >= 0x20000 && o < 0x40000) { // ram_r
		*v = p.ram[(p.ram_bank * 0x10000u + ((o - 0x20000) >> 1)) & 0x3fffff];
		return 1;
	}
	switch (o) {
	case 0x06: *v = !p.subcomm_busy; return 1; // subcomm_busy_r
	case 0x0e: *v = 1; return 1;               // unk_status1_r: the CPU program can be uploaded
	}
	return 0;
}

int s10_memp3_w16(uint32_t o, uint16_t d, uint32_t now)
{
	s10_memp3_sync(now);
	if (o >= 0x20000 && o < 0x40000) { // ram_w
		uint16_t *w = &p.ram[(p.ram_bank * 0x10000u + ((o - 0x20000) >> 1)) & 0x3fffff];
		*w = d;
		p.cpu->code_written((const uint8_t *)w); // (a program the sub CPU runs, rewritten)
		return 1;
	}
	switch (o) {
	case 0x00: // firmware_write_w: 1 starts the sub CPU
		if (d == 1) {
			p.cpu->reset();
			p.running = 1;
			p.debt = 0;
		}
		return 1;
	case 0x06: // subcomm_int_w: commands are taken on INT5
		p.cpu->set_input(TLCS900_INT5, ASSERT_LINE);
		p.subcomm_busy = 1;
		return 1;
	case 0x0c: p.ram_bank = d; return 1;
	}
	return 0;
}

void s10_memp3_dump(FILE *f) // diagnostics (libretro.c RS10_RAMDUMP): the sub CPU's RAM, 0x800000-0xffffff
{
	if (p.ram)
		fwrite(p.ram, 1, 0x800000, f);
}

void s10_memp3_render(int16_t *out, int frames)
{
	if (p.mp3)
		p.mp3->render(out, frames);
}

size_t s10_memp3_state_size(void)
{
	if (!p.cpu)
		return 0;
	return p.cpu->m_save.size() + p.mp3->host.m_save.size() + sizeof(Lc82310::Regs) + 0x800000 +
		sizeof(p) - offsetof(decltype(p), ram_bank);
}

void s10_memp3_state_save(void *dst)
{
	if (!p.cpu)
		return;
	uint8_t *o = (uint8_t *)dst;
	p.cpu->m_save.save(o);
	o += p.cpu->m_save.size();
	p.mp3->host.m_save.save(o);
	o += p.mp3->host.m_save.size();
	memcpy(o, &p.mp3->r, sizeof(Lc82310::Regs));
	o += sizeof(Lc82310::Regs);
	memcpy(o, p.ram, 0x800000);
	o += 0x800000;
	memcpy(o, &p.ram_bank, sizeof(p) - offsetof(decltype(p), ram_bank));
}

void s10_memp3_state_load(const void *src, uint32_t now)
{
	if (!p.cpu)
		return;
	const uint8_t *i = (const uint8_t *)src;
	p.cpu->m_save.load(i);
	i += p.cpu->m_save.size();
	p.mp3->host.m_save.load(i);
	i += p.mp3->host.m_save.size();
	memcpy(&p.mp3->r, i, sizeof(Lc82310::Regs));
	i += sizeof(Lc82310::Regs);
	memcpy(p.ram, i, 0x800000);
	i += 0x800000;
	memcpy(&p.ram_bank, i, sizeof(p) - offsetof(decltype(p), ram_bank));
	p.synced_cycle = now;
	p.cpu->drop_code(); // (its RAM came back from the state: decoded blocks may be stale)
}
