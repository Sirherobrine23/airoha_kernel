/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ECONET_ETH_H
#define ECONET_ETH_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/soc/airoha/airoha_offload.h>

#include "airoha_common.h"

struct airoha_eth_soc_data;

/* QDMA packet descriptors and descriptor messages. */
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))

/** etx:  */
struct etx {
	/**
	 * See accessors:
	 * get_etx_unknown0()
	 * set_etx_unknown0()
	 * get_etx_sp_tag()
	 * set_etx_sp_tag()
	 * is_etx_oam()
	 * set_etx_oam()
	 * get_etx_channel()
	 * set_etx_channel()
	 * get_etx_queue()
	 * set_etx_queue()
	 */
	u32 bitfield_0;

	/**
	 * See accessors:
	 * is_etx_ico()
	 * set_etx_ico()
	 * is_etx_uco()
	 * set_etx_uco()
	 * is_etx_tco()
	 * set_etx_tco()
	 * is_etx_sco()
	 * set_etx_sco()
	 * get_etx_udf_pmap()
	 * set_etx_udf_pmap()
	 * get_etx_fport()
	 * set_etx_fport()
	 * is_etx_vlan_en()
	 * set_etx_vlan_en()
	 * get_etx_vlan_type()
	 * set_etx_vlan_type()
	 * get_etx_vlan_tag()
	 * set_etx_vlan_tag()
	 */
	u32 bitfield_1;

};

/**
 * Bitfield accessors for: etx bitfield_0
 */

#define ETX_UNKNOWN0_MASK				GENMASK(31, 28)
#define ETX_SP_TAG_MASK					GENMASK(27, 12)
#define ETX_OAM						BIT(11)
#define ETX_CHANNEL_MASK				GENMASK(10, 3)
#define ETX_QUEUE_MASK					GENMASK(2, 0)


/** Unused, called "rev" probably short for reserved */
static inline u8 get_etx_unknown0(struct etx *x)
{
	return FIELD_GET(ETX_UNKNOWN0_MASK, x->bitfield_0);
}
static inline void set_etx_unknown0(struct etx *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_UNKNOWN0_MASK, v);
}

/**
 * MediaTek "Special Tag" format which encapsulates both switch port number and
 * possible VLAN
 */
static inline u16 get_etx_sp_tag(struct etx *x)
{
	return FIELD_GET(ETX_SP_TAG_MASK, x->bitfield_0);
}
static inline void set_etx_sp_tag(struct etx *x, u16 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_SP_TAG_MASK, v);
}

/** OAM (management) frame, never used with Ethernet transmissions */
static inline bool is_etx_oam(struct etx *x)
{
	return FIELD_GET(ETX_OAM, x->bitfield_0);
}
static inline void set_etx_oam(struct etx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_OAM, v);
}

/** The channel number for QoS prioritization */
static inline u8 get_etx_channel(struct etx *x)
{
	return FIELD_GET(ETX_CHANNEL_MASK, x->bitfield_0);
}
static inline void set_etx_channel(struct etx *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_CHANNEL_MASK, v);
}

/** The queue number for QoS prioritization */
static inline u8 get_etx_queue(struct etx *x)
{
	return FIELD_GET(ETX_QUEUE_MASK, x->bitfield_0);
}
static inline void set_etx_queue(struct etx *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_QUEUE_MASK, v);
}

/**
 * Bitfield accessors for: etx bitfield_1
 */

enum etx_fport {
	ETX_FPORT_QDMA0_CPU				= 0,
	ETX_FPORT_GDM1					= 1,
	ETX_FPORT_GDM2					= 2,
	ETX_FPORT_QDMA0_HWF				= 3,
	ETX_FPORT_PPE					= 4,
	ETX_FPORT_QDMA1_CPU				= 5,
	ETX_FPORT_QDMA1_HWF				= 6,
	ETX_FPORT_DROP					= 7,
};
enum etx_vlan_type {
	ETX_VLAN_TYPE_8100				= 0,
	ETX_VLAN_TYPE_9100				= 2,
	ETX_VLAN_TYPE_88A8				= 1,
	ETX_VLAN_TYPE_UNKNOWN				= 3,
};

#define ETX_ICO						BIT(31)
#define ETX_UCO						BIT(30)
#define ETX_TCO						BIT(29)
#define ETX_SCO						BIT(28)
#define ETX_UDF_PMAP_MASK				GENMASK(27, 22)
#define ETX_FPORT_MASK					GENMASK(21, 19)
#define ETX_VLAN_EN					BIT(18)
#define ETX_VLAN_TYPE_MASK				GENMASK(17, 16)
#define ETX_VLAN_TAG_MASK				GENMASK(15, 0)


/** Checksum offload, probably IP */
static inline bool is_etx_ico(struct etx *x)
{
	return FIELD_GET(ETX_ICO, x->bitfield_1);
}
static inline void set_etx_ico(struct etx *x, bool v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_ICO, v);
}

/** Checksum offload, probably UDP */
static inline bool is_etx_uco(struct etx *x)
{
	return FIELD_GET(ETX_UCO, x->bitfield_1);
}
static inline void set_etx_uco(struct etx *x, bool v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_UCO, v);
}

/** Checksum offload, probably TCP */
static inline bool is_etx_tco(struct etx *x)
{
	return FIELD_GET(ETX_TCO, x->bitfield_1);
}
static inline void set_etx_tco(struct etx *x, bool v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_TCO, v);
}

/** Unknown, maybe SCTP checksum offload */
static inline bool is_etx_sco(struct etx *x)
{
	return FIELD_GET(ETX_SCO, x->bitfield_1);
}
static inline void set_etx_sco(struct etx *x, bool v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_SCO, v);
}

/** Unknown / unused */
static inline u8 get_etx_udf_pmap(struct etx *x)
{
	return FIELD_GET(ETX_UDF_PMAP_MASK, x->bitfield_1);
}
static inline void set_etx_udf_pmap(struct etx *x, u8 v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_UDF_PMAP_MASK, v);
}

/**
 * Where in the Frame Engine to send the packet to QDMA0_CPU / QDMA1_CPU sends
 * to CPU via the relevant QDMA engine. QDMA0_HWF and QDMA1_HWF goes to QDMA
 * hardware forwarding. GDM1 is the LAN, GDM2 is the WAN, and PPE is the Packet
 * Processing Engine.
 */
static inline enum etx_fport get_etx_fport(struct etx *x)
{
	return FIELD_GET(ETX_FPORT_MASK, x->bitfield_1);
}
static inline void set_etx_fport(struct etx *x, enum etx_fport v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_FPORT_MASK, v);
}

/** If 1 then add a vlan header to the packet */
static inline bool is_etx_vlan_en(struct etx *x)
{
	return FIELD_GET(ETX_VLAN_EN, x->bitfield_1);
}
static inline void set_etx_vlan_en(struct etx *x, bool v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_VLAN_EN, v);
}

/** Which type of vlan to add to the packet header */
static inline enum etx_vlan_type get_etx_vlan_type(struct etx *x)
{
	return FIELD_GET(ETX_VLAN_TYPE_MASK, x->bitfield_1);
}
static inline void set_etx_vlan_type(struct etx *x, enum etx_vlan_type v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_VLAN_TYPE_MASK, v);
}

/** The VLAN number, if vlan_en is set */
static inline u16 get_etx_vlan_tag(struct etx *x)
{
	return FIELD_GET(ETX_VLAN_TAG_MASK, x->bitfield_1);
}
static inline void set_etx_vlan_tag(struct etx *x, u16 v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_VLAN_TAG_MASK, v);
}

/**
 * qdma_desc_erx
 *
 *     3                     2                   1                   0
 *     1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  0 |                            unknown0                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  4 |nknwn|I|P|F|T|L|A| sport |   crsn  |         ppe_entry         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  8 |                           unknown2                          |N|
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 12 |             sp_tag            |              tci              |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 16
 *
 * @unknown0 (32 bit): Unknown / unused field, first word in descriptor
 * @bitfield_0 (32 bit):
 *   @unknown1 "nknwn" (bits 31..29): Revision number? (unused)
 *   @ip6 "I" (bit 28): IPv6 packet indicator
 *   @ip4 "P" (bit 27): IPv4 packet indicator
 *   @ip4f "F" (bit 26): IPv4 header checksum failure
 *   @tack "T" (bit 25): TCP ACK flag
 *   @l2vld "L" (bit 24): Layer 2 valid flag
 *   @l4f "A" (bit 23): TCP/UDP checksum failure
 *   @sport (bits 22..19): Where the packet came from, mostly unknown / unused,
 *                         needs testing
 *   @crsn (bits 18..14): Most likely a MediaTek PPE CPU_REASON
 *   @ppe_entry (bits 13..0): PPE (Packet Processing Engine) entry index
 * @bitfield_1 (32 bit):
 *   @unknown2 (bits 31..1): Reserved
 *   @untag "N" (bit 0): VLAN untag flag
 * @bitfield_3 (32 bit):
 *   @sp_tag (bits 31..16): MediaTek "Special Tag" for switch port/VLAN encoding
 *   @tci (bits 15..0): The TCI of any vlan tag that was unpopped beneath the
 *                      MTK "Special Tag"
 */
struct qdma_desc_erx {
	u32 unknown0;
	u32 bitfield_0;
	u32 bitfield_1;
	u32 bitfield_3;
};

/* qdma_desc_erx bitfield_0 */

#define ERX_UNKNOWN1_MASK				GENMASK(31, 29)
#define ERX_IP6						BIT(28)
#define ERX_IP4						BIT(27)
#define ERX_IP4F					BIT(26)
#define ERX_TACK					BIT(25)
#define ERX_L2VLD					BIT(24)
#define ERX_L4F						BIT(23)
#define ERX_SPORT_MASK					GENMASK(22, 19)
#define ERX_CRSN_MASK					GENMASK(18, 14)
#define ERX_PPE_ENTRY_MASK				GENMASK(13, 0)

static inline u8 get_erx_unknown1(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_UNKNOWN1_MASK, x->bitfield_0);
}
static inline void set_erx_unknown1(struct qdma_desc_erx *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_UNKNOWN1_MASK, v);
}
static inline bool is_erx_ip6(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_IP6, x->bitfield_0);
}
static inline void set_erx_ip6(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_IP6, v);
}
static inline bool is_erx_ip4(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_IP4, x->bitfield_0);
}
static inline void set_erx_ip4(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_IP4, v);
}
static inline bool is_erx_ip4f(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_IP4F, x->bitfield_0);
}
static inline void set_erx_ip4f(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_IP4F, v);
}
static inline bool is_erx_tack(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_TACK, x->bitfield_0);
}
static inline void set_erx_tack(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_TACK, v);
}
static inline bool is_erx_l2vld(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_L2VLD, x->bitfield_0);
}
static inline void set_erx_l2vld(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_L2VLD, v);
}
static inline bool is_erx_l4f(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_L4F, x->bitfield_0);
}
static inline void set_erx_l4f(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_L4F, v);
}
static inline u8 get_erx_sport(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_SPORT_MASK, x->bitfield_0);
}
static inline void set_erx_sport(struct qdma_desc_erx *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_SPORT_MASK, v);
}
static inline u8 get_erx_crsn(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_CRSN_MASK, x->bitfield_0);
}
static inline void set_erx_crsn(struct qdma_desc_erx *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_CRSN_MASK, v);
}
static inline u16 get_erx_ppe_entry(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_PPE_ENTRY_MASK, x->bitfield_0);
}
static inline void set_erx_ppe_entry(struct qdma_desc_erx *x, u16 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_PPE_ENTRY_MASK, v);
}

/* qdma_desc_erx bitfield_1 */

#define ERX_UNKNOWN2_MASK				GENMASK(31, 1)
#define ERX_UNTAG					BIT(0)

static inline u32 get_erx_unknown2(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_UNKNOWN2_MASK, x->bitfield_1);
}
static inline void set_erx_unknown2(struct qdma_desc_erx *x, u32 v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ERX_UNKNOWN2_MASK, v);
}
static inline bool is_erx_untag(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_UNTAG, x->bitfield_1);
}
static inline void set_erx_untag(struct qdma_desc_erx *x, bool v)
{
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ERX_UNTAG, v);
}

/* qdma_desc_erx bitfield_3 */

#define ERX_SP_TAG_MASK					GENMASK(31, 16)
#define ERX_TCI_MASK					GENMASK(15, 0)

static inline u16 get_erx_sp_tag(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_SP_TAG_MASK, x->bitfield_3);
}
static inline void set_erx_sp_tag(struct qdma_desc_erx *x, u16 v)
{
	x->bitfield_3 = FIELD_SET(x->bitfield_3, ERX_SP_TAG_MASK, v);
}
static inline u16 get_erx_tci(struct qdma_desc_erx *x)
{
	return FIELD_GET(ERX_TCI_MASK, x->bitfield_3);
}
static inline void set_erx_tci(struct qdma_desc_erx *x, u16 v)
{
	x->bitfield_3 = FIELD_SET(x->bitfield_3, ERX_TCI_MASK, v);
}

/** desc: QDMA Packet Descriptor */
struct desc {
	/**
	 * desc_unknown0: Reserved / unused, we still export the symbol so we can show
	 * it in debugging
	 */
	u32 unknown0;

	/** desc_info:  */
	struct desc_info {
		/**
		 * See accessors:
		 * is_desc_info_done()
		 * set_desc_info_done()
		 * is_desc_info_dropped()
		 * set_desc_info_dropped()
		 * is_desc_info_nls()
		 * set_desc_info_nls()
		 * get_desc_info_unknown1()
		 * set_desc_info_unknown1()
		 * get_desc_info_pkt_len()
		 * set_desc_info_pkt_len()
		 */
		u32 word;

	} info;

	/** desc_pkt_addr: Physical (DMA) address of the packet */
	u32 pkt_addr;

	/** desc_next_idx: Index of the next descriptor in the ring, max 4096. */
	u32 next_idx;

	/** desc_msg: This is either Ethernet RX, Ethernet TX, xPON RX, or xPON TX */
	union desc_msg {
		struct qdma_desc_erx erx;

		struct etx etx;

		u32 raw[4];

	} msg;

};

/**
 * Bitfield accessors for: desc_info word
 */

#define DESC_INFO_DONE					BIT(31)
#define DESC_INFO_DROPPED				BIT(30)
#define DESC_INFO_NLS					BIT(29)
#define DESC_INFO_UNKNOWN1_MASK				GENMASK(28, 16)
#define DESC_INFO_PKT_LEN_MASK				GENMASK(15, 0)


/**
 * Descriptor Done flag, this roughly means that the DSCP "belongs to the
 * driver", the hardware will set it when it is done receiving or sending and
 * will check to make sure it's not touching a DSCP that is not meant for it.
 * This is not strictly necessary, you can determine which packets are yours
 * only through ring indexes and the TX Done List, and setting and checking of
 * this flag can be deactivated.
 */
static inline bool is_desc_info_done(struct desc_info *x)
{
	return FIELD_GET(DESC_INFO_DONE, x->word);
}
static inline void set_desc_info_done(struct desc_info *x, bool v)
{
	x->word = FIELD_SET(x->word, DESC_INFO_DONE, v);
}

/** Packet has been dropped */
static inline bool is_desc_info_dropped(struct desc_info *x)
{
	return FIELD_GET(DESC_INFO_DROPPED, x->word);
}
static inline void set_desc_info_dropped(struct desc_info *x, bool v)
{
	x->word = FIELD_SET(x->word, DESC_INFO_DROPPED, v);
}

/** Unknown meaning but used on EN761627 and EN7580 */
static inline bool is_desc_info_nls(struct desc_info *x)
{
	return FIELD_GET(DESC_INFO_NLS, x->word);
}
static inline void set_desc_info_nls(struct desc_info *x, bool v)
{
	x->word = FIELD_SET(x->word, DESC_INFO_NLS, v);
}

/**
 * Reserved / unused, we still export the symbol so we can show it in debugging
 */
static inline u16 get_desc_info_unknown1(struct desc_info *x)
{
	return FIELD_GET(DESC_INFO_UNKNOWN1_MASK, x->word);
}
static inline void set_desc_info_unknown1(struct desc_info *x, u16 v)
{
	x->word = FIELD_SET(x->word, DESC_INFO_UNKNOWN1_MASK, v);
}

/** Length of the packet in bytes */
static inline u16 get_desc_info_pkt_len(struct desc_info *x)
{
	return FIELD_GET(DESC_INFO_PKT_LEN_MASK, x->word);
}
static inline void set_desc_info_pkt_len(struct desc_info *x, u16 v)
{
	x->word = FIELD_SET(x->word, DESC_INFO_PKT_LEN_MASK, v);
}

/** fwdesc: QDMA Hardware Forward Packet Descriptor */
struct fwdesc {
	/** fwdesc_pkt_addr: Physical (DMA) address of the packet */
	u32 pkt_addr;

	/** fwdesc_info:  */
	struct fwdesc_info {
		/**
		 * See accessors:
		 * is_fwdesc_info_ctx()
		 * set_fwdesc_info_ctx()
		 * is_fwdesc_info_ctx_ring()
		 * set_fwdesc_info_ctx_ring()
		 * get_fwdesc_info_ctx_idx()
		 * set_fwdesc_info_ctx_idx()
		 * get_fwdesc_info_pkt_len()
		 * set_fwdesc_info_pkt_len()
		 */
		u32 word;

	} info;

	/**
	 * fwdesc_msg: This is either Ethernet RX, Ethernet TX, xPON RX, or xPON TX
	 */
	union fwdesc_msg {
		struct etx etx;

		u32 raw[2];

	} msg;

};

/**
 * Bitfield accessors for: fwdesc_info word
 */

#define FWDESC_INFO_CTX					BIT(31)
#define FWDESC_INFO_CTX_RING				BIT(28)
#define FWDESC_INFO_CTX_IDX_MASK			GENMASK(27, 16)
#define FWDESC_INFO_PKT_LEN_MASK			GENMASK(15, 0)


/** True if there is a context descriptor (i.e. it is send, not a forward) */
static inline bool is_fwdesc_info_ctx(struct fwdesc_info *x)
{
	return FIELD_GET(FWDESC_INFO_CTX, x->word);
}
static inline void set_fwdesc_info_ctx(struct fwdesc_info *x, bool v)
{
	x->word = FIELD_SET(x->word, FWDESC_INFO_CTX, v);
}

/**
 * If ctx is true then this is the number of the context ring (0 or 1) because
 * there are 2 transmit rings.
 */
static inline bool is_fwdesc_info_ctx_ring(struct fwdesc_info *x)
{
	return FIELD_GET(FWDESC_INFO_CTX_RING, x->word);
}
static inline void set_fwdesc_info_ctx_ring(struct fwdesc_info *x, bool v)
{
	x->word = FIELD_SET(x->word, FWDESC_INFO_CTX_RING, v);
}

/**
 * If ctx is true then this is the index within the TX ring of the context
 * packet descriptor.
 */
static inline u16 get_fwdesc_info_ctx_idx(struct fwdesc_info *x)
{
	return FIELD_GET(FWDESC_INFO_CTX_IDX_MASK, x->word);
}
static inline void set_fwdesc_info_ctx_idx(struct fwdesc_info *x, u16 v)
{
	x->word = FIELD_SET(x->word, FWDESC_INFO_CTX_IDX_MASK, v);
}

/** Length of the packet in bytes */
static inline u16 get_fwdesc_info_pkt_len(struct fwdesc_info *x)
{
	return FIELD_GET(FWDESC_INFO_PKT_LEN_MASK, x->word);
}
static inline void set_fwdesc_info_pkt_len(struct fwdesc_info *x, u16 v)
{
	x->word = FIELD_SET(x->word, FWDESC_INFO_PKT_LEN_MASK, v);
}

/* QDMA register layout and field accessors. */
#ifndef FIELD_SET
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))
#endif

/** qchain_regs: QDMA Chain Registers */
struct qchain_regs {
	/** qchain_regs_txbase: TX descriptor array address */
	u32 txbase;

	/** qchain_regs_rxbase: RX descriptor array address */
	u32 rxbase;

	/** qchain_regs_tx_cpui: TX ring CPU (driver) index */
	u32 tx_cpui;

	/** qchain_regs_tx_hwi: TX ring hardware index */
	u32 tx_hwi;

	/** qchain_regs_rx_cpui: RX ring CPU (driver) index */
	u32 rx_cpui;

	/** qchain_regs_rx_hwi: TX ring hardware index */
	u32 rx_hwi;

};

/** qregs: QDMA Registers */
struct qregs {
	/** qregs_version:  */
	u32 version;

	/** qregs_qcfg:  */
	struct qregs_qcfg {
		/**
		 * See accessors:
		 * is_qregs_qcfg_rx_2b_offset()
		 * set_qregs_qcfg_rx_2b_offset()
		 * get_qregs_qcfg_dma_pref()
		 * set_qregs_qcfg_dma_pref()
		 * is_qregs_qcfg_msg_word_swap()
		 * set_qregs_qcfg_msg_word_swap()
		 * is_qregs_qcfg_dscp_byte_swap()
		 * set_qregs_qcfg_dscp_byte_swap()
		 * is_qregs_qcfg_payload_byte_sw()
		 * set_qregs_qcfg_payload_byte_sw()
		 * is_qregs_qcfg_vchnl_map_en()
		 * set_qregs_qcfg_vchnl_map_en()
		 * is_qregs_qcfg_vchnl_map_mode()
		 * set_qregs_qcfg_vchnl_map_mode()
		 * is_qregs_qcfg_qdma_lpbk_rxq_sel()
		 * set_qregs_qcfg_qdma_lpbk_rxq_sel()
		 * is_qregs_qcfg_slm_release_en()
		 * set_qregs_qcfg_slm_release_en()
		 * is_qregs_qcfg_tx_immediate_done()
		 * set_qregs_qcfg_tx_immediate_done()
		 * is_qregs_qcfg_irq_en()
		 * set_qregs_qcfg_irq_en()
		 * is_qregs_qcfg_gdm_loopback()
		 * set_qregs_qcfg_gdm_loopback()
		 * is_qregs_qcfg_qdma_loopback()
		 * set_qregs_qcfg_qdma_loopback()
		 * is_qregs_qcfg_check_done()
		 * set_qregs_qcfg_check_done()
		 * is_qregs_qcfg_tx_wb_done()
		 * set_qregs_qcfg_tx_wb_done()
		 * get_qregs_qcfg_burst_size()
		 * set_qregs_qcfg_burst_size()
		 * is_qregs_qcfg_rx_dma_busy()
		 * set_qregs_qcfg_rx_dma_busy()
		 * is_qregs_qcfg_rx_dma_en()
		 * set_qregs_qcfg_rx_dma_en()
		 * is_qregs_qcfg_tx_dma_busy()
		 * set_qregs_qcfg_tx_dma_busy()
		 * is_qregs_qcfg_tx_dma_en()
		 * set_qregs_qcfg_tx_dma_en()
		 */
		u32 bitfield_0;

	} qdma_cfg;

	/** qregs_qchain0:  */
	struct qchain_regs qchain0;

	/**
	 * qregs_hwf_desc_addr: Hardware forwarding descriptor table address. This
	 * memory must be forward-descriptor-size (16 bytes) * fwd_desc_n in size.
	 */
	u32 hwf_desc_addr;

	/**
	 * qregs_hwf_data_addr: Hardware forwarding packet content address. This memory
	 * must be pkt_sz *  fwd_desc_n in size.
	 */
	u32 hwf_data_addr;

	/**
	 * See accessors:
	 * get_qregs_hwf_cfg_pkt_sz()
	 * set_qregs_hwf_cfg_pkt_sz()
	 * get_qregs_hwf_cfg_low_th()
	 * set_qregs_hwf_cfg_low_th()
	 */
	struct hwf_cfg { u32 word; } hwf_cfg;

	u32 unused_0;

	/**
	 * qregs_hwf_cfg1: Hardware forwarding configuration (also called LMGR)
	 * See accessors:
	 * is_qregs_hwf_cfg1_start()
	 * set_qregs_hwf_cfg1_start()
	 * is_qregs_hwf_cfg1_overhead_en()
	 * set_qregs_hwf_cfg1_overhead_en()
	 * get_qregs_hwf_cfg1_overhead()
	 * set_qregs_hwf_cfg1_overhead()
	 * get_qregs_hwf_cfg1_fwd_desc_n()
	 * set_qregs_hwf_cfg1_fwd_desc_n()
	 */
	struct qregs_hwf_cfg1 { u32 word; } hwf_cfg1;

	u8 unused_1[12];

	/** qregs_channel_retire: Referred to as QDMA_CSR_LMGR_CHNL_RETIRE */
	u32 channel_retire;

	u8 unused_2[12];

	/**
	 * qregs_int_status: When an interrupt is triggered, these are the pending
	 * events
	 */
	u32 int_status;

	/** qregs_int_enable: Enabled interrupts */
	u32 int_enable;

	/** qregs_tx_int_delay: Interrupt delay for reducing interrupt load */
	u32 tx_int_delay;

	/** qregs_rx_int_delay: Interrupt delay for reducing interrupt load */
	u32 rx_int_delay;

	/** qregs_doneq:  */
	struct qregs_doneq {
		/** qregs_doneq_addr:  */
		u32 address;

		/**
		 * qregs_doneq_cfg:
		 * See accessors:
		 * get_qregs_doneq_cfg_int_threshold()
		 * set_qregs_doneq_cfg_int_threshold()
		 * get_qregs_doneq_cfg_size()
		 * set_qregs_doneq_cfg_size()
		 */
		struct qregs_doneq_cfg { u32 word; } config;

		/**
		 * qregs_doneq_pop_back: Pop this number of items from the back of the the
		 * done queue, max 255
		 */
		u32 pop_back;

		/**
		 * qregs_doneq_state:
		 * See accessors:
		 * get_qregs_doneq_state_length()
		 * get_qregs_doneq_state_head_index()
		 */
		struct qregs_doneq_state { u32 word; } state;

		/**
		 * qregs_doneq_wait_time: If there is anything in the queue, fire an interrupt
		 * after this number of units of time, unit is 20 microseconds.
		 */
		u32 wait_time;

	} done_queue;

	u8 unused_3[12];

	/**
	 * See accessors:
	 * is_qregs_wrr_mode_use_16b()
	 * set_qregs_wrr_mode_use_16b()
	 * is_qregs_wrr_mode_by_byte()
	 * set_qregs_wrr_mode_by_byte()
	 */
	struct wrr_mode { u32 word; } wrr_mode;

	u32 unused_4;

	/**
	 * qregs_wrr_weight: Read and write a WRR weight for a channel+queue pair.
	 * Called QDMA_CSR_TXWRR_WEIGHT_CFG.
	 */
	struct qregs_wrr_weight {
		/**
		 * See accessors:
		 * set_qregs_wrr_weight_write()
		 * is_qregs_wrr_weight_done()
		 * set_qregs_wrr_weight_channel()
		 * set_qregs_wrr_weight_queue()
		 */
		u16 bitfield_0;

		u8 unused_0;

		/**
		 * qregs_wrr_weight_value: The WRR weight to read or write for the configured
		 * channel+queue.
		 */
		u8 value;

	} wrr_weight;

	u32 unused_5;

	/**
	 * qregs_buf_usage_cfg: Called QDMA_CSR_PSE_BUF_USAGE_CFG, a bitfield, TODO
	 * document
	 */
	u32 buf_usage_cfg;

	/**
	 * qregs_tx_meter_cfg: Called QDMA_CSR_EGRESS_RATEMETER_CFG, a bitfield, TODO
	 * document
	 */
	u32 tx_meter_cfg;

	/**
	 * qregs_tx_limit_cfg: Called QDMA_CSR_EGRESS_RATELIMIT_CFG, a bitfield, TODO
	 * document
	 */
	u32 tx_limit_cfg;

	/**
	 * qregs_tx_limit_param: Called QDMA_CSR_RATELIMIT_PARAMETER_CFG, a bitfield,
	 * per-channel tx rate limit, TODO document
	 */
	u32 tx_limit_param;

	/** qregs_tx_congest_cfg:  */
	struct qregs_tx_congest_cfg {
		/**
		 * See accessors:
		 * is_qregs_tx_congest_cfg_tail_drop_en()
		 * set_qregs_tx_congest_cfg_tail_drop_en()
		 * is_qregs_tx_congest_cfg_dei_drop_en()
		 * set_qregs_tx_congest_cfg_dei_drop_en()
		 * is_qregs_tx_congest_cfg_dyncong_en()
		 * set_qregs_tx_congest_cfg_dyncong_en()
		 * is_qregs_tx_congest_cfg_max_thr_blk_tx1()
		 * set_qregs_tx_congest_cfg_max_thr_blk_tx1()
		 * is_qregs_tx_congest_cfg_min_thr_blk_tx1()
		 * set_qregs_tx_congest_cfg_min_thr_blk_tx1()
		 * is_qregs_tx_congest_cfg_max_thr_blk_tx0()
		 * set_qregs_tx_congest_cfg_max_thr_blk_tx0()
		 * is_qregs_tx_congest_cfg_min_thr_blk_tx0()
		 * set_qregs_tx_congest_cfg_min_thr_blk_tx0()
		 * get_qregs_tx_congest_cfg_dyncong_margin()
		 * set_qregs_tx_congest_cfg_dyncong_margin()
		 * get_qregs_tx_congest_cfg_dyncong_dei_scale()
		 * set_qregs_tx_congest_cfg_dyncong_dei_scale()
		 * is_qregs_tx_congest_cfg_dyncong_upd_wrr()
		 * set_qregs_tx_congest_cfg_dyncong_upd_wrr()
		 * is_qregs_tx_congest_cfg_dyncong_upd_txrx()
		 * set_qregs_tx_congest_cfg_dyncong_upd_txrx()
		 * is_qregs_tx_congest_cfg_dyncong_upd_tick()
		 * set_qregs_tx_congest_cfg_dyncong_upd_tick()
		 */
		u16 bitfield_0;

		/**
		 * qregs_tx_congest_cfg_dyncong_tick: Dynamic congestion ticker rate in
		 * microseconds
		 */
		u16 dyncong_tick;

	} tx_congest_cfg;

	/** qregs_tx_congest_thr:  */
	struct qregs_tx_congest_thr {
		/**
		 * qregs_tx_congest_thr_max: When total buffer usage exceeds this, drop all
		 * packets except VIP
		 */
		u16 max;

		/**
		 * qregs_tx_congest_thr_min: When total buffer usage exceeds this, dynamic
		 * congestion control will start
		 */
		u16 min;

	} tx_congest_thr;

	/**
	 * qregs_tx_per_ch_dthr: Called QDMA_CSR_TXQ_DYN_CHNLTHR_CFG, tx per-channel
	 * dynamic threshold max/min threshold, TODO document
	 */
	u32 tx_per_ch_dthr;

	/**
	 * qregs_tx_per_q_dthr: Called QDMA_CSR_TXQ_DYN_QUEUETHR_CFG, tx per-queue
	 * dynamic threshold max/min threshold, TODO document
	 */
	u32 tx_per_q_dthr;

	/**
	 * qregs_tx_per_q_sthr: Called QDMA_CSR_STATIC_QUEUE_THR(0..7), tx per-queue
	 * static thresholds, if dynamic thresholds are disabled, TODO document
	 */
	u8 unused_6[32];

	u8 unused_7[16];

	/** qregs_debug:  */
	struct qregs_debug {
		/**
		 * See accessors:
		 * set_qregs_debug_mem_ctl_write()
		 * is_qregs_debug_mem_ctl_done()
		 * set_qregs_debug_mem_ctl_dataset()
		 * set_qregs_debug_mem_ctl_word_of_elem()
		 * set_qregs_debug_mem_ctl_elem()
		 */
		struct mem_ctl { u32 word; } mem_ctl;

		/**
		 * qregs_debug_mem_lo: Read or write the low bits of a memory element (use
		 * with mem_ctl) Called QDMA_CSR_DBG_MEM_XS_DATA_LO.
		 */
		u32 mem_lo;

		/**
		 * qregs_debug_mem_hi: Read or write the high bits of a memory element (use
		 * with mem_ctl) Called QDMA_CSR_DBG_MEM_XS_DATA_HI. Not used in practice.
		 */
		u32 mem_hi;

		u32 unused_0;

		/**
		 * qregs_debug_hwf_desc_free: Called QDMA_CSR_DBG_LMGR_STATUS, number of free
		 * hardware forwarding descriptors
		 */
		u32 hwf_desc_free;

		/**
		 * qregs_debug_hwd_buf_used: Number of bytes of buffer used for hardwre
		 * forwarding
		 */
		u32 hwd_buf_used;

		/**
		 * qregs_debug_probe_lo: Called QDMA_CSR_DBG_QDMA_PROBE_LO, unknown usage,
		 * TODO document
		 */
		u32 probe_lo;

		/**
		 * qregs_debug_probe_hi: Called QDMA_CSR_DBG_QDMA_PROBE_HI, unknown usage,
		 * TODO document
		 */
		u32 probe_hi;

	} debug;

	/**
	 * qregs_rxring_size:
	 * See accessors:
	 * get_qregs_rxring_size_ring0()
	 * set_qregs_rxring_size_ring0()
	 * get_qregs_rxring_size_ring1()
	 * set_qregs_rxring_size_ring1()
	 */
	struct qregs_rxring_size { u32 word; } rxring_size;

	/**
	 * qregs_rxring_low:
	 * See accessors:
	 * get_qregs_rxring_low_ring0()
	 * set_qregs_rxring_low_ring0()
	 * get_qregs_rxring_low_ring1()
	 * set_qregs_rxring_low_ring1()
	 */
	struct qregs_rxring_low { u32 word; } rxring_low;

	/** qregs_qchain1:  */
	struct qchain_regs qchain1;

	/** qregs_cpu_rx_limit: CPU protection RX limit, TODO document */
	u32 cpu_rx_limit;

	/** qregs_cpu_rx_limit_val: CPU protection RX limit, TODO document */
	u32 cpu_rx_limit_val;

	u8 unused_8[20];

	/** qregs_vch_wrr: Virtual channel WRR weighting, TODO document */
	u32 vch_wrr;

	/** qregs_vch_qmode: Virtual channel QoS mode (WRR / SP), TODO document */
	u32 vch_qmode;

	u8 unused_9[28];

	/**
	 * qregs_ch_lim_en: Per-channel rate-limit enable, each bit corriponds to one
	 * channel
	 */
	u32 ch_lim_en;

	u8 unused_10[28];

	/**
	 * qregs_ch_qmode: Per-channel queue prioritization mode (WRR / SP), TODO
	 * document
	 */
	u8 unused_11[16];

	u8 unused_12[112];

	/**
	 * qregs_ch_tx_rate: Channel data rate, each word corrisponds to 2 channels,
	 * upper 16 bits is the odd channel number, lower 16 is the even.
	 */
	u8 unused_13[64];

	u8 unused_14[64];

	/**
	 * qregs_ch_drop: Drop counter for normal packets, each byte corrisponds to one
	 * of the 32 channels.
	 */
	u8 unused_15[32];

	u8 unused_16[32];

	/**
	 * qregs_ch_dei_drop: Drop counter for Drop-Elligable (DEI) packets, each byte
	 * corrisponds to one of the 32 channels.
	 */
	u8 unused_17[32];

	u8 unused_18[32];

	/**
	 * qregs_pkt_ctrs: Every other word is a counter config and a counter value,
	 * called QDMA_CSR_DBG_CNTR_CFG / QDMA_CSR_DBG_CNTR_VAR. Definitely 40
	 * counters, possible 64. TODO document
	 */
	u8 unused_19[512];

	u8 unused_20[2816];

};

/**
 * Bitfield accessors for: qregs_qcfg bitfield_0
 */

enum qregs_qcfg_dma_pref {
	QREGS_QCFG_DMA_PREF_ROUND_ROBIN			= 0,
	QREGS_QCFG_DMA_PREF_FRX_TX1_TX0			= 1,
	QREGS_QCFG_DMA_PREF_TX1_FRX_TX0			= 2,
	QREGS_QCFG_DMA_PREF_TX1_TX0_FRX			= 3,
};
enum qregs_qcfg_burst_size {
	QREGS_QCFG_BURST_SIZE_16_BYTES			= 0,
	QREGS_QCFG_BURST_SIZE_32_BYTES			= 1,
	QREGS_QCFG_BURST_SIZE_64_BYTES			= 2,
	QREGS_QCFG_BURST_SIZE_128_BYTES			= 3,
};

#define QREGS_QCFG_RX_2B_OFFSET				BIT(31)
#define QREGS_QCFG_DMA_PREF_MASK			GENMASK(30, 29)
#define QREGS_QCFG_MSG_WORD_SWAP			BIT(28)
#define QREGS_QCFG_DSCP_BYTE_SWAP			BIT(27)
#define QREGS_QCFG_PAYLOAD_BYTE_SW			BIT(26)
#define QREGS_QCFG_VCHNL_MAP_EN				BIT(25)
#define QREGS_QCFG_VCHNL_MAP_MODE			BIT(24)
#define QREGS_QCFG_QDMA_LPBK_RXQ_SEL			BIT(22)
#define QREGS_QCFG_SLM_RELEASE_EN			BIT(21)
#define QREGS_QCFG_TX_IMMEDIATE_DONE			BIT(20)
#define QREGS_QCFG_IRQ_EN				BIT(19)
#define QREGS_QCFG_GDM_LOOPBACK				BIT(17)
#define QREGS_QCFG_QDMA_LOOPBACK			BIT(16)
#define QREGS_QCFG_CHECK_DONE				BIT(7)
#define QREGS_QCFG_TX_WB_DONE				BIT(6)
#define QREGS_QCFG_BURST_SIZE_MASK			GENMASK(5, 4)
#define QREGS_QCFG_RX_DMA_BUSY				BIT(3)
#define QREGS_QCFG_RX_DMA_EN				BIT(2)
#define QREGS_QCFG_TX_DMA_BUSY				BIT(1)
#define QREGS_QCFG_TX_DMA_EN				BIT(0)


/** If enabled, use (dscp_pkt_ptr + 2) as starting address for rx payload */
static inline bool is_qregs_qcfg_rx_2b_offset(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_RX_2B_OFFSET, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_2b_offset(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_2B_OFFSET, v);
}

/** DMA channel scheduling preference, FRX means "Forwarding and RX" */
static inline enum qregs_qcfg_dma_pref get_qregs_qcfg_dma_pref(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_DMA_PREF_MASK, x->bitfield_0);
}
static inline void set_qregs_qcfg_dma_pref(struct qregs_qcfg *x, enum qregs_qcfg_dma_pref v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_DMA_PREF_MASK, v);
}

/**
 * Enable message word swap, don't know what this does but every implementation
 * sets it on Big Endian.
 */
static inline bool is_qregs_qcfg_msg_word_swap(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_MSG_WORD_SWAP, x->bitfield_0);
}
static inline void set_qregs_qcfg_msg_word_swap(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_MSG_WORD_SWAP, v);
}

/**
 * Endian-swap packet descriptors (?), drivers always set this on Big Endian
 * machines.
 */
static inline bool is_qregs_qcfg_dscp_byte_swap(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_DSCP_BYTE_SWAP, x->bitfield_0);
}
static inline void set_qregs_qcfg_dscp_byte_swap(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_DSCP_BYTE_SWAP, v);
}

/**
 * Endian-swap payload bytes, drivers always set this on Big Endian machines.
 */
static inline bool is_qregs_qcfg_payload_byte_sw(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_PAYLOAD_BYTE_SW, x->bitfield_0);
}
static inline void set_qregs_qcfg_payload_byte_sw(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_PAYLOAD_BYTE_SW, v);
}

/** Enable virtual mapping to group queues per physical channel */
static inline bool is_qregs_qcfg_vchnl_map_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_VCHNL_MAP_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_vchnl_map_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_VCHNL_MAP_EN, v);
}

/** Map of 4 virtual channels per physical channel, 0 = map 2 */
static inline bool is_qregs_qcfg_vchnl_map_mode(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_VCHNL_MAP_MODE, x->bitfield_0);
}
static inline void set_qregs_qcfg_vchnl_map_mode(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_VCHNL_MAP_MODE, v);
}

/**
 * If enabled, qdma loopback goes to queue 1, otherwise it goes to queue zero
 */
static inline bool is_qregs_qcfg_qdma_lpbk_rxq_sel(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_QDMA_LPBK_RXQ_SEL, x->bitfield_0);
}
static inline void set_qregs_qcfg_qdma_lpbk_rxq_sel(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_QDMA_LPBK_RXQ_SEL, v);
}

/** Enable qdma fwd path release slm_block */
static inline bool is_qregs_qcfg_slm_release_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_SLM_RELEASE_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_slm_release_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_SLM_RELEASE_EN, v);
}

/** QDMA generate pkt_done itself instead of using pse pkt_done */
static inline bool is_qregs_qcfg_tx_immediate_done(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_IMMEDIATE_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_immediate_done(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_IMMEDIATE_DONE, v);
}

/** Enable "interrupt queue" (i.e. Done List) for tx dma done */
static inline bool is_qregs_qcfg_irq_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_IRQ_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_irq_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_IRQ_EN, v);
}

/** Enable gdm loopback tx packet to rx path */
static inline bool is_qregs_qcfg_gdm_loopback(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_GDM_LOOPBACK, x->bitfield_0);
}
static inline void set_qregs_qcfg_gdm_loopback(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_GDM_LOOPBACK, v);
}

/** Enable hw qdma loopback tx packet to rx path */
static inline bool is_qregs_qcfg_qdma_loopback(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_QDMA_LOOPBACK, x->bitfield_0);
}
static inline void set_qregs_qcfg_qdma_loopback(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_QDMA_LOOPBACK, v);
}

/**
 * Check the done bit of descriptor and don't use descriptors which are marked
 * done. If disabled, the QDMA engine will determine if a descriptor is usable
 * based only on the ring pointers.
 */
static inline bool is_qregs_qcfg_check_done(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_CHECK_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_check_done(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_CHECK_DONE, v);
}

/**
 * Set the "done" bit in tx descriptor after sending. If disabled then the
 * engine will skip setting the done bit and rely on the driver to check the
 * Done List (i.e. `irq_en`).
 */
static inline bool is_qregs_qcfg_tx_wb_done(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_WB_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_wb_done(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_WB_DONE, v);
}

/** Number of bytes per DMA burst */
static inline enum qregs_qcfg_burst_size get_qregs_qcfg_burst_size(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_BURST_SIZE_MASK, x->bitfield_0);
}
static inline void set_qregs_qcfg_burst_size(struct qregs_qcfg *x, enum qregs_qcfg_burst_size v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_BURST_SIZE_MASK, v);
}

/** RX DMA engine currently busy */
static inline bool is_qregs_qcfg_rx_dma_busy(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_RX_DMA_BUSY, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_dma_busy(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_DMA_BUSY, v);
}

/** Enable RX DMA */
static inline bool is_qregs_qcfg_rx_dma_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_RX_DMA_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_dma_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_DMA_EN, v);
}

/** TX DMA engine currently busy */
static inline bool is_qregs_qcfg_tx_dma_busy(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_DMA_BUSY, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_dma_busy(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_DMA_BUSY, v);
}

/** Enable TX DMA */
static inline bool is_qregs_qcfg_tx_dma_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_DMA_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_dma_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_DMA_EN, v);
}

/**
 * Bitfield accessors for: struct hwf_cfg
 * Hardware forwarding configuration
 */

enum qregs_hwf_cfg_pkt_sz {
	QREGS_HWF_CFG_PKT_SZ_2048			= 0,
	QREGS_HWF_CFG_PKT_SZ_4096			= 1,
	QREGS_HWF_CFG_PKT_SZ_8192			= 2,
	QREGS_HWF_CFG_PKT_SZ_16384			= 3,
};

#define QREGS_HWF_CFG_PKT_SZ_MASK			GENMASK(29, 28)
#define QREGS_HWF_CFG_LOW_TH_MASK			GENMASK(12, 0)


/**
 * The size of the packet buffers in hwf_data_addr (and therefore the maximum
 * effective MTU).
 */
static inline enum qregs_hwf_cfg_pkt_sz get_qregs_hwf_cfg_pkt_sz(struct hwf_cfg *x)
{
	return FIELD_GET(QREGS_HWF_CFG_PKT_SZ_MASK, x->word);
}
static inline void set_qregs_hwf_cfg_pkt_sz(struct hwf_cfg *x, enum qregs_hwf_cfg_pkt_sz v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG_PKT_SZ_MASK, v);
}

/**
 * When number of available (not busy) hardware descriptors is below this,
 * generate an interrupt and pause hardware forwarding.
 */
static inline u16 get_qregs_hwf_cfg_low_th(struct hwf_cfg *x)
{
	return FIELD_GET(QREGS_HWF_CFG_LOW_TH_MASK, x->word);
}
static inline void set_qregs_hwf_cfg_low_th(struct hwf_cfg *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG_LOW_TH_MASK, v);
}

/**
 * Bitfield accessors for: qregs_hwf_cfg1
 * Register layout (32-bit word):
 * - Bit 31: START
 * - Bit 24: OVERHEAD_EN
 * - Bits 23-16: OVERHEAD (8 bits)
 * - Bits 15-0: FWD_DESC_N (16 bits)
 */

#define QREGS_HWF_CFG1_START				BIT(31)
#define QREGS_HWF_CFG1_OVERHEAD_EN			BIT(24)
#define QREGS_HWF_CFG1_OVERHEAD_MASK			GENMASK(23, 16)
#define QREGS_HWF_CFG1_FWD_DESC_N_MASK			GENMASK(15, 0)


/** Start up the hardware forwarding subsystem */
static inline bool is_qregs_hwf_cfg1_start(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_START, x->word);
}
static inline void set_qregs_hwf_cfg1_start(struct qregs_hwf_cfg1 *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_START, v);
}

/** When set, add overhead to packet size for accounting purposes */
static inline bool is_qregs_hwf_cfg1_overhead_en(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_OVERHEAD_EN, x->word);
}
static inline void set_qregs_hwf_cfg1_overhead_en(struct qregs_hwf_cfg1 *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_OVERHEAD_EN, v);
}

/** Amount of overhead to add to packet size for accounting */
static inline u8 get_qregs_hwf_cfg1_overhead(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_OVERHEAD_MASK, x->word);
}
static inline void set_qregs_hwf_cfg1_overhead(struct qregs_hwf_cfg1 *x, u8 v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_OVERHEAD_MASK, v);
}

/** Number of forward descriptors to use */
static inline u16 get_qregs_hwf_cfg1_fwd_desc_n(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_FWD_DESC_N_MASK, x->word);
}
static inline void set_qregs_hwf_cfg1_fwd_desc_n(struct qregs_hwf_cfg1 *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_FWD_DESC_N_MASK, v);
}

/**
 * Bitfield accessors for: struct wrr_mode
 * WRR control register, called QDMA_CSR_TXWRR_MODE_CFG.
 */

#define QREGS_WRR_MODE_USE_16B				BIT(31)
#define QREGS_WRR_MODE_BY_BYTE				BIT(3)


/** If enabled, weighting is based on 16 byte units, otherwise 64 byte. */
static inline bool is_qregs_wrr_mode_use_16b(struct wrr_mode *x)
{
	return FIELD_GET(QREGS_WRR_MODE_USE_16B, x->word);
}
static inline void set_qregs_wrr_mode_use_16b(struct wrr_mode *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_WRR_MODE_USE_16B, v);
}

/** If enabled, weighting is by byte, otherwise by packet. */
static inline bool is_qregs_wrr_mode_by_byte(struct wrr_mode *x)
{
	return FIELD_GET(QREGS_WRR_MODE_BY_BYTE, x->word);
}
static inline void set_qregs_wrr_mode_by_byte(struct wrr_mode *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_WRR_MODE_BY_BYTE, v);
}

/**
 * Bitfield accessors for: qregs_wrr_weight bitfield_0
 */

#define QREGS_WRR_WEIGHT_WRITE				BIT(15)
#define QREGS_WRR_WEIGHT_DONE				BIT(14)
#define QREGS_WRR_WEIGHT_CHANNEL_MASK			GENMASK(7, 3)
#define QREGS_WRR_WEIGHT_QUEUE_MASK			GENMASK(2, 0)

static inline void set_qregs_wrr_weight_write(struct qregs_wrr_weight *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_WRR_WEIGHT_WRITE, v);
}

/**
 * After performing an operation, this bit asserts from the hardware to indicate
 * the operation is done.
 */
static inline bool is_qregs_wrr_weight_done(struct qregs_wrr_weight *x)
{
	return FIELD_GET(QREGS_WRR_WEIGHT_DONE, x->bitfield_0);
}
static inline void set_qregs_wrr_weight_channel(struct qregs_wrr_weight *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_WRR_WEIGHT_CHANNEL_MASK, v);
}
static inline void set_qregs_wrr_weight_queue(struct qregs_wrr_weight *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_WRR_WEIGHT_QUEUE_MASK, v);
}

/**
 * Bitfield accessors for: qregs_tx_congest_cfg bitfield_0
 * Configuration for TX congestion dropping
 */

enum qregs_tx_congest_cfg_dyncong_margin {
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_0_PCT	= 0,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_25_PCT	= 1,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_50_PCT	= 2,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_100_PCT	= 3,
};
enum qregs_tx_congest_cfg_dyncong_dei_scale {
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_HALF	= 0,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_QUARTER	= 1,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_EIGHTH	= 2,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_SIXTEENTH = 3,
};

#define QREGS_TX_CONGEST_CFG_TAIL_DROP_EN		BIT(15)
#define QREGS_TX_CONGEST_CFG_DEI_DROP_EN		BIT(14)
#define QREGS_TX_CONGEST_CFG_DYNCONG_EN			BIT(13)
#define QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1		BIT(11)
#define QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1		BIT(10)
#define QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0		BIT(9)
#define QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0		BIT(8)
#define QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK	GENMASK(7, 6)
#define QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK	GENMASK(5, 4)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR		BIT(2)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX		BIT(1)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK		BIT(0)

static inline bool is_qregs_tx_congest_cfg_tail_drop_en(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_TAIL_DROP_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_tail_drop_en(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_TAIL_DROP_EN, v);
}

/** Support 802.1ad DEI packet dropping */
static inline bool is_qregs_tx_congest_cfg_dei_drop_en(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DEI_DROP_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dei_drop_en(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DEI_DROP_EN, v);
}

/** Enable dynamic congestion algorithm */
static inline bool is_qregs_tx_congest_cfg_dyncong_en(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_en(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_EN, v);
}

/**
 * Block TX Ring1 when TX buffer usage exceeds max threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_max_thr_blk_tx1(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_max_thr_blk_tx1(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1, v);
}

/**
 * Block TX Ring1 when TX buffer usage exceeds min threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_min_thr_blk_tx1(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_min_thr_blk_tx1(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1, v);
}

/**
 * Block TX Ring0 when TX buffer usage exceeds max threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_max_thr_blk_tx0(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_max_thr_blk_tx0(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0, v);
}

/**
 * Block TX Ring0 when TX buffer usage exceeds min threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_min_thr_blk_tx0(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_min_thr_blk_tx0(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0, v);
}
static inline enum qregs_tx_congest_cfg_dyncong_margin get_qregs_tx_congest_cfg_dyncong_margin(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_margin(struct qregs_tx_congest_cfg *x, enum qregs_tx_congest_cfg_dyncong_margin v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK, v);
}
static inline enum qregs_tx_congest_cfg_dyncong_dei_scale get_qregs_tx_congest_cfg_dyncong_dei_scale(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_dei_scale(struct qregs_tx_congest_cfg *x, enum qregs_tx_congest_cfg_dyncong_dei_scale v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK, v);
}

/** Update dyanmic congestion after each update to WRR weights. */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_wrr(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_wrr(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR, v);
}

/** Update dyanmic congestion after each TX or RX */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_txrx(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_txrx(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX, v);
}

/** Update dyanmic congestion after each tick */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_tick(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_tick(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK, v);
}

/**
 * Bitfield accessors for: struct mem_ctl
 * Called QDMA_CSR_DBG_MEM_XS_CFG, used for debugging access to memory spaces.
 */

enum qregs_debug_mem_ctl_dataset {
	QREGS_DEBUG_MEM_CTL_DATASET_DESC		= 0,
	QREGS_DEBUG_MEM_CTL_DATASET_QUEUE		= 1,
	QREGS_DEBUG_MEM_CTL_DATASET_QOS_WEIGHT_CTR	= 2,
	QREGS_DEBUG_MEM_CTL_DATASET_DMA_IDX		= 3,
	QREGS_DEBUG_MEM_CTL_DATASET_BUF_MON		= 4,
	QREGS_DEBUG_MEM_CTL_DATASET_RL_PARAM		= 5,
	QREGS_DEBUG_MEM_CTL_DATASET_VCH_WEIGHT		= 6,
};

#define QREGS_DEBUG_MEM_CTL_WRITE			BIT(31)
#define QREGS_DEBUG_MEM_CTL_DONE			BIT(30)
#define QREGS_DEBUG_MEM_CTL_DATASET_MASK		GENMASK(26, 24)
#define QREGS_DEBUG_MEM_CTL_WORD_OF_ELEM_MASK		GENMASK(20, 16)
#define QREGS_DEBUG_MEM_CTL_ELEM_MASK			GENMASK(15, 0)

static inline void set_qregs_debug_mem_ctl_write(struct mem_ctl *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_WRITE, v);
}

/** Becomes set when the command is completed */
static inline bool is_qregs_debug_mem_ctl_done(struct mem_ctl *x)
{
	return FIELD_GET(QREGS_DEBUG_MEM_CTL_DONE, x->word);
}
static inline void set_qregs_debug_mem_ctl_dataset(struct mem_ctl *x, enum qregs_debug_mem_ctl_dataset v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_DATASET_MASK, v);
}
static inline void set_qregs_debug_mem_ctl_word_of_elem(struct mem_ctl *x, u8 v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_WORD_OF_ELEM_MASK, v);
}
static inline void set_qregs_debug_mem_ctl_elem(struct mem_ctl *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_ELEM_MASK, v);
}

/**
 * Bitfield accessors for: qregs_doneq_cfg
 * Register layout (32-bit word):
 * - Bits 27-16: INT_THRESHOLD (when done queue is this full, fire interrupt)
 * - Bits 11-0: SIZE (size of done queue buffer in 4-byte units)
 */
#define QREGS_DONEQ_CFG_INT_THRESHOLD_MASK		GENMASK(27, 16)
#define QREGS_DONEQ_CFG_SIZE_MASK			GENMASK(11, 0)

static inline u16 get_qregs_doneq_cfg_int_threshold(struct qregs_doneq_cfg *x)
{
	return FIELD_GET(QREGS_DONEQ_CFG_INT_THRESHOLD_MASK, x->word);
}
static inline void set_qregs_doneq_cfg_int_threshold(struct qregs_doneq_cfg *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_DONEQ_CFG_INT_THRESHOLD_MASK, v);
}
static inline u16 get_qregs_doneq_cfg_size(struct qregs_doneq_cfg *x)
{
	return FIELD_GET(QREGS_DONEQ_CFG_SIZE_MASK, x->word);
}
static inline void set_qregs_doneq_cfg_size(struct qregs_doneq_cfg *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_DONEQ_CFG_SIZE_MASK, v);
}

/**
 * Bitfield accessors for: qregs_doneq_state
 * Register layout (32-bit word):
 * - Bits 27-16: LENGTH (number of items waiting in the queue)
 * - Bits 11-0: HEAD_INDEX (index of the first item in the list)
 */
#define QREGS_DONEQ_STATE_LENGTH_MASK			GENMASK(27, 16)
#define QREGS_DONEQ_STATE_HEAD_INDEX_MASK		GENMASK(11, 0)

static inline u16 get_qregs_doneq_state_length(struct qregs_doneq_state *x)
{
	return FIELD_GET(QREGS_DONEQ_STATE_LENGTH_MASK, x->word);
}
static inline u16 get_qregs_doneq_state_head_index(struct qregs_doneq_state *x)
{
	return FIELD_GET(QREGS_DONEQ_STATE_HEAD_INDEX_MASK, x->word);
}

/**
 * Bitfield accessors for: qregs_rxring_size
 * Register layout (32-bit word):
 * - Bits 11-0: RING0_SIZE (size of RX ring 0, max 4095)
 * - Bits 27-16: RING1_SIZE (size of RX ring 1, max 4095)
 */
#define QREGS_RXRING_SIZE_RING0_MASK			GENMASK(11, 0)
#define QREGS_RXRING_SIZE_RING1_MASK			GENMASK(27, 16)

static inline u16 get_qregs_rxring_size_ring0(struct qregs_rxring_size *x)
{
	return FIELD_GET(QREGS_RXRING_SIZE_RING0_MASK, x->word);
}
static inline void set_qregs_rxring_size_ring0(struct qregs_rxring_size *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_SIZE_RING0_MASK, v);
}
static inline u16 get_qregs_rxring_size_ring1(struct qregs_rxring_size *x)
{
	return FIELD_GET(QREGS_RXRING_SIZE_RING1_MASK, x->word);
}
static inline void set_qregs_rxring_size_ring1(struct qregs_rxring_size *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_SIZE_RING1_MASK, v);
}

/**
 * Bitfield accessors for: qregs_rxring_low
 * Register layout (32-bit word):
 * - Bits 11-0: RING0_LOW (interrupt threshold for ring 0, max 4095)
 * - Bits 27-16: RING1_LOW (interrupt threshold for ring 1, max 4095)
 */
#define QREGS_RXRING_LOW_RING0_MASK			GENMASK(11, 0)
#define QREGS_RXRING_LOW_RING1_MASK			GENMASK(27, 16)

static inline u16 get_qregs_rxring_low_ring0(struct qregs_rxring_low *x)
{
	return FIELD_GET(QREGS_RXRING_LOW_RING0_MASK, x->word);
}
static inline void set_qregs_rxring_low_ring0(struct qregs_rxring_low *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_LOW_RING0_MASK, v);
}
static inline u16 get_qregs_rxring_low_ring1(struct qregs_rxring_low *x)
{
	return FIELD_GET(QREGS_RXRING_LOW_RING1_MASK, x->word);
}
static inline void set_qregs_rxring_low_ring1(struct qregs_rxring_low *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_LOW_RING1_MASK, v);
}

/* GDM register layout and field accessors. */
#ifndef FIELD_SET
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))
#endif

/** gdm: GDM Registers */
struct gdm {
	/** gdm_vlan: VLAN tag control */
	struct gdm_vlan {
		/** gdm_vlan_tpid: If inserting a MTK Special Tag, this TPID will be used */
		u16 tpid;

		/**
		 * See accessors:
		 * is_gdm_vlan_urx()
		 * set_gdm_vlan_urx()
		 * is_gdm_vlan_ttx()
		 * set_gdm_vlan_ttx()
		 */
		u16 bitfield_0;

	} vlan;

	/** gdm_pppoe: PPPoE header control */
	struct gdm_pppoe {
		/**
		 * See accessors:
		 * is_gdm_pppoe_ttx()
		 * set_gdm_pppoe_ttx()
		 */
		u16 bitfield_0;

		/** gdm_pppoe_pppid: PPPoE Session ID to insert on transmitted packets */
		u16 pppoe_id;

	} pppoe;

	/**
	 * See accessors:
	 * get_gdm_red_cp()
	 * set_gdm_red_cp()
	 * get_gdm_red_lp()
	 * set_gdm_red_lp()
	 * get_gdm_red_hp()
	 * set_gdm_red_hp()
	 * get_gdm_red_dscp_rxm_sz()
	 * set_gdm_red_dscp_rxm_sz()
	 */
	struct red_drop_criteria { u32 word; } red_drop_criteria;

	/**
	 * gdm_chan_en: Each bit corrisponds to a channel, if one bit is cleared then
	 * this GDM will refuse to forward traffic from that channel number.
	 */
	u32 channels_enabled;

	/**
	 * gdm_crsn_prio_lo: Priority of a packet based on the CPU_REASON for packet
	 * being sent to CPU. Each CPU_REASON corrisponds to 2 bits, 00 = crit_prio, 01
	 * = low_prio, 10 = high_prio. This word covers CPU_REASONS 0..15
	 */
	u32 cpu_reason_priority_lo;

	/**
	 * gdm_crsn_prio_hi: Priority of a packet based on the CPU_REASON for packet
	 * being sent to CPU. Each CPU_REASON corrisponds to 2 bits, 00 = crit_prio, 01
	 * = low_prio, 10 = high_prio. This word covers CPU_REASONS 16..31
	 */
	u32 cpu_reason_priority_hi;

	u8 unused_0[232];

	/**
	 * See accessors:
	 * is_gdm_fwd_cfg_vip()
	 * set_gdm_fwd_cfg_vip()
	 * is_gdm_fwd_cfg_l2lu_dcsp_2cpu()
	 * set_gdm_fwd_cfg_l2lu_dcsp_2cpu()
	 * is_gdm_fwd_cfg_l2lu_ctag_2cpu()
	 * set_gdm_fwd_cfg_l2lu_ctag_2cpu()
	 * is_gdm_fwd_cfg_l2lu_stag_2cpu()
	 * set_gdm_fwd_cfg_l2lu_stag_2cpu()
	 * is_gdm_fwd_cfg_g2_underrun_retry()
	 * set_gdm_fwd_cfg_g2_underrun_retry()
	 * is_gdm_fwd_cfg_g2_drop_256b()
	 * set_gdm_fwd_cfg_g2_drop_256b()
	 * is_gdm_fwd_cfg_drop_oversize()
	 * set_gdm_fwd_cfg_drop_oversize()
	 * is_gdm_fwd_cfg_drop_runt()
	 * set_gdm_fwd_cfg_drop_runt()
	 * is_gdm_fwd_cfg_drop_crc()
	 * set_gdm_fwd_cfg_drop_crc()
	 * is_gdm_fwd_cfg_drop_ip4_csum()
	 * set_gdm_fwd_cfg_drop_ip4_csum()
	 * is_gdm_fwd_cfg_drop_tcp_csum()
	 * set_gdm_fwd_cfg_drop_tcp_csum()
	 * is_gdm_fwd_cfg_drop_ucp_csum()
	 * set_gdm_fwd_cfg_drop_ucp_csum()
	 * is_gdm_fwd_cfg_g2_favor_oam()
	 * set_gdm_fwd_cfg_g2_favor_oam()
	 * is_gdm_fwd_cfg_l2lu_brg_cpu()
	 * set_gdm_fwd_cfg_l2lu_brg_cpu()
	 * is_gdm_fwd_cfg_l2lu_unbrg_cpu()
	 * set_gdm_fwd_cfg_l2lu_unbrg_cpu()
	 * is_gdm_fwd_cfg_strip_crc()
	 * set_gdm_fwd_cfg_strip_crc()
	 * get_gdm_fwd_cfg_mymac_fport()
	 * set_gdm_fwd_cfg_mymac_fport()
	 * get_gdm_fwd_cfg_bcast_fport()
	 * set_gdm_fwd_cfg_bcast_fport()
	 * get_gdm_fwd_cfg_mcast_fport()
	 * set_gdm_fwd_cfg_mcast_fport()
	 * get_gdm_fwd_cfg_default_fport()
	 * set_gdm_fwd_cfg_default_fport()
	 */
	struct fwd_cfg { u32 word; } fwd_cfg;

	/** gdm_tx_shaper: Traffic shaper for TX */
	u32 tx_shaper;

	/**
	 * gdm_mymac_lsb: Mac address bytes [c,d,e,f] (bytes 2-5 of MAC)
	 * Register layout (32-bit word):
	 * - Bits 31-24: MAC byte c (index 2)
	 * - Bits 23-16: MAC byte d (index 3)
	 * - Bits 15-8: MAC byte e (index 4)
	 * - Bits 7-0: MAC byte f (index 5)
	 * See accessors:
	 * get_gdm_mymac_lsb_c/d/e/f()
	 * set_gdm_mymac_lsb_c/d/e/f()
	 */
	struct gdm_mymac_lsb { u32 word; } mymac_lsb;

	/**
	 * gdm_mymac_msb: Mac address bytes [a,b] (bytes 0-1 of MAC) and mask
	 * Register layout (32-bit word):
	 * - Bits 31-24: unused
	 * - Bits 23-16: lsb_mask (LSB matching mask)
	 * - Bits 15-8: MAC byte a (index 0)
	 * - Bits 7-0: MAC byte b (index 1)
	 * See accessors:
	 * get_gdm_mymac_msb_lsb_mask/a/b()
	 * set_gdm_mymac_msb_lsb_mask/a/b()
	 */
	struct gdm_mymac_msb { u32 word; } mymac_msb;

	/** gdm_stag_en: Add Special Tag on RX frames, 0 or 1 */
	u32 stag_en;

	/**
	 * gdm_len_th:
	 * Register layout (32-bit word):
	 * - Bits 31-16: OVERSIZE_LEN (packet larger than this is oversize, max 0x3f00)
	 * - Bits 15-0: RUNT_LEN (packet smaller than this is a runt)
	 * See accessors:
	 * get_gdm_len_th_oversize_len()
	 * set_gdm_len_th_oversize_len()
	 * get_gdm_len_th_runt_len()
	 * set_gdm_len_th_runt_len()
	 */
	struct gdm_len_th { u32 word; } rx_len_threshold;

	/**
	 * See accessors:
	 * get_gdm_pcp_gdm_rx()
	 * set_gdm_pcp_gdm_rx()
	 * get_gdm_pcp_gdm_tx()
	 * set_gdm_pcp_gdm_tx()
	 * get_gdm_pcp_cdm_rx()
	 * set_gdm_pcp_cdm_rx()
	 * get_gdm_pcp_cdm_tx()
	 * set_gdm_pcp_cdm_tx()
	 */
	struct pcp { u32 word; } pcp;

	/**
	 * See accessors:
	 * get_gdm_lpbk_gap()
	 * set_gdm_lpbk_gap()
	 * get_gdm_lpbk_len()
	 * set_gdm_lpbk_len()
	 * get_gdm_lpbk_chan()
	 * set_gdm_lpbk_chan()
	 * is_gdm_lpbk_gap_mode()
	 * set_gdm_lpbk_gap_mode()
	 * is_gdm_lpbk_len_mode()
	 * set_gdm_lpbk_len_mode()
	 * is_gdm_lpbk_chan_mode()
	 * set_gdm_lpbk_chan_mode()
	 * is_gdm_lpbk_enabled()
	 * set_gdm_lpbk_enabled()
	 */
	struct loopback { u32 word; } loopback;

	/**
	 * See accessors:
	 * get_gdm_channel_retire_channel()
	 * set_gdm_channel_retire_channel()
	 * is_gdm_channel_retire_done()
	 * set_gdm_channel_retire_done()
	 * is_gdm_channel_retire_release()
	 * set_gdm_channel_retire_release()
	 */
	struct channel_retire { u32 word; } channel_retire;

	/**
	 * gdm_tx_ch_en: Each bit corripsonds to one of the 32 channels, set bit to
	 * enable channel
	 */
	u32 enabled_tx_channels;

	/**
	 * gdm_rx_ch_en: Each bit 0..16 corrisonds to one of the 16 RX channels, upper
	 * 16 bits are unused
	 */
	u32 enabled_rx_channels;

	/**
	 * See accessors:
	 * is_gdm_rx7_en()
	 * set_gdm_rx7_en()
	 * get_gdm_rx7_fport()
	 * set_gdm_rx7_fport()
	 */
	u32 bitfield_0;

	/** gdm_g2_rx_shapers: Incoming traffic shapers, only present in GDM2 */
	struct gdm_g2_rx_shapers {
		/** gdm_g2_rx_shapers_mymac: Shaper for incoming traffic to my MAC */
		u32 mymac;

		/** gdm_g2_rx_shapers_bcast: Shaper for incoming broadcast traffic */
		u32 bcast;

		/** gdm_g2_rx_shapers_mcast: Shaper for incoming multicast traffic */
		u32 mcast;

		/**
		 * gdm_g2_rx_shapers_dflt: Default shaper for incoming traffic not otherwise
		 * categorized
		 */
		u32 dflt;

	} g2_rx_shapers;

	/**
	 * See accessors:
	 * is_gdm_g1_cport_cfg_add_crc()
	 * set_gdm_g1_cport_cfg_add_crc()
	 * is_gdm_g1_cport_cfg_pad()
	 * set_gdm_g1_cport_cfg_pad()
	 * is_gdm_g1_cport_cfg_port_xfc()
	 * set_gdm_g1_cport_cfg_port_xfc()
	 * is_gdm_g1_cport_cfg_queue_xfc()
	 * set_gdm_g1_cport_cfg_queue_xfc()
	 * get_gdm_g1_cport_cfg_unknown()
	 * set_gdm_g1_cport_cfg_unknown()
	 */
	struct g1_cport_cfg { u32 word; } g1_cport_cfg;

	/**
	 * gdm_g1_cport_channel_map: Called FE_CPORT_CHN_MAP, never used, observed
	 * value is 0x76543210, GDM1 only
	 */
	u32 g1_cport_channel_map;

	/** gdm_g1_cport_shaper: Traffic shaper for CPORT */
	u32 g1_cport_shaper;

	/**
	 * gdm_g1_unknown0: Unused, no symbol, observed value 0x000003c0, GDM1 only.
	 */
	u32 g1_unknown0;

	/**
	 * gdm_g1_unknown1: Unused, no symbol, observed value 0x00000000, GDM1 only.
	 */
	u32 g1_unknown1;

	u8 unused_1[28];

	/**
	 * gdm_tx_chan_active: Each of the 32 bits represents one of the 32 channels, 1
	 * means channel is active used with channel_retire to confirm channel is
	 * shutdown. Also GDMA1_TX_CHN_VLD
	 */
	u32 tx_chan_active;

	/**
	 * gdm_rx_chan_active: Unused in practice except on EN7580, each bit represents
	 * an RX channel, 1 means RX channel is active, used with channel_retire.
	 * Unsure if 16 or 32 RX channels. Also GDMA1_RX_CHN_VLD
	 */
	u32 rx_chan_active;

	u8 unused_2[8];

	/** gdm_cdm_counters: Various packet counters for CDM1 / CDM2 */
	struct gdm_cdm_counters {
		/**
		 * gdm_cdm_counters_tx: Unused in practice, seems to be frames successfully
		 * sent by CDM, also CDMA1_TX_OK_CNT
		 */
		u32 tx;

		u8 unused_0[12];

		/**
		 * gdm_cdm_counters_rxcpu: Unused in practice, seems to be frames successfully
		 * received to CPU by CDM, also CDMA1_RXCPU_OK_CNT
		 */
		u32 rxcpu;

		/**
		 * gdm_cdm_counters_rxhwf: Unused in practice, seems to be frames successfully
		 * received to hardware forwarding chain by CDM, also CDMA1_RXHWF_OK_CNT
		 */
		u32 rxhwf;

		/**
		 * gdm_cdm_counters_ka: Unused in practice, seems to be frames sent to CPU for
		 * NAT keepalive purposes, also CDMA1_RXCPU_KA_CNT
		 */
		u32 ka;

		u32 unused_1;

		/**
		 * gdm_cdm_counters_rxcpu_drop: Unused in practice, seems to be frames dropped
		 * for congestion on the RX->CPU chain, also CDMA1_RXCPU_DROP_CNT
		 */
		u32 rxcpu_drop;

		/**
		 * gdm_cdm_counters_rxhwf_drop: Unused in practice, seems to be frames dropped
		 * for congestion on the RX->Hardware Forwarding chain, also
		 * CDMA1_RXHWF_DROP_CNT
		 */
		u32 rxhwf_drop;

		/**
		 * gdm_cdm_counters_unknown0: Unused, no symbol, observed value matches rxcpu
		 */
		u32 unknown0;

		/**
		 * gdm_cdm_counters_unknown1: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown1;

		/**
		 * gdm_cdm_counters_unknown2: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown2;

		/**
		 * gdm_cdm_counters_unknown3: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown3;

		/**
		 * gdm_cdm_counters_unknown4: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown4;

		/**
		 * gdm_cdm_counters_unknown5: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown5;

	} cdm_counters;

	u8 unused_3[48];

	/**
	 * See accessors:
	 * set_gdm_cl_cnt_rx()
	 * set_gdm_cl_cnt_tx()
	 */
	struct clear_counters { u32 word; } clear_counters;

	u8 unused_4[12];

	/** gdm_counters: Packet and byte counters for GDM port */
	struct gdm_counters {
		/** gdm_counters_tx:  */
		struct gdm_counters_tx {
			/**
			 * gdm_counters_tx_get: Unused in practice, believed to be number of packets
			 * enregistered for TX, also GDMA1_TX_GET_CNT / GDMA2_TX_GETCNT
			 */
			u32 get;

			/**
			 * gdm_counters_tx_pkts: Packets successfully transmitted, also
			 * GDMA1_TX_OK_CNT / GDMA2_TX_OKCNT
			 */
			u32 tx_pkts;

			/**
			 * gdm_counters_tx_drops: Packets dropped on transmission chain, also
			 * GDMA1_TX_DROP_CNT / GDMA2_TX_DROPCNT
			 */
			u32 drops;

			/**
			 * gdm_counters_tx_bytes: Bytes successfully transmitted, also
			 * GDMA1_TX_OK_BYTE_CNT / GDMA2_TX_OKBYTE_CNT
			 */
			u32 bytes;

			/** gdm_counters_tx_g2: GDM2 only extended TX stats */
			struct gdm_counters_tx_g2 {
				/**
				 * gdm_counters_tx_g2_epkts: Number of eth frames transmitted, should match
				 * .pkts, also GDMA2_TX_ETHCNT
				 */
				u32 tx_epkts;

				/**
				 * gdm_counters_tx_g2_ebytes: Bytes of eth bytes transmitted, also
				 * GDMA2_TX_ETHLENCNT
				 */
				u32 tx_ebytes;

				/**
				 * gdm_counters_tx_g2_edrops: Number of eth frames dropped, see
				 * GDMA2_TX_ETHDROPCNT
				 */
				u32 edrops;

				/**
				 * gdm_counters_tx_g2_bcast: Number of broadcast frames transmitted, see
				 * GDMA2_TX_ETHBCDCNT
				 */
				u32 tx_bcast;

				/**
				 * gdm_counters_tx_g2_mcast: Number of multicast frames transmitted, see
				 * GDMA2_TX_ETHMULTICASTCNT
				 */
				u32 tx_mcast;

				/**
				 * gdm_counters_tx_g2_f_less_64: Counter of TX frames of length less than
				 * 64, see GDMA2_TX_ETH_LESS64_CNT
				 */
				u32 f_less_64;

				/**
				 * gdm_counters_tx_g2_f_more_1518: Counter of TX frames of length more than
				 * 1518, see GDMA2_TX_ETH_MORE1518_CNT
				 */
				u32 f_more_1518;

				/**
				 * gdm_counters_tx_g2_f_64: Counter of TX frames of length 64, see
				 * GDMA2_TX_ETH_64_CNT
				 */
				u32 f_64;

				/**
				 * gdm_counters_tx_g2_f_65_127: Counter of TX frames of length 65-127, see
				 * GDMA2_TX_ETH_65_TO_127_CNT
				 */
				u32 f_65_127;

				/**
				 * gdm_counters_tx_g2_f_128_255: Counter of TX frames of length 128-255, see
				 * GDMA2_TX_ETH_128_TO_255_CNT
				 */
				u32 f_128_255;

				/**
				 * gdm_counters_tx_g2_f_256_511: Counter of TX frames of length 256-511, see
				 * GDMA2_TX_ETH_256_TO_511_CNT
				 */
				u32 f_256_511;

				/**
				 * gdm_counters_tx_g2_f_512_1023: Counter of TX frames of length 512-1023,
				 * see GDMA2_TX_ETH_512_TO_1023_CNT
				 */
				u32 f_512_1023;

				/**
				 * gdm_counters_tx_g2_f_1024_1518: Counter of TX frames of length 1024-1518,
				 * see GDMA2_TX_ETH_1024_TO_1518_CNT
				 */
				u32 f_1024_1518;

			} g2;

		} tx;

		u32 unused_0;

		/** gdm_counters_rx:  */
		struct gdm_counters_rx {
			/**
			 * gdm_counters_rx_pkts: Number of packets received, see GDMA1_RX_OK_CNT /
			 * GDMA2_RX_OKCNT
			 */
			u32 pkts;

			/**
			 * gdm_counters_rx_drops_fc: Packets dropped due to flow control, thought to
			 * be when port sends PAUSE signal. see GDMA1_RX_FC_DROP_CNT
			 */
			u32 drops_fc;

			/**
			 * gdm_counters_rx_drops_rc: Packets dropped due to rate control, thought to
			 * be shapers. See GDMA1_RX_RC_DROP_CNT / GDMA2_RX_RCDROPCNT
			 */
			u32 drops_rc;

			/**
			 * gdm_counters_rx_drops_overflow: Packets dropped because we ran out of
			 * queue resources, vendor code logs when this counter increases so it should
			 * not happen. GDMA2_RX_OVDROPCNT / GDMA1_RX_OVER_DROP_CNT
			 */
			u32 drops_overflow;

			/**
			 * gdm_counters_rx_drops_err: Packets dropped because of any error (crc,
			 * IP/TCP/UDP checksum, oversize, runt, etc)
			 */
			u32 drops_err;

			/**
			 * gdm_counters_rx_bytes: Received bytes, see GDMA1_RX_BYTECNT /
			 * GDMA2_RX_OKBYTECNT
			 */
			u32 bytes;

			/** gdm_counters_rx_g2: GDM2 only extended RX stats */
			struct gdm_counters_rx_g2 {
				/**
				 * gdm_counters_rx_g2_epkts: Number of eth frames received, should match
				 * .pkts, also GDMA2_RX_ETHERPCNT
				 */
				u32 epkts;

				/**
				 * gdm_counters_rx_g2_ebytes: Bytes of eth bytes received, also
				 * GDMA2_RX_ETHERPLEN
				 */
				u32 ebytes;

				/**
				 * gdm_counters_rx_g2_edrops: Number of eth frames dropped, see
				 * GDMA2_RX_ETHDROPCNT
				 */
				u32 edrops;

				/**
				 * gdm_counters_rx_g2_bcast: Number of broadcast frames received, see
				 * GDMA2_RX_ETHBCCNT
				 */
				u32 bcast;

				/**
				 * gdm_counters_rx_g2_mcast: Number of multicast frames received, see
				 * GDMA2_RX_ETHMCCNT
				 */
				u32 mcast;

				/**
				 * gdm_counters_rx_g2_ecrc: Number of frames with CRC errors, see
				 * GDMA2_RX_ETHCRCCNT
				 */
				u32 ecrc;

				/**
				 * gdm_counters_rx_g2_efrag: Number of frames with CRC error and which are
				 * less than 64 bytes (probably fragments from synchronization issue) see
				 * GDMA2_RX_ETHFRACCNT
				 */
				u32 efrag;

				/**
				 * gdm_counters_rx_g2_ejabber: Number of frames longer than 1518 with bad
				 * CRC, sign of potentially jabbering hardware, see GDMA2_RX_ETHJABCNT
				 */
				u32 ejabber;

				/**
				 * gdm_counters_rx_g2_f_less_64: Counter of TX frames of length less than
				 * 64, see GDMA2_RX_ETHRUNTCNT
				 */
				u32 f_less_64;

				/**
				 * gdm_counters_rx_g2_f_more_1518: Counter of TX frames of length more than
				 * 1518, see GDMA2_RX_ETHLONGCNT
				 */
				u32 f_more_1518;

				/**
				 * gdm_counters_rx_g2_f_64: Counter of TX frames of length 64, see
				 * GDMA2_RX_ETH_64_CNT
				 */
				u32 f_64;

				/**
				 * gdm_counters_rx_g2_f_65_127: Counter of TX frames of length 65-127, see
				 * GDMA2_RX_ETH_65_TO_127_CNT
				 */
				u32 f_65_127;

				/**
				 * gdm_counters_rx_g2_f_128_255: Counter of TX frames of length 128-255, see
				 * GDMA2_RX_ETH_128_TO_255_CNT
				 */
				u32 f_128_255;

				/**
				 * gdm_counters_rx_g2_f_256_511: Counter of TX frames of length 256-511, see
				 * GDMA2_RX_ETH_256_TO_511_CNT
				 */
				u32 f_256_511;

				/**
				 * gdm_counters_rx_g2_f_512_1023: Counter of TX frames of length 512-1023,
				 * see GDMA2_RX_ETH_512_TO_1023_CNT
				 */
				u32 f_512_1023;

				/**
				 * gdm_counters_rx_g2_f_1024_1518: Counter of TX frames of length 1024-1518,
				 * see GDMA2_RX_ETH_1024_TO_1518_CNT
				 */
				u32 f_1024_1518;

				/** gdm_counters_rx_g2_unknown0: Observed 0x00000000, possible counter */
				u32 tx_unknown0;

				/** gdm_counters_rx_g2_unknown1: Observed 0x00000000, possible counter */
				u32 unknown1;

			} g2;

		} rx;

	} counters;

};

/**
 * Bitfield accessors for: gdm_vlan bitfield_0
 */

#define GDM_VLAN_URX					BIT(1)
#define GDM_VLAN_TTX					BIT(0)


/** Untag incoming tagged packets (and put the tag in the DESC) */
static inline bool is_gdm_vlan_urx(struct gdm_vlan *x)
{
	return FIELD_GET(GDM_VLAN_URX, x->bitfield_0);
}
static inline void set_gdm_vlan_urx(struct gdm_vlan *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_VLAN_URX, v);
}

/** Insert MTK Special Tag when sending packets */
static inline bool is_gdm_vlan_ttx(struct gdm_vlan *x)
{
	return FIELD_GET(GDM_VLAN_TTX, x->bitfield_0);
}
static inline void set_gdm_vlan_ttx(struct gdm_vlan *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_VLAN_TTX, v);
}

/**
 * Bitfield accessors for: gdm_pppoe bitfield_0
 */

#define GDM_PPPOE_TTX					BIT(0)


/** Insert PPPoE header on transmitted packets */
static inline bool is_gdm_pppoe_ttx(struct gdm_pppoe *x)
{
	return FIELD_GET(GDM_PPPOE_TTX, x->bitfield_0);
}
static inline void set_gdm_pppoe_ttx(struct gdm_pppoe *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_PPPOE_TTX, v);
}

/**
 * Bitfield accessors for: struct red_drop_criteria
 */

enum gdm_red_cp {
	GDM_RED_CP_RL_OR_FULL				= 0,
	GDM_RED_CP_RL_OR_TH				= 1,
	GDM_RED_CP_RL_AND_FULL				= 2,
	GDM_RED_CP_RL_AND_TH				= 3,
};
enum gdm_red_dscp_rxm_sz {
	GDM_RED_DSCP_RXM_SZ_4B				= 0,
	GDM_RED_DSCP_RXM_SZ_8B				= 1,
	GDM_RED_DSCP_RXM_SZ_16B				= 2,
	GDM_RED_DSCP_RXM_SZ_32B				= 3,
};

#define GDM_RED_CP_MASK					GENMASK(31, 30)
#define GDM_RED_LP_MASK					GENMASK(29, 28)
#define GDM_RED_HP_MASK					GENMASK(27, 26)
#define GDM_RED_DSCP_RXM_SZ_MASK			GENMASK(25, 24)


/**
 * Trigger RED packet dropping on 1. Rate-limit-violation OR buffer is full 2.
 * Rate-limit-violation OR buffer has surpassed threshold 3.
 * Rate-limit-violation WHEN buffer is also full 4. Rate-limit-violation WHEN
 * buffer has surpassed threshold For critical priority packets (VIP)
 */
static inline enum gdm_red_cp get_gdm_red_cp(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_CP_MASK, x->word);
}
static inline void set_gdm_red_cp(struct red_drop_criteria *x, enum gdm_red_cp v)
{
	x->word = FIELD_SET(x->word, GDM_RED_CP_MASK, v);
}

/** Same as crit_prio, for packets classed as low priority by CPU Reason */
static inline u8 get_gdm_red_lp(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_LP_MASK, x->word);
}
static inline void set_gdm_red_lp(struct red_drop_criteria *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_RED_LP_MASK, v);
}

/** Same as crit_prio, for packets classed as low priority by CPU Reason */
static inline u8 get_gdm_red_hp(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_HP_MASK, x->word);
}
static inline void set_gdm_red_hp(struct red_drop_criteria *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_RED_HP_MASK, v);
}

/** Size of the msg field within the packet descriptor */
static inline enum gdm_red_dscp_rxm_sz get_gdm_red_dscp_rxm_sz(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_DSCP_RXM_SZ_MASK, x->word);
}
static inline void set_gdm_red_dscp_rxm_sz(struct red_drop_criteria *x, enum gdm_red_dscp_rxm_sz v)
{
	x->word = FIELD_SET(x->word, GDM_RED_DSCP_RXM_SZ_MASK, v);
}

/**
 * Bitfield accessors for: struct fwd_cfg
 * Configuration for GDM
 */

#define GDM_FWD_CFG_VIP					BIT(31)
#define GDM_FWD_CFG_L2LU_DCSP_2CPU			BIT(30)
#define GDM_FWD_CFG_L2LU_CTAG_2CPU			BIT(29)
#define GDM_FWD_CFG_L2LU_STAG_2CPU			BIT(28)
#define GDM_FWD_CFG_G2_UNDERRUN_RETRY			BIT(27)
#define GDM_FWD_CFG_G2_DROP_256B			BIT(26)
#define GDM_FWD_CFG_DROP_OVERSIZE			BIT(25)
#define GDM_FWD_CFG_DROP_RUNT				BIT(24)
#define GDM_FWD_CFG_DROP_CRC				BIT(23)
#define GDM_FWD_CFG_DROP_IP4_CSUM			BIT(22)
#define GDM_FWD_CFG_DROP_TCP_CSUM			BIT(21)
#define GDM_FWD_CFG_DROP_UCP_CSUM			BIT(20)
#define GDM_FWD_CFG_G2_FAVOR_OAM			BIT(19)
#define GDM_FWD_CFG_L2LU_BRG_CPU			BIT(18)
#define GDM_FWD_CFG_L2LU_UNBRG_CPU			BIT(17)
#define GDM_FWD_CFG_STRIP_CRC				BIT(16)
#define GDM_FWD_CFG_MYMAC_FPORT_MASK			GENMASK(15, 12)
#define GDM_FWD_CFG_BCAST_FPORT_MASK			GENMASK(11, 8)
#define GDM_FWD_CFG_MCAST_FPORT_MASK			GENMASK(7, 4)
#define GDM_FWD_CFG_DEFAULT_FPORT_MASK			GENMASK(3, 0)


/**
 * GDM2 only, configure whether VIP packets are included in rate control
 * calculation. They are not actually dropped in any case.
 */
static inline bool is_gdm_fwd_cfg_vip(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_VIP, x->word);
}
static inline void set_gdm_fwd_cfg_vip(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_VIP, v);
}

/** On L2LU DSCP lookup miss, send packet to CPU */
static inline bool is_gdm_fwd_cfg_l2lu_dcsp_2cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_DCSP_2CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_dcsp_2cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_DCSP_2CPU, v);
}

/** On L2LU CTAG lookup miss, send package to CPU */
static inline bool is_gdm_fwd_cfg_l2lu_ctag_2cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_CTAG_2CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_ctag_2cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_CTAG_2CPU, v);
}

/** On L2LU STAG lookup miss, send packet to CPU */
static inline bool is_gdm_fwd_cfg_l2lu_stag_2cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_STAG_2CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_stag_2cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_STAG_2CPU, v);
}

/** Unknown/Unused, see GDM2_UNDERRUN_RETRY / GDMA1_FWD_CFG_RETRY_OFFSET */
static inline bool is_gdm_fwd_cfg_g2_underrun_retry(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_G2_UNDERRUN_RETRY, x->word);
}
static inline void set_gdm_fwd_cfg_g2_underrun_retry(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_G2_UNDERRUN_RETRY, v);
}

/** Unknown/unused, referred to as GDM2_DROP_256B */
static inline bool is_gdm_fwd_cfg_g2_drop_256b(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_G2_DROP_256B, x->word);
}
static inline void set_gdm_fwd_cfg_g2_drop_256b(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_G2_DROP_256B, v);
}

/**
 * If frame length (with CRC) is greater than rx_len_threshold.oversize_len,
 * drop. Referred to as GDM2_DROP_LONG.
 */
static inline bool is_gdm_fwd_cfg_drop_oversize(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_OVERSIZE, x->word);
}
static inline void set_gdm_fwd_cfg_drop_oversize(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_OVERSIZE, v);
}

/**
 * If frame length (with CRC) is less than rx_len_threshold.runt_len, drop.
 * Referred to as GDM2_DROP_RUNT.
 */
static inline bool is_gdm_fwd_cfg_drop_runt(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_RUNT, x->word);
}
static inline void set_gdm_fwd_cfg_drop_runt(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_RUNT, v);
}

/** If eth CRC invalid, drop. See GDM2_DROP_CRC_ERR */
static inline bool is_gdm_fwd_cfg_drop_crc(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_CRC, x->word);
}
static inline void set_gdm_fwd_cfg_drop_crc(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_CRC, v);
}

/** Drop packet on IPv4 checksum error */
static inline bool is_gdm_fwd_cfg_drop_ip4_csum(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_IP4_CSUM, x->word);
}
static inline void set_gdm_fwd_cfg_drop_ip4_csum(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_IP4_CSUM, v);
}

/** Drop packet on TCP checksum error */
static inline bool is_gdm_fwd_cfg_drop_tcp_csum(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_TCP_CSUM, x->word);
}
static inline void set_gdm_fwd_cfg_drop_tcp_csum(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_TCP_CSUM, v);
}

/** Drop packet on UDP checksum error */
static inline bool is_gdm_fwd_cfg_drop_ucp_csum(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_UCP_CSUM, x->word);
}
static inline void set_gdm_fwd_cfg_drop_ucp_csum(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_UCP_CSUM, v);
}

/**
 * Favor OAM frames during transmission, this probably means raise their
 * priority. GDM2 only, EN761221 (EN7521/EN7526) only, see
 * GDMA2_TX_FAVOR_OAM_OFFSET
 */
static inline bool is_gdm_fwd_cfg_g2_favor_oam(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_G2_FAVOR_OAM, x->word);
}
static inline void set_gdm_fwd_cfg_g2_favor_oam(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_G2_FAVOR_OAM, v);
}

/**
 * Packets which are L2LU misses and which match an L2-Bridge to be forwarded to
 * the CPU. Referred to as GDMA1_FWD_CFG_BRG_OFFSET
 */
static inline bool is_gdm_fwd_cfg_l2lu_brg_cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_BRG_CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_brg_cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_BRG_CPU, v);
}

/**
 * Packets which are L2LU misses and which are NOT part of a L2-Bridge to be
 * forwarded to the CPU. Referred to as GDMA1_FWD_CFG_RUT_OFFSET
 */
static inline bool is_gdm_fwd_cfg_l2lu_unbrg_cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_UNBRG_CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_unbrg_cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_UNBRG_CPU, v);
}

/**
 * Strip eth crc trailer on RX, this is used with g1_cport_cfg.add_crc when xPON
 * WAN and LAN are bridged.
 */
static inline bool is_gdm_fwd_cfg_strip_crc(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_STRIP_CRC, x->word);
}
static inline void set_gdm_fwd_cfg_strip_crc(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_STRIP_CRC, v);
}

/** GDM_P_* port for packets with our MAC address */
static inline u8 get_gdm_fwd_cfg_mymac_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_MYMAC_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_mymac_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_MYMAC_FPORT_MASK, v);
}

/** GDM_P_* port for broadcast packets */
static inline u8 get_gdm_fwd_cfg_bcast_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_BCAST_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_bcast_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_BCAST_FPORT_MASK, v);
}

/** GDM_P_* port for multicast packets */
static inline u8 get_gdm_fwd_cfg_mcast_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_MCAST_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_mcast_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_MCAST_FPORT_MASK, v);
}

/** GDM_P_* port for other packets */
static inline u8 get_gdm_fwd_cfg_default_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DEFAULT_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_default_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DEFAULT_FPORT_MASK, v);
}

/**
 * Bitfield accessors for: struct pcp
 * These codings are defined in IEEE 802.1ad and define how priority and
 * drop-eligiblity. should be represented in the 3 bit VLAN PCP header. 8p0d
 * specifies 8 priorities of which none are drop-eligible. 7p1d specifies 7
 * priorities + p0d which is the same as p0, but marked drop-eligible. 6p2d
 * specieies 6 priorities + p0d and p1d, same as p0 and p1, but marked
 * drop-eligible. 5p3d specifies 5 priorities + p0d, p1d, and p2d, same as p0,
 * p1, p2, but marked drop-eligible.
 */

enum gdm_pcp_gdm_rx {
	GDM_PCP_GDM_RX_5P3D				= 8,
	GDM_PCP_GDM_RX_6P2D				= 4,
	GDM_PCP_GDM_RX_7P1D				= 2,
	GDM_PCP_GDM_RX_8P0D				= 1,
};

#define GDM_PCP_GDM_RX_MASK				GENMASK(15, 12)
#define GDM_PCP_GDM_TX_MASK				GENMASK(11, 8)
#define GDM_PCP_CDM_RX_MASK				GENMASK(7, 4)
#define GDM_PCP_CDM_TX_MASK				GENMASK(3, 0)

static inline enum gdm_pcp_gdm_rx get_gdm_pcp_gdm_rx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_GDM_RX_MASK, x->word);
}
static inline void set_gdm_pcp_gdm_rx(struct pcp *x, enum gdm_pcp_gdm_rx v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_GDM_RX_MASK, v);
}
static inline u8 get_gdm_pcp_gdm_tx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_GDM_TX_MASK, x->word);
}
static inline void set_gdm_pcp_gdm_tx(struct pcp *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_GDM_TX_MASK, v);
}
static inline u8 get_gdm_pcp_cdm_rx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_CDM_RX_MASK, x->word);
}
static inline void set_gdm_pcp_cdm_rx(struct pcp *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_CDM_RX_MASK, v);
}
static inline u8 get_gdm_pcp_cdm_tx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_CDM_TX_MASK, x->word);
}
static inline void set_gdm_pcp_cdm_tx(struct pcp *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_CDM_TX_MASK, v);
}

/**
 * Bitfield accessors for: struct loopback
 * This is a poorly understood / rarely used register which seems to control GDM
 * level loopback. It has gap, len, and channel. Channel seems to corrispond to
 * the QDMA channel but the meaning of gap and len are unknown. Names include
 * GDMA1_LPBK_CFG, GDMA2_LPBP_CFG, and REG_GDM_LPBK_CFG (Airoha).
 */

#define GDM_LPBK_GAP_MASK				GENMASK(31, 24)
#define GDM_LPBK_LEN_MASK				GENMASK(23, 10)
#define GDM_LPBK_CHAN_MASK				GENMASK(8, 4)
#define GDM_LPBK_GAP_MODE				BIT(3)
#define GDM_LPBK_LEN_MODE				BIT(2)
#define GDM_LPBK_CHAN_MODE				BIT(1)
#define GDM_LPBK_ENABLED				BIT(0)

static inline u8 get_gdm_lpbk_gap(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_GAP_MASK, x->word);
}
static inline void set_gdm_lpbk_gap(struct loopback *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_GAP_MASK, v);
}
static inline u16 get_gdm_lpbk_len(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_LEN_MASK, x->word);
}
static inline void set_gdm_lpbk_len(struct loopback *x, u16 v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_LEN_MASK, v);
}
static inline u8 get_gdm_lpbk_chan(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_CHAN_MASK, x->word);
}
static inline void set_gdm_lpbk_chan(struct loopback *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_CHAN_MASK, v);
}
static inline bool is_gdm_lpbk_gap_mode(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_GAP_MODE, x->word);
}
static inline void set_gdm_lpbk_gap_mode(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_GAP_MODE, v);
}
static inline bool is_gdm_lpbk_len_mode(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_LEN_MODE, x->word);
}
static inline void set_gdm_lpbk_len_mode(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_LEN_MODE, v);
}
static inline bool is_gdm_lpbk_chan_mode(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_CHAN_MODE, x->word);
}
static inline void set_gdm_lpbk_chan_mode(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_CHAN_MODE, v);
}
static inline bool is_gdm_lpbk_enabled(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_ENABLED, x->word);
}
static inline void set_gdm_lpbk_enabled(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_ENABLED, v);
}

/**
 * Bitfield accessors for: struct channel_retire
 * Shut down a channel and release its resources
 */

#define GDM_CHANNEL_RETIRE_CHANNEL_MASK			GENMASK(8, 4)
#define GDM_CHANNEL_RETIRE_DONE				BIT(1)
#define GDM_CHANNEL_RETIRE_RELEASE			BIT(0)


/** Number of the channel to release */
static inline u8 get_gdm_channel_retire_channel(struct channel_retire *x)
{
	return FIELD_GET(GDM_CHANNEL_RETIRE_CHANNEL_MASK, x->word);
}
static inline void set_gdm_channel_retire_channel(struct channel_retire *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_CHANNEL_RETIRE_CHANNEL_MASK, v);
}

/** Read 1 when channel release has completed */
static inline bool is_gdm_channel_retire_done(struct channel_retire *x)
{
	return FIELD_GET(GDM_CHANNEL_RETIRE_DONE, x->word);
}
static inline void set_gdm_channel_retire_done(struct channel_retire *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CHANNEL_RETIRE_DONE, v);
}

/** Write 1 to release the channel */
static inline bool is_gdm_channel_retire_release(struct channel_retire *x)
{
	return FIELD_GET(GDM_CHANNEL_RETIRE_RELEASE, x->word);
}
static inline void set_gdm_channel_retire_release(struct channel_retire *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CHANNEL_RETIRE_RELEASE, v);
}

/**
 * Bitfield accessors for: gdm bitfield_0
 * Apparently the lower 8 channels RX channels can be wired to forward to any
 * fport on receipt. This is not really ever used and the default value is
 * disabled + all route to PPE.
 */

#define GDM_RX7_EN					BIT(31)
#define GDM_RX7_FPORT_MASK				GENMASK(30, 28)

static inline bool is_gdm_rx7_en(struct gdm *x)
{
	return FIELD_GET(GDM_RX7_EN, x->bitfield_0);
}
static inline void set_gdm_rx7_en(struct gdm *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_RX7_EN, v);
}
static inline u8 get_gdm_rx7_fport(struct gdm *x)
{
	return FIELD_GET(GDM_RX7_FPORT_MASK, x->bitfield_0);
}
static inline void set_gdm_rx7_fport(struct gdm *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_RX7_FPORT_MASK, v);
}

/**
 * Bitfield accessors for: struct g1_cport_cfg
 * Not well known what the CPORT config does, a few bits are understood. Only
 * used in GDM1, but might still be present in GDM2.
 */

#define GDM_G1_CPORT_CFG_ADD_CRC			BIT(30)
#define GDM_G1_CPORT_CFG_PAD				BIT(26)
#define GDM_G1_CPORT_CFG_PORT_XFC			BIT(25)
#define GDM_G1_CPORT_CFG_QUEUE_XFC			BIT(24)
#define GDM_G1_CPORT_CFG_UNKNOWN_MASK			GENMASK(23, 0)


/**
 * Add the ethernet trailing CRC to incoming frames (from the switch) this is
 * used with fwd_cfg.strip_crc when xPON WAN and LAN are bridged because xPON
 * PHY always sends us a CRC.
 */
static inline bool is_gdm_g1_cport_cfg_add_crc(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_ADD_CRC, x->word);
}
static inline void set_gdm_g1_cport_cfg_add_crc(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_ADD_CRC, v);
}

/**
 * Enable padding (fe_api_set_padding, FE_CPORT_PAD), meaning is unknown but
 * this is always set at startup.
 */
static inline bool is_gdm_g1_cport_cfg_pad(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_PAD, x->word);
}
static inline void set_gdm_g1_cport_cfg_pad(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_PAD, v);
}

/**
 * FE_CPORT_PORT_XFC_MASK, always set on startup. Meaning is thought to be frame
 * engine will pause in case of congestion on port (i.e. switch)
 */
static inline bool is_gdm_g1_cport_cfg_port_xfc(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_PORT_XFC, x->word);
}
static inline void set_gdm_g1_cport_cfg_port_xfc(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_PORT_XFC, v);
}

/**
 * FE_CPORT_QUEUE_XFC_MASK, always cleared on startup. Meaning is throught to be
 * individual queue will pause in case of congestion on port (i.e. switch)
 */
static inline bool is_gdm_g1_cport_cfg_queue_xfc(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_QUEUE_XFC, x->word);
}
static inline void set_gdm_g1_cport_cfg_queue_xfc(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_QUEUE_XFC, v);
}

/** Observed value 0x000a02, never updated, unknown meaning */
static inline u32 get_gdm_g1_cport_cfg_unknown(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_UNKNOWN_MASK, x->word);
}
static inline void set_gdm_g1_cport_cfg_unknown(struct g1_cport_cfg *x, u32 v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_UNKNOWN_MASK, v);
}

/**
 * Bitfield accessors for: struct clear_counters
 */

#define GDM_CL_CNT_RX					BIT(1)
#define GDM_CL_CNT_TX					BIT(0)

static inline void set_gdm_cl_cnt_rx(struct clear_counters *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CL_CNT_RX, v);
}
static inline void set_gdm_cl_cnt_tx(struct clear_counters *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CL_CNT_TX, v);
}

/**
 * Bitfield accessors for: gdm_mymac_lsb
 * MAC address bytes c,d,e,f (indices 2-5)
 */
#define GDM_MYMAC_LSB_C_MASK				GENMASK(31, 24)
#define GDM_MYMAC_LSB_D_MASK				GENMASK(23, 16)
#define GDM_MYMAC_LSB_E_MASK				GENMASK(15, 8)
#define GDM_MYMAC_LSB_F_MASK				GENMASK(7, 0)

static inline u8 get_gdm_mymac_lsb_c(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_C_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_c(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_C_MASK, v);
}
static inline u8 get_gdm_mymac_lsb_d(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_D_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_d(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_D_MASK, v);
}
static inline u8 get_gdm_mymac_lsb_e(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_E_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_e(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_E_MASK, v);
}
static inline u8 get_gdm_mymac_lsb_f(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_F_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_f(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_F_MASK, v);
}

/**
 * Bitfield accessors for: gdm_mymac_msb
 * MAC address bytes a,b (indices 0-1) and LSB matching mask
 */
#define GDM_MYMAC_MSB_LSB_MASK_MASK			GENMASK(23, 16)
#define GDM_MYMAC_MSB_A_MASK				GENMASK(15, 8)
#define GDM_MYMAC_MSB_B_MASK				GENMASK(7, 0)

static inline u8 get_gdm_mymac_msb_lsb_mask(struct gdm_mymac_msb *x)
{
	return FIELD_GET(GDM_MYMAC_MSB_LSB_MASK_MASK, x->word);
}
static inline void set_gdm_mymac_msb_lsb_mask(struct gdm_mymac_msb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_MSB_LSB_MASK_MASK, v);
}
static inline u8 get_gdm_mymac_msb_a(struct gdm_mymac_msb *x)
{
	return FIELD_GET(GDM_MYMAC_MSB_A_MASK, x->word);
}
static inline void set_gdm_mymac_msb_a(struct gdm_mymac_msb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_MSB_A_MASK, v);
}
static inline u8 get_gdm_mymac_msb_b(struct gdm_mymac_msb *x)
{
	return FIELD_GET(GDM_MYMAC_MSB_B_MASK, x->word);
}
static inline void set_gdm_mymac_msb_b(struct gdm_mymac_msb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_MSB_B_MASK, v);
}

/**
 * Bitfield accessors for: gdm_len_th
 * Register layout (32-bit word):
 * - Bits 31-16: OVERSIZE_LEN (packet larger than this is treated as oversize)
 * - Bits 15-0: RUNT_LEN (packet smaller than this is treated as a runt)
 */
#define GDM_LEN_TH_OVERSIZE_LEN_MASK			GENMASK(31, 16)
#define GDM_LEN_TH_RUNT_LEN_MASK			GENMASK(15, 0)

static inline u16 get_gdm_len_th_oversize_len(struct gdm_len_th *x)
{
	return FIELD_GET(GDM_LEN_TH_OVERSIZE_LEN_MASK, x->word);
}
static inline void set_gdm_len_th_oversize_len(struct gdm_len_th *x, u16 v)
{
	x->word = FIELD_SET(x->word, GDM_LEN_TH_OVERSIZE_LEN_MASK, v);
}
static inline u16 get_gdm_len_th_runt_len(struct gdm_len_th *x)
{
	return FIELD_GET(GDM_LEN_TH_RUNT_LEN_MASK, x->word);
}
static inline void set_gdm_len_th_runt_len(struct gdm_len_th *x, u16 v)
{
	x->word = FIELD_SET(x->word, GDM_LEN_TH_RUNT_LEN_MASK, v);
}

/* EcoNet Ethernet driver-private data and interfaces. */
/* Number of QDMA engines in this ethernet device. */
#define ECONET_NUM_QDMA		2

/* Number of GDM ports. */
#define ECONET_NUM_GDM_PORTS	2

/* The hardware MTU limit is 16128 (see: rx_len_threshold.oversize_len).
 * This limit is set to save memory in the page pool. In the future we
 * may restart the QDMA engines on MTU change which crosses a page size
 * boundary. */
#define ECONET_MAX_PACKET_SIZE	2048

/* Internally these are properties of the QDMA engine, but they are important
 * to the GDM port driver because they define how QoS can be done. */
#define ECONET_NUM_QUEUES		8
#define ECONET_NUM_CHANNELS	32

/* Number of queues reported to the kernel.
 * Currently every chan/queue is made available. */
#define ECONET_NUM_SOFT_QUEUES	(ECONET_NUM_CHANNELS * ECONET_NUM_QUEUES)

/* A chain is 1 TX ring + 1 RX ring, each QDMA has 2 chains. */
#define QDMA_NUM_CHAINS		2
/* Each QDMA has one queue of TX-complete notifications. */
#define QDMA_NUM_TX_DONE	1
/* Each QDMA has only 1 IRQ and 1 mask + 1 status register (on EN751221). */
#define QDMA_NUM_IRQS		1
#define QDMA_REGS_PER_IRQ	1

union econet_irq_purpose {
	struct {
		enum econet_irq_purpose_type {
			IPS_INVAL = 0,
			IPS_DONE,
			IPS_LOW_DSCP,
			IPS_NO_DSCP,

			IPS_OVERFLOW,
			IPS_ERR_COHERENT,
			IPS_GPON_INT,
			IPS_EPON_INT,
			IPS_XPON_INT,
		} type: 16;

		/* If source is RX, TX, or DONE then chain is the number of the queue */
		int chain : 8;

		enum econet_irq_purpose_source {
			IPSC_RX = 1,
			IPSC_TX,
			IPSC_DONE,
			IPSC_FWD,
			IPSC_UNSPEC,
		} source: 8;
	};
	u32 word;
};

static inline char *econet_irq_purpose_type_str(enum econet_irq_purpose_type t)
{
	switch (t) {
	case IPS_DONE:
		return "DONE";
	case IPS_LOW_DSCP:
		return "LOW_DSCP";
	case IPS_NO_DSCP:
		return "NO_DSCP";
	case IPS_OVERFLOW:
		return "OVERFLOW";
	case IPS_ERR_COHERENT:
		return "ERR_COHERENT";
	case IPS_GPON_INT:
		return "GPON_INT";
	case IPS_EPON_INT:
		return "EPON_INT";
	case IPS_XPON_INT:
		return "XPON_INT";
	case IPS_INVAL:
	default:
		return "INVAL";
	}
}

static inline char *econet_irq_purpose_source_str(enum econet_irq_purpose_source s)
{
	switch (s) {
	case IPSC_RX:
		return "RX";
	case IPSC_TX:
		return "TX";
	case IPSC_DONE:
		return "DONE";
	case IPSC_FWD:
		return "FWD";
	case IPSC_UNSPEC:
		return "UNSPEC";
	default:
		return "INVAL";
	}
}


enum econet_fport {
	DPORT_CPU		= 0,
	DPORT_GDMA1		= 1,
	DPORT_GDMA2		= 2,
	DPORT_UNKNOWN_3		= 3,
	DPORT_PPE		= 4,
	DPORT_QDMA		= 5,
	DPORT_QDMA_HW		= 6,
	DPORT_DISCARD		= 7,
};

struct econet_eth {
	struct device *dev;
	struct airoha_ppe_dev *ppe;
};

/* Called in softirq context */
int econet_rx_before_recv(struct econet_eth *eth, struct sk_buff *skb,
			u8 sport);

struct econet_qdma_cfg {
	int num_rx_descs[QDMA_NUM_CHAINS];
	int num_tx_descs[QDMA_NUM_CHAINS];
	int done_list_size[QDMA_NUM_TX_DONE];
	int done_list_irq_threshold[QDMA_NUM_TX_DONE];
	int num_fwd_descs;
	int fwd_max_packet_size;
	int fwd_low_threshold;
	int num_channels;
	bool rx_2b_offset;
	const struct airoha_eth_soc_data *soc;
};

struct econet_qdma;

struct econet_qdma *econet_qdma_new(struct econet_eth *eth,
				void __iomem *qdma_regs,
				int id,
				int *irqs,
				int num_irqs,
				struct econet_qdma_cfg *cfg);

int econet_qdma_use(struct econet_qdma *qdma);
int econet_qdma_unuse(struct econet_qdma *qdma);
int econet_qdma_destroy(struct econet_qdma *qdma);
int econet_qdma_xmit(struct econet_qdma *qdma, struct sk_buff *skb,
		   union desc_msg *msg, int qid);


struct net_device *econet_alloc_gdm_port(struct econet_eth *eth,
				       struct device_node *np,
				       struct gdm __iomem *regs,
				       struct econet_qdma *qdma,
				       enum etx_fport fport,
				       bool has_g2_stats);

#define econet_rreg(reg) __extension__({ \
		BUILD_BUG_ON(sizeof(*(reg)) != sizeof(u32)); \
		union { typeof(*(reg)) v; u32 w; } __r = { \
			.w = airoha_rr((void __iomem *)(reg), 0), \
		}; \
		__r.v; \
	})

#define econet_wreg(val, reg) do { \
		BUILD_BUG_ON(sizeof(*(reg)) != sizeof(u32)); \
		BUILD_BUG_ON(!__same_type(*(reg), (val))); \
		union { typeof(*(reg)) v; u32 w; } __w = { .v = (val) }; \
		airoha_wr((void __iomem *)(reg), 0, __w.w); \
	} while (0)

#define econet_word(val) __extension__({ \
		union { typeof(val) v; u32 w; } __r = { .v = (val) }; \
		BUILD_BUG_ON(sizeof(__r) != sizeof(u32)); \
		__r.w; \
	})

#endif /* ECONET_ETH_H */
