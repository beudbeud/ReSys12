// ReSys12: Namco System 12 CDXA PCB (truckk, ujlnow, questff). Follows MAME src/mame/namco/namcos12_cdxa.cpp
// (BSD-3-Clause, windyfairy): SH7014 + C448 + ATAPI CD-ROM + LC78836M DAC. GPL-2.0-or-later.
#ifndef S12_CDXA_H
#define S12_CDXA_H

#include <stdint.h>

void s12_cdxa_reset(void);
// save states: the board (SH-2, RAMs, on-chip registers, DAC) and its drive
uint32_t s12_cdxa_state_size(void);
void s12_cdxa_state_save(void *dst);
void s12_cdxa_state_load(const void *src);
// opens the set's disc image (MAME layout <roms_dir>/<set>/<disc>.chd); 0 when found
int s12_cdxa_open_disc(const char *roms_dir, const char *set);
// main CPU side, 0x1f7c0000-0x1f7fffff; returns 0 when the address is not the board's
int s12_cdxa_r16(uint32_t a, uint16_t *v);
int s12_cdxa_w16(uint32_t a, uint16_t d);
// main CPU byte write into the shared RAM window (byte lanes kept, as MAME's mem_mask)
void s12_cdxa_w8(uint32_t a, uint8_t d);
// runs the SH2 up to the main CPU time (called from the sub CPU slices)
void s12_cdxa_run(uint32_t main_cycles);
// CD audio of the last frame as interleaved stereo at S12_CDXA_RATE, mixed by the frontend; returns sample pairs
#define S12_CDXA_RATE 37800
int s12_cdxa_audio(int16_t *out, int max_frames);
#ifdef RS12_HARNESS
#include <stdio.h>
void s12_cdxa_dump(FILE *f);
#endif

#endif
