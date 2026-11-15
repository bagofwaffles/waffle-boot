/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a on TinyUSB's device core (waffle/s10a_usb.h).
 *
 * Every weak TinyUSB callback this firmware overrides is defined in this
 * file, so that linking anything of it links all of them.
 *
 * Where TinyUSB's own answers differ from S-10a (chapter 9, S-10a.031 to
 * S-10a.034) s10a_usb_setup_filter() answers first, in the device
 * controller driver; what reaches usbd is what usbd handles as S-10a wants.
 * Bus resets and configuration changes reach the functions through the
 * class driver's reset() (usbd calls it for both) and the mount callbacks.
 */
#include "waffle/s10a_usb.h"

#include <string.h>

#include "tusb.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"

#include "waffle/s10a_cdc.h"
#include "waffle/s10a_ch9.h"
#include "waffle/s10a_msc.h"
#include "waffle/s10b_mtp.h"
#include "waffle/s10c_host.h"
#include "waffle/s10c_sysex.h"

#define RHPORT		0u
#define PKT		512u	/* the largest bulk packet */
#define UNIT_LOAD_MA	100u

/* A byte ring; size a power of two. */
struct ring {
	uint8_t *buf;
	uint32_t size, head, tail;
};

/* The bulk pair of the CDC data interface or of MIDIStreaming. */
struct stream {
	uint8_t in, out;	/* endpoint addresses, 0 when not open */
	uint16_t mps;		/* of `in`, at the current speed */
	uint16_t out_mps;	/* of `out`: one OUT arm */
	bool zlp;		/* the last packet sent was full */
	struct ring rx, tx;
	uint8_t rx_pkt[PKT], tx_pkt[PKT];
};

static struct {
	const struct s10a_usb_board *board;
	struct s10c_host *host;
	enum s10a_set set;
	char serial[S10A_SERIAL_LEN + 1];
	struct s10a_ch9 ch9;
	struct s10a_lu lu;
	struct s10a_bot bot;
	struct s10a_cdc cdc;
	struct s10c_midi_rx midi_rx;
	struct stream cdc_s, midi_s;
	uint8_t msc_in, msc_out, notify;
	bool polling;
	/* S-10b: the responder, its endpoints, an OUT transfer it has not
	 * taken yet. */
	struct s10b *mtp;
	uint8_t mtp_in, mtp_out, mtp_event;
	bool mtp_rx_done;
	uint16_t mtp_rx_n;
	/* The responder is inside a request (mtp_run()); what the bus did to
	 * it meanwhile waits until the request returns: a cancel, a reset
	 * and the packet size the reset found. */
	bool mtp_in_request;
	bool mtp_owed_cancel, mtp_owed_reset;
	uint16_t mtp_owed_mps;
	uint8_t mtp_ctl[8];
	uint8_t mtp_rx_pkt[PKT], mtp_tx_pkt[PKT];
	/* Where in the CDC byte stream each SET_CONTROL_LINE_STATE arrived
	 * (S-10c.032), oldest first: ring positions of cdc_s.rx. */
	uint32_t marks[8];
	unsigned n_marks;
	uint8_t ctl[1];
	uint8_t coding[S10A_CDC_CODING_LEN];
	uint8_t note[S10A_CDC_NOTIFY_LEN];
} u;

static uint8_t cdc_rx_buf[2048], cdc_tx_buf[2048];
static uint8_t midi_rx_buf[2048], midi_tx_buf[1024];

/* --- Rings -------------------------------------------------------------- */

static uint32_t ring_used(const struct ring *r)
{
	return r->head - r->tail;
}

static uint32_t ring_free(const struct ring *r)
{
	return r->size - ring_used(r);
}

static void ring_put(struct ring *r, const uint8_t *p, uint32_t n)
{
	while (n--)
		r->buf[r->head++ & (r->size - 1u)] = *p++;
}

static uint32_t ring_get(struct ring *r, uint8_t *p, uint32_t n)
{
	uint32_t i;

	if (n > ring_used(r))
		n = ring_used(r);
	for (i = 0; i < n; i++)
		p[i] = r->buf[r->tail++ & (r->size - 1u)];
	return n;
}

static void ring_init(struct ring *r, uint8_t *buf, uint32_t size)
{
	r->buf = buf;
	r->size = size;
	r->head = r->tail = 0;
}

/* --- Streams ------------------------------------------------------------ */

static bool ep_free(uint8_t ep)
{
	return ep && !usbd_edpt_busy(RHPORT, ep);
}

/* One packet: the controller ends an OUT transfer only on a short packet
 * or a full count, and a host transfer need not end on a short one. */
static void stream_arm_rx(struct stream *s)
{
	if (ep_free(s->out) && s->out_mps && ring_free(&s->rx) >= s->out_mps)
		(void)usbd_edpt_xfer(RHPORT, s->out, s->rx_pkt, s->out_mps,
				     false);
}

static void stream_kick_tx(struct stream *s)
{
	uint32_t n;

	if (!ep_free(s->in))
		return;
	n = ring_get(&s->tx, s->tx_pkt, PKT);
	if (n == 0 && !s->zlp)
		return;
	/* A transfer that ends on a full packet is ended by a short one. */
	s->zlp = n != 0 && n % s->mps == 0;
	(void)usbd_edpt_xfer(RHPORT, s->in, s->tx_pkt, (uint16_t)n, false);
}

static void stream_rx_done(struct stream *s, uint32_t n)
{
	ring_put(&s->rx, s->rx_pkt, n < PKT ? n : PKT);
	stream_arm_rx(s);
}

/* The endpoints were reset (SET_INTERFACE, a cleared halt): the transfers
 * in progress are gone. */
static void stream_restart(struct stream *s)
{
	s->zlp = false;
	stream_arm_rx(s);
	stream_kick_tx(s);
}

static void stream_close(struct stream *s)
{
	s->in = s->out = 0;
	s->out_mps = 0;
	s->zlp = false;
	s->rx.head = s->rx.tail = 0;
	s->tx.head = s->tx.tail = 0;
}

/* S-10a.062: one SERIAL_STATE per SET_CONTROL_LINE_STATE that sets a line. */
static void notify_kick(void)
{
	if (!u.cdc.notify_owed || !ep_free(u.notify))
		return;
	s10a_cdc_serial_state(&u.cdc, u.note);
	if (usbd_edpt_xfer(RHPORT, u.notify, u.note, sizeof(u.note), false))
		u.cdc.notify_owed--;
}

/* --- Mass storage ------------------------------------------------------- */

static void sync_reset_owed(void)
{
	u.ch9.reset_owed = s10a_bot_reset_owed(&u.bot);
}

static bool bot_xfer_in(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (!ep_free(u.msc_in) || n > UINT16_MAX)
		return false;
	return usbd_edpt_xfer(RHPORT, u.msc_in, (uint8_t *)(uintptr_t)buf,
			      (uint16_t)n, false);
}

static bool bot_xfer_out(void *ctx, uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (!ep_free(u.msc_out) || n > UINT16_MAX)
		return false;
	return usbd_edpt_xfer(RHPORT, u.msc_out, buf, (uint16_t)n, false);
}

static void bot_stall_in(void *ctx)
{
	(void)ctx;
	if (u.msc_in)
		usbd_edpt_stall(RHPORT, u.msc_in);
}

static void bot_stall_out(void *ctx)
{
	(void)ctx;
	if (u.msc_out)
		usbd_edpt_stall(RHPORT, u.msc_out);
}

static const struct s10a_bot_port bot_port = {
	.xfer_in = bot_xfer_in,
	.xfer_out = bot_xfer_out,
	.stall_in = bot_stall_in,
	.stall_out = bot_stall_out,
};

static void disk_released(void *ctx)
{
	(void)ctx;
	if (u.board && u.board->disk_released)
		u.board->disk_released(u.board->ctx);
}

/* --- MTP (S-10b) --------------------------------------------------------- */

/* Not while a request runs: the responder's state is its own until then,
 * and the host's next packet waits (NAK). */
static void mtp_arm_rx(void)
{
	size_t n;

	if (!u.mtp || u.mtp_in_request || u.mtp_rx_done || !ep_free(u.mtp_out))
		return;
	n = s10b_rx_size(u.mtp);
	if (n)
		(void)usbd_edpt_xfer(RHPORT, u.mtp_out, u.mtp_rx_pkt,
				     (uint16_t)(n < PKT ? n : PKT), false);
}

/*
 * The transaction ends without a response and the session stays
 * (S-10b.005). SET_INTERFACE(0) and a cleared halt of a bulk endpoint,
 * whose transfers are gone, end it the same way (S-10b.007). A packet
 * already handed to the controller still goes out: one, as transfers are
 * one packet each (S-10b.005: at most one; S-10b Q-06 is
 * still open).
 *
 * The bus is serviced from inside a request (the board's waits on the
 * bridge), so these events can come while the responder is in the middle
 * of one. They are then answered on the bus at once and recorded; the
 * responder is told when the request returns, before anything more of the
 * transaction is sent (mtp_deliver()).
 */
static void mtp_abort(void)
{
	if (!u.mtp)
		return;
	/* A packet received before it belongs to the transaction ended. No
	 * other can come while the request runs: the endpoint is not armed
	 * again until it returns. */
	u.mtp_rx_done = false;
	if (u.mtp_in_request) {
		u.mtp_owed_cancel = true;
		return;
	}
	s10b_cancel(u.mtp);
	mtp_arm_rx();
}

/* S-10b.007: the session closed, the packet size the configuration has. */
static void mtp_reset(uint16_t mps)
{
	if (!u.mtp)
		return;
	if (u.mtp_in_request) {
		/* a reset ends what a cancel recorded before it would */
		u.mtp_owed_reset = true;
		u.mtp_owed_cancel = false;
		u.mtp_owed_mps = mps;
		return;
	}
	s10b_reset(u.mtp, mps);
}

/* What came while the request ran, in the order it came. */
static void mtp_deliver(void)
{
	if (u.mtp_owed_reset) {
		u.mtp_owed_reset = false;
		s10b_reset(u.mtp, u.mtp_owed_mps);
	}
	if (u.mtp_owed_cancel) {
		u.mtp_owed_cancel = false;
		s10b_cancel(u.mtp);
	}
}

/* From s10a_usb_poll(): the engine reads the volume as it answers. */
static void mtp_run(void)
{
	size_t n = 0;
	bool send;

	if (!u.mtp || !u.mtp_in || !u.mtp_out)
		return;
	u.mtp_in_request = true;
	if (u.mtp_rx_done) {
		u.mtp_rx_done = false;
		s10b_rx(u.mtp, u.mtp_rx_pkt, u.mtp_rx_n);
	}
	mtp_deliver();
	send = ep_free(u.mtp_in) &&
	       s10b_tx(u.mtp, u.mtp_tx_pkt, u.mtp->mps, &n);
	u.mtp_in_request = false;
	/* a packet produced before a cancel or a reset is not sent */
	if (u.mtp_owed_cancel || u.mtp_owed_reset)
		send = false;
	mtp_deliver();
	if (send)
		(void)usbd_edpt_xfer(RHPORT, u.mtp_in, u.mtp_tx_pkt,
				     (uint16_t)n, false);
	mtp_arm_rx();
}

static void mtp_close(void)
{
	u.mtp_in = u.mtp_out = u.mtp_event = 0;
	u.mtp_rx_done = false;
	mtp_reset(0);
}

/*
 * The Still Image class requests (S-10b.005, S-10b.009). A Cancel Request
 * other than 6 bytes or with a code other than 0x4001, any request in the
 * other direction and any other request get a STALL and change nothing. A
 * Device Reset Request with a data stage gets a STALL too (S-10b.005 allows
 * either). Get Device Status answers OK, or Device_Busy while a transaction
 * is in progress, with no endpoint list; Get Extended Event Data answers
 * 8 zero bytes. usbd sends at most wLength bytes of an answer.
 */
static bool mtp_request(uint8_t rhport, uint8_t stage,
			const tusb_control_request_t *r)
{
	unsigned code;

	if (!u.mtp)
		return false;
	switch (r->bRequest) {
	case 0x64:	/* Cancel Request */
		if (r->bmRequestType != 0x21u || r->wLength != 6u)
			return false;
		if (stage == CONTROL_STAGE_SETUP)
			return tud_control_xfer(rhport, r, u.mtp_ctl, 6);
		if (stage == CONTROL_STAGE_DATA) {
			if (u.mtp_ctl[0] != 0x01u || u.mtp_ctl[1] != 0x40u)
				return false;	/* not 0x4001: STALL */
			mtp_abort();
		}
		return true;
	case 0x66:	/* Device Reset Request */
		if (r->bmRequestType != 0x21u || r->wLength != 0u)
			return false;
		if (stage != CONTROL_STAGE_SETUP)
			return true;
		mtp_abort();
		return tud_control_status(rhport, r);
	case 0x67:	/* Get Device Status */
		if (r->bmRequestType != 0xA1u)
			return false;
		if (stage != CONTROL_STAGE_SETUP)
			return true;
		/* OK as soon as a cancel is accepted, even while the
		 * request it ends has still to return; Device_Busy from a
		 * command's receipt, also while it waits to be run, as it
		 * does through a long operation (S-10b.009, S-10a.049) */
		code = u.mtp_owed_cancel || u.mtp_owed_reset ? S10B_OK :
		       u.mtp_in_request || u.mtp_rx_done || s10b_busy(u.mtp) ?
		       S10B_DEVICE_BUSY : S10B_OK;
		u.mtp_ctl[0] = 4;
		u.mtp_ctl[1] = 0;
		u.mtp_ctl[2] = (uint8_t)code;
		u.mtp_ctl[3] = (uint8_t)(code >> 8);
		return tud_control_xfer(rhport, r, u.mtp_ctl, 4);
	case 0x65:	/* Get Extended Event Data: no event (S-10b.008) */
		if (r->bmRequestType != 0xA1u)
			return false;
		if (stage != CONTROL_STAGE_SETUP)
			return true;
		memset(u.mtp_ctl, 0, 8);
		return tud_control_xfer(rhport, r, u.mtp_ctl, 8);
	default:
		break;
	}
	return false;
}

static void mtp_identity(void)
{
	if (u.mtp)
		s10b_identity(u.mtp, s10a_desc_text(u.set, NULL, 1),
			      s10a_desc_text(u.set, NULL, 2), u.serial);
}

/* --- The class driver --------------------------------------------------- */

static void input_current(unsigned ma)
{
	if (u.board && u.board->input_current)
		u.board->input_current(u.board->ctx, ma);
}

static void activity(void)
{
	if (u.board && u.board->activity)
		u.board->activity(u.board->ctx);
}

static void drv_init(void)
{
}

static bool drv_deinit(void)
{
	return true;
}

/*
 * usbd calls this on a bus reset, on the loss of VBUS and when a selected
 * configuration is left (SET_CONFIGURATION of another value or of 0,
 * S-10a.032's re-selection included). The endpoints are closed already.
 */
static void drv_reset(uint8_t rhport)
{
	(void)rhport;
	s10a_bot_stop(&u.bot);
	sync_reset_owed();
	s10a_lu_deconfigure(&u.lu);
	s10a_cdc_reset(&u.cdc);		/* S-10a.061 */
	s10c_midi_rx_init(&u.midi_rx);
	stream_close(&u.cdc_s);
	stream_close(&u.midi_s);
	u.n_marks = 0;
	u.msc_in = u.msc_out = u.notify = 0;
	mtp_close();
}

/*
 * The rest of a bus reset (S-10a.080), queued by the bus-event hook right
 * behind the reset itself, so it runs after usbd's reset and before any
 * request that follows the reset.
 */
static void bus_reset_done(void *param)
{
	(void)param;
	s10a_lu_bus_reset(&u.lu);
	input_current(UNIT_LOAD_MA);
	activity();
}

/* The interfaces of the function that starts at `first`. */
static unsigned function_span(unsigned first)
{
	enum s10a_function fn;
	unsigned n = 0, f;

	while (s10a_desc_interface(u.set, first + n, &fn, &f) && f == first)
		n++;
	return n;
}

static void open_endpoint(enum s10a_function fn,
			  const tusb_desc_endpoint_t *ep)
{
	const uint8_t addr = ep->bEndpointAddress;
	const bool in = (addr & 0x80u) != 0;
	struct stream *s = fn == S10A_FN_CDC ? &u.cdc_s :
			   fn == S10A_FN_MIDI ? &u.midi_s : NULL;

	if (fn == S10A_FN_MSC) {
		if (in)
			u.msc_in = addr;
		else
			u.msc_out = addr;
	} else if (s && ep->bmAttributes.xfer == TUSB_XFER_BULK) {
		if (in) {
			s->in = addr;
			s->mps = tu_edpt_packet_size(ep);
		} else {
			s->out = addr;
			s->out_mps = tu_edpt_packet_size(ep);
			if (s->out_mps > PKT)
				s->out_mps = PKT;
		}
	} else if (fn == S10A_FN_CDC && in) {
		u.notify = addr;
	} else if (fn == S10A_FN_MTP || fn == S10A_FN_MTP_VENDOR) {
		/* The interrupt endpoint stays idle (S-10b.008). */
		if (ep->bmAttributes.xfer != TUSB_XFER_BULK) {
			u.mtp_event = addr;
		} else if (in) {
			u.mtp_in = addr;
			mtp_reset(tu_edpt_packet_size(ep));
		} else {
			u.mtp_out = addr;
		}
	}
}

/*
 * Claims a whole function: its interfaces (S-10a.013's grouping, not the
 * class codes) and their endpoints. Returns the bytes it covers.
 */
static uint16_t drv_open(uint8_t rhport, const tusb_desc_interface_t *itf,
			 uint16_t max_len)
{
	const uint8_t *p = (const uint8_t *)itf;
	const uint8_t *const end = p + max_len;
	enum s10a_function fn;
	unsigned first, span;

	if (!s10a_desc_interface(u.set, itf->bInterfaceNumber, &fn, &first) ||
	    first != itf->bInterfaceNumber)
		return 0;
	span = function_span(first);
	while (p < end && p[0] >= 2u && p + p[0] <= end) {
		if (p[1] == TUSB_DESC_INTERFACE_ASSOCIATION && p != (const void *)itf)
			break;
		if (p[1] == TUSB_DESC_INTERFACE &&
		    (p[2] < first || p[2] >= first + span))
			break;
		if (p[1] == TUSB_DESC_ENDPOINT) {
			const tusb_desc_endpoint_t *ep =
				(const tusb_desc_endpoint_t *)(const void *)p;

			if (!usbd_edpt_open(rhport, ep))
				return 0;
			open_endpoint(fn, ep);
		}
		p += p[0];
	}
	return (uint16_t)(p - (const uint8_t *)itf);
}

static void clear(uint8_t ep)
{
	if (ep)
		usbd_edpt_clear_stall(RHPORT, ep);
}

/* SET_INTERFACE(0): the function's endpoints are reset (S-10a.032). */
static void function_restart(enum s10a_function fn)
{
	switch (fn) {
	case S10A_FN_MSC:
		clear(u.msc_in);
		clear(u.msc_out);
		s10a_bot_start(&u.bot);
		sync_reset_owed();
		break;
	case S10A_FN_CDC:
		clear(u.notify);
		clear(u.cdc_s.in);
		clear(u.cdc_s.out);
		stream_restart(&u.cdc_s);
		notify_kick();
		break;
	case S10A_FN_MIDI:
		clear(u.midi_s.in);
		clear(u.midi_s.out);
		stream_restart(&u.midi_s);
		break;
	case S10A_FN_MTP:
	case S10A_FN_MTP_VENDOR:
		clear(u.mtp_event);
		clear(u.mtp_in);
		clear(u.mtp_out);
		mtp_abort();
		break;
	default:
		break;
	}
}

/* CLEAR_FEATURE(ENDPOINT_HALT), after usbd has cleared it (S-10a.033). */
static void halt_cleared(uint8_t ep)
{
	if ((ep & 0x0Fu) == 0)
		return;
	if (ep == u.msc_in || ep == u.msc_out) {
		s10a_bot_clear_halt(&u.bot, ep == u.msc_in);
		sync_reset_owed();
	} else if (ep == u.cdc_s.in || ep == u.cdc_s.out) {
		stream_restart(&u.cdc_s);
	} else if (ep == u.midi_s.in || ep == u.midi_s.out) {
		stream_restart(&u.midi_s);
	} else if (ep == u.notify) {
		notify_kick();
	} else if (ep == u.mtp_in || ep == u.mtp_out) {
		mtp_abort();
	}
}

static bool msc_request(uint8_t rhport, uint8_t stage,
			const tusb_control_request_t *r)
{
	if (stage != CONTROL_STAGE_SETUP)
		return true;
	if (!s10a_bot_class_request(&u.bot, r->bmRequestType, r->bRequest,
				    r->wValue, r->wLength, u.ctl))
		return false;
	sync_reset_owed();
	return r->wLength ? tud_control_xfer(rhport, r, u.ctl, 1) :
			    tud_control_status(rhport, r);
}

static bool cdc_request(uint8_t rhport, uint8_t stage,
			const tusb_control_request_t *r)
{
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (stage == CONTROL_STAGE_DATA &&
	    r->bmRequestType_bit.direction == TUSB_DIR_OUT)
		s10a_cdc_coding_received(&u.cdc, u.coding);
	if (stage != CONTROL_STAGE_SETUP)
		return true;
	switch (s10a_cdc_request(&u.cdc, r->bmRequestType, r->bRequest,
				 r->wValue, r->wIndex, r->wLength, &data,
				 &len)) {
	case S10A_CDC_ACCEPT:
		/* SET_CONTROL_LINE_STATE: the receiver restarts at this
		 * point of the stream; two that cannot both be held become
		 * one at the later point. */
		if (u.n_marks == sizeof(u.marks) / sizeof(u.marks[0]))
			u.n_marks--;
		u.marks[u.n_marks++] = u.cdc_s.rx.head;
		notify_kick();
		return tud_control_status(rhport, r);
	case S10A_CDC_REPLY:
		return tud_control_xfer(rhport, r, data, len);
	case S10A_CDC_RECEIVE:
		/* Into a buffer of its own: the coding changes only once all
		 * 7 bytes have arrived. */
		return tud_control_xfer(rhport, r, u.coding, len);
	case S10A_CDC_STALL:
	default:
		break;
	}
	return false;
}

static bool drv_control(uint8_t rhport, uint8_t stage,
			const tusb_control_request_t *r)
{
	enum s10a_function fn;
	unsigned first;

	if (r->bmRequestType_bit.recipient == TUSB_REQ_RCPT_ENDPOINT) {
		/* usbd answers these itself; the function is only told. */
		if (stage == CONTROL_STAGE_SETUP &&
		    r->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
		    r->bRequest == TUSB_REQ_CLEAR_FEATURE &&
		    r->wValue == TUSB_REQ_FEATURE_EDPT_HALT)
			halt_cleared(tu_u16_low(r->wIndex));
		return true;
	}
	if (r->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
	    !s10a_desc_interface(u.set, tu_u16_low(r->wIndex), &fn, &first))
		return false;
	if (r->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
		if (r->bRequest != TUSB_REQ_SET_INTERFACE)
			return false;
		if (stage != CONTROL_STAGE_SETUP)
			return true;
		function_restart(fn);
		return tud_control_status(rhport, r);
	}
	if (r->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS)
		return false;
	switch (fn) {
	case S10A_FN_MSC:
		return msc_request(rhport, stage, r);
	case S10A_FN_CDC:
		return cdc_request(rhport, stage, r);
	case S10A_FN_MTP:
	case S10A_FN_MTP_VENDOR:
		return mtp_request(rhport, stage, r);
	case S10A_FN_MIDI:	/* audio: none (S-10a.070) */
	default:
		break;
	}
	return false;
}

static bool drv_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result,
		     uint32_t n)
{
	(void)rhport;
	if (result != XFER_RESULT_SUCCESS)
		return true;
	/* MTP's transfers move on from s10a_usb_poll(). */
	if (ep == u.mtp_out) {
		u.mtp_rx_n = (uint16_t)(n < PKT ? n : PKT);
		u.mtp_rx_done = true;
	} else if (ep == u.mtp_in) {
		/* the next one from s10a_usb_poll() */
	} else if (ep == u.msc_in || ep == u.msc_out) {
		s10a_bot_xfer_done(&u.bot, ep == u.msc_in, n);
		sync_reset_owed();
	} else if (ep == u.notify) {
		notify_kick();
	} else if (ep == u.cdc_s.in) {
		stream_kick_tx(&u.cdc_s);
	} else if (ep == u.cdc_s.out) {
		stream_rx_done(&u.cdc_s, n);
	} else if (ep == u.midi_s.in) {
		stream_kick_tx(&u.midi_s);
	} else if (ep == u.midi_s.out) {
		stream_rx_done(&u.midi_s, n);
	}
	return true;
}

static const usbd_class_driver_t driver = {
	.name = "s10a",
	.init = drv_init,
	.deinit = drv_deinit,
	.reset = drv_reset,
	.open = drv_open,
	.control_xfer_cb = drv_control,
	.xfer_cb = drv_xfer,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = 1;
	return &driver;
}

/* --- Configuration and bus events --------------------------------------- */

void tud_mount_cb(void)
{
	input_current(2u * s10a_desc_max_power(u.set, u.ch9.config));
	s10a_lu_select_configuration(&u.lu);
	if (u.msc_in && u.msc_out) {
		s10a_bot_start(&u.bot);
		sync_reset_owed();
	}
	stream_arm_rx(&u.cdc_s);
	stream_arm_rx(&u.midi_s);
	mtp_arm_rx();
}

void tud_umount_cb(void)
{
	/* SET_CONFIGURATION(0) or the loss of VBUS; not the first half of a
	 * re-selection, which keeps the target (S-10a.032). */
	if (u.ch9.config == 0)
		input_current(UNIT_LOAD_MA);
}

void tud_suspend_cb(bool remote_wakeup_en)
{
	(void)remote_wakeup_en;
	if (u.board && u.board->suspend)
		u.board->suspend(u.board->ctx, true);
	activity();
}

void tud_resume_cb(void)
{
	if (u.board && u.board->suspend)
		u.board->suspend(u.board->ctx, false);
	activity();
}

/* The loss of VBUS, as the bus reset: out of the interrupt. */
static void unplugged(void *param)
{
	(void)param;
	activity();
}

/* Interrupt context: every event as the controller driver queues it. */
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
	(void)rhport;
	if (eventid == DCD_EVENT_BUS_RESET) {
		s10a_ch9_reset(&u.ch9);
		usbd_defer_func(bus_reset_done, NULL, in_isr);
	} else if (eventid == DCD_EVENT_UNPLUGGED) {
		s10a_ch9_reset(&u.ch9);
		usbd_defer_func(unplugged, NULL, in_isr);
	}
}

enum wb_usb_setup_action s10a_usb_setup_filter(void *ctx,
		const uint8_t setup[8], uint8_t *reply, uint16_t *len)
{
	(void)ctx;
	switch (s10a_ch9_setup(&u.ch9, setup, reply, len)) {
	case S10A_CH9_STALL:
		return WB_USB_SETUP_STALL;
	case S10A_CH9_ACCEPT:
		return WB_USB_SETUP_ACCEPT;
	case S10A_CH9_REPLY:
		return WB_USB_SETUP_REPLY;
	case S10A_CH9_RESELECT:
		return WB_USB_SETUP_RESELECT;
	case S10A_CH9_FORWARD:
	default:
		break;
	}
	return WB_USB_SETUP_FORWARD;
}

/* --- Descriptors (S-10a.001 to S-10a.021) -------------------------------- */

static bool has_serial(void)
{
	return u.serial[0] != '\0';
}

static bool high_speed(void)
{
	return tud_speed_get() == TUSB_SPEED_HIGH;
}

uint8_t const *tud_descriptor_device_cb(void)
{
	static uint8_t d[18];

	s10a_desc_device(u.set, has_serial(), d);
	return d;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
	static uint8_t d[10];

	s10a_desc_qualifier(u.set, d);
	return d;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	static uint8_t d[S10A_CONFIG_MAX];

	return s10a_desc_config(u.set, has_serial(), index, high_speed(),
				false, d) ? d : NULL;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
	static uint8_t d[S10A_CONFIG_MAX];

	return s10a_desc_config(u.set, has_serial(), index, high_speed(),
				true, d) ? d : NULL;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	static union {
		uint16_t u16[S10A_STRING_MAX / 2u];
		uint8_t b[S10A_STRING_MAX];
	} d;

	(void)langid;
	return s10a_desc_string(u.set, u.serial, index, d.b) ? d.u16 : NULL;
}

/* --- The board's side --------------------------------------------------- */

void s10a_usb_init(const struct s10a_usb_board *board,
		   struct s10c_host *host)
{
	memset(&u, 0, sizeof(u));
	u.board = board;
	u.host = host;
	ring_init(&u.cdc_s.rx, cdc_rx_buf, sizeof(cdc_rx_buf));
	ring_init(&u.cdc_s.tx, cdc_tx_buf, sizeof(cdc_tx_buf));
	ring_init(&u.midi_s.rx, midi_rx_buf, sizeof(midi_rx_buf));
	ring_init(&u.midi_s.tx, midi_tx_buf, sizeof(midi_tx_buf));
	s10a_usb_select(S10A_SET_MIDI, NULL, NULL);
}

void s10a_usb_select(enum s10a_set set, const char *serial,
		     const struct wb_blockdev *disk)
{
	size_t n = 0;
	uint8_t itf = 0;
	enum s10a_function fn;
	unsigned i, first;

	u.set = set;
	memset(u.serial, 0, sizeof(u.serial));
	while (serial && n < S10A_SERIAL_LEN && serial[n]) {
		u.serial[n] = serial[n];
		n++;
	}
	s10a_ch9_start(&u.ch9, set);
	s10a_lu_init(&u.lu, disk, u.serial);
	u.lu.on_release = disk_released;
	s10a_bot_init(&u.bot, &u.lu, &bot_port);
	for (i = 0; s10a_desc_interface(set, i, &fn, &first); i++)
		if (fn == S10A_FN_CDC) {
			itf = (uint8_t)first;
			break;
		}
	s10a_cdc_init(&u.cdc, itf);
	s10c_midi_rx_init(&u.midi_rx);
	stream_close(&u.cdc_s);
	stream_close(&u.midi_s);
	u.msc_in = u.msc_out = u.notify = 0;
	u.n_marks = 0;
	mtp_close();
	mtp_identity();
}

void s10a_usb_mtp(struct s10b *m)
{
	u.mtp = m;
	mtp_close();
	mtp_identity();
}

void s10a_usb_poll(void)
{
	uint8_t pkt[4];

	stream_kick_tx(&u.cdc_s);
	stream_kick_tx(&u.midi_s);
	stream_arm_rx(&u.cdc_s);
	stream_arm_rx(&u.midi_s);
	notify_kick();
	if (u.polling)
		return;
	u.polling = true;
	mtp_run();
	/* Each restart once the board has read the bytes before it. */
	while (u.n_marks && u.marks[0] == u.cdc_s.rx.tail) {
		memmove(u.marks, u.marks + 1, --u.n_marks * sizeof(u.marks[0]));
		if (u.host)
			s10c_host_cdc_line_state(u.host);
	}
	if (!u.host) {
		u.polling = false;
		return;
	}
	while (ring_used(&u.midi_s.rx) >= sizeof(pkt) &&
	       (!u.board || !u.board->midi_ready ||
		u.board->midi_ready(u.board->ctx))) {
		size_t n;

		(void)ring_get(&u.midi_s.rx, pkt, sizeof(pkt));
		n = s10c_midi_rx_packet(&u.midi_rx, pkt);
		if (n)
			s10c_host_midi_message(u.host, u.midi_rx.buf, n);
		stream_arm_rx(&u.midi_s);
	}
	u.polling = false;
}

void s10a_usb_disk_offer(void)
{
	s10a_lu_offer(&u.lu);
}

void s10a_usb_disk_withdraw(enum s10a_withdrawal why)
{
	s10a_lu_withdraw(&u.lu, why);
}

bool s10a_usb_disk_busy(void)
{
	return s10a_bot_busy(&u.bot);
}

const struct s10a_lu *s10a_usb_disk(void)
{
	return &u.lu;
}

bool s10a_usb_cdc_open(void *ctx)
{
	(void)ctx;
	return u.cdc_s.in && s10a_cdc_open(&u.cdc);
}

size_t s10a_usb_cdc_read(uint8_t *buf, size_t n)
{
	uint32_t got, max = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;

	/* Not past a restart s10a_usb_poll() has still to deliver. */
	if (u.n_marks && u.marks[0] - u.cdc_s.rx.tail < max)
		max = u.marks[0] - u.cdc_s.rx.tail;
	got = ring_get(&u.cdc_s.rx, buf, max);

	stream_arm_rx(&u.cdc_s);
	return got;
}

size_t s10a_usb_cdc_write(const uint8_t *buf, size_t n)
{
	uint32_t fit;

	if (!u.cdc_s.in)
		return 0;
	fit = ring_free(&u.cdc_s.tx);
	if (n < fit)
		fit = (uint32_t)n;
	ring_put(&u.cdc_s.tx, buf, fit);
	stream_kick_tx(&u.cdc_s);
	return fit;
}

size_t s10a_usb_cdc_pending(void)
{
	return ring_used(&u.cdc_s.tx);
}

size_t s10a_usb_cdc_room(void)
{
	return u.cdc_s.in ? ring_free(&u.cdc_s.tx) : 0;
}

static void midi_emit(void *ctx, const uint8_t packet[4])
{
	(void)ctx;
	ring_put(&u.midi_s.tx, packet, 4);
}

bool s10a_usb_midi_open(void)
{
	return u.midi_s.in != 0;
}

size_t s10a_usb_midi_room(void)
{
	return u.midi_s.in ? ring_free(&u.midi_s.tx) : 0;
}

/* Three bytes to an event packet, the last packet's 1 to 3 included. */
size_t s10a_usb_sysex_size(size_t n)
{
	return n > SIZE_MAX / 2u ? SIZE_MAX : (n + 2u) / 3u * 4u;
}

void s10a_usb_send_sysex(void *ctx, const uint8_t *msg, size_t n)
{
	(void)ctx;
	if (n == 0 || s10a_usb_sysex_size(n) > s10a_usb_midi_room())
		return;
	s10c_usbmidi_send(msg, n, midi_emit, NULL);
	stream_kick_tx(&u.midi_s);
}
