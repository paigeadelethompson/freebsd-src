/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <net/ieee8023ad_lacp.h>
#include <net/if_lagg.h>
#include <net/if_stf.h>
#include <net/if_vlan_var.h>
#include <net/if_vxlan.h>

#include <libifconfig_sfp.h>

#include "ifgeneve.h"
#include "ifieee80211.h"

static char
    ifconfig_ifname_to_print[IFNAMSIZ]; /* Helper for
					   ifconfig_printifnamemaybe() */

#define IFIEEE80211_MAXCOL 78
static int ifieee80211_col;
static char ifieee80211_spacer;

void af_inet_print_addr(struct sockaddr_in *sin);
char *af_inet6_sec2str(time_t);
void af_inet6_print_addr(struct sockaddr_in6 *sin);
void af_inet6_print_pointtopoint(struct sockaddr_in6 *sin);
void af_inet6_print_mask(int plen);
void af_inet6_print_flags(int flags6);
void af_inet6_print_lifetime(const char *prepend, time_t px_time,
    struct timespec *now);

void af_link_print_ether(const struct ether_addr *addr, const char *prefix);
void af_link_print_lladdr(struct sockaddr_dl *sdl);
void af_link_print_pcp(if_ctx *ctx);
void ifbridge_print_vlans(ifbvlan_set_t *vlans);
void ifconfig_printifnamemaybe(void);
int ifconfig_parse_args(int ac, char *av[]);
void ifconfig_finish(void);
void ifconfig_open_container(const char *name);
void ifconfig_close_container(const char *name);
void ifconfig_open_list(const char *name);
void ifconfig_close_list(const char *name);
void ifconfig_open_instance(const char *name);
void ifconfig_close_instance(const char *name);
void ifconfig_print_ifcap_nv(if_ctx *ctx);
void ifconfig_print_ifcap(if_ctx *ctx);
void ifconfig_print_ifstatus(if_ctx *ctx);
void ifconfig_print_metric(if_ctx *ctx);
void ifconfig_print_mtu(if_ctx *ctx);
void ifconfig_print_description(if_ctx *ctx);
void ifconfig_print_bits(const char *btype, const char *child, uint32_t *v,
    const int v_count, const char **names, const int n_count);

void ifconfig_printb(const char *s, unsigned v, const char *bits);
void ifconfig_print_vhid(const struct ifaddrs *ifa);
void ifconfig_netlink_print_ifcaps(if_ctx *ctx, if_link_t *link);
void ifgroup_printgroup(const char *groupname);
void ifieee80211_line_init(char c);
void ifieee80211_line_break(void);
void ifieee80211_line_check(const char *fmt, ...);
void ifieee80211_print_chaninfo(const struct ieee80211_channel *c, int verb);
void ifieee80211_print_channels(if_ctx *ctx,
    const struct ieee80211req_chaninfo *chans, int allchans, int verb);

void ifieee80211_print_txpow(const struct ieee80211_channel *c);
void ifieee80211_print_txpow_verbose(const struct ieee80211_channel *c);
void ifieee80211_print_regdomain(const struct ieee80211_regdomain *reg,
    int verb);

void ifieee80211_printcipher(int s, struct ieee80211req *ireq, int keylenop);
void ifieee80211_printkey_index(uint16_t keyix, char *buf, size_t buflen);
void ifieee80211_printkey(if_ctx *ctx, const struct ieee80211req_key *ik);
void ifieee80211_printrate(const char *tag, int v, int defrate, int defmcs);
void ifieee80211_print_string(const u_int8_t *buf, int len);
void ifieee80211_printie(if_ctx *ctx, const char *tag, const uint8_t *ie,
    size_t ielen, unsigned int maxlen);

void ifieee80211_printwmeparam(if_ctx *ctx, const char *tag,
    const u_int8_t *ie);

void ifieee80211_printwmeinfo(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printhecap(if_ctx *ctx, const char *tag, const uint8_t *ie);
void ifieee80211_printheoper(if_ctx *ctx, const char *tag, const uint8_t *ie);
void ifieee80211_printmuedcaparamset(if_ctx *ctx, const char *tag,
    const uint8_t *ie);

void ifieee80211_printsupopclass(if_ctx *ctx, const char *tag,
    const u_int8_t *ie);

void ifieee80211_printvhtcap(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printvhtinfo(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printvhtpwrenv(if_ctx *ctx, const char *tag,
    const u_int8_t *ie, size_t ielen);

void ifieee80211_printhtcap(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printhtinfo(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printathie(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printmeshconf(if_ctx *ctx, const char *tag, const uint8_t *ie);
void ifieee80211_printbssload(if_ctx *ctx, const char *tag, const uint8_t *ie);
void ifieee80211_printapchanrep(if_ctx *ctx, const char *tag,
    const u_int8_t *ie, size_t ielen);

void ifieee80211_printwpaie(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printrsnie(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen);

void ifieee80211_printrsnxe(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen);

void ifieee80211_printwpsie(if_ctx *ctx, const char *tag, const u_int8_t *ie);
void ifieee80211_printtdmaie(if_ctx *ctx, const char *tag, const u_int8_t *ie,
    size_t ielen);

void ifieee80211_printssid(const char *tag, const u_int8_t *ie, int maxlen);
void ifieee80211_printrates(const char *tag, const u_int8_t *ie, size_t ielen);
void ifieee80211_printcountry(const char *tag, const u_int8_t *ie);
void ifieee80211_printexties(if_ctx *ctx, const u_int8_t *vp,
    unsigned int maxcols);

void ifieee80211_printies(if_ctx *ctx, const u_int8_t *vp, int ielen,
    unsigned int maxcols);

void ifieee80211_printmimo(const struct ieee80211_mimo_info *mi);
void ifieee80211_printbssidname(const struct ether_addr *n);
void ifieee80211_printpolicy(int policy);
void ifmedia_print_media(ifmedia_t media, bool print_toptype);
void ifmedia_print_media_ifconfig(ifmedia_t media);

void ifconfig_print_space(void);
void ifconfig_print_char(char c);
void ifconfig_print_tab(void);
void ifconfig_print_newline(void);

void ifconfig_print_flags(if_ctx *ctx, const struct ifaddrs *ifa);
void ifclone_print_cloners(const char *name);

void af_inet_print_addr_pointtopoint(struct sockaddr_in *sin);
void af_inet_print_cidr_mask(int cidr);
void af_inet_print_netmask_str(struct in_addr addr);
void af_inet_print_netmask(struct in_addr addr);
void af_inet_print_broadcast(struct sockaddr_in *sin);
void af_inet_print_vhid(if_addr_t *ifa);
void af_inet_print_tunnel_inet(const char *src, const char *dst);

void af_inet6_print_scopeid(uint32_t scopeid);
void af_inet6_print_vhid(if_addr_t *ifa);
void af_inet6_print_tunnel(const char *src, const char *dst);

void af_nd6_print_options(uint32_t bits);

void carp_print_summary(const char *state, struct ifconfig_carp *carpr);
void carp_print_key(struct ifconfig_carp *carpr);
void carp_print_peer(struct ifconfig_carp *carpr, const char *peer_addr);
void carp_print_vrrp3(const char *state, struct ifconfig_carp *carpr);

void iffib_print_fib(struct ifreq *ifr);
void iffib_print_tunnelfib(struct ifreq *ifr);

void ifgif_print_options(int opts);

void ifgre_print_key(uint32_t opts);
void ifgre_print_udpport(uint32_t port);
void ifgre_print_options(uint32_t opts);

void ifgroup_print_groups(void);
void ifgroup_print_group(struct ifg_req *ifg);

void ifipsec_print_reqid(uint32_t reqid);

void iflagg_print_laggproto(const char *proto);
void iflagg_print_lagghash(void);
void iflagg_print_l2(const char *sep);
void iflagg_print_l3(const char *sep);
void iflagg_print_l4(const char *sep);
void iflagg_print_options(void);
void iflagg_print_flowid_shift(struct lagg_reqopts *ro);
void iflagg_print_trr_limit(struct lagg_reqopts *ro);
void iflagg_print_statistics(void);
void iflagg_print_active_ports(struct lagg_reqopts *ro);
void iflagg_print_flapping(struct lagg_reqopts *ro);
void iflagg_print_lagg_id(struct lacp_opreq *lp);
void iflagg_print_laggport(struct lagg_reqport *port);
void iflagg_print_peer(struct lacp_opreq *lp);

void ifmac_print_maclabel(const char *label_text);

void ifmedia_print_media_header(void);
void ifmedia_print_status(const char *status);
void ifmedia_print_reason(struct ifdownreason *ifdr);
void ifmedia_print_reason_vendor(struct ifdownreason *ifdr);
void ifmedia_print_supmedia(void);

void ifpfsync_print_syncdev(const char *syncdev);
void ifpfsync_print_syncpeer(const char *syncpeer_str);
void ifpfsync_print_maxupd(int maxupdates);
void ifpfsync_print_defer(int flags);
void ifpfsync_print_version(int version);
void ifpfsync_print_syncok(int flags);

void ifstf_print_v4net(struct stfv4args *param);
void ifstf_print_tv4br(struct stfv4args *param);

void ifvlan_print_vlan(struct vlanreq *vreq);
void ifvlan_print_vlanproto(void);
void ifvlan_print_8021q(const char *proto_8021Q);
void ifvlan_print_8021d(const char *proto_8021ad);
void ifvlan_print_otherproto(struct vlanreq *vreq);
void ifvlan_print_vlanpcp(struct ifreq *ifr);
void ifvlan_print_parent(struct vlanreq *vreq);

void ifvxlan_print_vni(int vni);
void ifvxlan_print_src(int ipv6, const char *src, const char *srcport);
void ifvxlan_print_dst(int mc, int ipv6, const char *dst, const char *dstport);
void ifvxlan_print_config(void);
void ifvxlan_print_portrange(struct ifvxlancfg *cfg);
void ifvxlan_print_ftable(void);
void ifvxlan_print_threshold(struct ifvxlancfg *cfg);

void ifconfig_netlink_print_ifname(if_link_t *link);
void ifconfig_netlink_print_flags(if_link_t *link);
void ifconfig_netlink_print_mtu(if_link_t *link);
void ifconfig_netlink_print_ifalias(if_link_t *link);
void ifconfig_netlink_print_drivername(const char *drivername);

void ifgeneve_print_mode(void);
void ifgeneve_print_proto_l3(void);
void ifgeneve_print_proto_l2(void);
void ifgeneve_print_config(void);
void ifgeneve_print_config_vni_notconfigured(void);
void ifgeneve_print_vni(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_src(int ipv6, const char *src,
    struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_dst(int mc, int ipv6, const char *dst,
    struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_mcastdev(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_portrange(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_ttl(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_ttl_inherit(void);
void ifgeneve_print_dscp(void);
void ifgeneve_print_df_inherit(void);
void ifgeneve_print_df_set(void);
void ifgeneve_print_df_unset(void);
void ifgeneve_print_extctl(void);
void ifgeneve_print_ftable_mode(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_ftable(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_ftable_nospace(struct nl_parsed_geneve *geneve_data);
void ifgeneve_print_stats(struct nl_parsed_geneve *geneve_data);

void ifbridge_print_vlan(const char *prefix, struct ifbareq *ifba);
void ifbridge_print_summary(struct ifconfig_bridge_status *bridge);
void ifbridge_print_defuntagged(struct ifconfig_bridge_status *bridge);
void ifbridge_print_bridge_member(const char *prefix, struct ifbreq *member);
void ifbridge_print_bridge_member_pad(const char *pad);
void ifbridge_print_bridge_member_maxaddr(struct ifbreq *member);
void ifbridge_print_bridge_member_stp(struct ifbreq *member);
void ifbridge_print_bridge_member_proto(int proto);
void ifbridge_print_bridge_member_proto_unknown(int proto);
void ifbridge_print_bridge_member_role(int role);
void ifbridge_print_bridge_member_role_unknown(int role);
void ifbridge_print_bridge_member_state(int state);
void ifbridge_print_bridge_member_state_unknown(int state);
void ifbridge_print_bridge_member_vlan_proto(struct ifbreq *member);
void ifbridge_print_bridge_member_pvid(struct ifbreq *member);

void sfp_print_plugged(struct ifconfig_sfp_info *info,
    struct ifconfig_sfp_info_strings *strings);
void sfp_print_vendor(struct ifconfig_sfp_vendor_info *vendor_info);
void sfp_print_verbose_compliance(struct ifconfig_sfp_info_strings *strings);
void sfp_print_verbose_class(struct ifconfig_sfp_info *info,
    struct ifconfig_sfp_info_strings *strings);
void sfp_print_verbose_length(struct ifconfig_sfp_info_strings *strings);
void sfp_print_verbose_tech(struct ifconfig_sfp_info_strings *strings);
void sfp_print_verbose_media(struct ifconfig_sfp_info_strings *strings);
void sfp_print_verbose_speed(struct ifconfig_sfp_info_strings *strings);
void sfp_print_verbose_nombitrate(struct ifconfig_sfp_status *status);
void sfp_print_verbose_voltage(struct ifconfig_sfp_status *status);
void sfp_print_verbose_rxpower(size_t chan, uint16_t rx, uint16_t tx);
void sfp_print_verbose_cmis_dump1(const void *buf, int len);
void sfp_print_verbose_cmis_dump2(const void *buf, int len);
void sfp_print_verbose_cmis_dump3(const void *buf, int len);
void sfp_print_verbose_sff8436_dump1(const void *buf, int len);
void sfp_print_verbose_sff8436_dump2(const void *buf, int len);
void sfp_print_verbose_sff8472_dump1(const void *buf);

void ifieee80211_print_drivercaps(struct ieee80211_devcaps_req *dc);
void ifieee80211_print_cryptocaps(struct ieee80211_devcaps_req *dc);
void ifieee80211_print_htcaps(struct ieee80211_devcaps_req *dc);
void ifieee80211_print_vhtcaps(struct ieee80211_devcaps_req *dc);
void ifieee80211_print_regdomain_addchans(const char *func);
void ifieee80211_print_verbose_vht20_skip(uint32_t freq);
void ifieee80211_print_verbose_vht40_skip(uint32_t freq);
void ifieee80211_print_verbose_vht80_skip(uint32_t freq);
void ifieee80211_print_verbose_vht160_skip(uint32_t freq);
void ifieee80211_print_verbose_vht80p80_skip(uint32_t freq);
void ifieee80211_print_verbose_ht20_skip(uint32_t freq);
void ifieee80211_print_verbose_ht40_skip(uint32_t freq);
void ifieee80211_print_verbose_freq_skip(uint32_t freq);
void ifieee80211_print_verbose_checkchan_notavail(void);
void ifieee80211_print_verbose_ecm_chan(uint32_t freq);
void ifieee80211_print_verbose_indoor_chan_skip(uint32_t freq);
void ifieee80211_print_verbose_outdoor_chan_skip(uint32_t freq);
void ifieee80211_print_verbose_chansep_need(int freq, int separation, int need);
void ifieee80211_print_verbose_chan_table_full(uint32_t freq);
void ifieee80211_print_verbose_add_freq(struct ieee80211req_chaninfo *ci,
    const struct ieee80211_channel *c);
void ifieee80211_print_verbose_maxregpower(const struct ieee80211_channel *c);
void ifieee80211_print_country_codes(void);
void ifieee80211_print_country_code(const struct country *cp, int i);
void ifieee80211_print_reg_domains(void);
void ifieee80211_print_reg_domain(const struct regdomain *dp, int i);
void ifieee80211_print_list_scan_hdr(void);
void ifieee80211_print_list_scan_row(const struct ieee80211req_scan_result *sr,
    const uint8_t *idp, int idlen);
void ifieee80211_print_list_stations_hdr(void);
void ifieee80211_print_list_stations_hdr2(void);
void ifieee80211_print_list_stations_row(
    const struct ieee80211req_sta_info *si);
void ifieee80211_print_list_stations_row2(
    const struct ieee80211req_sta_info *si);
void ifieee80211_print_list_wme_aci_tag(const char *tag);
void ifieee80211_print_list_wme_aci_cwmin(int val);
void ifieee80211_print_list_wme_aci_cwmax(int val);
void ifieee80211_print_list_wme_aci_aifs(int val);
void ifieee80211_print_list_wme_aci_txoplimit(int val);
void ifieee80211_print_list_wme_aci_acm_enabled(void);
void ifieee80211_print_list_wme_aci_acm_disabled(void);
void ifieee80211_print_list_wme_aci_ack_enabled(void);
void ifieee80211_print_list_wme_aci_ack_disabled(void);
void ifieee80211_print_list_mac_acl_loaded(void);
void ifieee80211_print_list_mac_unknown_policy(unsigned int policy);
void ifieee80211_print_list_mac_nacl(char c, struct ieee80211req_maclist *acl);
void ifieee80211_print_list_mesh_hdr(void);
void ifieee80211_print_list_mesh_row(const struct ieee80211req_mesh_route *rt);
void ifieee80211_print_list_mesh_row2(const struct ieee80211req_mesh_route *rt);
void ifieee80211_print_ieee80211_status_meshid(void);
void ifieee80211_print_ieee80211_status_ssid(void);
void ifieee80211_print_ieee80211_status_ssid_idx(int i);
void ifieee80211_print_ieee80211_status_channel(
    const struct ieee80211_channel *c);
void ifieee80211_print_ieee80211_status_channel_undef(void);
void ifieee80211_print_ieee80211_status_bssid(const uint8_t *bssid);
void ifieee80211_print_ieee80211_status_stationname(void);

void if_err(int eval, const char *fmt, ...) __attribute__((__noreturn__))
__printflike(2, 3);
void if_errc(int eval, int code, const char *fmt, ...)
    __attribute__((__noreturn__)) __printflike(3, 4);
void if_errx(int eval, const char *fmt, ...) __attribute__((__noreturn__))
__printflike(2, 3);
void if_warn(const char *fmt, ...) __printflike(1, 2);
void if_warnc(int cond, int code, const char *fmt, ...) __printflike(3, 4);
void if_warnx(const char *fmt, ...) __printflike(1, 2);
