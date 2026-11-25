/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07.087: the host link's raw bridge on the PSoC link. Entry with and
 * without 0xEE; nothing of the master's (periodic messages, the reset
 * request, a log line) transmitted while the link is raw; the bytes both
 * ways unchanged; the leave: framed again, no exit packet, the greeting,
 * held mode reported and the keys subscribed again, or the wait for the
 * PSoC's own greeting; a power-off pending at the entry.
 */
#include "s07_test.h"

#define ID_GREET 0x05u
#define ID_GREET_OK 0x06u
#define ID_REPORT 0x51u
#define ID_SUBSCRIBE 0x43u
#define ID_BOOTLOADER 0xEEu

static bool answer_greet;

/* The PSoC application, when it answers: 0x06 {40} to each 0x05. */
static void psoc_answers(const struct t_rig *r, const struct t_item *it)
{
	static const uint8_t ok[1] = { 0x40 };

	(void)r;
	if (answer_greet && !it->raw && it->id == ID_GREET)
		t_rig_from_psoc(it->t + 1000u, s06_swap_route(it->route), ID_GREET_OK, ok, 1);
}

static void setup(void)
{
	t_rig_init(S06_MS(60000));
	rig.on_item = psoc_answers;
	answer_greet = false;
	s07_subscribe(&rig.psoc);		/* the link started, as in held mode */
	rig.nitems = 0;
}

static size_t count_id(size_t from, uint8_t id)
{
	size_t n = 0;

	for (size_t i = from; i < rig.nitems; i++)
		n += !rig.item[i].raw && rig.item[i].id == id;
	return n;
}

static size_t count_raw(size_t from)
{
	size_t n = 0;

	for (size_t i = from; i < rig.nitems; i++)
		n += rig.item[i].raw != 0;
	return n;
}

static void test_enter(void)
{
	static const uint8_t lim[2] = { 0x00, 0x64 };
	static const uint8_t text[3] = { 'h', 'i', '!' };
	size_t at;

	t_case = "enter with the entry message";
	setup();
	CHECK_EQ(s07_bridge_enter(&rig.psoc, true), S07_OK);
	CHECK_EQ(rig.nitems, 1u);
	CHECK_EQ(rig.item[0].id, ID_BOOTLOADER);
	CHECK_EQ(rig.item[0].len, 0u);
	CHECK(rig.link.raw);
	/* nothing of the master's on the raw link (S-16.147) */
	at = rig.nitems;
	CHECK(!s06_send(&rig.node, S06_LINK_PSOC, 0, 0x34, lim, 2));
	s07_reset_request(&rig.psoc);
	s07_led_bar(&rig.psoc, 5);
	(void)s07_service_pass(&rig.psoc);
	CHECK(!s06_send_log(&rig.node, S06_LINK_PSOC, 0x0E, text, sizeof text));
	CHECK_EQ(rig.nitems, at);

	t_case = "enter without the entry message";
	setup();
	CHECK_EQ(s07_bridge_enter(&rig.psoc, false), S07_OK);
	CHECK_EQ(rig.nitems, 0u);
	CHECK(rig.link.raw);

	t_case = "a power-off pending at the entry";
	setup();
	t_rig_from_psoc(rig.sim.now, 0x41, 0x31, (const uint8_t[]){ 0x10 }, 1);
	t_rig_from_psoc(rig.sim.now, 0x41, 0x31, (const uint8_t[]){ 0x14 }, 1);
	CHECK_EQ(s07_bridge_enter(&rig.psoc, true), S07_POWEROFF);
	CHECK(!rig.link.raw);
	CHECK_EQ(count_id(0, ID_BOOTLOADER), 0u);
}

static void test_bytes(void)
{
	/* a bootloader packet, and bytes that would look like frames */
	static const uint8_t to_psoc[7] = { 0x01, 0x3B, 0x00, 0x00, 0xC4, 0xFF, 0x17 };
	static const uint8_t from_psoc[9] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x17, 0x05 };
	uint8_t got[32];
	size_t n = 0, k;

	t_case = "the bytes both ways, unchanged";
	setup();
	(void)s07_bridge_enter(&rig.psoc, false);
	CHECK(s07_bridge_write(&rig.psoc, to_psoc, sizeof to_psoc));
	CHECK_EQ(rig.nitems, 1u);
	CHECK(rig.item[0].raw);
	CHECK_MEM(rig.item[0].d, rig.item[0].len, to_psoc, sizeof to_psoc);
	CHECK(s07_bridge_write(&rig.psoc, NULL, 0));		/* nothing: nothing written */
	CHECK_EQ(rig.nitems, 1u);
	t_port_inject(&rig.port, rig.sim.now, from_psoc, sizeof from_psoc);
	(void)s07_service_pass(&rig.psoc);			/* not decoded */
	while ((k = s07_bridge_read(&rig.psoc, got + n, 4)) != 0)
		n += k;
	CHECK_MEM(got, n, from_psoc, sizeof from_psoc);
	CHECK_EQ(rig.nkev, 0u);

	t_case = "a write with no room: nothing written";
	setup();
	(void)s07_bridge_enter(&rig.psoc, false);
	rig.port.room = 3;
	CHECK(!s07_bridge_write(&rig.psoc, to_psoc, sizeof to_psoc));
	CHECK_EQ(rig.nitems, 0u);
}

static void test_leave(void)
{
	bool greeted = true;
	uint32_t t0;

	t_case = "leave: greeted, reported, subscribed, no exit packet";
	setup();
	(void)s07_bridge_enter(&rig.psoc, true);
	t_port_inject(&rig.port, rig.sim.now, (const uint8_t[]){ 0x01, 0x02 }, 2);
	answer_greet = true;
	rig.nitems = 0;
	CHECK_EQ(s07_bridge_leave(&rig.psoc, &greeted), S07_OK);
	CHECK(greeted);
	CHECK(!rig.link.raw);
	CHECK_EQ(count_raw(0), 0u);
	CHECK(count_id(0, ID_GREET) >= 1u);
	CHECK_EQ(count_id(0, ID_REPORT), 1u);
	CHECK_EQ(count_id(0, ID_SUBSCRIBE), 1u);
	for (size_t i = 0; i < rig.nitems; i++)
		if (!rig.item[i].raw && rig.item[i].id == ID_REPORT)
			CHECK_MEM(rig.item[i].d, rig.item[i].len,
				  ((const uint8_t[]){ 0x3C, 0xFF, 0x02 }), 3u);

	t_case = "leave: a silent PSoC, at least 2 s, no exit packet";
	setup();
	(void)s07_bridge_enter(&rig.psoc, true);
	rig.nitems = 0;
	t0 = rig.sim.now;
	CHECK_EQ(s07_bridge_leave(&rig.psoc, &greeted), S07_OK);
	CHECK(!greeted);
	CHECK(rig.sim.now - t0 >= S06_MS(2000));
	CHECK_EQ(count_raw(0), 0u);
	CHECK_EQ(count_id(0, ID_SUBSCRIBE), 0u);

	t_case = "leave: the PSoC's own greeting ends the wait";
	setup();
	(void)s07_bridge_enter(&rig.psoc, false);
	rig.nitems = 0;
	t_rig_from_psoc(rig.sim.now + S06_MS(600), 0x41, ID_GREET,
			(const uint8_t[]){ 0x01, 0x04 }, 2);
	t0 = rig.sim.now;
	CHECK_EQ(s07_bridge_leave(&rig.psoc, &greeted), S07_OK);
	CHECK(greeted);
	CHECK(rig.sim.now - t0 < S06_MS(2000));
	CHECK_EQ(count_raw(0), 0u);
	CHECK(count_id(0, ID_GREET_OK) >= 1u);
	CHECK(count_id(0, ID_SUBSCRIBE) >= 1u);
}

int main(void)
{
	test_enter();
	test_bytes();
	test_leave();
	return t_finish("test_s07_bridge");
}
