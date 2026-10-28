# Doom (doomgeneric) for the m9b SoC: builds doom.bin, which the core
# firmware's loader copies into PSRAM and runs (see README.md).
# Requires riscv64-unknown-elf-gcc and picolibc-riscv64-unknown-elf.

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m9b core (m9b-core): its firmware (SD card, FatFs) and the runtime the
# programs share (system calls, trap handler, linker script).
CORE ?= ../core
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime

# Memory map (PSRAM, ../core/runtime/m9b.ld): the image (code, rodata,
# .data) from 0x800000, then bss and the heap up to a 64 KB stack at the
# top.

# make TIMEDEMO=1: a build (in build-td/) that runs -timedemo demo1.
ifdef TIMEDEMO
B := build-td
else
B := build
endif

ARCH    := -march=rv32im_zba_zicbop_zicsr_zifencei_zicbom -mabi=ilp32
CFLAGS  := $(ARCH) -O2 -g -specs=picolibc.specs --picolibc-buildtype=release \
           -ffunction-sections -fdata-sections \
           -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DM9B_DIRECT -DM9B_RAWWAD -DM9B_MOUSE -DFEATURE_SOUND -DM9B_SOUND -DM9B_NOQUIT -DM9B_SAVEDIR=\"DOOMS/SAVES/\" -DFF_FS_READONLY=0 -DNORMALUNIX \
           -Idoomgeneric -Iport -I$(CORE_FW) -I$(CORE_FW)/fatfs \
           -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses
LDFLAGS := $(ARCH) -specs=picolibc.specs --picolibc-buildtype=release \
           -Wl,--defsym=__stack_size=0x10000 \
           -Wl,--gc-sections -Wl,-Map=$(B)/doom.map \
           -T$(CORE_RT)/m9b.ld
ifndef NOHOT
LDFLAGS += -Wl,--section-ordering-file=port/hot.ld   # hot code together (port/hot.ld)
endif
# (-T given here, after the --defsym's: m9b.ld tests DEFINED(__stack_size)
# while it is read, and the specs file would put its -T before them.)

DOOM_SRC := $(filter-out doomgeneric/doomgeneric_%.c doomgeneric/i_allegro%.c doomgeneric/i_sdl%.c, $(wildcard doomgeneric/*.c))
PORT_SRC := $(wildcard port/*.c) $(CORE_RT)/string.c $(CORE_RT)/syscalls.c $(CORE_RT)/trap.c
PORT_ASM := $(CORE_RT)/trap_entry.S
FW_SRC   := $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
SRC      := $(DOOM_SRC) $(PORT_SRC) $(FW_SRC)
OBJ      = $(patsubst %.c,$(B)/%.o,$(notdir $(SRC))) $(patsubst %.S,$(B)/%.o,$(notdir $(PORT_ASM)))

vpath %.c doomgeneric port $(CORE_RT) $(CORE_FW) $(CORE_FW)/fatfs
vpath %.S $(CORE_RT)

# make BASELINE=1 (with B=...): without our renderer changes, for comparison.
ifndef BASELINE
CFLAGS += -DM9B_FASTDRAW
endif
ifdef TIMEDEMO
CFLAGS += -DM9B_TIMEDEMO -DM9B_PROFILE   # and the reports on the UART
endif
CFLAGS += -MMD -MP   # header dependencies ($(B)/*.d)
CFLAGS += $(EXTRA)   # make EXTRA=-D... B=build-x: experiments

all: $(B)/doom.bin

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# The string functions must not be turned back into calls to themselves.
$(B)/string.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

$(B)/%.o: %.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/doom.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/doom.bin: $(B)/doom.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

$(B):
	mkdir -p $(B)

-include $(OBJ:.o=.d)

clean:
	rm -rf build build-td

.PHONY: all clean
