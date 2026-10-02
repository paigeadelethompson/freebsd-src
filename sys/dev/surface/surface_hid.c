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
 * OR LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

/*
 * HID transport for the Surface Serial Hub (SAM): exposes the keyboard,
 * touchpad and auxiliary HID nodes of the System Aggregator Module as
 * hidbus(4) children, mirroring the reference linux-surface surface-hid
 * driver.
 *
 * One child device is created per HID node (see surface_sam_hid_node()).
 * Each child fetches its descriptors from the EC, attaches a hidbus
 * device and translates the HID report interface onto SAM requests.
 * Input reports arrive as asynchronous SAM events on target category
 * SSH_TC_HID and are demultiplexed by instance ID to the owning child.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>

#include <dev/evdev/input.h>
#include <dev/hid/hid.h>
#include <dev/hid/hidbus.h>

#include <dev/surface/surface_sam.h>

#include "hid_if.h"

/* Command IDs on the HID target category (TC 0x15). */
#define	SURFACE_HID_CID_OUTPUT_REPORT		0x01
#define	SURFACE_HID_CID_GET_FEATURE_REPORT	0x02
#define	SURFACE_HID_CID_SET_FEATURE_REPORT	0x03
#define	SURFACE_HID_CID_GET_DESCRIPTOR		0x04

/* Command IDs of the extended event registry (TC 0x21). */
#define	SURFACE_HID_CID_EVENT_ENABLE		0x01
#define	SURFACE_HID_CID_EVENT_DISABLE		0x02

/* Descriptor buffer entries fetched via SURFACE_HID_CID_GET_DESCRIPTOR. */
#define	SURFACE_HID_ENTRY_HID_DESC		0x00
#define	SURFACE_HID_ENTRY_REPORT_DESC		0x01
#define	SURFACE_HID_ENTRY_ATTRIBUTES		0x02

/* Extended event registry target category. */
#define	SURFACE_HID_TC_REG			0x21

/* Buffer slice used to transfer descriptor entries, see slice header. */
#define	SURFACE_HID_SLICE_SIZE			10
#define	SURFACE_HID_SLICE_CHUNK			0x76

/* HID descriptor types as stored in the HID descriptor entry. */
#define	SURFACE_HID_DT_HID			0x21
#define	SURFACE_HID_DT_REPORT			0x22

#define	SURFACE_HID_DESC_SIZE			9
#define	SURFACE_HID_ATTRS_SIZE			32
#define	SURFACE_HID_RDESC_MAX			4096

/* Instance IDs of the HID nodes on the KIP hub. */
#define	SURFACE_HID_IID_KEYBOARD		0x01
#define	SURFACE_HID_IID_TOUCHPAD		0x03
#define	SURFACE_HID_IID_AUX			0x05

/*
 * The EC firmware reports the function key as a button in the button
 * page, which is not used by any other part of the Surface input
 * hardware.  It is a modifier handled by the firmware itself (volume,
 * brightness, ...) and gets stuck when left alone, so it is masked out
 * of the reports before they are handed to hidbus(4).
 */
#define	SURFACE_HID_FN_USAGE			0x0100
#define	SURFACE_HID_FN_TLC			4
#define	SURFACE_HID_FN_MAX			4

/* Largest input report the function key filter is willing to copy. */
#define	SURFACE_HID_REPORT_MAX			1024

/*
 * HID descriptor entry (entry 0).  Multi-byte fields are little-endian.
 */
struct surface_hid_descriptor {
	uint8_t	 d_desc_len;		/* = SURFACE_HID_DESC_SIZE */
	uint8_t	 d_desc_type;		/* = SURFACE_HID_DT_HID */
	uint16_t d_hid_version;
	uint8_t	 d_country_code;
	uint8_t	 d_num_descriptors;	/* = 1 */
	uint8_t	 d_report_desc_type;	/* = SURFACE_HID_DT_REPORT */
	uint16_t d_report_desc_len;
} __packed;

/*
 * Device attributes entry (entry 2).  Multi-byte fields are little-endian.
 */
struct surface_hid_attributes {
	uint32_t a_length;		/* = SURFACE_HID_ATTRS_SIZE */
	uint16_t a_vendor;
	uint16_t a_product;
	uint16_t a_version;
	uint8_t	 a_reserved[22];
} __packed;

/*
 * Buffer slice header exchanged with the EC.  The request carries the
 * entry to fetch plus the offset/length window the host can accept; the
 * response repeats the header (updated by the EC) followed by data.
 */
struct surface_hid_slice {
	uint8_t	 s_entry;
	uint32_t s_offset;
	uint32_t s_length;
	uint8_t	 s_end;
} __packed;

CTASSERT(sizeof(struct surface_hid_descriptor) == SURFACE_HID_DESC_SIZE);
CTASSERT(sizeof(struct surface_hid_attributes) == SURFACE_HID_ATTRS_SIZE);
CTASSERT(sizeof(struct surface_hid_slice) == SURFACE_HID_SLICE_SIZE);

struct surface_hid_softc {
	device_t		 sh_dev;
	struct surface_sam	*sh_sam;
	uint8_t			 sh_tid;
	uint8_t			 sh_iid;
	struct hid_device_info	 sh_hw;
	struct surface_hid_descriptor sh_hiddesc;
	uint8_t			*sh_rdesc;
	hid_size_t		 sh_rdesclen;
	hid_intr_t		*sh_intr;
	void			*sh_intr_ctx;
	bool			 sh_events;
	hid_size_t		 sh_isize;
	uint8_t			 sh_reportid;
	uint8_t			 sh_nfn;
	struct hid_location	 sh_fn[SURFACE_HID_FN_MAX];
	uint8_t			 sh_fnid[SURFACE_HID_FN_MAX];
	uint8_t			*sh_report;
	hid_size_t		 sh_reportlen;
};

/*
 * Demultiplexer for SSH_TC_HID input events: one handler is registered
 * with the transport for the whole target category and events are
 * routed to their node by instance ID.
 */
static struct mtx surface_hid_mtx;
MTX_SYSINIT(surface_hid, &surface_hid_mtx, "surface hid demux", MTX_DEF);

static struct surface_hid_softc *surface_hid_by_iid[256];
static u_int surface_hid_handlers;

/*
 * Clear the function key bit of an input report.  hid_location(9)
 * positions are bit offsets into the report data behind the report ID
 * byte, which has to be accounted for.
 */
static void
surface_hid_mask_fn(struct surface_hid_softc *sc, uint8_t *data, size_t len)
{
	uint32_t bit, n, pos;
	uint8_t id;
	int i;

	id = 0;
	if (sc->sh_reportid != 0) {
		if (len < 1)
			return;
		id = data[0];
		data++;
		len--;
	}

	for (i = 0; i < sc->sh_nfn; i++) {
		if (sc->sh_fnid[i] != id)
			continue;
		n = sc->sh_fn[i].size * sc->sh_fn[i].count;
		if (n == 0)
			continue;
		pos = sc->sh_fn[i].pos;
		for (bit = 0; bit < n; bit++) {
			if ((pos + bit) / 8 >= len)
				break;
			data[(pos + bit) / 8] &= ~(1U << ((pos + bit) % 8));
		}
	}
}

/* Hand an input report to the child driver, masking the function key. */
static void
surface_hid_deliver(struct surface_hid_softc *sc, const uint8_t *data,
    size_t len)
{

	if (sc->sh_nfn == 0 || sc->sh_report == NULL ||
	    len > sc->sh_reportlen) {
		sc->sh_intr(sc->sh_intr_ctx, __DECONST(void *, data),
		    (hid_size_t)len);
		return;
	}

	memcpy(sc->sh_report, data, len);
	surface_hid_mask_fn(sc, sc->sh_report, len);
	sc->sh_intr(sc->sh_intr_ctx, sc->sh_report, (hid_size_t)len);
}

static void
surface_hid_event(void *arg, struct surface_sam_event *ev)
{
	struct surface_hid_softc *sc;

	(void)arg;

	/* Input reports are the only event command ID. */
	if (ev->ev_cid != 0x00)
		return;

	mtx_lock(&surface_hid_mtx);
	sc = surface_hid_by_iid[ev->ev_iid];
	if (sc != NULL && sc->sh_intr != NULL)
		surface_hid_deliver(sc, ev->ev_data, ev->ev_len);
	mtx_unlock(&surface_hid_mtx);
}

/*
 * Fetch a descriptor entry from the EC.  Entries are transferred in
 * slices of at most SURFACE_HID_SLICE_CHUNK data bytes; the EC updates
 * the slice header with the offset and length of the returned data.
 */
static int
surface_hid_get_entry(struct surface_hid_softc *sc, uint8_t entry,
    void *buf, size_t len)
{
	uint8_t req[SURFACE_HID_SLICE_SIZE];
	uint8_t rsp[SURFACE_HID_SLICE_SIZE + SURFACE_HID_SLICE_CHUNK];
	struct surface_hid_slice *rs;
	size_t rlen, offset, length, avail;
	int error;

	rs = (struct surface_hid_slice *)(void *)rsp;
	memset(req, 0, sizeof(req));
	memset(rsp, 0, sizeof(rsp));
	req[0] = entry;
	rs->s_entry = entry;
	rs->s_end = 0;

	offset = 0;
	length = SURFACE_HID_SLICE_CHUNK;
	while (!rs->s_end && offset < len) {
		le32enc(req + 1, offset);
		le32enc(req + 5, length);

		rlen = sizeof(rsp);
		error = surface_sam_request(sc->sh_sam, SSH_TC_HID,
		    sc->sh_tid, sc->sh_iid, SURFACE_HID_CID_GET_DESCRIPTOR,
		    req, sizeof(req), rsp, &rlen, 0);
		if (error != 0)
			return (error);
		if (rlen < SURFACE_HID_SLICE_SIZE)
			return (EIO);
		if (rs->s_entry != entry)
			return (EPROTO);

		offset = le32dec(rsp + 1);
		length = le32dec(rsp + 5);
		if (length > SURFACE_HID_SLICE_CHUNK || offset > len)
			return (EPROTO);
		avail = rlen - SURFACE_HID_SLICE_SIZE;
		if (length > avail)
			return (EPROTO);
		if (offset + length > len)
			length = len - offset;

		memcpy((uint8_t *)buf + offset, rsp + SURFACE_HID_SLICE_SIZE,
		    length);
		offset += length;
		if (length == 0 && !rs->s_end && offset < len)
			return (EPROTO);
		length = SURFACE_HID_SLICE_CHUNK;
	}
	if (offset != len)
		return (EPROTO);
	return (0);
}

static int
surface_hid_load_hid_descriptor(struct surface_hid_softc *sc)
{
	struct surface_hid_descriptor *d = &sc->sh_hiddesc;
	int error;

	error = surface_hid_get_entry(sc, SURFACE_HID_ENTRY_HID_DESC, d,
	    sizeof(*d));
	if (error != 0)
		return (error);
	if (d->d_desc_len != sizeof(*d) ||
	    d->d_desc_type != SURFACE_HID_DT_HID ||
	    d->d_num_descriptors != 1 ||
	    d->d_report_desc_type != SURFACE_HID_DT_REPORT)
		return (EPROTO);
	return (0);
}

static int
surface_hid_load_attributes(struct surface_hid_softc *sc)
{
	struct surface_hid_attributes attrs;
	uint32_t length;
	int error;

	error = surface_hid_get_entry(sc, SURFACE_HID_ENTRY_ATTRIBUTES, &attrs,
	    sizeof(attrs));
	if (error != 0)
		return (error);
	length = le32toh(attrs.a_length);
	if (length != sizeof(attrs))
		return (EPROTO);

	sc->sh_hw.idBus = BUS_HOST;
	sc->sh_hw.idVendor = le16toh(attrs.a_vendor);
	sc->sh_hw.idProduct = le16toh(attrs.a_product);
	sc->sh_hw.idVersion = le16toh(sc->sh_hiddesc.d_hid_version);
	length = le16toh(sc->sh_hiddesc.d_report_desc_len);
	if (length == 0 || length > SURFACE_HID_RDESC_MAX)
		return (EPROTO);
	sc->sh_hw.rdescsize = length;
	snprintf(sc->sh_hw.name, sizeof(sc->sh_hw.name),
	    "Microsoft Surface %04x:%04x", sc->sh_hw.idVendor,
	    sc->sh_hw.idProduct);
	return (0);
}

static int
surface_hid_load_report_descriptor(struct surface_hid_softc *sc)
{
	hid_size_t len;
	uint8_t *buf;
	int error;

	len = sc->sh_hw.rdescsize;
	buf = malloc(len, M_SURFACE_SAM, M_WAITOK | M_ZERO);
	error = surface_hid_get_entry(sc, SURFACE_HID_ENTRY_REPORT_DESC, buf,
	    len);
	if (error != 0) {
		free(buf, M_SURFACE_SAM);
		return (error);
	}
	sc->sh_rdesc = buf;
	sc->sh_rdesclen = len;
	return (0);
}

/*
 * Locate the function key in the report descriptor so that it can be
 * masked from the input reports.  The touchpad is left alone: its
 * buttons are real input.
 */
static void
surface_hid_setup_fn_filter(struct surface_hid_softc *sc)
{
	struct hid_location loc;
	uint32_t flags;
	uint8_t id, tlc;
	int isize;

	/*
	 * Size the input reports to know whether they carry a report ID
	 * and to have a buffer to filter them in.
	 */
	isize = hid_report_size_max(sc->sh_rdesc, sc->sh_rdesclen, hid_input,
	    &sc->sh_reportid);
	if (isize <= 0 || isize > SURFACE_HID_REPORT_MAX)
		return;
	sc->sh_isize = isize;
	sc->sh_reportlen = sc->sh_isize + sizeof(sc->sh_reportid);
	sc->sh_report = malloc(sc->sh_reportlen, M_SURFACE_SAM,
	    M_WAITOK | M_ZERO);
	if (sc->sh_report == NULL)
		return;

	if (sc->sh_iid == SURFACE_HID_IID_TOUCHPAD)
		return;

	for (tlc = 0; tlc < SURFACE_HID_FN_TLC &&
	    sc->sh_nfn < SURFACE_HID_FN_MAX; tlc++) {
		if (!hidbus_locate(sc->sh_rdesc, sc->sh_rdesclen,
		    HID_USAGE2(HUP_BUTTON, SURFACE_HID_FN_USAGE), hid_input,
		    tlc, 0, &loc, &flags, &id, NULL))
			continue;
		if (loc.size == 0 || loc.count == 0)
			continue;
		sc->sh_fn[sc->sh_nfn] = loc;
		sc->sh_fnid[sc->sh_nfn] = id;
		sc->sh_nfn++;
	}

	if (sc->sh_nfn != 0)
		device_printf(sc->sh_dev,
		    "filtering the function key from node instance %u\n",
		    sc->sh_iid);
}

/*
 * Send a raw output or feature report.  The first payload byte is the
 * report ID; the remaining bytes are the caller's buffer.
 */
static int
surface_hid_set_raw_report(struct surface_hid_softc *sc, uint8_t cid,
    const void *data, hid_size_t len)
{
	uint8_t *payload;
	size_t rlen;
	int error;

	if (len == 0)
		return (EINVAL);
	if (len > SSH_MAX_PAYLOAD)
		return (EMSGSIZE);

	payload = malloc(len, M_SURFACE_SAM, M_WAITOK);
	memcpy(payload, data, len);
	rlen = 0;
	error = surface_sam_request(sc->sh_sam, SSH_TC_HID, sc->sh_tid,
	    sc->sh_iid, cid, payload, len, NULL, &rlen, 0);
	free(payload, M_SURFACE_SAM);
	return (error);
}

/*
 * Fetch a raw feature report.  The request payload is the report ID
 * only; the response is the report data.
 */
static int
surface_hid_get_raw_report(struct surface_hid_softc *sc, uint8_t id,
    void *data, hid_size_t maxlen, hid_size_t *actlen)
{
	uint8_t rid;
	size_t rlen;
	int error;

	if (maxlen == 0)
		return (EINVAL);
	if (maxlen > SSH_MAX_PAYLOAD)
		return (EMSGSIZE);

	rid = id;
	rlen = maxlen;
	error = surface_sam_request(sc->sh_sam, SSH_TC_HID, sc->sh_tid,
	    sc->sh_iid, SURFACE_HID_CID_GET_FEATURE_REPORT, &rid, sizeof(rid),
	    data, &rlen, 0);
	if (error != 0)
		return (error);
	*actlen = rlen;
	return (0);
}

/*
 * Enable or disable input events for this node via the extended event
 * registry (TC 0x21).  The request ID used by the EC for the resulting
 * events is the target category itself.
 */
static int
surface_hid_set_events(struct surface_hid_softc *sc, bool enable)
{
	uint8_t params[5];
	uint8_t status;
	size_t rlen;
	int error;

	params[0] = SSH_TC_HID;
	params[1] = 0;
	le16enc(params + 2, SSH_TC_HID);
	params[4] = sc->sh_iid;

	status = 0;
	rlen = sizeof(status);
	error = surface_sam_request(sc->sh_sam, SURFACE_HID_TC_REG,
	    sc->sh_tid, 0x00, enable ? SURFACE_HID_CID_EVENT_ENABLE :
	    SURFACE_HID_CID_EVENT_DISABLE, params, sizeof(params), &status,
	    &rlen, 0);
	if (error != 0)
		return (error);
	if (status != 0)
		return (EIO);
	return (0);
}

static int
surface_hid_probe(device_t dev)
{
	struct surface_sam_hid_node node;

	if (surface_sam_hid_node(device_get_unit(dev), &node) != 0)
		return (ENXIO);
	return (BUS_PROBE_DEFAULT);
}

static int
surface_hid_attach(device_t dev)
{
	struct surface_hid_softc *sc = device_get_softc(dev);
	struct surface_sam_hid_node node;
	device_t child;
	bool register_fn;
	int error;

	sc->sh_dev = dev;
	error = surface_sam_hid_node(device_get_unit(dev), &node);
	if (error != 0)
		return (error);
	sc->sh_tid = node.hid_tid;
	sc->sh_iid = node.hid_iid;

	sc->sh_sam = surface_sam_get();
	if (sc->sh_sam == NULL)
		return (ENXIO);

	error = surface_hid_load_hid_descriptor(sc);
	if (error != 0) {
		device_printf(dev, "no HID descriptor (error %d)\n", error);
		return (ENXIO);
	}
	error = surface_hid_load_attributes(sc);
	if (error != 0) {
		device_printf(dev, "no device attributes (error %d)\n", error);
		return (ENXIO);
	}
	error = surface_hid_load_report_descriptor(sc);
	if (error != 0) {
		device_printf(dev, "no report descriptor (error %d)\n", error);
		return (ENXIO);
	}
	surface_hid_setup_fn_filter(sc);

	/*
	 * Register the category-wide event handler once for all nodes;
	 * events are routed by instance ID.  Do this before attaching
	 * hidbus since its children may enable events immediately.
	 */
	mtx_lock(&surface_hid_mtx);
	surface_hid_by_iid[sc->sh_iid] = sc;
	register_fn = (surface_hid_handlers++ == 0);
	mtx_unlock(&surface_hid_mtx);
	if (register_fn) {
		error = surface_sam_register_event(sc->sh_sam, SSH_TC_HID,
		    surface_hid_event, NULL);
		if (error != 0)
			goto fail_node;
	}

	child = device_add_child(dev, "hidbus", DEVICE_UNIT_ANY);
	if (child == NULL) {
		error = ENOMEM;
		goto fail_event;
	}
	device_set_ivars(child, &sc->sh_hw);

	/* bus_attach_children() attaches what it can and ignores the rest. */
	bus_attach_children(dev);

	device_printf(dev, "Surface HID %04x:%04x, node instance %u\n",
	    sc->sh_hw.idVendor, sc->sh_hw.idProduct, sc->sh_iid);
	return (0);

fail_event:
	if (register_fn)
		surface_sam_unregister_event(sc->sh_sam, SSH_TC_HID);
fail_node:
	mtx_lock(&surface_hid_mtx);
	surface_hid_by_iid[sc->sh_iid] = NULL;
	surface_hid_handlers--;
	mtx_unlock(&surface_hid_mtx);
	free(sc->sh_rdesc, M_SURFACE_SAM);
	sc->sh_rdesc = NULL;
	sc->sh_rdesclen = 0;
	free(sc->sh_report, M_SURFACE_SAM);
	sc->sh_report = NULL;
	sc->sh_reportlen = 0;
	return (error);
}

static int
surface_hid_detach(device_t dev)
{
	struct surface_hid_softc *sc = device_get_softc(dev);
	bool unregister_fn;
	int error;

	/* Best effort: stop the EC from sending events for this node. */
	if (sc->sh_events) {
		(void)surface_hid_set_events(sc, false);
		sc->sh_events = false;
	}

	error = bus_generic_detach(dev);
	if (error != 0)
		return (error);

	mtx_lock(&surface_hid_mtx);
	if (surface_hid_by_iid[sc->sh_iid] == sc)
		surface_hid_by_iid[sc->sh_iid] = NULL;
	unregister_fn = (--surface_hid_handlers == 0);
	mtx_unlock(&surface_hid_mtx);
	if (unregister_fn)
		surface_sam_unregister_event(sc->sh_sam, SSH_TC_HID);

	free(sc->sh_rdesc, M_SURFACE_SAM);
	sc->sh_rdesc = NULL;
	sc->sh_rdesclen = 0;
	free(sc->sh_report, M_SURFACE_SAM);
	sc->sh_report = NULL;
	sc->sh_reportlen = 0;
	return (0);
}

/*
 * Interrupt interface.  Input reports are pushed by the SAM event
 * handler; there are no interrupt transfers to manage.
 */
static void
surface_hid_intr_setup(device_t dev, device_t child, hid_intr_t intr,
    void *context, struct hid_rdesc_info *rdesc)
{
	struct surface_hid_softc *sc = device_get_softc(dev);

	if (intr == NULL)
		return;

	mtx_lock(&surface_hid_mtx);
	sc->sh_intr = intr;
	sc->sh_intr_ctx = context;
	mtx_unlock(&surface_hid_mtx);

	rdesc->rdsize = rdesc->isize;
	rdesc->wrsize = rdesc->osize;
	rdesc->grsize = rdesc->fsize;
	rdesc->srsize = rdesc->fsize;
}

static void
surface_hid_intr_unsetup(device_t dev, device_t child)
{
	struct surface_hid_softc *sc = device_get_softc(dev);

	mtx_lock(&surface_hid_mtx);
	sc->sh_intr = NULL;
	sc->sh_intr_ctx = NULL;
	mtx_unlock(&surface_hid_mtx);
}

static int
surface_hid_intr_start(device_t dev, device_t child)
{
	struct surface_hid_softc *sc = device_get_softc(dev);
	int error;

	if (sc->sh_events)
		return (0);
	error = surface_hid_set_events(sc, true);
	if (error != 0) {
		device_printf(sc->sh_dev,
		    "failed to enable HID events (error %d)\n", error);
		return (error);
	}
	sc->sh_events = true;
	return (0);
}

static int
surface_hid_intr_stop(device_t dev, device_t child)
{
	struct surface_hid_softc *sc = device_get_softc(dev);
	int error;

	if (!sc->sh_events)
		return (0);
	error = surface_hid_set_events(sc, false);
	sc->sh_events = false;
	if (error != 0)
		device_printf(sc->sh_dev,
		    "failed to disable HID events (error %d)\n", error);
	return (0);
}

static void
surface_hid_intr_poll(device_t dev, device_t child)
{

	/*
	 * Input arrives as asynchronous events and SAM requests sleep,
	 * so reports cannot be polled from the panic context.  Do nothing.
	 */
}

/*
 * HID interface.
 */
static int
surface_hid_get_rdesc(device_t dev, device_t child, void *data, hid_size_t len)
{
	struct surface_hid_softc *sc = device_get_softc(dev);

	if (sc->sh_rdesc != NULL && len == sc->sh_rdesclen) {
		memcpy(data, sc->sh_rdesc, len);
		return (0);
	}
	return (surface_hid_get_entry(sc, SURFACE_HID_ENTRY_REPORT_DESC, data,
	    len));
}

static int
surface_hid_read(device_t dev, device_t child, void *data, hid_size_t maxlen,
    hid_size_t *actlen)
{

	return (ENOTSUP);
}

static int
surface_hid_write(device_t dev, device_t child, const void *data,
    hid_size_t len)
{
	struct surface_hid_softc *sc = device_get_softc(dev);

	return (surface_hid_set_raw_report(sc, SURFACE_HID_CID_OUTPUT_REPORT,
	    data, len));
}

static int
surface_hid_get_report(device_t dev, device_t child, void *data,
    hid_size_t maxlen, hid_size_t *actlen, uint8_t type, uint8_t id)
{
	struct surface_hid_softc *sc = device_get_softc(dev);

	if (type != HID_FEATURE_REPORT)
		return (ENOTSUP);
	return (surface_hid_get_raw_report(sc, id, data, maxlen, actlen));
}

static int
surface_hid_set_report(device_t dev, device_t child, const void *data,
    hid_size_t len, uint8_t type, uint8_t id)
{
	struct surface_hid_softc *sc = device_get_softc(dev);
	uint8_t cid;

	switch (type) {
	case HID_OUTPUT_REPORT:
		cid = SURFACE_HID_CID_OUTPUT_REPORT;
		break;
	case HID_FEATURE_REPORT:
		cid = SURFACE_HID_CID_SET_FEATURE_REPORT;
		break;
	default:
		return (ENOTSUP);
	}

	/* The report buffer already carries the report ID when numbered. */
	return (surface_hid_set_raw_report(sc, cid, data, len));
}

static int
surface_hid_set_idle(device_t dev, device_t child, uint16_t duration,
    uint8_t id)
{

	return (ENOTSUP);
}

static int
surface_hid_set_protocol(device_t dev, device_t child, uint16_t protocol)
{

	return (ENOTSUP);
}

static int
surface_hid_ioctl(device_t dev, device_t child, unsigned long cmd,
    uintptr_t data)
{

	return (ENOTTY);
}

static device_method_t surface_hid_methods[] = {
	DEVMETHOD(device_probe,		surface_hid_probe),
	DEVMETHOD(device_attach,	surface_hid_attach),
	DEVMETHOD(device_detach,	surface_hid_detach),

	DEVMETHOD(hid_intr_setup,	surface_hid_intr_setup),
	DEVMETHOD(hid_intr_unsetup,	surface_hid_intr_unsetup),
	DEVMETHOD(hid_intr_start,	surface_hid_intr_start),
	DEVMETHOD(hid_intr_stop,	surface_hid_intr_stop),
	DEVMETHOD(hid_intr_poll,	surface_hid_intr_poll),

	DEVMETHOD(hid_get_rdesc,	surface_hid_get_rdesc),
	DEVMETHOD(hid_read,		surface_hid_read),
	DEVMETHOD(hid_write,		surface_hid_write),
	DEVMETHOD(hid_get_report,	surface_hid_get_report),
	DEVMETHOD(hid_set_report,	surface_hid_set_report),
	DEVMETHOD(hid_set_idle,		surface_hid_set_idle),
	DEVMETHOD(hid_set_protocol,	surface_hid_set_protocol),
	DEVMETHOD(hid_ioctl,		surface_hid_ioctl),

	DEVMETHOD_END
};

static driver_t surface_hid_driver = {
	.name = "surface_hid",
	.methods = surface_hid_methods,
	.size = sizeof(struct surface_hid_softc),
};

DRIVER_MODULE(surface_hid, surface_sam, surface_hid_driver, 0, 0);
MODULE_DEPEND(surface_sam, hidbus, 1, 1, 1);
