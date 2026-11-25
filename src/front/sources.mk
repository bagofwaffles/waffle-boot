# SPDX-License-Identifier: GPL-2.0-or-later
#
# The front's sources, included by the top-level Makefile. The order of
# FRONT_SRCS, then FRONT_ASM, is the link order: it places the front's code
# and data, and so decides the loaded bytes that front.pin holds.

FRONT_SRCS := \
	src/front/start/board.c \
	src/front/start/cache.c \
	src/front/start/device.c \
	src/front/display/draw.c \
	src/front/display/font.c \
	src/front/hw/cache.c \
	src/front/hw/flash.c \
	src/front/hw/lcd.c \
	src/front/hw/port.c \
	src/front/hw/uart.c \
	src/front/hw/usb.c \
	src/front/slots/image.c \
	src/front/start/init.c \
	src/front/start/libc.c \
	src/front/display/logo.c \
	src/front/display/panel.c \
	src/front/recovery/proto.c \
	src/front/psoc/psoc.c \
	src/front/recovery/recovery.c \
	src/front/recovery/screen.c \
	src/front/recovery/ui.c \
	src/front/slots/slots.c \
	src/front/start/start.c \
	src/front/slots/startlog.c \
	src/front/recovery/usb_desc.c \
	src/front/start/util.c

FRONT_ASM := \
	src/front/start/entry.S

# Built for the target only; on the host the tests and the host's C library
# stand in for them.
FRONT_TARGET_ONLY := \
	src/front/start/board.c \
	src/front/start/libc.c \
	src/front/hw/cache.c \
	src/front/hw/flash.c \
	src/front/hw/lcd.c \
	src/front/hw/port.c \
	src/front/hw/uart.c \
	src/front/hw/usb.c
