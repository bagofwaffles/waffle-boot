# SPDX-License-Identifier: GPL-2.0-or-later
#
# The front's sources, included by the top-level Makefile. The order of
# FRONT_SRCS, then FRONT_ASM, is the link order: it places the front's code
# and data, and so decides the loaded bytes that front.pin holds.

FRONT_SRCS := \
	src/front/fr_board.c \
	src/front/fr_cache.c \
	src/front/fr_device.c \
	src/front/fr_draw.c \
	src/front/fr_font.c \
	src/front/fr_hw_cache.c \
	src/front/fr_hw_flash.c \
	src/front/fr_hw_lcd.c \
	src/front/fr_hw_port.c \
	src/front/fr_hw_uart.c \
	src/front/fr_hw_usb.c \
	src/front/fr_image.c \
	src/front/fr_init.c \
	src/front/fr_libc.c \
	src/front/fr_logo.c \
	src/front/fr_panel.c \
	src/front/fr_proto.c \
	src/front/fr_psoc.c \
	src/front/fr_recovery.c \
	src/front/fr_recscreen.c \
	src/front/fr_recui.c \
	src/front/fr_slots.c \
	src/front/fr_start.c \
	src/front/fr_startlog.c \
	src/front/fr_usb_desc.c \
	src/front/fr_util.c

FRONT_ASM := \
	src/front/fr_entry.S

# Built for the target only; on the host the tests and the host's C library
# stand in for them.
FRONT_TARGET_ONLY := \
	src/front/fr_board.c \
	src/front/fr_libc.c \
	src/front/fr_hw_cache.c \
	src/front/fr_hw_flash.c \
	src/front/fr_hw_lcd.c \
	src/front/fr_hw_port.c \
	src/front/fr_hw_uart.c \
	src/front/fr_hw_usb.c
