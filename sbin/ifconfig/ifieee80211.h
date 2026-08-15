#pragma once

#include <net/if_strings.h>

#define WANT_NET80211	1
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_superg.h>
#include <net80211/ieee80211_tdma.h>
#include <net80211/ieee80211_mesh.h>
#include <net80211/ieee80211_wps.h>

#include <lib80211/lib80211_regdomain.h>
#include <lib80211/lib80211_ioctl.h>

#define LE_READ_2(p)				\
	((u_int16_t)				\
	 ((((const u_int8_t *)(p))[0]      ) |	\
	  (((const u_int8_t *)(p))[1] <<  8)))

#define LE_READ_4(p)				\
	((u_int32_t)				\
	 ((((const u_int8_t *)(p))[0]      ) |	\
	  (((const u_int8_t *)(p))[1] <<  8) |	\
	  (((const u_int8_t *)(p))[2] << 16) |	\
	  (((const u_int8_t *)(p))[3] << 24)))

#define BE_READ_2(p)				\
	((u_int16_t)				\
	 ((((const u_int8_t *)(p))[1]      ) |	\
	  (((const u_int8_t *)(p))[0] <<  8)))


/* Helper macros unified. */
#ifndef	_IEEE80211_MASKSHIFT
#define	_IEEE80211_MASKSHIFT(_v, _f)	(((_v) & _f) >> _f##_S)
#endif
#ifndef	_IEEE80211_SHIFTMASK
#define	_IEEE80211_SHIFTMASK(_v, _f)	(((_v) << _f##_S) & _f)
#endif

#ifndef IEEE80211_FIXED_RATE_NONE
#define	IEEE80211_FIXED_RATE_NONE	0xff
#endif

/* XXX need these publicly defined or similar */
#ifndef IEEE80211_NODE_AUTH
#define	IEEE80211_NODE_AUTH	0x000001	/* authorized for data */
#define	IEEE80211_NODE_QOS	0x000002	/* QoS enabled */
#define	IEEE80211_NODE_ERP	0x000004	/* ERP enabled */
#define	IEEE80211_NODE_PWR_MGT	0x000010	/* power save mode enabled */
#define	IEEE80211_NODE_AREF	0x000020	/* authentication ref held */
#define	IEEE80211_NODE_HT	0x000040	/* HT enabled */
#define	IEEE80211_NODE_HTCOMPAT	0x000080	/* HT setup w/ vendor OUI's */
#define	IEEE80211_NODE_WPS	0x000100	/* WPS association */
#define	IEEE80211_NODE_TSN	0x000200	/* TSN association */
#define	IEEE80211_NODE_AMPDU_RX	0x000400	/* AMPDU rx enabled */
#define	IEEE80211_NODE_AMPDU_TX	0x000800	/* AMPDU tx enabled */
#define	IEEE80211_NODE_MIMO_PS	0x001000	/* MIMO power save enabled */
#define	IEEE80211_NODE_MIMO_RTS	0x002000	/* send RTS in MIMO PS */
#define	IEEE80211_NODE_RIFS	0x004000	/* RIFS enabled */
#define	IEEE80211_NODE_SGI20	0x008000	/* Short GI in HT20 enabled */
#define	IEEE80211_NODE_SGI40	0x010000	/* Short GI in HT40 enabled */
#define	IEEE80211_NODE_ASSOCID	0x020000	/* xmit requires associd */
#define	IEEE80211_NODE_AMSDU_RX	0x040000	/* AMSDU rx enabled */
#define	IEEE80211_NODE_AMSDU_TX	0x080000	/* AMSDU tx enabled */
#define	IEEE80211_NODE_VHT	0x100000	/* VHT enabled */
#define	IEEE80211_NODE_LDPC	0x200000	/* LDPC enabled */
#define	IEEE80211_NODE_UAPSD	0x400000	/* UAPSD enabled */
#endif

/* XXX should also figure out where to put these for k/u-space sharing. */
#ifndef IEEE80211_FVHT_VHT
#define	IEEE80211_FVHT_VHT	0x000000001	/* CONF: VHT supported */
#define	IEEE80211_FVHT_USEVHT40	0x000000002	/* CONF: Use VHT40 */
#define	IEEE80211_FVHT_USEVHT80	0x000000004	/* CONF: Use VHT80 */
#define	IEEE80211_FVHT_USEVHT80P80 0x000000008	/* CONF: Use VHT 80+80 */
#define	IEEE80211_FVHT_USEVHT160 0x000000010	/* CONF: Use VHT160 */
#define	IEEE80211_FVHT_STBC_TX  0x00000020	/* CONF: STBC tx enabled */
#define	IEEE80211_FVHT_STBC_RX  0x00000040	/* CONF: STBC rx enabled */
#endif

#define	MAXCHAN	1536		/* max 1.5K channels */

static const char *modename[IEEE80211_MODE_MAX] = {
	[IEEE80211_MODE_AUTO]	  = "auto",
	[IEEE80211_MODE_11A]	  = "11a",
	[IEEE80211_MODE_11B]	  = "11b",
	[IEEE80211_MODE_11G]	  = "11g",
	[IEEE80211_MODE_FH]	  = "fh",
	[IEEE80211_MODE_TURBO_A]  = "turboA",
	[IEEE80211_MODE_TURBO_G]  = "turboG",
	[IEEE80211_MODE_STURBO_A] = "sturbo",
	[IEEE80211_MODE_11NA]	  = "11na",
	[IEEE80211_MODE_11NG]	  = "11ng",
	[IEEE80211_MODE_HALF]	  = "half",
	[IEEE80211_MODE_QUARTER]  = "quarter",
	[IEEE80211_MODE_VHT_2GHZ] = "11acg",
	[IEEE80211_MODE_VHT_5GHZ] = "11ac",
};

int get80211(if_ctx *ctx, int type, void *data, int len);
const char *rsn_cipher(const u_int8_t *sel);
const char *rsn_keymgmt(const u_int8_t *sel);
const char *iename(uint8_t elemid, const u_int8_t *vp);
int chanpref(const struct ieee80211_channel *c);
void getchaninfo(if_ctx *ctx);
const char * get_chaninfo(const struct ieee80211_channel *c, int precise, char buf[],
			  size_t bsize);

int ieee80211_mhz2ieee(int freq, int flags);
struct regdata *getregdata(void);
int copy_essid(char buf[], size_t bufsize, const u_int8_t *essid, size_t essid_len);
int iswpaoui(const u_int8_t *frm);
int iswmeinfo(const u_int8_t *frm);
int iswmeparam(const u_int8_t *frm);
int isatherosoui(const u_int8_t *frm);
int istdmaoui(const uint8_t *frm);
int iswpsoui(const uint8_t *frm);

int getmaxrate(const uint8_t rates[15], uint8_t nrates);
const char *getcaps(int capinfo);
const char *getflags(int flags);
int gettxseq(const struct ieee80211req_sta_info *si);
int getrxseq(const struct ieee80211req_sta_info *si);
const char *mesh_linkstate_string(uint8_t state);

static struct ieee80211req_chaninfo *chaninfo;
static struct ieee80211_regdomain regdomain;

static int gotregdomain = 0;

static struct ieee80211_roamparams_req roamparams;

static int gotroam = 0;

static struct ieee80211_txparams_req txparams;

static int gottxparams = 0;

static struct ieee80211_channel curchan;

static int gotcurchan = 0;

static struct ifmediareq *global_ifmr;

/* HT */
static int htconf = 0;
static int gothtconf = 0;

const char * wpa_cipher(const u_int8_t *sel);
const char * wpa_keymgmt(const u_int8_t *sel);
