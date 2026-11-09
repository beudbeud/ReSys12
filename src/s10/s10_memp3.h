// ReSys10: System 10 MEM(P3) board: sub CPU + LC82310 MP3 decoder (the NAND side is MEM(N)). See s10_memp3.cpp.
#ifndef S10_MEMP3_H
#define S10_MEMP3_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t s10_p3_analog[4]; // MAME P3_ANALOG1-4, 10 bits (g13jnr: gun Y, gun X)
extern uint16_t s10_p3_dipsw;     // MEMP3_DIPSW, active low
extern int s10_p3_clock_pct;      // the sub CPU's speed, % of its 50.7456 MHz (a frontend setting)

void s10_memp3_attach(int on);
int s10_memp3_active(void);
void s10_memp3_reset(uint32_t now);
void s10_memp3_sync(uint32_t now);
// main CPU registers, offset from 0x1f300000; 0 = not a MEM(P3) register
int s10_memp3_r16(uint32_t offset, uint32_t now, uint16_t *v);
int s10_memp3_w16(uint32_t offset, uint16_t data, uint32_t now);
// the MP3 decoder's output mixed into a 44100 Hz stereo buffer
void s10_memp3_dump(FILE *f);
void s10_memp3_render(int16_t *out, int frames);
size_t s10_memp3_state_size(void);
void s10_memp3_state_save(void *dst);
void s10_memp3_state_load(const void *src, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif
