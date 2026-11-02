// ReSys12: Hitachi SH-2 interpreter (big-endian, SH7014 core). Instruction semantics follow the SH-1/SH-2
// programming manual and Yabause sh2int.c (GPL-2.0-or-later). GPL-2.0-or-later.
#ifndef SH2_H
#define SH2_H

#include <stdint.h>

typedef struct {
	uint8_t (*rd8)(uint32_t a);
	uint16_t (*rd16)(uint32_t a);
	uint32_t (*rd32)(uint32_t a);
	void (*wr8)(uint32_t a, uint8_t v);
	void (*wr16)(uint32_t a, uint16_t v);
	void (*wr32)(uint32_t a, uint32_t v);
} SH2Bus;

typedef struct {
	uint32_t r[16];
	uint32_t pc, pr, gbr, vbr, mach, macl, sr;
	int64_t cycles;      // executed so far
	int irq_level;       // pending interrupt level (0 = none), set by the interrupt controller
	int irq_vector;
	int sleeping;
	void (*irq_ack)(int vector); // called when an interrupt is taken (optional)
	SH2Bus bus;
} SH2;

enum { SH2_T = 1u << 0, SH2_S = 1u << 1, SH2_Q = 1u << 8, SH2_M = 1u << 9 };

void sh2_reset(SH2 *s);
// runs until s->cycles reaches target
void sh2_run(SH2 *s, int64_t target);
// takes the pending interrupt now if its level is above the mask; returns 1 when taken
int sh2_check_irq(SH2 *s);

#endif
