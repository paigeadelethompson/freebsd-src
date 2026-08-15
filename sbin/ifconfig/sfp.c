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

#include <libifconfig.h>
#include <libifconfig_sfp.h>

#include "ifconfig.h"
#include "ifconfig_output.h"

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

	sfp_print_plugged(&info, &strings);

	if (ifconfig_sfp_get_sfp_vendor_info(lifh, ctx->ifname, &vendor_info) == -1)
		return;

	sfp_print_vendor(&vendor_info);

	if (ifconfig_sfp_id_is_cmis(info.sfp_id)) {
		/* CMIS: no legacy compliance info to show */
	} else if (ifconfig_sfp_id_is_qsfp(info.sfp_id)) {
		if (verbose > 1)
			sfp_print_verbose_compliance(&strings);
	} else {
		if (verbose > 5) {
			sfp_print_verbose_class(&info, &strings);
			sfp_print_verbose_length(&strings);
			sfp_print_verbose_tech(&strings);
			sfp_print_verbose_media(&strings);
			sfp_print_verbose_speed(&strings);
		}
	}

	if (ifconfig_sfp_get_sfp_status(lifh, ctx->ifname, &status) == 0) {
		if (ifconfig_sfp_id_is_qsfp(info.sfp_id) && verbose > 1)
			sfp_print_verbose_nombitrate(&status);
		sfp_print_verbose_voltage(&status);
		channel_count = ifconfig_sfp_channel_count(&info);
		for (size_t chan = 0; chan < channel_count; ++chan) {
			uint16_t rx = status.channel[chan].rx;
			uint16_t tx = status.channel[chan].tx;
			sfp_print_verbose_rxpower(chan, rx, tx);
		}
		ifconfig_sfp_free_sfp_status(&status);
	}

	if (verbose > 2) {
		struct ifconfig_sfp_dump dump;

		if (ifconfig_sfp_get_sfp_dump(lifh, ctx->ifname, &dump) == -1)
			return;

		if (ifconfig_sfp_id_is_cmis(info.sfp_id)) {
			sfp_print_verbose_cmis_dump1(dump.data, 128);
			sfp_print_verbose_cmis_dump2(dump.data + 128, 128);
			sfp_print_verbose_cmis_dump3(dump.data + CMIS_DUMP_P11,
			    128);
		} else if (ifconfig_sfp_id_is_qsfp(info.sfp_id)) {
			sfp_print_verbose_sff8436_dump1(
			    dump.data + QSFP_DUMP1_START, QSFP_DUMP1_SIZE);
			sfp_print_verbose_sff8436_dump2(
			    dump.data + QSFP_DUMP0_START, QSFP_DUMP0_SIZE);
		} else {
			sfp_print_verbose_sff8472_dump1(dump.data);
		}
	}
}
