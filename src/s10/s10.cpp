// ReSys10: Namco System 10 board (main CPU side): MEM(M) flash banking, MEM(N) NAND controller and PIO DMA, the
// ns10crypt decrypters, I2C, the I/O update interrupt. Behaviour follows MAME src/mame/namco/namcos10.cpp,
// ns10crypt.cpp, src/devices/machine/nandflash.cpp and intelfsh.cpp (BSD-3-Clause, smf, windyfairy, Andreas Naive,
// R. Belmont and contributors); the per-game tables are generated from namcos10.cpp (tools/gen_s10_games.py).
#include "s10.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

extern "C" {
#include "psxcommon.h"
#include "psxhw.h"
#include "psxmem.h"
#include "psxevents.h"
#include "r3000a.h"
#include "rs12_zip.h"
}
#include "s10_bus.h"
#include "s10_atapi.h"
#include "s10_exio.h"
#include "s10_memp3.h"

typedef uint64_t u64;

// ---- MAME helpers ----
// bitswap<N>(v, b(N-1), ..., b0): result bit N-1-k = v bit b(k) (a bit index >= the width of v reads 0)
template <int N, typename T, typename... B> static constexpr T bitswap(T v, B... b)
{
	const int idx[] = { int(b)... };
	static_assert(sizeof...(B) == N, "bitswap: one index per bit");
	uint64_t r = 0;
	for (int k = 0; k < N; k++)
		r = (r << 1) | (idx[k] < 64 ? (uint64_t(v) >> idx[k]) & 1 : 0);
	return T(r);
}
static int gf2_reduce(uint64_t num) { return std::popcount(num) & 1; }

// ---- per-game decrypter logic (ns10crypt.h ns10_type2_decrypter_device::ns10_crypto_logic) ----
struct ns10_logic {
	uint64_t eMask[16]{};
	uint64_t dMask[16]{};
	uint16_t xMask = 0;
	uint16_t (*nonlinear_calculation)(uint64_t, uint64_t) = nullptr;
	uint64_t (*iv_calculation)(int) = nullptr;
};
struct ns10_logic_nl { // ns10_type2_decrypter_nonlinear_device::ns10_crypto_logic
	uint64_t eMask[16]{};
	uint64_t dMask[16]{};
	uint16_t xMask = 0;
	uint16_t (*nonlinear_calculation)(uint16_t) = nullptr;
	uint64_t (*iv_calculation)(int) = nullptr;
};

struct S10Config {
	int board, io, nand_type, nand_count;
	uint16_t (*unscramble)(uint16_t);
	int dec;
	const void *logic; // ns10_logic or ns10_logic_nl
};

#include "s10_games.inc"

// ---- decrypters (ns10crypt.cpp) ----
static const int k_type1_sbox[16] = { 16, 16, 16, 0, 4, 9, 16, 16, 16, 8, 16, 1, 16, 9, 16, 5 };
static const int k_type2_sbox[16] = { 0, 12, 13, 6, 2, 4, 9, 8, 11, 1, 7, 15, 10, 5, 14, 3 };

struct Decrypter {
	int type = S10_DEC_NONE;
	const void *logic = nullptr;
	const uint8_t *nl_table = nullptr; // nonlinear bit table (ptblank3)
	uint32_t nl_bytes = 0;
	// state
	bool active = false;
	uint16_t mask = 0;
	uint8_t counter = 0; // type 1
	uint64_t prev_c = 0, prev_p = 0;
	uint32_t nl_count = 0;

	bool found() const { return type != S10_DEC_NONE; }
	void reset() { active = false; mask = 0; counter = 0; prev_c = prev_p = 0; nl_count = 0; }

	void activate(int iv)
	{
		if (type == S10_DEC_TYPE1) {
			mask = uint16_t(k_type1_sbox[iv]);
			counter = 0;
		} else {
			uint64_t (*ivf)(int) = type == S10_DEC_TYPE2 ? ((const ns10_logic *)logic)->iv_calculation
				: ((const ns10_logic_nl *)logic)->iv_calculation;
			prev_c = ivf ? ivf(iv) : bitswap<16>(uint64_t(k_type2_sbox[iv]), 3, 16, 16, 2, 1, 16, 16, 0, 16, 16, 16, 16, 16, 16, 16, 16);
			prev_p = 0;
			mask = 0;
		}
		active = true;
	}

	uint16_t decrypt(uint16_t cipherword)
	{
		if (type == S10_DEC_TYPE1) {
			const uint16_t plainword = mask ^ bitswap<16>(cipherword, 9, 13, 15, 7, 14, 8, 6, 10, 11, 12, 3, 5, 0, 1, 4, 2);
			auto B = [] (unsigned v, int b) { return (v >> b) & 1u; };
			const unsigned c = cipherword, n = counter;
			const uint16_t nbs = uint16_t(
				(B(n, 4) << 15) ^ ((B(c, 2) ^ B(c, 5)) << 14) ^ (B(c, 0) << 13) ^ ((B(c, 4) | B(c, 5)) << 12) ^
				(B(n, 0) << 11) ^ (B(c, 6) << 10) ^ ((B(c, 4) & B(n, 1)) << 8) ^ (B(n, 3) << 6) ^
				((B(c, 3) | B(n, 7)) << 5) ^ ((B(c, 2) ^ B(n, 3)) << 4) ^ (B(n, 2) << 3) ^ ((B(c, 7) & B(n, 7)) << 2) ^
				(B(n, 5) << 1) ^ (B(c, 7) | B(n, 1)));
			mask = uint16_t(nbs ^ bitswap<16>(cipherword, 6, 11, 3, 1, 13, 5, 15, 10, 2, 9, 8, 4, 0, 12, 7, 14) ^
				bitswap<16>(plainword, 9, 7, 5, 2, 14, 4, 13, 8, 0, 15, 10, 1, 3, 6, 12, 11) ^ 0xecbe);
			++counter;
			return plainword;
		}
		const uint16_t plainword = cipherword ^ mask;
		prev_c = (prev_c << 16) ^ cipherword;
		prev_p = (prev_p << 16) ^ plainword;
		const uint64_t *e = type == S10_DEC_TYPE2 ? ((const ns10_logic *)logic)->eMask : ((const ns10_logic_nl *)logic)->eMask;
		const uint64_t *d = type == S10_DEC_TYPE2 ? ((const ns10_logic *)logic)->dMask : ((const ns10_logic_nl *)logic)->dMask;
		mask = 0;
		for (int j = 15; j >= 0; --j) {
			mask <<= 1;
			mask ^= gf2_reduce(e[j] & prev_c);
			mask ^= gf2_reduce(d[j] & prev_p);
		}
		if (type == S10_DEC_TYPE2) {
			const ns10_logic *l = (const ns10_logic *)logic;
			mask ^= l->xMask;
			mask ^= l->nonlinear_calculation(prev_c, prev_p);
		} else {
			const ns10_logic_nl *l = (const ns10_logic_nl *)logic;
			mask ^= l->xMask;
			uint16_t bit = 0;
			if (nl_table && nl_bytes) {
				bit = (nl_table[nl_count / 8] >> (7 - nl_count % 8)) & 1;
				if (++nl_count >= nl_bytes * 8)
					nl_count = 0;
			}
			mask ^= l->nonlinear_calculation(bit);
		}
		return plainword;
	}
};

// ---- NAND flash (nandflash.cpp nand_device: K9F2808U0B, K9F5608U0D, TC58256AFT; 512 + 16 byte pages, 32 a block) ----
enum { SM_M_INIT, SM_M_READ, SM_M_PROGRAM, SM_M_ERASE, SM_M_READSTATUS, SM_M_READID, SM_M_30, SM_M_RANDOM_DATA_INPUT, SM_M_RANDOM_DATA_OUTPUT };
enum { SM_PM_A, SM_PM_B, SM_PM_C };

struct NandRegs { // (saved in states)
	uint8_t mode, pointer_mode, status, accumulated_status, mode_3065;
	uint32_t page_addr, byte_addr, addr_load_ptr, program_byte_count;
	uint8_t pagereg[528];
};

struct Nand {
	static const uint32_t page_data_size = 512, page_total_size = 528, log2_pages_per_block = 5;
	uint32_t num_pages = 0;
	uint8_t id[2] = { 0, 0 };
	uint8_t *data = nullptr; // num_pages * 528, the chip's contents (the ROM file)
	NandRegs r{};

	void init(int type)
	{
		num_pages = type == S10_K9F2808U0B ? 32 * 1024 : 32 * 2048;
		id[0] = type == S10_TC58256AFT ? 0x98 : 0xec;
		id[1] = type == S10_K9F2808U0B ? 0x73 : 0x75;
	}
	uint32_t size() const { return num_pages * page_total_size; }
	void reset()
	{
		memset(&r, 0, sizeof(r));
		r.mode = SM_M_INIT;
		r.pointer_mode = SM_PM_A;
		r.status = 0xc0;
	}

	void command_w(uint8_t d)
	{
		switch (d) {
		case 0xff: // reset
			r.mode = SM_M_INIT;
			r.pointer_mode = SM_PM_A;
			r.status = (r.status & 0x80) | 0x40;
			r.accumulated_status = 0;
			r.mode_3065 = 0;
			break;
		case 0x00:
			r.mode = SM_M_READ;
			r.pointer_mode = SM_PM_A;
			r.addr_load_ptr = 0;
			break;
		case 0x01:
			r.mode = SM_M_READ;
			r.pointer_mode = SM_PM_B;
			r.addr_load_ptr = 0;
			break;
		case 0x50:
			r.mode = SM_M_READ;
			r.pointer_mode = SM_PM_C;
			r.addr_load_ptr = 0;
			break;
		case 0x80: // page program
			r.mode = SM_M_PROGRAM;
			r.addr_load_ptr = 0;
			r.program_byte_count = 0;
			memset(r.pagereg, 0xff, sizeof(r.pagereg));
			break;
		case 0x10:
		case 0x15:
			if (r.mode != SM_M_PROGRAM && r.mode != SM_M_RANDOM_DATA_INPUT) {
				r.mode = SM_M_INIT;
				break;
			}
			r.status = (r.status & 0x80) | r.accumulated_status;
			if (r.page_addr < num_pages)
				for (uint32_t i = 0; i < page_total_size; i++)
					data[r.page_addr * page_total_size + i] &= r.pagereg[i];
			r.status |= 0x40;
			r.accumulated_status = d == 0x15 ? r.status & 0x1f : 0;
			r.mode = SM_M_INIT;
			break;
		case 0x60: // block erase
			r.mode = SM_M_ERASE;
			r.page_addr = 0;
			r.addr_load_ptr = 0;
			break;
		case 0xd0:
			if (r.mode != SM_M_ERASE) {
				r.mode = SM_M_INIT;
				break;
			}
			r.status &= 0x80;
			{
				const uint32_t page = r.page_addr & (~0u << log2_pages_per_block);
				if (page < num_pages)
					memset(data + page * page_total_size, 0xff, (1u << log2_pages_per_block) * page_total_size);
			}
			r.status |= 0x40;
			r.mode = SM_M_INIT;
			if (r.pointer_mode == SM_PM_B)
				r.pointer_mode = SM_PM_A;
			break;
		case 0x70:
			r.mode = SM_M_READSTATUS;
			break;
		case 0x90:
			r.mode = SM_M_READID;
			r.addr_load_ptr = 0;
			break;
		case 0x30:
			r.mode = SM_M_30; // (1 column address cycle)
			break;
		case 0x65:
			if (r.mode != SM_M_30)
				r.mode = SM_M_INIT;
			else
				r.mode_3065 = 1;
			break;
		case 0x05:
			if (r.mode != SM_M_READ && r.mode != SM_M_RANDOM_DATA_OUTPUT)
				r.mode = SM_M_INIT;
			else {
				r.mode = SM_M_RANDOM_DATA_OUTPUT;
				r.addr_load_ptr = 0;
			}
			break;
		case 0xe0:
			if (r.mode != SM_M_RANDOM_DATA_OUTPUT)
				r.mode = SM_M_INIT;
			break;
		case 0x85:
			if (r.mode != SM_M_PROGRAM && r.mode != SM_M_RANDOM_DATA_INPUT)
				r.mode = SM_M_INIT;
			else {
				r.mode = SM_M_RANDOM_DATA_INPUT;
				r.addr_load_ptr = 0;
				r.program_byte_count = 0;
			}
			break;
		default:
			r.mode = SM_M_INIT;
			break;
		}
	}

	void address_w(uint8_t d) // 1 column + 2 row address cycles
	{
		switch (r.mode) {
		case SM_M_READ:
		case SM_M_PROGRAM:
			if (r.addr_load_ptr == 0) {
				r.page_addr = 0;
				switch (r.pointer_mode) {
				case SM_PM_A: r.byte_addr = d; break;
				case SM_PM_B: r.byte_addr = d + 256; r.pointer_mode = SM_PM_A; break;
				case SM_PM_C: r.byte_addr = (d & 0x0f) + (r.mode_3065 ? 256 : page_data_size); break;
				}
			} else if (r.addr_load_ptr < 3) {
				const int sh = (r.addr_load_ptr - 1) * 8;
				r.page_addr = (r.page_addr & ~(0xffu << sh)) | (uint32_t(d) << sh);
			}
			r.addr_load_ptr++;
			break;
		case SM_M_ERASE:
			if (r.addr_load_ptr < 2) {
				const int sh = r.addr_load_ptr * 8;
				r.page_addr = (r.page_addr & ~(0xffu << sh)) | (uint32_t(d) << sh);
			}
			r.addr_load_ptr++;
			break;
		case SM_M_RANDOM_DATA_INPUT:
		case SM_M_RANDOM_DATA_OUTPUT:
			if (r.addr_load_ptr < 1)
				r.byte_addr = (r.byte_addr & ~0xffu) | d;
			r.addr_load_ptr++;
			break;
		case SM_M_READID:
			if (r.addr_load_ptr == 0)
				r.byte_addr = d;
			r.addr_load_ptr++;
			break;
		default:
			break;
		}
	}

	uint8_t data_r()
	{
		uint8_t reply = 0;
		switch (r.mode) {
		case SM_M_READ:
		case SM_M_RANDOM_DATA_OUTPUT:
			if (!r.mode_3065)
				reply = r.byte_addr < page_total_size && r.page_addr < num_pages ? data[r.page_addr * page_total_size + r.byte_addr] : 0xff;
			else
				reply = 0xff;
			r.byte_addr++; // (no sequential row read on these chips)
			break;
		case SM_M_READSTATUS:
			reply = r.status & 0xc1;
			break;
		case SM_M_READID:
			reply = r.byte_addr < 2 ? id[r.byte_addr] : 0;
			r.byte_addr++;
			break;
		default:
			break;
		}
		return reply;
	}

	void data_w(uint8_t d)
	{
		if (r.mode != SM_M_PROGRAM && r.mode != SM_M_RANDOM_DATA_INPUT)
			return;
		if (r.program_byte_count++ < page_total_size && r.byte_addr < page_total_size)
			r.pagereg[r.byte_addr] = d;
		r.byte_addr++;
		if (r.byte_addr == page_total_size)
			r.byte_addr = r.pointer_mode != SM_PM_C ? 0 : page_data_size;
	}
};

// ---- MEM(M) program flash (intelfsh.cpp intelfsh16 INTEL_28F640J5, Intel command set) ----
enum { FM_NORMAL, FM_READID, FM_READSTATUS, FM_WRITEPART1, FM_CLEARPART1, FM_SETMASTER, FM_WRITEBUFFER1, FM_WRITEBUFFER2 };

struct FlashRegs {
	uint8_t mode, status, master_lock;
	uint32_t wb_count, wb_start, wb_n;
	uint16_t wb[16];
};

struct Flash {
	uint16_t *data = nullptr; // 8MB as little-endian words (the ROM file)
	uint32_t words = 0;
	FlashRegs r{};

	void reset() { memset(&r, 0, sizeof(r)); r.status = 0x80; }

	uint16_t read(uint32_t a)
	{
		switch (r.mode) {
		case FM_READSTATUS:
		case FM_WRITEBUFFER2:
			return r.status;
		case FM_WRITEBUFFER1:
			return 0x80;
		case FM_READID:
			switch (a & 0xff) {
			case 0: return 0x89; // Intel
			case 1: return 0x15; // 28F640J5
			case 3: return r.master_lock;
			default: return 0;
			}
		default:
			return a < words ? data[a] : 0xffff;
		}
	}

	void write(uint32_t a, uint16_t d)
	{
		switch (r.mode) {
		case FM_NORMAL:
		case FM_READSTATUS:
		case FM_READID:
			switch (d & 0xff) {
			case 0xf0: case 0xff: r.mode = FM_NORMAL; break;
			case 0x90: r.mode = FM_READID; break;
			case 0x40: case 0x10: r.mode = FM_WRITEPART1; break;
			case 0x50: r.status = 0x80; r.mode = FM_READSTATUS; break;
			case 0x20: r.mode = FM_CLEARPART1; break;
			case 0x60: r.mode = FM_SETMASTER; break;
			case 0x70: r.mode = FM_READSTATUS; break;
			case 0xe8: r.mode = FM_WRITEBUFFER1; break;
			default: break;
			}
			break;
		case FM_WRITEPART1:
			if (a < words)
				data[a] &= d;
			r.status = 0x80;
			r.mode = FM_READSTATUS;
			break;
		case FM_CLEARPART1:
			if ((d & 0xff) == 0xd0) {
				// MAME: the 64KB block of the address (its erase timer's end status at once)
				const uint32_t base = (a * 2) & ~0xffffu;
				if (base / 2 < words)
					memset(data + base / 2, 0xff, 0x10000);
				r.status = 0x80;
				r.mode = FM_READSTATUS;
			}
			break;
		case FM_SETMASTER:
			if ((d & 0xff) == 0xf1) r.master_lock = 1;
			if ((d & 0xff) == 0xd0) r.master_lock = 0;
			r.mode = FM_READSTATUS;
			break;
		case FM_WRITEBUFFER1:
			r.wb_count = (d < 0xf ? d : 0xf) + 1u;
			r.status = 0x80;
			r.mode = FM_WRITEBUFFER2;
			r.wb_n = 0;
			break;
		case FM_WRITEBUFFER2:
			if (r.wb_n < r.wb_count) {
				if (r.wb_n == 0)
					r.wb_start = a;
				if (a >= r.wb_start + r.wb_count) {
					r.status = (1 << 4) | (1 << 5);
					r.mode = FM_READSTATUS;
				} else
					r.wb[r.wb_n++] = d;
			} else {
				if ((d & 0xff) == 0xd0)
					for (uint32_t i = 0; i < r.wb_count; i++)
						if (r.wb_start + i < words)
							data[r.wb_start + i] &= r.wb[i];
				r.status = 0x80;
				r.mode = FM_READSTATUS;
			}
			break;
		}
	}
};

// ---- the board ----
struct BoardRegs { // (saved in states)
	uint16_t i2c_host_clock, i2c_host_data, i2c_dev_clock, i2c_dev_data, i2c_prev_clock, i2c_prev_data;
	int8_t i2cp_mode;
	uint8_t i2c_byte;
	int32_t i2c_bit;
	int32_t exio_ident_bit;
	uint32_t exio_ident_byte;
	uint16_t irq; // MAME m_int
	// MEM(M)
	uint32_t bank_idx, bank_access_nand_direct;
	// MEM(N)
	uint16_t data_xor;
	uint32_t ctrl_reg, nand_device_idx, nand_address;
	// MEM(P3)
};

static struct {
	const S10Game *game;
	const S10Config *cfg;
	uint8_t *rgn[S10_RGN_COUNT];
	uint32_t rgn_size[S10_RGN_COUNT];
	uint8_t bios[0x400000];
	uint8_t share3[0x100000]; // 0x1f500000: the NAND block table (MEM(N))
	Nand nand[16];
	Flash flash;
	Decrypter dec;
	BoardRegs r;
	uint32_t unmapped[64];
	int n_unmapped;
} b;

uint32_t s10_in1;
uint32_t s10_exio_in1;
uint16_t s10_dipsw;
uint32_t s10_gun[4]; // EXIO light guns: P1 X, P1 Y, P2 X, P2 Y (MAME EXIO_LIGHTGUN1-4)

static bool is_set(const char *name) { return b.game && (!strcmp(b.game->name, name) || !strcmp(b.game->parent, name)); }

extern "C" int s10_has_gun(void)
{
	return is_set("ptblank3") ? S10_GUN_EXIO : is_set("g13jnr") ? S10_GUN_P3 : S10_GUN_NONE;
}

// IN1 bits wired active high (MAME IP_ACTIVE_HIGH): g13jnr's scope sensor
static uint32_t in1_active_high() { return is_set("g13jnr") ? S10_G13_SCOPE : 0; }

extern "C" void s10_set_gun(int pl, float fx, float fy)
{
	if (pl < 0 || pl > 1)
		return;
	fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
	fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
	if (s10_has_gun() == S10_GUN_P3) { // P3_ANALOG1 = Y (PORT_REVERSE), P3_ANALOG2 = X, 10 bits
		if (pl == 0) {
			s10_p3_analog[0] = uint16_t((1.0f - fy) * 0x3ff + 0.5f);
			s10_p3_analog[1] = uint16_t(fx * 0x3ff + 0.5f);
		}
		return;
	}
	s10_gun[pl * 2] = uint32_t(fx * 0x13f + 0.5f);     // MAME PORT_MINMAX(0x000, 0x13f)
	s10_gun[pl * 2 + 1] = uint32_t(fy * 0xef + 0.5f);  // PORT_MINMAX(0x000, 0xef)
}

extern "C" int s10_game_count(void) { return int(sizeof(k_s10_games) / sizeof(k_s10_games[0])); }
extern "C" const S10Game *s10_game(int i) { return i >= 0 && i < s10_game_count() ? &k_s10_games[i] : nullptr; }
extern "C" int s10_find_game(const char *name)
{
	for (int i = 0; i < s10_game_count(); i++)
		if (!strcmp(k_s10_games[i].name, name))
			return i;
	return -1;
}
extern "C" int s10_board_kind(const S10Game *g) { return k_s10_configs[g->cfg].board; }
extern "C" int s10_io_kind(const S10Game *g) { return k_s10_configs[g->cfg].io; }

static void note_unmapped(uint32_t a, int write)
{
	const uint32_t key = (a & ~1u) | (uint32_t)write;
	for (int i = 0; i < b.n_unmapped; i++)
		if (b.unmapped[i] == key)
			return;
	if (b.n_unmapped < 64)
		b.unmapped[b.n_unmapped++] = key;
	if (getenv("RS10_UNMAPPED"))
		fprintf(stderr, "s10: unmapped %s %08x pc %08x\n", write ? "write" : "read", a, psxRegs.pc);
}

extern "C" void s10_dump_unmapped(void)
{
	for (int i = 0; i < b.n_unmapped; i++)
		fprintf(stderr, "s10: unmapped %s %08x\n", (b.unmapped[i] & 1) ? "write" : "read", b.unmapped[i] & ~1u);
}

// ---- loading ----
extern "C" void s10_unload(void)
{
	for (int i = 0; i < S10_RGN_COUNT; i++)
		free(b.rgn[i]);
	s10_memp3_attach(0);
	s10_exio_attach(0);
	s10_atapi_close();
	for (int i = 0; i < 16; i++)
		free(b.nand[i].data);
	memset((void *)&b, 0, sizeof(b));
}

extern "C" int s10_load(const char *dir, const char *name, char *msg, size_t msglen)
{
	const int gi = s10_find_game(name);
	if (gi < 0) {
		snprintf(msg, msglen, "unknown set %s", name);
		return -1;
	}
	s10_unload();
	b.game = &k_s10_games[gi];
	b.cfg = &k_s10_configs[b.game->cfg];
	// region sizes from the files
	for (const S10File *f = b.game->files; f->name; f++)
		if (f->offset + f->size > b.rgn_size[f->rgn])
			b.rgn_size[f->rgn] = f->offset + f->size;
	for (int i = 0; i < S10_RGN_COUNT; i++)
		if (b.rgn_size[i] && !(b.rgn[i] = (uint8_t *)calloc(1, b.rgn_size[i]))) {
			snprintf(msg, msglen, "out of memory");
			return -1;
		}
	char zips[2][1100];
	snprintf(zips[0], sizeof(zips[0]), "%s/%s.zip", dir, name);
	snprintf(zips[1], sizeof(zips[1]), "%s/%s.zip", dir, b.game->parent);
	int nfiles = 0;
	for (const S10File *f = b.game->files; f->name; f++) {
		u8 *data = nullptr;
		u32 size = 0;
		for (int z = 0; z < 2 && !data; z++)
			if (z == 0 || b.game->parent[0])
				data = rs12_zip_load(zips[z], f->name, f->crc, &size);
		if (!data) {
			snprintf(msg, msglen, "%s: %s (crc %08x) not found in %s%s%s", name, f->name, f->crc, zips[0],
				b.game->parent[0] ? " or " : "", b.game->parent[0] ? zips[1] : "");
			return -1;
		}
		memcpy(b.rgn[f->rgn] + f->offset, data, size < f->size ? size : f->size);
		free(data);
		nfiles++;
	}
	// NAND chips: the ROM file is the chip's contents (MAME nvram_default from the region)
	const S10Config *c = b.cfg;
	for (int i = 0; i < c->nand_count; i++) {
		Nand &n = b.nand[i];
		n.init(c->nand_type);
		if (!(n.data = (uint8_t *)malloc(n.size()))) {
			snprintf(msg, msglen, "out of memory");
			return -1;
		}
		memset(n.data, 0xff, n.size());
		const uint32_t have = b.rgn_size[S10_RGN_NAND0 + i];
		if (have)
			memcpy(n.data, b.rgn[S10_RGN_NAND0 + i], have < n.size() ? have : n.size());
	}
	for (int i = 0; i < 16; i++) { // (the chips hold the data now)
		free(b.rgn[S10_RGN_NAND0 + i]);
		b.rgn[S10_RGN_NAND0 + i] = nullptr;
	}
	if (c->board == S10_MEMM) {
		b.flash.data = (uint16_t *)b.rgn[S10_RGN_NAND];
		b.flash.words = b.rgn_size[S10_RGN_NAND] / 2;
	}
	s10_memp3_attach(c->board == S10_MEMP3);
	s10_exio_attach(c->io == S10_IO_EXIO);
	if (b.game->disk[0]) { // the CD-ROM (MAME: the set's folder in the ROM path), <content dir>/<set>/<disk>.chd
		char chd[1200];
		snprintf(chd, sizeof(chd), "%s/%s/%s.chd", dir, name, b.game->disk);
		if (s10_atapi_open(chd) != 0) {
			snprintf(msg, msglen, "%s: the CD-ROM image %s is missing or unreadable", name, chd);
			return -1;
		}
	}
	b.dec.type = c->dec;
	b.dec.logic = c->logic;
	b.dec.nl_table = b.rgn[S10_RGN_NONLINEAR];
	b.dec.nl_bytes = b.rgn_size[S10_RGN_NONLINEAR];

	// the boot program: MAME memm_driver_init / init_mrdrilr2, memn_driver_init (the "maincpu:rom" region, 0x55 filled)
	memset(b.bios, 0x55, sizeof(b.bios));
	uint16_t *bios = (uint16_t *)b.bios;
	if (c->board == S10_MEMM) {
		const uint32_t n = b.rgn_size[S10_RGN_NAND] < 0x62000 ? b.rgn_size[S10_RGN_NAND] : 0x62000;
		memcpy(b.bios, b.rgn[S10_RGN_NAND], n);
		for (uint32_t i = 0; i < n / 2; i++)
			bios[i] = c->unscramble(bios[i] ^ 0xaaaa);
	} else {
		const uint16_t xr = c->board == S10_MEMIO ? 0x5300 : 0xaaaa;
		auto nand_copy = [&] (uint16_t *dst, uint32_t start_page, uint32_t len) {
			for (uint32_t page = start_page; page < start_page + len; page++) {
				const uint8_t *p = b.nand[0].data + page * 0x210;
				for (int i = 0; i < 0x200; i += 2)
					*dst++ = c->unscramble(uint16_t((p[i + 1] | (p[i] << 8)) ^ xr));
			}
		};
		if (b.nand[0].data) {
			nand_copy(bios, 0x40, 0xe0);
			nand_copy(bios + 0x10000, 0x120, 0x1f00);
		}
	}
	snprintf(msg, msglen, "%d files, %s board, %d NAND", nfiles,
		c->board == S10_MEMM ? "MEM(M)" : c->board == S10_MEMN ? "MEM(N)" : c->board == S10_MEMP3 ? "MEM(P3)" : "MEM(IO)",
		c->nand_count);
	return 0;
}

extern "C" void *s10_nvram(size_t *size)
{
	if (!b.cfg) {
		*size = 0;
		return nullptr;
	}
	if (b.cfg->board == S10_MEMM) { // MAME: settings at 0x40000 of the flash (one 64KB erase block)
		*size = b.rgn[S10_RGN_NAND] && b.rgn_size[S10_RGN_NAND] >= 0x50000 ? 0x10000 : 0;
		return *size ? b.rgn[S10_RGN_NAND] + 0x40000 : nullptr;
	}
	// NAND 0 block 1 (MAME: 0x4200), the only block the games write
	*size = b.nand[0].data ? 32 * 528 : 0;
	return *size ? b.nand[0].data + 32 * 528 : nullptr;
}

// ---- reset ----
extern "C" void s10_mem_reset(void)
{
	memcpy(psxRegs.ptrs.psxR, b.bios, PSX_ROM_SIZE < sizeof(b.bios) ? PSX_ROM_SIZE : sizeof(b.bios));
	// the NAND block table RAM reads straight from memory (writes take the board path); MEM(M) has its flash window there
	if (b.cfg && b.cfg->board != S10_MEMM)
		psxMemMapRead(0x1f500000u, 0x100000u, b.share3);
	// MAME installs the EXIO map over the top of the boot ROM window (m_psx_remapper)
	if (b.cfg && b.cfg->io == S10_IO_EXIO)
		psxMemMapRead(0x1fe00000u, 0x200000u, nullptr);
}

// ---- EXIO window (MAME namcos10_map_exio): the board (s10_exio.cpp), and the light gun inputs the main CPU reads ----
static uint16_t exio_r16(uint32_t a)
{
	const uint32_t o = a - 0x1fe00000u;
	if (o >= 0x50000 && o < 0x60000 && (o & 0x7ffc) == 0) // EXIO_IN1 / EXIO_IN2 (unused)
		return o == 0x50000 ? uint16_t(~s10_exio_in1) : 0xffff;
	if (o >= 0x80000 && o < 0xc0000) { // exio_lightgun_io_r<N>, N = 0..7 every 0x8000
		const uint32_t n = (o - 0x80000) >> 15, val = s10_gun[(n / 2) & 3];
		if ((o & 0x7fff) >= 4)
			return 0;
		return (n & 1) ? uint16_t(val >> 6) : uint16_t(val & 0x3f);
	}
	return s10_exio_r16(o, psxRegs.cycle);
}

extern "C" void s10_cpu_reset(void)
{
	memset(&b.r, 0, sizeof(b.r));
	BoardRegs &r = b.r;
	r.i2c_dev_clock = r.i2c_dev_data = 1;
	r.i2c_host_clock = r.i2c_host_data = 1;
	r.i2c_prev_clock = r.i2c_prev_data = 1;
	r.data_xor = b.cfg && b.cfg->board == S10_MEMIO ? 0x5300 : 0xaaaa;
	for (int i = 0; i < 16; i++)
		if (b.nand[i].data)
			b.nand[i].reset();
	b.flash.reset();
	b.dec.reset();
	s10_memp3_reset(psxRegs.cycle);
	memset(b.share3, 0, sizeof(b.share3));
	s10_exio_reset(psxRegs.cycle);
	s10_atapi_reset();
	set_event(PSXINT_S12, PSXCLK / 100); // MAME io_timer, 100 Hz
	// SIO1 (the boot program's debug serial port) idle as MAME's psxsio: TX ready + TX empty. pcsx_rearmed's PS1 value
	// (0x1a5) has the framing error bit set, and the boot program retries its reset of the port forever
	psxHu16ref(0x1054) = SWAPu16(0x0005);
}

// ---- the CD-ROM's INTRQ (MAME cdrom_interrupt): interrupt register bit 2, and the IRQ10 line ----
extern "C" void s10_atapi_irq(int state)
{
	if (state) {
		b.r.irq |= 4;
		psxHu32ref(0x1070) |= SWAPu32(0x400);
	} else
		b.r.irq &= ~4;
}

// ---- I/O update interrupt (MAME io_update_interrupt_callback -> psxirq intin10) ----
extern "C" void s10_event(void)
{
	b.r.irq |= 8;
	psxHu32ref(0x1070) |= SWAPu32(0x400);
	set_event(PSXINT_S12, PSXCLK / 100);
}

extern "C" void s10_vblank(int state)
{
	if (state) {
		s10_exio_sync(psxRegs.cycle); // (the sub CPUs keep up with the frames between accesses)
		s10_memp3_sync(psxRegs.cycle);
	}
}

// ---- I2C (namcos10_state::i2c_update: a receiver that acks every byte) ----
static void i2c_update()
{
	BoardRegs &r = b.r;
	uint16_t clock = r.i2c_dev_clock & r.i2c_host_clock & 1;
	uint16_t data = r.i2c_dev_data & r.i2c_host_data & 1;
	if (r.i2c_prev_data == data && r.i2c_prev_clock == clock)
		return;
	enum { IDLE, RECIEVE_BYTE, RECIEVE_ACK_1, RECIEVE_ACK_0 };
	switch (r.i2cp_mode) {
	case IDLE:
		if (clock && !data) {
			r.i2c_byte = 0;
			r.i2c_bit = 7;
			r.i2cp_mode = RECIEVE_BYTE;
		}
		break;
	case RECIEVE_BYTE:
		if (clock && data && !r.i2c_prev_data)
			r.i2cp_mode = IDLE;
		else if (clock && !r.i2c_prev_clock) {
			r.i2c_byte |= data << r.i2c_bit;
			r.i2c_bit--;
			if (r.i2c_bit < 0) {
				r.i2cp_mode = RECIEVE_ACK_1;
				r.i2c_dev_data = 0;
				data = 0;
			}
		}
		break;
	case RECIEVE_ACK_1:
		if (clock && !r.i2c_prev_clock)
			r.i2cp_mode = RECIEVE_ACK_0;
		break;
	case RECIEVE_ACK_0:
		if (!clock && r.i2c_prev_clock) {
			r.i2c_dev_data = 1;
			data = r.i2c_host_data & 1;
			r.i2c_byte = 0;
			r.i2c_bit = 7;
			r.i2cp_mode = RECIEVE_BYTE;
		}
		break;
	}
	r.i2c_prev_data = data;
	r.i2c_prev_clock = clock;
}

// ---- MEM(N) NAND controller ----
static uint16_t nand_data_r()
{
	BoardRegs &r = b.r;
	Nand &n = b.nand[r.nand_device_idx & 15];
	if (!n.data)
		return 0xffff;
	const uint16_t hi = n.data_r();
	const uint16_t data = uint16_t((hi << 8) | n.data_r());
	if (b.dec.found() && b.dec.active)
		return b.dec.decrypt(data);
	// blocks 0 (bad block table) and 1 (settings) of NAND 0 are plain
	if (r.nand_device_idx == 0 && ((r.nand_address >> 8) & 0xffff) >> 5 < 2)
		return data;
	return b.cfg->unscramble(data ^ r.data_xor);
}

static void crypto_switch_w(uint16_t data)
{
	if (!b.dec.found())
		return;
	if (data & 0x8000)
		b.dec.activate(data & 0xf);
	else
		b.dec.active = false;
}

// ---- MEM(M) flash window ----
static uint16_t memm_range_r(uint32_t offset)
{
	BoardRegs &r = b.r;
	if (r.bank_access_nand_direct == 0)
		return b.flash.read(0x100000 * r.bank_idx + offset);
	const uint16_t *bank = nullptr;
	uint32_t words = 0;
	if (r.bank_idx < 0x10) {
		bank = (const uint16_t *)b.rgn[S10_RGN_NAND];
		words = b.rgn_size[S10_RGN_NAND] / 2;
	} else {
		bank = (const uint16_t *)b.rgn[S10_RGN_DATA];
		words = b.rgn_size[S10_RGN_DATA] / 2;
	}
	const uint32_t i = 0x100000 * (r.bank_idx & 0xf) + offset;
	uint16_t data = bank && i < words ? bank[i] : 0xffff;
	if (b.dec.found() && b.dec.active)
		return b.dec.decrypt(data);
	if (r.bank_idx < 0x10)
		data = b.cfg->unscramble(data ^ 0xaaaa);
	return data;
}

// ---- bus: 16-bit board accesses (a = physical, even) ----
static uint16_t board_r16(uint32_t a)
{
	BoardRegs &r = b.r;
	const int board = b.cfg ? b.cfg->board : S10_MEMN;
	if (a >= 0x1fe00000u && b.cfg && b.cfg->io == S10_IO_EXIO)
		return exio_r16(a);
	if (board == S10_MEMM && a >= 0x1f400000u && a < 0x1f600000u)
		return memm_range_r((a - 0x1f400000u) >> 1);
	if (board == S10_MEMM && a >= 0x1fb40000u && a < 0x1fb60000u) { // bank_r
		r.bank_access_nand_direct = 1;
		return 0;
	}
	if (board != S10_MEMM) {
		if (a >= 0x1f500000u && a < 0x1f600000u)
			return *(uint16_t *)(b.share3 + (a - 0x1f500000u));
		switch (a) {
		case 0x1f400000u: return 0;                 // nand_rnb_r (ready: no busy time on these chips)
		case 0x1f450000u: return nand_data_r();
		case 0x1f470000u: return uint16_t(r.ctrl_reg);
		}
		if (board == S10_MEMP3 && a >= 0x1f300000u && a < 0x1f340000u) { // (s10_memp3.cpp)
			uint16_t v = 0;
			if (s10_memp3_r16(a - 0x1f300000u, psxRegs.cycle, &v))
				return v;
		}
	}
	switch (a) {
	case 0x1fb60000u: case 0x1fb60002u: return 0; // watchdog
	case 0x1fba0000u: return uint16_t(~s10_dipsw);  // SYSTEM (SW1, active low)
	case 0x1fba0002u: { // exio_ident_r: 0x30 = EXIO (CPU and I/O), 0xff = no I/O board
		const uint8_t resp = s10_exio_active() ? 0x30 : 0xff;
		const uint16_t res = r.exio_ident_byte >= 0x20 ? 3 : (((resp >> r.exio_ident_bit) & 1) ? 3 : 0);
		if (--r.exio_ident_bit == -1) {
			r.exio_ident_bit = 7;
			r.exio_ident_byte++;
		}
		return res;
	}
	case 0x1fba0004u: return uint16_t(~(s10_in1 ^ in1_active_high()));
	case 0x1fba0006u: return uint16_t(~(s10_in1 ^ in1_active_high()) >> 16);
	case 0x1fba0008u: return r.i2c_dev_clock & r.i2c_host_clock & 1;
	case 0x1fba000au: return r.i2c_dev_data & r.i2c_host_data & 1;
	case 0x1fba001au: return r.irq;
	case 0x1fbf0000u: return 1; // cdrom_unk_r: drive connected?
	}
	if (a >= 0x1fbc0000u && a < 0x1fbc0010u) // ATA (MAME cs0_r): the Taiko sets' CD-ROM drive
		return s10_atapi_present() ? s10_atapi_r(int(a - 0x1fbc0000u) >> 1) : 0;
	note_unmapped(a, 0);
	return 0;
}

static void board_w16(uint32_t a, uint16_t d)
{
	BoardRegs &r = b.r;
	const int board = b.cfg ? b.cfg->board : S10_MEMN;
	if (a >= 0x1fe00000u && b.cfg && b.cfg->io == S10_IO_EXIO) {
		s10_exio_w16(a - 0x1fe00000u, d, psxRegs.cycle);
		return;
	}
	if (board == S10_MEMM) {
		if (a == 0x1f300000u) {
			crypto_switch_w(d);
			return;
		}
		if (a >= 0x1f400000u && a < 0x1f600000u) { // nand_w
			if (r.bank_access_nand_direct == 0)
				b.flash.write((a - 0x1f400000u) >> 1, d);
			return;
		}
		if (a >= 0x1fb40000u && a < 0x1fb60000u) { // bank_w
			r.bank_idx = (a - 0x1fb40000u) >> 1;
			r.bank_access_nand_direct = r.bank_idx >= 0x10;
			return;
		}
	} else {
		if (a >= 0x1f500000u && a < 0x1f600000u) {
			*(uint16_t *)(b.share3 + (a - 0x1f500000u)) = d;
			return;
		}
		Nand &n = b.nand[r.nand_device_idx & 15];
		if (board == S10_MEMP3 && a >= 0x1f300000u && a < 0x1f340000u && s10_memp3_w16(a - 0x1f300000u, d, psxRegs.cycle))
			return;
		if (a == 0x1f380000u || (a == 0x1f300000u && board != S10_MEMP3)) {
			crypto_switch_w(d);
			return;
		}
		switch (a) {
		case 0x1f410000u:
			if (n.data) n.command_w(uint8_t(d));
			return;
		case 0x1f420000u:
			if (n.data) n.address_w(uint8_t(d));
			r.nand_address = (r.nand_address & 0xffff00) | (d & 0xff);
			return;
		case 0x1f430000u:
			if (n.data) n.address_w(uint8_t(d));
			r.nand_address = (r.nand_address & 0xff00ff) | ((d & 0xff) << 8);
			return;
		case 0x1f440000u:
			if (n.data) n.address_w(uint8_t(d));
			r.nand_address = (r.nand_address & 0x00ffff) | ((d & 0xff) << 16);
			return;
		case 0x1f450000u:
			if (n.data) {
				n.data_w(uint8_t(d >> 8));
				n.data_w(uint8_t(d));
			}
			return;
		case 0x1f460000u:
			if ((r.ctrl_reg & 4) == 0)
				r.nand_device_idx = d;
			return;
		case 0x1f470000u: r.ctrl_reg = d; return;
		case 0x1f480000u: return;
		}
		if (board == S10_MEMIO && a == 0x1f468000u) { // nand_dataxor_w
			r.data_xor = uint16_t(((d & 0xff) << 8) | (d >> 8));
			return;
		}
		if (board == S10_MEMIO && a == 0x1f478000u) // punch pad solenoid
			return;
	}
	switch (a) {
	case 0x1fb20000u: case 0x1fb20002u: return;
	case 0x1fb60000u: case 0x1fb60002u: return; // watchdog
	case 0x1fba0002u: // exio_ident_w
		r.exio_ident_bit = 7;
		r.exio_ident_byte = 0;
		return;
	case 0x1fba0008u: r.i2c_host_clock = d; i2c_update(); return;
	case 0x1fba000au: r.i2c_host_data = d; i2c_update(); return;
	case 0x1fba0012u: case 0x1fba0014u: case 0x1fba0016u: case 0x1fba0018u: // int_ack_w
		r.irq &= uint16_t(~(1u << ((a - 0x1fba0012u) >> 1)));
		return;
	}
	if (a >= 0x1fbc0000u && a < 0x1fbc0010u) {
		if (s10_atapi_present())
			s10_atapi_w(int(a - 0x1fbc0000u) >> 1, d);
		return;
	}
	if (a >= 0x1fc00000u) // (boot ROM area)
		return;
	note_unmapped(a, 1);
}

// diagnostics: RS10_BUSLOG=n logs the first n board accesses (pc, address, value)
static long s_buslog = -1;
static inline void buslog(const char *rw, uint32_t a, uint32_t v)
{
	if (s_buslog < 0)
		s_buslog = getenv("RS10_BUSLOG") ? atol(getenv("RS10_BUSLOG")) : 0;
	if (s_buslog > 0 && (a < 0x1f500000u || a >= 0x1f600000u)) {
		s_buslog--;
		fprintf(stderr, "bus %s %08x %04x pc %08x\n", rw, a, v, psxRegs.pc);
	}
}

extern "C" uint16_t s10_read16(uint32_t phys) { const uint16_t v = board_r16(phys & ~1u); buslog("r16", phys, v); return v; }
extern "C" uint8_t s10_read8(uint32_t phys)
{
	const uint16_t v = board_r16(phys & ~1u);
	return uint8_t((phys & 1) ? v >> 8 : v);
}
extern "C" uint32_t s10_read32(uint32_t phys)
{
	phys &= ~3u;
	const uint32_t lo = board_r16(phys);
	const uint32_t v = lo | (uint32_t(board_r16(phys + 2)) << 16);
	buslog("r32", phys, v);
	return v;
}
extern "C" void s10_write16(uint32_t phys, uint32_t v) { buslog("w16", phys, v & 0xffff); board_w16(phys & ~1u, uint16_t(v)); }
extern "C" void s10_write8(uint32_t phys, uint32_t v)
{
	// a byte reaches the 16-bit devices in its lane; the NAND command / address registers take the low byte
	buslog("w8", phys, v & 0xff);
	board_w16(phys & ~1u, uint16_t((phys & 1) ? (v & 0xff) << 8 : (v & 0xff)));
}
extern "C" void s10_write32(uint32_t phys, uint32_t v)
{
	phys &= ~3u;
	buslog("w32", phys, v);
	board_w16(phys, uint16_t(v));
	board_w16(phys + 2, uint16_t(v >> 16));
}

// ---- PIO DMA (channel 5): MEM(N) NAND data (MAME pio_dma_read) ----
extern "C" void s10_dma5_done(void)
{
	HW_DMA5_CHCR &= SWAP32(~0x01000000u);
	DMA_INTERRUPT(5);
}

extern "C" void s10_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
	int32_t size;
	if (chcr & 0x200) {
		uint32_t ba = bcr >> 16;
		if (!ba)
			ba = 0x10000;
		size = int32_t((bcr & 0xffff) * ba);
	} else
		size = (bcr & 0xffff) ? int32_t(bcr & 0xffff) : 0x10000;
	if (s10_atapi_dmarq() && (chcr & 1) == 0) { // the CD-ROM's data block (MAME cdrom_dmarq, read_dma)
		const uint32_t addr = (madr & PSX_RAM_MASK) & ~3u;
		const int32_t left = int32_t((PSX_RAM_SIZE - addr) / 4);
		if (size > left)
			size = left;
		uint8_t *ram = psxRegs.ptrs.psxM + addr;
		for (int32_t i = 0; i < size * 2; i++) {
			const uint16_t v = s10_atapi_r(0);
			ram[i * 2] = uint8_t(v);
			ram[i * 2 + 1] = uint8_t(v >> 8);
		}
		psxCpu->Clear(addr, uint32_t(size));
	} else if (b.cfg && b.cfg->board != S10_MEMM && (chcr & 1) == 0) { // device to RAM
		const uint32_t addr = (madr & PSX_RAM_MASK) & ~3u;
		const int32_t left = int32_t((PSX_RAM_SIZE - addr) / 4);
		if (size > left)
			size = left;
		uint8_t *ram = psxRegs.ptrs.psxM + addr;
		for (int32_t i = 0; i < size * 2; i++) {
			const uint16_t v = nand_data_r();
			ram[i * 2] = uint8_t(v);
			ram[i * 2 + 1] = uint8_t(v >> 8);
		}
		psxCpu->Clear(addr, uint32_t(size));
	}
	s10_dma5_done();
}

// ---- save states ----
static uint8_t *nvram_area(size_t *n) { return (uint8_t *)s10_nvram(n); }

extern "C" size_t s10_state_size(void)
{
	size_t n = 0;
	nvram_area(&n);
	return sizeof(BoardRegs) + sizeof(b.share3) + 16 * sizeof(NandRegs) + sizeof(FlashRegs) + sizeof(Decrypter) + n +
		s10_exio_state_size() + s10_memp3_state_size() + s10_atapi_state_size();
}

extern "C" void s10_state_save(void *dst)
{
	uint8_t *p = (uint8_t *)dst;
	auto put = [&] (const void *s, size_t n) { memcpy(p, s, n); p += n; };
	put(&b.r, sizeof(b.r));
	put(b.share3, sizeof(b.share3));
	for (int i = 0; i < 16; i++)
		put(&b.nand[i].r, sizeof(NandRegs));
	put(&b.flash.r, sizeof(FlashRegs));
	put(&b.dec, sizeof(Decrypter)); // (pointers are the same while the set stays loaded)
	size_t n = 0;
	uint8_t *nv = nvram_area(&n);
	put(nv, n);
	s10_exio_state_save(p);
	p += s10_exio_state_size();
	s10_memp3_state_save(p);
	p += s10_memp3_state_size();
	s10_atapi_state_save(p);
}

extern "C" int s10_state_load(const void *src, size_t size)
{
	if (size != s10_state_size())
		return -1;
	const uint8_t *p = (const uint8_t *)src;
	auto get = [&] (void *d, size_t n) { memcpy(d, p, n); p += n; };
	get(&b.r, sizeof(b.r));
	get(b.share3, sizeof(b.share3));
	for (int i = 0; i < 16; i++)
		get(&b.nand[i].r, sizeof(NandRegs));
	get(&b.flash.r, sizeof(FlashRegs));
	Decrypter d;
	get(&d, sizeof(Decrypter));
	b.dec.active = d.active, b.dec.mask = d.mask, b.dec.counter = d.counter;
	b.dec.prev_c = d.prev_c, b.dec.prev_p = d.prev_p, b.dec.nl_count = d.nl_count;
	size_t n = 0;
	uint8_t *nv = nvram_area(&n);
	get(nv, n);
	s10_exio_state_load(p, psxRegs.cycle);
	p += s10_exio_state_size();
	s10_memp3_state_load(p, psxRegs.cycle);
	p += s10_memp3_state_size();
	s10_atapi_state_load(p);
	return 0;
}
