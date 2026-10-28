# Descent (D1X Rebirth 0.40, the Descent 1.5 source with its assembly
# rewritten in C) for the m9b SoC: builds descent.bin, which the core
# firmware's loader copies into PSRAM and runs (see README.md). Requires
# riscv64-unknown-elf-gcc and picolibc-riscv64-unknown-elf.

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m9b core (m9b-core): its firmware (SD card, FatFs) and the runtime the
# programs share (system calls, trap handler, linker script).
CORE ?= ../core
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime
D := d1x

# Memory map (PSRAM, $(CORE)/runtime/m9b.ld): the image (code, rodata,
# .data) from 0x800000, then bss and the heap up to a 128 KB stack at the
# top.

B ?= build

ARCH    := -march=rv32im_zba_zicbop_zicsr_zifencei_zicbom -mabi=ilp32
CFLAGS  := $(ARCH) -O2 -g -std=gnu89 -specs=picolibc.specs --picolibc-buildtype=release \
           -ffunction-sections -fdata-sections -funsigned-char -fcommon \
           -DM9B -DNO_ASM -D__LINUX__ -DSHAREWARE -DNMONO -DRELEASE -DNDEBUG \
           -DD1XMAJOR=\"0\" -DD1XMINOR=\"40\" -DDESCENT_DATA_PATH=\"DESCENT/\" -DFF_FS_READONLY=0 \
           -Iport -I$(D)/include -I$(D)/main -I$(D)/texmap -I$(CORE_FW) -I$(CORE_FW)/fatfs \
           -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses \
           -Wno-pointer-sign -Wno-char-subscripts -Wno-format -Wno-unused-function \
           -Wno-implicit-int -Wno-missing-braces -Wno-unused-label -Wno-address
LDFLAGS := $(ARCH) -specs=picolibc.specs --picolibc-buildtype=release \
           -Wl,--defsym=__stack_size=0x20000 \
           -Wl,--gc-sections -Wl,-Map=$(B)/descent.map \
           -T$(CORE_RT)/m9b.ld

MAIN := inferno game gauges mglobal scores object laser physics bm menu switch effects \
        texmerge polyobj fireball ai aipath hostage powerup fuelcen render hash piggy args \
        gameseg wall collide lighting robot morph vclip weapon fvi newdemo titles gameseq \
        controls automap text newmenu gamefont hud playsave endlevel terrain kconfig \
        multi cntrlcen credits config kmatrix paging mission songs bmread state multibot \
        nocomlib slew dumpmine ignore custom multipow reorder m_inspak radar \
        d_gamecv d_conv mprofile hudlog cdplay mlticntl command ban pingstat vlcnfire \
        multiver reconfig altsound key gamesave gamemine snddecom
GAME_SRC := $(MAIN:%=$(D)/main/%.c) \
            $(addprefix $(D)/2d/, palette.c canvas.c pixel.c rect.c 2dsline.c line.c box.c poly.c \
              disc.c gpixel.c font.c bitmap.c bitblt.c pcx.c rle.c ibitblt.c circle.c tmerge.c scalec.c) \
            $(addprefix $(D)/3d/, clipper.c matrix.c rod.c draw.c instance.c points.c setup.c globvars.c interp.c) \
            $(addprefix $(D)/maths/, fixc.c tables.c vecmat.c rand.c) \
            $(addprefix $(D)/texmap/, ntmap.c scanline.c tmapflat.c) \
            $(D)/cfile/cfile.c $(D)/mem/mem.c $(D)/iff/iff.c \
            $(addprefix $(D)/misc/, error.c d_io.c d_glob.c strutil.c strio.c d_delay.c compare.c d_slash.c)
PORT_SRC := $(wildcard port/*.c) $(CORE_RT)/syscalls.c $(CORE_RT)/string.c $(CORE_RT)/trap.c
PORT_ASM := $(CORE_RT)/trap_entry.S
FW_SRC   := $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
SRC      := $(GAME_SRC) $(PORT_SRC) $(FW_SRC)
OBJ      = $(patsubst %.c,$(B)/%.o,$(notdir $(SRC))) $(patsubst %.S,$(B)/%.o,$(notdir $(PORT_ASM)))

vpath %.c $(D)/main $(D)/2d $(D)/3d $(D)/maths $(D)/texmap $(D)/cfile $(D)/mem $(D)/iff $(D)/misc \
          port $(CORE_RT) $(CORE_FW) $(CORE_FW)/fatfs
vpath %.S $(CORE_RT)

CFLAGS += -MMD -MP
CFLAGS += $(EXTRA)
LDFLAGS += $(LDEXTRA)   # e.g. -Wl,--wrap=memcpy with EXTRA=-DM9B_MEMPROF

all: $(B)/descent.bin

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/string.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

# The platform layer and FatFs are C99 (the game is gnu89: implicit ints).
$(patsubst %.c,$(B)/%.o,$(notdir $(PORT_SRC) $(FW_SRC))): CFLAGS += -std=gnu11

$(B)/%.o: %.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/descent.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/descent.bin: $(B)/descent.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

$(B):
	mkdir -p $(B)

-include $(OBJ:.o=.d)

clean:
	rm -rf build build-*

.PHONY: all clean
