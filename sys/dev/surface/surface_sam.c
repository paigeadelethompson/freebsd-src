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

struct sam_crs_ctx {
	struct sam_uart_config	*cfg;
	bool			 have_io;
	bool			 have_uart;
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

/* Receive callback from the UART layer (interrupt/poll context). */
static void
surface_sam_input(void *arg, const uint8_t *buf, size_t len)
{
	struct surface_sam *sam;
	size_t i;

	sam = arg;
	mtx_lock(&sam->sam_mtx);
	for (i = 0; i < len; i++)
		surface_sam_rx_byte(sam, buf[i]);
	mtx_unlock(&sam->sam_mtx);
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
			error = msleep(&sam->sam_ack_state, &sam->sam_mtx,
			    PRIBIO, "samack",
			    surface_ms2ticks(SAM_ACK_TIMEOUT_MS));
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
		error = msleep(&sam->sam_rqst_have, &sam->sam_mtx, PRIBIO,
		    "samrsp", surface_ms2ticks(timeout));
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
		    ACPI_RESOURCE_SERIAL_TYPE_UART)
			break;
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
		ctx->have_uart = true;
		break;
	case ACPI_RESOURCE_TYPE_IO:
		cfg->uart_iobase = res->Data.Io.Minimum;
		cfg->uart_nports = res->Data.Io.AddressLength;
		ctx->have_io = true;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		cfg->uart_iobase = res->Data.FixedIo.Address;
		cfg->uart_nports = res->Data.FixedIo.AddressLength;
		ctx->have_io = true;
		break;
	case ACPI_RESOURCE_TYPE_IRQ:
		if (res->Data.Irq.InterruptCount == 0)
			break;
		cfg->uart_irq = res->Data.Irq.Interrupts[0];
		cfg->uart_have_irq = true;
		cfg->uart_irq_trigger = res->Data.Irq.Triggering;
		cfg->uart_irq_polarity = res->Data.Irq.Polarity;
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		if (res->Data.ExtendedIrq.InterruptCount == 0)
			break;
		cfg->uart_irq = res->Data.ExtendedIrq.Interrupts[0];
		cfg->uart_have_irq = true;
		cfg->uart_irq_trigger = res->Data.ExtendedIrq.Triggering;
		cfg->uart_irq_polarity = res->Data.ExtendedIrq.Polarity;
		break;
	case ACPI_RESOURCE_TYPE_GENERIC_REGISTER:
		if (ctx->have_io ||
		    res->Data.GenericReg.SpaceId !=
		    ACPI_ADR_SPACE_SYSTEM_IO)
			break;
		cfg->uart_iobase = res->Data.GenericReg.Address;
		cfg->uart_nports = res->Data.GenericReg.BitWidth / 8;
		ctx->have_io = true;
		break;
	default:
		break;
	}
	return (AE_OK);
}

static int
surface_sam_parse_crs(ACPI_HANDLE handle, struct sam_uart_config *cfg)
{
	struct sam_crs_ctx ctx;
	unsigned long rclk;
	ACPI_STATUS status;

	memset(cfg, 0, sizeof(*cfg));
	cfg->uart_irq_trigger = 0xff;
	cfg->uart_irq_polarity = 0xff;
	cfg->uart_lcr = 0x03;	/* 8 data bits, no parity, 1 stop bit */

	ctx.cfg = cfg;
	ctx.have_io = false;
	ctx.have_uart = false;

	status = AcpiWalkResources(handle, "_CRS", surface_sam_crs_cb, &ctx);
	if (ACPI_FAILURE(status))
		return (ENOENT);

	/* The reference clock is not part of _CRS; allow an override. */
	rclk = 0;
	(void)TUNABLE_ULONG_FETCH("hw.surface_sam.rclk", &rclk);
	if (rclk > 0 && rclk <= UINT_MAX)
		cfg->uart_rclk = (uint32_t)rclk;

	if (!ctx.have_io)
		return (ENXIO);
	if (cfg->uart_nports == 0)
		cfg->uart_nports = 8;
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
	int error, i;

	sam = device_get_softc(dev);
	mtx_init(&sam->sam_mtx, "surface_sam", NULL, MTX_DEF);
	sam->sam_dev = dev;
	TAILQ_INIT(&sam->sam_evq);
	TASK_INIT(&sam->sam_ev_task, 0, surface_sam_event_task, sam);
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

	error = surface_sam_parse_crs(sam->sam_handle, &cfg);
	if (error != 0) {
		device_printf(dev,
		    "no usable UART resources in _CRS (error %d)\n", error);
		goto fail_tq;
	}

	ops.input = surface_sam_input;
	sam->sam_uart = sam_uart_attach(dev, &cfg, &ops, sam);
	if (sam->sam_uart == NULL) {
		error = ENXIO;
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

	/* Stops receive interrupts/polling; no new items can appear. */
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

	mtx_destroy(&sam->sam_mtx);
	return (0);
}
