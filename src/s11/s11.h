// ReSys12: Namco System 11 main board (CXD8530 CPU, stock PS1 GPU). Behaviour follows MAME src/mame/namco/namcos11.cpp
// and ns11prot.cpp (BSD-3-Clause, smf et al.). The set, ROM regions, EEPROM (s12.eeprom) and frontend inputs
// (s12.in) are the System 12 board's (s12.h); the C76 sound / I/O MCU is behind s11_c76.h. GPL-2.0-or-later.
#ifndef S11_H
#define S11_H

#include <stdint.h>
#include "s12_rom.h"

// called by s12_attach for a S12_M_S11 set
void s11_attach(S12Rom *rom);
// 16-bit board bus (s12.c routes 0x1f000000-0x1fffffff here on System 11); return 0 = unmapped
int s11_r16(uint32_t a, uint16_t *v);
int s11_w16(uint32_t a, uint16_t d);
int s11_w8(uint32_t a, uint8_t d);
void s11_mem_reset(void);  // from s12_mem_reset: bank windows
void s11_cpu_reset(void);  // from s12_cpu_reset: keycus, C76
void s11_event(void);      // PSXINT_S12: C76 catch-up
void s11_vblank(int state);
// once a frame, after the frontend filled s12.in: the C76 input ports
void s11_update_inputs(void);
uint32_t s11_state_size(void);
void s11_state_save(void *dst);
void s11_state_load(const void *src);

#endif
