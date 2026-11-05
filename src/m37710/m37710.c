// license:BSD-3-Clause
// copyright-holders:R. Belmont, Karl Stenerud
// ReSys12: Mitsubishi M37702/M37710 interpreter, based on MAME's m37710 by R. Belmont, Karl Stenerud
// (m37710.cpp, m37710cm.h, m37710il.h; the opcodes are m37710op.inc, MAME's m37710op.h). See m37710.h.
#include "m37710.h"

#include <string.h>

M37710State m37710;
#define M m37710

// ---- m37710cm.h ----
#define REG_A M.a
#define REG_B M.b
#define REG_BA M.ba
#define REG_BB M.bb
#define REG_X M.x
#define REG_Y M.y
#define REG_XH M.xh
#define REG_YH M.yh
#define REG_S M.s
#define REG_PC M.pc
#define REG_PPC M.ppc
#define REG_PG M.pg
#define REG_DT M.dt
#define REG_DPR M.dpr
#define FLAG_M M.flag_m
#define FLAG_X M.flag_x
#define FLAG_N M.flag_n
#define FLAG_V M.flag_v
#define FLAG_D M.flag_d
#define FLAG_I M.flag_i
#define FLAG_Z M.flag_z
#define FLAG_C M.flag_c
#define LINE_IRQ M.line_irq
#define REG_IR M.ir
#define REG_IM M.im
#define REG_IM2 M.im2
#define CLOCKS M.icount
#define CPU_STOPPED M.stopped
#define SRC M.source
#define DST M.destination
#define m_ipl M.ipl

#define STOP_LEVEL_WAI 1
#define STOP_LEVEL_STOP 2
#define EXECUTION_MODE_M0X0 0
#define EXECUTION_MODE_M0X1 1
#define EXECUTION_MODE_M1X0 2
#define EXECUTION_MODE_M1X1 3

#define MAKE_INT_8(A) ((int8_t)((A) & 0xff))
#define MAKE_UINT_8(A) ((A) & 0xff)
#define MAKE_UINT_16(A) ((A) & 0xffff)

#define CLK_OP 1
#define CLK_R8 1
#define CLK_R16 2
#define CLK_R24 3
#define CLK_W8 1
#define CLK_W16 2
#define CLK_W24 3
#define CLK_RMW8 3
#define CLK_RMW16 5
#define CLK_IMPLIED 1
#define CLK_RELATIVE_8 1
#define CLK_RELATIVE_16 2
#define CLK_IMM 0
#define CLK_AI 4
#define CLK_AXI 4
#define CLK_A 2
#define CLK_AL 3
#define CLK_ALX 3
#define CLK_AX 2
#define CLK_AY 2
#define CLK_D 1
#define CLK_DI 3
#define CLK_DIY 3
#define CLK_DLI 4
#define CLK_DLIY 4
#define CLK_DX 2
#define CLK_DXI 4
#define CLK_DY 2
#define CLK_S 2
#define CLK_SIY 5
// AX and AY addressing modes take 1 extra cycle when writing
#define CLK_W_IMM 0
#define CLK_W_AI 4
#define CLK_W_AXI 4
#define CLK_W_A 2
#define CLK_W_AL 3
#define CLK_W_ALX 3
#define CLK_W_AX 3
#define CLK_W_AY 3
#define CLK_W_D 1
#define CLK_W_DI 3
#define CLK_W_DIY 3
#define CLK_W_DLI 4
#define CLK_W_DLIY 4
#define CLK_W_DX 2
#define CLK_W_DXI 4
#define CLK_W_DY 2
#define CLK_W_S 2
#define CLK_W_SIY 5
#define CLK(A) CLOCKS -= (A)
#define USE_ALL_CLKS() CLOCKS = 0

#define FLAGPOS_D 0x08
#define FLAGPOS_I 0x04
#define FLAGPOS_Z 0x02
#define FLAGPOS_M 0x20
#define FLAGPOS_X 0x10
#define MFLAG_SET FLAGPOS_M
#define MFLAG_CLEAR 0
#define XFLAG_SET FLAGPOS_X
#define XFLAG_CLEAR 0
#define VFLAG_SET 0x80
#define VFLAG_CLEAR 0
#define DFLAG_CLEAR 0
#define IFLAG_SET FLAGPOS_I
#define IFLAG_CLEAR 0
#define CFLAG_SET 0x100
#define CFLAG_CLEAR 0

#define COND_CC() (!(FLAG_C & 0x100))
#define COND_CS() (FLAG_C & 0x100)
#define COND_EQ() (!FLAG_Z)
#define COND_NE() FLAG_Z
#define COND_MI() (FLAG_N & 0x80)
#define COND_PL() (!(FLAG_N & 0x80))
#define COND_VC() (!(FLAG_V & 0x80))
#define COND_VS() (FLAG_V & 0x80)

#define VFLAG_ADD_8(S, D, R) ((S ^ R) & (D ^ R))
#define VFLAG_ADD_16(S, D, R) (((S ^ R) & (D ^ R)) >> 8)
#define VFLAG_SUB_8(S, D, R) ((S ^ D) & (R ^ D))
#define VFLAG_SUB_16(S, D, R) (((S ^ D) & (R ^ D)) >> 8)
#define CFLAG_16(A) ((A) >> 8)
#define NFLAG_8(A) (A)
#define NFLAG_16(A) ((A) >> 8)
#define CFLAG_1() ((FLAG_C >> 8) & 1)

static uint32_t rd8(uint32_t a);
static uint32_t rd16(uint32_t a);
static uint32_t rd24(uint32_t a);
static void wr8(uint32_t a, uint32_t d);
static void wr16(uint32_t a, uint32_t d);

// every access mode of MAME's tables is a plain bus access here
#define read_8_NORM(A) rd8(A)
#define read_8_IMM(A) rd8(A)
#define read_8_D(A) rd8(A)
#define read_8_A(A) rd8(A)
#define read_8_AL(A) rd8(A)
#define read_8_DX(A) rd8(A)
#define read_8_DY(A) rd8(A)
#define read_8_AX(A) rd8(A)
#define read_8_ALX(A) rd8(A)
#define read_8_AY(A) rd8(A)
#define read_8_DI(A) rd8(A)
#define read_8_DLI(A) rd8(A)
#define read_8_AI(A) rd8(A)
#define read_8_DXI(A) rd8(A)
#define read_8_DIY(A) rd8(A)
#define read_8_DLIY(A) rd8(A)
#define read_8_S(A) rd8(A)
#define read_8_SIY(A) rd8(A)
#define read_16_NORM(A) rd16(A)
#define read_16_IMM(A) rd16(A)
#define read_16_D(A) rd16(A)
#define read_16_A(A) rd16(A)
#define read_16_AL(A) rd16(A)
#define read_16_DX(A) rd16(A)
#define read_16_DY(A) rd16(A)
#define read_16_AX(A) rd16(A)
#define read_16_ALX(A) rd16(A)
#define read_16_AY(A) rd16(A)
#define read_16_DI(A) rd16(A)
#define read_16_DLI(A) rd16(A)
#define read_16_AI(A) rd16(A)
#define read_16_DXI(A) rd16(A)
#define read_16_DIY(A) rd16(A)
#define read_16_DLIY(A) rd16(A)
#define read_16_AXI(A) rd16(A)
#define read_16_S(A) rd16(A)
#define read_16_SIY(A) rd16(A)
#define read_24_A(A) rd24(A)
#define write_8_NORM(A, V) wr8(A, V)
#define write_8_D(A, V) wr8(A, V)
#define write_8_A(A, V) wr8(A, V)
#define write_8_AL(A, V) wr8(A, V)
#define write_8_DX(A, V) wr8(A, V)
#define write_8_DY(A, V) wr8(A, V)
#define write_8_AX(A, V) wr8(A, V)
#define write_8_ALX(A, V) wr8(A, V)
#define write_8_AY(A, V) wr8(A, V)
#define write_8_DI(A, V) wr8(A, V)
#define write_8_DLI(A, V) wr8(A, V)
#define write_8_DXI(A, V) wr8(A, V)
#define write_8_DIY(A, V) wr8(A, V)
#define write_8_DLIY(A, V) wr8(A, V)
#define write_8_S(A, V) wr8(A, V)
#define write_8_SIY(A, V) wr8(A, V)
#define write_16_D(A, V) wr16(A, V)
#define write_16_A(A, V) wr16(A, V)
#define write_16_AL(A, V) wr16(A, V)
#define write_16_DX(A, V) wr16(A, V)
#define write_16_DY(A, V) wr16(A, V)
#define write_16_AX(A, V) wr16(A, V)
#define write_16_ALX(A, V) wr16(A, V)
#define write_16_AY(A, V) wr16(A, V)
#define write_16_DI(A, V) wr16(A, V)
#define write_16_DLI(A, V) wr16(A, V)
#define write_16_DXI(A, V) wr16(A, V)
#define write_16_DIY(A, V) wr16(A, V)
#define write_16_DLIY(A, V) wr16(A, V)
#define write_16_S(A, V) wr16(A, V)
#define write_16_SIY(A, V) wr16(A, V)

#define OPER_8_IMM() rd8(EA_IMM8())
#define OPER_8_D() rd8(EA_D())
#define OPER_8_A() rd8(EA_A())
#define OPER_8_AL() rd8(EA_AL())
#define OPER_8_DX() rd8(EA_DX())
#define OPER_8_DY() rd8(EA_DY())
#define OPER_8_AX() rd8(EA_AX())
#define OPER_8_ALX() rd8(EA_ALX())
#define OPER_8_AY() rd8(EA_AY())
#define OPER_8_DI() rd8(EA_DI())
#define OPER_8_DLI() rd8(EA_DLI())
#define OPER_8_DXI() rd8(EA_DXI())
#define OPER_8_DIY() rd8(EA_DIY())
#define OPER_8_DLIY() rd8(EA_DLIY())
#define OPER_8_S() rd8(EA_S())
#define OPER_8_SIY() rd8(EA_SIY())
#define OPER_16_IMM() rd16(EA_IMM16())
#define OPER_16_D() rd16(EA_D())
#define OPER_16_A() rd16(EA_A())
#define OPER_16_AL() rd16(EA_AL())
#define OPER_16_DX() rd16(EA_DX())
#define OPER_16_DY() rd16(EA_DY())
#define OPER_16_AX() rd16(EA_AX())
#define OPER_16_ALX() rd16(EA_ALX())
#define OPER_16_AY() rd16(EA_AY())
#define OPER_16_DI() rd16(EA_DI())
#define OPER_16_DLI() rd16(EA_DLI())
#define OPER_16_DXI() rd16(EA_DXI())
#define OPER_16_DIY() rd16(EA_DIY())
#define OPER_16_DLIY() rd16(EA_DLIY())
#define OPER_16_S() rd16(EA_S())
#define OPER_16_SIY() rd16(EA_SIY())
#define OPER_24_IMM() rd24(EA_IMM24())
#define OPER_24_D() rd24(EA_D())

// MAME logerror: counted, the board or a test decides what to print
#define logerror(...) (M.errors++, M.error_pc = REG_PG | REG_PPC)

// ---- bus: 24-bit, 16-bit little-endian; the on-chip registers are byte-wide ----
static uint8_t sfr_r(uint32_t a);
static void sfr_w(uint32_t a, uint8_t d);

static uint32_t rd8(uint32_t a)
{
	a &= 0xffffff;
	return a < 0x80 ? sfr_r(a) : m37710_read8(a);
}

static uint32_t rd16(uint32_t a)
{
	a &= 0xffffff;
	if ((a & 1) || a < 0x80) // unaligned: two byte lanes (MAME read_word_unaligned)
		return rd8(a) | rd8(a + 1) << 8;
	return m37710_read16(a);
}

// m37710il.h read_24
static uint32_t rd24(uint32_t a)
{
	if (a & 1)
		return rd8(a) | rd16(a + 1) << 8;
	return rd16(a) | rd8(a + 2) << 16;
}

static void wr8(uint32_t a, uint32_t d)
{
	a &= 0xffffff;
	if (a < 0x80)
		sfr_w(a, (uint8_t)d);
	else
		m37710_write8(a, (uint8_t)d);
}

static void wr16(uint32_t a, uint32_t d)
{
	a &= 0xffffff;
	if ((a & 1) || a < 0x80) {
		wr8(a, d & 0xff);
		wr8(a + 1, (d >> 8) & 0xff);
	} else
		m37710_write16(a, (uint16_t)d);
}

// ---- m37710il.h ----
static inline void m37710i_push_8(uint32_t value)
{
	wr8(REG_S, value);
	REG_S = MAKE_UINT_16(REG_S - 1);
}

static inline uint32_t m37710i_pull_8(void)
{
	REG_S = MAKE_UINT_16(REG_S + 1);
	return rd8(REG_S);
}

static inline void m37710i_push_16(uint32_t value)
{
	m37710i_push_8(value >> 8);
	m37710i_push_8(value);
}

static inline uint32_t m37710i_pull_16(void)
{
	uint32_t res = m37710i_pull_8();
	return res | (m37710i_pull_8() << 8);
}

static inline uint32_t m37710i_pull_24(void)
{
	uint32_t res = m37710i_pull_8();
	res |= m37710i_pull_8() << 8;
	return res | (m37710i_pull_8() << 16);
}

static inline void m37710i_jump_16(uint32_t address) { REG_PC = MAKE_UINT_16(address); }
static inline void m37710i_jump_24(uint32_t address)
{
	REG_PG = address & 0xff0000;
	REG_PC = MAKE_UINT_16(address);
}
static inline void m37710i_branch_8(uint32_t offset) { REG_PC = MAKE_UINT_16(REG_PC + MAKE_INT_8(offset)); }
static inline void m37710i_branch_16(uint32_t offset) { REG_PC = MAKE_UINT_16(REG_PC + offset); }

static inline uint32_t m37710i_get_reg_ps(void)
{
	return (FLAG_N & 0x80) | ((FLAG_V >> 1) & 0x40) | FLAG_M | FLAG_X | FLAG_D | FLAG_I | ((!FLAG_Z) << 1) |
		((FLAG_C >> 8) & 1);
}

static inline void m37710i_set_reg_ipl(uint32_t value) { m_ipl = value & 7; }

static inline uint32_t EA_IMM8(void) { REG_PC += 1; return REG_PG | MAKE_UINT_16(REG_PC - 1); }
static inline uint32_t EA_IMM16(void) { REG_PC += 2; return REG_PG | MAKE_UINT_16(REG_PC - 2); }
static inline uint32_t EA_IMM24(void) { REG_PC += 3; return REG_PG | MAKE_UINT_16(REG_PC - 3); }
static inline uint32_t EA_D(void) { if (MAKE_UINT_8(REG_DPR)) CLK(1); return MAKE_UINT_16(REG_DPR + OPER_8_IMM()); }
static inline uint32_t EA_A(void) { return REG_DT | OPER_16_IMM(); }
static inline uint32_t EA_AL(void) { return OPER_24_IMM(); }
static inline uint32_t EA_DX(void) { return MAKE_UINT_16(REG_DPR + OPER_8_IMM() + REG_X); }
static inline uint32_t EA_DY(void) { return MAKE_UINT_16(REG_DPR + OPER_8_IMM() + REG_Y); }
static inline uint32_t EA_AX(void) { uint32_t tmp = EA_A(); if ((tmp ^ (tmp + REG_X)) & 0xff00) CLK(1); return tmp + REG_X; }
static inline uint32_t EA_ALX(void) { return EA_AL() + REG_X; }
static inline uint32_t EA_AY(void) { uint32_t tmp = EA_A(); if ((tmp ^ (tmp + REG_X)) & 0xff00) CLK(1); return tmp + REG_Y; }
static inline uint32_t EA_DI(void) { return REG_DT | OPER_16_D(); }
static inline uint32_t EA_DLI(void) { return OPER_24_D(); }
static inline uint32_t EA_AI(void) { return read_16_A(OPER_16_IMM()); }
static inline uint32_t EA_DXI(void) { return REG_DT | OPER_16_DX(); }
static inline uint32_t EA_DIY(void) { uint32_t tmp = REG_DT | OPER_16_D(); if ((tmp ^ (tmp + REG_X)) & 0xff00) CLK(1); return tmp + REG_Y; }
static inline uint32_t EA_DLIY(void) { return OPER_24_D() + REG_Y; }
static inline uint32_t EA_S(void) { return MAKE_UINT_16(REG_S + OPER_8_IMM()); }
static inline uint32_t EA_SIY(void) { return MAKE_UINT_16(read_16_SIY(REG_S + OPER_8_IMM()) + REG_Y) | REG_DT; }

// ---- interrupts (m37710.cpp) ----
static const uint16_t irq_vectors[M37710_INTERRUPT_MAX] = {
	0xffce, 0xffd0, 0xffd2, 0xffd4, 0xffd6, 0xffd8, 0xffda, 0xffdc, 0xffde, 0xffe0, 0xffe2, 0xffe4, 0xffe6, 0xffe8,
	0xffea, 0xffec, 0xffee, 0xfff0, 0xfff2, 0xfff4,
	0xfff6, 0xfff8, 0xfffa, 0xfffc, 0xfffe, // non-maskable: watchdog, DBC, BRK, zero divide, RESET
};

static void irq_line(int line, int state)
{
	if (line < 0 || line >= M37710_MASKABLE_INTERRUPTS)
		return;
	if (state) {
		LINE_IRQ |= 1u << line;
		M.int_control[line] |= 8;
	} else {
		LINE_IRQ &= ~(1u << line);
		M.int_control[line] &= (uint8_t)~8;
	}
}

static void m37710i_update_irqs(void)
{
	int curirq, pending = (int)LINE_IRQ;
	int wantedIRQ = -1;
	int curpri = 0;

	for (curirq = M37710_INTERRUPT_MAX - 1; curirq >= 0; curirq--) {
		if (pending & (1 << curirq)) {
			if (curirq < M37710_MASKABLE_INTERRUPTS) {
				int thispri = M.int_control[curirq] & 7;
				// maskable: check the level against the best candidate and the current CPU level
				if (!FLAG_I && thispri > curpri && thispri > (int)m_ipl) {
					wantedIRQ = curirq;
					curpri = thispri;
				}
			} else {
				wantedIRQ = curirq; // non-maskable always wins
				curpri = 7;
				break;
			}
		}
	}

	if (wantedIRQ != -1) {
		CPU_STOPPED &= ~STOP_LEVEL_WAI;
		irq_line(wantedIRQ, 0); // auto-clear (HOLD_LINE)
		CLK(13);
		m37710i_push_8(REG_PG >> 16);
		m37710i_push_16(REG_PC);
		m37710i_push_8(m_ipl);
		m37710i_push_8(m37710i_get_reg_ps());
		FLAG_I = IFLAG_SET;
		m_ipl = (uint32_t)curpri;
		REG_PG = 0;
		REG_PC = rd16(irq_vectors[wantedIRQ]);
	}
}

static void m37710i_interrupt_software(uint32_t vector)
{
	CLK(13);
	m37710i_push_8(REG_PG >> 16);
	m37710i_push_16(REG_PC);
	m37710i_push_8(m_ipl);
	m37710i_push_8(m37710i_get_reg_ps());
	FLAG_I = IFLAG_SET;
	REG_PG = 0;
	REG_PC = rd16(vector);
}

static inline void m37710i_set_execution_mode(uint32_t mode) { M.mode = (int)mode; }

static void m37710i_op42(void);
static void m37710i_op89(void);

// ---- opcodes: MAME's m37710op.h once per execution mode ----
#define EXECUTION_MODE EXECUTION_MODE_M0X0
#include "m37710op.inc"
#undef EXECUTION_MODE
#define EXECUTION_MODE EXECUTION_MODE_M0X1
#include "m37710op.inc"
#undef EXECUTION_MODE
#define EXECUTION_MODE EXECUTION_MODE_M1X0
#include "m37710op.inc"
#undef EXECUTION_MODE
#define EXECUTION_MODE EXECUTION_MODE_M1X1
#include "m37710op.inc"
#undef EXECUTION_MODE

static void (*const *const op_tab[4])(void) = {
	m37710i_opcodes_M0X0, m37710i_opcodes_M0X1, m37710i_opcodes_M1X0, m37710i_opcodes_M1X1 };
static void (*const *const op42_tab[4])(void) = {
	m37710i_opcodes42_M0X0, m37710i_opcodes42_M0X1, m37710i_opcodes42_M1X0, m37710i_opcodes42_M1X1 };
static void (*const *const op89_tab[4])(void) = {
	m37710i_opcodes89_M0X0, m37710i_opcodes89_M0X1, m37710i_opcodes89_M1X0, m37710i_opcodes89_M1X1 };

static void m37710i_op42(void) { op42_tab[M.mode][REG_IR](); }
static void m37710i_op89(void) { op89_tab[M.mode][REG_IR](); }

// ---- on-chip peripherals (M37702M2 map) ----
uint64_t m37710_total_cycles(void)
{
	return M.total + (uint64_t)(M.slice - M.icount);
}

// a timer or the A-D was (re)started inside a slice: end the slice here so the run loop sees the new event in time
// (MAME aborts the timeslice when a new timer becomes the first one)
static void abort_slice(void)
{
	M.slice -= M.icount;
	M.icount = 0;
}

static void recalc_timer(int timer)
{
	static const int tscales[4] = { 2, 16, 64, 512 };
	if (!((M.count_start >> timer) & 1))
		return;
	const uint32_t tval = M.timer_reg[timer];
	// HACK (MAME): ignore if timer is 8MHz (MAME slows down to a crawl)
	if (tval == 0 && (M.timer_mode[timer] & 0xc0) == 0)
		return;
	// timer mode only for groups A and B; event counter, one-shot, PWM and pulse measurement modes do nothing (as
	// in MAME)
	if ((M.timer_mode[timer] & 3) == 0) {
		const uint32_t period = (uint32_t)tscales[M.timer_mode[timer] >> 6] * (tval + 1) / 2; // clocks -> cycles
		M.timer_reload[timer] = period;
		M.timer_next[timer] = m37710_total_cycles() + period;
		abort_slice();
	}
}

static uint32_t ad_period(void) { return 57 * ((M.ad_control & 0x80) ? 2 : 4); } // 57 * 2 * (2|4) clocks

static void ad_control_w(uint8_t data)
{
	if ((data & 0x40) && !(M.ad_control & 0x40)) {
		M.ad_next = m37710_total_cycles() + 57u * ((data & 0x80) ? 2 : 4);
		abort_slice();
		if (data & 0x10)
			data &= 0xf8;
	} else if (!(data & 0x40))
		M.ad_next = 0;
	M.ad_control = data;
}

static void ad_timer_cb(void)
{
	const int line = M.ad_control & 7;
	M.ad_result[line] = M.adc_in[line];
	if (M.ad_control & 0x10)
		M.ad_control = (uint8_t)((M.ad_control & 0xf8) | ((line + 1) & 7));
	// repeat or sweep conversion
	if ((M.ad_control & 0x08) || ((M.ad_control & 0x10) && line != (M.ad_sweep & 3) * 2 + 1))
		M.ad_next = M.total + ad_period();
	else {
		// interrupt occurs only when conversion stops (no wake-up from a spin, as in MAME)
		irq_line(M37710_LINE_ADC, 1);
		M.ad_control &= 0xbf;
	}
}

// interrupt control registers 0x70-0x7f -> line
static const uint8_t ic_line[16] = {
	M37710_LINE_ADC, M37710_LINE_UART0XMIT, M37710_LINE_UART0RECV, M37710_LINE_UART1XMIT, M37710_LINE_UART1RECV,
	M37710_LINE_TIMERA0, M37710_LINE_TIMERA1, M37710_LINE_TIMERA2, M37710_LINE_TIMERA3, M37710_LINE_TIMERA4,
	M37710_LINE_TIMERB0, M37710_LINE_TIMERB1, M37710_LINE_TIMERB2, M37710_LINE_IRQ0, M37710_LINE_IRQ1,
	M37710_LINE_IRQ2 };

// ports P0-P8: P(2k) at 0x02 + 4k (data) / 0x04 + 4k (direction) on even bytes, P(2k+1) on the odd bytes up to 0x11.
// Returns the port index, *dir = direction register; -1 if a is not a port register.
static int port_of(uint32_t a, int *dir)
{
	if (a < 0x02 || a > 0x15 || ((a & 1) && a > 0x11))
		return -1;
	const uint32_t off = (a - 2) >> 1;
	*dir = (int)(off & 1);
	return (int)(off & ~1u) + (int)(a & 1);
}

static uint8_t sfr_r(uint32_t a)
{
	int dir;
	const int p = port_of(a, &dir);
	if (p >= 0) {
		if (dir)
			return M.port_dir[p];
		// ponytail: no input pins wired (the C76 has none in MAME), inputs read 0xff (MAME's default port_in_cb)
		return (uint8_t)(~M.port_dir[p] | (M.port_regs[p] & M.port_dir[p]));
	}
	if (a >= 0x20 && a <= 0x2f)
		return (uint8_t)(M.ad_result[(a - 0x20) >> 1] >> ((a & 1) * 8));
	if (a >= 0x46 && a <= 0x55)
		return (uint8_t)(M.timer_reg[(a - 0x46) >> 1] >> ((a & 1) * 8));
	if (a >= 0x56 && a <= 0x5d)
		return M.timer_mode[a - 0x56];
	if (a >= 0x70)
		return M.int_control[ic_line[a - 0x70]];
	switch (a) {
	case 0x1e: return M.ad_control;
	case 0x1f: return M.ad_sweep;
	case 0x30: return M.uart_mode[0];
	case 0x34: return M.uart_ctrl_reg0[0];
	case 0x35: return M.uart_ctrl_reg1[0];
	case 0x38: return M.uart_mode[1];
	case 0x3c: return M.uart_ctrl_reg0[1];
	case 0x3d: return M.uart_ctrl_reg1[1];
	case 0x40: return M.count_start;
	case 0x44: return M.up_down_reg & 0x1f;
	case 0x5e: return M.proc_mode & 0xf7;
	case 0x61: return M.watchdog_freq;
	}
	return 0; // ponytail: UART receive buffers read 0, no serial I/O (MAME: same)
}

static void sfr_w(uint32_t a, uint8_t d)
{
	int dir;
	const int p = port_of(a, &dir);
	if (p >= 0) {
		if (dir)
			M.port_dir[p] = d;
		else
			M.port_regs[p] = d; // ponytail: no output pins wired
		return;
	}
	if (a >= 0x46 && a <= 0x55) {
		uint16_t *r = &M.timer_reg[(a - 0x46) >> 1];
		*r = (a & 1) ? (uint16_t)((*r & 0x00ff) | d << 8) : (uint16_t)((*r & 0xff00) | d);
		return;
	}
	if (a >= 0x56 && a <= 0x5d) {
		M.timer_mode[a - 0x56] = d;
		return;
	}
	if (a >= 0x70) {
		M.int_control[ic_line[a - 0x70]] = d;
		m37710i_update_irqs(); // ponytail: INT0-2 level-sense mode not implemented (as in MAME)
		return;
	}
	switch (a) {
	case 0x1e: ad_control_w(d); break;
	case 0x1f: M.ad_sweep = d; break;
	case 0x30: M.uart_mode[0] = d; break;
	case 0x31: M.uart_baud[0] = d; break;
	case 0x34: M.uart_ctrl_reg0[0] = (uint8_t)((d & ~8) | (M.uart_ctrl_reg0[0] & 8)); break;
	case 0x35: M.uart_ctrl_reg1[0] = (uint8_t)((M.uart_ctrl_reg1[0] & ((d & 4) ? 0xfa : 0x0a)) | (d & 5)); break;
	case 0x38: M.uart_mode[1] = d; break;
	case 0x39: M.uart_baud[1] = d; break;
	case 0x3c: M.uart_ctrl_reg0[1] = (uint8_t)((d & ~8) | (M.uart_ctrl_reg0[1] & 8)); break;
	case 0x3d: M.uart_ctrl_reg1[1] = (uint8_t)((M.uart_ctrl_reg1[1] & ((d & 4) ? 0xfa : 0x0a)) | (d & 5)); break;
	case 0x40: {
		const uint8_t prev = M.count_start;
		M.count_start = d;
		for (int i = 0; i < 8; i++)
			if (((d >> i) & 1) && !((prev >> i) & 1))
				recalc_timer(i);
		break;
	}
	case 0x42: M.one_shot_start = d; break;
	case 0x44: M.up_down_reg = d; break;
	case 0x5e: M.proc_mode = d; break;
	case 0x61: M.watchdog_freq = d; break; // ponytail: watchdog never fires (MAME: same)
	}
}

// ---- API ----
void m37710_set_irq_line(int line, int state)
{
	irq_line(line, state);
	if (state)
		M.spin = 0; // MAME signal_interrupt_trigger
}

void m37710_spin_until_interrupt(void)
{
	M.spin = 1;
	abort_slice();
}

void m37710_init(void)
{
	memset(&M, 0, sizeof(M));
}

void m37710_reset(void)
{
	int i;
	memset(M.timer_next, 0, sizeof(M.timer_next));
	memset(M.timer_reload, 0, sizeof(M.timer_reload));
	CPU_STOPPED = 0;
	M.spin = 0;
	memset(M.port_dir, 0, sizeof(M.port_dir));
	M.ad_control &= 7;
	M.ad_sweep = (uint8_t)((M.ad_sweep & ~0xdc) | 3);
	M.ad_next = 0;
	for (i = 0; i < 2; i++) {
		M.uart_mode[i] = 0;
		M.uart_ctrl_reg0[i] = (uint8_t)((M.uart_ctrl_reg0[i] & 0xe0) | 8);
		M.uart_ctrl_reg1[i] = 2;
	}
	M.count_start = 0;
	M.one_shot_start &= (uint8_t)~0x1f;
	M.up_down_reg = 0;
	memset(M.timer_mode, 0, sizeof(M.timer_mode));
	M.proc_mode = 0;
	M.watchdog_freq &= (uint8_t)~1;
	for (i = 0; i <= M37710_LINE_TIMERA0; i++)
		M.int_control[i] &= (uint8_t)~0xf;
	for (i = M37710_LINE_IRQ2; i <= M37710_LINE_IRQ0; i++)
		M.int_control[i] &= (uint8_t)~0x3f;

	m_ipl = 0;
	FLAG_M = MFLAG_CLEAR;
	FLAG_X = XFLAG_CLEAR;
	FLAG_D = DFLAG_CLEAR;
	FLAG_I = IFLAG_SET;
	LINE_IRQ = 0;

	REG_DPR = 0;
	REG_PG = 0;
	REG_DT = 0;
	REG_S = (REG_S & 0xff) | 0x100;
	REG_XH = REG_X & 0xff00; REG_X &= 0xff;
	REG_YH = REG_Y & 0xff00; REG_Y &= 0xff;
	REG_B = REG_A & 0xff00; REG_A &= 0xff;
	REG_BB = REG_BA & 0xff00; REG_BA &= 0xff;
	m37710i_set_execution_mode(EXECUTION_MODE_M0X0);
	REG_PC = rd16(0xfffe);
}

static void fire_events(void)
{
	for (int i = 0; i < 8; i++)
		if (M.timer_next[i] && M.timer_next[i] <= M.total) {
			M.timer_next[i] += M.timer_reload[i];
			m37710_set_irq_line(M37710_LINE_TIMERA0 - i, 1);
		}
	if (M.ad_next && M.ad_next <= M.total) {
		M.ad_next = 0;
		ad_timer_cb();
	}
}

void m37710_run(int cycles)
{
	M.budget += cycles;
	while (M.budget > 0) {
		fire_events();
		int n = M.budget;
		for (int i = 0; i < 9; i++) { // slices end at the next timer / A-D event, as MAME's scheduler does
			const uint64_t t = i < 8 ? M.timer_next[i] : M.ad_next;
			if (t && t - M.total < (uint64_t)n)
				n = (int)(t - M.total);
		}
		if (M.spin) {
			M.total += (uint64_t)n;
			M.budget -= n;
			continue;
		}
		M.slice = M.icount = n;
		m37710i_update_irqs(); // MAME execute_run
		if (CPU_STOPPED)
			M.icount = 0; // WAI / STP: the slice passes
		else
			do {
				REG_PPC = REG_PC;
				REG_PC++;
				REG_IR = rd8(REG_PG | REG_PPC);
				op_tab[M.mode][REG_IR]();
			} while (M.icount > 0);
		const int used = M.slice - M.icount;
		M.total += (uint64_t)used;
		M.budget -= used;
		M.slice = M.icount = 0;
	}
}
