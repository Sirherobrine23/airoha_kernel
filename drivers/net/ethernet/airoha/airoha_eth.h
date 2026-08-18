/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#ifndef AIROHA_ETH_H
#define AIROHA_ETH_H

#include "airoha_common.h"

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/reset.h>
#include <linux/soc/airoha/airoha_offload.h>
#include <net/dsa.h>

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

struct airoha_qdma_common {
	struct airoha_eth *eth;
	void __iomem *regs;
	u8 id;
	u8 num_channels;
};

struct airoha_qdma {
	struct airoha_qdma_common common;

	int users;

	struct airoha_irq_bank *irq_banks;

	struct airoha_tx_irq_queue q_tx_irq[AIROHA_NUM_TX_IRQ];

	struct airoha_queue *q_tx;
	struct airoha_queue *q_rx;

	DECLARE_BITMAP(qos_channel_map, AIROHA_NUM_QOS_CHANNELS);
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
	/* Generation-1 EcoNet routes the GPON/EPON MAC interrupt through
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
 * EcoNet generation-1 PPE/FoE logical layout.
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

struct econet_foe_entry {
	u32 ib1;
	union {
		struct econet_foe_ipv4 ipv4;
		u32 data[19];
	};
};

static_assert(sizeof(struct econet_foe_entry) == 80);
static_assert(offsetof(struct econet_foe_entry, ipv4.orig.src_port) == 12);
static_assert(offsetof(struct econet_foe_entry, ipv4.orig.dest_port) == 14);
static_assert(offsetof(struct econet_foe_entry, ipv4.ib2) == 16);
static_assert(offsetof(struct econet_foe_entry, ipv4.new.src_port) == 28);
static_assert(offsetof(struct econet_foe_entry, ipv4.new.dest_port) == 30);
static_assert(offsetof(struct econet_foe_entry, ipv4.udf_tsid) == 40);
static_assert(offsetof(struct econet_foe_entry, ipv4.l2.etype) == 44);
static_assert(offsetof(struct econet_foe_entry, ipv4.l2.vlan1) == 46);
static_assert(offsetof(struct econet_foe_entry, ipv4.l2.dest_mac_lo) == 52);
static_assert(offsetof(struct econet_foe_entry, ipv4.l2.vlan2) == 54);
static_assert(offsetof(struct econet_foe_entry, ipv4.l2.src_mac_lo) == 60);
static_assert(offsetof(struct econet_foe_entry, ipv4.l2.pppoe_id) == 62);

struct econet_flow_entry {
	struct list_head list;
	struct econet_foe_entry data;
	unsigned long cookie;
	u32 src_ip;
	u32 dest_ip;
	u16 src_port;
	u16 dest_port;
	u16 hash;
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

struct airoha_eth_ops {
	int (*probe)(struct platform_device *pdev, struct airoha_eth *eth);
	void (*remove)(struct platform_device *pdev, struct airoha_eth *eth);
};

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
	const struct airoha_eth_ops *eth_ops;
	const struct airoha_eth_xpon_ops *xpon_ops;
	const char * const *xsi_rsts_names;
	int num_xsi_rsts;
	int num_ppe;
	int tx_ring, rx_ring;
	int irq_banks;
	int max_gdm_ports;
	u32 ppe_stats_entries;
	u32 ppe_sram_entries;
	u32 ppe_dram_entries;
	struct {
		int (*get_sport)(struct airoha_gdm_port *port, int nbq);
		u32 (*get_vip_port)(struct airoha_gdm_port *port, int nbq);
		int (*get_dev_from_sport)(struct airoha_qdma_desc *desc,
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

	/* Common QDMA handles; generation-specific objects own the storage. */
	struct airoha_qdma_common *qdma_common[AIROHA_MAX_NUM_QDMA];
	struct airoha_qdma qdma[AIROHA_MAX_NUM_QDMA];
	struct airoha_gdm_port **ports;

	/* Generation-private host state. */
	void *priv;
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
	airoha_rr((qdma)->common.regs, (offset))
#define airoha_qdma_wr(qdma, offset, val)			\
	airoha_wr((qdma)->common.regs, (offset), (val))
#define airoha_qdma_rmw(qdma, offset, mask, val)		\
	airoha_rmw((qdma)->common.regs, (offset), (mask), (val))
#define airoha_qdma_set(qdma, offset, val)			\
	airoha_rmw((qdma)->common.regs, (offset), 0, (val))
#define airoha_qdma_clear(qdma, offset, val)			\
	airoha_rmw((qdma)->common.regs, (offset), (val), 0)
#define airoha_qdma_get(qdma, offset, mask)			\
	FIELD_GET((mask), airoha_qdma_rr((qdma), (offset)))

void airoha_qdma_common_init(struct airoha_qdma_common *qdma,
			     struct airoha_eth *eth, void __iomem *regs, u8 id);
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
	return qid % qdma->common.eth->soc->tx_ring;
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

static inline bool airoha_is_gen1(struct airoha_eth *eth)
{
	return airoha_is(eth, econet_en751221, econet_en7528);
}

static inline bool airoha_is_gen2(struct airoha_eth *eth)
{
	return airoha_is(eth, airoha_en7523, airoha_en7581, airoha_an7583);
}

static inline bool airoha_qdma_is_lro_queue(struct airoha_queue *q)
{
	struct airoha_qdma *qdma = q->qdma;
	int qid = q - &qdma->q_rx[0];
	
	switch (qdma->common.eth->soc->version) {
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

#endif /* AIROHA_ETH_H */
