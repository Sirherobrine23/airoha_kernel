// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rhashtable.h>
#include <net/addrconf.h>
#include <net/dsfield.h>
#include <net/inet_hashtables.h>
#include <net/inet6_hashtables.h>
#include <net/ipv6.h>
#include <net/pkt_cls.h>
#include <net/route.h>
#include <uapi/linux/ppp_defs.h>

#include "airoha_regs.h"
#include "airoha_eth.h"

/* Serialize airoha_gdm_dev flags, QDMA pointer and PPE CPU port
 * configuration.
 */
DEFINE_MUTEX(flow_offload_mutex);
static DEFINE_SPINLOCK(ppe_lock);

static const struct rhashtable_params airoha_flow_table_params = {
	.head_offset = offsetof(struct airoha_flow_table_entry, node),
	.key_offset = offsetof(struct airoha_flow_table_entry, cookie),
	.key_len = sizeof(unsigned long),
	.automatic_shrinking = true,
};

static const struct rhashtable_params airoha_l2_flow_table_params = {
	.head_offset = offsetof(struct airoha_flow_table_entry, l2_node),
	.key_offset = offsetof(struct airoha_flow_table_entry, data.bridge),
	.key_len = 2 * ETH_ALEN,
	.automatic_shrinking = true,
};

static int airoha_ppe_get_num_stats_entries(struct airoha_ppe *ppe)
{
	if (!IS_ENABLED(CONFIG_NET_AIROHA_FLOW_STATS))
		return -EOPNOTSUPP;

	if (!ppe->eth->soc->ppe_stats_entries)
		return -EOPNOTSUPP;

	return ppe->eth->soc->ppe_stats_entries;
}

static int airoha_ppe_get_total_num_stats_entries(struct airoha_ppe *ppe)
{
	int num_stats = airoha_ppe_get_num_stats_entries(ppe);

	if (num_stats > 0) {
		struct airoha_eth *eth = ppe->eth;

		num_stats = num_stats * eth->soc->num_ppe;
	}

	return num_stats;
}

static u32 airoha_ppe_get_total_sram_num_entries(struct airoha_ppe *ppe)
{
	struct airoha_eth *eth = ppe->eth;

	return eth->soc->ppe_sram_entries * eth->soc->num_ppe;
}

static int airoha_ppe_hw_to_sw_idx(struct airoha_ppe *ppe, u32 hw_idx,
				   u32 *sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	u32 dram_num_entries = ppe->eth->soc->ppe_dram_entries;
	u32 ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);

	if (!airoha_is_7523(ppe->eth)) {
		if (hw_idx >= ppe_num_entries)
			return -ERANGE;

		*sw_idx = hw_idx;
		return 0;
	}

	if (hw_idx < sram_num_entries) {
		*sw_idx = hw_idx;
		return 0;
	}

	if (hw_idx >= dram_num_entries &&
	    hw_idx < dram_num_entries + dram_num_entries) {
		*sw_idx = sram_num_entries + hw_idx - dram_num_entries;
		return 0;
	}

	return -ERANGE;
}

static u32 airoha_ppe_sw_to_hw_idx(struct airoha_ppe *ppe, u32 sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	if (airoha_is_7523(ppe->eth) && sw_idx >= sram_num_entries)
		return ppe->eth->soc->ppe_dram_entries +
		       sw_idx - sram_num_entries;

	return sw_idx;
}

u32 airoha_ppe_get_total_num_entries(struct airoha_ppe *ppe)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	/* The AN7552 PPE reports DRAM FoE hashes in the 0x4000.. range and
	 * indexes directly from foe_dma + hw_idx * PPE_ENTRY_SIZE, so the
	 * allocation must cover 0x4000 + ppe_dram_entries.
	 */
	if (airoha_is_7552(ppe->eth))
		return 0x4000 + ppe->eth->soc->ppe_dram_entries;

	return sram_num_entries + ppe->eth->soc->ppe_dram_entries;
}

static void airoha_ppe_foe_decode_entry(struct airoha_eth *eth,
					struct airoha_foe_entry *entry)
{
	struct an7552_foe_bridge br;

	if (!airoha_is_7552(eth) ||
	    FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, entry->ib1) !=
	    PPE_PKT_TYPE_BRIDGE)
		return;

	br = entry->an7552_bridge;
	memset(&entry->bridge, 0, sizeof(entry->bridge));
	entry->bridge.dest_mac_hi = br.dest_mac_hi;
	entry->bridge.dest_mac_lo = FIELD_GET(AIROHA_FOE_L2_DMAC_LO,
					      br.vlan_dest_mac_lo);
	entry->bridge.src_mac_hi = br.src_mac_hi >> 16;
	entry->bridge.src_mac_lo = (br.src_mac_hi << 16) |
		FIELD_GET(AIROHA_FOE_L2_SMAC_LO, br.src_mac_lo);
	entry->bridge.data = br.data;
	entry->bridge.ib2 = br.ib2;
	entry->bridge.l2.common.vlan1 = FIELD_GET(AIROHA_FOE_L2_VLAN1,
						  br.vlan1_etype);
	entry->bridge.l2.common.etype = FIELD_GET(AIROHA_FOE_L2_ETYPE,
						  br.vlan1_etype);
	entry->bridge.l2.common.vlan2 = FIELD_GET(AIROHA_FOE_L2_VLAN2,
						  br.vlan2);
	entry->bridge.l2.common.dest_mac_hi = br.dest_mac_hi;
	entry->bridge.l2.common.dest_mac_lo = entry->bridge.dest_mac_lo;
	entry->bridge.l2.common.src_mac_hi = br.src_mac_hi;
	entry->bridge.l2.src_mac_lo = FIELD_GET(AIROHA_FOE_L2_SMAC_LO,
						br.src_mac_lo);
}

static void airoha_ppe_foe_encode_entry(struct airoha_eth *eth,
					struct airoha_foe_entry *dst,
					const struct airoha_foe_entry *entry,
					const struct airoha_foe_entry *old)
{
	const struct airoha_foe_bridge *br = &entry->bridge;
	struct an7552_foe_bridge *hw = &dst->an7552_bridge;

	*dst = *entry;
	if (!airoha_is_7552(eth) ||
	    FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, entry->ib1) !=
	    PPE_PKT_TYPE_BRIDGE)
		return;

	memset(hw, 0, sizeof(*hw));
	/* Preserve the hardware-learned VLAN/HSK ingress key and hop fields. */
	if (FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, old->ib1) ==
	    PPE_PKT_TYPE_BRIDGE)
		memcpy(hw, &old->an7552_bridge,
		       offsetof(struct an7552_foe_bridge, data));
	hw->dest_mac_hi = br->dest_mac_hi;
	hw->vlan_dest_mac_lo &= ~AIROHA_FOE_L2_DMAC_LO;
	hw->vlan_dest_mac_lo |= FIELD_PREP(AIROHA_FOE_L2_DMAC_LO,
					   br->dest_mac_lo);
	hw->src_mac_hi = (u32)br->src_mac_hi << 16 | br->src_mac_lo >> 16;
	hw->src_mac_lo = FIELD_PREP(AIROHA_FOE_L2_SMAC_LO, br->src_mac_lo);
	hw->data = br->data;
	hw->ib2 = br->ib2;
	hw->vlan1_etype = FIELD_PREP(AIROHA_FOE_L2_VLAN1, br->l2.common.vlan1) |
			  FIELD_PREP(AIROHA_FOE_L2_ETYPE, br->l2.common.etype);
	hw->vlan2 = FIELD_PREP(AIROHA_FOE_L2_VLAN2, br->l2.common.vlan2);
}

bool airoha_ppe_is_enabled(struct airoha_eth *eth, int index)
{
	if (index >= eth->soc->num_ppe)
		return false;

	return airoha_fe_rr(eth, REG_PPE_GLO_CFG(index)) & PPE_GLO_CFG_EN_MASK;
}

static u32 airoha_ppe_get_timestamp(struct airoha_ppe *ppe)
{
	return airoha_fe_get(ppe->eth, REG_FE_FOE_TS,
			     AIROHA_FOE_IB1_BIND_TIMESTAMP);
}

void airoha_ppe_set_cpu_port(struct airoha_gdm_dev *dev, u8 ppe_id, u8 fport)
{
	struct airoha_qdma *qdma = airoha_qdma_deref(dev);
	struct airoha_eth *eth = dev->eth;
	u8 qdma_id = qdma - &eth->qdma[0];
	u32 fe_cpu_port;

	fe_cpu_port = qdma_id ? FE_PSE_PORT_CDM2 : FE_PSE_PORT_CDM1;
	airoha_fe_rmw(eth, REG_PPE_DFT_CPORT(ppe_id, fport),
		      DFT_CPORT_MASK(fport),
		      __field_prep(DFT_CPORT_MASK(fport), fe_cpu_port));
}

void airoha_ppe_set_xmit_frame_size(struct airoha_gdm_dev *dev)
{
	struct airoha_gdm_port *port = dev->port;
	struct airoha_eth *eth = dev->eth;
	int i, ppe_id, index;
	u32 len = 0;

	for (i = 0; i < ARRAY_SIZE(port->devs); i++) {
		struct airoha_gdm_dev *d = port->devs[i];
		struct net_device *netdev;

		if (!d)
			continue;

		netdev = netdev_from_priv(d);
		if (netif_running(netdev))
			len = max_t(u32, len, netdev->mtu);
	}
	len += VLAN_ETH_HLEN;

	ppe_id = !airoha_is_lan_gdm_dev(dev) && airoha_ppe_is_enabled(eth, 1);
	index = port->id == AIROHA_GDM4_IDX ? 7 : port->id;
	airoha_fe_rmw(eth, REG_PPE_MTU(ppe_id, index),
		      FP_EGRESS_MTU_MASK(index),
		      __field_prep(FP_EGRESS_MTU_MASK(index), len));
}

static void airoha_ppe_hw_init(struct airoha_ppe *ppe)
{
	struct airoha_eth *eth = ppe->eth;
	u32 sram_ppe_num_data_entries = eth->soc->ppe_sram_entries;
	u32 sram_num_entries, sram_tb_size, dram_num_entries;
	int i, sram_num_stats_entries;

	if (airoha_is_7552(eth) &&
	    WARN_ON_ONCE(sram_ppe_num_data_entries != AN7552_PPE_DRAM_IDX_BASE))
		return;

	sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	sram_tb_size = sram_num_entries * sizeof(struct airoha_foe_entry);
	dram_num_entries =
		airoha_ppe_ram_num_entries_shift(eth->soc->ppe_dram_entries);

	sram_num_stats_entries = airoha_ppe_get_num_stats_entries(ppe);
	if (sram_num_stats_entries > 0)
		sram_ppe_num_data_entries -= sram_num_stats_entries;
	sram_ppe_num_data_entries =
		airoha_ppe_ram_num_entries_shift(sram_ppe_num_data_entries);

	for (i = 0; i < eth->soc->num_ppe; i++) {
		airoha_fe_wr(eth, REG_PPE_TB_BASE(i),
			     ppe->foe_dma + sram_tb_size);

		airoha_fe_rmw(eth, REG_PPE_BND_AGE0(i),
			      PPE_BIND_AGE0_DELTA_NON_L4 |
			      PPE_BIND_AGE0_DELTA_UDP,
			      FIELD_PREP(PPE_BIND_AGE0_DELTA_NON_L4, 60) |
			      FIELD_PREP(PPE_BIND_AGE0_DELTA_UDP, 60));
		airoha_fe_rmw(eth, REG_PPE_BND_AGE1(i),
			      PPE_BIND_AGE1_DELTA_TCP_FIN |
			      PPE_BIND_AGE1_DELTA_TCP,
			      FIELD_PREP(PPE_BIND_AGE1_DELTA_TCP_FIN, 1) |
			      FIELD_PREP(PPE_BIND_AGE1_DELTA_TCP, 60));

		if (airoha_is_7523(eth)) {
			airoha_fe_rmw(eth, REG_PPE_TB_CFG(i),
				      EN7523_PPE_SRAM_TABLE_EN_MASK |
				      EN7523_PPE_SRAM_HASH1_EN_MASK |
				      EN7523_PPE_DRAM_TABLE_EN_MASK |
				      EN7523_PPE_SRAM_HASH0_MODE_MASK |
				      EN7523_PPE_SRAM_HASH1_MODE_MASK |
				      EN7523_PPE_DRAM_HASH0_MODE_MASK |
				      PPE_SRAM_TB_NUM_ENTRY_MASK |
				      PPE_DRAM_TB_NUM_ENTRY_MASK |
				      PPE_TB_CFG_SEARCH_MISS_MASK |
				      PPE_TB_CFG_KEEPALIVE_MASK |
				      PPE_TB_CFG_AGE_TCP_FIN_MASK |
				      PPE_TB_CFG_AGE_UDP_MASK |
				      PPE_TB_CFG_AGE_TCP_MASK |
				      PPE_TB_CFG_AGE_UNBIND_MASK |
				      PPE_TB_CFG_AGE_NON_L4_MASK |
				      PPE_TB_CFG_AGE_PREBIND_MASK |
				      PPE_TB_ENTRY_SIZE_MASK,
				      EN7523_PPE_SRAM_TABLE_EN_MASK |
				      EN7523_PPE_SRAM_HASH1_EN_MASK |
				      EN7523_PPE_DRAM_TABLE_EN_MASK |
				      FIELD_PREP(EN7523_PPE_SRAM_HASH0_MODE_MASK, 3) |
				      FIELD_PREP(EN7523_PPE_SRAM_HASH1_MODE_MASK, 1) |
				      FIELD_PREP(EN7523_PPE_DRAM_HASH0_MODE_MASK, 3) |
				      FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
				      FIELD_PREP(PPE_TB_CFG_KEEPALIVE_MASK, 3) |
				      PPE_TB_ENTRY_SIZE_MASK |
				      PPE_TB_CFG_AGE_TCP_FIN_MASK |
				      PPE_TB_CFG_AGE_UDP_MASK |
				      PPE_TB_CFG_AGE_TCP_MASK |
				      PPE_TB_CFG_AGE_UNBIND_MASK |
				      PPE_TB_CFG_AGE_NON_L4_MASK |
				      PPE_TB_CFG_AGE_PREBIND_MASK |
				      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK,
						 sram_ppe_num_data_entries) |
				      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK,
						 dram_num_entries));

			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i), 0x00038743);
			airoha_fe_wr(eth, REG_PPE_PPE_FLOW_CFG(i), 0x06bbf7c0);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i), 0x000f000f);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i) + 0x4,
				     0x04291106);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i) + 0x8,
				     0x00003a01);
			airoha_fe_wr(eth, REG_PPE_TB_CFG(i),
				     0xef403fb4 | PPE_TB_ENTRY_SIZE_MASK);
			airoha_fe_wr(eth, REG_PPE_TB_HASH_CFG(i), 0x31003001);
			airoha_fe_wr(eth, REG_PPE_KEEPALIVE(i),
				     FIELD_PREP(PPE_KEEPALIVE_UDP_MASK, 1) |
				     FIELD_PREP(PPE_KEEPALIVE_TCP_MASK, 1) |
				     FIELD_PREP(PPE_KEEPALIVE_NTU_MASK, 1));
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x54,
				     0x00000021);
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x88,
				     0x001d077f);
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x8c,
				     0x0000001b);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x00,
				     0x07d407d0);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x04,
				     0x07dc07d8);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x08,
				     0x07e407e0);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x0c,
				     0x07f007e8);
		} else {
			airoha_fe_rmw(eth, REG_PPE_TB_HASH_CFG(i),
				      PPE_SRAM_TABLE_EN_MASK |
				      PPE_SRAM_HASH1_EN_MASK |
				      PPE_DRAM_TABLE_EN_MASK |
				      PPE_SRAM_HASH0_MODE_MASK |
				      PPE_SRAM_HASH1_MODE_MASK |
				      PPE_DRAM_HASH0_MODE_MASK |
				      PPE_DRAM_HASH1_MODE_MASK,
				      FIELD_PREP(PPE_SRAM_TABLE_EN_MASK, 1) |
				      FIELD_PREP(PPE_SRAM_HASH1_EN_MASK, 1) |
				      FIELD_PREP(PPE_DRAM_TABLE_EN_MASK,
						 airoha_is_7552(eth)) |
				      FIELD_PREP(PPE_SRAM_HASH0_MODE_MASK,
						 airoha_is_7552(eth)) |
				      FIELD_PREP(PPE_SRAM_HASH1_MODE_MASK, 1) |
				      FIELD_PREP(PPE_DRAM_HASH0_MODE_MASK,
						 airoha_is_7552(eth)) |
				      FIELD_PREP(PPE_DRAM_HASH1_MODE_MASK, 3));

			airoha_fe_rmw(eth, REG_PPE_TB_CFG(i),
				      PPE_TB_CFG_SEARCH_MISS_MASK |
				      PPE_SRAM_TB_NUM_ENTRY_MASK |
				      PPE_DRAM_TB_NUM_ENTRY_MASK |
				      PPE_TB_CFG_KEEPALIVE_MASK |
				      PPE_TB_ENTRY_SIZE_MASK,
				      FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
				      FIELD_PREP(PPE_TB_CFG_KEEPALIVE_MASK,
						 airoha_is_7552(eth) ? 3 : 0) |
				      FIELD_PREP(PPE_TB_ENTRY_SIZE_MASK, 0) |
				      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK,
						 sram_ppe_num_data_entries) |
				      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK,
						 dram_num_entries));

			if (airoha_is_7552(eth)) {
				airoha_fe_set(eth, REG_PPE_PPE_FLOW_CFG(i),
					      PPE_FLOW_CFG_IP4_NAPT_MASK |
					      PPE_FLOW_CFG_IP4_NAT_MASK |
					      PPE_FLOW_CFG_IP6_3T_ROUTE_MASK |
					      PPE_FLOW_CFG_IP6_5T_ROUTE_MASK |
					      PPE_FLOW_CFG_L2_BRIDGE_MASK);
				airoha_fe_clear(eth, REG_PPE_TB_CFG(i),
						PPE_TB_CFG_FIN_FAST_RP_MASK);
				airoha_fe_set(eth, REG_PPE_TB_CFG(i),
					      PPE_TB_CFG_AGE_TCP_FIN_MASK |
					      PPE_TB_CFG_AGE_UDP_MASK |
					      PPE_TB_CFG_AGE_TCP_MASK |
					      PPE_TB_CFG_AGE_UNBIND_MASK |
					      PPE_TB_CFG_AGE_NON_L4_MASK);
				airoha_fe_wr(eth, REG_PPE_BIND_LIMIT0(i),
					     FIELD_PREP(PPE_BIND_LIMIT0_HALF_MASK, 4000) |
					     FIELD_PREP(PPE_BIND_LIMIT0_QUARTER_MASK, 4000));
				airoha_fe_wr(eth, REG_PPE_BIND_LIMIT1(i),
					     FIELD_PREP(PPE_BIND_LIMIT1_NON_L4_MASK, 1) |
					     FIELD_PREP(PPE_BIND_LIMIT1_FULL_MASK, 8000));
			}
		}

		airoha_fe_rmw(eth, REG_PPE_BIND_RATE(i),
			      PPE_BIND_RATE_L2B_BIND_MASK |
			      PPE_BIND_RATE_BIND_MASK,
			      FIELD_PREP(PPE_BIND_RATE_L2B_BIND_MASK, 0x1e) |
			      FIELD_PREP(PPE_BIND_RATE_BIND_MASK, 0x1e));

		airoha_fe_rmw(eth, REG_PPE_UNBIND_AGE(i),
			      PPE_UNBIND_AGE_MIN_PACKETS_MASK |
			      PPE_UNBIND_AGE_DELTA_MASK,
			      FIELD_PREP(PPE_UNBIND_AGE_MIN_PACKETS_MASK, 1) |
			      FIELD_PREP(PPE_UNBIND_AGE_DELTA_MASK, 30));

		airoha_fe_wr(eth, REG_PPE_HASH_SEED(i), PPE_HASH_SEED);
		airoha_fe_clear(eth, REG_PPE_PPE_FLOW_CFG(i),
				PPE_FLOW_CFG_IP6_6RD_MASK);
		airoha_fe_set(eth, REG_PPE_GLO_CFG(i), PPE_GLO_CFG_EN_MASK);
	}

	for (i = 0; i < ARRAY_SIZE(eth->ports); i++) {
		struct airoha_gdm_port *port = eth->ports[i];
		int j;

		if (!port)
			continue;

		for (j = 0; j < ARRAY_SIZE(port->devs); j++) {
			struct airoha_gdm_dev *dev = port->devs[j];
			int ppe_id;
			u8 fport;

			if (!dev)
				continue;

			ppe_id = !airoha_is_lan_gdm_dev(dev) &&
				 airoha_ppe_is_enabled(eth, 1);
			fport = airoha_get_fe_port(dev);
			airoha_ppe_set_cpu_port(dev, ppe_id, fport);
			airoha_ppe_set_xmit_frame_size(dev);
		}
	}
}

static void airoha_ppe_flow_mangle_eth(const struct flow_action_entry *act, void *eth)
{
	void *dest = eth + act->mangle.offset;
	const void *src = &act->mangle.val;

	if (act->mangle.offset > 8)
		return;

	if (act->mangle.mask == 0xffff) {
		src += 2;
		dest += 2;
	}

	memcpy(dest, src, act->mangle.mask ? 2 : 4);
}

static int airoha_ppe_flow_mangle_ports(const struct flow_action_entry *act,
					struct airoha_flow_data *data)
{
	u32 val = be32_to_cpu((__force __be32)act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if ((__force __be32)act->mangle.mask == ~cpu_to_be32(0xffff))
			data->dst_port = cpu_to_be16(val);
		else
			data->src_port = cpu_to_be16(val >> 16);
		break;
	case 2:
		data->dst_port = cpu_to_be16(val);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int airoha_ppe_flow_mangle_ipv4(const struct flow_action_entry *act,
				       struct airoha_flow_data *data)
{
	__be32 *dest;

	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		dest = &data->v4.src_addr;
		break;
	case offsetof(struct iphdr, daddr):
		dest = &data->v4.dst_addr;
		break;
	default:
		return -EINVAL;
	}

	memcpy(dest, &act->mangle.val, sizeof(u32));

	return 0;
}

static int airoha_ppe_get_wdma_info(struct net_device *dev, const u8 *addr,
				    struct airoha_wdma_info *info)
{
	struct net_device_path_stack stack;
	struct net_device_path_ctx ctx = {
		.dev = dev,
	};
	struct net_device_path *path;
	int err;

	if (!dev)
		return -ENODEV;

	ether_addr_copy(ctx.daddr, addr);

	rcu_read_lock();
	err = dev_fill_forward_path(&ctx, &stack);
	rcu_read_unlock();
	if (err)
		return err;

	path = &stack.path[stack.num_paths - 1];
	if (path->type != DEV_PATH_MTK_WDMA) {
		err = -EINVAL;
		goto err_out;
	}

	info->idx = path->mtk_wdma.wdma_idx;
	info->bss = path->mtk_wdma.bss;
	info->wcid = path->mtk_wdma.wcid;
err_out:
	dev_fill_forward_path_release(&stack);

	return err;
}

static int airoha_get_dsa_port(struct net_device **dev)
{
#if IS_ENABLED(CONFIG_NET_DSA)
	struct dsa_port *dp = dsa_port_from_netdev(*dev);

	if (IS_ERR(dp))
		return -ENODEV;

	*dev = dsa_port_to_conduit(dp);
	return dp->index;
#else
	return -ENODEV;
#endif
}

static void airoha_ppe_foe_set_bridge_addrs(struct airoha_foe_bridge *br,
					    struct ethhdr *eh)
{
	br->dest_mac_hi = get_unaligned_be32(eh->h_dest);
	br->dest_mac_lo = get_unaligned_be16(eh->h_dest + 4);
	br->src_mac_hi = get_unaligned_be16(eh->h_source);
	br->src_mac_lo = get_unaligned_be32(eh->h_source + 2);
}

static int airoha_ppe_foe_entry_prepare(struct airoha_eth *eth,
					struct airoha_foe_entry *hwe,
					struct net_device *netdev, int type,
					struct airoha_flow_data *data,
					int l4proto, u8 priority)
{
	u32 qdata = FIELD_PREP(AIROHA_FOE_SHAPER_ID, 0x7f), ports_pad, val;
	int wlan_etype = -EINVAL, dsa_port = airoha_get_dsa_port(&netdev);
	struct airoha_foe_mac_info_common *l2;
	u8 smac_id = 0xf;

	memset(hwe, 0, sizeof(*hwe));

	val = FIELD_PREP(AIROHA_FOE_IB1_BIND_STATE, AIROHA_FOE_STATE_BIND) |
	      FIELD_PREP(AIROHA_FOE_IB1_BIND_PACKET_TYPE, type) |
	      FIELD_PREP(AIROHA_FOE_IB1_BIND_UDP, l4proto == IPPROTO_UDP) |
	      FIELD_PREP(AIROHA_FOE_IB1_BIND_VLAN_LAYER, data->vlan.num) |
	      FIELD_PREP(AIROHA_FOE_IB1_BIND_VPM, data->vlan.num) |
	      FIELD_PREP(AIROHA_FOE_IB1_BIND_PPPOE, data->pppoe.num) |
	      AIROHA_FOE_IB1_BIND_TTL;
	hwe->ib1 = val;

	val = FIELD_PREP(AIROHA_FOE_IB2_PORT_AG, 0x1f);
	if (netdev) {
		struct airoha_wdma_info info = {};

		if (!airoha_ppe_get_wdma_info(netdev, data->eth.h_dest,
					      &info)) {
			val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ, info.idx) |
			       FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT,
					  FE_PSE_PORT_CDM4);
			qdata |= FIELD_PREP(AIROHA_FOE_ACTDP, info.bss);
			wlan_etype = FIELD_PREP(AIROHA_FOE_MAC_WDMA_BAND,
						info.idx) |
				     FIELD_PREP(AIROHA_FOE_MAC_WDMA_WCID,
						info.wcid);
		} else {
			struct airoha_gdm_dev *dev = netdev_priv(netdev);
			struct airoha_gdm_port *port;
			u8 pse_port, channel;

			if (!airoha_is_valid_gdm_dev(eth, dev))
				return -EINVAL;

			port = dev->port;
			if (dsa_port >= 0 || airoha_is_lan_gdm_dev(dev))
				pse_port = port->id == 4 ? FE_PSE_PORT_GDM4
							 : port->id;
			else
				pse_port = 2; /* uplink relies on GDM2
					       * loopback
					       */

			/* For traffic forwarded to DSA devices select QoS
			 * channel according to the DSA user port index, rely
			 * on port id otherwise.
			 */
			channel = dsa_port >= 0 ? dsa_port : port->id;
			channel = channel % AIROHA_NUM_QOS_CHANNELS;
			priority = priority % AIROHA_NUM_QOS_QUEUES;
			qdata |= FIELD_PREP(AIROHA_FOE_CHANNEL, channel) |
				 FIELD_PREP(AIROHA_FOE_QID, priority);

			val |= FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT, pse_port) |
			       AIROHA_FOE_IB2_PSE_QOS;
			/* For downlink traffic consume SRAM memory for hw
			 * forwarding descriptors queue.
			 */
			if (airoha_is_lan_gdm_dev(dev))
				val |= AIROHA_FOE_IB2_FAST_PATH;
			if (dsa_port >= 0)
				val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ,
						  dsa_port);

			smac_id = port->id;
		}
	}

	if (is_multicast_ether_addr(data->eth.h_dest))
		val |= AIROHA_FOE_IB2_MULTICAST;

	ports_pad = 0xa5a5a500 | (l4proto & 0xff);
	if (type == PPE_PKT_TYPE_IPV4_ROUTE)
		hwe->ipv4.orig_tuple.ports = ports_pad;
	if (type == PPE_PKT_TYPE_IPV6_ROUTE_3T)
		hwe->ipv6.ports = ports_pad;

	if (type == PPE_PKT_TYPE_BRIDGE) {
		airoha_ppe_foe_set_bridge_addrs(&hwe->bridge, &data->eth);
		hwe->bridge.data = qdata;
		hwe->bridge.ib2 = val;
		l2 = &hwe->bridge.l2.common;
	} else if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
		hwe->ipv6.data = qdata;
		hwe->ipv6.ib2 = val;
		l2 = &hwe->ipv6.l2;
		l2->etype = ETH_P_IPV6;
	} else {
		hwe->ipv4.data = qdata;
		hwe->ipv4.ib2 = val;
		l2 = &hwe->ipv4.l2.common;
		l2->etype = ETH_P_IP;
	}

	l2->dest_mac_hi = get_unaligned_be32(data->eth.h_dest);
	l2->dest_mac_lo = get_unaligned_be16(data->eth.h_dest + 4);
	if (type <= PPE_PKT_TYPE_IPV4_DSLITE) {
		struct airoha_foe_mac_info *mac_info;

		l2->src_mac_hi = get_unaligned_be32(data->eth.h_source);
		hwe->ipv4.l2.src_mac_lo =
			get_unaligned_be16(data->eth.h_source + 4);

		mac_info = (struct airoha_foe_mac_info *)l2;
		mac_info->pppoe_id = data->pppoe.sid;
	} else {
		l2->src_mac_hi = FIELD_PREP(AIROHA_FOE_MAC_SMAC_ID, smac_id) |
				 FIELD_PREP(AIROHA_FOE_MAC_PPPOE_ID,
					    data->pppoe.sid);
	}

	if (data->vlan.num) {
		l2->vlan1 = data->vlan.hdr[0].id;
		if (data->vlan.num == 2)
			l2->vlan2 = data->vlan.hdr[1].id;
	}

	if (wlan_etype >= 0) {
		l2->etype = wlan_etype;
	} else if (dsa_port >= 0) {
		l2->etype = BIT(dsa_port);
		l2->etype |= !data->vlan.num ? BIT(15) : 0;
	} else if (data->pppoe.num) {
		l2->etype = ETH_P_PPP_SES;
	}

	return 0;
}

static int airoha_ppe_foe_entry_set_ipv4_tuple(struct airoha_foe_entry *hwe,
					       struct airoha_flow_data *data,
					       bool egress)
{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe->ib1);
	struct airoha_foe_ipv4_tuple *t;

	switch (type) {
	case PPE_PKT_TYPE_IPV4_HNAPT:
		if (egress) {
			t = &hwe->ipv4.new_tuple;
			break;
		}
		fallthrough;
	case PPE_PKT_TYPE_IPV4_DSLITE:
	case PPE_PKT_TYPE_IPV4_ROUTE:
		t = &hwe->ipv4.orig_tuple;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	t->src_ip = be32_to_cpu(data->v4.src_addr);
	t->dest_ip = be32_to_cpu(data->v4.dst_addr);

	if (type != PPE_PKT_TYPE_IPV4_ROUTE) {
		t->src_port = be16_to_cpu(data->src_port);
		t->dest_port = be16_to_cpu(data->dst_port);
	}

	return 0;
}

static int airoha_ppe_foe_entry_set_ipv6_tuple(struct airoha_foe_entry *hwe,
					       struct airoha_flow_data *data)

{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe->ib1);
	u32 *src, *dest;

	switch (type) {
	case PPE_PKT_TYPE_IPV6_ROUTE_5T:
	case PPE_PKT_TYPE_IPV6_6RD:
		hwe->ipv6.src_port = be16_to_cpu(data->src_port);
		hwe->ipv6.dest_port = be16_to_cpu(data->dst_port);
		fallthrough;
	case PPE_PKT_TYPE_IPV6_ROUTE_3T:
		src = hwe->ipv6.src_ip;
		dest = hwe->ipv6.dest_ip;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	ipv6_addr_be32_to_cpu(src, data->v6.src_addr.s6_addr32);
	ipv6_addr_be32_to_cpu(dest, data->v6.dst_addr.s6_addr32);

	return 0;
}

static u32 airoha_ppe_foe_get_entry_hash(struct airoha_ppe *ppe,
					 struct airoha_foe_entry *hwe)
{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe->ib1);
	u32 ppe_hash_mask = airoha_ppe_get_total_num_entries(ppe) - 1;
	u32 hash, hv1, hv2, hv3;

	switch (type) {
	case PPE_PKT_TYPE_IPV4_ROUTE:
	case PPE_PKT_TYPE_IPV4_HNAPT:
		hv1 = hwe->ipv4.orig_tuple.ports;
		hv2 = hwe->ipv4.orig_tuple.dest_ip;
		hv3 = hwe->ipv4.orig_tuple.src_ip;
		break;
	case PPE_PKT_TYPE_IPV6_ROUTE_3T:
	case PPE_PKT_TYPE_IPV6_ROUTE_5T:
		hv1 = hwe->ipv6.src_ip[3] ^ hwe->ipv6.dest_ip[3];
		hv1 ^= hwe->ipv6.ports;

		hv2 = hwe->ipv6.src_ip[2] ^ hwe->ipv6.dest_ip[2];
		hv2 ^= hwe->ipv6.dest_ip[0];

		hv3 = hwe->ipv6.src_ip[1] ^ hwe->ipv6.dest_ip[1];
		hv3 ^= hwe->ipv6.src_ip[0];
		break;
	case PPE_PKT_TYPE_BRIDGE: {
		struct airoha_foe_mac_info *l2 = &hwe->bridge.l2;

		hv1 = l2->common.src_mac_hi & 0xffff;
		hv1 = hv1 << 16 | l2->src_mac_lo;

		hv2 = l2->common.dest_mac_lo;
		hv2 = hv2 << 16;
		hv2 = hv2 | ((l2->common.src_mac_hi & 0xffff0000) >> 16);

		hv3 = l2->common.dest_mac_hi;
		break;
	}
	case PPE_PKT_TYPE_IPV4_DSLITE:
	case PPE_PKT_TYPE_IPV6_6RD:
	default:
		WARN_ON_ONCE(1);
		return ppe_hash_mask;
	}

	hash = (hv1 & hv2) | ((~hv1) & hv3);
	hash = (hash >> 24) | ((hash & 0xffffff) << 8);
	hash ^= hv1 ^ hv2 ^ hv3;
	hash ^= hash >> 16;
	hash &= ppe_hash_mask;

	return hash;
}

static int airoha_ppe_foe_get_flow_stats_index(struct airoha_ppe *ppe,
					       u32 hash, u32 *index)
{
	int ppe_num_stats_entries;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries < 0)
		return ppe_num_stats_entries;

	*index = hash >= ppe_num_stats_entries ?
		 hash - ppe->eth->soc->ppe_stats_entries : hash;

	return 0;
}

static void airoha_ppe_foe_flow_stat_entry_reset(struct airoha_ppe *ppe,
						 struct airoha_npu *npu,
						 int index)
{
	struct airoha_foe_stats __iomem *stats = airoha_npu_get_ppe_stats(npu);

	if (stats)
		memset_io(&stats[index], 0, sizeof(*stats));
	memset(&ppe->foe_stats[index], 0, sizeof(*ppe->foe_stats));
}

static void airoha_ppe_foe_flow_stats_reset(struct airoha_ppe *ppe,
					    struct airoha_npu *npu)
{
	int i, ppe_num_stats_entries;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries < 0)
		return;

	for (i = 0; i < ppe_num_stats_entries; i++)
		airoha_ppe_foe_flow_stat_entry_reset(ppe, npu, i);
}

static void airoha_ppe_foe_flow_stats_update(struct airoha_ppe *ppe,
					     struct airoha_npu *npu,
					     struct airoha_foe_entry *hwe,
					     u32 hash)
{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe->ib1);
	u32 index, pse_port, val, *data, *ib2, *meter;
	int ppe_num_stats_entries;
	u8 nbq;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries < 0)
		return;

	if (airoha_ppe_foe_get_flow_stats_index(ppe, hash, &index))
		return;

	if (index >= ppe_num_stats_entries)
		return;

	if (type == PPE_PKT_TYPE_BRIDGE) {
		data = &hwe->bridge.data;
		ib2 = &hwe->bridge.ib2;
		meter = &hwe->bridge.l2.meter;
	} else if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
		data = &hwe->ipv6.data;
		ib2 = &hwe->ipv6.ib2;
		meter = &hwe->ipv6.meter;
	} else {
		data = &hwe->ipv4.data;
		ib2 = &hwe->ipv4.ib2;
		meter = &hwe->ipv4.l2.meter;
	}

	pse_port = FIELD_GET(AIROHA_FOE_IB2_PSE_PORT, *ib2);
	if (pse_port == FE_PSE_PORT_CDM4)
		return;

	airoha_ppe_foe_flow_stat_entry_reset(ppe, npu, index);

	val = FIELD_GET(AIROHA_FOE_CHANNEL | AIROHA_FOE_QID, *data);
	*data = (*data & ~AIROHA_FOE_ACTDP) |
		FIELD_PREP(AIROHA_FOE_ACTDP, val);

	val = *ib2 & (AIROHA_FOE_IB2_NBQ | AIROHA_FOE_IB2_PSE_PORT |
		      AIROHA_FOE_IB2_PSE_QOS | AIROHA_FOE_IB2_FAST_PATH);
	*meter |= FIELD_PREP(AIROHA_FOE_TUNNEL_MTU, val);

	nbq = pse_port == 1 ? 6 : 5;
	*ib2 &= ~(AIROHA_FOE_IB2_NBQ | AIROHA_FOE_IB2_PSE_PORT |
		  AIROHA_FOE_IB2_PSE_QOS);
	*ib2 |= FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT, 6) |
		FIELD_PREP(AIROHA_FOE_IB2_NBQ, nbq);
}

static struct airoha_foe_entry *
airoha_ppe_foe_get_entry_locked(struct airoha_ppe *ppe, u32 sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	lockdep_assert_held(&ppe_lock);

	if (sw_idx >= airoha_ppe_get_total_num_entries(ppe))
		return NULL;

	if (sw_idx < sram_num_entries) {
		u32 hw_idx = airoha_ppe_sw_to_hw_idx(ppe, sw_idx);
		u32 *hwe = ppe->foe + sw_idx * sizeof(struct airoha_foe_entry);
		bool ppe2 = sw_idx >= ppe->eth->soc->ppe_sram_entries;
		struct airoha_eth *eth = ppe->eth;
		u32 val;
		int i;

		airoha_fe_wr(eth, REG_PPE_RAM_CTRL(ppe2),
			     FIELD_PREP(PPE_SRAM_CTRL_ENTRY_MASK, hw_idx) |
			     PPE_SRAM_CTRL_REQ_MASK);
		if (read_poll_timeout_atomic(airoha_fe_rr, val,
					     val & PPE_SRAM_CTRL_ACK_MASK,
					     10, 100, false, eth,
					     REG_PPE_RAM_CTRL(ppe2)))
			return NULL;

		for (i = 0; i < PPE_ENTRY_WORDS; i++)
			hwe[i] = airoha_fe_rr(eth,
					      REG_PPE_RAM_ENTRY(ppe2, i));
	}

	return ppe->foe + sw_idx * sizeof(struct airoha_foe_entry);
}

static int airoha_ppe_foe_read_entry_locked(struct airoha_ppe *ppe,
					    u32 index,
					    struct airoha_foe_entry *entry)
{
	struct airoha_foe_entry *hwe;

	hwe = airoha_ppe_foe_get_entry_locked(ppe, index);
	if (!hwe)
		return -ETIMEDOUT;

	/* Ensure hardware writes to DRAM FoE entries are visible before read. */
	dma_rmb();
	memcpy(entry, hwe, sizeof(*entry));
	airoha_ppe_foe_decode_entry(ppe->eth, entry);

	return 0;
}

struct airoha_foe_entry *airoha_ppe_foe_get_entry(struct airoha_ppe *ppe,
						  u32 hash)
{
	struct airoha_foe_entry *hwe;

	spin_lock_bh(&ppe_lock);
	hwe = airoha_ppe_foe_get_entry_locked(ppe, hash);
	spin_unlock_bh(&ppe_lock);

	return hwe;
}

int airoha_ppe_debugfs_read_entry(struct airoha_ppe *ppe, u32 index,
				  struct airoha_foe_entry *entry)
{
	int err;

	if (index >= airoha_ppe_get_total_num_entries(ppe))
		return -EINVAL;

	spin_lock_bh(&ppe_lock);
	err = airoha_ppe_foe_read_entry_locked(ppe, index, entry);
	spin_unlock_bh(&ppe_lock);

	return err;
}

static bool airoha_ppe_foe_compare_entry(struct airoha_flow_table_entry *e,
					 struct airoha_foe_entry *hwe)
{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, e->data.ib1);
	int len;

	if ((hwe->ib1 ^ e->data.ib1) & (AIROHA_FOE_IB1_BIND_UDP |
					AIROHA_FOE_IB1_BIND_PACKET_TYPE))
		return false;

	if (type > PPE_PKT_TYPE_IPV4_DSLITE)
		len = offsetof(struct airoha_foe_entry, ipv6.data);
	else
		len = offsetof(struct airoha_foe_entry, ipv4.ib2);

	return !memcmp(&e->data.d, &hwe->d, len - sizeof(hwe->ib1));
}

static int airoha_ppe_foe_commit_sram_entry(struct airoha_ppe *ppe, u32 sw_idx)
{
	struct airoha_foe_entry *hwe = ppe->foe + sw_idx * sizeof(*hwe);
	bool ppe2 = sw_idx >= ppe->eth->soc->ppe_sram_entries;
	u32 hw_idx = airoha_ppe_sw_to_hw_idx(ppe, sw_idx);
	u32 *ptr = (u32 *)hwe, val;
	int i;

	for (i = 0; i < PPE_ENTRY_WORDS; i++)
		airoha_fe_wr(ppe->eth, REG_PPE_RAM_ENTRY(ppe2, i), ptr[i]);

	/* Order entry data writes before triggering SRAM commit. */
	wmb();
	airoha_fe_wr(ppe->eth, REG_PPE_RAM_CTRL(ppe2),
		     FIELD_PREP(PPE_SRAM_CTRL_ENTRY_MASK, hw_idx) |
		     PPE_SRAM_CTRL_WR_MASK | PPE_SRAM_CTRL_REQ_MASK);

	return read_poll_timeout_atomic(airoha_fe_rr, val,
					val & PPE_SRAM_CTRL_ACK_MASK,
					10, 100, false, ppe->eth,
					REG_PPE_RAM_CTRL(ppe2));
}

static int airoha_ppe_foe_commit_entry(struct airoha_ppe *ppe,
				       struct airoha_foe_entry *e,
				       u32 sw_idx, bool rx_wlan)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	struct airoha_foe_entry *hwe = ppe->foe + sw_idx * sizeof(*hwe);
	u32 ts = airoha_ppe_get_timestamp(ppe);
	struct airoha_eth *eth = ppe->eth;
	struct airoha_foe_entry encoded;
	struct airoha_npu *npu;
	int err = 0;

	airoha_ppe_foe_encode_entry(eth, &encoded, e, hwe);
	memcpy(&hwe->words[1], &encoded.words[1],
	       sizeof(*hwe) - sizeof(hwe->words[0]));
	/* Publish FoE payload before updating ib1 state/timestamp. */
	wmb();

	e->ib1 &= ~AIROHA_FOE_IB1_BIND_TIMESTAMP;
	e->ib1 |= FIELD_PREP(AIROHA_FOE_IB1_BIND_TIMESTAMP, ts);
	hwe->ib1 = e->ib1;

	rcu_read_lock();

	npu = rcu_dereference(eth->npu);
	if (!rx_wlan && npu)
		airoha_ppe_foe_flow_stats_update(ppe, npu, hwe, sw_idx);

	if (sw_idx < sram_num_entries)
		err = airoha_ppe_foe_commit_sram_entry(ppe, sw_idx);

	rcu_read_unlock();

	return err;
}

static void airoha_ppe_foe_remove_flow(struct airoha_ppe *ppe,
				       struct airoha_flow_table_entry *e)
{
	lockdep_assert_held(&ppe_lock);

	hlist_del_init(&e->list);
	if (e->hash != 0xffff) {
		e->data.ib1 &= ~AIROHA_FOE_IB1_BIND_STATE;
		e->data.ib1 |= FIELD_PREP(AIROHA_FOE_IB1_BIND_STATE,
					  AIROHA_FOE_STATE_INVALID);
		airoha_ppe_foe_commit_entry(ppe, &e->data, e->hash, false);
		e->hash = 0xffff;
	}
	if (e->type == FLOW_TYPE_L2_SUBFLOW) {
		hlist_del_init(&e->l2_subflow_node);
		kfree(e);
	}
}

static void airoha_ppe_foe_remove_l2_flow(struct airoha_ppe *ppe,
					  struct airoha_flow_table_entry *e)
{
	struct hlist_head *head = &e->l2_flows;
	struct hlist_node *n;

	lockdep_assert_held(&ppe_lock);

	rhashtable_remove_fast(&ppe->l2_flows, &e->l2_node,
			       airoha_l2_flow_table_params);
	hlist_for_each_entry_safe(e, n, head, l2_subflow_node)
		airoha_ppe_foe_remove_flow(ppe, e);
}

static void airoha_ppe_foe_flow_remove_entry(struct airoha_ppe *ppe,
					     struct airoha_flow_table_entry *e)
{
	spin_lock_bh(&ppe_lock);

	if (e->type == FLOW_TYPE_L2)
		airoha_ppe_foe_remove_l2_flow(ppe, e);
	else
		airoha_ppe_foe_remove_flow(ppe, e);

	spin_unlock_bh(&ppe_lock);
}

static int
airoha_ppe_foe_commit_subflow_entry(struct airoha_ppe *ppe,
				    struct airoha_flow_table_entry *e,
				    u32 sw_idx, bool rx_wlan)
{
	u32 mask = AIROHA_FOE_IB1_BIND_PACKET_TYPE | AIROHA_FOE_IB1_BIND_UDP;
	struct airoha_foe_entry hwe;
	struct airoha_flow_table_entry *f;
	int type;

	if (airoha_ppe_foe_read_entry_locked(ppe, sw_idx, &hwe))
		return -EINVAL;

	f = kzalloc_obj(*f, GFP_ATOMIC);
	if (!f)
		return -ENOMEM;

	hlist_add_head(&f->l2_subflow_node, &e->l2_flows);
	f->type = FLOW_TYPE_L2_SUBFLOW;
	f->hash = sw_idx;

	hwe.ib1 = (hwe.ib1 & mask) | (e->data.ib1 & ~mask);

	type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe.ib1);
	if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
		memcpy(&hwe.ipv6.l2, &e->data.bridge.l2, sizeof(hwe.ipv6.l2));
		hwe.ipv6.ib2 = e->data.bridge.ib2;
		/* setting smac_id to 0xf instruct the hw to keep original
		 * source mac address
		 */
		hwe.ipv6.l2.src_mac_hi = FIELD_PREP(AIROHA_FOE_MAC_SMAC_ID,
						    0xf);
		hwe.ipv6.data = e->data.bridge.data;
	} else if (type == PPE_PKT_TYPE_BRIDGE) {
		hwe.bridge.l2 = e->data.bridge.l2;
		hwe.bridge.ib2 = e->data.bridge.ib2;
		hwe.bridge.data = e->data.bridge.data;
	} else {
		hwe.ipv4.l2 = e->data.bridge.l2;
		hwe.ipv4.ib2 = e->data.bridge.ib2;
		hwe.ipv4.data = e->data.bridge.data;
		if (type == PPE_PKT_TYPE_IPV4_HNAPT)
			memcpy(&hwe.ipv4.new_tuple, &hwe.ipv4.orig_tuple,
			       sizeof(hwe.ipv4.new_tuple));
	}

	airoha_ppe_foe_commit_entry(ppe, &hwe, sw_idx, rx_wlan);

	return 0;
}

enum airoha_ppe_tuple_family {
	AIROHA_PPE_TUPLE_IPV4,
	AIROHA_PPE_TUPLE_IPV6,
};

struct airoha_ppe_skb_tuple {
	enum airoha_ppe_tuple_family family;
	u8 l4proto;
	bool has_ports;

	union {
		struct {
			__be32 src;
			__be32 dst;
		} v4;
		struct {
			struct in6_addr src;
			struct in6_addr dst;
		} v6;
	};

	u16 src_port;
	u16 dst_port;
};

static bool
airoha_ppe_skb_read_ports(struct sk_buff *skb, unsigned int off, u8 l4proto,
			  struct airoha_ppe_skb_tuple *tuple)
{
	struct udphdr _uh, *uh;
	struct tcphdr _th, *th;

	tuple->has_ports = false;

	switch (l4proto) {
	case IPPROTO_TCP:
		th = skb_header_pointer(skb, off, sizeof(_th), &_th);
		if (!th)
			return false;

		tuple->src_port = be16_to_cpu(th->source);
		tuple->dst_port = be16_to_cpu(th->dest);
		break;
	case IPPROTO_UDP:
		uh = skb_header_pointer(skb, off, sizeof(_uh), &_uh);
		if (!uh)
			return false;

		tuple->src_port = be16_to_cpu(uh->source);
		tuple->dst_port = be16_to_cpu(uh->dest);
		break;
	default:
		return false;
	}

	tuple->has_ports = true;
	return true;
}

static bool
airoha_ppe_skb_read_ipv4_tuple_at(struct sk_buff *skb, unsigned int off,
				  struct airoha_ppe_skb_tuple *tuple)
{
	struct iphdr _iph, *iph;
	unsigned int l4_off;
	u16 tot_len;

	if (off + sizeof(_iph) > skb->len)
		return false;

	iph = skb_header_pointer(skb, off, sizeof(_iph), &_iph);
	if (!iph || iph->version != 4 || iph->ihl < 5)
		return false;

	if (iph->protocol != IPPROTO_TCP && iph->protocol != IPPROTO_UDP)
		return false;

	tot_len = ntohs(iph->tot_len);
	if (tot_len < iph->ihl * 4 || off + tot_len > skb->len)
		return false;

	tuple->family = AIROHA_PPE_TUPLE_IPV4;
	tuple->l4proto = iph->protocol;
	tuple->v4.src = iph->saddr;
	tuple->v4.dst = iph->daddr;

	l4_off = off + iph->ihl * 4;
	return airoha_ppe_skb_read_ports(skb, l4_off, iph->protocol, tuple);
}

static bool
airoha_ppe_skb_read_ipv6_tuple_at(struct sk_buff *skb, unsigned int off,
				  struct airoha_ppe_skb_tuple *tuple)
{
	struct ipv6_opt_hdr _opth, *opth;
	struct ip_auth_hdr _authh, *authh;
	struct frag_hdr _fragh, *fragh;
	struct ipv6hdr _ip6h, *ip6h;
	unsigned int l4_off, hdr_len;
	u8 nexthdr;
	int i;

	if (off + sizeof(_ip6h) > skb->len)
		return false;

	ip6h = skb_header_pointer(skb, off, sizeof(_ip6h), &_ip6h);
	if (!ip6h || ip6h->version != 6)
		return false;

	if (ip6h->payload_len &&
	    off + sizeof(*ip6h) + ntohs(ip6h->payload_len) > skb->len)
		return false;

	nexthdr = ip6h->nexthdr;
	l4_off = off + sizeof(*ip6h);

	for (i = 0; i < 8; i++) {
		switch (nexthdr) {
		case NEXTHDR_HOP:
		case NEXTHDR_ROUTING:
		case NEXTHDR_DEST:
			opth = skb_header_pointer(skb, l4_off,
						  sizeof(_opth), &_opth);
			if (!opth)
				return false;

			hdr_len = ipv6_optlen(opth);
			if (!hdr_len || l4_off + hdr_len > skb->len)
				return false;

			nexthdr = opth->nexthdr;
			l4_off += hdr_len;
			continue;
		case NEXTHDR_FRAGMENT:
			fragh = skb_header_pointer(skb, l4_off,
						   sizeof(_fragh), &_fragh);
			if (!fragh)
				return false;

			if (ntohs(fragh->frag_off) & 0xfff8)
				return false;

			nexthdr = fragh->nexthdr;
			l4_off += sizeof(*fragh);
			continue;
		case NEXTHDR_AUTH:
			authh = skb_header_pointer(skb, l4_off,
						   sizeof(_authh), &_authh);
			if (!authh)
				return false;

			hdr_len = (authh->hdrlen + 2) << 2;
			if (!hdr_len || l4_off + hdr_len > skb->len)
				return false;

			nexthdr = authh->nexthdr;
			l4_off += hdr_len;
			continue;
		case NEXTHDR_NONE:
		case NEXTHDR_ESP:
			return false;
		default:
			goto l4;
		}
	}

	return false;

l4:
	if (nexthdr != IPPROTO_TCP && nexthdr != IPPROTO_UDP)
		return false;

	tuple->family = AIROHA_PPE_TUPLE_IPV6;
	tuple->l4proto = nexthdr;
	tuple->v6.src = ip6h->saddr;
	tuple->v6.dst = ip6h->daddr;

	return airoha_ppe_skb_read_ports(skb, l4_off, nexthdr, tuple);
}

static bool
airoha_ppe_skb_find_tuple(struct sk_buff *skb,
			  struct airoha_ppe_skb_tuple *tuple)
{
	static const unsigned int fixed_offsets[] = {
		0,
		ETH_HLEN,
		ETH_HLEN + VLAN_HLEN,
		ETH_HLEN + 2 * VLAN_HLEN,
	};
	unsigned int i, off, max_scan;

	for (i = 0; i < ARRAY_SIZE(fixed_offsets); i++) {
		off = fixed_offsets[i];
		if (airoha_ppe_skb_read_ipv4_tuple_at(skb, off, tuple) ||
		    airoha_ppe_skb_read_ipv6_tuple_at(skb, off, tuple))
			return true;
	}

	off = skb_network_offset(skb);
	if (off < skb->len &&
	    (airoha_ppe_skb_read_ipv4_tuple_at(skb, off, tuple) ||
	     airoha_ppe_skb_read_ipv6_tuple_at(skb, off, tuple)))
		return true;

	max_scan = min_t(unsigned int, skb->len, 96);
	for (off = 0; off < max_scan; off++) {
		if (airoha_ppe_skb_read_ipv4_tuple_at(skb, off, tuple) ||
		    airoha_ppe_skb_read_ipv6_tuple_at(skb, off, tuple))
			return true;
	}

	return false;
}

static bool
airoha_ppe_skb_tuple_is_candidate(const struct airoha_ppe_skb_tuple *tuple)
{
	switch (tuple->family) {
	case AIROHA_PPE_TUPLE_IPV4:
		return !ipv4_is_multicast(tuple->v4.dst) &&
		       !ipv4_is_lbcast(tuple->v4.dst) &&
		       !ipv4_is_zeronet(tuple->v4.dst);
	case AIROHA_PPE_TUPLE_IPV6:
		return !ipv6_addr_any(&tuple->v6.dst) &&
		       !ipv6_addr_is_multicast(&tuple->v6.dst) &&
		       !(ipv6_addr_type(&tuple->v6.dst) & IPV6_ADDR_LINKLOCAL);
	default:
		return false;
	}
}

static bool
airoha_ppe_ipv4_tuple_match(const struct airoha_foe_ipv4_tuple *foe,
			    const struct airoha_ppe_skb_tuple *skb,
			    bool with_ports)
{
	if (skb->family != AIROHA_PPE_TUPLE_IPV4)
		return false;

	if (foe->src_ip != be32_to_cpu(skb->v4.src) ||
	    foe->dest_ip != be32_to_cpu(skb->v4.dst))
		return false;

	if (!with_ports)
		return true;

	return skb->has_ports && foe->src_port == skb->src_port &&
	       foe->dest_port == skb->dst_port;
}

static bool
airoha_ppe_ipv6_tuple_match(const struct airoha_foe_ipv6 *foe,
			    const struct airoha_ppe_skb_tuple *skb,
			    bool with_ports)
{
	u32 src[IPV6_ADDR_WORDS], dst[IPV6_ADDR_WORDS];

	if (skb->family != AIROHA_PPE_TUPLE_IPV6)
		return false;

	ipv6_addr_be32_to_cpu(src, skb->v6.src.s6_addr32);
	ipv6_addr_be32_to_cpu(dst, skb->v6.dst.s6_addr32);

	if (memcmp(foe->src_ip, src, sizeof(src)) ||
	    memcmp(foe->dest_ip, dst, sizeof(dst)))
		return false;

	if (!with_ports)
		return true;

	return skb->has_ports && foe->src_port == skb->src_port &&
	       foe->dest_port == skb->dst_port;
}

static bool airoha_ppe_lro_local_socket(struct net_device *dev,
					const struct airoha_ppe_skb_tuple *tuple)
{
#if IS_ENABLED(CONFIG_INET)
	struct net *net = dev_net(dev);
	struct sock *sk = NULL;
	bool established;

	if (tuple->family == AIROHA_PPE_TUPLE_IPV4) {
		if (inet_addr_type(net, tuple->v4.dst) != RTN_LOCAL)
			return false;
		sk = inet_lookup_established(net, tuple->v4.src,
					     htons(tuple->src_port),
					     tuple->v4.dst,
					     htons(tuple->dst_port),
					     dev->ifindex);
	}
#if IS_REACHABLE(CONFIG_IPV6)
	if (tuple->family == AIROHA_PPE_TUPLE_IPV6) {
		if (!ipv6_chk_addr(net, &tuple->v6.dst, NULL, 0))
			return false;
		sk = __inet6_lookup_established(net, &tuple->v6.src,
						htons(tuple->src_port),
						&tuple->v6.dst,
						tuple->dst_port,
						dev->ifindex, 0);
	}
#endif
	if (!sk)
		return false;
	established = READ_ONCE(sk->sk_state) == TCP_ESTABLISHED;
	sock_gen_put(sk);
	return established;
#else
	return false;
#endif
}

static bool airoha_ppe_lro_parse(struct sk_buff *skb,
				 struct airoha_flow_data *data,
				 struct airoha_ppe_skb_tuple *tuple,
				 u8 *dsfield)
{
	__be16 proto = eth_hdr(skb)->h_proto;
	unsigned int off = 0, end, limit = skb->len;
	struct tcphdr th;
	unsigned int th_off;

	data->eth = *eth_hdr(skb);
	while (proto == htons(ETH_P_8021Q)) {
		struct vlan_hdr vh;
		unsigned int n = data->vlan.num;

		if (n == ARRAY_SIZE(data->vlan.hdr) ||
		    skb_copy_bits(skb, off, &vh, sizeof(vh)))
			return false;
		data->vlan.hdr[n].id = ntohs(vh.h_vlan_TCI);
		data->vlan.num++;
		proto = vh.h_vlan_encapsulated_proto;
		off += sizeof(vh);
	}
	if (proto == htons(ETH_P_PPP_SES)) {
		struct {
			u8 ver_type, code;
			__be16 sid, len, proto;
		} ph;

		if (skb_copy_bits(skb, off, &ph, sizeof(ph)) ||
		    ph.ver_type != 0x11 || ph.code || ntohs(ph.len) < 2 ||
		    off + sizeof(ph) - 2 + ntohs(ph.len) > skb->len)
			return false;
		limit = off + sizeof(ph) - 2 + ntohs(ph.len);
		data->pppoe.num = 1;
		data->pppoe.sid = ntohs(ph.sid);
		if (ph.proto == htons(PPP_IP))
			proto = htons(ETH_P_IP);
		else if (ph.proto == htons(PPP_IPV6))
			proto = htons(ETH_P_IPV6);
		else
			return false;
		off += sizeof(ph);
	}

	if (proto == htons(ETH_P_IP)) {
		struct iphdr iph;

		if (skb_copy_bits(skb, off, &iph, sizeof(iph)) ||
		    ip_is_fragment(&iph) ||
		    !airoha_ppe_skb_read_ipv4_tuple_at(skb, off, tuple))
			return false;
		th_off = off + iph.ihl * 4;
		end = off + ntohs(iph.tot_len);
		*dsfield = ipv4_get_dsfield(&iph);
	} else if (proto == htons(ETH_P_IPV6)) {
		struct ipv6hdr iph;

		if (skb_copy_bits(skb, off, &iph, sizeof(iph)) ||
		    iph.nexthdr != IPPROTO_TCP || !iph.payload_len ||
		    !airoha_ppe_skb_read_ipv6_tuple_at(skb, off, tuple))
			return false;
		th_off = off + sizeof(iph);
		end = th_off + ntohs(iph.payload_len);
		*dsfield = ipv6_get_dsfield(&iph);
	} else {
		return false;
	}

	if (tuple->l4proto != IPPROTO_TCP || !tuple->has_ports ||
	    skb_copy_bits(skb, th_off, &th, sizeof(th)))
		return false;
	return th.ack && !th.syn && !th.fin && !th.rst && th.doff >= 5 &&
	       th_off + th.doff * 4 <= end && end <= limit;
}

static bool airoha_ppe_lro_entry_matches(const struct airoha_foe_entry *entry,
					 const struct airoha_foe_entry *owner)
{
	u32 type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, owner->ib1);
	u32 mask = AIROHA_FOE_IB1_BIND_PACKET_TYPE | AIROHA_FOE_IB1_BIND_UDP;

	if (FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, entry->ib1) !=
	    AIROHA_FOE_STATE_BIND || ((entry->ib1 ^ owner->ib1) & mask))
		return false;
	if (type == PPE_PKT_TYPE_IPV4_HNAPT)
		return entry->ipv4.ib2 == owner->ipv4.ib2 &&
		       !memcmp(&entry->ipv4.orig_tuple, &owner->ipv4.orig_tuple,
			       sizeof(entry->ipv4.orig_tuple));
	return entry->ipv6.ib2 == owner->ipv6.ib2 &&
	       !memcmp(&entry->ipv6, &owner->ipv6,
		       offsetof(struct airoha_foe_ipv6, data));
}

void airoha_ppe_lro_flush(struct airoha_eth *eth, unsigned int qdma_id)
{
	struct airoha_ppe *ppe = eth->ppe;
	unsigned int i;

	if (!ppe || !airoha_is_7552(eth) || qdma_id >= AIROHA_MAX_NUM_QDMA)
		return;

	spin_lock_bh(&ppe_lock);
	for (i = 0; i < ARRAY_SIZE(ppe->lro[qdma_id]); i++) {
		struct airoha_ppe_lro_entry *slot = &ppe->lro[qdma_id][i];
		struct airoha_foe_entry entry;

		if (!slot->valid)
			continue;
		if (airoha_ppe_foe_read_entry_locked(ppe, slot->hash, &entry))
			continue;
		if (airoha_ppe_lro_entry_matches(&entry, &slot->entry)) {
			entry.ib1 &= ~AIROHA_FOE_IB1_BIND_STATE;
			if (airoha_ppe_foe_commit_entry(ppe, &entry,
							slot->hash, false))
				continue;
		}
		slot->valid = false;
	}
	spin_unlock_bh(&ppe_lock);
}

void airoha_ppe_check_lro(struct airoha_qdma *qdma, struct sk_buff *skb,
			  u16 hash)
{
	struct airoha_eth *eth = qdma->eth;
	struct airoha_ppe *ppe = eth->ppe;
	struct airoha_ppe_skb_tuple tuple;
	struct airoha_flow_data data = {};
	struct airoha_foe_entry entry, hwe;
	u32 qdma_id = qdma - eth->qdma;
	u32 sw_idx, type, ib2;
	u8 dsfield;
	int i, free_slot = -1;

	if (!ppe || !airoha_is_7552(eth) || skb->len <= 256 ||
	    !(skb->dev->features & NETIF_F_LRO) ||
	    !airoha_fe_get(eth, REG_CDM_LRO_EN(qdma_id + 1),
			   AN7552_LRO_RXQ_EN_MASK) ||
	    airoha_ppe_hw_to_sw_idx(ppe, hash, &sw_idx))
		return;
	if (!airoha_ppe_lro_parse(skb, &data, &tuple, &dsfield) ||
	    !airoha_ppe_lro_local_socket(skb->dev, &tuple))
		return;

	type = tuple.family == AIROHA_PPE_TUPLE_IPV4 ?
		PPE_PKT_TYPE_IPV4_HNAPT : PPE_PKT_TYPE_IPV6_ROUTE_5T;
	if (airoha_ppe_foe_entry_prepare(eth, &entry, NULL, type, &data,
					 IPPROTO_TCP, dsfield))
		return;
	/* CPU delivery must preserve TTL and the ingress source MAC. */
	entry.ib1 &= ~AIROHA_FOE_IB1_BIND_TTL;
	if (type == PPE_PKT_TYPE_IPV6_ROUTE_5T)
		entry.ipv6.l2.src_mac_hi =
			FIELD_PREP(AIROHA_FOE_MAC_SMAC_ID, 0x10) |
			data.pppoe.sid;

	spin_lock_bh(&ppe_lock);
	if (READ_ONCE(qdma->users) != 1 ||
	    !airoha_fe_get(eth, REG_CDM_LRO_EN(qdma_id + 1),
			   AN7552_LRO_RXQ_EN_MASK) ||
	    airoha_ppe_foe_read_entry_locked(ppe, sw_idx, &hwe))
		goto unlock;
	if (FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, hwe.ib1) !=
	    AIROHA_FOE_STATE_UNBIND ||
	    FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe.ib1) != type ||
	    (hwe.ib1 & AIROHA_FOE_IB1_BIND_UDP))
		goto unlock;
	if (type == PPE_PKT_TYPE_IPV4_HNAPT) {
		if (!airoha_ppe_ipv4_tuple_match(&hwe.ipv4.orig_tuple,
						 &tuple, true))
			goto unlock;
		entry.ipv4.orig_tuple = hwe.ipv4.orig_tuple;
		entry.ipv4.new_tuple = hwe.ipv4.orig_tuple;
	} else {
		if (!airoha_ppe_ipv6_tuple_match(&hwe.ipv6, &tuple, true))
			goto unlock;
		memcpy(&entry.ipv6, &hwe.ipv6,
		       offsetof(struct airoha_foe_ipv6, data));
	}

	for (i = 0; i < ARRAY_SIZE(ppe->lro[qdma_id]); i++) {
		struct airoha_ppe_lro_entry *slot = &ppe->lro[qdma_id][i];
		struct airoha_foe_entry old;

		if (slot->valid) {
			if (airoha_ppe_foe_read_entry_locked(ppe, slot->hash,
							     &old))
				continue;
			if (airoha_ppe_lro_entry_matches(&old, &slot->entry))
				continue;
			slot->valid = false;
		}
		free_slot = i;
		break;
	}
	if (free_slot < 0)
		goto unlock;

	/* FQOS=0 selects CPU RX, using the LAN or WAN CDM directly. */
	ib2 = FIELD_PREP(AIROHA_FOE_IB2_PORT_AG, 0x1f) |
	      FIELD_PREP(AIROHA_FOE_IB2_DSCP, dsfield) |
	      FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT,
			 qdma_id ? FE_PSE_PORT_CDM2 : FE_PSE_PORT_CDM1) |
	      FIELD_PREP(AIROHA_FOE_IB2_NBQ, 11 + free_slot);
	if (type == PPE_PKT_TYPE_IPV4_HNAPT)
		entry.ipv4.ib2 = ib2;
	else
		entry.ipv6.ib2 = ib2;
	if (!airoha_ppe_foe_commit_entry(ppe, &entry, sw_idx, false)) {
		ppe->lro[qdma_id][free_slot].entry = entry;
		ppe->lro[qdma_id][free_slot].hash = sw_idx;
		ppe->lro[qdma_id][free_slot].valid = true;
	}
unlock:
	spin_unlock_bh(&ppe_lock);
}

static bool
airoha_ppe_foe_entry_match_skb_tuple(struct airoha_flow_table_entry *e,
				     const struct airoha_ppe_skb_tuple *skb)
{
	u32 type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, e->data.ib1);
	u8 l4proto = e->data.ib1 & AIROHA_FOE_IB1_BIND_UDP ?
		     IPPROTO_UDP : IPPROTO_TCP;

	if (skb->l4proto != l4proto)
		return false;

	switch (type) {
	case PPE_PKT_TYPE_IPV4_ROUTE:
		return airoha_ppe_ipv4_tuple_match(&e->data.ipv4.orig_tuple,
						   skb, false);
	case PPE_PKT_TYPE_IPV4_HNAPT:
		return airoha_ppe_ipv4_tuple_match(&e->data.ipv4.orig_tuple,
						   skb, true) ||
		       airoha_ppe_ipv4_tuple_match(&e->data.ipv4.new_tuple,
						   skb, true);
	case PPE_PKT_TYPE_IPV6_ROUTE_3T:
		return airoha_ppe_ipv6_tuple_match(&e->data.ipv6, skb, false);
	case PPE_PKT_TYPE_IPV6_ROUTE_5T:
		return airoha_ppe_ipv6_tuple_match(&e->data.ipv6, skb, true);
	default:
		return false;
	}
}

static struct airoha_flow_table_entry *
airoha_ppe_find_pending_flow_by_skb(struct airoha_ppe *ppe, struct sk_buff *skb)
{
	struct airoha_ppe_skb_tuple tuple;
	struct airoha_flow_table_entry *e;

	if (!airoha_ppe_skb_find_tuple(skb, &tuple) ||
	    !airoha_ppe_skb_tuple_is_candidate(&tuple))
		return NULL;

	hlist_for_each_entry(e, &ppe->pending_flows, list) {
		if (e->type != FLOW_TYPE_L4 || e->hash != 0xffff)
			continue;

		if (airoha_ppe_foe_entry_match_skb_tuple(e, &tuple))
			return e;
	}

	return NULL;
}

static void airoha_ppe_foe_insert_entry(struct airoha_ppe *ppe,
					struct sk_buff *skb,
					u32 sw_idx, bool rx_wlan)
{
	struct airoha_flow_table_entry *e;
	struct airoha_foe_bridge br = {};
	struct airoha_foe_entry entry, *hwe = &entry;
	bool commit_done = false;
	struct hlist_node *n;
	u32 index, state;

	spin_lock_bh(&ppe_lock);

	if (airoha_ppe_foe_read_entry_locked(ppe, sw_idx, hwe))
		goto unlock;

	state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, hwe->ib1);
	if (state == AIROHA_FOE_STATE_BIND)
		goto unlock;

	if (airoha_is_7523(ppe->eth) || airoha_is_7552(ppe->eth)) {
		e = airoha_ppe_find_pending_flow_by_skb(ppe, skb);
		if (e && !airoha_ppe_foe_commit_entry(ppe, &e->data,
						      sw_idx, rx_wlan)) {
			hlist_del_init(&e->list);
			hlist_add_head(&e->list, &ppe->foe_flow[sw_idx]);
			e->hash = sw_idx;
		}
		if (e || !airoha_is_7552(ppe->eth))
			goto unlock;
	}

	if (state == AIROHA_FOE_STATE_INVALID)
		goto unlock;

	index = airoha_ppe_foe_get_entry_hash(ppe, hwe);
	hlist_for_each_entry_safe(e, n, &ppe->foe_flow[index], list) {
		if (e->type == FLOW_TYPE_L2_SUBFLOW) {
			state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, hwe->ib1);
			if (state != AIROHA_FOE_STATE_BIND) {
				e->hash = 0xffff;
				airoha_ppe_foe_remove_flow(ppe, e);
			}
			continue;
		}

		if (!airoha_ppe_foe_compare_entry(e, hwe))
			continue;

		airoha_ppe_foe_commit_entry(ppe, &e->data, sw_idx, rx_wlan);
		commit_done = true;
		e->hash = sw_idx;
	}

	if (commit_done)
		goto unlock;

	airoha_ppe_foe_set_bridge_addrs(&br, eth_hdr(skb));
	e = rhashtable_lookup_fast(&ppe->l2_flows, &br,
				   airoha_l2_flow_table_params);
	if (e)
		airoha_ppe_foe_commit_subflow_entry(ppe, e, sw_idx, rx_wlan);
unlock:
	spin_unlock_bh(&ppe_lock);
}

static int
airoha_ppe_foe_l2_flow_commit_entry(struct airoha_ppe *ppe,
				    struct airoha_flow_table_entry *e)
{
	struct airoha_flow_table_entry *prev;

	e->type = FLOW_TYPE_L2;
	prev = rhashtable_lookup_get_insert_fast(&ppe->l2_flows, &e->l2_node,
						 airoha_l2_flow_table_params);
	if (!prev)
		return 0;

	if (IS_ERR(prev))
		return PTR_ERR(prev);

	return rhashtable_replace_fast(&ppe->l2_flows, &prev->l2_node,
				       &e->l2_node,
				       airoha_l2_flow_table_params);
}

static int airoha_ppe_foe_flow_commit_entry(struct airoha_ppe *ppe,
					    struct airoha_flow_table_entry *e)
{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, e->data.ib1);
	u32 hash;

	if (type == PPE_PKT_TYPE_BRIDGE)
		return airoha_ppe_foe_l2_flow_commit_entry(ppe, e);

	hash = airoha_ppe_foe_get_entry_hash(ppe, &e->data);
	e->type = FLOW_TYPE_L4;
	e->hash = 0xffff;

	spin_lock_bh(&ppe_lock);
	if (airoha_is_7523(ppe->eth) || airoha_is_7552(ppe->eth))
		hlist_add_head(&e->list, &ppe->pending_flows);
	else
		hlist_add_head(&e->list, &ppe->foe_flow[hash]);
	spin_unlock_bh(&ppe_lock);

	return 0;
}

static int airoha_ppe_get_entry_idle_time(struct airoha_ppe *ppe, u32 ib1)
{
	u32 state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, ib1);
	u32 ts, ts_mask, now = airoha_ppe_get_timestamp(ppe);
	int idle;

	if (state == AIROHA_FOE_STATE_BIND) {
		ts = FIELD_GET(AIROHA_FOE_IB1_BIND_TIMESTAMP, ib1);
		ts_mask = AIROHA_FOE_IB1_BIND_TIMESTAMP;
	} else {
		ts = FIELD_GET(AIROHA_FOE_IB1_UNBIND_TIMESTAMP, ib1);
		now = FIELD_GET(AIROHA_FOE_IB1_UNBIND_TIMESTAMP, now);
		ts_mask = AIROHA_FOE_IB1_UNBIND_TIMESTAMP;
	}
	idle = now - ts;

	return idle < 0 ? idle + ts_mask + 1 : idle;
}

static void
airoha_ppe_foe_flow_l2_entry_update(struct airoha_ppe *ppe,
				    struct airoha_flow_table_entry *e)
{
	int min_idle = airoha_ppe_get_entry_idle_time(ppe, e->data.ib1);
	struct airoha_flow_table_entry *iter;
	struct hlist_node *n;

	lockdep_assert_held(&ppe_lock);

	hlist_for_each_entry_safe(iter, n, &e->l2_flows, l2_subflow_node) {
		struct airoha_foe_entry *hwe;
		u32 ib1, state;
		int idle;

		hwe = airoha_ppe_foe_get_entry_locked(ppe, iter->hash);
		if (!hwe)
			continue;

		ib1 = READ_ONCE(hwe->ib1);
		state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, ib1);
		if (state != AIROHA_FOE_STATE_BIND) {
			iter->hash = 0xffff;
			airoha_ppe_foe_remove_flow(ppe, iter);
			continue;
		}

		idle = airoha_ppe_get_entry_idle_time(ppe, ib1);
		if (idle >= min_idle)
			continue;

		min_idle = idle;
		e->data.ib1 &= ~AIROHA_FOE_IB1_BIND_TIMESTAMP;
		e->data.ib1 |= ib1 & AIROHA_FOE_IB1_BIND_TIMESTAMP;
	}
}

static void airoha_ppe_foe_flow_entry_update(struct airoha_ppe *ppe,
					     struct airoha_flow_table_entry *e)
{
	struct airoha_foe_entry hwe = {};

	spin_lock_bh(&ppe_lock);

	if (e->type == FLOW_TYPE_L2) {
		airoha_ppe_foe_flow_l2_entry_update(ppe, e);
		goto unlock;
	}

	if (e->hash == 0xffff)
		goto unlock;

	if (airoha_ppe_foe_read_entry_locked(ppe, e->hash, &hwe))
		goto unlock;

	if (!airoha_ppe_foe_compare_entry(e, &hwe)) {
		e->hash = 0xffff;
		goto unlock;
	}

	e->data.ib1 = hwe.ib1;
unlock:
	spin_unlock_bh(&ppe_lock);
}

static int airoha_ppe_entry_idle_time(struct airoha_ppe *ppe,
				      struct airoha_flow_table_entry *e)
{
	airoha_ppe_foe_flow_entry_update(ppe, e);

	return airoha_ppe_get_entry_idle_time(ppe, e->data.ib1);
}

static int airoha_ppe_flow_offload_replace(struct airoha_eth *eth,
					   struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct airoha_flow_table_entry *e;
	struct airoha_flow_data data = {};
	struct net_device *odev = NULL;
	struct flow_action_entry *act;
	u8 l4proto = 0, priority = 0;
	struct airoha_foe_entry hwe;
	int err, i, offload_type;
	u16 addr_type = 0;

	if (rhashtable_lookup(&eth->flow_table, &f->cookie,
			      airoha_flow_table_params))
		return -EEXIST;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
		return -EOPNOTSUPP;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
		if (flow_rule_has_control_flags(match.mask->flags,
						f->common.extack))
			return -EOPNOTSUPP;
	} else {
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		l4proto = match.key->ip_proto;
	} else {
		return -EOPNOTSUPP;
	}

	switch (addr_type) {
	case 0:
		offload_type = PPE_PKT_TYPE_BRIDGE;
		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
			struct flow_match_eth_addrs match;

			flow_rule_match_eth_addrs(rule, &match);
			memcpy(data.eth.h_dest, match.key->dst, ETH_ALEN);
			memcpy(data.eth.h_source, match.key->src, ETH_ALEN);
		} else {
			return -EOPNOTSUPP;
		}
		break;
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		offload_type = PPE_PKT_TYPE_IPV4_HNAPT;
		break;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		offload_type = PPE_PKT_TYPE_IPV6_ROUTE_5T;
		break;
	default:
		return -EOPNOTSUPP;
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (offload_type == PPE_PKT_TYPE_BRIDGE)
				return -EOPNOTSUPP;

			if (act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				airoha_ppe_flow_mangle_eth(act, &data.eth);
			break;
		case FLOW_ACTION_REDIRECT:
			odev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (data.vlan.num == 2 ||
			    act->vlan.proto != htons(ETH_P_8021Q))
				return -EOPNOTSUPP;

			data.vlan.hdr[data.vlan.num].id = act->vlan.vid;
			data.vlan.hdr[data.vlan.num].proto = act->vlan.proto;
			data.vlan.num++;
			break;
		case FLOW_ACTION_VLAN_POP:
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (data.pppoe.num == 1 || data.vlan.num == 2)
				return -EOPNOTSUPP;

			data.pppoe.sid = act->pppoe.sid;
			data.pppoe.num++;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(data.eth.h_source) ||
	    !is_valid_ether_addr(data.eth.h_dest))
		return -EINVAL;

	err = airoha_ppe_foe_entry_prepare(eth, &hwe, odev, offload_type,
					   &data, l4proto, priority);
	if (err)
		return err;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports ports;

		if (offload_type == PPE_PKT_TYPE_BRIDGE)
			return -EOPNOTSUPP;

		flow_rule_match_ports(rule, &ports);
		data.src_port = ports.key->src;
		data.dst_port = ports.key->dst;
	} else if (offload_type != PPE_PKT_TYPE_BRIDGE) {
		return -EOPNOTSUPP;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs addrs;

		flow_rule_match_ipv4_addrs(rule, &addrs);
		data.v4.src_addr = addrs.key->src;
		data.v4.dst_addr = addrs.key->dst;
		airoha_ppe_foe_entry_set_ipv4_tuple(&hwe, &data, false);
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs addrs;

		flow_rule_match_ipv6_addrs(rule, &addrs);

		data.v6.src_addr = addrs.key->src;
		data.v6.dst_addr = addrs.key->dst;
		airoha_ppe_foe_entry_set_ipv6_tuple(&hwe, &data);
	}

	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_MANGLE)
			continue;

		if (offload_type == PPE_PKT_TYPE_BRIDGE)
			return -EOPNOTSUPP;

		switch (act->mangle.htype) {
		case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
		case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
			err = airoha_ppe_flow_mangle_ports(act, &data);
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
			err = airoha_ppe_flow_mangle_ipv4(act, &data);
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
			/* handled earlier */
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (err)
			return err;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		err = airoha_ppe_foe_entry_set_ipv4_tuple(&hwe, &data, true);
		if (err)
			return err;
	}

	e = kzalloc_obj(*e);
	if (!e)
		return -ENOMEM;

	e->cookie = f->cookie;
	memcpy(&e->data, &hwe, sizeof(e->data));

	err = airoha_ppe_foe_flow_commit_entry(eth->ppe, e);
	if (err)
		goto free_entry;

	err = rhashtable_insert_fast(&eth->flow_table, &e->node,
				     airoha_flow_table_params);
	if (err < 0)
		goto remove_foe_entry;

	return 0;

remove_foe_entry:
	airoha_ppe_foe_flow_remove_entry(eth->ppe, e);
free_entry:
	kfree(e);

	return err;
}

static int airoha_ppe_flow_offload_destroy(struct airoha_eth *eth,
					   struct flow_cls_offload *f)
{
	struct airoha_flow_table_entry *e;

	e = rhashtable_lookup(&eth->flow_table, &f->cookie,
			      airoha_flow_table_params);
	if (!e)
		return -ENOENT;

	airoha_ppe_foe_flow_remove_entry(eth->ppe, e);
	rhashtable_remove_fast(&eth->flow_table, &e->node,
			       airoha_flow_table_params);
	kfree(e);

	return 0;
}

void airoha_ppe_foe_entry_get_stats(struct airoha_ppe *ppe, u32 hash,
				    struct airoha_foe_stats64 *stats)
{
	struct airoha_eth *eth = ppe->eth;
	int ppe_num_stats_entries;
	struct airoha_npu *npu;
	u32 index;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries < 0)
		return;

	if (airoha_ppe_foe_get_flow_stats_index(ppe, hash, &index))
		return;

	if (index >= ppe_num_stats_entries)
		return;

	rcu_read_lock();

	npu = rcu_dereference(eth->npu);
	if (npu) {
		struct airoha_foe_stats __iomem *npu_stats_io;

		npu_stats_io = airoha_npu_get_ppe_stats(npu);
		if (npu_stats_io) {
			u64 packets = ppe->foe_stats[index].packets;
			u64 bytes = ppe->foe_stats[index].bytes;
			struct airoha_foe_stats npu_stats;

			memcpy_fromio(&npu_stats, &npu_stats_io[index],
				      sizeof(*npu_stats_io));
			stats->packets = packets << 32 | npu_stats.packets;
			stats->bytes = bytes << 32 | npu_stats.bytes;
		}
	}

	rcu_read_unlock();
}

static int airoha_ppe_flow_offload_stats(struct airoha_eth *eth,
					 struct flow_cls_offload *f)
{
	struct airoha_flow_table_entry *e;
	u32 idle;

	e = rhashtable_lookup(&eth->flow_table, &f->cookie,
			      airoha_flow_table_params);
	if (!e)
		return -ENOENT;

	idle = airoha_ppe_entry_idle_time(eth->ppe, e);
	f->stats.lastused = jiffies - idle * HZ;

	if (e->hash != 0xffff) {
		struct airoha_foe_stats64 stats = {};

		airoha_ppe_foe_entry_get_stats(eth->ppe, e->hash, &stats);
		f->stats.pkts += (stats.packets - e->stats.packets);
		f->stats.bytes += (stats.bytes - e->stats.bytes);
		e->stats = stats;
	}

	return 0;
}

static int airoha_ppe_flow_offload_cmd(struct airoha_eth *eth,
				       struct flow_cls_offload *f)
{
	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return airoha_ppe_flow_offload_replace(eth, f);
	case FLOW_CLS_DESTROY:
		return airoha_ppe_flow_offload_destroy(eth, f);
	case FLOW_CLS_STATS:
		return airoha_ppe_flow_offload_stats(eth, f);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int airoha_ppe_flush_sram_entries(struct airoha_ppe *ppe)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	struct airoha_foe_entry *hwe = ppe->foe;
	int i;

	for (i = 0; i < sram_num_entries; i++) {
		int err;

		memset(&hwe[i], 0, sizeof(*hwe));
		err = airoha_ppe_foe_commit_sram_entry(ppe, i);
		if (err)
			return err;
	}

	return 0;
}

static struct airoha_npu *airoha_ppe_npu_get(struct airoha_eth *eth)
{
	struct airoha_npu *npu = airoha_npu_get(eth->dev);

	if (IS_ERR_OR_NULL(npu)) {
		if (!npu)
			return ERR_PTR(-EOPNOTSUPP);
		request_module("airoha-npu");
		npu = airoha_npu_get(eth->dev);
		if (!npu)
			return ERR_PTR(-EOPNOTSUPP);
	}

	return npu;
}

static int airoha_ppe_wait_for_npu_init(struct airoha_eth *eth)
{
	int err;
	u32 val;

	/* PPE_FLOW_CFG default register value is 0. Since we reset FE
	 * during the device probe we can just check the configured value
	 * is not 0 here.
	 */
	err = read_poll_timeout(airoha_fe_rr, val, val, USEC_PER_MSEC,
				100 * USEC_PER_MSEC, false, eth,
				REG_PPE_PPE_FLOW_CFG(0));
	if (err)
		return err;

	if (airoha_ppe_is_enabled(eth, 1))
		err = read_poll_timeout(airoha_fe_rr, val, val, USEC_PER_MSEC,
					100 * USEC_PER_MSEC, false, eth,
					REG_PPE_PPE_FLOW_CFG(1));

	return err;
}

static int airoha_ppe_offload_setup(struct airoha_eth *eth)
{
	struct airoha_npu *npu = airoha_ppe_npu_get(eth);
	struct airoha_ppe *ppe = eth->ppe;
	int err, ppe_num_stats_entries;

	if (IS_ERR(npu)) {
		/* Run CPU-direct PPE offload when NPU is disabled or absent. */
		if (!ppe->offload_setup_done) {
			airoha_ppe_hw_init(ppe);
			WRITE_ONCE(ppe->offload_setup_done, true);
		}
		return 0;
	}

	err = airoha_npu_init_ppe(npu);
	if (err && err != -EOPNOTSUPP)
		goto error_npu_put;

	if (err != -EOPNOTSUPP) {
		/* Wait for NPU PPE configuration to complete */
		err = airoha_ppe_wait_for_npu_init(eth);
		if (err)
			goto error_npu_put;

		ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
		if (ppe_num_stats_entries > 0) {
			err = airoha_npu_init_ppe_stats(npu,
							ppe->foe_stats_dma,
							ppe_num_stats_entries);
			if (err)
				goto error_npu_put;
		}
	}

	airoha_ppe_hw_init(ppe);
	airoha_ppe_foe_flow_stats_reset(ppe, npu);

	rcu_assign_pointer(eth->npu, npu);
	synchronize_rcu();
	WRITE_ONCE(ppe->offload_setup_done, true);

	return 0;

error_npu_put:
	airoha_npu_put(npu);

	return err;
}

int airoha_ppe_setup_tc_block_cb(struct airoha_ppe_dev *dev, void *type_data)
{
	struct airoha_ppe *ppe = dev->priv;
	struct airoha_eth *eth = ppe->eth;
	int err = 0;

	/* Netfilter flowtable can try to offload flower rules while not all
	 * the net_devices are registered or initialized. Delay offloading
	 * until all net_devices are registered in the system.
	 */
	if (!test_bit(DEV_STATE_REGISTERED, &eth->state))
		return -EBUSY;

	mutex_lock(&flow_offload_mutex);

	if (!eth->npu && !ppe->offload_setup_done)
		err = airoha_ppe_offload_setup(eth);
	if (!err)
		err = airoha_ppe_flow_offload_cmd(eth, type_data);

	mutex_unlock(&flow_offload_mutex);

	return err;
}

void airoha_ppe_check_skb(struct airoha_ppe_dev *dev, struct sk_buff *skb,
			  u16 hash, bool rx_wlan)
{
	struct airoha_ppe *ppe = dev->priv;
	u16 now, diff;
	u32 sw_idx;

	if (airoha_ppe_hw_to_sw_idx(ppe, hash, &sw_idx))
		return;

	now = (u16)jiffies;
	diff = now - ppe->foe_check_time[sw_idx];
	if (diff < HZ / 10)
		return;

	ppe->foe_check_time[sw_idx] = now;
	airoha_ppe_foe_insert_entry(ppe, skb, sw_idx, rx_wlan);
}

void airoha_ppe_init_upd_mem(struct airoha_gdm_dev *dev, const u8 *addr)
{
	struct airoha_gdm_port *port = dev->port;
	struct airoha_eth *eth = dev->eth;
	u32 val;

	val = (addr[2] << 24) | (addr[3] << 16) | (addr[4] << 8) | addr[5];
	airoha_fe_wr(eth, REG_UPDMEM_DATA(0), val);
	airoha_fe_wr(eth, REG_UPDMEM_CTRL(0),
		     FIELD_PREP(PPE_UPDMEM_ADDR_MASK, port->id) |
		     PPE_UPDMEM_WR_MASK | PPE_UPDMEM_REQ_MASK);

	val = (addr[0] << 8) | addr[1];
	airoha_fe_wr(eth, REG_UPDMEM_DATA(0), val);
	airoha_fe_wr(eth, REG_UPDMEM_CTRL(0),
		     FIELD_PREP(PPE_UPDMEM_ADDR_MASK, port->id) |
		     FIELD_PREP(PPE_UPDMEM_OFFSET_MASK, 1) |
		     PPE_UPDMEM_WR_MASK | PPE_UPDMEM_REQ_MASK);
}

struct airoha_ppe_dev *airoha_ppe_get_dev(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct airoha_eth *eth;

	np = of_parse_phandle(dev->of_node, "airoha,eth", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		dev_err(dev, "cannot find device node %s\n", np->name);
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}
	of_node_put(np);

	if (!try_module_get(THIS_MODULE)) {
		dev_err(dev, "failed to get the device driver module\n");
		goto error_pdev_put;
	}

	eth = platform_get_drvdata(pdev);
	if (!eth)
		goto error_module_put;

	if (!device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_SUPPLIER)) {
		dev_err(&pdev->dev,
			"failed to create device link to consumer %s\n",
			dev_name(dev));
		goto error_module_put;
	}

	return &eth->ppe->dev;

error_module_put:
	module_put(THIS_MODULE);
error_pdev_put:
	platform_device_put(pdev);

	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(airoha_ppe_get_dev);

void airoha_ppe_put_dev(struct airoha_ppe_dev *dev)
{
	struct airoha_ppe *ppe = dev->priv;
	struct airoha_eth *eth = ppe->eth;

	module_put(THIS_MODULE);
	put_device(eth->dev);
}
EXPORT_SYMBOL_GPL(airoha_ppe_put_dev);

int airoha_ppe_init(struct airoha_eth *eth)
{
	int foe_size, err, ppe_num_stats_entries;
	u32 ppe_num_entries;
	struct airoha_ppe *ppe;

	ppe = devm_kzalloc_obj(eth->dev, *ppe);
	if (!ppe)
		return -ENOMEM;

	ppe->dev.ops.setup_tc_block_cb = airoha_ppe_setup_tc_block_cb;
	ppe->dev.ops.check_skb = airoha_ppe_check_skb;
	ppe->dev.priv = ppe;
	ppe->eth = eth;
	INIT_HLIST_HEAD(&ppe->pending_flows);
	eth->ppe = ppe;

	ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);
	foe_size = ppe_num_entries * sizeof(struct airoha_foe_entry);
	ppe->foe = dmam_alloc_coherent(eth->dev, foe_size, &ppe->foe_dma,
				       GFP_KERNEL);
	if (!ppe->foe)
		return -ENOMEM;

	ppe->foe_flow = devm_kzalloc(eth->dev,
				     ppe_num_entries * sizeof(*ppe->foe_flow),
				     GFP_KERNEL);
	if (!ppe->foe_flow)
		return -ENOMEM;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries > 0) {
		foe_size = ppe_num_stats_entries * sizeof(*ppe->foe_stats);
		ppe->foe_stats = dmam_alloc_coherent(eth->dev, foe_size,
						     &ppe->foe_stats_dma,
						     GFP_KERNEL);
		if (!ppe->foe_stats)
			return -ENOMEM;
	}

	ppe->foe_check_time = devm_kzalloc(eth->dev,
					   ppe_num_entries * sizeof(*ppe->foe_check_time),
					   GFP_KERNEL);
	if (!ppe->foe_check_time)
		return -ENOMEM;

	err = airoha_ppe_flush_sram_entries(ppe);
	if (err)
		return err;

	err = rhashtable_init(&eth->flow_table, &airoha_flow_table_params);
	if (err)
		return err;

	err = rhashtable_init(&ppe->l2_flows, &airoha_l2_flow_table_params);
	if (err)
		goto error_flow_table_destroy;

	err = airoha_ppe_debugfs_init(ppe);
	if (err)
		goto error_l2_flow_table_destroy;

	return 0;

error_l2_flow_table_destroy:
	rhashtable_destroy(&ppe->l2_flows);
error_flow_table_destroy:
	rhashtable_destroy(&eth->flow_table);

	return err;
}

void airoha_ppe_deinit(struct airoha_eth *eth)
{
	struct airoha_npu *npu;
	int i;

	if (!eth->ppe)
		return;

	for (i = 0; i < eth->soc->num_ppe; i++)
		airoha_fe_clear(eth, REG_PPE_GLO_CFG(i), PPE_GLO_CFG_EN_MASK);

	mutex_lock(&flow_offload_mutex);

	npu = rcu_replace_pointer(eth->npu, NULL,
				  lockdep_is_held(&flow_offload_mutex));
	if (npu) {
		synchronize_rcu();
		airoha_npu_deinit_ppe(npu);
		airoha_npu_put(npu);
	}

	mutex_unlock(&flow_offload_mutex);

	rhashtable_destroy(&eth->ppe->l2_flows);
	rhashtable_destroy(&eth->flow_table);
	debugfs_remove(eth->ppe->debugfs_dir);
}
