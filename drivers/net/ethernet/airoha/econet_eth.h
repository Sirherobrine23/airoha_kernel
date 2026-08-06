/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ECONET_ETH_H
#define ECONET_ETH_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>

#include "airoha_common.h"

#include "asm-generic/int-ll64.h"
#include "econet_qdma_desc.h"
#include "econet_qdma_regs.h"
#include "econet_gdm_regs.h"

struct en75_debug;

struct en75_soc_data {
	bool dscp_byte_swap;
};

/* Number of QDMA engines in this ethernet device. */
#define EN75_NUM_QDMA		2

/* Number of GDM ports. */
#define EN75_NUM_GDM_PORTS	2

/* The hardware MTU limit is 16128 (see: rx_len_threshold.oversize_len).
 * This limit is set to save memory in the page pool. In the future we
 * may restart the QDMA engines on MTU change which crosses a page size
 * boundary. */
#define EN75_MAX_PACKET_SIZE	2048

/* Internally these are properties of the QDMA engine, but they are important
 * to the GDM port driver because they define how QoS can be done. */
#define EN75_NUM_QUEUES		8
#define EN75_NUM_CHANNELS	32

/* Number of queues reported to the kernel.
 * Currently every chan/queue is made available. */
#define EN75_NUM_SOFT_QUEUES	(EN75_NUM_CHANNELS * EN75_NUM_QUEUES)

/* A chain is 1 TX ring + 1 RX ring, each QDMA has 2 chains. */
#define QDMA_NUM_CHAINS		2
/* Each QDMA has one queue of TX-complete notifications. */
#define QDMA_NUM_TX_DONE	1
/* Each QDMA has only 1 IRQ and 1 mask + 1 status register (on EN751221). */
#define QDMA_NUM_IRQS		1
#define QDMA_REGS_PER_IRQ	1

union en75_irq_purpose {
	struct {
		enum en75_irq_purpose_type {
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

		enum en75_irq_purpose_source {
			IPSC_RX = 1,
			IPSC_TX,
			IPSC_DONE,
			IPSC_FWD,
			IPSC_UNSPEC,
		} source: 8;
	};
	u32 word;
};

static inline char *en75_irq_purpose_type_str(enum en75_irq_purpose_type t)
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

static inline char *en75_irq_purpose_source_str(enum en75_irq_purpose_source s)
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

/* Set by QDMA for each chain (RX+TX) */
struct en75_debug_qdma_chain_conf {
	struct desc				*rx_descs;
	int 					rx_count;
	struct desc 				*tx_descs;
	int 					tx_count;

	/* Must be EN75_NUM_QUEUES * EN75_NUM_QUEUES length */
	u32					*tx_per_ch_q;
};

/* Set by QDMA for each IRQ (actual interrupt number) */
struct en75_debug_qdma_irq_conf {
	union en75_irq_purpose 			desc[QDMA_REGS_PER_IRQ * 32];

	/* Must match length of desc. */
	u32 					*counters;
};

struct en75_debug_qdma_conf {
	struct qregs __iomem			*regs;
	struct fwdesc				*hwf_desc;
	struct en75_debug_qdma_chain_conf	chains[QDMA_NUM_CHAINS];
	struct en75_debug_qdma_irq_conf		irqs[QDMA_NUM_IRQS];
};

struct en75_debug_conf {
	struct en75_debug_qdma_conf		qdma[EN75_NUM_QDMA];
};

struct en75_debug *en75_debugfs_init(struct en75_debug_conf *config);
void en75_debugfs_exit(struct en75_debug *debug);

enum en75_fport {
	DPORT_CPU		= 0,
	DPORT_GDMA1		= 1,
	DPORT_GDMA2		= 2,
	DPORT_UNKNOWN_3		= 3,
	DPORT_PPE		= 4,
	DPORT_QDMA		= 5,
	DPORT_QDMA_HW		= 6,
	DPORT_DISCARD		= 7,
};

struct en75_eth {
	struct device *dev;
};

/* Called in softirq context */
int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb,
			enum etx_fport sport);

/* Called from the port driver to the main eth to notify the switch */
int en75_port_set_macaddr(struct en75_eth *eth, enum etx_fport portn,
			  const u8 *addr);

struct en75_qdma_cfg {
	int num_rx_descs[QDMA_NUM_CHAINS];
	int num_tx_descs[QDMA_NUM_CHAINS];
	int done_list_size[QDMA_NUM_TX_DONE];
	int num_fwd_descs;
	int fwd_max_packet_size;
	int fwd_low_threshold;
	const struct en75_soc_data *soc;
};

struct en75_qdma;

struct en75_qdma *en75_qdma_new(struct en75_eth *eth,
				void __iomem *qdma_regs,
				int id,
				int *irqs,
				int num_irqs,
				struct en75_qdma_cfg *cfg,
				struct en75_debug_qdma_conf *debug);

int en75_qdma_use(struct en75_qdma *qdma);
int en75_qdma_unuse(struct en75_qdma *qdma);
int en75_qdma_destroy(struct en75_qdma *qdma);
int en75_qdma_xmit(struct en75_qdma *qdma, struct sk_buff *skb,
		   union desc_msg *msg, int qid);


struct net_device *en75_alloc_gdm_port(struct en75_eth *eth,
				       struct device_node *np,
				       struct gdm __iomem *regs,
				       struct en75_qdma *qdma,
				       enum etx_fport fport,
				       bool has_g2_stats);

#define en75_rreg(reg) __extension__({ \
		BUILD_BUG_ON(sizeof(*(reg)) != sizeof(u32)); \
		union { typeof(*(reg)) v; u32 w; } __r = { \
			.w = airoha_rr((void __iomem *)(reg), 0), \
		}; \
		__r.v; \
	})

#define en75_wreg(val, reg) do { \
		BUILD_BUG_ON(sizeof(*(reg)) != sizeof(u32)); \
		BUILD_BUG_ON(!__same_type(*(reg), (val))); \
		union { typeof(*(reg)) v; u32 w; } __w = { .v = (val) }; \
		airoha_wr((void __iomem *)(reg), 0, __w.w); \
	} while (0)

#define en75_word(val) __extension__({ \
		union { typeof(val) v; u32 w; } __r = { .v = (val) }; \
		BUILD_BUG_ON(sizeof(__r) != sizeof(u32)); \
		__r.w; \
	})
#endif
