#define _WANT_IFCAP_BIT_NAMES

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/nv.h>

#include <net/ethernet.h>
#include <net/ieee8023ad_lacp.h>
#include <net/if.h>
#include <net/if_bridgevar.h>
#include <net/if_dl.h>
#include <net/if_lagg.h>
#include <net/if_media.h>
#include <net/if_pfsync.h>
#include <net/if_stf.h>
#include <net/if_strings.h>
#include <net/if_types.h>
#include <net/if_vlan_var.h>
#include <net/if_vxlan.h>
#include <netinet/in.h>
#include <netinet/in_var.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <ifaddrs.h>
#include <langinfo.h>
#include <libifconfig_sfp.h>
#include <libutil.h>
#include <locale.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WITHOUT_NETLINK
#include <netlink/netlink.h>
#include <netlink/netlink_route.h>
#include <netlink/netlink_snl.h>
#include <netlink/netlink_snl_route.h>
#include <netlink/netlink_snl_route_compat.h>
#include <netlink/netlink_snl_route_parsers.h>
#endif

#include "af_inet6.h"
#include "ifbridge.h"
#include "ifconfig.h"
#include "ifconfig_output.h"
#include "ifieee80211.h"
#include "iflagg.h"

#ifdef WITH_LIBXO
#include <libxo/xo.h>
#define IFCONFIG_XO_VERSION "1"
#endif

static char addr_buf[NI_MAXHOST]; /*for getnameinfo()*/

void
af_inet_print_addr(struct sockaddr_in *sin)
{
	int error, n_flags;

	if (f_addr != NULL && strcmp(f_addr, "fqdn") == 0)
		n_flags = 0;
	else if (f_addr != NULL && strcmp(f_addr, "host") == 0)
		n_flags = NI_NOFQDN;
	else
		n_flags = NI_NUMERICHOST;

	error = getnameinfo((struct sockaddr *)sin, sin->sin_len, addr_buf,
	    sizeof(addr_buf), NULL, 0, n_flags);

	if (error)
		inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf));

#ifdef WITH_LIBXO
	xo_emit("\tinet {:inet-addr/%s}", addr_buf);
#else
	printf("\tinet %s", addr_buf);
#endif
}

void
af_inet6_print_addr(struct sockaddr_in6 *sin)
{
	int error, n_flags;

	if (f_addr != NULL && strcmp(f_addr, "fqdn") == 0)
		n_flags = 0;
	else if (f_addr != NULL && strcmp(f_addr, "host") == 0)
		n_flags = NI_NOFQDN;
	else
		n_flags = NI_NUMERICHOST;
	error = getnameinfo((struct sockaddr *)sin, sin->sin6_len, addr_buf,
	    sizeof(addr_buf), NULL, 0, n_flags);
	if (error != 0)
		inet_ntop(AF_INET6, &sin->sin6_addr, addr_buf,
		    sizeof(addr_buf));
#ifdef WITH_LIBXO
	xo_emit("\tinet6 {:inet6-addr/%s}", addr_buf);
#else
	printf("\tinet6 %s", addr_buf);
#endif
}

void
af_inet6_print_pointtopoint(struct sockaddr_in6 *sin)
{
	int error;

	error = getnameinfo((struct sockaddr *)sin, sin->sin6_len, addr_buf,
	    sizeof(addr_buf), NULL, 0, NI_NUMERICHOST);

	if (error != 0)
		inet_ntop(AF_INET6, &sin->sin6_addr, addr_buf,
		    sizeof(addr_buf));
#ifdef WITH_LIBXO
	xo_emit(" --> {:dst-addr/%s}", addr_buf);
#else
	printf(" --> %s", addr_buf);
#endif
}

void
af_inet6_print_mask(int plen)
{
	if (f_inet6 != NULL && strcmp(f_inet6, "cidr") == 0)
#ifdef WITH_LIBXO
		xo_emit("/{:prefixlen/%d}", plen);
#else
		printf("/%d", plen);
#endif
	else
#ifdef WITH_LIBXO
		xo_emit(" prefixlen {:prefixlen/%d}", plen);
#else
		printf(" prefixlen %d", plen);
#endif
}

void
af_inet6_print_flags(int flags6)
{
#ifdef WITH_LIBXO
	xo_open_list("flags6");
	if ((flags6 & IN6_IFF_ANYCAST) != 0)
		xo_emit(" {P:/anycast}{le:flags6/%s}", "anycast");
	if ((flags6 & IN6_IFF_TENTATIVE) != 0)
		xo_emit(" {P:/tentative}{le:flags6/%s}", "tentative");
	if ((flags6 & IN6_IFF_DUPLICATED) != 0)
		xo_emit(" {P:/duplicated}{le:flags6/%s}", "duplicated");
	if ((flags6 & IN6_IFF_DETACHED) != 0)
		xo_emit(" {P:/detached}{le:flags6/%s}", "detached");
	if ((flags6 & IN6_IFF_DEPRECATED) != 0)
		xo_emit(" {P:/deprecated}{le:flags6/%s}", "deprecated");
	if ((flags6 & IN6_IFF_AUTOCONF) != 0)
		xo_emit(" {P:/autoconf}{le:flags6/%s}", "autoconf");
	if ((flags6 & IN6_IFF_TEMPORARY) != 0)
		xo_emit(" {P:/temporary}{le:flags6/%s}", "temporary");
	if ((flags6 & IN6_IFF_PREFER_SOURCE) != 0)
		xo_emit(" {P:/prefer_source}{le:flags6/%s}", "prefer_source");
	xo_close_list("flags6");
#else
	if ((flags6 & IN6_IFF_ANYCAST) != 0)
		printf(" anycast");
	if ((flags6 & IN6_IFF_TENTATIVE) != 0)
		printf(" tentative");
	if ((flags6 & IN6_IFF_DUPLICATED) != 0)
		printf(" duplicated");
	if ((flags6 & IN6_IFF_DETACHED) != 0)
		printf(" detached");
	if ((flags6 & IN6_IFF_DEPRECATED) != 0)
		printf(" deprecated");
	if ((flags6 & IN6_IFF_AUTOCONF) != 0)
		printf(" autoconf");
	if ((flags6 & IN6_IFF_TEMPORARY) != 0)
		printf(" temporary");
	if ((flags6 & IN6_IFF_PREFER_SOURCE) != 0)
		printf(" prefer_source");
#endif
}

void
af_inet6_print_lifetime(const char *prepend, time_t px_time,
    struct timespec *now)
{
#ifdef WITH_LIBXO
	xo_emit(" {P:/%s}", prepend);
	if (px_time == 0)
		xo_emit(" {P:/infty}");

	xo_emit(" {P:/%s}",
	    px_time < now->tv_sec ? "0" : sec2str(px_time - now->tv_sec));
#else
	printf(" %s", prepend);
	if (px_time == 0)
		printf(" infty");

	printf(" %s",
	    px_time < now->tv_sec ? "0" : sec2str(px_time - now->tv_sec));
#endif
}

void
af_link_print_ether(const struct ether_addr *addr, const char *prefix)
{
	char *ether_format = ether_ntoa(addr);

	if (f_ether != NULL) {
		if (strcmp(f_ether, "dash") == 0) {
			char *format_char;

			while (
			    (format_char = strchr(ether_format, ':')) != NULL) {
				*format_char = '-';
			}
		} else if (strcmp(f_ether, "dotted") == 0) {
			/* Indices 0 and 1 is kept as is. */
			ether_format[2] = ether_format[3];
			ether_format[3] = ether_format[4];
			ether_format[4] = '.';
			ether_format[5] = ether_format[6];
			ether_format[6] = ether_format[7];
			ether_format[7] = ether_format[9];
			ether_format[8] = ether_format[10];
			ether_format[9] = '.';
			ether_format[10] = ether_format[12];
			ether_format[11] = ether_format[13];
			ether_format[12] = ether_format[15];
			ether_format[13] = ether_format[16];
			ether_format[14] = '\0';
		}
	}
#ifdef WITH_LIBXO
	xo_open_container("link-address");
	xo_emit("{P:\t}{:addr-type/%s} {:link-addr/%s}\n", prefix,
	    ether_format);
	xo_close_container("link-address");
#else
	printf("\t%s %s\n", prefix, ether_format);
#endif
}

void
af_link_print_lladdr(struct sockaddr_dl *sdl)
{
	if (match_ether(sdl)) {
		af_link_print_ether((struct ether_addr *)LLADDR(sdl), "ether");
	} else {
		int n = sdl->sdl_nlen > 0 ? sdl->sdl_nlen + 1 : 0;
#ifdef WITH_LIBXO
		xo_open_container("link-address");
		xo_emit("{P:\t}lladdr {:link-addr/%s}\n", link_ntoa(sdl) + n);
		xo_close_container("link-address");
#else
		printf("\tlladdr %s\n", link_ntoa(sdl) + n);
#endif
	}
}

void
af_link_print_pcp(if_ctx *ctx)
{
	struct ifreq ifr = {};

	if (ioctl_ctx_ifr(ctx, SIOCGLANPCP, &ifr) == 0 &&
	    ifr.ifr_lan_pcp != IFNET_PCP_NONE)
#ifdef WITH_LIBXO
		xo_emit("\tpcp {:pcp/%d}\n", ifr.ifr_lan_pcp);
#else
		printf("\tpcp %d\n", ifr.ifr_lan_pcp);
#endif
}

void
ifbridge_print_vlans(ifbvlan_set_t *vlans)
{
	unsigned printed = 0;

	for (unsigned vlan = DOT1Q_VID_MIN; vlan <= DOT1Q_VID_MAX;) {
		unsigned last;

		if (!BRVLAN_TEST(vlans, vlan)) {
			++vlan;
			continue;
		}

		last = vlan;
		while (last < DOT1Q_VID_MAX && BRVLAN_TEST(vlans, last + 1))
			++last;

#ifdef WITH_LIBXO
		if (printed == 0)
			xo_emit("{P: tagged }");
		else
			xo_emit("{P:,}");

		xo_open_list("vlan");
		xo_open_container("vlan");
		xo_emit("{:vlan-id/%d}", vlan);
		if (last != vlan)
			xo_emit("{P:-}{:vlan-end/%d}", last);
		xo_close_container("vlan");
		xo_close_list("vlan");
		++printed;
		vlan = last + 1;
#else
		if (printed == 0)
			printf(" tagged ");
		else
			printf(",");

		printf("%u", vlan);
		if (last != vlan)
			printf("-%u", last);
		++printed;
		vlan = last + 1;
#endif
	}
}

void
ifconfig_printifnamemaybe(void)
{
	if (ifconfig_ifname_to_print[0] != '\0')
#ifdef WITH_LIBXO
		xo_emit("{:ifname-print/%s}\n", ifconfig_ifname_to_print);
#else
		printf("%s\n", ifconfig_ifname_to_print);
#endif
}

int
ifconfig_parse_args(int ac, char *av[] __unused)
{
#ifdef WITH_LIBXO
	ac = xo_parse_args(ac, av);
	if (ac < 0)
		exit(EXIT_FAILURE);
	xo_set_flags(NULL, XOF_PRETTY);

	xo_set_version(IFCONFIG_XO_VERSION);
#endif
	return (ac);
}

void
ifconfig_finish(void)
{
#ifdef WITH_LIBXO
	xo_finish();
#endif
}

void
ifconfig_open_container(const char *name __unused)
{
#ifdef WITH_LIBXO
	xo_open_container(name);
#endif
}

void
ifconfig_close_container(const char *name __unused)
{
#ifdef WITH_LIBXO
	xo_close_container(name);
#endif
}

void
ifconfig_open_list(const char *name __unused)
{
#ifdef WITH_LIBXO
	xo_open_list(name);
#endif
}

void
ifconfig_close_list(const char *name __unused)
{
#ifdef WITH_LIBXO
	xo_close_list(name);
#endif
}

void
ifconfig_open_instance(const char *name __unused)
{
#ifdef WITH_LIBXO
	xo_open_instance(name);
#endif
}

void
ifconfig_close_instance(const char *name __unused)
{
#ifdef WITH_LIBXO
	xo_close_instance(name);
#endif
}

#ifdef WITHOUT_NETLINK
void
ifconfig_print_ifcap_nv(if_ctx *ctx)
{
	struct ifreq ifr = {};
	nvlist_t *nvcap;
	const char *nvname;
	void *buf, *cookie;
	bool first, val;
	int type;

	buf = malloc(IFR_CAP_NV_MAXBUFSIZE);
	if (buf == NULL)
		Perror("malloc");
	ifr.ifr_cap_nv.buffer = buf;
	ifr.ifr_cap_nv.buf_length = IFR_CAP_NV_MAXBUFSIZE;
	if (ioctl_ctx_ifr(ctx, SIOCGIFCAPNV, &ifr) != 0)
		Perror("ioctl (SIOCGIFCAPNV)");
	nvcap = nvlist_unpack(ifr.ifr_cap_nv.buffer, ifr.ifr_cap_nv.length, 0);
	if (nvcap == NULL)
		Perror("nvlist_unpack");
#ifdef WITH_LIBXO
	xo_emit("\toptions");
#else
	printf("\toptions");
#endif
	cookie = NULL;
#ifdef WITH_LIBXO
	xo_open_list("options");
#endif
	for (first = true;; first = false) {
		nvname = nvlist_next(nvcap, &type, &cookie);
		if (nvname == NULL) {
			ifconfig_print_newline();
			break;
		}
		if (type == NV_TYPE_BOOL) {
			val = nvlist_get_bool(nvcap, nvname);
			if (val) {
#ifdef WITH_LIBXO
				xo_emit("{P:/%c%s}", first ? ' ' : ',', nvname);
				xo_emit("{le:options/%s}", nvname);
#else
				printf("%c%s", first ? ' ' : ',', nvname);
#endif
			}
		}
	}
#ifdef WITH_LIBXO
	xo_close_list("options");
#endif
	if (ctx->args->supmedia) {
#ifdef WITH_LIBXO
		xo_emit("\tcapabilities");
#else
		printf("\tcapabilities");
#endif
		cookie = NULL;
#ifdef WITH_LIBXO
		xo_open_list("capabilities");
#endif
		for (first = true;; first = false) {
			nvname = nvlist_next(nvcap, &type, &cookie);
			if (nvname == NULL) {
				ifconfig_print_newline();
				break;
			}
			if (type == NV_TYPE_BOOL)
#ifdef WITH_LIBXO
			{
				xo_emit("{P:/%c%s}", first ? ' ' : ',', nvname);
				xo_emit("{le:capabilities/%s}", nvname);
			}
#else
				printf("%c%s", first ? ' ' : ',', nvname);
#endif
		}
#ifdef WITH_LIBXO
		xo_close_list("capabilities");
#endif
	}
	nvlist_destroy(nvcap);
	free(buf);

	if (ioctl_ctx(ctx, SIOCGIFCAP, (caddr_t)&ifr) != 0)
		Perror("ioctl (SIOCGIFCAP)");
}

void
ifconfig_print_ifcap(if_ctx *ctx)
{
	struct ifreq ifr = {};

	if (ioctl_ctx_ifr(ctx, SIOCGIFCAP, &ifr) != 0)
		return;

	if ((ifr.ifr_curcap & IFCAP_NV) != 0)
		ifconfig_print_ifcap_nv(ctx);
	else {
#ifdef WITH_LIBXO
		xo_emit("\toptions={:ifcap-options-hex/%x}", ifr.ifr_curcap);
		ifconfig_print_bits("options", "option", &ifr.ifr_curcap, 1,
		    IFCAPBITS, nitems(IFCAPBITS));

		ifconfig_print_newline();

		if (ctx->args->supmedia && ifr.ifr_reqcap != 0) {
			xo_emit("\tcapabilities={:capab-hex/%x}",
			    ifr.ifr_reqcap);
			ifconfig_print_bits("capabilities", "capability",
			    &ifr.ifr_reqcap, 1, IFCAPBITS, nitems(IFCAPBITS));
			ifconfig_print_newline();
		}
#else
		printf("\toptions=%x", ifr.ifr_curcap);
		ifconfig_print_bits("options", "option", &ifr.ifr_curcap, 1,
		    IFCAPBITS, nitems(IFCAPBITS));
		ifconfig_print_newline();
		if (ctx->args->supmedia && ifr.ifr_reqcap != 0) {
			printf("\tcapabilities=%x", ifr.ifr_reqcap);
			ifconfig_print_bits("capabilities", "capability",
			    &ifr.ifr_reqcap, 1, IFCAPBITS, nitems(IFCAPBITS));
			ifconfig_print_newline();
		}
#endif
	}
}
#endif

void
ifconfig_print_ifstatus(if_ctx *ctx)
{
	struct ifstat ifs;

	strlcpy(ifs.ifs_name, ctx->ifname, sizeof ifs.ifs_name);
	if (ioctl_ctx(ctx, SIOCGIFSTATUS, &ifs) == 0)
#ifdef WITH_LIBXO
	{
		char *p = ifs.ascii;

		while (*p == '\t' || *p == ' ' || *p == '\n')
			p++;
		size_t len = strlen(p);
		while (len > 0 &&
		    (p[len - 1] == '\n' || p[len - 1] == '\t' ||
			p[len - 1] == ' '))
			p[--len] = '\0';
		xo_emit("{P:\t}{:if-status/%s}\n", p);
	}
#else
		printf("%s", ifs.ascii);
#endif
}

void
ifconfig_print_metric(if_ctx *ctx)
{
	struct ifreq ifr = {};

	if (ioctl_ctx_ifr(ctx, SIOCGIFMETRIC, &ifr) != -1)
#ifdef WITH_LIBXO
		xo_emit(" metric {:metric/%d}", ifr.ifr_metric);
#else
		printf(" metric %d", ifr.ifr_metric);
#endif
}

#ifdef WITHOUT_NETLINK
void
ifconfig_print_mtu(if_ctx *ctx)
{
	struct ifreq ifr = {};

	if (ioctl_ctx_ifr(ctx, SIOCGIFMTU, &ifr) != -1)
#ifdef WITH_LIBXO
		xo_emit(" mtu {:mtu/%d}", ifr.ifr_mtu);
#else
		printf(" mtu %d", ifr.ifr_mtu);
#endif
}

void
ifconfig_print_description(if_ctx *ctx)
{
	struct ifreq ifr = {};

	ifr_set_name(&ifr, ctx->ifname);
	for (;;) {
		if ((descr = reallocf(descr, descrlen)) != NULL) {
			ifr.ifr_buffer.buffer = descr;
			ifr.ifr_buffer.length = descrlen;
			if (ioctl_ctx(ctx, SIOCGIFDESCR, &ifr) == 0) {
				if (ifr.ifr_buffer.buffer == descr) {
					if (strlen(descr) > 0)
#ifdef WITH_LIBXO
						xo_emit(
						    "\tdescription: {:descr/%s}\n",
						    descr);
#else
						printf("\tdescription: %s\n",
						    descr);
#endif
				} else if (ifr.ifr_buffer.length > descrlen) {
					descrlen = ifr.ifr_buffer.length;
					continue;
				}
			}
		} else
			if_warn("unable to allocate memory for interface"
				"description");
		break;
	}
}
#endif

void
ifconfig_print_bits(const char *btype, const char *child __unused, uint32_t *v,
    const int v_count, const char **names, const int n_count)
{
	int num = 0;
#ifdef WITH_LIBXO
	char tag_fmt[64];

	snprintf(tag_fmt, sizeof(tag_fmt), "{le:%s/%%s}", btype);
	/*
	 * A JSON list of scalars has no element names, so for XML we emit
	 * a container with named children instead of repeated leaf elements.
	 */
	if (xo_get_style(NULL) == XO_STYLE_XML) {
		char child_fmt[64];

		snprintf(child_fmt, sizeof(child_fmt), "{:%s/%%s}", child);
		ifconfig_open_container(btype);
		for (int i = 0; i < v_count * 32; i++) {
			bool is_set = v[i / 32] & (1U << (i % 32));
			if (is_set) {
				if (i < n_count)
					xo_emit(child_fmt, names[i]);
				else {
					char buf[64];

					snprintf(buf, sizeof(buf), "%s_%d",
					    btype, i);
					xo_emit(child_fmt, buf);
				}
			}
		}
		ifconfig_close_container(btype);
		return;
	}
	xo_open_list(btype);
	for (int i = 0; i < v_count * 32; i++) {
		bool is_set = v[i / 32] & (1U << (i % 32));
		if (is_set) {
			if (num++ == 0)
				xo_emit("{P:<}");
			if (num != 1)
				xo_emit("{P:,}");
			if (i < n_count) {
				xo_emit("{P:/%s}", names[i]);
				xo_emit(tag_fmt, names[i]);
			} else {
				char buf[64];

				snprintf(buf, sizeof(buf), "%s_%d", btype, i);
				xo_emit("{P:/%s}", buf);
				xo_emit(tag_fmt, buf);
			}
		}
	}
	if (num > 0)
		xo_emit("{P:>}");
	xo_close_list(btype);
#else
	for (int i = 0; i < v_count * 32; i++) {
		bool is_set = v[i / 32] & (1U << (i % 32));
		if (is_set) {
			if (num++ == 0)
				printf("<");
			if (num != 1)
				printf(",");
			if (i < n_count)
				printf("%s", names[i]);
			else
				printf("%s_%d", btype, i);
		}
	}
	if (num > 0)
		printf(">");
#endif
}

/*
 * Print a value a la the %b format of the kernel's printf
 */
void
ifconfig_printb(const char *s, unsigned v, const char *bits)
{
	int i, any = 0;
	char c;
#ifdef WITH_LIBXO
	if (bits && *bits == 8)
		xo_emit("{P:/%s=%o}", s, v);
	else
		xo_emit("{P:/%s=%x}", s, v);
	if (bits) {
		bits++;
		xo_emit("{P:<}");
		while ((i = *bits++) != '\0') {
			if (v & (1u << (i - 1))) {
				if (any)
					xo_emit("{P:,}");
				any = 1;
				for (; (c = *bits) > 32; bits++)
					xo_emit("{P:/%c}", c);
			} else
				for (; *bits > 32; bits++)
					;
		}
		xo_emit("{P:>}");
	}
#else
	if (bits && *bits == 8)
		printf("%s=%o", s, v);
	else
		printf("%s=%x", s, v);
	if (bits) {
		bits++;
		putchar('<');
		while ((i = *bits++) != '\0') {
			if (v & (1u << (i - 1))) {
				if (any)
					putchar(',');
				any = 1;
				for (; (c = *bits) > 32; bits++)
					putchar(c);
			} else
				for (; *bits > 32; bits++)
					;
		}
		putchar('>');
	}
#endif
}

void
ifconfig_print_vhid(const struct ifaddrs *ifa)
{
	struct if_data *ifd;

	if (ifa->ifa_data == NULL)
		return;

	ifd = ifa->ifa_data;
	if (ifd->ifi_vhid == 0)
		return;
#ifdef WITH_LIBXO
	xo_emit(" vhid {:vhid/%d}", ifd->ifi_vhid);
#else
	printf(" vhid %d", ifd->ifi_vhid);
#endif
}

void
ifconfig_netlink_print_ifcaps(if_ctx *ctx, if_link_t *link)
{
	uint32_t sz_u32 = roundup2(link->iflaf_caps.nla_bitset_size, 32) / 32;
#ifdef WITH_LIBXO
	if (sz_u32 > 0) {
		uint32_t *caps = link->iflaf_caps.nla_bitset_value;

		xo_emit("\toptions={:options/%x}", caps[0]);
		ifconfig_print_bits("interface-capabilities", "capability",
		    caps, sz_u32, ifcap_bit_names, nitems(ifcap_bit_names));
		ifconfig_print_newline();
	}

	if (ctx->args->supmedia && sz_u32 > 0) {
		uint32_t *caps = link->iflaf_caps.nla_bitset_mask;

		xo_emit("\tcapabilities={:capabilities/%x}", caps[0]);
		ifconfig_print_bits("interface-capabilities", "capability",
		    caps, sz_u32, ifcap_bit_names, nitems(ifcap_bit_names));
		ifconfig_print_newline();
	}
#else
	if (sz_u32 > 0) {
		uint32_t *caps = link->iflaf_caps.nla_bitset_value;

		printf("\toptions=%x", caps[0]);
		ifconfig_print_bits("IFCAPS", "ifcap", caps, sz_u32,
		    ifcap_bit_names, nitems(ifcap_bit_names));
		ifconfig_print_newline();
	}

	if (ctx->args->supmedia && sz_u32 > 0) {
		uint32_t *caps = link->iflaf_caps.nla_bitset_mask;

		printf("\tcapabilities=%x", caps[0]);
		ifconfig_print_bits("IFCAPS", "ifcap", caps, sz_u32,
		    ifcap_bit_names, nitems(ifcap_bit_names));
		ifconfig_print_newline();
	}
#endif
}

void
ifgroup_printgroup(const char *groupname)
{
	struct ifgroupreq ifgr;
	struct ifg_req *ifg;
	unsigned int len;
	int s;

	s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s == -1)
		if_err(1, "socket(AF_LOCAL,SOCK_DGRAM)");
	bzero(&ifgr, sizeof(ifgr));
	strlcpy(ifgr.ifgr_name, groupname, sizeof(ifgr.ifgr_name));
	if (ioctl(s, SIOCGIFGMEMB, (caddr_t)&ifgr) == -1) {
		if (errno == EINVAL || errno == ENOTTY || errno == ENOENT)
			exit(exit_code);
		else
			if_err(1, "SIOCGIFGMEMB");
	}

	len = ifgr.ifgr_len;
	if ((ifgr.ifgr_groups = calloc(1, len)) == NULL)
		if_err(1, "printgroup");
	if (ioctl(s, SIOCGIFGMEMB, (caddr_t)&ifgr) == -1)
		if_err(1, "SIOCGIFGMEMB");

	for (ifg = ifgr.ifgr_groups; ifg && len >= sizeof(struct ifg_req);
	    ifg++) {
		len -= sizeof(struct ifg_req);
#ifdef WITH_LIBXO
		xo_emit("{:member-name/%s}\n", ifg->ifgrq_member);
#else
		printf("%s\n", ifg->ifgrq_member);
#endif
	}
	free(ifgr.ifgr_groups);

	exit(exit_code);
}

void
ifieee80211_line_init(char c)
{
#ifdef WITH_LIBXO
	ifieee80211_spacer = c;
	if (c == '\t')
		ifieee80211_col = 8;
	else
		ifieee80211_col = 1;
#else
	ifieee80211_spacer = c;
	if (c == '\t')
		ifieee80211_col = 8;
	else
		ifieee80211_col = 1;
#endif
}

void
ifieee80211_line_break(void)
{
#ifdef WITH_LIBXO
	if (ifieee80211_spacer != '\t') {
		xo_emit("{P:\n}");
		ifieee80211_spacer = '\t';
	}
	ifieee80211_col = 8; /* 8-ifieee80211_col tab */
#else
	if (ifieee80211_spacer != '\t') {
		ifconfig_print_newline();
		ifieee80211_spacer = '\t';
	}
	ifieee80211_col = 8; /* 8-ifieee80211_col tab */
#endif
}

void
ifieee80211_line_check(const char *fmt, ...)
{
	char buf[80];
	va_list ap;
	int n;
#ifdef WITH_LIBXO
	va_start(ap, fmt);
	n = vsnprintf(buf + 1, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	ifieee80211_col += 1 + n;
	if (ifieee80211_col > IFIEEE80211_MAXCOL) {
		ifieee80211_line_break();
		ifieee80211_col += n;
	}
	buf[0] = ifieee80211_spacer;
	xo_emit("{P:/%s}", buf);
	ifieee80211_spacer = ' ';
#else
	va_start(ap, fmt);
	n = vsnprintf(buf + 1, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	ifieee80211_col += 1 + n;
	if (ifieee80211_col > IFIEEE80211_MAXCOL) {
		ifieee80211_line_break();
		ifieee80211_col += n;
	}
	buf[0] = ifieee80211_spacer;
	printf("%s", buf);
	ifieee80211_spacer = ' ';
#endif
}

void
ifieee80211_print_chaninfo(const struct ieee80211_channel *c, int verb)
{
	char buf[14];

#ifdef WITH_LIBXO
	if (verb)
		xo_emit("{P:/Channel %3u : %u%c%c%c%c%c MHz%-14.14s}",
		    ieee80211_mhz2ieee(c->ic_freq, c->ic_flags), c->ic_freq,
		    IEEE80211_IS_CHAN_PASSIVE(c) ? '*' : ' ',
		    IEEE80211_IS_CHAN_DFS(c) ? 'D' : ' ',
		    IEEE80211_IS_CHAN_RADAR(c) ? 'R' : ' ',
		    IEEE80211_IS_CHAN_CWINT(c) ? 'I' : ' ',
		    IEEE80211_IS_CHAN_CACDONE(c) ? 'C' : ' ',
		    get_chaninfo(c, verb, buf, sizeof(buf)));
	else
		xo_emit("{P:/Channel %3u : %u%c MHz%-14.14s}",
		    ieee80211_mhz2ieee(c->ic_freq, c->ic_flags), c->ic_freq,
		    IEEE80211_IS_CHAN_PASSIVE(c) ? '*' : ' ',
		    get_chaninfo(c, verb, buf, sizeof(buf)));
#else
	if (verb)
		printf("Channel %3u : %u%c%c%c%c%c MHz%-14.14s",
		    ieee80211_mhz2ieee(c->ic_freq, c->ic_flags), c->ic_freq,
		    IEEE80211_IS_CHAN_PASSIVE(c) ? '*' : ' ',
		    IEEE80211_IS_CHAN_DFS(c) ? 'D' : ' ',
		    IEEE80211_IS_CHAN_RADAR(c) ? 'R' : ' ',
		    IEEE80211_IS_CHAN_CWINT(c) ? 'I' : ' ',
		    IEEE80211_IS_CHAN_CACDONE(c) ? 'C' : ' ',
		    get_chaninfo(c, verb, buf, sizeof(buf)));
	else
		printf("Channel %3u : %u%c MHz%-14.14s",
		    ieee80211_mhz2ieee(c->ic_freq, c->ic_flags), c->ic_freq,
		    IEEE80211_IS_CHAN_PASSIVE(c) ? '*' : ' ',
		    get_chaninfo(c, verb, buf, sizeof(buf)));
#endif
}

void
ifieee80211_print_channels(if_ctx *ctx,
    const struct ieee80211req_chaninfo *chans, int allchans, int verb)
{
	struct ieee80211req_chaninfo *achans;
	uint8_t reported[IEEE80211_CHAN_BYTES];
	const struct ieee80211_channel *c;
	unsigned int i, half;

	achans = malloc(IEEE80211_CHANINFO_SPACE(chans));
	if (achans == NULL)
		if_errx(1, "no space for active channel list");
	achans->ic_nchans = 0;
	memset(reported, 0, sizeof(reported));
	if (!allchans) {
		struct ieee80211req_chanlist active;

		if (get80211(ctx, IEEE80211_IOC_CHANLIST, &active,
			sizeof(active)) < 0)
			if_errx(1, "unable to get active channel list");
		for (i = 0; i < chans->ic_nchans; i++) {
			c = &chans->ic_chans[i];
			if (!isset(active.ic_channels, c->ic_ieee))
				continue;
			/*
			 * Suppress compatible duplicates unless
			 * verbose.  The kernel gives us it's
			 * complete channel list which has separate
			 * entries for 11g/11b and 11a/turbo.
			 */
			if (isset(reported, c->ic_ieee) && !verb) {
				/* XXX we assume duplicates are adjacent */
				achans->ic_chans[achans->ic_nchans - 1] = *c;
			} else {
				achans->ic_chans[achans->ic_nchans++] = *c;
				setbit(reported, c->ic_ieee);
			}
		}
	} else {
		for (i = 0; i < chans->ic_nchans; i++) {
			c = &chans->ic_chans[i];
			/* suppress duplicates as above */
			if (isset(reported, c->ic_ieee) && !verb) {
				/* XXX we assume duplicates are adjacent */
				struct ieee80211_channel *a =
				    &achans->ic_chans[achans->ic_nchans - 1];
				if (chanpref(c) > chanpref(a))
					*a = *c;
			} else {
				achans->ic_chans[achans->ic_nchans++] = *c;
				setbit(reported, c->ic_ieee);
			}
		}
	}
	half = achans->ic_nchans / 2;
	if (achans->ic_nchans % 2)
		half++;

#ifdef WITH_LIBXO
	for (i = 0; i < achans->ic_nchans / 2; i++) {
		ifieee80211_print_chaninfo(&achans->ic_chans[i], verb);
		ifieee80211_print_chaninfo(&achans->ic_chans[half + i], verb);
		xo_emit("{P:\n}");
	}
	if (achans->ic_nchans % 2) {
		ifieee80211_print_chaninfo(&achans->ic_chans[i], verb);
		xo_emit("{P:\n}");
	}
#else
	for (i = 0; i < achans->ic_nchans / 2; i++) {
		ifieee80211_print_chaninfo(&achans->ic_chans[i], verb);
		ifieee80211_print_chaninfo(&achans->ic_chans[half + i], verb);
		ifconfig_print_newline();
	}
	if (achans->ic_nchans % 2) {
		ifieee80211_print_chaninfo(&achans->ic_chans[i], verb);
		printf("\n");
	}
#endif
	free(achans);
}

void
ifieee80211_print_txpow(const struct ieee80211_channel *c)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/Channel %3u : %u MHz %3.1f reg %2d  }", c->ic_ieee,
	    c->ic_freq, c->ic_maxpower / 2., c->ic_maxregpower);
#else
	printf("Channel %3u : %u MHz %3.1f reg %2d  ", c->ic_ieee, c->ic_freq,
	    c->ic_maxpower / 2., c->ic_maxregpower);
#endif
}

void
ifieee80211_print_txpow_verbose(const struct ieee80211_channel *c)
{
#ifdef WITH_LIBXO
	ifieee80211_print_chaninfo(c, 1);
	xo_emit("{P:/min %4.1f dBm  max %3.1f dBm  reg %2d dBm}",
	    c->ic_minpower / 2., c->ic_maxpower / 2., c->ic_maxregpower);
	/* indicate where regulatory cap limits power use */
	if (c->ic_maxpower > 2 * c->ic_maxregpower)
		xo_emit("{P: <}");
#else
	ifieee80211_print_chaninfo(c, 1);
	printf("min %4.1f dBm  max %3.1f dBm  reg %2d dBm", c->ic_minpower / 2.,
	    c->ic_maxpower / 2., c->ic_maxregpower);
	/* indicate where regulatory cap limits power use */
	if (c->ic_maxpower > 2 * c->ic_maxregpower)
		printf(" <");
#endif
}

void
ifieee80211_print_regdomain(const struct ieee80211_regdomain *reg, int verb)
{
	if ((reg->regdomain != 0 && reg->regdomain != reg->country) || verb) {
		const struct regdomain *rd =
		    lib80211_regdomain_findbysku(getregdata(), reg->regdomain);
		if (rd == NULL)
			ifieee80211_line_check("regdomain %d", reg->regdomain);
		else
			ifieee80211_line_check("regdomain %s", rd->name);
	}
	if (reg->country != 0 || verb) {
		const struct country *cc =
		    lib80211_country_findbycc(getregdata(), reg->country);
		if (cc == NULL)
			ifieee80211_line_check("country %d", reg->country);
		else
			ifieee80211_line_check("country %s", cc->isoname);
	}
	if (reg->location == 'I')
		ifieee80211_line_check("indoor");
	else if (reg->location == 'O')
		ifieee80211_line_check("outdoor");
	else if (verb)
		ifieee80211_line_check("anywhere");
	if (reg->ecm)
		ifieee80211_line_check("ecm");
	else if (verb)
		ifieee80211_line_check("-ecm");
}

#if 0 /* XXX not interesting with WPA done in user space */
void
ifieee80211_printcipher(int s, struct ieee80211req *ireq, int keylenop)
{
#ifdef WITH_LIBXO
	switch (ireq->i_val) {
	case IEEE80211_CIPHER_WEP:
		ireq->i_type = keylenop;
		if (ioctl(s, SIOCG80211, ireq) != -1)
			xo_emit("{P:/WEP-%s}",
			       ireq->i_len <= 5 ? "40" :
			       ireq->i_len <= 13 ? "104" : "128");
		else
			xo_emit("{P:WEP}");
		break;
	case IEEE80211_CIPHER_TKIP:
		xo_emit("{P:TKIP}");
		break;
	case IEEE80211_CIPHER_AES_OCB:
		xo_emit("{P:AES-OCB}");
		break;
	case IEEE80211_CIPHER_AES_CCM:
		xo_emit("{P:AES-CCM}");
		break;
	case IEEE80211_CIPHER_AES_GCM_128:
		xo_emit("{P:AES-GCM}");
		break;
	case IEEE80211_CIPHER_CKIP:
		xo_emit("{P:CKIP}");
		break;
	case IEEE80211_CIPHER_NONE:
		xo_emit("{P:NONE}");
		break;
	default:
		xo_emit("{P:/UNKNOWN (0x%x)}", ireq->i_val);
		break;
	}
#endif
}
#endif

void
ifieee80211_printkey_index(uint16_t keyix, char *buf, size_t buflen)
{
	buf[0] = '\0';
	if (keyix == IEEE80211_KEYIX_NONE) {
		snprintf(buf, buflen, "ucast");
	} else {
		snprintf(buf, buflen, "%u", keyix + 1);
	}
}

void
ifieee80211_printkey(if_ctx *ctx, const struct ieee80211req_key *ik)
{
	static const uint8_t zerodata[IEEE80211_KEYBUF_SIZE];
	u_int keylen = ik->ik_keylen;
	int printcontents;
	const int verbose = ctx->args->verbose;
	const bool printkeys = ctx->args->printkeys;
	char keyix[16];

	printcontents = printkeys &&
	    (memcmp(ik->ik_keydata, zerodata, keylen) != 0 || verbose);
	if (printcontents)
		ifieee80211_line_break();
	ifieee80211_printkey_index(ik->ik_keyix, keyix, sizeof(keyix));
	switch (ik->ik_type) {
	case IEEE80211_CIPHER_WEP:
		/* compatibility */
		ifieee80211_line_check("wepkey %s:%s", keyix,
		    keylen <= 5	     ? "40-bit" :
			keylen <= 13 ? "104-bit" :
				       "128-bit");
		break;
	case IEEE80211_CIPHER_TKIP:
		if (keylen > 128 / 8)
			keylen -= 128 / 8; /* ignore MIC for now */
		ifieee80211_line_check("TKIP %s:%u-bit", keyix, 8 * keylen);
		break;
	case IEEE80211_CIPHER_AES_OCB:
		ifieee80211_line_check("AES-OCB %s:%u-bit", keyix, 8 * keylen);
		break;
	case IEEE80211_CIPHER_AES_CCM:
		ifieee80211_line_check("AES-CCM %s:%u-bit", keyix, 8 * keylen);
		break;
	case IEEE80211_CIPHER_AES_GCM_128:
		ifieee80211_line_check("AES-GCM %s:%u-bit", keyix, 8 * keylen);
		break;
	case IEEE80211_CIPHER_CKIP:
		ifieee80211_line_check("CKIP %s:%u-bit", keyix, 8 * keylen);
		break;
	case IEEE80211_CIPHER_NONE:
		ifieee80211_line_check("NULL %s:%u-bit", keyix, 8 * keylen);
		break;
	default:
		ifieee80211_line_check("UNKNOWN (0x%x) %s:%u-bit", ik->ik_type,
		    keyix, 8 * keylen);
		break;
	}
	if (printcontents) {
		u_int i;
#ifdef WITH_LIBXO
		xo_emit("{P: <}");
		for (i = 0; i < keylen; i++)
			xo_emit("{P:/%02x}", ik->ik_keydata[i]);
		xo_emit("{P:>}");
		if (ik->ik_type != IEEE80211_CIPHER_WEP &&
		    (ik->ik_keyrsc != 0 || verbose))
			xo_emit("{P:/ rsc %ju}", (uintmax_t)ik->ik_keyrsc);
		if (ik->ik_type != IEEE80211_CIPHER_WEP &&
		    (ik->ik_keytsc != 0 || verbose))
			xo_emit("{P:/ tsc %ju}", (uintmax_t)ik->ik_keytsc);
		if (ik->ik_flags != 0 && verbose) {
			const char *sep = " ";

			if (ik->ik_flags & IEEE80211_KEY_XMIT)
				xo_emit("{P:/%stx}", sep), sep = "+";
			if (ik->ik_flags & IEEE80211_KEY_RECV)
				xo_emit("{P:/%srx}", sep), sep = "+";
			if (ik->ik_flags & IEEE80211_KEY_DEFAULT)
				xo_emit("{P:/%sdef}", sep), sep = "+";
		}
		ifieee80211_line_break();
#else
		printf(" <");
		for (i = 0; i < keylen; i++)
			printf("%02x", ik->ik_keydata[i]);
		printf(">");
		if (ik->ik_type != IEEE80211_CIPHER_WEP &&
		    (ik->ik_keyrsc != 0 || verbose))
			printf(" rsc %ju", (uintmax_t)ik->ik_keyrsc);
		if (ik->ik_type != IEEE80211_CIPHER_WEP &&
		    (ik->ik_keytsc != 0 || verbose))
			printf(" tsc %ju", (uintmax_t)ik->ik_keytsc);
		if (ik->ik_flags != 0 && verbose) {
			const char *sep = " ";

			if (ik->ik_flags & IEEE80211_KEY_XMIT)
				printf("%stx", sep), sep = "+";
			if (ik->ik_flags & IEEE80211_KEY_RECV)
				printf("%srx", sep), sep = "+";
			if (ik->ik_flags & IEEE80211_KEY_DEFAULT)
				printf("%sdef", sep), sep = "+";
		}
		ifieee80211_line_break();
#endif
	}
}

void
ifieee80211_printrate(const char *tag, int v, int defrate, int defmcs)
{
	if ((v & IEEE80211_RATE_MCS) == 0) {
		if (v != defrate) {
			if (v & 1)
				ifieee80211_line_check("%s %d.5", tag, v / 2);
			else
				ifieee80211_line_check("%s %d", tag, v / 2);
		}
	} else {
		if (v != defmcs)
			ifieee80211_line_check("%s %d", tag, v & ~0x80);
	}
}

void
ifieee80211_print_string(const u_int8_t *buf, int len)
{
	int i;
	int hasspc;
	int utf8;

	i = 0;
	hasspc = 0;

	setlocale(LC_CTYPE, "");
	utf8 = strncmp("UTF-8", nl_langinfo(CODESET), 5) == 0;

	for (; i < len; i++) {
		if (!isprint(buf[i]) && buf[i] != '\0' && !utf8)
			break;
		if (isspace(buf[i]))
			hasspc++;
	}
#ifdef WITH_LIBXO
	if (i == len || utf8) {
		if (hasspc || len == 0 || buf[0] == '\0')
			xo_emit("{P:/\"%.*s\"}", len, buf);
		else
			xo_emit("{P:/%.*s}", len, buf);
	} else {
		xo_emit("{P:0x}");
		for (i = 0; i < len; i++)
			xo_emit("{P:/%02x}", buf[i]);
	}
#else
	if (i == len || utf8) {
		if (hasspc || len == 0 || buf[0] == '\0')
			printf("\"%.*s\"", len, buf);
		else
			printf("%.*s", len, buf);
	} else {
		printf("0x");
		for (i = 0; i < len; i++)
			printf("%02x", buf[i]);
	}
#endif
}

void
ifieee80211_printie(if_ctx *ctx, const char *tag, const uint8_t *ie,
    size_t ielen, unsigned int maxlen)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		maxlen -= strlen(tag) + 2;
		if (2 * ielen > maxlen)
			maxlen--;
		xo_emit("{P:<}");
		for (; ielen > 0; ie++, ielen--) {
			if (maxlen-- <= 0)
				break;
			xo_emit("{P:/%02x}", *ie);
		}
		if (ielen != 0)
			xo_emit("{P:-}");
		xo_emit("{P:>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		maxlen -= strlen(tag) + 2;
		if (2 * ielen > maxlen)
			maxlen--;
		printf("<");
		for (; ielen > 0; ie++, ielen--) {
			if (maxlen-- <= 0)
				break;
			printf("%02x", *ie);
		}
		if (ielen != 0)
			printf("-");
		printf(">");
	}
#endif
}

/*
 * NB: The decoding routines assume a properly formatted ie
 *     which should be safe as the kernel only retains them
 *     if they parse ok.
 */

void
ifieee80211_printwmeparam(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
	static const char *acnames[] = { "BE", "BK", "VO", "VI" };
	const struct ieee80211_wme_param *wme =
	    (const struct ieee80211_wme_param *)ie;
	int i;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (!ctx->args->verbose)
		return;
	xo_emit("{P:/<qosinfo 0x%x}", wme->param_qosInfo);
	ie += offsetof(struct ieee80211_wme_param, params_acParams);
	for (i = 0; i < WME_NUM_AC; i++) {
		const struct ieee80211_wme_acparams *ac =
		    &wme->params_acParams[i];

		xo_emit("{P:/ %s[%saifsn %u cwmin %u cwmax %u txop %u]}",
		    acnames[i],
		    _IEEE80211_MASKSHIFT(ac->acp_aci_aifsn, WME_PARAM_ACM) ?
			"acm " :
			"",
		    _IEEE80211_MASKSHIFT(ac->acp_aci_aifsn, WME_PARAM_AIFSN),
		    _IEEE80211_MASKSHIFT(ac->acp_logcwminmax,
			WME_PARAM_LOGCWMIN),
		    _IEEE80211_MASKSHIFT(ac->acp_logcwminmax,
			WME_PARAM_LOGCWMAX),
		    LE_READ_2(&ac->acp_txop));
	}
	xo_emit("{P:>}");
#else
	printf("%s", tag);
	if (!ctx->args->verbose)
		return;
	printf("<qosinfo 0x%x", wme->param_qosInfo);
	ie += offsetof(struct ieee80211_wme_param, params_acParams);
	for (i = 0; i < WME_NUM_AC; i++) {
		const struct ieee80211_wme_acparams *ac =
		    &wme->params_acParams[i];

		printf(" %s[%saifsn %u cwmin %u cwmax %u txop %u]", acnames[i],
		    _IEEE80211_MASKSHIFT(ac->acp_aci_aifsn, WME_PARAM_ACM) ?
			"acm " :
			"",
		    _IEEE80211_MASKSHIFT(ac->acp_aci_aifsn, WME_PARAM_AIFSN),
		    _IEEE80211_MASKSHIFT(ac->acp_logcwminmax,
			WME_PARAM_LOGCWMIN),
		    _IEEE80211_MASKSHIFT(ac->acp_logcwminmax,
			WME_PARAM_LOGCWMAX),
		    LE_READ_2(&ac->acp_txop));
	}
	printf(">");
#endif
}

void
ifieee80211_printwmeinfo(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_wme_info *wme =
		    (const struct ieee80211_wme_info *)ie;
		xo_emit("{P:/<version 0x%x info 0x%x>}", wme->wme_version,
		    wme->wme_info);
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_wme_info *wme =
		    (const struct ieee80211_wme_info *)ie;
		printf("<version 0x%x info 0x%x>", wme->wme_version,
		    wme->wme_info);
	}
#endif
}

void
ifieee80211_printhecap(if_ctx *ctx, const char *tag, const uint8_t *ie)
{
	const struct ieee80211_he_cap_elem *hecap;
	const struct ieee80211_he_mcs_nss_supp *mcsnss;
	unsigned int i;
	uint8_t chw;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (!ctx->args->verbose)
		return;

	/* Check that the right size. */
	if (ie[1] < 1 + sizeof(*hecap) + 4) {
		xo_emit("{P:/<err: he_cap inval. length %#0x>}", ie[1]);
		return;
	}
	/* Skip Element ID, Length, EID Extension. */
	hecap = (const struct ieee80211_he_cap_elem *)(ie + 3);

	/* XXX-BZ we need to somehow decode each field? */
	xo_emit("{P:<mac_cap}");
	for (i = 0; i < nitems(hecap->mac_cap_info); i++)
		xo_emit("{P:/ %#04x}", hecap->mac_cap_info[i]);
	xo_emit("{P: phy_cap}");
	for (i = 0; i < nitems(hecap->phy_cap_info); i++)
		xo_emit("{P:/ %#04x}", hecap->phy_cap_info[i]);

	chw = hecap->phy_cap_info[0];
	ie = (const uint8_t *)(const void *)(hecap + 1);
	mcsnss = (const struct ieee80211_he_mcs_nss_supp *)ie;
	/* Cannot use <=  as < is a delimiter. */
	xo_emit("{P:/ rx/tx_he_mcs map: loweq80 %#06x/%#06x}",
	    mcsnss->rx_mcs_80, mcsnss->tx_mcs_80);
	ie += 2;
	if ((chw & (1 << 2)) != 0) {
		xo_emit("{P:/ 160 %#06x/%#06x}", mcsnss->rx_mcs_160,
		    mcsnss->tx_mcs_160);
		ie += 2;
	}
	if ((chw & (1 << 3)) != 0) {
		xo_emit("{P:/ 80+80 %#06x/%#06x}", mcsnss->rx_mcs_80p80,
		    mcsnss->tx_mcs_80p80);
		ie += 2;
	}
	/* TODO: ppet = (struct ... *)ie; */

	xo_emit("{P:>}");
#else
	printf("%s", tag);
	if (!ctx->args->verbose)
		return;

	/* Check that the right size. */
	if (ie[1] < 1 + sizeof(*hecap) + 4) {
		printf("<err: he_cap inval. length %#0x>", ie[1]);
		return;
	}
	/* Skip Element ID, Length, EID Extension. */
	hecap = (const struct ieee80211_he_cap_elem *)(ie + 3);

	/* XXX-BZ we need to somehow decode each field? */
	printf("<mac_cap");
	for (i = 0; i < nitems(hecap->mac_cap_info); i++)
		printf(" %#04x", hecap->mac_cap_info[i]);
	printf(" phy_cap");
	for (i = 0; i < nitems(hecap->phy_cap_info); i++)
		printf(" %#04x", hecap->phy_cap_info[i]);

	chw = hecap->phy_cap_info[0];
	ie = (const uint8_t *)(const void *)(hecap + 1);
	mcsnss = (const struct ieee80211_he_mcs_nss_supp *)ie;
	/* Cannot use <=  as < is a delimiter. */
	printf(" rx/tx_he_mcs map: loweq80 %#06x/%#06x", mcsnss->rx_mcs_80,
	    mcsnss->tx_mcs_80);
	ie += 2;
	if ((chw & (1 << 2)) != 0) {
		printf(" 160 %#06x/%#06x", mcsnss->rx_mcs_160,
		    mcsnss->tx_mcs_160);
		ie += 2;
	}
	if ((chw & (1 << 3)) != 0) {
		printf(" 80+80 %#06x/%#06x", mcsnss->rx_mcs_80p80,
		    mcsnss->tx_mcs_80p80);
		ie += 2;
	}
	/* TODO: ppet = (struct ... *)ie; */

	printf(">");
#endif
}

void
ifieee80211_printheoper(if_ctx *ctx, const char *tag, const uint8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_he_operation *heoper;
		uint32_t params;

		/* Check that the right size. */
		if (ie[1] < 1 + sizeof(*heoper)) {
			xo_emit("{P:/<err: he_oper inval. length %#0x>}",
			    ie[1]);
			return;
		}
		/* Skip Element ID, Length, EID Extension. */
		heoper = (const struct ieee80211_he_operation *)(ie + 3);

		/* XXX-BZ we need to somehow decode each field? */
		params = heoper->he_oper_params & 0x00ffffff;
		xo_emit("{P:/<params %#08x}", params);
		xo_emit("{P:/ bss_ifieee80211_col %#04x}",
		    (heoper->he_oper_params & 0xff000000) >> 24);
		xo_emit("{P:/ mcs_nss %#06x}", heoper->he_mcs_nss_set);
		if ((params & (1 << 14)) != 0) {
			xo_emit("{P: vht_op 0-3}");
		}
		if ((params & (1 << 15)) != 0) {
			xo_emit("{P: max_coh_bssid 0-1}");
		}
		if ((params & (1 << 17)) != 0) {
			xo_emit("{P: 6ghz_op 0-5}");
		}
		xo_emit("{P:>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_he_operation *heoper;
		uint32_t params;

		/* Check that the right size. */
		if (ie[1] < 1 + sizeof(*heoper)) {
			printf("<err: he_oper inval. length %#0x>", ie[1]);
			return;
		}
		/* Skip Element ID, Length, EID Extension. */
		heoper = (const struct ieee80211_he_operation *)(ie + 3);

		/* XXX-BZ we need to somehow decode each field? */
		params = heoper->he_oper_params & 0x00ffffff;
		printf("<params %#08x", params);
		printf(" bss_ifieee80211_col %#04x",
		    (heoper->he_oper_params & 0xff000000) >> 24);
		printf(" mcs_nss %#06x", heoper->he_mcs_nss_set);
		if ((params & (1 << 14)) != 0) {
			printf(" vht_op 0-3");
		}
		if ((params & (1 << 15)) != 0) {
			printf(" max_coh_bssid 0-1");
		}
		if ((params & (1 << 17)) != 0) {
			printf(" 6ghz_op 0-5");
		}
		printf(">");
	}
#endif
}

void
ifieee80211_printmuedcaparamset(if_ctx *ctx, const char *tag, const uint8_t *ie)
{
	static const char *acnames[] = { "BE", "BK", "VO", "VI" };
	const struct ieee80211_mu_edca_param_set *mu_edca;
	int i;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (!ctx->args->verbose)
		return;

	/* Check that the right size. */
	if (ie[1] != 1 + sizeof(*mu_edca)) {
		xo_emit("{P:/<err: mu_edca inval. length %#04x>}", ie[1]);
		return;
	}
	/* Skip Element ID, Length, EID Extension. */
	mu_edca = (const struct ieee80211_mu_edca_param_set *)(ie + 3);

	xo_emit("{P:/<qosinfo 0x%x}", mu_edca->mu_qos_info);
	ie++;
	for (i = 0; i < WME_NUM_AC; i++) {
		const struct ieee80211_he_mu_edca_param_ac_rec *ac =
		    &mu_edca->param_ac_recs[i];

		xo_emit("{P:/ %s[aifsn %u ecwmin %u ecwmax %u timer %u]}",
		    acnames[i], ac->aifsn,
		    _IEEE80211_MASKSHIFT(ac->ecw_min_max, WME_PARAM_LOGCWMIN),
		    _IEEE80211_MASKSHIFT(ac->ecw_min_max, WME_PARAM_LOGCWMAX),
		    ac->mu_edca_timer);
	}
	xo_emit("{P:>}");
#else
	printf("%s", tag);
	if (!ctx->args->verbose)
		return;

	/* Check that the right size. */
	if (ie[1] != 1 + sizeof(*mu_edca)) {
		printf("<err: mu_edca inval. length %#04x>", ie[1]);
		return;
	}
	/* Skip Element ID, Length, EID Extension. */
	mu_edca = (const struct ieee80211_mu_edca_param_set *)(ie + 3);

	printf("<qosinfo 0x%x", mu_edca->mu_qos_info);
	ie++;
	for (i = 0; i < WME_NUM_AC; i++) {
		const struct ieee80211_he_mu_edca_param_ac_rec *ac =
		    &mu_edca->param_ac_recs[i];

		printf(" %s[aifsn %u ecwmin %u ecwmax %u timer %u]", acnames[i],
		    ac->aifsn,
		    _IEEE80211_MASKSHIFT(ac->ecw_min_max, WME_PARAM_LOGCWMIN),
		    _IEEE80211_MASKSHIFT(ac->ecw_min_max, WME_PARAM_LOGCWMAX),
		    ac->mu_edca_timer);
	}
	printf(">");
#endif
}

void
ifieee80211_printsupopclass(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
	uint8_t len, i;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (!ctx->args->verbose)
		return;

	/* Check that the right size. */
	len = ie[1];
	if (len < 2) {
		xo_emit("{P:/<err: sup_op_class inval. length %#04x>}", ie[1]);
		return;
	}

	ie += 2;
	i = 0;
	xo_emit("{P:/<cur op class %u}", *ie);
	i++;
	if (i < len && *(ie + i) != 130)
		xo_emit("{P: op classes}");
	while (i < len && *(ie + i) != 130) {
		xo_emit("{P:/ %u}", *(ie + i));
		i++;
	}
	if (i > 1 && i < len && *(ie + i) != 130) {
		xo_emit("{P:/ parsing error at %#0x>}", i);
		return;
	}
	/* Skip OneHundredAndThirty Delimiter. */
	i++;
	if (i < len && *(ie + i) != 0)
		xo_emit("{P: ext seq}");
	while (i < len && *(ie + i) != 0) {
		xo_emit("{P:/ %u}", *(ie + i));
		i++;
	}
	if (i > 1 && i < len && *(ie + i) != 0) {
		xo_emit("{P:/ parsing error at %#0x>}", i);
		return;
	}
	/* Skip Zero Delimiter. */
	i++;
	if ((i + 1) < len)
		xo_emit("{P: duple seq}");
	while ((i + 1) < len) {
		xo_emit("{P:/ %u/%u}", *(ie + i), *(ie + i + 1));
		i += 2;
	}
	xo_emit("{P:>}");
#else
	printf("%s", tag);
	if (!ctx->args->verbose)
		return;

	/* Check that the right size. */
	len = ie[1];
	if (len < 2) {
		printf("<err: sup_op_class inval. length %#04x>", ie[1]);
		return;
	}

	ie += 2;
	i = 0;
	printf("<cur op class %u", *ie);
	i++;
	if (i < len && *(ie + i) != 130)
		printf(" op classes");
	while (i < len && *(ie + i) != 130) {
		printf(" %u", *(ie + i));
		i++;
	}
	if (i > 1 && i < len && *(ie + i) != 130) {
		printf(" parsing error at %#0x>", i);
		return;
	}
	/* Skip OneHundredAndThirty Delimiter. */
	i++;
	if (i < len && *(ie + i) != 0)
		printf(" ext seq");
	while (i < len && *(ie + i) != 0) {
		printf(" %u", *(ie + i));
		i++;
	}
	if (i > 1 && i < len && *(ie + i) != 0) {
		printf(" parsing error at %#0x>", i);
		return;
	}
	/* Skip Zero Delimiter. */
	i++;
	if ((i + 1) < len)
		printf(" duple seq");
	while ((i + 1) < len) {
		printf(" %u/%u", *(ie + i), *(ie + i + 1));
		i += 2;
	}
	printf(">");
#endif
}

void
ifieee80211_printvhtcap(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_vht_cap *vhtcap;
		uint32_t vhtcap_info;

		/* Check that the right size. */
		if (ie[1] != sizeof(*vhtcap)) {
			xo_emit("{P:<err: vht_cap inval. length>}");
			return;
		}
		/* Skip Element ID and Length. */
		vhtcap = (const struct ieee80211_vht_cap *)(ie + 2);

		vhtcap_info = LE_READ_4(&vhtcap->vht_cap_info);
		xo_emit("{P:/<cap 0x%08x}", vhtcap_info);
		xo_emit("{P:/ rx_mcs_map 0x%x}",
		    LE_READ_2(&vhtcap->supp_mcs.rx_mcs_map));
		xo_emit("{P:/ rx_highest %d}",
		    LE_READ_2(&vhtcap->supp_mcs.rx_highest) & 0x1fff);
		xo_emit("{P:/ tx_mcs_map 0x%x}",
		    LE_READ_2(&vhtcap->supp_mcs.tx_mcs_map));
		xo_emit("{P:/ tx_highest %d}",
		    LE_READ_2(&vhtcap->supp_mcs.tx_highest) & 0x1fff);

		xo_emit("{P:>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_vht_cap *vhtcap;
		uint32_t vhtcap_info;

		/* Check that the right size. */
		if (ie[1] != sizeof(*vhtcap)) {
			printf("<err: vht_cap inval. length>");
			return;
		}
		/* Skip Element ID and Length. */
		vhtcap = (const struct ieee80211_vht_cap *)(ie + 2);

		vhtcap_info = LE_READ_4(&vhtcap->vht_cap_info);
		printf("<cap 0x%08x", vhtcap_info);
		printf(" rx_mcs_map 0x%x",
		    LE_READ_2(&vhtcap->supp_mcs.rx_mcs_map));
		printf(" rx_highest %d",
		    LE_READ_2(&vhtcap->supp_mcs.rx_highest) & 0x1fff);
		printf(" tx_mcs_map 0x%x",
		    LE_READ_2(&vhtcap->supp_mcs.tx_mcs_map));
		printf(" tx_highest %d",
		    LE_READ_2(&vhtcap->supp_mcs.tx_highest) & 0x1fff);

		printf(">");
	}
#endif
}

void
ifieee80211_printvhtinfo(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_vht_operation *vhtinfo;

		/* Check that the right size. */
		if (ie[1] != sizeof(*vhtinfo)) {
			xo_emit("{P:<err: vht_operation inval. length>}");
			return;
		}
		/* Skip Element ID and Length. */
		vhtinfo = (const struct ieee80211_vht_operation *)(ie + 2);

		xo_emit(
		    "{P:/<chw %d freq0_idx %d freq1_idx %d basic_mcs_set 0x%04x>}",
		    vhtinfo->chan_width, vhtinfo->center_freq_seq0_idx,
		    vhtinfo->center_freq_seq1_idx,
		    LE_READ_2(&vhtinfo->basic_mcs_set));
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_vht_operation *vhtinfo;

		/* Check that the right size. */
		if (ie[1] != sizeof(*vhtinfo)) {
			printf("<err: vht_operation inval. length>");
			return;
		}
		/* Skip Element ID and Length. */
		vhtinfo = (const struct ieee80211_vht_operation *)(ie + 2);

		printf(
		    "<chw %d freq0_idx %d freq1_idx %d basic_mcs_set 0x%04x>",
		    vhtinfo->chan_width, vhtinfo->center_freq_seq0_idx,
		    vhtinfo->center_freq_seq1_idx,
		    LE_READ_2(&vhtinfo->basic_mcs_set));
	}
#endif
}

void
ifieee80211_printvhtpwrenv(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen)
{
	static const char *txpwrmap[] = {
		"20",
		"40",
		"80",
		"160",
	};
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);

	if (ctx->args->verbose) {
		const struct ieee80211_ie_vht_txpwrenv *vhtpwr =
		    (const struct ieee80211_ie_vht_txpwrenv *)ie;
		size_t i, n;
		const char *sep = "";

		/* Get count; trim at ielen */
		n = (vhtpwr->tx_info & IEEE80211_VHT_TXPWRENV_INFO_COUNT_MASK) +
		    1;
		/* Trim at ielen */
		if (n + 3 > ielen)
			n = ielen - 3;
		xo_emit("{P:/<tx_info 0x%02x pwr:[}", vhtpwr->tx_info);
		for (i = 0; i < n; i++) {
			xo_emit("{P:/%s%s:%.2f}", sep, txpwrmap[i],
			    ((float)((int8_t)ie[i + 3])) / 2.0);
			sep = " ";
		}

		xo_emit("{P:]>}");
	}
#else
	printf("%s", tag);

	if (ctx->args->verbose) {
		const struct ieee80211_ie_vht_txpwrenv *vhtpwr =
		    (const struct ieee80211_ie_vht_txpwrenv *)ie;
		size_t i, n;
		const char *sep = "";

		/* Get count; trim at ielen */
		n = (vhtpwr->tx_info & IEEE80211_VHT_TXPWRENV_INFO_COUNT_MASK) +
		    1;
		/* Trim at ielen */
		if (n + 3 > ielen)
			n = ielen - 3;
		printf("<tx_info 0x%02x pwr:[", vhtpwr->tx_info);
		for (i = 0; i < n; i++) {
			printf("%s%s:%.2f", sep, txpwrmap[i],
			    ((float)((int8_t)ie[i + 3])) / 2.0);
			sep = " ";
		}

		printf("]>");
	}
#endif
}

void
ifieee80211_printhtcap(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ie_htcap *htcap =
		    (const struct ieee80211_ie_htcap *)ie;
		const char *sep;
		int i, j;

		xo_emit("{P:/<cap 0x%x param 0x%x}", LE_READ_2(&htcap->hc_cap),
		    htcap->hc_param);
		xo_emit("{P: mcsset[}");
		sep = "";
		for (i = 0; i < IEEE80211_HTRATE_MAXSIZE; i++)
			if (isset(htcap->hc_mcsset, i)) {
				for (j = i + 1; j < IEEE80211_HTRATE_MAXSIZE;
				    j++)
					if (isclr(htcap->hc_mcsset, j))
						break;
				j--;
				if (i == j)
					xo_emit("{P:/%s%u}", sep, i);
				else
					xo_emit("{P:/%s%u-%u}", sep, i, j);
				i += j - i;
				sep = ",";
			}
		xo_emit("{P:/] extcap 0x%x txbf 0x%x antenna 0x%x>}",
		    LE_READ_2(&htcap->hc_extcap), LE_READ_4(&htcap->hc_txbf),
		    htcap->hc_antenna);
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ie_htcap *htcap =
		    (const struct ieee80211_ie_htcap *)ie;
		const char *sep;
		int i, j;

		printf("<cap 0x%x param 0x%x", LE_READ_2(&htcap->hc_cap),
		    htcap->hc_param);
		printf(" mcsset[");
		sep = "";
		for (i = 0; i < IEEE80211_HTRATE_MAXSIZE; i++)
			if (isset(htcap->hc_mcsset, i)) {
				for (j = i + 1; j < IEEE80211_HTRATE_MAXSIZE;
				    j++)
					if (isclr(htcap->hc_mcsset, j))
						break;
				j--;
				if (i == j)
					printf("%s%u", sep, i);
				else
					printf("%s%u-%u", sep, i, j);
				i += j - i;
				sep = ",";
			}
		printf("] extcap 0x%x txbf 0x%x antenna 0x%x>",
		    LE_READ_2(&htcap->hc_extcap), LE_READ_4(&htcap->hc_txbf),
		    htcap->hc_antenna);
	}
#endif
}

void
ifieee80211_printhtinfo(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ie_htinfo *htinfo =
		    (const struct ieee80211_ie_htinfo *)ie;
		const char *sep;
		int i, j;

		xo_emit("{P:/<ctl %u, %x,%x,%x,%x}", htinfo->hi_ctrlchannel,
		    htinfo->hi_byte1, htinfo->hi_byte2, htinfo->hi_byte3,
		    LE_READ_2(&htinfo->hi_byte45));
		xo_emit("{P: basicmcs[}");
		sep = "";
		for (i = 0; i < IEEE80211_HTRATE_MAXSIZE; i++)
			if (isset(htinfo->hi_basicmcsset, i)) {
				for (j = i + 1; j < IEEE80211_HTRATE_MAXSIZE;
				    j++)
					if (isclr(htinfo->hi_basicmcsset, j))
						break;
				j--;
				if (i == j)
					xo_emit("{P:/%s%u}", sep, i);
				else
					xo_emit("{P:/%s%u-%u}", sep, i, j);
				i += j - i;
				sep = ",";
			}
		xo_emit("{P:]>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ie_htinfo *htinfo =
		    (const struct ieee80211_ie_htinfo *)ie;
		const char *sep;
		int i, j;

		printf("<ctl %u, %x,%x,%x,%x", htinfo->hi_ctrlchannel,
		    htinfo->hi_byte1, htinfo->hi_byte2, htinfo->hi_byte3,
		    LE_READ_2(&htinfo->hi_byte45));
		printf(" basicmcs[");
		sep = "";
		for (i = 0; i < IEEE80211_HTRATE_MAXSIZE; i++)
			if (isset(htinfo->hi_basicmcsset, i)) {
				for (j = i + 1; j < IEEE80211_HTRATE_MAXSIZE;
				    j++)
					if (isclr(htinfo->hi_basicmcsset, j))
						break;
				j--;
				if (i == j)
					printf("%s%u", sep, i);
				else
					printf("%s%u-%u", sep, i, j);
				i += j - i;
				sep = ",";
			}
		printf("]>");
	}
#endif
}

void
ifieee80211_printathie(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ath_ie *ath =
		    (const struct ieee80211_ath_ie *)ie;

		xo_emit("{P:<}");
		if (ath->ath_capability & ATHEROS_CAP_TURBO_PRIME)
			xo_emit("{P:DTURBO,}");
		if (ath->ath_capability & ATHEROS_CAP_COMPRESSION)
			xo_emit("{P:COMP,}");
		if (ath->ath_capability & ATHEROS_CAP_FAST_FRAME)
			xo_emit("{P:FF,}");
		if (ath->ath_capability & ATHEROS_CAP_XR)
			xo_emit("{P:XR,}");
		if (ath->ath_capability & ATHEROS_CAP_AR)
			xo_emit("{P:AR,}");
		if (ath->ath_capability & ATHEROS_CAP_BURST)
			xo_emit("{P:BURST,}");
		if (ath->ath_capability & ATHEROS_CAP_WME)
			xo_emit("{P:WME,}");
		if (ath->ath_capability & ATHEROS_CAP_BOOST)
			xo_emit("{P:BOOST,}");
		xo_emit("{P:/0x%x>}", LE_READ_2(ath->ath_defkeyix));
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ath_ie *ath =
		    (const struct ieee80211_ath_ie *)ie;

		printf("<");
		if (ath->ath_capability & ATHEROS_CAP_TURBO_PRIME)
			printf("DTURBO,");
		if (ath->ath_capability & ATHEROS_CAP_COMPRESSION)
			printf("COMP,");
		if (ath->ath_capability & ATHEROS_CAP_FAST_FRAME)
			printf("FF,");
		if (ath->ath_capability & ATHEROS_CAP_XR)
			printf("XR,");
		if (ath->ath_capability & ATHEROS_CAP_AR)
			printf("AR,");
		if (ath->ath_capability & ATHEROS_CAP_BURST)
			printf("BURST,");
		if (ath->ath_capability & ATHEROS_CAP_WME)
			printf("WME,");
		if (ath->ath_capability & ATHEROS_CAP_BOOST)
			printf("BOOST,");
		printf("0x%x>", LE_READ_2(ath->ath_defkeyix));
	}
#endif
}

void
ifieee80211_printmeshconf(if_ctx *ctx, const char *tag, const uint8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_meshconf_ie *mconf =
		    (const struct ieee80211_meshconf_ie *)ie;
		xo_emit("{P:<PATH:}");
		if (mconf->conf_pselid == IEEE80211_MESHCONF_PATH_HWMP)
			xo_emit("{P:HWMP}");
		else
			xo_emit("{P:UNKNOWN}");
		xo_emit("{P: LINK:}");
		if (mconf->conf_pmetid == IEEE80211_MESHCONF_METRIC_AIRTIME)
			xo_emit("{P:AIRTIME}");
		else
			xo_emit("{P:UNKNOWN}");
		xo_emit("{P: CONGESTION:}");
		if (mconf->conf_ccid == IEEE80211_MESHCONF_CC_DISABLED)
			xo_emit("{P:DISABLED}");
		else
			xo_emit("{P:UNKNOWN}");
		xo_emit("{P: SYNC:}");
		if (mconf->conf_syncid == IEEE80211_MESHCONF_SYNC_NEIGHOFF)
			xo_emit("{P:NEIGHOFF}");
		else
			xo_emit("{P:UNKNOWN}");
		xo_emit("{P: AUTH:}");
		if (mconf->conf_authid == IEEE80211_MESHCONF_AUTH_DISABLED)
			xo_emit("{P:DISABLED}");
		else
			xo_emit("{P:UNKNOWN}");
		xo_emit("{P:/ FORM:0x%x CAPS:0x%x>}", mconf->conf_form,
		    mconf->conf_cap);
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_meshconf_ie *mconf =
		    (const struct ieee80211_meshconf_ie *)ie;
		printf("<PATH:");
		if (mconf->conf_pselid == IEEE80211_MESHCONF_PATH_HWMP)
			printf("HWMP");
		else
			printf("UNKNOWN");
		printf(" LINK:");
		if (mconf->conf_pmetid == IEEE80211_MESHCONF_METRIC_AIRTIME)
			printf("AIRTIME");
		else
			printf("UNKNOWN");
		printf(" CONGESTION:");
		if (mconf->conf_ccid == IEEE80211_MESHCONF_CC_DISABLED)
			printf("DISABLED");
		else
			printf("UNKNOWN");
		printf(" SYNC:");
		if (mconf->conf_syncid == IEEE80211_MESHCONF_SYNC_NEIGHOFF)
			printf("NEIGHOFF");
		else
			printf("UNKNOWN");
		printf(" AUTH:");
		if (mconf->conf_authid == IEEE80211_MESHCONF_AUTH_DISABLED)
			printf("DISABLED");
		else
			printf("UNKNOWN");
		printf(" FORM:0x%x CAPS:0x%x>", mconf->conf_form,
		    mconf->conf_cap);
	}
#endif
}

void
ifieee80211_printbssload(if_ctx *ctx, const char *tag, const uint8_t *ie)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_bss_load_ie *bssload =
		    (const struct ieee80211_bss_load_ie *)ie;
		xo_emit("{P:/<sta count %d, chan load %d, aac %d>}",
		    LE_READ_2(&bssload->sta_count), bssload->chan_load,
		    bssload->aac);
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_bss_load_ie *bssload =
		    (const struct ieee80211_bss_load_ie *)ie;
		printf("<sta count %d, chan load %d, aac %d>",
		    LE_READ_2(&bssload->sta_count), bssload->chan_load,
		    bssload->aac);
	}
#endif
}

void
ifieee80211_printapchanrep(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ap_chan_report_ie *ap =
		    (const struct ieee80211_ap_chan_report_ie *)ie;
		const char *sep = "";

		xo_emit("{P:/<class %u, chan:[}", ap->i_class);

		for (size_t i = 3; i < ielen; i++) {
			xo_emit("{P:/%s%u}", sep, ie[i]);
			sep = ",";
		}
		xo_emit("{P:]>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const struct ieee80211_ap_chan_report_ie *ap =
		    (const struct ieee80211_ap_chan_report_ie *)ie;
		const char *sep = "";

		printf("<class %u, chan:[", ap->i_class);

		for (size_t i = 3; i < ielen; i++) {
			printf("%s%u", sep, ie[i]);
			sep = ",";
		}
		printf("]>");
	}
#endif
}

void
ifieee80211_printwpaie(if_ctx *ctx, const char *tag, const u_int8_t *ie)
{
	u_int8_t len = ie[1];
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const char *sep;
		int n;

		ie += 6, len -= 4; /* NB: len is payload only */

		xo_emit("{P:/<v%u}", LE_READ_2(ie));
		ie += 2, len -= 2;

		xo_emit("{P:/ mc:%s}", wpa_cipher(ie));
		ie += 4, len -= 4;

		/* unicast ciphers */
		n = LE_READ_2(ie);
		ie += 2, len -= 2;
		sep = " uc:";
		for (; n > 0; n--) {
			xo_emit("{P:/%s%s}", sep, wpa_cipher(ie));
			ie += 4, len -= 4;
			sep = "+";
		}

		/* key management algorithms */
		n = LE_READ_2(ie);
		ie += 2, len -= 2;
		sep = " km:";
		for (; n > 0; n--) {
			xo_emit("{P:/%s%s}", sep, wpa_keymgmt(ie));
			ie += 4, len -= 4;
			sep = "+";
		}

		if (len > 2) /* optional capabilities */
			xo_emit("{P:/, caps 0x%x}", LE_READ_2(ie));
		xo_emit("{P:>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const char *sep;
		int n;

		ie += 6, len -= 4; /* NB: len is payload only */

		printf("<v%u", LE_READ_2(ie));
		ie += 2, len -= 2;

		printf(" mc:%s", wpa_cipher(ie));
		ie += 4, len -= 4;

		/* unicast ciphers */
		n = LE_READ_2(ie);
		ie += 2, len -= 2;
		sep = " uc:";
		for (; n > 0; n--) {
			printf("%s%s", sep, wpa_cipher(ie));
			ie += 4, len -= 4;
			sep = "+";
		}

		/* key management algorithms */
		n = LE_READ_2(ie);
		ie += 2, len -= 2;
		sep = " km:";
		for (; n > 0; n--) {
			printf("%s%s", sep, wpa_keymgmt(ie));
			ie += 4, len -= 4;
			sep = "+";
		}

		if (len > 2) /* optional capabilities */
			printf(", caps 0x%x", LE_READ_2(ie));
		printf(">");
	}
#endif
}

void
ifieee80211_printrsnie(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose) {
		const char *sep;
		int n;

		ie += 2, ielen -= 2;

		xo_emit("{P:/<v%u}", LE_READ_2(ie));
		ie += 2, ielen -= 2;

		xo_emit("{P:/ mc:%s}", rsn_cipher(ie));
		ie += 4, ielen -= 4;

		/* unicast ciphers */
		n = LE_READ_2(ie);
		ie += 2, ielen -= 2;
		sep = " uc:";
		for (; n > 0; n--) {
			xo_emit("{P:/%s%s}", sep, rsn_cipher(ie));
			ie += 4, ielen -= 4;
			sep = "+";
		}

		/* key management algorithms */
		n = LE_READ_2(ie);
		ie += 2, ielen -= 2;
		sep = " km:";
		for (; n > 0; n--) {
			xo_emit("{P:/%s%s}", sep, rsn_keymgmt(ie));
			ie += 4, ielen -= 4;
			sep = "+";
		}

		if (ielen > 2) /* optional capabilities */
			xo_emit("{P:/, caps 0x%x}", LE_READ_2(ie));
		/* XXXPMKID */
		xo_emit("{P:>}");
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose) {
		const char *sep;
		int n;

		ie += 2, ielen -= 2;

		printf("<v%u", LE_READ_2(ie));
		ie += 2, ielen -= 2;

		printf(" mc:%s", rsn_cipher(ie));
		ie += 4, ielen -= 4;

		/* unicast ciphers */
		n = LE_READ_2(ie);
		ie += 2, ielen -= 2;
		sep = " uc:";
		for (; n > 0; n--) {
			printf("%s%s", sep, rsn_cipher(ie));
			ie += 4, ielen -= 4;
			sep = "+";
		}

		/* key management algorithms */
		n = LE_READ_2(ie);
		ie += 2, ielen -= 2;
		sep = " km:";
		for (; n > 0; n--) {
			printf("%s%s", sep, rsn_keymgmt(ie));
			ie += 4, ielen -= 4;
			sep = "+";
		}

		if (ielen > 2) /* optional capabilities */
			printf(", caps 0x%x", LE_READ_2(ie));
		/* XXXPMKID */
		printf(">");
	}
#endif
}

void
ifieee80211_printrsnxe(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen)
{
	size_t n;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (!ctx->args->verbose)
		return;

	ie += 2, ielen -= 2;

	n = (*ie & 0x0f);
	xo_emit("{P:/<%zu}", n + 1);

	/* We do not yet know about more than n=1 (0). */
	if (n != 0)
		goto end;

	if (*ie & 0x10)
		xo_emit("{P: PTWTOPS}");
	if (*ie & 0x20)
		xo_emit("{P: SAE h-t-e}");

end:
	xo_emit("{P:>}");
#else
	printf("%s", tag);
	if (!ctx->args->verbose)
		return;

	ie += 2, ielen -= 2;

	n = (*ie & 0x0f);
	printf("<%zu", n + 1);

	/* We do not yet know about more than n=1 (0). */
	if (n != 0)
		goto end;

	if (*ie & 0x10)
		printf(" PTWTOPS");
	if (*ie & 0x20)
		printf(" SAE h-t-e");

end:
	printf(">");
#endif
}

void
ifieee80211_printwpsie(if_ctx *ctx, const char *tag __unused,
    const u_int8_t *ie)
{
	u_int8_t len = ie[1];
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
#endif
	if (ctx->args->verbose) {
		static const char *dev_pass_id[] = {
			"D", /* Default (PIN) */
			"U", /* User-specified */
			"M", /* Machine-specified */
			"K", /* Rekey */
			"P", /* PushButton */
			"R"  /* Registrar-specified */
		};
		int n;
		int f;

		ie += 6, len -= 4; /* NB: len is payload only */

		/* WPS IE in Beacon and Probe Resp frames have different fields
		 */
#ifdef WITH_LIBXO
		xo_emit("{P:<}");
#else
		printf("<");
#endif
		while (len) {
			uint16_t tlv_type = BE_READ_2(ie);
			uint16_t tlv_len = BE_READ_2(ie + 2);
			uint16_t cfg_mthd;

			/* some devices broadcast invalid WPS frames */
			if (tlv_len > len) {
#ifdef WITH_LIBXO
				xo_emit("{P:/bad frame length tlv_type=0x%02x "
					"tlv_len=%d len=%d}",
				    tlv_type, tlv_len, len);
#else
				printf("bad frame length tlv_type=0x%02x "
				       "tlv_len=%d len=%d",
				    tlv_type, tlv_len, len);
#endif
				break;
			}

			ie += 4, len -= 4;

			switch (tlv_type) {
			case IEEE80211_WPS_ATTR_VERSION:
#ifdef WITH_LIBXO
				xo_emit("{P:/v:%d.%d}", *ie >> 4, *ie & 0xf);
#else
				printf("v:%d.%d", *ie >> 4, *ie & 0xf);
#endif
				break;
			case IEEE80211_WPS_ATTR_AP_SETUP_LOCKED:
#ifdef WITH_LIBXO
				xo_emit("{P:/ ap_setup:%s}",
				    *ie ? "locked" : "unlocked");
#else
				printf(" ap_setup:%s",
				    *ie ? "locked" : "unlocked");
#endif
				break;
			case IEEE80211_WPS_ATTR_CONFIG_METHODS:
			case IEEE80211_WPS_ATTR_SELECTED_REGISTRAR_CONFIG_METHODS:
#ifdef WITH_LIBXO
				if (tlv_type ==
				    IEEE80211_WPS_ATTR_SELECTED_REGISTRAR_CONFIG_METHODS)
					xo_emit("{P: sel_reg_cfg_mthd:}");
				else
					xo_emit("{P: cfg_mthd:}");
#else
				if (tlv_type ==
				    IEEE80211_WPS_ATTR_SELECTED_REGISTRAR_CONFIG_METHODS)
					printf(" sel_reg_cfg_mthd:");
				else
					printf(" cfg_mthd:");
#endif
				cfg_mthd = BE_READ_2(ie);
				f = 0;
				for (n = 15; n >= 0; n--) {
					if (f) {
#ifdef WITH_LIBXO
						xo_emit("{P:,}");
#else
						printf(",");
#endif
						f = 0;
					}
					switch (cfg_mthd & (1 << n)) {
					case 0:
						break;
					case IEEE80211_WPS_CONFIG_USBA:
#ifdef WITH_LIBXO
						xo_emit("{P:usba}");
#else
						printf("usba");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_ETHERNET:
#ifdef WITH_LIBXO
						xo_emit("{P:ethernet}");
#else
						printf("ethernet");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_LABEL:
#ifdef WITH_LIBXO
						xo_emit("{P:label}");
#else
						printf("label");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_DISPLAY:
						if (!(cfg_mthd &
							(IEEE80211_WPS_CONFIG_VIRT_DISPLAY |
							    IEEE80211_WPS_CONFIG_PHY_DISPLAY))) {
#ifdef WITH_LIBXO
							xo_emit("{P:display}");
#else
							printf("display");
#endif
							f++;
						}
						break;
					case IEEE80211_WPS_CONFIG_EXT_NFC_TOKEN:
#ifdef WITH_LIBXO
						xo_emit("{P:ext_nfc_tokenk}");
#else
						printf("ext_nfc_tokenk");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_INT_NFC_TOKEN:
#ifdef WITH_LIBXO
						xo_emit("{P:int_nfc_token}");
#else
						printf("int_nfc_token");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_NFC_INTERFACE:
#ifdef WITH_LIBXO
						xo_emit("{P:nfc_interface}");
#else
						printf("nfc_interface");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_PUSHBUTTON:
						if (!(cfg_mthd &
							(IEEE80211_WPS_CONFIG_VIRT_PUSHBUTTON |
							    IEEE80211_WPS_CONFIG_PHY_PUSHBUTTON))) {
#ifdef WITH_LIBXO
							xo_emit(
							    "{P:push_button}");
#else
							printf("push_button");
#endif
							f++;
						}
						break;
					case IEEE80211_WPS_CONFIG_KEYPAD:
#ifdef WITH_LIBXO
						xo_emit("{P:keypad}");
#else
						printf("keypad");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_VIRT_PUSHBUTTON:
#ifdef WITH_LIBXO
						xo_emit(
						    "{P:virtual_push_button}");
#else
						printf("virtual_push_button");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_PHY_PUSHBUTTON:
#ifdef WITH_LIBXO
						xo_emit(
						    "{P:physical_push_button}");
#else
						printf("physical_push_button");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_P2PS:
#ifdef WITH_LIBXO
						xo_emit("{P:p2ps}");
#else
						printf("p2ps");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_VIRT_DISPLAY:
#ifdef WITH_LIBXO
						xo_emit("{P:virtual_display}");
#else
						printf("virtual_display");
#endif
						f++;
						break;
					case IEEE80211_WPS_CONFIG_PHY_DISPLAY:
#ifdef WITH_LIBXO
						xo_emit("{P:physical_display}");
#else
						printf("physical_display");
#endif
						f++;
						break;
					default:
#ifdef WITH_LIBXO
						xo_emit(
						    "{P:/unknown_wps_config<%04x>}",
						    cfg_mthd & (1 << n));
#else
						printf(
						    "unknown_wps_config<%04x>",
						    cfg_mthd & (1 << n));
#endif
						f++;
						break;
					}
				}
				break;
			case IEEE80211_WPS_ATTR_DEV_NAME:
#ifdef WITH_LIBXO
				xo_emit("{P:/ device_name:<%.*s>}", tlv_len,
				    ie);
#else
				printf(" device_name:<%.*s>", tlv_len, ie);
#endif
				break;
			case IEEE80211_WPS_ATTR_DEV_PASSWORD_ID:
				n = LE_READ_2(ie);
				if (n < (int)nitems(dev_pass_id))
#ifdef WITH_LIBXO
					xo_emit("{P:/ dpi:%s}", dev_pass_id[n]);
#else
					printf(" dpi:%s", dev_pass_id[n]);
#endif
				break;
			case IEEE80211_WPS_ATTR_MANUFACTURER:
#ifdef WITH_LIBXO
				xo_emit("{P:/ manufacturer:<%.*s>}", tlv_len,
				    ie);
#else
				printf(" manufacturer:<%.*s>", tlv_len, ie);
#endif
				break;
			case IEEE80211_WPS_ATTR_MODEL_NAME:
#ifdef WITH_LIBXO
				xo_emit("{P:/ model_name:<%.*s>}", tlv_len, ie);
#else
				printf(" model_name:<%.*s>", tlv_len, ie);
#endif
				break;
			case IEEE80211_WPS_ATTR_MODEL_NUMBER:
#ifdef WITH_LIBXO
				xo_emit("{P:/ model_number:<%.*s>}", tlv_len,
				    ie);
#else
				printf(" model_number:<%.*s>", tlv_len, ie);
#endif
				break;
			case IEEE80211_WPS_ATTR_PRIMARY_DEV_TYPE:
#ifdef WITH_LIBXO
				xo_emit("{P: prim_dev:}");
				for (n = 0; n < tlv_len; n++)
					xo_emit("{P:/%02x}", ie[n]);
#else
				printf(" prim_dev:");
				for (n = 0; n < tlv_len; n++)
					printf("%02x", ie[n]);
#endif
				break;
			case IEEE80211_WPS_ATTR_RF_BANDS:
#ifdef WITH_LIBXO
				xo_emit("{P: rf:}");
				f = 0;
				for (n = 7; n >= 0; n--) {
					if (f) {
						xo_emit("{P:,}");
						f = 0;
					}
					switch (*ie & (1 << n)) {
					case 0:
						break;
					case IEEE80211_WPS_RF_BAND_24GHZ:
						xo_emit("{P:2.4Ghz}");
						f++;
						break;
					case IEEE80211_WPS_RF_BAND_50GHZ:
						xo_emit("{P:5Ghz}");
						f++;
						break;
					case IEEE80211_WPS_RF_BAND_600GHZ:
						xo_emit("{P:60Ghz}");
						f++;
						break;
					default:
						xo_emit("{P:/unknown<%02x>}",
						    *ie & (1 << n));
						f++;
						break;
					}
				}
#else
				printf(" rf:");
				f = 0;
				for (n = 7; n >= 0; n--) {
					if (f) {
#ifdef WITH_LIBXO
						xo_emit("{P:,}");
#else
						printf(",");
#endif
						f = 0;
					}
					switch (*ie & (1 << n)) {
					case 0:
						break;
					case IEEE80211_WPS_RF_BAND_24GHZ:
						printf("2.4Ghz");
						f++;
						break;
					case IEEE80211_WPS_RF_BAND_50GHZ:
						printf("5Ghz");
						f++;
						break;
					case IEEE80211_WPS_RF_BAND_600GHZ:
						printf("60Ghz");
						f++;
						break;
					default:
						printf("unknown<%02x>",
						    *ie & (1 << n));
						f++;
						break;
					}
				}
#endif
				break;
			case IEEE80211_WPS_ATTR_RESPONSE_TYPE:
#ifdef WITH_LIBXO
				xo_emit("{P:/ resp_type:0x%02x}", *ie);
#else
				printf(" resp_type:0x%02x", *ie);
#endif
				break;
			case IEEE80211_WPS_ATTR_SELECTED_REGISTRAR:
#ifdef WITH_LIBXO
				xo_emit("{P:/ sel:%s}", *ie ? "T" : "F");
#else
				printf(" sel:%s", *ie ? "T" : "F");
#endif
				break;
			case IEEE80211_WPS_ATTR_SERIAL_NUMBER:
#ifdef WITH_LIBXO
				xo_emit("{P:/ serial_number:<%.*s>}", tlv_len,
				    ie);
#else
				printf(" serial_number:<%.*s>", tlv_len, ie);
#endif
				break;
			case IEEE80211_WPS_ATTR_UUID_E:
#ifdef WITH_LIBXO
				xo_emit("{P: uuid-e:}");
				for (n = 0; n < (tlv_len - 1); n++)
					xo_emit("{P:/%02x-}", ie[n]);
				xo_emit("{P:/%02x}", ie[n]);
#else
				printf(" uuid-e:");
				for (n = 0; n < (tlv_len - 1); n++)
					printf("%02x-", ie[n]);
				printf("%02x", ie[n]);
#endif
				break;
			case IEEE80211_WPS_ATTR_VENDOR_EXT:
#ifdef WITH_LIBXO
				xo_emit("{P: vendor:}");
				for (n = 0; n < tlv_len; n++)
					xo_emit("{P:/%02x}", ie[n]);
#else
				printf(" vendor:");
				for (n = 0; n < tlv_len; n++)
					printf("%02x", ie[n]);
#endif
				break;
			case IEEE80211_WPS_ATTR_WPS_STATE:
				switch (*ie) {
				case IEEE80211_WPS_STATE_NOT_CONFIGURED:
#ifdef WITH_LIBXO
					xo_emit("{P: state:N}");
#else
					printf(" state:N");
#endif
					break;
				case IEEE80211_WPS_STATE_CONFIGURED:
#ifdef WITH_LIBXO
					xo_emit("{P: state:C}");
#else
					printf(" state:C");
#endif
					break;
				default:
#ifdef WITH_LIBXO
					xo_emit("{P:/ state:B<%02x>}", *ie);
#else
					printf(" state:B<%02x>", *ie);
#endif
					break;
				}
				break;
			default:
#ifdef WITH_LIBXO
				xo_emit("{P:/ unknown_wps_attr:0x%x}",
				    tlv_type);
#else
				printf(" unknown_wps_attr:0x%x", tlv_type);
#endif
				break;
			}
			ie += tlv_len, len -= tlv_len;
		}
#ifdef WITH_LIBXO
		xo_emit("{P:>}");
#else
		printf(">");
#endif
	}
}

void
ifieee80211_printtdmaie(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	if (ctx->args->verbose &&
	    ielen >= sizeof(struct ieee80211_tdma_param)) {
		const struct ieee80211_tdma_param *tdma =
		    (const struct ieee80211_tdma_param *)ie;

		/* XXX tstamp */
		xo_emit(
		    "{P:/<v%u slot:%u slotcnt:%u slotlen:%u bintval:%u inuse:0x%x>}",
		    tdma->tdma_version, tdma->tdma_slot, tdma->tdma_slotcnt,
		    LE_READ_2(&tdma->tdma_slotlen), tdma->tdma_bintval,
		    tdma->tdma_inuse[0]);
	}
#else
	printf("%s", tag);
	if (ctx->args->verbose &&
	    ielen >= sizeof(struct ieee80211_tdma_param)) {
		const struct ieee80211_tdma_param *tdma =
		    (const struct ieee80211_tdma_param *)ie;

		/* XXX tstamp */
		printf(
		    "<v%u slot:%u slotcnt:%u slotlen:%u bintval:%u inuse:0x%x>",
		    tdma->tdma_version, tdma->tdma_slot, tdma->tdma_slotcnt,
		    LE_READ_2(&tdma->tdma_slotlen), tdma->tdma_bintval,
		    tdma->tdma_inuse[0]);
	}
#endif
}

void
ifieee80211_printssid(const char *tag, const u_int8_t *ie, int maxlen)
{
	char ssid[2 * IEEE80211_NWID_LEN + 1];
#ifdef WITH_LIBXO
	xo_emit("{P:/%s<%.*s>}", tag, copy_essid(ssid, maxlen, ie + 2, ie[1]),
	    ssid);
#else
	printf("%s<%.*s>", tag, copy_essid(ssid, maxlen, ie + 2, ie[1]), ssid);
#endif
}

void
ifieee80211_printrates(const char *tag, const u_int8_t *ie, size_t ielen)
{
	const char *sep;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
	sep = "<";
	for (size_t i = 2; i < ielen; i++) {
		xo_emit("{P:/%s%s%d}", sep,
		    ie[i] & IEEE80211_RATE_BASIC ? "B" : "",
		    ie[i] & IEEE80211_RATE_VAL);
		sep = ",";
	}
	xo_emit("{P:>}");
#else
	printf("%s", tag);
	sep = "<";
	for (size_t i = 2; i < ielen; i++) {
		printf("%s%s%d", sep, ie[i] & IEEE80211_RATE_BASIC ? "B" : "",
		    ie[i] & IEEE80211_RATE_VAL);
		sep = ",";
	}
	printf(">");
#endif
}

void
ifieee80211_printcountry(const char *tag, const u_int8_t *ie)
{
	const struct ieee80211_country_ie *cie =
	    (const struct ieee80211_country_ie *)ie;
	int i, nbands, schan, nchan;
#ifdef WITH_LIBXO
	xo_emit("{P:/%s<%c%c%c}", tag, cie->cc[0], cie->cc[1], cie->cc[2]);
	nbands = (cie->len - 3) / sizeof(cie->band[0]);
	for (i = 0; i < nbands; i++) {
		schan = cie->band[i].schan;
		nchan = cie->band[i].nchan;
		if (nchan != 1)
			xo_emit("{P:/ %u-%u,%u}", schan, schan + nchan - 1,
			    cie->band[i].maxtxpwr);
		else
			xo_emit("{P:/ %u,%u}", schan, cie->band[i].maxtxpwr);
	}
	xo_emit("{P:>}");
#else
	printf("%s<%c%c%c", tag, cie->cc[0], cie->cc[1], cie->cc[2]);
	nbands = (cie->len - 3) / sizeof(cie->band[0]);
	for (i = 0; i < nbands; i++) {
		schan = cie->band[i].schan;
		nchan = cie->band[i].nchan;
		if (nchan != 1)
			printf(" %u-%u,%u", schan, schan + nchan - 1,
			    cie->band[i].maxtxpwr);
		else
			printf(" %u,%u", schan, cie->band[i].maxtxpwr);
	}
	printf(">");
#endif
}

void
ifieee80211_printexties(if_ctx *ctx, const u_int8_t *vp,
    unsigned int maxifieee80211_cols)
{
	const int verbose = ctx->args->verbose;

	if (vp[1] < 1)
		return;

	switch (vp[2]) {
	case IEEE80211_ELEMID_EXT_HE_CAPA:
		ifieee80211_printhecap(ctx, " HECAP", vp);
		break;
	case IEEE80211_ELEMID_EXT_HE_OPER:
		ifieee80211_printheoper(ctx, " HEOPER", vp);
		break;
	case IEEE80211_ELEMID_EXT_MU_EDCA_PARAM_SET:
		ifieee80211_printmuedcaparamset(ctx, " MU_EDCA_PARAM_SET", vp);
		break;
	default:
		if (verbose)
			ifieee80211_printie(ctx, iename(vp[0], vp), vp,
			    2 + vp[1], maxifieee80211_cols);
		break;
	}
}

void
ifieee80211_printies(if_ctx *ctx, const u_int8_t *vp, int ielen,
    unsigned int maxifieee80211_cols)
{
	const int verbose = ctx->args->verbose;

	while (ielen > 0) {
		switch (vp[0]) {
		case IEEE80211_ELEMID_SSID:
			if (verbose)
				ifieee80211_printssid(" SSID", vp,
				    maxifieee80211_cols);
			break;
		case IEEE80211_ELEMID_RATES:
		case IEEE80211_ELEMID_XRATES:
			if (verbose)
				ifieee80211_printrates(vp[0] ==
					    IEEE80211_ELEMID_RATES ?
					" RATES" :
					" XRATES",
				    vp, 2 + vp[1]);
			break;
		case IEEE80211_ELEMID_DSPARMS:
			if (verbose)
#ifdef WITH_LIBXO
				xo_emit("{P:/ DSPARMS<%u>}", vp[2]);
#else
				printf(" DSPARMS<%u>", vp[2]);
#endif
			break;
		case IEEE80211_ELEMID_COUNTRY:
			if (verbose)
				ifieee80211_printcountry(" COUNTRY", vp);
			break;
		case IEEE80211_ELEMID_ERP:
			if (verbose)
#ifdef WITH_LIBXO
				xo_emit("{P:/ ERP<0x%x>}", vp[2]);
#else
				printf(" ERP<0x%x>", vp[2]);
#endif
			break;
		case IEEE80211_ELEMID_VENDOR:
			if (iswpaoui(vp))
				ifieee80211_printwpaie(ctx, " WPA", vp);
			else if (iswmeinfo(vp))
				ifieee80211_printwmeinfo(ctx, " WME", vp);
			else if (iswmeparam(vp))
				ifieee80211_printwmeparam(ctx, " WME", vp);
			else if (isatherosoui(vp))
				ifieee80211_printathie(ctx, " ATH", vp);
			else if (iswpsoui(vp))
				ifieee80211_printwpsie(ctx, " WPS", vp);
			else if (istdmaoui(vp))
				ifieee80211_printtdmaie(ctx, " TDMA", vp,
				    2 + vp[1]);
			else if (verbose)
				ifieee80211_printie(ctx, " VEN", vp, 2 + vp[1],
				    maxifieee80211_cols);
			break;
		case IEEE80211_ELEMID_RSN:
			ifieee80211_printrsnie(ctx, " RSN", vp, 2 + vp[1]);
			break;
		case IEEE80211_ELEMID_HTCAP:
			ifieee80211_printhtcap(ctx, " HTCAP", vp);
			break;
		case IEEE80211_ELEMID_SUP_OP_CLASS:
			ifieee80211_printsupopclass(ctx, " SUP_OP_CLASS", vp);
			break;
		case IEEE80211_ELEMID_HTINFO:
			if (verbose)
				ifieee80211_printhtinfo(ctx, " HTINFO", vp);
			break;
		case IEEE80211_ELEMID_MESHID:
			if (verbose)
				ifieee80211_printssid(" MESHID", vp,
				    maxifieee80211_cols);
			break;
		case IEEE80211_ELEMID_MESHCONF:
			ifieee80211_printmeshconf(ctx, " MESHCONF", vp);
			break;
		case IEEE80211_ELEMID_VHT_CAP:
			ifieee80211_printvhtcap(ctx, " VHTCAP", vp);
			break;
		case IEEE80211_ELEMID_VHT_OPMODE:
			ifieee80211_printvhtinfo(ctx, " VHTOPMODE", vp);
			break;
		case IEEE80211_ELEMID_VHT_PWR_ENV:
			ifieee80211_printvhtpwrenv(ctx, " VHTPWRENV", vp,
			    2 + vp[1]);
			break;
		case IEEE80211_ELEMID_BSSLOAD:
			ifieee80211_printbssload(ctx, " BSSLOAD", vp);
			break;
		case IEEE80211_ELEMID_APCHANREP:
			ifieee80211_printapchanrep(ctx, " APCHANREP", vp,
			    2 + vp[1]);
			break;
		case IEEE80211_ELEMID_RSN_EXT:
			ifieee80211_printrsnxe(ctx, " RSNXE", vp, 2 + vp[1]);
			break;
		case IEEE80211_ELEMID_EXTFIELD:
			ifieee80211_printexties(ctx, vp, maxifieee80211_cols);
			break;
		default:
			if (verbose)
				ifieee80211_printie(ctx, iename(vp[0], vp), vp,
				    2 + vp[1], maxifieee80211_cols);
			break;
		}
		ielen -= 2 + vp[1];
		vp += 2 + vp[1];
	}
}

void
ifieee80211_printmimo(const struct ieee80211_mimo_info *mi)
{
	int i;
	int r = 0;

	for (i = 0; i < IEEE80211_MAX_CHAINS; i++) {
		if (mi->ch[i].rssi[0] != 0) {
			r = 1;
			break;
		}
	}

	/* NB: don't muddy display unless there's something to show */
	if (r == 0)
		return;
#ifdef WITH_LIBXO
	/* XXX TODO: ignore EVM; secondary channels for now */
	xo_emit("{P:/ (rssi %.1f:%.1f:%.1f:%.1f nf %d:%d:%d:%d)}",
	    mi->ch[0].rssi[0] / 2.0, mi->ch[1].rssi[0] / 2.0,
	    mi->ch[2].rssi[0] / 2.0, mi->ch[3].rssi[0] / 2.0,
	    mi->ch[0].noise[0], mi->ch[1].noise[0], mi->ch[2].noise[0],
	    mi->ch[3].noise[0]);
#else
	/* XXX TODO: ignore EVM; secondary channels for now */
	printf(" (rssi %.1f:%.1f:%.1f:%.1f nf %d:%d:%d:%d)",
	    mi->ch[0].rssi[0] / 2.0, mi->ch[1].rssi[0] / 2.0,
	    mi->ch[2].rssi[0] / 2.0, mi->ch[3].rssi[0] / 2.0,
	    mi->ch[0].noise[0], mi->ch[1].noise[0], mi->ch[2].noise[0],
	    mi->ch[3].noise[0]);
#endif
}

void
ifieee80211_printbssidname(const struct ether_addr *n)
{
	char name[MAXHOSTNAMELEN + 1];

	if (ether_ntohost(name, n) != 0)
		return;
#ifdef WITH_LIBXO
	xo_emit("{P:/ (%s)}", name);
#else
	printf(" (%s)", name);
#endif
}

void
ifieee80211_printpolicy(int policy)
{
#ifdef WITH_LIBXO
	switch (policy) {
	case IEEE80211_MACCMD_POLICY_OPEN:
		xo_emit("{P:policy: open\n}");
		break;
	case IEEE80211_MACCMD_POLICY_ALLOW:
		xo_emit("{P:policy: allow\n}");
		break;
	case IEEE80211_MACCMD_POLICY_DENY:
		xo_emit("{P:policy: deny\n}");
		break;
	case IEEE80211_MACCMD_POLICY_RADIUS:
		xo_emit("{P:policy: radius\n}");
		break;
	default:
		xo_emit("{P:/policy: unknown (%u)\n}", policy);
		break;
	}
#else
	switch (policy) {
	case IEEE80211_MACCMD_POLICY_OPEN:
		printf("policy: open\n");
		break;
	case IEEE80211_MACCMD_POLICY_ALLOW:
		printf("policy: allow\n");
		break;
	case IEEE80211_MACCMD_POLICY_DENY:
		printf("policy: deny\n");
		break;
	case IEEE80211_MACCMD_POLICY_RADIUS:
		printf("policy: radius\n");
		break;
	default:
		printf("policy: unknown (%u)\n", policy);
		break;
	}
#endif
}

void
ifmedia_print_media(ifmedia_t media, bool print_toptype)
{
	const char *val, **options;
#ifdef WITH_LIBXO
	xo_open_container("media");

	val = ifconfig_media_get_type(media);
	if (val == NULL) {
		xo_emit("{P:<unknown type>}");
		xo_close_container("media");
		return;
	} else if (print_toptype) {
		xo_emit("{:type/%s}", val);
	}

	val = ifconfig_media_get_subtype(media);
	if (val == NULL) {
		xo_emit("{P:<unknown subtype>}");
		xo_close_container("media");
		return;
	}

	if (print_toptype)
		xo_emit("{P: }");

	xo_emit("{:subtype/%s}", val);

	if (print_toptype) {
		val = ifconfig_media_get_mode(media);
		if (val != NULL && strcasecmp("autoselect", val) != 0)
			xo_emit("{P: mode }{:mode/%s}", val);
	}

	options = ifconfig_media_get_options(media);
	if (options != NULL && options[0] != NULL) {
		xo_emit("{P: <}");
		xo_open_list("option");
		for (size_t i = 0; options[i] != NULL; ++i) {
			if (i > 0)
				xo_emit("{P:,}");
			xo_emit("{P:/%s}", options[i]);
			xo_emit("{le:option/%s}", options[i]);
		}
		xo_close_list("option");
		xo_emit("{P:>}");
	}
	free(options);

	if (print_toptype && IFM_INST(media) != 0)
		xo_emit("{P: instance }{:instance/%d}", IFM_INST(media));

	xo_close_container("media");
#else
	val = ifconfig_media_get_type(media);
	if (val == NULL) {
		printf("<unknown type>");
		return;
	} else if (print_toptype) {
		printf("%s", val);
	}

	val = ifconfig_media_get_subtype(media);
	if (val == NULL) {
		printf("<unknown subtype>");
		return;
	}

	if (print_toptype)
		ifconfig_print_space();

	printf("%s", val);

	if (print_toptype) {
		val = ifconfig_media_get_mode(media);
		if (val != NULL && strcasecmp("autoselect", val) != 0)
			printf(" mode %s", val);
	}

	options = ifconfig_media_get_options(media);
	if (options != NULL && options[0] != NULL) {
		printf(" <%s", options[0]);
		for (size_t i = 1; options[i] != NULL; ++i)
			printf(",%s", options[i]);
		printf(">");
	}
	free(options);

	if (print_toptype && IFM_INST(media) != 0)
		printf(" instance %d", IFM_INST(media));
#endif
}

void
ifmedia_print_media_ifconfig(ifmedia_t media)
{
#ifdef WITH_LIBXO
	const char *val, **options;

	xo_open_container("media");

	val = ifconfig_media_get_type(media);
	if (val == NULL) {
		xo_emit("{P:<unknown type>}");
		xo_close_container("media");
		return;
	}

	/*
	 * Don't print the top-level type; it's not like we can
	 * change it, or anything.
	 */

	val = ifconfig_media_get_subtype(media);
	if (val == NULL) {
		xo_emit("{P:<unknown subtype>}");
		xo_close_container("media");
		return;
	}

	xo_emit("{P:media }{:subtype/%s}", val);

	val = ifconfig_media_get_mode(media);
	if (val != NULL)
		xo_emit("{P: mode }{:mode/%s}", val);

	options = ifconfig_media_get_options(media);
	if (options != NULL && options[0] != NULL) {
		xo_emit("{P: mediaopt }");
		xo_open_list("option");
		for (size_t i = 0; options[i] != NULL; ++i) {
			if (i > 0)
				xo_emit("{P:,}");
			xo_emit("{P:/%s}", options[i]);
			xo_emit("{le:option/%s}", options[i]);
		}
		xo_close_list("option");
	}
	free(options);

	if (IFM_INST(media) != 0)
		xo_emit("{P: instance }{:instance/%d}", IFM_INST(media));

	xo_close_container("media");
#else
	const char *val, **options;

	val = ifconfig_media_get_type(media);
	if (val == NULL) {
		printf("<unknown type>");
		return;
	}

	/*
	 * Don't print the top-level type; it's not like we can
	 * change it, or anything.
	 */

	val = ifconfig_media_get_subtype(media);
	if (val == NULL) {
		printf("<unknown subtype>");
		return;
	}

	printf("media %s", val);

	val = ifconfig_media_get_mode(media);
	if (val != NULL)
		printf(" mode %s", val);

	options = ifconfig_media_get_options(media);
	if (options != NULL && options[0] != NULL) {
		printf(" mediaopt %s", options[0]);
		for (size_t i = 1; options[i] != NULL; ++i)
			printf(",%s", options[i]);
	}
	free(options);

	if (IFM_INST(media) != 0)
		printf(" instance %d", IFM_INST(media));
#endif
}

void
if_err(int eval, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef WITH_LIBXO
	xo_emit_err_v(eval, errno, fmt, ap);
#else
	verr(eval, fmt, ap);
#endif
	va_end(ap);
}

void
if_errc(int eval, int code, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef WITH_LIBXO
	xo_emit_err_v(eval, code, fmt, ap);
#else
	verrc(eval, code, fmt, ap);
#endif
	va_end(ap);
}

void
if_errx(int eval, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef WITH_LIBXO
	xo_emit_err_v(eval, -1, fmt, ap);
#else
	verrx(eval, fmt, ap);
#endif
	va_end(ap);
}

void
if_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef WITH_LIBXO
	xo_emit_warn_hcv(NULL, 1, errno, fmt, ap);
#else
	vwarn(fmt, ap);
#endif
	va_end(ap);
}

void
if_warnc(int cond, int code, const char *fmt, ...)
{
	if (cond) {
		va_list ap;
		va_start(ap, fmt);
#ifdef WITH_LIBXO
		xo_emit_warn_hcv(NULL, 1, code, fmt, ap);
#else
		vwarnc(code, fmt, ap);
#endif
		va_end(ap);
	}
}

void
if_warnx(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef WITH_LIBXO
	xo_emit_warn_hcv(NULL, 1, -1, fmt, ap);
#else
	vwarnx(fmt, ap);
#endif
	va_end(ap);
}

void
ifconfig_print_space(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: }");
#else
	printf(" ");
#endif
}

void
ifconfig_print_char(char c)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%c}", c);
#else
	printf("%c", c);
#endif
}

void
ifconfig_print_tab(void)
{
#ifdef WITH_LIBXO
	xo_emit("\t");
#else
	printf("\t");
#endif
}

void
ifconfig_print_newline(void)
{
#ifdef WITH_LIBXO
	xo_emit("\n");
#else
	printf("\n");
#endif
}

void
ifconfig_print_flags(if_ctx *ctx, const struct ifaddrs *ifa)
{
#ifdef WITH_LIBXO
	xo_emit("{:ifname/%s}: flags={:flags-hex/%x}", ctx->ifname,
	    ifa->ifa_flags);
#else
	printf("%s: flags=%x", ctx->ifname, ifa->ifa_flags);
#endif
}

void
ifclone_print_cloners(const char *name)
{
#ifdef WITH_LIBXO
	xo_emit("{:cloner-name/%s}", name);
#else
	printf("%s", name);
#endif
}

void
af_inet_print_addr_pointtopoint(struct sockaddr_in *sin)
{
#ifdef WITH_LIBXO
	xo_emit(" --> {:dst-addr/%s}", inet_ntoa(sin->sin_addr));
#else
	printf(" --> %s", inet_ntoa(sin->sin_addr));
#endif
}

void
af_inet_print_cidr_mask(int cidr)
{
#ifdef WITH_LIBXO
	xo_emit("/{:prefixlen/%d}", cidr);
#else
	printf("/%d", cidr);
#endif
}

void
af_inet_print_netmask_str(struct in_addr addr)
{
#ifdef WITH_LIBXO
	xo_emit(" netmask {:netmask/%s}", inet_ntoa(addr));
#else
	printf(" netmask %s", inet_ntoa(addr));
#endif
}

void
af_inet_print_netmask(struct in_addr addr)
{
#ifdef WITH_LIBXO
	xo_emit(" netmask {:netmask/0x%lx}", (unsigned long)ntohl(addr.s_addr));
#else
	printf(" netmask 0x%lx", (unsigned long)ntohl(addr.s_addr));
#endif
}

void
af_inet_print_broadcast(struct sockaddr_in *sin)
{
#ifdef WITH_LIBXO
	xo_emit(" broadcast {:broadcast/%s}", inet_ntoa(sin->sin_addr));
#else
	printf(" broadcast %s", inet_ntoa(sin->sin_addr));
#endif
}

void
af_inet_print_vhid(if_addr_t *ifa)
{
#ifdef WITH_LIBXO
	xo_emit(" vhid {:vhid/%d}", ifa->ifaf_vhid);
#else
	printf(" vhid %d", ifa->ifaf_vhid);
#endif
}

void
af_inet_print_tunnel_inet(const char *src, const char *dst)
{
#ifdef WITH_LIBXO
	xo_emit("\ttunnel inet {:tunnel-src/%s} --> {:tunnel-dst/%s}\n", src,
	    dst);
#else
	printf("\ttunnel inet %s --> %s\n", src, dst);
#endif
}

void
af_inet6_print_scopeid(uint32_t scopeid)
{
#ifdef WITH_LIBXO
	xo_emit(" scopeid {:scopeid/0x%x}", scopeid);
#else
	printf(" scopeid 0x%x", scopeid);
#endif
}

void
af_inet6_print_vhid(if_addr_t *ifa)
{
#ifdef WITH_LIBXO
	xo_emit(" vhid {:vhid/%d}", ifa->ifaf_vhid);
#else
	printf(" vhid %d", ifa->ifaf_vhid);
#endif
}

void
af_inet6_print_tunnel(const char *src, const char *dst)
{
#ifdef WITH_LIBXO
	xo_emit("\ttunnel inet6 {:tunnel-src/%s} --> {:tunnel-dst/%s}\n", src,
	    dst);
#else
	printf("\ttunnel inet6 %s --> %s\n", src, dst);
#endif
}

void
af_nd6_print_options(uint32_t bits)
{
#ifdef WITH_LIBXO
	xo_emit("\tnd6 options={:nd6-options-hex/%x}", bits);
#else
	printf("\tnd6 options=%x", bits);
#endif
}

void
carp_print_summary(const char *state, struct ifconfig_carp *carpr)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tcarp: }{:carp-state/%s}{P: vhid }{:vhid/%d}"
		"{P: advbase }{:advbase/%d}{P: advskew }{:advskew/%d}",
	    state, carpr->carpr_vhid, carpr->carpr_advbase,
	    carpr->carpr_advskew);
#else
	printf("\tcarp: %s vhid %d advbase %d advskew %d", state,
	    carpr->carpr_vhid, carpr->carpr_advbase, carpr->carpr_advskew);
#endif
}

void
carp_print_key(struct ifconfig_carp *carpr)
{
#ifdef WITH_LIBXO
	xo_emit("{P: key \"}{:carp_key/%s}{P:\"}\n", carpr->carpr_key);
#else
	printf(" key \"%s\"\n", carpr->carpr_key);
#endif
}

void
carp_print_peer(struct ifconfig_carp *carpr, const char *peer_addr)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t      peer }{:carp_peer/%s}{P: peer6 }{:carp_peer6/%s}\n",
	    inet_ntoa(carpr->carpr_addr), peer_addr);
#else
	printf("\t      peer %s peer6 %s\n", inet_ntoa(carpr->carpr_addr),
	    peer_addr);
#endif
}

void
carp_print_vrrp3(const char *state, struct ifconfig_carp *carpr)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tvrrp: }{:vrrp_state/%s}{P: vrid }{:vrid/%d}"
		"{P: prio }{:vrrp_prio/%d}{P: interval }{:vrrp_interval/%d}\n",
	    state, carpr->carpr_vhid, carpr->carpr_vrrp_prio,
	    carpr->carpr_vrrp_adv_inter);
#else
	printf("\tvrrp: %s vrid %d prio %d interval %d\n", state,
	    carpr->carpr_vhid, carpr->carpr_vrrp_prio,
	    carpr->carpr_vrrp_adv_inter);
#endif
}

void
iffib_print_fib(struct ifreq *ifr)
{
#ifdef WITH_LIBXO
	xo_emit("\tfib: {:fib-id/%u}\n", ifr->ifr_fib);
#else
	printf("\tfib: %u\n", ifr->ifr_fib);
#endif
}

void
iffib_print_tunnelfib(struct ifreq *ifr)
{
#ifdef WITH_LIBXO
	xo_emit("\ttunnelfib: {:tunnelfib-id/%u}\n", ifr->ifr_fib);
#else
	printf("\ttunnelfib: %u\n", ifr->ifr_fib);
#endif
}

void
ifgif_print_options(int opts)
{
#ifdef WITH_LIBXO
	xo_emit("\toptions={:gif-options-hex/%x}", opts);
#else
	printf("\toptions=%x", opts);
#endif
}

void
ifgre_print_key(uint32_t opts)
{
#ifdef WITH_LIBXO
	xo_emit("\tgrekey: 0x{:x} ({:u})\n", opts, opts);
#else
	printf("\tgrekey: 0x%x (%u)\n", opts, opts);
#endif
}

void
ifgre_print_udpport(uint32_t port)
{
#ifdef WITH_LIBXO
	xo_emit("\tudpport: {:udpport/%u}\n", port);
#else
	printf("\tudpport: %u\n", port);
#endif
}

void
ifgre_print_options(uint32_t opts)
{
#ifdef WITH_LIBXO
	xo_emit("\toptions={:options/%x}", opts);
#else
	printf("\toptions=%x", opts);
#endif
}

void
ifgroup_print_groups(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tgroups:}");
#else
	printf("\tgroups:");
#endif
}

void
ifgroup_open_groups(void)
{
#ifdef WITH_LIBXO
	if (xo_get_style(NULL) == XO_STYLE_XML) {
		ifconfig_open_container("groups");
		return;
	}
	ifconfig_open_list("groups");
#endif
	ifgroup_print_groups();
}

void
ifgroup_close_groups(void)
{
#ifdef WITH_LIBXO
	switch (xo_get_style(NULL)) {
	case XO_STYLE_XML:
		ifconfig_close_container("groups");
		return;
	case XO_STYLE_TEXT:
		break;
	default:
		ifconfig_close_list("groups");
		return;
	}
#endif
	ifconfig_print_newline();
}

void
ifgroup_print_group(struct ifg_req *ifg)
{
#ifdef WITH_LIBXO
	switch (xo_get_style(NULL)) {
	case XO_STYLE_XML:
		xo_emit("{:group/%s}", ifg->ifgrq_group);
		return;
	case XO_STYLE_JSON:
		xo_emit("{le:group/%s}", ifg->ifgrq_group);
		return;
	}
	xo_emit("{P: }{:name/%s}", ifg->ifgrq_group);
#else
	printf(" %s", ifg->ifgrq_group);
#endif
}

void
ifipsec_print_reqid(uint32_t reqid)
{
#ifdef WITH_LIBXO
	xo_emit("\treqid: {:reqid/%u}\n", reqid);
#else
	printf("\treqid: %u\n", reqid);
#endif
}

void
iflagg_print_laggproto(const char *proto)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tlaggproto }{:lagg-proto/%s}", proto);
#else
	printf("\tlaggproto %s", proto);
#endif
}

void
iflagg_print_lagghash(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: lagghash }");
#else
	printf(" lagghash ");
#endif
}

void
iflagg_print_l2(const char *sep)
{
#ifdef WITH_LIBXO
	if (sep[0] != '\0')
		xo_emit("{P:,}");
	xo_emit("{:lagg-hash-l2/%s}", "l2");
#else
	printf("%sl2", sep);
#endif
}

void
iflagg_print_l3(const char *sep)
{
#ifdef WITH_LIBXO
	if (sep[0] != '\0')
		xo_emit("{P:,}");
	xo_emit("{:lagg-hash-l3/%s}", "l3");
#else
	printf("%sl3", sep);
#endif
}

void
iflagg_print_l4(const char *sep)
{
#ifdef WITH_LIBXO
	if (sep[0] != '\0')
		xo_emit("{P:,}");
	xo_emit("{:lagg-hash-l4/%s}", "l4");
#else
	printf("%sl4", sep);
#endif
}

void
iflagg_print_options(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tlagg options:}\n");
#else
	printf("\tlagg options:\n");
#endif
}

void
iflagg_print_flowid_shift(struct lagg_reqopts *ro)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t\tflowid_shift: }{:flowid-shift/%d}\n",
	    ro->ro_flowid_shift);
#else
	printf("\t\tflowid_shift: %d\n", ro->ro_flowid_shift);
#endif
}

void
iflagg_print_trr_limit(struct lagg_reqopts *ro)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t\trr_limit: }{:rr-limit/%d}\n", ro->ro_bkt);
#else
	printf("\t\trr_limit: %d\n", ro->ro_bkt);
#endif
}

void
iflagg_print_statistics(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tlagg statistics:}\n");
#else
	printf("\tlagg statistics:\n");
#endif
}

void
iflagg_print_active_ports(struct lagg_reqopts *ro)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t\tactive ports: }{:active-ports/%d}\n", ro->ro_active);
#else
	printf("\t\tactive ports: %d\n", ro->ro_active);
#endif
}

void
iflagg_print_flapping(struct lagg_reqopts *ro)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t\tflapping: }{:flapping/%u}\n", ro->ro_flapping);
#else
	printf("\t\tflapping: %u\n", ro->ro_flapping);
#endif
}

void
iflagg_print_lagg_id(struct lacp_opreq *lp)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tlag id: }{:lag-id/%s}\n",
	    lacp_format_peer(lp, "\n\t\t "));
#else
	printf("\tlag id: %s\n", lacp_format_peer(lp, "\n\t\t "));
#endif
}

void
iflagg_print_laggport(struct lagg_reqport *port)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tlaggport: }{:name/%s} ", port->rp_portname);
#else
	printf("\tlaggport: %s ", port->rp_portname);
#endif
}

void
iflagg_print_peer(struct lacp_opreq *lp)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t\t}{:lacp-peer/%s}\n", lacp_format_peer(lp, "\n\t\t "));
#else
	printf("\t\t%s\n", lacp_format_peer(lp, "\n\t\t "));
#endif
}

void
ifmac_print_maclabel(const char *label_text)
{
#ifdef WITH_LIBXO
	xo_emit("{T:}maclabel {:maclabel/%s}\n", label_text);
#else
	printf("\tmaclabel %s\n", label_text);
#endif
}

void
ifmedia_print_media_header(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tmedia: }");
#else
	printf("\tmedia: ");
#endif
}

void
ifmedia_print_status(const char *status)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tstatus: }{:status/%s}", status);
#else
	printf("\tstatus: %s", status);
#endif
}

void
ifmedia_print_reason(struct ifdownreason *ifdr)
{
#ifdef WITH_LIBXO
	xo_emit("{P: (}{:reason-msg/%s}{P:)}", ifdr->ifdr_msg);
#else
	printf(" (%s)", ifdr->ifdr_msg);
#endif
}

void
ifmedia_print_reason_vendor(struct ifdownreason *ifdr)
{
#ifdef WITH_LIBXO
	xo_emit("{P: (vendor code }{:vendor-code/%d}{P:)}", ifdr->ifdr_vendor);
#else
	printf(" (vendor code %d)", ifdr->ifdr_vendor);
#endif
}

void
ifmedia_print_supmedia(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tsupported media:}{P:\n}");
#else
	printf("\tsupported media:\n");
#endif
}

void
ifpfsync_print_syncdev(const char *syncdev)
{
#ifdef WITH_LIBXO
	xo_emit("syncdev: {:syncdev/%s} ", syncdev);
#else
	printf("syncdev: %s ", syncdev);
#endif
}

void
ifpfsync_print_syncpeer(const char *syncpeer_str)
{
#ifdef WITH_LIBXO
	xo_emit("syncpeer: {:syncpeer-str/%s} ", syncpeer_str);
#else
	printf("syncpeer: %s ", syncpeer_str);
#endif
}

void
ifpfsync_print_maxupd(int maxupdates)
{
#ifdef WITH_LIBXO
	xo_emit("maxupd: {:maxupdates/%d} ", maxupdates);
#else
	printf("maxupd: %d ", maxupdates);
#endif
}

void
ifpfsync_print_defer(int flags)
{
#ifdef WITH_LIBXO
	xo_emit("defer: {:defer-status/%s} ",
	    (flags & PFSYNCF_DEFER) ? "on" : "off");
#else
	printf("defer: %s ", (flags & PFSYNCF_DEFER) ? "on" : "off");
#endif
}

void
ifpfsync_print_version(int version)
{
#ifdef WITH_LIBXO
	xo_emit("version: {:pfsync-version/%d}\n", version);
#else
	printf("version: %d\n", version);
#endif
}

void
ifpfsync_print_syncok(int flags)
{
#ifdef WITH_LIBXO
	xo_emit("\tsyncok: {:syncok/%d}\n", (flags & PFSYNCF_OK) ? 1 : 0);
#else
	printf("\tsyncok: %d\n", (flags & PFSYNCF_OK) ? 1 : 0);
#endif
}

void
ifstf_print_v4net(struct stfv4args *param)
{
#ifdef WITH_LIBXO
	xo_emit("\tv4net {:srcv4-addr/%s}/{:v4-prefixlen/%d} -> ",
	    inet_ntoa(param->srcv4_addr),
	    param->v4_prefixlen ? param->v4_prefixlen : 32);
#else
	printf("\tv4net %s/%d -> ", inet_ntoa(param->srcv4_addr),
	    param->v4_prefixlen ? param->v4_prefixlen : 32);
#endif
}

void
ifstf_print_tv4br(struct stfv4args *param)
{
#ifdef WITH_LIBXO
	xo_emit("tv4br {:braddr/%s}\n", inet_ntoa(param->braddr));
#else
	printf("tv4br %s\n", inet_ntoa(param->braddr));
#endif
}

void
ifvlan_print_vlan(struct vlanreq *vreq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tvlan: }{:vlantag/%d}", vreq->vlr_tag);
#else
	printf("\tvlan: %d", vreq->vlr_tag);
#endif
}

void
ifvlan_print_vlanproto(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: vlanproto: }");
#else
	printf(" vlanproto: ");
#endif
}

void
ifvlan_print_8021q(const char *proto_8021Q)
{
#ifdef WITH_LIBXO
	xo_emit("{:vlan-protocol/%s}", proto_8021Q);
#else
	printf("%s", proto_8021Q);
#endif
}

void
ifvlan_print_8021d(const char *proto_8021ad)
{
#ifdef WITH_LIBXO
	xo_emit("{:vlan-protocol/%s}", proto_8021ad);
#else
	printf("%s", proto_8021ad);
#endif
}

void
ifvlan_print_otherproto(struct vlanreq *vreq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:0x}{:vlan-protocol/%04x}", vreq->vlr_proto);
#else
	printf("0x%04x", vreq->vlr_proto);
#endif
}

void
ifvlan_print_vlanpcp(struct ifreq *ifr)
{
#ifdef WITH_LIBXO
	xo_emit("{P: vlanpcp: }{:vlanpcp/%u}", ifr->ifr_vlan_pcp);
#else
	printf(" vlanpcp: %u", ifr->ifr_vlan_pcp);
#endif
}

void
ifvlan_print_parent(struct vlanreq *vreq)
{
#ifdef WITH_LIBXO
	xo_emit("{P: parent interface: }{:parent-ifname/%s}",
	    vreq->vlr_parent[0] == '\0' ? "<none>" : vreq->vlr_parent);
#else
	printf(" parent interface: %s",
	    vreq->vlr_parent[0] == '\0' ? "<none>" : vreq->vlr_parent);
#endif
}

void
ifvxlan_print_vni(int vni)
{
#ifdef WITH_LIBXO
	xo_emit("\tvxlan vni {:vni/%d}", vni);
#else
	printf("\tvxlan vni %d", vni);
#endif
}

void
ifvxlan_print_src(int ipv6, const char *src, const char *srcport)
{
#ifdef WITH_LIBXO
	xo_emit(" local {P:/%s}{:local-src/%s}{P:/%s}:{:local-src-port/%s}",
	    ipv6 ? "[" : "", src, ipv6 ? "]" : "", srcport);
#else
	printf(" local %s%s%s:%s", ipv6 ? "[" : "", src, ipv6 ? "]" : "",
	    srcport);
#endif
}

void
ifvxlan_print_dst(int mc, int ipv6, const char *dst, const char *dstport)
{
#ifdef WITH_LIBXO
	xo_emit(
	    " {:peer-type/%s} {P:/%s}{:remote-dst/%s}{P:/%s}:{:remote-dst-port/%s}",
	    mc ? "group" : "remote", ipv6 ? "[" : "", dst, ipv6 ? "]" : "",
	    dstport);
#else
	printf(" %s %s%s%s:%s", mc ? "group" : "remote", ipv6 ? "[" : "", dst,
	    ipv6 ? "]" : "", dstport);
#endif
}

void
ifvxlan_print_config(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\n\t\tconfig: }");
#else
	printf("\n\t\tconfig: ");
#endif
}

void
ifvxlan_print_portrange(struct ifvxlancfg *cfg)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "{:learning-status/%s}learning portrange {:port-min/%d}-{:port-max/%d} ttl {:ttl/%d}",
	    cfg->vxlc_learn ? "" : "no", cfg->vxlc_port_min, cfg->vxlc_port_max,
	    cfg->vxlc_ttl);
#else
	printf("%slearning portrange %d-%d ttl %d", cfg->vxlc_learn ? "" : "no",
	    cfg->vxlc_port_min, cfg->vxlc_port_max, cfg->vxlc_ttl);
#endif
}

void
ifvxlan_print_ftable(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\n\t\tftable: }");
#else
	printf("\n\t\tftable: ");
#endif
}

void
ifvxlan_print_threshold(struct ifvxlancfg *cfg)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "cnt {:ftable-cnt/%d} max {:ftable-max/%d} timeout {:ftable-timeout/%d}",
	    cfg->vxlc_ftable_cnt, cfg->vxlc_ftable_max,
	    cfg->vxlc_ftable_timeout);
#else
	printf("cnt %d max %d timeout %d", cfg->vxlc_ftable_cnt,
	    cfg->vxlc_ftable_max, cfg->vxlc_ftable_timeout);
#endif
}

void
ifconfig_netlink_print_ifname(if_link_t *link)
{
#ifdef WITH_LIBXO
	xo_emit("{:ifname/%s}: ", link->ifla_ifname);
#else
	printf("%s: ", link->ifla_ifname);
#endif
}

void
ifconfig_netlink_print_flags(if_link_t *link)
{
#ifdef WITH_LIBXO
	xo_emit("flags={:flags-hex/%x}", link->ifi_flags);
#else
	printf("flags=%x", link->ifi_flags);
#endif
}

void
ifconfig_netlink_print_mtu(if_link_t *link)
{
#ifdef WITH_LIBXO
	xo_emit(" mtu {:mtu/%d}\n", link->ifla_mtu);
#else
	printf(" mtu %d\n", link->ifla_mtu);
#endif
}

void
ifconfig_netlink_print_ifalias(if_link_t *link)
{
#ifdef WITH_LIBXO
	xo_emit("\tdescription: {:descr/%s}\n", link->ifla_ifalias);
#else
	printf("\tdescription: %s\n", link->ifla_ifalias);
#endif
}

void
ifconfig_netlink_print_drivername(const char *drivername)
{
#ifdef WITH_LIBXO
	xo_emit("\tdrivername: {:drivername/%s}\n", drivername);
#else
	printf("\tdrivername: %s\n", drivername);
#endif
}

void
ifgeneve_print_mode(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tgeneve mode: }");
#else
	printf("\tgeneve mode: ");
#endif
}

void
ifgeneve_print_proto_l3(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:l3}");
#else
	printf("l3");
#endif
}

void
ifgeneve_print_proto_l2(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:l2}");
#else
	printf("l2");
#endif
}

void
ifgeneve_print_config(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\n\tgeneve config:\n}");
#else
	printf("\n\tgeneve config:\n");
#endif
}

void
ifgeneve_print_config_vni_notconfigured(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\t\tvirtual network identifier (vni): not configured\n}");
#else
	printf("\t\tvirtual network identifier (vni): not configured\n");
#endif
}

void
ifgeneve_print_vni(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/virtual network identifier (vni): %d}",
	    geneve_data->ifla_vni);
#else
	printf("\t\tvirtual network identifier (vni): %d",
	    geneve_data->ifla_vni);
#endif
}

void
ifgeneve_print_src(int ipv6, const char *src,
    struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/local: %s%s%s:%u}", ipv6 ? "[" : "", src, ipv6 ? "]" : "",
	    geneve_data->ifla_local_port);
#else
	printf("\n\t\tlocal: %s%s%s:%u", ipv6 ? "[" : "", src, ipv6 ? "]" : "",
	    geneve_data->ifla_local_port);
#endif
}

void
ifgeneve_print_dst(int mc, int ipv6, const char *dst,
    struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s: %s%s%s:%u}", mc ? "group" : "remote", ipv6 ? "[" : "",
	    dst, ipv6 ? "]" : "", geneve_data->ifla_local_port);
#else
	printf("\n\t\t%s: %s%s%s:%u", mc ? "group" : "remote", ipv6 ? "[" : "",
	    dst, ipv6 ? "]" : "", geneve_data->ifla_local_port);
#endif
}

void
ifgeneve_print_mcastdev(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ dev: %s}", geneve_data->ifla_mc_ifname);
#else
	printf(", dev: %s", geneve_data->ifla_mc_ifname);
#endif
}

void
ifgeneve_print_portrange(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/portrange: %u-%u}", geneve_data->ifla_port_range->low,
	    geneve_data->ifla_port_range->high);
#else
	printf("\n\t\tportrange: %u-%u", geneve_data->ifla_port_range->low,
	    geneve_data->ifla_port_range->high);
#endif
}

void
ifgeneve_print_ttl(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ ttl: %d}", geneve_data->ifla_ttl);
#else
	printf(", ttl: %d", geneve_data->ifla_ttl);
#endif
}

void
ifgeneve_print_ttl_inherit(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: ttl: inherit}");
#else
	printf(", ttl: inherit");
#endif
}

void
ifgeneve_print_dscp(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: dscp: inherit}");
#else
	printf(", dscp: inherit");
#endif
}

void
ifgeneve_print_df_inherit(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: df: inherit}");
#else
	printf(", df: inherit");
#endif
}

void
ifgeneve_print_df_set(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: df: set}");
#else
	printf(", df: set");
#endif
}

void
ifgeneve_print_df_unset(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: df: unset}");
#else
	printf(", df: unset");
#endif
}

void
ifgeneve_print_extctl(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: externally controlled}");
#else
	printf(", externally controlled");
#endif
}

void
ifgeneve_print_ftable_mode(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ftable mode: %slearning}",
	    geneve_data->ifla_ftable_learn ? "" : "no");
#else
	printf("\n\t\tftable mode: %slearning",
	    geneve_data->ifla_ftable_learn ? "" : "no");
#endif
}

void
ifgeneve_print_ftable(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ count: %d, max: %d, timeout: %d}",
	    geneve_data->ifla_ftable_count, geneve_data->ifla_ftable_max,
	    geneve_data->ifla_ftable_timeout);
#else
	printf(", count: %d, max: %d, timeout: %d",
	    geneve_data->ifla_ftable_count, geneve_data->ifla_ftable_max,
	    geneve_data->ifla_ftable_timeout);
#endif
}

void
ifgeneve_print_ftable_nospace(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ nospace: %u}", geneve_data->ifla_ftable_nospace);
#else
	printf(", nospace: %u", geneve_data->ifla_ftable_nospace);
#endif
}

void
ifgeneve_print_stats(struct nl_parsed_geneve *geneve_data)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/stats: tso %ju, txcsum %ju, rxcsum %ju}",
	    (uintmax_t)geneve_data->ifla_stats_tso,
	    (uintmax_t)geneve_data->ifla_stats_txcsum,
	    (uintmax_t)geneve_data->ifla_stats_rxcsum);
#else
	printf("\n\t\tstats: tso %ju, txcsum %ju, rxcsum %ju",
	    (uintmax_t)geneve_data->ifla_stats_tso,
	    (uintmax_t)geneve_data->ifla_stats_txcsum,
	    (uintmax_t)geneve_data->ifla_stats_rxcsum);
#endif
}

void
ifbridge_print_vlan(const char *prefix, struct ifbareq *ifba)
{
	struct ether_addr ea;

#ifdef WITH_LIBXO
	memcpy(ea.octet, ifba->ifba_dst, sizeof(ea.octet));
	xo_emit(
	    "{:prefix/%s}{:mac-addr/%s}{P: Vlan}{:vlan/%d}{P: }{:port-name/%s}{P: }{:expire/%lu}{P: }",
	    prefix, ether_ntoa(&ea), ifba->ifba_vlan, ifba->ifba_ifsname,
	    ifba->ifba_expire);
#else
	memcpy(ea.octet, ifba->ifba_dst, sizeof(ea.octet));
	printf("%s%s Vlan%d %s %lu ", prefix, ether_ntoa(&ea), ifba->ifba_vlan,
	    ifba->ifba_ifsname, ifba->ifba_expire);
#endif
}

void
ifbridge_print_summary(struct ifconfig_bridge_status *bridge)
{
	struct ifbropreq *params;
	uint8_t lladdr[ETHER_ADDR_LEN];
	uint16_t bprio;

#ifdef WITH_LIBXO
	params = bridge->params;

	PV2ID(params->ifbop_bridgeid, bprio, lladdr);
	xo_emit(
	    "{P:\tid }{:bridge-id/%s}{P: priority }{:bridge-priority/%u}{P: hellotime }{:hellotime/%u}{P: fwddelay }{:fwddelay/%u}\n",
	    ether_ntoa((struct ether_addr *)lladdr), params->ifbop_priority,
	    params->ifbop_hellotime, params->ifbop_fwddelay);
	xo_emit(
	    "{P:\tmaxage }{:maxage/%u}{P: holdcnt }{:holdcnt/%u}{P: proto }{:stp-proto/%s}{P: maxaddr }{:maxaddr/%u}{P: timeout }{:timeout/%u}\n",
	    params->ifbop_maxage, params->ifbop_holdcount,
	    stpproto[params->ifbop_protocol], bridge->cache_size,
	    bridge->cache_lifetime);
	PV2ID(params->ifbop_designated_root, bprio, lladdr);
	xo_emit(
	    "{P:\troot id }{:root-id/%s}{P: priority }{:root-priority/%d}{P: ifcost }{:ifcost/%u}{P: port }{:root-port/%u}\n",
	    ether_ntoa((struct ether_addr *)lladdr), bprio,
	    params->ifbop_root_path_cost, params->ifbop_root_port & 0xfff);
#else
	params = bridge->params;

	PV2ID(params->ifbop_bridgeid, bprio, lladdr);
	printf("\tid %s priority %u hellotime %u fwddelay %u\n",
	    ether_ntoa((struct ether_addr *)lladdr), params->ifbop_priority,
	    params->ifbop_hellotime, params->ifbop_fwddelay);
	printf("\tmaxage %u holdcnt %u proto %s maxaddr %u timeout %u\n",
	    params->ifbop_maxage, params->ifbop_holdcount,
	    stpproto[params->ifbop_protocol], bridge->cache_size,
	    bridge->cache_lifetime);
	PV2ID(params->ifbop_designated_root, bprio, lladdr);
	printf("\troot id %s priority %d ifcost %u port %u\n",
	    ether_ntoa((struct ether_addr *)lladdr), bprio,
	    params->ifbop_root_path_cost, params->ifbop_root_port & 0xfff);
#endif
}

void
ifbridge_print_defuntagged(struct ifconfig_bridge_status *bridge)
{
#ifdef WITH_LIBXO
	xo_emit("{P: defuntagged=}{:defuntagged/%u}",
	    (unsigned)bridge->defpvid);
#else
	printf(" defuntagged=%u", (unsigned)bridge->defpvid);
#endif
}

void
ifbridge_print_bridge_member(const char *prefix, struct ifbreq *member)
{
#ifdef WITH_LIBXO
	(void)prefix;
	xo_emit("{P:\tmember: }{:member-name/%s}{P: }", member->ifbr_ifsname);
#else
	printf("%s%s ", prefix, member->ifbr_ifsname);
#endif
}

void
ifbridge_print_bridge_member_pad(const char *pad)
{
#ifdef WITH_LIBXO
	(void)pad;
	xo_emit("\n{P:\t        }");
#else
	printf("\n%s", pad);
#endif
}

void
ifbridge_print_bridge_member_maxaddr(struct ifbreq *member)
{
#ifdef WITH_LIBXO
	xo_emit("{P:ifmaxaddr }{:ifmaxaddr/%u}{P: }", member->ifbr_addrmax);
#else
	printf("ifmaxaddr %u ", member->ifbr_addrmax);
#endif
}

void
ifbridge_print_bridge_member_stp(struct ifbreq *member)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "{P:port }{:port-number/%u}{P: priority }{:port-priority/%u}{P: path cost }{:path-cost/%u}",
	    member->ifbr_portno, member->ifbr_priority, member->ifbr_path_cost);
#else
	printf("port %u priority %u path cost %u", member->ifbr_portno,
	    member->ifbr_priority, member->ifbr_path_cost);
#endif
}

void
ifbridge_print_bridge_member_proto(int proto)
{
#ifdef WITH_LIBXO
	xo_emit("{P: proto }{:stp-proto/%s}", stpproto[proto]);
#else
	printf(" proto %s", stpproto[proto]);
#endif
}

void
ifbridge_print_bridge_member_proto_unknown(int proto)
{
#ifdef WITH_LIBXO
	xo_emit("{P: <unknown proto }{:unknown-proto/%d}{P:>}", proto);
#else
	printf(" <unknown proto %d>", proto);
#endif
}

void
ifbridge_print_bridge_member_role(int role)
{
#ifdef WITH_LIBXO
	xo_emit("{P:role }{:stp-role/%s}", stproles[role]);
#else
	printf("role %s", stproles[role]);
#endif
}

void
ifbridge_print_bridge_member_role_unknown(int role)
{
#ifdef WITH_LIBXO
	xo_emit("{P:<unknown role }{:unknown-role/%d}{P:>}", role);
#else
	printf("<unknown role %d>", role);
#endif
}

void
ifbridge_print_bridge_member_state(int state)
{
#ifdef WITH_LIBXO
	xo_emit("{P: state }{:stp-state/%s}", stpstates[state]);
#else
	printf(" state %s", stpstates[state]);
#endif
}

void
ifbridge_print_bridge_member_state_unknown(int state)
{
#ifdef WITH_LIBXO
	xo_emit("{P: <unknown state }{:unknown-state/%d}{P:>}", state);
#else
	printf(" <unknown state %d>", state);
#endif
}

void
ifbridge_print_bridge_member_vlan_proto(struct ifbreq *member)
{
#ifdef WITH_LIBXO
	xo_emit("{P: vlan protocol }{:vlan-proto/%s}",
	    vlan_proto_name(member->ifbr_vlanproto));
#else
	printf(" vlan protocol %s", vlan_proto_name(member->ifbr_vlanproto));
#endif
}

void
ifbridge_print_bridge_member_pvid(struct ifbreq *member)
{
#ifdef WITH_LIBXO
	xo_emit("{P: untagged }{:untagged/%u}", (unsigned)member->ifbr_pvid);
#else
	printf(" untagged %u", (unsigned)member->ifbr_pvid);
#endif
}

void
sfp_print_plugged(struct ifconfig_sfp_info *info,
    struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "{P:\tplugged: }{:sfp-id/%s}{P: }{:sfp-physical-spec/%s}{P: (}{:sfp-connector/%s}{P:)\n}",
	    ifconfig_sfp_id_display(info->sfp_id),
	    ifconfig_sfp_physical_spec(info, strings), strings->sfp_conn);
#else
	printf("\tplugged: %s %s (%s)\n", ifconfig_sfp_id_display(info->sfp_id),
	    ifconfig_sfp_physical_spec(info, strings), strings->sfp_conn);
#endif
}

void
sfp_print_vendor(struct ifconfig_sfp_vendor_info *vendor_info)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "{P:\tvendor: }{:sfp-vendor/%s}{P: PN: }{:sfp-part-number/%s}{P: SN: }{:sfp-serial/%s}{P: DATE: }{:sfp-date/%s}{P:\n}",
	    vendor_info->name, vendor_info->pn, vendor_info->sn,
	    vendor_info->date);
#else
	printf("\tvendor: %s PN: %s SN: %s DATE: %s\n", vendor_info->name,
	    vendor_info->pn, vendor_info->sn, vendor_info->date);
#endif
}

void
sfp_print_verbose_compliance(struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tcompliance level: }{:sfp-revision/%s}{P:\n}",
	    strings->sfp_rev);
#else
	printf("\tcompliance level: %s\n", strings->sfp_rev);
#endif
}

void
sfp_print_verbose_class(struct ifconfig_sfp_info *info,
    struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit("{P:Class: }{:sfp-class/%s}{P:\n}",
	    ifconfig_sfp_physical_spec(info, strings));
#else
	printf("Class: %s\n", ifconfig_sfp_physical_spec(info, strings));
#endif
}

void
sfp_print_verbose_length(struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit("{P:Length: }{:sfp-length/%s}{P:\n}", strings->sfp_fc_len);
#else
	printf("Length: %s\n", strings->sfp_fc_len);
#endif
}

void
sfp_print_verbose_tech(struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit("{P:Tech: }{:sfp-tech/%s}{P:\n}", strings->sfp_cab_tech);
#else
	printf("Tech: %s\n", strings->sfp_cab_tech);
#endif
}

void
sfp_print_verbose_media(struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit("{P:Media: }{:sfp-media/%s}{P:\n}", strings->sfp_fc_media);
#else
	printf("Media: %s\n", strings->sfp_fc_media);
#endif
}

void
sfp_print_verbose_speed(struct ifconfig_sfp_info_strings *strings)
{
#ifdef WITH_LIBXO
	xo_emit("{P:Speed: }{:sfp-speed/%s}{P:\n}", strings->sfp_fc_speed);
#else
	printf("Speed: %s\n", strings->sfp_fc_speed);
#endif
}

void
sfp_print_verbose_nombitrate(struct ifconfig_sfp_status *status)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\tnominal bitrate: }{:sfp-bitrate/%u}{P: Mbps\n}",
	    status->bitrate);
#else
	printf("\tnominal bitrate: %u Mbps\n", status->bitrate);
#endif
}

void
sfp_print_verbose_voltage(struct ifconfig_sfp_status *status)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "{P:\tmodule temperature: }{:sfp-temperature/%.2f}{P: C voltage: }{:sfp-voltage/%.2f}{P: Volts\n}",
	    status->temp, status->voltage);
#else
	printf("\tmodule temperature: %.2f C voltage: %.2f Volts\n",
	    status->temp, status->voltage);
#endif
}

void
sfp_print_verbose_rxpower(size_t chan, uint16_t rx, uint16_t tx)
{
#ifdef WITH_LIBXO
	xo_emit(
	    "{P:\tlane }{:sfp-lane/%zu}{P: RX power: }{:sfp-rx-power-mw/%.2f}{P: mW (}{:sfp-rx-power-dbm/%.2f}{P: dBm) TX bias: }{:sfp-tx-bias/%.2f}{P: mA\n}",
	    chan + 1, power_mW(rx), power_dBm(rx), bias_mA(tx));
#else
	printf("\tlane %zu: "
	       "RX power: %.2f mW (%.2f dBm) TX bias: %.2f mA\n",
	    chan + 1, power_mW(rx), power_dBm(rx), bias_mA(tx));
#endif
}

static void
sfp_print_hexdump(const uint8_t *bytes, int len, const char *tag __unused)
{
#ifdef WITH_LIBXO
	int i;

	if (xo_get_style(NULL) == XO_STYLE_TEXT) {
		for (i = 0; i < len; i++) {
			if (i % 16 == 0)
				xo_emit("{P:\t}");
			else
				xo_emit("{P: }");
			xo_emit("{P:/%02x}", bytes[i]);
			if (i % 16 == 15 || i == len - 1)
				xo_emit("{P:\n}");
		}
	} else {
		if (xo_get_style(NULL) == XO_STYLE_JSON) {
			ifconfig_open_list(tag);
			for (i = 0; i < len; i++)
				xo_emit("{l:byte/%d}", bytes[i]);
			ifconfig_close_list(tag);
		} else {
			ifconfig_open_container(tag);
			for (i = 0; i < len; i++)
				xo_emit("{:byte/%d}", bytes[i]);
			ifconfig_close_container(tag);
		}
	}
#else
	hexdump(bytes, len, "\t", HD_OMIT_COUNT | HD_OMIT_CHARS);
#endif
}

void
sfp_print_verbose_cmis_dump1(const void *buf, int len)
{
	const uint8_t *bytes = buf;

#ifdef WITH_LIBXO
	xo_emit("{P:\n\tCMIS DUMP (Lower Memory 0..127):\n}");
#else
	printf("\n\tCMIS DUMP (Lower Memory 0..127):\n");
#endif
	sfp_print_hexdump(bytes, len, "sfp-cmis-dump1");
}

void
sfp_print_verbose_cmis_dump2(const void *buf, int len)
{
	const uint8_t *bytes = buf;

#ifdef WITH_LIBXO
	xo_emit("{P:\n\tCMIS DUMP (Page 00h 128..255):\n}");
#else
	printf("\n\tCMIS DUMP (Page 00h 128..255):\n");
#endif
	sfp_print_hexdump(bytes, len, "sfp-cmis-dump2");
}

void
sfp_print_verbose_cmis_dump3(const void *buf, int len)
{
	const uint8_t *bytes = buf;

#ifdef WITH_LIBXO
	xo_emit("{P:\n\tCMIS DUMP (Page 11h 128..255):\n}");
#else
	printf("\n\tCMIS DUMP (Page 11h 128..255):\n");
#endif
	sfp_print_hexdump(bytes, len, "sfp-cmis-dump3");
}

void
sfp_print_verbose_sff8436_dump1(const void *buf, int len)
{
	const uint8_t *bytes = buf;

#ifdef WITH_LIBXO
	xo_emit("{P:\n\tSFF8436 DUMP (0xA0 128..255 range):\n}");
#else
	printf("\n\tSFF8436 DUMP (0xA0 128..255 range):\n");
#endif
	sfp_print_hexdump(bytes, len, "sfp-sff8436-dump1");
}

void
sfp_print_verbose_sff8436_dump2(const void *buf, int len)
{
	const uint8_t *bytes = buf;

#ifdef WITH_LIBXO
	xo_emit("{P:\n\tSFF8436 DUMP (0xA0 0..81 range):\n}");
#else
	printf("\n\tSFF8436 DUMP (0xA0 0..81 range):\n");
#endif
	sfp_print_hexdump(bytes, len, "sfp-sff8436-dump2");
}

void
sfp_print_verbose_sff8472_dump1(const void *buf)
{
	const uint8_t *bytes = (const uint8_t *)buf + SFP_DUMP_START;
	int len = SFP_DUMP_SIZE;

#ifdef WITH_LIBXO
	xo_emit("{P:\n\tSFF8472 DUMP (0xA0 0..127 range):\n}");
#else
	printf("\n\tSFF8472 DUMP (0xA0 0..127 range):\n");
#endif
	sfp_print_hexdump(bytes, len, "sfp-sff8472-dump1");
}

void
ifieee80211_print_drivercaps(struct ieee80211_devcaps_req *dc)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/drivercaps: 0x%x\n}", dc->dc_drivercaps);
#else
	printf("drivercaps: 0x%x\n", dc->dc_drivercaps);
#endif
}

void
ifieee80211_print_cryptocaps(struct ieee80211_devcaps_req *dc)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/cryptocaps: 0x%x\n}", dc->dc_cryptocaps);
#else
	printf("cryptocaps: 0x%x\n", dc->dc_cryptocaps);
#endif
}

void
ifieee80211_print_htcaps(struct ieee80211_devcaps_req *dc)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/htcaps    : 0x%x\n}", dc->dc_htcaps);
#else
	printf("htcaps    : 0x%x\n", dc->dc_htcaps);
#endif
}

void
ifieee80211_print_vhtcaps(struct ieee80211_devcaps_req *dc)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/vhtcaps   : 0x%x\n}", dc->dc_vhtcaps);
#else
	printf("vhtcaps   : 0x%x\n", dc->dc_vhtcaps);
#endif
}

void
ifieee80211_print_regdomain_addchans(const char *func)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s:}", func);
#else
	printf("%s:", func);
#endif
}

void
ifieee80211_print_verbose_vht20_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not a VHT20 channel\n}", freq);
#else
	printf("%u: skip, not a VHT20 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_vht40_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not a VHT40 channel\n}", freq);
#else
	printf("%u: skip, not a VHT40 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_vht80_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not a VHT80 channel\n}", freq);
#else
	printf("%u: skip, not a VHT80 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_vht160_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not a VHT160 channel\n}", freq);
#else
	printf("%u: skip, not a VHT160 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_vht80p80_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not a VHT80+80 channel\n}", freq);
#else
	printf("%u: skip, not a VHT80+80 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_ht20_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not an HT20 channel\n}", freq);
#else
	printf("%u: skip, not an HT20 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_ht40_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, not an HT40 channel\n}", freq);
#else
	printf("%u: skip, not an HT40 channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_freq_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, }", freq);
#else
	printf("%u: skip, ", freq);
#endif
}

void
ifieee80211_print_verbose_checkchan_notavail(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: not available\n}");
#else
	printf(" not available\n");
#endif
}

void
ifieee80211_print_verbose_ecm_chan(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, ECM channel\n}", freq);
#else
	printf("%u: skip, ECM channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_indoor_chan_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, indoor channel\n}", freq);
#else
	printf("%u: skip, indoor channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_outdoor_chan_skip(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, outdoor channel\n}", freq);
#else
	printf("%u: skip, outdoor channel\n", freq);
#endif
}

void
ifieee80211_print_verbose_chansep_need(int freq, int separation, int need)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, only %u channel separation, need %d\n}", freq,
	    separation, need);
#else
	printf("%u: skip, only %u channel separation, need %d\n", freq,
	    separation, need);
#endif
}

void
ifieee80211_print_verbose_chan_table_full(uint32_t freq)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%u: skip, channel table full\n}", freq);
#else
	printf("%u: skip, channel table full\n", freq);
#endif
}

void
ifieee80211_print_verbose_add_freq(struct ieee80211req_chaninfo *ci,
    const struct ieee80211_channel *c)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/[%3d] add freq %u }", ci->ic_nchans - 1, c->ic_freq);
#else
	printf("[%3d] add freq %u ", ci->ic_nchans - 1, c->ic_freq);
#endif
}

void
ifieee80211_print_verbose_maxregpower(const struct ieee80211_channel *c)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ power %u\n}", c->ic_maxregpower);
#else
	printf(" power %u\n", c->ic_maxregpower);
#endif
}

void
ifieee80211_print_country_codes(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\nCountry codes:\n}");
#else
	printf("\nCountry codes:\n");
#endif
}

void
ifieee80211_print_country_code(const struct country *cp, int i)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%2s %-15.15s%s}", cp->isoname, cp->name,
	    ((i + 1) % 4) == 0 ? "\n" : " ");
#else
	printf("%2s %-15.15s%s", cp->isoname, cp->name,
	    ((i + 1) % 4) == 0 ? "\n" : " ");
#endif
}

void
ifieee80211_print_reg_domains(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\nRegulatory domains:\n}");
#else
	printf("\nRegulatory domains:\n");
#endif
}

void
ifieee80211_print_reg_domain(const struct regdomain *dp, int i)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%-15.15s%s}", dp->name, ((i + 1) % 4) == 0 ? "\n" : " ");
#else
	printf("%-15.15s%s", dp->name, ((i + 1) % 4) == 0 ? "\n" : " ");
#endif
}

void
ifieee80211_print_list_scan_hdr(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%-*.*s  %-17.17s  %4s %4s   %-7s  %3s %4s\n}",
	    IEEE80211_NWID_LEN, IEEE80211_NWID_LEN, "SSID/MESH ID", "BSSID",
	    "CHAN", "RATE", " S:N", "INT", "CAPS");
#else
	printf("%-*.*s  %-17.17s  %4s %4s   %-7s  %3s %4s\n",
	    IEEE80211_NWID_LEN, IEEE80211_NWID_LEN, "SSID/MESH ID", "BSSID",
	    "CHAN", "RATE", " S:N", "INT", "CAPS");
#endif
}

void
ifieee80211_print_list_scan_row(const struct ieee80211req_scan_result *sr,
    const uint8_t *idp, int idlen)
{
	char ssid[IEEE80211_NWID_LEN + 1];

#ifdef WITH_LIBXO
	xo_emit("{P:/%-*.*s  %s  %3d  %3dM %4d:%-4d %4d %-4.4s}",
	    IEEE80211_NWID_LEN,
	    copy_essid(ssid, IEEE80211_NWID_LEN, idp, idlen), ssid,
	    ether_ntoa((const struct ether_addr *)sr->isr_bssid),
	    ieee80211_mhz2ieee(sr->isr_freq, sr->isr_flags),
	    getmaxrate(sr->isr_rates, sr->isr_nrates),
	    (sr->isr_rssi / 2) + sr->isr_noise, sr->isr_noise, sr->isr_intval,
	    getcaps(sr->isr_capinfo));
#else
	printf("%-*.*s  %s  %3d  %3dM %4d:%-4d %4d %-4.4s", IEEE80211_NWID_LEN,
	    copy_essid(ssid, IEEE80211_NWID_LEN, idp, idlen), ssid,
	    ether_ntoa((const struct ether_addr *)sr->isr_bssid),
	    ieee80211_mhz2ieee(sr->isr_freq, sr->isr_flags),
	    getmaxrate(sr->isr_rates, sr->isr_nrates),
	    (sr->isr_rssi / 2) + sr->isr_noise, sr->isr_noise, sr->isr_intval,
	    getcaps(sr->isr_capinfo));
#endif
}

void
ifieee80211_print_list_stations_hdr(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%-17.17s %4s %5s %5s %7s %4s %4s %4s %6s %6s\n}", "ADDR",
	    "CHAN", "LOCAL", "PEER", "STATE", "RATE", "RSSI", "IDLE", "TXSEQ",
	    "RXSEQ");
#else
	printf("%-17.17s %4s %5s %5s %7s %4s %4s %4s %6s %6s\n", "ADDR", "CHAN",
	    "LOCAL", "PEER", "STATE", "RATE", "RSSI", "IDLE", "TXSEQ", "RXSEQ");
#endif
}

void
ifieee80211_print_list_stations_hdr2(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%-17.17s %4s %4s %4s %4s %4s %6s %6s %4s %-12s\n}", "ADDR",
	    "AID", "CHAN", "RATE", "RSSI", "IDLE", "TXSEQ", "RXSEQ", "CAPS",
	    "FLAG");
#else
	printf("%-17.17s %4s %4s %4s %4s %4s %6s %6s %4s %-12s\n", "ADDR",
	    "AID", "CHAN", "RATE", "RSSI", "IDLE", "TXSEQ", "RXSEQ", "CAPS",
	    "FLAG");
#endif
}

void
ifieee80211_print_list_stations_row(const struct ieee80211req_sta_info *si)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s %4d %5x %5x %7.7s %3dM %4.1f %4d %6d %6d}",
	    ether_ntoa((const struct ether_addr *)si->isi_macaddr),
	    ieee80211_mhz2ieee(si->isi_freq, si->isi_flags), si->isi_localid,
	    si->isi_peerid, mesh_linkstate_string(si->isi_peerstate),
	    si->isi_txmbps / 2, si->isi_rssi / 2., si->isi_inact, gettxseq(si),
	    getrxseq(si));
#else
	printf("%s %4d %5x %5x %7.7s %3dM %4.1f %4d %6d %6d",
	    ether_ntoa((const struct ether_addr *)si->isi_macaddr),
	    ieee80211_mhz2ieee(si->isi_freq, si->isi_flags), si->isi_localid,
	    si->isi_peerid, mesh_linkstate_string(si->isi_peerstate),
	    si->isi_txmbps / 2, si->isi_rssi / 2., si->isi_inact, gettxseq(si),
	    getrxseq(si));
#endif
}

void
ifieee80211_print_list_stations_row2(const struct ieee80211req_sta_info *si)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s %4u %4d %3dM %4.1f %4d %6d %6d %-4.4s %-12.12s}",
	    ether_ntoa((const struct ether_addr *)si->isi_macaddr),
	    IEEE80211_AID(si->isi_associd),
	    ieee80211_mhz2ieee(si->isi_freq, si->isi_flags), si->isi_txmbps / 2,
	    si->isi_rssi / 2., si->isi_inact, gettxseq(si), getrxseq(si),
	    getcaps(si->isi_capinfo), getflags(si->isi_state));
#else
	printf("%s %4u %4d %3dM %4.1f %4d %6d %6d %-4.4s %-12.12s",
	    ether_ntoa((const struct ether_addr *)si->isi_macaddr),
	    IEEE80211_AID(si->isi_associd),
	    ieee80211_mhz2ieee(si->isi_freq, si->isi_flags), si->isi_txmbps / 2,
	    si->isi_rssi / 2., si->isi_inact, gettxseq(si), getrxseq(si),
	    getcaps(si->isi_capinfo), getflags(si->isi_state));
#endif
}

void
ifieee80211_print_list_wme_aci_tag(const char *tag)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s}", tag);
#else
	printf("\t%s", tag);
#endif
}

void
ifieee80211_print_list_wme_aci_cwmin(int val)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ cwmin %2u}", val);
#else
	printf(" cwmin %2u", val);
#endif
}

void
ifieee80211_print_list_wme_aci_cwmax(int val)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ cwmax %2u}", val);
#else
	printf(" cwmax %2u", val);
#endif
}

void
ifieee80211_print_list_wme_aci_aifs(int val)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ aifs %2u}", val);
#else
	printf(" aifs %2u", val);
#endif
}

void
ifieee80211_print_list_wme_aci_txoplimit(int val)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ txopLimit %3u}", val);
#else
	printf(" txopLimit %3u", val);
#endif
}

void
ifieee80211_print_list_wme_aci_acm_enabled(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: acm}");
#else
	printf(" acm");
#endif
}

void
ifieee80211_print_list_wme_aci_acm_disabled(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: -acm}");
#else
	printf(" -acm");
#endif
}

void
ifieee80211_print_list_wme_aci_ack_enabled(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: ack}");
#else
	printf(" ack");
#endif
}

void
ifieee80211_print_list_wme_aci_ack_disabled(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: -ack}");
#else
	printf(" -ack");
#endif
}

void
ifieee80211_print_list_mac_acl_loaded(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:No acl policy loaded\n}");
#else
	printf("No acl policy loaded\n");
#endif
}

void
ifieee80211_print_list_mac_unknown_policy(unsigned int policy)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/policy: unknown (%u)\n}", policy);
#else
	printf("policy: unknown (%u)\n", policy);
#endif
}

void
ifieee80211_print_list_mac_nacl(char c, struct ieee80211req_maclist *acl)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%c%s\n}", c,
	    ether_ntoa((const struct ether_addr *)acl->ml_macaddr));
#else
	printf("%c%s\n", c,
	    ether_ntoa((const struct ether_addr *)acl->ml_macaddr));
#endif
}

void
ifieee80211_print_list_mesh_hdr(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%-17.17s %-17.17s %4s %4s %4s %6s %s\n}", "DEST",
	    "NEXT HOP", "HOPS", "METRIC", "LIFETIME", "MSEQ", "FLAGS");
#else
	printf("%-17.17s %-17.17s %4s %4s %4s %6s %s\n", "DEST", "NEXT HOP",
	    "HOPS", "METRIC", "LIFETIME", "MSEQ", "FLAGS");
#endif
}

void
ifieee80211_print_list_mesh_row(const struct ieee80211req_mesh_route *rt)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s }",
	    ether_ntoa((const struct ether_addr *)rt->imr_dest));
#else
	printf("%s ", ether_ntoa((const struct ether_addr *)rt->imr_dest));
#endif
}

void
ifieee80211_print_list_mesh_row2(const struct ieee80211req_mesh_route *rt)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/%s %4u   %4u   %6u %6u    %c%c\n}",
	    ether_ntoa((const struct ether_addr *)rt->imr_nexthop),
	    rt->imr_nhops, rt->imr_metric, rt->imr_lifetime, rt->imr_lastmseq,
	    (rt->imr_flags & IEEE80211_MESHRT_FLAGS_DISCOVER)  ? 'D' :
		(rt->imr_flags & IEEE80211_MESHRT_FLAGS_VALID) ? 'V' :
								 '!',
	    (rt->imr_flags & IEEE80211_MESHRT_FLAGS_PROXY)    ? 'P' :
		(rt->imr_flags & IEEE80211_MESHRT_FLAGS_GATE) ? 'G' :
								' ');
#else
	printf("%s %4u   %4u   %6u %6u    %c%c\n",
	    ether_ntoa((const struct ether_addr *)rt->imr_nexthop),
	    rt->imr_nhops, rt->imr_metric, rt->imr_lifetime, rt->imr_lastmseq,
	    (rt->imr_flags & IEEE80211_MESHRT_FLAGS_DISCOVER)  ? 'D' :
		(rt->imr_flags & IEEE80211_MESHRT_FLAGS_VALID) ? 'V' :
								 '!',
	    (rt->imr_flags & IEEE80211_MESHRT_FLAGS_PROXY)    ? 'P' :
		(rt->imr_flags & IEEE80211_MESHRT_FLAGS_GATE) ? 'G' :
								' ');
#endif
}

void
ifieee80211_print_ieee80211_status_meshid(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:meshid }");
#else
	printf("meshid ");
#endif
}

void
ifieee80211_print_ieee80211_status_ssid(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:ssid }");
#else
	printf("ssid ");
#endif
}

void
ifieee80211_print_ieee80211_status_ssid_idx(int i)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ %d:}", i + 1);
#else
	printf(" %d:", i + 1);
#endif
}

void
ifieee80211_print_ieee80211_status_channel(const struct ieee80211_channel *c)
{
	char buf[14];

#ifdef WITH_LIBXO
	xo_emit("{P:/ channel %d (%u MHz%s)}", c->ic_ieee, c->ic_freq,
	    get_chaninfo(c, 1, buf, sizeof(buf)));
#else
	printf(" channel %d (%u MHz%s)", c->ic_ieee, c->ic_freq,
	    get_chaninfo(c, 1, buf, sizeof(buf)));
#endif
}

void
ifieee80211_print_ieee80211_status_channel_undef(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P: channel UNDEF}");
#else
	printf(" channel UNDEF");
#endif
}

void
ifieee80211_print_ieee80211_status_bssid(const uint8_t *bssid)
{
#ifdef WITH_LIBXO
	xo_emit("{P:/ bssid %s}", ether_ntoa((const struct ether_addr *)bssid));
#else
	printf(" bssid %s", ether_ntoa((const struct ether_addr *)bssid));
#endif
}

void
ifieee80211_print_ieee80211_status_stationname(void)
{
#ifdef WITH_LIBXO
	xo_emit("{P:\n\tstationname }");
#else
	printf("\n\tstationname ");
#endif
}
