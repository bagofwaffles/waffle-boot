/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-01: the memory map the boot chain's interfaces use, and the checks on
 * it: where an LDR stream of each stage may load, and an implementation's
 * declared layout.  Addresses are u32; range ends are computed in 64 bits,
 * so a range [start, start + count) never wraps.
 */
#ifndef WAFFLE_S01_MEMMAP_H
#define WAFFLE_S01_MEMMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S-01.012 address spaces, [start, end). */
#define S01_L2_START          0x08000000u
#define S01_L2_END            0x08040000u
#define S01_L1_DATA_A_START   0x11800000u
#define S01_L1_DATA_A_END     0x11808000u
#define S01_L1_DATA_B_START   0x11900000u
#define S01_L1_DATA_B_END     0x11908000u
#define S01_L1_CODE_START     0x11A00000u
#define S01_L1_CODE_END       0x11A0C000u
#define S01_L1_SCRATCH_START  0x11B00000u
#define S01_L1_SCRATCH_END    0x11B02000u
#define S01_DDR_START         0x80000000u
#define S01_DDR_END           0x90000000ull	/* one past the last DDR byte */
#define S01_RCU0_SVECT0       0x20000020u
#define S01_SPI2_WINDOW       0x40000000u

/* S-01.004 boot ROM entries, S-01.001 the first stage's boot command. */
#define S01_ROM_BASE          0x04000000u
#define S01_ROM_BOOT_KERNEL   0x04000008u
#define S01_ROM_BOOT          0x0400000Cu
#define S01_ROM_OTP_GET       0x04000048u
#define S01_ROM_OTP_CFG       0x04000054u
#define S01_FIRST_STAGE_BOOT_CMD 0x207u

/*
 * S-01.007 the SP a stage (the first stage, an INIT block, the bootloader)
 * is entered with: a 4-byte aligned value from S01_ROM_L2_ENTRY_SP_MIN up
 * to and including S01_ROM_RESET_SP, on the ROM's own stack in its boot
 * reserve; S01_ROM_L1_ENTRY_SP in L1 scratch (until S-01 question 1 is
 * settled); or, in a boot or kernel call, below the caller's SP. A stage
 * uses at most S01_ENTRY_STACK_BUDGET bytes below it before it moves SP
 * to a stack of its own, and relies on nothing else about it.
 */
#define S01_ROM_RESET_SP        0x08000BECu	/* the HRM's reset path */
#define S01_ROM_RESET_STACK_START 0x08000000u
#define S01_ROM_RESET_STACK_END 0x08000C00u
#define S01_ROM_L2_ENTRY_SP_MIN 0x080000E8u
#define S01_ROM_L1_ENTRY_SP     0x11B01FF0u
#define S01_ENTRY_STACK_BUDGET  0xE8u
/* S-01.007: SP stays at or above this while in L1 scratch, off the record. */
#define S01_RECORD_FLOOR        0x11B01F08u

/* S-01.008 the ROM's boot reserve in L2, which no stream loads into. */
#define S01_BOOT_RESERVE_START  0x08000000u
#define S01_BOOT_RESERVE_END    0x08002000u
/* S-01.008 the reboot record and the L1 scratch above it. */
#define S01_RECORD_START      0x11B01F00u
#define S01_RECORD_END        0x11B02000u
/* S-01.013 the handover reserve. */
#define S01_RESERVE_L2_START  0x0803C000u
#define S01_RESERVE_L2_END    0x0803E000u
#define S01_RESERVE_DDR_START 0x8803D000u
#define S01_RESERVE_DDR_END   0x88A65000u
/* S-01.014 the staging area. */
#define S01_STAGING_START     0x8A000000u
#define S01_STAGING_END       0x8A800000u
#define S01_STAGING_MAX       0x800000u
/* S-02.091 rule 4: the stack the first stage boots a slot on. */
#define S01_FACTORY_FS_STACK_START 0x11807000u
#define S01_FACTORY_FS_STACK_END   0x11808000u
/* S-01.020: bits 3-0 of every block code of the first-stage and bootloader streams. */
#define S01_SPI_MASTER_BCODE  0x4u
/* S-01.019 the display record. */
#define S01_DISPLAY_RECORD_START 0x8FFFFF00u
#define S01_DISPLAY_RECORD_END   0x8FFFFF2Cu

/* A half-open range [start, end), end in 64 bits. */
struct s01_range {
	uint32_t start;
	uint64_t end;
};

bool s01_meets(struct s01_range a, struct s01_range b);
/* Inside one of the six spaces of S-01.012. */
bool s01_inside_spaces(struct s01_range r);
bool s01_inside_l1(struct s01_range r);
/* L1 or L2 (S-01.005). */
bool s01_inside_on_chip(struct s01_range r);
bool s01_meets_reserve(struct s01_range r);

/* S-01.017: why an OS destination is incompatible, or S01_OS_OK. */
enum s01_os_dest {
	S01_OS_OK = 0,
	S01_OS_SPACE,		/* outside L1, L2 and DDR */
	S01_OS_STAGING,		/* meets the staging area */
	S01_OS_RESERVE,		/* meets the handover reserve */
	S01_OS_RECORD,		/* meets 0x11B01F00-0x11B02000 */
	S01_OS_BOOT_RESERVE	/* meets the ROM's boot reserve (S-01.008) */
};

enum s01_os_dest s01_os_destination(uint32_t target, uint32_t count);

/* --- The declared layout (S-01.011, S-01.013 to S-01.016, S-01.019) --- */

enum s01_kind { S01_CODE = 0, S01_DATA, S01_STACK, S01_HEAP, S01_HELD_BUFFER };

struct s01_region {
	uint32_t start;
	uint64_t end;
	enum s01_kind kind;
	bool handover;		/* used by the handover path */
};

/* Rule bits, set for each rule that fails (check_layout.py's names). */
#define S01_RULE_HANDOVER_IN_RESERVE   (1u << 0)
#define S01_RULE_HANDOVER_NOT_STAGING  (1u << 1)
#define S01_RULE_HANDOVER_NO_HEAP      (1u << 2)
#define S01_RULE_STAGING_CLEAR         (1u << 3)
#define S01_RULE_HELD_BUFFERS_CLEAR    (1u << 4)
#define S01_RULE_RECORD_CLEAR          (1u << 5)
#define S01_RULE_DISPLAY_RECORD_CLEAR  (1u << 6)
#define S01_RULE_INSIDE_SPACES         (1u << 7)
#define S01_RULE_HEAP_CLEAR            (1u << 8)	/* informative only */

/*
 * Returns the failing rules (0: the layout passes every rule).  The
 * staging_clear and held_buffers_clear rules are S-01.015's static check.
 */
uint32_t s01_layout_check(const struct s01_region *r, size_t n);

/* --- Where a stage's LDR stream may load (check_stream.py) --- */

enum s01_profile { S01_FIRST_STAGE = 0, S01_BOOTLOADER, S01_OS };

#define S01_SRULE_PARSE                (1u << 0)
#define S01_SRULE_INSIDE_SPACES        (1u << 1)
#define S01_SRULE_RECORD_AND_ROM_STACK (1u << 2)	/* S-01.008 */
#define S01_SRULE_ON_CHIP_ONLY         (1u << 3)	/* S-01.005 */
#define S01_SRULE_ONE_INIT_BLOCK       (1u << 4)	/* S-01.010 */
#define S01_SRULE_INIT_IN_L1           (1u << 5)
#define S01_SRULE_INIT_BEFORE_DDR      (1u << 6)
#define S01_SRULE_SPARES_FS_STACK      (1u << 7)	/* S-01.009 */
#define S01_SRULE_OUTSIDE_RESERVE      (1u << 8)	/* S-01.017 */
#define S01_SRULE_OUTSIDE_STAGING      (1u << 9)
#define S01_SRULE_FITS_STAGING         (1u << 10)
#define S01_SRULE_ROM_BOOT_RESERVE     (1u << 11)	/* S-01.008 */
#define S01_SRULE_BCODE                (1u << 12)	/* S-01.020 */

/*
 * Check a stage's stream against its profile.  fs_stack is the first stage's
 * declared stack for the bootloader profile, or NULL.  Returns the failing
 * rules; a stream that does not parse gives S01_SRULE_PARSE alone.
 */
uint32_t s01_stream_check(enum s01_profile p, const uint8_t *img, uint32_t len,
			  const struct s01_range *fs_stack);

/*
 * The same rules over a stream that arrives in pieces (an image as it is
 * extracted, S-11.035): s01_stream_begin(), s01_stream_feed() with each
 * piece in order, then s01_stream_end() for the failing rules. Bytes after
 * the FINAL block are taken and ignored. S01_SRULE_FITS_STAGING is not
 * checked here.
 */
struct s01_stream {
	enum s01_profile p;
	bool has_fs_stack;
	struct s01_range fs_stack;
	uint8_t hdr[16];
	unsigned hn;			/* header bytes held */
	uint64_t skip;			/* payload bytes still to pass */
	uint32_t fail;			/* rules failed so far */
	uint32_t pre;			/* INIT rules of the blocks up to the first INIT */
	unsigned inits;
	bool in_final;			/* the payload being passed is the FINAL block's */
	bool final;			/* the FINAL block has ended */
	bool bad;			/* a header failed */
};

void s01_stream_begin(struct s01_stream *s, enum s01_profile p,
		      const struct s01_range *fs_stack);
void s01_stream_feed(struct s01_stream *s, const uint8_t *p, size_t n);
uint32_t s01_stream_end(const struct s01_stream *s);

/*
 * S-02.091: a bootable stream of its kind. S01_BOOTLOADER: a slot image, by
 * the bootloader profile with the first stage's stack (rule 4); any other
 * profile by its own rules. s01_bootable_begin() starts the same check for
 * s01_stream_feed(); it is bootable when s01_stream_end() gives 0.
 */
void s01_bootable_begin(struct s01_stream *s, enum s01_profile p);
bool s01_bootable(enum s01_profile p, const uint8_t *img, uint32_t len);

#endif /* WAFFLE_S01_MEMMAP_H */
