// ReSys10: host code for the blocks of Tmp95Cpu (s10_tmp95.h), through GNU lightning (deps/lightning: x86-64 and
// aarch64 from the same generator). The instructions the System 10 sub CPU programs spend their time in are generated
// inline, with MAME's flag results (900tbl.hxx); every other instruction is a call to its MAME handler with its
// pre-decoded operands, as run_block does. BSD-3-Clause.
#include "s10_tmp95_impl.h"

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

extern "C" {
#include <lightning.h>
}

namespace {
constexpr uint8_t FLAG_CF = 0x01, FLAG_NF = 0x02, FLAG_VF = 0x04, FLAG_HF = 0x10, FLAG_ZF = 0x40, FLAG_SF = 0x80;
constexpr uint8_t FLAGS_LOGIC = FLAG_SF | FLAG_ZF | FLAG_HF | FLAG_VF | FLAG_NF | FLAG_CF; // (0xd7)

// S, Z and the parity flag of a byte result (MAME parity(): V set for an even number of bits)
struct SzpTable {
	uint8_t t[256];
	SzpTable()
	{
		for (int i = 0; i < 256; i++)
			t[i] = uint8_t((i & 0x80) | (i ? 0 : FLAG_ZF) | ((__builtin_popcount(i) & 1) ? 0 : FLAG_VF));
	}
};
const SzpTable k_szp;

// one executable arena for every block: reset with the block cache
struct CodeArena {
	uint8_t *base = nullptr;
	size_t size = 0, used = 0;
	void *alloc(size_t n)
	{
		if (!base) {
			size = 16u << 20;
			void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p == MAP_FAILED) {
				size = 0;
				return nullptr;
			}
			base = (uint8_t *)p;
		}
		n = (n + 15) & ~size_t(15);
		if (used + n > size)
			return nullptr;
		void *p = base + used;
		used += n;
		return p;
	}
};
CodeArena g_arena;
bool g_jit_ready;
} // namespace

// ---- calls from the generated code ----
int Tmp95Cpu::jit_exec(Tmp95Cpu *c, const Desc *d)
{
	c->m_pc.d = d->pc_next;
	c->m_op = d->op;
	const uint8_t s = d->set;
	if (s & SET_REG1) c->m_reg1 = d->reg1;
	if (s & SET_REG2) c->m_reg2 = d->reg2;
	if (s & SET_IMM1) c->m_imm1.d = d->imm1;
	if (s & SET_IMM2) c->m_imm2.d = d->imm2;
	if (s & SET_EA2) c->m_ea2.d = d->ea2;
	c->prepare_ea(*d);
	if (s & SET_EA1) c->m_ea1.d = d->ea1;
	if (s & EA1_FROM_EA2) c->m_ea1.d = c->m_ea2.d;
	(c->*d->fn)();
	c->m_cycles += d->cycles;
	return c->m_pc.d != d->pc_next || c->m_check_irqs || c->m_halted || c->m_regbank != c->m_jit_bank;
}

uint32_t Tmp95Cpu::jit_read8(Tmp95Cpu *c, uint32_t a) { return c->m_space.read_byte(a); }
uint32_t Tmp95Cpu::jit_read16(Tmp95Cpu *c, uint32_t a) { return c->m_space.read_word_unaligned(a); }
uint32_t Tmp95Cpu::jit_read32(Tmp95Cpu *c, uint32_t a) { return c->m_space.read_dword_unaligned(a); }
uint32_t Tmp95Cpu::jit_write16(Tmp95Cpu *c, uint32_t a, uint32_t v)
{
	c->m_space.write_word_unaligned(a, uint16_t(v));
	return c->m_check_irqs || c->m_flush_pending;
}
uint32_t Tmp95Cpu::jit_write32(Tmp95Cpu *c, uint32_t a, uint32_t v)
{
	c->m_space.write_dword_unaligned(a, v);
	return c->m_check_irqs || c->m_flush_pending;
}

uint32_t Tmp95Cpu::jit_write8(Tmp95Cpu *c, uint32_t a, uint32_t v)
{
	c->m_space.write_byte(a, uint8_t(v));
	return c->m_check_irqs || c->m_flush_pending; // (an on-chip register asking for an interrupt check, or code rewritten)
}

// ---- generator ----
#define OFF(field) jit_word_t((const uint8_t *)&(field) - (const uint8_t *)this)

bool Tmp95Cpu::jit_compile(Block &b)
{
	if (!g_jit_ready) {
		init_jit(nullptr);
		g_jit_ready = true;
	}
	jit_state_t *_jit = jit_new_state();
	jit_prolog();
	jit_node_t *arg = jit_arg();
	jit_getarg(JIT_V0, arg);  // V0: this
	jit_movi(JIT_V1, 0);      // V1: the cycles of the inline instructions
	std::vector<jit_node_t *> to_exit;     // branches to the epilogue, m_pc already set
	int inline_cycles = 0;
	const jit_word_t o_sr = OFF(m_sr.b.l), o_pc = OFF(m_pc.d);

	// a register operand, in this block's register bank (the cache key)
	auto r8 = [&] (uint16_t sel) { return OFF(reg8(sel)); };
	auto r16 = [&] (uint16_t sel) { return OFF(reg16(sel)); };
	auto set_flags_szp = [&] (jit_gpr_t res, uint8_t extra) { // F = F & ~0xd7 | szp[res] | extra
		jit_movi(JIT_R2, jit_word_t(k_szp.t));
		jit_ldxr_uc(JIT_R2, JIT_R2, res);
		jit_ldxi_uc(JIT_R1, JIT_V0, o_sr);
		jit_andi(JIT_R1, JIT_R1, uint8_t(~FLAGS_LOGIC));
		jit_orr(JIT_R1, JIT_R1, JIT_R2);
		if (extra)
			jit_ori(JIT_R1, JIT_R1, extra);
		jit_stxi_c(o_sr, JIT_V0, JIT_R1);
	};

	// leave the block: m_pc = pc, the cycles so far (inline_cycles + extra)
	auto exit_at = [&] (uint32_t pc, int extra) {
		jit_movi(JIT_R0, pc);
		jit_stxi_i(o_pc, JIT_V0, JIT_R0);
		jit_addi(JIT_V1, JIT_V1, inline_cycles + extra);
		to_exit.push_back(jit_jmpi());
	};
	// the effective address of a memory operand into V2 (MAME prepare_ea / the fixed address)
	auto emit_ea = [&] (const Desc &d, bool first) {
		switch (d.ea_kind) {
		case EA_NONE:
			jit_movi(JIT_V2, first ? d.ea1 : d.ea2);
			break;
		case EA_CUR32:
		case EA_REG32:
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(d.ea_kind == EA_CUR32 ? get_reg32_current(d.ea_sel) : get_reg32(d.ea_sel)));
			if (d.disp)
				jit_addi(JIT_V2, JIT_V2, d.disp);
			break;
		case EA_REG32_R8:
		case EA_REG32_R16:
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(get_reg32(d.ea_sel)));
			if (d.ea_kind == EA_REG32_R8)
				jit_ldxi_c(JIT_R0, JIT_V0, OFF(get_reg8(d.ea_sel2)));
			else
				jit_ldxi_s(JIT_R0, JIT_V0, OFF(get_reg16(d.ea_sel2)));
			jit_addr(JIT_V2, JIT_V2, JIT_R0);
			break;
		case EA_PREDEC:
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(get_reg32(d.ea_sel)));
			jit_subi(JIT_V2, JIT_V2, d.ea_step);
			jit_stxi_i(OFF(get_reg32(d.ea_sel)), JIT_V0, JIT_V2);
			break;
		case EA_POSTINC:
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(get_reg32(d.ea_sel)));
			jit_addi(JIT_R0, JIT_V2, d.ea_step);
			jit_stxi_i(OFF(get_reg32(d.ea_sel)), JIT_V0, JIT_R0);
			break;
		}
	};
	// memory at V2 into R0: RAM pages straight, the rest (on-chip registers, devices) through the address space
	auto emit_read = [&] (int size) {
		jit_andi(JIT_R1, JIT_V2, 0xffffff);
		jit_rshi_u(JIT_R2, JIT_R1, 12);
		jit_lshi(JIT_R2, JIT_R2, sizeof(void *) == 8 ? 3 : 2);
		jit_addr(JIT_R2, JIT_R2, JIT_V0);
		jit_ldxi(JIT_R2, JIT_R2, OFF(m_space.page[0]));
		jit_node_t *slow1 = jit_beqi(JIT_R2, 0);
		jit_andi(JIT_R1, JIT_R1, 0xfff);
		jit_node_t *slow2 = size > 1 ? jit_bgti_u(JIT_R1, 0x1000 - size) : nullptr;
		if (size == 1) jit_ldxr_uc(JIT_R0, JIT_R2, JIT_R1);
		else if (size == 2) jit_ldxr_us(JIT_R0, JIT_R2, JIT_R1);
		else jit_ldxr_i(JIT_R0, JIT_R2, JIT_R1);
		jit_node_t *done = jit_jmpi();
		jit_patch(slow1);
		if (slow2)
			jit_patch(slow2);
		jit_prepare();
		jit_pushargr(JIT_V0);
		jit_pushargr(JIT_V2);
		jit_finishi(size == 1 ? (jit_pointer_t)&Tmp95Cpu::jit_read8 : size == 2 ? (jit_pointer_t)&Tmp95Cpu::jit_read16 :
			(jit_pointer_t)&Tmp95Cpu::jit_read32);
		jit_retval(JIT_R0);
		jit_patch(done);
	};
	// R0 to memory at V2; RAM pages without decoded code straight. The slow path may ask to leave the block
	// (an interrupt to check, code rewritten): then m_pc = next
	auto emit_write = [&] (int size, uint32_t next, int cycles_here) {
		jit_andi(JIT_R1, JIT_V2, 0xffffff);
		jit_rshi_u(JIT_R2, JIT_R1, 12);
		jit_addr(JIT_R2, JIT_R2, JIT_V0);
		jit_ldxi_uc(JIT_R2, JIT_R2, OFF(m_space.code_page[0]));
		jit_node_t *slow1 = jit_bnei(JIT_R2, 0);
		jit_rshi_u(JIT_R2, JIT_R1, 12);
		jit_lshi(JIT_R2, JIT_R2, sizeof(void *) == 8 ? 3 : 2);
		jit_addr(JIT_R2, JIT_R2, JIT_V0);
		jit_ldxi(JIT_R2, JIT_R2, OFF(m_space.page[0]));
		jit_node_t *slow2 = jit_beqi(JIT_R2, 0);
		jit_andi(JIT_R1, JIT_R1, 0xfff);
		jit_node_t *slow3 = size > 1 ? jit_bgti_u(JIT_R1, 0x1000 - size) : nullptr;
		if (size == 1) jit_stxr_c(JIT_R1, JIT_R2, JIT_R0);
		else if (size == 2) jit_stxr_s(JIT_R1, JIT_R2, JIT_R0);
		else jit_stxr_i(JIT_R1, JIT_R2, JIT_R0);
		jit_node_t *done = jit_jmpi();
		jit_patch(slow1);
		jit_patch(slow2);
		if (slow3)
			jit_patch(slow3);
		jit_prepare();
		jit_pushargr(JIT_V0);
		jit_pushargr(JIT_V2);
		jit_pushargr(JIT_R0);
		jit_finishi(size == 1 ? (jit_pointer_t)&Tmp95Cpu::jit_write8 : size == 2 ? (jit_pointer_t)&Tmp95Cpu::jit_write16 :
			(jit_pointer_t)&Tmp95Cpu::jit_write32);
		jit_retval(JIT_R0);
		jit_node_t *stay = jit_beqi(JIT_R0, 0);
		exit_at(next, cycles_here);
		jit_patch(stay);
		jit_patch(done);
	};
	// MAME add<T> / sub<T> flags of a (R0) and a constant b, result into V2 (truncated); C kept for INC / DEC
	auto emit_addsub = [&] (int bits, bool sub, uint32_t bconst, bool keep_carry, bool store_flags_only) {
		const uint32_t mask = bits == 8 ? 0xff : 0xffff, sign = bits == 8 ? 0x80 : 0x8000;
		(void)store_flags_only;
		if (sub)
			jit_subi(JIT_V2, JIT_R0, bconst);
		else
			jit_addi(JIT_V2, JIT_R0, bconst);
		jit_andi(JIT_V2, JIT_V2, mask);
		jit_ldxi_uc(JIT_R1, JIT_V0, o_sr);
		uint8_t clear = FLAG_SF | FLAG_ZF | FLAG_HF | FLAG_VF | (keep_carry ? 0 : FLAG_CF) | (sub ? 0 : FLAG_NF);
		jit_andi(JIT_R1, JIT_R1, uint8_t(~clear));
		if (sub)
			jit_ori(JIT_R1, JIT_R1, FLAG_NF);
		jit_rshi_u(JIT_R2, JIT_V2, bits - 8);                  // S
		jit_andi(JIT_R2, JIT_R2, FLAG_SF);
		jit_orr(JIT_R1, JIT_R1, JIT_R2);
		jit_node_t *nz = jit_bnei(JIT_V2, 0);                  // Z
		jit_ori(JIT_R1, JIT_R1, FLAG_ZF);
		jit_patch(nz);
		jit_xori(JIT_R2, JIT_R0, bconst);                      // H: (a ^ b ^ r) & 0x10
		jit_xorr(JIT_R2, JIT_R2, JIT_V2);
		jit_andi(JIT_R2, JIT_R2, FLAG_HF);
		jit_orr(JIT_R1, JIT_R1, JIT_R2);
		// V: add ((r ^ a) & (r ^ b)), sub ((r ^ a) & (a ^ b)), sign bit
		jit_xorr(JIT_R2, JIT_V2, JIT_R0);
		jit_stxi_c(o_sr, JIT_V0, JIT_R1);                      // (R1 free for the second term)
		if (sub)
			jit_xori(JIT_R1, JIT_R0, bconst);
		else
			jit_xori(JIT_R1, JIT_V2, bconst);
		jit_andr(JIT_R2, JIT_R2, JIT_R1);
		jit_ldxi_uc(JIT_R1, JIT_V0, o_sr);
		jit_node_t *nv = jit_bmci(JIT_R2, sign);
		jit_ori(JIT_R1, JIT_R1, FLAG_VF);
		jit_patch(nv);
		if (!keep_carry) {                                     // C: add r < a, sub r > a
			jit_node_t *nc = sub ? jit_bler_u(JIT_V2, JIT_R0) : jit_bger_u(JIT_V2, JIT_R0);
			jit_ori(JIT_R1, JIT_R1, FLAG_CF);
			jit_patch(nc);
		}
		jit_stxi_c(o_sr, JIT_V0, JIT_R1);
	};

	for (size_t i = 0; i < b.d.size(); i++) {
		const Desc &d = b.d[i];
		const bool ops_static = d.ea_kind == EA_NONE && !(d.set & EA1_FROM_EA2);
		const ophandler f = d.fn;
		const bool has1 = d.set & SET_REG1, has2 = d.set & SET_REG2;
		bool done = true;
		const bool ea2_known = d.ea_kind != EA_NONE || (d.set & SET_EA2);
		const bool ea1_known = (d.set & EA1_FROM_EA2) || (d.ea_kind == EA_NONE && (d.set & SET_EA1));
		const int msize = (f == &Tmp95Cpu::op_LDBRM || f == &Tmp95Cpu::op_LDBMR || f == &Tmp95Cpu::op_LDBMI) ? 1 :
			(f == &Tmp95Cpu::op_LDWRM || f == &Tmp95Cpu::op_LDWMR || f == &Tmp95Cpu::op_LDWMI) ? 2 : 4;
		if ((f == &Tmp95Cpu::op_LDBRM || f == &Tmp95Cpu::op_LDWRM || f == &Tmp95Cpu::op_LDLRM) && has1 && ea2_known &&
			!(d.set & EA1_FROM_EA2 && d.ea_kind == EA_NONE)) {
			emit_ea(d, false);
			emit_read(msize);
			if (msize == 1) jit_stxi_c(r8(d.reg1), JIT_V0, JIT_R0);
			else if (msize == 2) jit_stxi_s(r16(d.reg1), JIT_V0, JIT_R0);
			else jit_stxi_i(OFF(reg32(d.reg1)), JIT_V0, JIT_R0);
		} else if ((f == &Tmp95Cpu::op_LDBMR || f == &Tmp95Cpu::op_LDWMR || f == &Tmp95Cpu::op_LDLMR) && has2 && ea1_known) {
			emit_ea(d, true);
			if (msize == 1) jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg2));
			else if (msize == 2) jit_ldxi_us(JIT_R0, JIT_V0, r16(d.reg2));
			else jit_ldxi_i(JIT_R0, JIT_V0, OFF(reg32(d.reg2)));
			emit_write(msize, d.pc_next, d.cycles);
		} else if ((f == &Tmp95Cpu::op_LDBMI || f == &Tmp95Cpu::op_LDWMI) && (d.set & SET_IMM2) && ea1_known) {
			emit_ea(d, true);
			jit_movi(JIT_R0, d.imm2 & (msize == 1 ? 0xff : 0xffff));
			emit_write(msize, d.pc_next, d.cycles);
		} else if ((f == &Tmp95Cpu::op_PUSHWR || f == &Tmp95Cpu::op_PUSHLR) && has1) {
			const int n = f == &Tmp95Cpu::op_PUSHWR ? 2 : 4;
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(m_xssp.d));
			jit_subi(JIT_V2, JIT_V2, n);
			jit_stxi_i(OFF(m_xssp.d), JIT_V0, JIT_V2);
			if (n == 2) jit_ldxi_us(JIT_R0, JIT_V0, r16(d.reg1));
			else jit_ldxi_i(JIT_R0, JIT_V0, OFF(reg32(d.reg1)));
			emit_write(n, d.pc_next, d.cycles);
		} else if ((f == &Tmp95Cpu::op_POPWR || f == &Tmp95Cpu::op_POPLR) && has1) {
			const int n = f == &Tmp95Cpu::op_POPWR ? 2 : 4;
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(m_xssp.d));
			emit_read(n);
			jit_addi(JIT_V2, JIT_V2, n);
			jit_stxi_i(OFF(m_xssp.d), JIT_V0, JIT_V2);
			if (n == 2) jit_stxi_s(r16(d.reg1), JIT_V0, JIT_R0);
			else jit_stxi_i(OFF(reg32(d.reg1)), JIT_V0, JIT_R0);
		} else if (f == &Tmp95Cpu::op_CALR && (d.set & SET_EA1) && d.ea_kind == EA_NONE && i + 1 == b.d.size()) {
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(m_xssp.d));
			jit_subi(JIT_V2, JIT_V2, 4);
			jit_stxi_i(OFF(m_xssp.d), JIT_V0, JIT_V2);
			jit_movi(JIT_R0, d.pc_next);
			emit_write(4, d.ea1, d.cycles); // (leaving early lands on the callee too: the push is done)
			exit_at(d.ea1, d.cycles);
			inline_cycles = 0;
			break;
		} else if (f == &Tmp95Cpu::op_RET && i + 1 == b.d.size()) {
			jit_ldxi_i(JIT_V2, JIT_V0, OFF(m_xssp.d));
			emit_read(4);
			jit_addi(JIT_V2, JIT_V2, 4);
			jit_stxi_i(OFF(m_xssp.d), JIT_V0, JIT_V2);
			jit_stxi_i(o_pc, JIT_V0, JIT_R0);
			jit_addi(JIT_V1, JIT_V1, inline_cycles + d.cycles);
			to_exit.push_back(jit_jmpi());
			inline_cycles = 0;
			break;
		} else if (ops_static && (f == &Tmp95Cpu::op_INCBIR || f == &Tmp95Cpu::op_DECBIR) && has2 && (d.set & SET_IMM1)) {
			jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg2));
			emit_addsub(8, f == &Tmp95Cpu::op_DECBIR, (d.imm1 & 0xff) ? (d.imm1 & 0xff) : 8, true, false);
			jit_stxi_c(r8(d.reg2), JIT_V0, JIT_V2);
		} else if (ops_static && f == &Tmp95Cpu::op_CPBRI && has1 && (d.set & SET_IMM2)) {
			jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg1));
			emit_addsub(8, true, d.imm2 & 0xff, false, true);
		} else if (ops_static && f == &Tmp95Cpu::op_CPWRI && has1 && (d.set & SET_IMM2)) {
			jit_ldxi_us(JIT_R0, JIT_V0, r16(d.reg1));
			emit_addsub(16, true, d.imm2 & 0xffff, false, true);
		} else if (ops_static && f == &Tmp95Cpu::op_LDBRR && has1 && has2) {
			jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg2));
			jit_stxi_c(r8(d.reg1), JIT_V0, JIT_R0);
		} else if (ops_static && f == &Tmp95Cpu::op_LDWRR && has1 && has2) {
			jit_ldxi_us(JIT_R0, JIT_V0, r16(d.reg2));
			jit_stxi_s(r16(d.reg1), JIT_V0, JIT_R0);
		} else if (ops_static && f == &Tmp95Cpu::op_LDBRI && has1 && (d.set & SET_IMM2)) {
			jit_movi(JIT_R0, d.imm2 & 0xff);
			jit_stxi_c(r8(d.reg1), JIT_V0, JIT_R0);
		} else if (ops_static && f == &Tmp95Cpu::op_LDWRI && has1 && (d.set & SET_IMM2)) {
			jit_movi(JIT_R0, d.imm2 & 0xffff);
			jit_stxi_s(r16(d.reg1), JIT_V0, JIT_R0);
		} else if (ops_static && (f == &Tmp95Cpu::op_RESBIR || f == &Tmp95Cpu::op_SETBIR) && has2 && (d.set & SET_IMM1)) {
			const uint8_t m = uint8_t(1 << (d.imm1 & 7));
			jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg2));
			if (f == &Tmp95Cpu::op_RESBIR)
				jit_andi(JIT_R0, JIT_R0, uint8_t(~m));
			else
				jit_ori(JIT_R0, JIT_R0, m);
			jit_stxi_c(r8(d.reg2), JIT_V0, JIT_R0);
		} else if (ops_static && f == &Tmp95Cpu::op_BITBIR && has2 && (d.set & SET_IMM1)) {
			const unsigned bit = d.imm1 & 0x0f;
			jit_ldxi_uc(JIT_R1, JIT_V0, o_sr);
			jit_andi(JIT_R1, JIT_R1, uint8_t(~(FLAG_ZF | FLAG_NF)));
			jit_ori(JIT_R1, JIT_R1, FLAG_HF);
			if (bit < 8) {
				jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg2));
				jit_node_t *set = jit_bmsi(JIT_R0, 1 << bit);
				jit_ori(JIT_R1, JIT_R1, FLAG_ZF);
				jit_patch(set);
			} else
				jit_ori(JIT_R1, JIT_R1, FLAG_ZF);
			jit_stxi_c(o_sr, JIT_V0, JIT_R1);
		} else if (ops_static && (f == &Tmp95Cpu::op_SRLBIR || f == &Tmp95Cpu::op_SLLBIR) && has2 && (d.set & SET_IMM1)) {
			// (sla / srl of a byte: the last bit shifted out is the carry; a count of 0 means 16)
			const unsigned n = (d.imm1 & 0x0f) ? (d.imm1 & 0x0f) : 16;
			const bool right = f == &Tmp95Cpu::op_SRLBIR;
			jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg2));
			if (n <= 8) {
				jit_rshi_u(JIT_V2, JIT_R0, right ? n - 1 : 8 - n); // V2: carry
				jit_andi(JIT_V2, JIT_V2, 1);
			} else
				jit_movi(JIT_V2, 0);
			if (n >= 8)
				jit_movi(JIT_R0, 0);
			else if (right)
				jit_rshi_u(JIT_R0, JIT_R0, n);
			else {
				jit_lshi(JIT_R0, JIT_R0, n);
				jit_andi(JIT_R0, JIT_R0, 0xff);
			}
			jit_stxi_c(r8(d.reg2), JIT_V0, JIT_R0);
			set_flags_szp(JIT_R0, 0);
			jit_ldxi_uc(JIT_R1, JIT_V0, o_sr);
			jit_orr(JIT_R1, JIT_R1, JIT_V2);
			jit_stxi_c(o_sr, JIT_V0, JIT_R1);
			inline_cycles += tlcs900_shift_cycles(uint8_t(n));
		} else if (ops_static && (f == &Tmp95Cpu::op_ANDBRI || f == &Tmp95Cpu::op_ORBRI || f == &Tmp95Cpu::op_ANDBRR ||
			f == &Tmp95Cpu::op_ORBRR) && has1 && ((d.set & SET_IMM2) || has2)) {
			const bool is_and = f == &Tmp95Cpu::op_ANDBRI || f == &Tmp95Cpu::op_ANDBRR;
			const bool imm = f == &Tmp95Cpu::op_ANDBRI || f == &Tmp95Cpu::op_ORBRI;
			if (imm && !(d.set & SET_IMM2))
				done = false;
			else if (!imm && !has2)
				done = false;
			else {
				jit_ldxi_uc(JIT_R0, JIT_V0, r8(d.reg1));
				if (imm) {
					if (is_and)
						jit_andi(JIT_R0, JIT_R0, d.imm2 & 0xff);
					else
						jit_ori(JIT_R0, JIT_R0, d.imm2 & 0xff);
				} else {
					jit_ldxi_uc(JIT_R1, JIT_V0, r8(d.reg2));
					if (is_and)
						jit_andr(JIT_R0, JIT_R0, JIT_R1);
					else
						jit_orr(JIT_R0, JIT_R0, JIT_R1);
				}
				jit_stxi_c(r8(d.reg1), JIT_V0, JIT_R0);
				set_flags_szp(JIT_R0, is_and ? FLAG_HF : 0);
			}
		} else if (ops_static && f == &Tmp95Cpu::op_INCWIR && has2 && (d.set & SET_IMM1)) {
			jit_ldxi_us(JIT_R0, JIT_V0, r16(d.reg2));
			jit_addi(JIT_R0, JIT_R0, (d.imm1 & 0xff) ? (d.imm1 & 0xff) : 8);
			jit_stxi_s(r16(d.reg2), JIT_V0, JIT_R0);
		} else if (ops_static && (f == &Tmp95Cpu::op_JR || f == &Tmp95Cpu::op_JRL) && (d.set & SET_EA2) && i + 1 == b.d.size()) {
			// condition_true(m_op): the cc field of the opcode
			const unsigned cc = d.op & 0x0f;
			jit_node_t *taken = nullptr;
			if (cc != 0x00 && cc != 0x08) {
				jit_ldxi_uc(JIT_R0, JIT_V0, o_sr);
				switch (cc & 7) {
				case 1: case 2: // LT (S xor V), LE (LT or Z)
					jit_rshi_u(JIT_R1, JIT_R0, 7);
					jit_rshi_u(JIT_R2, JIT_R0, 2);
					jit_xorr(JIT_R1, JIT_R1, JIT_R2);
					jit_andi(JIT_R1, JIT_R1, 1);
					if ((cc & 7) == 2) {
						jit_rshi_u(JIT_R2, JIT_R0, 6);
						jit_andi(JIT_R2, JIT_R2, 1);
						jit_orr(JIT_R1, JIT_R1, JIT_R2);
					}
					break;
				case 3: jit_andi(JIT_R1, JIT_R0, FLAG_ZF | FLAG_CF); break; // ULE
				case 4: jit_andi(JIT_R1, JIT_R0, FLAG_VF); break;           // OV
				case 5: jit_andi(JIT_R1, JIT_R0, FLAG_SF); break;           // MI
				case 6: jit_andi(JIT_R1, JIT_R0, FLAG_ZF); break;           // Z
				case 7: jit_andi(JIT_R1, JIT_R0, FLAG_CF); break;           // C
				}
				taken = cc < 8 ? jit_bnei(JIT_R1, 0) : jit_beqi(JIT_R1, 0);
				jit_movi(JIT_R0, d.pc_next); // not taken
				jit_stxi_i(o_pc, JIT_V0, JIT_R0);
				jit_addi(JIT_V1, JIT_V1, inline_cycles + d.cycles);
				to_exit.push_back(jit_jmpi());
				jit_patch(taken);
			}
			if (cc != 0x00) {
				jit_movi(JIT_R0, d.ea2);
				jit_stxi_i(o_pc, JIT_V0, JIT_R0);
				jit_addi(JIT_V1, JIT_V1, inline_cycles + d.cycles + tlcs900_jp_true_cycles());
			} else {
				jit_movi(JIT_R0, d.pc_next);
				jit_stxi_i(o_pc, JIT_V0, JIT_R0);
				jit_addi(JIT_V1, JIT_V1, inline_cycles + d.cycles);
			}
			to_exit.push_back(jit_jmpi());
			inline_cycles = 0;
			break;
		} else
			done = false;

		if (done) {
			inline_cycles += d.cycles;
			continue;
		}
		// any other instruction: its MAME handler (jit_exec)
		if (inline_cycles) {
			jit_addi(JIT_V1, JIT_V1, inline_cycles);
			inline_cycles = 0;
		}
		jit_prepare();
		jit_pushargr(JIT_V0);
		jit_pushargi(jit_word_t(&d));
		jit_finishi((jit_pointer_t)&Tmp95Cpu::jit_exec);
		jit_retval(JIT_R0);
		to_exit.push_back(jit_bnei(JIT_R0, 0));
	}
	// fall through the last instruction
	if (inline_cycles)
		jit_addi(JIT_V1, JIT_V1, inline_cycles);
	jit_movi(JIT_R0, b.d.back().pc_next);
	jit_stxi_i(o_pc, JIT_V0, JIT_R0);
	jit_node_t *exit = jit_label();
	for (jit_node_t *n : to_exit)
		jit_patch_at(n, exit);
	jit_ldxi_i(JIT_R0, JIT_V0, OFF(m_cycles));
	jit_addr(JIT_R0, JIT_R0, JIT_V1);
	jit_stxi_i(OFF(m_cycles), JIT_V0, JIT_R0);
	jit_reti(0);
	jit_epilog();

	jit_realize();
	jit_set_data(nullptr, 0, JIT_DISABLE_DATA | JIT_DISABLE_NOTE);
	jit_word_t size = 0;
	jit_get_code(&size);
	void *mem = g_arena.alloc(size_t(size) + 64);
	void *code = nullptr;
	if (mem) {
		jit_set_code(mem, size + 64);
		code = jit_emit();
	}
	jit_clear_state();
	jit_destroy_state();
	if (!code)
		return false;
	b.code = (int (*)(Tmp95Cpu *))code;
	return true;
}

void Tmp95Cpu::jit_flush_code() { g_arena.used = 0; }
