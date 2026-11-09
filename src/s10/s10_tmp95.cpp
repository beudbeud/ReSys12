// ReSys10: block execution for MAME's TMP95C061 (see s10_tmp95.h). BSD-3-Clause (as the MAME core it drives).
#include "s10_tmp95.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_set>

#include "s10_tmp95_impl.h"

static void code_hook(void *ctx, const u8 *host) { static_cast<Tmp95Cpu *>(ctx)->code_written(host); }

Tmp95Cpu::Tmp95Cpu(uint32_t clock) : tmp95c061_device(machine_config(), "tmp95c061", nullptr, clock)
{
	if (getenv("RS10_TLCS_NOJIT"))
		mode = MODE_BLOCKS;
	if (getenv("RS10_TLCS_INTERP"))
		mode = MODE_INTERP;
	if (getenv("RS10_TLCS_VERIFY"))
		mode = MODE_VERIFY;
}

// RS10_TLCS_PROF=<file> (with RS10_TLCS_NOJIT): how often each instruction ran, written when the CPU goes away
static std::unordered_map<uint32_t, uint64_t> *s_prof;

Tmp95Cpu::~Tmp95Cpu()
{
	if (!s_prof)
		return;
	std::vector<std::pair<uint64_t, uint32_t>> v;
	uint64_t total = 0;
	for (auto &e : *s_prof)
		v.push_back({ e.second, e.first }), total += e.second;
	std::sort(v.rbegin(), v.rend());
	fprintf(stderr, "tlcs prof: %llu instructions\n", (unsigned long long)total);
	for (size_t i = 0; i < v.size() && i < 60; i++)
		fprintf(stderr, "tlcs prof %06x %5.2f%%\n", v[i].second, 100.0 * v[i].first / total);
	if (FILE *f = fopen(getenv("RS10_TLCS_PROF"), "w")) { // (every address run: the JIT test's inputs)
		for (auto &e : v)
			fprintf(f, "%06x %llu\n", e.second, (unsigned long long)e.first);
		fclose(f);
	}
	delete s_prof;
	s_prof = nullptr;
}

void Tmp95Cpu::start()
{
	machine_config cfg;
	config_device() = this;
	device_add_mconfig(cfg);
	device_config_complete();
	m_space.install(m_program_config, this);
	m_space.code_hook = code_hook;
	m_space.code_ctx = this;
	device_resolve_objects();
	device_start();
}

void Tmp95Cpu::reset()
{
	device_reset();
	flush(); // (a new program is about to be uploaded)
}

// ---- block cache ----
void Tmp95Cpu::flush()
{
	m_flush_pending = false;
	memset(m_fast, 0, sizeof(m_fast));
	m_blocks.clear();
	jit_flush_code();
	m_code_flags.clear();
	memset(m_space.code_page, 0, sizeof(m_space.code_page));
	m_has_code = false;
}

uint8_t *Tmp95Cpu::host_ptr(uint32_t a)
{
	a &= 0xffffff;
	u8 *p = m_space.page[a >> 12];
	return p ? p + (a & 0xfff) : nullptr;
}

uint8_t Tmp95Cpu::peek(uint32_t a)
{
	const uint8_t *p = host_ptr(a);
	return p ? *p : 0;
}

static const uint8_t *chunk_of(const uint8_t *host) { return (const uint8_t *)((uintptr_t)host & ~uintptr_t(0xff)); }

void Tmp95Cpu::mark_code(uint32_t a, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		const uint8_t *h = host_ptr(a + i);
		if (!h)
			continue;
		const uint8_t *page = (const uint8_t *)((uintptr_t)h & ~uintptr_t(0xfff));
		auto &flags = m_code_flags[page];
		if (flags.empty())
			flags.assign(16, 0);
		if (flags[((uintptr_t)h >> 8) & 15])
			continue;
		flags[((uintptr_t)h >> 8) & 15] = 1;
		// every space page backed by that host page (mirrors) now reports its writes
		for (int pg = 0; pg < 0x1000; pg++)
			if (m_space.page[pg] && (const uint8_t *)((uintptr_t)m_space.page[pg] & ~uintptr_t(0xfff)) == page)
				m_space.code_page[pg] = 1;
	}
	m_has_code = true;
}

void Tmp95Cpu::code_written(const uint8_t *host)
{
	if (!m_has_code)
		return;
	for (const uint8_t *h : { host, host + 1 }) {
		auto it = m_code_flags.find((const uint8_t *)((uintptr_t)h & ~uintptr_t(0xfff)));
		if (it != m_code_flags.end() && it->second[((uintptr_t)h >> 8) & 15]) {
			m_flush_pending = true; // ponytail: the whole cache; code is rewritten only when a program is uploaded
			return;
		}
	}
	(void)chunk_of;
}

// ---- decoder: MAME's execute_op / prefix handlers / prepare_operands, the fetches done here once ----
bool Tmp95Cpu::decode(uint32_t pc, Desc &d)
{
	memset(&d, 0, sizeof(d));
	d.pc = pc;
	// MAME's prepare_operands runs for real against this sandbox: operand fields set to markers, m_pc on the fetch
	// position; what it leaves is the decoded operand
	const PAIR save_pc = m_pc, save_imm1 = m_imm1, save_imm2 = m_imm2, save_ea1 = m_ea1, save_ea2 = m_ea2;
	const uint16_t save_reg1 = m_reg1, save_reg2 = m_reg2;
	const uint8_t save_op = m_op;
	const int save_cycles = m_cycles;
	const bool save_clear = m_prefetch_clear;
	const uint8_t save_index = m_prefetch_index;
	uint8_t save_prefetch[4];
	memcpy(save_prefetch, m_prefetch, sizeof(save_prefetch));

	constexpr uint32_t MARK = 0xdeadbeefu;
	constexpr uint16_t MARK16 = 0xfffe;
	m_imm1.d = m_imm2.d = m_ea1.d = m_ea2.d = MARK;
	m_reg1 = m_reg2 = MARK16;
	m_cycles = 0;
	m_pc.d = pc;
	m_prefetch_clear = true;
	bool ok = true;
	int extra = 0;
	// (MAME's RDOP is inline in tlcs900.cpp: the same byte, with its prefetch queue refilled at the next RDOP)
	auto fetch = [&] () { const uint8_t v = RDMEM(m_pc.d); m_pc.d++; m_prefetch_clear = true; return v; };

	const tlcs900inst (*table)[256] = m_mnemonic;
	const tlcs900inst *inst = nullptr;
	for (int depth = 0; ; depth++) {
		m_op = fetch();
		inst = &(*table)[m_op];
		const int before = m_cycles;
		prepare_operands(*inst);
		if (m_cycles != before) // (it never does in MAME; a decoder that did would need a different model)
			ok = false;
		extra += inst->cycles;
		const ophandler f = inst->opfunc;
		const uint8_t op = m_op;
		// (MAME's prefix handlers: an addressing mode, then execute_op on the next table)
		if (f == &Tmp95Cpu::op_80 || f == &Tmp95Cpu::op_90) {
			m_reg1 = get_reg32_current_sel(op - 1);
			m_reg2 = get_reg32_current_sel(op);
			d.ea_kind = EA_CUR32, d.ea_sel = op;
			table = f == &Tmp95Cpu::op_80 ? m_mnemonic_80 : m_mnemonic_90;
		} else if (f == &Tmp95Cpu::op_88) {
			m_reg1 = get_reg32_current_sel(op - 1);
			m_reg2 = get_reg32_current_sel(op);
			d.ea_kind = EA_CUR32, d.ea_sel = op, d.disp = int8_t(fetch());
			extra += tlcs900_mem_index_cycles();
			table = m_mnemonic_80;
		} else if (f == &Tmp95Cpu::op_A0 || f == &Tmp95Cpu::op_B0) {
			d.ea_kind = EA_CUR32, d.ea_sel = op;
			table = f == &Tmp95Cpu::op_A0 ? m_mnemonic_a0 : m_mnemonic_b0;
		} else if (f == &Tmp95Cpu::op_98 || f == &Tmp95Cpu::op_A8 || f == &Tmp95Cpu::op_B8) {
			d.ea_kind = EA_CUR32, d.ea_sel = op, d.disp = int8_t(fetch());
			extra += tlcs900_mem_index_cycles();
			table = f == &Tmp95Cpu::op_98 ? m_mnemonic_98 : f == &Tmp95Cpu::op_A8 ? m_mnemonic_a0 : m_mnemonic_b8;
		} else if (f == &Tmp95Cpu::op_C0 || f == &Tmp95Cpu::op_D0 || f == &Tmp95Cpu::op_E0 ||
			f == &Tmp95Cpu::op_F0) {
			const bool d0 = f == &Tmp95Cpu::op_D0;
			switch (op & 0x07) {
			case 0x00: m_ea2.d = fetch(); extra += tlcs900_mem_absolute_8_cycles(); break;
			case 0x01: { uint32_t a = fetch(); a |= fetch() << 8; m_ea2.d = a; extra += tlcs900_mem_absolute_16_cycles(); break; }
			case 0x02: { uint32_t a = fetch(); a |= fetch() << 8; a |= fetch() << 16; m_ea2.d = a; extra += tlcs900_mem_absolute_24_cycles(); break; }
			case 0x03: {
				const uint8_t m = fetch();
				switch (m & 0x03) {
				case 0x00: d.ea_kind = EA_REG32, d.ea_sel = m; extra += tlcs900_mem_gpr_indirect_cycles(); break;
				case 0x01: { uint16_t x = fetch(); x |= fetch() << 8; d.ea_kind = EA_REG32, d.ea_sel = m, d.disp = int16_t(x); extra += tlcs900_mem_gpr_index_cycles(); break; }
				case 0x02: ok = false; break; // (illegal: MAME leaves m_ea2 as it was)
				case 0x03:
					if (m == 0x03 || m == 0x07) {
						d.ea_kind = m == 0x03 ? EA_REG32_R8 : EA_REG32_R16;
						d.ea_sel = fetch();
						d.ea_sel2 = fetch();
						extra += tlcs900_mem_gpr_reg_index_cycles();
					} else if (m == 0x13) {
						uint16_t x = fetch();
						x |= fetch() << 8;
						m_ea2.d = m_pc.d + int16_t(x);
						extra += d0 ? tlcs900_mem_gpr_reg_index_cycles() : tlcs900_mem_gpr_index_cycles();
					} else
						ok = false;
					break;
				}
				break;
			}
			case 0x04: case 0x05: {
				const uint8_t m = fetch();
				d.ea_kind = (op & 7) == 4 ? EA_PREDEC : EA_POSTINC, d.ea_sel = m, d.ea_step = uint8_t(1 << (m & 0x03));
				extra += tlcs900_mem_indirect_prepost_cycles();
				break;
			}
			default: ok = false; break;
			}
			table = f == &Tmp95Cpu::op_C0 ? m_mnemonic_c0 : d0 ? m_mnemonic_d0 : f == &Tmp95Cpu::op_E0 ? m_mnemonic_e0 : m_mnemonic_f0;
		} else if (f == &Tmp95Cpu::oC8 || f == &Tmp95Cpu::oD8 || f == &Tmp95Cpu::op_E8) {
			if (BIT(op, 3))
				m_reg2 = f == &Tmp95Cpu::oC8 ? get_reg8_current_sel(op) : f == &Tmp95Cpu::oD8 ? get_reg16_current_sel(op) : get_reg32_current_sel(op);
			else
				m_reg2 = fetch();
			table = f == &Tmp95Cpu::oC8 ? m_mnemonic_c8 : f == &Tmp95Cpu::oD8 ? m_mnemonic_d8 : m_mnemonic_e8;
		} else
			break;
		if (depth > 4) {
			ok = false;
			break;
		}
	}
	d.fn = inst->opfunc;
	d.op = m_op;
	d.pc_next = m_pc.d;
	d.len = m_pc.d - pc;
	d.cycles = extra;
	if (m_reg1 != MARK16) d.reg1 = m_reg1, d.set |= SET_REG1;
	if (m_reg2 != MARK16) d.reg2 = m_reg2, d.set |= SET_REG2;
	if (m_imm1.d != MARK) d.imm1 = m_imm1.d, d.set |= SET_IMM1;
	if (m_imm2.d != MARK) d.imm2 = m_imm2.d, d.set |= SET_IMM2;
	// a register-based address is known only when the instruction runs: p_M first operands copy it
	if (d.ea_kind != EA_NONE && inst->operand1 == kP_M)
		d.set |= EA1_FROM_EA2;
	else if (m_ea1.d != MARK)
		d.ea1 = m_ea1.d, d.set |= SET_EA1;
	if (m_ea2.d != MARK) {
		if (d.ea_kind != EA_NONE) // (a register address overwritten by a p_M8 / p_M16 operand: left to the interpreter)
			ok = false;
		d.ea2 = m_ea2.d, d.set |= SET_EA2;
	}
	if (inst->operand1 == kP_R && !(d.set & SET_REG2)) // (m_reg1 = m_reg2 of an earlier instruction)
		ok = false;
	// handlers that fetch for themselves, or change the decoding context: run by the interpreter
	if (d.fn == &Tmp95Cpu::op_LDX || d.fn == &Tmp95Cpu::op_DB)
		ok = false;

	m_pc = save_pc, m_imm1 = save_imm1, m_imm2 = save_imm2, m_ea1 = save_ea1, m_ea2 = save_ea2;
	m_reg1 = save_reg1, m_reg2 = save_reg2, m_op = save_op, m_cycles = save_cycles;
	m_prefetch_clear = save_clear, m_prefetch_index = save_index;
	memcpy(m_prefetch, save_prefetch, sizeof(save_prefetch));
	return ok;
}

bool Tmp95Cpu::ends_block(ophandler f)
{
	using T = Tmp95Cpu;
	static const T::ophandler k[] = {
		&T::op_CALLI, &T::op_CALLM, &T::op_CALR, &T::op_JPI, &T::op_JPM, &T::op_JR, &T::op_JRL, &T::op_RET,
		&T::op_RETCC, &T::op_RETD, &T::op_RETI, &T::op_DJNZB, &T::op_DJNZW, &T::op_SWI, &T::op_SWI900, &T::op_HALT,
		&T::op_EI, &T::op_LDF, &T::op_INCF, &T::op_DECF, &T::op_POPWSR,
	};
	for (auto x : k)
		if (f == x)
			return true;
	return false;
}

Tmp95Cpu::Block *Tmp95Cpu::compile(uint32_t pc)
{
	auto b = std::make_unique<Block>();
	uint32_t a = pc;
	for (int n = 0; n < 64; n++) {
		if (!host_ptr(a) || !host_ptr(a + 15)) // (code outside the mapped RAM: interpreted)
			break;
		Desc d;
		if (!decode(a, d))
			break;
		b->d.push_back(d);
		mark_code(a, d.len);
		a = d.pc_next;
		if (ends_block(d.fn))
			break;
	}
	if (b->d.empty())
		return nullptr;
	Block *r = b.get();
	if (mode == MODE_JIT)
		jit_compile(*r); // (on failure the block runs through run_block)
	m_blocks[(uint64_t(m_regbank) << 32) | pc] = std::move(b);
	return r;
}

Tmp95Cpu::Block *Tmp95Cpu::get_block(uint32_t pc)
{
	const uint32_t key = (pc & 0xffffff) | uint32_t(m_regbank) << 24 | 0x80000000u;
	FastEntry &e = m_fast[(pc ^ (pc >> 13)) & 8191];
	if (e.key == key)
		return e.b;
	auto it = m_blocks.find((uint64_t(m_regbank) << 32) | pc);
	Block *b = it != m_blocks.end() ? it->second.get() : compile(pc);
	if (b)
		e = { key, b };
	return b;
}

// ---- execution ----
void Tmp95Cpu::prepare_ea(const Desc &d)
{
	switch (d.ea_kind) {
	case EA_NONE: break;
	case EA_CUR32: m_ea2.d = get_reg32_current(d.ea_sel) + d.disp; break;
	case EA_REG32: m_ea2.d = get_reg32(d.ea_sel) + d.disp; break;
	case EA_REG32_R8: m_ea2.d = get_reg32(d.ea_sel) + int8_t(get_reg8(d.ea_sel2)); break;
	case EA_REG32_R16: m_ea2.d = get_reg32(d.ea_sel) + int16_t(get_reg16(d.ea_sel2)); break;
	case EA_PREDEC: { uint32_t &r = get_reg32(d.ea_sel); r -= d.ea_step; m_ea2.d = r; break; }
	case EA_POSTINC: { uint32_t &r = get_reg32(d.ea_sel); m_ea2.d = r; r += d.ea_step; break; }
	}
}

int Tmp95Cpu::run_block(const Block &b)
{
	const int bank = m_regbank;
	m_cycles = 0;
	static const bool prof = getenv("RS10_TLCS_PROF") != nullptr;
	if (prof && !s_prof)
		s_prof = new std::unordered_map<uint32_t, uint64_t>();
	for (const Desc &d : b.d) {
		if (prof)
			(*s_prof)[d.pc]++;
		m_pc.d = d.pc_next;
		m_op = d.op;
		const uint8_t s = d.set;
		if (s & SET_REG1) m_reg1 = d.reg1;
		if (s & SET_REG2) m_reg2 = d.reg2;
		if (s & SET_IMM1) m_imm1.d = d.imm1;
		if (s & SET_IMM2) m_imm2.d = d.imm2;
		if (s & SET_EA2) m_ea2.d = d.ea2;
		prepare_ea(d);
		if (s & SET_EA1) m_ea1.d = d.ea1;
		if (s & EA1_FROM_EA2) m_ea1.d = m_ea2.d;
		(this->*d.fn)();
		m_cycles += d.cycles;
		if (m_pc.d != d.pc_next || m_check_irqs || m_halted || m_regbank != bank || m_flush_pending)
			break;
	}
	m_prefetch_clear = true;
	return m_cycles;
}

void Tmp95Cpu::step_interp()
{
	const int left = m_icount;
	m_icount = 1;
	execute_run();
	m_icount = left - (1 - m_icount);
}

// RS10_TLCS_VERIFY: the decoder's operands for the instruction at pc, compared with what MAME's own decode leaves
void Tmp95Cpu::verify_step()
{
	Desc d;
	const uint32_t pc = m_pc.d;
	const bool can = !m_check_irqs && !m_halted && host_ptr(pc) && decode(pc, d);
	uint32_t ea2 = 0;
	if (can && d.ea_kind != EA_NONE) { // (computed from the registers before the instruction, without the pre/post step)
		switch (d.ea_kind) {
		case EA_CUR32: ea2 = get_reg32_current(d.ea_sel) + d.disp; break;
		case EA_REG32: ea2 = get_reg32(d.ea_sel) + d.disp; break;
		case EA_REG32_R8: ea2 = get_reg32(d.ea_sel) + int8_t(get_reg8(d.ea_sel2)); break;
		case EA_REG32_R16: ea2 = get_reg32(d.ea_sel) + int16_t(get_reg16(d.ea_sel2)); break;
		case EA_PREDEC: ea2 = get_reg32(d.ea_sel) - d.ea_step; break;
		case EA_POSTINC: ea2 = get_reg32(d.ea_sel); break;
		}
	}
	m_reg1 = m_reg2 = 0xfffe;
	m_ea2.d = 0xdeadbeef;
	step_interp();
	if (!can || ends_block(d.fn))
		return;
	static unsigned bad;
	auto report = [&] (const char *what, uint32_t want, uint32_t got) {
		if (bad++ < 200)
			fprintf(stderr, "tlcs verify: pc %06x %s: decoder %x, MAME %x\n", pc, what, want, got);
	};
	if (m_pc.d != d.pc_next && !ends_block(d.fn))
		report("next pc", d.pc_next, m_pc.d);
	if (d.ea_kind != EA_NONE && m_ea2.d != ea2)
		report("ea2", ea2, m_ea2.d);
	if ((d.set & SET_EA2) && m_ea2.d != d.ea2)
		report("ea2 static", d.ea2, m_ea2.d);
	if ((d.set & SET_REG2) && m_reg2 != d.reg2)
		report("reg2", d.reg2, m_reg2);
	if ((d.set & SET_REG1) && m_reg1 != d.reg1)
		report("reg1", d.reg1, m_reg1);
}

void Tmp95Cpu::run(int cycles)
{
	m_icount = cycles;
	if (mode == MODE_INTERP) {
		execute_run();
		return;
	}
	// the on-chip timers, A/D converter and micro-DMA catch up every few blocks (ponytail: ~64 cycles, 1.3 us at
	// 50 MHz; MAME's interpreter does it after every instruction)
	int pending = 0;
	auto catch_up = [&] {
		m_cycles = pending;
		pending = 0;
		tlcs900_handle_ad();
		tlcs900_handle_timers();
		const int before = m_cycles;
		tlcs900_check_hdma();
		m_icount -= m_cycles - before; // (a micro-DMA transfer's cycles)
	};
	while (m_icount > 0) {
		if (mode == MODE_VERIFY) {
			verify_step();
			continue;
		}
		// interrupt dispatch, the instruction after EI / RETI, HALT: as the interpreter, one instruction at a time
		if (m_check_irqs || m_halted || m_irq_inhibit) {
			if (pending)
				catch_up();
			step_interp();
			continue;
		}
		if (m_flush_pending)
			flush();
		const Block *b = get_block(m_pc.d);
		if (!b) {
			if (pending)
				catch_up();
			step_interp();
			continue;
		}
		if (b->code) {
			m_cycles = 0;
			m_jit_bank = m_regbank;
			b->code(this);
			m_prefetch_clear = true;
		} else
			run_block(*b);
		pending += m_cycles;
		m_icount -= m_cycles;
		if (pending >= 64 || m_check_irqs || m_halted || m_icount <= 0)
			catch_up();
	}
}

// ---- tests ----
Tmp95Cpu::State Tmp95Cpu::test_state()
{
	State s;
	memset(&s, 0, sizeof(s));
	for (int i = 0; i < 4; i++)
		s.xwa[i] = m_xwa[i].d, s.xbc[i] = m_xbc[i].d, s.xde[i] = m_xde[i].d, s.xhl[i] = m_xhl[i].d;
	s.xix = m_xix.d, s.xiy = m_xiy.d, s.xiz = m_xiz.d, s.xssp = m_xssp.d, s.xnsp = m_xnsp.d;
	s.pc = m_pc.d, s.sr = m_sr.d, s.f2 = m_f2.d, s.regbank = m_regbank, s.cycles = m_cycles;
	return s;
}

void Tmp95Cpu::test_set_state(const State &s)
{
	for (int i = 0; i < 4; i++)
		m_xwa[i].d = s.xwa[i], m_xbc[i].d = s.xbc[i], m_xde[i].d = s.xde[i], m_xhl[i].d = s.xhl[i];
	m_xix.d = s.xix, m_xiy.d = s.xiy, m_xiz.d = s.xiz, m_xssp.d = s.xssp, m_xnsp.d = s.xnsp;
	m_pc.d = s.pc, m_sr.d = s.sr, m_f2.d = s.f2, m_regbank = s.regbank, m_cycles = s.cycles;
	m_check_irqs = 0, m_halted = 0, m_irq_inhibit = false, m_prefetch_clear = true;
}

void Tmp95Cpu::test_step(bool jit)
{
	m_cycles = 0;
	if (!jit) {
		// (MAME's execute_run for one instruction, minus the timers and DMA the block path runs after the block)
		m_prefetch_clear = true;
		execute_op(*m_mnemonic);
		return;
	}
	Block b;
	Desc d;
	if (!decode(m_pc.d, d)) {
		m_prefetch_clear = true;
		execute_op(*m_mnemonic);
		return;
	}
	b.d.push_back(d);
	m_jit_bank = m_regbank;
	if (jit_compile(b))
		b.code(this);
	else
		run_block(b);
	jit_flush_code();
}
