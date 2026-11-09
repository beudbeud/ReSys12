// ReSys10: System 10 save states (from ReSys12's s12_state.c). Layout follows pcsx_rearmed misc.c SaveState/LoadState
// for the PS1-derived part (RAM, hw registers, CPU registers, GPU registers + 2MB VRAM, SPU, root counters, MDEC), then
// the board (s10.cpp). GPL-2.0-or-later.
#include "s10_state.h"

#include <stdlib.h>
#include <string.h>

#include "psxcommon.h"
#include "psxhw.h"
#include "psxmem.h"
#include "psxevents.h"
#include "plugins.h"
#include "r3000a.h"
#include "mdec.h"
#include "gpu.h"
#include "s10.h"

#define STATE_MAGIC "RS10STA1"
#define VRAM_BYTES (1024 * 1024 * 2)
#define SPURAM_BYTES (512 * 1024)
#define SPU_PART2_MAX (64 * 1024) // (pcsx_rearmed SPUFREEZE_F2_MAX_SIZE is smaller)

extern unsigned int frame_counter;
extern s32 psxRcntFreeze(void *f, s32 Mode);

// ---- memory stream behind pcsx_rearmed's SaveFuncs (used by the *Freeze helpers) ----
typedef struct {
	uint8_t *buf;
	size_t size, cap, pos;
	int failed;
} MemStream;

static void *ms_open(const char *name, const char *mode) { (void)name; (void)mode; return NULL; }

static int ms_write(void *f, const void *p, u32 len)
{
	MemStream *s = (MemStream *)f;
	if (s->failed)
		return 0;
	if (s->pos + len > s->cap) {
		size_t cap = s->cap ? s->cap * 2 : (size_t)8 << 20;
		while (cap < s->pos + len)
			cap *= 2;
		uint8_t *b = (uint8_t *)realloc(s->buf, cap);
		if (!b) {
			s->failed = 1;
			return 0;
		}
		s->buf = b;
		s->cap = cap;
	}
	memcpy(s->buf + s->pos, p, len);
	s->pos += len;
	if (s->pos > s->size)
		s->size = s->pos;
	return (int)len;
}

static int ms_read(void *f, void *p, u32 len)
{
	MemStream *s = (MemStream *)f;
	if (s->failed || s->pos + len > s->size) {
		s->failed = 1;
		memset(p, 0, len);
		return 0;
	}
	memcpy(p, s->buf + s->pos, len);
	s->pos += len;
	return (int)len;
}

static long ms_seek(void *f, long offs, int whence)
{
	MemStream *s = (MemStream *)f;
	s->pos = whence == 0 ? (size_t)offs : s->pos + (size_t)offs;
	return (long)s->pos;
}

static void ms_close(void *f) { (void)f; }

static void use_mem_stream(void)
{
	SaveFuncs.open = ms_open;
	SaveFuncs.read = ms_read;
	SaveFuncs.write = ms_write;
	SaveFuncs.seek = ms_seek;
	SaveFuncs.close = ms_close;
}

// CPU state saved besides the psxRegisters prefix (fields after gteBusyCycle; pointers are never saved)
typedef struct {
	u32 gteBusyCycle, muldivBusyCycle, biuReg, gpuIdleAfter, gpu_sr, frame_counter, subCycle;
} CpuExtra;

static const char *s_game; // set name in the header

void s10_state_set_game(const char *name) { s_game = name; }

uint8_t *s10_state_save_buf(size_t *size)
{
	MemStream ms;
	memset(&ms, 0, sizeof(ms));
	use_mem_stream();
	psxCpu->Notify(R3000ACPU_NOTIFY_BEFORE_SAVE, NULL);

	char hdr[32];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, STATE_MAGIC, 8);
	strncpy(hdr + 8, s_game ? s_game : "", 23);
	ms_write(&ms, hdr, sizeof(hdr));

	ms_write(&ms, psxRegs.ptrs.psxM, PSX_RAM_SIZE);
	ms_write(&ms, psxRegs.ptrs.psxH, 0x10000);
	ms_write(&ms, &psxRegs, offsetof(psxRegisters, gteBusyCycle));
	CpuExtra cx = { psxRegs.gteBusyCycle, psxRegs.muldivBusyCycle, psxRegs.biuReg, psxRegs.gpuIdleAfter,
		HW_GPU_STATUS, frame_counter, psxRegs.subCycle };
	ms_write(&ms, &cx, sizeof(cx));

	GPUFreeze_t gh;
	uint16_t *vram = NULL;
	memset(&gh, 0, sizeof(gh));
	gh.ulFreezeVersion = 1;
	GPU_freeze(1, &gh, &vram);
	ms_write(&ms, &gh, sizeof(gh));
	ms_write(&ms, vram, VRAM_BYTES);

	{ // SPU (misc.c layout: header, SPU RAM, the rest)
		static struct { SPUFreeze_t hdr; uint8_t part2[SPU_PART2_MAX]; } u;
		unsigned short *spuram = NULL;
		memset(&u.hdr, 0, sizeof(u.hdr));
		SPU_freeze(1, &u.hdr, &spuram, u.part2, psxRegs.cycle);
		const u32 n2 = u.hdr.Size - sizeof(u.hdr) - SPURAM_BYTES;
		if (!spuram || n2 > SPU_PART2_MAX)
			ms.failed = 1;
		else {
			ms_write(&ms, &u.hdr, sizeof(u.hdr));
			ms_write(&ms, spuram, SPURAM_BYTES);
			ms_write(&ms, u.part2, n2);
		}
	}

	psxHwFreeze(&ms, 1);
	psxRcntFreeze(&ms, 1);
	mdecFreeze(&ms, 1);

	u32 n = (u32)s10_state_size();
	uint8_t *tmp = (uint8_t *)malloc(n);
	if (tmp) {
		s10_state_save(tmp);
		ms_write(&ms, &n, sizeof(n));
		ms_write(&ms, tmp, n);
		free(tmp);
	} else
		ms.failed = 1;

	if (ms.failed) {
		free(ms.buf);
		return NULL;
	}
	*size = ms.size;
	return ms.buf;
}

int s10_state_load_buf(const uint8_t *buf, size_t size)
{
	MemStream ms;
	memset(&ms, 0, sizeof(ms));
	ms.buf = (uint8_t *)buf;
	ms.size = ms.cap = size;
	use_mem_stream();

	// validate before touching anything: header, set, total size (the same for a set and a build)
	char hdr[32];
	ms_read(&ms, hdr, sizeof(hdr));
	if (ms.failed || memcmp(hdr, STATE_MAGIC, 8) != 0 || !s_game || strncmp(hdr + 8, s_game, 23) != 0)
		return -1;
	{
		size_t cur = 0;
		uint8_t *probe = s10_state_save_buf(&cur);
		free(probe);
		if (!probe || cur != size)
			return -1;
		use_mem_stream();
	}

	ms_read(&ms, psxRegs.ptrs.psxM, PSX_RAM_SIZE);
	psxCpu->Clear(0, PSX_RAM_SIZE / 4);
	ms_read(&ms, psxRegs.ptrs.psxH, 0x10000);
	ms_read(&ms, &psxRegs, offsetof(psxRegisters, gteBusyCycle));
	CpuExtra cx;
	ms_read(&ms, &cx, sizeof(cx));
	psxRegs.gteBusyCycle = cx.gteBusyCycle;
	psxRegs.muldivBusyCycle = cx.muldivBusyCycle;
	psxRegs.biuReg = cx.biuReg;
	psxRegs.gpuIdleAfter = cx.gpuIdleAfter;
	psxRegs.branching = 0;
	psxRegs.biosBranchCheck = ~0u;
	psxRegs.cpuInRecursion = 0;
	HW_GPU_STATUS = cx.gpu_sr;
	frame_counter = cx.frame_counter;

	GPUFreeze_t gh;
	uint16_t *vram = NULL;
	ms_read(&ms, &gh, sizeof(gh));
	GPU_freeze(0, &gh, &vram);
	ms_read(&ms, vram, VRAM_BYTES);
	gpuSyncPluginSR();

	{
		static struct { SPUFreeze_t hdr; uint8_t part2[SPU_PART2_MAX]; } u;
		unsigned short *spuram = NULL;
		ms_read(&ms, &u.hdr, sizeof(u.hdr));
		const u32 n2 = u.hdr.Size - sizeof(u.hdr) - SPURAM_BYTES;
		if (n2 > SPU_PART2_MAX)
			return -2;
		SPU_freeze(0, NULL, &spuram, NULL, psxRegs.cycle);
		ms_read(&ms, spuram, SPURAM_BYTES);
		ms_read(&ms, u.part2, n2);
		SPU_freeze(0, &u.hdr, &spuram, u.part2, psxRegs.cycle);
	}

	psxHwFreeze(&ms, 0);
	psxRcntFreeze(&ms, 0);
	mdecFreeze(&ms, 0);

	u32 n = 0;
	ms_read(&ms, &n, sizeof(n));
	int r = ms.failed || ms.pos + n > ms.size || s10_state_load(ms.buf + ms.pos, n) != 0 ? -2 : 0;

	events_restore();
	psxCpu->Notify(R3000ACPU_NOTIFY_AFTER_LOAD_STATE, NULL);
	psxRegs.subCycle = cx.subCycle;
	return r;
}
