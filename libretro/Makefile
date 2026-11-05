# ReSys12 libretro core (Namco System 12 and System 11). Linux hosts: x86_64 (lightrec recompiler), armv7 / aarch64 (ari64 recompiler).
#   make -C libretro                               native
#   make -C libretro CC=aarch64-linux-gnu-gcc      Raspberry Pi 5 (64-bit OS)
CC ?= gcc
ARCH ?= $(shell $(CC) -dumpmachine | cut -d- -f1)
ROOT := ..
PSX := $(ROOT)/src/psx
TARGET := resys12_libretro.so

SRC := $(PSX)/libpcsxcore/r3000a.c $(PSX)/libpcsxcore/psxmem.c $(PSX)/libpcsxcore/psxinterpreter.c \
  $(PSX)/libpcsxcore/psxhw.c $(PSX)/libpcsxcore/psxdma.c $(PSX)/libpcsxcore/psxcounters.c \
  $(PSX)/libpcsxcore/psxevents.c $(PSX)/libpcsxcore/gte.c $(PSX)/libpcsxcore/gte_nf.c \
  $(PSX)/libpcsxcore/gte_divider.c $(PSX)/libpcsxcore/mdec.c $(PSX)/libpcsxcore/gpu.c \
  $(PSX)/plugins/gpulib/gpu.c $(PSX)/plugins/gpulib/vout_pl.c $(PSX)/plugins/gpulib/prim.c \
  $(PSX)/plugins/gpulib/gpu_async.c $(PSX)/plugins/gpu_neon/psx_gpu_if.c \
  $(PSX)/shim/rs12_psx_glue.c $(PSX)/shim/rs12_gpu_bind.c \
  $(PSX)/libpcsxcore/new_dynarec/emu_if.c \
  $(ROOT)/src/s12/s12.c $(ROOT)/src/s12/s12_sub.c $(ROOT)/src/s12/s12_jvs.c $(ROOT)/src/s12/s12_hle.c \
  $(ROOT)/src/s12/s12_sound.c $(ROOT)/src/s12/s12_state.c $(ROOT)/src/s12/s12_link.c \
  $(ROOT)/src/s12/s12_cdxa.c $(ROOT)/src/s12/s12_cdmix.c $(ROOT)/src/s12/s12_atapi.c $(ROOT)/src/sh2/sh2.c \
  $(ROOT)/src/s11/s11.c \
  $(ROOT)/src/m37710/m37710.c $(ROOT)/src/s11/s11_c76.c \
  $(ROOT)/deps/libchdr/unity.c \
  $(ROOT)/src/h8/h8.c $(ROOT)/src/h8/h8_periph.c $(ROOT)/src/sound/c352.c \
  $(ROOT)/src/romload/s12_rom.c $(ROOT)/src/romload/rs12_zip.c $(ROOT)/src/romload/junzip.c \
  $(ROOT)/src/sys/rs12_jit.c \
  libretro.c rs12_threads_pthread.c rs12_link_udp.c

DEFS := -DP_HAVE_MMAP=0 -DP_HAVE_POSIX_MEMALIGN=0 -DDISABLE_MEM_LUTS=0 -DGPU_NEON -DTEXTURE_CACHE_4BPP \
  -DTEXTURE_CACHE_8BPP -DNDEBUG -DUSE_ASYNC_GPU
INC := -I$(PSX)/libpcsxcore -I$(PSX)/include -I$(PSX)/shim -I$(ROOT)/src/s12 -I$(ROOT)/src/s11 -I$(ROOT)/src/romload \
  -I$(ROOT)/src/sound -I$(ROOT)/src/h8 -I$(ROOT)/src/sys -I$(ROOT)/src/sh2 -I$(ROOT)/deps/libchdr/include -I.
CFLAGS ?= -O3 -ffast-math
CFLAGS += $(EXTRA_CFLAGS) -fPIC -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
  -fno-optimize-sibling-calls -fno-strict-aliasing $(DEFS) $(INC)
GPU_IF_FLAGS := -DNEON_BUILD

ifeq ($(ARCH),arm)
  CFLAGS += -marm -mfpu=neon -mfloat-abi=hard -DHAVE_NEON_ASM
  SRC += $(PSX)/libpcsxcore/gte_arm.S $(PSX)/libpcsxcore/gte_nf_arm.S $(PSX)/libpcsxcore/gte_neon.S \
    $(PSX)/libpcsxcore/gte_nf_neon.S $(PSX)/plugins/gpu_neon/psx_gpu/psx_gpu_arm_neon.S \
    $(PSX)/libpcsxcore/new_dynarec/new_dynarec.c $(PSX)/libpcsxcore/new_dynarec/pcsxmem.c \
    $(PSX)/libpcsxcore/new_dynarec/linkage_arm.S
else ifeq ($(ARCH),aarch64)
  SRC += $(PSX)/libpcsxcore/gte_arm64.S $(PSX)/libpcsxcore/gte_nf_arm64.S \
    $(PSX)/plugins/gpu_neon/psx_gpu/psx_gpu_simd.c \
    $(PSX)/libpcsxcore/new_dynarec/new_dynarec.c $(PSX)/libpcsxcore/new_dynarec/pcsxmem.c \
    $(PSX)/libpcsxcore/new_dynarec/linkage_arm64.S
  GPU_IF_FLAGS += -DSIMD_BUILD
  CFLAGS += -DSIMD_BUILD
else
  # no ari64 backend: the lightrec recompiler on x86_64 (deps/lightrec + GNU lightning, x86 only), else the R3000A
  # interpreter; make LIGHTREC=0 for the interpreter on x86_64
  LIGHTREC ?= $(if $(filter x86_64,$(ARCH)),1,0)
  GPU_IF_FLAGS :=
  ifeq ($(LIGHTREC),1)
    LR := $(ROOT)/deps/lightrec
    LN := $(ROOT)/deps/lightning
    SRC += $(PSX)/libpcsxcore/lightrec/plugin.c $(LR)/blockcache.c $(LR)/constprop.c $(LR)/disassembler.c \
      $(LR)/emitter.c $(LR)/interpreter.c $(LR)/lightrec.c $(LR)/memmanager.c $(LR)/optimizer.c $(LR)/regcache.c \
      $(LR)/tlsf/tlsf.c $(LN)/lib/lightning.c $(LN)/lib/jit_disasm.c $(LN)/lib/jit_memory.c $(LN)/lib/jit_names.c \
      $(LN)/lib/jit_note.c $(LN)/lib/jit_print.c $(LN)/lib/jit_size.c
    CFLAGS += -DLIGHTREC -DLIGHTREC_STATIC -DLIGHTREC_CUSTOM_MAP=0 -DLIGHTREC_CODE_INV=0 \
      -DLIGHTREC_ENABLE_THREADED_COMPILER=0 -DLIGHTREC_ENABLE_DISASSEMBLER=0 -DLIGHTREC_NO_DEBUG=1 \
      -I$(LN)/include -I$(LR) -I$(PSX)/libpcsxcore/lightrec
  else
    CFLAGS += -DDRC_DISABLE
  endif
endif

# (object paths keep the tree: ../src/x.c -> obj/<arch>/src/x.o)
OBJ := $(addprefix obj/$(ARCH)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(subst ../,,$(SRC)))))
# kartduel link CPU: src/h8 built a second time with its symbols renamed (src/h8/h8_link_names.h)
OBJ += obj/$(ARCH)/link/h8.o obj/$(ARCH)/link/h8_periph.o

# header dependencies (.d next to each object, read at the end: the default goal stays all)
CFLAGS += -MMD -MP

# (linked per arch, then copied: switching CC relinks)
all: obj/$(ARCH)/$(TARGET)
	cp $< $(TARGET)

obj/$(ARCH)/$(TARGET): $(OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS) -Wl,--version-script=link.T -Wl,--no-undefined -lpthread -lm -lz

obj/$(ARCH)/src/psx/plugins/gpu_neon/psx_gpu_if.o: CFLAGS += $(GPU_IF_FLAGS)
obj/$(ARCH)/src/psx/plugins/gpu_neon/psx_gpu/psx_gpu_simd.o: CFLAGS += $(GPU_IF_FLAGS)

obj/$(ARCH)/deps/lightning/%.o: CFLAGS += -DHAVE_MMAP=0 -w
obj/$(ARCH)/deps/lightrec/%.o: CFLAGS += -w -DLOG_LEVEL=1

CHDR := $(ROOT)/deps/libchdr
obj/$(ARCH)/deps/libchdr/unity.o: CFLAGS += -DZ7_ST -I$(CHDR)/deps/lzma-25.01/include -I$(CHDR)/deps/zstd-1.5.7 -I$(CHDR)/deps/miniz-3.1.1 -w
obj/$(ARCH)/deps/%.o: ../deps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
obj/$(ARCH)/src/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
obj/$(ARCH)/src/%.o: ../src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
obj/$(ARCH)/link/%.o: ../src/h8/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include ../src/h8/h8_link_names.h -c -o $@ $<
obj/$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf obj $(TARGET)

.PHONY: all clean

-include $(OBJ:.o=.d)
