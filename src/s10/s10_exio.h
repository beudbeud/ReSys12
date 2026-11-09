// ReSys10: System 10 EXIO board (TMP95C061 + I/O), the part behind the main CPU's 0x1fe00000 window. See s10_exio.cpp.
#ifndef S10_EXIO_H
#define S10_EXIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t s10_exio_analog[8]; // MAME EXIO_ANALOG1-8, 10 bits (the frontend fills them)
extern uint8_t s10_exio_trackball[8]; // nflclsfb: the four trackballs' X / Y counters (the frontend moves them)

void s10_exio_attach(int on);   // at load: makes (or drops) the board
int s10_exio_active(void);
void s10_exio_dump(FILE *f);
void s10_exio_reset(uint32_t now); // board reset: the CPU waits for the main CPU to start it
void s10_exio_sync(uint32_t now); // run the EXIO CPU up to the main CPU's time (psxRegs.cycle)
uint16_t s10_exio_r16(uint32_t offset, uint32_t now); // offset from 0x1fe00000
void s10_exio_w16(uint32_t offset, uint16_t data, uint32_t now);
size_t s10_exio_state_size(void);
void s10_exio_state_save(void *dst);
void s10_exio_state_load(const void *src, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif
