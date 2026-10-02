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
 * Performance profile support for the Surface Serial Hub (SAM).  The
 * System Aggregator Module implements the thermal management profile
 * of the machine as a target category of its own: the host reads and
 * writes a four step profile which the firmware applies to the CPU and
 * (on machines with a fan) to the fan controller.
 *
 * FreeBSD only has a two step notion of a power profile
 * (performance and economy), so this driver maps the two extremes of
 * the EC scale onto it and keeps the full scale reachable through a
 * sysctl.  Changes made anywhere else in the system (AC adapter
 * events, for example) are picked up through the power_profile_change
 * event handler and forwarded to the EC.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <sys/power.h>

#include <dev/surface/surface_sam.h>

#include "device_if.h"

/* Command IDs on the thermal management category (SSH_TC_TMP). */
#define	SURFACE_PROFILE_CID_GET			0x02
#define	SURFACE_PROFILE_CID_SET			0x03

/* Command ID on the fan category (SSH_TC_FAN). */
#define	SURFACE_PROFILE_CID_FAN_SET		0x0e

/* Profiles understood by the EC. */
#define	SURFACE_PROFILE_NORMAL			1
#define	SURFACE_PROFILE_BATTERY_SAVER		2
#define	SURFACE_PROFILE_BETTER_PERFORMANCE	3
#define	SURFACE_PROFILE_BEST_PERFORMANCE	4
#define	SURFACE_PROFILE_MIN			SURFACE_PROFILE_NORMAL
#define	SURFACE_PROFILE_MAX			SURFACE_PROFILE_BEST_PERFORMANCE

/* Profile responses consist of the profile plus two unknown fields. */
#define	SURFACE_PROFILE_INFO_SIZE		8

/*
 * Response to a profile read or write.  Multi-byte fields are
 * little-endian.
 */
struct surface_profile_info {
	uint32_t	pi_profile;
	uint16_t	pi_unknown1;
	uint16_t	pi_unknown2;
} __packed;

CTASSERT(sizeof(struct surface_profile_info) == SURFACE_PROFILE_INFO_SIZE);

struct surface_profile_softc {
	device_t		 sc_dev;
	struct surface_sam	*sc_sam;
	struct mtx		 sc_mtx;
	struct taskqueue	*sc_tq;
	struct task		 sc_task;
	int			 sc_profile;
	int			 sc_target;
	eventhandler_tag	 sc_evh;
	struct sysctl_ctx_list	 sc_sysctl_ctx;
};

/*
 * Convert a profile reported by the EC into the system power profile.
 * Everything but the battery saver profile counts as performance: the
 * EC scale has two intermediate steps FreeBSD cannot represent.
 */
static int
surface_profile_to_power(int profile)
{

	return (profile == SURFACE_PROFILE_BATTERY_SAVER ?
	    POWER_PROFILE_ECONOMY : POWER_PROFILE_PERFORMANCE);
}

/* Convert the system power profile into the closest EC profile. */
static int
surface_profile_from_power(int state)
{

	return (state == POWER_PROFILE_ECONOMY ?
	    SURFACE_PROFILE_BATTERY_SAVER : SURFACE_PROFILE_BEST_PERFORMANCE);
}

static int
surface_profile_read(struct surface_profile_softc *sc, int *profile)
{
	struct surface_profile_info info;
	size_t rlen;
	uint32_t value;
	int error;

	rlen = sizeof(info);
	error = surface_sam_request(sc->sc_sam, SSH_TC_TMP, SSH_TID_SAM, 0x00,
	    SURFACE_PROFILE_CID_GET, NULL, 0, &info, &rlen, 0);
	if (error != 0)
		return (error);
	if (rlen < sizeof(value))
		return (EPROTO);

	value = le32toh(info.pi_profile);
	if (value < SURFACE_PROFILE_MIN || value > SURFACE_PROFILE_MAX)
		return (EPROTO);
	*profile = value;
	return (0);
}

/*
 * The fan controller is programmed with its own profile scale, which
 * has the normal and battery saver steps swapped.
 */
static uint8_t
surface_profile_fan(int profile)
{

	switch (profile) {
	case SURFACE_PROFILE_NORMAL:
		return (2);
	case SURFACE_PROFILE_BATTERY_SAVER:
		return (1);
	default:
		return (profile);
	}
}

/*
 * Write a profile to the EC.  Not every machine has a fan and the
 * firmware does not necessarily implement the fan profile on those
 * which do not, so a failure there is only reported.
 */
static int
surface_profile_write(struct surface_profile_softc *sc, int profile)
{
	struct surface_profile_info info;
	uint8_t fan;
	uint32_t value;
	size_t rlen;
	int error;

	value = htole32(profile);
	rlen = sizeof(info);
	error = surface_sam_request(sc->sc_sam, SSH_TC_TMP, SSH_TID_SAM, 0x00,
	    SURFACE_PROFILE_CID_SET, &value, sizeof(value), &info, &rlen, 0);
	if (error != 0)
		return (error);

	fan = surface_profile_fan(profile);
	rlen = 0;
	error = surface_sam_request(sc->sc_sam, SSH_TC_FAN, SSH_TID_SAM, 0x01,
	    SURFACE_PROFILE_CID_FAN_SET, &fan, sizeof(fan), NULL, &rlen, 0);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "failed to set the fan profile (error %d)\n", error);
		return (0);
	}
	return (0);
}

/* Apply a profile read from the EC or from the sysctl. */
static bool
surface_profile_apply(struct surface_profile_softc *sc, int profile)
{
	bool done;
	int error;

	mtx_lock(&sc->sc_mtx);
	error = surface_profile_write(sc, profile);
	done = (error == 0);
	if (done)
		sc->sc_profile = profile;
	mtx_unlock(&sc->sc_mtx);

	if (!done) {
		device_printf(sc->sc_dev,
		    "failed to set profile %d (error %d)\n", profile, error);
		return (false);
	}

	/*
	 * Publish the change to the rest of the system.  Our own event
	 * handler then sees that the EC already holds the profile and
	 * does nothing.
	 */
	if (power_profile_get_state() != surface_profile_to_power(profile))
		power_profile_set_state(surface_profile_to_power(profile));
	return (true);
}

/*
 * Apply profiles until the EC holds the last requested one; the
 * profile may be changed again while a request is in flight.
 */
static void
surface_profile_task(void *arg, int pending)
{
	struct surface_profile_softc *sc = arg;
	int profile;

	for (;;) {
		mtx_lock(&sc->sc_mtx);
		profile = sc->sc_target;
		if (profile == sc->sc_profile)
			profile = 0;
		mtx_unlock(&sc->sc_mtx);
		if (profile == 0)
			break;
		if (!surface_profile_apply(sc, profile))
			break;
	}
}

static void
surface_profile_power_profile(void *arg, int state)
{
	struct surface_profile_softc *sc = arg;
	int profile;

	(void)state;

	/*
	 * Requests against the EC sleep, which is not allowed in the
	 * context the power profile is usually changed from; do the
	 * work on our own task thread.
	 */
	profile = surface_profile_from_power(power_profile_get_state());

	mtx_lock(&sc->sc_mtx);
	if (sc->sc_profile != profile) {
		sc->sc_target = profile;
		taskqueue_enqueue(sc->sc_tq, &sc->sc_task);
	}
	mtx_unlock(&sc->sc_mtx);
}

static int
surface_profile_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct surface_profile_softc *sc = oidp->oid_arg1;
	int profile, error;

	mtx_lock(&sc->sc_mtx);
	profile = sc->sc_profile;
	mtx_unlock(&sc->sc_mtx);

	error = sysctl_handle_int(oidp, &profile, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (profile < SURFACE_PROFILE_MIN ||
	    profile > SURFACE_PROFILE_MAX)
		return (EINVAL);

	return (surface_profile_apply(sc, profile) ? 0 : EIO);
}

static int
surface_profile_probe(device_t dev)
{

	if (surface_sam_get() == NULL)
		return (ENXIO);
	return (BUS_PROBE_DEFAULT);
}

static int
surface_profile_attach(device_t dev)
{
	struct surface_profile_softc *sc = device_get_softc(dev);
	int profile;
	int error;

	sc->sc_dev = dev;
	sc->sc_sam = surface_sam_get();
	if (sc->sc_sam == NULL)
		return (ENXIO);

	mtx_init(&sc->sc_mtx, "surface profile", NULL, MTX_DEF);
	sc->sc_tq = taskqueue_create("surface_profile", M_WAITOK,
	    taskqueue_thread_enqueue, NULL);
	if (sc->sc_tq == NULL) {
		error = ENOMEM;
		goto fail_mtx;
	}
	TASK_INIT(&sc->sc_task, 0, surface_profile_task, sc);
	taskqueue_start_threads(&sc->sc_tq, 1, PWAIT,
	    "surface_profile %d", device_get_unit(dev));

	error = surface_profile_read(sc, &profile);
	if (error != 0) {
		device_printf(dev,
		    "no performance profile support (error %d)\n", error);
		error = ENXIO;
		goto fail_tq;
	}
	sc->sc_profile = profile;

	(void)sysctl_ctx_init(&sc->sc_sysctl_ctx);
	SYSCTL_ADD_PROC(&sc->sc_sysctl_ctx,
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "profile",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    surface_profile_sysctl, "I",
	    "performance profile (1: normal, 2: battery saver, "
	    "3: better performance, 4: best performance)");

	sc->sc_evh = EVENTHANDLER_REGISTER(power_profile_change,
	    surface_profile_power_profile, sc, 0);

	/* Let the rest of the system know what the EC is doing. */
	if (power_profile_get_state() != surface_profile_to_power(profile))
		power_profile_set_state(surface_profile_to_power(profile));

	device_printf(dev, "performance profile %d\n", profile);
	return (0);

fail_tq:
	taskqueue_drain_all(sc->sc_tq);
	taskqueue_free(sc->sc_tq);
	sc->sc_tq = NULL;
fail_mtx:
	mtx_destroy(&sc->sc_mtx);
	return (error);
}

static int
surface_profile_detach(device_t dev)
{
	struct surface_profile_softc *sc = device_get_softc(dev);

	EVENTHANDLER_DEREGISTER(power_profile_change, sc->sc_evh);
	taskqueue_drain_all(sc->sc_tq);
	taskqueue_free(sc->sc_tq);
	sc->sc_tq = NULL;
	sysctl_ctx_free(&sc->sc_sysctl_ctx);
	mtx_destroy(&sc->sc_mtx);
	return (0);
}

static device_method_t surface_profile_methods[] = {
	DEVMETHOD(device_probe,		surface_profile_probe),
	DEVMETHOD(device_attach,	surface_profile_attach),
	DEVMETHOD(device_detach,	surface_profile_detach),

	DEVMETHOD_END
};

static driver_t surface_profile_driver = {
	.name = "surface_profile",
	.methods = surface_profile_methods,
	.size = sizeof(struct surface_profile_softc),
};

DRIVER_MODULE(surface_profile, surface_sam, surface_profile_driver, 0, 0);
