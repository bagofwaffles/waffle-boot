/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Configuration of the picolibc subset (src/third_party/picolibc), in place of
 * the picolibc.h that picolibc's own build generates from picolibc.h.in.
 * Only the options the imported sources test are set; everything else
 * keeps picolibc's default (off).
 */
#ifndef WAFFLE_PICOLIBC_H
#define WAFFLE_PICOLIBC_H

/* errno is one global variable: the bootloader has no threads. */
#define __GLOBAL_ERRNO 1
#define __SINGLE_THREAD 1

/*
 * Prefer small code (byte loops, no unrolling), except in memcpy and
 * memset, which the build compiles with WB_LIBC_FAST_COPY: they copy by
 * words.
 */
#ifndef WB_LIBC_FAST_COPY
#define __PREFER_SIZE_OVER_SPEED 1
#endif

/*
 * The inline ctype functions (ASCII only) rather than the table, whatever
 * the optimization level: picolibc picks them by default only at -Os.
 */
#define _PICOLIBC_CTYPE_SMALL 1

/*
 * GCC may turn the loops of memset and memcpy into calls to themselves;
 * this makes picolibc mark them so it does not (clang has an attribute
 * for it that picolibc finds on its own).
 */
#if defined(__GNUC__) && !defined(__clang__)
#define __HAVE_CC_INHIBIT_LOOP_TO_LIBCALL 1
#endif

/*
 * picolibc's own small stdio. printf-family functions are the integer
 * variant with long long ('l': no floating point), with the C99 length
 * modifiers (%zu, %jd, %td, %hhx).
 */
#define __TINY_STDIO 1
#define __IO_DEFAULT 'l'
#define __IO_C99_FORMATS 1

#endif /* WAFFLE_PICOLIBC_H */
