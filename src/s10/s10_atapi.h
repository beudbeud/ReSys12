// ReSys10: the Taiko no Tatsujin sets' ATAPI CD-ROM drive (MAME namcos10.cpp: a Matsushita CR-589 on the main board's
// ATA port), read from a MAME CHD. See s10_atapi.c.
#ifndef S10_ATAPI_H
#define S10_ATAPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int s10_atapi_open(const char *path); // 0 when the CHD is open
void s10_atapi_close(void);
int s10_atapi_present(void);
void s10_atapi_reset(void);
// ATA task file registers 0 (data) .. 7 (status / command), the main CPU's 0x1fbc0000 + 2 * reg
uint16_t s10_atapi_r(int reg);
void s10_atapi_w(int reg, uint16_t v);
// DMARQ: a block of data waits for a DMA (the PACKET command asked for DMA)
int s10_atapi_dmarq(void);
// the INTRQ line changed (the board: s10.cpp)
void s10_atapi_irq(int state);
// save states: the drive's registers, command and buffer (the open disc stays)
uint32_t s10_atapi_state_size(void);
void s10_atapi_state_save(void *dst);
void s10_atapi_state_load(const void *src);

#ifdef __cplusplus
}
#endif

#endif
