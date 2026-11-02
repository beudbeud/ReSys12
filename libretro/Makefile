# ReSys12 libretro core. Linux hosts: x86_64 (interpreter), armv7 / aarch64 (ari64 recompiler).
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
  $(ROOT)/src/s12/s12_sound.c $(ROOT)/src/s12/s12_state.c \
  $(ROOT)/src/h8/h8.c $(ROOT)/src/h8/h8_periph.c $(ROOT)/src/sound/c352.c \
  $(ROOT)/src/romload/s12_rom.c $(ROOT)/src/romload/rs12_zip.c $(ROOT)/src/romload/junzip.c \
  $(ROOT)/src/sys/rs12_jit.c \
  libretro.c rs12_threads_pthread.c

DEFS := -DP_HAVE_MMAP=0 -DP_HAVE_POSIX_MEMALIGN=0 -DDISABLE_MEM_LUTS=0 -DGPU_NEON -DTEXTURE_CACHE_4BPP \
  -DTEXTURE_CACHE_8BPP -DNDEBUG -DUSE_ASYNC_GPU
INC := -I$(PSX)/libpcsxcore -I$(PSX)/include -I$(PSX)/shim -I$(ROOT)/src/s12 -I$(ROOT)/src/romload \
  -I$(ROOT)/src/sound -I$(ROOT)/src/h8 -I$(ROOT)/src/sys -I.
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
  # no ari64 backend: R3000A interpreter
  CFLAGS += -DDRC_DISABLE
  GPU_IF_FLAGS :=
endif

# (object paths keep the tree: ../src/x.c -> obj/<arch>/src/x.o)
OBJ := $(addprefix obj/$(ARCH)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(subst ../,,$(SRC)))))

# (linked per arch, then copied: switching CC relinks)
all: obj/$(ARCH)/$(TARGET)
	cp $< $(TARGET)

obj/$(ARCH)/$(TARGET): $(OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS) -Wl,--version-script=link.T -Wl,--no-undefined -lpthread -lm -lz

obj/$(ARCH)/src/psx/plugins/gpu_neon/psx_gpu_if.o: CFLAGS += $(GPU_IF_FLAGS)
obj/$(ARCH)/src/psx/plugins/gpu_neon/psx_gpu/psx_gpu_simd.o: CFLAGS += $(GPU_IF_FLAGS)

obj/$(ARCH)/src/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
obj/$(ARCH)/src/%.o: ../src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
obj/$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf obj $(TARGET)

.PHONY: all clean
