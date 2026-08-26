/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#ifndef AIROHA_ETH_H
#define AIROHA_ETH_H

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/soc/airoha/airoha_offload.h>
#include <net/dsa.h>

/* Common Ethernet/GDM/QDMA definitions. */
struct device;
struct device_node;
struct ethtool_drvinfo;
struct sk_buff;
struct airoha_ppe_dev;
struct airoha_eth;
struct gdm;

#define AIROHA_MTK_INVALID_CHANNEL		7
#define AIROHA_MTK_HDR_LEN			4
#define AIROHA_MTK_STAG_PORT_MASK		GENMASK(5, 0)
#define AIROHA_MTK_HDR_XMIT_TAGGED_TPID_8100	1
#define AIROHA_MTK_HDR_XMIT_TAGGED_TPID_88A8	2

#define AIROHA_XPON_DATA_DUMP_LEN		128

#define EN751221_GPON_RX_CHN_MASK		GENMASK(1, 0)
#define EN751221_EPON_RX_CHN_MASK		GENMASK(7, 0)
#define EN751221_EPON_TX_CHN_MASK		(GENMASK(7, 0) | GENMASK(23, 16))
#define EN751221_EPON_HWF_CHN_MASK		GENMASK(7, 0)

#define EN751221_DSA_SPORT_BASE		8
#define EN751221_DSA_NUM_PORTS		5

#define EN7523_GPON_MAX_FRAME_LEN		2000
#define EN7523_GPON_PSE_BUF_INIT_CHAN_THR	4
#define EN7523_GPON_PSE_BUF_INIT_TOTAL_THR	0xe0
#define EN7523_GPON_PSE_BUF_FEW_CHAN_THR	0x20
#define EN7523_GPON_PSE_BUF_MANY_CHAN_THR	0x08
#define EN7523_GPON_PSE_BUF_CHAN_OVERHEAD	0x10
#define EN7523_GPON_PSE_BUF_FEW_CHAN_MAX	8
#define EN7523_GPON_DBA_RATE_KBPS		0xa00000
#define EN7523_GPON_DBA_CBS_BYTES		0x8000
#define EN7523_GPON_DBA_PBS_BYTES		0xffff

#define AIROHA_XPON_TX_OFFLOAD_FEATURES		\
	(NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | \
	 NETIF_F_SG | NETIF_F_TSO | NETIF_F_TSO6)

enum airoha_mtk_tag_mode {
	AIROHA_MTK_TAG_IN_SKB,
	AIROHA_MTK_TAG_TO_DESC,
};

/**
 * struct airoha_qdma_skb_meta - common QDMA metadata derived from an skb
 * @mtk_tag: first 16 bits of the MediaTek DSA special tag
 * @port_mask: destination-port bitmap carried by @mtk_tag
 * @channel: first destination port, or AIROHA_MTK_INVALID_CHANNEL
 * @has_mtk_tag: skb belongs to a MediaTek DSA conduit and carries a tag
 *
 * EcoNet and newer Airoha frame engines carry the same logical packet
 * metadata even though their descriptor bit layouts differ. Keep skb
 * classification common and leave raw descriptor encoding to each backend.
 */
struct airoha_qdma_skb_meta {
	u16 mtk_tag;
	u8 port_mask;
	u8 channel;
	bool has_mtk_tag;
};

void airoha_qdma_skb_get_mtk_meta(struct sk_buff *skb,
				  struct net_device *netdev,
				  enum airoha_mtk_tag_mode mode,
				  struct airoha_qdma_skb_meta *meta);

enum airoha_eth_family {
	AIROHA_ETH_FAMILY_AIROHA,
	AIROHA_ETH_FAMILY_ECONET,
};

/**
 * struct airoha_gdm_mac_ops - SoC-specific MAC callbacks
 * @mac_config: optional phylink MAC configuration callback
 * @mac_link_up: optional phylink link-up callback
 * @mac_link_down: optional phylink link-down callback
 *
 * GDM1 and GDM2 are functionally equivalent on the EcoNet and Airoha frame
 * engines.  The common object owns phylink and dispatches only the hardware
 * differences to the Ethernet backend.
 */
struct airoha_gdm_mac_ops {
	void (*mac_config)(void *priv, unsigned int mode,
			   const struct phylink_link_state *state);
	void (*mac_link_up)(void *priv, struct phy_device *phy,
			    unsigned int mode, phy_interface_t interface,
			    int speed, int duplex, bool tx_pause,
			    bool rx_pause);
	void (*mac_link_down)(void *priv, unsigned int mode,
			      phy_interface_t interface);
};

#define AIROHA_GDM_COMMON_MAGIC	0x47444d43 /* "GDMC" */

/**
 * struct airoha_gdm_common - common GDM netdev/phylink state
 * @magic: identifies a common GDM private object
 * @family: frame-engine family
 * @id: one-based GDM identifier
 * @pse_port: PPE/PSE destination port used by the backend
 * @eth: common frame-engine host
 * @netdev: associated Linux network device
 * @ppe: optional common PPE frontend
 * @priv: backend GDM object passed to @mac_ops
 * @mac_ops: optional SoC-specific MAC callbacks
 * @phylink: phylink instance
 * @phylink_config: phylink configuration owned by this GDM
 * @phylink_started: whether phylink_start() has been issued
 */
struct airoha_gdm_common {
	u32 magic;
	enum airoha_eth_family family;
	u8 id;
	u8 pse_port;
	struct airoha_eth *eth;
	struct net_device *netdev;
	struct airoha_ppe_dev *ppe;
	void *priv;
	const struct airoha_gdm_mac_ops *mac_ops;
	struct phylink *phylink;
	struct phylink_config phylink_config;
	bool phylink_started;
};

u32 airoha_rr(void __iomem *base, u32 offset);
void airoha_wr(void __iomem *base, u32 offset, u32 val);
u32 airoha_rmw(void __iomem *base, u32 offset, u32 mask, u32 val);

int airoha_eth_set_dma_mask(struct device *dev);
int airoha_eth_get_port_id(struct device *dev, struct device_node *np,
			   u32 min, u32 max, u32 *id);
struct net_device *airoha_eth_alloc_napi_dev(const char *name);
int airoha_eth_init_mac_address(struct device *dev, struct device_node *np,
				struct net_device *netdev);
void airoha_eth_get_drvinfo(struct net_device *netdev,
			    struct ethtool_drvinfo *info);

void airoha_gdm_common_init(struct airoha_gdm_common *gdm,
			    struct airoha_eth *eth, struct net_device *netdev,
			    enum airoha_eth_family family, u8 id,
			    u8 pse_port, void *priv,
			    const struct airoha_gdm_mac_ops *mac_ops);
int airoha_gdm_phylink_create(struct airoha_gdm_common *gdm,
			      struct device_node *np,
			      phy_interface_t phy_mode);
int airoha_gdm_phylink_connect(struct airoha_gdm_common *gdm,
			       bool allow_no_phy);
void airoha_gdm_phylink_disconnect(struct airoha_gdm_common *gdm);
void airoha_gdm_phylink_destroy(struct airoha_gdm_common *gdm);

static inline struct airoha_gdm_common *
airoha_gdm_common_from_netdev(struct net_device *netdev)
{
	struct airoha_gdm_common *gdm = netdev_priv(netdev);

	return gdm->magic == AIROHA_GDM_COMMON_MAGIC ? gdm : NULL;
}

#define AIROHA_MAX_NUM_GDM_DEVS		3
#define AIROHA_MAX_NUM_QDMA		2
#define AIROHA_MAX_DSA_PORTS		7
#define AIROHA_MAX_NUM_RSTS		3
#define AIROHA_MAX_MTU			9220
#define AIROHA_MAX_RX_SIZE		16128
#define AIROHA_MAX_PACKET_SIZE		2048
#define AIROHA_NUM_QOS_CHANNELS		4
#define AIROHA_NUM_QOS_QUEUES		8
#define AIROHA_NUM_NETDEV_TX_RINGS(_soc) (_soc->tx_ring + \
					 AIROHA_NUM_QOS_CHANNELS)
#define AIROHA_FE_MC_MAX_VLAN_TABLE	64
#define AIROHA_FE_MC_MAX_VLAN_PORT	16
#define AIROHA_NUM_TX_IRQ		2
/*
 * Software classification rules rather than a hardware table: one entry per
 * UNI and priority combination, all of which can share a single GEM port and
 * T-CONT. An OLT mapping eight priorities across several UNIs, plus a virtual
 * Ethernet interface point, needs far more than the thirty-two this started with.
 */
#define AIROHA_XPON_MAX_SERVICES	256
#define AIROHA_XPON_SVC_MAX_COOKIES	8
#define HW_DSCP_NUM			2048
#define IRQ_QUEUE_LEN(_n)		((_n) ? 1024 : 2048)
#define TX_DSCP_NUM(_n) 	\
	((_n) == 0 ? 1536 : 	\
	(_n) == 1 ? 128 : 	\
	(_n) == 2 ? 128 : 	\
	(_n) == 3 ? 128 : 	\
	(_n) == 4 ? 128 : 	\
	(_n) == 5 ? 128 : 	\
	(_n) == 6 ? 1024 : 	\
	(_n) == 7 ? 4096 : 1024)
#define RX_DSCP_NUM(_n)		\
	((_n) == 0 ? 512 :	\
	(_n) == 1 ? 1024 :	\
	(_n) == 2 ? 128 :	\
	(_n) == 3 ? 16 :	\
	(_n) == 4 ? 16 :	\
	(_n) == 5 ? 8 :		\
	(_n) == 6 ? 8 :		\
	(_n) == 7 ? 16 :	\
	(_n) == 8 ? 16 :	\
	(_n) == 9 ? 16 :	\
	(_n) == 10 ? 16 :	\
	(_n) == 11 ? 128 :	\
	(_n) == 12 ? 16 :	\
	(_n) == 13 ? 16 :	\
	(_n) == 14 ? 16 :	\
	(_n) == 15 ? 128 : 16)

#define AIROHA_LRO_PAGE_ORDER			2
#define EN7523_AIROHA_LRO_PAGE_ORDER		4
#define AIROHA_MAX_NUM_LRO_QUEUES		8
#define EN7523_AIROHA_MAX_NUM_LRO_QUEUES	4
#define AIROHA_RXQ_LRO_EN_MASK			GENMASK(31, 24)
#define EN7523_AIROHA_RXQ_LRO_EN_MASK		GENMASK(14, 11)
#define AIROHA_RXQ_LRO_MAX_AGG_COUNT		64
#define EN7523_AIROHA_RXQ_LRO_MAX_AGG_COUNT	30
#define EN7523_AIROHA_RXQ_LRO_MAX_AGG_SIZE	44000
#define AIROHA_RXQ_LRO_MAX_AGG_TIME		100
#define AIROHA_RXQ_LRO_MAX_AGE_TIME		2000 /* 1ms */

#define AIROHA_HW_FEATURES			\
	(NETIF_F_IP_CSUM | NETIF_F_RXCSUM |	\
	 NETIF_F_TSO6 | NETIF_F_IPV6_CSUM |	\
	 NETIF_F_SG | NETIF_F_TSO | NETIF_F_HW_TC)

#define PSE_RSV_PAGES			128
#define PSE_QUEUE_RSV_PAGES		64

#define QDMA_METER_IDX(_n)		((_n) & 0xff)
#define QDMA_METER_GROUP(_n)		(((_n) >> 8) & 0x3)

#define PPE_ENTRY_SIZE			64
#define PPE_RAM_NUM_ENTRIES_SHIFT(_n)	((_n) == 512 ? 7 : __ffs((_n) >> 10))

enum {
	QDMA_INT_REG_IDX0,
	QDMA_INT_REG_IDX1,
	QDMA_INT_REG_IDX2,
	QDMA_INT_REG_IDX3,
	QDMA_INT_REG_IDX4,
	QDMA_INT_REG_IDX5,
	QDMA_INT_REG_IDX6,
	QDMA_INT_REG_MAX
};

enum {
	HSGMII_LAN_7581_PCIE0_SRCPORT	= 0x16,
	HSGMII_LAN_7581_PCIE1_SRCPORT,
	HSGMII_LAN_7581_ETH_SRCPORT,
	HSGMII_LAN_7581_USB_SRCPORT,
};

enum {
	HSGMII_LAN_7583_ETH_SRCPORT	= 0x16,
	HSGMII_LAN_7583_PCIE_SRCPORT	= 0x18,
	HSGMII_LAN_7583_USB_SRCPORT,
};

enum {
	HSGMII_LAN_7523_PCIE0_SRCPORT	= 0x16,
	HSGMII_LAN_7523_PCIE1_SRCPORT,
	HSGMII_LAN_7523_USB_SRCPORT,
	HSGMII_LAN_7523_ETH_SRCPORT	= 0xffff,
};

enum {
	XSI_PCIE0_VIP_PORT_MASK	= BIT(22),
	XSI_PCIE1_VIP_PORT_MASK	= BIT(23),
	XSI_USB_VIP_PORT_MASK	= BIT(25),
	XSI_ETH_VIP_PORT_MASK	= BIT(24),
};

enum {
	DEV_STATE_INITIALIZED,
	DEV_STATE_REGISTERED,
};

enum {
	CDM_CRSN_QSEL_Q1 = 1,
	CDM_CRSN_QSEL_Q5 = 5,
	CDM_CRSN_QSEL_Q6 = 6,
	CDM_CRSN_QSEL_Q15 = 15,
};

enum {
	CRSN_08 = 0x8,
	CRSN_21 = 0x15, /* KA */
	CRSN_22 = 0x16, /* hit bind and force route to CPU */
	CRSN_24 = 0x18,
	CRSN_25 = 0x19,
};

enum airoha_gdm_index {
	AIROHA_GDM1_IDX = 1,
	AIROHA_GDM2_IDX = 2,
	AIROHA_GDM3_IDX = 3,
	AIROHA_GDM4_IDX = 4,
};

enum airoha_xpon_mode {
	AIROHA_XPON_MODE_GPON,
	AIROHA_XPON_MODE_EPON,
	AIROHA_XPON_MODE_XGSPON,
};

enum {
	FE_PSE_PORT_CDM1,
	FE_PSE_PORT_GDM1,
	FE_PSE_PORT_GDM2,
	FE_PSE_PORT_GDM3,
	FE_PSE_PORT_PPE1,
	FE_PSE_PORT_CDM2,
	FE_PSE_PORT_CDM3,
	FE_PSE_PORT_CDM4,
	FE_PSE_PORT_PPE2,
	FE_PSE_PORT_GDM4,
	FE_PSE_PORT_CDM5,
	FE_PSE_PORT_DROP = 0xf,
};

enum tx_sched_mode {
	TC_SCH_WRR8,
	TC_SCH_SP,
	TC_SCH_WRR7,
	TC_SCH_WRR6,
	TC_SCH_WRR5,
	TC_SCH_WRR4,
	TC_SCH_WRR3,
	TC_SCH_WRR2,
};

enum trtcm_unit_type {
	TRTCM_BYTE_UNIT,
	TRTCM_PACKET_UNIT,
};

enum trtcm_param_type {
	TRTCM_MISC_MODE, /* meter_en, pps_mode, tick_sel */
	TRTCM_TOKEN_RATE_MODE,
	TRTCM_BUCKETSIZE_SHIFT_MODE,
	TRTCM_BUCKET_COUNTER_MODE,
};

enum trtcm_mode_type {
	TRTCM_COMMIT_MODE,
	TRTCM_PEAK_MODE,
};

enum trtcm_param {
	TRTCM_TICK_SEL = BIT(0),
	TRTCM_PKT_MODE = BIT(1),
	TRTCM_METER_MODE = BIT(2),
};

#define MIN_TOKEN_SIZE				4096
#define MAX_TOKEN_SIZE_OFFSET			17
#define TRTCM_TOKEN_RATE_MASK			GENMASK(23, 6)
#define TRTCM_TOKEN_RATE_FRACTION_MASK		GENMASK(5, 0)

struct airoha_queue_entry {
	union {
		void *buf;
		struct {
			struct list_head list;
			struct sk_buff *skb;
		};
	};
	dma_addr_t dma_addr;
	u16 dma_len;
	bool dma_map_page;
};

struct airoha_queue {
	struct airoha_qdma *qdma;

	/* protect concurrent queue accesses */
	spinlock_t lock;
	struct airoha_queue_entry *entry;
	struct airoha_qdma_desc *desc;
	u16 head;
	u16 tail;

	int queued;
	int ndesc;
	int free_thr;
	int buf_size;
	bool txq_stopped;

	struct napi_struct napi;
	struct page_pool *page_pool;
	struct sk_buff *skb;

	struct list_head tx_list;
};

struct airoha_tx_irq_queue {
	struct airoha_qdma *qdma;

	struct napi_struct napi;

	int size;
	u32 *q;
};

struct airoha_hw_stats {
	struct u64_stats_sync syncp;

	/* get_stats64 */
	u64 rx_ok_pkts;
	u64 tx_ok_pkts;
	u64 rx_ok_bytes;
	u64 tx_ok_bytes;
	u64 rx_multicast;
	u64 rx_errors;
	u64 rx_drops;
	u64 tx_drops;
	u64 rx_crc_error;
	u64 rx_over_errors;
	/* ethtool stats */
	u64 tx_broadcast;
	u64 tx_multicast;
	u64 tx_len[7];
	u64 rx_broadcast;
	u64 rx_fragment;
	u64 rx_jabber;
	u64 rx_len[7];
};

enum {
	AIROHA_FOE_STATE_INVALID,
	AIROHA_FOE_STATE_UNBIND,
	AIROHA_FOE_STATE_BIND,
	AIROHA_FOE_STATE_FIN
};

enum {
	PPE_PKT_TYPE_IPV4_HNAPT = 0,
	PPE_PKT_TYPE_IPV4_ROUTE = 1,
	PPE_PKT_TYPE_BRIDGE = 2,
	PPE_PKT_TYPE_IPV4_DSLITE = 3,
	PPE_PKT_TYPE_IPV6_ROUTE_3T = 4,
	PPE_PKT_TYPE_IPV6_ROUTE_5T = 5,
	PPE_PKT_TYPE_IPV6_6RD = 7,
};

#define AIROHA_FOE_MAC_SMAC_ID		GENMASK(20, 16)
#define AIROHA_FOE_MAC_PPPOE_ID		GENMASK(15, 0)

#define AIROHA_FOE_MAC_WDMA_QOS		GENMASK(15, 12)
#define AIROHA_FOE_MAC_WDMA_BAND	BIT(11)
#define AIROHA_FOE_MAC_WDMA_WCID	GENMASK(10, 0)

struct airoha_foe_mac_info_common {
	u16 vlan1;
	u16 etype;

	u32 dest_mac_hi;

	u16 vlan2;
	u16 dest_mac_lo;

	u32 src_mac_hi;
};

struct airoha_foe_mac_info {
	struct airoha_foe_mac_info_common common;

	u16 pppoe_id;
	u16 src_mac_lo;

	/* 64-byte FOE entry mode (EN7523): no room for the trailing meter
	 * word; the per-flow meter/stats path is NPU-only and unused here.
	 */
};

#define AIROHA_FOE_IB1_UNBIND_PREBIND		BIT(24)
#define AIROHA_FOE_IB1_UNBIND_PACKETS		GENMASK(23, 8)
#define AIROHA_FOE_IB1_UNBIND_TIMESTAMP		GENMASK(7, 0)

#define AIROHA_FOE_IB1_BIND_STATIC		BIT(31)
#define AIROHA_FOE_IB1_BIND_UDP			BIT(30)
#define AIROHA_FOE_IB1_BIND_STATE		GENMASK(29, 28)
#define AIROHA_FOE_IB1_BIND_PACKET_TYPE		GENMASK(27, 25)
#define AIROHA_FOE_IB1_BIND_TTL			BIT(24)
#define AIROHA_FOE_IB1_BIND_TUNNEL_DECAP	BIT(23)
#define AIROHA_FOE_IB1_BIND_PPPOE		BIT(22)
#define AIROHA_FOE_IB1_BIND_VPM			GENMASK(21, 20)
#define AIROHA_FOE_IB1_BIND_VLAN_LAYER		GENMASK(19, 16)
#define AIROHA_FOE_IB1_BIND_KEEPALIVE		BIT(15)
#define AIROHA_FOE_IB1_BIND_TIMESTAMP		GENMASK(14, 0)

#define AIROHA_FOE_IB2_DSCP			GENMASK(31, 24)
#define AIROHA_FOE_IB2_PORT_AG			GENMASK(23, 13)
#define AIROHA_FOE_IB2_PCP			BIT(12)
#define AIROHA_FOE_IB2_MULTICAST		BIT(11)
#define AIROHA_FOE_IB2_FAST_PATH		BIT(10)
#define AIROHA_FOE_IB2_PSE_QOS			BIT(9)
#define AIROHA_FOE_IB2_PSE_PORT			GENMASK(8, 5)
#define AIROHA_FOE_IB2_NBQ			GENMASK(4, 0)

#define AIROHA_FOE_ACTDP			GENMASK(31, 24)
#define AIROHA_FOE_SHAPER_ID			GENMASK(23, 16)
#define AIROHA_FOE_CHANNEL			GENMASK(15, 11)
#define AIROHA_FOE_QID				GENMASK(10, 8)
#define AIROHA_FOE_DPI				BIT(7)
#define AIROHA_FOE_TUNNEL			BIT(6)
#define AIROHA_FOE_TUNNEL_ID			GENMASK(5, 0)

#define AIROHA_FOE_TUNNEL_MTU			GENMASK(31, 16)
#define AIROHA_FOE_ACNT_GRP3			GENMASK(15, 9)
#define AIROHA_FOE_METER_GRP3			GENMASK(8, 5)
#define AIROHA_FOE_METER_GRP2			GENMASK(4, 0)

struct airoha_foe_bridge {
	u32 dest_mac_hi;

	u16 src_mac_hi;
	u16 dest_mac_lo;

	u32 src_mac_lo;

	u32 ib2;

	u32 rsv[5];

	u32 data;

	struct airoha_foe_mac_info l2;
};

struct airoha_foe_ipv4_tuple {
	u32 src_ip;
	u32 dest_ip;
	union {
		struct {
			u16 dest_port;
			u16 src_port;
		};
		struct {
			u8 protocol;
			u8 _pad[3]; /* fill with 0xa5a5a5 */
		};
		u32 ports;
	};
};

struct airoha_foe_ipv4 {
	struct airoha_foe_ipv4_tuple orig_tuple;

	u32 ib2;

	struct airoha_foe_ipv4_tuple new_tuple;

	u32 rsv[2];

	u32 data;

	struct airoha_foe_mac_info l2;
};

struct airoha_foe_ipv4_dslite {
	struct airoha_foe_ipv4_tuple ip4;

	u32 ib2;

	u8 flow_label[3];
	u8 priority;

	u32 rsv[4];

	u32 data;

	struct airoha_foe_mac_info l2;
};

struct airoha_foe_ipv6 {
	u32 src_ip[4];
	u32 dest_ip[4];

	union {
		struct {
			u16 dest_port;
			u16 src_port;
		};
		struct {
			u8 protocol;
			u8 pad[3];
		};
		u32 ports;
	};

	/* 64-byte FOE entry mode (EN7523): drop rsv2[3] and the trailing
	 * meter word so the entry fits in 64 bytes.
	 */
	u32 data;

	u32 ib2;

	struct airoha_foe_mac_info_common l2;
};

struct airoha_foe_entry {
	union {
		struct {
			u32 ib1;
			union {
				struct airoha_foe_bridge bridge;
				struct airoha_foe_ipv4 ipv4;
				struct airoha_foe_ipv4_dslite dslite;
				struct airoha_foe_ipv6 ipv6;
				DECLARE_FLEX_ARRAY(u32, d);
			};
		};
		u8 data[PPE_ENTRY_SIZE];
	};
};

struct airoha_foe_stats {
	u32 bytes;
	u32 packets;
};

struct airoha_foe_stats64 {
	u64 bytes;
	u64 packets;
};

struct airoha_flow_data {
	struct ethhdr eth;

	union {
		struct {
			__be32 src_addr;
			__be32 dst_addr;
		} v4;

		struct {
			struct in6_addr src_addr;
			struct in6_addr dst_addr;
		} v6;
	};

	__be16 src_port;
	__be16 dst_port;

	struct {
		struct {
			u16 id;
			__be16 proto;
			u8 prio;
		} hdr[2];
		u8 num;
	} vlan;
	struct {
		u16 sid;
		u8 num;
	} pppoe;
};

enum airoha_flow_entry_type {
	FLOW_TYPE_L4,
	FLOW_TYPE_L2,
	FLOW_TYPE_L2_SUBFLOW,
};

struct airoha_flow_table_entry {
	union {
		struct hlist_node list; /* PPE L3 flow entry */
		struct {
			struct rhash_head l2_node;  /* L2 flow entry */
			struct hlist_head l2_flows; /* PPE L2 subflows list */
		};
	};

	struct hlist_node l2_subflow_node; /* PPE L2 subflow entry */
	u32 hash;

	struct airoha_foe_stats64 stats;
	enum airoha_flow_entry_type type;

	struct rhash_head node;
	unsigned long cookie;

	/* Must be last --ends in a flexible-array member. */
	struct airoha_foe_entry data;
};

struct airoha_wdma_info {
	u8 idx;
	u8 queue;
	u16 wcid;
	u8 bss;
};

/* RX queue to IRQ mapping: BIT(q) in IRQ(n) */
#define RX_IRQ0_BANK_PIN_MASK			0x839f
#define RX_IRQ1_BANK_PIN_MASK			0x7fe00000
#define RX_IRQ2_BANK_PIN_MASK			0x20
#define RX_IRQ3_BANK_PIN_MASK			0x40
#define RX_IRQ_BANK_PIN_MASK(_n)		\
	(((_n) == 3) ? RX_IRQ3_BANK_PIN_MASK :	\
	 ((_n) == 2) ? RX_IRQ2_BANK_PIN_MASK :	\
	 ((_n) == 1) ? RX_IRQ1_BANK_PIN_MASK :	\
	 RX_IRQ0_BANK_PIN_MASK)

struct airoha_irq_bank {
	struct airoha_qdma *qdma;

	/* protect concurrent irqmask accesses */
	spinlock_t irq_lock;
	u32 irqmask[QDMA_INT_REG_MAX];
	int irq;
};

#define EN751221_SLM_BASE			0x1fa60000
#define EN751221_SLM_REG_SIZE			SZ_256
#define EN751221_SLM_HWF_VIRT_BASE		0x1b800000
#define EN751221_SLM_PHYS_SIZE			SZ_4M
#define EN751221_SLM_MAX_DRAM_SIZE		SZ_256M
#define EN751221_SLM_SECTOR_SIZE		SZ_256
#define EN751221_SLM_GLO_CFG_BYPASS		BIT(0)
#define EN751221_SLM_EN_ENABLE			BIT(0)
#define EN751221_SLM_EN_ACTIVE			BIT(1)

#define EN7516_QDMA_INT_STATUS1_OFFSET		0x700
#define EN7516_QDMA_INT_STATUS2_OFFSET		0x704
#define EN7516_QDMA_INT_ENABLE_BASE		0x704
#define EN7516_QDMA_INT_ENABLE_STRIDE		0x0c

struct en751221_slm_regs {
	u32 glo_cfg;
	u32 enable;
	u32 reserved_08[2];
	u32 virt_base;
	u32 virt_size;
	u32 phys_base;
	u32 phys_size;
	u32 free_min_cnt;
	u32 free_cur_cnt;
	u32 free_threshold;
	u32 reserved_2c;
	u32 api_cmd;
	u32 api_base;
	u32 api_rd_addr;
	u32 reserved_3c;
	u32 int_status;
	u32 int_mask;
	u32 bus_rd_null_addr;
	u32 reserved_4c;
	u32 drop_cmd;
	u32 drop_addr;
	u32 drop_cnt;
};

/**
 * struct airoha_qdma_slm - EN751221-family SLM state
 * @regs: SLM control-register window
 * @buf: CPU address of the backing memory
 * @dma_addr: DMA address of @buf
 * @buf_size: size of the backing memory
 * @enabled: SLM translation is active
 *
 * SLM only exists on the MIPS EcoNet QDMA generations.  Keep it attached to
 * the common QDMA object rather than exposing a second QDMA implementation.
 */
struct airoha_qdma_slm {
	struct en751221_slm_regs __iomem *regs;
	void *buf;
	dma_addr_t dma_addr;
	size_t buf_size;
	bool enabled;
};

struct airoha_qdma_mips;

struct airoha_qdma {
	struct airoha_eth *eth;
	void __iomem *regs;
	u8 id;
	u8 num_channels;

	int users;

	struct airoha_irq_bank *irq_banks;

	struct airoha_tx_irq_queue q_tx_irq[AIROHA_NUM_TX_IRQ];

	struct airoha_queue *q_tx;
	struct airoha_queue *q_rx;

	DECLARE_BITMAP(qos_channel_map, AIROHA_NUM_QOS_CHANNELS);

	/* Private queue/ring state for the MIPS QDMA layout. */
	struct airoha_qdma_mips *econet;
};

enum airoha_priv_flags {
	AIROHA_PRIV_F_WAN = BIT(0),
	AIROHA_PRIV_F_QOS = BIT(1),
	AIROHA_PRIV_F_XPON_MANAGED = BIT(2),
};

#define AIROHA_XPON_OAM_RX_F_MIC_PRESENT	BIT(0)
#define AIROHA_XPON_OAM_RX_F_MIC_VALID	BIT(1)
#define AIROHA_XPON_OAM_RX_F_CRC_ERROR	BIT(2)

struct airoha_xpon_oam_handler {
	bool (*rx)(void *priv, struct sk_buff *skb, u16 gem_port_id,
		   u32 flags);
	void *priv;
};

/**
 * struct airoha_xpon_link_ops - xPON netdev lifecycle notifications
 * @start: notify the provider that the data netdev has been opened
 * @stop: notify the provider that the data netdev is being closed
 *
 * These callbacks control netdev-facing state only. Protocol control planes
 * such as OMCI acquire the shared GDM2/QDMA transport separately.
 */
struct airoha_xpon_link_ops {
	int (*start)(void *priv);
	void (*stop)(void *priv);
	/* EcoNet routes the GPON/EPON MAC interrupt through
	 * QDMA1 instead of exposing a dedicated platform IRQ.  Providers that
	 * need that path publish the hard-IRQ callback here.
	 */
	void (*mac_irq)(void *priv);
};

/**
 * struct airoha_xpon_link_state - xPON link state exposed by GDM2
 * @mode: active xPON protocol
 * @valid: true after the provider has published its first state
 * @link: true when the protocol is operational
 * @speed: ethtool-compatible nominal downstream speed
 * @duplex: ethtool duplex mode
 * @autoneg: ethtool autonegotiation mode
 * @port: ethtool port type
 * @rx_line_rate_bps: exact downstream line rate in bits per second
 * @tx_line_rate_bps: exact upstream line rate in bits per second
 *
 * GPON has asymmetric line rates, while ethtool exposes a single speed.
 * Keep the exact rates in the provider snapshot and use @speed for the
 * conventional netdev representation.
 */
struct airoha_xpon_link_state {
	enum airoha_xpon_mode mode;
	bool valid;
	bool link;
	u32 speed;
	u8 duplex;
	u8 autoneg;
	u8 port;
	u64 rx_line_rate_bps;
	u64 tx_line_rate_bps;
};

struct airoha_xpon_service_cfg {
	u32 cookie;
	u16 gem_port_id;
	u16 vlan_id;
	u8 tcont;
	u8 queue;
	u8 pcp;
	bool vlan_valid;
	bool pcp_valid;
	bool default_service;
	bool valid;
};

struct airoha_xpon_tx_info {
	u16 gem_port_id;
	u8 tcont;
	u8 queue;
	bool oam;
};

struct airoha_gdm_dev {
	/* Must stay first: shared GDM/phylink and PPE port metadata. */
	struct airoha_gdm_common common;

	struct airoha_qdma __rcu *qdma;
	struct airoha_gdm_port *port;
	struct airoha_eth *eth;

	/* MIPS GDM registers share the same logical GDM device object. */
	union {
		void __iomem *regs;
		struct gdm __iomem *econet_regs;
	};
	spinlock_t reg_lock;
	bool g2_stats;
	u8 fport;

	DECLARE_BITMAP(qos_sq_bmap, AIROHA_NUM_QOS_CHANNELS);
	/* qos stats counters */
	u64 cpu_tx_packets;
	u64 fwd_tx_packets;

	u32 flags;
	int nbq;

	struct airoha_hw_stats stats;

	struct airoha_xpon_oam_handler __rcu *xpon_oam;
	atomic64_t xpon_oam_rx_packets;
	atomic64_t xpon_oam_rx_bytes;
	atomic64_t xpon_oam_rx_delivered;
	atomic64_t xpon_oam_rx_dropped;
	atomic64_t xpon_oam_rx_no_handler;

	/* Serializes xPON registration, netdev notifications and control-plane IO. */
	struct mutex xpon_lock;
	const struct airoha_xpon_link_ops *xpon_ops;
	void *xpon_priv;
	enum airoha_xpon_mode xpon_mode;
	bool xpon_started;
	bool xpon_control_started;
	/* Protects the xPON state consumed by netdev and ethtool callbacks. */
	spinlock_t xpon_state_lock;
	struct airoha_xpon_link_state xpon_link;

	/* Protects GPON GEM/T-CONT service classification. */
	spinlock_t xpon_service_lock;
	struct airoha_xpon_service_cfg
		xpon_services[AIROHA_XPON_MAX_SERVICES];
	u32 xpon_service_cookies[AIROHA_XPON_MAX_SERVICES]
				 [AIROHA_XPON_SVC_MAX_COOKIES];
	u8 xpon_service_ncookies[AIROHA_XPON_MAX_SERVICES];

};

struct airoha_gdm_port {
	struct airoha_gdm_dev *devs[AIROHA_MAX_NUM_GDM_DEVS];
	int id;
	int users;

	/* protect concurrent hw_stats and frag register accesses */
	spinlock_t lock;

	struct metadata_dst *dsa_meta[AIROHA_MAX_DSA_PORTS];
};

#define AIROHA_RXD4_PPE_CPU_REASON			GENMASK(20, 16)
#define AIROHA_RXD4_FOE_ENTRY				GENMASK(15, 0)
#define AN7581_AIROHA_RXD4_FOE_ENTRY_INVALID		0xffff
#define EN7523_AIROHA_RXD4_FOE_ENTRY_INVALID		0x7fff

#define AIROHA_PPE_CPU_REASON_FOE_UNHIT			0x0d
#define AIROHA_PPE_CPU_REASON_HIT_UNBIND		0x0e
#define AIROHA_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED	0x0f

struct airoha_ppe_host_ops {
	int (*get_fe_port)(struct airoha_gdm_dev *dev);
	bool (*is_valid_gdm_dev)(struct airoha_eth *eth,
				 struct airoha_gdm_dev *dev);
	int (*xpon_get_tx_info)(struct net_device *netdev, bool vlan_valid,
				u16 vlan_id, bool pcp_valid, u8 pcp,
				struct airoha_xpon_tx_info *info);
	struct airoha_npu *(*npu_get)(struct airoha_eth *eth);
	void (*npu_put)(struct airoha_npu *npu);
	int (*npu_ppe_init)(struct airoha_npu *npu);
	int (*npu_ppe_deinit)(struct airoha_npu *npu);
	int (*npu_ppe_init_stats)(struct airoha_npu *npu, dma_addr_t addr,
				  u32 num_stats_entries);
	void (*npu_stats_read)(struct airoha_npu *npu, u32 index,
			       struct airoha_foe_stats *stats);
	void (*npu_stats_clear)(struct airoha_npu *npu, u32 index);
};

struct airoha_ppe_common {
	struct airoha_ppe_dev dev;
	struct airoha_eth *eth;

	void *foe;
	dma_addr_t foe_dma;

	struct dentry *debugfs_dir;
};

struct airoha_ppe {
	struct airoha_ppe_common common;

	struct rhashtable l2_flows;
	struct hlist_head pending_flows;

	struct hlist_head *foe_flow;
	u16 *foe_check_time;

	struct airoha_foe_stats *foe_stats;
	dma_addr_t foe_stats_dma;

	/* Set once CPU-direct PPE offload has been initialized on systems
	 * with no attached NPU, to avoid re-running setup (and re-requesting
	 * the airoha-npu module) on every flow.
	 */
	bool offload_setup_done;
};

/*
 * EcoNet PPE/FoE logical layout.
 *
 * Keep the mixed-width fields in the same lane order as the EN7512 SDK
 * _ipv4_hnapt structure.  The first 64 bytes are the IPv4 HNAPT payload;
 * struct econet_foe_entry remains 80 bytes because the hardware table uses
 * the largest FoE V1 union member as its stride.
 */
struct econet_foe_mac_info {
	u16 etype;
	u16 vlan1;
	u32 dest_mac_hi;
	u16 dest_mac_lo;
	u16 vlan2;
	u32 src_mac_hi;
	u16 src_mac_lo;
	u16 pppoe_id;
};

struct econet_ipv4_tuple {
	u32 src_ip;
	u32 dest_ip;
	union {
		struct {
			u16 src_port;
			u16 dest_port;
		};
		u32 ports;
	};
};

struct econet_foe_ipv4 {
	struct econet_ipv4_tuple orig;
	u32 ib2;
	struct econet_ipv4_tuple new;
	u32 reserved[2];
	u32 udf_tsid;
	struct econet_foe_mac_info l2;
};

/* EN7512 SDK _ipv6_5t_route layout. The hardware keeps the 5-tuple in
 * words 1..9 and PpeClearEntryInfo() clears the entry starting at byte 40.
 */
struct econet_foe_ipv6 {
	u32 src_ip[4];
	u32 dest_ip[4];
	union {
		struct {
			u16 src_port;
			u16 dest_port;
		};
		u32 ports;
	};
	u32 reserved[3];
	u32 udf_tsid;
	u32 ib2;
	struct econet_foe_mac_info l2;
};

struct econet_foe_entry {
	u32 ib1;
	union {
		struct econet_foe_ipv4 ipv4;
		struct econet_foe_ipv6 ipv6;
		u32 data[19];
	};
};


struct econet_flow_entry {
	struct list_head list;
	struct econet_foe_entry data;
	unsigned long cookie;
	u32 src_ip;
	u32 dest_ip;
	struct in6_addr src_ip6;
	struct in6_addr dest_ip6;
	u16 src_port;
	u16 dest_port;
	u16 hash;
	u16 addr_type;
};

struct econet_ppe {
	struct airoha_ppe_common common;

	/* Protects flows and FoE slot ownership in RX and TC paths. */
	spinlock_t lock;
	struct list_head flows;
	struct econet_flow_entry **foe_owner;
	struct list_head block_cb_list;
	bool armed;
};

enum airoha_ids {
	econet_en751221 = 0x751221,
	econet_en7528 = 0x7528,
	airoha_en7523 = 0x7523,
	airoha_en7581 = 0x7581,
	airoha_an7583 = 0x7583,
};

struct platform_device;

struct airoha_eth;

struct airoha_eth_xpon_ops {
	int (*set_mode)(struct net_device *netdev, enum airoha_xpon_mode mode);
	int (*set_datapath)(struct net_device *netdev, enum airoha_xpon_mode mode,
			    bool enable);
	int (*set_tcont_channel)(struct net_device *netdev, unsigned int channel,
				 bool enable);
	int (*register_link)(struct net_device *netdev, enum airoha_xpon_mode mode,
			     const struct airoha_xpon_link_ops *ops, void *priv);
	void (*unregister_link)(struct net_device *netdev,
			       const struct airoha_xpon_link_ops *ops, void *priv);
	void (*update_link)(struct net_device *netdev,
			    const struct airoha_xpon_link_state *state);
	int (*control_start)(struct net_device *netdev);
	void (*control_stop)(struct net_device *netdev);
	void (*dump_oam_rx_state)(struct net_device *netdev);
	int (*register_oam)(struct net_device *netdev,
			    struct airoha_xpon_oam_handler *handler);
	void (*unregister_oam)(struct net_device *netdev,
			       struct airoha_xpon_oam_handler *handler);
	int (*xmit_oam)(struct net_device *netdev, struct sk_buff *skb,
			u16 gem_port_id);
	int (*add_service)(struct net_device *netdev,
			   const struct airoha_xpon_service_cfg *cfg);
	int (*get_tx_info)(struct net_device *netdev, bool vlan_valid, u16 vlan_id,
			   bool pcp_valid, u8 pcp,
			   struct airoha_xpon_tx_info *info);
	bool (*del_service)(struct net_device *netdev, u32 cookie, u16 *gem_port_id);
	bool (*has_gem_service)(struct net_device *netdev, u16 gem_port_id);
	void (*flush_services)(struct net_device *netdev);
};

struct airoha_eth_soc_data {
	enum airoha_ids version;
	const struct airoha_eth_xpon_ops *xpon_ops;
	const char * const *xsi_rsts_names;
	int num_xsi_rsts;
	int num_ppe;
	int tx_ring, rx_ring;
	int irq_banks;
	int max_gdm_ports;
	u32 pse_fq_cfg;
	u32 ppe_stats_entries;
	u32 ppe_sram_entries;
	u32 ppe_dram_entries;
	struct {
		int (*get_sport)(struct airoha_gdm_port *port, int nbq);
		u32 (*get_vip_port)(struct airoha_gdm_port *port, int nbq);
		int (*get_dev_from_sport)(struct airoha_eth *eth, u32 sport,
					  u16 *port, u16 *dev);
	} ops;
};

struct airoha_eth {
	struct device *dev;

	const struct airoha_eth_soc_data *soc;

	unsigned long state;
	void __iomem *fe_regs;
	void __iomem *gdmp_regs;

	struct airoha_npu __rcu *npu;

	struct airoha_ppe *ppe;
	struct airoha_ppe_dev *ppe_dev;
	const struct airoha_ppe_host_ops *ppe_host_ops;
	struct rhashtable flow_table;

	struct reset_control_bulk_data rsts[AIROHA_MAX_NUM_RSTS];
	struct reset_control_bulk_data *xsi_rsts;

	struct net_device *napi_dev;
	int fe_irq;

	struct airoha_qdma qdma[AIROHA_MAX_NUM_QDMA];
	struct airoha_gdm_port **ports;

};

#define airoha_fe_rr(eth, offset)				\
	airoha_rr((eth)->fe_regs, (offset))
#define airoha_fe_wr(eth, offset, val)				\
	airoha_wr((eth)->fe_regs, (offset), (val))
#define airoha_fe_rmw(eth, offset, mask, val)			\
	airoha_rmw((eth)->fe_regs, (offset), (mask), (val))
#define airoha_fe_set(eth, offset, val)				\
	airoha_rmw((eth)->fe_regs, (offset), 0, (val))
#define airoha_fe_clear(eth, offset, val)			\
	airoha_rmw((eth)->fe_regs, (offset), (val), 0)
#define airoha_fe_get(eth, offset, mask)			\
	FIELD_GET((mask), airoha_fe_rr((eth), (offset)))

#define airoha_qdma_rr(qdma, offset)				\
	airoha_rr((qdma)->regs, (offset))
#define airoha_qdma_wr(qdma, offset, val)			\
	airoha_wr((qdma)->regs, (offset), (val))
#define airoha_qdma_rmw(qdma, offset, mask, val)		\
	airoha_rmw((qdma)->regs, (offset), (mask), (val))
#define airoha_qdma_set(qdma, offset, val)			\
	airoha_rmw((qdma)->regs, (offset), 0, (val))
#define airoha_qdma_clear(qdma, offset, val)			\
	airoha_rmw((qdma)->regs, (offset), (val), 0)
#define airoha_qdma_get(qdma, offset, mask)			\
	FIELD_GET((mask), airoha_qdma_rr((qdma), (offset)))

void airoha_qdma_setup(struct airoha_qdma *qdma, struct airoha_eth *eth,
		       void __iomem *regs, u8 id, u8 num_channels);
int airoha_qdma_init(struct platform_device *pdev, struct airoha_eth *eth,
		     struct airoha_qdma *qdma);
void airoha_qdma_cleanup(struct airoha_qdma *qdma);
void airoha_qdma_cleanup_tx_queue(struct airoha_queue *q);
void airoha_qdma_unmap_tx_entry(struct airoha_eth *eth,
				struct airoha_queue_entry *e);
void airoha_qdma_start_napi(struct airoha_qdma *qdma);
void airoha_qdma_stop_napi(struct airoha_qdma *qdma);
void airoha_qdma_start(struct airoha_qdma *qdma);
void airoha_qdma_stop(struct airoha_qdma *qdma);

static inline u16 airoha_qdma_get_txq(struct airoha_qdma *qdma, u16 qid)
{
	return qid % qdma->eth->soc->tx_ring;
}

static inline bool airoha_is_lan_gdm_dev(struct airoha_gdm_dev *dev)
{
	return !(dev->flags & AIROHA_PRIV_F_WAN);
}

#define airoha_is(eth, ...) ({ \
	const enum airoha_ids _ids[] = {__VA_ARGS__, 0}; \
	bool _found = false; \
	for (int _i = 0; _ids[_i] != 0; _i++) { \
		if (_ids[_i] == (eth)->soc->version) { \
			_found = true; \
			break; \
		} \
	} \
	_found; \
})

static inline bool airoha_is_econet(struct airoha_eth *eth)
{
	return airoha_is(eth, econet_en751221, econet_en7528);
}

static inline bool airoha_qdma_is_lro_queue(struct airoha_queue *q)
{
	struct airoha_qdma *qdma = q->qdma;
	int qid = q - &qdma->q_rx[0];
	
	switch (qdma->eth->soc->version) {
	case airoha_en7523:
		/* EN7523 11-14 */
		BUILD_BUG_ON(hweight32(EN7523_AIROHA_RXQ_LRO_EN_MASK) >
			     EN7523_AIROHA_MAX_NUM_LRO_QUEUES);

		return !!(EN7523_AIROHA_RXQ_LRO_EN_MASK & BIT(qid));
	case airoha_en7581:
	case airoha_an7583:
		/* EN7581 SoC supports at most 8 LRO rx queues (24-31) */
		BUILD_BUG_ON(hweight32(AIROHA_RXQ_LRO_EN_MASK) >
			     AIROHA_MAX_NUM_LRO_QUEUES);
	
		return !!(AIROHA_RXQ_LRO_EN_MASK & BIT(qid));
	default:
		return false;
	}
}

extern const struct airoha_eth_soc_data econet_en751221_soc_data;
extern const struct airoha_eth_soc_data econet_en7528_soc_data;
extern const struct airoha_eth_soc_data airoha_en7523_soc_data;
extern const struct airoha_eth_soc_data airoha_en7581_soc_data;
extern const struct airoha_eth_soc_data airoha_an7583_soc_data;

struct net_device *airoha_eth_get_xpon_netdev(void);
int airoha_eth_register_xpon(struct net_device *netdev,
			     enum airoha_xpon_mode mode,
			     const struct airoha_xpon_link_ops *ops,
			     void *priv);
void airoha_eth_unregister_xpon(struct net_device *netdev,
				const struct airoha_xpon_link_ops *ops,
				void *priv);
void airoha_eth_xpon_update_link(struct net_device *netdev,
				 const struct airoha_xpon_link_state *state);
int airoha_eth_xpon_control_start(struct net_device *netdev);
void airoha_eth_xpon_control_stop(struct net_device *netdev);
void airoha_eth_xpon_dump_oam_rx_state(struct net_device *netdev);
int airoha_eth_register_xpon_oam(struct net_device *netdev,
				 struct airoha_xpon_oam_handler *handler);
void airoha_eth_unregister_xpon_oam(struct net_device *netdev,
				    struct airoha_xpon_oam_handler *handler);
int airoha_eth_xmit_xpon_oam(struct net_device *netdev, struct sk_buff *skb,
			     u16 gem_port_id);
int airoha_eth_xpon_add_service(struct net_device *netdev,
				const struct airoha_xpon_service_cfg *cfg);
int airoha_eth_xpon_get_tx_info(struct net_device *netdev, bool vlan_valid,
				u16 vlan_id, bool pcp_valid, u8 pcp,
				struct airoha_xpon_tx_info *info);
bool airoha_eth_xpon_del_service(struct net_device *netdev, u32 cookie,
				  u16 *gem_port_id);
bool airoha_eth_xpon_has_gem_service(struct net_device *netdev,
				     u16 gem_port_id);
void airoha_eth_xpon_flush_services(struct net_device *netdev);
int airoha_eth_set_xpon_mode(struct net_device *netdev,
			      enum airoha_xpon_mode mode);
int airoha_eth_set_xpon_datapath(struct net_device *netdev,
				  enum airoha_xpon_mode mode, bool enable);
int airoha_eth_set_xpon_tcont_channel(struct net_device *netdev,
				      unsigned int channel, bool enable);
int airoha_get_fe_port(struct airoha_gdm_dev *dev);
bool airoha_is_valid_gdm_dev(struct airoha_eth *eth,
			     struct airoha_gdm_dev *dev);

extern struct mutex flow_offload_mutex;

static inline struct airoha_qdma *
airoha_qdma_deref(struct airoha_gdm_dev *dev)
{
	return rcu_dereference_protected(dev->qdma,
					 lockdep_rtnl_is_held() ||
					 lockdep_is_held(&flow_offload_mutex));
}

void airoha_ppe_set_mtu(struct airoha_gdm_dev *dev);
void airoha_ppe_set_cpu_port(struct airoha_gdm_dev *dev, u8 ppe_id, u8 fport);
bool airoha_ppe_is_enabled(struct airoha_eth *eth, int index);
void airoha_ppe_check_skb(struct airoha_ppe_dev *dev, struct sk_buff *skb,
			  u16 hash, bool rx_wlan);
int airoha_ppe_setup_tc_block_cb(struct airoha_ppe_dev *dev, void *type_data);
int airoha_ppe_init(struct airoha_eth *eth);
void airoha_ppe_deinit(struct airoha_eth *eth);
void airoha_ppe_init_upd_mem(struct airoha_gdm_dev *dev, const u8 *addr);
u32 airoha_ppe_get_total_num_entries(struct airoha_ppe *ppe);
struct airoha_foe_entry *airoha_ppe_foe_get_entry(struct airoha_ppe *ppe,
						  u32 hash);
void airoha_ppe_foe_entry_get_stats(struct airoha_ppe *ppe, u32 hash,
				    struct airoha_foe_stats64 *stats);
void econet_ppe_read_entry(struct econet_ppe *ppe, u16 hash,
			   struct econet_foe_entry *entry);
void econet_ppe_decode_entry(const struct econet_foe_entry *raw,
			     struct econet_foe_entry *entry);

#if IS_ENABLED(CONFIG_NET_AIROHA_PPE_DEBUGFS)
int airoha_ppe_debugfs_init(struct airoha_ppe_common *ppe);
#else
static inline int airoha_ppe_debugfs_init(struct airoha_ppe_common *ppe)
{
	return 0;
}
#endif


/* EcoNet/EN751x descriptor and register definitions. */
struct airoha_eth;
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
/* EN751221 xPON TX message word 0 overlays the Ethernet special-tag field. */
#define ETX_XPON_GEM_MASK				GENMASK(23, 12)
#define ETX_XPON_DEI					BIT(24)
#define ETX_XPON_TSE					BIT(25)
#define ETX_XPON_TSID_MASK				GENMASK(30, 26)
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

static inline u16 get_etx_xpon_gem(struct etx *x)
{
	return FIELD_GET(ETX_XPON_GEM_MASK, x->bitfield_0);
}

static inline void set_etx_xpon_gem(struct etx *x, u16 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_XPON_GEM_MASK, v);
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

/* EN751221 xPON RX message word 0.  The remaining words overlap the
 * regular Ethernet RX metadata and are deliberately kept in qdma_desc_erx.
 */
#define ERX_XPON_CRC_ERROR				BIT(0)
#define ERX_XPON_RUNT					BIT(1)
#define ERX_XPON_LONG					BIT(2)
#define ERX_XPON_CHANNEL_MASK				GENMASK(10, 3)
#define ERX_XPON_OAM					BIT(11)
#define ERX_XPON_GEM_MASK				GENMASK(23, 12)

#define ERX_UNKNOWN1_MASK				GENMASK(31, 29)
#define ERX_IP6						BIT(28)
#define ERX_IP4						BIT(27)
#define ERX_IP4F					BIT(26)
#define ERX_TACK					BIT(25)
#define ERX_L2VLD					BIT(24)
#define ERX_L4F						BIT(23)
#define ERX_SPORT_MASK					GENMASK(22, 19)
#define EN7528_ERX_SPORT_MASK				GENMASK(23, 19)
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

/* EcoNet MIPS QDMA layout. The Ethernet block still exposes the same
 * two QDMA engines and uses AIROHA_MAX_PACKET_SIZE for page-pool sizing.
 */
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
/* EN751221 has one IRQ bank and one status/enable register. EN7516/EN7527
 * expose four IRQ banks, each selecting from three shared status registers.
 */
#define ECONET_MAX_QDMA_IRQS	4
#define ECONET_QDMA_IRQ_REGS	3

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

/* Called in softirq context */
int econet_rx_before_recv(struct airoha_eth *eth, struct sk_buff *skb,
			u8 sport);

struct airoha_qdma_mips_cfg {
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

bool econet_rx_xpon_oam(struct airoha_eth *eth, u8 qdma_id,
				 struct sk_buff *skb, union desc_msg *msg);
void econet_xpon_irq(struct airoha_eth *eth, u8 qdma_id,
			      enum airoha_xpon_mode mode);


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

#endif /* AIROHA_ETH_H */
