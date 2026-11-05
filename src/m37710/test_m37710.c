// ReSys12: M37710 self-check. gcc -std=c99 -Wall -O2 test_m37710.c m37710.c -o test_m37710 && ./test_m37710
// Optional: ./test_m37710 c76.bin tes1sprog.6d also runs the C76 BIOS with a sound program for 2 emulated seconds
// (flat memory, IRQ0/IRQ2 at 60 Hz) and checks it hits no unimplemented opcode.
#include "m37710.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *mem; // 16 MB flat
static int rom_lock; // BIOS run: 0xc000-0xffff and >= 0x80000 are ROM

uint8_t m37710_read8(uint32_t a) { return mem[a]; }
uint16_t m37710_read16(uint32_t a) { return (uint16_t)(mem[a] | mem[a + 1] << 8); }
void m37710_write8(uint32_t a, uint8_t d)
{
	if (!rom_lock || ((a < 0xc000 || a > 0xffff) && a < 0x80000))
		mem[a] = d;
}
void m37710_write16(uint32_t a, uint16_t d)
{
	m37710_write8(a, (uint8_t)d);
	m37710_write8(a + 1, (uint8_t)(d >> 8));
}

static void put(uint32_t a, const uint8_t *p, size_t n) { memcpy(mem + a, p, n); }
static void step(void) { m37710.budget = 0; m37710_run(1); } // exactly one instruction (plus an interrupt entry)
static uint16_t w(uint32_t a) { return m37710_read16(a); }

static void test_ops(void)
{
	static const uint8_t prog[] = {
		0xa9, 0x34, 0x12,       // LDA #$1234
		0x42, 0xa9, 0x78, 0x56, // LDB #$5678
		0x89, 0x09, 0x00, 0x01, // MPY #$0100     A:BA = 0x1234 * 0x100
		0x42, 0x69, 0x01, 0x00, // ADCB #$0001
		0xe2, 0x30,             // SEP #$30       M=1 X=1 (high bytes kept aside in B / BB)
		0xa9, 0xf0,             // LDA #$f0
		0x69, 0x20,             // ADC #$20       carry out
		0x42, 0xa9, 0x00,       // LDB #$00
		0x89, 0x29, 0x05,       // DIV #$05       (BA:A = 0x0010) / 5
		0x8d, 0x00, 0x30,       // STA $3000
		0xc2, 0x30,             // REP #$30       A = B | A
		0x89, 0x49, 0x04, 0x00, // RLA #$0004
		0x8d, 0x02, 0x30,       // STA $3002
		0xdb,                   // STP
	};
	memset(mem, 0, 0x10000);
	put(0x8000, prog, sizeof(prog));
	mem[0xfffe] = 0x00, mem[0xffff] = 0x80;
	m37710_init();
	m37710_reset();
	assert(m37710.pc == 0x8000 && m37710.mode == 0 && m37710.s == 0x100);
	step(); assert(m37710.a == 0x1234 && m37710.flag_z && !(m37710.flag_n & 0x80));
	step(); assert(m37710.ba == 0x5678);
	step(); assert(m37710.a == 0x3400 && m37710.ba == 0x0012 && !(m37710.flag_c & 0x100));
	step(); assert(m37710.ba == 0x0013);
	step(); assert(m37710.mode == 3 && m37710.a == 0 && m37710.b == 0x3400 && m37710.ba == 0x13);
	step(); assert(m37710.a == 0xf0 && (m37710.flag_n & 0x80));
	step(); assert(m37710.a == 0x10 && (m37710.flag_c & 0x100));
	step(); assert(m37710.ba == 0);
	step(); assert(m37710.a == 3 && m37710.ba == 1 && !m37710.flag_v);
	step(); assert(mem[0x3000] == 3 && mem[0x3001] == 0);
	step(); assert(m37710.mode == 0 && m37710.a == 0x3403);
	step(); assert(m37710.a == 0x4033);
	step(); assert(w(0x3002) == 0x4033);
	step(); assert(m37710.stopped);
	assert(m37710.errors == 0);
	printf("ops: ok\n");
}

static void test_irq(void)
{
	static const uint8_t prog[] = {
		0xa9, 0x01, 0x00, // LDA #$0001
		0x8d, 0x7d, 0x00, // STA $007d   INT0 priority 1
		0x58,             // CLI
		0xcb,             // WAI
	};
	memset(mem, 0, 0x10000);
	put(0x8000, prog, sizeof(prog));
	mem[0xfffe] = 0x00, mem[0xffff] = 0x80;
	mem[0xfff4] = 0x00, mem[0xfff5] = 0x90; // INT0 vector
	mem[0x9000] = 0xdb;                     // STP
	m37710_init();
	m37710_reset();
	m37710_run(100);
	assert(m37710.stopped == 1 && m37710.pc == 0x8008); // waiting
	m37710_set_irq_line(M37710_LINE_IRQ0, 1);
	m37710_run(100);
	assert(m37710.stopped == 2 && m37710.pc == 0x9001 && m37710.ipl == 1 && m37710.flag_i);
	assert(!(m37710.line_irq & (1u << M37710_LINE_IRQ0))); // taken -> cleared
	// stack: PG, PC hi, PC lo, IPL, PS (S was 0x100)
	assert(mem[0x100] == 0 && mem[0xff] == 0x80 && mem[0xfe] == 0x08 && mem[0xfd] == 0 && m37710.s == 0xfb);
	printf("irq: ok\n");
}

static void test_timer(void)
{
	static const uint8_t prog[] = {
		0xa9, 0x63, 0x00, // LDA #99
		0x8d, 0x46, 0x00, // STA $0046   timer A0 = 99 (16-bit write)
		0xa9, 0x02, 0x00, // LDA #$0002
		0x8d, 0x75, 0x00, // STA $0075   timer A0 int priority 2 (and A1 = 0)
		0xe2, 0x20,       // SEP #$20
		0xa9, 0x01,       // LDA #$01
		0x8d, 0x40, 0x00, // STA $0040   start A0 (timer mode, f/2: 100 cycles)
		0x58,             // CLI
		0x80, 0xfe,       // BRA *
	};
	memset(mem, 0, 0x10000);
	put(0x8000, prog, sizeof(prog));
	mem[0xfffe] = 0x00, mem[0xffff] = 0x80;
	mem[0xffee] = 0x00, mem[0xffef] = 0x90;              // timer A0 vector
	put(0x9000, (const uint8_t[]){ 0xee, 0x00, 0x31, 0x40 }, 4); // INC $3100 / RTI
	m37710_init();
	m37710_reset();
	m37710_run(10000);
	const int n = mem[0x3100];
	assert(n >= 98 && n <= 100);
	printf("timer: ok (%d irqs in 10000 cycles)\n", n);
}

static long load(const char *path, uint32_t at, long max)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	const long n = (long)fread(mem + at, 1, (size_t)max, f);
	fclose(f);
	return n;
}

static void test_bios(const char *bios, const char *sprog)
{
	memset(mem, 0, 0x1000000);
	if (load(bios, 0xc000, 0x4000) != 0x4000 || load(sprog, 0x80000, 0x80000) <= 0) {
		printf("bios: cannot load %s / %s\n", bios, sprog);
		exit(1);
	}
	memcpy(mem + 0x200000, mem + 0x80000, 0x80000);
	memcpy(mem + 0x280000, mem + 0x80000, 0x80000);
	rom_lock = 1;
	m37710_init();
	m37710_reset();
	for (int f = 0; f < 120; f++) {
		m37710_run(8467200 / 60);
		m37710_set_irq_line(M37710_LINE_IRQ0, 1);
		m37710_set_irq_line(M37710_LINE_IRQ2, 1);
	}
	printf("bios: pc %06x, errors %u (last at %06x), stopped %u\n", (unsigned)m37710_pc(), (unsigned)m37710.errors,
		(unsigned)m37710.error_pc, (unsigned)m37710.stopped);
	assert(m37710.errors == 0 && m37710.stopped == 0);
	printf("bios: ok\n");
}

int main(int argc, char **argv)
{
	mem = calloc(1, 0x1000000 + 4);
	test_ops();
	test_irq();
	test_timer();
	if (argc > 2)
		test_bios(argv[1], argv[2]);
	free(mem);
	return 0;
}
