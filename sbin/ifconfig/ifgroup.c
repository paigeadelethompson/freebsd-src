/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2006 Max Laier. All rights reserved.
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

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libifconfig.h>

#include "ifconfig.h"
#include "ifconfig_output.h"

static void
setifgroup(if_ctx *ctx, const char *group_name, int dummy __unused)
{
	struct ifgroupreq ifgr = {};

	strlcpy(ifgr.ifgr_name, ctx->ifname, IFNAMSIZ);

	if (group_name[0] && isdigit(group_name[strlen(group_name) - 1]))
		if_errx(1, "setifgroup: group names may not end in a digit");

	if (strlcpy(ifgr.ifgr_group, group_name, IFNAMSIZ) >= IFNAMSIZ)
		if_errx(1, "setifgroup: group name too long");
	if (ioctl_ctx(ctx, SIOCAIFGROUP, (caddr_t)&ifgr) == -1 && errno != EEXIST)
		if_err(1," SIOCAIFGROUP");
}

static void
unsetifgroup(if_ctx *ctx, const char *group_name, int dummy __unused)
{
	struct ifgroupreq ifgr = {};

	strlcpy(ifgr.ifgr_name, ctx->ifname, IFNAMSIZ);

	if (group_name[0] && isdigit(group_name[strlen(group_name) - 1]))
		if_errx(1, "unsetifgroup: group names may not end in a digit");

	if (strlcpy(ifgr.ifgr_group, group_name, IFNAMSIZ) >= IFNAMSIZ)
		if_errx(1, "unsetifgroup: group name too long");
	if (ioctl_ctx(ctx, SIOCDIFGROUP, (caddr_t)&ifgr) == -1 && errno != ENOENT)
		if_err(1, "SIOCDIFGROUP");
}

static void
getifgroups(if_ctx *ctx)
{
	struct ifgroupreq ifgr;
	size_t cnt;

	if (ifconfig_get_groups(lifh, ctx->ifname, &ifgr) == -1)
		return;

	cnt = 0;
	for (size_t i = 0; i < ifgr.ifgr_len / sizeof(struct ifg_req); ++i) {
		struct ifg_req *ifg = &ifgr.ifgr_groups[i];

		if (strcmp(ifg->ifgrq_group, "all")) {
			if (cnt++ == 0)
				ifgroup_open_groups();
			ifgroup_print_group(ifg);
		}
	}
	if (cnt > 0)
		ifgroup_close_groups();

	free(ifgr.ifgr_groups);
}

static struct cmd group_cmds[] = {
	DEF_CMD_ARG("group",	setifgroup),
	DEF_CMD_ARG("-group",	unsetifgroup),
};

static struct afswtch af_group = {
	.af_name	= "af_group",
	.af_af		= AF_UNSPEC,
	.af_other_status = getifgroups,
};

static struct option group_gopt = {
	.opt		= "g:",
	.opt_usage	= "[-g groupname]",
	.cb		= ifgroup_printgroup,
};

static __constructor void
group_ctor(void)
{
	for (size_t i = 0; i < nitems(group_cmds);  i++)
		cmd_register(&group_cmds[i]);
	af_register(&af_group);
	opt_register(&group_gopt);
}
