/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a identity and descriptor sets. Checks S-10a.001 to S-10a.005 and
 * S-10a.012 to S-10a.021 byte for byte against tests/oracle/S-10a-usb-msc's
 * descriptors.json and serial.json: every set, both speeds, with and
 * without a serial, every configuration and its other-speed form, and every
 * string index; on the OP-1F against descriptors_1f.json (S-18.030's
 * idProduct and product strings).
 */
#include <stdint.h>
#include <string.h>

#include "waffle/boot/device.h"
#include "waffle/usb/desc.h"

#include "support/check.h"
#include "vectors/test_s10a_vectors.h"

#define B(r) (s10av_blob + (r).off)

static int same(const uint8_t *got, size_t n, struct s10av_ref want)
{
	return want.present && n == want.len && memcmp(got, B(want), n) == 0;
}

/* S-10a.004 */
static void test_serial(void)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(s10av_serials); i++) {
		const struct s10av_serial *v = &s10av_serials[i];
		char out[S10A_SERIAL_LEN + 1];

		s10a_serial_from_meta(B(v->meta), v->meta.len, out);
		CHECK(strcmp(out, v->usb ? v->usb : "") == 0);
	}
}

static void check_sets(const struct wb_device *d, const struct s10av_set *sets,
		       size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		const struct s10av_set *v = &sets[i];
		const char *serial = v->serial ? "XY0A12BC" : "";
		const unsigned before = check_failures;
		uint8_t buf[S10A_CONFIG_MAX], dev[18], qual[10];
		unsigned c;

		s10a_desc_device(d, v->set, v->serial, dev);
		CHECK(same(dev, sizeof(dev), v->device));
		s10a_desc_qualifier(v->set, qual);
		CHECK(same(qual, sizeof(qual), v->qualifier));
		CHECK(s10a_desc_num_configs(v->set) == (unsigned)v->n_configs);
		for (c = 0; c < (unsigned)v->n_configs; c++) {
			size_t n = s10a_desc_config(v->set, v->serial, c,
						    v->high, false, buf);

			CHECK(same(buf, n, v->configs[c]));
			n = s10a_desc_config(v->set, v->serial, c, v->high,
					     true, buf);
			CHECK(same(buf, n, v->other[c]));
		}
		/* S-10a.031: an index of bNumConfigurations or more: STALL */
		CHECK(s10a_desc_config(v->set, v->serial, c, v->high, false,
				       buf) == 0);
		for (c = 0; c < ARRAY_LEN(v->strings); c++) {
			uint8_t s[S10A_STRING_MAX];
			const size_t n = s10a_desc_string(d, v->set, serial, c, s);
			/* The same text, for S-10b's DeviceInfo. */
			char t[S10A_TEXT_MAX];
			const bool has = s10a_desc_text(d, v->set, serial, c, t);

			CHECK(c == 0 ? !has : has == (n != 0));
			if (c && has && n)
				CHECK(n == 2u + 2u * strlen(t) && s[2] == t[0]);
			if (v->strings[c].present)
				CHECK(same(s, n, v->strings[c]));
			else
				CHECK(n == 0);	/* S-10a.005: STALL */
		}
		if (check_failures != before)
			printf("  %s set %s\n", d->name, v->name);
	}
}

static void test_sets(void)
{
	check_sets(&wb_device_opxy, s10av_sets, ARRAY_LEN(s10av_sets));
	check_sets(&wb_device_op1f, s10av_sets_1f, ARRAY_LEN(s10av_sets_1f));
}

/* The facts the chapter 9 answers are drawn from (S-10a.013, S-10a.020). */
static void test_layout(void)
{
	enum s10a_function fn;
	unsigned first;

	CHECK(s10a_desc_num_interfaces(S10A_SET_MIDI_CDC) == 4);
	CHECK(s10a_desc_num_interfaces(S10A_SET_DISK_CDC) == 3);
	CHECK(s10a_desc_num_interfaces(S10A_SET_MTP) == 1);
	CHECK(s10a_desc_interface(S10A_SET_MIDI_CDC, 3, &fn, &first));
	CHECK(fn == S10A_FN_CDC && first == 2);
	CHECK(s10a_desc_interface(S10A_SET_DISK_CDC, 2, &fn, &first));
	CHECK(fn == S10A_FN_MSC && first == 2);
	CHECK(!s10a_desc_interface(S10A_SET_DISK, 1, &fn, &first));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MIDI, 0x87));
	CHECK(!s10a_desc_has_endpoint(S10A_SET_MIDI, 0x81));
	CHECK(!s10a_desc_has_endpoint(S10A_SET_MIDI, 0x07));
	CHECK(s10a_desc_has_endpoint(S10A_SET_SERIAL, 0x0A));
	CHECK(!s10a_desc_has_endpoint(S10A_SET_SERIAL, 0x08));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_VENDOR, 0x83));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP, 0x89));
	CHECK(!s10a_desc_has_endpoint(S10A_SET_MTP, 0x87));
	/* MTP beside the CDC port: CDC at 0-1 on its own endpoints, MTP at 2
	 * on the mass-storage pair and its event endpoint. */
	CHECK(s10a_desc_num_interfaces(S10A_SET_MTP_CDC) == 3);
	CHECK(s10a_desc_interface(S10A_SET_MTP_CDC, 1, &fn, &first));
	CHECK(fn == S10A_FN_CDC && first == 0);
	CHECK(s10a_desc_interface(S10A_SET_MTP_VENDOR_CDC, 2, &fn, &first));
	CHECK(fn == S10A_FN_MTP_VENDOR && first == 2);
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_CDC, 0x81));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_CDC, 0x89));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_CDC, 0x0A));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_CDC, 0x87));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_CDC, 0x08));
	CHECK(s10a_desc_has_endpoint(S10A_SET_MTP_VENDOR_CDC, 0x83));
	CHECK(!s10a_desc_has_endpoint(S10A_SET_MTP_CDC, 0x07));
	CHECK(s10a_desc_max_power(S10A_SET_MTP_CDC, 1) == 250);
	CHECK(s10a_desc_max_power(S10A_SET_MIDI, 3) == 1);
	CHECK(s10a_desc_max_power(S10A_SET_DISK, 2) == 50);
	CHECK(s10a_desc_max_power(S10A_SET_DISK, 3) == 0);
	CHECK(s10a_desc_max_power(S10A_SET_SERIAL, 0) == 0);
}

/* S-10a.010's table row by row: the serial flag clear, set, and developer
 * mode on (which sets the flag). */
static void test_set_for_state(void)
{
	static const struct {
		enum s10a_state state;
		enum s10a_set clear, flag, dev;
	} rows[] = {
		{ S10A_STATE_MAIN, S10A_SET_MIDI, S10A_SET_MIDI_CDC,
		  S10A_SET_MIDI_CDC },
		{ S10A_STATE_UPGRADE_40, S10A_SET_MIDI, S10A_SET_MIDI_CDC,
		  S10A_SET_MIDI_CDC },
		{ S10A_STATE_UPGRADE, S10A_SET_DISK, S10A_SET_DISK_CDC,
		  S10A_SET_DISK_CDC },
		{ S10A_STATE_BATTERY, S10A_SET_DISK, S10A_SET_DISK,
		  S10A_SET_DISK },
		{ S10A_STATE_FACTORY_MTP_VENDOR, S10A_SET_MTP_VENDOR,
		  S10A_SET_MTP_VENDOR, S10A_SET_MTP_VENDOR_CDC },
		{ S10A_STATE_FACTORY_MTP, S10A_SET_MTP, S10A_SET_MTP,
		  S10A_SET_MTP_CDC },
		{ S10A_STATE_FACTORY_MTP_OFF, S10A_SET_SERIAL, S10A_SET_SERIAL,
		  S10A_SET_SERIAL },
	};
	enum s10a_set set;
	size_t i;

	for (i = 0; i < ARRAY_LEN(rows); i++) {
		set = S10A_SET_COUNT;
		CHECK(s10a_set_for_state(rows[i].state, false, false, &set) &&
		      set == rows[i].clear);
		set = S10A_SET_COUNT;
		CHECK(s10a_set_for_state(rows[i].state, true, false, &set) &&
		      set == rows[i].flag);
		set = S10A_SET_COUNT;
		CHECK(s10a_set_for_state(rows[i].state, true, true, &set) &&
		      set == rows[i].dev);
		set = S10A_SET_COUNT;
		CHECK(s10a_set_for_state(rows[i].state, false, true, &set) &&
		      set == rows[i].dev);
	}
	/* A resumed or forced upgrade starts none: the set presented stays. */
	set = S10A_SET_COUNT;
	CHECK(!s10a_set_for_state(S10A_STATE_UPGRADE_KEEP, false, false, &set));
	CHECK(!s10a_set_for_state(S10A_STATE_UPGRADE_KEEP, true, true, &set));
	CHECK(set == S10A_SET_COUNT);
}

int main(void)
{
	RUN(test_serial);
	RUN(test_sets);
	RUN(test_layout);
	RUN(test_set_for_state);
	return check_summary("test_s10a_desc");
}
