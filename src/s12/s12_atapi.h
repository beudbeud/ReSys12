// ReSys12: ATAPI CD-ROM of the System 12 CDXA PCB, read from a MAME CHD. GPL-2.0-or-later.
#ifndef S12_ATAPI_H
#define S12_ATAPI_H

#include <stdint.h>

// 0 when the CHD is open (without a disc every read fails with "medium not present")
int s12_atapi_open(const char *path);
void s12_atapi_close(void);
void s12_atapi_reset(void);
// ATA task file registers 0 (data) .. 7 (status / command)
uint16_t s12_atapi_r(int reg);
void s12_atapi_w(int reg, uint16_t v);
// DREQ: the drive has data for the host to read
int s12_atapi_drq(void);

#endif
