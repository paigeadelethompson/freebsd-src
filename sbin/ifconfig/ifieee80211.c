/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright 2001 The Aerospace Corporation.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of The Aerospace Corporation may not be used to endorse or
 *    promote products derived from this software.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AEROSPACE CORPORATION ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AEROSPACE CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * Copyright (c) 1997, 1998, 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe of the Numerical Aerospace Simulation Facility,
 * NASA Ames Research Center.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/route.h>

#define WANT_NET80211	1
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_superg.h>
#include <net80211/ieee80211_tdma.h>
#include <net80211/ieee80211_mesh.h>
#include <net80211/ieee80211_wps.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stddef.h>		/* NB: for offsetof */
#include <locale.h>
#include <langinfo.h>

#include <lib80211/lib80211_regdomain.h>
#include <lib80211/lib80211_ioctl.h>

#include "ifconfig.h"
#include "ifieee80211.h"
#include "ifconfig_output.h"

static void set80211(if_ctx *ctx, int type, int val, int len, void *data);
static int get80211len(if_ctx *ctx, int type, void *data, int len, int *plen);
static int get80211val(if_ctx *ctx, int type, int *val);

static const char *get_string(const char *val, const char *sep,
			      u_int8_t *buf, int *lenp);

static void regdomain_makechannels(if_ctx *, struct ieee80211_regdomain_req *,
				   const struct ieee80211_devcaps_req *);

static void
gethtconf(if_ctx *ctx)
{
	if (gothtconf)
		return;
	if (get80211val(ctx, IEEE80211_IOC_HTCONF, &htconf) < 0)
		if_warn("unable to get HT configuration information");
	gothtconf = 1;
}

/* VHT */
static int vhtconf = 0;
static int gotvhtconf = 0;

static void
getvhtconf(if_ctx *ctx)
{
	if (gotvhtconf)
		return;
	if (get80211val(ctx, IEEE80211_IOC_VHTCONF, &vhtconf) < 0)
		if_warn("unable to get VHT configuration information");
	gotvhtconf = 1;
}

/*
 * Collect channel info from the kernel.  We use this (mostly)
 * to handle mapping between frequency and IEEE channel number.
 */
void
getchaninfo(if_ctx *ctx)
{
	if (chaninfo != NULL)
		return;
	chaninfo = malloc(IEEE80211_CHANINFO_SIZE(MAXCHAN));
	if (chaninfo == NULL)
		if_errx(1, "no space for channel list");
	if (get80211(ctx, IEEE80211_IOC_CHANINFO, chaninfo,
	    IEEE80211_CHANINFO_SIZE(MAXCHAN)) < 0)
		if_err(1, "unable to get channel information");
	global_ifmr = ifmedia_getstate(ctx);
	gethtconf(ctx);
	getvhtconf(ctx);
}

struct regdata *
getregdata(void)
{
	static struct regdata *rdp = NULL;
	if (rdp == NULL) {
		rdp = lib80211_alloc_regdata();
		if (rdp == NULL)
			if_errx(-1, "missing or corrupted regdomain database");
	}
	return rdp;
}

/*
 * Given the channel at index i with attributes from,
 * check if there is a channel with attributes to in
 * the channel table.  With suitable attributes this
 * allows the caller to look for promotion; e.g. from
 * 11b > 11g.
 */
static int
canpromote(unsigned int i, uint32_t from, uint32_t to)
{
	const struct ieee80211_channel *fc = &chaninfo->ic_chans[i];
	u_int j;

	if ((fc->ic_flags & from) != from)
		return i;
	/* NB: quick check exploiting ordering of chans w/ same frequency */
	if (i+1 < chaninfo->ic_nchans &&
	    chaninfo->ic_chans[i+1].ic_freq == fc->ic_freq &&
	    (chaninfo->ic_chans[i+1].ic_flags & to) == to)
		return i+1;
	/* brute force search in case channel list is not ordered */
	for (j = 0; j < chaninfo->ic_nchans; j++) {
		const struct ieee80211_channel *tc = &chaninfo->ic_chans[j];
		if (j != i &&
		    tc->ic_freq == fc->ic_freq && (tc->ic_flags & to) == to)
		return j;
	}
	return i;
}

/*
 * Handle channel promotion.  When a channel is specified with
 * only a frequency we want to promote it to the ``best'' channel
 * available.  The channel list has separate entries for 11b, 11g,
 * 11a, and 11n[ga] channels so specifying a frequency w/o any
 * attributes requires we upgrade, e.g. from 11b -> 11g.  This
 * gets complicated when the channel is specified on the same
 * command line with a media request that constrains the available
 * channe list (e.g. mode 11a); we want to honor that to avoid
 * confusing behaviour.
 */
/*
 * XXX VHT
 */
static int
promote(unsigned int i)
{
	/*
	 * Query the current mode of the interface in case it's
	 * constrained (e.g. to 11a).  We must do this carefully
	 * as there may be a pending ifmedia request in which case
	 * asking the kernel will give us the wrong answer.  This
	 * is an unfortunate side-effect of the way ifconfig is
	 * structure for modularity (yech).
	 *
	 * NB: ifmr is actually setup in getchaninfo (above); we
	 *     assume it's called coincident with to this call so
	 *     we have a ``current setting''; otherwise we must pass
	 *     the socket descriptor down to here so we can make
	 *     the ifmedia_getstate call ourselves.
	 */
	int chanmode = global_ifmr != NULL ? IFM_MODE(global_ifmr->ifm_current) : IFM_AUTO;

	/* when ambiguous promote to ``best'' */
	/* NB: we abitrarily pick HT40+ over HT40- */
	if (chanmode != IFM_IEEE80211_11B)
		i = canpromote(i, IEEE80211_CHAN_B, IEEE80211_CHAN_G);
	if (chanmode != IFM_IEEE80211_11G && (htconf & 1)) {
		i = canpromote(i, IEEE80211_CHAN_G,
			IEEE80211_CHAN_G | IEEE80211_CHAN_HT20);
		if (htconf & 2) {
			i = canpromote(i, IEEE80211_CHAN_G,
				IEEE80211_CHAN_G | IEEE80211_CHAN_HT40D);
			i = canpromote(i, IEEE80211_CHAN_G,
				IEEE80211_CHAN_G | IEEE80211_CHAN_HT40U);
		}
	}
	if (chanmode != IFM_IEEE80211_11A && (htconf & 1)) {
		i = canpromote(i, IEEE80211_CHAN_A,
			IEEE80211_CHAN_A | IEEE80211_CHAN_HT20);
		if (htconf & 2) {
			i = canpromote(i, IEEE80211_CHAN_A,
				IEEE80211_CHAN_A | IEEE80211_CHAN_HT40D);
			i = canpromote(i, IEEE80211_CHAN_A,
				IEEE80211_CHAN_A | IEEE80211_CHAN_HT40U);
		}
	}
	return i;
}

static void
mapfreq(struct ieee80211_channel *chan, uint16_t freq, unsigned int flags)
{
	u_int i;

	for (i = 0; i < chaninfo->ic_nchans; i++) {
		const struct ieee80211_channel *c = &chaninfo->ic_chans[i];

		if (c->ic_freq == freq && (c->ic_flags & flags) == flags) {
			if (flags == 0) {
				/* when ambiguous promote to ``best'' */
				c = &chaninfo->ic_chans[promote(i)];
			}
			*chan = *c;
			return;
		}
	}
	if_errx(1, "unknown/undefined frequency %u/0x%x", freq, flags);
}

static void
mapchan(struct ieee80211_channel *chan, uint8_t ieee, unsigned int flags)
{
	u_int i;

	for (i = 0; i < chaninfo->ic_nchans; i++) {
		const struct ieee80211_channel *c = &chaninfo->ic_chans[i];

		if (c->ic_ieee == ieee && (c->ic_flags & flags) == flags) {
			if (flags == 0) {
				/* when ambiguous promote to ``best'' */
				c = &chaninfo->ic_chans[promote(i)];
			}
			*chan = *c;
			return;
		}
	}
	if_errx(1, "unknown/undefined channel number %d flags 0x%x", ieee, flags);
}

static const struct ieee80211_channel *
getcurchan(if_ctx *ctx)
{
	if (gotcurchan)
		return &curchan;
	if (get80211(ctx, IEEE80211_IOC_CURCHAN, &curchan, sizeof(curchan)) < 0) {
		int val;
		/* fall back to legacy ioctl */
		if (get80211val(ctx, IEEE80211_IOC_CHANNEL, &val) < 0)
			if_err(-1, "cannot figure out current channel");
		getchaninfo(ctx);
		mapchan(&curchan, val, 0);
	}
	gotcurchan = 1;
	return &curchan;
}

static enum ieee80211_phymode
chan2mode(const struct ieee80211_channel *c)
{
	if (IEEE80211_IS_CHAN_VHTA(c))
		return IEEE80211_MODE_VHT_5GHZ;
	if (IEEE80211_IS_CHAN_VHTG(c))
		return IEEE80211_MODE_VHT_2GHZ;
	if (IEEE80211_IS_CHAN_HTA(c))
		return IEEE80211_MODE_11NA;
	if (IEEE80211_IS_CHAN_HTG(c))
		return IEEE80211_MODE_11NG;
	if (IEEE80211_IS_CHAN_108A(c))
		return IEEE80211_MODE_TURBO_A;
	if (IEEE80211_IS_CHAN_108G(c))
		return IEEE80211_MODE_TURBO_G;
	if (IEEE80211_IS_CHAN_ST(c))
		return IEEE80211_MODE_STURBO_A;
	if (IEEE80211_IS_CHAN_FHSS(c))
		return IEEE80211_MODE_FH;
	if (IEEE80211_IS_CHAN_HALF(c))
		return IEEE80211_MODE_HALF;
	if (IEEE80211_IS_CHAN_QUARTER(c))
		return IEEE80211_MODE_QUARTER;
	if (IEEE80211_IS_CHAN_A(c))
		return IEEE80211_MODE_11A;
	if (IEEE80211_IS_CHAN_ANYG(c))
		return IEEE80211_MODE_11G;
	if (IEEE80211_IS_CHAN_B(c))
		return IEEE80211_MODE_11B;
	return IEEE80211_MODE_AUTO;
}

static void
getroam(if_ctx *ctx)
{
	if (gotroam)
		return;
	if (get80211(ctx, IEEE80211_IOC_ROAM,
	    &roamparams, sizeof(roamparams)) < 0)
		if_err(1, "unable to get roaming parameters");
	gotroam = 1;
}

static void
setroam_cb(if_ctx *ctx, void *arg)
{
	struct ieee80211_roamparams_req *roam = arg;
	set80211(ctx, IEEE80211_IOC_ROAM, 0, sizeof(*roam), roam);
}

static void
gettxparams(if_ctx *ctx)
{
	if (gottxparams)
		return;
	if (get80211(ctx, IEEE80211_IOC_TXPARAMS,
	    &txparams, sizeof(txparams)) < 0)
		if_err(1, "unable to get transmit parameters");
	gottxparams = 1;
}

static void
settxparams_cb(if_ctx *ctx, void *arg)
{
	struct ieee80211_txparams_req *txp = arg;
	set80211(ctx, IEEE80211_IOC_TXPARAMS, 0, sizeof(*txp), txp);
}

static void
getregdomain(if_ctx *ctx)
{
	if (gotregdomain)
		return;
	if (get80211(ctx, IEEE80211_IOC_REGDOMAIN,
	    &regdomain, sizeof(regdomain)) < 0)
		if_err(1, "unable to get regulatory domain info");
	gotregdomain = 1;
}

static void
getdevcaps(if_ctx *ctx, struct ieee80211_devcaps_req *dc)
{
	if (get80211(ctx, IEEE80211_IOC_DEVCAPS, dc,
	    IEEE80211_DEVCAPS_SPACE(dc)) < 0)
		if_err(1, "unable to get device capabilities");
}

static void
setregdomain_cb(if_ctx *ctx, void *arg)
{
	struct ieee80211_regdomain_req *req;
	struct ieee80211_regdomain *rd = arg;
	struct ieee80211_devcaps_req *dc;
	struct regdata *rdp = getregdata();

	if (rd->country != NO_COUNTRY) {
		const struct country *cc;
		/*
		 * Check current country seting to make sure it's
		 * compatible with the new regdomain.  If not, then
		 * override it with any default country for this
		 * SKU.  If we cannot arrange a match, then abort.
		 */
		cc = lib80211_country_findbycc(rdp, rd->country);
		if (cc == NULL)
			if_errx(1, "unknown ISO country code %d", rd->country);
		if (cc->rd->sku != rd->regdomain) {
			const struct regdomain *rp;
			/*
			 * Check if country is incompatible with regdomain.
			 * To enable multiple regdomains for a country code
			 * we permit a mismatch between the regdomain and
			 * the country's associated regdomain when the
			 * regdomain is setup w/o a default country.  For
			 * example, US is bound to the FCC regdomain but
			 * we allow US to be combined with FCC3 because FCC3
			 * has not default country.  This allows bogus
			 * combinations like FCC3+DK which are resolved when
			 * constructing the channel list by deferring to the
			 * regdomain to construct the channel list.
			 */
			rp = lib80211_regdomain_findbysku(rdp, rd->regdomain);
			if (rp == NULL)
				if_errx(1, "country %s (%s) is not usable with "
				    "regdomain %d", cc->isoname, cc->name,
				    rd->regdomain);
			else if (rp->cc != NULL && rp->cc != cc)
				if_errx(1, "country %s (%s) is not usable with "
				   "regdomain %s", cc->isoname, cc->name,
				   rp->name);
		}
	}
	/*
	 * Fetch the device capabilities and calculate the
	 * full set of netbands for which we request a new
	 * channel list be constructed.  Once that's done we
	 * push the regdomain info + channel list to the kernel.
	 */
	dc = malloc(IEEE80211_DEVCAPS_SIZE(MAXCHAN));
	if (dc == NULL)
		if_errx(1, "no space for device capabilities");
	dc->dc_chaninfo.ic_nchans = MAXCHAN;
	getdevcaps(ctx, dc);
#if 0
	if (verbose) {
		ifieee80211_print_drivercaps(dc);
		ifieee80211_print_cryptocaps(dc);
		ifieee80211_print_htcaps(dc);
		ifieee80211_print_vhtcaps(dc);
#if 0
		memcpy(chaninfo, &dc->dc_chaninfo,
		    IEEE80211_CHANINFO_SPACE(&dc->dc_chaninfo));
		ifieee80211_print_channels(s, &dc->dc_chaninfo, 1/*allchans*/, 1/*verbose*/);
#endif
	}
#endif
	req = malloc(IEEE80211_REGDOMAIN_SIZE(dc->dc_chaninfo.ic_nchans));
	if (req == NULL)
		if_errx(1, "no space for regdomain request");
	req->rd = *rd;
	regdomain_makechannels(ctx, req, dc);
	if (ctx->args->verbose) {
		ifieee80211_line_init(':');
		ifieee80211_print_regdomain(rd, 1/*verbose*/);
		ifieee80211_line_break();
		/* blech, reallocate channel list for new data */
		if (chaninfo != NULL)
			free(chaninfo);
		chaninfo = malloc(IEEE80211_CHANINFO_SPACE(&req->chaninfo));
		if (chaninfo == NULL)
			if_errx(1, "no space for channel list");
		memcpy(chaninfo, &req->chaninfo,
		    IEEE80211_CHANINFO_SPACE(&req->chaninfo));
		ifieee80211_print_channels(ctx, &req->chaninfo, 1/*allchans*/, 1/*verbose*/);
	}
	if (req->chaninfo.ic_nchans == 0)
		if_errx(1, "no channels calculated");
	set80211(ctx, IEEE80211_IOC_REGDOMAIN, 0,
	    IEEE80211_REGDOMAIN_SPACE(req), req);
	free(req);
	free(dc);
}

int
ieee80211_mhz2ieee(int freq, int flags)
{
	struct ieee80211_channel chan;
	mapfreq(&chan, freq, flags);
	return chan.ic_ieee;
}

static int
isanyarg(const char *arg)
{
	return (strncmp(arg, "-", 1) == 0 ||
	    strncasecmp(arg, "any", 3) == 0 || strncasecmp(arg, "off", 3) == 0);
}

static void
set80211ssid(if_ctx *ctx, const char *val, int dummy __unused)
{
	int		ssid;
	int		len;
	u_int8_t	data[IEEE80211_NWID_LEN];

	ssid = 0;
	len = strlen(val);
	if (len > 2 && isdigit((int)val[0]) && val[1] == ':') {
		ssid = atoi(val)-1;
		val += 2;
	}

	bzero(data, sizeof(data));
	len = sizeof(data);
	if (get_string(val, NULL, data, &len) == NULL)
		exit(1);

	set80211(ctx, IEEE80211_IOC_SSID, ssid, len, data);
}

static void
set80211meshid(if_ctx *ctx, const char *val, int dummy __unused)
{
	int		len;
	u_int8_t	data[IEEE80211_NWID_LEN];

	memset(data, 0, sizeof(data));
	len = sizeof(data);
	if (get_string(val, NULL, data, &len) == NULL)
		exit(1);

	set80211(ctx, IEEE80211_IOC_MESH_ID, 0, len, data);
}	

static void
set80211stationname(if_ctx *ctx, const char *val, int dummy __unused)
{
	int			len;
	u_int8_t		data[33];

	bzero(data, sizeof(data));
	len = sizeof(data);
	get_string(val, NULL, data, &len);

	set80211(ctx, IEEE80211_IOC_STATIONNAME, 0, len, data);
}

/*
 * Parse a channel specification for attributes/flags.
 * The syntax is:
 *	freq/xx		channel width (5,10,20,40,40+,40-)
 *	freq:mode	channel mode (a,b,g,h,n,t,s,d)
 *
 * These can be combined in either order; e.g. 2437:ng/40.
 * Modes are case insensitive.
 *
 * The result is not validated here; it's assumed to be
 * checked against the channel table fetched from the kernel.
 */ 
static unsigned int
getchannelflags(const char *val, int freq)
{
#define	_CHAN_HT	0x80000000
	const char *cp;
	int flags;
	int is_vht = 0;

	flags = 0;

	cp = strchr(val, ':');
	if (cp != NULL) {
		for (cp++; isalpha((int) *cp); cp++) {
			/* accept mixed case */
			int c = *cp;
			if (isupper(c))
				c = tolower(c);
			switch (c) {
			case 'a':		/* 802.11a */
				flags |= IEEE80211_CHAN_A;
				break;
			case 'b':		/* 802.11b */
				flags |= IEEE80211_CHAN_B;
				break;
			case 'g':		/* 802.11g */
				flags |= IEEE80211_CHAN_G;
				break;
			case 'v':		/* vht: 802.11ac */
				is_vht = 1;
				/* Fallthrough */
			case 'h':		/* ht = 802.11n */
			case 'n':		/* 802.11n */
				flags |= _CHAN_HT;	/* NB: private */
				break;
			case 'd':		/* dt = Atheros Dynamic Turbo */
				flags |= IEEE80211_CHAN_TURBO;
				break;
			case 't':		/* ht, dt, st, t */
				/* dt and unadorned t specify Dynamic Turbo */
				if ((flags & (IEEE80211_CHAN_STURBO|_CHAN_HT)) == 0)
					flags |= IEEE80211_CHAN_TURBO;
				break;
			case 's':		/* st = Atheros Static Turbo */
				flags |= IEEE80211_CHAN_STURBO;
				break;
			default:
				if_errx(-1, "%s: Invalid channel attribute %c\n",
				    val, *cp);
			}
		}
	}
	cp = strchr(val, '/');
	if (cp != NULL) {
		char *ep;
		u_long cw = strtoul(cp+1, &ep, 10);

		switch (cw) {
		case 5:
			flags |= IEEE80211_CHAN_QUARTER;
			break;
		case 10:
			flags |= IEEE80211_CHAN_HALF;
			break;
		case 20:
			/* NB: this may be removed below */
			flags |= IEEE80211_CHAN_HT20;
			break;
		case 40:
		case 80:
		case 160:
			/* Handle the 80/160 VHT flag */
			if (cw == 80)
				flags |= IEEE80211_CHAN_VHT80;
			else if (cw == 160)
				flags |= IEEE80211_CHAN_VHT160;

			/* Fallthrough */
			if (ep != NULL && *ep == '+')
				flags |= IEEE80211_CHAN_HT40U;
			else if (ep != NULL && *ep == '-')
				flags |= IEEE80211_CHAN_HT40D;
			break;
		default:
			if_errx(-1, "%s: Invalid channel width\n", val);
		}
	}

	/*
	 * Cleanup specifications.
	 */ 
	if ((flags & _CHAN_HT) == 0) {
		/*
		 * If user specified freq/20 or freq/40 quietly remove
		 * HT cw attributes depending on channel use.  To give
		 * an explicit 20/40 width for an HT channel you must
		 * indicate it is an HT channel since all HT channels
		 * are also usable for legacy operation; e.g. freq:n/40.
		 */
		flags &= ~IEEE80211_CHAN_HT;
		flags &= ~IEEE80211_CHAN_VHT;
	} else {
		/*
		 * Remove private indicator that this is an HT channel
		 * and if no explicit channel width has been given
		 * provide the default settings.
		 */
		flags &= ~_CHAN_HT;
		if ((flags & IEEE80211_CHAN_HT) == 0) {
			struct ieee80211_channel chan;
			/*
			 * Consult the channel list to see if we can use
			 * HT40+ or HT40- (if both the map routines choose).
			 */
			if (freq > 255)
				mapfreq(&chan, freq, 0);
			else
				mapchan(&chan, freq, 0);
			flags |= (chan.ic_flags & IEEE80211_CHAN_HT);
		}

		/*
		 * If VHT is enabled, then also set the VHT flag and the
		 * relevant channel up/down.
		 */
		if (is_vht && (flags & IEEE80211_CHAN_HT)) {
			/*
			 * XXX yes, maybe we should just have VHT, and reuse
			 * HT20/HT40U/HT40D
			 */
			if (flags & IEEE80211_CHAN_VHT80)
				;
			else if (flags & IEEE80211_CHAN_HT20)
				flags |= IEEE80211_CHAN_VHT20;
			else if (flags & IEEE80211_CHAN_HT40U)
				flags |= IEEE80211_CHAN_VHT40U;
			else if (flags & IEEE80211_CHAN_HT40D)
				flags |= IEEE80211_CHAN_VHT40D;
		}
	}
	return flags;
#undef _CHAN_HT
}

static void
getchannel(if_ctx *ctx, struct ieee80211_channel *chan, const char *val)
{
	unsigned int v, flags;
	char *eptr;

	memset(chan, 0, sizeof(*chan));
	if (isanyarg(val)) {
		chan->ic_freq = IEEE80211_CHAN_ANY;
		return;
	}
	getchaninfo(ctx);
	errno = 0;
	v = strtol(val, &eptr, 10);
	if (val[0] == '\0' || val == eptr || errno == ERANGE ||
	    /* channel may be suffixed with nothing, :flag, or /width */
	    (eptr[0] != '\0' && eptr[0] != ':' && eptr[0] != '/'))
		if_errx(1, "invalid channel specification%s",
		    errno == ERANGE ? " (out of range)" : "");
	flags = getchannelflags(val, v);
	if (v > 255) {		/* treat as frequency */
		mapfreq(chan, v, flags);
	} else {
		mapchan(chan, v, flags);
	}
}

static void
set80211channel(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct ieee80211_channel chan;

	getchannel(ctx, &chan, val);
	set80211(ctx, IEEE80211_IOC_CURCHAN, 0, sizeof(chan), &chan);
}

static void
set80211chanswitch(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct ieee80211_chanswitch_req csr;

	getchannel(ctx, &csr.csa_chan, val);
	csr.csa_mode = 1;
	csr.csa_count = 5;
	set80211(ctx, IEEE80211_IOC_CHANSWITCH, 0, sizeof(csr), &csr);
}

static void
set80211authmode(if_ctx *ctx, const char *val, int dummy __unused)
{
	int	mode;

	if (strcasecmp(val, "none") == 0) {
		mode = IEEE80211_AUTH_NONE;
	} else if (strcasecmp(val, "open") == 0) {
		mode = IEEE80211_AUTH_OPEN;
	} else if (strcasecmp(val, "shared") == 0) {
		mode = IEEE80211_AUTH_SHARED;
	} else if (strcasecmp(val, "8021x") == 0) {
		mode = IEEE80211_AUTH_8021X;
	} else if (strcasecmp(val, "wpa") == 0) {
		mode = IEEE80211_AUTH_WPA;
	} else {
		if_errx(1, "unknown authmode");
	}

	set80211(ctx, IEEE80211_IOC_AUTHMODE, mode, 0, NULL);
}

static void
set80211powersavemode(if_ctx *ctx, const char *val, int dummy __unused)
{
	int	mode;

	if (strcasecmp(val, "off") == 0) {
		mode = IEEE80211_POWERSAVE_OFF;
	} else if (strcasecmp(val, "on") == 0) {
		mode = IEEE80211_POWERSAVE_ON;
	} else if (strcasecmp(val, "cam") == 0) {
		mode = IEEE80211_POWERSAVE_CAM;
	} else if (strcasecmp(val, "psp") == 0) {
		mode = IEEE80211_POWERSAVE_PSP;
	} else if (strcasecmp(val, "psp-cam") == 0) {
		mode = IEEE80211_POWERSAVE_PSP_CAM;
	} else {
		if_errx(1, "unknown powersavemode");
	}

	set80211(ctx, IEEE80211_IOC_POWERSAVE, mode, 0, NULL);
}

static void
set80211powersave(if_ctx *ctx, const char *val __unused, int d)
{
	if (d == 0)
		set80211(ctx, IEEE80211_IOC_POWERSAVE, IEEE80211_POWERSAVE_OFF,
		    0, NULL);
	else
		set80211(ctx, IEEE80211_IOC_POWERSAVE, IEEE80211_POWERSAVE_ON,
		    0, NULL);
}

static void
set80211powersavesleep(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_POWERSAVESLEEP, atoi(val), 0, NULL);
}

static void
set80211wepmode(if_ctx *ctx, const char *val, int dummy __unused)
{
	int	mode;

	if (strcasecmp(val, "off") == 0) {
		mode = IEEE80211_WEP_OFF;
	} else if (strcasecmp(val, "on") == 0) {
		mode = IEEE80211_WEP_ON;
	} else if (strcasecmp(val, "mixed") == 0) {
		mode = IEEE80211_WEP_MIXED;
	} else {
		if_errx(1, "unknown wep mode");
	}

	set80211(ctx, IEEE80211_IOC_WEP, mode, 0, NULL);
}

static void
set80211wep(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_WEP, d, 0, NULL);
}

static int
isundefarg(const char *arg)
{
	return (strcmp(arg, "-") == 0 || strncasecmp(arg, "undef", 5) == 0);
}

static void
set80211weptxkey(if_ctx *ctx, const char *val, int dummy __unused)
{
	if (isundefarg(val))
		set80211(ctx, IEEE80211_IOC_WEPTXKEY, IEEE80211_KEYIX_NONE, 0, NULL);
	else
		set80211(ctx, IEEE80211_IOC_WEPTXKEY, atoi(val)-1, 0, NULL);
}

static void
set80211wepkey(if_ctx *ctx, const char *val, int dummy __unused)
{
	int		key = 0;
	int		len;
	u_int8_t	data[IEEE80211_KEYBUF_SIZE];

	if (isdigit((int)val[0]) && val[1] == ':') {
		key = atoi(val)-1;
		val += 2;
	}

	bzero(data, sizeof(data));
	len = sizeof(data);
	get_string(val, NULL, data, &len);

	set80211(ctx, IEEE80211_IOC_WEPKEY, key, len, data);
}

/*
 * This function is purely a NetBSD compatibility interface.  The NetBSD
 * interface is too inflexible, but it's there so we'll support it since
 * it's not all that hard.
 */
static void
set80211nwkey(if_ctx *ctx, const char *val, int dummy __unused)
{
	int		txkey;
	int		i, len;
	u_int8_t	data[IEEE80211_KEYBUF_SIZE];

	set80211(ctx, IEEE80211_IOC_WEP, IEEE80211_WEP_ON, 0, NULL);

	if (isdigit((int)val[0]) && val[1] == ':') {
		txkey = val[0]-'0'-1;
		val += 2;

		for (i = 0; i < 4; i++) {
			bzero(data, sizeof(data));
			len = sizeof(data);
			val = get_string(val, ",", data, &len);
			if (val == NULL)
				exit(1);

			set80211(ctx, IEEE80211_IOC_WEPKEY, i, len, data);
		}
	} else {
		bzero(data, sizeof(data));
		len = sizeof(data);
		get_string(val, NULL, data, &len);
		txkey = 0;

		set80211(ctx, IEEE80211_IOC_WEPKEY, 0, len, data);

		bzero(data, sizeof(data));
		for (i = 1; i < 4; i++)
			set80211(ctx, IEEE80211_IOC_WEPKEY, i, 0, data);
	}

	set80211(ctx, IEEE80211_IOC_WEPTXKEY, txkey, 0, NULL);
}

static void
set80211rtsthreshold(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_RTSTHRESHOLD,
		isundefarg(val) ? IEEE80211_RTS_MAX : atoi(val), 0, NULL);
}

static void
set80211protmode(if_ctx *ctx, const char *val, int dummy __unused)
{
	int	mode;

	if (strcasecmp(val, "off") == 0) {
		mode = IEEE80211_PROTMODE_OFF;
	} else if (strcasecmp(val, "cts") == 0) {
		mode = IEEE80211_PROTMODE_CTS;
	} else if (strncasecmp(val, "rtscts", 3) == 0) {
		mode = IEEE80211_PROTMODE_RTSCTS;
	} else {
		if_errx(1, "unknown protection mode");
	}

	set80211(ctx, IEEE80211_IOC_PROTMODE, mode, 0, NULL);
}

static void
set80211htprotmode(if_ctx *ctx, const char *val, int dummy __unused)
{
	int	mode;

	if (strcasecmp(val, "off") == 0) {
		mode = IEEE80211_PROTMODE_OFF;
	} else if (strncasecmp(val, "rts", 3) == 0) {
		mode = IEEE80211_PROTMODE_RTSCTS;
	} else {
		if_errx(1, "unknown protection mode");
	}

	set80211(ctx, IEEE80211_IOC_HTPROTMODE, mode, 0, NULL);
}

static void
set80211txpower(if_ctx *ctx, const char *val, int dummy __unused)
{
	double v = atof(val);
	int txpow;

	txpow = (int) (2*v);
	if (txpow != 2*v)
		if_errx(-1, "invalid tx power (must be .5 dBm units)");
	set80211(ctx, IEEE80211_IOC_TXPOWER, txpow, 0, NULL);
}

#define	IEEE80211_ROAMING_DEVICE	0
#define	IEEE80211_ROAMING_AUTO		1
#define	IEEE80211_ROAMING_MANUAL	2

static void
set80211roaming(if_ctx *ctx, const char *val, int dummy __unused)
{
	int mode;

	if (strcasecmp(val, "device") == 0) {
		mode = IEEE80211_ROAMING_DEVICE;
	} else if (strcasecmp(val, "auto") == 0) {
		mode = IEEE80211_ROAMING_AUTO;
	} else if (strcasecmp(val, "manual") == 0) {
		mode = IEEE80211_ROAMING_MANUAL;
	} else {
		if_errx(1, "unknown roaming mode");
	}
	set80211(ctx, IEEE80211_IOC_ROAMING, mode, 0, NULL);
}

static void
set80211wme(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_WME, d, 0, NULL);
}

static void
set80211hidessid(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_HIDESSID, d, 0, NULL);
}

static void
set80211apbridge(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_APBRIDGE, d, 0, NULL);
}

static void
set80211fastframes(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_FF, d, 0, NULL);
}

static void
set80211dturbo(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_TURBOP, d, 0, NULL);
}

static void
set80211chanlist(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct ieee80211req_chanlist chanlist;
	char *temp, *cp, *tp;

	temp = malloc(strlen(val) + 1);
	if (temp == NULL)
		if_errx(1, "malloc failed");
	strcpy(temp, val);
	memset(&chanlist, 0, sizeof(chanlist));
	cp = temp;
	for (;;) {
		int first, last, f, c;

		tp = strchr(cp, ',');
		if (tp != NULL)
			*tp++ = '\0';
		switch (sscanf(cp, "%u-%u", &first, &last)) {
		case 1:
			if (first > IEEE80211_CHAN_MAX)
				if_errx(-1, "channel %u out of range, max %u",
					first, IEEE80211_CHAN_MAX);
			setbit(chanlist.ic_channels, first);
			break;
		case 2:
			if (first > IEEE80211_CHAN_MAX)
				if_errx(-1, "channel %u out of range, max %u",
					first, IEEE80211_CHAN_MAX);
			if (last > IEEE80211_CHAN_MAX)
				if_errx(-1, "channel %u out of range, max %u",
					last, IEEE80211_CHAN_MAX);
			if (first > last)
				if_errx(-1, "void channel range, %u > %u",
					first, last);
			for (f = first; f <= last; f++)
				setbit(chanlist.ic_channels, f);
			break;
		}
		if (tp == NULL)
			break;
		c = *tp;
		while (isspace(c))
			tp++;
		if (!isdigit(c))
			break;
		cp = tp;
	}
	set80211(ctx, IEEE80211_IOC_CHANLIST, 0, sizeof(chanlist), &chanlist);
	free(temp);
}

static void
set80211bssid(if_ctx *ctx, const char *val, int dummy __unused)
{
	if (!isanyarg(val)) {
		char *temp;
		struct sockaddr_dl sdl;

		temp = malloc(strlen(val) + 2); /* ':' and '\0' */
		if (temp == NULL)
			if_errx(1, "malloc failed");
		temp[0] = ':';
		strcpy(temp + 1, val);
		sdl.sdl_len = sizeof(sdl);
		link_addr(temp, &sdl);
		free(temp);
		if (sdl.sdl_alen != IEEE80211_ADDR_LEN)
			if_errx(1, "malformed link-level address");
		set80211(ctx, IEEE80211_IOC_BSSID, 0,
			IEEE80211_ADDR_LEN, LLADDR(&sdl));
	} else {
		uint8_t zerobssid[IEEE80211_ADDR_LEN];
		memset(zerobssid, 0, sizeof(zerobssid));
		set80211(ctx, IEEE80211_IOC_BSSID, 0,
			IEEE80211_ADDR_LEN, zerobssid);
	}
}

static int
getac(const char *ac)
{
	if (strcasecmp(ac, "ac_be") == 0 || strcasecmp(ac, "be") == 0)
		return WME_AC_BE;
	if (strcasecmp(ac, "ac_bk") == 0 || strcasecmp(ac, "bk") == 0)
		return WME_AC_BK;
	if (strcasecmp(ac, "ac_vi") == 0 || strcasecmp(ac, "vi") == 0)
		return WME_AC_VI;
	if (strcasecmp(ac, "ac_vo") == 0 || strcasecmp(ac, "vo") == 0)
		return WME_AC_VO;
	if_errx(1, "unknown wme access class %s", ac);
}

static void
set80211cwmin(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_CWMIN, atoi(val), getac(ac), NULL);
}

static void
set80211cwmax(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_CWMAX, atoi(val), getac(ac), NULL);
}

static void
set80211aifs(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_AIFS, atoi(val), getac(ac), NULL);
}

static void
set80211txoplimit(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_TXOPLIMIT, atoi(val), getac(ac), NULL);
}

static void
set80211acm(if_ctx *ctx, const char *ac, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_WME_ACM, 1, getac(ac), NULL);
}

static void
set80211noacm(if_ctx *ctx, const char *ac, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_WME_ACM, 0, getac(ac), NULL);
}

static void
set80211ackpolicy(if_ctx *ctx, const char *ac, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_WME_ACKPOLICY, 1, getac(ac), NULL);
}
static void
set80211noackpolicy(if_ctx *ctx, const char *ac, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_WME_ACKPOLICY, 0, getac(ac), NULL);
}

static void
set80211bsscwmin(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_CWMIN, atoi(val),
		getac(ac)|IEEE80211_WMEPARAM_BSS, NULL);
}

static void
set80211bsscwmax(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_CWMAX, atoi(val),
		getac(ac)|IEEE80211_WMEPARAM_BSS, NULL);
}

static void
set80211bssaifs(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_AIFS, atoi(val),
		getac(ac)|IEEE80211_WMEPARAM_BSS, NULL);
}

static void
set80211bsstxoplimit(if_ctx *ctx, const char *ac, const char *val)
{
	set80211(ctx, IEEE80211_IOC_WME_TXOPLIMIT, atoi(val),
		getac(ac)|IEEE80211_WMEPARAM_BSS, NULL);
}

static void
set80211dtimperiod(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_DTIM_PERIOD, atoi(val), 0, NULL);
}

static void
set80211bintval(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_BEACON_INTERVAL, atoi(val), 0, NULL);
}

static void
set80211macmac(if_ctx *ctx, int op, const char *val)
{
	char *temp;
	struct sockaddr_dl sdl;

	temp = malloc(strlen(val) + 2); /* ':' and '\0' */
	if (temp == NULL)
		if_errx(1, "malloc failed");
	temp[0] = ':';
	strcpy(temp + 1, val);
	sdl.sdl_len = sizeof(sdl);
	link_addr(temp, &sdl);
	free(temp);
	if (sdl.sdl_alen != IEEE80211_ADDR_LEN)
		if_errx(1, "malformed link-level address");
	set80211(ctx, op, 0, IEEE80211_ADDR_LEN, LLADDR(&sdl));
}

static void
set80211addmac(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211macmac(ctx, IEEE80211_IOC_ADDMAC, val);
}

static void
set80211delmac(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211macmac(ctx, IEEE80211_IOC_DELMAC, val);
}

static void
set80211kickmac(if_ctx *ctx, const char *val, int dummy __unused)
{
	char *temp;
	struct sockaddr_dl sdl;
	struct ieee80211req_mlme mlme;

	temp = malloc(strlen(val) + 2); /* ':' and '\0' */
	if (temp == NULL)
		if_errx(1, "malloc failed");
	temp[0] = ':';
	strcpy(temp + 1, val);
	sdl.sdl_len = sizeof(sdl);
	link_addr(temp, &sdl);
	free(temp);
	if (sdl.sdl_alen != IEEE80211_ADDR_LEN)
		if_errx(1, "malformed link-level address");
	memset(&mlme, 0, sizeof(mlme));
	mlme.im_op = IEEE80211_MLME_DEAUTH;
	mlme.im_reason = IEEE80211_REASON_AUTH_EXPIRE;
	memcpy(mlme.im_macaddr, LLADDR(&sdl), IEEE80211_ADDR_LEN);
	set80211(ctx, IEEE80211_IOC_MLME, 0, sizeof(mlme), &mlme);
}

static void
set80211maccmd(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_MACCMD, d, 0, NULL);
}

static void
set80211meshrtmac(if_ctx *ctx, int req, const char *val)
{
	char *temp;
	struct sockaddr_dl sdl;

	temp = malloc(strlen(val) + 2); /* ':' and '\0' */
	if (temp == NULL)
		if_errx(1, "malloc failed");
	temp[0] = ':';
	strcpy(temp + 1, val);
	sdl.sdl_len = sizeof(sdl);
	link_addr(temp, &sdl);
	free(temp);
	if (sdl.sdl_alen != IEEE80211_ADDR_LEN)
		if_errx(1, "malformed link-level address");
	set80211(ctx, IEEE80211_IOC_MESH_RTCMD, req,
	    IEEE80211_ADDR_LEN, LLADDR(&sdl));
}

static void
set80211addmeshrt(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211meshrtmac(ctx, IEEE80211_MESH_RTCMD_ADD, val);
}

static void
set80211delmeshrt(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211meshrtmac(ctx, IEEE80211_MESH_RTCMD_DELETE, val);
}

static void
set80211meshrtcmd(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_MESH_RTCMD, d, 0, NULL);
}

static void
set80211hwmprootmode(if_ctx *ctx, const char *val, int dummy __unused)
{
	int mode;

	if (strcasecmp(val, "normal") == 0)
		mode = IEEE80211_HWMP_ROOTMODE_NORMAL;
	else if (strcasecmp(val, "proactive") == 0)
		mode = IEEE80211_HWMP_ROOTMODE_PROACTIVE;
	else if (strcasecmp(val, "rann") == 0)
		mode = IEEE80211_HWMP_ROOTMODE_RANN;
	else
		mode = IEEE80211_HWMP_ROOTMODE_DISABLED;
	set80211(ctx, IEEE80211_IOC_HWMP_ROOTMODE, mode, 0, NULL);
}

static void
set80211hwmpmaxhops(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_HWMP_MAXHOPS, atoi(val), 0, NULL);
}

static void
set80211pureg(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_PUREG, d, 0, NULL);
}

static void
set80211quiet(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_QUIET, d, 0, NULL);
}

static void
set80211quietperiod(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_QUIET_PERIOD, atoi(val), 0, NULL);
}

static void
set80211quietcount(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_QUIET_COUNT, atoi(val), 0, NULL);
}

static void
set80211quietduration(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_QUIET_DUR, atoi(val), 0, NULL);
}

static void
set80211quietoffset(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_QUIET_OFFSET, atoi(val), 0, NULL);
}

static void
set80211bgscan(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_BGSCAN, d, 0, NULL);
}

static void
set80211bgscanidle(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_BGSCAN_IDLE, atoi(val), 0, NULL);
}

static void
set80211bgscanintvl(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_BGSCAN_INTERVAL, atoi(val), 0, NULL);
}

static void
set80211scanvalid(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_SCANVALID, atoi(val), 0, NULL);
}

/*
 * Parse an optional trailing specification of which netbands
 * to apply a parameter to.  This is basically the same syntax
 * as used for channels but you can concatenate to specify
 * multiple.  For example:
 *	14:abg		apply to 11a, 11b, and 11g
 *	6:ht		apply to 11na and 11ng
 * We don't make a big effort to catch silly things; this is
 * really a convenience mechanism.
 */
static int
getmodeflags(const char *val)
{
	const char *cp;
	int flags;

	flags = 0;

	cp = strchr(val, ':');
	if (cp != NULL) {
		for (cp++; isalpha((int) *cp); cp++) {
			/* accept mixed case */
			int c = *cp;
			if (isupper(c))
				c = tolower(c);
			switch (c) {
			case 'a':		/* 802.11a */
				flags |= IEEE80211_CHAN_A;
				break;
			case 'b':		/* 802.11b */
				flags |= IEEE80211_CHAN_B;
				break;
			case 'g':		/* 802.11g */
				flags |= IEEE80211_CHAN_G;
				break;
			case 'n':		/* 802.11n */
				flags |= IEEE80211_CHAN_HT;
				break;
			case 'd':		/* dt = Atheros Dynamic Turbo */
				flags |= IEEE80211_CHAN_TURBO;
				break;
			case 't':		/* ht, dt, st, t */
				/* dt and unadorned t specify Dynamic Turbo */
				if ((flags & (IEEE80211_CHAN_STURBO|IEEE80211_CHAN_HT)) == 0)
					flags |= IEEE80211_CHAN_TURBO;
				break;
			case 's':		/* st = Atheros Static Turbo */
				flags |= IEEE80211_CHAN_STURBO;
				break;
			case 'h':		/* 1/2-width channels */
				flags |= IEEE80211_CHAN_HALF;
				break;
			case 'q':		/* 1/4-width channels */
				flags |= IEEE80211_CHAN_QUARTER;
				break;
			case 'v':
				/* XXX set HT too? */
				flags |= IEEE80211_CHAN_VHT;
				break;
			default:
				if_errx(-1, "%s: Invalid mode attribute %c\n",
				    val, *cp);
			}
		}
	}
	return flags;
}

#define	_APPLY(_flags, _base, _param, _v) do {				\
    if (_flags & IEEE80211_CHAN_HT) {					\
	    if ((_flags & (IEEE80211_CHAN_5GHZ|IEEE80211_CHAN_2GHZ)) == 0) {\
		    _base.params[IEEE80211_MODE_11NA]._param = _v;	\
		    _base.params[IEEE80211_MODE_11NG]._param = _v;	\
	    } else if (_flags & IEEE80211_CHAN_5GHZ)			\
		    _base.params[IEEE80211_MODE_11NA]._param = _v;	\
	    else							\
		    _base.params[IEEE80211_MODE_11NG]._param = _v;	\
    }									\
    if (_flags & IEEE80211_CHAN_TURBO) {				\
	    if ((_flags & (IEEE80211_CHAN_5GHZ|IEEE80211_CHAN_2GHZ)) == 0) {\
		    _base.params[IEEE80211_MODE_TURBO_A]._param = _v;	\
		    _base.params[IEEE80211_MODE_TURBO_G]._param = _v;	\
	    } else if (_flags & IEEE80211_CHAN_5GHZ)			\
		    _base.params[IEEE80211_MODE_TURBO_A]._param = _v;	\
	    else							\
		    _base.params[IEEE80211_MODE_TURBO_G]._param = _v;	\
    }									\
    if (_flags & IEEE80211_CHAN_STURBO)					\
	    _base.params[IEEE80211_MODE_STURBO_A]._param = _v;		\
    if ((_flags & IEEE80211_CHAN_A) == IEEE80211_CHAN_A)		\
	    _base.params[IEEE80211_MODE_11A]._param = _v;		\
    if ((_flags & IEEE80211_CHAN_G) == IEEE80211_CHAN_G)		\
	    _base.params[IEEE80211_MODE_11G]._param = _v;		\
    if ((_flags & IEEE80211_CHAN_B) == IEEE80211_CHAN_B)		\
	    _base.params[IEEE80211_MODE_11B]._param = _v;		\
    if (_flags & IEEE80211_CHAN_HALF)					\
	    _base.params[IEEE80211_MODE_HALF]._param = _v;		\
    if (_flags & IEEE80211_CHAN_QUARTER)				\
	    _base.params[IEEE80211_MODE_QUARTER]._param = _v;		\
} while (0)
#define	_APPLY1(_flags, _base, _param, _v) do {				\
    if (_flags & IEEE80211_CHAN_HT) {					\
	    if (_flags & IEEE80211_CHAN_5GHZ)				\
		    _base.params[IEEE80211_MODE_11NA]._param = _v;	\
	    else							\
		    _base.params[IEEE80211_MODE_11NG]._param = _v;	\
    } else if ((_flags & IEEE80211_CHAN_108A) == IEEE80211_CHAN_108A)	\
	    _base.params[IEEE80211_MODE_TURBO_A]._param = _v;		\
    else if ((_flags & IEEE80211_CHAN_108G) == IEEE80211_CHAN_108G)	\
	    _base.params[IEEE80211_MODE_TURBO_G]._param = _v;		\
    else if ((_flags & IEEE80211_CHAN_ST) == IEEE80211_CHAN_ST)		\
	    _base.params[IEEE80211_MODE_STURBO_A]._param = _v;		\
    else if (_flags & IEEE80211_CHAN_HALF)				\
	    _base.params[IEEE80211_MODE_HALF]._param = _v;		\
    else if (_flags & IEEE80211_CHAN_QUARTER)				\
	    _base.params[IEEE80211_MODE_QUARTER]._param = _v;		\
    else if ((_flags & IEEE80211_CHAN_A) == IEEE80211_CHAN_A)		\
	    _base.params[IEEE80211_MODE_11A]._param = _v;		\
    else if ((_flags & IEEE80211_CHAN_G) == IEEE80211_CHAN_G)		\
	    _base.params[IEEE80211_MODE_11G]._param = _v;		\
    else if ((_flags & IEEE80211_CHAN_B) == IEEE80211_CHAN_B)		\
	    _base.params[IEEE80211_MODE_11B]._param = _v;		\
} while (0)
#define	_APPLY_RATE(_flags, _base, _param, _v) do {			\
    if (_flags & IEEE80211_CHAN_HT) {					\
	(_v) = (_v / 2) | IEEE80211_RATE_MCS;				\
    }									\
    _APPLY(_flags, _base, _param, _v);					\
} while (0)
#define	_APPLY_RATE1(_flags, _base, _param, _v) do {			\
    if (_flags & IEEE80211_CHAN_HT) {					\
	(_v) = (_v / 2) | IEEE80211_RATE_MCS;				\
    }									\
    _APPLY1(_flags, _base, _param, _v);					\
} while (0)

static void
set80211roamrssi(if_ctx *ctx, const char *val, int dummy __unused)
{
	double v = atof(val);
	int rssi, flags;

	rssi = (int) (2*v);
	if (rssi != 2*v)
		if_errx(-1, "invalid rssi (must be .5 dBm units)");
	flags = getmodeflags(val);
	getroam(ctx);
	if (flags == 0) {		/* NB: no flags => current channel */
		flags = getcurchan(ctx)->ic_flags;
		_APPLY1(flags, roamparams, rssi, rssi);
	} else
		_APPLY(flags, roamparams, rssi, rssi);
	callback_register(setroam_cb, &roamparams);
}

static int
getrate(const char *val, const char *tag)
{
	double v = atof(val);
	int rate;

	rate = (int) (2*v);
	if (rate != 2*v)
		if_errx(-1, "invalid %s rate (must be .5 Mb/s units)", tag);
	return rate;		/* NB: returns 2x the specified value */
}

static void
set80211roamrate(if_ctx *ctx, const char *val, int dummy __unused)
{
	int rate, flags;

	rate = getrate(val, "roam");
	flags = getmodeflags(val);
	getroam(ctx);
	if (flags == 0) {		/* NB: no flags => current channel */
		flags = getcurchan(ctx)->ic_flags;
		_APPLY_RATE1(flags, roamparams, rate, rate);
	} else
		_APPLY_RATE(flags, roamparams, rate, rate);
	callback_register(setroam_cb, &roamparams);
}

static void
set80211mcastrate(if_ctx *ctx, const char *val, int dummy __unused)
{
	int rate, flags;

	rate = getrate(val, "mcast");
	flags = getmodeflags(val);
	gettxparams(ctx);
	if (flags == 0) {		/* NB: no flags => current channel */
		flags = getcurchan(ctx)->ic_flags;
		_APPLY_RATE1(flags, txparams, mcastrate, rate);
	} else
		_APPLY_RATE(flags, txparams, mcastrate, rate);
	callback_register(settxparams_cb, &txparams);
}

static void
set80211mgtrate(if_ctx *ctx, const char *val, int dummy __unused)
{
	int rate, flags;

	rate = getrate(val, "mgmt");
	flags = getmodeflags(val);
	gettxparams(ctx);
	if (flags == 0) {		/* NB: no flags => current channel */
		flags = getcurchan(ctx)->ic_flags;
		_APPLY_RATE1(flags, txparams, mgmtrate, rate);
	} else
		_APPLY_RATE(flags, txparams, mgmtrate, rate);
	callback_register(settxparams_cb, &txparams);
}

static void
set80211ucastrate(if_ctx *ctx, const char *val, int dummy __unused)
{
	int flags;

	gettxparams(ctx);
	flags = getmodeflags(val);
	if (isanyarg(val)) {
		if (flags == 0) {	/* NB: no flags => current channel */
			flags = getcurchan(ctx)->ic_flags;
			_APPLY1(flags, txparams, ucastrate,
			    IEEE80211_FIXED_RATE_NONE);
		} else
			_APPLY(flags, txparams, ucastrate,
			    IEEE80211_FIXED_RATE_NONE);
	} else {
		int rate = getrate(val, "ucast");
		if (flags == 0) {	/* NB: no flags => current channel */
			flags = getcurchan(ctx)->ic_flags;
			_APPLY_RATE1(flags, txparams, ucastrate, rate);
		} else
			_APPLY_RATE(flags, txparams, ucastrate, rate);
	}
	callback_register(settxparams_cb, &txparams);
}

static void
set80211maxretry(if_ctx *ctx, const char *val, int dummy __unused)
{
	int v = atoi(val), flags;

	flags = getmodeflags(val);
	gettxparams(ctx);
	if (flags == 0) {		/* NB: no flags => current channel */
		flags = getcurchan(ctx)->ic_flags;
		_APPLY1(flags, txparams, maxretry, v);
	} else
		_APPLY(flags, txparams, maxretry, v);
	callback_register(settxparams_cb, &txparams);
}
#undef _APPLY_RATE
#undef _APPLY

static void
set80211fragthreshold(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_FRAGTHRESHOLD,
		isundefarg(val) ? IEEE80211_FRAG_MAX : atoi(val), 0, NULL);
}

static void
set80211bmissthreshold(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_BMISSTHRESHOLD,
		isundefarg(val) ? IEEE80211_HWBMISS_MAX : atoi(val), 0, NULL);
}

static void
set80211burst(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_BURST, d, 0, NULL);
}

static void
set80211doth(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_DOTH, d, 0, NULL);
}

static void
set80211dfs(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_DFS, d, 0, NULL);
}

static void
set80211shortgi(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_SHORTGI,
		d ? (IEEE80211_HTCAP_SHORTGI20 | IEEE80211_HTCAP_SHORTGI40) : 0,
		0, NULL);
}

/* XXX 11ac density/size is different */
static void
set80211ampdu(if_ctx *ctx, const char *val __unused, int d)
{
	int ampdu;

	if (get80211val(ctx, IEEE80211_IOC_AMPDU, &ampdu) < 0)
		if_errx(-1, "cannot set AMPDU setting");
	if (d < 0) {
		d = -d;
		ampdu &= ~d;
	} else
		ampdu |= d;
	set80211(ctx, IEEE80211_IOC_AMPDU, ampdu, 0, NULL);
}

static void
set80211stbc(if_ctx *ctx, const char *val __unused, int d)
{
	int stbc;

	if (get80211val(ctx, IEEE80211_IOC_STBC, &stbc) < 0)
		if_errx(-1, "cannot set STBC setting");
	if (d < 0) {
		d = -d;
		stbc &= ~d;
	} else
		stbc |= d;
	set80211(ctx, IEEE80211_IOC_STBC, stbc, 0, NULL);
}

static void
set80211ldpc(if_ctx *ctx, const char *val __unused, int d)
{
        int ldpc;
 
        if (get80211val(ctx, IEEE80211_IOC_LDPC, &ldpc) < 0)
                if_errx(-1, "cannot set LDPC setting");
        if (d < 0) {
                d = -d;
                ldpc &= ~d;
        } else
                ldpc |= d;
        set80211(ctx, IEEE80211_IOC_LDPC, ldpc, 0, NULL);
}

static void
set80211uapsd(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_UAPSD, d, 0, NULL);
}

static void
set80211ampdulimit(if_ctx *ctx, const char *val, int dummy __unused)
{
	int v = 0;

	switch (atoi(val)) {
	case 8:
	case 8*1024:
		v = IEEE80211_HTCAP_MAXRXAMPDU_8K;
		break;
	case 16:
	case 16*1024:
		v = IEEE80211_HTCAP_MAXRXAMPDU_16K;
		break;
	case 32:
	case 32*1024:
		v = IEEE80211_HTCAP_MAXRXAMPDU_32K;
		break;
	case 64:
	case 64*1024:
		v = IEEE80211_HTCAP_MAXRXAMPDU_64K;
		break;
	default:
		if_errx(-1, "invalid A-MPDU limit %s", val);
	}
	set80211(ctx, IEEE80211_IOC_AMPDU_LIMIT, v, 0, NULL);
}

/* XXX 11ac density/size is different */
static void
set80211ampdudensity(if_ctx *ctx, const char *val, int dummy __unused)
{
	int v;

	if (isanyarg(val) || strcasecmp(val, "na") == 0)
		v = IEEE80211_HTCAP_MPDUDENSITY_NA;
	else switch ((int)(atof(val)*4)) {
	case 0:
		v = IEEE80211_HTCAP_MPDUDENSITY_NA;
		break;
	case 1:
		v = IEEE80211_HTCAP_MPDUDENSITY_025;
		break;
	case 2:
		v = IEEE80211_HTCAP_MPDUDENSITY_05;
		break;
	case 4:
		v = IEEE80211_HTCAP_MPDUDENSITY_1;
		break;
	case 8:
		v = IEEE80211_HTCAP_MPDUDENSITY_2;
		break;
	case 16:
		v = IEEE80211_HTCAP_MPDUDENSITY_4;
		break;
	case 32:
		v = IEEE80211_HTCAP_MPDUDENSITY_8;
		break;
	case 64:
		v = IEEE80211_HTCAP_MPDUDENSITY_16;
		break;
	default:
		if_errx(-1, "invalid A-MPDU density %s", val);
	}
	set80211(ctx, IEEE80211_IOC_AMPDU_DENSITY, v, 0, NULL);
}

static void
set80211amsdu(if_ctx *ctx, const char *val __unused, int d)
{
	int amsdu;

	if (get80211val(ctx, IEEE80211_IOC_AMSDU, &amsdu) < 0)
		if_err(-1, "cannot get AMSDU setting");
	if (d < 0) {
		d = -d;
		amsdu &= ~d;
	} else
		amsdu |= d;
	set80211(ctx, IEEE80211_IOC_AMSDU, amsdu, 0, NULL);
}

static void
set80211amsdulimit(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_AMSDU_LIMIT, atoi(val), 0, NULL);
}

static void
set80211puren(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_PUREN, d, 0, NULL);
}

static void
set80211htcompat(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_HTCOMPAT, d, 0, NULL);
}

static void
set80211htconf(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_HTCONF, d, 0, NULL);
	htconf = d;
}

static void
set80211dwds(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_DWDS, d, 0, NULL);
}

static void
set80211inact(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_INACTIVITY, d, 0, NULL);
}

static void
set80211tsn(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_TSN, d, 0, NULL);
}

static void
set80211dotd(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_DOTD, d, 0, NULL);
}

static void
set80211smps(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_SMPS, d, 0, NULL);
}

static void
set80211rifs(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_RIFS, d, 0, NULL);
}

static void
set80211vhtconf(if_ctx *ctx, const char *val __unused, int d)
{
	if (get80211val(ctx, IEEE80211_IOC_VHTCONF, &vhtconf) < 0)
		if_errx(-1, "cannot set VHT setting");
	if (d < 0) {
		d = -d;
		vhtconf &= ~d;
	} else
		vhtconf |= d;
	set80211(ctx, IEEE80211_IOC_VHTCONF, vhtconf, 0, NULL);
}

static void
set80211tdmaslot(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_TDMA_SLOT, atoi(val), 0, NULL);
}

static void
set80211tdmaslotcnt(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_TDMA_SLOTCNT, atoi(val), 0, NULL);
}

static void
set80211tdmaslotlen(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_TDMA_SLOTLEN, atoi(val), 0, NULL);
}

static void
set80211tdmabintval(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_TDMA_BINTERVAL, atoi(val), 0, NULL);
}

static void
set80211meshttl(if_ctx *ctx, const char *val, int dummy __unused)
{
	set80211(ctx, IEEE80211_IOC_MESH_TTL, atoi(val), 0, NULL);
}

static void
set80211meshforward(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_MESH_FWRD, d, 0, NULL);
}

static void
set80211meshgate(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_MESH_GATE, d, 0, NULL);
}

static void
set80211meshpeering(if_ctx *ctx, const char *val __unused, int d)
{
	set80211(ctx, IEEE80211_IOC_MESH_AP, d, 0, NULL);
}

static void
set80211meshmetric(if_ctx *ctx, const char *val, int dummy __unused)
{
	char v[12];

	memcpy(v, val, sizeof(v));
	set80211(ctx, IEEE80211_IOC_MESH_PR_METRIC, 0, 0, v);
}

static void
set80211meshpath(if_ctx *ctx, const char *val, int dummy __unused)
{
	char v[12];

	memcpy(v, val, sizeof(v));
	set80211(ctx, IEEE80211_IOC_MESH_PR_PATH, 0, 0, v);
}

static int
regdomain_sort(const void *a, const void *b)
{
#define	CHAN_ALL \
	(IEEE80211_CHAN_ALLTURBO|IEEE80211_CHAN_HALF|IEEE80211_CHAN_QUARTER)
	const struct ieee80211_channel *ca = a;
	const struct ieee80211_channel *cb = b;

	return ca->ic_freq == cb->ic_freq ?
	    (int)(ca->ic_flags & CHAN_ALL) - (int)(cb->ic_flags & CHAN_ALL) :
	    ca->ic_freq - cb->ic_freq;
#undef CHAN_ALL
}

static const struct ieee80211_channel *
chanlookup(const struct ieee80211_channel chans[], int nchans,
	int freq, uint32_t flags)
{
	int i;

	flags &= IEEE80211_CHAN_ALLTURBO;
	for (i = 0; i < nchans; i++) {
		const struct ieee80211_channel *c = &chans[i];
		if (c->ic_freq == freq &&
		    (c->ic_flags & IEEE80211_CHAN_ALLTURBO) == flags)
			return c;
	}
	return NULL;
}

static int
chanfind(const struct ieee80211_channel chans[], int nchans, uint32_t flags)
{
	for (int i = 0; i < nchans; i++) {
		const struct ieee80211_channel *c = &chans[i];
		if ((c->ic_flags & flags) == flags)
			return 1;
	}
	return 0;
}

/*
 * Check channel compatibility.
 */
static int
checkchan(const struct ieee80211req_chaninfo *avail, int freq, uint32_t flags)
{
	flags &= ~REQ_FLAGS;
	/*
	 * Check if exact channel is in the calibration table;
	 * everything below is to deal with channels that we
	 * want to include but that are not explicitly listed.
	 */
	if (chanlookup(avail->ic_chans, avail->ic_nchans, freq, flags) != NULL)
		return 1;
	if (flags & IEEE80211_CHAN_GSM) {
		/*
		 * XXX GSM frequency mapping is handled in the kernel
		 * so we cannot find them in the calibration table;
		 * just accept the channel and the kernel will reject
		 * the channel list if it's wrong.
		 */
		return 1;
	}
	/*
	 * If this is a 1/2 or 1/4 width channel allow it if a full
	 * width channel is present for this frequency, and the device
	 * supports fractional channels on this band.  This is a hack
	 * that avoids bloating the calibration table; it may be better
	 * by per-band attributes though (we are effectively calculating
	 * this attribute by scanning the channel list ourself).
	 */
	if ((flags & (IEEE80211_CHAN_HALF | IEEE80211_CHAN_QUARTER)) == 0)
		return 0;
	if (chanlookup(avail->ic_chans, avail->ic_nchans, freq,
	    flags &~ (IEEE80211_CHAN_HALF | IEEE80211_CHAN_QUARTER)) == NULL)
		return 0;
	if (flags & IEEE80211_CHAN_HALF) {
		return chanfind(avail->ic_chans, avail->ic_nchans,
		    IEEE80211_CHAN_HALF |
		       (flags & (IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_5GHZ)));
	} else {
		return chanfind(avail->ic_chans, avail->ic_nchans,
		    IEEE80211_CHAN_QUARTER |
			(flags & (IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_5GHZ)));
	}
}

static void
regdomain_addchans(if_ctx *ctx, struct ieee80211req_chaninfo *ci,
	const netband_head *bands,
	const struct ieee80211_regdomain *reg,
	uint32_t chanFlags,
	const struct ieee80211req_chaninfo *avail)
{
	const struct netband *nb;
	const struct freqband *b;
	struct ieee80211_channel *c, *prev;
	int freq, hi_adj, lo_adj, channelSep;
	uint32_t flags;
	const int verbose = ctx->args->verbose;

	hi_adj = (chanFlags & IEEE80211_CHAN_HT40U) ? -20 : 0;
	lo_adj = (chanFlags & IEEE80211_CHAN_HT40D) ? 20 : 0;
	channelSep = (chanFlags & IEEE80211_CHAN_2GHZ) ? 0 : 40;

	LIST_FOREACH(nb, bands, next) {
		b = nb->band;
		if (verbose) {
			ifieee80211_print_regdomain_addchans(__func__);
			ifconfig_printb(" chanFlags", chanFlags, IEEE80211_CHAN_BITS);
			ifconfig_printb(" bandFlags", nb->flags | b->flags,
			    IEEE80211_CHAN_BITS);
			ifconfig_print_newline();
		}
		prev = NULL;

		for (freq = b->freqStart + lo_adj;
		     freq <= b->freqEnd + hi_adj; freq += b->chanSep) {
			/*
			 * Construct flags for the new channel.  We take
			 * the attributes from the band descriptions except
			 * for HT40 which is enabled generically (i.e. +/-
			 * extension channel) in the band description and
			 * then constrained according by channel separation.
			 */
			flags = nb->flags | b->flags;

			/*
			 * VHT first - HT is a subset.
			 */
			if (flags & IEEE80211_CHAN_VHT) {
				if ((chanFlags & IEEE80211_CHAN_VHT20) &&
				    (flags & IEEE80211_CHAN_VHT20) == 0) {
					if (verbose)
						ifieee80211_print_verbose_vht20_skip(freq);
					continue;
				}
				if ((chanFlags & IEEE80211_CHAN_VHT40) &&
				    (flags & IEEE80211_CHAN_VHT40) == 0) {
					if (verbose)
						ifieee80211_print_verbose_vht40_skip(freq);
					continue;
				}
				if ((chanFlags & IEEE80211_CHAN_VHT80) &&
				    (flags & IEEE80211_CHAN_VHT80) == 0) {
					if (verbose)
						ifieee80211_print_verbose_vht80_skip(freq);
					continue;
				}
				if ((chanFlags & IEEE80211_CHAN_VHT160) &&
				    (flags & IEEE80211_CHAN_VHT160) == 0) {
					if (verbose)
						ifieee80211_print_verbose_vht160_skip(freq);
					continue;
				}
				if ((chanFlags & IEEE80211_CHAN_VHT80P80) &&
				    (flags & IEEE80211_CHAN_VHT80P80) == 0) {
					if (verbose)
						ifieee80211_print_verbose_vht80p80_skip(freq);
					continue;
				}
				flags &= ~IEEE80211_CHAN_VHT;
				flags |= chanFlags & IEEE80211_CHAN_VHT;
			}

			/* Now, constrain HT */
			if (flags & IEEE80211_CHAN_HT) {
				/*
				 * HT channels are generated specially; we're
				 * called to add HT20, HT40+, and HT40- chan's
				 * so we need to expand only band specs for
				 * the HT channel type being added.
				 */
				if ((chanFlags & IEEE80211_CHAN_HT20) &&
				    (flags & IEEE80211_CHAN_HT20) == 0) {
					if (verbose)
						ifieee80211_print_verbose_ht20_skip(freq);
					continue;
				}
				if ((chanFlags & IEEE80211_CHAN_HT40) &&
				    (flags & IEEE80211_CHAN_HT40) == 0) {
					if (verbose)
						ifieee80211_print_verbose_ht40_skip(freq);
					continue;
				}
				/* NB: HT attribute comes from caller */
				flags &= ~IEEE80211_CHAN_HT;
				flags |= chanFlags & IEEE80211_CHAN_HT;
			}
			/*
			 * Check if device can operate on this frequency.
			 */
			if (!checkchan(avail, freq, flags)) {
				if (verbose) {
					ifieee80211_print_verbose_freq_skip(freq);
					ifconfig_printb("flags", flags,
					    IEEE80211_CHAN_BITS);
					ifieee80211_print_verbose_checkchan_notavail();
				}
				continue;
			}
			if ((flags & REQ_ECM) && !reg->ecm) {
				if (verbose)
					ifieee80211_print_verbose_ecm_chan(freq);
				continue;
			}
			if ((flags & REQ_INDOOR) && reg->location == 'O') {
				if (verbose)
					ifieee80211_print_verbose_indoor_chan_skip(freq);
				continue;
			}
			if ((flags & REQ_OUTDOOR) && reg->location == 'I') {
				if (verbose)
					ifieee80211_print_verbose_outdoor_chan_skip(freq);
				continue;
			}
			if ((flags & IEEE80211_CHAN_HT40) &&
			    prev != NULL && (freq - prev->ic_freq) < channelSep) {
				if (verbose)
					ifieee80211_print_verbose_chansep_need(freq,
					    freq - prev->ic_freq, channelSep);
				continue;
			}
			if (ci->ic_nchans == IEEE80211_CHAN_MAX) {
				if (verbose)
					ifieee80211_print_verbose_chan_table_full(freq);
				break;
			}
			c = &ci->ic_chans[ci->ic_nchans++];
			memset(c, 0, sizeof(*c));
			c->ic_freq = freq;
			c->ic_flags = flags;
			if (c->ic_flags & IEEE80211_CHAN_DFS)
				c->ic_maxregpower = nb->maxPowerDFS;
			else
				c->ic_maxregpower = nb->maxPower;
			if (verbose) {
				ifieee80211_print_verbose_add_freq(ci, c);
				ifconfig_printb("flags", c->ic_flags, IEEE80211_CHAN_BITS);
				ifieee80211_print_verbose_maxregpower(c);
			}
			/* NB: kernel fills in other fields */
			prev = c;
		}
	}
}

static void
regdomain_makechannels(
	if_ctx *ctx,
	struct ieee80211_regdomain_req *req,
	const struct ieee80211_devcaps_req *dc)
{
	struct regdata *rdp = getregdata();
	const struct country *cc;
	const struct ieee80211_regdomain *reg = &req->rd;
	struct ieee80211req_chaninfo *ci = &req->chaninfo;
	const struct regdomain *rd;

	/*
	 * Locate construction table for new channel list.  We treat
	 * the regdomain/SKU as definitive so a country can be in
	 * multiple with different properties (e.g. US in FCC+FCC3).
	 * If no regdomain is specified then we fallback on the country
	 * code to find the associated regdomain since countries always
	 * belong to at least one regdomain.
	 */
	if (reg->regdomain == 0) {
		cc = lib80211_country_findbycc(rdp, reg->country);
		if (cc == NULL)
			if_errx(1, "internal error, country %d not found",
			    reg->country);
		rd = cc->rd;
	} else
		rd = lib80211_regdomain_findbysku(rdp, reg->regdomain);
	if (rd == NULL)
		if_errx(1, "internal error, regdomain %d not found",
			    reg->regdomain);
	if (rd->sku != SKU_DEBUG) {
		/*
		 * regdomain_addchans incrememnts the channel count for
		 * each channel it adds so initialize ic_nchans to zero.
		 * Note that we know we have enough space to hold all possible
		 * channels because the devcaps list size was used to
		 * allocate our request.
		 */
		ci->ic_nchans = 0;
		if (!LIST_EMPTY(&rd->bands_11b))
			regdomain_addchans(ctx, ci, &rd->bands_11b, reg,
			    IEEE80211_CHAN_B, &dc->dc_chaninfo);
		if (!LIST_EMPTY(&rd->bands_11g))
			regdomain_addchans(ctx, ci, &rd->bands_11g, reg,
			    IEEE80211_CHAN_G, &dc->dc_chaninfo);
		if (!LIST_EMPTY(&rd->bands_11a))
			regdomain_addchans(ctx, ci, &rd->bands_11a, reg,
			    IEEE80211_CHAN_A, &dc->dc_chaninfo);
		if (!LIST_EMPTY(&rd->bands_11na) && dc->dc_htcaps != 0) {
			regdomain_addchans(ctx, ci, &rd->bands_11na, reg,
			    IEEE80211_CHAN_A | IEEE80211_CHAN_HT20,
			    &dc->dc_chaninfo);
			if (dc->dc_htcaps & IEEE80211_HTCAP_CHWIDTH40) {
				regdomain_addchans(ctx, ci, &rd->bands_11na, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40U,
				    &dc->dc_chaninfo);
				regdomain_addchans(ctx, ci, &rd->bands_11na, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40D,
				    &dc->dc_chaninfo);
			}
		}
		if (!LIST_EMPTY(&rd->bands_11ac) && dc->dc_vhtcaps != 0) {
			regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
			    IEEE80211_CHAN_A | IEEE80211_CHAN_HT20 |
			    IEEE80211_CHAN_VHT20,
			    &dc->dc_chaninfo);

			/* VHT40 is a function of HT40.. */
			if (dc->dc_htcaps & IEEE80211_HTCAP_CHWIDTH40) {
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40U |
				    IEEE80211_CHAN_VHT40U,
				    &dc->dc_chaninfo);
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40D |
				    IEEE80211_CHAN_VHT40D,
				    &dc->dc_chaninfo);
			}

			/* VHT80 is mandatory (and so should be VHT40 above). */
			if (1) {
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40U |
				    IEEE80211_CHAN_VHT80,
				    &dc->dc_chaninfo);
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40D |
				    IEEE80211_CHAN_VHT80,
				    &dc->dc_chaninfo);
			}

			/* VHT160 */
			if (IEEE80211_VHTCAP_SUPP_CHAN_WIDTH_IS_160MHZ(
			    dc->dc_vhtcaps)) {
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40U |
				    IEEE80211_CHAN_VHT160,
				    &dc->dc_chaninfo);
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40D |
				    IEEE80211_CHAN_VHT160,
				    &dc->dc_chaninfo);
			}

			/* VHT80P80 */
			if (IEEE80211_VHTCAP_SUPP_CHAN_WIDTH_IS_160_80P80MHZ(
			    dc->dc_vhtcaps)) {
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40U |
				    IEEE80211_CHAN_VHT80P80,
				    &dc->dc_chaninfo);
				regdomain_addchans(ctx, ci, &rd->bands_11ac, reg,
				    IEEE80211_CHAN_A | IEEE80211_CHAN_HT40D |
				    IEEE80211_CHAN_VHT80P80,
				    &dc->dc_chaninfo);
			}
		}

		if (!LIST_EMPTY(&rd->bands_11ng) && dc->dc_htcaps != 0) {
			regdomain_addchans(ctx, ci, &rd->bands_11ng, reg,
			    IEEE80211_CHAN_G | IEEE80211_CHAN_HT20,
			    &dc->dc_chaninfo);
			if (dc->dc_htcaps & IEEE80211_HTCAP_CHWIDTH40) {
				regdomain_addchans(ctx, ci, &rd->bands_11ng, reg,
				    IEEE80211_CHAN_G | IEEE80211_CHAN_HT40U,
				    &dc->dc_chaninfo);
				regdomain_addchans(ctx, ci, &rd->bands_11ng, reg,
				    IEEE80211_CHAN_G | IEEE80211_CHAN_HT40D,
				    &dc->dc_chaninfo);
			}
		}
		qsort(ci->ic_chans, ci->ic_nchans, sizeof(ci->ic_chans[0]),
		    regdomain_sort);
	} else
		memcpy(ci, &dc->dc_chaninfo,
		    IEEE80211_CHANINFO_SPACE(&dc->dc_chaninfo));
}

static void
list_countries(void)
{
	struct regdata *rdp = getregdata();
	const struct country *cp;
	const struct regdomain *dp;
	int i;

	i = 0;
	ifieee80211_print_country_codes();
	LIST_FOREACH(cp, &rdp->countries, next) {
		ifieee80211_print_country_code(cp, i);
		i++;
	}
	i = 0;
	ifieee80211_print_reg_domains();
	LIST_FOREACH(dp, &rdp->domains, next) {
		ifieee80211_print_reg_domain(dp, i);
		i++;
	}
	ifconfig_print_newline();
}

static void
defaultcountry(const struct regdomain *rd)
{
	struct regdata *rdp = getregdata();
	const struct country *cc;

	cc = lib80211_country_findbycc(rdp, rd->cc->code);
	if (cc == NULL)
		if_errx(1, "internal error, ISO country code %d not "
		    "defined for regdomain %s", rd->cc->code, rd->name);
	regdomain.country = cc->code;
	regdomain.isocc[0] = cc->isoname[0];
	regdomain.isocc[1] = cc->isoname[1];
}

static void
set80211regdomain(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct regdata *rdp = getregdata();
	const struct regdomain *rd;

	rd = lib80211_regdomain_findbyname(rdp, val);
	if (rd == NULL) {
		char *eptr;
		long sku = strtol(val, &eptr, 0);

		if (eptr != val)
			rd = lib80211_regdomain_findbysku(rdp, sku);
		if (eptr == val || rd == NULL)
			if_errx(1, "unknown regdomain %s", val);
	}
	getregdomain(ctx);
	regdomain.regdomain = rd->sku;
	if (regdomain.country == 0 && rd->cc != NULL) {
		/*
		 * No country code setup and there's a default
		 * one for this regdomain fill it in.
		 */
		defaultcountry(rd);
	}
	callback_register(setregdomain_cb, &regdomain);
}

static void
set80211country(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct regdata *rdp = getregdata();
	const struct country *cc;

	cc = lib80211_country_findbyname(rdp, val);
	if (cc == NULL) {
		char *eptr;
		long code = strtol(val, &eptr, 0);

		if (eptr != val)
			cc = lib80211_country_findbycc(rdp, code);
		if (eptr == val || cc == NULL)
			if_errx(1, "unknown ISO country code %s", val);
	}
	getregdomain(ctx);
	regdomain.regdomain = cc->rd->sku;
	regdomain.country = cc->code;
	regdomain.isocc[0] = cc->isoname[0];
	regdomain.isocc[1] = cc->isoname[1];
	callback_register(setregdomain_cb, &regdomain);
}

static void
set80211location(if_ctx *ctx, const char *val __unused, int d)
{
	getregdomain(ctx);
	regdomain.location = d;
	callback_register(setregdomain_cb, &regdomain);
}

static void
set80211ecm(if_ctx *ctx, const char *val __unused, int d)
{
	getregdomain(ctx);
	regdomain.ecm = d;
	callback_register(setregdomain_cb, &regdomain);
}

int
getmaxrate(const uint8_t rates[15], uint8_t nrates)
{
	int i, maxrate = -1;

	for (i = 0; i < nrates; i++) {
		int rate = rates[i] & IEEE80211_RATE_VAL;
		if (rate > maxrate)
			maxrate = rate;
	}
	return maxrate / 2;
}

const char *
getcaps(int capinfo)
{
	static char capstring[32];
	char *cp = capstring;

	if (capinfo & IEEE80211_CAPINFO_ESS)
		*cp++ = 'E';
	if (capinfo & IEEE80211_CAPINFO_IBSS)
		*cp++ = 'I';
	if (capinfo & IEEE80211_CAPINFO_CF_POLLABLE)
		*cp++ = 'c';
	if (capinfo & IEEE80211_CAPINFO_CF_POLLREQ)
		*cp++ = 'C';
	if (capinfo & IEEE80211_CAPINFO_PRIVACY)
		*cp++ = 'P';
	if (capinfo & IEEE80211_CAPINFO_SHORT_PREAMBLE)
		*cp++ = 'S';
	if (capinfo & IEEE80211_CAPINFO_PBCC)
		*cp++ = 'B';
	if (capinfo & IEEE80211_CAPINFO_CHNL_AGILITY)
		*cp++ = 'A';
	if (capinfo & IEEE80211_CAPINFO_SHORT_SLOTTIME)
		*cp++ = 's';
	if (capinfo & IEEE80211_CAPINFO_RSN)
		*cp++ = 'R';
	if (capinfo & IEEE80211_CAPINFO_DSSSOFDM)
		*cp++ = 'D';
	*cp = '\0';
	return capstring;
}

const char *
getflags(int flags)
{
	static char flagstring[32];
	char *cp = flagstring;

	if (flags & IEEE80211_NODE_AUTH)
		*cp++ = 'A';
	if (flags & IEEE80211_NODE_QOS)
		*cp++ = 'Q';
	if (flags & IEEE80211_NODE_ERP)
		*cp++ = 'E';
	if (flags & IEEE80211_NODE_PWR_MGT)
		*cp++ = 'P';
	if (flags & IEEE80211_NODE_HT) {
		*cp++ = 'H';
		if (flags & IEEE80211_NODE_HTCOMPAT)
			*cp++ = '+';
	}
	if (flags & IEEE80211_NODE_VHT)
		*cp++ = 'V';
	if (flags & IEEE80211_NODE_WPS)
		*cp++ = 'W';
	if (flags & IEEE80211_NODE_TSN)
		*cp++ = 'N';
	if (flags & IEEE80211_NODE_AMPDU_TX)
		*cp++ = 'T';
	if (flags & IEEE80211_NODE_AMPDU_RX)
		*cp++ = 'R';
	if (flags & IEEE80211_NODE_MIMO_PS) {
		*cp++ = 'M';
		if (flags & IEEE80211_NODE_MIMO_RTS)
			*cp++ = '+';
	}
	if (flags & IEEE80211_NODE_RIFS)
		*cp++ = 'I';
	if (flags & IEEE80211_NODE_SGI40) {
		*cp++ = 'S';
		if (flags & IEEE80211_NODE_SGI20)
			*cp++ = '+';
	} else if (flags & IEEE80211_NODE_SGI20)
		*cp++ = 's';
	if (flags & IEEE80211_NODE_AMSDU_TX)
		*cp++ = 't';
	if (flags & IEEE80211_NODE_AMSDU_RX)
		*cp++ = 'r';
	if (flags & IEEE80211_NODE_UAPSD)
		*cp++ = 'U';
	if (flags & IEEE80211_NODE_LDPC)
		*cp++ = 'L';
	*cp = '\0';
	return flagstring;
}

const char *
wpa_cipher(const u_int8_t *sel)
{
#define	WPA_SEL(x)	(((x)<<24)|WPA_OUI)
	u_int32_t w = LE_READ_4(sel);

	switch (w) {
	case WPA_SEL(WPA_CSE_NULL):
		return "NONE";
	case WPA_SEL(WPA_CSE_WEP40):
		return "WEP40";
	case WPA_SEL(WPA_CSE_WEP104):
		return "WEP104";
	case WPA_SEL(WPA_CSE_TKIP):
		return "TKIP";
	case WPA_SEL(WPA_CSE_CCMP):
		return "AES-CCMP";
	}
	return "?";		/* NB: so 1<< is discarded */
#undef WPA_SEL
}

const char *
wpa_keymgmt(const u_int8_t *sel)
{
#define	WPA_SEL(x)	(((x)<<24)|WPA_OUI)
	u_int32_t w = LE_READ_4(sel);

	switch (w) {
	case WPA_SEL(WPA_ASE_8021X_UNSPEC):
		return "8021X-UNSPEC";
	case WPA_SEL(WPA_ASE_8021X_PSK):
		return "8021X-PSK";
	case WPA_SEL(WPA_ASE_NONE):
		return "NONE";
	}
	return "?";
#undef WPA_SEL
}

const char *
rsn_cipher(const u_int8_t *sel)
{
#define	RSN_SEL(x)	(((x)<<24)|RSN_OUI)
	u_int32_t w = LE_READ_4(sel);

	switch (w) {
	case RSN_SEL(RSN_CSE_NULL):
		return "NONE";
	case RSN_SEL(RSN_CSE_WEP40):
		return "WEP40";
	case RSN_SEL(RSN_CSE_WEP104):
		return "WEP104";
	case RSN_SEL(RSN_CSE_TKIP):
		return "TKIP";
	case RSN_SEL(RSN_CSE_CCMP):
		return "AES-CCMP";
	case RSN_SEL(RSN_CSE_WRAP):
		return "AES-OCB";
	case RSN_SEL(RSN_CSE_GCMP_128):
		return "AES-GCMP";
	case RSN_SEL(RSN_CSE_CCMP_256):
		return "AES-CCMP-256";
	case RSN_SEL(RSN_CSE_GCMP_256):
		return "AES-GCMP-256";
	}
	return "?";
#undef WPA_SEL
}

const char *
rsn_keymgmt(const u_int8_t *sel)
{
#define	RSN_SEL(x)	(((x)<<24)|RSN_OUI)
	u_int32_t w = LE_READ_4(sel);

	switch (w) {
	case RSN_SEL(RSN_ASE_8021X_UNSPEC):
		return "8021X-UNSPEC";
	case RSN_SEL(RSN_ASE_8021X_PSK):
		return "8021X-PSK";
	case RSN_SEL(RSN_ASE_8021X_UNSPEC_SHA256):
		return "8021X-UNSPEC-SHA256";
	case RSN_SEL(RSN_ASE_8021X_PSK_SHA256):
		return "8021X-PSK-256";
	case RSN_SEL(RSN_ASE_NONE):
		return "NONE";
	}
	return "?";
#undef RSN_SEL
}

/*
 * Copy the ssid string contents into buf, truncating to fit.  If the
 * ssid is entirely printable then just copy intact.  Otherwise convert
 * to hexadecimal.  If the result is truncated then replace the last
 * three characters with "...".
 */
int
copy_essid(char buf[], size_t bufsize, const u_int8_t *essid, size_t essid_len)
{
	const u_int8_t *p; 
	size_t maxlen;
	u_int i;

	if (essid_len > bufsize)
		maxlen = bufsize;
	else
		maxlen = essid_len;
	/* determine printable or not */
	for (i = 0, p = essid; i < maxlen; i++, p++) {
		if (*p < ' ' || *p > 0x7e)
			break;
	}
	if (i != maxlen) {		/* not printable, print as hex */
		if (bufsize < 3)
			return 0;
		strlcpy(buf, "0x", bufsize);
		bufsize -= 2;
		p = essid;
		for (i = 0; i < maxlen && bufsize >= 2; i++) {
			sprintf(&buf[2+2*i], "%02x", p[i]);
			bufsize -= 2;
		}
		if (i != essid_len)
			memcpy(&buf[2+2*i-3], "...", 3);
	} else {			/* printable, truncate as needed */
		memcpy(buf, essid, maxlen);
		if (maxlen != essid_len)
			memcpy(&buf[maxlen-3], "...", 3);
	}
	return maxlen;
}

__inline int
iswpaoui(const u_int8_t *frm)
{
	return frm[1] > 3 && LE_READ_4(frm+2) == ((WPA_OUI_TYPE<<24)|WPA_OUI);
}

__inline int
iswmeinfo(const u_int8_t *frm)
{
	return frm[1] > 5 && LE_READ_4(frm+2) == ((WME_OUI_TYPE<<24)|WME_OUI) &&
		frm[6] == WME_INFO_OUI_SUBTYPE;
}

__inline int
iswmeparam(const u_int8_t *frm)
{
	return frm[1] > 5 && LE_READ_4(frm+2) == ((WME_OUI_TYPE<<24)|WME_OUI) &&
		frm[6] == WME_PARAM_OUI_SUBTYPE;
}

__inline int
isatherosoui(const u_int8_t *frm)
{
	return frm[1] > 3 && LE_READ_4(frm+2) == ((ATH_OUI_TYPE<<24)|ATH_OUI);
}

__inline int
istdmaoui(const uint8_t *frm)
{
	return frm[1] > 3 && LE_READ_4(frm+2) == ((TDMA_OUI_TYPE<<24)|TDMA_OUI);
}

__inline int
iswpsoui(const uint8_t *frm)
{
	return frm[1] > 3 && LE_READ_4(frm+2) == ((WPS_OUI_TYPE<<24)|WPA_OUI);
}

static const char *
ie_ext_name(uint8_t ext_elemid)
{
	static char iename_buf[32];

	switch (ext_elemid) {
	case IEEE80211_ELEMID_EXT_HE_CAPA:		return " HECAP";
	case IEEE80211_ELEMID_EXT_HE_OPER:		return " HEOPER";
	case IEEE80211_ELEMID_EXT_MU_EDCA_PARAM_SET:	return " MU_EDCA_PARAM_SET";
	}
	snprintf(iename_buf, sizeof(iename_buf), " ELEMID_EXT_%d",
	    ext_elemid & 0xff);
	return (const char *) iename_buf;
}

const char *
iename(uint8_t elemid, const u_int8_t *vp)
{
	static char iename_buf[64];
	switch (elemid) {
	case IEEE80211_ELEMID_FHPARMS:	return " FHPARMS";
	case IEEE80211_ELEMID_CFPARMS:	return " CFPARMS";
	case IEEE80211_ELEMID_TIM:	return " TIM";
	case IEEE80211_ELEMID_IBSSPARMS:return " IBSSPARMS";
	case IEEE80211_ELEMID_BSSLOAD:	return " BSSLOAD";
	case IEEE80211_ELEMID_CHALLENGE:return " CHALLENGE";
	case IEEE80211_ELEMID_PWRCNSTR:	return " PWRCNSTR";
	case IEEE80211_ELEMID_PWRCAP:	return " PWRCAP";
	case IEEE80211_ELEMID_TPCREQ:	return " TPCREQ";
	case IEEE80211_ELEMID_TPCREP:	return " TPCREP";
	case IEEE80211_ELEMID_SUPPCHAN:	return " SUPPCHAN";
	case IEEE80211_ELEMID_CSA:	return " CSA";
	case IEEE80211_ELEMID_MEASREQ:	return " MEASREQ";
	case IEEE80211_ELEMID_MEASREP:	return " MEASREP";
	case IEEE80211_ELEMID_QUIET:	return " QUIET";
	case IEEE80211_ELEMID_IBSSDFS:	return " IBSSDFS";
	case IEEE80211_ELEMID_RESERVED_47:
					return " RESERVED_47";
	case IEEE80211_ELEMID_SUP_OP_CLASS:
					return " SUP_OP_CLASS";
	case IEEE80211_ELEMID_MOBILITY_DOMAIN:
					return " MOBILITY_DOMAIN";
	case IEEE80211_ELEMID_RRM_ENACAPS:
					return " RRM_ENCAPS";
	case IEEE80211_ELEMID_OVERLAP_BSS_SCAN_PARAM:
					return " OVERLAP_BSS";
	case IEEE80211_ELEMID_TPC:	return " TPC";
	case IEEE80211_ELEMID_CCKM:	return " CCKM";
	case IEEE80211_ELEMID_EXTCAP:	return " EXTCAP";
	case IEEE80211_ELEMID_RSN_EXT:	return " RSNXE";
	case IEEE80211_ELEMID_EXTFIELD:
		if (vp[1] >= 1)
			return ie_ext_name(vp[2]);
		break;
	}
	snprintf(iename_buf, sizeof(iename_buf), " ELEMID_%d",
	    elemid);
	return (const char *) iename_buf;
}

static void
list_scan(if_ctx *ctx)
{
	uint8_t buf[24*1024];
	const uint8_t *cp;
	int len, idlen;

	if (get80211len(ctx, IEEE80211_IOC_SCAN_RESULTS, buf, sizeof(buf), &len) < 0)
		if_errx(1, "unable to get scan results");
	if (len < (int)sizeof(struct ieee80211req_scan_result))
		return;

	getchaninfo(ctx);

	ifieee80211_print_list_scan_hdr();
	cp = buf;
	do {
		const struct ieee80211req_scan_result *sr;
		const uint8_t *vp, *idp;

		sr = (const struct ieee80211req_scan_result *)(const void *) cp;
		vp = cp + sr->isr_ie_off;
		if (sr->isr_meshid_len) {
			idp = vp + sr->isr_ssid_len;
			idlen = sr->isr_meshid_len;
		} else {
			idp = vp;
			idlen = sr->isr_ssid_len;
		}
		ifieee80211_print_list_scan_row(sr, idp, idlen);
		ifieee80211_printies(ctx, vp + sr->isr_ssid_len + sr->isr_meshid_len,
		    sr->isr_ie_len, 24);
		ifieee80211_printbssidname((const struct ether_addr *)sr->isr_bssid);
		ifconfig_print_newline();
		cp += sr->isr_len, len -= sr->isr_len;
	} while (len >= (int)sizeof(struct ieee80211req_scan_result));
}

static void
scan_and_wait(if_ctx *ctx)
{
	struct ieee80211_scan_req sr;
	struct ieee80211req ireq;
	int sroute;

	sroute = socket(PF_ROUTE, SOCK_RAW, 0);
	if (sroute < 0) {
		perror("socket(PF_ROUTE,SOCK_RAW)");
		return;
	}
	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, ctx->ifname, sizeof(ireq.i_name));
	ireq.i_type = IEEE80211_IOC_SCAN_REQ;

	memset(&sr, 0, sizeof(sr));
	sr.sr_flags = IEEE80211_IOC_SCAN_ACTIVE
		    | IEEE80211_IOC_SCAN_BGSCAN
		    | IEEE80211_IOC_SCAN_NOPICK
		    | IEEE80211_IOC_SCAN_ONCE;
	sr.sr_duration = IEEE80211_IOC_SCAN_FOREVER;
	sr.sr_nssid = 0;

	ireq.i_data = &sr;
	ireq.i_len = sizeof(sr);
	/*
	 * NB: only root can trigger a scan so ignore errors. Also ignore
	 * possible errors from net80211, even if no new scan could be
	 * started there might still be a valid scan cache.
	 */
	if (ioctl_ctx(ctx, SIOCS80211, &ireq) == 0) {
		char buf[2048];
		struct if_announcemsghdr *ifan;
		struct rt_msghdr *rtm;

		do {
			if (read(sroute, buf, sizeof(buf)) < 0) {
				perror("read(PF_ROUTE)");
				break;
			}
			rtm = (struct rt_msghdr *)(void *)buf;
			if (rtm->rtm_version != RTM_VERSION)
				break;
			ifan = (struct if_announcemsghdr *) rtm;
		} while (rtm->rtm_type != RTM_IEEE80211 ||
		    ifan->ifan_what != RTM_IEEE80211_SCAN);
	}
	close(sroute);
}

static void
set80211scan(if_ctx *ctx, const char *val __unused, int dummy __unused)
{
	scan_and_wait(ctx);
	list_scan(ctx);
}

static enum ieee80211_opmode get80211opmode(if_ctx *ctx);

int
gettxseq(const struct ieee80211req_sta_info *si)
{
	int i, txseq;

	if ((si->isi_state & IEEE80211_NODE_QOS) == 0)
		return si->isi_txseqs[0];
	/* XXX not right but usually what folks want */
	txseq = 0;
	for (i = 0; i < IEEE80211_TID_SIZE; i++)
		if (si->isi_txseqs[i] > txseq)
			txseq = si->isi_txseqs[i];
	return txseq;
}

int
getrxseq(const struct ieee80211req_sta_info *si)
{
	int rxseq;

	if ((si->isi_state & IEEE80211_NODE_QOS) == 0)
		return si->isi_rxseqs[0];
	/* XXX not right but usually what folks want */
	rxseq = 0;
	for (unsigned int i = 0; i < IEEE80211_TID_SIZE; i++)
		if (si->isi_rxseqs[i] > rxseq)
			rxseq = si->isi_rxseqs[i];
	return rxseq;
}

static void
list_stations(if_ctx *ctx)
{
	union {
		struct ieee80211req_sta_req req;
		uint8_t buf[24*1024];
	} u;
	enum ieee80211_opmode opmode = get80211opmode(ctx);
	const uint8_t *cp;
	int len;

	/* broadcast address =>'s get all stations */
	(void) memset(u.req.is_u.macaddr, 0xff, IEEE80211_ADDR_LEN);
	if (opmode == IEEE80211_M_STA) {
		/*
		 * Get information about the associated AP.
		 */
		(void) get80211(ctx, IEEE80211_IOC_BSSID,
		    u.req.is_u.macaddr, IEEE80211_ADDR_LEN);
	}
	if (get80211len(ctx, IEEE80211_IOC_STA_INFO, &u, sizeof(u), &len) < 0)
		if_errx(1, "unable to get station information");
	if (len < (int)sizeof(struct ieee80211req_sta_info))
		return;

	getchaninfo(ctx);

	if (opmode == IEEE80211_M_MBSS)
		ifieee80211_print_list_stations_hdr();
	else
		ifieee80211_print_list_stations_hdr2();
	cp = (const uint8_t *) u.req.info;
	do {
		const struct ieee80211req_sta_info *si;

		si = (const struct ieee80211req_sta_info *)(const void *)cp;
		if (si->isi_len < sizeof(*si))
			break;
		if (opmode == IEEE80211_M_MBSS)
			ifieee80211_print_list_stations_row(si);
		else
			ifieee80211_print_list_stations_row2(si);
		ifieee80211_printies(ctx, cp + si->isi_ie_off, si->isi_ie_len, 24);
		ifieee80211_printmimo(&si->isi_mimo);
		ifconfig_print_newline();
		cp += si->isi_len, len -= si->isi_len;
	} while (len >= (int)sizeof(struct ieee80211req_sta_info));
}

const char *
mesh_linkstate_string(uint8_t state)
{
	static const char *state_names[] = {
	    [0] = "IDLE",
	    [1] = "OPEN-TX",
	    [2] = "OPEN-RX",
	    [3] = "CONF-RX",
	    [4] = "ESTAB",
	    [5] = "HOLDING",
	};

	if (state >= nitems(state_names)) {
		static char buf[10];
		snprintf(buf, sizeof(buf), "#%u", state);
		return buf;
	} else
		return state_names[state];
}

const char *
get_chaninfo(const struct ieee80211_channel *c, int precise,
	char buf[], size_t bsize)
{
	buf[0] = '\0';
	if (IEEE80211_IS_CHAN_FHSS(c))
		strlcat(buf, " FHSS", bsize);
	if (IEEE80211_IS_CHAN_A(c))
		strlcat(buf, " 11a", bsize);
	else if (IEEE80211_IS_CHAN_ANYG(c))
		strlcat(buf, " 11g", bsize);
	else if (IEEE80211_IS_CHAN_B(c))
		strlcat(buf, " 11b", bsize);
	if (IEEE80211_IS_CHAN_HALF(c))
		strlcat(buf, "/10MHz", bsize);
	if (IEEE80211_IS_CHAN_QUARTER(c))
		strlcat(buf, "/5MHz", bsize);
	if (IEEE80211_IS_CHAN_TURBO(c))
		strlcat(buf, " Turbo", bsize);
	if (precise) {
		if (IEEE80211_IS_CHAN_VHT80P80(c))
			strlcat(buf, " vht/80p80", bsize);
		else if (IEEE80211_IS_CHAN_VHT160(c))
			strlcat(buf, " vht/160", bsize);
		else if (IEEE80211_IS_CHAN_VHT80(c) &&
		    IEEE80211_IS_CHAN_HT40D(c))
			strlcat(buf, " vht/80-", bsize);
		else if (IEEE80211_IS_CHAN_VHT80(c) &&
		    IEEE80211_IS_CHAN_HT40U(c))
			strlcat(buf, " vht/80+", bsize);
		else if (IEEE80211_IS_CHAN_VHT80(c))
			strlcat(buf, " vht/80", bsize);
		else if (IEEE80211_IS_CHAN_VHT40D(c))
			strlcat(buf, " vht/40-", bsize);
		else if (IEEE80211_IS_CHAN_VHT40U(c))
			strlcat(buf, " vht/40+", bsize);
		else if (IEEE80211_IS_CHAN_VHT20(c))
			strlcat(buf, " vht/20", bsize);
		else if (IEEE80211_IS_CHAN_HT20(c))
			strlcat(buf, " ht/20", bsize);
		else if (IEEE80211_IS_CHAN_HT40D(c))
			strlcat(buf, " ht/40-", bsize);
		else if (IEEE80211_IS_CHAN_HT40U(c))
			strlcat(buf, " ht/40+", bsize);
	} else {
		if (IEEE80211_IS_CHAN_VHT(c))
			strlcat(buf, " vht", bsize);
		else if (IEEE80211_IS_CHAN_HT(c))
			strlcat(buf, " ht", bsize);
	}
	return buf;
}

int
chanpref(const struct ieee80211_channel *c)
{

	if (IEEE80211_IS_CHAN_VHT80P80(c))
		return 90;
	if (IEEE80211_IS_CHAN_VHT160(c))
		return 80;
	if (IEEE80211_IS_CHAN_VHT80(c))
		return 70;
	if (IEEE80211_IS_CHAN_VHT40(c))
		return 60;
	if (IEEE80211_IS_CHAN_VHT20(c))
		return 50;
	if (IEEE80211_IS_CHAN_HT40(c))
		return 40;
	if (IEEE80211_IS_CHAN_HT20(c))
		return 30;
	if (IEEE80211_IS_CHAN_HALF(c))
		return 10;
	if (IEEE80211_IS_CHAN_QUARTER(c))
		return 5;
	if (IEEE80211_IS_CHAN_TURBO(c))
		return 25;
	if (IEEE80211_IS_CHAN_A(c))
		return 20;
	if (IEEE80211_IS_CHAN_G(c))
		return 20;
	if (IEEE80211_IS_CHAN_B(c))
		return 15;
	if (IEEE80211_IS_CHAN_PUREG(c))
		return 15;
	return 0;
}

static void
list_channels(if_ctx *ctx, int allchans)
{
	getchaninfo(ctx);
	ifieee80211_print_channels(ctx, chaninfo, allchans, ctx->args->verbose);
}

static void
list_txpow(if_ctx *ctx)
{
	struct ieee80211req_chaninfo *achans;
	uint8_t reported[IEEE80211_CHAN_BYTES];
	struct ieee80211_channel *c, *prev;
	unsigned int i, half;

	getchaninfo(ctx);
	achans = malloc(IEEE80211_CHANINFO_SPACE(chaninfo));
	if (achans == NULL)
		if_errx(1, "no space for active channel list");
	achans->ic_nchans = 0;
	memset(reported, 0, sizeof(reported));
	for (i = 0; i < chaninfo->ic_nchans; i++) {
		c = &chaninfo->ic_chans[i];
		/* suppress duplicates as above */
		if (isset(reported, c->ic_ieee) && !ctx->args->verbose) {
			/* XXX we assume duplicates are adjacent */
			assert(achans->ic_nchans > 0);
			prev = &achans->ic_chans[achans->ic_nchans-1];
			/* display highest power on channel */
			if (c->ic_maxpower > prev->ic_maxpower)
				*prev = *c;
		} else {
			achans->ic_chans[achans->ic_nchans++] = *c;
			setbit(reported, c->ic_ieee);
		}
	}
	if (!ctx->args->verbose) {
		half = achans->ic_nchans / 2;
		if (achans->ic_nchans % 2)
			half++;

		for (i = 0; i < achans->ic_nchans / 2; i++) {
			ifieee80211_print_txpow(&achans->ic_chans[i]);
			ifieee80211_print_txpow(&achans->ic_chans[half+i]);
			ifconfig_print_newline();
		}
		if (achans->ic_nchans % 2) {
			ifieee80211_print_txpow(&achans->ic_chans[i]);
			ifconfig_print_newline();
		}
	} else {
		for (i = 0; i < achans->ic_nchans; i++) {
			ifieee80211_print_txpow_verbose(&achans->ic_chans[i]);
			ifconfig_print_newline();
		}
	}
	free(achans);
}

static void
list_keys(int s __unused)
{
}

static void
list_capabilities(if_ctx *ctx)
{
	struct ieee80211_devcaps_req *dc;
	const int verbose = ctx->args->verbose;

	if (verbose)
		dc = malloc(IEEE80211_DEVCAPS_SIZE(MAXCHAN));
	else
		dc = malloc(IEEE80211_DEVCAPS_SIZE(1));
	if (dc == NULL)
		if_errx(1, "no space for device capabilities");
	dc->dc_chaninfo.ic_nchans = verbose ? MAXCHAN : 1;
	getdevcaps(ctx, dc);
	ifconfig_printb("drivercaps", dc->dc_drivercaps, IEEE80211_C_BITS);
	if (dc->dc_cryptocaps != 0 || verbose) {
		ifconfig_print_newline();
		ifconfig_printb("cryptocaps", dc->dc_cryptocaps, IEEE80211_CRYPTO_BITS);
	}
	if (dc->dc_htcaps != 0 || verbose) {
		ifconfig_print_newline();
		ifconfig_printb("htcaps", dc->dc_htcaps, IEEE80211_HTCAP_BITS);
	}
	if (dc->dc_vhtcaps != 0 || verbose) {
		ifconfig_print_newline();
		ifconfig_printb("vhtcaps", dc->dc_vhtcaps, IEEE80211_VHTCAP_BITS);
	}

	ifconfig_print_newline();
	if (verbose) {
		chaninfo = &dc->dc_chaninfo;	/* XXX */
		ifieee80211_print_channels(ctx, &dc->dc_chaninfo, 1/*allchans*/, verbose);
	}
	free(dc);
}

static int
get80211wme(if_ctx *ctx, int param, int ac, int *val)
{
	struct ieee80211req ireq = {};

	strlcpy(ireq.i_name, ctx->ifname, sizeof(ireq.i_name));
	ireq.i_type = param;
	ireq.i_len = ac;
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0) {
		if_warn("cannot get WME parameter %d, ac %d%s",
		    param, ac & IEEE80211_WMEPARAM_VAL,
		    ac & IEEE80211_WMEPARAM_BSS ? " (BSS)" : "");
		return -1;
	}
	*val = ireq.i_val;
	return 0;
}

static void
list_wme_aci(if_ctx *ctx, const char *tag, int ac)
{
	int val;

	ifconfig_print_tab();
	ifieee80211_print_list_wme_aci_tag(tag);

	/* show WME BSS parameters */
	if (get80211wme(ctx, IEEE80211_IOC_WME_CWMIN, ac, &val) != -1)
		ifieee80211_print_list_wme_aci_cwmin(val);
	if (get80211wme(ctx, IEEE80211_IOC_WME_CWMAX, ac, &val) != -1)
		ifieee80211_print_list_wme_aci_cwmax(val);
	if (get80211wme(ctx, IEEE80211_IOC_WME_AIFS, ac, &val) != -1)
		ifieee80211_print_list_wme_aci_aifs(val);
	if (get80211wme(ctx, IEEE80211_IOC_WME_TXOPLIMIT, ac, &val) != -1)
		ifieee80211_print_list_wme_aci_txoplimit(val);
	if (get80211wme(ctx, IEEE80211_IOC_WME_ACM, ac, &val) != -1) {
		if (val)
			ifieee80211_print_list_wme_aci_acm_enabled();
		else if (ctx->args->verbose)
			ifieee80211_print_list_wme_aci_acm_disabled();
	}
	/* !BSS only */
	if ((ac & IEEE80211_WMEPARAM_BSS) == 0) {
		if (get80211wme(ctx, IEEE80211_IOC_WME_ACKPOLICY, ac, &val) != -1) {
			if (!val)
				ifieee80211_print_list_wme_aci_ack_disabled();
			else if (ctx->args->verbose)
				ifieee80211_print_list_wme_aci_ack_enabled();
		}
	}
	ifconfig_print_newline();
}

static void
list_wme(if_ctx *ctx)
{
	static const char *acnames[] = { "AC_BE", "AC_BK", "AC_VI", "AC_VO" };
	int ac;

	if (ctx->args->verbose) {
		/* display both BSS and local settings */
		for (ac = WME_AC_BE; ac <= WME_AC_VO; ac++) {
	again:
			if (ac & IEEE80211_WMEPARAM_BSS)
				list_wme_aci(ctx, "     ", ac);
			else
				list_wme_aci(ctx, acnames[ac], ac);
			if ((ac & IEEE80211_WMEPARAM_BSS) == 0) {
				ac |= IEEE80211_WMEPARAM_BSS;
				goto again;
			} else
				ac &= ~IEEE80211_WMEPARAM_BSS;
		}
	} else {
		/* display only channel settings */
		for (ac = WME_AC_BE; ac <= WME_AC_VO; ac++)
			list_wme_aci(ctx, acnames[ac], ac);
	}
}

static void
list_roam(if_ctx *ctx)
{
	const struct ieee80211_roamparam *rp;
	int mode;

	getroam(ctx);
	for (mode = IEEE80211_MODE_11A; mode < IEEE80211_MODE_MAX; mode++) {
		rp = &roamparams.params[mode];
		if (rp->rssi == 0 && rp->rate == 0)
			continue;
		if (mode == IEEE80211_MODE_11NA ||
		    mode == IEEE80211_MODE_11NG ||
		    mode == IEEE80211_MODE_VHT_2GHZ ||
		    mode == IEEE80211_MODE_VHT_5GHZ) {
			if (rp->rssi & 1)
				ifieee80211_line_check("roam:%-7.7s rssi %2u.5dBm  MCS %2u    ",
				    modename[mode], rp->rssi/2,
				    rp->rate &~ IEEE80211_RATE_MCS);
			else
				ifieee80211_line_check("roam:%-7.7s rssi %4udBm  MCS %2u    ",
				    modename[mode], rp->rssi/2,
				    rp->rate &~ IEEE80211_RATE_MCS);
		} else {
			if (rp->rssi & 1)
				ifieee80211_line_check("roam:%-7.7s rssi %2u.5dBm rate %2u Mb/s",
				    modename[mode], rp->rssi/2, rp->rate/2);
			else
				ifieee80211_line_check("roam:%-7.7s rssi %4udBm rate %2u Mb/s",
				    modename[mode], rp->rssi/2, rp->rate/2);
		}
	}
}

/* XXX TODO: rate-to-string method... */
static const char*
get_mcs_mbs_rate_str(uint8_t rate)
{
	return (rate & IEEE80211_RATE_MCS) ? "MCS " : "Mb/s";
}

static uint8_t
get_rate_value(uint8_t rate)
{
	if (rate & IEEE80211_RATE_MCS)
		return (rate &~ IEEE80211_RATE_MCS);
	return (rate / 2);
}

static void
list_txparams(if_ctx *ctx)
{
	const struct ieee80211_txparam *tp;
	int mode;

	gettxparams(ctx);
	for (mode = IEEE80211_MODE_11A; mode < IEEE80211_MODE_MAX; mode++) {
		tp = &txparams.params[mode];
		if (tp->mgmtrate == 0 && tp->mcastrate == 0)
			continue;
		if (mode == IEEE80211_MODE_11NA ||
		    mode == IEEE80211_MODE_11NG ||
		    mode == IEEE80211_MODE_VHT_2GHZ ||
		    mode == IEEE80211_MODE_VHT_5GHZ) {
			if (tp->ucastrate == IEEE80211_FIXED_RATE_NONE)
				ifieee80211_line_check("%-7.7s ucast NONE    mgmt %2u %s "
				    "mcast %2u %s maxretry %u",
				    modename[mode],
				    get_rate_value(tp->mgmtrate),
				    get_mcs_mbs_rate_str(tp->mgmtrate),
				    get_rate_value(tp->mcastrate),
				    get_mcs_mbs_rate_str(tp->mcastrate),
				    tp->maxretry);
			else
				ifieee80211_line_check("%-7.7s ucast %2u MCS  mgmt %2u %s "
				    "mcast %2u %s maxretry %u",
				    modename[mode],
				    tp->ucastrate &~ IEEE80211_RATE_MCS,
				    get_rate_value(tp->mgmtrate),
				    get_mcs_mbs_rate_str(tp->mgmtrate),
				    get_rate_value(tp->mcastrate),
				    get_mcs_mbs_rate_str(tp->mcastrate),
				    tp->maxretry);
		} else {
			if (tp->ucastrate == IEEE80211_FIXED_RATE_NONE)
				ifieee80211_line_check("%-7.7s ucast NONE    mgmt %2u Mb/s "
				    "mcast %2u Mb/s maxretry %u",
				    modename[mode],
				    tp->mgmtrate/2,
				    tp->mcastrate/2, tp->maxretry);
			else
				ifieee80211_line_check("%-7.7s ucast %2u Mb/s mgmt %2u Mb/s "
				    "mcast %2u Mb/s maxretry %u",
				    modename[mode],
				    tp->ucastrate/2, tp->mgmtrate/2,
				    tp->mcastrate/2, tp->maxretry);
		}
	}
}

static void
list_mac(if_ctx *ctx)
{
	struct ieee80211req ireq = {};
	struct ieee80211req_maclist *acllist;
	int i, nacls, policy, len;
	uint8_t *data;
	char c;

	strlcpy(ireq.i_name, ctx->ifname, sizeof(ireq.i_name)); /* XXX ?? */
	ireq.i_type = IEEE80211_IOC_MACCMD;
	ireq.i_val = IEEE80211_MACCMD_POLICY;
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0) {
		if (errno == EINVAL) {
			ifieee80211_print_list_mac_acl_loaded();
			return;
		}
		if_err(1, "unable to get mac policy");
	}
	policy = ireq.i_val;
	if (policy == IEEE80211_MACCMD_POLICY_OPEN) {
		c = '*';
	} else if (policy == IEEE80211_MACCMD_POLICY_ALLOW) {
		c = '+';
	} else if (policy == IEEE80211_MACCMD_POLICY_DENY) {
		c = '-';
	} else if (policy == IEEE80211_MACCMD_POLICY_RADIUS) {
		c = 'r';		/* NB: should never have entries */
	} else {
		// XXX should this maybe be an err/errx? or warn/warnx?
		ifieee80211_print_list_mac_unknown_policy(policy);
		c = '?';
	}
	if (ctx->args->verbose || c == '?')
		ifieee80211_printpolicy(policy);

	ireq.i_val = IEEE80211_MACCMD_LIST;
	ireq.i_len = 0;
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0)
		if_err(1, "unable to get mac acl list size");
	if (ireq.i_len == 0) {		/* NB: no acls */
		if (!(ctx->args->verbose || c == '?'))
			ifieee80211_printpolicy(policy);
		return;
	}
	len = ireq.i_len;

	data = malloc(len);
	if (data == NULL)
		if_err(1, "out of memory for acl list");

	ireq.i_data = data;
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0)
		if_err(1, "unable to get mac acl list");
	nacls = len / sizeof(*acllist);
	acllist = (struct ieee80211req_maclist *) data;
	for (i = 0; i < nacls; i++)
		ifieee80211_print_list_mac_nacl(c, &acllist[i]);
	free(data);
}

static void
list_regdomain(if_ctx *ctx, int channelsalso)
{
	getregdomain(ctx);
	if (channelsalso) {
		getchaninfo(ctx);
		ifieee80211_spacer = ':';
		ifieee80211_print_regdomain(&regdomain, 1);
		ifieee80211_line_break();
		ifieee80211_print_channels(ctx, chaninfo, 1/*allchans*/, 1/*verbose*/);
	} else
		ifieee80211_print_regdomain(&regdomain, ctx->args->verbose);
}

static void
list_mesh(if_ctx *ctx)
{
	struct ieee80211req ireq = {};
	struct ieee80211req_mesh_route routes[128];
	struct ieee80211req_mesh_route *rt;

	strlcpy(ireq.i_name, ctx->ifname, sizeof(ireq.i_name));
	ireq.i_type = IEEE80211_IOC_MESH_RTCMD;
	ireq.i_val = IEEE80211_MESH_RTCMD_LIST;
	ireq.i_data = &routes;
	ireq.i_len = sizeof(routes);
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0)
	 	if_err(1, "unable to get the Mesh routing table");

	ifieee80211_print_list_mesh_hdr();

	for (unsigned int i = 0; i < ireq.i_len / sizeof(*rt); i++) {
		rt = &routes[i];
		ifieee80211_print_list_mesh_row(rt);
		ifieee80211_print_list_mesh_row2(rt);
	}
}

static void
set80211list(if_ctx *ctx, const char *arg, int dummy __unused)
{
	int s = ctx->io_s;
#define	iseq(a,b)	(strncasecmp(a,b,sizeof(b)-1) == 0)

	ifieee80211_line_init('\t');

	if (iseq(arg, "sta"))
		list_stations(ctx);
	else if (iseq(arg, "scan") || iseq(arg, "ap"))
		list_scan(ctx);
	else if (iseq(arg, "chan") || iseq(arg, "freq"))
		list_channels(ctx, 1);
	else if (iseq(arg, "active"))
		list_channels(ctx, 0);
	else if (iseq(arg, "keys"))
		list_keys(s);
	else if (iseq(arg, "caps"))
		list_capabilities(ctx);
	else if (iseq(arg, "wme") || iseq(arg, "wmm"))
		list_wme(ctx);
	else if (iseq(arg, "mac"))
		list_mac(ctx);
	else if (iseq(arg, "txpow"))
		list_txpow(ctx);
	else if (iseq(arg, "roam"))
		list_roam(ctx);
	else if (iseq(arg, "txparam") || iseq(arg, "txparm"))
		list_txparams(ctx);
	else if (iseq(arg, "regdomain"))
		list_regdomain(ctx, 1);
	else if (iseq(arg, "countries"))
		list_countries();
	else if (iseq(arg, "mesh"))
		list_mesh(ctx);
	else
		if_errx(1, "Don't know how to list %s for %s", arg, ctx->ifname);
	ifieee80211_line_break();
#undef iseq
}

static enum ieee80211_opmode
get80211opmode(if_ctx *ctx)
{
	struct ifmediareq ifmr = {};

	strlcpy(ifmr.ifm_name, ctx->ifname, sizeof(ifmr.ifm_name));

	if (ioctl_ctx(ctx, SIOCGIFMEDIA, (caddr_t)&ifmr) >= 0) {
		if (ifmr.ifm_current & IFM_IEEE80211_ADHOC) {
			if (ifmr.ifm_current & IFM_FLAG0)
				return IEEE80211_M_AHDEMO;
			else
				return IEEE80211_M_IBSS;
		}
		if (ifmr.ifm_current & IFM_IEEE80211_HOSTAP)
			return IEEE80211_M_HOSTAP;
		if (ifmr.ifm_current & IFM_IEEE80211_IBSS)
			return IEEE80211_M_IBSS;
		if (ifmr.ifm_current & IFM_IEEE80211_MONITOR)
			return IEEE80211_M_MONITOR;
		if (ifmr.ifm_current & IFM_IEEE80211_MBSS)
			return IEEE80211_M_MBSS;
	}
	return IEEE80211_M_STA;
}

static int
getid(if_ctx *ctx, int ix, void *data, size_t len, int *plen, int mesh)
{
	struct ieee80211req ireq = {};

	strlcpy(ireq.i_name, ctx->ifname, sizeof(ireq.i_name));
	ireq.i_type = (!mesh) ? IEEE80211_IOC_SSID : IEEE80211_IOC_MESH_ID;
	ireq.i_val = ix;
	ireq.i_data = data;
	ireq.i_len = len;
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0)
		return -1;
	*plen = ireq.i_len;
	return 0;
}

static int
getdevicename(if_ctx *ctx, void *data, size_t len, int *plen)
{
	struct ieee80211req ireq = {};

	strlcpy(ireq.i_name, ctx->ifname, sizeof(ireq.i_name));
	ireq.i_type = IEEE80211_IOC_IC_NAME;
	ireq.i_val = -1;
	ireq.i_data = data;
	ireq.i_len = len;
	if (ioctl_ctx(ctx, SIOCG80211, &ireq) < 0)
		return (-1);
	*plen = ireq.i_len;
	return (0);
}

static void
ieee80211_status(if_ctx *ctx)
{
	int s = ctx->io_s;
	static const uint8_t zerobssid[IEEE80211_ADDR_LEN];
	uint8_t bssid[IEEE80211_ADDR_LEN];
	enum ieee80211_opmode opmode = get80211opmode(ctx);
	int i, num, wpa, wme, bgscan, bgscaninterval, val, len, wepmode;
	uint8_t data[32];
	const struct ieee80211_channel *c;
	const struct ieee80211_roamparam *rp;
	const struct ieee80211_txparam *tp;
	const int verbose = ctx->args->verbose;

	if (getid(ctx, -1, data, sizeof(data), &len, 0) < 0) {
		/* If we can't get the SSID, this isn't an 802.11 device. */
		return;
	}

	/*
	 * Invalidate cached state so printing status for multiple
	 * if's doesn't reuse the first interfaces' cached state.
	 */
	gotcurchan = 0;
	gotroam = 0;
	gottxparams = 0;
	gothtconf = 0;
	gotregdomain = 0;

	ifconfig_print_tab();
	if (opmode == IEEE80211_M_MBSS) {
		ifieee80211_print_ieee80211_status_meshid();
		getid(ctx, 0, data, sizeof(data), &len, 1);
		ifieee80211_print_string(data, len);
	} else {
		if (get80211val(ctx, IEEE80211_IOC_NUMSSIDS, &num) < 0)
			num = 0;
		ifieee80211_print_ieee80211_status_ssid();
		if (num > 1) {
			for (i = 0; i < num; i++) {
				if (getid(ctx, i, data, sizeof(data), &len, 0) >= 0 && len > 0) {
					ifieee80211_print_ieee80211_status_ssid_idx(i);
					ifieee80211_print_string(data, len);
				}
			}
		} else
			ifieee80211_print_string(data, len);
	}
	c = getcurchan(ctx);
	if (c->ic_freq != IEEE80211_CHAN_ANY) {
		ifieee80211_print_ieee80211_status_channel(c);
	} else if (verbose)
		ifieee80211_print_ieee80211_status_channel_undef();

	if (get80211(ctx, IEEE80211_IOC_BSSID, bssid, IEEE80211_ADDR_LEN) >= 0 &&
	    (memcmp(bssid, zerobssid, sizeof(zerobssid)) != 0 || verbose)) {
		ifieee80211_print_ieee80211_status_bssid(bssid);
		ifieee80211_printbssidname((struct ether_addr *)bssid);
	}

	if (get80211len(ctx, IEEE80211_IOC_STATIONNAME, data, sizeof(data), &len) != -1) {
		ifieee80211_print_ieee80211_status_stationname();
		ifieee80211_print_string(data, len);
	}

	ifieee80211_spacer = ' ';		/* force first break */
	ifieee80211_line_break();

	list_regdomain(ctx, 0);

	wpa = 0;
	if (get80211val(ctx, IEEE80211_IOC_AUTHMODE, &val) != -1) {
		switch (val) {
		case IEEE80211_AUTH_NONE:
			ifieee80211_line_check("authmode NONE");
			break;
		case IEEE80211_AUTH_OPEN:
			ifieee80211_line_check("authmode OPEN");
			break;
		case IEEE80211_AUTH_SHARED:
			ifieee80211_line_check("authmode SHARED");
			break;
		case IEEE80211_AUTH_8021X:
			ifieee80211_line_check("authmode 802.1x");
			break;
		case IEEE80211_AUTH_WPA:
			if (get80211val(ctx, IEEE80211_IOC_WPA, &wpa) < 0)
				wpa = 1;	/* default to WPA1 */
			switch (wpa) {
			case 2:
				ifieee80211_line_check("authmode WPA2/802.11i");
				break;
			case 3:
				ifieee80211_line_check("authmode WPA1+WPA2/802.11i");
				break;
			default:
				ifieee80211_line_check("authmode WPA");
				break;
			}
			break;
		case IEEE80211_AUTH_AUTO:
			ifieee80211_line_check("authmode AUTO");
			break;
		default:
			ifieee80211_line_check("authmode UNKNOWN (0x%x)", val);
			break;
		}
	}

	if (wpa || verbose) {
		if (get80211val(ctx, IEEE80211_IOC_WPS, &val) != -1) {
			if (val)
				ifieee80211_line_check("wps");
			else if (verbose)
				ifieee80211_line_check("-wps");
		}
		if (get80211val(ctx, IEEE80211_IOC_TSN, &val) != -1) {
			if (val)
				ifieee80211_line_check("tsn");
			else if (verbose)
				ifieee80211_line_check("-tsn");
		}
		if (ioctl(s, IEEE80211_IOC_COUNTERMEASURES, &val) != -1) {
			if (val)
				ifieee80211_line_check("countermeasures");
			else if (verbose)
				ifieee80211_line_check("-countermeasures");
		}
#if 0
		/* XXX not interesting with WPA done in user space */
		ireq.i_type = IEEE80211_IOC_KEYMGTALGS;
		if (ioctl(s, SIOCG80211, &ireq) != -1) {
		}

		ireq.i_type = IEEE80211_IOC_MCASTCIPHER;
		if (ioctl(s, SIOCG80211, &ireq) != -1) {
			ifieee80211_line_check("mcastcipher ");
			ifieee80211_printcipher(s, &ireq, IEEE80211_IOC_MCASTKEYLEN);
			ifieee80211_spacer = ' ';
		}

		ireq.i_type = IEEE80211_IOC_UCASTCIPHER;
		if (ioctl(s, SIOCG80211, &ireq) != -1) {
			ifieee80211_line_check("ucastcipher ");
			ifieee80211_printcipher(s, &ireq, IEEE80211_IOC_UCASTKEYLEN);
		}

		if (wpa & 2) {
			ireq.i_type = IEEE80211_IOC_RSNCAPS;
			if (ioctl(s, SIOCG80211, &ireq) != -1) {
				ifieee80211_line_check("RSN caps 0x%x", ireq.i_val);
				ifieee80211_spacer = ' ';
			}
		}

		ireq.i_type = IEEE80211_IOC_UCASTCIPHERS;
		if (ioctl(s, SIOCG80211, &ireq) != -1) {
		}
#endif
	}

	if (get80211val(ctx, IEEE80211_IOC_WEP, &wepmode) != -1 &&
	    wepmode != IEEE80211_WEP_NOSUP) {

		switch (wepmode) {
		case IEEE80211_WEP_OFF:
			ifieee80211_line_check("privacy OFF");
			break;
		case IEEE80211_WEP_ON:
			ifieee80211_line_check("privacy ON");
			break;
		case IEEE80211_WEP_MIXED:
			ifieee80211_line_check("privacy MIXED");
			break;
		default:
			ifieee80211_line_check("privacy UNKNOWN (0x%x)", wepmode);
			break;
		}

		/*
		 * If we get here then we've got WEP support so we need
		 * to print WEP status.
		 */

		if (get80211val(ctx, IEEE80211_IOC_WEPTXKEY, &val) < 0) {
			if_warn("WEP support, but no tx key!");
			goto end;
		}
		if (val != -1)
			ifieee80211_line_check("deftxkey %d", val+1);
		else if (wepmode != IEEE80211_WEP_OFF || verbose)
			ifieee80211_line_check("deftxkey UNDEF");

		if (get80211val(ctx, IEEE80211_IOC_NUMWEPKEYS, &num) < 0) {
			if_warn("WEP support, but no NUMWEPKEYS support!");
			goto end;
		}

		for (i = 0; i < num; i++) {
			struct ieee80211req_key ik;

			memset(&ik, 0, sizeof(ik));
			ik.ik_keyix = i;
			if (get80211(ctx, IEEE80211_IOC_WPAKEY, &ik, sizeof(ik)) < 0) {
				if_warn("WEP support, but cannot get keys!");
				goto end;
			}
			if (ik.ik_keylen != 0) {
				if (verbose)
					ifieee80211_line_break();
				ifieee80211_printkey(ctx, &ik);
			}
		}
		if (opmode == IEEE80211_M_STA && wpa >= 2) {
			struct ieee80211req_key ik;
			int error;

			memset(&ik, 0, sizeof(ik));
			ik.ik_keyix = IEEE80211_KEYIX_NONE;
			memcpy(ik.ik_macaddr, bssid, sizeof(ik.ik_macaddr));
			error = get80211(ctx, IEEE80211_IOC_WPAKEY, &ik, sizeof(ik));
			if (error == 0 && ik.ik_keylen != 0) {
				if (verbose)
					ifieee80211_line_break();
				ifieee80211_printkey(ctx, &ik);
				i++;
			}
		}
		if (i > 0 && verbose)
			ifieee80211_line_break();
end:
		;
	}

	if (get80211val(ctx, IEEE80211_IOC_POWERSAVE, &val) != -1 &&
	    val != IEEE80211_POWERSAVE_NOSUP ) {
		if (val != IEEE80211_POWERSAVE_OFF || verbose) {
			switch (val) {
			case IEEE80211_POWERSAVE_OFF:
				ifieee80211_line_check("powersavemode OFF");
				break;
			case IEEE80211_POWERSAVE_CAM:
				ifieee80211_line_check("powersavemode CAM");
				break;
			case IEEE80211_POWERSAVE_PSP:
				ifieee80211_line_check("powersavemode PSP");
				break;
			case IEEE80211_POWERSAVE_PSP_CAM:
				ifieee80211_line_check("powersavemode PSP-CAM");
				break;
			}
			if (get80211val(ctx, IEEE80211_IOC_POWERSAVESLEEP, &val) != -1)
				ifieee80211_line_check("powersavesleep %d", val);
		}
	}

	if (get80211val(ctx, IEEE80211_IOC_TXPOWER, &val) != -1) {
		if (val & 1)
			ifieee80211_line_check("txpower %d.5", val/2);
		else
			ifieee80211_line_check("txpower %d", val/2);
	}
	if (verbose) {
		if (get80211val(ctx, IEEE80211_IOC_TXPOWMAX, &val) != -1)
			ifieee80211_line_check("txpowmax %.1f", val/2.);
	}

	if (get80211val(ctx, IEEE80211_IOC_DOTD, &val) != -1) {
		if (val)
			ifieee80211_line_check("dotd");
		else if (verbose)
			ifieee80211_line_check("-dotd");
	}

	if (get80211val(ctx, IEEE80211_IOC_RTSTHRESHOLD, &val) != -1) {
		if (val != IEEE80211_RTS_MAX || verbose)
			ifieee80211_line_check("rtsthreshold %d", val);
	}

	if (get80211val(ctx, IEEE80211_IOC_FRAGTHRESHOLD, &val) != -1) {
		if (val != IEEE80211_FRAG_MAX || verbose)
			ifieee80211_line_check("fragthreshold %d", val);
	}
	if (opmode == IEEE80211_M_STA || verbose) {
		if (get80211val(ctx, IEEE80211_IOC_BMISSTHRESHOLD, &val) != -1) {
			if (val != IEEE80211_HWBMISS_MAX || verbose)
				ifieee80211_line_check("bmiss %d", val);
		}
	}

	if (!verbose) {
		gettxparams(ctx);
		tp = &txparams.params[chan2mode(c)];
		ifieee80211_printrate("ucastrate", tp->ucastrate,
		    IEEE80211_FIXED_RATE_NONE, IEEE80211_FIXED_RATE_NONE);
		ifieee80211_printrate("mcastrate", tp->mcastrate, 2*1,
		    IEEE80211_RATE_MCS|0);
		ifieee80211_printrate("mgmtrate", tp->mgmtrate, 2*1,
		    IEEE80211_RATE_MCS|0);
		if (tp->maxretry != 6)		/* XXX */
			ifieee80211_line_check("maxretry %d", tp->maxretry);
	} else {
		ifieee80211_line_break();
		list_txparams(ctx);
	}

	bgscaninterval = -1;
	(void) get80211val(ctx, IEEE80211_IOC_BGSCAN_INTERVAL, &bgscaninterval);

	if (get80211val(ctx, IEEE80211_IOC_SCANVALID, &val) != -1) {
		if (val != bgscaninterval || verbose)
			ifieee80211_line_check("scanvalid %u", val);
	}

	bgscan = 0;
	if (get80211val(ctx, IEEE80211_IOC_BGSCAN, &bgscan) != -1) {
		if (bgscan)
			ifieee80211_line_check("bgscan");
		else if (verbose)
			ifieee80211_line_check("-bgscan");
	}
	if (bgscan || verbose) {
		if (bgscaninterval != -1)
			ifieee80211_line_check("bgscanintvl %u", bgscaninterval);
		if (get80211val(ctx, IEEE80211_IOC_BGSCAN_IDLE, &val) != -1)
			ifieee80211_line_check("bgscanidle %u", val);
		if (!verbose) {
			getroam(ctx);
			rp = &roamparams.params[chan2mode(c)];
			if (rp->rssi & 1)
				ifieee80211_line_check("roam:rssi %u.5", rp->rssi/2);
			else
				ifieee80211_line_check("roam:rssi %u", rp->rssi/2);
			ifieee80211_line_check("roam:rate %s%u",
			    (rp->rate & IEEE80211_RATE_MCS) ? "MCS " : "",
			    get_rate_value(rp->rate));
		} else {
			ifieee80211_line_break();
			list_roam(ctx);
			ifieee80211_line_break();
		}
	}

	if (IEEE80211_IS_CHAN_ANYG(c) || verbose) {
		if (get80211val(ctx, IEEE80211_IOC_PUREG, &val) != -1) {
			if (val)
				ifieee80211_line_check("pureg");
			else if (verbose)
				ifieee80211_line_check("-pureg");
		}
		if (get80211val(ctx, IEEE80211_IOC_PROTMODE, &val) != -1) {
			switch (val) {
			case IEEE80211_PROTMODE_OFF:
				ifieee80211_line_check("protmode OFF");
				break;
			case IEEE80211_PROTMODE_CTS:
				ifieee80211_line_check("protmode CTS");
				break;
			case IEEE80211_PROTMODE_RTSCTS:
				ifieee80211_line_check("protmode RTSCTS");
				break;
			default:
				ifieee80211_line_check("protmode UNKNOWN (0x%x)", val);
				break;
			}
		}
	}

	if (IEEE80211_IS_CHAN_HT(c) || verbose) {
		gethtconf(ctx);
		switch (htconf & 3) {
		case 0:
		case 2:
			ifieee80211_line_check("-ht");
			break;
		case 1:
			ifieee80211_line_check("ht20");
			break;
		case 3:
			if (verbose)
				ifieee80211_line_check("ht");
			break;
		}
		if (get80211val(ctx, IEEE80211_IOC_HTCOMPAT, &val) != -1) {
			if (!val)
				ifieee80211_line_check("-htcompat");
			else if (verbose)
				ifieee80211_line_check("htcompat");
		}
		if (get80211val(ctx, IEEE80211_IOC_AMPDU, &val) != -1) {
			switch (val) {
			case 0:
				ifieee80211_line_check("-ampdu");
				break;
			case 1:
				ifieee80211_line_check("ampdutx -ampdurx");
				break;
			case 2:
				ifieee80211_line_check("-ampdutx ampdurx");
				break;
			case 3:
				if (verbose)
					ifieee80211_line_check("ampdu");
				break;
			}
		}
		/* XXX 11ac density/size is different */
		if (get80211val(ctx, IEEE80211_IOC_AMPDU_LIMIT, &val) != -1) {
			switch (val) {
			case IEEE80211_HTCAP_MAXRXAMPDU_8K:
				ifieee80211_line_check("ampdulimit 8k");
				break;
			case IEEE80211_HTCAP_MAXRXAMPDU_16K:
				ifieee80211_line_check("ampdulimit 16k");
				break;
			case IEEE80211_HTCAP_MAXRXAMPDU_32K:
				ifieee80211_line_check("ampdulimit 32k");
				break;
			case IEEE80211_HTCAP_MAXRXAMPDU_64K:
				ifieee80211_line_check("ampdulimit 64k");
				break;
			}
		}
		/* XXX 11ac density/size is different */
		if (get80211val(ctx, IEEE80211_IOC_AMPDU_DENSITY, &val) != -1) {
			switch (val) {
			case IEEE80211_HTCAP_MPDUDENSITY_NA:
				if (verbose)
					ifieee80211_line_check("ampdudensity NA");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_025:
				ifieee80211_line_check("ampdudensity .25");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_05:
				ifieee80211_line_check("ampdudensity .5");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_1:
				ifieee80211_line_check("ampdudensity 1");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_2:
				ifieee80211_line_check("ampdudensity 2");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_4:
				ifieee80211_line_check("ampdudensity 4");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_8:
				ifieee80211_line_check("ampdudensity 8");
				break;
			case IEEE80211_HTCAP_MPDUDENSITY_16:
				ifieee80211_line_check("ampdudensity 16");
				break;
			}
		}
		if (get80211val(ctx, IEEE80211_IOC_AMSDU, &val) != -1) {
			switch (val) {
			case 0:
				ifieee80211_line_check("-amsdu");
				break;
			case 1:
				ifieee80211_line_check("amsdutx -amsdurx");
				break;
			case 2:
				ifieee80211_line_check("-amsdutx amsdurx");
				break;
			case 3:
				if (verbose)
					ifieee80211_line_check("amsdu");
				break;
			}
		}
		/* XXX amsdu limit */
		if (get80211val(ctx, IEEE80211_IOC_SHORTGI, &val) != -1) {
			if (val)
				ifieee80211_line_check("shortgi");
			else if (verbose)
				ifieee80211_line_check("-shortgi");
		}
		if (get80211val(ctx, IEEE80211_IOC_HTPROTMODE, &val) != -1) {
			if (val == IEEE80211_PROTMODE_OFF)
				ifieee80211_line_check("htprotmode OFF");
			else if (val != IEEE80211_PROTMODE_RTSCTS)
				ifieee80211_line_check("htprotmode UNKNOWN (0x%x)", val);
			else if (verbose)
				ifieee80211_line_check("htprotmode RTSCTS");
		}
		if (get80211val(ctx, IEEE80211_IOC_PUREN, &val) != -1) {
			if (val)
				ifieee80211_line_check("puren");
			else if (verbose)
				ifieee80211_line_check("-puren");
		}
		if (get80211val(ctx, IEEE80211_IOC_SMPS, &val) != -1) {
			if (val == IEEE80211_HTCAP_SMPS_DYNAMIC)
				ifieee80211_line_check("smpsdyn");
			else if (val == IEEE80211_HTCAP_SMPS_ENA)
				ifieee80211_line_check("smps");
			else if (verbose)
				ifieee80211_line_check("-smps");
		}
		if (get80211val(ctx, IEEE80211_IOC_RIFS, &val) != -1) {
			if (val)
				ifieee80211_line_check("rifs");
			else if (verbose)
				ifieee80211_line_check("-rifs");
		}

		/* XXX VHT STBC? */
		if (get80211val(ctx, IEEE80211_IOC_STBC, &val) != -1) {
			switch (val) {
			case 0:
				ifieee80211_line_check("-stbc");
				break;
			case 1:
				ifieee80211_line_check("stbctx -stbcrx");
				break;
			case 2:
				ifieee80211_line_check("-stbctx stbcrx");
				break;
			case 3:
				if (verbose)
					ifieee80211_line_check("stbc");
				break;
			}
		}
		if (get80211val(ctx, IEEE80211_IOC_LDPC, &val) != -1) {
			switch (val) {
			case 0:
				ifieee80211_line_check("-ldpc");
				break;
			case 1:
				ifieee80211_line_check("ldpctx -ldpcrx");
				break;
			case 2:
				ifieee80211_line_check("-ldpctx ldpcrx");
				break;
			case 3:
				if (verbose)
					ifieee80211_line_check("ldpc");
				break;
			}
		}
		if (get80211val(ctx, IEEE80211_IOC_UAPSD, &val) != -1) {
			switch (val) {
			case 0:
				ifieee80211_line_check("-uapsd");
				break;
			case 1:
				ifieee80211_line_check("uapsd");
				break;
			}
		}
	}

	if (IEEE80211_IS_CHAN_VHT(c) || verbose) {
		getvhtconf(ctx);
		if (vhtconf & IEEE80211_FVHT_VHT) {
			ifieee80211_line_check("vht");

			if (vhtconf & IEEE80211_FVHT_USEVHT40)
				ifieee80211_line_check("vht40");
			else
				ifieee80211_line_check("-vht40");
			if (vhtconf & IEEE80211_FVHT_USEVHT80)
				ifieee80211_line_check("vht80");
			else
				ifieee80211_line_check("-vht80");
			if (vhtconf & IEEE80211_FVHT_USEVHT160)
				ifieee80211_line_check("vht160");
			else
				ifieee80211_line_check("-vht160");
			if (vhtconf & IEEE80211_FVHT_USEVHT80P80)
				ifieee80211_line_check("vht80p80");
			else
				ifieee80211_line_check("-vht80p80");
		} else if (verbose)
			ifieee80211_line_check("-vht");
	}

	if (get80211val(ctx, IEEE80211_IOC_WME, &wme) != -1) {
		if (wme)
			ifieee80211_line_check("wme");
		else if (verbose)
			ifieee80211_line_check("-wme");
	} else
		wme = 0;

	if (get80211val(ctx, IEEE80211_IOC_BURST, &val) != -1) {
		if (val)
			ifieee80211_line_check("burst");
		else if (verbose)
			ifieee80211_line_check("-burst");
	}

	if (get80211val(ctx, IEEE80211_IOC_FF, &val) != -1) {
		if (val)
			ifieee80211_line_check("ff");
		else if (verbose)
			ifieee80211_line_check("-ff");
	}
	if (get80211val(ctx, IEEE80211_IOC_TURBOP, &val) != -1) {
		if (val)
			ifieee80211_line_check("dturbo");
		else if (verbose)
			ifieee80211_line_check("-dturbo");
	}
	if (get80211val(ctx, IEEE80211_IOC_DWDS, &val) != -1) {
		if (val)
			ifieee80211_line_check("dwds");
		else if (verbose)
			ifieee80211_line_check("-dwds");
	}

	if (opmode == IEEE80211_M_HOSTAP) {
		if (get80211val(ctx, IEEE80211_IOC_HIDESSID, &val) != -1) {
			if (val)
				ifieee80211_line_check("hidessid");
			else if (verbose)
				ifieee80211_line_check("-hidessid");
		}
		if (get80211val(ctx, IEEE80211_IOC_APBRIDGE, &val) != -1) {
			if (!val)
				ifieee80211_line_check("-apbridge");
			else if (verbose)
				ifieee80211_line_check("apbridge");
		}
		if (get80211val(ctx, IEEE80211_IOC_DTIM_PERIOD, &val) != -1)
			ifieee80211_line_check("dtimperiod %u", val);

		if (get80211val(ctx, IEEE80211_IOC_DOTH, &val) != -1) {
			if (!val)
				ifieee80211_line_check("-doth");
			else if (verbose)
				ifieee80211_line_check("doth");
		}
		if (get80211val(ctx, IEEE80211_IOC_DFS, &val) != -1) {
			if (!val)
				ifieee80211_line_check("-dfs");
			else if (verbose)
				ifieee80211_line_check("dfs");
		}
		if (get80211val(ctx, IEEE80211_IOC_INACTIVITY, &val) != -1) {
			if (!val)
				ifieee80211_line_check("-inact");
			else if (verbose)
				ifieee80211_line_check("inact");
		}
	} else {
		if (get80211val(ctx, IEEE80211_IOC_ROAMING, &val) != -1) {
			if (val != IEEE80211_ROAMING_AUTO || verbose) {
				switch (val) {
				case IEEE80211_ROAMING_DEVICE:
					ifieee80211_line_check("roaming DEVICE");
					break;
				case IEEE80211_ROAMING_AUTO:
					ifieee80211_line_check("roaming AUTO");
					break;
				case IEEE80211_ROAMING_MANUAL:
					ifieee80211_line_check("roaming MANUAL");
					break;
				default:
					ifieee80211_line_check("roaming UNKNOWN (0x%x)",
						val);
					break;
				}
			}
		}
	}

	if (opmode == IEEE80211_M_AHDEMO) {
		if (get80211val(ctx, IEEE80211_IOC_TDMA_SLOT, &val) != -1)
			ifieee80211_line_check("tdmaslot %u", val);
		if (get80211val(ctx, IEEE80211_IOC_TDMA_SLOTCNT, &val) != -1)
			ifieee80211_line_check("tdmaslotcnt %u", val);
		if (get80211val(ctx, IEEE80211_IOC_TDMA_SLOTLEN, &val) != -1)
			ifieee80211_line_check("tdmaslotlen %u", val);
		if (get80211val(ctx, IEEE80211_IOC_TDMA_BINTERVAL, &val) != -1)
			ifieee80211_line_check("tdmabintval %u", val);
	} else if (get80211val(ctx, IEEE80211_IOC_BEACON_INTERVAL, &val) != -1) {
		/* XXX default define not visible */
		if (val != 100 || verbose)
			ifieee80211_line_check("bintval %u", val);
	}

	if (wme && verbose) {
		ifieee80211_line_break();
		list_wme(ctx);
	}

	if (opmode == IEEE80211_M_MBSS) {
		if (get80211val(ctx, IEEE80211_IOC_MESH_TTL, &val) != -1) {
			ifieee80211_line_check("meshttl %u", val);
		}
		if (get80211val(ctx, IEEE80211_IOC_MESH_AP, &val) != -1) {
			if (val)
				ifieee80211_line_check("meshpeering");
			else
				ifieee80211_line_check("-meshpeering");
		}
		if (get80211val(ctx, IEEE80211_IOC_MESH_FWRD, &val) != -1) {
			if (val)
				ifieee80211_line_check("meshforward");
			else
				ifieee80211_line_check("-meshforward");
		}
		if (get80211val(ctx, IEEE80211_IOC_MESH_GATE, &val) != -1) {
			if (val)
				ifieee80211_line_check("meshgate");
			else
				ifieee80211_line_check("-meshgate");
		}
		if (get80211len(ctx, IEEE80211_IOC_MESH_PR_METRIC, data, 12,
		    &len) != -1) {
			data[len] = '\0';
			ifieee80211_line_check("meshmetric %s", data);
		}
		if (get80211len(ctx, IEEE80211_IOC_MESH_PR_PATH, data, 12,
		    &len) != -1) {
			data[len] = '\0';
			ifieee80211_line_check("meshpath %s", data);
		}
		if (get80211val(ctx, IEEE80211_IOC_HWMP_ROOTMODE, &val) != -1) {
			switch (val) {
			case IEEE80211_HWMP_ROOTMODE_DISABLED:
				ifieee80211_line_check("hwmprootmode DISABLED");
				break;
			case IEEE80211_HWMP_ROOTMODE_NORMAL:
				ifieee80211_line_check("hwmprootmode NORMAL");
				break;
			case IEEE80211_HWMP_ROOTMODE_PROACTIVE:
				ifieee80211_line_check("hwmprootmode PROACTIVE");
				break;
			case IEEE80211_HWMP_ROOTMODE_RANN:
				ifieee80211_line_check("hwmprootmode RANN");
				break;
			default:
				ifieee80211_line_check("hwmprootmode UNKNOWN(%d)", val);
				break;
			}
		}
		if (get80211val(ctx, IEEE80211_IOC_HWMP_MAXHOPS, &val) != -1) {
			ifieee80211_line_check("hwmpmaxhops %u", val);
		}
	}

	ifieee80211_line_break();

	if (getdevicename(ctx, data, sizeof(data), &len) < 0)
		return;
	ifieee80211_line_check("parent interface: %s", data);

	ifieee80211_line_break();
}

int
get80211(if_ctx *ctx, int type, void *data, int len)
{

	return (lib80211_get80211(ctx->io_s, ctx->ifname, type, data, len));
}

static int
get80211len(if_ctx *ctx, int type, void *data, int len, int *plen)
{

	return (lib80211_get80211len(ctx->io_s, ctx->ifname, type, data, len, plen));
}

static int
get80211val(if_ctx *ctx, int type, int *val)
{

	return (lib80211_get80211val(ctx->io_s, ctx->ifname, type, val));
}

static void
set80211(if_ctx *ctx, int type, int val, int len, void *data)
{
	int ret;

	ret = lib80211_set80211(ctx->io_s, ctx->ifname, type, val, len, data);
	if (ret < 0)
		if_err(1, "SIOCS80211");
}

static const char *
get_string(const char *val, const char *sep, u_int8_t *buf, int *lenp)
{
	int len;
	int hexstr;
	u_int8_t *p;

	len = *lenp;
	p = buf;
	hexstr = (val[0] == '0' && tolower((u_char)val[1]) == 'x');
	if (hexstr)
		val += 2;
	for (;;) {
		if (*val == '\0')
			break;
		if (sep != NULL && strchr(sep, *val) != NULL) {
			val++;
			break;
		}
		if (hexstr) {
			if (!isxdigit((u_char)val[0])) {
				if_warnx("bad hexadecimal digits");
				return NULL;
			}
			if (!isxdigit((u_char)val[1])) {
				if_warnx("odd count hexadecimal digits");
				return NULL;
			}
		}
		if (p >= buf + len) {
			if (hexstr)
				if_warnx("hexadecimal digits too long");
			else
				if_warnx("string too long");
			return NULL;
		}
		if (hexstr) {
#define	tohex(x)	(isdigit(x) ? (x) - '0' : tolower(x) - 'a' + 10)
			*p++ = (tohex((u_char)val[0]) << 4) |
			    tohex((u_char)val[1]);
#undef tohex
			val += 2;
		} else
			*p++ = *val++;
	}
	len = p - buf;
	/* The string "-" is treated as the empty string. */
	if (!hexstr && len == 1 && buf[0] == '-') {
		len = 0;
		memset(buf, 0, *lenp);
	} else if (len < *lenp)
		memset(p, 0, *lenp - len);
	*lenp = len;
	return val;
}

static void
setdefregdomain(if_ctx *ctx)
{
	struct regdata *rdp = getregdata();
	const struct regdomain *rd;

	/* Check if regdomain/country was already set by a previous call. */
	/* XXX is it possible? */
	if (regdomain.regdomain != 0 ||
	    regdomain.country != CTRY_DEFAULT)
		return;

	getregdomain(ctx);

	/* Check if it was already set by the driver. */
	if (regdomain.regdomain != 0 ||
	    regdomain.country != CTRY_DEFAULT)
		return;

	/* Set FCC/US as default. */
	rd = lib80211_regdomain_findbysku(rdp, SKU_FCC);
	if (rd == NULL)
		if_errx(1, "FCC regdomain was not found");

	regdomain.regdomain = rd->sku;
	if (rd->cc != NULL)
		defaultcountry(rd);

	/* Send changes to net80211. */
	setregdomain_cb(ctx, &regdomain);

	/* Cleanup (so it can be overridden by subsequent parameters). */
	regdomain.regdomain = 0;
	regdomain.country = CTRY_DEFAULT;
	regdomain.isocc[0] = 0;
	regdomain.isocc[1] = 0;
}

/*
 * Virtual AP cloning support.
 */
static struct ieee80211_clone_params params = {
	.icp_opmode	= IEEE80211_M_STA,	/* default to station mode */
};

static void
wlan_create(if_ctx *ctx, struct ifreq *ifr)
{
	static const uint8_t zerobssid[IEEE80211_ADDR_LEN];

	if (params.icp_parent[0] == '\0')
		if_errx(1, "must specify a parent device (wlandev) when creating "
		    "a wlan device");
	if (params.icp_opmode == IEEE80211_M_WDS &&
	    memcmp(params.icp_bssid, zerobssid, sizeof(zerobssid)) == 0)
		if_errx(1, "no bssid specified for WDS (use wlanbssid)");
	ifr->ifr_data = (caddr_t) &params;
	ifcreate_ioctl(ctx, ifr);

	setdefregdomain(ctx);
}

static void
set80211clone_wlandev(if_ctx *ctx __unused, const char *arg, int dummy __unused)
{
	strlcpy(params.icp_parent, arg, IFNAMSIZ);
}

static void
set80211clone_wlanbssid(if_ctx *ctx __unused, const char *arg, int dummy __unused)
{
	const struct ether_addr *ea;

	ea = ether_aton(arg);
	if (ea == NULL)
		if_errx(1, "%s: cannot parse bssid", arg);
	memcpy(params.icp_bssid, ea->octet, IEEE80211_ADDR_LEN);
}

static void
set80211clone_wlanaddr(if_ctx *ctx __unused, const char *arg, int dummy __unused)
{
	const struct ether_addr *ea;

	ea = ether_aton(arg);
	if (ea == NULL)
		if_errx(1, "%s: cannot parse address", arg);
	memcpy(params.icp_macaddr, ea->octet, IEEE80211_ADDR_LEN);
	params.icp_flags |= IEEE80211_CLONE_MACADDR;
}

static void
set80211clone_wlanmode(if_ctx *ctx, const char *arg, int dummy __unused)
{
#define	iseq(a,b)	(strncasecmp(a,b,sizeof(b)-1) == 0)
	if (iseq(arg, "sta"))
		params.icp_opmode = IEEE80211_M_STA;
	else if (iseq(arg, "ahdemo") || iseq(arg, "adhoc-demo"))
		params.icp_opmode = IEEE80211_M_AHDEMO;
	else if (iseq(arg, "ibss") || iseq(arg, "adhoc"))
		params.icp_opmode = IEEE80211_M_IBSS;
	else if (iseq(arg, "ap") || iseq(arg, "host"))
		params.icp_opmode = IEEE80211_M_HOSTAP;
	else if (iseq(arg, "wds"))
		params.icp_opmode = IEEE80211_M_WDS;
	else if (iseq(arg, "monitor"))
		params.icp_opmode = IEEE80211_M_MONITOR;
	else if (iseq(arg, "tdma")) {
		params.icp_opmode = IEEE80211_M_AHDEMO;
		params.icp_flags |= IEEE80211_CLONE_TDMA;
	} else if (iseq(arg, "mesh") || iseq(arg, "mp")) /* mesh point */
		params.icp_opmode = IEEE80211_M_MBSS;
	else
		if_errx(1, "Don't know to create %s for %s", arg, ctx->ifname);
#undef iseq
}

static void
set80211clone_beacons(if_ctx *ctx __unused, const char *val __unused, int d)
{
	/* NB: inverted sense */
	if (d)
		params.icp_flags &= ~IEEE80211_CLONE_NOBEACONS;
	else
		params.icp_flags |= IEEE80211_CLONE_NOBEACONS;
}

static void
set80211clone_bssid(if_ctx *ctx __unused, const char *val __unused, int d)
{
	if (d)
		params.icp_flags |= IEEE80211_CLONE_BSSID;
	else
		params.icp_flags &= ~IEEE80211_CLONE_BSSID;
}

static void
set80211clone_wdslegacy(if_ctx *ctx __unused, const char *val __unused, int d)
{
	if (d)
		params.icp_flags |= IEEE80211_CLONE_WDSLEGACY;
	else
		params.icp_flags &= ~IEEE80211_CLONE_WDSLEGACY;
}

static struct cmd ieee80211_cmds[] = {
	DEF_CMD_ARG("ssid",		set80211ssid),
	DEF_CMD_ARG("nwid",		set80211ssid),
	DEF_CMD_ARG("meshid",		set80211meshid),
	DEF_CMD_ARG("stationname",	set80211stationname),
	DEF_CMD_ARG("station",		set80211stationname),	/* BSD/OS */
	DEF_CMD_ARG("channel",		set80211channel),
	DEF_CMD_ARG("authmode",		set80211authmode),
	DEF_CMD_ARG("powersavemode",	set80211powersavemode),
	DEF_CMD("powersave",	1,	set80211powersave),
	DEF_CMD("-powersave",	0,	set80211powersave),
	DEF_CMD_ARG("powersavesleep", 	set80211powersavesleep),
	DEF_CMD_ARG("wepmode",		set80211wepmode),
	DEF_CMD("wep",		1,	set80211wep),
	DEF_CMD("-wep",		0,	set80211wep),
	DEF_CMD_ARG("deftxkey",		set80211weptxkey),
	DEF_CMD_ARG("weptxkey",		set80211weptxkey),
	DEF_CMD_ARG("wepkey",		set80211wepkey),
	DEF_CMD_ARG("nwkey",		set80211nwkey),		/* NetBSD */
	DEF_CMD("-nwkey",	0,	set80211wep),		/* NetBSD */
	DEF_CMD_ARG("rtsthreshold",	set80211rtsthreshold),
	DEF_CMD_ARG("protmode",		set80211protmode),
	DEF_CMD_ARG("txpower",		set80211txpower),
	DEF_CMD_ARG("roaming",		set80211roaming),
	DEF_CMD("wme",		1,	set80211wme),
	DEF_CMD("-wme",		0,	set80211wme),
	DEF_CMD("wmm",		1,	set80211wme),
	DEF_CMD("-wmm",		0,	set80211wme),
	DEF_CMD("hidessid",	1,	set80211hidessid),
	DEF_CMD("-hidessid",	0,	set80211hidessid),
	DEF_CMD("apbridge",	1,	set80211apbridge),
	DEF_CMD("-apbridge",	0,	set80211apbridge),
	DEF_CMD_ARG("chanlist",		set80211chanlist),
	DEF_CMD_ARG("bssid",		set80211bssid),
	DEF_CMD_ARG("ap",		set80211bssid),
	DEF_CMD("scan",	0,		set80211scan),
	DEF_CMD_ARG("list",		set80211list),
	DEF_CMD_ARG2("cwmin",		set80211cwmin),
	DEF_CMD_ARG2("cwmax",		set80211cwmax),
	DEF_CMD_ARG2("aifs",		set80211aifs),
	DEF_CMD_ARG2("txoplimit",	set80211txoplimit),
	DEF_CMD_ARG("acm",		set80211acm),
	DEF_CMD_ARG("-acm",		set80211noacm),
	DEF_CMD_ARG("ack",		set80211ackpolicy),
	DEF_CMD_ARG("-ack",		set80211noackpolicy),
	DEF_CMD_ARG2("bss:cwmin",	set80211bsscwmin),
	DEF_CMD_ARG2("bss:cwmax",	set80211bsscwmax),
	DEF_CMD_ARG2("bss:aifs",	set80211bssaifs),
	DEF_CMD_ARG2("bss:txoplimit",	set80211bsstxoplimit),
	DEF_CMD_ARG("dtimperiod",	set80211dtimperiod),
	DEF_CMD_ARG("bintval",		set80211bintval),
	DEF_CMD("mac:open",	IEEE80211_MACCMD_POLICY_OPEN,	set80211maccmd),
	DEF_CMD("mac:allow",	IEEE80211_MACCMD_POLICY_ALLOW,	set80211maccmd),
	DEF_CMD("mac:deny",	IEEE80211_MACCMD_POLICY_DENY,	set80211maccmd),
	DEF_CMD("mac:radius",	IEEE80211_MACCMD_POLICY_RADIUS,	set80211maccmd),
	DEF_CMD("mac:flush",	IEEE80211_MACCMD_FLUSH,		set80211maccmd),
	DEF_CMD("mac:detach",	IEEE80211_MACCMD_DETACH,	set80211maccmd),
	DEF_CMD_ARG("mac:add",		set80211addmac),
	DEF_CMD_ARG("mac:del",		set80211delmac),
	DEF_CMD_ARG("mac:kick",		set80211kickmac),
	DEF_CMD("pureg",	1,	set80211pureg),
	DEF_CMD("-pureg",	0,	set80211pureg),
	DEF_CMD("ff",		1,	set80211fastframes),
	DEF_CMD("-ff",		0,	set80211fastframes),
	DEF_CMD("dturbo",	1,	set80211dturbo),
	DEF_CMD("-dturbo",	0,	set80211dturbo),
	DEF_CMD("bgscan",	1,	set80211bgscan),
	DEF_CMD("-bgscan",	0,	set80211bgscan),
	DEF_CMD_ARG("bgscanidle",	set80211bgscanidle),
	DEF_CMD_ARG("bgscanintvl",	set80211bgscanintvl),
	DEF_CMD_ARG("scanvalid",	set80211scanvalid),
	DEF_CMD("quiet",	1,	set80211quiet),
	DEF_CMD("-quiet",	0,	set80211quiet),
	DEF_CMD_ARG("quiet_count",	set80211quietcount),
	DEF_CMD_ARG("quiet_period",	set80211quietperiod),
	DEF_CMD_ARG("quiet_duration",	set80211quietduration),
	DEF_CMD_ARG("quiet_offset",	set80211quietoffset),
	DEF_CMD_ARG("roam:rssi",	set80211roamrssi),
	DEF_CMD_ARG("roam:rate",	set80211roamrate),
	DEF_CMD_ARG("mcastrate",	set80211mcastrate),
	DEF_CMD_ARG("ucastrate",	set80211ucastrate),
	DEF_CMD_ARG("mgtrate",		set80211mgtrate),
	DEF_CMD_ARG("mgmtrate",		set80211mgtrate),
	DEF_CMD_ARG("maxretry",		set80211maxretry),
	DEF_CMD_ARG("fragthreshold",	set80211fragthreshold),
	DEF_CMD("burst",	1,	set80211burst),
	DEF_CMD("-burst",	0,	set80211burst),
	DEF_CMD_ARG("bmiss",		set80211bmissthreshold),
	DEF_CMD_ARG("bmissthreshold",	set80211bmissthreshold),
	DEF_CMD("shortgi",	1,	set80211shortgi),
	DEF_CMD("-shortgi",	0,	set80211shortgi),
	DEF_CMD("ampdurx",	2,	set80211ampdu),
	DEF_CMD("-ampdurx",	-2,	set80211ampdu),
	DEF_CMD("ampdutx",	1,	set80211ampdu),
	DEF_CMD("-ampdutx",	-1,	set80211ampdu),
	DEF_CMD("ampdu",	3,	set80211ampdu),		/* NB: tx+rx */
	DEF_CMD("-ampdu",	-3,	set80211ampdu),
	DEF_CMD_ARG("ampdulimit",	set80211ampdulimit),
	DEF_CMD_ARG("ampdudensity",	set80211ampdudensity),
	DEF_CMD("amsdurx",	2,	set80211amsdu),
	DEF_CMD("-amsdurx",	-2,	set80211amsdu),
	DEF_CMD("amsdutx",	1,	set80211amsdu),
	DEF_CMD("-amsdutx",	-1,	set80211amsdu),
	DEF_CMD("amsdu",	3,	set80211amsdu),		/* NB: tx+rx */
	DEF_CMD("-amsdu",	-3,	set80211amsdu),
	DEF_CMD_ARG("amsdulimit",	set80211amsdulimit),
	DEF_CMD("stbcrx",	2,	set80211stbc),
	DEF_CMD("-stbcrx",	-2,	set80211stbc),
	DEF_CMD("stbctx",	1,	set80211stbc),
	DEF_CMD("-stbctx",	-1,	set80211stbc),
	DEF_CMD("stbc",		3,	set80211stbc),		/* NB: tx+rx */
	DEF_CMD("-stbc",	-3,	set80211stbc),
	DEF_CMD("ldpcrx",	2,	set80211ldpc),
	DEF_CMD("-ldpcrx",	-2,	set80211ldpc),
	DEF_CMD("ldpctx",	1,	set80211ldpc),
	DEF_CMD("-ldpctx",	-1,	set80211ldpc),
	DEF_CMD("ldpc",		3,	set80211ldpc),		/* NB: tx+rx */
	DEF_CMD("-ldpc",	-3,	set80211ldpc),
	DEF_CMD("uapsd",	1,	set80211uapsd),
	DEF_CMD("-uapsd",	0,	set80211uapsd),
	DEF_CMD("puren",	1,	set80211puren),
	DEF_CMD("-puren",	0,	set80211puren),
	DEF_CMD("doth",		1,	set80211doth),
	DEF_CMD("-doth",	0,	set80211doth),
	DEF_CMD("dfs",		1,	set80211dfs),
	DEF_CMD("-dfs",		0,	set80211dfs),
	DEF_CMD("htcompat",	1,	set80211htcompat),
	DEF_CMD("-htcompat",	0,	set80211htcompat),
	DEF_CMD("dwds",		1,	set80211dwds),
	DEF_CMD("-dwds",	0,	set80211dwds),
	DEF_CMD("inact",	1,	set80211inact),
	DEF_CMD("-inact",	0,	set80211inact),
	DEF_CMD("tsn",		1,	set80211tsn),
	DEF_CMD("-tsn",		0,	set80211tsn),
	DEF_CMD_ARG("regdomain",	set80211regdomain),
	DEF_CMD_ARG("country",		set80211country),
	DEF_CMD("indoor",	'I',	set80211location),
	DEF_CMD("-indoor",	'O',	set80211location),
	DEF_CMD("outdoor",	'O',	set80211location),
	DEF_CMD("-outdoor",	'I',	set80211location),
	DEF_CMD("anywhere",	' ',	set80211location),
	DEF_CMD("ecm",		1,	set80211ecm),
	DEF_CMD("-ecm",		0,	set80211ecm),
	DEF_CMD("dotd",		1,	set80211dotd),
	DEF_CMD("-dotd",	0,	set80211dotd),
	DEF_CMD_ARG("htprotmode",	set80211htprotmode),
	DEF_CMD("ht20",		1,	set80211htconf),
	DEF_CMD("-ht20",	0,	set80211htconf),
	DEF_CMD("ht40",		3,	set80211htconf),	/* NB: 20+40 */
	DEF_CMD("-ht40",	0,	set80211htconf),
	DEF_CMD("ht",		3,	set80211htconf),	/* NB: 20+40 */
	DEF_CMD("-ht",		0,	set80211htconf),
	DEF_CMD("vht",		IEEE80211_FVHT_VHT,		set80211vhtconf),
	DEF_CMD("-vht",		-IEEE80211_FVHT_VHT,		set80211vhtconf),
	DEF_CMD("vht40",	IEEE80211_FVHT_USEVHT40,	set80211vhtconf),
	DEF_CMD("-vht40",	-IEEE80211_FVHT_USEVHT40,	set80211vhtconf),
	DEF_CMD("vht80",	IEEE80211_FVHT_USEVHT80,	set80211vhtconf),
	DEF_CMD("-vht80",	-IEEE80211_FVHT_USEVHT80,	set80211vhtconf),
	DEF_CMD("vht160",	IEEE80211_FVHT_USEVHT160,	set80211vhtconf),
	DEF_CMD("-vht160",	-IEEE80211_FVHT_USEVHT160,	set80211vhtconf),
	DEF_CMD("vht80p80",	IEEE80211_FVHT_USEVHT80P80,	set80211vhtconf),
	DEF_CMD("-vht80p80",	-IEEE80211_FVHT_USEVHT80P80,	set80211vhtconf),
	DEF_CMD("vhtstbctx",	IEEE80211_FVHT_STBC_TX,		set80211vhtconf),
	DEF_CMD("-vhtstbctx",	-IEEE80211_FVHT_STBC_TX,	set80211vhtconf),
	DEF_CMD("vhtstbcrx",	IEEE80211_FVHT_STBC_RX,		set80211vhtconf),
	DEF_CMD("-vhtstbcrx",	-IEEE80211_FVHT_STBC_RX,	set80211vhtconf),
	DEF_CMD("vhtstbc",	(IEEE80211_FVHT_STBC_TX|IEEE80211_FVHT_STBC_RX),	set80211vhtconf),
	DEF_CMD("-vhtstbc",	-(IEEE80211_FVHT_STBC_TX|IEEE80211_FVHT_STBC_RX),	set80211vhtconf),
	DEF_CMD("rifs",		1,	set80211rifs),
	DEF_CMD("-rifs",	0,	set80211rifs),
	DEF_CMD("smps",		IEEE80211_HTCAP_SMPS_ENA,	set80211smps),
	DEF_CMD("smpsdyn",	IEEE80211_HTCAP_SMPS_DYNAMIC,	set80211smps),
	DEF_CMD("-smps",	IEEE80211_HTCAP_SMPS_OFF,	set80211smps),
	/* XXX for testing */
	DEF_CMD_ARG("chanswitch",	set80211chanswitch),

	DEF_CMD_ARG("tdmaslot",		set80211tdmaslot),
	DEF_CMD_ARG("tdmaslotcnt",	set80211tdmaslotcnt),
	DEF_CMD_ARG("tdmaslotlen",	set80211tdmaslotlen),
	DEF_CMD_ARG("tdmabintval",	set80211tdmabintval),

	DEF_CMD_ARG("meshttl",		set80211meshttl),
	DEF_CMD("meshforward",	1,	set80211meshforward),
	DEF_CMD("-meshforward",	0,	set80211meshforward),
	DEF_CMD("meshgate",	1,	set80211meshgate),
	DEF_CMD("-meshgate",	0,	set80211meshgate),
	DEF_CMD("meshpeering",	1,	set80211meshpeering),
	DEF_CMD("-meshpeering",	0,	set80211meshpeering),
	DEF_CMD_ARG("meshmetric",	set80211meshmetric),
	DEF_CMD_ARG("meshpath",		set80211meshpath),
	DEF_CMD("meshrt:flush",	IEEE80211_MESH_RTCMD_FLUSH,	set80211meshrtcmd),
	DEF_CMD_ARG("meshrt:add",	set80211addmeshrt),
	DEF_CMD_ARG("meshrt:del",	set80211delmeshrt),
	DEF_CMD_ARG("hwmprootmode",	set80211hwmprootmode),
	DEF_CMD_ARG("hwmpmaxhops",	set80211hwmpmaxhops),

	/* vap cloning support */
	DEF_CLONE_CMD_ARG("wlanaddr",	set80211clone_wlanaddr),
	DEF_CLONE_CMD_ARG("wlanbssid",	set80211clone_wlanbssid),
	DEF_CLONE_CMD_ARG("wlandev",	set80211clone_wlandev),
	DEF_CLONE_CMD_ARG("wlanmode",	set80211clone_wlanmode),
	DEF_CLONE_CMD("beacons", 1,	set80211clone_beacons),
	DEF_CLONE_CMD("-beacons", 0,	set80211clone_beacons),
	DEF_CLONE_CMD("bssid",	1,	set80211clone_bssid),
	DEF_CLONE_CMD("-bssid",	0,	set80211clone_bssid),
	DEF_CLONE_CMD("wdslegacy", 1,	set80211clone_wdslegacy),
	DEF_CLONE_CMD("-wdslegacy", 0,	set80211clone_wdslegacy),
};
static struct afswtch af_ieee80211 = {
	.af_name	= "af_ieee80211",
	.af_af		= AF_UNSPEC,
	.af_other_status = ieee80211_status,
};

static __constructor void
ieee80211_ctor(void)
{
	for (size_t i = 0; i < nitems(ieee80211_cmds);  i++)
		cmd_register(&ieee80211_cmds[i]);
	af_register(&af_ieee80211);
	clone_setdefcallback_prefix("wlan", wlan_create);
}
