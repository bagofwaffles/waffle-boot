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
# choose it.
H_CFLAGS := -std=c11 $(WARNINGS) -Isrc/include -Itests/conformance -O2 -g -DOPXY_BOOT_LINK_FC=1

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

H_s02_SRCS := src/s02_flash.c src/s02_write.c src/s01_memmap.c src/s05_handoff.c \
	src/front/fr_slots.c src/front/fr_proto.c src/front/fr_image.c src/front/fr_util.c \
	src/front/fr_startlog.c
# Recovery's writer and protocol (S-02.084, S-02.093) and the start log (S-02.095) are the front's own.
H_s02_CPPFLAGS := -Isrc
# The settings sector (S-02.096-.100, S-02.105) and its CRC-32, zlib's (S-02.015).
H_s02_SRCS += src/s02_store.c src/wire_crc.c
H_s02_TP := $(call tp_o,$(TP_crc_SRCS) $(TP_crc_GLUE) $(TP_zlib_SRCS) $(TP_zlib_GLUE))
H_s03_SRCS := src/s03_emmc.c src/device.c
H_s04_SRCS := src/s04_records.c src/s10c_text.c src/div64.c src/s11_common.c src/s11_version.c src/device.c
H_s05_SRCS := src/s05_handoff.c src/s01_memmap.c src/device.c
# The boot flow's order (S-05.001, S-15.175); built with the boot prompt's op
# (S-05.025), which a run leaves NULL to stand for a build without it.
H_s05_SRCS += src/boot_flow.c
H_s05_CPPFLAGS := -DOPXY_BOOT_PROMPT=1
H_s06_SRCS := src/s06_frame.c src/s06_rx.c src/s06_link.c src/device.c
H_s07_SRCS := src/s07_rules.c src/div64.c src/s07_cypress.c src/device.c
# The PSoC link on the unit tests' clock and port (tests/unit/s07_test.h).
H_s07p_SRCS := src/s07_psoc.c src/div64.c src/s07_program.c src/s07_cypress.c src/s07_rules.c \
	src/s06_frame.c src/s06_rx.c src/s06_link.c \
	src/s15_prompt.c src/s15_console.c src/s15_lines.c src/s14_log.c src/s14_line.c \
	src/s17_revisions.c src/s11_common.c src/s11_version.c
H_s08_SRCS := src/s08_rules.c src/device.c
H_s09_SRCS := src/s09_bridge.c src/div64.c src/s09_master.c src/s09_slave.c src/s09_fast.c src/s06_frame.c src/s06_rx.c \
	src/s06_link.c
H_s10c_SRCS := src/s10c_sysex.c src/div64.c src/s10c_text.c src/s11_common.c src/s11_version.c
# The command server (S-10c.070, S-10c.076).
H_s10c_SRCS += src/s10c_host.c
H_s10a_SRCS := src/s10a_desc.c src/s10a_ch9.c src/s10a_scsi.c src/s10a_bot.c
H_s10b_SRCS := src/s10b_mtp.c src/div64.c src/s10b_text.c src/s04_records.c src/s10c_text.c src/s11_common.c \
	src/s11_version.c
H_s13c_SRCS := src/s13c_panel.c src/div64.c src/display_draw.c src/display_font.c src/display_title.c src/s15_console.c src/s15_lines.c \
	src/s15_prompt.c src/s14_log.c src/s14_line.c src/s17_revisions.c src/s11_common.c src/s11_version.c
# The front's start and recovery (src/front/'s portable code) and the model
# PSoC's S-06 framing.
H_s13cf_SRCS := src/front/fr_start.c src/front/fr_recovery.c src/front/fr_recscreen.c \
	src/front/fr_recui.c src/front/fr_panel.c src/front/fr_draw.c src/front/fr_font.c \
	src/front/fr_psoc.c src/front/fr_proto.c src/front/fr_slots.c src/front/fr_image.c \
	src/front/fr_startlog.c src/front/fr_util.c src/front/fr_device.c src/front/fr_logo.c \
	src/s06_frame.c src/s06_rx.c
H_s13cf_CPPFLAGS := -Isrc
H_s14_SRCS := src/s14_line.c src/s14_log.c src/s14_export.c
H_s11_SRCS := $(sort $(wildcard src/s11_*.c)) src/s17_revisions.c src/wire_crc.c src/s01_memmap.c src/s05_handoff.c \
	src/s02_flash.c \
	src/wire_package.c src/wire_zip.c src/wire_yaffs.c src/wire_ckpt.c src/wire_fat.c \
	src/s09_files.c src/s09_master.c src/s09_slave.c src/s09_bridge.c src/wire_s09.c src/s15_settings.c \
	src/s06_frame.c src/s06_rx.c src/s06_link.c src/s03_emmc.c src/s04_records.c \
	src/s10c_text.c src/div64.c
H_s12_SRCS := src/s04_records.c src/div64.c src/s10c_text.c src/s11_common.c src/s11_version.c \
	src/s11_content.c src/s14_export.c src/wire_fat.c src/wire_package.c src/wire_yaffs.c src/wire_ckpt.c \
	src/wire_crc.c src/wire_zip.c
# Programs that link the vendored libraries.
# S-18: the device table the modules read (src/device.c, with its CRC-32, zlib's)
H_DEVICE_TP := $(call tp_o,$(TP_crc_SRCS) $(TP_crc_GLUE) $(TP_zlib_SRCS) $(TP_zlib_GLUE))
H_s07p_SRCS += src/device.c src/wire_crc.c
H_s07p_TP := $(H_DEVICE_TP)
H_s10a_SRCS += src/device.c src/wire_crc.c
H_s10a_TP := $(H_DEVICE_TP)
H_s10c_SRCS += src/device.c src/wire_crc.c
H_s10c_TP := $(H_DEVICE_TP)
H_s13c_SRCS += src/device.c src/wire_crc.c
H_s13c_TP := $(H_DEVICE_TP)
H_s11_SRCS += src/device.c
H_s12_SRCS += src/device.c
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
