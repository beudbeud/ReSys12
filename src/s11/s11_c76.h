// Namco System 11 C76 sound / I/O MCU (Mitsubishi M37702, 16.9344 MHz): the interface between the main board
// (src/s11/s11.c) and the C76 subsystem (src/s11/s11_c76.c, M37710 core in src/m37710). MAME namcos11.cpp c76_map:
//   0x001000-0x001007 inputs (PLAYER4, SWITCH, PLAYER1, PLAYER2), 0x002000-0x002fff C352,
//   0x004000-0x00bfff shared RAM (main CPU 0x1fa04000-0x1fa0ffff, 16-bit), 0x080000-0x0fffff / 0x200000-0x2fffff
//   sound program (sprog, 512 KB region "c76"), internal BIOS 16 KB (namcoc76.zip c76.bin); IRQ0 and IRQ2 at 60 Hz;
//   ADC0-7 = analog inputs
#ifndef S11_C76_H
#define S11_C76_H
#include <stdint.h>
#include <stddef.h>

#define S11_C76_SHARED_SIZE 0x8000 // bytes, 0x4000-0xbfff on the C76

typedef struct {
	uint16_t player1, player2, player4, sw; // the four input words as MAME's ports read them (active low)
	uint8_t adc[8];                         // ADC0-7 (0x00-0xff)
} S11C76Inputs;

// bios: 16 KB internal ROM; sprog: sound program (MAME region "c76", 512 KB, mirrored as in c76_map); wave: C352 samples
int s11_c76_init(const uint8_t *bios, size_t bios_size, const uint8_t *sprog, size_t sprog_size,
		 const uint8_t *wave, size_t wave_size);
void s11_c76_reset(void);
uint8_t *s11_c76_shared(void);                // the shared RAM, little-endian bytes (main CPU reads 16-bit words)
void s11_c76_set_inputs(const S11C76Inputs *in);
// runs the C76 up to the main CPU cycle count `main_cycles` (33.8688 MHz main clock, a free-running count that may
// wrap, e.g. psxRegs.cycle). The M37702 runs at its 16.9344 MHz input / 2 as in MAME: 1 C76 cycle = 4 main cycles.
// The first call after s11_c76_reset only latches the count.
void s11_c76_run_until(uint32_t main_cycles);
void s11_c76_vblank(void);                    // IRQ0 and IRQ2 (60 Hz, as MAME's timers)
// sound: C352 at 25401600 / 288 = 88200 Hz (S12_SOUND_RATE), stereo interleaved L,R, rendered in step with the C76.
// Returns the pairs since the last call (~1470 per 60 Hz frame); up to 4096 are buffered, older ones are dropped.
// wave: the C352 ROM as one power-of-two image (e.g. the 4 MB tes1wave.8k file: the chip's 24-bit addresses wrap, which
// covers MAME's loads at 0 / 0x800000).
int s11_c76_sound_frame(int16_t *out, int max_pairs);
size_t s11_c76_state_size(void);
void s11_c76_state_save(void *dst);
void s11_c76_state_load(const void *src);
#endif
