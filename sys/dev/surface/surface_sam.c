/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Paige Adele Thompson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Surface Serial Hub (SSH) transport driver for the Surface Aggregator
 * Module (SAM/SSAM) embedded controller, found in Microsoft Surface
 * devices including the Surface Laptop 4.  Attaches to the ACPI serial
 * hub device (MSHW0084), drives its 16550-compatible UART directly and
 * implements the SSH wire protocol: framing, CRC, sequence numbers,
 * acknowledgement/retransmission, synchronous requests and events.
 */

#include "opt_acpi.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "acpi_if.h"

#include "surface_sam.h"
#include "surface_sam_uart.h"

MALLOC_DEFINE(M_SURFACE_SAM, "surface_sam",
    "Surface Serial Hub (SAM) transport");

/* Timing, mirroring the reference implementation's packet layer. */
#define	SAM_ACK_TIMEOUT_MS	1000	/* wait for EC ACK of our frame */
#define	SAM_ACK_TRIES		3	/* transmission attempts per frame */
#define	SAM_RQST_TIMEOUT_MS	1000	/* wait for EC response payload */

/* Recent EC sequence numbers retained for duplicate detection. */
#define	SAM_SEQ_WINDOW		8

/*
 * Message layout offsets.  A transmitted message starts with SYN; a
 * received one has the SYN consumed by the parser, so frame/payload
 * offsets differ by the SYN size.
 */
#define	SAM_TX_PAYLOAD_OFF	(SSH_SYN_SIZE + SSH_FRAME_SIZE + \
	    SSH_CRC_SIZE)
#define	SAM_RX_PAYLOAD_OFF	(SSH_FRAME_SIZE + SSH_CRC_SIZE)

/* Receive parser states. */
#define	SAM_RX_SYN1		0	/* hunting for 0xaa */
#define	SAM_RX_SYN2		0x55	/* hunting for 0x55 */
#define	SAM_RX_FRAME		1	/* collecting frame + frame CRC */
#define	SAM_RX_PAYLOAD		2	/* collecting payload + payload CRC */

/* Acknowledgement states for the frame currently in flight. */
#define	SAM_ACK_NONE		0	/* not waiting / already handled */
#define	SAM_ACK_WAIT		1	/* waiting for ACK/NAK */
#define	SAM_ACK_DONE		2	/* acknowledged */
#define	SAM_ACK_NAK		3	/* negatively acknowledged */

/*
 * Received bytes are queued here by the UART layer (interrupt context) and
 * parsed on the private taskqueue thread.  The parser acknowledges frames
 * by transmitting, and transmitting can wait for the transmitter, so doing
 * that straight from the interrupt handler would hold up the thread that
 * interrupt handlers and timed sleeps run on.
 */
#define	SAM_RXQ_SIZE		2048

static char *surface_sam_ids[] = {
	"MSHW0084",
	NULL
};

struct surface_sam {
	device_t		 sam_dev;
	ACPI_HANDLE		 sam_handle;
	struct mtx		 sam_mtx;
	struct sam_uart		*sam_uart;
	struct taskqueue	*sam_tq;
	struct task		 sam_ev_task;
	TAILQ_HEAD(, sam_event_item) sam_evq;
	struct sam_event_reg {
		surface_sam_event_fn fn;
		void		*arg;
	} sam_regs[SSH_NUM_EVENTS];

	/* Transmit side (all fields guarded by sam_mtx). */
	uint8_t			 sam_tx_seq;
	uint16_t		 sam_rqid;

	/* Synchronous request in flight (guarded by sam_mtx). */
	bool			 sam_rqst_active;
	bool			 sam_rqst_have;
	int			 sam_rqst_status;
	size_t			 sam_rqst_rlen;
	size_t			 sam_rqst_rcap;
	void			*sam_rqst_data;
	uint16_t		 sam_rqst_rqid;
	int			 sam_ack_state;
	uint8_t			 sam_ack_seq;

	/* Receive parser (guarded by sam_mtx). */
	uint8_t			 sam_rx_state;
	size_t			 sam_rx_n;
	size_t			 sam_rx_plen;
	size_t			 sam_rx_payn;
	uint8_t			 sam_rx_buf[SAM_RX_PAYLOAD_OFF +
	    SSH_MAX_PAYLOAD + SSH_CRC_SIZE];
	uint8_t			 sam_rx_win[SAM_SEQ_WINDOW];
	uint8_t			 sam_rx_win_off;

	/* Bytes received in interrupt context, parsed on sam_tq. */
	struct task		 sam_rx_task;
	struct mtx		 sam_rxq_mtx;
	uint8_t			 sam_rxq[SAM_RXQ_SIZE];
	size_t			 sam_rxq_head;
	size_t			 sam_rxq_tail;
	uint64_t		 sam_st_rx_overrun;

	/* Rate limit on the NAKs we answer bad frames with. */
	int			 sam_nak_ticks;

	/* Statistics / sysctls. */
	uint64_t		 sam_st_tx;
	uint64_t		 sam_st_rx;
	uint64_t		 sam_st_dup;
	uint64_t		 sam_st_crc_err;
	uint64_t		 sam_st_bad;
	uint64_t		 sam_st_unexpected;
	uint64_t		 sam_st_events;
	uint64_t		 sam_st_unhandled;
	uint64_t		 sam_st_dropped;
	uint64_t		 sam_st_ack_timeout;
	uint64_t		 sam_st_nak;
	uint64_t		 sam_st_timeout;
	uint64_t		 sam_st_tx_err;
	uint32_t		 sam_fw_version;
};

struct sam_event_item {
	TAILQ_ENTRY(sam_event_item) link;
	surface_sam_event_fn	 fn;
	void			*arg;
	struct surface_sam_event ev;
	uint8_t			 data[];
};

/* Resource source string of the UART serial bus descriptor. */
#define	SAM_RSRC_PATH_MAX	128

struct sam_crs_ctx {
	struct sam_uart_config	*cfg;
	device_t		 dev;
	bool			 have_io;
	bool			 have_uart;
	bool			 have_rsrc;
	char			 rsrc[SAM_RSRC_PATH_MAX];
};

static struct surface_sam *surface_sam_sc;

static int surface_sam_probe(device_t dev);
static int surface_sam_attach(device_t dev);
static int surface_sam_detach(device_t dev);

static device_method_t surface_sam_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		surface_sam_probe),
	DEVMETHOD(device_attach,	surface_sam_attach),
	DEVMETHOD(device_detach,	surface_sam_detach),

	DEVMETHOD_END
};

static driver_t surface_sam_driver = {
	"surface_sam",
	surface_sam_methods,
	sizeof(struct surface_sam),
};

DRIVER_MODULE(surface_sam, acpi, surface_sam_driver, 0, 0);
MODULE_DEPEND(surface_sam, acpi, 1, 1, 1);
MODULE_VERSION(surface_sam, 1);
ACPI_PNP_INFO(surface_sam_ids);

static int
surface_ms2ticks(int ms)
{
	int ticks;

	ticks = ms * hz / 1000;
	return (ticks > 0 ? ticks : 1);
}

static uint16_t
surface_sam_crc16(const uint8_t *buf, size_t len)
{
	uint16_t crc;
	size_t i;
	int b;

	crc = 0xffff;
	for (i = 0; i < len; i++) {
		crc ^= (uint16_t)buf[i] << 8;
		for (b = 0; b < 8; b++) {
			if ((crc & 0x8000) != 0)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	return (crc);
}

/*
 * Transmit one SSH message: SYN + frame + frame CRC + payload + payload
 * CRC.  Called with sam_mtx held; may busy-wait for the transmitter.
 */
static int
surface_sam_tx_msg(struct surface_sam *sam, uint8_t type, uint8_t seq,
    const uint8_t *payload, size_t plen)
{
	uint8_t buf[SSH_MSG_OVERHEAD + SSH_MAX_PAYLOAD];
	uint16_t crc;

	if (plen > SSH_MAX_PAYLOAD)
		return (EMSGSIZE);

	/* SYN 0x55AA, little-endian on the wire. */
	le16enc(buf, SSH_MSG_SYN);
	buf[2] = type;
	le16enc(buf + 3, (uint16_t)plen);
	buf[5] = seq;
	crc = surface_sam_crc16(buf + 2, SSH_FRAME_SIZE);
	le16enc(buf + 6, crc);
	if (plen > 0)
		memcpy(buf + SAM_TX_PAYLOAD_OFF, payload, plen);
	crc = surface_sam_crc16(buf + SAM_TX_PAYLOAD_OFF, plen);
	le16enc(buf + SAM_TX_PAYLOAD_OFF + plen, crc);

	return (sam_uart_tx(sam->sam_uart, buf,
	    SAM_TX_PAYLOAD_OFF + plen + SSH_CRC_SIZE));
}

static void
surface_sam_send_ack(struct surface_sam *sam, uint8_t seq)
{
	int error;

	error = surface_sam_tx_msg(sam, SSH_FRAME_TYPE_ACK, seq, NULL, 0);
	if (error != 0)
		sam->sam_st_tx_err++;
}

static void
surface_sam_send_nak(struct surface_sam *sam)
{
	int error;

	/*
	 * A NAK is best effort, so rate limit it.  A port that reads back
	 * 0xff, or a line running at the wrong rate, produces frames we
	 * reject; answering each of them would pit the EC and us in a
	 * ping-pong that keeps the thread that delivers received bytes
	 * (the callout thread when there is no interrupt) busy enough to
	 * hold up every timed sleep queued behind it.
	 */
	if (ticks - sam->sam_nak_ticks < hz / 10)
		return;
	sam->sam_nak_ticks = ticks;

	error = surface_sam_tx_msg(sam, SSH_FRAME_TYPE_NAK, 0x00, NULL, 0);
	if (error != 0)
		sam->sam_st_tx_err++;
}

static void
surface_sam_event_task(void *arg, int pending)
{
	struct surface_sam *sam;
	struct sam_event_item *item;

	(void)pending;
	sam = arg;
	for (;;) {
		mtx_lock(&sam->sam_mtx);
		item = TAILQ_FIRST(&sam->sam_evq);
		if (item != NULL)
			TAILQ_REMOVE(&sam->sam_evq, item, link);
		mtx_unlock(&sam->sam_mtx);
		if (item == NULL)
			break;
		item->fn(item->arg, &item->ev);
		free(item, M_SURFACE_SAM);
	}
}

/* Called with sam_mtx held. */
static void
surface_sam_deliver_event(struct surface_sam *sam, uint16_t rqid, uint8_t tc,
    uint8_t sid, uint8_t iid, uint8_t cid, const uint8_t *data, size_t len)
{
	struct sam_event_item *item;
	size_t idx;

	idx = rqid - 1;
	if (sam->sam_regs[idx].fn == NULL) {
		sam->sam_st_unhandled++;
		return;
	}

	item = malloc(sizeof(*item) + len, M_SURFACE_SAM, M_NOWAIT | M_ZERO);
	if (item == NULL) {
		sam->sam_st_dropped++;
		return;
	}
	item->fn = sam->sam_regs[idx].fn;
	item->arg = sam->sam_regs[idx].arg;
	item->ev.ev_tc = tc;
	item->ev.ev_tid = sid;
	item->ev.ev_iid = iid;
	item->ev.ev_cid = cid;
	item->ev.ev_data = item->data;
	item->ev.ev_len = len;
	if (len > 0)
		memcpy(item->data, data, len);
	TAILQ_INSERT_TAIL(&sam->sam_evq, item, link);
	taskqueue_enqueue(sam->sam_tq, &sam->sam_ev_task);
	sam->sam_st_events++;
}

/*
 * Handle a validated data frame payload: command header plus optional
 * event/response data.  Called with sam_mtx held.
 */
static void
surface_sam_rx_payload(struct surface_sam *sam, const uint8_t *payload,
    size_t plen)
{
	uint16_t rqid;
	size_t len;
	uint8_t tc, sid, iid, cid;
	const uint8_t *data;

	if (plen < SSH_CMD_SIZE || payload[0] != SSH_PAYLOAD_TYPE_CMD) {
		sam->sam_st_bad++;
		return;
	}
	tc = payload[1];
	sid = payload[3];
	iid = payload[4];
	rqid = le16dec(payload + 5);
	cid = payload[7];
	data = payload + SSH_CMD_SIZE;
	len = plen - SSH_CMD_SIZE;

	if (rqid > 0 && rqid <= SSH_NUM_EVENTS) {
		/* Event: request IDs 1..38, keyed by target category. */
		surface_sam_deliver_event(sam, rqid, tc, sid, iid, cid,
		    data, len);
		return;
	}

	if (sam->sam_rqst_active && !sam->sam_rqst_have &&
	    rqid == sam->sam_rqst_rqid) {
		if (len > sam->sam_rqst_rcap) {
			sam->sam_rqst_status = ENOBUFS;
			len = sam->sam_rqst_rcap;
		}
		if (len > 0 && sam->sam_rqst_data != NULL)
			memcpy(sam->sam_rqst_data, data, len);
		sam->sam_rqst_rlen = len;
		sam->sam_rqst_have = true;
		wakeup(&sam->sam_rqst_have);
		return;
	}

	sam->sam_st_unexpected++;
}

static bool
surface_sam_seq_recent(struct surface_sam *sam, uint8_t seq)
{
	size_t i;

	for (i = 0; i < SAM_SEQ_WINDOW; i++) {
		if (sam->sam_rx_win[i] == seq)
			return (true);
	}
	return (false);
}

/* Called with sam_mtx held: a complete, CRC-valid message was parsed. */
static void
surface_sam_rx_frame(struct surface_sam *sam)
{
	const uint8_t *payload;
	uint8_t type, seq;
	uint16_t plen;

	type = sam->sam_rx_buf[0];
	plen = le16dec(sam->sam_rx_buf + 1);
	seq = sam->sam_rx_buf[3];

	switch (type) {
	case SSH_FRAME_TYPE_ACK:
	case SSH_FRAME_TYPE_NAK:
		if (sam->sam_ack_state != SAM_ACK_WAIT ||
		    seq != sam->sam_ack_seq)
			break;
		sam->sam_ack_state = type == SSH_FRAME_TYPE_ACK ?
		    SAM_ACK_DONE : SAM_ACK_NAK;
		wakeup(&sam->sam_ack_state);
		break;
	case SSH_FRAME_TYPE_DATA_SEQ:
		if (surface_sam_seq_recent(sam, seq)) {
			/* Retransmission: re-ACK, do not reprocess. */
			sam->sam_st_dup++;
			surface_sam_send_ack(sam, seq);
			break;
		}
		sam->sam_rx_win[sam->sam_rx_win_off] = seq;
		sam->sam_rx_win_off = (sam->sam_rx_win_off + 1) %
		    SAM_SEQ_WINDOW;
		sam->sam_st_rx++;
		surface_sam_send_ack(sam, seq);
		payload = sam->sam_rx_buf + SAM_RX_PAYLOAD_OFF;
		surface_sam_rx_payload(sam, payload, plen);
		break;
	case SSH_FRAME_TYPE_DATA_NSQ:
		sam->sam_st_rx++;
		payload = sam->sam_rx_buf + SAM_RX_PAYLOAD_OFF;
		surface_sam_rx_payload(sam, payload, plen);
		break;
	default:
		sam->sam_st_bad++;
		break;
	}
}

/*
 * Byte-at-a-time receive parser.  Buffer layout: frame header and frame
 * CRC in the first 6 bytes, payload and payload CRC after that.
 * Called with sam_mtx held.
 */
static void
surface_sam_rx_byte(struct surface_sam *sam, uint8_t b)
{
	uint16_t crc, want;

	switch (sam->sam_rx_state) {
	case SAM_RX_SYN1:
		if (b == 0xaa)
			sam->sam_rx_state = SAM_RX_SYN2;
		break;
	case SAM_RX_SYN2:
		if (b == 0x55) {
			sam->sam_rx_state = SAM_RX_FRAME;
			sam->sam_rx_n = 0;
		} else if (b != 0xaa) {
			sam->sam_rx_state = SAM_RX_SYN1;
		}
		break;
	case SAM_RX_FRAME:
		sam->sam_rx_buf[sam->sam_rx_n++] = b;
		if (sam->sam_rx_n < 6)
			break;
		crc = surface_sam_crc16(sam->sam_rx_buf, SSH_FRAME_SIZE);
		want = le16dec(sam->sam_rx_buf + SSH_FRAME_SIZE);
		if (crc != want) {
			sam->sam_st_crc_err++;
			surface_sam_send_nak(sam);
			sam->sam_rx_state = SAM_RX_SYN1;
			break;
		}
		sam->sam_rx_plen = le16dec(sam->sam_rx_buf + 1);
		if (sam->sam_rx_plen > SSH_MAX_PAYLOAD) {
			sam->sam_st_bad++;
			surface_sam_send_nak(sam);
			sam->sam_rx_state = SAM_RX_SYN1;
			break;
		}
		sam->sam_rx_payn = 0;
		sam->sam_rx_state = SAM_RX_PAYLOAD;
		break;
	case SAM_RX_PAYLOAD:
		sam->sam_rx_buf[SAM_RX_PAYLOAD_OFF + sam->sam_rx_payn++] = b;
		if (sam->sam_rx_payn < sam->sam_rx_plen + SSH_CRC_SIZE)
			break;
		crc = surface_sam_crc16(
		    sam->sam_rx_buf + SAM_RX_PAYLOAD_OFF, sam->sam_rx_plen);
		want = le16dec(sam->sam_rx_buf + SAM_RX_PAYLOAD_OFF +
		    sam->sam_rx_plen);
		if (crc != want) {
			sam->sam_st_crc_err++;
			surface_sam_send_nak(sam);
			sam->sam_rx_state = SAM_RX_SYN1;
			break;
		}
		surface_sam_rx_frame(sam);
		sam->sam_rx_state = SAM_RX_SYN1;
		break;
	default:
		sam->sam_rx_state = SAM_RX_SYN1;
		break;
	}
}

/*
 * Parse queued receive bytes.  Runs on the private taskqueue thread, so it
 * may take sam_mtx and transmit (frames are acknowledged from here); the
 * receive callback that feeds the queue runs in interrupt context and does
 * neither.
 */
static void
surface_sam_rx_task(void *arg, int pending)
{
	struct surface_sam *sam;
	uint8_t buf[64];
	size_t n, i, off;

	(void)pending;
	sam = arg;
	for (;;) {
		/* Move a chunk out of the queue, then parse it unlocked. */
		n = 0;
		mtx_lock(&sam->sam_rxq_mtx);
		while (n < sizeof(buf) &&
		    sam->sam_rxq_head != sam->sam_rxq_tail) {
			off = sam->sam_rxq_head;
			buf[n++] = sam->sam_rxq[off];
			sam->sam_rxq_head = (off + 1) % SAM_RXQ_SIZE;
		}
		mtx_unlock(&sam->sam_rxq_mtx);
		if (n == 0)
			break;

		mtx_lock(&sam->sam_mtx);
		for (i = 0; i < n; i++)
			surface_sam_rx_byte(sam, buf[i]);
		mtx_unlock(&sam->sam_mtx);
	}
}

/*
 * Receive callback from the UART layer.  Called in interrupt context with no
 * locks held: queue the bytes and let the taskqueue thread parse them, so
 * that neither sam_mtx nor the transmitter is waited for here.
 */
static void
surface_sam_input(void *arg, const uint8_t *buf, size_t len)
{
	struct surface_sam *sam;
	size_t i, off, next;

	sam = arg;
	mtx_lock(&sam->sam_rxq_mtx);
	for (i = 0; i < len; i++) {
		next = (sam->sam_rxq_tail + 1) % SAM_RXQ_SIZE;
		if (next == sam->sam_rxq_head) {
			/*
			 * The EC outran us.  Drop the rest of this burst and
			 * let the frame CRC checks resynchronise the parser.
			 */
			sam->sam_st_rx_overrun += len - i;
			break;
		}
		off = sam->sam_rxq_tail;
		sam->sam_rxq[off] = buf[i];
		sam->sam_rxq_tail = next;
	}
	mtx_unlock(&sam->sam_rxq_mtx);

	taskqueue_enqueue(sam->sam_tq, &sam->sam_rx_task);
}

/*
 * Wait for a condition to change, with sam_mtx released, and report
 * EWOULDBLOCK if it did not change within the time given.
 *
 * While the kernel is still coming up - which is when the transport attaches,
 * since ACPI devices attach from root_bus_configure() - a timed sleep on
 * thread0 panics ("timed sleep before timers are working") and the clock
 * does not run yet either.  There, wait by spinning, bounded by an iteration
 * count rather than by the clock.
 */
static int
surface_sam_wait(struct surface_sam *sam, const void *what, int expect,
    int ms)
{
	const volatile int *cond;
	int i, rounds;

	if (!cold)
		return (msleep(what, &sam->sam_mtx, PRIBIO, "samwait",
		    surface_ms2ticks(ms)));

	/* Read the condition through a volatile view of the same address. */
	cond = what;
	mtx_unlock(&sam->sam_mtx);
	rounds = ms * 20;			/* DELAY(50) rounds per ms */
	for (i = 0; i < rounds; i++) {
		DELAY(50);
		if (*cond != expect)
			break;
	}
	mtx_lock(&sam->sam_mtx);
	if (*cond == expect)
		return (EWOULDBLOCK);
	return (0);
}

/*
 * As surface_sam_wait(), for a condition held in a bool.
 */
static int
surface_sam_wait_bool(struct surface_sam *sam, const void *what, bool expect,
    int ms)
{
	const volatile bool *cond;
	int i, rounds;

	if (!cold)
		return (msleep(what, &sam->sam_mtx, PRIBIO, "samwait",
		    surface_ms2ticks(ms)));

	cond = what;
	mtx_unlock(&sam->sam_mtx);
	rounds = ms * 20;			/* DELAY(50) rounds per ms */
	for (i = 0; i < rounds; i++) {
		DELAY(50);
		if (*cond != expect)
			break;
	}
	mtx_lock(&sam->sam_mtx);
	if (*cond == expect)
		return (EWOULDBLOCK);
	return (0);
}

/*
 * Execute a synchronous request.  Caller holds sam_mtx; it is released
 * while waiting for the ACK and the response.
 */
static int
surface_sam_request_locked(struct surface_sam *sam, uint8_t tc, uint8_t tid,
    uint8_t iid, uint8_t cid, const void *wdata, size_t wlen, void *rdata,
    size_t *rlen, int timeout_ms)
{
	uint8_t payload[SSH_CMD_SIZE + SSH_MAX_PAYLOAD];
	int error, tries, timeout;
	uint16_t rqid;
	uint8_t seq;
	size_t cap;

	if (wlen > SSH_MAX_PAYLOAD - SSH_CMD_SIZE)
		return (EMSGSIZE);
	if (sam->sam_rqst_active)
		return (EBUSY);

	cap = *rlen;
	if (rdata == NULL || cap > SSH_MAX_PAYLOAD)
		cap = rdata != NULL ? SSH_MAX_PAYLOAD : 0;
	*rlen = 0;

	/* Build the command payload: header + caller data. */
	payload[0] = SSH_PAYLOAD_TYPE_CMD;
	payload[1] = tc;
	payload[2] = tid;
	payload[3] = SSH_TID_HOST;
	payload[4] = iid;
	rqid = sam->sam_rqid;
	sam->sam_rqid = rqid > 0 ? rqid + 1 : SSH_NUM_EVENTS + 1;
	le16enc(payload + 5, rqid);
	sam->sam_rqst_rqid = rqid;
	payload[7] = cid;
	if (wlen > 0)
		memcpy(payload + SSH_CMD_SIZE, wdata, wlen);

	/*
	 * The sequence number is chosen once per message; retransmissions
	 * reuse it so the EC can detect duplicates.
	 */
	seq = sam->sam_tx_seq++;
	sam->sam_rqst_active = true;
	sam->sam_rqst_have = false;
	sam->sam_rqst_status = 0;
	sam->sam_rqst_rlen = 0;
	sam->sam_rqst_rcap = cap;
	sam->sam_rqst_data = rdata;
	sam->sam_ack_seq = seq;

	error = 0;
	for (tries = 0; tries < SAM_ACK_TRIES; tries++) {
		sam->sam_ack_state = SAM_ACK_WAIT;
		error = surface_sam_tx_msg(sam, SSH_FRAME_TYPE_DATA_SEQ, seq,
		    payload, SSH_CMD_SIZE + wlen);
		if (error != 0)
			break;
		while (error == 0 && sam->sam_ack_state == SAM_ACK_WAIT) {
			error = surface_sam_wait(sam, &sam->sam_ack_state,
			    SAM_ACK_WAIT, SAM_ACK_TIMEOUT_MS);
			if (error == EWOULDBLOCK)
				error = ETIMEDOUT;
		}
		if (error == ETIMEDOUT) {
			sam->sam_st_ack_timeout++;
			continue;
		}
		if (error != 0)
			break;
		if (sam->sam_ack_state == SAM_ACK_NAK) {
			sam->sam_st_nak++;
			continue;
		}
		break;
	}
	if (error != 0)
		goto fail;
	if (tries == SAM_ACK_TRIES) {
		error = ETIMEDOUT;
		goto fail;
	}

	/* Transmit succeeded. */
	sam->sam_st_tx++;

	/*
	 * Requests with no response payload complete as soon as the EC has
	 * acknowledged them: the EC only sends a response frame when the
	 * host asked for one (SSAM_REQUEST_HAS_RESPONSE semantics).
	 */
	if (rdata == NULL) {
		sam->sam_rqst_active = false;
		sam->sam_rqst_status = 0;
		return (0);
	}

	/* Wait for the response. */
	timeout = timeout_ms > 0 ? timeout_ms : SAM_RQST_TIMEOUT_MS;
	error = 0;
	while (!sam->sam_rqst_have && error == 0) {
		error = surface_sam_wait_bool(sam, &sam->sam_rqst_have,
		    false, timeout);
		if (error == EWOULDBLOCK)
			error = ETIMEDOUT;
	}
	if (error != 0) {
		sam->sam_st_timeout++;
		goto fail;
	}

	sam->sam_rqst_active = false;
	error = sam->sam_rqst_status;
	*rlen = sam->sam_rqst_rlen;
	return (error);

fail:
	sam->sam_rqst_active = false;
	sam->sam_ack_state = SAM_ACK_NONE;
	return (error);
}

struct surface_sam *
surface_sam_get(void)
{

	return (surface_sam_sc);
}

/*
 * HID-over-SAM nodes found on Surface Laptop 3/4/5 class machines: the
 * keyboard, the touchpad and an auxiliary node, all behind the KIP hub.
 * A node is only present if the EC implements it; probing a missing node
 * fails cleanly.
 */
static const struct surface_sam_hid_node surface_sam_hid_nodes[] = {
	{ SSH_TID_KIP, 0x01 },
	{ SSH_TID_KIP, 0x03 },
	{ SSH_TID_KIP, 0x05 },
};

int
surface_sam_hid_node(u_int unit, struct surface_sam_hid_node *node)
{

	if (unit >= nitems(surface_sam_hid_nodes))
		return (ENOENT);
	node->hid_tid = surface_sam_hid_nodes[unit].hid_tid;
	node->hid_iid = surface_sam_hid_nodes[unit].hid_iid;
	return (0);
}

int
surface_sam_request(struct surface_sam *sam, uint8_t tc, uint8_t tid,
    uint8_t iid, uint8_t cid, const void *wdata, size_t wlen, void *rdata,
    size_t *rlen, int timeout_ms)
{
	int error;

	if (sam == NULL || rlen == NULL)
		return (ENXIO);

	mtx_lock(&sam->sam_mtx);
	error = surface_sam_request_locked(sam, tc, tid, iid, cid, wdata,
	    wlen, rdata, rlen, timeout_ms);
	mtx_unlock(&sam->sam_mtx);
	return (error);
}

int
surface_sam_register_event(struct surface_sam *sam, uint8_t tc,
    surface_sam_event_fn fn, void *arg)
{

	if (sam == NULL || tc == 0 || tc > SSH_NUM_EVENTS)
		return (EINVAL);

	mtx_lock(&sam->sam_mtx);
	sam->sam_regs[tc - 1].fn = fn;
	sam->sam_regs[tc - 1].arg = arg;
	mtx_unlock(&sam->sam_mtx);
	return (0);
}

void
surface_sam_unregister_event(struct surface_sam *sam, uint8_t tc)
{

	if (sam == NULL || tc == 0 || tc > SSH_NUM_EVENTS)
		return;

	mtx_lock(&sam->sam_mtx);
	sam->sam_regs[tc - 1].fn = NULL;
	sam->sam_regs[tc - 1].arg = NULL;
	mtx_unlock(&sam->sam_mtx);
}

static ACPI_STATUS
surface_sam_crs_cb(ACPI_RESOURCE *res, void *arg)
{
	struct sam_crs_ctx *ctx;
	struct sam_uart_config *cfg;
	ACPI_RESOURCE_UART_SERIALBUS *ub;
	uint8_t lcr;

	ctx = arg;
	cfg = ctx->cfg;

	switch (res->Type) {
	case ACPI_RESOURCE_TYPE_SERIAL_BUS:
		/*
		 * The type field of the common serial-bus header overlays
		 * the same offset in every serial-bus descriptor, so this
		 * also rejects I2C/SPI resources safely.
		 */
		if (res->Data.UartSerialBus.Type !=
		    ACPI_RESOURCE_SERIAL_TYPE_UART) {
			if (bootverbose)
				device_printf(ctx->dev,
				    "_CRS: serial bus type %u, not UART\n",
				    res->Data.UartSerialBus.Type);
			break;
		}
		ub = &res->Data.UartSerialBus;
		cfg->uart_baud = ub->DefaultBaudRate;
		cfg->uart_hwflow = ub->FlowControl ==
		    ACPI_UART_FLOW_CONTROL_HW;
		/*
		 * ACPI data/stop-bit encodings map directly onto the
		 * 16550 line control register fields.
		 */
		lcr = ub->DataBits & 0x03;
		if (ub->StopBits == ACPI_UART_2_STOP_BITS)
			lcr |= 0x04;
		switch (ub->Parity) {
		case ACPI_UART_PARITY_EVEN:
			lcr |= 0x08;
			break;
		case ACPI_UART_PARITY_ODD:
			lcr |= 0x08 | 0x10;
			break;
		case ACPI_UART_PARITY_MARK:
			lcr |= 0x08 | 0x10 | 0x20;
			break;
		case ACPI_UART_PARITY_SPACE:
			lcr |= 0x08 | 0x20;
			break;
		default:
			break;
		}
		cfg->uart_lcr = lcr;
		/*
		 * Remember where the controller lives (e.g.
		 * \_SB.PCI0.UA00); the actual window follows it.
		 */
		if (ub->ResourceSource.StringPtr != NULL &&
		    ub->ResourceSource.StringLength > 0) {
			strlcpy(ctx->rsrc, ub->ResourceSource.StringPtr,
			    sizeof(ctx->rsrc));
			ctx->have_rsrc = true;
		}
		ctx->have_uart = true;
		if (bootverbose)
			device_printf(ctx->dev,
			    "_CRS: uart baud %u hwflow %d lcr %#x, "
			    "controller %s\n", cfg->uart_baud,
			    cfg->uart_hwflow, cfg->uart_lcr,
			    ctx->have_rsrc ? ctx->rsrc : "-");
		break;
	case ACPI_RESOURCE_TYPE_IO:
		cfg->uart_iobase = res->Data.Io.Minimum;
		cfg->uart_nports = res->Data.Io.AddressLength;
		ctx->have_io = true;
		if (bootverbose)
			device_printf(ctx->dev, "_CRS: I/O %#jx+%u\n",
			    (uintmax_t)res->Data.Io.Minimum,
			    res->Data.Io.AddressLength);
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		cfg->uart_iobase = res->Data.FixedIo.Address;
		cfg->uart_nports = res->Data.FixedIo.AddressLength;
		ctx->have_io = true;
		if (bootverbose)
			device_printf(ctx->dev, "_CRS: fixed I/O %#jx+%u\n",
			    (uintmax_t)res->Data.FixedIo.Address,
			    res->Data.FixedIo.AddressLength);
		break;
	case ACPI_RESOURCE_TYPE_IRQ:
		if (res->Data.Irq.InterruptCount == 0)
			break;
		cfg->uart_irq = res->Data.Irq.Interrupts[0];
		cfg->uart_have_irq = true;
		cfg->uart_irq_trigger = res->Data.Irq.Triggering;
		cfg->uart_irq_polarity = res->Data.Irq.Polarity;
		if (bootverbose)
			device_printf(ctx->dev,
			    "_CRS: irq %ju trig %u pol %u\n",
			    (uintmax_t)cfg->uart_irq,
			    cfg->uart_irq_trigger, cfg->uart_irq_polarity);
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		if (res->Data.ExtendedIrq.InterruptCount == 0)
			break;
		cfg->uart_irq = res->Data.ExtendedIrq.Interrupts[0];
		cfg->uart_have_irq = true;
		cfg->uart_irq_trigger = res->Data.ExtendedIrq.Triggering;
		cfg->uart_irq_polarity = res->Data.ExtendedIrq.Polarity;
		if (bootverbose)
			device_printf(ctx->dev,
			    "_CRS: irq %ju trig %u pol %u (extended)\n",
			    (uintmax_t)cfg->uart_irq,
			    cfg->uart_irq_trigger, cfg->uart_irq_polarity);
		break;
	case ACPI_RESOURCE_TYPE_GENERIC_REGISTER:
		if (ctx->have_io ||
		    res->Data.GenericReg.SpaceId !=
		    ACPI_ADR_SPACE_SYSTEM_IO)
			break;
		cfg->uart_iobase = res->Data.GenericReg.Address;
		cfg->uart_nports = res->Data.GenericReg.BitWidth / 8;
		ctx->have_io = true;
		if (bootverbose)
			device_printf(ctx->dev,
			    "_CRS: generic reg %#jx (%u bits), SystemIo\n",
			    (uintmax_t)res->Data.GenericReg.Address,
			    res->Data.GenericReg.BitWidth);
		break;
	default:
		if (bootverbose)
			device_printf(ctx->dev,
			    "_CRS: resource type %u\n", res->Type);
		break;
	}
	return (AE_OK);
}

/*
 * Take the register window and interrupt of the UART controller named by
 * the serial bus descriptor's resource source.  On this firmware the
 * controller (e.g. \_SB.PCI0.UA00) has no _CRS of its own: it is a PCI
 * function whose window is a BAR, matching the Intel LPSS layout that the
 * reference OS drives via intel-lpss + 8250_dw (reg-io-width 4, reg-shift
 * 2, 16550-compatible registers, 100 MHz reference clock).
 */
/*
 * Reference clock feeding the LPSS additional registers, by PCI device id.
 * The Sunrisepoint generation and its Ice Lake and Cannon Lake successors
 * run it at 120 MHz, the rest since Bay Trail at 100 MHz - the same split
 * the reference driver draws from its per platform table (120 MHz for the
 * Sunrisepoint UART entries, 100 MHz for the Bay Trail ones).
 */
static uint32_t
surface_sam_lpss_freq(uint16_t device)
{
	static const uint16_t mhz120[] = {
		0x02a8, 0x02a9, 0x02c7,	/* Cannon Lake LP */
		0x06a8, 0x06a9, 0x06c7,	/* Cannon Lake LP */
		0x34a8, 0x34a9, 0x34c7,	/* Ice Lake LP */
		0x9d27, 0x9d28, 0x9d66,	/* Sunrisepoint LP */
	};
	u_int i;

	for (i = 0; i < nitems(mhz120); i++)
		if (device == mhz120[i])
			return (120000000);
	return (100000000);
}

static int
surface_sam_crs_pci(device_t dev, ACPI_HANDLE handle, struct sam_crs_ctx *ctx)
{
	struct sam_uart_config *cfg;
	ACPI_BUFFER buf;
	ACPI_OBJECT *obj;
	ACPI_STATUS status;
	device_t pcidev;
	uint32_t adr, bar, barhi, barbase, irq, mem_base;
	int i, rid, mem_rid, zero_rid, func, error, pwr;

	cfg = ctx->cfg;
	mem_rid = -1;
	zero_rid = -1;
	mem_base = 0;

	memset(&buf, 0, sizeof(buf));
	buf.Length = ACPI_ALLOCATE_BUFFER;
	status = AcpiEvaluateObject(handle, "_ADR", NULL, &buf);
	if (ACPI_FAILURE(status) || buf.Pointer == NULL) {
		device_printf(dev, "%s: _ADR evaluation failed (0x%x)\n",
		    ctx->rsrc, status);
		return (ENXIO);
	}
	obj = buf.Pointer;
	if (obj->Type != ACPI_TYPE_INTEGER) {
		device_printf(dev, "%s: _ADR type %u is not an integer\n",
		    ctx->rsrc, obj->Type);
		AcpiOsFree(buf.Pointer);
		return (ENXIO);
	}
	adr = (uint32_t)obj->Integer.Value;
	AcpiOsFree(buf.Pointer);

	/*
	 * _SB.PCI0 is the firmware PCI root bridge, so bus 0.  The ACPI
	 * spec packs the function into bits 15:8, but this firmware uses
	 * the low byte (UA00 = 0x001E0000, UA01 = 0x001E0001); prefer
	 * the spec field and fall back when it is empty.
	 */
	if (((adr >> 8) & 0xff) != 0)
		func = (adr >> 8) & 0xff;
	else
		func = adr & 0xff;
	if (bootverbose)
		device_printf(dev, "%s: ADR %#x -> bus 0 slot %#x func %u\n",
		    ctx->rsrc, adr, (adr >> 16) & 0xff, func);
	pcidev = pci_find_bsf(0, (adr >> 16) & 0xff, func);
	if (pcidev == NULL) {
		device_printf(dev,
		    "no pci function for %s (ADR %#x: bus 0 slot %#x func %u)\n",
		    ctx->rsrc, adr, (adr >> 16) & 0xff, func);
		return (ENXIO);
	}

	/* The function may sit in D3 until someone powers it up. */
	pwr = pci_get_powerstate(pcidev);
	if (pwr != PCI_POWERSTATE_D0) {
		error = pci_set_powerstate(pcidev, PCI_POWERSTATE_D0);
		if (error != 0)
			device_printf(dev,
			    "%s: pci_set_powerstate(D0) failed (%d)\n",
			    ctx->rsrc, error);
		if (bootverbose || error != 0)
			device_printf(dev, "%s: power state %d -> %d\n",
			    ctx->rsrc, pwr, pci_get_powerstate(pcidev));
	}

	/*
	 * Prefer an I/O BAR (a plain 16550 window), else the first
	 * memory BAR with a base from the firmware.  A memory BAR that
	 * reads 0 was never assigned - keep it as a last resort, since
	 * pci_alloc_resource() sizes and assigns such a BAR when the
	 * window is requested.  64-bit BARs only work with a zero
	 * upper half.
	 */
	for (i = 0; i <= PCIR_MAX_BAR_0; i++) {
		rid = PCIR_BAR(i);
		bar = pci_read_config(pcidev, rid, 4);
		if (bar == 0xffffffff)
			continue;
		if (PCI_BAR_IO(bar)) {
			if ((bar & PCIM_BAR_IO_RESERVED) != 0)
				continue;
			cfg->uart_mem = false;
			cfg->uart_iobase = bar & PCIM_BAR_IO_BASE;
			cfg->uart_nports = 8;
			cfg->uart_pcidev = pcidev;
			cfg->uart_rid = rid;
			ctx->have_io = true;
			break;
		}
		if (!PCI_BAR_MEM(bar))
			continue;
		switch (bar & PCIM_BAR_MEM_TYPE) {
		case PCIM_BAR_MEM_32:
			barhi = 0;
			break;
		case PCIM_BAR_MEM_64:
			barhi = pci_read_config(pcidev, rid + 4, 4);
			if (barhi != 0)
				continue;	/* above 4G */
			break;
		default:
			continue;	/* legacy 1MB type */
		}
		barbase = bar & (uint32_t)PCIM_BAR_MEM_BASE;
		if (barbase != 0) {
			if (mem_rid < 0) {
				mem_rid = rid;
				mem_base = barbase;
			}
		} else if (zero_rid < 0)
			zero_rid = rid;
	}
	if (!ctx->have_io && (mem_rid >= 0 || zero_rid >= 0)) {
		cfg->uart_mem = true;
		cfg->uart_iobase = mem_base;
		cfg->uart_nports = 0;	/* sized by the window allocator */
		cfg->uart_pcidev = pcidev;
		cfg->uart_rid = mem_rid >= 0 ? mem_rid : zero_rid;
		ctx->have_io = true;
		if (mem_rid < 0)
			device_printf(dev,
			    "%s: BAR %d has no base; the bus will assign it\n",
			    ctx->rsrc, PCI_RID2BAR(cfg->uart_rid));
	}
	if (!ctx->have_io) {
		device_printf(dev,
		    "%s: %s: no usable BAR (pwr %d): "
		    "%#x %#x %#x %#x %#x %#x\n",
		    ctx->rsrc, device_get_nameunit(pcidev),
		    pci_get_powerstate(pcidev),
		    pci_read_config(pcidev, PCIR_BAR(0), 4),
		    pci_read_config(pcidev, PCIR_BAR(1), 4),
		    pci_read_config(pcidev, PCIR_BAR(2), 4),
		    pci_read_config(pcidev, PCIR_BAR(3), 4),
		    pci_read_config(pcidev, PCIR_BAR(4), 4),
		    pci_read_config(pcidev, PCIR_BAR(5), 4));
		return (ENXIO);
	}

	/*
	 * Do not take the interrupt from the interrupt line register: a
	 * firmware that routes this function through ACPI leaves that
	 * register empty and describes the line in the routing table of the
	 * parent bridge instead.  The PCI bus sorts that out when the
	 * driver asks the function for an interrupt.
	 */
	irq = pci_read_config(pcidev, PCIR_INTLINE, 1);
	if (bootverbose)
		device_printf(dev, "%s: intpin %u, intline %u\n", ctx->rsrc,
		    pci_read_config(pcidev, PCIR_INTPIN, 1), irq);
	cfg->uart_base_freq = surface_sam_lpss_freq(pci_get_device(pcidev));
	device_printf(dev, "%s: using BAR %d at 00:%02x.%u, reference clock "
	    "%u Hz\n", ctx->rsrc, PCI_RID2BAR(cfg->uart_rid),
	    (adr >> 16) & 0xff, func, cfg->uart_base_freq);
	return (0);
}

static void
surface_sam_follow_rsrc(device_t dev, ACPI_HANDLE handle,
    struct sam_crs_ctx *ctx)
{
	ACPI_HANDLE ctrl;
	ACPI_STATUS status;

	status = AcpiGetHandle(handle, ctx->rsrc, &ctrl);
	if (ACPI_FAILURE(status)) {
		device_printf(dev, "serial controller %s not found (0x%x)\n",
		    ctx->rsrc, status);
		return;
	}

	/* Some firmware gives the controller its own _CRS. */
	status = AcpiWalkResources(ctrl, "_CRS", surface_sam_crs_cb, ctx);
	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND)
		device_printf(dev, "%s: _CRS walk error 0x%x\n",
		    ctx->rsrc, status);
	if (ACPI_SUCCESS(status) && ctx->have_io)
		return;

	if (bootverbose)
		device_printf(dev, "%s: no window in _CRS, "
		    "trying its pci function\n", ctx->rsrc);
	if (surface_sam_crs_pci(dev, ctrl, ctx) != 0)
		device_printf(dev, "no usable UART window on %s\n",
		    ctx->rsrc);
}

static int
surface_sam_parse_crs(device_t dev, ACPI_HANDLE handle,
    struct sam_uart_config *cfg)
{
	struct sam_crs_ctx ctx;
	unsigned long rclk;
	ACPI_STATUS status;
	int flow;

	memset(cfg, 0, sizeof(*cfg));
	cfg->uart_irq_trigger = 0xff;
	cfg->uart_irq_polarity = 0xff;
	cfg->uart_lcr = 0x03;	/* 8 data bits, no parity, 1 stop bit */

	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = cfg;
	ctx.dev = dev;

	status = AcpiWalkResources(handle, "_CRS", surface_sam_crs_cb, &ctx);
	if (ACPI_FAILURE(status)) {
		device_printf(dev, "_CRS walk failed (0x%x)\n", status);
		return (ENOENT);
	}
	if (!ctx.have_uart)
		device_printf(dev,
		    "_CRS has no UART serial bus descriptor, "
		    "using defaults\n");

	/*
	 * MSHW0084 carries no I/O resource of its own: its _CRS only
	 * names the UART controller behind which the port lives.  Follow
	 * that resource source to the actual window.
	 */
	if (!ctx.have_io && ctx.have_rsrc) {
		if (bootverbose)
			device_printf(dev, "following resource source %s\n",
			    ctx.rsrc);
		surface_sam_follow_rsrc(dev, handle, &ctx);
	}

	/* The reference clock is not part of _CRS; allow an override. */
	rclk = 0;
	(void)TUNABLE_ULONG_FETCH("hw.surface_sam.rclk", &rclk);
	if (rclk > 0 && rclk <= UINT_MAX) {
		cfg->uart_rclk = (uint32_t)rclk;
		device_printf(dev, "rclk override %u Hz\n", cfg->uart_rclk);
	}

	/*
	 * _CRS asks for RTS/CTS flow control.  The reference stack leaves
	 * it disabled on this port, and a transmitter that never sees CTS
	 * asserted would stall, so it stays off unless asked for.
	 */
	flow = 0;
	(void)TUNABLE_INT_FETCH("hw.surface_sam.hwflow", &flow);
	if (cfg->uart_hwflow && flow == 0)
		device_printf(dev, "_CRS asks for hw flow control, "
		    "leaving it off (hw.surface_sam.hwflow=1 to enable)\n");
	cfg->uart_hwflow = flow != 0;

	/*
	 * The reference clock is not part of _CRS.  For a window in a PCI
	 * function it comes out of the controller itself (the LPSS clock
	 * ratio register) once the UART layer has taken the device out of
	 * reset, so leave it alone here; only hw.surface_sam.rclk overrides
	 * it.  The clock that ends up being used is printed once the port
	 * has been programmed.
	 */

	if (!ctx.have_io)
		return (ENXIO);
	if (cfg->uart_pcidev == NULL && cfg->uart_nports == 0)
		cfg->uart_nports = 8;
	if (bootverbose) {
		/*
		 * The window is not allocated and the function not probed
		 * yet, so report what was chosen rather than a base address
		 * and a name: the bus assigns the BAR when it is requested.
		 */
		if (cfg->uart_pcidev != NULL)
			device_printf(dev, "resolved window: %s BAR %d on the "
			    "uart function\n", cfg->uart_mem ? "memory" : "I/O",
			    PCI_RID2BAR(cfg->uart_rid));
		else
			device_printf(dev, "resolved window: I/O %#jx+%u\n",
			    (uintmax_t)cfg->uart_iobase, cfg->uart_nports);
		if (cfg->uart_pcidev != NULL)
			device_printf(dev, "resolved irq: from the pci bus "
			    "(trigger and polarity come from its routing)\n");
		else if (cfg->uart_have_irq)
			device_printf(dev,
			    "resolved irq: %ju (trig %u pol %u)\n",
			    (uintmax_t)cfg->uart_irq, cfg->uart_irq_trigger,
			    cfg->uart_irq_polarity);
		else
			device_printf(dev, "resolved irq: none in _CRS\n");
		device_printf(dev, "resolved line: baud %u lcr %#x "
		    "hwflow %d\n", cfg->uart_baud, cfg->uart_lcr,
		    cfg->uart_hwflow);
	}
	return (0);
}

static void
surface_sam_add_sysctls(struct surface_sam *sam)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree, *stats;

	ctx = device_get_sysctl_ctx(sam->sam_dev);
	tree = device_get_sysctl_tree(sam->sam_dev);
	if (ctx == NULL || tree == NULL)
		return;

	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
	    "firmware_version", CTLFLAG_RD, &sam->sam_fw_version, 0,
	    "SAM firmware version (raw)");

	stats = SYSCTL_ADD_NODE(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
	    "stats", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "Surface Serial Hub transport statistics");
	if (stats == NULL)
		return;
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "tx_frames",
	    CTLFLAG_RD, &sam->sam_st_tx, "frames transmitted (ACKed)");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "rx_frames",
	    CTLFLAG_RD, &sam->sam_st_rx, "data frames received");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "rx_dup",
	    CTLFLAG_RD, &sam->sam_st_dup, "duplicate frames suppressed");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "rx_overrun",
	    CTLFLAG_RD, &sam->sam_st_rx_overrun,
	    "received bytes dropped (EC outran the driver)");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "crc_errors",
	    CTLFLAG_RD, &sam->sam_st_crc_err, "CRC validation failures");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "bad_frames",
	    CTLFLAG_RD, &sam->sam_st_bad, "malformed frames");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO,
	    "unexpected", CTLFLAG_RD, &sam->sam_st_unexpected,
	    "unexpected responses/acks");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "events",
	    CTLFLAG_RD, &sam->sam_st_events, "events dispatched");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO,
	    "events_unhandled", CTLFLAG_RD, &sam->sam_st_unhandled,
	    "events with no registered handler");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO,
	    "events_dropped", CTLFLAG_RD, &sam->sam_st_dropped,
	    "events dropped (out of memory)");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO,
	    "ack_timeouts", CTLFLAG_RD, &sam->sam_st_ack_timeout,
	    "transmissions not acknowledged by the EC");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "naks",
	    CTLFLAG_RD, &sam->sam_st_nak, "negative acknowledgements");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "timeouts",
	    CTLFLAG_RD, &sam->sam_st_timeout, "requests without response");
	SYSCTL_ADD_UQUAD(ctx, SYSCTL_CHILDREN(stats), OID_AUTO, "tx_errors",
	    CTLFLAG_RD, &sam->sam_st_tx_err, "UART transmit failures");
}

static int
surface_sam_probe(device_t dev)
{
	int rv;

	if (acpi_disabled("surface_sam") || device_get_unit(dev) != 0)
		return (ENXIO);

	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, surface_sam_ids,
	    NULL);
	if (rv > 0)
		return (rv);

	device_set_desc(dev, "Microsoft Surface Serial Hub (SAM)");
	/*
	 * Out-rank uart(4) (BUS_PROBE_DEFAULT) so that this driver owns
	 * the SAM serial device instead of exposing it as a tty.
	 */
	if (rv == BUS_PROBE_DEFAULT)
		return (BUS_PROBE_DEFAULT + 1);
	return (rv);
}

static int
surface_sam_attach(device_t dev)
{
	struct sam_uart_config cfg;
	struct sam_uart_ops ops;
	struct surface_sam *sam;
	struct surface_sam_hid_node node;
	uint32_t response[2];
	size_t rlen;
	uint32_t version;
	int error, i, intpin;

	sam = device_get_softc(dev);
	mtx_init(&sam->sam_mtx, "surface_sam", NULL, MTX_DEF);
	mtx_init(&sam->sam_rxq_mtx, "surface_sam_rxq", NULL, MTX_DEF);
	sam->sam_dev = dev;
	TAILQ_INIT(&sam->sam_evq);
	TASK_INIT(&sam->sam_ev_task, 0, surface_sam_event_task, sam);
	TASK_INIT(&sam->sam_rx_task, 0, surface_sam_rx_task, sam);
	memset(sam->sam_rx_win, 0xff, sizeof(sam->sam_rx_win));
	sam->sam_rx_state = SAM_RX_SYN1;
	sam->sam_ack_state = SAM_ACK_NONE;

	sam->sam_tq = taskqueue_create("surface_sam", M_WAITOK,
	    taskqueue_thread_enqueue, &sam->sam_tq);
	if (sam->sam_tq == NULL) {
		error = ENOMEM;
		goto fail_mtx;
	}
	taskqueue_start_threads(&sam->sam_tq, 1, PWAIT, "surface_sam%d",
	    device_get_unit(dev));

	sam->sam_handle = acpi_get_handle(dev);
	if (sam->sam_handle == NULL) {
		device_printf(dev, "no ACPI handle\n");
		error = ENXIO;
		goto fail_tq;
	}

	error = surface_sam_parse_crs(dev, sam->sam_handle, &cfg);
	if (error != 0) {
		device_printf(dev,
		    "no usable UART resources (error %d)\n", error);
		goto fail_tq;
	}

	ops.input = surface_sam_input;
	sam->sam_uart = sam_uart_attach(dev, &cfg, &ops, sam);
	if (sam->sam_uart == NULL) {
		error = ENXIO;
		if (cfg.uart_pcidev != NULL) {
			intpin = pci_get_intpin(cfg.uart_pcidev);
			if (intpin >= 1 && intpin <= 4)
				device_printf(dev, "if the firmware does "
				    "not route this interrupt, set "
				    "hw.pci%u.%u.%u.INT%c.irq to its GSI in "
				    "loader.conf\n",
				    pci_get_bus(cfg.uart_pcidev),
				    pci_get_slot(cfg.uart_pcidev),
				    pci_get_function(cfg.uart_pcidev),
				    'A' + intpin - 1);
		}
		goto fail_tq;
	}

	surface_sam_sc = sam;

	/*
	 * Initial SAM requests: log the firmware version and notify the
	 * EC that the host is up, mirroring the reference driver.
	 */
	rlen = sizeof(version);
	error = surface_sam_request(sam, SSH_TC_SAM, SSH_TID_SAM, 0x00, 0x13,
	    NULL, 0, &version, &rlen, 2000);
	if (error != 0) {
		device_printf(dev,
		    "failed to get SAM firmware version (error %d)\n",
		    error);
		error = ENXIO;
		goto fail_uart;
	}
	sam->sam_fw_version = le32toh(version);
	device_printf(dev, "SAM firmware version %u.%u.%u\n",
	    (sam->sam_fw_version >> 24) & 0xff,
	    (sam->sam_fw_version >> 8) & 0xffff,
	    sam->sam_fw_version & 0xff);

	rlen = sizeof(response);
	error = surface_sam_request(sam, SSH_TC_SAM, SSH_TID_SAM, 0x00, 0x34,
	    NULL, 0, response, &rlen, 0);
	if (error != 0)
		device_printf(dev,
		    "D0-entry notification failed (error %d), continuing\n",
		    error);

	rlen = sizeof(response);
	error = surface_sam_request(sam, SSH_TC_SAM, SSH_TID_SAM, 0x00, 0x16,
	    NULL, 0, response, &rlen, 0);
	if (error != 0)
		device_printf(dev,
		    "display-on notification failed (error %d), continuing\n",
		    error);

	surface_sam_add_sysctls(sam);

	/*
	 * Instantiate the HID-over-SAM nodes (surface_hid(4)).  A node the
	 * EC does not implement fails to attach; that is not fatal to the
	 * transport itself.
	 */
	for (i = 0; ; i++) {
		if (surface_sam_hid_node(i, &node) != 0)
			break;
		(void)device_add_child(dev, "surface_hid", i);
	}

	/*
	 * Instantiate the remaining EC clients: the performance
	 * profile, the monitoring sensors and the real time clock.  A
	 * function the firmware does not implement fails to attach;
	 * that is not fatal to the transport itself.
	 */
	(void)device_add_child(dev, "surface_profile", 0);
	(void)device_add_child(dev, "surface_mon", 0);
	(void)device_add_child(dev, "surface_rtc", 0);

	/* bus_attach_children() attaches what it can and ignores the rest. */
	bus_attach_children(dev);
	return (0);

fail_uart:
	surface_sam_sc = NULL;
	sam_uart_detach(sam->sam_uart);
	sam->sam_uart = NULL;
fail_tq:
	taskqueue_drain_all(sam->sam_tq);
	taskqueue_free(sam->sam_tq);
	sam->sam_tq = NULL;
fail_mtx:
	mtx_destroy(&sam->sam_rxq_mtx);
	mtx_destroy(&sam->sam_mtx);
	return (error);
}

static int
surface_sam_detach(device_t dev)
{
	struct surface_sam *sam;
	struct sam_event_item *item;
	int error;

	sam = device_get_softc(dev);

	/* Detach HID client children before tearing down the transport. */
	error = bus_generic_detach(dev);
	if (error != 0)
		return (error);

	if (surface_sam_sc == sam)
		surface_sam_sc = NULL;

	/* Stops the receive interrupt; no new items can appear. */
	if (sam->sam_uart != NULL) {
		sam_uart_detach(sam->sam_uart);
		sam->sam_uart = NULL;
	}

	if (sam->sam_tq != NULL) {
		taskqueue_drain_all(sam->sam_tq);
		mtx_lock(&sam->sam_mtx);
		while ((item = TAILQ_FIRST(&sam->sam_evq)) != NULL) {
			TAILQ_REMOVE(&sam->sam_evq, item, link);
			free(item, M_SURFACE_SAM);
		}
		mtx_unlock(&sam->sam_mtx);
		taskqueue_free(sam->sam_tq);
		sam->sam_tq = NULL;
	}

	mtx_destroy(&sam->sam_rxq_mtx);
	mtx_destroy(&sam->sam_mtx);
	return (0);
}
