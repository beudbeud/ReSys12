// pcsx_rearmed libpcsxcore/lightrec/plugin.c, changed for ReSys12 (Namco System 12): the memory map (4MB RAM, 4MB
// program flash, the board's I/O and banked ROM window on the core's memory handlers), the interpreter's cycle
// accounting (16.16 cycles per instruction, the fraction kept in psxRegs.subCycle) and its instruction
// substitutions. Changes carry "ReSys12:" comments. GPL-2.0-or-later (as pcsx_rearmed).
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>

#include "lightrec.h"
#include "../cdrom.h"
#include "../gpu.h"
#include "../gte.h"
#include "../mdec.h"
#include "../psxdma.h"
#include "../psxhw.h"
#include "../psxmem.h"
#include "../r3000a.h"
#include "../psxinterpreter.h"
#include "../psxhle.h"
#include "../psxevents.h"

#include "mem.h"
#include "plugin.h"
#include "s12_hle.h" // ReSys12: game routines run natively
#include "s12.h"     // ReSys12: the board (the System 10 memory map)
#include "lightrec-private.h"

#if (defined(__arm__) || defined(__aarch64__)) && !defined(ALLOW_LIGHTREC_ON_ARM)
#error "Lightrec should not be used on ARM (please specify DYNAREC=ari64 to make)"
#endif

#define ARRAY_SIZE(x) (sizeof(x) ? sizeof(x) / sizeof((x)[0]) : 0)

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#	define LE32TOH(x)	__builtin_bswap32(x)
#	define HTOLE32(x)	__builtin_bswap32(x)
#	define LE16TOH(x)	__builtin_bswap16(x)
#	define HTOLE16(x)	__builtin_bswap16(x)
#else
#	define LE32TOH(x)	(x)
#	define HTOLE32(x)	(x)
#	define LE16TOH(x)	(x)
#	define HTOLE16(x)	(x)
#endif

#ifdef __GNUC__
#	define likely(x)       __builtin_expect(!!(x),1)
#	define unlikely(x)     __builtin_expect(!!(x),0)
#else
#	define likely(x)       (x)
#	define unlikely(x)     (x)
#endif

#ifndef LIGHTREC_PROG_NAME
#  ifdef __linux__
#    define LIGHTREC_PROG_NAME "/proc/self/exe"
#  else
#    define LIGHTREC_PROG_NAME "retroarch.exe"
#  endif
#endif

psxRegisters psxRegs;
Rcnt rcnts[4];

void* code_buffer;

static struct lightrec_state *lightrec_state;

static bool use_lightrec_interpreter;
static bool block_stepping;
//static bool use_pcsx_interpreter;
#define use_pcsx_interpreter 0
static bool ram_disabled;
static bool lightrec_debug, lightrec_very_debug;
static u32 lightrec_begin_cycles;

static u32 lightrec_hacks; // ReSys12: (pcsx_rearmed frontend/main.c) none: System 12 games keep their stack in the scratchpad

// ReSys12: decode-time instruction substitutions (new_dynarec.c rs12_subst_source): entries of address, expected
// word, expected previous word, replacement; set by the board code (s12.c)
const unsigned int *rs12_ndrc_subst;
int rs12_ndrc_subst_n;

u32 rs12_lightrec_subst(u32 pc, u32 code, u32 prev)
{
	if (rs12_hle_lz_head && pc == rs12_hle_lz_head) // a game routine run natively (s12_hle.c), as the interpreter
		return RS12_HLE_OP | RS12_HLE_CODE_LZ;
	for (int k = 0; k < rs12_ndrc_subst_n; k++) {
		const unsigned int *e = rs12_ndrc_subst + k * 4;
		if (e[0] == pc && e[1] == code && e[2] == prev)
			return e[3];
	}
	return code;
}

static void lightrec_plugin_apply_config();
extern void lightrec_code_inv(void *ptr, uint32_t len);

static char cache_buf[64 * 1024];

static void cop2_op(struct lightrec_state *state, u32 func)
{
	struct lightrec_registers *regs = lightrec_get_registers(state);
	/* This works because regs->cp2c comes right after regs->cp2d,
	 * so it can be cast to a pcsxCP2Regs pointer. */
	gteDispatch((psxCP2Regs *)regs->cp2d, func);
}

static bool has_interrupt(void)
{
	struct lightrec_registers *regs = lightrec_get_registers(lightrec_state);

	return ((psxHu32(0x1070) & psxHu32(0x1074)) &&
		(regs->cp0[12] & 0x401) == 0x401) ||
		(regs->cp0[12] & regs->cp0[13] & 0x0300);
}

// ReSys12: lightrec counts psxRegs.subCycleStep per instruction (16.16 cycles, the interpreter's addCycle) and
// psxRegs.subCycle keeps the fraction across both, so the cycle totals agree exactly. The count of a run fits
// 31 bits: 30000 cycles at most per run (the events are usually much closer)
static void lightrec_transition_to_pcsx(struct lightrec_state *state)
{
	u32 c = lightrec_current_cycle_count(state) + psxRegs.subCycle;
	psxRegs.cycle += c >> 16;
	psxRegs.subCycle = c & 0xffff;
	lightrec_reset_cycle_count(state, 0);
}

static u32 lightrec_cycles_for(s32 cycles_pcsx)
{
	if (cycles_pcsx > 30000)
		cycles_pcsx = 30000;
	return (u32)cycles_pcsx * 65536 - psxRegs.subCycle;
}

static void lightrec_transition_from_pcsx(struct lightrec_state *state)
{
	s32 cycles_left = psxRegs.next_interupt - psxRegs.cycle;

	if (block_stepping || cycles_left <= 0 || has_interrupt())
		lightrec_set_exit_flags(state, LIGHTREC_EXIT_CHECK_INTERRUPT);
	else {
		lightrec_set_target_cycle_count(state, lightrec_cycles_for(cycles_left));
	}
}

// ReSys12: everything outside RAM, scratchpad and the flash goes through the core's memory functions (psxmem.c):
// hardware registers, the board's I/O and the banked ROM window through its read LUT (the current bank), the
// flash's writes (ignored) and its tektagt protection register
static void mem_write_byte(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	lightrec_transition_to_pcsx(state);

	psxMemWrite8(&psxRegs, mem, val);

	lightrec_transition_from_pcsx(state);
}

static void mem_write_half(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	lightrec_transition_to_pcsx(state);

	psxMemWrite16(&psxRegs, mem, val);

	lightrec_transition_from_pcsx(state);
}

static void mem_write_word(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	lightrec_transition_to_pcsx(state);

	psxMemWrite32(&psxRegs, mem, val);

	lightrec_transition_from_pcsx(state);
}

static u8 mem_read_byte(struct lightrec_state *state, u32 op, void *host, u32 mem)
{
	u8 val;

	lightrec_transition_to_pcsx(state);

	val = psxMemRead8(&psxRegs, mem);

	lightrec_transition_from_pcsx(state);

	return val;
}

static u16 mem_read_half(struct lightrec_state *state,
			u32 op, void *host, u32 mem)
{
	u16 val;

	lightrec_transition_to_pcsx(state);

	val = psxMemRead16(&psxRegs, mem);

	lightrec_transition_from_pcsx(state);

	return val;
}

static u32 mem_read_word(struct lightrec_state *state,
			u32 op, void *host, u32 mem)
{
	u32 val;

	lightrec_transition_to_pcsx(state);

	val = psxMemRead32(&psxRegs, mem);

	lightrec_transition_from_pcsx(state);

	return val;
}

// ReSys12: the unaligned word accesses lightrec makes of LWL/LWR and SWL/SWR pairs, as the pairs: two aligned words
static u32 mem_read_word_u(struct lightrec_state *state, u32 op, void *host, u32 mem)
{
	const u32 s = (mem & 3) * 8, a = mem & ~3u;
	u32 v = mem_read_word(state, op, host, a);
	if (s)
		v = (v >> s) | (mem_read_word(state, op, host, a + 4) << (32 - s));
	return v;
}

static void mem_write_word_u(struct lightrec_state *state, u32 op, void *host, u32 mem, u32 val)
{
	const u32 s = (mem & 3) * 8, a = mem & ~3u;
	if (!s) {
		mem_write_word(state, op, host, a, val);
		return;
	}
	const u32 m = (1u << s) - 1;
	mem_write_word(state, op, host, a, (mem_read_word(state, op, host, a) & m) | (val << s));
	mem_write_word(state, op, host, a + 4, (mem_read_word(state, op, host, a + 4) & ~m) | (val >> (32 - s)));
}

static struct lightrec_mem_map_ops mem_ops = {
	.sb = mem_write_byte,
	.sh = mem_write_half,
	.sw = mem_write_word,
	.lb = mem_read_byte,
	.lh = mem_read_half,
	.lw = mem_read_word,
	.lwu = mem_read_word_u,
	.swu = mem_write_word_u,
};

static void cache_ctrl_write_word(struct lightrec_state *state,
				  u32 op, void *host, u32 mem, u32 val)
{
	psxRegs.biuReg = val; // ReSys12: where the interpreter keeps it
}

static u32 cache_ctrl_read_word(struct lightrec_state *state,
				u32 op, void *host, u32 mem)
{
	return psxRegs.biuReg;
}

// ReSys12: the interpreter's stalls (psxinterpreter.c psxMULT_stall ... mflohiCheckStall, gte.c gteCheckStall),
// called by the compiled code and lightrec's interpreter before the instruction, with the exact cycle count
void rs12_lightrec_stall_cb(struct lightrec_state *state, u32 opcode)
{
	const struct lightrec_registers *regs = lightrec_get_registers(state);
	u32 rs, lz, left;

	lightrec_transition_to_pcsx(state);
	switch (opcode >> 26) {
	case 0: // SPECIAL
		switch (opcode & 0x3f) {
		case 0x18: // MULT
			rs = regs->gpr[(opcode >> 21) & 31];
			lz = __builtin_clz(((rs ^ ((s32)rs >> 21)) | 1));
			psxRegs.muldivBusyCycle = psxRegs.cycle + 7 + (2 - (lz / 11)) * 4;
			break;
		case 0x19: // MULTU
			rs = regs->gpr[(opcode >> 21) & 31];
			lz = __builtin_clz(rs | 1);
			psxRegs.muldivBusyCycle = psxRegs.cycle + 7 + (2 - (lz / 11)) * 4;
			break;
		case 0x1a: case 0x1b: // DIV, DIVU
			psxRegs.muldivBusyCycle = psxRegs.cycle + 37;
			break;
		case 0x10: case 0x12: // MFHI, MFLO
			left = psxRegs.muldivBusyCycle - psxRegs.cycle;
			if (left <= 37)
				psxRegs.cycle = psxRegs.muldivBusyCycle;
			break;
		}
		break;
	case 0x19: // lightrec's OP_META_MULT2: a MULT by a power of two
		rs = regs->gpr[(opcode >> 21) & 31];
		lz = __builtin_clz(((rs ^ ((s32)rs >> 21)) | 1));
		psxRegs.muldivBusyCycle = psxRegs.cycle + 7 + (2 - (lz / 11)) * 4;
		break;
	case 0x1a: // OP_META_MULTU2
		rs = regs->gpr[(opcode >> 21) & 31];
		lz = __builtin_clz(rs | 1);
		psxRegs.muldivBusyCycle = psxRegs.cycle + 7 + (2 - (lz / 11)) * 4;
		break;
	case 0x12: // COP2 (psxCOP2_stall, when COP2 is enabled)
		if (regs->cp0[12] & (1u << 30))
			gteCheckStall(opcode & 0x3f);
		break;
	case 0x32: case 0x3a: // LWC2, SWC2
		if (regs->cp0[12] & (1u << 30))
			gteCheckStall(0);
		break;
	}
	lightrec_transition_from_pcsx(state);
}

static struct lightrec_mem_map_ops cache_ctrl_ops = {
	.sw = cache_ctrl_write_word,
	.lw = cache_ctrl_read_word,
};

// ReSys12: the System 12 map. 4MB of RAM mirrored once in the 8MB window; the 4MB program flash at 0x1fc00000 read
// directly up to 0x1feffff and through the handlers above it (tektagt's protection register at 0x1ff00000); the
// board (0x1f000000-0x1fbfffff outside the scratchpad and the hardware registers) on the handlers; the unused
// PS1 entries have no length. The code LUT covers RAM and the flash (lightrec-private.h RAM_SIZE / BIOS_SIZE).
// System 10 (lightrec_plugin_init): the boot ROM read directly up to 0x1fdfffff, the handlers from 0x1fe00000 (the
// EXIO board and the light guns: the core's LUT tells ROM from board)
static struct lightrec_mem_map lightrec_map[] = {
	[PSX_MAP_KERNEL_USER_RAM] = {
		/* Kernel and user memory */
		.pc = 0x00000000,
		.length = PSX_RAM_SIZE,
	},
	[PSX_MAP_BIOS] = {
		/* BIOS */
		.pc = 0x1fc00000,
		.length = 0x300000,
		.ops = &mem_ops, // ReSys12: reads of known addresses stay direct; writes (and the rest) go to the core: the
		                 // flash is not written (mrdrillrja2 stores into it and reads the word back)
	},
	[PSX_MAP_SCRATCH_PAD] = {
		/* Scratch pad */
		.pc = 0x1f800000,
		.length = 0x400,
	},
	[PSX_MAP_PARALLEL_PORT] = {
		/* Parallel port */
		.pc = 0x1f000000,
		.length = 0,
	},
	[PSX_MAP_HW_REGISTERS] = {
		/* Hardware registers */
		.pc = 0x1f801000,
		.length = 0xf000,
		.ops = &mem_ops,
	},
	[PSX_MAP_CACHE_CONTROL] = {
		/* Cache control */
		.pc = 0x5ffe0130,
		.length = 4,
		.ops = &cache_ctrl_ops,
	},

	/* Mirrors of the kernel/user memory */
	[PSX_MAP_MIRROR1] = {
		.pc = PSX_RAM_SIZE,
		.length = PSX_RAM_SIZE,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_MIRROR2] = {
		.pc = 0x00400000,
		.length = 0,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_MIRROR3] = {
		.pc = 0x00600000,
		.length = 0,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	// ReSys12: the board, in the entries of the PS1 BIOS mirrors
	[PSX_MAP_BIOS_MIRROR1] = {
		.pc = 0x1f000000,
		.length = 0x800000,
		.ops = &mem_ops,
	},
	[PSX_MAP_BIOS_MIRROR2] = {
		.pc = 0x1f800400,
		.length = 0xc00,
		.ops = &mem_ops,
	},
	[PSX_MAP_BIOS_MIRROR3] = {
		.pc = 0x1f810000,
		.length = 0x3f0000,
		.ops = &mem_ops,
	},
	[PSX_MAP_BIOS_MIRROR4] = {
		.pc = 0x1ff00000,
		.length = 0x100000,
		.ops = &mem_ops,
	},
	[PSX_MAP_BIOS_MIRROR5] = {
		.pc = 0x1fe80000,
		.length = 0,
	},
	[PSX_MAP_BIOS_MIRROR6] = {
		.pc = 0x1ff00000,
		.length = 0,
	},
	[PSX_MAP_BIOS_MIRROR7] = {
		.pc = 0x1ff80000,
		.length = 0,
	},

	/* Mirror of the parallel port. Only used by the PS2/PS3 BIOS */
	[PSX_MAP_PPORT_MIRROR] = {
		.pc = 0x1fa00000,
		.length = 0,
		.mirror_of = &lightrec_map[PSX_MAP_PARALLEL_PORT],
	},

	/* Code buffer */
	[PSX_MAP_CODE_BUFFER] = {
		.length = CODE_BUFFER_SIZE,
	},
};

static void lightrec_enable_ram(struct lightrec_state *state, bool enable)
{
	if (enable)
		memcpy(psxRegs.ptrs.psxM, cache_buf, sizeof(cache_buf));
	else
		memcpy(cache_buf, psxRegs.ptrs.psxM, sizeof(cache_buf));

	ram_disabled = !enable;
}

static const struct lightrec_ops lightrec_ops = {
	.cop2_op = cop2_op,
	.enable_ram = lightrec_enable_ram,
	.hw_direct = NULL, // ReSys12: every hardware register access through psxhw.c (as the interpreter)
	.code_inv = LIGHTREC_CODE_INV ? lightrec_code_inv : NULL,
};

static int lightrec_plugin_init(void)
{
	lightrec_map[PSX_MAP_KERNEL_USER_RAM].address = psxRegs.ptrs.psxM;
	lightrec_map[PSX_MAP_BIOS].address = psxRegs.ptrs.psxR;
	lightrec_map[PSX_MAP_SCRATCH_PAD].address = psxRegs.ptrs.psxH;
	lightrec_map[PSX_MAP_HW_REGISTERS].address = psxRegs.ptrs.psxH + 0x1000;
	lightrec_map[PSX_MAP_PARALLEL_PORT].address = psxRegs.ptrs.psxP;
	{ // ReSys12: the board window inside the 4MB ROM area, per board (the map is static: a reload may switch boards)
		const u32 direct = s12.s10 ? 0x200000 : 0x300000;
		lightrec_map[PSX_MAP_BIOS].length = direct;
		lightrec_map[PSX_MAP_BIOS_MIRROR4].pc = 0x1fc00000 + direct;
		lightrec_map[PSX_MAP_BIOS_MIRROR4].length = 0x400000 - direct;
		lightrec_map[PSX_MAP_BIOS_MIRROR4].address = psxRegs.ptrs.psxR + direct; // (code fetches)
	}

	// ReSys12: the code buffer is mapped executable here (the core's own memory uses calloc, P_HAVE_MMAP=0)
	code_buffer = mmap(0, CODE_BUFFER_SIZE,
			   PROT_EXEC | PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code_buffer == MAP_FAILED)
		return -ENOMEM;

	lightrec_map[PSX_MAP_CODE_BUFFER].address = code_buffer;

	use_lightrec_interpreter = !!getenv("LIGHTREC_INTERPRETER");

#ifdef LIGHTREC_DEBUG
	char *cycles = getenv("LIGHTREC_BEGIN_CYCLES");

	lightrec_very_debug = !!getenv("LIGHTREC_VERY_DEBUG");
	lightrec_debug = lightrec_very_debug || !!getenv("LIGHTREC_DEBUG");

	if (cycles)
		lightrec_begin_cycles = (unsigned int) strtol(cycles, NULL, 0);
#endif

	lightrec_state = lightrec_init(LIGHTREC_PROG_NAME,
			lightrec_map, ARRAY_SIZE(lightrec_map),
			&lightrec_ops);
	if (!lightrec_state)
		return -1;

	lightrec_plugin_apply_config();
	return 0;
}

static u32 do_calculate_hash(const void *buffer, u32 count, u32 needle, bool le)
{
	unsigned int i;
	const u32 *data = (const u32 *) buffer;
	u32 hash = needle;

	count /= 4;
	for(i = 0; i < count; ++i) {
		hash += le ? LE32TOH(data[i]) : data[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

static u32 hash_calculate_le(const void *buffer, u32 count)
{
	return do_calculate_hash(buffer, count, 0xffffffff, true);
}

u32 hash_calculate(const void *buffer, u32 count)
{
	return do_calculate_hash(buffer, count, 0xffffffff, false);
}

static u32 hash_calculate_ram(const void *buffer, u32 ram_size)
{
	u32 hash;

	if (ram_disabled)
		hash = hash_calculate_le(cache_buf, sizeof(cache_buf));
	else
		hash = hash_calculate_le(buffer, sizeof(cache_buf));

	return do_calculate_hash(buffer + sizeof(cache_buf),
				 ram_size - sizeof(cache_buf),
				 hash, true);
}

static const char * const mips_regs[] = {
	"zero",
	"at",
	"v0", "v1",
	"a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9",
	"k0", "k1",
	"gp", "sp", "fp", "ra",
	"lo", "hi",
};

static void print_for_big_ass_debugger(void)
{
	struct lightrec_registers *regs;
	unsigned int i;

	regs = lightrec_get_registers(lightrec_state);

	printf("CYCLE 0x%08x PC 0x%08x", psxRegs.cycle, psxRegs.pc);

	if (lightrec_very_debug)
		printf(" RAM 0x%08x SCRATCH 0x%08x HW 0x%08x",
				hash_calculate_ram(psxRegs.ptrs.psxM, PSX_RAM_SIZE),
				hash_calculate_le(psxRegs.ptrs.psxH, 0x400),
				hash_calculate_le(psxRegs.ptrs.psxH + 0x1000, 0x2000));

	printf(" CP0 0x%08x CP2D 0x%08x CP2C 0x%08x INT 0x%04x INTCYCLE 0x%08x GPU 0x%08x",
			hash_calculate(regs->cp0, sizeof(regs->cp0)),
			hash_calculate(regs->cp2d, sizeof(regs->cp2d)),
			hash_calculate(regs->cp2c, sizeof(regs->cp2c)),
			psxRegs.interrupt,
			hash_calculate(psxRegs.intCycle, sizeof(psxRegs.intCycle)),
			LE32TOH(HW_GPU_STATUS));

	if (lightrec_very_debug) {
		for (i = 0; i < 32; i++)
			printf(" CP2D%u 0x%08x", i, regs->cp2d[i]);
		for (i = 0; i < 32; i++)
			printf(" CP2C%u 0x%08x", i, regs->cp2c[i]);
	}

	if (lightrec_very_debug)
		for (i = 0; i < 34; i++)
			printf(" %s 0x%08x", mips_regs[i], regs->gpr[i]);
	else
		printf(" GPR 0x%08x",
		       hash_calculate(regs->gpr, sizeof(regs->gpr)));
	printf("\n");

	fflush(stdout);
}

static void lightrec_plugin_sync_regs_to_pcsx(bool need_cp2);
static void lightrec_plugin_sync_regs_from_pcsx(bool need_cp2);

static void lightrec_plugin_execute_internal(bool block_only)
{
	struct lightrec_registers *regs;
	u32 flags, cycles_pcsx;
	u32 old_pc = psxRegs.pc;

	regs = lightrec_get_registers(lightrec_state);
	gen_interupt((psxCP0Regs *)regs->cp0);
	if (!block_only && psxRegs.stop)
		return;

	cycles_pcsx = psxRegs.next_interupt - psxRegs.cycle;
	assert((s32)cycles_pcsx > 0);
again:

	// step during early boot so that 0x80030000 fastboot hack works
	// must not use 0 cycles to ensure progress
	block_stepping = block_only;
	if (block_only)
		cycles_pcsx = 1;

	if (use_pcsx_interpreter) {
		psxInt.ExecuteBlock(&psxRegs, 0);
	} else {
		u32 cycles_lightrec = lightrec_cycles_for((s32)cycles_pcsx); // ReSys12
		if (unlikely(use_lightrec_interpreter)) {
			psxRegs.pc = lightrec_run_interpreter(lightrec_state,
							      psxRegs.pc,
							      cycles_lightrec);
		} else {
			psxRegs.pc = lightrec_execute(lightrec_state,
						      psxRegs.pc, cycles_lightrec);
		}

		lightrec_transition_to_pcsx(lightrec_state);

		flags = lightrec_exit_flags(lightrec_state);

		if (flags & LIGHTREC_EXIT_SEGFAULT) {
			fprintf(stderr, "Exiting at cycle 0x%08x\n",
				psxRegs.cycle);
			if (lightrec_debug)
				print_for_big_ass_debugger();
			exit(1);
		}
		else if (flags & (LIGHTREC_EXIT_SYSCALL | LIGHTREC_EXIT_BREAK)) {
			// ReSys12: the handler runs on without the events being checked here: the interpreter's
			// intException runs them at the handler's first branch (psxBranchTest), not at the exception
			s32 left;
			psxException((flags & LIGHTREC_EXIT_SYSCALL ? R3000E_Syscall : R3000E_Bp) << 2, 0,
				     (psxCP0Regs *)regs->cp0);
			left = psxRegs.next_interupt - psxRegs.cycle;
			cycles_pcsx = left > 0 ? left : 1;
			goto again;
		}
		else if (flags & LIGHTREC_EXIT_UNKNOWN_OP) {
			u32 op = intFakeFetch(psxRegs.pc);
			u32 hlec = op & 0x03ffffff;
			if (rs12_hle_lz_head && psxRegs.pc == rs12_hle_lz_head) {
				// ReSys12: the HLE opcode of s12_hle.c (its cycle was counted with the block, as the
				// interpreter's addCycle); the routine sets the pc. As the interpreter, which keeps
				// next_interupt at 0: one control byte a visit, and the loop's jump runs as code before
				// the events are checked (the test pictures were recorded with that timing)
				u32 next = psxRegs.next_interupt;
				s32 left;
				lightrec_plugin_sync_regs_to_pcsx(0);
				psxRegs.next_interupt = 0;
				rs12_hle_lz();
				psxRegs.next_interupt = next;
				lightrec_plugin_sync_regs_from_pcsx(0);
				left = next - psxRegs.cycle;
				cycles_pcsx = left > 0 ? left : 1;
				goto again;
			}
			else if ((op >> 26) == 0x3b && hlec < ARRAY_SIZE(psxHLEt) && Config.HLE) {
				lightrec_plugin_sync_regs_to_pcsx(0);
				psxRegs.pc += 4; // match psxinterpreter
				psxHLEt[hlec]();
				lightrec_plugin_sync_regs_from_pcsx(0);
			}
			else
				psxException(R3000E_RI << 2, 0, (psxCP0Regs *)regs->cp0);
		}
		else if (psxRegs.biosFuncsHooked && ((psxRegs.pc & ~0xa0000010) == 0xa0 ||
			 (psxRegs.pc & ~0xa0000000) == 0xc0)) {
			// this is rather costly, but hooking should be off in most cases
			lightrec_plugin_sync_regs_to_pcsx(0);
			psxBiosJumpTest(&psxRegs);
			lightrec_plugin_sync_regs_from_pcsx(0);
		}
	}

	if (lightrec_debug && psxRegs.cycle >= lightrec_begin_cycles && psxRegs.pc != old_pc) {
		print_for_big_ass_debugger();
	}

	if ((regs->cp0[13] & regs->cp0[12] & 0x300) && (regs->cp0[12] & 0x1)) {
		/* Handle software interrupts */
		regs->cp0[13] &= ~0x7c;
		psxException(regs->cp0[13], 0, (psxCP0Regs *)regs->cp0);
	}
}

static void lightrec_plugin_execute(psxRegisters *regs)
{
	while (!regs->stop)
		lightrec_plugin_execute_internal(lightrec_very_debug);
}

static void lightrec_plugin_execute_block(psxRegisters *regs,
	enum blockExecCaller caller)
{
	lightrec_plugin_execute_internal(true);
}

static void lightrec_plugin_clear(u32 addr, u32 size)
{
	if ((addr == 0 && size == UINT32_MAX)
	    || (lightrec_hacks & LIGHTREC_OPT_INV_DMA_ONLY))
		lightrec_invalidate_all(lightrec_state);
	else
		/* size * 4: PCSX uses DMA units */
		lightrec_invalidate(lightrec_state, addr, size * 4);
}

static void lightrec_plugin_notify(enum R3000Anote note, void *data)
{
	switch (note)
	{
	case R3000ACPU_NOTIFY_CACHE_ISOLATED:
	case R3000ACPU_NOTIFY_CACHE_UNISOLATED:
		/* not used, lightrec calls lightrec_enable_ram() instead */
		break;
	case R3000ACPU_NOTIFY_BEFORE_SAVE:
		/* non-null 'data' means this is HLE related sync */
		lightrec_plugin_sync_regs_to_pcsx(data == NULL);
		break;
	case R3000ACPU_NOTIFY_AFTER_LOAD:
	case R3000ACPU_NOTIFY_AFTER_LOAD_STATE:
		lightrec_plugin_sync_regs_from_pcsx(data == NULL);
		if (data == NULL)
			lightrec_invalidate_all(lightrec_state);
		break;
	}
}

static void lightrec_plugin_apply_config()
{
	// ReSys12: the interpreter's step (intApplyConfig: 0x10000 * cycle multiplier / 100) is lightrec's cycles per
	// instruction
	intApplyConfig();
	assert(psxRegs.subCycleStep);
	lightrec_set_cycles_per_opcode(lightrec_state, psxRegs.subCycleStep);

	lightrec_set_unsafe_opt_flags(lightrec_state, lightrec_hacks);
}

static void lightrec_plugin_shutdown(void)
{
	lightrec_destroy(lightrec_state);
	munmap(code_buffer, CODE_BUFFER_SIZE);
}

static void lightrec_plugin_reset(void)
{
	/* Invalidate all blocks */
	lightrec_invalidate_all(lightrec_state);

	// ReSys12: the registers psxReset set (the interpreter's SR), not lightrec's own reset values
	lightrec_plugin_sync_regs_from_pcsx(true);
}

static void lightrec_plugin_sync_regs_from_pcsx(bool need_cp2)
{
	struct lightrec_registers *regs;

	regs = lightrec_get_registers(lightrec_state);
	memcpy(regs->gpr, &psxRegs.GPR, sizeof(regs->gpr));
	memcpy(regs->cp0, &psxRegs.CP0, sizeof(regs->cp0));
	if (need_cp2)
		memcpy(regs->cp2d, &psxRegs.CP2, sizeof(regs->cp2d) + sizeof(regs->cp2c));
}

static void lightrec_plugin_sync_regs_to_pcsx(bool need_cp2)
{
	struct lightrec_registers *regs;

	regs = lightrec_get_registers(lightrec_state);
	memcpy(&psxRegs.GPR, regs->gpr, sizeof(regs->gpr));
	memcpy(&psxRegs.CP0, regs->cp0, sizeof(regs->cp0));
	if (need_cp2)
		memcpy(&psxRegs.CP2, regs->cp2d, sizeof(regs->cp2d) + sizeof(regs->cp2c));
}

R3000Acpu psxRec =
{
	lightrec_plugin_init,
	lightrec_plugin_reset,
	lightrec_plugin_execute,
	lightrec_plugin_execute_block,
	lightrec_plugin_clear,
	lightrec_plugin_notify,
	lightrec_plugin_apply_config,
	lightrec_plugin_shutdown,
};
