/*  ReSys12: executable memory for the R3000A dynamic recompiler (pcsx_rearmed new_dynarec) and the H8 blocks:
    mmap RWX, writes need no bracketing (the begin / end calls only sync the I-cache).
    GPL-2.0-or-later. */
#ifndef RS12_JIT_H
#define RS12_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the R3000A recompiler's code (pcsx_rearmed struct ndrc_mem: 16MB translation cache incl. trampolines)
#define RS12_JIT_RESERVE_SIZE (16u << 20)

int rs12_jit_code_reserve(size_t size); // nothing to reserve here (kept for the callers)
void *rs12_jit_code_alloc(size_t size);
void *rs12_jit_code_region(size_t *size); // NULL: nothing reserved
// Call before writing generated code (or any data inside the code region)
void rs12_jit_code_write_begin(void);
// Call after writing: makes [p, p+len) executable and coherent with the I-cache
void rs12_jit_code_write_end(void *p, size_t len);
// Makes [p, p+len) coherent without ending the write section (several ranges, then write_end on the last)
void rs12_jit_code_sync(void *p, size_t len);
void rs12_jit_shutdown(void); /* before the process exits */
int rs12_jit_code_uid(void); // -1 (no memory block id here)
// code area of the H8 blocks (src/h8), mapped RWX
#define RS12_JIT_H8_SIZE (1u << 20)
void *rs12_jit_h8_region(size_t *size);

#ifdef __cplusplus
}
#endif

#endif
