// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/ip.h>
#include <linux/if_vlan.h>
#include <linux/ipv6.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rhashtable.h>
#include <net/ipv6.h>
#include <net/flow_offload.h>
#include <net/netlink.h>
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

static int airoha_ppe_v1_flow_offload_replace(struct net_device *dev,
					       struct flow_cls_offload *f);
static int airoha_ppe_v1_flow_offload_destroy(struct net_device *dev,
					       struct flow_cls_offload *f);
static int airoha_ppe_v1_flow_offload_stats(struct flow_cls_offload *f);
static void airoha_ppe_v1_hw_init(struct airoha_ppe *ppe);

static int airoha_ppe_get_num_stats_entries(struct airoha_ppe *ppe)
{
	if (!IS_ENABLED(CONFIG_NET_AIROHA_FLOW_STATS))
		return -EOPNOTSUPP;

	if (airoha_is(ppe->common.eth, airoha_an7583))
		return -EOPNOTSUPP;

	return ppe->common.eth->soc->ppe_stats_entries;
}

static int airoha_ppe_get_total_num_stats_entries(struct airoha_ppe *ppe)
{
	int num_stats = airoha_ppe_get_num_stats_entries(ppe);

	if (num_stats > 0) {
		struct airoha_eth *eth = ppe->common.eth;

		num_stats = num_stats * eth->soc->num_ppe;
	}

	return num_stats;
}

static u32 airoha_ppe_get_total_sram_num_entries(struct airoha_ppe *ppe)
{
	struct airoha_eth *eth = ppe->common.eth;

	return ppe->common.eth->soc->ppe_sram_entries * eth->soc->num_ppe;
}

static u32 airoha_ppe_get_num_entries_shift(u32 entries)
{
	switch (entries) {
	case 256:
		return 6;
	case 512:
		return 7;
	default:
		return __ffs(entries >> 10);
	}
}

static int airoha_ppe_hw_to_sw_idx(struct airoha_ppe *ppe, u32 hw_idx,
				   u32 *sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	u32 dram_num_entries = ppe->common.eth->soc->ppe_dram_entries;
	u32 ppe_num_entries = sram_num_entries + dram_num_entries;

	if (!airoha_is(ppe->common.eth, airoha_en7523)) {
		if (hw_idx >= ppe_num_entries)
			return -ERANGE;

		*sw_idx = hw_idx;
		return 0;
	}

	if (hw_idx < sram_num_entries) {
		*sw_idx = hw_idx;
		return 0;
	}

	if (hw_idx >= ppe->common.eth->soc->ppe_dram_entries &&
	    hw_idx < ppe->common.eth->soc->ppe_dram_entries + dram_num_entries) {
		*sw_idx = sram_num_entries + hw_idx - dram_num_entries;
		return 0;
	}

	return -ERANGE;
}

static u32 airoha_ppe_sw_to_hw_idx(struct airoha_ppe *ppe, u32 sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	if (airoha_is(ppe->common.eth, airoha_en7523) && sw_idx >= sram_num_entries)
		return ppe->common.eth->soc->ppe_dram_entries + sw_idx - sram_num_entries;

	return sw_idx;
}

u32 airoha_ppe_get_total_num_entries(struct airoha_ppe *ppe)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	return sram_num_entries + ppe->common.eth->soc->ppe_dram_entries;
}

bool airoha_ppe_is_enabled(struct airoha_eth *eth, int index)
{
	if (eth->soc->foe_format == AIROHA_FOE_FORMAT_V1)
		return !index && eth->ppe_dev && eth->ppe_dev->enabled;

	if (index >= eth->soc->num_ppe)
		return false;

	return airoha_fe_rr(eth, REG_PPE_GLO_CFG(index)) & PPE_GLO_CFG_EN_MASK;
}

static u32 airoha_ppe_get_timestamp(struct airoha_ppe *ppe)
{
	return airoha_fe_get(ppe->common.eth, REG_FE_FOE_TS,
			     AIROHA_FOE_IB1_BIND_TIMESTAMP);
}

void airoha_ppe_set_cpu_port(struct airoha_gdm_dev *dev, u8 ppe_id, u8 fport)
{
	struct airoha_qdma *qdma = airoha_qdma_deref(dev);
	struct airoha_eth *eth = dev->eth;
	u8 qdma_id = qdma - &eth->qdma[0];
	u32 fe_cpu_port;

	fe_cpu_port = qdma_id ? FE_PSE_PORT_CDM2 : FE_PSE_PORT_CDM1;

	dev_info(eth->dev, "Setting CPU port for PPE %d, fport %d to CDM%d\n", ppe_id, fport, qdma_id ? 2 : 1);
	airoha_fe_rmw(eth, REG_PPE_DFT_CPORT(ppe_id, fport),
		      DFT_CPORT_MASK(fport),
		      __field_prep(DFT_CPORT_MASK(fport), fe_cpu_port));
}

void airoha_ppe_set_mtu(struct airoha_gdm_dev *dev)
{
	struct airoha_gdm_port *port = dev->port;
	struct airoha_eth *eth = dev->eth;
	struct airoha_qdma *qdma;
	int i, ppe_id, index;
	u32 len = 0;

	qdma = airoha_qdma_deref(dev);
	for (i = 0; i < ARRAY_SIZE(port->devs); i++) {
		struct airoha_gdm_dev *d = port->devs[i];
		struct net_device *netdev;
		struct airoha_qdma *q;

		if (!d)
			continue;

		q = airoha_qdma_deref(d);
		if (q != qdma)
			continue;

		netdev = netdev_from_priv(d);
		if (netif_running(netdev))
			len = max_t(u32, len, netdev->mtu);
	}

	/* REG_PPE_MTU limits the egress *L2 frame* length, while netdev->mtu is
	 * only the L3 payload. Without accounting for the Ethernet header (and
	 * any VLAN/PPPoE tags + FCS the egress may carry) a full-size forwarded
	 * frame is one header larger than the programmed MTU, so the PPE bounces
	 * every such packet to the CPU with HIT_BIND_EXCEED_MTU and hw offload
	 * never actually forwards. The vendor SDK programs ~2000 here regardless.
	 */
	if (len)
		len += ETH_HLEN + 2 * VLAN_HLEN + 8 /* PPPoE */ + ETH_FCS_LEN;

	/* On EN7523 the hw-forwarded path may egress on a different FP than
	 * this port index, and hw_init already programs a generous MTU for all
	 * FPs; don't undercut it with the (smaller) per-port L2 size here.
	 */
	if (airoha_is(eth, airoha_en7523))
		len = max_t(u32, len, 2000);

	ppe_id = !airoha_is_lan_gdm_dev(dev) && airoha_ppe_is_enabled(eth, 1);
	index = port->id == AIROHA_GDM4_IDX ? 7 : port->id;

	dev_info(eth->dev, "Setting PPE %d MTU for index %d to %u\n", ppe_id, index, len);
	airoha_fe_rmw(eth, REG_PPE_MTU(ppe_id, index),
		      FP_EGRESS_MTU_MASK(index),
		      __field_prep(FP_EGRESS_MTU_MASK(index), len));
}

static void airoha_ppe_hw_init(struct airoha_ppe *ppe)
{
	u32 sram_ppe_num_data_entries = ppe->common.eth->soc->ppe_sram_entries, sram_num_entries;
	u32 sram_tb_size, dram_num_entries;
	struct airoha_eth *eth = ppe->common.eth;
	int i, sram_num_stats_entries;

	if (eth->soc->foe_format == AIROHA_FOE_FORMAT_V1) {
		airoha_ppe_v1_hw_init(ppe);
		return;
	}

	dev_info(eth->dev, "Initializing PPE Hardware\n");
	sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	sram_tb_size = sram_num_entries * AIROHA_FOE_ENTRY_SIZE;
	dram_num_entries = airoha_ppe_get_num_entries_shift(eth->soc->ppe_dram_entries);

	sram_num_stats_entries = airoha_ppe_get_num_stats_entries(ppe);
	if (sram_num_stats_entries > 0)
		sram_ppe_num_data_entries -= sram_num_stats_entries;
	sram_ppe_num_data_entries =
		airoha_ppe_get_num_entries_shift(sram_ppe_num_data_entries);

	for (i = 0; i < eth->soc->num_ppe; i++) {
		dev_info(eth->dev, "Configuring PPE %d: SRAM entries %u, DRAM entries %u, stats entries %d\n",
			 i, sram_ppe_num_data_entries, dram_num_entries, sram_num_stats_entries);

		airoha_fe_wr(eth, REG_PPE_TB_BASE(i),
			     ppe->common.foe_dma + sram_tb_size);

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

		switch (eth->soc->version) {
		case airoha_en7523:
			/**
			 * the airoha_en7523 support for 64 and 80 bytes, current use 80 bytes for ppe
			 * 0 = 64 Bytes
			 * 1 = 80 Bytes
			 */

			airoha_fe_rmw(eth, REG_PPE_TB_CFG(i),
				      EN7523_PPE_SRAM_TABLE_EN_MASK |
				      EN7523_PPE_SRAM_HASH1_EN_MASK |
				      EN7523_PPE_DRAM_TABLE_EN_MASK |
				      EN7523_PPE_SRAM_HASH0_MODE_MASK |
				      EN7523_PPE_SRAM_HASH1_MODE_MASK |
				      EN7523_PPE_DRAM_HASH0_MODE_MASK,
				      FIELD_PREP(EN7523_PPE_SRAM_TABLE_EN_MASK, 1) |
				      FIELD_PREP(EN7523_PPE_SRAM_HASH1_EN_MASK, 1) |
				      FIELD_PREP(EN7523_PPE_DRAM_TABLE_EN_MASK, 1) |
				      FIELD_PREP(EN7523_PPE_SRAM_HASH0_MODE_MASK, 3) |
				      FIELD_PREP(EN7523_PPE_SRAM_HASH1_MODE_MASK, 1) |
				      FIELD_PREP(EN7523_PPE_DRAM_HASH0_MODE_MASK, 3));

			airoha_fe_rmw(eth, REG_PPE_TB_CFG(i),
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
				      FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
				      FIELD_PREP(PPE_TB_CFG_KEEPALIVE_MASK, 3) |
				      FIELD_PREP(PPE_TB_ENTRY_SIZE_MASK, 1) |
				      PPE_TB_CFG_AGE_TCP_FIN_MASK |
				      PPE_TB_CFG_AGE_UDP_MASK |
				      PPE_TB_CFG_AGE_TCP_MASK |
				      PPE_TB_CFG_AGE_UNBIND_MASK |
				      PPE_TB_CFG_AGE_NON_L4_MASK |
				      PPE_TB_CFG_AGE_PREBIND_MASK |
				      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK, sram_ppe_num_data_entries) |
				      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK, dram_num_entries));

			if (FIELD_GET(PPE_SRAM_TB_NUM_ENTRY_MASK,
				      airoha_fe_rr(eth, REG_PPE_TB_CFG(i))) != sram_ppe_num_data_entries) {
				u32 tb_cfg = airoha_fe_rr(eth, REG_PPE_TB_CFG(i));

				dev_warn(eth->dev,
					 "EN7523 PPE%d SRAM size mismatch: wrote %u, read %lu, TB_CFG=%08x\n",
					 i, sram_ppe_num_data_entries,
					 FIELD_GET(PPE_SRAM_TB_NUM_ENTRY_MASK, tb_cfg),
					 tb_cfg);
			}

			/* Match the working vendor (IOWRT stock) PPE register
			 * setup, dumped via devmem from the EN7523 vendor
			 * firmware (where TB_USED>0, hw-NAT forwards). The generic
			 * mainline EN7581 values leave IPv4_NAPT and IP_PROT
			 * unconfigured so EN7523 never resolves hw-NAT flows.
			 *
			 * The SDK uses 80-byte FoE entries on EN7523. The stock
			 * register dump below has TB_ENTRY_SIZE cleared, so restore
			 * that bit after loading the remaining known-good fields.
			 */
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i), 0x00038743);
			airoha_fe_wr(eth, REG_PPE_PPE_FLOW_CFG(i), 0x06bbf7c0);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i), 0x000f000f);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i) + 0x4, 0x04291106);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i) + 0x8, 0x00003a01);
			airoha_fe_wr(eth, REG_PPE_TB_CFG(i), 0xef403fb4);
			airoha_fe_set(eth, REG_PPE_TB_CFG(i), PPE_TB_ENTRY_SIZE_MASK);
			airoha_fe_wr(eth, REG_PPE_TB_HASH_CFG(i), 0x31003001);
			/* Remaining vendor PPE config registers (dumped from stock
			 * fw): KA, MIRROR, L2 bridge cfg / ethertype enable.
			 * Offsets relative to GLO_CFG (PPE base + 0x200):
			 *   KA=0xE34(+0x34) MIRROR=0xE54(+0x54)
			 *   L2B_CFG=0xE88(+0x88) L2B_ETYPE_EN=0xE8C(+0x8c)
			 */
			airoha_fe_wr(eth, REG_PPE_KEEPALIVE(i),
				     FIELD_PREP(PPE_KEEPALIVE_UDP_MASK, 1) |
				     FIELD_PREP(PPE_KEEPALIVE_TCP_MASK, 1) |
				     FIELD_PREP(PPE_KEEPALIVE_NTU_MASK, 1));
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x54, 0x00000021);
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x88, 0x001d077f);
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x8c, 0x0000001b);
			/* Give every egress forwarding port a generous MTU so a
			 * full-size offloaded frame is never bounced to the CPU
			 * with HIT_BIND_EXCEED_MTU. The hardware-forwarded path may
			 * egress on an FP other than the wan/lan port index that
			 * airoha_ppe_set_mtu programs, so set all of FP0..FP9 here.
			 * REG_PPE_MTU = PPE base + 0x304; +0x00..+0x10 cover FP0..FP9.
			 */
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x00, 0x07d407d0);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x04, 0x07dc07d8);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x08, 0x07e407e0);
			airoha_fe_wr(eth, REG_PPE_MTU_BASE(i) + 0x0c, 0x07f007e8);

			break;
		case airoha_en7581:
		case airoha_an7583:
			airoha_fe_rmw(eth, REG_PPE_TB_HASH_CFG(i),
				      EN7581_PPE_SRAM_TABLE_EN_MASK |
				      EN7581_PPE_SRAM_HASH1_EN_MASK |
				      EN7581_PPE_DRAM_TABLE_EN_MASK |
				      EN7581_PPE_SRAM_HASH0_MODE_MASK |
				      EN7581_PPE_SRAM_HASH1_MODE_MASK |
				      EN7581_PPE_DRAM_HASH0_MODE_MASK |
				      EN7581_PPE_DRAM_HASH1_MODE_MASK,
				      FIELD_PREP(EN7581_PPE_SRAM_TABLE_EN_MASK, 1) |
				      FIELD_PREP(EN7581_PPE_SRAM_HASH1_EN_MASK, 1) |
				      FIELD_PREP(EN7581_PPE_DRAM_TABLE_EN_MASK, 1) |
				      FIELD_PREP(EN7581_PPE_SRAM_HASH0_MODE_MASK, 1) |
				      FIELD_PREP(EN7581_PPE_SRAM_HASH1_MODE_MASK, 1) |
				      FIELD_PREP(EN7581_PPE_DRAM_HASH0_MODE_MASK, 1) |
				      FIELD_PREP(EN7581_PPE_DRAM_HASH1_MODE_MASK, 3));

			airoha_fe_rmw(eth, REG_PPE_TB_CFG(i),
				      PPE_TB_CFG_SEARCH_MISS_MASK |
				      PPE_SRAM_TB_NUM_ENTRY_MASK |
				      PPE_DRAM_TB_NUM_ENTRY_MASK |
				      PPE_TB_CFG_KEEPALIVE_MASK |
				      PPE_TB_ENTRY_SIZE_MASK,
				      FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
				      FIELD_PREP(PPE_TB_CFG_KEEPALIVE_MASK, 3) |
				      FIELD_PREP(PPE_TB_ENTRY_SIZE_MASK, 0) |
				      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK,
						 sram_ppe_num_data_entries) |
				      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK,
						 dram_num_entries));

			airoha_fe_set(eth, REG_PPE_PPE_FLOW_CFG(i),
				      PPE_FLOW_CFG_IP4_NAPT_MASK |
				      PPE_FLOW_CFG_IP4_NAT_MASK |
				      PPE_FLOW_CFG_IP6_3T_ROUTE_MASK |
				      PPE_FLOW_CFG_IP6_5T_ROUTE_MASK |
				      PPE_FLOW_CFG_L2_BRIDGE_MASK);
			break;
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

		/* Enable PPE */
		airoha_fe_set(eth, REG_PPE_GLO_CFG(i), PPE_GLO_CFG_EN_MASK);
	}

	for (i = 0; i < eth->soc->max_gdm_ports; i++) {
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
			fport = eth->ppe_host_ops->get_fe_port(dev);
			airoha_ppe_set_cpu_port(dev, ppe_id, fport);
			airoha_ppe_set_mtu(dev);
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
	struct net_device_path *path;
	int err;

	if (!dev)
		return -ENODEV;

	rcu_read_lock();
	err = dev_fill_forward_path(dev, addr, &stack);
	rcu_read_unlock();
	if (err)
		return err;

	path = &stack.path[stack.num_paths - 1];
	if (path->type != DEV_PATH_MTK_WDMA)
		return -EINVAL;

	info->idx = path->mtk_wdma.wdma_idx;
	info->bss = path->mtk_wdma.bss;
	info->wcid = path->mtk_wdma.wcid;

	return 0;
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

static int
airoha_ppe_xpon_get_tx_info(struct airoha_eth *eth,
			    struct net_device *netdev,
			    const struct airoha_flow_data *data,
			    struct airoha_xpon_tx_info *info)
{
	int i;

	for (i = data->vlan.num - 1; i >= 0; i--) {
		const typeof(data->vlan.hdr[0]) *vlan = &data->vlan.hdr[i];

		if (!eth->ppe_host_ops->xpon_get_tx_info(netdev, true, vlan->id,
							 true, vlan->prio, info))
			return 0;
	}

	return eth->ppe_host_ops->xpon_get_tx_info(netdev, false, 0, false, 0, info);
}

static int airoha_ppe_foe_entry_prepare(struct airoha_eth *eth,
					struct airoha_foe_entry *hwe,
					struct net_device *netdev, int type,
					struct airoha_flow_data *data,
					int l4proto, u8 dsfield)
{
	struct airoha_xpon_tx_info xpon = {};
	u32 qdata = FIELD_PREP(AIROHA_FOE_SHAPER_ID, 0x7f), ports_pad, val;
	int wlan_etype = -EINVAL, dsa_port = airoha_get_dsa_port(&netdev);
	struct airoha_foe_mac_info_common *l2;
	bool xpon_flow = false;
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

	/* EN7523 vendor entries use PORT_AG=0 (account group); mainline's 0x1f
	 * yields ib2=0x3e240 vs the vendor's 0x240 for the same flow.
	 */
	val = FIELD_PREP(AIROHA_FOE_IB2_PORT_AG,
			 airoha_is(eth, airoha_en7523) ? 0x0 : 0x1f);
	if (netdev) {
		struct airoha_wdma_info info = {};

		if (!airoha_ppe_get_wdma_info(netdev, data->eth.h_dest,
					      &info)) {
			val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ, info.idx) |
			       FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT,
					  airoha_is(eth, airoha_en7523) ?
					  FE_PSE_PORT_GDM3 :
					  FE_PSE_PORT_CDM4);
			if (airoha_is(eth, airoha_en7523))
				val |= AIROHA_FOE_IB2_PSE_QOS;
			qdata |= FIELD_PREP(AIROHA_FOE_ACTDP, info.bss);
			wlan_etype = FIELD_PREP(AIROHA_FOE_MAC_WDMA_BAND,
						info.idx) |
				     FIELD_PREP(AIROHA_FOE_MAC_WDMA_WCID,
						info.wcid);
		} else {
			struct airoha_gdm_dev *dev = netdev_priv(netdev);
			struct airoha_gdm_port *port;
			u8 pse_port, channel, priority;

			if (!eth->ppe_host_ops->is_valid_gdm_dev(eth, dev))
				return -EINVAL;

			port = dev->port;
			if (!port) {
				dev_err(eth->dev, "GDM device without parent port during FOE prepare\n");
				return -EINVAL;
			}

			if (airoha_is(eth, airoha_en7523) &&
			    (dev->flags & AIROHA_PRIV_F_XPON_MANAGED) &&
			    dev->xpon_mode == AIROHA_XPON_MODE_GPON) {
				int ret;

				ret = airoha_ppe_xpon_get_tx_info(eth, netdev, data,
								  &xpon);
				if (ret)
					return ret;
				xpon_flow = true;
			}

			if (dsa_port >= 0 || airoha_is_lan_gdm_dev(dev))
				pse_port = port->id == 4 ? FE_PSE_PORT_GDM4
							 : port->id;
			else
				pse_port = 2; /* uplink relies on GDM2
					       * loopback
					       */

			/* GPON hardware forwarding must carry the same T-CONT,
			 * queue and NBOQ metadata as CPU-originated descriptors.
			 */
			if (xpon_flow) {
				channel = xpon.tcont;
				priority = xpon.queue;
			} else {
				channel = dsa_port >= 0 ? dsa_port : port->id;
				channel %= AIROHA_NUM_QOS_CHANNELS;
				priority = rt_tos2priority(dsfield);
				priority %= AIROHA_NUM_QOS_QUEUES;
			}
			qdata |= FIELD_PREP(AIROHA_FOE_CHANNEL, channel) |
				 FIELD_PREP(AIROHA_FOE_QID, priority);

			val |= FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT, pse_port) |
			       FIELD_PREP(AIROHA_FOE_IB2_DSCP, dsfield) |
			       AIROHA_FOE_IB2_PSE_QOS;
			/* For downlink traffic consume SRAM memory for hw
			 * forwarding descriptors queue.
			 */
			/* The vendor SDK defines SUPPORT_FAST_PATH only for
			 * EN7580/EN7581/AN7583.  EN7523 must use the normal
			 * PPE-to-QDMA path; setting FAST_PATH makes a bound FOE
			 * entry consume an unsupported descriptor path and drops
			 * the flow as soon as it transitions to BIND.
			 */
			if (!airoha_is(eth, airoha_en7523) &&
			    airoha_is_lan_gdm_dev(dev))
				val |= AIROHA_FOE_IB2_FAST_PATH;
			if (xpon_flow)
				val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ, xpon.tcont);
			else if (dsa_port >= 0)
				val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ,
						  dsa_port);
			else if (airoha_is(eth, airoha_en7523) &&
				 port->id == AIROHA_GDM3_IDX &&
				 dev->nbq != 0)
				val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ, dev->nbq);

			smac_id = port->id;
			dev_dbg(eth->dev,
				 "foe_prepare: port_id=%d dev_nbq=%d dsa_port=%d ib2=%08x nbq=%lu xpon=%u gem=%u channel=%u queue=%u\n",
				 port->id, dev->nbq, dsa_port, val,
				 FIELD_GET(AIROHA_FOE_IB2_NBQ, val), xpon_flow,
				 xpon.gem_port_id, channel, priority);
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
	} else if (xpon_flow) {
		/* EN7523 reuses the FOE EtherType field as the GPON GEM tag. */
		l2->etype = xpon.gem_port_id;
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
	u32 ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);
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
		return ppe_num_entries - 1;
	}

	hash = (hv1 & hv2) | ((~hv1) & hv3);
	hash = (hash >> 24) | ((hash & 0xffffff) << 8);
	hash ^= hv1 ^ hv2 ^ hv3;
	hash ^= hash >> 16;

	if (airoha_is(ppe->common.eth, airoha_en7523)) {
		u32 dram_entries = ppe->common.eth->soc->ppe_dram_entries;
		u32 sram_entries = airoha_ppe_get_total_sram_num_entries(ppe);

		/* For the EN7523, the hardware maps the main BINDs using a mask on the DRAM.
		 * We masked to the DRAM boundaries and adjusted sw_idx to point to the area after the SRAM.
		 */
		hash &= (dram_entries - 1);
		hash += sram_entries;
	} else {
		/* For EN7581/AN7583, ppe_num_entries is a power of 2,
		 * so the bitwise AND works perfectly across the entire table.
		 */
		hash &= (ppe_num_entries - 1);
	}

	return hash;
}

static int airoha_ppe_foe_get_flow_stats_index(struct airoha_ppe *ppe,
					       u32 sw_idx, u32 *index)
{
	int ppe_num_stats_entries;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries < 0)
		return ppe_num_stats_entries;

	*index = sw_idx >= ppe_num_stats_entries ? sw_idx - ppe->common.eth->soc->ppe_stats_entries
					       : sw_idx;

	return 0;
}

void airoha_ppe_foe_entry_get_stats(struct airoha_ppe *ppe, u32 sw_idx,
				    struct airoha_foe_stats64 *stats)
{
	struct airoha_eth *eth = ppe->common.eth;
	int ppe_num_stats_entries;
	struct airoha_npu *npu;
	u32 index;

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries <= 0)
		return;

	if (airoha_ppe_foe_get_flow_stats_index(ppe, sw_idx, &index))
		return;

	if (index >= ppe_num_stats_entries)
		return;

	rcu_read_lock();

	npu = rcu_dereference(eth->npu);
	if (npu && ppe->foe_stats) {
		u64 packets = ppe->foe_stats[index].packets;
		u64 bytes = ppe->foe_stats[index].bytes;
		struct airoha_foe_stats npu_stats;

		if (!eth->ppe_host_ops || !eth->ppe_host_ops->npu_stats_read)
			goto unlock;
		eth->ppe_host_ops->npu_stats_read(npu, index, &npu_stats);
		stats->packets = packets << 32 | npu_stats.packets;
		stats->bytes = bytes << 32 | npu_stats.bytes;
	}

unlock:
	rcu_read_unlock();
}

static void airoha_ppe_foe_flow_stat_entry_reset(struct airoha_ppe *ppe,
						 struct airoha_npu *npu,
						 int index)
{
	if (ppe->common.eth->ppe_host_ops && ppe->common.eth->ppe_host_ops->npu_stats_clear)
		ppe->common.eth->ppe_host_ops->npu_stats_clear(npu, index);
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
	} else if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
		data = &hwe->ipv6.data;
		ib2 = &hwe->ipv6.ib2;
	} else {
		data = &hwe->ipv4.data;
		ib2 = &hwe->ipv4.ib2;
	}

	/* EN7581/AN7583 keep the meter/accounting word at offset 0x40.
	 * EN7523 has no flow-stats table and returns before reaching here.
	 */
	meter = &hwe->words[0x40 / sizeof(u32)];

	pse_port = FIELD_GET(AIROHA_FOE_IB2_PSE_PORT, *ib2);
	if (pse_port == FE_PSE_PORT_CDM4 ||
	    (airoha_is(ppe->common.eth, airoha_en7523) &&
	     pse_port == FE_PSE_PORT_GDM3))
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

	if (sw_idx < sram_num_entries) {
		u32 hw_idx = airoha_ppe_sw_to_hw_idx(ppe, sw_idx);
		u32 *hwe = ppe->common.foe + sw_idx * AIROHA_FOE_ENTRY_SIZE;
		bool ppe2 = sw_idx >= ppe->common.eth->soc->ppe_sram_entries;
		struct airoha_eth *eth = ppe->common.eth;
		u32 val;
		int i;

		airoha_fe_wr(ppe->common.eth, REG_PPE_RAM_CTRL(ppe2),
			     FIELD_PREP(PPE_SRAM_CTRL_ENTRY_MASK, hw_idx) |
			     PPE_SRAM_CTRL_REQ_MASK);
		if (read_poll_timeout_atomic(airoha_fe_rr, val,
					     val & PPE_SRAM_CTRL_ACK_MASK,
					     10, 100, false, eth,
					     REG_PPE_RAM_CTRL(ppe2))) {
			dev_err(eth->dev, "Timeout reading PPE SRAM entry for hash %u\n", hw_idx);
			return NULL;
		}

		for (i = 0; i < AIROHA_FOE_ENTRY_WORDS; i++)
			hwe[i] = airoha_fe_rr(eth,
					      REG_PPE_RAM_ENTRY(ppe2, i));
	}

	return ppe->common.foe + sw_idx * AIROHA_FOE_ENTRY_SIZE;
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

static bool airoha_ppe_foe_compare_entry(struct airoha_flow_table_entry *e,
					 struct airoha_foe_entry *hwe)
{
	int type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, e->data.ib1);
	int len;

	if ((hwe->ib1 ^ e->data.ib1) & AIROHA_FOE_IB1_BIND_UDP)
		return false;

	if (type > PPE_PKT_TYPE_IPV4_DSLITE)
		len = offsetof(struct airoha_foe_entry, ipv6.data);
	else
		len = offsetof(struct airoha_foe_entry, ipv4.ib2);

	return !memcmp(&e->data.d, &hwe->d, len - sizeof(hwe->ib1));
}

static int airoha_ppe_foe_commit_sram_entry(struct airoha_ppe *ppe, u32 sw_idx)
{
	struct airoha_foe_entry *hwe = ppe->common.foe + sw_idx * AIROHA_FOE_ENTRY_SIZE;
	bool ppe2 = sw_idx >= ppe->common.eth->soc->ppe_sram_entries;
	u32 hw_idx = airoha_ppe_sw_to_hw_idx(ppe, sw_idx);
	u32 *ptr = (u32 *)hwe, val;
	int i, err;

	for (i = 0; i < AIROHA_FOE_ENTRY_WORDS; i++)
		airoha_fe_wr(ppe->common.eth, REG_PPE_RAM_ENTRY(ppe2, i), ptr[i]);

	wmb();
	airoha_fe_wr(ppe->common.eth, REG_PPE_RAM_CTRL(ppe2),
		     FIELD_PREP(PPE_SRAM_CTRL_ENTRY_MASK, hw_idx) |
		     PPE_SRAM_CTRL_WR_MASK | PPE_SRAM_CTRL_REQ_MASK);

	err = read_poll_timeout_atomic(airoha_fe_rr, val,
					val & PPE_SRAM_CTRL_ACK_MASK,
					10, 100, false, ppe->common.eth,
					REG_PPE_RAM_CTRL(ppe2));
	if (err)
		dev_err(ppe->common.eth->dev, "Timeout committing SRAM entry hash %u\n", hw_idx);

	return err;
}

static int airoha_ppe_foe_commit_entry(struct airoha_ppe *ppe,
				       struct airoha_foe_entry *e,
				       u32 sw_idx, bool rx_wlan)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	struct airoha_foe_entry *hwe = ppe->common.foe + sw_idx * AIROHA_FOE_ENTRY_SIZE;
	u32 ts = airoha_ppe_get_timestamp(ppe);
	struct airoha_eth *eth = ppe->common.eth;
	struct airoha_npu *npu;
	int err = 0;

	memcpy(&hwe->words[1], &e->words[1],
	       AIROHA_FOE_ENTRY_SIZE - sizeof(hwe->words[0]));
	wmb();

	e->ib1 &= ~AIROHA_FOE_IB1_BIND_TIMESTAMP;
	e->ib1 |= FIELD_PREP(AIROHA_FOE_IB1_BIND_TIMESTAMP, ts);
	hwe->ib1 = e->ib1;

	rcu_read_lock();

	npu = rcu_dereference(eth->npu);
	if (!rx_wlan && npu)
		airoha_ppe_foe_flow_stats_update(ppe, npu, hwe, sw_idx);
	else if (!npu)
		dev_dbg(eth->dev, "NPU not attached, skipping FOE stats setup for entry %u\n",
			sw_idx);

	/* FOE programming must not depend on the optional NPU/stats path.
	 * EN7523 systems can run with no attached NPU; in that case DRAM entries
	 * are already written above through the coherent FOE table, while SRAM
	 * entries still need the explicit SRAM_CTRL write below.
	 */
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
	struct airoha_foe_entry *hwe_p, hwe;
	struct airoha_flow_table_entry *f;
	int type;

	hwe_p = airoha_ppe_foe_get_entry_locked(ppe, sw_idx);
	if (!hwe_p) {
		dev_err(ppe->common.eth->dev,
			"Failed to get locked entry for subflow commit (hash: %u)\n",
			sw_idx);
		return -EINVAL;
	}

	f = kzalloc(sizeof(*f), GFP_ATOMIC);
	if (!f) {
		dev_err(ppe->common.eth->dev, "OOM in subflow commit\n");
		return -ENOMEM;
	}

	hlist_add_head(&f->l2_subflow_node, &e->l2_flows);
	f->type = FLOW_TYPE_L2_SUBFLOW;
	f->hash = sw_idx;

	memcpy(&hwe, hwe_p, sizeof(*hwe_p));
	hwe.ib1 = (hwe.ib1 & mask) | (e->data.ib1 & ~mask);

	type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe.ib1);
	if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
		memcpy(&hwe.ipv6.l2, &e->data.bridge.l2, sizeof(hwe.ipv6.l2));
		hwe.ipv6.ib2 = e->data.bridge.ib2;
		hwe.ipv6.l2.src_mac_hi = FIELD_PREP(AIROHA_FOE_MAC_SMAC_ID,
						    0xf);
	} else {
		memcpy(&hwe.bridge.l2, &e->data.bridge.l2,
		       sizeof(hwe.bridge.l2));
		hwe.bridge.ib2 = e->data.bridge.ib2;
		if (type == PPE_PKT_TYPE_IPV4_HNAPT)
			memcpy(&hwe.ipv4.new_tuple, &hwe.ipv4.orig_tuple,
			       sizeof(hwe.ipv4.new_tuple));
	}

	hwe.bridge.data = e->data.bridge.data;
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

	/* payload_len can be zero for jumbograms. */
	if (ip6h->payload_len &&
	    off + sizeof(*ip6h) + ntohs(ip6h->payload_len) > skb->len)
		return false;

	nexthdr = ip6h->nexthdr;
	l4_off = off + sizeof(*ip6h);

	/* Traverse a bounded number of IPv6 extension headers. */
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

			/* Only the first fragment contains the L4 header. */
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

	/* skb->protocol is not reliable at this point on EN7523. */
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

	/* The packet can still carry a switch/special tag before L3. */
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
	struct airoha_foe_entry *hwe;
	bool commit_done = false;
	struct hlist_node *n;
	u32 index, state;

	spin_lock_bh(&ppe_lock);

	hwe = airoha_ppe_foe_get_entry_locked(ppe, sw_idx);
	if (!hwe)
		goto unlock;

	state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, hwe->ib1);
	if (state == AIROHA_FOE_STATE_BIND)
		goto unlock;

	if (airoha_is(ppe->common.eth, airoha_en7523)) {
		/* On EN7523 the offloaded flows live in ppe->pending_flows (the
		 * SW hash does not match the HW hash), so they are matched by
		 * skb tuple rather than via foe_flow[index]. The HW may present
		 * the slot either as INVALID (FOE_UNHIT) or as an auto-created
		 * UNBIND entry (HIT_UNBIND, when SRAM auto-learn is active);
		 * both must drive the same UNBIND/INVALID -> BIND commit.
		 */
		e = airoha_ppe_find_pending_flow_by_skb(ppe, skb);
		if (e) {
			int err;

			err = airoha_ppe_foe_commit_entry(ppe, &e->data,
							  sw_idx, rx_wlan);
			if (!err) {
				hlist_del_init(&e->list);
				hlist_add_head(&e->list,
					       &ppe->foe_flow[sw_idx]);
				e->hash = sw_idx;
			}
		}

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

	if (IS_ERR(prev)) {
		dev_err(ppe->common.eth->dev,
			"Error inserting L2 flow to rhashtable: %ld\n",
			PTR_ERR(prev));
		return PTR_ERR(prev);
	}

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
	if (airoha_is(ppe->common.eth, airoha_en7523))
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
	struct airoha_foe_entry *hwe_p, hwe = {};

	spin_lock_bh(&ppe_lock);

	if (e->type == FLOW_TYPE_L2) {
		airoha_ppe_foe_flow_l2_entry_update(ppe, e);
		goto unlock;
	}

	if (e->hash == 0xffff)
		goto unlock;

	hwe_p = airoha_ppe_foe_get_entry_locked(ppe, e->hash);
	if (!hwe_p)
		goto unlock;

	memcpy(&hwe, hwe_p, sizeof(*hwe_p));
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
	struct airoha_foe_entry hwe;
	u8 dsfield = 0, l4proto = 0;
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

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);
		dsfield = match.key->tos;
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
			data.vlan.hdr[data.vlan.num].prio = act->vlan.prio;
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
	    !is_valid_ether_addr(data.eth.h_dest)) {
		dev_err(eth->dev, "Invalid ether addr on flow replace\n");
		return -EINVAL;
	}

	err = airoha_ppe_foe_entry_prepare(eth, &hwe, odev, offload_type,
					   &data, l4proto, dsfield);
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

		if (err) {
			dev_err(eth->dev, "Failed to mangle flow (err: %d)\n", err);
			return err;
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		err = airoha_ppe_foe_entry_set_ipv4_tuple(&hwe, &data, true);
		if (err)
			return err;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		dev_err(eth->dev, "OOM allocating flow table entry\n");
		return -ENOMEM;
	}

	e->cookie = f->cookie;
	memcpy(&e->data, &hwe, sizeof(e->data));

	err = airoha_ppe_foe_flow_commit_entry(eth->ppe, e);
	if (err) {
		dev_err(eth->dev, "Failed to commit PPE flow entry (err: %d)\n", err);
		goto free_entry;
	}

	err = rhashtable_insert_fast(&eth->flow_table, &e->node,
				     airoha_flow_table_params);
	if (err < 0) {
		dev_err(eth->dev, "Failed to insert into rhashtable (err: %d)\n", err);
		goto remove_foe_entry;
	}

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
				       struct net_device *dev,
				       struct flow_cls_offload *f)
{
	bool v1 = eth->soc->foe_format == AIROHA_FOE_FORMAT_V1;

	if (v1 && !dev)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return v1 ? airoha_ppe_v1_flow_offload_replace(dev, f) :
			    airoha_ppe_flow_offload_replace(eth, f);
	case FLOW_CLS_DESTROY:
		return v1 ? airoha_ppe_v1_flow_offload_destroy(dev, f) :
			    airoha_ppe_flow_offload_destroy(eth, f);
	case FLOW_CLS_STATS:
		return v1 ? airoha_ppe_v1_flow_offload_stats(f) :
			    airoha_ppe_flow_offload_stats(eth, f);
	default:
		return -EOPNOTSUPP;
	}
}

static int airoha_ppe_flush_sram_entries(struct airoha_ppe *ppe)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	struct airoha_foe_entry *hwe = ppe->common.foe;
	int i, err = 0;

	dev_info(ppe->common.eth->dev, "Flushing %u SRAM entries\n", sram_num_entries);
	for (i = 0; i < sram_num_entries; i++) {
		memset(&hwe[i], 0, sizeof(*hwe));
		err = airoha_ppe_foe_commit_sram_entry(ppe, i);
		if (err) {
			dev_err(ppe->common.eth->dev,
				"failed to flush SRAM entry %d, err %d\n",
				i, err);
			break;
		}
	}

	return err;
}

static struct airoha_npu *airoha_ppe_npu_get(struct airoha_eth *eth)
{
	struct airoha_npu *npu;

	if (!eth->ppe_host_ops || !eth->ppe_host_ops->npu_get)
		return ERR_PTR(-EOPNOTSUPP);

	npu = eth->ppe_host_ops->npu_get(eth);
	if (!npu)
		npu = ERR_PTR(-EOPNOTSUPP);

	if (IS_ERR(npu) && PTR_ERR(npu) != -EOPNOTSUPP) {
		dev_info(eth->dev, "Requesting airoha-npu module\n");
		request_module("airoha-npu");
		npu = eth->ppe_host_ops->npu_get(eth);
		if (!npu)
			npu = ERR_PTR(-EOPNOTSUPP);
	}

	if (IS_ERR(npu))
		dev_err(eth->dev, "Failed to get NPU module\n");

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
	dev_info(eth->dev, "Waiting for NPU init on PPE 0\n");
	err = read_poll_timeout(airoha_fe_rr, val, val, USEC_PER_MSEC,
				100 * USEC_PER_MSEC, false, eth,
				REG_PPE_PPE_FLOW_CFG(0));
	if (err) {
		dev_err(eth->dev, "Timeout waiting for NPU initialization on PPE 0\n");
		return err;
	}

	if (airoha_ppe_is_enabled(eth, 1)) {
		dev_info(eth->dev, "Waiting for NPU init on PPE 1\n");
		err = read_poll_timeout(airoha_fe_rr, val, val, USEC_PER_MSEC,
					100 * USEC_PER_MSEC, false, eth,
					REG_PPE_PPE_FLOW_CFG(1));
		if (err)
			dev_err(eth->dev, "Timeout waiting for NPU initialization on PPE 1\n");
	}

	return err;
}

static int airoha_ppe_offload_setup(struct airoha_eth *eth)
{
	struct airoha_npu *npu = airoha_ppe_npu_get(eth);
	struct airoha_ppe *ppe = eth->ppe;
	int err, ppe_num_stats_entries;

	dev_info(eth->dev, "Setting up PPE offload\n");
	if (IS_ERR(npu)) {
		/* NPU disabled / unavailable (e.g. EN7523 with no NPU firmware).
		 * Run CPU-direct PPE offload: the FOE commit path already
		 * supports the no-NPU case. Do the HW init once and leave
		 * eth->npu NULL so the commit/stats paths skip the NPU.
		 */
		if (!ppe->offload_setup_done) {
			airoha_ppe_hw_init(ppe);
			ppe->offload_setup_done = true;
			dev_info(eth->dev,
				 "PPE offload setup completed (no NPU, CPU-direct)\n");
		}
		return 0;
	}

	err = eth->ppe_host_ops->npu_ppe_init(npu);
	if (err && err != -EOPNOTSUPP) {
		dev_err(eth->dev, "NPU PPE init failed (err: %d)\n", err);
		goto error_npu_put;
	}

	if (err != -EOPNOTSUPP) {
		err = airoha_ppe_wait_for_npu_init(eth);
		if (err)
			goto error_npu_put;

		ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
		if (ppe_num_stats_entries > 0) {
			err = eth->ppe_host_ops->npu_ppe_init_stats(npu,
						      ppe->foe_stats_dma,
						      ppe_num_stats_entries);
			if (err) {
				dev_err(eth->dev, "NPU PPE stats init failed (err: %d)\n", err);
				goto error_npu_put;
			}
		}
	}

	airoha_ppe_hw_init(ppe);
	airoha_ppe_foe_flow_stats_reset(ppe, npu);

	rcu_assign_pointer(eth->npu, npu);
	synchronize_rcu();

	dev_info(eth->dev, "PPE offload setup completed successfully\n");
	return 0;

error_npu_put:
	eth->ppe_host_ops->npu_put(npu);

	return err;
}

int airoha_ppe_setup_tc_block_cb(struct airoha_ppe_dev *dev, void *type_data)
{
	struct airoha_ppe *ppe = dev->priv;
	struct airoha_eth *eth = ppe->common.eth;
	int err = 0;

	/* Netfilter flowtable can try to offload flower rules while not all
	 * the net_devices are registered or initialized. Delay offloading
	 * until all net_devices are registered in the system.
	 */
	if (!test_bit(DEV_STATE_REGISTERED, &eth->state))
		return -EBUSY;

	mutex_lock(&flow_offload_mutex);

	if (!eth->npu && !eth->ppe->offload_setup_done) {
		dev_info(eth->dev, "NPU not attached, setting up offload\n");
		err = airoha_ppe_offload_setup(eth);
	}
	if (!err)
		err = airoha_ppe_flow_offload_cmd(eth, NULL, type_data);

	mutex_unlock(&flow_offload_mutex);

	return err;
}

void airoha_ppe_check_skb(struct airoha_ppe_dev *dev, struct sk_buff *skb,
			  u16 hash, bool rx_wlan)
{
	struct airoha_ppe *ppe = dev->priv;
	u32 sw_idx;
	u16 now, diff;

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

	dev_info(eth->dev, "Initializing UPD mem for port id %d\n", port->id);

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
	if (!eth) {
		dev_err(dev, "failed to get platform drvdata for airoha_eth\n");
		goto error_module_put;
	}

	if (!device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_SUPPLIER)) {
		dev_err(&pdev->dev,
			"failed to create device link to consumer %s\n",
			dev_name(dev));
		goto error_module_put;
	}

	if (!eth->ppe_dev) {
		dev_err(dev, "Ethernet PPE is not available\n");
		goto error_module_put;
	}

	dev_info(dev, "Successfully retrieved PPE device\n");
	return eth->ppe_dev;

error_module_put:
	module_put(THIS_MODULE);
error_pdev_put:
	platform_device_put(pdev);

	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(airoha_ppe_get_dev);

void airoha_ppe_put_dev(struct airoha_ppe_dev *dev)
{
	module_put(THIS_MODULE);
	if (dev->parent)
		put_device(dev->parent);
}
EXPORT_SYMBOL_GPL(airoha_ppe_put_dev);

static void airoha_ppe_common_init(struct airoha_ppe_common *common,
				   struct airoha_eth *eth, void *priv)
{
	common->eth = eth;
	common->dev.priv = priv;
	common->dev.parent = eth->dev;
}

static int airoha_ppe_common_alloc_foe(struct airoha_ppe_common *common,
				       size_t size)
{
	struct device *dev = common->eth->dev;

	common->foe = dmam_alloc_coherent(dev, size, &common->foe_dma, GFP_KERNEL);
	if (!common->foe)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate %zu bytes for PPE FoE table\n",
				     size);

	return 0;
}

static void airoha_ppe_common_enable(struct airoha_ppe_common *common)
{
	common->dev.enabled = true;
	common->eth->ppe_dev = &common->dev;
}

static void airoha_ppe_common_disable(struct airoha_ppe_common *common)
{
	debugfs_remove_recursive(common->debugfs_dir);
	common->debugfs_dir = NULL;
	common->dev.enabled = false;
	if (common->eth->ppe_dev == &common->dev)
		common->eth->ppe_dev = NULL;
}

static int airoha_ppe_datapath_init(struct airoha_eth *eth)
{
	int foe_size, err, ppe_num_stats_entries;
	u32 ppe_num_entries;
	struct airoha_ppe *ppe;

	dev_info(eth->dev, "Starting PPE Initialization\n");
	ppe = devm_kzalloc(eth->dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe) {
		dev_err(eth->dev, "OOM allocating struct airoha_ppe\n");
		return -ENOMEM;
	}

	airoha_ppe_common_init(&ppe->common, eth, ppe);
	ppe->common.dev.ops.setup_tc_block_cb = airoha_ppe_setup_tc_block_cb;
	ppe->common.dev.ops.check_skb = airoha_ppe_check_skb;
	INIT_LIST_HEAD(&ppe->block_cb_list);
	INIT_HLIST_HEAD(&ppe->pending_flows);

	ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);
	foe_size = ppe_num_entries * AIROHA_FOE_ENTRY_SIZE;
	err = airoha_ppe_common_alloc_foe(&ppe->common, foe_size);
	if (err)
		return err;

	ppe->foe_flow = devm_kzalloc(eth->dev,
				     ppe_num_entries * sizeof(*ppe->foe_flow),
				     GFP_KERNEL);
	if (!ppe->foe_flow) {
		dev_err(eth->dev, "OOM allocating FOE flows array\n");
		return -ENOMEM;
	}

	ppe_num_stats_entries = airoha_ppe_get_total_num_stats_entries(ppe);
	if (ppe_num_stats_entries > 0) {
		foe_size = ppe_num_stats_entries * sizeof(*ppe->foe_stats);
		ppe->foe_stats = dmam_alloc_coherent(eth->dev, foe_size,
						     &ppe->foe_stats_dma,
						     GFP_KERNEL);
		if (!ppe->foe_stats) {
			dev_err(eth->dev, "Failed to allocate DMA coherent memory for FOE stats\n");
			return -ENOMEM;
		}
	}

	ppe->foe_check_time = devm_kcalloc(eth->dev, ppe_num_entries,
					   sizeof(*ppe->foe_check_time), GFP_KERNEL);
	if (!ppe->foe_check_time) {
		dev_err(eth->dev, "OOM allocating FOE check time array\n");
		return -ENOMEM;
	}

	err = airoha_ppe_flush_sram_entries(ppe);
	if (err) {
		dev_err(eth->dev, "Failed to flush SRAM entries during init (err: %d)\n", err);
		return err;
	}

	err = rhashtable_init(&eth->flow_table, &airoha_flow_table_params);
	if (err) {
		dev_err(eth->dev, "Failed to initialize global flow rhashtable (err: %d)\n", err);
		return err;
	}

	err = rhashtable_init(&ppe->l2_flows, &airoha_l2_flow_table_params);
	if (err) {
		dev_err(eth->dev, "Failed to initialize L2 flow rhashtable (err: %d)\n", err);
		goto error_flow_table_destroy;
	}

	err = airoha_ppe_debugfs_init(&ppe->common);
	if (err) {
		dev_err(eth->dev, "Failed to init PPE debugfs (err: %d)\n", err);
		goto error_l2_flow_table_destroy;
	}

	eth->ppe = ppe;
	airoha_ppe_common_enable(&ppe->common);
	dev_info(eth->dev, "PPE Initialization completed successfully\n");
	return 0;

error_l2_flow_table_destroy:
	rhashtable_destroy(&ppe->l2_flows);
error_flow_table_destroy:
	rhashtable_destroy(&eth->flow_table);

	return err;
}

static void airoha_ppe_datapath_deinit(struct airoha_eth *eth)
{
	struct airoha_npu *npu;

	mutex_lock(&flow_offload_mutex);

	npu = rcu_replace_pointer(eth->npu, NULL,
				  lockdep_is_held(&flow_offload_mutex));
	if (npu) {
		synchronize_rcu();
		if (eth->ppe_host_ops && eth->ppe_host_ops->npu_ppe_deinit)
			eth->ppe_host_ops->npu_ppe_deinit(npu);
		if (eth->ppe_host_ops && eth->ppe_host_ops->npu_put)
			eth->ppe_host_ops->npu_put(npu);
	}

	mutex_unlock(&flow_offload_mutex);

	airoha_ppe_common_disable(&eth->ppe->common);
	rhashtable_destroy(&eth->ppe->l2_flows);
	rhashtable_destroy(&eth->flow_table);
	eth->ppe = NULL;
}

/* FoE v1 backend shared by EN751221 and EN7528. */

static size_t airoha_ppe_v1_foe_size(struct airoha_eth *eth)
{
	return (size_t)eth->soc->ppe_dram_entries * AIROHA_FOE_ENTRY_SIZE;
}

static u32 *airoha_ppe_v1_slot(struct airoha_ppe *ppe, u16 hash)
{
	return (u32 *)((u8 *)ppe->common.foe +
			 (size_t)hash * AIROHA_FOE_ENTRY_SIZE);
}

void airoha_ppe_v1_read_entry(struct airoha_ppe *ppe, u16 hash,
			   struct airoha_foe_entry *entry)
{
	u32 *slot = airoha_ppe_v1_slot(ppe, hash);
	int i;

	dma_rmb();
	for (i = 0; i < AIROHA_FOE_ENTRY_WORDS; i++)
		entry->words[i] = READ_ONCE(slot[i]);
}

static bool airoha_ppe_v1_cache_cmd(struct airoha_ppe *ppe, u32 cmd)
{
	void __iomem *reg = ppe->common.eth->fe_regs + REG_EN751221_PPE_CACHE_CTL;
	u32 val;

	writel(FIELD_PREP(EN751221_PPE_CACHE_CTL_CMD, cmd) |
	       EN751221_PPE_CACHE_CTL_REQ, reg);
	return !readl_poll_timeout_atomic(reg, val,
					 !(val & EN751221_PPE_CACHE_CTL_REQ),
					 1, 100000);
}

static void airoha_ppe_v1_cache_clean(struct airoha_ppe *ppe)
{
	u32 gate = airoha_fe_rr(ppe->common.eth, REG_EN751221_PPE_CAH_GATE);

	if (!(gate & EN751221_PPE_CAH_GATE_EN)) {
		airoha_fe_wr(ppe->common.eth, REG_EN751221_PPE_CAH_GATE,
			     EN751221_PPE_CAH_GATE_DEFAULT);
		gate = airoha_fe_rr(ppe->common.eth, REG_EN751221_PPE_CAH_GATE);
	}

	airoha_fe_wr(ppe->common.eth, REG_EN751221_PPE_CAH_GATE,
		     gate & ~EN751221_PPE_CAH_GATE_EN);
	if (!airoha_ppe_v1_cache_cmd(ppe, 4))
		dev_warn(ppe->common.eth->dev,
			 "PPE cache clear-all timed out\n");
	airoha_fe_wr(ppe->common.eth, REG_EN751221_PPE_CAH_GATE,
		     gate | EN751221_PPE_CAH_GATE_EN);
}

static u32 *airoha_foe_v1_ib2(struct airoha_foe_entry *entry)
{
	return &entry->words[airoha_foe_v1_ib2_word(entry)];
}

static u32 *airoha_foe_v1_data(struct airoha_foe_entry *entry)
{
	return &entry->words[airoha_foe_v1_data_word(entry)];
}

static u32 *airoha_foe_v1_l2(struct airoha_foe_entry *entry)
{
	return &entry->words[airoha_foe_v1_l2_word(entry)];
}

static void airoha_foe_v1_l2_set(struct airoha_foe_entry *entry,
				 unsigned int word, u32 mask, u32 val)
{
	u32 *l2 = airoha_foe_v1_l2(entry);

	l2[word] &= ~mask;
	l2[word] |= FIELD_PREP(mask, val);
}

static void airoha_foe_v1_entry_prepare(struct airoha_foe_entry *entry,
					u8 l4proto, u8 pkt_type,
					u8 pse_port, const u8 *src_mac,
					const u8 *dest_mac)
{
	u32 *ib2, *l2;

	memset(entry, 0, sizeof(*entry));
	entry->ib1 = FIELD_PREP(AIROHA_FOE_IB1_BIND_STATE,
				AIROHA_FOE_STATE_BIND) |
		     FIELD_PREP(AIROHA_FOE_IB1_BIND_PACKET_TYPE, pkt_type) |
		     FIELD_PREP(AIROHA_FOE_IB1_BIND_UDP,
				l4proto == IPPROTO_UDP) |
		     AIROHA_FOE_V1_IB1_BIND_CACHE | AIROHA_FOE_IB1_BIND_TTL;

	ib2 = airoha_foe_v1_ib2(entry);
	*ib2 = FIELD_PREP(AIROHA_FOE_V1_IB2_PSE_PORT, pse_port) |
	       FIELD_PREP(AIROHA_FOE_V1_IB2_PORT_MG, 0x3f) |
	       FIELD_PREP(AIROHA_FOE_V1_IB2_PORT_AG, 0x3f);
	if (is_multicast_ether_addr(dest_mac))
		*ib2 |= AIROHA_FOE_V1_IB2_MULTICAST;

	l2 = airoha_foe_v1_l2(entry);
	l2[0] = FIELD_PREP(AIROHA_FOE_L2_ETYPE,
			   pkt_type == PPE_PKT_TYPE_IPV6_ROUTE_5T ?
			   ETH_P_IPV6 : ETH_P_IP);
	l2[1] = get_unaligned_be32(dest_mac);
	l2[2] = FIELD_PREP(AIROHA_FOE_L2_DMAC_LO,
			   get_unaligned_be16(dest_mac + 4));
	l2[3] = get_unaligned_be32(src_mac);
	l2[4] = FIELD_PREP(AIROHA_FOE_L2_SMAC_LO,
			   get_unaligned_be16(src_mac + 4));
}

static void airoha_foe_v1_entry_set_ipv4_tuple(struct airoha_foe_entry *entry,
						bool egress, __be32 src_addr,
						__be16 src_port,
						__be32 dest_addr,
						__be16 dest_port)
{
	unsigned int base = egress ? 5 : 1;

	entry->words[base] = be32_to_cpu(src_addr);
	entry->words[base + 1] = be32_to_cpu(dest_addr);
	entry->words[base + 2] =
		FIELD_PREP(AIROHA_FOE_PORTS_SPORT, be16_to_cpu(src_port)) |
		FIELD_PREP(AIROHA_FOE_PORTS_DPORT, be16_to_cpu(dest_port));
}

static void airoha_foe_v1_entry_set_ipv6_tuple(struct airoha_foe_entry *entry,
						const struct in6_addr *src_addr,
						__be16 src_port,
						const struct in6_addr *dest_addr,
						__be16 dest_port)
{
	int i;

	for (i = 0; i < 4; i++) {
		entry->words[1 + i] = be32_to_cpu(src_addr->s6_addr32[i]);
		entry->words[5 + i] = be32_to_cpu(dest_addr->s6_addr32[i]);
	}
	entry->words[9] =
		FIELD_PREP(AIROHA_FOE_PORTS_SPORT, be16_to_cpu(src_port)) |
		FIELD_PREP(AIROHA_FOE_PORTS_DPORT, be16_to_cpu(dest_port));
}

static void airoha_foe_v1_entry_set_pse_port(struct airoha_foe_entry *entry,
					      u8 port)
{
	u32 *ib2 = airoha_foe_v1_ib2(entry);

	*ib2 &= ~(AIROHA_FOE_V1_IB2_PSE_PORT | AIROHA_FOE_V1_IB2_PSE_QOS);
	*ib2 |= FIELD_PREP(AIROHA_FOE_V1_IB2_PSE_PORT, port);
	if (port == AIROHA_FOE_V1_FP_QDMA_HW)
		*ib2 |= AIROHA_FOE_V1_IB2_PSE_QOS;
}

static void airoha_foe_v1_entry_set_queue(struct airoha_foe_entry *entry,
					   u8 queue)
{
	u32 *ib2 = airoha_foe_v1_ib2(entry);

	*ib2 &= ~AIROHA_FOE_V1_IB2_QID;
	*ib2 |= FIELD_PREP(AIROHA_FOE_V1_IB2_QID, queue);
}

static int airoha_foe_v1_entry_set_xpon(struct airoha_foe_entry *entry,
					 struct net_device *odev,
					 bool vlan_valid, u16 vlan_id,
					 bool pcp_valid, u8 pcp)
{
	struct airoha_xpon_tx_info info = {};
	u32 *data;
	int err;

	err = airoha_eth_xpon_get_tx_info(odev, vlan_valid, vlan_id,
					  pcp_valid, pcp, &info);
	if (err)
		return err;
	if (info.oam || info.gem_port_id > 0xfff || info.tcont >= 32 ||
	    info.queue >= 8)
		return -EOPNOTSUPP;

	airoha_foe_v1_entry_set_pse_port(entry, AIROHA_FOE_V1_FP_QDMA_HW);
	airoha_foe_v1_entry_set_queue(entry, info.queue);
	data = airoha_foe_v1_data(entry);
	*data &= ~(AIROHA_FOE_V1_SHAPER_ID | AIROHA_FOE_V1_CHANNEL);
	*data |= FIELD_PREP(AIROHA_FOE_V1_CHANNEL, info.tcont);
	airoha_foe_v1_l2_set(entry, 0, AIROHA_FOE_L2_ETYPE,
			     info.gem_port_id);

	return 0;
}

static int airoha_foe_v1_entry_set_vlan(struct airoha_foe_entry *entry,
					u16 vid, u8 prio, __be16 proto)
{
	if (proto != htons(ETH_P_8021Q) || vid > VLAN_VID_MASK || prio > 7)
		return -EOPNOTSUPP;

	entry->ib1 &= ~(AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER |
			AIROHA_FOE_IB1_BIND_VPM);
	entry->ib1 |= FIELD_PREP(AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER, 1) |
		      FIELD_PREP(AIROHA_FOE_IB1_BIND_VPM, 1);
	airoha_foe_v1_l2_set(entry, 0, AIROHA_FOE_L2_VLAN1,
			     FIELD_PREP(VLAN_PRIO_MASK, prio) | vid);

	return 0;
}

static void airoha_foe_v1_entry_set_dsa(struct airoha_foe_entry *entry,
					 int port, bool passthrough)
{
	u32 etype = BIT(port) & GENMASK(5, 0);

	if (passthrough)
		etype |= BIT(7);
	if (!(entry->ib1 & AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER))
		entry->ib1 |= FIELD_PREP(AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER, 1);
	else
		etype |= BIT(8);

	airoha_foe_v1_l2_set(entry, 0, AIROHA_FOE_L2_ETYPE, etype);
	airoha_foe_v1_l2_set(entry, 0, AIROHA_FOE_L2_VLAN1, 0);
	entry->ib1 &= ~AIROHA_FOE_V1_IB1_BIND_VLAN_TAG;
}

static void airoha_foe_v1_entry_set_pppoe(struct airoha_foe_entry *entry,
					   u16 sid)
{
	if (!(entry->ib1 & AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER) ||
	    (entry->ib1 & AIROHA_FOE_V1_IB1_BIND_VLAN_TAG))
		airoha_foe_v1_l2_set(entry, 0, AIROHA_FOE_L2_ETYPE,
				     ETH_P_PPP_SES);
	entry->ib1 |= AIROHA_FOE_V1_IB1_BIND_PPPOE;
	airoha_foe_v1_l2_set(entry, 4, AIROHA_FOE_L2_PPPOE_ID, sid);
}

static void airoha_foe_v1_entry_set_bind_metadata(struct airoha_foe_entry *entry)
{
	u32 *data = airoha_foe_v1_data(entry);
	u32 *ib2 = airoha_foe_v1_ib2(entry);
	u8 port = FIELD_GET(AIROHA_FOE_V1_IB2_PSE_PORT, *ib2);

	entry->ib1 &= ~AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER;
	entry->ib1 |= FIELD_PREP(AIROHA_FOE_V1_IB1_BIND_VLAN_LAYER, 1);

	*data &= ~AIROHA_FOE_V1_ACTDP;
	if (port != AIROHA_FOE_V1_FP_QDMA_HW)
		*data |= FIELD_PREP(AIROHA_FOE_V1_ACTDP, port + 6);
}

static u32 airoha_foe_v1_packet_type(const struct airoha_foe_entry *entry)
{
	return FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, entry->ib1);
}

static void airoha_ppe_v1_commit_entry(struct airoha_ppe *ppe,
				    struct airoha_foe_entry *entry, u16 hash,
				    const struct airoha_foe_entry *lookup_raw)
{
	u32 *slot = airoha_ppe_v1_slot(ppe, hash);
	u16 timestamp = airoha_fe_rr(ppe->common.eth, REG_FE_FOE_TS) &
			AIROHA_FOE_IB1_BIND_TIMESTAMP;
	int i;

	entry->ib1 &= ~AIROHA_FOE_IB1_BIND_TIMESTAMP;
	entry->ib1 |= FIELD_PREP(AIROHA_FOE_IB1_BIND_TIMESTAMP, timestamp) |
		AIROHA_FOE_V1_IB1_BIND_CACHE | AIROHA_FOE_IB1_BIND_TTL |
		AIROHA_FOE_IB1_BIND_KEEPALIVE;
	airoha_foe_v1_entry_set_bind_metadata(entry);

	if (lookup_raw) {
		switch (airoha_foe_v1_packet_type(lookup_raw)) {
		case PPE_PKT_TYPE_IPV4_HNAPT:
			entry->words[3] = lookup_raw->words[3];
			fallthrough;
		case PPE_PKT_TYPE_IPV4_ROUTE:
			entry->words[1] = lookup_raw->words[1];
			entry->words[2] = lookup_raw->words[2];
			break;
		case PPE_PKT_TYPE_IPV6_ROUTE_5T:
			for (i = 1; i <= 9; i++)
				entry->words[i] = lookup_raw->words[i];
			break;
		default:
			break;
		}
	}

	for (i = 1; i < AIROHA_FOE_ENTRY_WORDS; i++)
		WRITE_ONCE(slot[i], entry->words[i]);
	dma_wmb();
	WRITE_ONCE(slot[0], entry->words[0]);
	dma_wmb();
	airoha_ppe_v1_cache_clean(ppe);
}

static void airoha_ppe_v1_invalidate_entry(struct airoha_ppe *ppe, u16 hash)
{
	if (hash == AIROHA_FOE_V1_INVALID_HASH ||
	    hash >= ppe->common.eth->soc->ppe_dram_entries)
		return;
	WRITE_ONCE(airoha_ppe_v1_slot(ppe, hash)[0], 0);
	/* Publish the invalid state before the caller clears the cache. */
	dma_wmb();
}

static u32 airoha_ppe_v1_raw_state(const struct airoha_foe_entry *raw)
{
	return FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, raw->ib1);
}

static void airoha_ppe_v1_clear_owner(struct airoha_ppe *ppe, u16 hash)
{
	struct airoha_flow_table_entry *owner;

	if (hash == AIROHA_FOE_V1_INVALID_HASH ||
	    hash >= ppe->common.eth->soc->ppe_dram_entries)
		return;

	owner = ppe->v1.foe_owner[hash];
	if (owner && owner->hash == hash)
		owner->hash = AIROHA_FOE_V1_INVALID_HASH;
	ppe->v1.foe_owner[hash] = NULL;
}

static void airoha_ppe_v1_release_flow_slot(struct airoha_ppe *ppe,
					 struct airoha_flow_table_entry *flow,
					 bool invalidate)
{
	u16 hash = flow->hash;

	if (hash == AIROHA_FOE_V1_INVALID_HASH ||
	    hash >= ppe->common.eth->soc->ppe_dram_entries ||
	    ppe->v1.foe_owner[hash] != flow) {
		flow->hash = AIROHA_FOE_V1_INVALID_HASH;
		return;
	}

	if (invalidate)
		airoha_ppe_v1_invalidate_entry(ppe, hash);
	ppe->v1.foe_owner[hash] = NULL;
	flow->hash = AIROHA_FOE_V1_INVALID_HASH;
}

static u16 airoha_ppe_v1_find_bind_way(struct airoha_ppe *ppe,
				    struct airoha_flow_table_entry *flow, u16 hash,
				    struct airoha_foe_entry *lookup_raw,
				    bool *lookup_valid)
{
	struct airoha_foe_entry raw[2];
	u16 way[2] = { hash, hash ^ 1 };
	u32 state[2];
	int i;

	*lookup_valid = false;

	/*
	 * FoeHashFun() in the EN7512 SDK returns an even bucket base and then
	 * checks base/base + 1.  RX metadata may contain either way, so retain
	 * the reported way as the first preference and inspect its sibling too.
	 */
	for (i = 0; i < ARRAY_SIZE(way); i++) {
		struct airoha_flow_table_entry *owner;

		if (way[i] >= ppe->common.eth->soc->ppe_dram_entries)
			continue;
		airoha_ppe_v1_read_entry(ppe, way[i], &raw[i]);
		state[i] = airoha_ppe_v1_raw_state(&raw[i]);

		owner = ppe->v1.foe_owner[way[i]];
		if (owner && state[i] != AIROHA_FOE_STATE_BIND) {
			/* Hardware aging/replacement ended the previous ownership. */
			airoha_ppe_v1_clear_owner(ppe, way[i]);
			owner = NULL;
		}
		if (owner == flow && state[i] == AIROHA_FOE_STATE_BIND)
			return way[i];
	}

	/*
	 * Match the EN7512 FoeHashFun() allocation policy: INVALID and UNBIND
	 * ways are reusable, while BIND and FIN are occupied.  An UNBIND way
	 * contains the lookup key generated by the PPE and must be preserved;
	 * an INVALID way has no key, so airoha_ppe_v1_commit_entry() encodes the
	 * logical tuple from flow->data instead.
	 */
	for (i = 0; i < ARRAY_SIZE(way); i++) {
		if (way[i] >= ppe->common.eth->soc->ppe_dram_entries ||
		    ppe->v1.foe_owner[way[i]])
			continue;

		switch (state[i]) {
		case AIROHA_FOE_STATE_UNBIND:
			*lookup_raw = raw[i];
			*lookup_valid = true;
			return way[i];
		case AIROHA_FOE_STATE_INVALID:
			return way[i];
		default:
			break;
		}
	}

	return AIROHA_FOE_V1_INVALID_HASH;
}

struct airoha_foe_v1_tuple {
	u16 addr_type;
	union {
		struct {
			u32 src;
			u32 dest;
		} v4;
		struct {
			struct in6_addr src;
			struct in6_addr dest;
		} v6;
	};
	u16 src_port;
	u16 dest_port;
};

static bool airoha_foe_v1_parse_tuple(struct sk_buff *skb,
				   struct airoha_foe_v1_tuple *tuple)
{
	const u8 *data = skb->data;
	unsigned int len = skb->len;
	unsigned int proto_off = 12, off;
	u16 proto;

	if (len < ETH_HLEN)
		return false;

	proto = get_unaligned_be16(data + proto_off);
	if (proto == ETH_P_8021Q || proto == ETH_P_8021AD) {
		if (len < ETH_HLEN + VLAN_HLEN)
			return false;
		proto_off += VLAN_HLEN;
		proto = get_unaligned_be16(data + proto_off);
	} else if (proto != ETH_P_IP && proto != ETH_P_IPV6 &&
		   proto != ETH_P_PPP_SES) {
		/* MTK special tag occupies the ethertype/TCI position. */
		if (len < ETH_HLEN + 4)
			return false;
		proto_off += 4;
		proto = get_unaligned_be16(data + proto_off);
	}
	off = proto_off + sizeof(__be16);

	if (proto == ETH_P_PPP_SES) {
		u16 ppp_proto;

		if (len < proto_off + 10)
			return false;
		ppp_proto = get_unaligned_be16(data + proto_off + 8);
		if (ppp_proto == PPP_IP)
			proto = ETH_P_IP;
		else if (ppp_proto == PPP_IPV6)
			proto = ETH_P_IPV6;
		else
			return false;
		off = proto_off + 10;
	}

	memset(tuple, 0, sizeof(*tuple));
	if (proto == ETH_P_IP) {
		const u8 *iph, *l4;
		u8 ihl;

		if (len < off + sizeof(struct iphdr))
			return false;
		iph = data + off;
		if ((iph[0] >> 4) != 4 ||
		    (iph[9] != IPPROTO_TCP && iph[9] != IPPROTO_UDP))
			return false;
		ihl = (iph[0] & 0x0f) * 4;
		if (ihl < sizeof(struct iphdr) || len < off + ihl + 4)
			return false;

		l4 = iph + ihl;
		tuple->addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
		tuple->v4.src = get_unaligned_be32(iph + offsetof(struct iphdr, saddr));
		tuple->v4.dest = get_unaligned_be32(iph + offsetof(struct iphdr, daddr));
		tuple->src_port = get_unaligned_be16(l4);
		tuple->dest_port = get_unaligned_be16(l4 + 2);
		return true;
	}

	if (proto == ETH_P_IPV6) {
		unsigned short fragoff = 0;
		unsigned int l4off = off;
		int flags = 0, nexthdr;

		if (len < off + sizeof(struct ipv6hdr) ||
		    (data[off] >> 4) != 6)
			return false;

		nexthdr = ipv6_find_hdr(skb, &l4off, -1, &fragoff, &flags);
		if ((nexthdr != IPPROTO_TCP && nexthdr != IPPROTO_UDP) ||
		    (flags & IP6_FH_F_FRAG) || len < l4off + 4)
			return false;

		tuple->addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		memcpy(&tuple->v6.src,
		       data + off + offsetof(struct ipv6hdr, saddr),
		       sizeof(tuple->v6.src));
		memcpy(&tuple->v6.dest,
		       data + off + offsetof(struct ipv6hdr, daddr),
		       sizeof(tuple->v6.dest));
		tuple->src_port = get_unaligned_be16(data + l4off);
		tuple->dest_port = get_unaligned_be16(data + l4off + 2);
		return true;
	}

	return false;
}

static bool
airoha_foe_v1_flow_matches_tuple(const struct airoha_flow_table_entry *flow,
			      const struct airoha_foe_v1_tuple *tuple)
{
	if (flow->addr_type != tuple->addr_type ||
	    flow->src_port != tuple->src_port ||
	    flow->dest_port != tuple->dest_port)
		return false;

	if (tuple->addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		return flow->src_ip == tuple->v4.src &&
		       flow->dest_ip == tuple->v4.dest;

	if (tuple->addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS)
		return ipv6_addr_equal(&flow->src_ip6, &tuple->v6.src) &&
		       ipv6_addr_equal(&flow->dest_ip6, &tuple->v6.dest);

	return false;
}

static void airoha_ppe_v1_rx_check(struct airoha_ppe *ppe, struct sk_buff *skb,
				u16 hash, u8 reason)
{
	struct airoha_foe_v1_tuple tuple;
	struct airoha_flow_table_entry *flow;
	struct airoha_foe_entry entry, lookup_raw = {};
	bool lookup_valid;
	u16 bind_hash;

	if (!ppe || !READ_ONCE(ppe->v1.armed) ||
	    hash >= ppe->common.eth->soc->ppe_dram_entries)
		return;
	if (reason != EN751221_PPE_CPU_REASON_NO_FLOW &&
	    reason != AIROHA_PPE_CPU_REASON_HIT_UNBIND &&
	    reason != AIROHA_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
		return;
	if (!airoha_foe_v1_parse_tuple(skb, &tuple))
		return;

	spin_lock_bh(&ppe->v1.lock);
	list_for_each_entry(flow, &ppe->v1.flows, v1_list) {
		if (!airoha_foe_v1_flow_matches_tuple(flow, &tuple))
			continue;

		bind_hash = airoha_ppe_v1_find_bind_way(ppe, flow, hash,
						     &lookup_raw, &lookup_valid);
		if (bind_hash == AIROHA_FOE_V1_INVALID_HASH)
			break;
		if (ppe->v1.foe_owner[bind_hash] == flow &&
		    flow->hash == bind_hash)
			break;

		entry = flow->data;
		if (flow->hash != bind_hash)
			airoha_ppe_v1_release_flow_slot(ppe, flow, true);

		airoha_ppe_v1_commit_entry(ppe, &entry, bind_hash,
				       lookup_valid ? &lookup_raw : NULL);
		ppe->v1.foe_owner[bind_hash] = flow;
		flow->hash = bind_hash;
		break;
	}
	spin_unlock_bh(&ppe->v1.lock);
}

static void airoha_ppe_check_skb_reason(struct airoha_ppe_dev *ppe_dev,
				       struct sk_buff *skb, u16 hash, u8 reason)
{
	struct airoha_ppe *ppe = ppe_dev->priv;

	if (ppe->common.eth->soc->foe_format == AIROHA_FOE_FORMAT_V1)
		airoha_ppe_v1_rx_check(ppe, skb, hash, reason);
}

static void airoha_ppe_v1_flush(struct airoha_ppe *ppe)
{
	struct airoha_flow_table_entry *flow, *tmp;
	LIST_HEAD(free_list);

	spin_lock_bh(&ppe->v1.lock);
	list_for_each_entry(flow, &ppe->v1.flows, v1_list)
		airoha_ppe_v1_release_flow_slot(ppe, flow, true);
	list_splice_init(&ppe->v1.flows, &free_list);
	spin_unlock_bh(&ppe->v1.lock);

	list_for_each_entry_safe(flow, tmp, &free_list, v1_list) {
		rhashtable_remove_fast(&ppe->common.eth->flow_table, &flow->node,
				       airoha_flow_table_params);
		list_del(&flow->v1_list);
		kfree(flow);
	}

	memset(ppe->common.foe, 0, airoha_ppe_v1_foe_size(ppe->common.eth));
	/* Publish the cleared table before invalidating the lookup cache. */
	dma_wmb();
	airoha_ppe_v1_cache_clean(ppe);
}

/*
 * EN751221 vendor hw_nat (PPE type 3) programs the otherwise unnamed
 * generation-1 GLO_CFG bits 1 and 8 together with byte-swap/hash-offset
 * and flow-drop-update. Bit 10 is not touched by hw_nat itself, but it is set
 * in the production FE state (GLO_CFG = 0x763); the consolidated Linux driver
 * owns the FE reset, so restore that steady-state bit explicitly on enable.
 */
#define EN751221_PPE_GLO_CFG_VENDOR_SET	(BIT(1) | BIT(8) | \
					 PPE_GLO_CFG_PPE_BSWAP_MASK | \
					 PPE_GLO_CFG_PSE_HASH_OFS_MASK | \
					 PPE_GLO_CFG_FLOW_DROP_UPDATE_MASK)
#define EN751221_PPE_GLO_CFG_STOCK_BIT10	BIT(10)
#define EN751221_PPE_GLO_CFG_VENDOR_CLEAR	(PPE_GLO_CFG_TTL_DROP_MASK | \
					 PPE_GLO_CFG_IP4_CS_DROP_MASK | \
					 PPE_GLO_CFG_IP4_L4_CS_DROP_MASK)

/*
 * The type-3 stock flow profile is 0x07e0f740. Bits 21..24 are not named
 * by the common Airoha register definitions yet, but are part of the
 * EN751221 L2B/IPv4/IPv6/DS-LITE/6RD profile. IP protocol blacklist mode
 * (bit 16) is configured separately by the vendor driver.
 */
#define EN751221_PPE_FLOW_CFG_TYPE3	(GENMASK(26, 21) | \
					 PPE_FLOW_CFG_L2_BRIDGE_MASK | \
					 PPE_FLOW_CFG_IP4_DSLITE_MASK | \
					 PPE_FLOW_CFG_IP4_NAPT_MASK | \
					 PPE_FLOW_CFG_IP4_NAT_MASK | \
					 PPE_FLOW_CFG_IP6_6RD_MASK | \
					 PPE_FLOW_CFG_IP6_5T_ROUTE_MASK | \
					 PPE_FLOW_CFG_IP6_3T_ROUTE_MASK | \
					 PPE_FLOW_CFG_IP4_TCP_FRAG_MASK)

static void airoha_ppe_v1_set_flow_profile(struct airoha_ppe *ppe)
{
	airoha_fe_wr(ppe->common.eth, REG_PPE_PPE_FLOW_CFG(0),
		     EN751221_PPE_FLOW_CFG_TYPE3 |
		     PPE_FLOW_CFG_IP_PROTO_BLACKLIST_MASK);
}

static void airoha_ppe_v1_hw_init(struct airoha_ppe *ppe)
{
	struct airoha_eth *eth = ppe->common.eth;
	u32 dram_num_entries;
	u32 tb_cfg;

	dram_num_entries = airoha_ppe_get_num_entries_shift(
		eth->soc->ppe_dram_entries);

	dev_info(eth->dev, "Initializing generation-1 PPE hardware\n");

	/*
	 * FoE v1 uses CAH_GATE to enable the lookup cache. CAH_CTRL is the
	 * cache command register, not an enable bit. Match the vendor hw_nat
	 * programming and invalidate stale cache contents before publishing
	 * the DMA-backed 80-byte FoE table.
	 */
	airoha_fe_wr(eth, REG_EN751221_PPE_CAH_GATE,
		     EN751221_PPE_CAH_GATE_DEFAULT);
	airoha_ppe_v1_cache_clean(ppe);

	memset(ppe->common.foe, 0, airoha_ppe_v1_foe_size(eth));
	dma_wmb();

	airoha_fe_wr(eth, REG_PPE_TB_BASE(0),
		     lower_32_bits(ppe->common.foe_dma));
	tb_cfg = PPE_TB_CFG_AGE_TCP_FIN_MASK | PPE_TB_CFG_AGE_UDP_MASK |
		 PPE_TB_CFG_AGE_TCP_MASK | PPE_TB_CFG_AGE_UNBIND_MASK |
		 PPE_TB_CFG_AGE_NON_L4_MASK |
		 FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
		 FIELD_PREP(PPE_TB_CFG_HASH_MODE_MASK, 3) |
		 FIELD_PREP(PPE_TB_CFG_KEEPALIVE_MASK, 3) |
		 FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK, dram_num_entries) |
		 PPE_TB_ENTRY_SIZE_MASK;
	airoha_fe_wr(eth, REG_PPE_TB_CFG(0), tb_cfg);
	airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(0),
		     PPE_IP_PROTO_CHK_IPV4_MASK | PPE_IP_PROTO_CHK_IPV6_MASK);

	/* Keep the same protocol classifier programmed by the vendor stack. */
	airoha_fe_wr(eth, REG_PPE_IP_PROT(0, 0),
		     FIELD_PREP(GENMASK(7, 0), IPPROTO_TCP) |
		     FIELD_PREP(GENMASK(15, 8), IPPROTO_UDP) |
		     FIELD_PREP(GENMASK(23, 16), IPPROTO_IPV6) |
		     FIELD_PREP(GENMASK(31, 24), IPPROTO_IPIP));
	airoha_fe_wr(eth, REG_PPE_IP_PROT(0, 1), 0);
	airoha_fe_wr(eth, REG_PPE_IP_PROT(0, 2), 0);
	airoha_fe_wr(eth, REG_PPE_IP_PROT(0, 3), 0);
	airoha_ppe_v1_set_flow_profile(ppe);

	airoha_fe_wr(eth, REG_PPE_UNBIND_AGE(0),
		     FIELD_PREP(PPE_UNBIND_AGE_MIN_PACKETS_MASK, 1000) |
		     FIELD_PREP(PPE_UNBIND_AGE_DELTA_MASK, 3));
	airoha_fe_wr(eth, REG_PPE_BND_AGE0(0),
		     FIELD_PREP(PPE_BIND_AGE0_DELTA_NON_L4, 15) |
		     FIELD_PREP(PPE_BIND_AGE0_DELTA_UDP, 15));
	airoha_fe_wr(eth, REG_PPE_BND_AGE1(0),
		     FIELD_PREP(PPE_BIND_AGE1_DELTA_TCP_FIN, 5) |
		     FIELD_PREP(PPE_BIND_AGE1_DELTA_TCP, 15));
	airoha_fe_wr(eth, REG_PPE_BIND_LIMIT0(0),
		     FIELD_PREP(PPE_BIND_LIMIT0_HALF_MASK, 800) |
		     FIELD_PREP(PPE_BIND_LIMIT0_QUARTER_MASK, 1600));
	airoha_fe_wr(eth, REG_PPE_BIND_LIMIT1(0),
		     FIELD_PREP(PPE_BIND_LIMIT1_NON_L4_MASK, 1) |
		     FIELD_PREP(PPE_BIND_LIMIT1_FULL_MASK, 400));
	airoha_fe_wr(eth, REG_PPE_BIND_RATE(0),
		     FIELD_PREP(PPE_BIND_RATE_BIND_MASK, 30));
	airoha_fe_wr(eth, REG_PPE_HASH_SEED(0), PPE_HASH_SEED);
	airoha_fe_wr(eth, REG_PPE_DFT_CPORT_BASE(0), 0);
	airoha_fe_clear(eth, REG_PPE_GLO_CFG(0), PPE_GLO_CFG_EN_MASK);
}

static int airoha_ppe_v1_engine_set(struct airoha_ppe *ppe, bool enable)
{
	void __iomem *reg = ppe->common.eth->fe_regs + REG_PPE_GLO_CFG(0);
	u32 val;

	if (enable) {
		if (readl_poll_timeout_atomic(reg, val,
					      !(val & PPE_GLO_CFG_BUSY_MASK),
					      10, 10000))
			return -EBUSY;

		/* Match FUN_00013f50(1) from the stock hw_nat module. */
		val = readl(reg);
		val &= ~EN751221_PPE_GLO_CFG_VENDOR_CLEAR;
		val |= EN751221_PPE_GLO_CFG_VENDOR_SET |
		       EN751221_PPE_GLO_CFG_STOCK_BIT10 | PPE_GLO_CFG_EN_MASK;
		writel(val, reg);
	} else {
		/* Match FUN_00013f50(0), preserving unrelated FE-owned bits. */
		val = readl(reg);
		val &= ~(EN751221_PPE_GLO_CFG_VENDOR_SET |
			 PPE_GLO_CFG_TTL_DROP_MASK | PPE_GLO_CFG_EN_MASK);
		val |= PPE_GLO_CFG_IP4_CS_DROP_MASK |
		       PPE_GLO_CFG_IP4_L4_CS_DROP_MASK;
		writel(val, reg);
	}
	return 0;
}

static void airoha_ppe_v1_set_gdm_ingress(struct airoha_ppe *ppe, int gdm,
				       u8 fport)
{
	u32 mask = GDM_UCFQ_MASK | GDM_BCFQ_MASK |
		   GDM_MCFQ_MASK | GDM_OCFQ_MASK;
	u32 val = FIELD_PREP(GDM_UCFQ_MASK, fport) |
		  FIELD_PREP(GDM_BCFQ_MASK, fport) |
		  FIELD_PREP(GDM_MCFQ_MASK, fport) |
		  FIELD_PREP(GDM_OCFQ_MASK, fport);

	/*
	 * Stock SetGdmaFwd() sends all four ingress classes to fport 4 while
	 * HWNAT is enabled (the observed low word is 0x4444). Keep this an
	 * RMW so EN751221 special-tag/UNTAG state in the upper bits survives.
	 */
	airoha_fe_rmw(ppe->common.eth, REG_GDM_FWD_CFG(gdm), mask, val);
}

static int airoha_ppe_v1_engine_arm(struct airoha_ppe *ppe)
{
	int err;

	if (!ppe)
		return -ENODEV;
	if (READ_ONCE(ppe->v1.armed))
		return 0;

	airoha_fe_wr(ppe->common.eth, REG_PPE_TB_CFG(0),
		     FIELD_PREP(PPE_TB_CFG_HASH_MODE_MASK, 3) |
		     FIELD_PREP(PPE_TB_CFG_KEEPALIVE_MASK, 3) |
		     PPE_TB_CFG_AGE_TCP_FIN_MASK | PPE_TB_CFG_AGE_UDP_MASK |
		     PPE_TB_CFG_AGE_TCP_MASK | PPE_TB_CFG_AGE_UNBIND_MASK |
		     PPE_TB_CFG_AGE_NON_L4_MASK |
		     FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
		     PPE_TB_ENTRY_SIZE_MASK |
		     FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK, 4));
	airoha_fe_wr(ppe->common.eth, REG_PPE_KEEPALIVE(0),
		     FIELD_PREP(PPE_KEEPALIVE_UDP_MASK, 1) |
		     FIELD_PREP(PPE_KEEPALIVE_TCP_MASK, 1) |
		     FIELD_PREP(PPE_KEEPALIVE_NTU_MASK, 1));

	/*
	 * Program the complete type-3 datapath before enabling GLO_CFG. The
	 * stock module only calls SetGdmaFwd(1) after cache, FoE/table state,
	 * protocol parsing and the PPE engine itself are ready.
	 */
	airoha_fe_wr(ppe->common.eth, REG_EN751221_PPE_CAH_GATE,
		     EN751221_PPE_CAH_GATE_DEFAULT);
	airoha_ppe_v1_set_flow_profile(ppe);
	airoha_fe_wr(ppe->common.eth, REG_PPE_VPM_TPID(0), ETH_P_8021Q);
	airoha_fe_wr(ppe->common.eth, REG_CDM_VLAN_CTRL(1),
		     FIELD_PREP(CDM_VLAN_MASK, ETH_P_8021Q) | STAG_EN);

	/* SetGdmaFwd() in the stock module writes PPE_DFT_CPORT = 0x500. */
	airoha_fe_wr(ppe->common.eth, REG_PPE_DFT_CPORT_BASE(0),
		     FIELD_PREP(DFT_CPORT_MASK(2),
				ppe->common.eth->soc->ppe_cpu_fport[1]));

	err = airoha_ppe_v1_engine_set(ppe, true);
	if (err)
		return err;

	/* GDM redirection is deliberately the last arm step. */
	airoha_ppe_v1_set_gdm_ingress(ppe, 1, ppe->common.eth->soc->ppe_fport);
	airoha_ppe_v1_set_gdm_ingress(ppe, 2, ppe->common.eth->soc->ppe_fport);
	WRITE_ONCE(ppe->v1.armed, true);
	return 0;
}

static void airoha_ppe_v1_engine_disarm(struct airoha_ppe *ppe)
{
	if (!ppe || !READ_ONCE(ppe->v1.armed))
		return;

	/* Restore the CPU ingress paths while preserving STAG/UNTAG state. */
	airoha_ppe_v1_set_gdm_ingress(ppe, 1, ppe->common.eth->soc->ppe_cpu_fport[0]);
	airoha_ppe_v1_set_gdm_ingress(ppe, 2, ppe->common.eth->soc->ppe_cpu_fport[1]);
	WRITE_ONCE(ppe->v1.armed, false);
	airoha_ppe_v1_flush(ppe);
	airoha_ppe_v1_engine_set(ppe, false);
}

static struct airoha_ppe *airoha_ppe_from_netdev(struct net_device *netdev)
{
	struct airoha_gdm_common *gdm;

	gdm = airoha_gdm_common_from_netdev(netdev);
	if (!gdm || !gdm->ppe)
		return NULL;

	return gdm->ppe->priv;
}

static int airoha_ppe_v1_flow_set_output(struct airoha_foe_entry *entry,
				  struct net_device *odev, bool vlan_valid,
				  u16 vlan_id, u8 vlan_prio, bool *xpon)
{
	struct airoha_gdm_common *gdm;
	struct dsa_port *dp;
	int dsa_port = -1;
	int err;

	*xpon = false;
	if (!odev)
		return -EOPNOTSUPP;

	if (dsa_user_dev_check(odev)) {
		dp = dsa_port_from_netdev(odev);
		if (IS_ERR(dp))
			return PTR_ERR(dp);
		dsa_port = dp->index;
		odev = dsa_port_to_conduit(dp);
		airoha_foe_v1_entry_set_dsa(entry, dsa_port, dp->ds->index != 0);
	}

	if (!odev)
		return -EOPNOTSUPP;

	gdm = airoha_gdm_common_from_netdev(odev);
	if (!gdm || gdm->family != AIROHA_ETH_FAMILY_ECONET)
		return -EOPNOTSUPP;

	/*
	 * Probe the xPON service API before using direct GDM2 egress. A managed
	 * GPON port needs QDMA_HW plus GEM/T-CONT metadata; sending a FoE flow
	 * straight to GDM2 bypasses exactly the metadata which the CPU PWAN
	 * descriptor normally supplies.
	 */
	if (dsa_port < 0) {
		err = airoha_foe_v1_entry_set_xpon(entry, odev, vlan_valid, vlan_id,
						vlan_valid, vlan_prio);
		if (!err) {
			*xpon = true;
			return 0;
		}
		if (err != -EOPNOTSUPP)
			return err;
	}

	/* VLAN insertion is only implemented for the GPON/QDMA FoE format. */
	if (vlan_valid)
		return -EOPNOTSUPP;

	/*
	 * Keep direct GDM egress on the default queue. The EN751221 PPE
	 * flow path does not enable PSE_QOS for GDM1/GDM2 destinations,
	 * and the previous 3 + DSA-port QID mapping is not part of the
	 * vendor DSA special-tag programming.
	 *
	 * The DSA destination is still selected by the in-band special tag;
	 * leaving QID at its reset/default value isolates queue selection from
	 * the hardware forwarding path.
	 */
	airoha_foe_v1_entry_set_pse_port(entry, gdm->pse_port);

	return 0;
}

static int airoha_ppe_v1_flow_offload_replace(struct net_device *dev,
				       struct flow_cls_offload *cls)
{
	struct airoha_ppe *ppe = airoha_ppe_from_netdev(dev);
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_action_entry *act;
	struct airoha_flow_table_entry *flow;
	struct airoha_flow_data data = {};
	struct airoha_foe_entry entry;
	struct net_device *odev = NULL;
	struct in6_addr src_addr6 = {}, dest_addr6 = {};
	__be32 src_addr = 0, dest_addr = 0;
	__be16 src_port, dest_port;
	__be16 vlan_proto = 0;
	u16 pppoe_sid = 0, vlan_id = 0, addr_type;
	u8 l4proto, vlan_prio = 0, pkt_type;
	bool vlan_push = false, xpon = false;
	int i, err;

	if (!ppe)
		return -EOPNOTSUPP;
	if (rhashtable_lookup(&ppe->common.eth->flow_table, &cls->cookie,
			      airoha_flow_table_params))
		return -EEXIST;
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "FoE v1 requires TCP/UDP 5-tuple flows");
		return -EOPNOTSUPP;
	}

	flow_rule_match_basic(rule, &basic);
	l4proto = basic.key->ip_proto;
	if (l4proto != IPPROTO_TCP && l4proto != IPPROTO_UDP)
		return -EOPNOTSUPP;
	if (basic.mask->n_proto != htons(0xffff) ||
	    basic.mask->ip_proto != 0xff)
		return -EOPNOTSUPP;

	if (basic.key->n_proto == htons(ETH_P_IP)) {
		struct flow_match_ipv4_addrs addrs;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv4_addrs(rule, &addrs);
		if (addrs.mask->src != htonl(0xffffffff) ||
		    addrs.mask->dst != htonl(0xffffffff)) {
			NL_SET_ERR_MSG_MOD(cls->common.extack,
					   "FoE v1 requires exact IPv4 addresses");
			return -EOPNOTSUPP;
		}
		src_addr = addrs.key->src;
		dest_addr = addrs.key->dst;
		data.v4.src_addr = src_addr;
		data.v4.dst_addr = dest_addr;
		addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
		pkt_type = PPE_PKT_TYPE_IPV4_HNAPT;
	} else if (basic.key->n_proto == htons(ETH_P_IPV6)) {
		struct flow_match_ipv6_addrs addrs;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv6_addrs(rule, &addrs);
		if (memchr_inv(&addrs.mask->src, 0xff, sizeof(addrs.mask->src)) ||
		    memchr_inv(&addrs.mask->dst, 0xff, sizeof(addrs.mask->dst))) {
			NL_SET_ERR_MSG_MOD(cls->common.extack,
					   "FoE v1 requires exact IPv6 addresses");
			return -EOPNOTSUPP;
		}
		src_addr6 = addrs.key->src;
		dest_addr6 = addrs.key->dst;
		data.v6.src_addr = src_addr6;
		data.v6.dst_addr = dest_addr6;
		addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		pkt_type = PPE_PKT_TYPE_IPV6_ROUTE_5T;
	} else {
		return -EOPNOTSUPP;
	}

	flow_rule_match_ports(rule, &ports);
	if (ports.mask->src != htons(0xffff) ||
	    ports.mask->dst != htons(0xffff)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "FoE v1 requires exact TCP/UDP ports");
		return -EOPNOTSUPP;
	}
	if (!flow_action_basic_hw_stats_check(&rule->action,
					      cls->common.extack))
		return -EOPNOTSUPP;

	src_port = ports.key->src;
	dest_port = ports.key->dst;
	data.src_port = src_port;
	data.dst_port = dest_port;

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				airoha_ppe_flow_mangle_eth(act, &data.eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				if (addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
					return -EOPNOTSUPP;
				err = airoha_ppe_flow_mangle_ipv4(act, &data);
				if (err)
					return err;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				/* IPv6 5T is routing-only on FoE V1; NAT66/port
				 * translation has no rewrite tuple in this format.
				 */
				if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS)
					return -EOPNOTSUPP;
				err = airoha_ppe_flow_mangle_ports(act, &data);
				if (err)
					return err;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP6:
				return -EOPNOTSUPP;
			default:
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (vlan_push)
				return -EOPNOTSUPP;
			vlan_push = true;
			vlan_id = act->vlan.vid;
			vlan_prio = act->vlan.prio;
			vlan_proto = act->vlan.proto;
			break;
		case FLOW_ACTION_VLAN_POP:
			/* No output VLAN is encoded for this direction. */
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			pppoe_sid = act->pppoe.sid;
			break;
		case FLOW_ACTION_REDIRECT:
			odev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			break;
		default:
			NL_SET_ERR_MSG_MOD(cls->common.extack,
					   "unsupported action for FoE v1");
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(data.eth.h_source) ||
	    !is_valid_ether_addr(data.eth.h_dest)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "missing Ethernet rewrite addresses");
		return -EOPNOTSUPP;
	}

	airoha_foe_v1_entry_prepare(&entry, l4proto, pkt_type, FE_PSE_PORT_GDM1,
				 data.eth.h_source, data.eth.h_dest);
	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		airoha_foe_v1_entry_set_ipv4_tuple(&entry, false, src_addr, src_port,
						dest_addr, dest_port);
		airoha_foe_v1_entry_set_ipv4_tuple(&entry, true, data.v4.src_addr,
						data.src_port, data.v4.dst_addr,
						data.dst_port);
	} else {
		airoha_foe_v1_entry_set_ipv6_tuple(&entry, &src_addr6, src_port,
						&dest_addr6, dest_port);
	}

	/* Match the vendor ordering: PpeFillInL2Info() installs VLAN/PPPoE
	 * first and PpeSetPortInfo(FOE_MAGIC_GPON) runs last, repurposing
	 * etype for the GEM port ID.
	 */
	if (vlan_push) {
		err = airoha_foe_v1_entry_set_vlan(&entry, vlan_id, vlan_prio,
						vlan_proto);
		if (err)
			return err;
	}
	if (pppoe_sid)
		airoha_foe_v1_entry_set_pppoe(&entry, pppoe_sid);

	err = airoha_ppe_v1_flow_set_output(&entry, odev, vlan_push, vlan_id,
				     vlan_prio, &xpon);
	if (err)
		return err;
	if (vlan_push && !xpon)
		return -EOPNOTSUPP;

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return -ENOMEM;
	flow->data = entry;
	flow->cookie = cls->cookie;
	flow->addr_type = addr_type;
	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		flow->src_ip = ntohl(src_addr);
		flow->dest_ip = ntohl(dest_addr);
	} else {
		flow->src_ip6 = src_addr6;
		flow->dest_ip6 = dest_addr6;
	}
	flow->src_port = ntohs(src_port);
	flow->dest_port = ntohs(dest_port);
	flow->hash = AIROHA_FOE_V1_INVALID_HASH;
	INIT_LIST_HEAD(&flow->v1_list);

	err = rhashtable_insert_fast(&ppe->common.eth->flow_table, &flow->node,
				     airoha_flow_table_params);
	if (err) {
		kfree(flow);
		return err;
	}

	spin_lock_bh(&ppe->v1.lock);
	list_add_tail(&flow->v1_list, &ppe->v1.flows);
	spin_unlock_bh(&ppe->v1.lock);
	return 0;
}

static int airoha_ppe_v1_flow_offload_destroy(struct net_device *dev,
					       struct flow_cls_offload *cls)
{
	struct airoha_ppe *ppe = airoha_ppe_from_netdev(dev);
	struct airoha_flow_table_entry *flow;

	if (!ppe)
		return -EOPNOTSUPP;

	flow = rhashtable_lookup(&ppe->common.eth->flow_table, &cls->cookie,
				 airoha_flow_table_params);
	if (!flow)
		return -ENOENT;

	spin_lock_bh(&ppe->v1.lock);
	airoha_ppe_v1_release_flow_slot(ppe, flow, true);
	airoha_ppe_v1_cache_clean(ppe);
	list_del_init(&flow->v1_list);
	spin_unlock_bh(&ppe->v1.lock);

	rhashtable_remove_fast(&ppe->common.eth->flow_table, &flow->node,
			       airoha_flow_table_params);
	kfree(flow);

	return 0;
}

static int airoha_ppe_v1_flow_offload_stats(struct flow_cls_offload *cls)
{
	flow_stats_update(&cls->stats, 0, 0, 0, jiffies,
			  FLOW_ACTION_HW_STATS_DELAYED);
	return 0;
}

static int airoha_ppe_tc_block_cb(enum tc_setup_type type, void *type_data,
				void *cb_priv)
{
	struct net_device *dev = cb_priv;
	struct airoha_ppe *ppe = airoha_ppe_from_netdev(dev);

	if (!ppe || type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return airoha_ppe_flow_offload_cmd(ppe->common.eth, dev, type_data);
}

static int airoha_ppe_setup_tc_block(struct net_device *dev,
				 struct flow_block_offload *offload)
{
	struct airoha_ppe *ppe = airoha_ppe_from_netdev(dev);
	flow_setup_cb_t *cb = airoha_ppe_tc_block_cb;
	struct flow_block_cb *block_cb;
	int err;

	if (!ppe || offload->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	offload->driver_block_list = &ppe->block_cb_list;
	switch (offload->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(offload->block, cb, dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		err = airoha_ppe_v1_engine_arm(ppe);
		if (err)
			return err;
		block_cb = flow_block_cb_alloc(cb, dev, dev, NULL);
		if (IS_ERR(block_cb)) {
			if (list_empty(&ppe->block_cb_list))
				airoha_ppe_v1_engine_disarm(ppe);
			return PTR_ERR(block_cb);
		}
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, offload);
		list_add_tail(&block_cb->driver_list, &ppe->block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(offload->block, cb, dev);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, offload);
			list_del(&block_cb->driver_list);
		}
		if (list_empty(&ppe->block_cb_list))
			airoha_ppe_v1_engine_disarm(ppe);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int airoha_ppe_setup_tc(struct airoha_ppe_dev *ppe_dev,
			       struct net_device *dev,
			       enum tc_setup_type type, void *type_data)
{
	if (airoha_ppe_from_netdev(dev) != ppe_dev->priv)
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return airoha_ppe_setup_tc_block(dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

static int airoha_ppe_v1_init(struct airoha_eth *eth)
{
	u32 dram_entries = eth->soc->ppe_dram_entries;
	size_t foe_size = airoha_ppe_v1_foe_size(eth);
	struct device *dev = eth->dev;
	struct airoha_ppe *ppe;
	int err;

	ppe = devm_kzalloc(dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return -ENOMEM;

	airoha_ppe_common_init(&ppe->common, eth, ppe);
	err = airoha_ppe_common_alloc_foe(&ppe->common, foe_size);
	if (err)
		return err;

	ppe->v1.foe_owner = devm_kcalloc(dev, dram_entries,
				      sizeof(*ppe->v1.foe_owner), GFP_KERNEL);
	if (!ppe->v1.foe_owner)
		return -ENOMEM;

	err = rhashtable_init(&eth->flow_table, &airoha_flow_table_params);
	if (err)
		return err;

	spin_lock_init(&ppe->v1.lock);
	INIT_LIST_HEAD(&ppe->v1.flows);
	INIT_LIST_HEAD(&ppe->block_cb_list);

	airoha_ppe_hw_init(ppe);

	ppe->common.dev.ops.check_skb_reason = airoha_ppe_check_skb_reason;
	ppe->common.dev.ops.setup_tc = airoha_ppe_setup_tc;

	if (airoha_ppe_debugfs_init(&ppe->common))
		dev_warn(dev, "failed to initialize generation-1 PPE debugfs\n");

	eth->ppe = ppe;
	airoha_ppe_common_enable(&ppe->common);

	dev_info(dev, "PPE FoE table at %pad, %u entries\n",
		 &ppe->common.foe_dma, dram_entries);
	return 0;
}

static void airoha_ppe_v1_deinit(struct airoha_eth *eth)
{
	struct airoha_ppe *ppe = eth->ppe;

	if (!ppe)
		return;

	if (READ_ONCE(ppe->v1.armed))
		airoha_ppe_v1_engine_disarm(ppe);
	else
		airoha_ppe_v1_flush(ppe);
	airoha_ppe_common_disable(&ppe->common);
	rhashtable_destroy(&eth->flow_table);
	eth->ppe = NULL;
}

int airoha_ppe_init(struct airoha_eth *eth)
{
	if (eth->soc->foe_format == AIROHA_FOE_FORMAT_V1)
		return airoha_ppe_v1_init(eth);

	return airoha_ppe_datapath_init(eth);
}

void airoha_ppe_deinit(struct airoha_eth *eth)
{
	if (eth->soc->foe_format == AIROHA_FOE_FORMAT_V1)
		airoha_ppe_v1_deinit(eth);
	else
		airoha_ppe_datapath_deinit(eth);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha PPE flow offload");
