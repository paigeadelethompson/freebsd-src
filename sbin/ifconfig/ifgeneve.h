#pragma once


#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/nv.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <netdb.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_strings.h>
#include <netinet/in.h>
#include <net/if_geneve.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>

#include <netlink/route/interface.h> // enum ifla_geneve_df
                                           
static struct geneve_params gnvp = {
        .ifla_proto             =       GENEVE_PROTO_ETHER,
};

struct nl_parsed_geneve {
	/* essential */
	uint32_t			ifla_vni;
	uint16_t			ifla_proto;
	struct sockaddr			*ifla_local;
	struct sockaddr			*ifla_remote;
	uint16_t			ifla_local_port;
	uint16_t			ifla_remote_port;

	/* optional */
	struct ifla_geneve_port_range	*ifla_port_range;
	enum ifla_geneve_df		ifla_df;
	uint8_t				ifla_ttl;
	bool				ifla_ttl_inherit;
	bool				ifla_dscp_inherit;
	bool				ifla_external;

	/* l2 specific */
	bool				ifla_ftable_learn;
	bool				ifla_ftable_flush;
	uint32_t			ifla_ftable_max;
	uint32_t			ifla_ftable_timeout;
	uint32_t			ifla_ftable_count;
	uint32_t			ifla_ftable_nospace;
	uint32_t			ifla_ftable_lock_upgrade_failed;

	/* multicast specific */
	char				*ifla_mc_ifname;
	uint32_t			ifla_mc_ifindex;

	/* csum info */
	uint64_t			ifla_stats_txcsum;
	uint64_t			ifla_stats_tso;
	uint64_t			ifla_stats_rxcsum;
};
