/*  ReSys12: executable memory for the R3000A dynamic recompiler (pcsx_rearmed new_dynarec) (see rs12_jit.h).
    GPL-2.0-or-later. */
#include "rs12_jit.h"

#include <stdint.h>
#include <string.h>

void rs12_jit_shutdown(void) {}
#include <sys/mman.h>

int rs12_jit_keep_open = 1; // code memory is always writable here

int rs12_jit_code_reserve(size_t size) { (void)size; return 0; }
void *rs12_jit_h8_region(size_t *size)
{
	static void *p;
	if (!p) {
		p = mmap(NULL, RS12_JIT_H8_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED)
			p = NULL;
	}
	if (size)
		*size = p ? RS12_JIT_H8_SIZE : 0;
	return p;
}
void *rs12_jit_code_region(size_t *size) { if (size) *size = 0; return NULL; }
int rs12_jit_code_uid(void) { return -1; }

void *rs12_jit_code_alloc(size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

void rs12_jit_code_write_begin(void) {}

void rs12_jit_code_write_end(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
}
void rs12_jit_code_sync(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
}
