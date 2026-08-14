#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>

#include <bsdxml.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ifconfig.h"
#include "ifconfig_xml.h"

#define MAX_ARGS	   256
#define MAX_STACK	   64
#define MAX_BRIDGE_MEMBERS 256
#define MAX_LAGG_PORTS	   64
#define EMPTY		   '\0'

int ifconfig_xml_dry_run;

enum {
	ELEM_NONE,
	ELEM_INTERFACE,
	ELEM_IFNAME,
	ELEM_IF_FLAGS,
	ELEM_METRIC,
	ELEM_MTU,
	ELEM_DESCRIPTION,
	ELEM_DESCR,
	ELEM_LINK_ADDR_PARENT,
	ELEM_ADDR_TYPE,
	ELEM_LINK_ADDR_VAL,
	ELEM_GROUP,
	ELEM_GROUP_NAME,
	ELEM_MEDIA,
	ELEM_MEDIA_TYPE,
	ELEM_MEDIA_SUBTYPE,
	ELEM_MEDIA_MODE,
	ELEM_MEDIA_OPTION,
	ELEM_IF_CAP,
	ELEM_OPTIONS,
	ELEM_FIB,
	ELEM_TUNNEL_FIB,
	ELEM_ADDRESS_CONTAINER,
	ELEM_INET_ADDR,
	ELEM_INET6_ADDR,
	ELEM_DST_ADDR,
	ELEM_NETMASK,
	ELEM_BROADCAST,
	ELEM_PREFIXLEN,
	ELEM_BRIDGE,
	ELEM_BRIDGE_PRIORITY,
	ELEM_HELLOTIME,
	ELEM_FWDDELAY,
	ELEM_MAXAGE,
	ELEM_HOLDCNT,
	ELEM_STP_PROTO,
	ELEM_MAXADDR,
	ELEM_TIMEOUT,
	ELEM_MEMBER,
	ELEM_MEMBER_NAME,
	ELEM_PORT_PRIORITY,
	ELEM_PATH_COST,
	ELEM_VLAN_PROTO,
	ELEM_LAGG_PROTO,
	ELEM_LAGG_HASH_L2,
	ELEM_LAGG_HASH_L3,
	ELEM_LAGG_HASH_L4,
	ELEM_LAGG_PORT,
	ELEM_TUNNEL_SRC,
	ELEM_TUNNEL_DST,
	ELEM_VLAN_TAG,
	ELEM_VLAN_PROTOCOL,
	ELEM_VLAN_PCP,
	ELEM_PARENT_IFNAME,
	ELEM_PARENT,
	ELEM_VXLAN_VNI,
	ELEM_VXLAN_LOCAL,
	ELEM_VXLAN_LOCAL_PORT,
	ELEM_VXLAN_PEER_TYPE,
	ELEM_VXLAN_REMOTE,
	ELEM_VXLAN_REMOTE_PORT,
	ELEM_VXLAN_LEARNING,
	ELEM_VXLAN_PORT_MIN,
	ELEM_VXLAN_PORT_MAX,
	ELEM_VXLAN_TTL,
	ELEM_VXLAN_FTABLE_MAX,
	ELEM_VXLAN_FTABLE_TIMEOUT,
	ELEM_PFSYNC_SYNCDEV,
	ELEM_PFSYNC_SYNCPEER,
	ELEM_PFSYNC_MAXUPDATES,
	ELEM_PFSYNC_DEFER,
	ELEM_PFSYNC_VERSION,
	ELEM_CARP_STATE,
	ELEM_CARP_VHID,
	ELEM_CARP_ADVBASE,
	ELEM_CARP_ADVSKEW,
	ELEM_CARP_KEY,
	ELEM_CARP_PEER,
	ELEM_CARP_PEER6,
	ELEM_VRRP_STATE,
	ELEM_VRRP_VRID,
	ELEM_VRRP_PRIO,
	ELEM_VRRP_INTERVAL,
	ELEM_GRE_KEY,
	ELEM_GRE_UDPPORT,
	ELEM_FLAGS6,
	ELEM_BRIDGE_DEFUNTAGGED,
	ELEM_MEMBER_IFMAXADDR,
	ELEM_MEMBER_UNTAGGED,
	ELEM_MEMBER_VLAN,
	ELEM_MEMBER_VLAN_ID,
	ELEM_MEMBER_VLAN_END,
	ELEM_LAGG_FLOWID_SHIFT,
	ELEM_LAGG_RR_LIMIT,
	ELEM_LINK_PCP,
	ELEM_WLAN,
	ELEM_WME_ACI,
	ELEM_WME_CWMIN,
	ELEM_WME_CWMAX,
	ELEM_WME_AIFS,
	ELEM_WME_TXOPLIMIT,
};

struct ifxml_cfg {
	char ifname[IFNAMSIZ];
	bool ifname_set;
	char *argv[MAX_ARGS];
	int argc;
	char inet_addr[128];
	char inet_dst[128];
	char inet_netmask[128];
	char inet_broadcast[128];
	char inet6_addr[128];
	char inet6_dst[128];
	char inet6_prefixlen[16];
};

struct ifxml_addr {
	char inet_addr[128];
	char inet6_addr[128];
	char dst_addr[128];
	char netmask[128];
	char broadcast[128];
	char prefixlen[16];
};

struct ifxml_bridge_member {
	char name[IFNAMSIZ];
	char priority[16];
	char path_cost[32];
	char vlan_proto[16];
	char *ifmaxaddr;
	char *untagged;
	char *vlan_tagged;
};

struct ifxml_bridge {
	struct ifxml_bridge_member members[MAX_BRIDGE_MEMBERS];
	int num_members;
	char priority[16];
	char hellotime[16];
	char fwddelay[16];
	char maxage[16];
	char holdcnt[16];
	char stp_proto[16];
	char maxaddr[16];
	char timeout[16];
	char *defuntagged;
};

struct ifxml_lagg_port {
	char name[IFNAMSIZ];
};

struct ifxml_lagg {
	struct ifxml_lagg_port ports[MAX_LAGG_PORTS];
	int num_ports;
	char proto[32];
	char hash[64];
};

struct ifxml_state {
	XML_Parser parser;
	if_ctx *ctx;
	struct ifconfig_args *args;

	int stack[MAX_STACK];
	int sp;

	char text[4096];
	int text_len;

	struct ifxml_cfg cfg;
	int cfg_count;
	struct ifxml_cfg *all_cfgs;

	char addr_type[64];
	char link_addr[256];
	char media_type[64];
	char media_subtype[64];
	char *media_mode;
	char *media_option;
	int media_capture;

	int in_addr_block;
	struct ifxml_addr addr;

	int in_bridge;
	struct ifxml_bridge bridge;
	int in_member;
	struct ifxml_bridge_member cur_member;

	char tunnel_src[128];
	char tunnel_dst[128];

	struct ifxml_lagg lagg;
	int in_lagg_port;
	int lagg_hash_l2;
	int lagg_hash_l3;
	int lagg_hash_l4;

	struct {
		char *vni;
		char *local;
		char *local_port;
		char *peer_type;
		char *remote;
		char *remote_port;
		char *port_min;
		char *port_max;
		char *ttl;
		char *ftable_max;
		char *ftable_timeout;
		int learning_seen;
		int learning_on;
	} vxlan;

	char *wlan_chan_num;
	int wlan_bssid_seen;
	char *carp_state;
	int in_wme_aci;
	char *wme_aci_name;

	struct {
		char *syncdev;
		char *syncpeer;
		char *maxupdates;
		char *version;
		int defer_seen;
		int defer_on;
	} pfsync;
};

static void buf_append(char **buf, const char *sep, const char *val);
static void set_str(char **dst, const char *src);
static void ifxml_free_iface(struct ifxml_state *st);
static void cfg_add_arg(struct ifxml_cfg *cfg, const char *arg);
static void cfg_add_arg_kv(struct ifxml_cfg *cfg, const char *key,
    const char *val);

static int
null_or_empty(const char *s)
{
	return s == NULL || s[0] == EMPTY;
}

static int
str_eq(const char *a, const char *b)
{
	size_t la, lb;

	la = strlen(a);
	lb = strlen(b);

	if (la != lb)
		return 0;

	return strncmp(a, b, la) == 0;
}

static void
concat(char *buf, size_t bufsz, const char *s)
{
	strlcat(buf, s, bufsz);
}

static int
wlan_lookup(const char *name, const char **cmd, int *is_flag)
{
	if (str_eq(name, "privacy")) {
		*cmd = "wepmode";
		*is_flag = 0;
		return 1;
	}
	if (str_eq(name, "roam_rssi")) {
		*cmd = "roam:rssi";
		*is_flag = 0;
		return 1;
	}
	if (str_eq(name, "roam_rate")) {
		*cmd = "roam:rate";
		*is_flag = 0;
		return 1;
	}
	if (str_eq(name, "inactivity")) {
		*cmd = "inact";
		*is_flag = 1;
		return 1;
	}
	if (str_eq(name, "turbo")) {
		*cmd = "dturbo";
		*is_flag = 1;
		return 1;
	}
	if (str_eq(name, "ssid") || str_eq(name, "bssid") ||
	    str_eq(name, "authmode") || str_eq(name, "powersavemode") ||
	    str_eq(name, "powersavesleep") || str_eq(name, "deftxkey") ||
	    str_eq(name, "rtsthreshold") || str_eq(name, "protmode") ||
	    str_eq(name, "txpower") || str_eq(name, "roaming") ||
	    str_eq(name, "dtimperiod") || str_eq(name, "bintval") ||
	    str_eq(name, "fragthreshold") || str_eq(name, "bmiss") ||
	    str_eq(name, "scanvalid") || str_eq(name, "bgscanintvl") ||
	    str_eq(name, "bgscanidle") || str_eq(name, "maxretry") ||
	    str_eq(name, "ampdulimit") || str_eq(name, "ampdudensity") ||
	    str_eq(name, "htprotmode") || str_eq(name, "tdmaslot") ||
	    str_eq(name, "tdmaslotcnt") || str_eq(name, "tdmaslotlen") ||
	    str_eq(name, "tdmabintval") || str_eq(name, "meshttl") ||
	    str_eq(name, "meshmetric") || str_eq(name, "meshpath") ||
	    str_eq(name, "hwmprootmode") || str_eq(name, "hwmpmaxhops") ||
	    str_eq(name, "regdomain") || str_eq(name, "country") ||
	    str_eq(name, "ucastrate") || str_eq(name, "mcastrate") ||
	    str_eq(name, "mgmtrate")) {
		*cmd = name;
		*is_flag = 0;
		return 1;
	}
	if (str_eq(name, "tsn") || str_eq(name, "ecm") ||
	    str_eq(name, "pureg") || str_eq(name, "htcompat") ||
	    str_eq(name, "dotd") || str_eq(name, "dfs") ||
	    str_eq(name, "rifs") || str_eq(name, "uapsd") ||
	    str_eq(name, "shortgi") || str_eq(name, "puren") ||
	    str_eq(name, "bgscan") || str_eq(name, "wme") ||
	    str_eq(name, "burst") || str_eq(name, "ff") ||
	    str_eq(name, "dwds") || str_eq(name, "hidessid") ||
	    str_eq(name, "apbridge") || str_eq(name, "doth") ||
	    str_eq(name, "ht") || str_eq(name, "ht20") ||
	    str_eq(name, "ht40") || str_eq(name, "vht") ||
	    str_eq(name, "vht40") || str_eq(name, "vht80") ||
	    str_eq(name, "vht160") || str_eq(name, "vht80p80") ||
	    str_eq(name, "ampdu") || str_eq(name, "ampdutx") ||
	    str_eq(name, "ampdurx") || str_eq(name, "amsdu") ||
	    str_eq(name, "amsdutx") || str_eq(name, "amsdurx") ||
	    str_eq(name, "stbc") || str_eq(name, "stbctx") ||
	    str_eq(name, "stbcrx") || str_eq(name, "ldpc") ||
	    str_eq(name, "ldpctx") || str_eq(name, "ldpcrx") ||
	    str_eq(name, "meshpeering") || str_eq(name, "meshforward") ||
	    str_eq(name, "meshgate")) {
		*cmd = name;
		*is_flag = 1;
		return 1;
	}
	return 0;
}

static int
wlan_elem_id(const char *name)
{
	const char *cmd;
	int is_flag;

	if (str_eq(name, "chan-num") || str_eq(name, "chan-mode") ||
	    str_eq(name, "smps") || str_eq(name, "location"))
		return ELEM_WLAN;
	if (wlan_lookup(name, &cmd, &is_flag))
		return ELEM_WLAN;
	return ELEM_NONE;
}

static void
wlan_emit_cmd(struct ifxml_state *st, const char *name, const char *txt)
{
	const char *cmd;
	int is_flag;

	if (str_eq(name, "smps")) {
		if (str_eq(txt, "dynamic"))
			cfg_add_arg(&st->cfg, "smpsdyn");
		else if (str_eq(txt, "static"))
			cfg_add_arg(&st->cfg, "smps");
		return;
	}
	if (str_eq(name, "location")) {
		if (str_eq(txt, "indoor") || str_eq(txt, "outdoor") ||
		    str_eq(txt, "anywhere"))
			cfg_add_arg(&st->cfg, strdup(txt));
		return;
	}
	if (str_eq(name, "chan-num")) {
		set_str(&st->wlan_chan_num, txt);
		return;
	}
	if (str_eq(name, "ssid")) {
		cfg_add_arg_kv(&st->cfg,
		    st->wlan_bssid_seen ? "stationname" : "ssid", txt);
		return;
	}
	if (str_eq(name, "authmode")) {
		if (str_eq(txt, "802.1x"))
			cfg_add_arg_kv(&st->cfg, "authmode", "8021x");
		else if (str_eq(txt, "WPA2/802.11i") ||
		    str_eq(txt, "WPA1+WPA2/802.11i") || str_eq(txt, "AUTO"))
			; /* not settable via authmode */
		else
			cfg_add_arg_kv(&st->cfg, "authmode", txt);
		return;
	}
	if (wlan_lookup(name, &cmd, &is_flag)) {
		if (is_flag) {
			if (str_eq(txt, "0")) {
				char *neg;

				if (asprintf(&neg, "-%s", cmd) == -1)
					err(1, "asprintf");
				cfg_add_arg(&st->cfg, neg);
			} else {
				cfg_add_arg(&st->cfg, cmd);
			}
		} else {
			cfg_add_arg_kv(&st->cfg, cmd, txt);
		}
	}
}

static void
wme_emit_cmd(struct ifxml_state *st, const char *cmd, const char *val)
{
	static const char *acnames[] = { "AC_BE", "AC_BK", "AC_VI", "AC_VO" };
	size_t i;

	if (!st->in_wme_aci || null_or_empty(val) ||
	    null_or_empty(st->wme_aci_name))
		return;
	for (i = 0; i < nitems(acnames); i++)
		if (strcasecmp(st->wme_aci_name, acnames[i]) == 0)
			break;
	if (i == nitems(acnames))
		return;
	cfg_add_arg(&st->cfg, cmd);
	cfg_add_arg(&st->cfg, strdup(st->wme_aci_name));
	cfg_add_arg(&st->cfg, strdup(val));
}

static int
elem_id(const char *name)
{
	if (str_eq(name, "interface"))
		return ELEM_INTERFACE;
	if (str_eq(name, "ifname"))
		return ELEM_IFNAME;
	if (str_eq(name, "interface-flags"))
		return ELEM_IF_FLAGS;
	if (str_eq(name, "metric"))
		return ELEM_METRIC;
	if (str_eq(name, "mtu"))
		return ELEM_MTU;
	if (str_eq(name, "description"))
		return ELEM_DESCRIPTION;
	if (str_eq(name, "descr"))
		return ELEM_DESCR;
	if (str_eq(name, "link-address"))
		return ELEM_LINK_ADDR_PARENT;
	if (str_eq(name, "addr-type"))
		return ELEM_ADDR_TYPE;
	if (str_eq(name, "link-addr"))
		return ELEM_LINK_ADDR_VAL;
	if (str_eq(name, "group"))
		return ELEM_GROUP;
	if (str_eq(name, "name"))
		return ELEM_GROUP_NAME;
	if (str_eq(name, "media"))
		return ELEM_MEDIA;
	if (str_eq(name, "type"))
		return ELEM_MEDIA_TYPE;
	if (str_eq(name, "subtype"))
		return ELEM_MEDIA_SUBTYPE;
	if (str_eq(name, "mode"))
		return ELEM_MEDIA_MODE;
	if (str_eq(name, "option"))
		return ELEM_MEDIA_OPTION;
	if (str_eq(name, "interface-capabilities"))
		return ELEM_IF_CAP;
	if (str_eq(name, "options"))
		return ELEM_OPTIONS;
	if (str_eq(name, "fib-id"))
		return ELEM_FIB;
	if (str_eq(name, "tunnelfib-id"))
		return ELEM_TUNNEL_FIB;
	if (str_eq(name, "address"))
		return ELEM_ADDRESS_CONTAINER;
	if (str_eq(name, "inet-addr"))
		return ELEM_INET_ADDR;
	if (str_eq(name, "inet6-addr"))
		return ELEM_INET6_ADDR;
	if (str_eq(name, "dst-addr"))
		return ELEM_DST_ADDR;
	if (str_eq(name, "netmask"))
		return ELEM_NETMASK;
	if (str_eq(name, "broadcast"))
		return ELEM_BROADCAST;
	if (str_eq(name, "prefixlen"))
		return ELEM_PREFIXLEN;
	if (str_eq(name, "bridge"))
		return ELEM_BRIDGE;
	if (str_eq(name, "bridge-priority"))
		return ELEM_BRIDGE_PRIORITY;
	if (str_eq(name, "hellotime"))
		return ELEM_HELLOTIME;
	if (str_eq(name, "fwddelay"))
		return ELEM_FWDDELAY;
	if (str_eq(name, "maxage"))
		return ELEM_MAXAGE;
	if (str_eq(name, "holdcnt"))
		return ELEM_HOLDCNT;
	if (str_eq(name, "stp-proto"))
		return ELEM_STP_PROTO;
	if (str_eq(name, "maxaddr"))
		return ELEM_MAXADDR;
	if (str_eq(name, "timeout"))
		return ELEM_TIMEOUT;
	if (str_eq(name, "member"))
		return ELEM_MEMBER;
	if (str_eq(name, "member-name"))
		return ELEM_MEMBER_NAME;
	if (str_eq(name, "port-priority"))
		return ELEM_PORT_PRIORITY;
	if (str_eq(name, "path-cost"))
		return ELEM_PATH_COST;
	if (str_eq(name, "vlan-proto"))
		return ELEM_VLAN_PROTO;
	if (str_eq(name, "lagg-proto"))
		return ELEM_LAGG_PROTO;
	if (str_eq(name, "lagg-hash-l2"))
		return ELEM_LAGG_HASH_L2;
	if (str_eq(name, "lagg-hash-l3"))
		return ELEM_LAGG_HASH_L3;
	if (str_eq(name, "lagg-hash-l4"))
		return ELEM_LAGG_HASH_L4;
	if (str_eq(name, "laggport"))
		return ELEM_LAGG_PORT;
	if (str_eq(name, "tunnel-src"))
		return ELEM_TUNNEL_SRC;
	if (str_eq(name, "tunnel-dst"))
		return ELEM_TUNNEL_DST;
	if (str_eq(name, "vlantag"))
		return ELEM_VLAN_TAG;
	if (str_eq(name, "vlan-protocol"))
		return ELEM_VLAN_PROTOCOL;
	if (str_eq(name, "vlanpcp"))
		return ELEM_VLAN_PCP;
	if (str_eq(name, "parent-ifname"))
		return ELEM_PARENT_IFNAME;
	if (str_eq(name, "parent"))
		return ELEM_PARENT;
	if (str_eq(name, "vni"))
		return ELEM_VXLAN_VNI;
	if (str_eq(name, "local-src"))
		return ELEM_VXLAN_LOCAL;
	if (str_eq(name, "local-src-port"))
		return ELEM_VXLAN_LOCAL_PORT;
	if (str_eq(name, "peer-type"))
		return ELEM_VXLAN_PEER_TYPE;
	if (str_eq(name, "remote-dst"))
		return ELEM_VXLAN_REMOTE;
	if (str_eq(name, "remote-dst-port"))
		return ELEM_VXLAN_REMOTE_PORT;
	if (str_eq(name, "learning-status"))
		return ELEM_VXLAN_LEARNING;
	if (str_eq(name, "port-min"))
		return ELEM_VXLAN_PORT_MIN;
	if (str_eq(name, "port-max"))
		return ELEM_VXLAN_PORT_MAX;
	if (str_eq(name, "ttl"))
		return ELEM_VXLAN_TTL;
	if (str_eq(name, "ftable-max"))
		return ELEM_VXLAN_FTABLE_MAX;
	if (str_eq(name, "ftable-timeout"))
		return ELEM_VXLAN_FTABLE_TIMEOUT;
	if (str_eq(name, "syncdev"))
		return ELEM_PFSYNC_SYNCDEV;
	if (str_eq(name, "syncpeer-str"))
		return ELEM_PFSYNC_SYNCPEER;
	if (str_eq(name, "maxupdates"))
		return ELEM_PFSYNC_MAXUPDATES;
	if (str_eq(name, "defer-status"))
		return ELEM_PFSYNC_DEFER;
	if (str_eq(name, "pfsync-version"))
		return ELEM_PFSYNC_VERSION;
	if (str_eq(name, "carp-state"))
		return ELEM_CARP_STATE;
	if (str_eq(name, "vhid"))
		return ELEM_CARP_VHID;
	if (str_eq(name, "advbase"))
		return ELEM_CARP_ADVBASE;
	if (str_eq(name, "advskew"))
		return ELEM_CARP_ADVSKEW;
	if (str_eq(name, "carp_key"))
		return ELEM_CARP_KEY;
	if (str_eq(name, "carp_peer"))
		return ELEM_CARP_PEER;
	if (str_eq(name, "carp_peer6"))
		return ELEM_CARP_PEER6;
	if (str_eq(name, "vrrp_state"))
		return ELEM_VRRP_STATE;
	if (str_eq(name, "vrid"))
		return ELEM_VRRP_VRID;
	if (str_eq(name, "vrrp_prio"))
		return ELEM_VRRP_PRIO;
	if (str_eq(name, "vrrp_interval"))
		return ELEM_VRRP_INTERVAL;
	if (str_eq(name, "u"))
		return ELEM_GRE_KEY;
	if (str_eq(name, "udpport"))
		return ELEM_GRE_UDPPORT;
	if (str_eq(name, "flags6"))
		return ELEM_FLAGS6;
	if (str_eq(name, "defuntagged"))
		return ELEM_BRIDGE_DEFUNTAGGED;
	if (str_eq(name, "ifmaxaddr"))
		return ELEM_MEMBER_IFMAXADDR;
	if (str_eq(name, "untagged"))
		return ELEM_MEMBER_UNTAGGED;
	if (str_eq(name, "vlan"))
		return ELEM_MEMBER_VLAN;
	if (str_eq(name, "vlan-id"))
		return ELEM_MEMBER_VLAN_ID;
	if (str_eq(name, "vlan-end"))
		return ELEM_MEMBER_VLAN_END;
	if (str_eq(name, "flowid-shift"))
		return ELEM_LAGG_FLOWID_SHIFT;
	if (str_eq(name, "rr-limit"))
		return ELEM_LAGG_RR_LIMIT;
	if (str_eq(name, "pcp"))
		return ELEM_LINK_PCP;
	if (str_eq(name, "wme-aci"))
		return ELEM_WME_ACI;
	if (str_eq(name, "cwmin"))
		return ELEM_WME_CWMIN;
	if (str_eq(name, "cwmax"))
		return ELEM_WME_CWMAX;
	if (str_eq(name, "aifs"))
		return ELEM_WME_AIFS;
	if (str_eq(name, "txop-limit"))
		return ELEM_WME_TXOPLIMIT;
	if (wlan_elem_id(name) != ELEM_NONE)
		return ELEM_WLAN;
	return ELEM_NONE;
}

static void
push_elem(struct ifxml_state *st, int id)
{
	if (st->sp < MAX_STACK)
		st->stack[st->sp++] = id;
}

static int
pop_elem(struct ifxml_state *st)
{
	if (st->sp > 0)
		return st->stack[--st->sp];
	return ELEM_NONE;
}

static int
cur_elem(struct ifxml_state *st)
{
	if (st->sp > 0)
		return st->stack[st->sp - 1];
	return ELEM_NONE;
}

static void
reset_text(struct ifxml_state *st)
{
	st->text[0] = EMPTY;
	st->text_len = 0;
}

static void
add_text(struct ifxml_state *st, const XML_Char *s, int len)
{
	int space = (int)sizeof(st->text) - st->text_len - 1;
	if (len > space)
		len = space;
	if (len > 0) {
		memcpy(st->text + st->text_len, s, len);
		st->text_len += len;
		st->text[st->text_len] = EMPTY;
	}
}

static const char *
get_text(struct ifxml_state *st)
{
	return st->text;
}

static void
cfg_add_arg(struct ifxml_cfg *cfg, const char *arg)
{
	if (cfg->argc >= MAX_ARGS - 1)
		errx(1, "too many arguments in XML interface config");
	cfg->argv[cfg->argc++] = __DECONST(char *, arg);
}

static void
cfg_add_arg_kv(struct ifxml_cfg *cfg, const char *key, const char *val)
{
	char *v;

	cfg_add_arg(cfg, key);
	v = strdup(val);
	if (v == NULL)
		err(1, "strdup");
	cfg_add_arg(cfg, v);
}

static void
cfg_reset(struct ifxml_cfg *cfg)
{
	cfg->ifname[0] = EMPTY;
	cfg->ifname_set = false;
	cfg->argc = 0;
	cfg->inet_addr[0] = EMPTY;
	cfg->inet_dst[0] = EMPTY;
	cfg->inet_netmask[0] = EMPTY;
	cfg->inet_broadcast[0] = EMPTY;
	cfg->inet6_addr[0] = EMPTY;
	cfg->inet6_dst[0] = EMPTY;
	cfg->inet6_prefixlen[0] = EMPTY;
}

static void
buf_append(char **buf, const char *sep, const char *val)
{
	char *nbuf;
	size_t curlen, seplen, need;

	curlen = *buf != NULL ? strlen(*buf) : 0;
	seplen = (curlen > 0 && sep != NULL) ? strlen(sep) : 0;
	need = curlen + seplen + strlen(val) + 1;
	nbuf = realloc(*buf, need);
	if (nbuf == NULL)
		err(1, "realloc");
	if (seplen > 0)
		memcpy(nbuf + curlen, sep, seplen);
	memcpy(nbuf + curlen + seplen, val, strlen(val) + 1);
	*buf = nbuf;
}

static void
ifxml_free_iface(struct ifxml_state *st)
{
	free(st->media_mode);
	free(st->media_option);
	free(st->wlan_chan_num);
	free(st->carp_state);
	free(st->wme_aci_name);
	free(st->bridge.defuntagged);
	for (int i = 0; i < st->bridge.num_members; i++) {
		free(st->bridge.members[i].ifmaxaddr);
		free(st->bridge.members[i].untagged);
		free(st->bridge.members[i].vlan_tagged);
	}
	free(st->vxlan.vni);
	free(st->vxlan.local);
	free(st->vxlan.local_port);
	free(st->vxlan.peer_type);
	free(st->vxlan.remote);
	free(st->vxlan.remote_port);
	free(st->vxlan.port_min);
	free(st->vxlan.port_max);
	free(st->vxlan.ttl);
	free(st->vxlan.ftable_max);
	free(st->vxlan.ftable_timeout);
	free(st->pfsync.syncdev);
	free(st->pfsync.syncpeer);
	free(st->pfsync.maxupdates);
	free(st->pfsync.version);
}

static void
set_str(char **dst, const char *src)
{
	char *v;

	v = strdup(src);
	if (v == NULL)
		err(1, "strdup");
	free(*dst);
	*dst = v;
}

static const char *
flag_name_to_cmd(const char *name)
{
	if (str_eq(name, "UP"))
		return "up";
	if (str_eq(name, "DEBUG"))
		return "debug";
	if (str_eq(name, "PROMISC"))
		return "promisc";
	if (str_eq(name, "ALLMULTI"))
		return "allmulti";
	if (str_eq(name, "MONITOR"))
		return "monitor";
	if (str_eq(name, "STATICARP"))
		return "staticarp";
	if (str_eq(name, "STICKYARP"))
		return "stickyarp";
	if (str_eq(name, "LINK0"))
		return "link0";
	if (str_eq(name, "LINK1"))
		return "link1";
	if (str_eq(name, "LINK2"))
		return "link2";
	if (str_eq(name, "NOARP"))
		return "arp";
	if (str_eq(name, "PPROMISC"))
		return "promisc";
	return NULL;
}

static const char *
cap_name_to_cmd(const char *name)
{
	if (str_eq(name, "RXCSUM"))
		return "rxcsum";
	if (str_eq(name, "TXCSUM"))
		return "txcsum";
	if (str_eq(name, "RXCSUM_IPV6"))
		return "rxcsum6";
	if (str_eq(name, "TXCSUM_IPV6"))
		return "txcsum6";
	if (str_eq(name, "NETCONS"))
		return "netcons";
	if (str_eq(name, "POLLING"))
		return "polling";
	if (str_eq(name, "TSO4"))
		return "tso4";
	if (str_eq(name, "TSO6"))
		return "tso6";
	if (str_eq(name, "LRO"))
		return "lro";
	if (str_eq(name, "WOL_UCAST"))
		return "wol_ucast";
	if (str_eq(name, "WOL_MCAST"))
		return "wol_mcast";
	if (str_eq(name, "WOL_MAGIC"))
		return "wol_magic";
	if (str_eq(name, "TXRTLMT"))
		return "txrtlmt";
	if (str_eq(name, "HWRXTSTMP"))
		return "hwrxtstmp";
	if (str_eq(name, "MEXTPG"))
		return "mextpg";
	if (str_eq(name, "VLAN_MTU"))
		return "vlanmtu";
	if (str_eq(name, "VLAN_HWTAGGING"))
		return "vlanhwtag";
	if (str_eq(name, "VLAN_HWFILTER"))
		return "vlanhwfilter";
	if (str_eq(name, "VLAN_HWTSO"))
		return "vlanhwtso";
	if (str_eq(name, "VLAN_HWCSUM"))
		return "vlanhwcsum";
	if (str_eq(name, "TXTLS4"))
		return "txtls";
	if (str_eq(name, "TXTLS6"))
		return "txtls";
	if (str_eq(name, "TXTLS_RTLMT"))
		return "txtlsrtlmt";
	if (str_eq(name, "RXTLS4"))
		return "rxtls";
	if (str_eq(name, "RXTLS6"))
		return "rxtls";
	if (str_eq(name, "IPSEC"))
		return "ipsec";
	if (str_eq(name, "TOE4"))
		return "toe";
	if (str_eq(name, "TOE6"))
		return "toe";
	return NULL;
}

static const char *
gre_opt_name_to_cmd(const char *name)
{
	if (str_eq(name, "ENABLE_CSUM"))
		return "enable_csum";
	if (str_eq(name, "ENABLE_SEQ"))
		return "enable_seq";
	if (str_eq(name, "UDPENCAP"))
		return "udpencap";
	return NULL;
}

static const char *
gif_opt_name_to_cmd(const char *name)
{
	if (str_eq(name, "NOCLAMP"))
		return "noclamp";
	if (str_eq(name, "IGNORE_SOURCE"))
		return "ignore_source";
	return NULL;
}

static const char *
flags6_name_to_cmd(const char *name)
{
	if (str_eq(name, "anycast"))
		return "anycast";
	if (str_eq(name, "tentative"))
		return "tentative";
	if (str_eq(name, "deprecated"))
		return "deprecated";
	if (str_eq(name, "autoconf"))
		return "autoconf";
	if (str_eq(name, "prefer_source"))
		return "prefer_source";
	return NULL;
}

static void
dry_run_print(const char *ifname, char **argv, int argc)
{
	printf("dry-run: %s", ifname);
	for (int i = 0; i < argc; i++)
		printf(" %s", argv[i]);
	printf("\n");
}

static void
build_apply_argv(struct ifxml_cfg *cfg, int iscreate,
    const char *const *af_args, int af_argc, const struct afswtch *afp)
{
	struct ifconfig_args _args = { };
	struct ifconfig_context _ctx = { };
	char *combined[MAX_ARGS];
	int comboc = 0;

	if (iscreate) {
		combined[comboc++] = __DECONST(char *, "create");
		/* iscreate only on first call, reset for subsequent */
		iscreate = 0;
	}
	for (int i = 0; i < cfg->argc; i++)
		combined[comboc++] = cfg->argv[i];
	for (int i = 0; i < af_argc; i++)
		combined[comboc++] = __DECONST(char *, af_args[i]);

	if (comboc == 0)
		return;

	if (ifconfig_xml_dry_run) {
		dry_run_print(cfg->ifname, combined, comboc);
		return;
	}

	_args.argc = comboc;
	_args.argv = combined;
	_args.ifname = cfg->ifname;
	_ctx.args = &_args;
	_ctx.ifname = cfg->ifname;
	_ctx.io_s = -1;
	_ctx.io_ss = NULL;

	ifmaybeload(&_args, cfg->ifname);
	ifconfig_ioctl(&_ctx, comboc > 0 && str_eq(combined[0], "create"), afp);
}

static void
apply_one_iface(struct ifxml_cfg *cfg)
{
	uint32_t ifindex;
	int need_create = 0;

	if (!cfg->ifname_set || null_or_empty(cfg->ifname))
		return;

	ifindex = if_nametoindex(cfg->ifname);
	if (ifindex == 0)
		need_create = 1;

	if (need_create) {
		const char *p = cfg->ifname;
		if (strncmp(p, "epair", 5)) {
			p += 5;
			while (*p >= '0' && *p <= '9')
				p++;
			if (*p == 'b' && *(p + 1) == EMPTY)
				need_create = 0;
		}
	}

	if (!need_create && cfg->argc == 0 && null_or_empty(cfg->inet_addr) &&
	    null_or_empty(cfg->inet6_addr))
		return;

	if (null_or_empty(cfg->inet_addr) && null_or_empty(cfg->inet6_addr)) {
		build_apply_argv(cfg, need_create, NULL, 0, NULL);
		return;
	}

	if (!null_or_empty(cfg->inet_addr)) {
		const char *af_extra[16];
		int aec = 0;
		af_extra[aec++] = cfg->inet_addr;
		if (!null_or_empty(cfg->inet_dst))
			af_extra[aec++] = cfg->inet_dst;
		if (!null_or_empty(cfg->inet_netmask)) {
			af_extra[aec++] = "netmask";
			af_extra[aec++] = cfg->inet_netmask;
		}
		if (!null_or_empty(cfg->inet_broadcast)) {
			af_extra[aec++] = "broadcast";
			af_extra[aec++] = cfg->inet_broadcast;
		}
		build_apply_argv(cfg, need_create, af_extra, aec, NULL);
		need_create = 0;
	}

	if (!null_or_empty(cfg->inet6_addr)) {
		const char *af_extra[16];
		int aec = 0;
		af_extra[aec++] = cfg->inet6_addr;
		if (!null_or_empty(cfg->inet6_dst))
			af_extra[aec++] = cfg->inet6_dst;
		if (!null_or_empty(cfg->inet6_prefixlen)) {
			af_extra[aec++] = "prefixlen";
			af_extra[aec++] = cfg->inet6_prefixlen;
		}
		build_apply_argv(cfg, need_create, af_extra, aec,
		    af_getbyfamily(AF_INET6));
	}
}

static void
flush_cfg(struct ifxml_state *st)
{
	struct ifxml_cfg *tmp;

	if (!st->cfg.ifname_set)
		return;
	tmp = realloc(st->all_cfgs,
	    (st->cfg_count + 1) * sizeof(struct ifxml_cfg));
	if (tmp == NULL) {
		warn("ifconfig_xml: realloc");
		return;
	}
	st->all_cfgs = tmp;
	st->all_cfgs[st->cfg_count++] = st->cfg;
}

static void
apply_all_ifaces(struct ifxml_state *st)
{
	for (int i = 0; i < st->cfg_count; i++)
		apply_one_iface(&st->all_cfgs[i]);
}

static void XMLCALL
start_elem(void *userData, const XML_Char *name, const XML_Char **atts)
{
	struct ifxml_state *st = userData;
	int id = elem_id(name);

	push_elem(st, id);
	reset_text(st);

	switch (id) {
	case ELEM_INTERFACE:
		ifxml_free_iface(st);
		cfg_reset(&st->cfg);
		memset(st->addr_type, 0, sizeof(st->addr_type));
		memset(st->link_addr, 0, sizeof(st->link_addr));
		memset(st->media_type, 0, sizeof(st->media_type));
		memset(st->media_subtype, 0, sizeof(st->media_subtype));
		memset(st->tunnel_src, 0, sizeof(st->tunnel_src));
		memset(st->tunnel_dst, 0, sizeof(st->tunnel_dst));
		st->in_addr_block = 0;
		memset(&st->addr, 0, sizeof(st->addr));
		st->in_bridge = 0;
		memset(&st->bridge, 0, sizeof(st->bridge));
		st->in_member = 0;
		memset(&st->cur_member, 0, sizeof(st->cur_member));
		memset(&st->lagg, 0, sizeof(st->lagg));
		st->in_lagg_port = 0;
		st->lagg_hash_l2 = 0;
		st->lagg_hash_l3 = 0;
		st->lagg_hash_l4 = 0;
		memset(&st->vxlan, 0, sizeof(st->vxlan));
		memset(&st->pfsync, 0, sizeof(st->pfsync));
		st->wlan_bssid_seen = 0;
		st->in_wme_aci = 0;
		break;
	case ELEM_MEDIA:
		st->media_capture = 0;
		memset(st->media_type, 0, sizeof(st->media_type));
		memset(st->media_subtype, 0, sizeof(st->media_subtype));
		free(st->media_mode);
		free(st->media_option);
		st->media_mode = NULL;
		st->media_option = NULL;
		break;
	case ELEM_ADDRESS_CONTAINER:
		st->in_addr_block = 1;
		memset(&st->addr, 0, sizeof(st->addr));
		break;
	case ELEM_BRIDGE:
		st->in_bridge = 1;
		memset(&st->bridge, 0, sizeof(st->bridge));
		break;
	case ELEM_MEMBER:
		st->in_member = 1;
		memset(&st->cur_member, 0, sizeof(st->cur_member));
		break;
	case ELEM_LAGG_PORT:
		st->in_lagg_port = 1;
		break;
	case ELEM_LAGG_PROTO:
	case ELEM_LAGG_HASH_L2:
	case ELEM_LAGG_HASH_L3:
	case ELEM_LAGG_HASH_L4:
		break;
	case ELEM_WME_ACI:
		st->in_wme_aci = 1;
		free(st->wme_aci_name);
		st->wme_aci_name = NULL;
		break;
	default:
		break;
	}

	(void)atts;
}

static void XMLCALL
end_elem(void *userData, const XML_Char *name)
{
	struct ifxml_state *st = userData;
	int id = pop_elem(st);
	const char *txt = get_text(st);
	int parent;

	switch (id) {
	case ELEM_IFNAME:
		strlcpy(st->cfg.ifname, txt, sizeof(st->cfg.ifname));
		st->cfg.ifname_set = true;
		break;
	case ELEM_MTU:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "mtu", txt);
		break;
	case ELEM_METRIC:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "metric", txt);
		break;
	case ELEM_DESCRIPTION:
	case ELEM_DESCR:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "description", txt);
		break;
	case ELEM_IF_FLAGS: {
		const char *cmd = flag_name_to_cmd(txt);
		if (cmd != NULL)
			cfg_add_arg(&st->cfg, cmd);
		break;
	}
	case ELEM_OPTIONS:
	case ELEM_IF_CAP: {
		const char *cmd;
		int par = cur_elem(st);

		if (par == id && !null_or_empty(txt)) {
			cmd = cap_name_to_cmd(txt);
			if (cmd == NULL)
				cmd = gre_opt_name_to_cmd(txt);
			if (cmd == NULL)
				cmd = gif_opt_name_to_cmd(txt);
			if (cmd != NULL)
				cfg_add_arg(&st->cfg, cmd);
		}
		break;
	}

	case ELEM_ADDR_TYPE:
		strlcpy(st->addr_type, txt, sizeof(st->addr_type));
		break;
	case ELEM_LINK_ADDR_VAL:
		strlcpy(st->link_addr, txt, sizeof(st->link_addr));
		parent = cur_elem(st);
		if (parent == ELEM_LINK_ADDR_PARENT &&
		    str_eq(st->addr_type, "ether") &&
		    !null_or_empty(st->link_addr)) {
			cfg_add_arg(&st->cfg, "ether");
			cfg_add_arg(&st->cfg, strdup(st->link_addr));
		}
		break;
	case ELEM_GROUP_NAME:
		parent = cur_elem(st);
		if (parent == ELEM_GROUP && !null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "group", txt);
		else if (parent == ELEM_LAGG_PORT && !null_or_empty(txt) &&
		    st->lagg.num_ports < MAX_LAGG_PORTS)
			strlcpy(st->lagg.ports[st->lagg.num_ports].name, txt,
			    sizeof(st->lagg.ports[st->lagg.num_ports].name));
		else if (parent == ELEM_WME_ACI && !null_or_empty(txt))
			set_str(&st->wme_aci_name, txt);
		break;
	case ELEM_MEDIA_TYPE:
		st->media_capture = 1;
		strlcpy(st->media_type, txt, sizeof(st->media_type));
		break;
	case ELEM_MEDIA_SUBTYPE:
		if (st->media_capture)
			strlcpy(st->media_subtype, txt,
			    sizeof(st->media_subtype));
		break;
	case ELEM_MEDIA_MODE:
		if (st->media_capture && !null_or_empty(txt))
			set_str(&st->media_mode, txt);
		break;
	case ELEM_MEDIA_OPTION:
		if (st->media_capture && !null_or_empty(txt))
			buf_append(&st->media_option, ",", txt);
		break;
	case ELEM_MEDIA:
		break;
	case ELEM_INET_ADDR:
		if (st->in_addr_block)
			strlcpy(st->addr.inet_addr, txt,
			    sizeof(st->addr.inet_addr));
		break;
	case ELEM_INET6_ADDR:
		if (st->in_addr_block)
			strlcpy(st->addr.inet6_addr, txt,
			    sizeof(st->addr.inet6_addr));
		break;
	case ELEM_DST_ADDR:
		if (st->in_addr_block)
			strlcpy(st->addr.dst_addr, txt,
			    sizeof(st->addr.dst_addr));
		break;
	case ELEM_NETMASK:
		if (st->in_addr_block)
			strlcpy(st->addr.netmask, txt,
			    sizeof(st->addr.netmask));
		break;
	case ELEM_BROADCAST:
		if (st->in_addr_block)
			strlcpy(st->addr.broadcast, txt,
			    sizeof(st->addr.broadcast));
		break;
	case ELEM_PREFIXLEN:
		if (st->in_addr_block)
			strlcpy(st->addr.prefixlen, txt,
			    sizeof(st->addr.prefixlen));
		break;
	case ELEM_ADDRESS_CONTAINER:
		st->in_addr_block = 0;
		if (!null_or_empty(st->addr.inet_addr)) {
			strlcpy(st->cfg.inet_addr, st->addr.inet_addr,
			    sizeof(st->cfg.inet_addr));
			strlcpy(st->cfg.inet_dst, st->addr.dst_addr,
			    sizeof(st->cfg.inet_dst));
			strlcpy(st->cfg.inet_netmask, st->addr.netmask,
			    sizeof(st->cfg.inet_netmask));
			strlcpy(st->cfg.inet_broadcast, st->addr.broadcast,
			    sizeof(st->cfg.inet_broadcast));
		} else if (!null_or_empty(st->addr.inet6_addr)) {
			strlcpy(st->cfg.inet6_addr, st->addr.inet6_addr,
			    sizeof(st->cfg.inet6_addr));
			strlcpy(st->cfg.inet6_dst, st->addr.dst_addr,
			    sizeof(st->cfg.inet6_dst));
			strlcpy(st->cfg.inet6_prefixlen, st->addr.prefixlen,
			    sizeof(st->cfg.inet6_prefixlen));
		}
		break;
	case ELEM_FIB:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "fib", txt);
		break;
	case ELEM_TUNNEL_FIB:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "tunnelfib", txt);
		break;
	case ELEM_BRIDGE_PRIORITY:
		strlcpy(st->bridge.priority, txt, sizeof(st->bridge.priority));
		break;
	case ELEM_HELLOTIME:
		strlcpy(st->bridge.hellotime, txt,
		    sizeof(st->bridge.hellotime));
		break;
	case ELEM_FWDDELAY:
		strlcpy(st->bridge.fwddelay, txt, sizeof(st->bridge.fwddelay));
		break;
	case ELEM_MAXAGE:
		strlcpy(st->bridge.maxage, txt, sizeof(st->bridge.maxage));
		break;
	case ELEM_HOLDCNT:
		strlcpy(st->bridge.holdcnt, txt, sizeof(st->bridge.holdcnt));
		break;
	case ELEM_STP_PROTO:
		if (!st->in_member)
			strlcpy(st->bridge.stp_proto, txt,
			    sizeof(st->bridge.stp_proto));
		break;
	case ELEM_MAXADDR:
		strlcpy(st->bridge.maxaddr, txt, sizeof(st->bridge.maxaddr));
		break;
	case ELEM_TIMEOUT:
		strlcpy(st->bridge.timeout, txt, sizeof(st->bridge.timeout));
		break;
	case ELEM_MEMBER_NAME:
		if (st->in_member)
			strlcpy(st->cur_member.name, txt,
			    sizeof(st->cur_member.name));
		break;
	case ELEM_PORT_PRIORITY:
		if (st->in_member)
			strlcpy(st->cur_member.priority, txt,
			    sizeof(st->cur_member.priority));
		break;
	case ELEM_PATH_COST:
		if (st->in_member)
			strlcpy(st->cur_member.path_cost, txt,
			    sizeof(st->cur_member.path_cost));
		break;
	case ELEM_VLAN_PROTO:
		if (st->in_member)
			strlcpy(st->cur_member.vlan_proto, txt,
			    sizeof(st->cur_member.vlan_proto));
		break;
	case ELEM_MEMBER:
		if (st->in_bridge && !null_or_empty(st->cur_member.name) &&
		    st->bridge.num_members < MAX_BRIDGE_MEMBERS) {
			st->bridge.members[st->bridge.num_members++] =
			    st->cur_member;
			st->cur_member.ifmaxaddr = NULL;
			st->cur_member.untagged = NULL;
			st->cur_member.vlan_tagged = NULL;
		} else {
			free(st->cur_member.ifmaxaddr);
			free(st->cur_member.untagged);
			free(st->cur_member.vlan_tagged);
		}
		st->in_member = 0;
		break;
	case ELEM_BRIDGE:
		st->in_bridge = 0;
		if (!null_or_empty(st->bridge.priority))
			cfg_add_arg_kv(&st->cfg, "priority",
			    st->bridge.priority);
		if (!null_or_empty(st->bridge.hellotime))
			cfg_add_arg_kv(&st->cfg, "hellotime",
			    st->bridge.hellotime);
		if (!null_or_empty(st->bridge.fwddelay))
			cfg_add_arg_kv(&st->cfg, "fwddelay",
			    st->bridge.fwddelay);
		if (!null_or_empty(st->bridge.maxage))
			cfg_add_arg_kv(&st->cfg, "maxage", st->bridge.maxage);
		if (!null_or_empty(st->bridge.holdcnt))
			cfg_add_arg_kv(&st->cfg, "holdcnt", st->bridge.holdcnt);
		if (!null_or_empty(st->bridge.stp_proto))
			cfg_add_arg_kv(&st->cfg, "proto", st->bridge.stp_proto);
		if (!null_or_empty(st->bridge.maxaddr))
			cfg_add_arg_kv(&st->cfg, "maxaddr", st->bridge.maxaddr);
		if (!null_or_empty(st->bridge.timeout))
			cfg_add_arg_kv(&st->cfg, "timeout", st->bridge.timeout);
		if (!null_or_empty(st->bridge.defuntagged))
			cfg_add_arg_kv(&st->cfg, "defuntagged",
			    st->bridge.defuntagged);
		for (int i = 0; i < st->bridge.num_members; i++) {
			struct ifxml_bridge_member *m = &st->bridge.members[i];

			cfg_add_arg_kv(&st->cfg, "addm", m->name);
		}
		for (int i = 0; i < st->bridge.num_members; i++) {
			struct ifxml_bridge_member *m = &st->bridge.members[i];

			if (!null_or_empty(m->priority)) {
				cfg_add_arg(&st->cfg, "ifpriority");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->priority));
			}
			if (!null_or_empty(m->path_cost)) {
				cfg_add_arg(&st->cfg, "ifpathcost");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->path_cost));
			}
			if (!null_or_empty(m->ifmaxaddr)) {
				cfg_add_arg(&st->cfg, "ifmaxaddr");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->ifmaxaddr));
			}
			if (!null_or_empty(m->untagged)) {
				cfg_add_arg(&st->cfg, "ifuntagged");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->untagged));
			}
			if (!null_or_empty(m->vlan_tagged)) {
				cfg_add_arg(&st->cfg, "iftagged");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->vlan_tagged));
			}
			if (!null_or_empty(m->vlan_proto) &&
			    (str_eq(m->vlan_proto, "802.1q") ||
				str_eq(m->vlan_proto, "802.1ad"))) {
				cfg_add_arg(&st->cfg, "ifvlanproto");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->vlan_proto));
			}
		}
		break;
	case ELEM_TUNNEL_SRC:
		strlcpy(st->tunnel_src, txt, sizeof(st->tunnel_src));
		break;
	case ELEM_TUNNEL_DST:
		strlcpy(st->tunnel_dst, txt, sizeof(st->tunnel_dst));
		break;
	case ELEM_VLAN_TAG:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "vlan", txt);
		break;
	case ELEM_VLAN_PROTOCOL:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "vlanproto", txt);
		break;
	case ELEM_VLAN_PCP:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "vlanpcp", txt);
		break;
	case ELEM_PARENT_IFNAME:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "vlandev", txt);
		break;
	case ELEM_PARENT:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "wlandev", txt);
		break;
	case ELEM_VXLAN_VNI:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.vni, txt);
		break;
	case ELEM_VXLAN_LOCAL:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.local, txt);
		break;
	case ELEM_VXLAN_LOCAL_PORT:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.local_port, txt);
		break;
	case ELEM_VXLAN_PEER_TYPE:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.peer_type, txt);
		break;
	case ELEM_VXLAN_REMOTE:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.remote, txt);
		break;
	case ELEM_VXLAN_REMOTE_PORT:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.remote_port, txt);
		break;
	case ELEM_VXLAN_LEARNING:
		st->vxlan.learning_seen = 1;
		st->vxlan.learning_on = !str_eq(txt, "no");
		break;
	case ELEM_VXLAN_PORT_MIN:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.port_min, txt);
		break;
	case ELEM_VXLAN_PORT_MAX:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.port_max, txt);
		break;
	case ELEM_VXLAN_TTL:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.ttl, txt);
		break;
	case ELEM_VXLAN_FTABLE_MAX:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.ftable_max, txt);
		break;
	case ELEM_VXLAN_FTABLE_TIMEOUT:
		if (!null_or_empty(txt))
			set_str(&st->vxlan.ftable_timeout, txt);
		break;
	case ELEM_PFSYNC_SYNCDEV:
		if (!null_or_empty(txt))
			set_str(&st->pfsync.syncdev, txt);
		break;
	case ELEM_PFSYNC_SYNCPEER:
		if (!null_or_empty(txt))
			set_str(&st->pfsync.syncpeer, txt);
		break;
	case ELEM_PFSYNC_MAXUPDATES:
		if (!null_or_empty(txt))
			set_str(&st->pfsync.maxupdates, txt);
		break;
	case ELEM_PFSYNC_DEFER:
		st->pfsync.defer_seen = 1;
		st->pfsync.defer_on = str_eq(txt, "on");
		break;
	case ELEM_PFSYNC_VERSION:
		if (!null_or_empty(txt))
			set_str(&st->pfsync.version, txt);
		break;
	case ELEM_CARP_STATE:
	case ELEM_VRRP_STATE:
		if (!null_or_empty(txt))
			set_str(&st->carp_state, txt);
		break;
	case ELEM_CARP_VHID:
	case ELEM_VRRP_VRID:
		if (st->in_addr_block) {
			free(st->carp_state);
			st->carp_state = NULL;
			break;
		}
		if (!null_or_empty(txt)) {
			cfg_add_arg_kv(&st->cfg, "vhid", txt);
			if (!null_or_empty(st->carp_state)) {
				cfg_add_arg_kv(&st->cfg, "state",
				    st->carp_state);
				free(st->carp_state);
				st->carp_state = NULL;
			}
		}
		break;
	case ELEM_CARP_ADVBASE:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "advbase", txt);
		break;
	case ELEM_CARP_ADVSKEW:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "advskew", txt);
		break;
	case ELEM_CARP_KEY:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "pass", txt);
		break;
	case ELEM_CARP_PEER:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "peer", txt);
		break;
	case ELEM_CARP_PEER6:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "peer6", txt);
		break;
	case ELEM_VRRP_PRIO:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "vrrpprio", txt);
		break;
	case ELEM_VRRP_INTERVAL:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "vrrpinterval", txt);
		break;
	case ELEM_GRE_KEY:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "grekey", txt);
		break;
	case ELEM_GRE_UDPPORT:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "udpport", txt);
		break;
	case ELEM_FLAGS6: {
		const char *cmd;
		int par = cur_elem(st);

		if (par == id && !null_or_empty(txt)) {
			cmd = flags6_name_to_cmd(txt);
			if (cmd != NULL)
				cfg_add_arg(&st->cfg, cmd);
		}
		break;
	}
	case ELEM_BRIDGE_DEFUNTAGGED:
		if (!null_or_empty(txt))
			set_str(&st->bridge.defuntagged, txt);
		break;
	case ELEM_MEMBER_IFMAXADDR:
		if (st->in_member && !null_or_empty(txt))
			set_str(&st->cur_member.ifmaxaddr, txt);
		break;
	case ELEM_MEMBER_UNTAGGED:
		if (st->in_member && !null_or_empty(txt))
			set_str(&st->cur_member.untagged, txt);
		break;
	case ELEM_MEMBER_VLAN:
		break;
	case ELEM_MEMBER_VLAN_ID:
		if (st->in_member && !null_or_empty(txt))
			buf_append(&st->cur_member.vlan_tagged, ",", txt);
		break;
	case ELEM_MEMBER_VLAN_END:
		if (st->in_member && !null_or_empty(txt))
			buf_append(&st->cur_member.vlan_tagged, "-", txt);
		break;
	case ELEM_LAGG_FLOWID_SHIFT:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "flowid_shift", txt);
		break;
	case ELEM_LAGG_RR_LIMIT:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "rr_limit", txt);
		break;
	case ELEM_LINK_PCP:
		if (!null_or_empty(txt))
			cfg_add_arg_kv(&st->cfg, "pcp", txt);
		break;
	case ELEM_WLAN:
		if (!null_or_empty(txt)) {
			if (str_eq(name, "bssid"))
				st->wlan_bssid_seen = 1;
			wlan_emit_cmd(st, name, txt);
		}
		break;
	case ELEM_WME_ACI:
		st->in_wme_aci = 0;
		break;
	case ELEM_WME_CWMIN:
		wme_emit_cmd(st, "cwmin", txt);
		break;
	case ELEM_WME_CWMAX:
		wme_emit_cmd(st, "cwmax", txt);
		break;
	case ELEM_WME_AIFS:
		wme_emit_cmd(st, "aifs", txt);
		break;
	case ELEM_WME_TXOPLIMIT:
		wme_emit_cmd(st, "txoplimit", txt);
		break;
	case ELEM_LAGG_PROTO:
		strlcpy(st->lagg.proto, txt, sizeof(st->lagg.proto));
		break;
	case ELEM_LAGG_HASH_L2:
		st->lagg_hash_l2 = 1;
		break;
	case ELEM_LAGG_HASH_L3:
		st->lagg_hash_l3 = 1;
		break;
	case ELEM_LAGG_HASH_L4:
		st->lagg_hash_l4 = 1;
		break;
	case ELEM_LAGG_PORT:
		st->in_lagg_port = 0;
		if (!null_or_empty(st->lagg.ports[st->lagg.num_ports].name))
			st->lagg.num_ports++;
		break;
	case ELEM_INTERFACE:
		if (!null_or_empty(st->lagg.proto))
			cfg_add_arg_kv(&st->cfg, "laggproto", st->lagg.proto);
		if (st->lagg_hash_l2 || st->lagg_hash_l3 || st->lagg_hash_l4) {
			char hashbuf[32];
			hashbuf[0] = EMPTY;
			if (st->lagg_hash_l2)
				concat(hashbuf, sizeof(hashbuf), "l2");
			if (st->lagg_hash_l3) {
				if (!null_or_empty(hashbuf))
					concat(hashbuf, sizeof(hashbuf), ",");
				concat(hashbuf, sizeof(hashbuf), "l3");
			}
			if (st->lagg_hash_l4) {
				if (!null_or_empty(hashbuf))
					concat(hashbuf, sizeof(hashbuf), ",");
				concat(hashbuf, sizeof(hashbuf), "l4");
			}
			if (!null_or_empty(hashbuf))
				cfg_add_arg_kv(&st->cfg, "lagghash", hashbuf);
		}
		for (int i = 0; i < st->lagg.num_ports; i++)
			cfg_add_arg_kv(&st->cfg, "laggport",
			    st->lagg.ports[i].name);
		if (!null_or_empty(st->media_subtype)) {
			cfg_add_arg(&st->cfg, "media");
			cfg_add_arg(&st->cfg, strdup(st->media_subtype));
		}
		if (!null_or_empty(st->media_mode))
			cfg_add_arg_kv(&st->cfg, "mode", st->media_mode);
		if (!null_or_empty(st->media_option))
			cfg_add_arg_kv(&st->cfg, "mediaopt", st->media_option);
		if (!null_or_empty(st->tunnel_src) ||
		    !null_or_empty(st->tunnel_dst)) {
			cfg_add_arg(&st->cfg, "tunnel");
			cfg_add_arg(&st->cfg,
			    strdup(!null_or_empty(st->tunnel_src) ?
				    st->tunnel_src :
				    "0.0.0.0"));
			cfg_add_arg(&st->cfg,
			    strdup(!null_or_empty(st->tunnel_dst) ?
				    st->tunnel_dst :
				    "0.0.0.0"));
		}
		if (!null_or_empty(st->vxlan.vni))
			cfg_add_arg_kv(&st->cfg, "vni", st->vxlan.vni);
		if (!null_or_empty(st->vxlan.local))
			cfg_add_arg_kv(&st->cfg, "vxlanlocal", st->vxlan.local);
		if (!null_or_empty(st->vxlan.local_port))
			cfg_add_arg_kv(&st->cfg, "vxlanlocalport",
			    st->vxlan.local_port);
		if (!null_or_empty(st->vxlan.remote)) {
			if (!null_or_empty(st->vxlan.peer_type) &&
			    str_eq(st->vxlan.peer_type, "group"))
				cfg_add_arg_kv(&st->cfg, "vxlangroup",
				    st->vxlan.remote);
			else
				cfg_add_arg_kv(&st->cfg, "vxlanremote",
				    st->vxlan.remote);
		}
		if (!null_or_empty(st->vxlan.remote_port))
			cfg_add_arg_kv(&st->cfg, "vxlanremoteport",
			    st->vxlan.remote_port);
		if (st->vxlan.learning_seen)
			cfg_add_arg(&st->cfg,
			    st->vxlan.learning_on ? "vxlanlearn" :
						    "-vxlanlearn");
		if (!null_or_empty(st->vxlan.port_min) &&
		    !null_or_empty(st->vxlan.port_max)) {
			cfg_add_arg(&st->cfg, "vxlanportrange");
			cfg_add_arg(&st->cfg, strdup(st->vxlan.port_min));
			cfg_add_arg(&st->cfg, strdup(st->vxlan.port_max));
		}
		if (!null_or_empty(st->vxlan.ttl))
			cfg_add_arg_kv(&st->cfg, "vxlanttl", st->vxlan.ttl);
		if (!null_or_empty(st->vxlan.ftable_max))
			cfg_add_arg_kv(&st->cfg, "vxlanmaxaddr",
			    st->vxlan.ftable_max);
		if (!null_or_empty(st->vxlan.ftable_timeout))
			cfg_add_arg_kv(&st->cfg, "vxlantimeout",
			    st->vxlan.ftable_timeout);
		if (!null_or_empty(st->pfsync.syncdev))
			cfg_add_arg_kv(&st->cfg, "syncdev", st->pfsync.syncdev);
		if (!null_or_empty(st->pfsync.syncpeer))
			cfg_add_arg_kv(&st->cfg, "syncpeer",
			    st->pfsync.syncpeer);
		if (!null_or_empty(st->pfsync.maxupdates))
			cfg_add_arg_kv(&st->cfg, "maxupd",
			    st->pfsync.maxupdates);
		if (st->pfsync.defer_seen)
			cfg_add_arg(&st->cfg,
			    st->pfsync.defer_on ? "defer" : "-defer");
		if (!null_or_empty(st->pfsync.version))
			cfg_add_arg_kv(&st->cfg, "version", st->pfsync.version);
		if (!null_or_empty(st->wlan_chan_num))
			cfg_add_arg_kv(&st->cfg, "channel", st->wlan_chan_num);
		flush_cfg(st);
		break;
	default:
		break;
	}
}

static void XMLCALL
char_data(void *userData, const XML_Char *s, int len)
{
	struct ifxml_state *st = userData;
	add_text(st, s, len);
}

int
ifconfig_xml_apply(if_ctx *ctx, const char *filename)
{
	char buf[65536];
	struct ifxml_state st;
	int fd;
	ssize_t n;
	enum XML_Status xstatus;

	memset(&st, 0, sizeof(st));
	st.ctx = ctx;
	if (ctx != NULL)
		st.args = ctx->args;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		err(1, "cannot open %s", filename);

	st.parser = XML_ParserCreate(NULL);
	if (st.parser == NULL)
		errx(1, "XML_ParserCreate failed");

	XML_SetUserData(st.parser, &st);
	XML_SetElementHandler(st.parser, start_elem, end_elem);
	XML_SetCharacterDataHandler(st.parser, char_data);

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		xstatus = XML_Parse(st.parser, buf, (int)n, 0);
		if (xstatus == XML_STATUS_ERROR) {
			enum XML_Error xerr = XML_GetErrorCode(st.parser);
			warnx("XML parse error in %s at line %lu: %s", filename,
			    (unsigned long)XML_GetCurrentLineNumber(st.parser),
			    XML_ErrorString(xerr));
			break;
		}
	}
	xstatus = XML_Parse(st.parser, buf, 0, 1);
	if (xstatus == XML_STATUS_ERROR) {
		enum XML_Error xerr = XML_GetErrorCode(st.parser);
		warnx("XML final parse error in %s: %s", filename,
		    XML_ErrorString(xerr));
	}

	XML_ParserFree(st.parser);
	close(fd);

	apply_all_ifaces(&st);
	return (0);
}

static void
ifxml_cb(const char *arg)
{
	ifconfig_xml_apply(NULL, arg);
	exit(exit_code);
}

static void
dryrun_cb(const char *arg __unused)
{
	ifconfig_xml_dry_run = 1;
}

static struct option xml_opt = {
	.opt = "x:",
	.opt_usage = "[-x xmlfile]",
	.cb = ifxml_cb,
};

static struct option xml_dryrun_opt = {
	.opt = "N",
	.opt_usage = "[-N]",
	.cb = dryrun_cb,
};

static __constructor void
ifxml_ctor(void)
{
	opt_register(&xml_dryrun_opt);
	opt_register(&xml_opt);
}
