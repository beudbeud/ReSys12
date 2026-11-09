// ReSys10: hooks from the pcsx_rearmed core into the System 10 board (src/s10). GPL-2.0-or-later.
#ifndef S10_BUS_H
#define S10_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0x1f000000-0x1fffffff in kuseg/kseg0/kseg1 that the core does not handle itself (RAM, scratchpad, 0x1f801000 hw regs)
#define S10_IS_BOARD(a) ((((a) & 0x1f000000u) == 0x1f000000u) && (((a) >> 29) == 0 || ((a) >> 29) == 4 || ((a) >> 29) == 5))

// phys = address & 0x1fffffff
uint8_t s10_read8(uint32_t phys);
uint16_t s10_read16(uint32_t phys);
uint32_t s10_read32(uint32_t phys);
void s10_write8(uint32_t phys, uint32_t v);
void s10_write16(uint32_t phys, uint32_t v);
void s10_write32(uint32_t phys, uint32_t v);

// called from psxMemReset: the boot program (unscrambled from the NAND) into psxR
void s10_mem_reset(void);
// called from psxReset after the CPU reset: board registers, NAND chips, decrypter, I/O timer
void s10_cpu_reset(void);
// DMA channel 5 (NAND PIO DMA) and its completion event (PSXINT_S12DMA)
void s10_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr);
void s10_dma5_done(void);
// scheduled event (PSXINT_S12): the 100 Hz I/O update interrupt (IRQ 10)
void s10_event(void);
// screen VBLANK edge from the root counters (1 = start, 0 = end)
void s10_vblank(int state);

#ifdef __cplusplus
}
#endif

#endif
