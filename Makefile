# SPDX-License-Identifier: GPL-2.0-or-later
#
# waffle-boot build.
#
#   make [host]   host build of the library and unit tests (default)
#   make check    build and run the host unit tests
#   make regen    run the vector generators of tests/oracle/ (tools/regen.py);
#                 the unit tests' vector headers are built, under
#                 $(BUILD)/gen/vectors/, by the targets that need them
#   make ui-update  after changing a screen: the tests' S-15 header, then
#                 the screenshots in docs/ui/ (where make check runs)
#   make cross    cross build for the ADSP-BF703 (needs the bfin-elf toolchain,
#                 see tools/toolchain/README.md), the bootloader image included
#   make image    the bootloader's ELF file and LDR stream, with the layout and
#                 size checks (build/bf703/waffle-boot.{elf,ldr,layout.json})
#                 and the frozen front's pin and boundary (src/front/)
#   make front-image  the front alone as a recovery image (build/bf703/front.ldr)
#   make front-pin    record a changed front's pin: only with the maintainer's
#                 sign-off (CONTRIBUTING.md, "The frozen front")
#   make test-os  the test OS (wiki/Test-OS.md): held mode as an OS image
#                 for another bootloader's developer menu, with its gates, in
#                 its two panel variants:
#                 build/bf703-testos/waffle-test-{smc,gpio}.{elf,ldr,layout.json};
#                 TESTOS_PANEL=smc|gpio builds one
#   make test-os-mon      the test OS with the USB debug monitor
#   make installer        the installer (wiki/Installer-Reference.md): the hardware checks,
#                 then this build's image written to the unit when every
#                 check passes: $(BUILD)/waffle-install.ldr
#   make analyze  clang static analyzer over src/ and tests/
#   make cppcheck cppcheck over src/ and tests/, the vendored code left out
#   make fuzz     libFuzzer targets (clang), see tests/fuzz/README.md
#   make fuzz-run each fuzzer for FUZZ_TIME seconds
#   make size     target size of each vendored component and of each memory
#                 region of the image; fails when one is over its limit
#   BOOT_PROMPT=1 builds the boot prompt in (S-05.025, S-15.180: a development
#                 build's screen before the OS is started; off by default and
#                 in every release, and nothing at run time turns it on):
#                 make image BOOT_PROMPT=1 BUILD=build/prompt
#   LOG_DEBUG=1   a development build that also logs the debug lines:
#                 make image LOG_DEBUG=1 BUILD=build/debug
#   BOOT_CRUMBS=1 a trial boot's breadcrumbs (src/board/opxy_crumbs.c)
#   BOOT_TIMING=1 the boot's milestones timed (src/board/opxy_timing.c)
#   BOOT_LINK_SETTLE_US=<n>  microseconds between a byte of the slave's boot
#                 stream and the sample of PA6 that decides the next (S-13b.022),
#                 default 2; 0 for none
#   BOOT_TIMING_WINDOW_US=<n>  with BOOT_TIMING=1, how long PA6 is watched after
#                 each stream byte, when longer than the settle time
#   BOOT_LINK_FC=1  the slave's boot stream paced by the SPI controller's own
#                 flow control on PA6 (S-13b.022), off by default
#   REPLY_END=count  the slave's replies end at their received-word count
#                 from the start; default (REPLY_END=factory) the factory's
#                 end, TF (S-13b.083)
#   LOGO=<file>.png  that image at the OS hand-over in place of the logo
#                 (S-15.132; tools/png2logo.py)
#   OPXY_BL_BUILD=<n>  the bootloader's build number (S-17.032), default 0
#   OPXY_BL_VERSION=A.B.C  a test build's own version, in place of the header's
#   make bump-version  raise the patch version, once per build for a unit
#   make clean
#
# Variables: CC (host compiler), CROSS_COMPILE (default: the toolchain built
# by tools/toolchain/build.sh if present, else bfin-elf- on PATH), CLANG,
# CPPCHECK, PYTHON, OBJCOPY, BUILD (output directory).

BUILD    ?= build
CLANG    ?= clang
CPPCHECK ?= cppcheck
PYTHON   ?= python3
OBJCOPY  ?= objcopy

TOOLCHAIN_PREFIX := $(shell tools/toolchain/build.sh --print-prefix)
CROSS_COMPILE ?= $(if $(wildcard $(TOOLCHAIN_PREFIX)/bin/bfin-elf-gcc),$(TOOLCHAIN_PREFIX)/bin/)bfin-elf-
CROSS_CC := $(CROSS_COMPILE)gcc
CROSS_AR := $(CROSS_COMPILE)ar
CROSS_SIZE := $(CROSS_COMPILE)size

# Warnings shared by every compiler we use (GCC 8 for the target, current
# GCC and clang on the host).
WARNINGS := -Wall -Wextra -Werror -pedantic-errors \
	-Wshadow -Wundef -Wvla -Wdate-time \
	-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wold-style-definition -Wredundant-decls -Wnested-externs \
	-Wcast-qual -Wcast-align -Wpointer-arith -Wwrite-strings \
	-Wconversion -Wsign-conversion -Wdouble-promotion \
	-Wformat=2 -Wswitch-default -Wswitch-enum

# Build paths must not leak into the outputs (see tools/repro-check.sh).
COMMON := -std=c11 $(WARNINGS) -Isrc/include -Isrc -g -fdebug-prefix-map=$(CURDIR)=.

# The image spans L1, L2 and DDR, beyond a CALL's reach, so every call on
# the target goes through a register (S-01.011; bf703.ld).
TARGET_CFLAGS := $(COMMON) -mcpu=bf703-none -mlong-calls -Os -ffreestanding \
	-fno-common -ffunction-sections -fdata-sections
TARGET_ASFLAGS := -mcpu=bf703-none -Isrc/include
# The bootloader's build number (S-17.032), default 0.
ifneq ($(OPXY_BL_BUILD),)
TARGET_CFLAGS += -DOPXY_BL_VERSION_BUILD=$(OPXY_BL_BUILD)u
endif
# A test build's own version, as A.B.C, in place of the header's.
ifneq ($(OPXY_BL_VERSION),)
OPXY_BL_VERSION_WORDS := $(subst ., ,$(OPXY_BL_VERSION))
TARGET_CFLAGS += -DOPXY_BL_VERSION_MAJOR=$(word 1,$(OPXY_BL_VERSION_WORDS))u \
	-DOPXY_BL_VERSION_MINOR=$(word 2,$(OPXY_BL_VERSION_WORDS))u \
	-DOPXY_BL_VERSION_PATCH=$(word 3,$(OPXY_BL_VERSION_WORDS))u
endif
# S-05.025: the boot prompt (OPXY_BOOT_PROMPT, waffle/board/opxy_boot.h) is in
# the image only with BOOT_PROMPT=1. The host build has it on, so that its code
# is built, analyzed and tested there; its op is never set on the host.
# BOOT_CRUMBS=1: a trial boot's breadcrumbs (src/board/opxy_crumbs.c). In the
# bootloader alone, and it goes into the installer with it:
# make installer BOOT_CRUMBS=1 BUILD=build/crumbs
ifneq ($(BOOT_CRUMBS),)
TARGET_CFLAGS += -DOPXY_BOOT_CRUMBS_ON=1
endif
ifneq ($(BOOT_PROMPT),)
TARGET_CFLAGS += -DOPXY_BOOT_PROMPT=1
endif
# BOOT_TIMING=1: the boot's milestones timed (src/board/opxy_timing.c), never in
# a release. The report repeats on the log in held mode, and the monitor's
# test OS reads a hand-over's report (wiki/Test-OS.md):
# make installer test-os-mon BOOT_TIMING=1 BUILD=build/timing
ifneq ($(BOOT_TIMING),)
TARGET_CFLAGS += -DOPXY_BOOT_TIMING_ON=1
endif
# BOOT_LINK_SETTLE_US=n: the time between a byte of the slave's boot stream and
# the sample of PA6 that decides the next byte (OPXY_BOOT_LINK_SETTLE_US,
# waffle/board/opxy.h), default 2. How soon the slave's boot ROM raises PA6
# after a byte is not measured; a timing build measures it, and watches PA6
# for BOOT_TIMING_WINDOW_US after each byte when that is longer
# (wiki/Test-OS.md):
# make installer BOOT_TIMING=1 BOOT_TIMING_WINDOW_US=5 BUILD=build/timing
ifneq ($(BOOT_LINK_SETTLE_US),)
TARGET_CFLAGS += -DOPXY_BOOT_LINK_SETTLE_US=$(BOOT_LINK_SETTLE_US)u
endif
ifneq ($(BOOT_TIMING_WINDOW_US),)
TARGET_CFLAGS += -DOPXY_BOOT_TIMING_WINDOW_US=$(BOOT_TIMING_WINDOW_US)u
endif
# BOOT_LINK_FC=1: the slave's boot stream sent through the transmit FIFO with
# SPI0's flow control on PA6 holding off each byte, in place of a sample of
# PA6 before each byte (S-13b.022). Off by default: how soon the slave's boot
# ROM raises PA6 after a byte is not measured (wiki/Test-OS.md). The host
# build has it on, so that its code is built, analyzed and tested there:
# make installer BOOT_LINK_FC=1 BOOT_TIMING=1 BUILD=build/linkfc
ifneq ($(BOOT_LINK_FC),)
TARGET_CFLAGS += -DOPXY_BOOT_LINK_FC=1
endif
# REPLY_END=count: the slave's single-I/O replies end at their received-word
# count (REN set) from the start, in place of the factory's end, TF with REN
# clear (S-13b.083), which is the default. The test OS's replies follow it too:
# make installer REPLY_END=count BUILD=build/count
ifeq ($(REPLY_END),count)
TARGET_CFLAGS += -DOPXY_REPLY_END_COUNT=1
else ifneq ($(filter-out factory,$(REPLY_END)),)
$(error REPLY_END is factory or count)
endif
# LOG_DEBUG=1: a development build whose log shows the debug lines too
# (S-14.011's threshold starts at 4, not 3). Never a release.
ifneq ($(LOG_DEBUG),)
TARGET_CFLAGS += -DOPXY_LOG_DEBUG=1
endif
# YAFFS_ZERO_ERASE=1: a yaffs2 block emptied by writing zeros over its
# sectors in place of the eMMC erase S-12.042 asks for: a fallback should an
# erased group of the unit's eMMC not read 0x00 (S-03.003). Never a release.
ifneq ($(YAFFS_ZERO_ERASE),)
TARGET_CFLAGS += -DOPXY_YAFFS_ZERO_ERASE=1
endif
# The frozen front (src/front/, CONTRIBUTING.md): its own flags, so that no build
# option of the image changes its bytes.
FRONT_CFLAGS := -std=c11 $(WARNINGS) -Isrc/include -g -fdebug-prefix-map=$(CURDIR)=. \
	-mcpu=bf703-none -mlong-calls -Os -ffreestanding -fno-common \
	-ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns
HOST_LIB_CFLAGS  := $(COMMON) -O2 -ffreestanding -fno-common -DOPXY_BOOT_PROMPT=1 -DOPXY_BOOT_LINK_FC=1
HOST_TEST_CFLAGS := $(COMMON) -O2 -DOPXY_BOOT_PROMPT=1 -DOPXY_BOOT_LINK_FC=1

# lcd_gpio.c is the test OS's gpio variant alone (TESTOS_LCD_GPIO below).
# HOOK_SRCS hold strong definitions of the board's weak hooks; they come
# after the board's files, so that a host test's link, which takes an
# archive member for the first definition it finds, takes the weak ones.
# The test OS's own files (src/testos/, nothing outside the test OS) sort
# among the board's by name (by_name), since the link order places their
# code in the test OS's images.
HOOK_SRCS := src/board/opxy_display.c src/board/opxy_rtc_board.c
by_name = $(foreach f,$(sort $(1:src/testos/%=src/board/%)),\
	$(if $(filter $(f),$(1)),$(f),$(f:src/board/%=src/testos/%)))
SRCS  := $(filter-out src/drivers/bf70x/lcd_gpio.c $(HOOK_SRCS),\
	$(call by_name,$(wildcard src/*.c src/board/*.c src/testos/*.c src/drivers/bf70x/*.c))) $(HOOK_SRCS)
TESTS := $(sort $(wildcard tests/unit/test_*.c))
# Host-only test support (the register access fake, helpers).
TEST_SUPPORT := $(sort $(wildcard tests/unit/support/*.c))

HOST_OBJS  := $(SRCS:src/%.c=$(BUILD)/host/obj/%.o)
HOST_LIB   := $(BUILD)/host/libwaffle.a

# The front: every source on the target; on the host all but its drivers,
# which the host's tests stand in for.
FRONT_SRCS := $(sort $(wildcard src/front/*.c))
FRONT_ASM  := $(sort $(wildcard src/front/*.S))
FRONT_TARGET_ONLY := src/front/fr_board.c src/front/fr_libc.c $(wildcard src/front/fr_hw_*.c)
FRONT_HOST_OBJS := $(filter-out $(FRONT_TARGET_ONLY),$(FRONT_SRCS))
FRONT_HOST_OBJS := $(FRONT_HOST_OBJS:src/front/%.c=$(BUILD)/host/front/%.o)
FRONT_HOST_LIB  := $(BUILD)/host/libfront.a
HOST_TESTS := $(TESTS:tests/unit/%.c=$(BUILD)/host/tests/%)
TEST_SUPPORT_OBJS := $(TEST_SUPPORT:tests/unit/%.c=$(BUILD)/host/tests/%.o)
TEST_SUPPORT_LIB  := $(BUILD)/host/tests/libsupport.a

# Target-only start-up code.
ASM_SRCS   := $(call by_name,$(wildcard src/drivers/bf70x/*.S src/board/*.S src/testos/*.S))
CROSS_OBJS := $(SRCS:src/%.c=$(BUILD)/bf703/obj/%.o) \
	$(ASM_SRCS:src/%.S=$(BUILD)/bf703/obj/%.o)
CROSS_LIB  := $(BUILD)/bf703/libwaffle.a
# A .c and a .S with one name would share an object, and the archive would
# silently keep only one of them.
ifneq ($(words $(CROSS_OBJS)),$(words $(sort $(CROSS_OBJS))))
$(error a C and an assembly source share an object name: rename one)
endif

.PHONY: all host check cross image test-os analyze cppcheck size clean regen ui-update bump-version

all: host

# The vendored libraries and their glue (src/third_party/README.md), and the
# fuzzers of their parsers.
include src/third_party/third_party.mk
include tests/fuzz/fuzz.mk

HOST_LIB_CFLAGS  += $(TP_CPPFLAGS)
HOST_TEST_CFLAGS += $(TP_CPPFLAGS)
TARGET_CFLAGS    += $(TP_CPPFLAGS) $(LIBC_INCLUDES)
# The image spans L1, L2 and DDR: the vendored code's calls go through a
# register too.
$(foreach c,$(TP_COMPONENTS),$(TP_$(c)_TARGET_OBJS)): TP_CFLAGS += -mlong-calls
# S-09.072: the bridge's check runs over every byte an install sends, on both
# CPUs; -O2, as third_party.mk gives the CRCs, speeds its word loop.
$(BUILD)/bf703/obj/s09_bridge.o: TARGET_CFLAGS += -O2

host: $(HOST_LIB) $(FRONT_HOST_LIB) $(TP_HOST_LIBS) $(HOST_TESTS)

# The unit tests read the vectors of tests/oracle/, which tests/vectors/
# exports (and the S-12 images, which tests/oracle/S-12-filesystems writes).
VECTORS := $(BUILD)/vectors

# The unit tests' vector headers, written by tests/vectors/*/gen_*.py into
# $(GEN_VEC) and never tracked; a test includes "vectors/<name>.h". A
# generator runs once per header it writes (it replaces a header only when
# the text changes, so two runs at once under -j are safe), and its
# dependency file, <header>.d, names every file and directory it read, so a
# changed vector or generator rebuilds exactly what depends on it.
GEN     := $(BUILD)/gen
GEN_VEC := $(GEN)/vectors
VEC_HEADERS :=

# $(1): the header's name; $(2): its generator.
define VEC_HEADER
VEC_HEADERS += $(GEN_VEC)/$(1)
$(GEN_VEC)/$(1): $(2) tests/vectors/genout.py
	$$(PYTHON) -I -B $(2) --out $$(@D) --deps $$@.d
	@touch $$@
endef
$(eval $(call VEC_HEADER,s02_vectors.h,tests/vectors/s02/gen_s02_vectors.py))
$(eval $(call VEC_HEADER,s03_vectors.h,tests/vectors/s03/gen_s03_vectors.py))
$(eval $(call VEC_HEADER,s04_vectors.h,tests/vectors/s04/gen_s04_vectors.py))
$(eval $(call VEC_HEADER,s05_vectors.h,tests/vectors/s05/gen_s05_vectors.py))
$(eval $(call VEC_HEADER,s06_vectors.h,tests/vectors/s06/gen_s06.py))
$(eval $(call VEC_HEADER,s07_vectors.h,tests/vectors/s07/gen_s07.py))
$(eval $(call VEC_HEADER,s07_transcripts.h,tests/vectors/s07/gen_s07.py))
$(eval $(call VEC_HEADER,s08_vectors.h,tests/vectors/s08/gen_s08.py))
$(eval $(call VEC_HEADER,s08_transcripts.h,tests/vectors/s08/gen_s08.py))
$(eval $(call VEC_HEADER,s09_vectors.h,tests/vectors/s09/gen_s09.py))
$(eval $(call VEC_HEADER,test_s10a_vectors.h,tests/vectors/s10a/gen_s10a_vectors.py))
$(eval $(call VEC_HEADER,test_s10b_vectors.h,tests/vectors/s10b/gen_s10b_vectors.py))
$(eval $(call VEC_HEADER,s10c_vectors.h,tests/vectors/s10c/gen_s10c_vectors.py))
$(eval $(call VEC_HEADER,s11_vectors.h,tests/vectors/s11/gen_s11_vectors.py))
$(eval $(call VEC_HEADER,s13b_vectors.h,tests/vectors/s13b/gen_s13b_vectors.py))
$(eval $(call VEC_HEADER,s14_vectors.h,tests/vectors/s14/gen_s14_vectors.py))
$(eval $(call VEC_HEADER,s15_vectors.h,tests/vectors/s15/gen_s15_vectors.py))
$(eval $(call VEC_HEADER,s17_vectors.h,tests/vectors/s17/gen_s17_vectors.py))
$(eval $(call VEC_HEADER,wire_vectors.h,tests/vectors/wire/gen_wire_vectors.py))

# A test's own dependency file names the headers it includes once it has
# been built; before that, every test waits for every header.
HOST_TEST_CFLAGS += -I$(GEN)
$(HOST_TESTS): | $(VEC_HEADERS)

check: $(TESTS:tests/unit/%.c=check/%) check/image_tools check/front_image check/layering check/mtpcp \
	check/testos_mon check/wbrecover check/web check/bump_version check/generated check/leak \
	check/wiki_links

# The image tools' own tests (tests/tools/test_image_tools.py).
.PHONY: check/image_tools
check/image_tools:
	$(PYTHON) -I -B tests/tools/test_image_tools.py

# The frozen front's build checks (tests/tools/test_front_image.py).
.PHONY: check/front_image
check/front_image:
	$(PYTHON) -I -B tests/tools/test_front_image.py

# The core, board and driver layers' includes (tools/layering.py).
.PHONY: check/layering
check/layering:
	$(PYTHON) -I -B tests/tools/test_layering.py
	$(PYTHON) -I -B tools/layering.py

# tools/mtpcp.py against the MTP responder and a fake of the factory's.
.PHONY: check/mtpcp
check/mtpcp:
	CC="$(CC)" $(PYTHON) -I -B tests/tools/test_mtpcp.py

# tools/testos_mon.py's frame codec and transport, over a pseudo-terminal.
.PHONY: check/testos_mon
check/testos_mon:
	$(PYTHON) -I -B tests/tools/test_testos_mon.py

# tools/wbrecover.py against a model of recovery's protocol and the front's
# own C, over a pseudo-terminal.
.PHONY: check/wbrecover
check/wbrecover:
	CC="$(CC)" $(PYTHON) -I -B tests/tools/test_wbrecover.py

# docs/web/'s core.js against tools/wbrecover.py, on the same models and the
# front's own C (Node 20 or later; said and skipped without it).
.PHONY: check/web
check/web:
	CC="$(CC)" sh tests/web/run.sh

# wiki/'s links between pages, its pictures, and the tree's pointers into it.
.PHONY: check/wiki_links
check/wiki_links:
	$(PYTHON) -I -B tests/tools/test_wiki_links.py

# tools/bump_version.py on copies of the board header.
.PHONY: check/bump_version
check/bump_version:
	$(PYTHON) -I -B tests/tools/test_bump_version.py

# The vector files of tests/oracle/ from their generators (tools/regen.py;
# its --check runs in tools/conformance.sh).
regen:
	$(PYTHON) -I -B tools/regen.py

# After a change to a screen's wording, look or keys: the tests' S-15 header
# rebuilt from tests/vectors/s15/ui/ (nothing to commit), then docs/ui/
# redrawn. Then read the diff.
ui-update: $(GEN_VEC)/s15_vectors.h
	BUILD=$(BUILD) tools/ui-shots.sh

# Run once for each build that goes onto a unit: raises the patch version.
bump-version:
	$(PYTHON) -I -B tools/bump_version.py

# Nothing personal or foreign is tracked (tools/leak-check.py).
.PHONY: check/leak
check/leak:
	$(PYTHON) -I -B tests/tools/test_leak_check.py
	$(PYTHON) -I -B tools/leak-check.py tree

# Generated sources match their generators: the display font from the
# vendored Spleen BDF, quickly. The vector files of tests/oracle/:
# tools/regen.py --check, in tools/conformance.sh.
.PHONY: check/generated
check/generated:
	$(PYTHON) -I -B tools/bdf2c.py
	$(PYTHON) -I -B tools/bdf2c.py --front

$(VECTORS)/.stamp: tests/vectors/export.py $(shell find tests/oracle -name '*.py' -o -name '*.json' -o -name '*.bin')
	@rm -rf $(VECTORS)
	$(PYTHON) -I -B tests/vectors/export.py $(VECTORS)
	$(PYTHON) -I -B tests/oracle/S-12-filesystems/gen_vectors.py --images $(VECTORS)/s12
	@touch $@

# The start-up code linked alone with the BF703 linker script, so that a
# broken script or a missing start-up symbol fails the cross build. main
# is stood in for by the reset entry.
START_CHECK := $(BUILD)/bf703/start-check.elf
LDSCRIPT    := src/drivers/bf70x/bf703.ld

cross: $(CROSS_LIB) $(TP_TARGET_LIBS) $(START_CHECK) image front-image

$(START_CHECK): $(CROSS_LIB) $(BUILD)/bf703/libpicolibc.a $(LDSCRIPT)
	$(CROSS_CC) -mcpu=bf703-none -nostartfiles -nostdlib -T $(LDSCRIPT) \
		-Wl,-u,_wb_reset -Wl,--defsym,_main=_wb_reset \
		$(CROSS_LIB) $(BUILD)/bf703/libpicolibc.a -lgcc -o $@

# The bootloader image (S-01.010-S-01.017): every object linked by
# bf703.ld, its LDR stream (tools/mkldr.py), and the checks of its size, its
# handover closure (tools/layout.py) and of its stream and layout against
# S-01's own checkers.
IMAGE  := $(BUILD)/bf703/waffle-boot.elf
STREAM := $(IMAGE:.elf=.ldr)
LAYOUT := $(IMAGE:.elf=.layout.json)
LAYOUT_ARGS := --ld $(LDSCRIPT) --board src/include/waffle/board/opxy.h \
	--flash src/include/waffle/s02_flash.h --memmap src/include/waffle/s01_memmap.h \
	--stream $(STREAM)

# S-01.009: the stack the factory first stage calls the ROM's boot entry on,
# which no block of the stream may meet (S-02.091 rule 4).
FIRST_STAGE_STACK := 0x11807000:0x11808000
# S-01.007: what the INIT block runs on the ROM's entry SP, against the
# 0xE8 budget (check_stack.py). S-01.020's BCODE is check_stream.py's.
INIT_STACK := src/front/fr_entry.stack.json

# The frozen front (src/front/fr_handoff.h): linked alone by front.ld from
# its own objects and libgcc, so that a reference to anything else or a
# section front.ld does not place fails the link. Checked by
# tools/front_image.py: its includes (boundary; -MD lists the compiler's
# headers too), the INIT stage's reach into L1 (init; the link keeps its
# relocations, -q, which leaves the loaded bytes as they are), its frames
# (stack, from -fstack-usage), and its loaded bytes against the pin kept in
# the tree (pin). The image's stream carries it (mkldr.py --front);
# front-image streams it alone.
FRONT_LD    := src/front/front.ld
FRONT_PIN   := src/front/front.pin
FRONT_OBJS  := $(FRONT_SRCS:src/front/%.c=$(BUILD)/bf703/front/%.o) \
	$(FRONT_ASM:src/front/%.S=$(BUILD)/bf703/front/%.o)
FRONT_SU    := $(FRONT_SRCS:src/front/%.c=$(BUILD)/bf703/front/%.su)
FRONT_ELF   := $(BUILD)/bf703/front.elf
FRONT_OK    := $(BUILD)/bf703/front.checked
FRONT_STREAM := $(BUILD)/bf703/front.ldr

$(BUILD)/bf703/front/%.o $(BUILD)/bf703/front/%.su: src/front/%.c
	@mkdir -p $(@D)
	$(CROSS_CC) $(FRONT_CFLAGS) -fstack-usage -MD -MP -c $< -o $(@:.su=.o)

$(BUILD)/bf703/front/%.o: src/front/%.S
	@mkdir -p $(@D)
	$(CROSS_CC) -mcpu=bf703-none -MD -MP -c $< -o $@

$(FRONT_ELF): $(FRONT_OBJS) $(FRONT_LD)
	$(CROSS_CC) -mcpu=bf703-none -nostartfiles -nostdlib -T $(FRONT_LD) \
		-Wl,-q -Wl,--orphan-handling=error \
		-Wl,--gc-sections -Wl,-Map,$(@:.elf=.map) \
		-Wl,-u,_fr_entry -Wl,-u,_fr_init $(FRONT_OBJS) -lgcc -o $@

$(FRONT_OK): $(FRONT_ELF) $(FRONT_SU) $(FRONT_PIN) tools/front_image.py tools/layout.py \
		tools/mkldr.py tools/elf32.py
	$(PYTHON) -I -B tools/front_image.py boundary \
		--cc-include "$$($(CROSS_CC) -print-file-name=include)" $(FRONT_OBJS:.o=.d)
	$(PYTHON) -I -B tools/front_image.py init $(FRONT_ELF)
	$(PYTHON) -I -B tools/front_image.py stack --ld $(FRONT_LD) $(FRONT_SU)
	$(PYTHON) -I -B tools/front_image.py pin $(FRONT_ELF) $(FRONT_PIN)
	@touch $@

.PHONY: front-image front-pin
front-image: $(FRONT_STREAM)

$(FRONT_STREAM): $(FRONT_ELF) $(FRONT_OK) tools/mkldr.py tools/elf32.py
	$(PYTHON) -I -B tools/mkldr.py $(FRONT_ELF) $@.tmp --front-only
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_stream.py bootloader $@.tmp \
		--first-stage-stack $(FIRST_STAGE_STACK)
	@mv $@.tmp $@

front-pin: $(FRONT_ELF)
	$(PYTHON) -I -B tools/front_image.py pin $(FRONT_ELF) $(FRONT_PIN) --write

image: $(STREAM) $(LAYOUT)

# S-15.132: a build-time image for the hand-over frame, a 480 x 222 PNG.
# Its object is linked beside libwaffle, and its wb_display_boot_image
# replaces the board's weak default. The stamp relinks the image when LOGO
# is given, changed or dropped.
LOGO_STAMP := $(BUILD)/bf703/logo.stamp
LOGO_SRC   := $(BUILD)/bf703/gen/boot_image.c
LOGO_OBJ   := $(BUILD)/bf703/obj/gen/boot_image.o

$(LOGO_STAMP): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(LOGO)' | cmp -s - $@ || printf '%s\n' '$(LOGO)' > $@

$(LOGO_SRC): $(LOGO) tools/png2logo.py $(LOGO_STAMP)
	@mkdir -p $(@D)
	$(PYTHON) -I -B tools/png2logo.py $(LOGO) $@

$(LOGO_OBJ): $(LOGO_SRC)
	@mkdir -p $(@D)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

.PHONY: FORCE
FORCE:

# libwaffle is linked whole: a feature's strong hook in its own file must
# replace the board's weak default, and an archive member is not pulled for
# a symbol a weak definition already resolves. --gc-sections drops the rest.
$(IMAGE): $(CROSS_LIB) $(TP_TARGET_LIBS) $(LDSCRIPT) $(LOGO_STAMP) \
		$(if $(LOGO),$(LOGO_OBJ))
	$(CROSS_CC) -mcpu=bf703-none -nostartfiles -nostdlib -T $(LDSCRIPT) \
		-Wl,-q -Wl,--gc-sections -Wl,-Map,$(@:.elf=.map) \
		-Wl,-u,_wb_reset -Wl,-u,_main \
		$(if $(LOGO),$(LOGO_OBJ)) \
		-Wl,--whole-archive $(CROSS_LIB) -Wl,--no-whole-archive \
		-Wl,--start-group $(TP_TARGET_LIBS) -lgcc -Wl,--end-group -o $@

$(STREAM): $(IMAGE) $(FRONT_ELF) $(FRONT_OK) tools/mkldr.py tools/elf32.py $(INIT_STACK)
	$(PYTHON) -I -B tools/mkldr.py $< $@.tmp --front $(FRONT_ELF)
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_stack.py $(INIT_STACK)
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_stream.py bootloader $@.tmp \
		--first-stage-stack $(FIRST_STAGE_STACK)
	@mv $@.tmp $@

$(LAYOUT): $(IMAGE) $(STREAM) tools/layout.py tools/elf32.py
	$(PYTHON) -I -B tools/layout.py $< $(LAYOUT_ARGS) --json $@.tmp
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_layout.py $@.tmp
	@mv $@.tmp $@

# The test OS (wiki/Test-OS.md): the bootloader's held mode as an OS image
# that another bootloader's developer menu writes to a boot partition and
# boots (S-15.094), so that the drivers, menus, bridge, USB and display run
# on a unit before anything is written to the SPI flash. The same sources
# compiled with -DOPXY_TEST_OS=1 (waffle/testos.h) into their own objects,
# linked by bf703_testos.ld outside the handover reserve and the staging
# area (S-01.017) with src/testos/opxy_testos_entry.S as the entry
# (S-05.013, S-05.014), streamed with no INIT block (the clocks and DDR are
# up at OS entry, S-05.016), and gated by S-01's os
# profile, S-05.024's refusals and the layout rules (tools/testos_layout.py,
# check_layout.py). The vendored libraries are the normal build's. The bare
# stream is what goes to /yaffs2/os.ldr; the developer menu writes S-03's
# header. The S-13a table the image reads its registers against is
# generated from tests/oracle/S-13a-core-bring-up (tools/testos_regs.py).
#
# Two variants, one per panel byte path, built by make test-os
# into build/bf703-testos/waffle-test-<variant>.{elf,ldr,layout.json}:
#   smc   every byte the panel latches is a core write into SMC0 bank 0, D/C
#         on PA5, WR the SMC's strobe (src/drivers/bf70x/lcd.c, the normal
#         image's driver);
#   gpio  every byte bit-banged: PB0-PB7 data, D/C PA5, WR pulsed on PA14,
#         CS PA15, each pin a GPIO (src/drivers/bf70x/lcd_gpio.c, built only
#         here with -DOPXY_TESTOS_PANEL_GPIO=1, in place of lcd.c).
# TESTOS_PANEL=smc|gpio builds one: make test-os TESTOS_PANEL=gpio.
TESTOS_BUILD  := $(BUILD)/bf703-testos
TESTOS_SRCS   := $(SRCS)
TESTOS_ASM    := $(ASM_SRCS)
TESTOS_LD     := src/testos/bf703_testos.ld
TESTOS_REGS   := $(TESTOS_BUILD)/gen/testos_regs.c
TESTOS_CFLAGS := $(TARGET_CFLAGS) -DOPXY_TEST_OS=1
TESTOS_PANELS := smc gpio
TESTOS_PANEL  ?= $(TESTOS_PANELS)
TESTOS_LCD_GPIO := src/drivers/bf70x/lcd_gpio.c

$(TESTOS_REGS): tools/testos_regs.py tests/oracle/S-13a-core-bring-up/vectors/register_states.tsv
	@mkdir -p $(@D)
	$(PYTHON) -I -B tools/testos_regs.py $@

# One variant's objects, library, ELF, stream and layout: $(1) is smc or
# gpio. The gpio variant compiles lcd_gpio.c in place of lcd.c, with its
# macro; the smc variant is the normal driver.
define testos_variant
TESTOS_$(1)_OBJDIR := $(TESTOS_BUILD)/$(1)
TESTOS_$(1)_SRCS   := $(if $(filter gpio,$(1)),\
	$(filter-out src/drivers/bf70x/lcd.c,$(TESTOS_SRCS)) $(TESTOS_LCD_GPIO),\
	$(TESTOS_SRCS))
TESTOS_$(1)_CFLAGS := $(TESTOS_CFLAGS) $(TESTOS_FLAGS_$(1))
TESTOS_$(1)_OBJS   := $$(TESTOS_$(1)_SRCS:src/%.c=$$(TESTOS_$(1)_OBJDIR)/obj/%.o) \
	$(TESTOS_ASM:src/%.S=$$(TESTOS_$(1)_OBJDIR)/obj/%.o) \
	$$(TESTOS_$(1)_OBJDIR)/obj/gen/testos_regs.o \
	$(if $(filter installer,$(1)),$$(TESTOS_$(1)_OBJDIR)/obj/gen/installer_image.o)
TESTOS_$(1)_LIB    := $$(TESTOS_$(1)_OBJDIR)/libwaffle.a
TESTOS_$(1)_IMAGE  := $(TESTOS_BUILD)/waffle-test-$(1).elf
TESTOS_$(1)_STREAM := $(TESTOS_BUILD)/waffle-test-$(1).ldr
TESTOS_$(1)_LAYOUT := $(TESTOS_BUILD)/waffle-test-$(1).layout.json
TESTOS_ALL_OBJS    += $$(TESTOS_$(1)_OBJS)

test-os-$(1): $$(TESTOS_$(1)_STREAM) $$(TESTOS_$(1)_LAYOUT)
.PHONY: test-os-$(1)

$$(TESTOS_$(1)_OBJDIR)/obj/gen/%.o: $(TESTOS_BUILD)/gen/%.c
	@mkdir -p $$(@D)
	$(CROSS_CC) $$(TESTOS_$(1)_CFLAGS) -MMD -MP -c $$< -o $$@

$$(TESTOS_$(1)_OBJDIR)/obj/%.o: src/%.c
	@mkdir -p $$(@D)
	$(CROSS_CC) $$(TESTOS_$(1)_CFLAGS) -MMD -MP -c $$< -o $$@

$$(TESTOS_$(1)_OBJDIR)/obj/%.o: src/%.S
	@mkdir -p $$(@D)
	$(CROSS_CC) $(TARGET_ASFLAGS) -DOPXY_TEST_OS=1 -MMD -MP -c $$< -o $$@

$$(TESTOS_$(1)_LIB): $$(TESTOS_$(1)_OBJS)
	@rm -f $$@
	$(CROSS_AR) rcsD $$@ $$^

$$(TESTOS_$(1)_IMAGE): $$(TESTOS_$(1)_LIB) $(TP_TARGET_LIBS) $(TESTOS_LD)
	$(CROSS_CC) -mcpu=bf703-none -nostartfiles -nostdlib -T $(TESTOS_LD) \
		-Wl,-q -Wl,--gc-sections -Wl,-Map,$$(@:.elf=.map) \
		-Wl,-u,_wb_testos_entry -Wl,-u,_main $(TESTOS_KEEP_$(1)) \
		-Wl,--whole-archive $$(TESTOS_$(1)_LIB) -Wl,--no-whole-archive \
		-Wl,--start-group $(TP_TARGET_LIBS) -lgcc -Wl,--end-group -o $$@

$$(TESTOS_$(1)_STREAM): $$(TESTOS_$(1)_IMAGE) tools/mkldr.py tools/elf32.py
	$(PYTHON) -I -B tools/mkldr.py --no-init $$< $$@.tmp
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_stream.py os $$@.tmp
	@mv $$@.tmp $$@

$$(TESTOS_$(1)_LAYOUT): $$(TESTOS_$(1)_IMAGE) $$(TESTOS_$(1)_STREAM) tools/testos_layout.py tools/layout.py tools/elf32.py
	$(PYTHON) -I -B tools/testos_layout.py $$< --ld $(TESTOS_LD) \
		--board src/include/waffle/board/opxy.h --memmap src/include/waffle/s01_memmap.h \
		--stream $$(TESTOS_$(1)_STREAM) --json $$@.tmp
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_layout.py $$@.tmp
	@mv $$@.tmp $$@
endef

# The USB debug monitor's image (wiki/Test-OS.md).
TESTOS_MON := mon
TESTOS_FLAGS_gpio    := -DOPXY_TESTOS_PANEL_GPIO=1
TESTOS_FLAGS_mon     := -DOPXY_TESTOS_MONITOR=1
# The installer (wiki/Installer-Reference.md): the check suite, then this build's image
# written to the unit, every write held on the unit's key; no monitor.
TESTOS_INSTALLER := installer
TESTOS_FLAGS_installer := -DOPXY_TESTOS_CHECK=1 -DOPXY_TESTOS_INSTALLER=1
# A local, untracked local.mk may add targets, test-OS variants
# (TESTOS_EXTRA) and sources to analyze (LOCAL_SRCS).
TESTOS_EXTRA :=
-include local.mk
# The image the installer carries: this build's, checked as a bootloader
# stream and against the pinned front.
$(TESTOS_BUILD)/gen/installer_image.c: $(STREAM) tools/installer_image.py src/front/front.pin
	@mkdir -p $(@D)
	$(PYTHON) -I -B tests/oracle/S-01-boot-chain/check_stream.py bootloader $(STREAM)
	$(PYTHON) -I -B tools/installer_image.py $(STREAM) src/front/front.pin $@
$(foreach v,$(TESTOS_PANELS) $(TESTOS_MON) $(TESTOS_INSTALLER) $(TESTOS_EXTRA),$(eval $(call testos_variant,$(v))))

.PHONY: installer
installer: $(BUILD)/waffle-install.ldr
$(BUILD)/waffle-install.ldr: $(TESTOS_installer_STREAM) $(TESTOS_installer_LAYOUT)
	cp $< $@

test-os: $(addprefix test-os-,$(TESTOS_PANEL))

size: $(TP_TARGET_LIBS) $(LAYOUT)
	@for c in $(TP_COMPONENTS); do \
		$(CROSS_SIZE) -t $(BUILD)/bf703/lib$$c.a | tail -n 1 | \
			awk -v c=$$c '{ printf "%-10s text %7d  data %5d  bss %6d\n", c, $$1, $$2, $$3 }'; \
	done
	@$(PYTHON) -I -B tools/layout.py $(IMAGE) $(LAYOUT_ARGS)

$(BUILD)/host/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(HOST_LIB_CFLAGS) -MMD -MP -c $< -o $@

$(HOST_LIB): $(HOST_OBJS)
	@rm -f $@
	$(AR) rcsD $@ $^

$(BUILD)/host/front/%.o: src/front/%.c
	@mkdir -p $(@D)
	$(CC) $(HOST_LIB_CFLAGS) -MMD -MP -c $< -o $@

$(FRONT_HOST_LIB): $(FRONT_HOST_OBJS)
	@rm -f $@
	$(AR) rcsD $@ $^

$(BUILD)/host/tests/support/%.o: tests/unit/support/%.c
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_SUPPORT_LIB): $(TEST_SUPPORT_OBJS)
	@rm -f $@
	$(AR) rcsD $@ $^

# Driver, board and boot-flow tests need neither picolibc nor the vectors,
# so they also build where those do not (macOS cannot build picolibc's
# aliases). The board's tests take TinyUSB, FatFs, yaffs2 and the package
# libraries, which do build there: held mode brings in S-10a, the RAM
# disk's FAT glue and the scan's checks (S-11), and
# these call back into libwaffle, so they go in a group (macOS's linker
# needs none).
NOTP_TEST = $(CC) $(HOST_TEST_CFLAGS) -MMD -MP $< $(HOST_LIB) $(TEST_SUPPORT_LIB) -o $@
TU_HOST_LIB := $(BUILD)/host/libtinyusb.a
BOARD_TP_LIBS := $(TU_HOST_LIB) $(BUILD)/host/libfatfs.a $(BUILD)/host/libyaffs2.a \
	$(BUILD)/host/libcrc.a $(BUILD)/host/libzlib.a $(BUILD)/host/libminizip.a \
	$(BUILD)/host/libmicroecc.a $(BUILD)/host/libbearssl.a \
	$(BUILD)/host/libblockdev.a
ifeq ($(shell uname -s),Darwin)
BOARD_TEST_LIBS = $(HOST_LIB) $(TEST_SUPPORT_LIB) $(BOARD_TP_LIBS)
else
BOARD_TEST_LIBS = -Wl,--start-group $(HOST_LIB) $(TEST_SUPPORT_LIB) \
	$(BOARD_TP_LIBS) -Wl,--end-group
endif

$(BUILD)/host/tests/test_drv_%: tests/unit/test_drv_%.c $(TEST_SUPPORT_LIB) $(HOST_LIB)
	@mkdir -p $(@D)
	$(NOTP_TEST)

$(BUILD)/host/tests/test_board_%: tests/unit/test_board_%.c $(TEST_SUPPORT_LIB) $(HOST_LIB) $(BOARD_TP_LIBS)
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) -MMD -MP $< $(BOARD_TEST_LIBS) -o $@

$(BUILD)/host/tests/test_boot_%: tests/unit/test_boot_%.c $(TEST_SUPPORT_LIB) $(HOST_LIB)
	@mkdir -p $(@D)
	$(NOTP_TEST)

# The front's tests: the front's portable code, beside libwaffle, whose
# S-02 code some of them use as the reference.
$(BUILD)/host/tests/test_front_%: tests/unit/test_front_%.c $(TEST_SUPPORT_LIB) $(FRONT_HOST_LIB) $(HOST_LIB) $(TP_HOST_LIBS)
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) -MMD -MP $< $(FRONT_HOST_LIB) $(TEST_SUPPORT_LIB) $(HOST_LIB) \
		-Wl,--start-group $(TP_HOST_LIBS) -Wl,--end-group -o $@

$(BUILD)/host/tests/%: tests/unit/%.c $(TEST_SUPPORT_LIB) $(HOST_LIB) $(TP_HOST_LIBS)
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) -MMD -MP $< $(TEST_SUPPORT_LIB) $(HOST_LIB) \
		-Wl,--start-group $(TP_HOST_LIBS) -Wl,--end-group -o $@

$(BUILD)/bf703/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CROSS_CC) $(TARGET_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/bf703/obj/%.o: src/%.S
	@mkdir -p $(@D)
	$(CROSS_CC) $(TARGET_ASFLAGS) -MMD -MP -c $< -o $@

$(CROSS_LIB): $(CROSS_OBJS)
	@rm -f $@
	$(CROSS_AR) rcsD $@ $^

# Each test and each analyzed file is a target of its own, so that -j runs
# them side by side; the output of each comes out in one piece. The files
# with a test-OS variant (waffle/testos.h) are analyzed in it as well.
CHECKS  := $(TESTS:tests/unit/%.c=check/%)
ANALYZE := $(addprefix analyze/,$(SRCS) $(FRONT_SRCS) $(TP_GLUE) $(TESTS) $(TEST_SUPPORT) \
	$(filter %.c,$(LOCAL_SRCS)))
TESTOS_VARIANT := $(sort $(shell grep -l 'OPXY_TEST_OS\|OPXY_TESTOS' src/*.c src/board/*.c src/testos/*.c src/drivers/bf70x/*.c))
ANALYZE_TESTOS := $(addprefix analyze-testos/,$(TESTOS_VARIANT))
# The monitor's test OS with the hand-over timing report, the installer, and
# the development image (BOOT_CRUMBS=1 BOOT_TIMING=1 LOG_DEBUG=1), over the
# files their options reach.
ANALYZE_MON := $(addprefix analyze-mon/,$(filter-out $(TESTOS_LCD_GPIO),$(TESTOS_VARIANT)))
ANALYZE_INSTALLER := $(addprefix analyze-installer/,$(filter-out $(TESTOS_LCD_GPIO),$(TESTOS_VARIANT)))
DEV_VARIANT := $(sort $(shell grep -l 'OPXY_BOOT_CRUMBS\|OPXY_CRUMB\|OPXY_TIM\|OPXY_STARTUP\|S14_DEFAULT_THRESHOLD' \
	src/*.c src/board/*.c src/testos/*.c src/drivers/bf70x/*.c))
ANALYZE_DEV := $(addprefix analyze-dev/,$(DEV_VARIANT))
.PHONY: $(CHECKS) $(ANALYZE) $(ANALYZE_TESTOS) $(ANALYZE_MON) $(ANALYZE_INSTALLER) $(ANALYZE_DEV)

$(CHECKS): check/%: $(BUILD)/host/tests/% $(VECTORS)/.stamp
	@out=$$(WB_VECTORS=$(VECTORS) ./$< 2>&1); s=$$?; printf '%s\n' "$$out"; exit $$s

analyze: $(ANALYZE) $(ANALYZE_TESTOS) $(ANALYZE_MON) $(ANALYZE_INSTALLER) $(ANALYZE_DEV)

$(addprefix analyze/,$(TESTS)): | $(VEC_HEADERS)

$(ANALYZE): analyze/%:
	@out=$$($(CLANG) --analyze -Xanalyzer -analyzer-werror \
		-Xanalyzer -analyzer-output=text \
		$(COMMON) $(TP_CPPFLAGS) -I$(GEN) -DOPXY_BOOT_PROMPT=1 -DOPXY_BOOT_LINK_FC=1 $* -o /dev/null 2>&1); s=$$?; \
	printf '  ANALYZE %s\n' '$*'; [ -z "$$out" ] || printf '%s\n' "$$out"; exit $$s

$(ANALYZE_TESTOS): analyze-testos/%:
	@out=$$($(CLANG) --analyze -Xanalyzer -analyzer-werror \
		-Xanalyzer -analyzer-output=text \
		$(COMMON) $(TP_CPPFLAGS) -DOPXY_TEST_OS=1 \
		$(if $(filter $(TESTOS_LCD_GPIO),$*),-DOPXY_TESTOS_PANEL_GPIO=1) \
		$* -o /dev/null 2>&1); s=$$?; \
	printf '  ANALYZE %s (test OS)\n' '$*'; [ -z "$$out" ] || printf '%s\n' "$$out"; exit $$s

$(ANALYZE_MON): analyze-mon/%:
	@out=$$($(CLANG) --analyze -Xanalyzer -analyzer-werror \
		-Xanalyzer -analyzer-output=text \
		$(COMMON) $(TP_CPPFLAGS) -DOPXY_TEST_OS=1 $(TESTOS_FLAGS_mon) \
		-DOPXY_BOOT_TIMING_ON=1 $* -o /dev/null 2>&1); s=$$?; \
	printf '  ANALYZE %s (test OS monitor)\n' '$*'; [ -z "$$out" ] || printf '%s\n' "$$out"; exit $$s

$(ANALYZE_INSTALLER): analyze-installer/%:
	@out=$$($(CLANG) --analyze -Xanalyzer -analyzer-werror \
		-Xanalyzer -analyzer-output=text \
		$(COMMON) $(TP_CPPFLAGS) -DOPXY_TEST_OS=1 $(TESTOS_FLAGS_installer) \
		$* -o /dev/null 2>&1); s=$$?; \
	printf '  ANALYZE %s (installer)\n' '$*'; [ -z "$$out" ] || printf '%s\n' "$$out"; exit $$s

$(ANALYZE_DEV): analyze-dev/%:
	@out=$$($(CLANG) --analyze -Xanalyzer -analyzer-werror \
		-Xanalyzer -analyzer-output=text \
		$(COMMON) $(TP_CPPFLAGS) -DOPXY_BOOT_PROMPT=1 \
		-DOPXY_BOOT_CRUMBS_ON=1 -DOPXY_BOOT_TIMING_ON=1 -DOPXY_LOG_DEBUG=1 \
		-DOPXY_BOOT_LINK_FC=1 $* -o /dev/null 2>&1); s=$$?; \
	printf '  ANALYZE %s (development build)\n' '$*'; [ -z "$$out" ] || printf '%s\n' "$$out"; exit $$s

# cppcheck checks the files in parallel itself, with make's job count (-jN,
# from GNU make 4.2 on). Its whole-program checks then need a build
# directory, which is emptied first so that every run is a full one.
CPPCHECK_JOBS = $(or $(patsubst -j%,%,$(lastword $(filter -j%,$(MAKEFLAGS)))),1)

cppcheck: $(VEC_HEADERS)
	@rm -rf $(BUILD)/cppcheck && mkdir -p $(BUILD)/cppcheck
	$(CPPCHECK) -j $(CPPCHECK_JOBS) --cppcheck-build-dir=$(BUILD)/cppcheck \
		--std=c11 --enable=warning,style,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet \
		--suppress=missingIncludeSystem -Isrc/include -I$(GEN) \
		-Isrc/glue/crc -Isrc/glue/blockdev -Isrc/glue/fatfs -Isrc/glue/yaffs2 -Isrc/glue/minizip \
		-isrc/third_party src tests

clean:
	rm -rf $(BUILD)

-include $(HOST_OBJS:.o=.d) $(HOST_TESTS:=.d) $(TEST_SUPPORT_OBJS:.o=.d) \
	$(FRONT_HOST_OBJS:.o=.d) $(FRONT_OBJS:.o=.d) \
	$(CROSS_OBJS:.o=.d) $(TESTOS_ALL_OBJS:.o=.d) $(VEC_HEADERS:=.d)
