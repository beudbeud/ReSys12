// ReSys12: SH-2 interpreter self-check. cc -O2 -Isrc/sh2 src/sh2/sh2.c src/sh2/test_sh2.c -o /tmp/test_sh2 && /tmp/test_sh2
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sh2.h"

static uint8_t mem[0x10000];
static uint8_t rd8(uint32_t a) { return mem[a & 0xffff]; }
static uint16_t rd16(uint32_t a) { a &= 0xfffe; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static uint32_t rd32(uint32_t a) { return (uint32_t)rd16(a) << 16 | rd16(a + 2); }
static void wr8(uint32_t a, uint8_t v) { mem[a & 0xffff] = v; }
static void wr16(uint32_t a, uint16_t v) { a &= 0xfffe; mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
static void wr32(uint32_t a, uint32_t v) { wr16(a, (uint16_t)(v >> 16)); wr16(a + 2, (uint16_t)v); }

// loads ops at 0x100 (reset vector -> 0x100, stack 0x8000), runs n instructions
static SH2 run(const uint16_t *ops, int count, int n, void (*setup)(SH2 *))
{
	memset(mem, 0, sizeof(mem));
	wr32(0, 0x100);
	wr32(4, 0x8000);
	for (int i = 0; i < count; i++)
		wr16(0x100 + (uint32_t)i * 2, ops[i]);
	SH2 s = { .bus = { rd8, rd16, rd32, wr8, wr16, wr32 } };
	sh2_reset(&s);
	if (setup)
		setup(&s);
	for (int i = 0; i < n; i++)
		sh2_run(&s, s.cycles + 1);
	return s;
}

static void div_setup(SH2 *s) { s->r[1] = 1000; s->r[0] = 7; }
static void addc_setup(SH2 *s) { s->r[1] = 0xffffffffu; s->r[2] = 0; s->r[3] = 1; s->r[4] = 0; s->sr &= ~SH2_T; }
static void mac_setup(SH2 *s)
{
	s->r[1] = 0x200; s->r[2] = 0x300;
	wr32(0x200, (uint32_t)-3); wr32(0x204, 5); wr32(0x300, 7); wr32(0x304, 11);
}

int main(void)
{
	// 1000 / 7 = 142: SHLL16 R0; DIV0U; 16 x DIV1 R0,R1; ROTCL R1; EXTU.W R1,R1
	uint16_t div[20] = { 0x4028, 0x0019 };
	for (int i = 0; i < 16; i++)
		div[2 + i] = 0x3104;
	div[18] = 0x4124;
	div[19] = 0x611d;
	assert(run(div, 20, 20, div_setup).r[1] == 142);

	// 64-bit add {R2:R1} + {R4:R3}: CLRT; ADDC R3,R1; ADDC R4,R2 -> carry goes into R2
	const uint16_t addc[] = { 0x0008, 0x313e, 0x324e };
	const SH2 a = run(addc, 3, 3, addc_setup);
	assert(a.r[1] == 0 && a.r[2] == 1 && !(a.sr & SH2_T));

	// MOV #5,R2; BRA +2; ADD #1,R2 (delay slot); MOV #0,R2 (skipped); MOV R2,R3
	const uint16_t bra[] = { 0xe205, 0xa001, 0x7201, 0xe200, 0x6323 };
	assert(run(bra, 5, 3, NULL).r[3] == 6);

	// CLRMAC; MAC.L @R1+,@R2+ twice: -3*7 + 5*11 = 34
	const uint16_t mac[] = { 0x0028, 0x021f, 0x021f };
	const SH2 m = run(mac, 3, 3, mac_setup);
	assert(m.macl == 34 && m.mach == 0);

	puts("sh2 ok");
	return 0;
}
