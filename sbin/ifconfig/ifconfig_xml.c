/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Paige Thompson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 */

#if defined(WITH_BSDXML)

#include <sys/param.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <ctype.h>
#include <err.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bsdxml.h>

#include "ifconfig.h"
#include "ifconfig_output.h"
#include "ifconfig_xml.h"

#define	XML_TEXT_MAX	8192

struct xml_addr {
	int family;
	char *addr;
	char *netmask;
	int prefixlen;
	int vhid;
	struct xml_addr *next;
};

struct xml_kv {
	char *key;
	char *value;
};

struct xml_cmdentry {
	struct ifconfig_args args;
	int iscreate;
};

struct xml_iface {
	char *name;
	char *mac;
	char *drivername;
	char **ifcaps;
	size_t nifcaps;
	size_t ifcapcap;
	char **flags;
	size_t nflags;
	size_t flagcap;
	char **nd6;
	size_t nnd6;
	size_t nd6cap;
	int vlanid;
	bool up;
	struct xml_kv *kvs;
	size_t nkvs;
	size_t kvcap;
	struct xml_addr *addrs;
	struct xml_addr *addrs_tail;
	char **groups;
	size_t ngroups;
	size_t groupcap;
	struct xml_cmdentry *entries;
	size_t nentries;
	size_t entrycap;
	struct xml_iface *next;
};

struct xml_cloners {
	char **names;
	size_t count;
};

struct xml_parse_ctx {
	struct xml_iface *head;
	struct xml_iface *tail;
	struct xml_iface *cur;
	struct xml_addr *cur_inet;
	struct xml_addr *cur_inet6;
	char text[XML_TEXT_MAX];
	size_t tlen;
	char addrtype[32];
	bool in_groups;
	bool in_linkaddr;
	bool in_address;
	bool in_media;
	bool in_ifflags;
	bool in_ifcaps;
	bool in_nd6opts;
};

static char *
xml_strdup(const char *s)
{
	if (s == NULL)
		return (NULL);
	return (strdup(s));
}

struct xml_token {
	const char *name;
	const char *token;
};

static const struct xml_token xml_cap_tokens[] = {
	{ "RXCSUM",		"rxcsum" },
	{ "TXCSUM",		"txcsum" },
	{ "NETCONS",		"netcons" },
	{ "POLLING",		"polling" },
	{ "TSO4",		"tso4" },
	{ "TSO6",		"tso6" },
	{ "LRO",		"lro" },
	{ "MEXTPG",		"mextpg" },
	{ "RXCSUM_IPV6",	"rxcsum6" },
	{ "TXCSUM_IPV6",	"txcsum6" },
	{ "WOL_UCAST",		"wol_ucast" },
	{ "WOL_MCAST",		"wol_mcast" },
	{ "WOL_MAGIC",		"wol_magic" },
	{ "TXRTLMT",		"txrtlmt" },
	{ "TXTLS_RTLMT",	"txtlsrtlmt" },
	{ "HWRXTSTMP",		"hwrxtstmp" },
	{ "VLAN_MTU",		"vlanmtu" },
	{ "VLAN_HWTAGGING",	"vlanhwtag" },
	{ "VLAN_HWCSUM",	"vlanhwcsum" },
	{ "VLAN_HWFILTER",	"vlanhwfilter" },
	{ "VLAN_HWTSO",		"vlanhwtso" },
	{ "VXLAN_HWCSUM",	"vxlanhwcsum" },
	{ "VXLAN_HWTSO",	"vxlanhwtso" },
};

static const struct xml_token xml_flag_tokens[] = {
	{ "LINK0",		"link0" },
	{ "LINK1",		"link1" },
	{ "LINK2",		"link2" },
	{ "MONITOR",		"monitor" },
	{ "STATICARP",		"staticarp" },
	{ "STICKYARP",		"stickyarp" },
};

static const struct xml_token xml_nd6_tokens[] = {
	{ "PERFORMNUD",		"nud" },
	{ "ACCEPT_RTADV",	"accept_rtadv" },
	{ "IFDISABLED",		"ifdisabled" },
	{ "AUTO_LINKLOCAL",	"auto_linklocal" },
	{ "NO_RADR",		"no_radr" },
	{ "NO_PREFER_IFACE",	"no_prefer_iface" },
	{ "NO_DAD",		"no_dad" },
	{ "DEFAULTIF",		"defaultif" },
};

/*
 * Map status XML element names to the DEF_CMD keywords they restore.
 * Rows with a family are only applied when basename(drivername)
 * matches, since several modules reuse the same element names.
 */
static const struct xml_kw {
	const char *elem;
	const char *family;
	const char *kw;
} xml_kws[] = {
	{ "mtu",		NULL,	"mtu" },
	{ "metric",		NULL,	"metric" },
	{ "descr",		NULL,	"description" },
	{ "vlanpcp",		NULL,	"vlanpcp" },
	{ "pcp",		NULL,	"pcp" },
	{ "vlantag",		NULL,	"vlan" },
	{ "parent-ifname",	NULL,	"vlandev" },
	{ "vlan-protocol",	NULL,	"vlanproto" },
	{ "tunnel-src",		NULL,	"@tunnelpair" },
	{ "tunnel-dst",		NULL,	"@tunneldst" },
	{ "udpport",		NULL,	"udpport" },
	{ "fib-id",		NULL,	"fib" },
	{ "tunnelfib-id",	NULL,	"tunnelfib" },
	{ "reqid",		NULL,	"reqid" },
	{ "maclabel",		NULL,	"maclabel" },
	{ "lagg-proto",		"lagg",	"laggproto" },
	{ "flowid-shift",	"lagg",	"flowid_shift" },
	{ "rr-limit",		"lagg",	"rr_limit" },
	{ "subtype",		NULL,	"media" },
	{ "mode",		NULL,	"mode" },
	{ "vni",		"vxlan", "vni" },
	{ "vni",		"geneve", "geneveid" },
	{ "local-src",		"vxlan", "vxlanlocal" },
	{ "local-src",		"geneve", "genevelocal" },
	{ "remote-dst",		"vxlan", "vxlanremote" },
	{ "remote-dst",		"geneve", "geneveremote" },
	{ "local-src-port",	"vxlan", "vxlanlocalport" },
	{ "local-src-port",	"geneve", "genevelocalport" },
	{ "remote-dst-port",	"vxlan", "vxlanremoteport" },
	{ "remote-dst-port",	"geneve", "geneveremoteport" },
	{ "ttl",		"vxlan", "vxlanttl" },
	{ "ttl",		"geneve", "genevettl" },
	{ "timeout",		"vxlan", "vxlantimeout" },
	{ "timeout",		"geneve", "genevetimeout" },
	{ "maxaddr",		"vxlan", "vxlanmaxaddr" },
	{ "maxaddr",		"bridge", "maxaddr" },
	{ "maxaddr",		"geneve", "genevemaxaddr" },
	{ "syncdev",		"pfsync", "syncdev" },
	{ "maxupd",		"pfsync", "maxupd" },
	{ "version",		"pfsync", "version" },
	{ "advbase",		NULL,	"advbase" },
	{ "advskew",		NULL,	"advskew" },
	{ "priority",		"bridge", "priority" },
	{ "proto",		"bridge", "proto" },
	{ "hellotime",		"bridge", "hellotime" },
	{ "fwddelay",		"bridge", "fwddelay" },
	{ "maxage",		"bridge", "maxage" },
	{ "holdcnt",		"bridge", "holdcnt" },
	{ "timeout",		"bridge", "timeout" },
	{ "defuntagged",	"bridge", "defuntagged" },
};

static const struct xml_token *
xml_token_lookup(const struct xml_token *tbl, size_t len, const char *name)
{
	for (size_t i = 0; i < len; i++)
		if (strcmp(tbl[i].name, name) == 0)
			return (&tbl[i]);
	return (NULL);
}

static void
xml_strlist_add(char ***arrp, size_t *np, size_t *capp, const char *s)
{
	if (*np == *capp) {
		size_t ncap = *capp == 0 ? 4 : *capp * 2;

		char **na = reallocarray(*arrp, ncap, sizeof(*na));

		if (na == NULL)
			err(1, "reallocarray");
		*arrp = na;
		*capp = ncap;
	}
	(*arrp)[*np] = strdup(s);
	if ((*arrp)[*np] == NULL)
		err(1, "strdup");
	(*np)++;
}

static bool
xml_strlist_has(char **arr, size_t n, const char *s)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(arr[i], s) == 0)
			return (true);
	return (false);
}

static bool
xml_kv_skip(const char *name)
{
	static const char *skip[] = {
		"ifname", "drivername", "flags-hex", "capab-hex",
		"ifcap-options-hex", "gif-options-hex", "gre-options-hex",
		"nd6-options-hex", "if-status", "cloner-name",
		"ifname-print",
	};

	if (strncmp(name, "sfp-", 4) == 0)
		return (true);
	for (size_t i = 0; i < nitems(skip); i++)
		if (strcmp(skip[i], name) == 0)
			return (true);
	return (false);
}

static const struct xml_kw *xml_kw_row(const char *elem, const char *family);
static bool xml_kw_exists(const char *elem);

static struct xml_cmdentry *
xml_entry_new(struct xml_iface *ifp, int iscreate)
{
	struct xml_cmdentry *ne;

	if (ifp->nentries == ifp->entrycap) {
		size_t ncap = ifp->entrycap == 0 ? 4 : ifp->entrycap * 2;

		ne = reallocarray(ifp->entries, ncap, sizeof(*ne));
		if (ne == NULL)
			err(1, "reallocarray");
		ifp->entries = ne;
		ifp->entrycap = ncap;
	}
	ne = &ifp->entries[ifp->nentries++];
	memset(ne, 0, sizeof(*ne));
	ne->iscreate = iscreate;
	ne->args.ifname = ifp->name;
	return (ne);
}

static void
xml_argv_push(struct ifconfig_args *args, const char *arg)
{
	char **nav;

	nav = reallocarray(args->argv, args->argc + 1, sizeof(*nav));
	if (nav == NULL)
		err(1, "reallocarray");
	args->argv = nav;
	args->argv[args->argc] = strdup(arg);
	if (args->argv[args->argc] == NULL)
		err(1, "strdup");
	args->argc++;
}

static void
xml_args_free(struct ifconfig_args *args)
{
	while (args->argc > 0)
		free(args->argv[--args->argc]);
	free(args->argv);
	args->argv = NULL;
}

static int
xml_mask_to_prefixlen(const char *mask)
{
	char *endp;
	unsigned long value;
	int plen;

	if (mask[0] == '0' && (mask[1] == 'x' || mask[1] == 'X')) {
		value = strtoul(mask + 2, &endp, 16);
		if (*endp != '\0' || endp == mask + 2)
			return (-1);
	} else {
		value = strtoul(mask, &endp, 10);
		if (*endp != '\0' || endp == mask)
			return (-1);
		if (value <= 32)
			return ((int)value);
	}
	if (value > 0xffffffffUL)
		return (-1);
	for (plen = 0; plen < 32 && (value & (1UL << (31 - plen))) != 0;
	    plen++);
	if (((value << plen) & 0xffffffffUL) != 0)
		return (-1);
	return (plen);
}

static bool
xml_arg_needs_quotes(const char *s)
{
	static const char safe[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	    "0123456789_-.,:/@%+=";

	if (*s == '\0')
		return (true);
	for (const char *cp = s; *cp != '\0'; cp++)
		if (strchr(safe, *cp) == NULL)
			return (true);
	return (false);
}

static void
xml_print_arg(const char *s)
{
	if (!xml_arg_needs_quotes(s)) {
		fputs(s, stdout);
		return;
	}
	fputc('\'', stdout);
	for (const char *cp = s; *cp != '\0'; cp++) {
		if (*cp == '\'')
			fputs("'\\''", stdout);
		else
			fputc(*cp, stdout);
	}
	fputc('\'', stdout);
}

static void
xml_print_entry(const struct xml_cmdentry *e)
{
	printf("ifconfig %s", e->args.ifname);
	for (int i = 0; i < e->args.argc; i++) {
		fputc(' ', stdout);
		xml_print_arg(e->args.argv[i]);
	}
	fputc('\n', stdout);
}

static void
xml_exec(const struct xml_cmdentry *e, bool pretend)
{
	struct ifconfig_context ctx;

	if (pretend) {
		xml_print_entry(e);
		return;
	}

#ifdef WITHOUT_NETLINK
	memset(&ctx, 0, sizeof(ctx));
	ctx.args = __DECONST(struct ifconfig_args *, &e->args);
	ctx.ifname = e->args.ifname;
	(void)ifconfig_ioctl(&ctx, e->iscreate, e->args.afp);
#else
	memset(&ctx, 0, sizeof(ctx));
	ctx.args = __DECONST(struct ifconfig_args *, &e->args);
	ctx.io_s = -1;
	ctx.ifname = e->args.ifname;
	(void)ifconfig_nl(&ctx, e->iscreate, e->args.afp);
#endif
}

static struct xml_iface *
xml_iface_alloc(void)
{
	struct xml_iface *ifp = calloc(1, sizeof(*ifp));

	if (ifp == NULL)
		err(1, "calloc");
	ifp->vlanid = -1;
	return (ifp);
}

static void
xml_kv_add(struct xml_iface *ifp, const char *key, const char *value)
{
	if (ifp->nkvs == ifp->kvcap) {
		size_t ncap = ifp->kvcap == 0 ? 8 : ifp->kvcap * 2;

		struct xml_kv *nk = reallocarray(ifp->kvs, ncap,
		    sizeof(*nk));

		if (nk == NULL)
			err(1, "reallocarray");
		ifp->kvs = nk;
		ifp->kvcap = ncap;
	}
	ifp->kvs[ifp->nkvs].key = strdup(key);
	ifp->kvs[ifp->nkvs].value = strdup(value);
	if (ifp->kvs[ifp->nkvs].key == NULL ||
	    ifp->kvs[ifp->nkvs].value == NULL)
		err(1, "strdup");
	ifp->nkvs++;
}

static struct xml_kv *
xml_kv_find(struct xml_iface *ifp, const char *key)
{
	for (size_t i = 0; i < ifp->nkvs; i++)
		if (strcmp(ifp->kvs[i].key, key) == 0)
			return (&ifp->kvs[i]);
	return (NULL);
}

static void
xml_iface_free(struct xml_iface *ifp)
{
	struct xml_addr *ap;

	free(ifp->name);
	free(ifp->mac);
	free(ifp->drivername);
	for (size_t i = 0; i < ifp->nifcaps; i++)
		free(ifp->ifcaps[i]);
	free(ifp->ifcaps);
	for (size_t i = 0; i < ifp->nflags; i++)
		free(ifp->flags[i]);
	free(ifp->flags);
	for (size_t i = 0; i < ifp->nnd6; i++)
		free(ifp->nd6[i]);
	free(ifp->nd6);
	for (size_t i = 0; i < ifp->nkvs; i++) {
		free(ifp->kvs[i].key);
		free(ifp->kvs[i].value);
	}
	free(ifp->kvs);
	ap = ifp->addrs;
	while (ap != NULL) {
		struct xml_addr *next = ap->next;

		free(ap->addr);
		free(ap->netmask);
		free(ap);
		ap = next;
	}
	for (size_t i = 0; i < ifp->ngroups; i++)
		free(ifp->groups[i]);
	free(ifp->groups);
	for (size_t i = 0; i < ifp->nentries; i++)
		xml_args_free(&ifp->entries[i].args);
	free(ifp->entries);
	free(ifp);
}

static void
xml_add_group(struct xml_iface *ifp, const char *group)
{
	if (strcmp(group, "all") == 0)
		return;
	xml_strlist_add(&ifp->groups, &ifp->ngroups, &ifp->groupcap, group);
}

static struct xml_addr *
xml_addr_alloc(int family)
{
	struct xml_addr *ap = calloc(1, sizeof(*ap));

	if (ap == NULL)
		err(1, "calloc");
	ap->family = family;
	ap->prefixlen = -1;
	return (ap);
}

static void
xml_commit_address(struct xml_parse_ctx *ctx)
{
	struct xml_addr *ap = ctx->cur_inet;

	if (ap == NULL)
		ap = ctx->cur_inet6;
	ctx->cur_inet = NULL;
	ctx->cur_inet6 = NULL;
	if (ap == NULL)
		return;
	if (ap->family == AF_INET6 && ap->addr != NULL &&
	    strncmp(ap->addr, "fe80:", 5) == 0) {
		free(ap->addr);
		free(ap->netmask);
		free(ap);
		return;
	}
	if (ap->addr == NULL) {
		free(ap->netmask);
		free(ap);
		return;
	}
	if (ctx->cur->addrs_tail == NULL)
		ctx->cur->addrs = ap;
	else
		ctx->cur->addrs_tail->next = ap;
	ctx->cur->addrs_tail = ap;
}

static long
xml_text_number(struct xml_parse_ctx *ctx)
{
	char *endp;
	long value;

	value = strtol(ctx->text, &endp, 10);
	if (*endp != '\0')
		return (0);
	return (value);
}

static void XMLCALL
xml_start_element(void *ud, const XML_Char *name,
    const XML_Char **attrs __unused)
{
	struct xml_parse_ctx *ctx = ud;

	ctx->tlen = 0;

	if (ctx->cur == NULL) {
		if (strcmp(name, "interface") == 0)
			ctx->cur = xml_iface_alloc();
		return;
	}

	if (strcmp(name, "address") == 0 && !ctx->in_address) {
		ctx->in_address = true;
		ctx->cur_inet = NULL;
		ctx->cur_inet6 = NULL;
	} else if (strcmp(name, "groups") == 0) {
		ctx->in_groups = true;
	} else if (strcmp(name, "link-address") == 0) {
		ctx->in_linkaddr = true;
		ctx->addrtype[0] = '\0';
	} else if (strcmp(name, "media") == 0) {
		ctx->in_media = true;
	} else if (strcmp(name, "interface-flags") == 0) {
		ctx->in_ifflags = true;
	} else if (strcmp(name, "IFCAPS") == 0) {
		ctx->in_ifcaps = true;
	} else if (strcmp(name, "nd6-options") == 0) {
		ctx->in_nd6opts = true;
	}
}

static void
xml_text_save(struct xml_parse_ctx *ctx)
{
	while (ctx->tlen > 0 &&
	    isspace((unsigned char)ctx->text[ctx->tlen - 1]))
		ctx->tlen--;
	size_t off = 0;
	while (off < ctx->tlen && isspace((unsigned char)ctx->text[off]))
		off++;
	if (off > 0) {
		memmove(ctx->text, ctx->text + off, ctx->tlen - off);
		ctx->tlen -= off;
	}
	ctx->text[ctx->tlen] = '\0';
}

static void XMLCALL
xml_end_element(void *ud, const XML_Char *name)
{
	struct xml_parse_ctx *ctx = ud;
	const struct xml_token *tp;

	xml_text_save(ctx);

	if (ctx->cur == NULL)
		return;

	if (ctx->in_linkaddr) {
		if (strcmp(name, "addr-type") == 0 && ctx->tlen > 0 &&
		    ctx->tlen < sizeof(ctx->addrtype))
			strlcpy(ctx->addrtype, ctx->text, sizeof(ctx->addrtype));
		else if (strcmp(name, "link-addr") == 0 &&
		    strcmp(ctx->addrtype, "ether") == 0)
			ctx->cur->mac = xml_strdup(ctx->text);
		else if (strcmp(name, "link-address") == 0)
			ctx->in_linkaddr = false;
	} else if (ctx->in_media) {
		if ((strcmp(name, "subtype") == 0 ||
		    strcmp(name, "mode") == 0 || strcmp(name, "option") == 0) &&
		    ctx->tlen > 0) {
			if (strcmp(name, "option") == 0) {
				struct xml_kv *kv =
				    xml_kv_find(ctx->cur, "@mediaopt");
				char joined[512];

				if (kv != NULL) {
					snprintf(joined, sizeof(joined),
					    "%s,%s", kv->value, ctx->text);
					free(kv->value);
					kv->value = strdup(joined);
				} else
					xml_kv_add(ctx->cur, "@mediaopt",
					    ctx->text);
			} else
				xml_kv_add(ctx->cur, name, ctx->text);
		} else if (strcmp(name, "media") == 0)
			ctx->in_media = false;
	} else if (ctx->in_ifflags) {
		if (strcmp(name, "flag") == 0 && ctx->tlen > 0) {
			if (strcmp(ctx->text, "UP") == 0)
				ctx->cur->up = true;
			else {
				tp = xml_token_lookup(xml_flag_tokens,
				    nitems(xml_flag_tokens), ctx->text);
				if (tp != NULL)
					xml_strlist_add(&ctx->cur->flags,
					    &ctx->cur->nflags,
					    &ctx->cur->flagcap, tp->token);
			}
		} else if (strcmp(name, "interface-flags") == 0)
			ctx->in_ifflags = false;
	} else if (ctx->in_ifcaps) {
		if (strcmp(name, "ifcap") == 0 && ctx->tlen > 0)
			xml_strlist_add(&ctx->cur->ifcaps,
			    &ctx->cur->nifcaps, &ctx->cur->ifcapcap,
			    ctx->text);
		else if (strcmp(name, "IFCAPS") == 0)
			ctx->in_ifcaps = false;
	} else if (ctx->in_nd6opts) {
		if (strcmp(name, "option") == 0 && ctx->tlen > 0) {
			tp = xml_token_lookup(
			    xml_nd6_tokens, nitems(xml_nd6_tokens), ctx->text);

			if (tp != NULL)
				xml_strlist_add(&ctx->cur->nd6,
				    &ctx->cur->nnd6, &ctx->cur->nd6cap,
				    tp->token);
		} else if (strcmp(name, "nd6-options") == 0)
			ctx->in_nd6opts = false;
	} else if (ctx->in_groups) {
		if (strcmp(name, "group") == 0 && ctx->tlen > 0)
			xml_add_group(ctx->cur, ctx->text);
		else if (strcmp(name, "groups") == 0)
			ctx->in_groups = false;
	} else if (ctx->in_address) {
		if (strcmp(name, "inet-addr") == 0 && ctx->tlen > 0) {
			ctx->cur_inet = xml_addr_alloc(AF_INET);
			ctx->cur_inet->addr = xml_strdup(ctx->text);
		} else if (strcmp(name, "netmask") == 0 &&
		    ctx->cur_inet != NULL) {
			ctx->cur_inet->netmask = xml_strdup(ctx->text);
		} else if (strcmp(name, "inet6-addr") == 0 && ctx->tlen > 0) {
			ctx->cur_inet6 = xml_addr_alloc(AF_INET6);
			ctx->cur_inet6->addr = xml_strdup(ctx->text);
		} else if (strcmp(name, "prefixlen") == 0 &&
		    ctx->cur_inet6 != NULL) {
			ctx->cur_inet6->prefixlen = (int)xml_text_number(ctx);
		} else if (strcmp(name, "vhid") == 0 && ctx->tlen > 0) {
			int vhid = (int)xml_text_number(ctx);

			if (ctx->cur_inet != NULL)
				ctx->cur_inet->vhid = vhid;
			else if (ctx->cur_inet6 != NULL)
				ctx->cur_inet6->vhid = vhid;
		} else if (strcmp(name, "address") == 0) {
			xml_commit_address(ctx);
			ctx->in_address = false;
		}
	} else {
		if (strcmp(name, "ifname") == 0 && ctx->tlen > 0) {
			ctx->cur->name = xml_strdup(ctx->text);
		} else if (strcmp(name, "drivername") == 0 &&
		    ctx->tlen > 0) {
			ctx->cur->drivername = xml_strdup(ctx->text);
		} else {
			if (strcmp(name, "vlantag") == 0 && ctx->tlen > 0)
				ctx->cur->vlanid =
				    (int)xml_text_number(ctx);
			if (!xml_kv_skip(name) && ctx->tlen > 0 &&
			    xml_kw_exists(name))
				xml_kv_add(ctx->cur, name, ctx->text);
		}

		if (strcmp(name, "interface") == 0) {
			if (ctx->tail == NULL)
				ctx->head = ctx->cur;
			else
				ctx->tail->next = ctx->cur;
			ctx->tail = ctx->cur;
			ctx->cur = NULL;
		}
	}

	ctx->tlen = 0;
}

static void XMLCALL
xml_char_data(void *ud, const XML_Char *s, int len)
{
	struct xml_parse_ctx *ctx = ud;
	int copy = len;

	if (ctx->cur == NULL)
		return;
	if (copy > XML_TEXT_MAX - 1 - (int)ctx->tlen)
		copy = XML_TEXT_MAX - 1 - (int)ctx->tlen;
	if (copy <= 0)
		return;
	memcpy(ctx->text + ctx->tlen, s, copy);
	ctx->tlen += copy;
	ctx->text[ctx->tlen] = '\0';
}

static void
xml_parse_file(const char *path, struct xml_parse_ctx *ctx)
{
	XML_Parser parser = XML_ParserCreate(NULL);
	char buf[65536];
	FILE *fp;
	size_t len;
	bool done;

	fp = fopen(path, "re");
	if (fp == NULL)
		err(1, "%s", path);
	if (parser == NULL)
		errx(1, "%s: cannot create XML parser", path);
	XML_SetUserData(parser, ctx);
	XML_SetElementHandler(parser, xml_start_element, xml_end_element);
	XML_SetCharacterDataHandler(parser, xml_char_data);

	do {
		len = fread(buf, 1, sizeof(buf), fp);
		done = len < sizeof(buf);
		if (ferror(fp))
			err(1, "%s", path);
		if (XML_Parse(parser, buf, (int)len, done) == XML_STATUS_ERROR)
			errx(1, "%s: %s at line %lu", path,
			    XML_ErrorString(XML_GetErrorCode(parser)),
			    XML_GetCurrentLineNumber(parser));
	} while (!done);
	fclose(fp);
	XML_ParserFree(parser);
}

static const struct xml_kw *
xml_kw_row(const char *elem, const char *family)
{
	const struct xml_kw *found = NULL;

	for (size_t i = 0; i < nitems(xml_kws); i++) {
		if (strcmp(xml_kws[i].elem, elem) != 0)
			continue;
		if (xml_kws[i].family == NULL)
			return (&xml_kws[i]);
		if (family != NULL && strcmp(family, xml_kws[i].family) == 0)
			found = &xml_kws[i];
	}
	return (found);
}

static bool
xml_kw_exists(const char *elem)
{
	for (size_t i = 0; i < nitems(xml_kws); i++)
		if (strcmp(xml_kws[i].elem, elem) == 0)
			return (true);
	return (false);
}

static void
xml_build_entries(struct xml_iface *ifp, const char *base)
{
	struct xml_cmdentry *e;
	char buf[64];

	e = xml_entry_new(ifp, 0);

	for (size_t i = 0; i < ifp->nifcaps; i++) {
		const struct xml_token *tp = xml_token_lookup(
		    xml_cap_tokens, nitems(xml_cap_tokens), ifp->ifcaps[i]);

		if (tp != NULL)
			xml_argv_push(&e->args, tp->token);
	}
	if (xml_strlist_has(ifp->ifcaps, ifp->nifcaps, "TOE4") &&
	    xml_strlist_has(ifp->ifcaps, ifp->nifcaps, "TOE6"))
		xml_argv_push(&e->args, "toe");
	if (xml_strlist_has(ifp->ifcaps, ifp->nifcaps, "TXTLS4") &&
	    xml_strlist_has(ifp->ifcaps, ifp->nifcaps, "TXTLS6"))
		xml_argv_push(&e->args, "txtls");
	for (size_t i = 0; i < ifp->nflags; i++)
		xml_argv_push(&e->args, ifp->flags[i]);
	for (size_t i = 0; i < ifp->nnd6; i++)
		xml_argv_push(&e->args, ifp->nd6[i]);

	for (size_t i = 0; i < ifp->nkvs; i++) {
		struct xml_kv *kv = &ifp->kvs[i];
		const struct xml_kw *kw = xml_kw_row(kv->key, base);
		bool dup = false;

		for (size_t j = 0; j < i; j++) {
			if (strcmp(ifp->kvs[j].key, kv->key) == 0) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		if (kw == NULL)
			continue;
		if (strcmp(kw->kw, "@tunnelpair") == 0) {
			struct xml_kv *dst = xml_kv_find(ifp, "tunnel-dst");

			if (dst == NULL)
				continue;
			xml_argv_push(&e->args, "tunnel");
			xml_argv_push(&e->args, kv->value);
			xml_argv_push(&e->args, dst->value);
			continue;
		}
		if (strcmp(kw->kw, "@mediaopt") == 0) {
			xml_argv_push(&e->args, "mediaopt");
			xml_argv_push(&e->args, kv->value);
			continue;
		}
		xml_argv_push(&e->args, kw->kw);
		xml_argv_push(&e->args, kv->value);
	}

	for (size_t i = 0; i < ifp->ngroups; i++) {
		xml_argv_push(&e->args, "group");
		xml_argv_push(&e->args, ifp->groups[i]);
	}

	for (struct xml_addr *ap = ifp->addrs; ap != NULL; ap = ap->next) {
		e = xml_entry_new(ifp, 0);
		if (ap->family == AF_INET) {
			e->args.afp = af_getbyname("inet");
			if (ap->netmask != NULL) {
				int plen = xml_mask_to_prefixlen(ap->netmask);

				if (plen >= 0 && plen <= 32) {
					snprintf(buf, sizeof(buf), "%s/%d",
					    ap->addr, plen);
					xml_argv_push(&e->args, buf);
				} else {
					xml_argv_push(&e->args, ap->addr);
					xml_argv_push(&e->args, "netmask");
					xml_argv_push(&e->args, ap->netmask);
				}
			} else {
				xml_argv_push(&e->args, ap->addr);
			}
		} else if (ap->family == AF_INET6) {
			e->args.afp = af_getbyname("inet6");
			xml_argv_push(&e->args, ap->addr);
			xml_argv_push(&e->args, "prefixlen");
			snprintf(buf, sizeof(buf), "%d",
			    ap->prefixlen < 0 ? 64 : ap->prefixlen);
			xml_argv_push(&e->args, buf);
		} else {
			continue;
		}
		if (ap->vhid > 0) {
			xml_argv_push(&e->args, "vhid");
			snprintf(buf, sizeof(buf), "%d", ap->vhid);
			xml_argv_push(&e->args, buf);
		}
	}

	if (ifp->up)
		xml_argv_push(&e->args, "up");
}

static void
xml_basename(const char *name, char *dst, size_t dstlen)
{
	size_t len = strlen(name);

	while (len > 0 && isdigit((unsigned char)name[len - 1]))
		len--;
	if (len >= dstlen)
		len = dstlen - 1;
	memcpy(dst, name, len);
	dst[len] = '\0';
}

static void
xml_cloners_load(struct xml_cloners *cl)
{
	char *raw = NULL;
	size_t count = 0;

	memset(cl, 0, sizeof(*cl));
	if (ifconfig_list_cloners(lifh, &raw, &count) < 0 || raw == NULL)
		return;
	cl->names = calloc(count, sizeof(*cl->names));
	if (cl->names == NULL)
		err(1, "calloc");
	for (size_t i = 0; i < count; i++) {
		cl->names[i] = strdup(raw + i * IFNAMSIZ);
		if (cl->names[i] == NULL)
			err(1, "strdup");
	}
	cl->count = count;
	free(raw);
}

static void
xml_cloners_free(struct xml_cloners *cl)
{
	for (size_t i = 0; i < cl->count; i++)
		free(cl->names[i]);
	free(cl->names);
	cl->names = NULL;
	cl->count = 0;
}

static bool
xml_base_is_cloner(const struct xml_cloners *cl, const char *base)
{
	for (size_t i = 0; i < cl->count; i++)
		if (strcmp(cl->names[i], base) == 0)
			return (true);
	return (false);
}

static bool
xml_is_clonable(const struct xml_iface *ifp, const struct xml_cloners *cl)
{
	char base[IFNAMSIZ];

	xml_basename(ifp->drivername != NULL ? ifp->drivername : ifp->name,
	    base, sizeof(base));
	return (xml_base_is_cloner(cl, base));
}

static bool
xml_name_matches_create(const struct xml_iface *ifp, const char *createname)
{
	size_t clen = strlen(createname);

	if (strcmp(ifp->name, createname) == 0)
		return (true);
	if (strncmp(ifp->name, createname, clen) != 0)
		return (false);
	return ((ifp->name[clen] == 'a' || ifp->name[clen] == 'b') &&
	    ifp->name[clen + 1] == '\0');
}

static bool
xml_create_name(const struct xml_iface *ifp, const struct xml_cloners *cl,
    char *dst, size_t dstlen)
{
	char base[IFNAMSIZ];
	size_t len, ulen;

	xml_basename(ifp->name, base, sizeof(base));
	if (strchr(ifp->name, '.') != NULL ||
	    xml_base_is_cloner(cl, base)) {
		strlcpy(dst, ifp->name, dstlen);
		return (true);
	}

	if (ifp->drivername == NULL || ifp->drivername[0] == '\0')
		return (false);
	xml_basename(ifp->drivername, base, sizeof(base));
	if (!xml_base_is_cloner(cl, base))
		return (false);

	len = strlen(ifp->drivername);
	if (len > 0 && isdigit((unsigned char)ifp->drivername[len - 1])) {
		strlcpy(dst, ifp->drivername, dstlen);
		return (true);
	}
	len = strlen(ifp->name);
	ulen = len;
	while (ulen > 0 && isdigit((unsigned char)ifp->name[ulen - 1]))
		ulen--;
	if (ulen == 0 || ulen == len) {
		strlcpy(dst, ifp->drivername, dstlen);
		return (true);
	}
	snprintf(dst, dstlen, "%s%s", ifp->drivername, ifp->name + ulen);
	return (true);
}

static void
xml_run_simple(const char *ifname, const char *arg0, const char *arg1,
    int iscreate, bool pretend)
{
	struct xml_cmdentry e;

	memset(&e, 0, sizeof(e));
	e.iscreate = iscreate;
	e.args.ifname = ifname;
	xml_argv_push(&e.args, arg0);
	if (arg1 != NULL)
		xml_argv_push(&e.args, arg1);
	xml_exec(&e, pretend);
	xml_args_free(&e.args);
}

int
ifconfig_xml_restore(const struct ifconfig_args *args)
{
	struct xml_parse_ctx pctx = {};
	struct xml_iface *ifp;
	struct xml_cloners cloners;
	bool pretend = args->restore_pretend;
	int rc = 0;

	if (!ifconfig_style_is_text())
		if_warnx("xml/json output is not supported for restore");

	xml_parse_file(args->restore_file, &pctx);

	for (struct xml_iface *ip = pctx.head; ip != NULL; ip = ip->next) {
		if (ip->name == NULL || ip->name[0] == '\0') {
			if_warnx("skipping interface with no name");
			rc = 1;
		} else if (ip->drivername == NULL ||
		    ip->drivername[0] == '\0') {
			ip->drivername = strdup(ip->name);
			if (ip->drivername == NULL)
				err(1, "strdup");
		}
	}

	xml_cloners_load(&cloners);

	for (int pass = 0; pass < 2; pass++) {
		for (ifp = pctx.head; ifp != NULL; ifp = ifp->next) {
			unsigned int ifindex;
			bool create, clonable;
			char base[IFNAMSIZ];

			if (ifp->name == NULL || ifp->name[0] == '\0')
				continue;
			if ((pass == 0) == (ifp->vlanid >= 0))
				continue;

			xml_basename(ifp->drivername, base, sizeof(base));
			xml_build_entries(ifp, base);

			ifindex = if_nametoindex(ifp->name);
			create = (ifindex == 0);
			clonable = xml_is_clonable(ifp, &cloners);

			if (!create && !args->restore_force) {
				if (pretend)
					printf("# %s exists\n", ifp->name);
				continue;
			}

			if (create && !clonable) {
				if_warnx("%s: does not exist and %s is not "
				    "a cloning driver", ifp->name,
				    ifp->drivername);
				rc = 1;
				continue;
			}

			if (!create && args->restore_force) {
				if (!clonable) {
					if_warnx("%s: not a cloned interface; "
					    "configuring in place",
					    ifp->name);
				} else {
					xml_run_simple(ifp->name, "destroy",
					    NULL, 0, pretend);
					create = true;
				}
			}

			if (create) {
				char createname[IFNAMSIZ];

				if (!xml_create_name(ifp, &cloners,
				    createname, sizeof(createname))) {
					if_warnx("%s: unable to determine "
					    "a cloner for creation",
					    ifp->name);
					rc = 1;
					continue;
				}
				ifmaybeload(&ifp->entries[0].args, createname);
				xml_run_simple(createname, "create", NULL,
				    1, pretend);
				if (!xml_name_matches_create(ifp, createname))
					xml_run_simple(createname, "name",
					    ifp->name, 0, pretend);
			}

			for (size_t i = 0; i < ifp->nentries; i++) {
				struct xml_cmdentry *e = &ifp->entries[i];

				if (e->args.argc == 0)
					continue;
				xml_exec(e, pretend);
			}
		}
	}

	xml_cloners_free(&cloners);
	ifp = pctx.head;
	while (ifp != NULL) {
		struct xml_iface *next = ifp->next;

		xml_iface_free(ifp);
		ifp = next;
	}
	if (rc == 0 && pctx.head == NULL) {
		if_warnx("%s: no interfaces found", args->restore_file);
		rc = 1;
	}
	return (rc);
}

#endif
