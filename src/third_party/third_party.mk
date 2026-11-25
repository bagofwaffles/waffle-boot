# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build rules for the vendored libraries (src/third_party/) and their glue
# (src/glue/). Included by the top-level Makefile, which defines BUILD, CC,
# CROSS_CC, CROSS_AR, WARNINGS, COMMON and TARGET_SPEED.
#
# Each component <c> is built into $(BUILD)/host/lib<c>.a and
# $(BUILD)/bf703/lib<c>.a from:
#   TP_<c>_SRCS    upstream sources, built with relaxed warnings
#                  (TP_WARN plus TP_<c>_WARN); the upstream code is not ours
#                  to clean up
#   TP_<c>_GLUE    our sources under src/glue/, built with the strict WARNINGS
#   TP_<c>_FLAGS   include paths and configuration for both
#
# The target's C library is the picolibc subset. On the host the components
# use the host's C library, and picolibc is built only for its own unit
# test, with its symbols renamed (pl_memcpy, ...) so that it cannot clash
# with the host's.

TP_COMPONENTS := picolibc crc blockdev fatfs yaffs2 zlib minizip microecc bearssl \
	tinyusb

# Every include directory of the components. Our code (src/glue/, src/, tests/)
# sees the upstream ones as system headers: their warnings are not ours.
TP_INCLUDES := -Isrc/glue/crc -Isrc/glue/blockdev -Isrc/glue/fatfs -Isrc/glue/yaffs2 -Isrc/glue/minizip \
	-isystem src/third_party/fatfs/source \
	-isystem src/third_party/yaffs2/direct -isystem src/third_party/yaffs2/core \
	-isystem src/third_party/zlib -isystem src/third_party/minizip \
	-isystem src/third_party/micro-ecc -isystem src/third_party/bearssl/inc \
	-Isrc/glue/tinyusb -isystem src/third_party/tinyusb/src

# micro-ecc: P-224 only (S-11.012); its header depends on these.
TP_DEFINES := -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 \
	-DuECC_SUPPORTS_secp224r1=1 -DuECC_SUPPORTS_secp256r1=0 \
	-DuECC_SUPPORTS_secp256k1=0 -DuECC_SUPPORT_COMPRESSED_POINT=0

TP_CPPFLAGS := $(TP_INCLUDES) $(TP_DEFINES)

# The target's C library headers: our configuration, then picolibc's.
LIBC_INCLUDES := -isystem src/glue/libc -isystem src/third_party/picolibc/libc/include

# Relaxed warnings for upstream code: reported, never errors.
TP_WARN := -Wall -Wno-unused-parameter

# ---------------------------------------------------------------------------
# Components

PL := src/third_party/picolibc/libc
TP_picolibc_SRCS := $(addprefix $(PL)/, \
	string/memchr.c string/memcmp.c string/memcpy.c string/memmove.c \
	string/memset.c string/strcat.c string/strchr.c string/strcmp.c \
	string/strcpy.c string/strcspn.c string/strlen.c string/strncat.c \
	string/strncmp.c string/strncpy.c string/strnlen.c string/strrchr.c \
	string/strspn.c string/strstr.c \
	stdio/snprintf.c stdio/vsnprintf.c stdio/vflprintf.c stdio/filestrput.c \
	stdio/strtol.c stdio/strtoul.c \
	stdlib/malloc.c stdlib/free.c stdlib/calloc.c stdlib/realloc.c \
	search/qsort.c errno/errno.c)
TP_picolibc_FLAGS := -ffreestanding -I$(PL)/locale -I$(PL)/stdio
TP_picolibc_WARN := -Wno-pragmas

TP_crc_GLUE := src/glue/crc/crc.c
# CRC-16/XMODEM runs over every byte of a package: -O2 gives its loop a
# hardware loop on the target.
TP_crc_FLAGS := -O2

TP_blockdev_GLUE := src/glue/blockdev/ramdisk.c

TP_fatfs_SRCS := src/third_party/fatfs/source/ff.c src/third_party/fatfs/source/ffunicode.c
TP_fatfs_GLUE := src/glue/fatfs/diskio_blockdev.c src/glue/fatfs/format.c

YAFFS := src/third_party/yaffs2
# Five core files are not built: under the one configuration of every
# device they do nothing or are never reached, and yaffs_fixed.c stands in.
YAFFS_UNBUILT := yaffs_tagscompat.c yaffs_ecc.c yaffs_nameval.c yaffs_endian.c \
	yaffs_verify.c
TP_yaffs2_SRCS := $(filter-out $(addprefix $(YAFFS)/core/,$(YAFFS_UNBUILT)), \
		$(sort $(wildcard $(YAFFS)/core/*.c))) \
	$(addprefix $(YAFFS)/direct/, yaffsfs.c yaffs_attribs.c yaffs_error.c \
		yaffs_hweight.c)
TP_yaffs2_GLUE := src/glue/yaffs2/yaffs_blockdev.c src/glue/yaffs2/yaffs_osglue.c \
	src/glue/yaffs2/yaffs_fixed.c
# yaffs_port.h includes waffle/util/div64.h.
TP_yaffs2_FLAGS := -Isrc/include -include src/glue/yaffs2/yaffs_port.h
TP_yaffs2_WARN := -Wno-sign-compare -Wno-unused-but-set-variable

TP_zlib_SRCS := $(addprefix src/third_party/zlib/, adler32.c crc32.c inffast.c \
	inflate.c inftrees.c zutil.c)
# Every byte a package extracts goes through inflate and CRC-32: built for
# speed on the target. BUILDFIXED: inflate builds its fixed-code tables in
# RAM on first use instead of carrying them (the same tables). Z_TESTW empty:
# crc32() byte by byte from its one 1 KiB table, without the 4 KiB braid
# tables; the bulk CRC-32s run on the CRC peripheral (src/board/install/crc.c).
TP_zlib_FLAGS := -O2 -DBUILDFIXED -DZ_TESTW=

TP_minizip_SRCS := src/third_party/minizip/unzip.c src/third_party/minizip/ioapi.c
TP_minizip_GLUE := src/glue/minizip/unzip_mem.c
TP_minizip_FLAGS := -DNOUNCRYPT -DMINIZIP_NO_STDIO -DIOAPI_NO_64

TP_microecc_SRCS := src/third_party/micro-ecc/uECC.c

BR := src/third_party/bearssl/src
TP_bearssl_SRCS := $(addprefix $(BR)/, hash/sha2small.c codec/dec32be.c \
	codec/enc32be.c symcipher/aes_common.c symcipher/aes_big_dec.c \
	symcipher/aes_big_cbcdec.c)
TP_bearssl_FLAGS := -I$(BR)

# TinyUSB: the device core only. The device controller driver
# (src/drivers/bf70x/usb_dcd.c) and the class drivers (src/core/usb/device.c) are
# ours; src/glue/tinyusb/tusb_config.h configures it. Upstream raises
# -Wconversion warnings, which TP_WARN does not enable.
TU := src/third_party/tinyusb/src
TP_tinyusb_SRCS := $(addprefix $(TU)/, tusb.c common/tusb_fifo.c device/usbd.c)

# ---------------------------------------------------------------------------
# Flags

TP_COMMON := -g -fdebug-prefix-map=$(CURDIR)=. $(TP_CPPFLAGS)

TP_HOST_CFLAGS := -std=gnu11 -O2 $(TP_COMMON)
TP_HOST_GLUE_CFLAGS := $(COMMON) -O2 $(TP_CPPFLAGS)

TP_TARGET_ARCH := -mcpu=bf703-none -Os -fno-common -ffunction-sections \
	-fdata-sections
TP_TARGET_CFLAGS := -std=gnu11 $(TP_TARGET_ARCH) $(TP_COMMON) $(LIBC_INCLUDES)
TP_TARGET_GLUE_CFLAGS := $(COMMON) $(TP_TARGET_ARCH) $(TP_CPPFLAGS) \
	$(LIBC_INCLUDES)
# A timing build's glue points (WB_YAFFS_TIME in src/glue/yaffs2/wb_yaffs.h):
# the glue is built apart from TARGET_CFLAGS, so it takes the switch here.
ifneq ($(BOOT_TIMING),)
TP_TARGET_GLUE_CFLAGS += -DOPXY_BOOT_TIMING_ON=1
endif

# picolibc on the host: only its own headers, no builtins, symbols renamed.
TP_HOST_LIBC_CFLAGS := -std=gnu11 -O2 $(TP_COMMON) -nostdinc $(LIBC_INCLUDES) \
	-isystem $(shell $(CC) -print-file-name=include) \
	-fno-stack-protector -fno-builtin -U_FORTIFY_SOURCE

# ---------------------------------------------------------------------------
# Rules

tp_obj = $(patsubst %.c,$(BUILD)/$(1)/obj/%.o,$(2))

define TP_COMPONENT
TP_$(1)_HOST_OBJS := $$(call tp_obj,host,$$(TP_$(1)_SRCS) $$(TP_$(1)_GLUE))
TP_$(1)_TARGET_OBJS := $$(call tp_obj,bf703,$$(TP_$(1)_SRCS) $$(TP_$(1)_GLUE))

$$(call tp_obj,host,$$(TP_$(1)_SRCS)): TP_CFLAGS := $$(TP_HOST_CFLAGS) $$(TP_$(1)_FLAGS) $$(TP_WARN) $$(TP_$(1)_WARN)
$$(call tp_obj,host,$$(TP_$(1)_GLUE)): TP_CFLAGS := $$(TP_HOST_GLUE_CFLAGS) $$(TP_$(1)_FLAGS)
$$(call tp_obj,bf703,$$(TP_$(1)_SRCS)): TP_CFLAGS := $$(TP_TARGET_CFLAGS) $$(TP_$(1)_FLAGS) $$(TP_WARN) $$(TP_$(1)_WARN)
$$(call tp_obj,bf703,$$(TP_$(1)_GLUE)): TP_CFLAGS := $$(TP_TARGET_GLUE_CFLAGS) $$(TP_$(1)_FLAGS)

$(BUILD)/bf703/lib$(1).a: $$(TP_$(1)_TARGET_OBJS)
	@rm -f $$@
	$$(CROSS_AR) qcsD $$@ $$^

ifneq ($(1),picolibc)
$(BUILD)/host/lib$(1).a: $$(TP_$(1)_HOST_OBJS)
	@rm -f $$@
	$$(AR) qcsD $$@ $$^
endif
endef

$(foreach c,$(TP_COMPONENTS),$(eval $(call TP_COMPONENT,$(c))))

# The CRCs and zlib are built for speed on the target (their -O2 above).
$(TP_crc_TARGET_OBJS) $(TP_zlib_TARGET_OBJS): TP_CFLAGS += $(TARGET_SPEED)
# Except adler32(): only a zlib-wrapped stream checks it, and the
# bootloader inflates raw deflate (src/core/install/zip.c).
$(call tp_obj,bf703,src/third_party/zlib/adler32.c): TP_CFLAGS += -Os

# memcpy and memset move every sector, chunk and window: they copy by words
# (src/glue/libc/picolibc.h), on the host too, where their test runs.
TP_LIBC_FAST := $(PL)/string/memcpy.c $(PL)/string/memset.c
$(call tp_obj,bf703,$(TP_LIBC_FAST)): TP_CFLAGS += $(TARGET_SPEED) -DWB_LIBC_FAST_COPY

# The host picolibc build differs from the generic one.
$(TP_picolibc_HOST_OBJS): TP_CFLAGS := $(TP_HOST_LIBC_CFLAGS) $(TP_picolibc_FLAGS) $(TP_WARN) $(TP_picolibc_WARN)
$(call tp_obj,host,$(TP_LIBC_FAST)): TP_CFLAGS += -DWB_LIBC_FAST_COPY

# One relocatable object with every symbol prefixed by pl_.
$(BUILD)/host/libpicolibc.a: $(TP_picolibc_HOST_OBJS)
	@rm -f $@ $(@:.a=.o)
	$(LD) -r -o $(@:.a=.o) $^
	$(OBJCOPY) --prefix-symbols=pl_ $(@:.a=.o)
	$(AR) qcsD $@ $(@:.a=.o)

$(BUILD)/host/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(TP_CFLAGS) -MD -MP -c $< -o $@

$(BUILD)/bf703/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CROSS_CC) $(TP_CFLAGS) -MD -MP -c $< -o $@

TP_HOST_LIBS := $(TP_COMPONENTS:%=$(BUILD)/host/lib%.a)
TP_TARGET_LIBS := $(TP_COMPONENTS:%=$(BUILD)/bf703/lib%.a)

TP_GLUE := $(foreach c,$(TP_COMPONENTS),$(TP_$(c)_GLUE))

TP_DEPS := $(foreach c,$(TP_COMPONENTS),$(TP_$(c)_HOST_OBJS:.o=.d) $(TP_$(c)_TARGET_OBJS:.o=.d))
-include $(TP_DEPS)
