#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bsdxml.h>
#include "ifconfig.h"
#include "ifconfig_xml.h"

#define MAX_ARGS 256
#define NONEMPTY(s) ((s) != NULL && (s)[0] != '\0')
#define ELEMINNEREQ(a, b) (strcmp(a, b) == 0)

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

#define MAX_STACK 64

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

#define MAX_BRIDGE_MEMBERS 256

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

#define MAX_LAGG_PORTS 64

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

struct wlan_cmd {
	const char *name;
	const char *cmd;
	int is_flag;
};

static void buf_append(char **buf, const char *sep, const char *val);
static void set_str(char **dst, const char *src);
static void ifxml_free_iface(struct ifxml_state *st);
static void cfg_add_arg(struct ifxml_cfg *cfg, const char *arg);
static void cfg_add_arg_kv(struct ifxml_cfg *cfg, const char *key,
    const char *val);

static const struct wlan_cmd wlan_cmds[] = {
	{ "ssid",		"ssid",			0 },
	{ "bssid",		"bssid",		0 },
	{ "authmode",		"authmode",		0 },
	{ "powersavemode",	"powersavemode",	0 },
	{ "powersavesleep",	"powersavesleep",	0 },
	{ "deftxkey",		"deftxkey",		0 },
	{ "privacy",		"wepmode",		0 },
	{ "rtsthreshold",	"rtsthreshold",		0 },
	{ "protmode",		"protmode",		0 },
	{ "txpower",		"txpower",		0 },
	{ "roaming",		"roaming",		0 },
	{ "dtimperiod",		"dtimperiod",		0 },
	{ "bintval",		"bintval",		0 },
	{ "fragthreshold",	"fragthreshold",	0 },
	{ "bmiss",		"bmiss",		0 },
	{ "scanvalid",		"scanvalid",		0 },
	{ "bgscanintvl",	"bgscanintvl",		0 },
	{ "bgscanidle",		"bgscanidle",		0 },
	{ "roam_rssi",		"roam:rssi",		0 },
	{ "roam_rate",		"roam:rate",		0 },
	{ "maxretry",		"maxretry",		0 },
	{ "ampdulimit",		"ampdulimit",		0 },
	{ "ampdudensity",	"ampdudensity",		0 },
	{ "htprotmode",		"htprotmode",		0 },
	{ "tdmaslot",		"tdmaslot",		0 },
	{ "tdmaslotcnt",	"tdmaslotcnt",		0 },
	{ "tdmaslotlen",	"tdmaslotlen",		0 },
	{ "tdmabintval",	"tdmabintval",		0 },
	{ "meshttl",		"meshttl",		0 },
	{ "meshmetric",		"meshmetric",		0 },
	{ "meshpath",		"meshpath",		0 },
	{ "hwmprootmode",	"hwmprootmode",		0 },
	{ "hwmpmaxhops",	"hwmpmaxhops",		0 },
	{ "regdomain",		"regdomain",		0 },
	{ "country",		"country",		0 },
	{ "ucastrate",		"ucastrate",		0 },
	{ "mcastrate",		"mcastrate",		0 },
	{ "mgmtrate",		"mgmtrate",		0 },
	{ "tsn",		"tsn",			1 },
	{ "ecm",		"ecm",			1 },
	{ "pureg",		"pureg",		1 },
	{ "htcompat",		"htcompat",		1 },
	{ "dotd",		"dotd",			1 },
	{ "dfs",		"dfs",			1 },
	{ "inactivity",		"inact",		1 },
	{ "rifs",		"rifs",			1 },
	{ "uapsd",		"uapsd",		1 },
	{ "shortgi",		"shortgi",		1 },
	{ "puren",		"puren",		1 },
	{ "bgscan",		"bgscan",		1 },
	{ "wme",		"wme",			1 },
	{ "burst",		"burst",		1 },
	{ "ff",			"ff",			1 },
	{ "turbo",		"dturbo",		1 },
	{ "dwds",		"dwds",			1 },
	{ "hidessid",		"hidessid",		1 },
	{ "apbridge",		"apbridge",		1 },
	{ "doth",		"doth",			1 },
	{ "ht",			"ht",			1 },
	{ "ht20",		"ht20",			1 },
	{ "ht40",		"ht40",			1 },
	{ "vht",		"vht",			1 },
	{ "vht40",		"vht40",		1 },
	{ "vht80",		"vht80",		1 },
	{ "vht160",		"vht160",		1 },
	{ "vht80p80",		"vht80p80",		1 },
	{ "ampdu",		"ampdu",		1 },
	{ "ampdutx",		"ampdutx",		1 },
	{ "ampdurx",		"ampdurx",		1 },
	{ "amsdu",		"amsdu",		1 },
	{ "amsdutx",		"amsdutx",		1 },
	{ "amsdurx",		"amsdurx",		1 },
	{ "stbc",		"stbc",			1 },
	{ "stbctx",		"stbctx",		1 },
	{ "stbcrx",		"stbcrx",		1 },
	{ "ldpc",		"ldpc",			1 },
	{ "ldpctx",		"ldpctx",		1 },
	{ "ldpcrx",		"ldpcrx",		1 },
	{ "meshpeering",	"meshpeering",		1 },
	{ "meshforward",	"meshforward",		1 },
	{ "meshgate",		"meshgate",		1 },
};

static int
wlan_elem_id(const char *name)
{
	if (strcmp(name, "chan-num") == 0)
		return ELEM_WLAN;
	if (strcmp(name, "chan-mode") == 0)
		return ELEM_WLAN;
	if (strcmp(name, "smps") == 0)
		return ELEM_WLAN;
	if (strcmp(name, "location") == 0)
		return ELEM_WLAN;
	for (size_t i = 0; i < nitems(wlan_cmds); i++)
		if (strcmp(name, wlan_cmds[i].name) == 0)
			return ELEM_WLAN;
	return ELEM_NONE;
}

static void
wlan_emit_cmd(struct ifxml_state *st, const char *name, const char *txt)
{
	if (strcmp(name, "smps") == 0) {
		if (ELEMINNEREQ(txt, "dynamic"))
			cfg_add_arg(&st->cfg, "smpsdyn");
		else if (ELEMINNEREQ(txt, "static"))
			cfg_add_arg(&st->cfg, "smps");
		return;
	}
	if (strcmp(name, "location") == 0) {
		if (ELEMINNEREQ(txt, "indoor") || ELEMINNEREQ(txt, "outdoor") ||
		    ELEMINNEREQ(txt, "anywhere"))
			cfg_add_arg(&st->cfg, strdup(txt));
		return;
	}
	if (strcmp(name, "chan-num") == 0) {
		set_str(&st->wlan_chan_num, txt);
		return;
	}
	if (strcmp(name, "ssid") == 0) {
		cfg_add_arg_kv(&st->cfg,
		    st->wlan_bssid_seen ? "stationname" : "ssid", txt);
		return;
	}
	if (strcmp(name, "authmode") == 0) {
		if (ELEMINNEREQ(txt, "802.1x"))
			cfg_add_arg_kv(&st->cfg, "authmode", "8021x");
		else if (ELEMINNEREQ(txt, "WPA2/802.11i") ||
		    ELEMINNEREQ(txt, "WPA1+WPA2/802.11i") ||
		    ELEMINNEREQ(txt, "AUTO"))
			; /* not settable via authmode */
		else
			cfg_add_arg_kv(&st->cfg, "authmode", txt);
		return;
	}
	
	for (size_t i = 0; i < nitems(wlan_cmds); i++) {
		if (strcmp(name, wlan_cmds[i].name) != 0)
			continue;
		if (wlan_cmds[i].is_flag) {
			if (ELEMINNEREQ(txt, "0")) {
				char *neg;

				if (asprintf(&neg, "-%s", wlan_cmds[i].cmd) == -1)
					err(1, "asprintf");
				cfg_add_arg(&st->cfg, neg);
			} else {
				cfg_add_arg(&st->cfg, wlan_cmds[i].cmd);
			}
		} else {
			cfg_add_arg_kv(&st->cfg, wlan_cmds[i].cmd, txt);
		}
		return;
	}
}

static void
wme_emit_cmd(struct ifxml_state *st, const char *cmd, const char *val)
{
	static const char *acnames[] = { "AC_BE", "AC_BK", "AC_VI", "AC_VO" };
	size_t i;

	if (!st->in_wme_aci || !NONEMPTY(val) || !NONEMPTY(st->wme_aci_name))
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
	if (strcmp(name, "interface") == 0) return ELEM_INTERFACE;
	if (strcmp(name, "ifname") == 0) return ELEM_IFNAME;
	if (strcmp(name, "interface-flags") == 0) return ELEM_IF_FLAGS;
	if (strcmp(name, "metric") == 0) return ELEM_METRIC;
	if (strcmp(name, "mtu") == 0) return ELEM_MTU;
	if (strcmp(name, "description") == 0) return ELEM_DESCRIPTION;
	if (strcmp(name, "descr") == 0) return ELEM_DESCR;
	if (strcmp(name, "link-address") == 0) return ELEM_LINK_ADDR_PARENT;
	if (strcmp(name, "addr-type") == 0) return ELEM_ADDR_TYPE;
	if (strcmp(name, "link-addr") == 0) return ELEM_LINK_ADDR_VAL;
	if (strcmp(name, "group") == 0) return ELEM_GROUP;
	if (strcmp(name, "name") == 0) return ELEM_GROUP_NAME;
	if (strcmp(name, "media") == 0) return ELEM_MEDIA;
	if (strcmp(name, "type") == 0) return ELEM_MEDIA_TYPE;
	if (strcmp(name, "subtype") == 0) return ELEM_MEDIA_SUBTYPE;
	if (strcmp(name, "mode") == 0) return ELEM_MEDIA_MODE;
	if (strcmp(name, "option") == 0) return ELEM_MEDIA_OPTION;
	if (strcmp(name, "interface-capabilities") == 0) return ELEM_IF_CAP;
	if (strcmp(name, "options") == 0) return ELEM_OPTIONS;
	if (strcmp(name, "fib-id") == 0) return ELEM_FIB;
	if (strcmp(name, "tunnelfib-id") == 0) return ELEM_TUNNEL_FIB;
	if (strcmp(name, "address") == 0) return ELEM_ADDRESS_CONTAINER;
	if (strcmp(name, "inet-addr") == 0) return ELEM_INET_ADDR;
	if (strcmp(name, "inet6-addr") == 0) return ELEM_INET6_ADDR;
	if (strcmp(name, "dst-addr") == 0) return ELEM_DST_ADDR;
	if (strcmp(name, "netmask") == 0) return ELEM_NETMASK;
	if (strcmp(name, "broadcast") == 0) return ELEM_BROADCAST;
	if (strcmp(name, "prefixlen") == 0) return ELEM_PREFIXLEN;
	if (strcmp(name, "bridge") == 0) return ELEM_BRIDGE;
	if (strcmp(name, "bridge-priority") == 0) return ELEM_BRIDGE_PRIORITY;
	if (strcmp(name, "hellotime") == 0) return ELEM_HELLOTIME;
	if (strcmp(name, "fwddelay") == 0) return ELEM_FWDDELAY;
	if (strcmp(name, "maxage") == 0) return ELEM_MAXAGE;
	if (strcmp(name, "holdcnt") == 0) return ELEM_HOLDCNT;
	if (strcmp(name, "stp-proto") == 0) return ELEM_STP_PROTO;
	if (strcmp(name, "maxaddr") == 0) return ELEM_MAXADDR;
	if (strcmp(name, "timeout") == 0) return ELEM_TIMEOUT;
	if (strcmp(name, "member") == 0) return ELEM_MEMBER;
	if (strcmp(name, "member-name") == 0) return ELEM_MEMBER_NAME;
	if (strcmp(name, "port-priority") == 0) return ELEM_PORT_PRIORITY;
	if (strcmp(name, "path-cost") == 0) return ELEM_PATH_COST;
	if (strcmp(name, "vlan-proto") == 0) return ELEM_VLAN_PROTO;
	if (strcmp(name, "lagg-proto") == 0) return ELEM_LAGG_PROTO;
	if (strcmp(name, "lagg-hash-l2") == 0) return ELEM_LAGG_HASH_L2;
	if (strcmp(name, "lagg-hash-l3") == 0) return ELEM_LAGG_HASH_L3;
	if (strcmp(name, "lagg-hash-l4") == 0) return ELEM_LAGG_HASH_L4;
	if (strcmp(name, "laggport") == 0) return ELEM_LAGG_PORT;
	if (strcmp(name, "tunnel-src") == 0) return ELEM_TUNNEL_SRC;
	if (strcmp(name, "tunnel-dst") == 0) return ELEM_TUNNEL_DST;
	if (strcmp(name, "vlantag") == 0) return ELEM_VLAN_TAG;
	if (strcmp(name, "vlan-protocol") == 0) return ELEM_VLAN_PROTOCOL;
	if (strcmp(name, "vlanpcp") == 0) return ELEM_VLAN_PCP;
	if (strcmp(name, "parent-ifname") == 0) return ELEM_PARENT_IFNAME;
	if (strcmp(name, "parent") == 0) return ELEM_PARENT;
	if (strcmp(name, "vni") == 0) return ELEM_VXLAN_VNI;
	if (strcmp(name, "local-src") == 0) return ELEM_VXLAN_LOCAL;
	if (strcmp(name, "local-src-port") == 0) return ELEM_VXLAN_LOCAL_PORT;
	if (strcmp(name, "peer-type") == 0) return ELEM_VXLAN_PEER_TYPE;
	if (strcmp(name, "remote-dst") == 0) return ELEM_VXLAN_REMOTE;
	if (strcmp(name, "remote-dst-port") == 0) return ELEM_VXLAN_REMOTE_PORT;
	if (strcmp(name, "learning-status") == 0) return ELEM_VXLAN_LEARNING;
	if (strcmp(name, "port-min") == 0) return ELEM_VXLAN_PORT_MIN;
	if (strcmp(name, "port-max") == 0) return ELEM_VXLAN_PORT_MAX;
	if (strcmp(name, "ttl") == 0) return ELEM_VXLAN_TTL;
	if (strcmp(name, "ftable-max") == 0) return ELEM_VXLAN_FTABLE_MAX;
	if (strcmp(name, "ftable-timeout") == 0) return ELEM_VXLAN_FTABLE_TIMEOUT;
	if (strcmp(name, "syncdev") == 0) return ELEM_PFSYNC_SYNCDEV;
	if (strcmp(name, "syncpeer-str") == 0) return ELEM_PFSYNC_SYNCPEER;
	if (strcmp(name, "maxupdates") == 0) return ELEM_PFSYNC_MAXUPDATES;
	if (strcmp(name, "defer-status") == 0) return ELEM_PFSYNC_DEFER;
	if (strcmp(name, "pfsync-version") == 0) return ELEM_PFSYNC_VERSION;
	if (strcmp(name, "carp-state") == 0) return ELEM_CARP_STATE;
	if (strcmp(name, "vhid") == 0) return ELEM_CARP_VHID;
	if (strcmp(name, "advbase") == 0) return ELEM_CARP_ADVBASE;
	if (strcmp(name, "advskew") == 0) return ELEM_CARP_ADVSKEW;
	if (strcmp(name, "carp_key") == 0) return ELEM_CARP_KEY;
	if (strcmp(name, "carp_peer") == 0) return ELEM_CARP_PEER;
	if (strcmp(name, "carp_peer6") == 0) return ELEM_CARP_PEER6;
	if (strcmp(name, "vrrp_state") == 0) return ELEM_VRRP_STATE;
	if (strcmp(name, "vrid") == 0) return ELEM_VRRP_VRID;
	if (strcmp(name, "vrrp_prio") == 0) return ELEM_VRRP_PRIO;
	if (strcmp(name, "vrrp_interval") == 0) return ELEM_VRRP_INTERVAL;
	if (strcmp(name, "u") == 0) return ELEM_GRE_KEY;
	if (strcmp(name, "udpport") == 0) return ELEM_GRE_UDPPORT;
	if (strcmp(name, "flags6") == 0) return ELEM_FLAGS6;
	if (strcmp(name, "defuntagged") == 0) return ELEM_BRIDGE_DEFUNTAGGED;
	if (strcmp(name, "ifmaxaddr") == 0) return ELEM_MEMBER_IFMAXADDR;
	if (strcmp(name, "untagged") == 0) return ELEM_MEMBER_UNTAGGED;
	if (strcmp(name, "vlan") == 0) return ELEM_MEMBER_VLAN;
	if (strcmp(name, "vlan-id") == 0) return ELEM_MEMBER_VLAN_ID;
	if (strcmp(name, "vlan-end") == 0) return ELEM_MEMBER_VLAN_END;
	if (strcmp(name, "flowid-shift") == 0) return ELEM_LAGG_FLOWID_SHIFT;
	if (strcmp(name, "rr-limit") == 0) return ELEM_LAGG_RR_LIMIT;
	if (strcmp(name, "pcp") == 0) return ELEM_LINK_PCP;
	if (strcmp(name, "wme-aci") == 0) return ELEM_WME_ACI;
	if (strcmp(name, "cwmin") == 0) return ELEM_WME_CWMIN;
	if (strcmp(name, "cwmax") == 0) return ELEM_WME_CWMAX;
	if (strcmp(name, "aifs") == 0) return ELEM_WME_AIFS;
	if (strcmp(name, "txop-limit") == 0) return ELEM_WME_TXOPLIMIT;
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
	st->text[0] = '\0';
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
		st->text[st->text_len] = '\0';
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
	cfg->ifname[0] = '\0';
	cfg->ifname_set = false;
	cfg->argc = 0;
	cfg->inet_addr[0] = '\0';
	cfg->inet_dst[0] = '\0';
	cfg->inet_netmask[0] = '\0';
	cfg->inet_broadcast[0] = '\0';
	cfg->inet6_addr[0] = '\0';
	cfg->inet6_dst[0] = '\0';
	cfg->inet6_prefixlen[0] = '\0';
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
	if (strcmp(name, "UP") == 0) return "up";
	if (strcmp(name, "DEBUG") == 0) return "debug";
	if (strcmp(name, "PROMISC") == 0) return "promisc";
	if (strcmp(name, "ALLMULTI") == 0) return "allmulti";
	if (strcmp(name, "MONITOR") == 0) return "monitor";
	if (strcmp(name, "STATICARP") == 0) return "staticarp";
	if (strcmp(name, "STICKYARP") == 0) return "stickyarp";
	if (strcmp(name, "LINK0") == 0) return "link0";
	if (strcmp(name, "LINK1") == 0) return "link1";
	if (strcmp(name, "LINK2") == 0) return "link2";
	if (strcmp(name, "NOARP") == 0) return "arp";
	if (strcmp(name, "PPROMISC") == 0) return "promisc";
	return NULL;
}

static const char *
cap_name_to_cmd(const char *name)
{
	if (strcmp(name, "RXCSUM") == 0) return "rxcsum";
	if (strcmp(name, "TXCSUM") == 0) return "txcsum";
	if (strcmp(name, "RXCSUM_IPV6") == 0) return "rxcsum6";
	if (strcmp(name, "TXCSUM_IPV6") == 0) return "txcsum6";
	if (strcmp(name, "NETCONS") == 0) return "netcons";
	if (strcmp(name, "POLLING") == 0) return "polling";
	if (strcmp(name, "TSO4") == 0) return "tso4";
	if (strcmp(name, "TSO6") == 0) return "tso6";
	if (strcmp(name, "LRO") == 0) return "lro";
	if (strcmp(name, "WOL_UCAST") == 0) return "wol_ucast";
	if (strcmp(name, "WOL_MCAST") == 0) return "wol_mcast";
	if (strcmp(name, "WOL_MAGIC") == 0) return "wol_magic";
	if (strcmp(name, "TXRTLMT") == 0) return "txrtlmt";
	if (strcmp(name, "HWRXTSTMP") == 0) return "hwrxtstmp";
	if (strcmp(name, "MEXTPG") == 0) return "mextpg";
	if (strcmp(name, "VLAN_MTU") == 0) return "vlanmtu";
	if (strcmp(name, "VLAN_HWTAGGING") == 0) return "vlanhwtag";
	if (strcmp(name, "VLAN_HWFILTER") == 0) return "vlanhwfilter";
	if (strcmp(name, "VLAN_HWTSO") == 0) return "vlanhwtso";
	if (strcmp(name, "VLAN_HWCSUM") == 0) return "vlanhwcsum";
	if (strcmp(name, "TXTLS4") == 0) return "txtls";
	if (strcmp(name, "TXTLS6") == 0) return "txtls";
	if (strcmp(name, "TXTLS_RTLMT") == 0) return "txtlsrtlmt";
	if (strcmp(name, "RXTLS4") == 0) return "rxtls";
	if (strcmp(name, "RXTLS6") == 0) return "rxtls";
	if (strcmp(name, "IPSEC") == 0) return "ipsec";
	if (strcmp(name, "TOE4") == 0) return "toe";
	if (strcmp(name, "TOE6") == 0) return "toe";
	return NULL;
}

static const char *
gre_opt_name_to_cmd(const char *name)
{
	if (strcmp(name, "ENABLE_CSUM") == 0) return "enable_csum";
	if (strcmp(name, "ENABLE_SEQ") == 0) return "enable_seq";
	if (strcmp(name, "UDPENCAP") == 0) return "udpencap";
	return NULL;
}

static const char *
gif_opt_name_to_cmd(const char *name)
{
	if (strcmp(name, "NOCLAMP") == 0) return "noclamp";
	if (strcmp(name, "IGNORE_SOURCE") == 0) return "ignore_source";
	return NULL;
}

static const char *
flags6_name_to_cmd(const char *name)
{
	if (strcmp(name, "anycast") == 0) return "anycast";
	if (strcmp(name, "tentative") == 0) return "tentative";
	if (strcmp(name, "deprecated") == 0) return "deprecated";
	if (strcmp(name, "autoconf") == 0) return "autoconf";
	if (strcmp(name, "prefer_source") == 0) return "prefer_source";
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
    const char *const *af_args, int af_argc,
    const struct afswtch *afp)
{
	struct ifconfig_args _args = {};
	struct ifconfig_context _ctx = {};
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
	ifconfig_ioctl(&_ctx, comboc > 0 && strcmp(combined[0], "create") == 0, afp);
}

static void
apply_one_iface(struct ifxml_cfg *cfg)
{
	uint32_t ifindex;
	int need_create = 0;

	if (!cfg->ifname_set || !NONEMPTY(cfg->ifname))
		return;

	ifindex = if_nametoindex(cfg->ifname);
	if (ifindex == 0)
		need_create = 1;

	if (need_create) {
		const char *p = cfg->ifname;
		if (strncmp(p, "epair", 5) == 0) {
			p += 5;
			while (*p >= '0' && *p <= '9')
				p++;
			if (*p == 'b' && *(p + 1) == '\0')
				need_create = 0;
		}
	}

	if (!need_create && cfg->argc == 0 &&
	    !NONEMPTY(cfg->inet_addr) &&
	    !NONEMPTY(cfg->inet6_addr))
		return;

	if (!NONEMPTY(cfg->inet_addr) && !NONEMPTY(cfg->inet6_addr)) {
		build_apply_argv(cfg, need_create, NULL, 0, NULL);
		return;
	}

	if (NONEMPTY(cfg->inet_addr)) {
		const char *af_extra[16];
		int aec = 0;
		af_extra[aec++] = cfg->inet_addr;
		if (NONEMPTY(cfg->inet_dst))
			af_extra[aec++] = cfg->inet_dst;
		if (NONEMPTY(cfg->inet_netmask)) {
			af_extra[aec++] = "netmask";
			af_extra[aec++] = cfg->inet_netmask;
		}
		if (NONEMPTY(cfg->inet_broadcast)) {
			af_extra[aec++] = "broadcast";
			af_extra[aec++] = cfg->inet_broadcast;
		}
		build_apply_argv(cfg, need_create, af_extra, aec, NULL);
		need_create = 0;
	}

	if (NONEMPTY(cfg->inet6_addr)) {
		const char *af_extra[16];
		int aec = 0;
		af_extra[aec++] = cfg->inet6_addr;
		if (NONEMPTY(cfg->inet6_dst))
			af_extra[aec++] = cfg->inet6_dst;
		if (NONEMPTY(cfg->inet6_prefixlen)) {
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
	tmp = realloc(st->all_cfgs, (st->cfg_count + 1) * sizeof(struct ifxml_cfg));
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
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "mtu", txt);
		break;
	case ELEM_METRIC:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "metric", txt);
		break;
	case ELEM_DESCRIPTION:
	case ELEM_DESCR:
		if (NONEMPTY(txt))
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

		if (par == id && NONEMPTY(txt)) {
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
		    ELEMINNEREQ(st->addr_type, "ether") &&
		    NONEMPTY(st->link_addr)) {
			cfg_add_arg(&st->cfg, "ether");
			cfg_add_arg(&st->cfg, strdup(st->link_addr));
		}
		break;
	case ELEM_GROUP_NAME:
		parent = cur_elem(st);
		if (parent == ELEM_GROUP && NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "group", txt);
		else if (parent == ELEM_LAGG_PORT && NONEMPTY(txt) &&
		    st->lagg.num_ports < MAX_LAGG_PORTS)
			strlcpy(st->lagg.ports[st->lagg.num_ports].name, txt,
			    sizeof(st->lagg.ports[st->lagg.num_ports].name));
		else if (parent == ELEM_WME_ACI && NONEMPTY(txt))
			set_str(&st->wme_aci_name, txt);
		break;
	case ELEM_MEDIA_TYPE:
		st->media_capture = 1;
		strlcpy(st->media_type, txt, sizeof(st->media_type));
		break;
	case ELEM_MEDIA_SUBTYPE:
		if (st->media_capture)
			strlcpy(st->media_subtype, txt, sizeof(st->media_subtype));
		break;
	case ELEM_MEDIA_MODE:
		if (st->media_capture && NONEMPTY(txt))
			set_str(&st->media_mode, txt);
		break;
	case ELEM_MEDIA_OPTION:
		if (st->media_capture && NONEMPTY(txt))
			buf_append(&st->media_option, ",", txt);
		break;
	case ELEM_MEDIA:
		break;
	case ELEM_INET_ADDR:
		if (st->in_addr_block)
			strlcpy(st->addr.inet_addr, txt, sizeof(st->addr.inet_addr));
		break;
	case ELEM_INET6_ADDR:
		if (st->in_addr_block)
			strlcpy(st->addr.inet6_addr, txt, sizeof(st->addr.inet6_addr));
		break;
	case ELEM_DST_ADDR:
		if (st->in_addr_block)
			strlcpy(st->addr.dst_addr, txt, sizeof(st->addr.dst_addr));
		break;
	case ELEM_NETMASK:
		if (st->in_addr_block)
			strlcpy(st->addr.netmask, txt, sizeof(st->addr.netmask));
		break;
	case ELEM_BROADCAST:
		if (st->in_addr_block)
			strlcpy(st->addr.broadcast, txt, sizeof(st->addr.broadcast));
		break;
	case ELEM_PREFIXLEN:
		if (st->in_addr_block)
			strlcpy(st->addr.prefixlen, txt, sizeof(st->addr.prefixlen));
		break;
	case ELEM_ADDRESS_CONTAINER:
		st->in_addr_block = 0;
		if (NONEMPTY(st->addr.inet_addr)) {
			strlcpy(st->cfg.inet_addr, st->addr.inet_addr,
			    sizeof(st->cfg.inet_addr));
			strlcpy(st->cfg.inet_dst, st->addr.dst_addr,
			    sizeof(st->cfg.inet_dst));
			strlcpy(st->cfg.inet_netmask, st->addr.netmask,
			    sizeof(st->cfg.inet_netmask));
			strlcpy(st->cfg.inet_broadcast, st->addr.broadcast,
			    sizeof(st->cfg.inet_broadcast));
		} else if (NONEMPTY(st->addr.inet6_addr)) {
			strlcpy(st->cfg.inet6_addr, st->addr.inet6_addr,
			    sizeof(st->cfg.inet6_addr));
			strlcpy(st->cfg.inet6_dst, st->addr.dst_addr,
			    sizeof(st->cfg.inet6_dst));
			strlcpy(st->cfg.inet6_prefixlen, st->addr.prefixlen,
			    sizeof(st->cfg.inet6_prefixlen));
		}
		break;
	case ELEM_FIB:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "fib", txt);
		break;
	case ELEM_TUNNEL_FIB:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "tunnelfib", txt);
		break;
	case ELEM_BRIDGE_PRIORITY:
		strlcpy(st->bridge.priority, txt, sizeof(st->bridge.priority));
		break;
	case ELEM_HELLOTIME:
		strlcpy(st->bridge.hellotime, txt, sizeof(st->bridge.hellotime));
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
			strlcpy(st->bridge.stp_proto, txt, sizeof(st->bridge.stp_proto));
		break;
	case ELEM_MAXADDR:
		strlcpy(st->bridge.maxaddr, txt, sizeof(st->bridge.maxaddr));
		break;
	case ELEM_TIMEOUT:
		strlcpy(st->bridge.timeout, txt, sizeof(st->bridge.timeout));
		break;
	case ELEM_MEMBER_NAME:
		if (st->in_member)
			strlcpy(st->cur_member.name, txt, sizeof(st->cur_member.name));
		break;
	case ELEM_PORT_PRIORITY:
		if (st->in_member)
			strlcpy(st->cur_member.priority, txt, sizeof(st->cur_member.priority));
		break;
	case ELEM_PATH_COST:
		if (st->in_member)
			strlcpy(st->cur_member.path_cost, txt, sizeof(st->cur_member.path_cost));
		break;
	case ELEM_VLAN_PROTO:
		if (st->in_member)
			strlcpy(st->cur_member.vlan_proto, txt, sizeof(st->cur_member.vlan_proto));
		break;
	case ELEM_MEMBER:
		if (st->in_bridge && NONEMPTY(st->cur_member.name) &&
		    st->bridge.num_members < MAX_BRIDGE_MEMBERS) {
			st->bridge.members[st->bridge.num_members++] = st->cur_member;
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
		if (NONEMPTY(st->bridge.priority))
			cfg_add_arg_kv(&st->cfg, "priority", st->bridge.priority);
		if (NONEMPTY(st->bridge.hellotime))
			cfg_add_arg_kv(&st->cfg, "hellotime", st->bridge.hellotime);
		if (NONEMPTY(st->bridge.fwddelay))
			cfg_add_arg_kv(&st->cfg, "fwddelay", st->bridge.fwddelay);
		if (NONEMPTY(st->bridge.maxage))
			cfg_add_arg_kv(&st->cfg, "maxage", st->bridge.maxage);
		if (NONEMPTY(st->bridge.holdcnt))
			cfg_add_arg_kv(&st->cfg, "holdcnt", st->bridge.holdcnt);
		if (NONEMPTY(st->bridge.stp_proto))
			cfg_add_arg_kv(&st->cfg, "proto", st->bridge.stp_proto);
		if (NONEMPTY(st->bridge.maxaddr))
			cfg_add_arg_kv(&st->cfg, "maxaddr", st->bridge.maxaddr);
		if (NONEMPTY(st->bridge.timeout))
			cfg_add_arg_kv(&st->cfg, "timeout", st->bridge.timeout);
		if (NONEMPTY(st->bridge.defuntagged))
			cfg_add_arg_kv(&st->cfg, "defuntagged", st->bridge.defuntagged);
		for (int i = 0; i < st->bridge.num_members; i++) {
			struct ifxml_bridge_member *m = &st->bridge.members[i];

			cfg_add_arg_kv(&st->cfg, "addm", m->name);
		}
		for (int i = 0; i < st->bridge.num_members; i++) {
			struct ifxml_bridge_member *m = &st->bridge.members[i];

			if (NONEMPTY(m->priority)) {
				cfg_add_arg(&st->cfg, "ifpriority");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->priority));
			}
			if (NONEMPTY(m->path_cost)) {
				cfg_add_arg(&st->cfg, "ifpathcost");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->path_cost));
			}
			if (NONEMPTY(m->ifmaxaddr)) {
				cfg_add_arg(&st->cfg, "ifmaxaddr");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->ifmaxaddr));
			}
			if (NONEMPTY(m->untagged)) {
				cfg_add_arg(&st->cfg, "ifuntagged");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->untagged));
			}
			if (NONEMPTY(m->vlan_tagged)) {
				cfg_add_arg(&st->cfg, "iftagged");
				cfg_add_arg(&st->cfg, strdup(m->name));
				cfg_add_arg(&st->cfg, strdup(m->vlan_tagged));
			}
			if (NONEMPTY(m->vlan_proto) &&
			    (ELEMINNEREQ(m->vlan_proto, "802.1q") ||
			    ELEMINNEREQ(m->vlan_proto, "802.1ad"))) {
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
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "vlan", txt);
		break;
	case ELEM_VLAN_PROTOCOL:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "vlanproto", txt);
		break;
	case ELEM_VLAN_PCP:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "vlanpcp", txt);
		break;
	case ELEM_PARENT_IFNAME:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "vlandev", txt);
		break;
	case ELEM_PARENT:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "wlandev", txt);
		break;
	case ELEM_VXLAN_VNI:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.vni, txt);
		break;
	case ELEM_VXLAN_LOCAL:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.local, txt);
		break;
	case ELEM_VXLAN_LOCAL_PORT:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.local_port, txt);
		break;
	case ELEM_VXLAN_PEER_TYPE:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.peer_type, txt);
		break;
	case ELEM_VXLAN_REMOTE:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.remote, txt);
		break;
	case ELEM_VXLAN_REMOTE_PORT:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.remote_port, txt);
		break;
	case ELEM_VXLAN_LEARNING:
		st->vxlan.learning_seen = 1;
		st->vxlan.learning_on = !ELEMINNEREQ(txt, "no");
		break;
	case ELEM_VXLAN_PORT_MIN:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.port_min, txt);
		break;
	case ELEM_VXLAN_PORT_MAX:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.port_max, txt);
		break;
	case ELEM_VXLAN_TTL:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.ttl, txt);
		break;
	case ELEM_VXLAN_FTABLE_MAX:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.ftable_max, txt);
		break;
	case ELEM_VXLAN_FTABLE_TIMEOUT:
		if (NONEMPTY(txt))
			set_str(&st->vxlan.ftable_timeout, txt);
		break;
	case ELEM_PFSYNC_SYNCDEV:
		if (NONEMPTY(txt))
			set_str(&st->pfsync.syncdev, txt);
		break;
	case ELEM_PFSYNC_SYNCPEER:
		if (NONEMPTY(txt))
			set_str(&st->pfsync.syncpeer, txt);
		break;
	case ELEM_PFSYNC_MAXUPDATES:
		if (NONEMPTY(txt))
			set_str(&st->pfsync.maxupdates, txt);
		break;
	case ELEM_PFSYNC_DEFER:
		st->pfsync.defer_seen = 1;
		st->pfsync.defer_on = ELEMINNEREQ(txt, "on");
		break;
	case ELEM_PFSYNC_VERSION:
		if (NONEMPTY(txt))
			set_str(&st->pfsync.version, txt);
		break;
	case ELEM_CARP_STATE:
	case ELEM_VRRP_STATE:
		if (NONEMPTY(txt))
			set_str(&st->carp_state, txt);
		break;
	case ELEM_CARP_VHID:
	case ELEM_VRRP_VRID:
		if (st->in_addr_block) {
			free(st->carp_state);
			st->carp_state = NULL;
			break;
		}
		if (NONEMPTY(txt)) {
			cfg_add_arg_kv(&st->cfg, "vhid", txt);
			if (NONEMPTY(st->carp_state)) {
				cfg_add_arg_kv(&st->cfg, "state", st->carp_state);
				free(st->carp_state);
				st->carp_state = NULL;
			}
		}
		break;
	case ELEM_CARP_ADVBASE:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "advbase", txt);
		break;
	case ELEM_CARP_ADVSKEW:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "advskew", txt);
		break;
	case ELEM_CARP_KEY:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "pass", txt);
		break;
	case ELEM_CARP_PEER:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "peer", txt);
		break;
	case ELEM_CARP_PEER6:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "peer6", txt);
		break;
	case ELEM_VRRP_PRIO:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "vrrpprio", txt);
		break;
	case ELEM_VRRP_INTERVAL:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "vrrpinterval", txt);
		break;
	case ELEM_GRE_KEY:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "grekey", txt);
		break;
	case ELEM_GRE_UDPPORT:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "udpport", txt);
		break;
	case ELEM_FLAGS6: {
		const char *cmd;
		int par = cur_elem(st);

		if (par == id && NONEMPTY(txt)) {
			cmd = flags6_name_to_cmd(txt);
			if (cmd != NULL)
				cfg_add_arg(&st->cfg, cmd);
		}
		break;
	}
	case ELEM_BRIDGE_DEFUNTAGGED:
		if (NONEMPTY(txt))
			set_str(&st->bridge.defuntagged, txt);
		break;
	case ELEM_MEMBER_IFMAXADDR:
		if (st->in_member && NONEMPTY(txt))
			set_str(&st->cur_member.ifmaxaddr, txt);
		break;
	case ELEM_MEMBER_UNTAGGED:
		if (st->in_member && NONEMPTY(txt))
			set_str(&st->cur_member.untagged, txt);
		break;
	case ELEM_MEMBER_VLAN:
		break;
	case ELEM_MEMBER_VLAN_ID:
		if (st->in_member && NONEMPTY(txt))
			buf_append(&st->cur_member.vlan_tagged, ",", txt);
		break;
	case ELEM_MEMBER_VLAN_END:
		if (st->in_member && NONEMPTY(txt))
			buf_append(&st->cur_member.vlan_tagged, "-", txt);
		break;
	case ELEM_LAGG_FLOWID_SHIFT:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "flowid_shift", txt);
		break;
	case ELEM_LAGG_RR_LIMIT:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "rr_limit", txt);
		break;
	case ELEM_LINK_PCP:
		if (NONEMPTY(txt))
			cfg_add_arg_kv(&st->cfg, "pcp", txt);
		break;
	case ELEM_WLAN:
		if (NONEMPTY(txt)) {
			if (strcmp(name, "bssid") == 0)
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
		if (NONEMPTY(st->lagg.ports[st->lagg.num_ports].name))
			st->lagg.num_ports++;
		break;
	case ELEM_INTERFACE:
		if (NONEMPTY(st->lagg.proto))
			cfg_add_arg_kv(&st->cfg, "laggproto", st->lagg.proto);
		if (st->lagg_hash_l2 || st->lagg_hash_l3 || st->lagg_hash_l4) {
			char hashbuf[32];
			hashbuf[0] = '\0';
			if (st->lagg_hash_l2)
				strlcat(hashbuf, "l2", sizeof(hashbuf));
			if (st->lagg_hash_l3) {
				if (NONEMPTY(hashbuf))
					strlcat(hashbuf, ",", sizeof(hashbuf));
				strlcat(hashbuf, "l3", sizeof(hashbuf));
			}
			if (st->lagg_hash_l4) {
				if (NONEMPTY(hashbuf))
					strlcat(hashbuf, ",", sizeof(hashbuf));
				strlcat(hashbuf, "l4", sizeof(hashbuf));
			}
			if (NONEMPTY(hashbuf))
				cfg_add_arg_kv(&st->cfg, "lagghash", hashbuf);
		}
		for (int i = 0; i < st->lagg.num_ports; i++)
			cfg_add_arg_kv(&st->cfg, "laggport", st->lagg.ports[i].name);
		if (NONEMPTY(st->media_subtype)) {
			cfg_add_arg(&st->cfg, "media");
			cfg_add_arg(&st->cfg, strdup(st->media_subtype));
		}
		if (NONEMPTY(st->media_mode))
			cfg_add_arg_kv(&st->cfg, "mode", st->media_mode);
		if (NONEMPTY(st->media_option))
			cfg_add_arg_kv(&st->cfg, "mediaopt", st->media_option);
		if (NONEMPTY(st->tunnel_src) || NONEMPTY(st->tunnel_dst)) {
			cfg_add_arg(&st->cfg, "tunnel");
			cfg_add_arg(&st->cfg, strdup(
			    NONEMPTY(st->tunnel_src) ? st->tunnel_src : "0.0.0.0"));
			cfg_add_arg(&st->cfg, strdup(
			    NONEMPTY(st->tunnel_dst) ? st->tunnel_dst : "0.0.0.0"));
		}
		if (NONEMPTY(st->vxlan.vni))
			cfg_add_arg_kv(&st->cfg, "vni", st->vxlan.vni);
		if (NONEMPTY(st->vxlan.local))
			cfg_add_arg_kv(&st->cfg, "vxlanlocal", st->vxlan.local);
		if (NONEMPTY(st->vxlan.local_port))
			cfg_add_arg_kv(&st->cfg, "vxlanlocalport", st->vxlan.local_port);
		if (NONEMPTY(st->vxlan.remote)) {
			if (NONEMPTY(st->vxlan.peer_type) &&
			    ELEMINNEREQ(st->vxlan.peer_type, "group"))
				cfg_add_arg_kv(&st->cfg, "vxlangroup", st->vxlan.remote);
			else
				cfg_add_arg_kv(&st->cfg, "vxlanremote", st->vxlan.remote);
		}
		if (NONEMPTY(st->vxlan.remote_port))
			cfg_add_arg_kv(&st->cfg, "vxlanremoteport", st->vxlan.remote_port);
		if (st->vxlan.learning_seen)
			cfg_add_arg(&st->cfg, st->vxlan.learning_on ?
			    "vxlanlearn" : "-vxlanlearn");
		if (NONEMPTY(st->vxlan.port_min) && NONEMPTY(st->vxlan.port_max)) {
			cfg_add_arg(&st->cfg, "vxlanportrange");
			cfg_add_arg(&st->cfg, strdup(st->vxlan.port_min));
			cfg_add_arg(&st->cfg, strdup(st->vxlan.port_max));
		}
		if (NONEMPTY(st->vxlan.ttl))
			cfg_add_arg_kv(&st->cfg, "vxlanttl", st->vxlan.ttl);
		if (NONEMPTY(st->vxlan.ftable_max))
			cfg_add_arg_kv(&st->cfg, "vxlanmaxaddr", st->vxlan.ftable_max);
		if (NONEMPTY(st->vxlan.ftable_timeout))
			cfg_add_arg_kv(&st->cfg, "vxlantimeout", st->vxlan.ftable_timeout);
		if (NONEMPTY(st->pfsync.syncdev))
			cfg_add_arg_kv(&st->cfg, "syncdev", st->pfsync.syncdev);
		if (NONEMPTY(st->pfsync.syncpeer))
			cfg_add_arg_kv(&st->cfg, "syncpeer", st->pfsync.syncpeer);
		if (NONEMPTY(st->pfsync.maxupdates))
			cfg_add_arg_kv(&st->cfg, "maxupd", st->pfsync.maxupdates);
		if (st->pfsync.defer_seen)
			cfg_add_arg(&st->cfg, st->pfsync.defer_on ? "defer" : "-defer");
		if (NONEMPTY(st->pfsync.version))
			cfg_add_arg_kv(&st->cfg, "version", st->pfsync.version);
		if (NONEMPTY(st->wlan_chan_num))
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
			warnx("XML parse error in %s at line %lu: %s",
			    filename,
			    (unsigned long)XML_GetCurrentLineNumber(st.parser),
			    XML_ErrorString(xerr));
			break;
		}
	}
	xstatus = XML_Parse(st.parser, buf, 0, 1);
	if (xstatus == XML_STATUS_ERROR) {
		enum XML_Error xerr = XML_GetErrorCode(st.parser);
		warnx("XML final parse error in %s: %s",
		    filename, XML_ErrorString(xerr));
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
