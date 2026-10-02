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
 * Native FreeBSD driver for the Surface Serial Hub (SSH): the transport
 * between the host and the System Aggregator Module (SAM/SSAM) embedded
 * controller found in recent Microsoft Surface devices, including the
 * Surface Laptop 4.  Client drivers use the API below to send requests
 * to the EC and to receive events from it.
 */

#ifndef _SURFACE_SAM_H_
#define _SURFACE_SAM_H_

#include <sys/types.h>
#include <sys/malloc.h>

struct surface_sam;

MALLOC_DECLARE(M_SURFACE_SAM);

/*
 * SSH wire protocol.  A message is:
 *
 *	SYN(0x55AA, LE16) + frame + frameCRC + payload + payloadCRC
 *
 * where frame is { u8 type; le16 len; u8 seq; } and both CRCs are
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff) stored LE16.  The frame
 * CRC covers the 4 frame bytes, the payload CRC covers the payload
 * (including the command header for command frames).  All multi-byte
 * fields on the wire are little-endian.
 */
#define	SSH_MSG_SYN			0x55aa

#define	SSH_FRAME_TYPE_DATA_NSQ		0x00
#define	SSH_FRAME_TYPE_NAK		0x04
#define	SSH_FRAME_TYPE_ACK		0x40
#define	SSH_FRAME_TYPE_DATA_SEQ		0x80

#define	SSH_PAYLOAD_TYPE_CMD		0x80

#define	SSH_SYN_SIZE			2
#define	SSH_FRAME_SIZE			4
#define	SSH_CRC_SIZE			2
#define	SSH_CMD_SIZE			8
#define	SSH_MSG_OVERHEAD		(SSH_SYN_SIZE + SSH_FRAME_SIZE + \
					 2 * SSH_CRC_SIZE)

/* IDs 1..SSH_NUM_EVENTS (inclusive) are events, all others responses. */
#define	SSH_NUM_EVENTS			38

/* Source/target IDs on the serial hub. */
#define	SSH_TID_HOST			0x00
#define	SSH_TID_SAM			0x01
#define	SSH_TID_KIP			0x02

/* Target categories used by Surface Laptop 4 client drivers. */
#define	SSH_TC_SAM			0x01
#define	SSH_TC_BAT			0x02
#define	SSH_TC_TMP			0x03
#define	SSH_TC_FAN			0x05
#define	SSH_TC_KIP			0x0e
#define	SSH_TC_HID			0x15
#define	SSH_TC_BKL			0x17
#define	SSH_TC_POS			0x26

/* Maximum command payload accepted by the transport. */
#define	SSH_MAX_PAYLOAD			512

/*
 * An event as delivered to registered event handlers.  The data pointer
 * refers to driver-owned storage which is only valid for the duration
 * of the callback.
 */
struct surface_sam_event {
	uint8_t		ev_tc;		/* target category */
	uint8_t		ev_tid;		/* source target ID */
	uint8_t		ev_iid;		/* instance ID */
	uint8_t		ev_cid;		/* command ID */
	const uint8_t		*ev_data;	/* event payload */
	size_t		ev_len;
};

typedef void (*surface_sam_event_fn)(void *arg,
	    struct surface_sam_event *ev);

/*
 * Return the attached controller, or NULL if surface_sam(4) has not
 * attached yet.  Returned pointer stays valid while the driver is
 * attached; client drivers must only be attached while it is.
 */
struct surface_sam *surface_sam_get(void);

/*
 * Execute a synchronous request against the EC and wait for its
 * response.  On success *rlen is updated to the response length; the
 * caller sets *rlen to the capacity of rdata beforehand.  timeout_ms
 * <= 0 selects the default response timeout.  May be called from
 * process context only and not from an event handler registered via
 * surface_sam_register_event().
 */
int	surface_sam_request(struct surface_sam *sam, uint8_t tc,
	    uint8_t tid, uint8_t iid, uint8_t cid, const void *wdata,
	    size_t wlen, void *rdata, size_t *rlen, int timeout_ms);

/*
 * Register a handler for events of the given target category.  Only one
 * handler per category; registering over an existing one replaces it.
 */
int	surface_sam_register_event(struct surface_sam *sam, uint8_t tc,
	    surface_sam_event_fn fn, void *arg);

void	surface_sam_unregister_event(struct surface_sam *sam, uint8_t tc);

/*
 * A HID node reachable through the SAM transport.  Nodes are identified
 * by their unit number in the order given by the transport driver.
 */
struct surface_sam_hid_node {
	uint8_t	 hid_tid;	/* target ID (hub) */
	uint8_t	 hid_iid;	/* instance ID (node) */
};

/*
 * Look up the HID node for the given unit number.  Returns 0 on success
 * or ENOENT if the unit does not exist.
 */
int	surface_sam_hid_node(u_int unit, struct surface_sam_hid_node *node);

#endif /* _SURFACE_SAM_H_ */
