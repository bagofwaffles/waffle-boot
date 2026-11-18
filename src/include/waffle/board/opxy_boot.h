/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOARD_OPXY_BOOT_H
#define WAFFLE_BOARD_OPXY_BOOT_H

/*
 * The OP-XY's boot steps: the ops of waffle/boot.h for each role, built
 * from the S-xx logic, the wire_* adapters and the drivers. One image,
 * two roles (S-01.003): opxy_main() reads the strap and runs one of them.
 */

#include <stdbool.h>
#include <stdint.h>

#include "front/fr_handoff.h"
#include "waffle/board/opxy_ops.h"
#include "waffle/board/opxy_services.h"
#include "waffle/boot.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/s02_flash.h"
#include "waffle/s03_emmc.h"
#include "waffle/s04_records.h"
#include "waffle/s06_link.h"
#include "waffle/s07_psoc.h"
#include "waffle/s08_nordic.h"
#include "waffle/s09_master.h"
#include "waffle/s09_slave.h"

/*
 * S-05.025: the boot prompt, a property of the build: make BOOT_PROMPT=1
 * defines OPXY_BOOT_PROMPT=1; off by default and in every release, and
 * nothing at run time turns it on. With it off the prompt's code is not in
 * the image (opxy_prompt.c compiles to nothing, the op is NULL).
 */
/*
 * A trial boot's breadcrumbs (src/board/opxy_crumbs.c): BOOT_CRUMBS=1, never in
 * a release and never in the test OS. They go to the last sector of the slot
 * the selector does not name, the trial's.
 */
#if defined(OPXY_BOOT_CRUMBS_ON) && !defined(OPXY_TEST_OS)
#define OPXY_BOOT_CRUMBS 1
void opxy_crumb(unsigned n);
#define OPXY_CRUMB(n) opxy_crumb(n)
void opxy_crumb_read(uint32_t addr);
#define OPXY_CRUMB_READ(a) opxy_crumb_read(a)
#else
#define OPXY_BOOT_CRUMBS 0
#define OPXY_CRUMB(n) ((void)0)
#define OPXY_CRUMB_READ(a) ((void)0)
#endif

/*
 * Boot milestone timing (src/board/opxy_timing.c, waffle/boot_time.h):
 * BOOT_TIMING=1, never in a release. The bootloader stamps the master's
 * milestones, repeats the report in held mode and, at a hand-over, leaves it
 * at OPXY_BOOT_TIME_RECORD, between the frame buffers and the display record;
 * the test OS built with the option reads it at entry and logs it.
 */
#define OPXY_BOOT_TIME_RECORD 0x8FFFF800u
#if defined(OPXY_BOOT_TIMING_ON) && !defined(OPXY_TEST_OS)
#define OPXY_BOOT_TIMING 1
#include "waffle/boot_time.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/s07_psoc.h"
extern struct boot_time opxy_boot_time;
#define OPXY_TIME(id) boot_time_mark(&opxy_boot_time, (id), wb_core_cycles())
void opxy_timing_start(void);
/* S-07.023 steps 1-6, as s07_startup(), with the start-up's stamps. */
int opxy_timing_startup(struct s07_psoc *p, struct s07_key_status *ks);
void opxy_timing_battery(bool on);
/* The front's stamps and the waits its flags tell of. */
void opxy_timing_front(const struct fr_handoff *ho);
/* The boot link's ops with the stream's pacing counted: the bytes whose
 * deciding sample of the ready line found it high and the time waited, and
 * the line watched through a window after each byte, in place of the settle
 * time (struct boot_time_watch). The window is the settle time, or longer
 * with make BOOT_TIMING_WINDOW_US=n, which a build without timing ignores.
 * The stream's steps, its time split by where it went, and its blocks
 * (struct boot_time_split); the stream's end stamped by the second. */
void opxy_timing_stream(struct s09_boot_ops *ops);
void opxy_timing_stream_end(void);
/* The stream's read by the flash's faster read: how it came, after
 * s02_image_read(), and what its CRC then made of it, after
 * s02_image_check(). */
void opxy_timing_slot_read(const struct s02_image *im);
void opxy_timing_slot_check(const struct s02_image *im);
#ifndef OPXY_BOOT_TIMING_WINDOW_US
#define OPXY_BOOT_TIMING_WINDOW_US OPXY_BOOT_LINK_SETTLE_US
#endif
/* The OS image's copy: armed for its eMMC reads, each read counted, the
 * link it went on and its CRC's end. */
void opxy_timing_copy(struct opxy_master *m, bool on);
void opxy_timing_read(uint32_t bytes);
/* The copy read again in single I/O (S-09.081). */
void opxy_timing_reread(void);
/* The record an OS finds after the hand-over. */
void opxy_timing_leave(void);
/* A developer hand-over's line window that no wait on PA6 came before:
 * PA6 at its start (start true), then at each sample; its first low is
 * the slave's server stop, the first high after it the slave's OS. */
void opxy_timing_pa6(bool start, bool high);
/* Each pass of held mode: the report, a line every 100 ms, then 2 s apart. */
void opxy_timing_held(struct opxy_master *m);
#define OPXY_STARTUP opxy_timing_startup
#define OPXY_TIMING_START() opxy_timing_start()
#define OPXY_TIMING_BATTERY(on) opxy_timing_battery(on)
#define OPXY_TIMING_FRONT(m) opxy_timing_front(&(m)->ho)
#define OPXY_TIMING_STREAM(ops) opxy_timing_stream(ops)
#define OPXY_TIMING_STREAM_END() opxy_timing_stream_end()
#define OPXY_TIMING_SLOT_READ(im) opxy_timing_slot_read(im)
#define OPXY_TIMING_SLOT_CHECK(im) opxy_timing_slot_check(im)
#define OPXY_TIMING_COPY(m, on) opxy_timing_copy(m, on)
#define OPXY_TIMING_READ(n) opxy_timing_read(n)
#define OPXY_TIMING_REREAD() opxy_timing_reread()
#define OPXY_TIMING_LEAVE() opxy_timing_leave()
#define OPXY_TIMING_HELD(m) opxy_timing_held(m)
#define OPXY_TIMING_LOGO_KEPT(kept) \
	((kept) ? (void)(opxy_boot_time.flags |= BOOT_TIME_LOGO_KEPT) : (void)0)
#else
#define OPXY_BOOT_TIMING 0
#define OPXY_TIME(id) ((void)0)
#define OPXY_STARTUP s07_startup
#define OPXY_TIMING_START() ((void)0)
#define OPXY_TIMING_BATTERY(on) ((void)0)
#define OPXY_TIMING_FRONT(m) ((void)(m))
#define OPXY_TIMING_STREAM(ops) ((void)0)
#define OPXY_TIMING_STREAM_END() ((void)0)
#define OPXY_TIMING_SLOT_READ(im) ((void)0)
#define OPXY_TIMING_SLOT_CHECK(im) ((void)0)
#define OPXY_TIMING_COPY(m, on) ((void)0)
#define OPXY_TIMING_READ(n) ((void)0)
#define OPXY_TIMING_REREAD() ((void)0)
#define OPXY_TIMING_LEAVE() ((void)0)
#define OPXY_TIMING_HELD(m) ((void)0)
#define OPXY_TIMING_LOGO_KEPT(kept) ((void)(kept))
#endif
#if defined(OPXY_BOOT_TIMING_ON) && defined(OPXY_TEST_OS)
/* The test OS: the loading bootloader's record, taken at entry, then logged. */
void opxy_testos_timing_capture(void);
void opxy_testos_timing_log(struct opxy_master *m);
#define OPXY_TESTOS_TIMING(x) x
#else
#define OPXY_TESTOS_TIMING(x)
#endif

#ifndef OPXY_BOOT_PROMPT
#define OPXY_BOOT_PROMPT 0
#endif

/*
 * The slave's stream (S-09.002), as opxy_master.c's slave start takes it:
 * what the running slot is compared with and read by, then the two steps.
 * opxy_stream_slot() gives the running slot, or with *established false
 * the selected one and a log line, its stream read once into the stream
 * buffer when it can be. opxy_stream_checked() gives the stream of the slot
 * to send, read now if the buffer holds another's, and whether it is to be
 * sent: S-02.016 good over those bytes, or tried (the trial mark) when
 * `running` says this start runs that slot; *len is 0 when it is not. Not in
 * the test OS, which streams nothing.
 */
struct opxy_stream_src {
	const struct s02_mem_ops *mem;	/* the memory this image was loaded into */
	struct s02_probe probe;		/* the bootloader version's bytes there */
	s02_read_fn fast;		/* the stream's faster read, or NULL */
	void *fast_ctx;
};

unsigned opxy_stream_slot(struct opxy_master *m, const struct opxy_stream_src *src,
			  bool *established);
bool opxy_stream_checked(struct opxy_master *m, unsigned slot, bool running,
			 const uint8_t **stream, uint32_t *len);
/* Whether the last slot opxy_stream_checked() judged was slot, read under
 * the trial mark (S-02.016): this start runs a tried slot. */
bool opxy_stream_trial(unsigned slot);

/*
 * With BOOT_LINK_FC=1 or 2 (S-09.009): how this start's stream went, kept
 * from the slave start for its log line and a developer's request
 * (src/board/opxy_stream.c). begin() before the stream, end() with
 * s09_boot_slave()'s outcome, linked() once the bridge's start-up has ended
 * either way, which logs the line. report() writes OPXY_STREAM_REPORT bytes,
 * little-endian:
 *   +0  version (2)
 *   +1  the way: 0 byte by byte, 1 by the controller fed by the core, 2 fed by DMA
 *   +2  s09_boot_slave()'s outcome (S09_BOOT_*), +3 how it ended (S09_END_*)
 *   +4  the fall-back's cause (S09_PART_*, 0 none), +5 1 when the bridge linked,
 *   +6  1 when the slave held the line for T_STREAM, +7 1 when the stream ran
 *   +8  stream sessions, +12 parts by the controller, +16 bytes it clocked,
 *   +20 bytes sent one at a time after a fall-back, +24 the fall-back's session,
 *   +28 the slot's bytes before it, +32 the stream's time in us (from the wait
 *   for the ready line), +36 the guards' time, +40 the waits for the line,
 *   +44 the longest such wait, +48 parts begun with the line high, +52 the
 *   guard's length, +56 the driver's four fault words of the part that fell back,
 *   +72 SPI0_STAT reads in the parts' waits, +76 those with FCS set, +80 the
 *   parts that saw it, +84 the slot's bytes before the first such part, plus
 *   one (0 none) (struct opxy_boot_fcs). Version 1 ended at +72.
 * The macros sit on the slave start's own lines and are empty in every other build.
 */
/*
 * S-09.019, with BOOT_LINK_FC=1 or 2 or BOOT_L1_CHECK=1 (OPXY_BOOT_L1_CHECK):
 * the slave checks its L1 code, which the
 * front's check of the loaded image does not read (S-01.023), before it
 * serves the bridge or touches the eMMC, against the mark tools/mkldr.py
 * fills (mark_l1()): the address and length of the run the stream loads into
 * L1 code and the CRC-32 of its bytes. opxy_l1_check() takes the mark and a
 * reader of one word; opxy_l1_code_ok() reads L1 code with L1DM_DCTL.ENX set
 * (target only). A mark left as compiled fails.
 */
struct opxy_l1_mark {
	uint32_t magic, addr, len, crc;
};
#define OPXY_L1_MARK_EMPTY 0x4C314D30u	/* as compiled */
#define OPXY_L1_MARK_FULL  0x4C314D31u	/* as mkldr.py fills it */
#if OPXY_BOOT_L1_CHECK && !defined(OPXY_TEST_OS)
extern const volatile struct opxy_l1_mark opxy_l1_mark;
bool opxy_l1_check(const volatile struct opxy_l1_mark *mk, uint32_t (*word)(uint32_t addr));
bool opxy_l1_code_ok(void);
#endif

#define OPXY_STREAM_REPORT 88u
#if OPXY_BOOT_LINK_FC && !defined(OPXY_TEST_OS)
void opxy_stream_begin(void);
void opxy_stream_end(int rc, int end);
void opxy_stream_linked(struct opxy_master *m);
size_t opxy_stream_report(uint8_t *out);
#define OPXY_STREAM_BEGIN() opxy_stream_begin()
#define OPXY_STREAM_END(rc, end) opxy_stream_end((rc), (end))
#define OPXY_STREAM_LINKED(m) opxy_stream_linked(m)
#else
#define OPXY_STREAM_BEGIN() ((void)0)
#define OPXY_STREAM_END(rc, end) ((void)0)
#define OPXY_STREAM_LINKED(m) ((void)0)
#endif

/* The UART software buffers (S-06's links; S-13b.070, .071). */
#define OPXY_UART_BUF 0x800u
/* S-13b.070: the PSoC link's DMA receive ring, 40.96 ms of the line. */
#define OPXY_PSOC_RING 0x1000u

/* S-05.004: the longest wait for the PSoC link's queued bytes to leave; a
 * full ring takes 20.5 ms at 1 Mbaud (S-13b.071). */
#define OPXY_PSOC_DRAIN_US 25000u

/* S-03 transfers: 16 MSI descriptors of 4 KiB move 128 sectors. */
#define OPXY_MSI_DESC 16u
/* S-13b.057: the whole bring-up is made at most this many times. */
#define OPXY_EMMC_TRIES 2u
/* S-09.018: a repeat starts only this soon after the first try began. */
#define OPXY_EMMC_REPEAT_US 1500000u

struct opxy_master {
	struct opxy_hw hw;
	struct s06_clock clock;
	/* The frozen front's hand-off (front/fr_handoff.h): front is true when
	 * the record was found whole, and ho is its copy. */
	bool front;
	struct fr_handoff ho;
	bool log_finished;		/* S-02.095: the finished mark made or tried */
	bool log_tidied;		/* and the log's erase decided */
	bool log_viewed;		/* and the log read for Device Info */
	uint32_t dma9_xmod, dma9_ymod;	/* S-01.006, kept at entry */
	uint16_t strap;			/* S-13b.001 */
	uint16_t boot_reason;		/* S-04.004: the reason this boot took */
	bool debug_key;			/* S-07.032's debug at start (S-10a) */
	/* S-13c.019: 0x51 {3C FF 02} sent after DISPON */
	bool display_lit;
	/* S-01.002: the running slot as the boot established it, kept by
	 * the slave's start for the status bar and the prompt's Running
	 * value (S-15.180). */
	bool running_established;
	uint8_t running_slot;
	/* S-02.095: the running slot is established and holds the trial mark,
	 * and whether the boot menu's mark was made or tried */
	bool running_trial;
	bool trial_marked;
	/* S-02 */
	struct s02_flash_ops flash_ops;
	struct s02_env flash;
	struct s04_meta meta;
	uint8_t meta_raw[S04_META_SIZE];
	bool meta_failed;		/* the record could not be read */
	bool meta_restore_failed;	/* S-02.065 failed: no metadata write */
	/* S-09: the boot link, then the bridge, and the eMMC behind it */
	struct s09_boot_ops boot_ops;
	struct s09_bus_ops bus_ops;
	struct s09_master bridge;
	bool slave_present;
	/* S-09.075: the highest rung opxy_bridge_fast() raises the link to,
	 * OPXY_LINK_CEILING from the bridge's start */
	uint8_t link_ceiling;
	struct s09_clock_msg clock_msg;	/* S-09.046: the last 0xBD sent */
	struct s03_emmc_ops emmc_ops;
	struct s03_env emmc;
	/* S-06 links: the PSoC, the Nordic, the slave (the tunnel); USB's
	 * CDC and MIDI join in held mode (opxy_usb_port). */
	struct s06_node node;
	struct s06_link link[S06_MAX_LINKS];
	struct s07_ops psoc_ops;
	struct s07_psoc psoc;
	uint32_t psoc_rx_logged;	/* the receiver's losses already logged */
	struct s08_ops nordic_ops;
	struct s08_nordic nordic;
	/* S-14, S-15, S-04's reboots, S-10c, S-17 (opxy_services.c) */
	struct opxy_services sv;
	/* S-07.032: the one key-status reply psoc_start() read or was given;
	 * the install's confirmation (S-15.116) asks whether it was read */
	struct s07_key_status key_status;
	/* S-05 */
	struct s05_master_ops handoff;
	struct boot_master_ops ops;
};

/*
 * The front's hand-off record (front/fr_handoff.h) at FR_HANDOFF_ADDR, whole:
 * its magic, version, size and CRC, copied out; the magic in L2 is then
 * cleared, so a later run never takes it again. False on the host, and when
 * there is none (the test OS).
 */
bool opxy_front_take(struct fr_handoff *out);
/*
 * S-02.095: the main image's half of the start log. opxy_log_finish_at()
 * gives the byte and value that clear the finished bit of the entry the
 * record names, false when it names none (no flag, a slot past 1, an entry
 * past the log). opxy_start_finished() programs that byte once per boot,
 * through the master's flash accessor (S-02.040), once the boot is alive:
 * held mode's first pass, the battery wait's loop, the boot prompt shown,
 * or the OS image staged and checked. Nothing without the front's record.
 */
bool opxy_log_finish_at(const struct fr_handoff *ho, uint32_t *off, uint8_t *value);
void opxy_start_finished(struct opxy_master *m);
/*
 * S-02.095: a trial start that reaches the boot menu says so in the tried
 * slot's log. Once, at held mode's first service pass, when the running slot
 * is established and read under the trial mark, the selector names the other
 * slot, and byte 0 of the running slot's last sector reads 0xFC (the front's
 * trial start mark) with nothing else written in that sector: one 1-byte
 * program, 0xFC to OPXY_TRIAL_REACHED_BYTE, entry 1 finished. The installer
 * keeps a trial only with this mark. A failed program is not repeated.
 */
#define OPXY_TRIAL_STARTED_BYTE 0xFCu
#define OPXY_TRIAL_REACHED_BYTE 0xF0u
bool opxy_trial_reached_at(const struct s02_env *env, unsigned running, uint32_t *off);
void opxy_trial_reached(struct opxy_master *m);
/*
 * S-02.101: the main image's erase of the start log, so that the front's
 * count never stops for a full log and a sector takes a bounded number of
 * one-bit programs between erases. Once per boot, after the finished mark,
 * on a normal start of the established running slot when it is the
 * selected slot: the selected slot's log is erased when it holds
 * OPXY_LOG_ERASE_USED or more used entries with this start's, named by
 * the front, the newest and finished; or when it reads full or odd.
 * Everything is read again first, as the front reads it; any doubt or
 * failed read leaves the sector alone. The erase is read back; a failure is
 * logged and stops nothing.
 */
#define OPXY_LOG_ERASE_USED 256u	/* 512 one-bit programs between erases */
/* An entry's two bits, the started bit low (S-02.095). */
#define OPXY_LOG_E_FREE       3u
#define OPXY_LOG_E_UNFINISHED 2u
#define OPXY_LOG_E_INVALID    1u

enum opxy_log { OPXY_LOG_IO, OPXY_LOG_OK, OPXY_LOG_FULL, OPXY_LOG_ODD, OPXY_LOG_NONE };

/* A slot's log as read: the front's states, with the entries used and the
 * unfinished ones just before the first free one (OPXY_LOG_OK only), and
 * whether byte 0 holds a trial's entry (0xFD, 0xFC, 0xF0, 0xF9) with every
 * other byte 0xFF. -1 when a read failed. Reads the record and the sector,
 * through env's scratch. */
struct opxy_log_scan {
	uint8_t state;		/* enum opxy_log */
	uint8_t byte0;
	bool trial_alone;
	uint16_t used;
	uint16_t unfinished;
};
int opxy_log_scan(const struct s02_env *env, unsigned slot, struct opxy_log_scan *out);

/* What the guard knows of this start. */
struct opxy_log_start {
	bool front;		/* the hand-off record was taken */
	bool logged;		/* it names this start's entry */
	uint8_t log_slot;
	uint16_t log_entry;
	bool established;	/* the running slot (S-01.002) */
	uint8_t running;
	bool trial;		/* its stream was read under the trial mark */
};
enum opxy_log_erase { OPXY_ERASE_NO, OPXY_ERASE_USED, OPXY_ERASE_FULL, OPXY_ERASE_ODD };
/* The guard: why the sector *off, the selected slot's last sector, is to be
 * erased now, or OPXY_ERASE_NO. */
enum opxy_log_erase opxy_log_erase_at(const struct s02_env *env, const struct opxy_log_start *st,
				      uint32_t *off);
void opxy_start_log_tidy(struct opxy_master *m);
/* The selected slot's log, for Device Info: once, in held mode. */
void opxy_start_log_view(struct opxy_master *m);
/* The same again, each time Device Info's second page opens (S-15.082). */
void opxy_start_log_reread(struct opxy_master *m);
/* The INIT stage's stamps, from the hand-off (BOOT_TIMING=1 reports them). */
extern uint64_t opxy_init_cycles[3];

struct opxy_emmc_more;

struct opxy_slave {
	struct opxy_hw hw;
	struct s06_clock clock;
	_Alignas(32) uint8_t ext_csd[512];	/* the eMMC driver's DMA (S-13b.054) */
	/* S-13b.059: false after a failed bring-up; then emmc_ops fail at
	 * once, and nothing of ext_csd is used. */
	bool emmc_up;
	/* S-03.002: up, but not the eMMC of S-03.001: its user area is never
	 * read or written, nor a boot partition written. */
	bool emmc_other;
	bool features;		/* S-03.009's switches made */
	/* S-03.011: the power-off notification given; no command until a
	 * request needs the eMMC, which first brings it up again. */
	bool emmc_notified;
	/* The partition the eMMC has selected (S-03.007): 0, 1 or 2, or
	 * OPXY_PART_UNKNOWN after a select that failed. */
	unsigned part;
	struct s03_emmc_ops emmc_ops;
	/* The erase with an argument and the reliable write, on emmc_ops'
	 * ctx (waffle/board/opxy_bridge.h); NULL while the eMMC is down. */
	const struct opxy_emmc_more *emmc_more;
	struct s03_env emmc;
	struct s09_store_ops store;
	struct s09_slave server;
	uint8_t last_cmd;		/* the last request header's command */
	enum s05_fail staged;		/* S-05.020 step 1's outcome */
	uint32_t staged_len;
	struct s06_node node;
	struct s06_link cpu_link;
	struct s09_cpu_ops cpu_ops;
	struct s05_slave_ops handoff;
	struct boot_slave_ops ops;
};

#define OPXY_PART_UNKNOWN 0xFFu

extern struct opxy_master opxy_master;
extern struct opxy_slave opxy_slave;

/* S-01.003: the entry after start-up, both roles. */
void opxy_main(void);
void opxy_master_main(struct opxy_master *m);
/* S-05's master ops (m->handoff), as opxy_master_main() sets them; public
 * for the host's tests. */
void opxy_master_handoff(struct opxy_master *m);
/* S-07.054's usb_msc hook: USB for the battery wait's loop, then the start
 * log's mark (S-02.095). Public for the host's tests. */
void opxy_battery_usb(void *ctx, bool on);
void opxy_slave_main(struct opxy_slave *s);
/*
 * S-13b.050-.059, S-03.008, S-03.009, then S-12's volume and the S-09
 * server's store: the slave's storage, after the DDR check. Public for
 * the host's tests.
 */
void opxy_slave_storage(struct opxy_slave *s);
/*
 * The user area as yaffs2's block device (S-12), on s's eMMC ops while
 * the user area is selected. Its chunks go as reliable writes through
 * s->emmc_more (S-12.060); its erase is the eMMC's (S-12.042), or none in
 * a YAFFS_ZERO_ERASE=1 build. Public for the host's tests.
 */
void opxy_slave_user_bd(struct opxy_slave *s, struct wb_blockdev *bd);

/* Held mode on the master (src/board/opxy_held.c). */
void opxy_held(void *ctx, const struct boot_held *h);
/* Its service loop (boot_held_ops.service), which does not return; public
 * for the host's tests. */
void opxy_held_service(void *ctx, const struct boot_held *h);
/* Its PSoC link: the transmitter on UART0's status interrupt (DMA11's ring
 * receives, S-13b.070), and what a long operation left read before the
 * menu's keys act again (s15_menu_held()). */
void opxy_held_psoc(struct opxy_master *m);
int opxy_held_settle(struct opxy_master *m);
/* S-11.080 (src/board/opxy_install.c): this boot found the state 0xAB0D.
 * Sets it to 0 and runs nothing; before the Nordic link's first use. */
void opxy_install_interrupted(struct opxy_master *m);

/*
 * S-05.025, S-15.180 (src/board/opxy_prompt.c, a build with the prompt):
 * the boot master's boot_prompt op. The prompt's frame, lit by the early
 * logo's 0x51 {3C FF 02}; while it waits the PSoC's messages are handled
 * (S-07.076), nothing is written and no bridge request is made. Returns BOOT_PROMPT_START or BOOT_PROMPT_HELD;
 * a power-off the PSoC reports does not return (S-07.051).
 */
int opxy_boot_prompt(void *ctx);

/* S-05.004: send what is queued for the PSoC before the load; the UART
 * moves bytes only when serviced, and the teardown disables it. A loss
 * the receiver counted is logged first (opxy_psoc_rx_report()). */
int opxy_psoc_drain(struct opxy_master *m);
/* S-13b.070: UART0 for the PSoC link, receiving through DMA11's ring. */
int opxy_psoc_uart_open(struct opxy_master *m);
/* S-13b.070, S-14: one line when the PSoC link's receiver counted an
 * overrun, a lost or possibly lost ring, or a DMA error since the last. */
void opxy_psoc_rx_report(struct opxy_master *m);

/* S-05 on the target (src/board/opxy_handover.c). */
void opxy_teardown_master(void *ctx);
void opxy_teardown_slave(void *ctx);
/* S-05.020 step 3: the slave's ready line PA4 at the end of its server, a
 * GPIO driven low until its OS takes it (OPXY_HO_PA4_SPI2: to SPI2). */
void opxy_ho_leave_pa4(void);
/*
 * S-05.032 on the master, after a release reply of 0: PA6 low (the slave's
 * server stopped), then high (its OS started), within T_OSSTART. The high is
 * read from PINT0's latch, so a pulse of any width counts. 0 when it came
 * (at once with OPXY_HO_NO_OS_WAIT or OPXY_HO_STAY), -1 when the bound ran
 * out.
 */
int opxy_os_wait(struct opxy_master *m);
#define OPXY_T_OSSTART_US 20000000u	/* S-09.060: 10 s - 30 s */
/*
 * S-05.011, S-05.014: the CPLBs and caches the OS starts with, filled by the
 * teardown and installed from the reserve after the kernel returns
 * (opxy_handover_entry.S): each side's control value, default descriptor and
 * sixteen descriptor pairs.
 */
struct opxy_ho_cache {
	uint32_t on;			/* nonzero to install */
	uint32_t ictl, idflt;
	uint32_t icplb[16][2];		/* data, address */
	uint32_t dctl, ddflt;
	uint32_t dcplb[16][2];
};
extern struct opxy_ho_cache opxy_ho_cache;
/* The table opxy_ho_cache is filled from (S-05.031's pages), for the tests. */
extern const struct wb_cplb opxy_ho_icplb[];
extern const struct wb_cplb opxy_ho_dcplb[];
extern const unsigned opxy_ho_n_icplb, opxy_ho_n_dcplb;
/* Install opxy_ho_cache, then wait for the DDR controller to be idle
 * (S-05.016); from the reserve, after the kernel returns. */
void opxy_ho_finish(void);
/* S-05.014: SP, FP and the vectors the OS gets, set by the teardown. */
extern struct opxy_os_entry opxy_handover_os;
void opxy_kernel_call(void *ctx, uint32_t config_addr);
uint32_t opxy_read_svect0(void *ctx);
void opxy_jump(void *ctx, uint32_t entry);
/* The configuration block in the handover reserve, built (S-05.008). */
uint32_t opxy_handover_config(void);
/* S-05.009: the load function the block names (cfg: the block). */
void opxy_load_fn(uint32_t cfg);
/* S-01.014: the staging area. */
#define OPXY_STAGING 0x8A000000u

/*
 * Hooks into the drivers. Each has a weak default (opxy_main.c) that does
 * nothing; the driver, when linked, overrides it.
 */
/* S-05.004, S-13c: the hand-over frame and the OS's frame memory. True
 * when the panel's last frame was the hand-over frame already, so none was
 * sent. */
bool opxy_display_boot_logo(void);
/* S-05.004: the early logo frame, with the run's one
 * initialization and the gamma load, before the slave's stream; false
 * without a display. */
bool opxy_display_early_logo(void);
void opxy_display_dma_take(void);
/* S-13c.021 via S-01.019: the panel reset handshake taken at entry. */
void opxy_display_handshake(uint32_t xmod, uint32_t ymod);
/* S-01.023: the panel as the front left it, on, at its reset time. */
void opxy_display_adopt(uint32_t reset_ms);
/* S-05.016, S-10a.030: USB detached, stopped and its PHY off
 * (src/board/opxy_usb.c). */
void opxy_usb_stop(void);

/* S-13a.061: the hard reset of S-04.028, either role; never returns. */
#define OPXY_WDOG_COUNT 20000000u	/* informative: 200 ms */
void opxy_hard_reset(void);

/* S-16.104 fatal halt: the record, then nothing more. */
void opxy_fatal(enum s04_fatal_cause cause);

#endif /* WAFFLE_BOARD_OPXY_BOOT_H */
