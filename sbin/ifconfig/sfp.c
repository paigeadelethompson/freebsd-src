/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2014 Alexander V. Chernikov. All rights reserved.
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

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/sff8436.h>
#include <net/sff8472.h>

#include <math.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libutil.h>

#include <libifconfig.h>
#include <libifconfig_sfp.h>

#include "ifconfig.h"
#include <libxo/xo.h>

static void
sfp_dump_bytes(const uint8_t *data, size_t len)
{

	xo_open_list("sfp-dump");
	for (size_t i = 0; i < len; i++) {
		if (i % 16 == 0) {
			if (i > 0)
				xo_emit("{P:\n}");
			xo_emit("{P:\t}");
		}
		xo_emit("{P:/%02x}", data[i]);
		xo_emit("{le:sfp-dump/%u}", data[i]);
		if (i % 16 != 15 && i + 1 < len)
			xo_emit("{P: }");
	}
	xo_emit("{P:\n}");
	xo_close_list("sfp-dump");
}

void
sfp_status(if_ctx *ctx)
{
	struct ifconfig_sfp_info info;
	struct ifconfig_sfp_info_strings strings;
	struct ifconfig_sfp_vendor_info vendor_info;
	struct ifconfig_sfp_status status;
	size_t channel_count;
	int verbose = ctx->args->verbose;

	if (ifconfig_sfp_get_sfp_info(lifh, ctx->ifname, &info) == -1)
		return;

	ifconfig_sfp_get_sfp_info_strings(&info, &strings);

	xo_emit("{P:\tplugged: }{:sfp-id/%s}{P: }{:sfp-physical-spec/%s}{P: (}{:sfp-connector/%s}{P:)\n}",
	    ifconfig_sfp_id_display(info.sfp_id),
	    ifconfig_sfp_physical_spec(&info, &strings),
	    strings.sfp_conn);

	if (ifconfig_sfp_get_sfp_vendor_info(lifh, ctx->ifname, &vendor_info) == -1)
		return;

	xo_emit("{P:\tvendor: }{:sfp-vendor/%s}{P: PN: }{:sfp-part-number/%s}{P: SN: }{:sfp-serial/%s}{P: DATE: }{:sfp-date/%s}{P:\n}",
	    vendor_info.name, vendor_info.pn, vendor_info.sn, vendor_info.date);

	if (ifconfig_sfp_id_is_cmis(info.sfp_id)) {
		/* CMIS: no legacy compliance info to show */
	} else if (ifconfig_sfp_id_is_qsfp(info.sfp_id)) {
		if (verbose > 1)
			xo_emit("{P:\tcompliance level: }{:sfp-revision/%s}{P:\n}", strings.sfp_rev);
	} else {
		if (verbose > 5) {
			xo_emit("{P:Class: }{:sfp-class/%s}{P:\n}",
			    ifconfig_sfp_physical_spec(&info, &strings));
			xo_emit("{P:Length: }{:sfp-length/%s}{P:\n}", strings.sfp_fc_len);
			xo_emit("{P:Tech: }{:sfp-tech/%s}{P:\n}", strings.sfp_cab_tech);
			xo_emit("{P:Media: }{:sfp-media/%s}{P:\n}", strings.sfp_fc_media);
			xo_emit("{P:Speed: }{:sfp-speed/%s}{P:\n}", strings.sfp_fc_speed);
		}
	}

	if (ifconfig_sfp_get_sfp_status(lifh, ctx->ifname, &status) == 0) {
		if (ifconfig_sfp_id_is_qsfp(info.sfp_id) && verbose > 1)
			xo_emit("{P:\tnominal bitrate: }{:sfp-bitrate/%u}{P: Mbps\n}", status.bitrate);
		xo_emit("{P:\tmodule temperature: }{:sfp-temperature/%.2f}{P: C voltage: }{:sfp-voltage/%.2f}{P: Volts\n}",
		    status.temp, status.voltage);
		channel_count = ifconfig_sfp_channel_count(&info);
		for (size_t chan = 0; chan < channel_count; ++chan) {
			uint16_t rx = status.channel[chan].rx;
			uint16_t tx = status.channel[chan].tx;
			xo_emit("{P:\tlane }{:sfp-lane/%zu}{P: RX power: }{:sfp-rx-power-mw/%.2f}{P: mW (}{:sfp-rx-power-dbm/%.2f}{P: dBm) TX bias: }{:sfp-tx-bias/%.2f}{P: mA\n}",
			    chan + 1, power_mW(rx), power_dBm(rx), bias_mA(tx));
		}
		ifconfig_sfp_free_sfp_status(&status);
	}

	if (verbose > 2) {
		struct ifconfig_sfp_dump dump;

		if (ifconfig_sfp_get_sfp_dump(lifh, ctx->ifname, &dump) == -1)
			return;

		if (ifconfig_sfp_id_is_qsfp(info.sfp_id)) {
			xo_emit("{P:\n\tSFF8436 DUMP (0xA0 128..255 range):\n}");
			sfp_dump_bytes(dump.data + QSFP_DUMP1_START, QSFP_DUMP1_SIZE);
			xo_emit("{P:\n\tSFF8436 DUMP (0xA0 0..81 range):\n}");
			sfp_dump_bytes(dump.data + QSFP_DUMP0_START, QSFP_DUMP0_SIZE);
		} else {
			xo_emit("{P:\n\tSFF8472 DUMP (0xA0 0..127 range):\n}");
			sfp_dump_bytes(dump.data + SFP_DUMP_START, SFP_DUMP_SIZE);
		}
	}
}
