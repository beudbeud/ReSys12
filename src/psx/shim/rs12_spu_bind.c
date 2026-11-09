// ReSys12 (System 10): the dfsound SPU entry points, bound to the core's SPU_* pointers by rs12_psx_bind (rs12_psx_glue.c), in
// the order of rs12_spu_entry. (plugins.h names its function pointer types after them, hence a file of its own.)
// GPL-2.0-or-later.
#include "../plugins/dfsound/stdafx.h"
#include "../plugins/dfsound/externals.h"
#include "../plugins/dfsound/spu.h"
#include "../plugins/dfsound/registers.h"

void CALLBACK SPUregisterCallback(void (CALLBACK *callback)(int));
void CALLBACK SPUregisterScheduleCb(void (CALLBACK *callback)(unsigned int));

void *const rs12_spu_entry[] = {
	(void *)SPUinit, (void *)SPUshutdown, (void *)SPUopen, (void *)SPUclose, (void *)SPUwriteRegister,
	(void *)SPUreadRegister, (void *)SPUwriteDMAMem, (void *)SPUreadDMAMem, (void *)SPUfreeze, (void *)SPUasync,
	(void *)SPUregisterCallback, (void *)SPUregisterScheduleCb,
};
