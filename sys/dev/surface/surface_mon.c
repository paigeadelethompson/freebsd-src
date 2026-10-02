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
 * Monitoring for the Surface Serial Hub (SAM): the temperature sensors
 * of the thermal management target category and, on machines which
 * have one, the speed of the fan.
 *
 * The EC does not push sensor updates, so the values are read from the
 * EC when they are looked at rather than being cached.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/surface/surface_sam.h>

#include "device_if.h"

/* Command IDs on the thermal management category (SSH_TC_TMP). */
#define	SURFACE_MON_CID_GET_TEMP	0x01
#define	SURFACE_MON_CID_GET_SENSORS	0x04
#define	SURFACE_MON_CID_GET_NAME	0x0e

/* Command ID on the fan category (SSH_TC_FAN). */
#define	SURFACE_MON_CID_GET_FAN_RPM	0x01

/*
 * Temperature sensors are addressed by channel number; the sensor of
 * channel n lives on instance ID n + SURFACE_MON_CHANNEL_FIRST.
 */
#define	SURFACE_MON_CHANNEL_FIRST	1

/* The set of implemented sensors is a bitmap of channel numbers. */
#define	SURFACE_MON_SENSOR_COUNT	16

/* Length of the sensor name field in a name response. */
#define	SURFACE_MON_NAME_SIZE		18
#define	SURFACE_MON_NAME_MAX		(SURFACE_MON_NAME_SIZE + 1)

/* Temperatures are reported in tenths of a degree Kelvin. */
#define	SURFACE_MON_TEMP_KELVIN_0	2731

/*
 * Response to a sensor name request.  Multi-byte fields are
 * little-endian.
 */
struct surface_mon_name {
	uint16_t	mn_unknown1;
	uint8_t		mn_unknown2;
	char		mn_name[SURFACE_MON_NAME_SIZE];
} __packed;

CTASSERT(sizeof(struct surface_mon_name) ==
    sizeof(uint16_t) + sizeof(uint8_t) + SURFACE_MON_NAME_SIZE);

struct surface_mon_softc;

struct surface_mon_sensor {
	struct surface_mon_softc	*sm_sc;
	uint8_t				 sm_channel;
	char				 sm_name[SURFACE_MON_NAME_MAX];
};

struct surface_mon_softc {
	device_t			 sc_dev;
	struct surface_sam		*sc_sam;
	struct surface_mon_sensor	*sc_sensors;
	uint8_t				 sc_nsensor;
	bool				 sc_has_fan;
	struct sysctl_ctx_list		 sc_sysctl_ctx;
};

/*
 * Read one temperature sensor.  The EC returns the temperature in
 * tenths of a degree Kelvin; negative values mean that the sensor is
 * not currently reporting.
 */
static int
surface_mon_get_temp(struct surface_mon_sensor *sensor, int *temp)
{
	struct surface_mon_softc *sc = sensor->sm_sc;
	uint16_t value;
	size_t rlen;
	int error;

	rlen = sizeof(value);
	error = surface_sam_request(sc->sc_sam, SSH_TC_TMP, SSH_TID_SAM,
	    sensor->sm_channel, SURFACE_MON_CID_GET_TEMP, NULL, 0, &value,
	    &rlen, 0);
	if (error != 0)
		return (error);
	if (rlen < sizeof(value))
		return (EPROTO);

	value = le16toh(value);
	if (value < SURFACE_MON_TEMP_KELVIN_0)
		return (ERANGE);

	*temp = value;
	return (0);
}

static int
surface_mon_get_fan_rpm(struct surface_mon_softc *sc, int *rpm)
{
	uint16_t value;
	size_t rlen;
	int error;

	rlen = sizeof(value);
	error = surface_sam_request(sc->sc_sam, SSH_TC_FAN, SSH_TID_SAM, 0x01,
	    SURFACE_MON_CID_GET_FAN_RPM, NULL, 0, &value, &rlen, 0);
	if (error != 0)
		return (error);
	if (rlen < sizeof(value))
		return (EPROTO);

	*rpm = le16toh(value);
	return (0);
}

static int
surface_mon_temp_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct surface_mon_sensor *sensor = oidp->oid_arg1;
	int error, temp;

	if (req->newptr != NULL)
		return (EPERM);

	temp = 0;
	error = surface_mon_get_temp(sensor, &temp);
	if (error != 0)
		return (error);

	return (sysctl_handle_int(oidp, &temp, 0, req));
}

static int
surface_mon_fan_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct surface_mon_softc *sc = oidp->oid_arg1;
	int error, rpm;

	if (req->newptr != NULL)
		return (EPERM);

	rpm = 0;
	error = surface_mon_get_fan_rpm(sc, &rpm);
	if (error != 0)
		return (error);

	return (sysctl_handle_int(oidp, &rpm, 0, req));
}

/*
 * Ask the EC for the name of a sensor.  Sensors which do not answer
 * are still usable, they are simply unnamed.
 */
static void
surface_mon_get_name(struct surface_mon_sensor *sensor)
{
	struct surface_mon_name name;
	size_t rlen;
	int error;

	rlen = sizeof(name);
	error = surface_sam_request(sensor->sm_sc->sc_sam, SSH_TC_TMP,
	    SSH_TID_SAM, sensor->sm_channel, SURFACE_MON_CID_GET_NAME, NULL, 0,
	    &name, &rlen, 0);
	if (error != 0 || rlen < sizeof(name))
		return;

	memcpy(sensor->sm_name, name.mn_name, SURFACE_MON_NAME_SIZE);
	sensor->sm_name[SURFACE_MON_NAME_SIZE] = '\0';
	/* The firmware does not necessarily pad the name. */
	sensor->sm_name[SURFACE_MON_NAME_SIZE - 1] = '\0';
}

static void
surface_mon_add_sysctls(struct surface_mon_softc *sc)
{
	struct surface_mon_sensor *sensor;
	struct sysctl_oid *node;
	char name[16];
	int i;

	(void)sysctl_ctx_init(&sc->sc_sysctl_ctx);
	for (i = 0; i < sc->sc_nsensor; i++) {
		sensor = &sc->sc_sensors[i];
		snprintf(name, sizeof(name), "temp%d",
		    sensor->sm_channel - SURFACE_MON_CHANNEL_FIRST);
		node = SYSCTL_ADD_NODE(&sc->sc_sysctl_ctx,
		    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->sc_dev)),
		    OID_AUTO, name, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
		    "Surface temperature sensor");
		SYSCTL_ADD_STRING(&sc->sc_sysctl_ctx, SYSCTL_CHILDREN(node),
		    OID_AUTO, "name", CTLFLAG_RD | CTLFLAG_MPSAFE,
		    sensor->sm_name, 0, "sensor name");
		SYSCTL_ADD_PROC(&sc->sc_sysctl_ctx, SYSCTL_CHILDREN(node),
		    OID_AUTO, "temperature",
		    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, sensor, 0,
		    surface_mon_temp_sysctl, "IK",
		    "temperature in tenths of a degree Kelvin");
	}

	if (!sc->sc_has_fan)
		return;
	node = SYSCTL_ADD_NODE(&sc->sc_sysctl_ctx,
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->sc_dev)), OID_AUTO,
	    "fan", CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "Surface fan");
	SYSCTL_ADD_PROC(&sc->sc_sysctl_ctx, SYSCTL_CHILDREN(node), OID_AUTO,
	    "rpm", CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    surface_mon_fan_sysctl, "I", "fan speed in revolutions per minute");
}

/*
 * Enumerate the temperature sensors of the EC.  The EC answers with a
 * bitmap of the channels it implements; channel n is the instance ID
 * n + SURFACE_MON_CHANNEL_FIRST.
 */
static int
surface_mon_attach_sensors(struct surface_mon_softc *sc)
{
	struct surface_mon_sensor *sensor;
	uint16_t sensors;
	size_t rlen;
	int error, i;

	rlen = sizeof(sensors);
	error = surface_sam_request(sc->sc_sam, SSH_TC_TMP, SSH_TID_SAM, 0x00,
	    SURFACE_MON_CID_GET_SENSORS, NULL, 0, &sensors, &rlen, 0);
	if (error != 0)
		return (error);
	if (rlen < sizeof(sensors))
		return (EPROTO);

	sensors = le16toh(sensors);
	for (i = 0; i < SURFACE_MON_SENSOR_COUNT; i++)
		if (sensors & (1U << i))
			sc->sc_nsensor++;
	if (sc->sc_nsensor == 0)
		return (ENXIO);

	sc->sc_sensors = malloc(sc->sc_nsensor * sizeof(*sc->sc_sensors),
	    M_SURFACE_SAM, M_WAITOK | M_ZERO);
	if (sc->sc_sensors == NULL)
		return (ENOMEM);

	sc->sc_nsensor = 0;
	for (i = 0; i < SURFACE_MON_SENSOR_COUNT; i++) {
		if ((sensors & (1U << i)) == 0)
			continue;
		sensor = &sc->sc_sensors[sc->sc_nsensor++];
		sensor->sm_sc = sc;
		sensor->sm_channel = i + SURFACE_MON_CHANNEL_FIRST;
		snprintf(sensor->sm_name, sizeof(sensor->sm_name),
		    "sensor%d", i);
		surface_mon_get_name(sensor);
	}
	return (0);
}

static int
surface_mon_probe(device_t dev)
{

	if (surface_sam_get() == NULL)
		return (ENXIO);
	return (BUS_PROBE_DEFAULT);
}

static int
surface_mon_attach(device_t dev)
{
	struct surface_mon_softc *sc = device_get_softc(dev);
	int error, rpm;

	sc->sc_dev = dev;
	sc->sc_sam = surface_sam_get();
	if (sc->sc_sam == NULL)
		return (ENXIO);

	error = surface_mon_attach_sensors(sc);
	if (error != 0) {
		device_printf(dev,
		    "no temperature sensors (error %d)\n", error);
		error = ENXIO;
		goto fail_free;
	}

	/*
	 * Machines without a fan do not answer this request; that is
	 * not an error, the fan is simply not exposed.
	 */
	if (surface_mon_get_fan_rpm(sc, &rpm) == 0)
		sc->sc_has_fan = true;

	surface_mon_add_sysctls(sc);

	device_printf(dev, "%u temperature sensor%s%s\n", sc->sc_nsensor,
	    sc->sc_nsensor == 1 ? "" : "s",
	    sc->sc_has_fan ? ", fan" : "");
	return (0);

fail_free:
	free(sc->sc_sensors, M_SURFACE_SAM);
	sc->sc_sensors = NULL;
	return (error);
}

static int
surface_mon_detach(device_t dev)
{
	struct surface_mon_softc *sc = device_get_softc(dev);

	sysctl_ctx_free(&sc->sc_sysctl_ctx);
	free(sc->sc_sensors, M_SURFACE_SAM);
	sc->sc_sensors = NULL;
	return (0);
}

static device_method_t surface_mon_methods[] = {
	DEVMETHOD(device_probe,		surface_mon_probe),
	DEVMETHOD(device_attach,	surface_mon_attach),
	DEVMETHOD(device_detach,	surface_mon_detach),

	DEVMETHOD_END
};

static driver_t surface_mon_driver = {
	.name = "surface_mon",
	.methods = surface_mon_methods,
	.size = sizeof(struct surface_mon_softc),
};

DRIVER_MODULE(surface_mon, surface_sam, surface_mon_driver, 0, 0);
