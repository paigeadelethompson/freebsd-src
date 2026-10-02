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
 */

/*
 * Real time clock of the Surface Serial Hub (SAM).  The System
 * Aggregator Module keeps its own battery backed clock, which survives
 * when the machine is powered down completely; it is used as an
 * additional time-of-day clock(9) and is reachable directly through a
 * sysctl.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/clock.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <dev/surface/surface_sam.h>

#include "clock_if.h"
#include "device_if.h"

/* Command IDs on the SAM category (SSH_TC_SAM). */
#define	SURFACE_RTC_CID_GET	0x10
#define	SURFACE_RTC_CID_SET	0x0f

/* The clock of the EC has a resolution of one second. */
#define	SURFACE_RTC_RESOLUTION_US	1000000

struct surface_rtc_softc {
	device_t		 sc_dev;
	struct surface_sam	*sc_sam;
	struct sysctl_ctx_list	 sc_sysctl_ctx;
};

static int
surface_rtc_gettime(struct surface_rtc_softc *sc, time_t *secs)
{
	uint32_t value;
	size_t rlen;
	int error;

	rlen = sizeof(value);
	error = surface_sam_request(sc->sc_sam, SSH_TC_SAM, SSH_TID_SAM, 0x00,
	    SURFACE_RTC_CID_GET, NULL, 0, &value, &rlen, 0);
	if (error != 0)
		return (error);
	if (rlen < sizeof(value))
		return (EPROTO);

	*secs = (time_t)le32toh(value);
	return (0);
}

static int
surface_rtc_settime(struct surface_rtc_softc *sc, time_t secs)
{
	uint32_t value;
	size_t rlen;

	value = htole32((uint32_t)secs);
	rlen = 0;
	return (surface_sam_request(sc->sc_sam, SSH_TC_SAM, SSH_TID_SAM, 0x00,
	    SURFACE_RTC_CID_SET, &value, sizeof(value), NULL, &rlen, 0));
}

static int
surface_rtc_epoch_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct surface_rtc_softc *sc = oidp->oid_arg1;
	time_t secs;
	long epoch;
	int error;

	error = surface_rtc_gettime(sc, &secs);
	if (error != 0)
		return (error);

	epoch = secs;
	error = sysctl_handle_long(oidp, &epoch, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (epoch < 0)
		return (EINVAL);

	return (surface_rtc_settime(sc, (time_t)epoch));
}

static int
surface_rtc_probe(device_t dev)
{

	if (surface_sam_get() == NULL)
		return (ENXIO);
	return (BUS_PROBE_DEFAULT);
}

static int
surface_rtc_attach(device_t dev)
{
	struct surface_rtc_softc *sc = device_get_softc(dev);
	time_t secs;
	int error;

	sc->sc_dev = dev;
	sc->sc_sam = surface_sam_get();
	if (sc->sc_sam == NULL)
		return (ENXIO);

	error = surface_rtc_gettime(sc, &secs);
	if (error != 0) {
		device_printf(dev, "no clock support (error %d)\n", error);
		return (ENXIO);
	}

	(void)sysctl_ctx_init(&sc->sc_sysctl_ctx);
	SYSCTL_ADD_PROC(&sc->sc_sysctl_ctx,
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "epoch",
	    CTLTYPE_LONG | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    surface_rtc_epoch_sysctl, "J",
	    "clock time in seconds since the epoch");

	clock_register(dev, SURFACE_RTC_RESOLUTION_US);

	device_printf(dev, "clock time %lld\n", (long long)secs);
	return (0);
}

static int
surface_rtc_detach(device_t dev)
{
	struct surface_rtc_softc *sc = device_get_softc(dev);

	clock_unregister(dev);
	sysctl_ctx_free(&sc->sc_sysctl_ctx);
	return (0);
}

/*
 * Clock interface.  The time of the clock is seconds since the epoch
 * in UTC; the framework takes care of the timezone offset.
 */
static int
surface_rtc_clock_gettime(device_t dev, struct timespec *ts)
{
	struct surface_rtc_softc *sc = device_get_softc(dev);
	time_t secs;
	int error;

	error = surface_rtc_gettime(sc, &secs);
	if (error != 0)
		return (error);

	ts->tv_sec = secs;
	ts->tv_nsec = 0;
	return (0);
}

static int
surface_rtc_clock_settime(device_t dev, struct timespec *ts)
{
	struct surface_rtc_softc *sc = device_get_softc(dev);

	return (surface_rtc_settime(sc, ts->tv_sec));
}

static device_method_t surface_rtc_methods[] = {
	DEVMETHOD(device_probe,		surface_rtc_probe),
	DEVMETHOD(device_attach,	surface_rtc_attach),
	DEVMETHOD(device_detach,	surface_rtc_detach),

	DEVMETHOD(clock_gettime,	surface_rtc_clock_gettime),
	DEVMETHOD(clock_settime,	surface_rtc_clock_settime),

	DEVMETHOD_END
};

static driver_t surface_rtc_driver = {
	.name = "surface_rtc",
	.methods = surface_rtc_methods,
	.size = sizeof(struct surface_rtc_softc),
};

DRIVER_MODULE(surface_rtc, surface_sam, surface_rtc_driver, 0, 0);
