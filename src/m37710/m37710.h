// license:BSD-3-Clause
// copyright-holders:R. Belmont, Karl Stenerud
// ReSys12: Mitsubishi M37702/M37710 (M7700 family) interpreter, based on MAME's m37710 by R. Belmont, Karl Stenerud
// (src/devices/cpu/m37710, BSD-3-Clause), ported to C. Single instance. On-chip peripherals of the M37702M2 map at
// 0x00-0x7f (ports, A-D, UART registers, timers A0-A4 / B0-B2, interrupt control); everything else goes to the board
// callbacks below (internal RAM and ROM included: the board owns them). Times are in CPU cycles = input clock / 2
// (MAME execute_clocks_to_cycles), so the 16.9344 MHz C76 runs 8467200 cycles per second.
#ifndef RS12_M37710_H
#define RS12_M37710_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// interrupt lines, in vector table order (MAME m37710.h)
enum {
	M37710_LINE_DMA3, M37710_LINE_DMA2, M37710_LINE_DMA1, M37710_LINE_DMA0, M37710_LINE_ADC,
	M37710_LINE_UART1XMIT, M37710_LINE_UART1RECV, M37710_LINE_UART0XMIT, M37710_LINE_UART0RECV,
	M37710_LINE_TIMERB2, M37710_LINE_TIMERB1, M37710_LINE_TIMERB0,
	M37710_LINE_TIMERA4, M37710_LINE_TIMERA3, M37710_LINE_TIMERA2, M37710_LINE_TIMERA1, M37710_LINE_TIMERA0,
	M37710_LINE_IRQ2, M37710_LINE_IRQ1, M37710_LINE_IRQ0,
	// non-maskable
	M37710_LINE_WATCHDOG, M37710_LINE_DEBUG, M37710_LINE_BRK, M37710_LINE_ZERODIV, M37710_LINE_RESET,
	M37710_INTERRUPT_MAX
};
#define M37710_MASKABLE_INTERRUPTS (M37710_INTERRUPT_MAX - 5)

typedef struct {
	// core (MAME member names without m_; all 32-bit as in MAME so the ported macros behave bit for bit)
	uint32_t a, b, ba, bb, x, y, xh, yh, s, pc, ppc, pg, dt, dpr;
	uint32_t flag_m, flag_x, flag_n, flag_v, flag_d, flag_i, flag_z, flag_c;
	uint32_t line_irq, ipl, ir, im, im2, im3, im4, stopped, source, destination;
	int mode;   // execution mode (M0X0..M1X1), picks the opcode tables
	int icount; // cycles left in the current slice
	int slice;  // length of the current slice
	int budget; // cycles still to run (negative = overrun carried over)
	int spin;   // spinning until an interrupt (MAME spin_until_interrupt)
	uint64_t total; // cycles executed before the current slice

	// peripherals
	uint8_t port_regs[11], port_dir[11];
	uint8_t ad_control, ad_sweep;
	uint16_t ad_result[8];
	uint8_t adc_in[8]; // analog inputs AN0-AN7, set by the board (MAME anN_cb)
	uint8_t uart_mode[2], uart_baud[2], uart_ctrl_reg0[2], uart_ctrl_reg1[2];
	uint8_t count_start, one_shot_start, up_down_reg;
	uint16_t timer_reg[8];
	uint8_t timer_mode[8];
	uint8_t proc_mode, watchdog_freq;
	uint8_t int_control[M37710_MASKABLE_INTERRUPTS];
	uint64_t timer_next[8]; // absolute cycle of the next expiry, 0 = not running
	uint32_t timer_reload[8];
	uint64_t ad_next;       // 0 = idle

	uint32_t errors, error_pc; // unimplemented opcodes / BRK seen (MAME logerror), last PC
} M37710State;

extern M37710State m37710;

void m37710_init(void);  // power on (MAME device_start): clears everything
void m37710_reset(void); // reset line (MAME device_reset), fetches the vector at 0xfffe through the board
// run for `cycles` CPU cycles (overrun is carried over to the next call)
void m37710_run(int cycles);
uint64_t m37710_total_cycles(void); // current time, valid inside board callbacks too
// state: 1 = assert (MAME ASSERT_LINE / HOLD_LINE: cleared when taken), 0 = clear
void m37710_set_irq_line(int line, int state);
// MAME spin_until_interrupt: ends the current slice and eats cycles until an interrupt is raised
void m37710_spin_until_interrupt(void);
static inline uint32_t m37710_pc(void) { return m37710.pg | m37710.pc; } // as MAME pc() (mid-instruction inside callbacks)

// ---- board callbacks (outside 0x00-0x7f; a is 24-bit) ----
uint8_t m37710_read8(uint32_t a);
void m37710_write8(uint32_t a, uint8_t d);
uint16_t m37710_read16(uint32_t a);          // a even
void m37710_write16(uint32_t a, uint16_t d); // a even

#ifdef __cplusplus
}
#endif

#endif
