# SPDX-License-Identifier: GPL-2.0-or-later
#
# The conformance harness's host programs (tests/conformance/README.md).
# Run from the repository root:
#
#   make -f tests/conformance/Makefile [CC=...] [BUILD=...]
#
# Each program h_<name> is built from tests/conformance/h_<name>.c, the
# harness support code and only the src/ files H_<name>_SRCS lists, so that
# drivers and adapters that do not build on the host stay out.  Output goes
# to $(BUILD)/conformance/.

BUILD ?= build
OUT   := $(BUILD)/conformance

# The same warnings as WARNINGS in the top-level Makefile; keep them equal.
WARNINGS := -Wall -Wextra -Werror -pedantic-errors \
	-Wshadow -Wundef -Wvla -Wdate-time \
	-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wold-style-definition -Wredundant-decls -Wnested-externs \
	-Wcast-qual -Wcast-align -Wpointer-arith -Wwrite-strings \
	-Wconversion -Wsign-conversion -Wdouble-promotion \
	-Wformat=2 -Wswitch-default -Wswitch-enum

# BOOT_LINK_FC's code is built in, as in the host build; a boot link's ops
# choose it. -Isrc, as in the host build, for includes rooted at src/
# ("front/handoff.h").
H_CFLAGS := -std=c11 $(WARNINGS) -Isrc/include -Isrc -Itests/conformance -O2 -g -DOPXY_BOOT_LINK_FC=1

H_SUPPORT := tests/conformance/hlib.c

# The vendored libraries (src/third_party/third_party.mk gives their sources and
# flags), built here as objects under $(OUT)/tp/ and linked as they are: the
# top-level Makefile's archives need GNU ar and ld's --start-group. picolibc
# is the target's C library; the host programs use the host's.
COMMON := -std=c11 $(WARNINGS) -Isrc/include -g -fdebug-prefix-map=$(CURDIR)=.
.DEFAULT_GOAL := all
include src/third_party/third_party.mk

TP_USED := crc blockdev fatfs yaffs2 zlib minizip microecc bearssl
tp_o = $(patsubst %.c,$(OUT)/tp/%.o,$(1))
$(foreach c,$(TP_USED),$(eval $(call tp_o,$(TP_$(c)_SRCS)): TP_CC_FLAGS := \
	$(TP_HOST_CFLAGS) $(TP_$(c)_FLAGS) $(TP_WARN) $(TP_$(c)_WARN)))
$(foreach c,$(TP_USED),$(eval $(call tp_o,$(TP_$(c)_GLUE)): TP_CC_FLAGS := \
	$(TP_HOST_GLUE_CFLAGS) $(TP_$(c)_FLAGS)))
TP_OBJS := $(foreach c,$(TP_USED),$(call tp_o,$(TP_$(c)_SRCS) $(TP_$(c)_GLUE)))

$(OUT)/tp/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(TP_CC_FLAGS) -c $< -o $@

H_s02_SRCS := src/core/flash/layout.c src/core/flash/write.c src/core/boot/memmap.c src/core/boot/handoff.c \
	src/front/slots/slots.c src/front/recovery/proto.c src/front/slots/image.c src/front/start/util.c \
	src/front/slots/startlog.c
# Recovery's writer and protocol (S-02.084, S-02.093) and the start log (S-02.095) are the front's own.
H_s02_CPPFLAGS := -Isrc
# The settings sector (S-02.096-.100, S-02.105) and its CRC-32, zlib's (S-02.015).
H_s02_SRCS += src/core/flash/settings.c src/core/util/crc.c
# The boot logo (S-02.109 - S-02.112): the main image's and the front's.
H_s02_SRCS += src/core/flash/logo.c src/front/display/logo.c
H_s02_TP := $(call tp_o,$(TP_crc_SRCS) $(TP_crc_GLUE) $(TP_zlib_SRCS) $(TP_zlib_GLUE))
H_s03_SRCS := src/core/emmc/emmc.c src/core/boot/device.c
H_s04_SRCS := src/core/boot/records.c src/core/host/text.c src/core/util/div64.c src/core/install/common.c src/core/util/version.c src/core/boot/device.c
H_s05_SRCS := src/core/boot/handoff.c src/core/boot/memmap.c src/core/boot/device.c
# The boot flow's order (S-05.001, S-15.175); built with the boot prompt's op
# (S-05.025), which a run leaves NULL to stand for a build without it.
H_s05_SRCS += src/core/boot/flow.c
H_s05_CPPFLAGS := -DOPXY_BOOT_PROMPT=1
H_s06_SRCS := src/core/link/frame.c src/core/link/rx.c src/core/link/link.c src/core/boot/device.c
H_s07_SRCS := src/core/psoc/rules.c src/core/util/div64.c src/core/psoc/cypress.c src/core/boot/device.c
# The PSoC link on the unit tests' clock and port (tests/unit/s07_test.h).
H_s07p_SRCS := src/core/psoc/psoc.c src/core/util/div64.c src/core/psoc/program.c src/core/psoc/cypress.c src/core/psoc/rules.c \
	src/core/link/frame.c src/core/link/rx.c src/core/link/link.c \
	src/core/ui/prompt.c src/core/ui/console.c src/core/ui/lines.c src/core/log/log.c src/core/log/line.c \
	src/core/boot/revisions.c src/core/install/common.c src/core/util/version.c
H_s08_SRCS := src/core/nordic/rules.c src/core/boot/device.c
H_s09_SRCS := src/core/bridge/records.c src/core/util/div64.c src/core/bridge/master.c src/core/bridge/slave.c src/core/bridge/fast.c src/core/link/frame.c src/core/link/rx.c \
	src/core/link/link.c
H_s10c_SRCS := src/core/host/sysex.c src/core/util/div64.c src/core/host/text.c src/core/install/common.c src/core/util/version.c
# The command server (S-10c.070, S-10c.076).
H_s10c_SRCS += src/core/host/commands.c
H_s10a_SRCS := src/core/usb/desc.c src/core/usb/ch9.c src/core/usb/msc_scsi.c src/core/usb/msc_bot.c
H_s10b_SRCS := src/core/mtp/mtp.c src/core/util/div64.c src/core/mtp/text.c src/core/boot/records.c src/core/host/text.c src/core/install/common.c \
	src/core/util/version.c
H_s13c_SRCS := src/core/display/panel.c src/core/util/div64.c src/core/display/draw.c src/core/display/font.c src/core/ui/console.c src/core/ui/lines.c \
	src/core/ui/prompt.c src/core/log/log.c src/core/log/line.c src/core/boot/revisions.c src/core/install/common.c src/core/util/version.c
# The front's start and recovery (src/front/'s portable code) and the model
# PSoC's S-06 framing.
H_s13cf_SRCS := src/front/start/start.c src/front/recovery/recovery.c src/front/recovery/screen.c \
	src/front/recovery/ui.c src/front/display/panel.c src/front/display/draw.c src/front/display/font.c \
	src/front/psoc/psoc.c src/front/recovery/proto.c src/front/slots/slots.c src/front/slots/image.c \
	src/front/slots/startlog.c src/front/start/util.c src/front/start/device.c src/front/display/logo.c \
	src/core/link/frame.c src/core/link/rx.c
H_s13cf_CPPFLAGS := -Isrc
H_s14_SRCS := src/core/log/line.c src/core/log/log.c src/core/log/export.c
H_s11_SRCS := src/core/install/common.c src/core/install/content.c src/core/install/install.c src/core/install/package.c src/core/install/run.c src/core/install/staging.c src/core/util/version.c src/core/boot/revisions.c src/core/util/crc.c src/core/boot/memmap.c src/core/boot/handoff.c \
	src/core/flash/layout.c \
	src/core/install/crypto.c src/core/install/zip.c src/core/fs/yaffs.c src/core/fs/checkpoint.c src/core/fs/fat.c \
	src/core/bridge/files.c src/core/bridge/master.c src/core/bridge/slave.c src/core/bridge/records.c src/core/bridge/slave_files.c src/core/ui/settings.c \
	src/core/ui/osfiles.c src/core/link/frame.c src/core/link/rx.c src/core/link/link.c src/core/emmc/emmc.c src/core/boot/records.c \
	src/core/host/text.c src/core/util/div64.c
H_s12_SRCS := src/core/boot/records.c src/core/util/div64.c src/core/host/text.c src/core/install/common.c src/core/util/version.c \
	src/core/install/content.c src/core/log/export.c src/core/fs/fat.c src/core/install/crypto.c src/core/fs/yaffs.c src/core/fs/checkpoint.c \
	src/core/util/crc.c src/core/install/zip.c
# Programs that link the vendored libraries.
# S-18: the device table the modules read (src/core/boot/device.c, with its CRC-32, zlib's)
H_DEVICE_TP := $(call tp_o,$(TP_crc_SRCS) $(TP_crc_GLUE) $(TP_zlib_SRCS) $(TP_zlib_GLUE))
H_s07p_SRCS += src/core/boot/device.c src/core/util/crc.c
H_s07p_TP := $(H_DEVICE_TP)
H_s10a_SRCS += src/core/boot/device.c src/core/util/crc.c
H_s10a_TP := $(H_DEVICE_TP)
H_s10c_SRCS += src/core/boot/device.c src/core/util/crc.c
H_s10c_TP := $(H_DEVICE_TP)
H_s13c_SRCS += src/core/boot/device.c src/core/util/crc.c
H_s13c_TP := $(H_DEVICE_TP)
H_s11_SRCS += src/core/boot/device.c
H_s12_SRCS += src/core/boot/device.c
H_s11_TP := $(TP_OBJS)
H_s12_TP := $(TP_OBJS)
# Programs that need only a vendored library's header.
H_s10a_CPPFLAGS := -Isrc/glue/blockdev

PROGS := s02 s03 s04 s05 s06 s07 s07p s08 s09 s10a s10b s10c s11 s12 s13c s13cf s14

.PHONY: all clean
all: $(PROGS:%=$(OUT)/h_%)

.SECONDEXPANSION:
$(OUT)/h_%: tests/conformance/h_%.c $(H_SUPPORT) tests/conformance/hlib.h $$(H_$$*_SRCS) $$(H_$$*_TP)
	@mkdir -p $(@D)
	$(CC) $(H_CFLAGS) $(if $(H_$*_TP),$(TP_CPPFLAGS)) $(H_$*_CPPFLAGS) $< $(H_SUPPORT) $(H_$*_SRCS) $(H_$*_TP) -o $@

clean:
	rm -rf $(OUT)
