// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/ip.h>
#include <linux/if_vlan.h>
#include <linux/ipv6.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
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

static int airoha_ppe_get_num_stats_entries(struct airoha_ppe *ppe)
{
	if (!IS_ENABLED(CONFIG_NET_AIROHA_FLOW_STATS))
		return -EOPNOTSUPP;

	if (airoha_is(ppe->eth, an7583))
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

	return ppe->eth->soc->ppe_sram_entries * eth->soc->num_ppe;
}

static u32 airoha_ppe_get_num_entries_shift(u32 entries)
{
	switch (entries) {
	case 256:
		/* EN7523 SRAM_TB_ETRY_NUM encoding (EPG PPE_TB_CFG 0x1FB50E1C):
		 * 6 = 256 entries. In 80-byte entry mode the on-chip SRAM only
		 * holds 256 entries (512 is the 64-byte mode).
		 */
		return 6;
	case 512:
		return 7; /* EPG: 7 = 512 entries (64-byte mode) */
	default:
		return __ffs(entries >> 10);
	}
}

static int airoha_ppe_hw_to_sw_idx(struct airoha_ppe *ppe, u32 hw_idx,
				   u32 *sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	u32 dram_num_entries = ppe->eth->soc->ppe_dram_entries;
	u32 ppe_num_entries = sram_num_entries + dram_num_entries;

	if (!airoha_is(ppe->eth, en7523)) {
		if (hw_idx >= ppe_num_entries)
			return -ERANGE;

		*sw_idx = hw_idx;
		return 0;
	}

	if (hw_idx < sram_num_entries) {
		*sw_idx = hw_idx;
		return 0;
	}

	if (hw_idx >= ppe->eth->soc->ppe_dram_entries &&
	    hw_idx < ppe->eth->soc->ppe_dram_entries + dram_num_entries) {
		*sw_idx = sram_num_entries + hw_idx - dram_num_entries;
		return 0;
	}

	return -ERANGE;
}

static u32 airoha_ppe_sw_to_hw_idx(struct airoha_ppe *ppe, u32 sw_idx)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	if (airoha_is(ppe->eth, en7523) && sw_idx >= sram_num_entries)
		return ppe->eth->soc->ppe_dram_entries + sw_idx - sram_num_entries;

	return sw_idx;
}

u32 airoha_ppe_get_total_num_entries(struct airoha_ppe *ppe)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);

	return sram_num_entries + ppe->eth->soc->ppe_dram_entries;
}

bool airoha_ppe_is_enabled(struct airoha_eth *eth, int index)
{
	if (index >= eth->soc->num_ppe)
		return false;

	return airoha_fe_rr(eth, REG_PPE_GLO_CFG(index)) & PPE_GLO_CFG_EN_MASK;
}
EXPORT_SYMBOL_GPL(airoha_ppe_is_enabled);

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

	dev_info(eth->dev, "Setting CPU port for PPE %d, fport %d to CDM%d\n", ppe_id, fport, qdma_id ? 2 : 1);
	airoha_fe_rmw(eth, REG_PPE_DFT_CPORT(ppe_id, fport),
		      DFT_CPORT_MASK(fport),
		      __field_prep(DFT_CPORT_MASK(fport), fe_cpu_port));
}
EXPORT_SYMBOL_GPL(airoha_ppe_set_cpu_port);

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
	if (airoha_is(eth, en7523))
		len = max_t(u32, len, 2000);

	ppe_id = !airoha_is_lan_gdm_dev(dev) && airoha_ppe_is_enabled(eth, 1);
	index = port->id == AIROHA_GDM4_IDX ? 7 : port->id;

	dev_info(eth->dev, "Setting PPE %d MTU for index %d to %u\n", ppe_id, index, len);
	airoha_fe_rmw(eth, REG_PPE_MTU(ppe_id, index),
		      FP_EGRESS_MTU_MASK(index),
		      __field_prep(FP_EGRESS_MTU_MASK(index), len));
}
EXPORT_SYMBOL_GPL(airoha_ppe_set_mtu);

static void airoha_ppe_hw_init(struct airoha_ppe *ppe)
{
	u32 sram_ppe_num_data_entries = ppe->eth->soc->ppe_sram_entries, sram_num_entries;
	u32 sram_tb_size, dram_num_entries;
	struct airoha_eth *eth = ppe->eth;
	int i, sram_num_stats_entries;

	dev_info(eth->dev, "Initializing PPE Hardware\n");
	sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	sram_tb_size = sram_num_entries * sizeof(struct airoha_foe_entry);
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

		switch (eth->soc->version) {
		case en7523:
			/**
			 * the en7523 support for 64 and 80 bytes, current use 80 bytes for ppe
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
			 * Uses the vendor's 64-byte entry mode (TB_CFG bit3=0,
			 * SRAM_TB_ETRY_NUM=7=512); our airoha_foe_entry is now 64B.
			 */
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i), 0x00038743);
			airoha_fe_wr(eth, REG_PPE_PPE_FLOW_CFG(i), 0x06bbf7c0);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i), 0x000f000f);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i) + 0x4, 0x04291106);
			airoha_fe_wr(eth, REG_PPE_IP_PROTO_CHK(i) + 0x8, 0x00003a01);
			airoha_fe_wr(eth, REG_PPE_TB_CFG(i), 0xef403fb4);
			airoha_fe_wr(eth, REG_PPE_TB_HASH_CFG(i), 0x31003001);
			/* Remaining vendor PPE config registers (dumped from stock
			 * fw): KA, MIRROR, L2 bridge cfg / ethertype enable.
			 * Offsets relative to GLO_CFG (PPE base + 0x200):
			 *   KA=0xE34(+0x34) MIRROR=0xE54(+0x54)
			 *   L2B_CFG=0xE88(+0x88) L2B_ETYPE_EN=0xE8C(+0x8c)
			 */
			airoha_fe_wr(eth, REG_PPE_GLO_CFG(i) + 0x34, 0x01010001);
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
		case en7581:
		case an7583:
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
			 airoha_is(eth, en7523) ? 0x0 : 0x1f);
	if (netdev) {
		struct airoha_wdma_info info = {};

		if (!airoha_ppe_get_wdma_info(netdev, data->eth.h_dest,
					      &info)) {
			val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ, info.idx) |
			       FIELD_PREP(AIROHA_FOE_IB2_PSE_PORT,
					  airoha_is(eth, en7523) ?
					  FE_PSE_PORT_GDM3 :
					  FE_PSE_PORT_CDM4);
			if (airoha_is(eth, en7523))
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

			if (airoha_is(eth, en7523) &&
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
			if (!airoha_is(eth, en7523) &&
			    airoha_is_lan_gdm_dev(dev))
				val |= AIROHA_FOE_IB2_FAST_PATH;
			if (xpon_flow)
				val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ, xpon.tcont);
			else if (dsa_port >= 0)
				val |= FIELD_PREP(AIROHA_FOE_IB2_NBQ,
						  dsa_port);
			else if (airoha_is(eth, en7523) &&
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

	if (airoha_is(ppe->eth, en7523)) {
		u32 dram_entries = ppe->eth->soc->ppe_dram_entries;
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

	*index = sw_idx >= ppe_num_stats_entries ? sw_idx - ppe->eth->soc->ppe_stats_entries
					       : sw_idx;

	return 0;
}

void airoha_ppe_foe_entry_get_stats(struct airoha_ppe *ppe, u32 sw_idx,
				    struct airoha_foe_stats64 *stats)
{
	struct airoha_eth *eth = ppe->eth;
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
	if (ppe->eth->ppe_host_ops && ppe->eth->ppe_host_ops->npu_stats_clear)
		ppe->eth->ppe_host_ops->npu_stats_clear(npu, index);
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

	/* 64-byte FOE entries have no meter word; this NPU-only stats path is
	 * unused on EN7523 (it returns early above when stats are disabled).
	 */
	meter = data;

	pse_port = FIELD_GET(AIROHA_FOE_IB2_PSE_PORT, *ib2);
	if (pse_port == FE_PSE_PORT_CDM4 ||
	    (airoha_is(ppe->eth, en7523) &&
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
		u32 *hwe = ppe->foe + sw_idx * sizeof(struct airoha_foe_entry);
		bool ppe2 = sw_idx >= ppe->eth->soc->ppe_sram_entries;
		struct airoha_eth *eth = ppe->eth;
		u32 val;
		int i;

		airoha_fe_wr(ppe->eth, REG_PPE_RAM_CTRL(ppe2),
			     FIELD_PREP(PPE_SRAM_CTRL_ENTRY_MASK, hw_idx) |
			     PPE_SRAM_CTRL_REQ_MASK);
		if (read_poll_timeout_atomic(airoha_fe_rr, val,
					     val & PPE_SRAM_CTRL_ACK_MASK,
					     10, 100, false, eth,
					     REG_PPE_RAM_CTRL(ppe2))) {
			dev_err(eth->dev, "Timeout reading PPE SRAM entry for hash %u\n", hw_idx);
			return NULL;
		}

		for (i = 0; i < sizeof(struct airoha_foe_entry) / sizeof(*hwe);
		     i++)
			hwe[i] = airoha_fe_rr(eth,
					      REG_PPE_RAM_ENTRY(ppe2, i));
	}

	return ppe->foe + sw_idx * sizeof(struct airoha_foe_entry);
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
	struct airoha_foe_entry *hwe = ppe->foe + sw_idx * sizeof(*hwe);
	bool ppe2 = sw_idx >= ppe->eth->soc->ppe_sram_entries;
	u32 hw_idx = airoha_ppe_sw_to_hw_idx(ppe, sw_idx);
	u32 *ptr = (u32 *)hwe, val;
	int i, err;

	for (i = 0; i < sizeof(*hwe) / sizeof(*ptr); i++)
		airoha_fe_wr(ppe->eth, REG_PPE_RAM_ENTRY(ppe2, i), ptr[i]);

	wmb();
	airoha_fe_wr(ppe->eth, REG_PPE_RAM_CTRL(ppe2),
		     FIELD_PREP(PPE_SRAM_CTRL_ENTRY_MASK, hw_idx) |
		     PPE_SRAM_CTRL_WR_MASK | PPE_SRAM_CTRL_REQ_MASK);

	err = read_poll_timeout_atomic(airoha_fe_rr, val,
					val & PPE_SRAM_CTRL_ACK_MASK,
					10, 100, false, ppe->eth,
					REG_PPE_RAM_CTRL(ppe2));
	if (err)
		dev_err(ppe->eth->dev, "Timeout committing SRAM entry hash %u\n", hw_idx);

	return err;
}

static int airoha_ppe_foe_commit_entry(struct airoha_ppe *ppe,
				       struct airoha_foe_entry *e,
				       u32 sw_idx, bool rx_wlan)
{
	u32 sram_num_entries = airoha_ppe_get_total_sram_num_entries(ppe);
	struct airoha_foe_entry *hwe = ppe->foe + sw_idx * sizeof(*hwe);
	u32 ts = airoha_ppe_get_timestamp(ppe);
	struct airoha_eth *eth = ppe->eth;
	struct airoha_npu *npu;
	int err = 0;

	memcpy(&hwe->d, &e->d, sizeof(*hwe) - sizeof(hwe->ib1));
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
		dev_err(ppe->eth->dev, "Failed to get locked entry for subflow commit (hash: %u)\n", sw_idx);
		return -EINVAL;
	}

	f = kzalloc(sizeof(*f), GFP_ATOMIC);
	if (!f) {
		dev_err(ppe->eth->dev, "OOM in subflow commit\n");
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

	if (airoha_is(ppe->eth, en7523)) {
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
		dev_err(ppe->eth->dev, "Error inserting L2 flow to rhashtable: %ld\n", PTR_ERR(prev));
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
	if (airoha_is(ppe->eth, en7523))
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
	int i, err = 0;

	dev_info(ppe->eth->dev, "Flushing %u SRAM entries\n", sram_num_entries);
	for (i = 0; i < sram_num_entries; i++) {
		memset(&hwe[i], 0, sizeof(*hwe));
		err = airoha_ppe_foe_commit_sram_entry(ppe, i);
		if (err) {
			dev_err(ppe->eth->dev, "failed to flush SRAM entry %d, err %d\n", i, err);
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
	struct airoha_eth *eth = ppe->eth;
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
		err = airoha_ppe_flow_offload_cmd(eth, type_data);

	mutex_unlock(&flow_offload_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(airoha_ppe_setup_tc_block_cb);

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
EXPORT_SYMBOL_GPL(airoha_ppe_check_skb);

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
EXPORT_SYMBOL_GPL(airoha_ppe_init_upd_mem);

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

	dev_info(dev, "Successfully retrieved PPE device\n");
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

	dev_info(eth->dev, "Starting PPE Initialization\n");
	ppe = devm_kzalloc(eth->dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe) {
		dev_err(eth->dev, "OOM allocating struct airoha_ppe\n");
		return -ENOMEM;
	}

	ppe->dev.ops.setup_tc_block_cb = airoha_ppe_setup_tc_block_cb;
	ppe->dev.ops.check_skb = airoha_ppe_check_skb;
	ppe->dev.priv = ppe;
	ppe->dev.enabled = true;
	ppe->eth = eth;
	INIT_HLIST_HEAD(&ppe->pending_flows);
	eth->ppe = ppe;

	ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);
	foe_size = ppe_num_entries * sizeof(struct airoha_foe_entry);
	ppe->foe = dmam_alloc_coherent(eth->dev, foe_size, &ppe->foe_dma,
				       GFP_KERNEL);
	if (!ppe->foe) {
		dev_err(eth->dev, "Failed to allocate DMA coherent memory for FOE entries (%d bytes)\n", foe_size);
		return -ENOMEM;
	}

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

	err = airoha_ppe_debugfs_init(ppe);
	if (err) {
		dev_err(eth->dev, "Failed to init PPE debugfs (err: %d)\n", err);
		goto error_l2_flow_table_destroy;
	}

	dev_info(eth->dev, "PPE Initialization completed successfully\n");
	return 0;

error_l2_flow_table_destroy:
	rhashtable_destroy(&ppe->l2_flows);
error_flow_table_destroy:
	rhashtable_destroy(&eth->flow_table);

	return err;
}
EXPORT_SYMBOL_GPL(airoha_ppe_init);

void airoha_ppe_deinit(struct airoha_eth *eth)
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

	rhashtable_destroy(&eth->ppe->l2_flows);
	rhashtable_destroy(&eth->flow_table);
	debugfs_remove(eth->ppe->debugfs_dir);
}
EXPORT_SYMBOL_GPL(airoha_ppe_deinit);

/* EcoNet EN751221 backend. */
#if IS_ENABLED(CONFIG_NET_ECONET_PPE)

#define EN75_PPE_ENTRIES		16384
#define EN75_FOE_ENTRY_SIZE		80
#define EN75_PPE_TABLE_SIZE		(EN75_PPE_ENTRIES * EN75_FOE_ENTRY_SIZE)
#define EN75_PPE_INVALID_HASH		0xffff
#define EN75_DPORT_GDMA1		1
#define EN75_DPORT_QDMA_HW		6

#define EN75_PPE_GLO_CFG		0x200
#define EN75_PPE_FLOW_CFG		0x204
#define EN75_PPE_IP_PROTO_CHK		0x208
#define EN75_PPE_TB_CFG			0x21c
#define EN75_PPE_TB_BASE		0x220
#define EN75_PPE_TB_USED		0x224
#define EN75_PPE_BIND_RATE		0x228
#define EN75_PPE_BIND_LIMIT0		0x22c
#define EN75_PPE_BIND_LIMIT1		0x230
#define EN75_PPE_UNBIND_AGE		0x238
#define EN75_PPE_BIND_AGE0		0x23c
#define EN75_PPE_BIND_AGE1		0x240
#define EN75_PPE_HASH_SEED		0x244
#define EN75_PPE_DEFAULT_CPU_PORT	0x248
#define EN75_PPE_CACHE_CTL		0x320
#define EN75_PPE_CAH_GATE		0x334

#define EN75_PPE_GLO_CFG_EN		BIT(0)
#define EN75_PPE_GLO_CFG_BUSY		BIT(31)
#define EN75_PPE_GLO_CFG_ON		(EN75_PPE_GLO_CFG_EN | BIT(2) | BIT(3) | BIT(9))

#define EN75_FE_TIMESTAMP		0x10
#define EN75_FE_CDMA1_VLAN_CTRL		0x400
#define EN75_FE_GDM1_FWD_CFG		0x500
#define EN75_FE_GDM1_VLAN_CHECK		0x510

#define EN75_FOE_IB1_BIND_TIMESTAMP	GENMASK(14, 0)
#define EN75_FOE_IB1_BIND_KEEPALIVE	BIT(15)
#define EN75_FOE_IB1_BIND_VLAN_LAYER	GENMASK(18, 16)
#define EN75_FOE_IB1_BIND_PPPOE	BIT(19)
#define EN75_FOE_IB1_BIND_VLAN_TAG	BIT(20)
#define EN75_FOE_IB1_BIND_CACHE	BIT(22)
#define EN75_FOE_IB1_BIND_TTL		BIT(24)
#define EN75_FOE_IB1_PACKET_TYPE	GENMASK(27, 25)
#define EN75_FOE_IB1_STATE		GENMASK(29, 28)
#define EN75_FOE_IB1_UDP		BIT(30)

#define EN75_FOE_IB2_QID		GENMASK(3, 0)
#define EN75_FOE_IB2_PSE_QOS		BIT(4)
#define EN75_FOE_IB2_DEST_PORT		GENMASK(7, 5)
#define EN75_FOE_IB2_MULTICAST		BIT(8)
#define EN75_FOE_IB2_PORT_MG		GENMASK(17, 12)
#define EN75_FOE_IB2_PORT_AG		GENMASK(23, 18)

#define EN75_PPE_CPU_REASON_NO_FLOW	0x07
#define EN75_PPE_CPU_REASON_HIT_UNBIND	0x0e
#define EN75_PPE_CPU_REASON_BIND_RATE	0x0f

#define EN75_PPE_PKT_TYPE_IPV4_HNAPT	0
#define EN75_FOE_STATE_INVALID		0
#define EN75_FOE_STATE_BIND		2

struct en75_foe_mac_info {
	u16 vlan1;
	u16 etype;
	u32 dest_mac_hi;
	u16 vlan2;
	u16 dest_mac_lo;
	u32 src_mac_hi;
	u16 pppoe_id;
	u16 src_mac_lo;
	u16 minfo;
	u16 winfo;
	u32 w3info;
	u32 amsdu;
};

struct en75_ipv4_tuple {
	u32 src_ip;
	u32 dest_ip;
	union {
		struct {
			u16 dest_port;
			u16 src_port;
		};
		u32 ports;
	};
};

struct en75_foe_ipv4 {
	struct en75_ipv4_tuple orig;
	u32 ib2;
	struct en75_ipv4_tuple new;
	u16 timestamp;
	u16 reserved[3];
	u32 udf_tsid;
	struct en75_foe_mac_info l2;
};

struct en75_foe_entry {
	u32 ib1;
	union {
		struct en75_foe_ipv4 ipv4;
		u32 data[19];
	};
};

static_assert(sizeof(struct en75_foe_entry) == EN75_FOE_ENTRY_SIZE);

struct en75_flow_entry {
	struct list_head list;
	struct en75_foe_entry data;
	unsigned long cookie;
	u32 src_ip;
	u32 dest_ip;
	u16 src_port;
	u16 dest_port;
	u16 hash;
};

struct en75_ppe {
	struct airoha_ppe_dev dev;
	struct device *parent;
	void __iomem *base;
	void __iomem *fe_base;
	void __iomem *foe_table;
	phys_addr_t foe_phys;
	/* Protects flows and FoE slot ownership in RX and TC paths. */
	spinlock_t lock;
	struct list_head flows;
	struct list_head block_cb_list;
	bool armed;
};

static void __iomem *en75_ppe_slot(struct en75_ppe *ppe, u16 hash)
{
	return ppe->foe_table + (size_t)hash * EN75_FOE_ENTRY_SIZE;
}

static void en75_ppe_read_entry(struct en75_ppe *ppe, u16 hash,
				struct en75_foe_entry *entry)
{
	void __iomem *slot = en75_ppe_slot(ppe, hash);
	int i;

	for (i = 0; i < ARRAY_SIZE(entry->data) + 1; i++)
		((u32 *)entry)[i] = readl(slot + i * sizeof(u32));
}

static bool en75_ppe_cache_cmd(struct en75_ppe *ppe, u32 cmd)
{
	u32 val;

	writel((cmd << 12) | BIT(8), ppe->base + EN75_PPE_CACHE_CTL);
	return !readl_poll_timeout_atomic(ppe->base + EN75_PPE_CACHE_CTL,
					 val, !(val & BIT(8)), 1, 100000);
}

static void en75_ppe_cache_clean(struct en75_ppe *ppe)
{
	u32 gate = readl(ppe->base + EN75_PPE_CAH_GATE);

	if (!(gate & BIT(0))) {
		writel(0x33, ppe->base + EN75_PPE_CAH_GATE);
		gate = readl(ppe->base + EN75_PPE_CAH_GATE);
	}

	writel(gate & ~BIT(0), ppe->base + EN75_PPE_CAH_GATE);
	en75_ppe_cache_cmd(ppe, 4);
	writel(gate | BIT(0), ppe->base + EN75_PPE_CAH_GATE);
}

static void en75_foe_entry_prepare(struct en75_foe_entry *entry, u8 l4proto,
				   u8 pse_port, const u8 *src_mac,
				   const u8 *dest_mac)
{
	u32 ib2;

	memset(entry, 0, sizeof(*entry));
	entry->ib1 = FIELD_PREP(EN75_FOE_IB1_STATE, EN75_FOE_STATE_BIND) |
		FIELD_PREP(EN75_FOE_IB1_PACKET_TYPE,
			   EN75_PPE_PKT_TYPE_IPV4_HNAPT) |
		FIELD_PREP(EN75_FOE_IB1_UDP, l4proto == IPPROTO_UDP) |
		EN75_FOE_IB1_BIND_CACHE | EN75_FOE_IB1_BIND_TTL;

	ib2 = FIELD_PREP(EN75_FOE_IB2_DEST_PORT, pse_port) |
		FIELD_PREP(EN75_FOE_IB2_PORT_MG, 0x3f) |
		FIELD_PREP(EN75_FOE_IB2_PORT_AG, 0x3f);
	if (is_multicast_ether_addr(dest_mac))
		ib2 |= EN75_FOE_IB2_MULTICAST;
	entry->ipv4.ib2 = ib2;

	entry->ipv4.l2.dest_mac_hi = get_unaligned_be32(dest_mac);
	entry->ipv4.l2.dest_mac_lo = get_unaligned_be16(dest_mac + 4);
	entry->ipv4.l2.src_mac_hi = get_unaligned_be32(src_mac);
	entry->ipv4.l2.src_mac_lo = get_unaligned_be16(src_mac + 4);
	entry->ipv4.l2.etype = ETH_P_IP;
}

static void en75_foe_entry_set_ipv4_tuple(struct en75_foe_entry *entry,
					  bool egress, __be32 src_addr,
					  __be16 src_port, __be32 dest_addr,
					  __be16 dest_port)
{
	struct en75_ipv4_tuple *tuple = egress ? &entry->ipv4.new :
						      &entry->ipv4.orig;

	tuple->src_ip = be32_to_cpu(src_addr);
	tuple->dest_ip = be32_to_cpu(dest_addr);
	tuple->src_port = be16_to_cpu(src_port);
	tuple->dest_port = be16_to_cpu(dest_port);
}

static void en75_foe_entry_set_pse_port(struct en75_foe_entry *entry, u8 port)
{
	entry->ipv4.ib2 &= ~(EN75_FOE_IB2_DEST_PORT | EN75_FOE_IB2_PSE_QOS);
	entry->ipv4.ib2 |= FIELD_PREP(EN75_FOE_IB2_DEST_PORT, port);
	if (port == EN75_DPORT_QDMA_HW)
		entry->ipv4.ib2 |= EN75_FOE_IB2_PSE_QOS;
}

static void en75_foe_entry_set_queue(struct en75_foe_entry *entry, u8 queue)
{
	entry->ipv4.ib2 &= ~EN75_FOE_IB2_QID;
	entry->ipv4.ib2 |= FIELD_PREP(EN75_FOE_IB2_QID, queue);
}

static void en75_foe_entry_set_dsa(struct en75_foe_entry *entry, int port,
				   bool passthrough)
{
	struct en75_foe_mac_info *l2 = &entry->ipv4.l2;

	l2->etype = BIT(port) & GENMASK(5, 0);
	if (passthrough)
		l2->etype |= BIT(7);
	if (!(entry->ib1 & EN75_FOE_IB1_BIND_VLAN_LAYER))
		entry->ib1 |= FIELD_PREP(EN75_FOE_IB1_BIND_VLAN_LAYER, 1);
	else
		l2->etype |= BIT(8);
	l2->vlan1 = 0;
	entry->ib1 &= ~EN75_FOE_IB1_BIND_VLAN_TAG;
}

static void en75_foe_entry_set_pppoe(struct en75_foe_entry *entry, u16 sid)
{
	struct en75_foe_mac_info *l2 = &entry->ipv4.l2;

	if (!(entry->ib1 & EN75_FOE_IB1_BIND_VLAN_LAYER) ||
	    (entry->ib1 & EN75_FOE_IB1_BIND_VLAN_TAG))
		l2->etype = ETH_P_PPP_SES;
	entry->ib1 |= EN75_FOE_IB1_BIND_PPPOE;
	l2->pppoe_id = sid;
}

static void en75_foe_entry_set_bind_metadata(struct en75_foe_entry *entry)
{
	u8 port = FIELD_GET(EN75_FOE_IB2_DEST_PORT, entry->ipv4.ib2);

	entry->ib1 &= ~EN75_FOE_IB1_BIND_VLAN_LAYER;
	entry->ib1 |= FIELD_PREP(EN75_FOE_IB1_BIND_VLAN_LAYER, 1);
	entry->ipv4.udf_tsid &= ~0xff;
	entry->ipv4.udf_tsid |= port + 6;
}

static void en75_ppe_commit_entry(struct en75_ppe *ppe,
				  struct en75_foe_entry *entry, u16 hash)
{
	void __iomem *slot = en75_ppe_slot(ppe, hash);
	u16 timestamp = readl(ppe->fe_base + EN75_FE_TIMESTAMP) &
			EN75_FOE_IB1_BIND_TIMESTAMP;
	const u32 *src;
	int i;

	entry->ib1 &= ~EN75_FOE_IB1_BIND_TIMESTAMP;
	entry->ib1 |= FIELD_PREP(EN75_FOE_IB1_BIND_TIMESTAMP, timestamp) |
		EN75_FOE_IB1_BIND_CACHE | EN75_FOE_IB1_BIND_TTL |
		EN75_FOE_IB1_BIND_KEEPALIVE;
	en75_foe_entry_set_bind_metadata(entry);

	/*
	 * EN751221 is normally used by a big-endian MIPS CPU while the PPE
	 * consumes little-endian words. Mixed-u16 words need an additional
	 * half-word exchange to preserve the vendor FoE V1 layout.
	 */
	src = entry->data;
	for (i = 0; i < ARRAY_SIZE(entry->data); i++) {
		u32 val = src[i];

		if (i == 6 || i == 10 || i == 12 || i == 14)
			val = rol32(val, 16);
		writel(swab32(val), slot + sizeof(u32) * (i + 1));
	}
	/* Publish the rewrite payload before marking the entry bound. */
	wmb();
	writel(swab32(entry->ib1), slot);
	/* Publish the complete entry before invalidating the lookup cache. */
	wmb();
	en75_ppe_cache_clean(ppe);
}

static void en75_ppe_invalidate_entry(struct en75_ppe *ppe, u16 hash)
{
	if (hash == EN75_PPE_INVALID_HASH || hash >= EN75_PPE_ENTRIES)
		return;
	writel(0, en75_ppe_slot(ppe, hash));
	/* Publish the invalid state before the caller clears the cache. */
	wmb();
}

static bool en75_ppe_parse_ipv4_tuple(struct sk_buff *skb, u32 *src_ip,
				      u32 *dest_ip, u16 *src_port,
				      u16 *dest_port)
{
	const u8 *data = skb->data;
	unsigned int len = skb->len;
	const u8 *iph, *l4;
	unsigned int proto_off = 12, off;
	u16 proto;
	u8 ihl;

	if (len < ETH_HLEN)
		return false;

	proto = get_unaligned_be16(data + proto_off);
	if (proto == ETH_P_8021Q || proto == ETH_P_8021AD) {
		if (len < ETH_HLEN + VLAN_HLEN)
			return false;
		proto_off += VLAN_HLEN;
		proto = get_unaligned_be16(data + proto_off);
	} else if (proto != ETH_P_IP && proto != ETH_P_PPP_SES) {
		/* MTK special tag occupies the ethertype/TCI position. */
		if (len < ETH_HLEN + 4)
			return false;
		proto_off += 4;
		proto = get_unaligned_be16(data + proto_off);
	}
	off = proto_off + sizeof(__be16);

	if (proto == ETH_P_PPP_SES) {
		if (len < proto_off + 10 ||
		    get_unaligned_be16(data + proto_off + 8) != PPP_IP)
			return false;
		off = proto_off + 10;
		proto = ETH_P_IP;
	}
	if (proto != ETH_P_IP || len < off + sizeof(struct iphdr))
		return false;

	iph = data + off;
	if ((iph[0] >> 4) != 4 ||
	    (iph[9] != IPPROTO_TCP && iph[9] != IPPROTO_UDP))
		return false;
	ihl = (iph[0] & 0x0f) * 4;
	if (ihl < sizeof(struct iphdr) || len < off + ihl + 4)
		return false;

	l4 = iph + ihl;
	*src_ip = get_unaligned_be32(iph + offsetof(struct iphdr, saddr));
	*dest_ip = get_unaligned_be32(iph + offsetof(struct iphdr, daddr));
	*src_port = get_unaligned_be16(l4);
	*dest_port = get_unaligned_be16(l4 + 2);
	return true;
}

static void en75_ppe_rx_check(struct en75_ppe *ppe, struct sk_buff *skb,
			      u16 hash, u8 reason)
{
	struct en75_flow_entry *flow;
	struct en75_foe_entry entry, hardware;
	u32 src_ip, dest_ip;
	u16 src_port, dest_port;

	if (!ppe || !READ_ONCE(ppe->armed) || hash >= EN75_PPE_ENTRIES)
		return;
	if (reason != EN75_PPE_CPU_REASON_NO_FLOW &&
	    reason != EN75_PPE_CPU_REASON_HIT_UNBIND &&
	    reason != EN75_PPE_CPU_REASON_BIND_RATE)
		return;
	if (!en75_ppe_parse_ipv4_tuple(skb, &src_ip, &dest_ip,
				       &src_port, &dest_port))
		return;

	spin_lock_bh(&ppe->lock);
	list_for_each_entry(flow, &ppe->flows, list) {
		if (flow->src_ip != src_ip || flow->dest_ip != dest_ip ||
		    flow->src_port != src_port || flow->dest_port != dest_port)
			continue;
		if (flow->hash == hash)
			break;

		entry = flow->data;
		en75_ppe_read_entry(ppe, hash, &hardware);
		if (hardware.ib1) {
			/*
			 * Preserve the exact key generated by the hardware. The commit
			 * path byte-swaps every word, therefore pre-swap these raw words
			 * so they round-trip unchanged.
			 */
			entry.ipv4.orig.src_ip = swab32(hardware.ipv4.orig.src_ip);
			entry.ipv4.orig.dest_ip = swab32(hardware.ipv4.orig.dest_ip);
			entry.ipv4.orig.ports = swab32(hardware.ipv4.orig.ports);
		}
		if (flow->hash != EN75_PPE_INVALID_HASH)
			en75_ppe_invalidate_entry(ppe, flow->hash);
		flow->hash = hash;
		en75_ppe_commit_entry(ppe, &entry, hash);
		break;
	}
	spin_unlock_bh(&ppe->lock);
}

static void en75_ppe_check_skb_reason(struct airoha_ppe_dev *ppe_dev,
				      struct sk_buff *skb, u16 hash, u8 reason)
{
	struct en75_ppe *ppe = ppe_dev->priv;

	en75_ppe_rx_check(ppe, skb, hash, reason);
}

static void en75_ppe_flush(struct en75_ppe *ppe)
{
	struct en75_flow_entry *flow, *tmp;
	LIST_HEAD(free_list);

	spin_lock_bh(&ppe->lock);
	list_for_each_entry(flow, &ppe->flows, list)
		en75_ppe_invalidate_entry(ppe, flow->hash);
	list_splice_init(&ppe->flows, &free_list);
	spin_unlock_bh(&ppe->lock);

	list_for_each_entry_safe(flow, tmp, &free_list, list) {
		list_del(&flow->list);
		kfree(flow);
	}
	memset_io(ppe->foe_table, 0, EN75_PPE_TABLE_SIZE);
	/* Publish the cleared table before invalidating the lookup cache. */
	wmb();
	en75_ppe_cache_clean(ppe);
}

static int en75_ppe_engine_set(struct en75_ppe *ppe, bool enable)
{
	u32 val;

	if (enable) {
		if (readl_poll_timeout_atomic(ppe->base + EN75_PPE_GLO_CFG, val,
					      !(val & EN75_PPE_GLO_CFG_BUSY),
					      10, 10000))
			return -EBUSY;
		writel(EN75_PPE_GLO_CFG_ON, ppe->base + EN75_PPE_GLO_CFG);
	} else {
		val = readl(ppe->base + EN75_PPE_GLO_CFG);
		writel(val & ~EN75_PPE_GLO_CFG_EN,
		       ppe->base + EN75_PPE_GLO_CFG);
	}
	return 0;
}

static int en75_ppe_engine_arm(struct en75_ppe *ppe)
{
	int err;

	if (!ppe)
		return -ENODEV;
	if (READ_ONCE(ppe->armed))
		return 0;

	writel(0x0000ffbc, ppe->fe_base + 0xe1c);
	writel(0x01010001, ppe->fe_base + 0xe34);
	err = en75_ppe_engine_set(ppe, true);
	if (err)
		return err;
	writel(0x33, ppe->base + EN75_PPE_CAH_GATE);
	writel(0x0600f700, ppe->fe_base + 0xe04);
	writel(0x00008100, ppe->fe_base + 0xf18);
	writel(0x81000001, ppe->fe_base + EN75_FE_CDMA1_VLAN_CTRL);
	writel(0x00000001, ppe->fe_base + EN75_FE_GDM1_VLAN_CHECK);
	writel(0x03f04004, ppe->fe_base + EN75_FE_GDM1_FWD_CFG);
	writel(0x00000500, ppe->fe_base + 0xe48);
	WRITE_ONCE(ppe->armed, true);
	dev_info(ppe->parent, "PPE hardware flow offload enabled\n");
	return 0;
}

static void en75_ppe_engine_disarm(struct en75_ppe *ppe)
{
	if (!ppe || !READ_ONCE(ppe->armed))
		return;

	writel(0x03f00000, ppe->fe_base + EN75_FE_GDM1_FWD_CFG);
	WRITE_ONCE(ppe->armed, false);
	en75_ppe_flush(ppe);
	en75_ppe_engine_set(ppe, false);
	dev_info(ppe->parent, "PPE hardware flow offload disabled\n");
}

static void en75_flow_mangle_eth(const struct flow_action_entry *act,
				 void *header)
{
	u8 *dest = header + act->mangle.offset;
	const u8 *src = (const u8 *)&act->mangle.val;

	if (act->mangle.offset > 8)
		return;
	if (!act->mangle.mask)
		memcpy(dest, src, sizeof(u32));
	else if (act->mangle.mask == 0xffff0000)
		memcpy(dest, src + 2, sizeof(u16));
	else if (act->mangle.mask == 0x0000ffff)
		memcpy(dest + 2, src, sizeof(u16));
}

static int en75_flow_mangle_ports(const struct flow_action_entry *act,
				  __be16 *src_port, __be16 *dest_port)
{
	u32 val = ntohl(act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if (act->mangle.mask == ~htonl(0xffff))
			*dest_port = cpu_to_be16(val);
		else
			*src_port = cpu_to_be16(val >> 16);
		return 0;
	case 2:
		*dest_port = cpu_to_be16(val);
		return 0;
	default:
		return -EINVAL;
	}
}

static int en75_flow_mangle_ipv4(const struct flow_action_entry *act,
				 __be32 *src_addr, __be32 *dest_addr)
{
	__be32 *dest;

	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		dest = src_addr;
		break;
	case offsetof(struct iphdr, daddr):
		dest = dest_addr;
		break;
	default:
		return -EINVAL;
	}
	memcpy(dest, &act->mangle.val, sizeof(*dest));
	return 0;
}

static struct en75_ppe *en75_ppe_from_netdev(struct net_device *netdev)
{
	struct airoha_gdm_common *gdm;

	gdm = airoha_gdm_common_from_netdev(netdev);
	if (!gdm || gdm->family != AIROHA_ETH_FAMILY_ECONET || !gdm->ppe)
		return NULL;

	return gdm->ppe->priv;
}

static int en75_flow_set_output(struct en75_foe_entry *entry,
				struct net_device *odev)
{
	struct airoha_gdm_common *gdm;
	struct dsa_port *dp;
	int dsa_port = -1;

	if (!odev)
		return -EOPNOTSUPP;

	if (dsa_user_dev_check(odev)) {
		dp = dsa_port_from_netdev(odev);
		if (IS_ERR(dp))
			return PTR_ERR(dp);
		dsa_port = dp->index;
		odev = dsa_port_to_conduit(dp);
		en75_foe_entry_set_dsa(entry, dsa_port, dp->ds->index != 0);
	}

	if (!odev)
		return -EOPNOTSUPP;

	gdm = airoha_gdm_common_from_netdev(odev);
	if (!gdm || gdm->family != AIROHA_ETH_FAMILY_ECONET)
		return -EOPNOTSUPP;

	en75_foe_entry_set_pse_port(entry, gdm->pse_port);
	if (dsa_port >= 0)
		en75_foe_entry_set_queue(entry, 3 + dsa_port);

	return 0;
}

static struct en75_flow_entry *
en75_ppe_find_flow(struct en75_ppe *ppe, unsigned long cookie)
{
	struct en75_flow_entry *flow;

	list_for_each_entry(flow, &ppe->flows, list)
		if (flow->cookie == cookie)
			return flow;
	return NULL;
}

static int en75_flow_offload_replace(struct net_device *dev,
				     struct flow_cls_offload *cls)
{
	struct en75_ppe *ppe = en75_ppe_from_netdev(dev);
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct flow_match_basic basic;
	struct flow_match_ipv4_addrs addrs;
	struct flow_match_ports ports;
	struct flow_action_entry *act;
	struct en75_flow_entry *flow;
	struct en75_foe_entry entry;
	struct ethhdr eth = {};
	struct net_device *odev = NULL;
	__be32 src_addr, dest_addr, new_src_addr, new_dest_addr;
	__be16 src_port, dest_port, new_src_port, new_dest_port;
	u16 pppoe_sid = 0;
	u8 l4proto;
	int i, err;

	if (!ppe)
		return -EOPNOTSUPP;
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "EcoNet PPE supports IPv4 TCP/UDP flows only");
		return -EOPNOTSUPP;
	}

	flow_rule_match_basic(rule, &basic);
	l4proto = basic.key->ip_proto;
	if (basic.key->n_proto != htons(ETH_P_IP) ||
	    (l4proto != IPPROTO_TCP && l4proto != IPPROTO_UDP))
		return -EOPNOTSUPP;

	flow_rule_match_ipv4_addrs(rule, &addrs);
	flow_rule_match_ports(rule, &ports);
	if (basic.mask->n_proto != htons(0xffff) ||
	    basic.mask->ip_proto != 0xff ||
	    addrs.mask->src != htonl(0xffffffff) ||
	    addrs.mask->dst != htonl(0xffffffff) ||
	    ports.mask->src != htons(0xffff) ||
	    ports.mask->dst != htons(0xffff)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "EcoNet PPE requires exact IPv4 5-tuple matches");
		return -EOPNOTSUPP;
	}
	if (!flow_action_basic_hw_stats_check(&rule->action,
					      cls->common.extack))
		return -EOPNOTSUPP;

	src_addr = addrs.key->src;
	dest_addr = addrs.key->dst;
	src_port = ports.key->src;
	dest_port = ports.key->dst;
	new_src_addr = src_addr;
	new_dest_addr = dest_addr;
	new_src_port = src_port;
	new_dest_port = dest_port;

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				en75_flow_mangle_eth(act, &eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				err = en75_flow_mangle_ipv4(act, &new_src_addr,
							    &new_dest_addr);
				if (err)
					return err;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				err = en75_flow_mangle_ports(act, &new_src_port,
							     &new_dest_port);
				if (err)
					return err;
				break;
			default:
				return -EOPNOTSUPP;
			}
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
					   "unsupported action for EcoNet PPE");
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(eth.h_source) ||
	    !is_valid_ether_addr(eth.h_dest)) {
		NL_SET_ERR_MSG_MOD(cls->common.extack,
				   "missing Ethernet rewrite addresses");
		return -EOPNOTSUPP;
	}

	en75_foe_entry_prepare(&entry, l4proto, EN75_DPORT_GDMA1,
			       eth.h_source, eth.h_dest);
	en75_foe_entry_set_ipv4_tuple(&entry, false, src_addr, src_port,
				      dest_addr, dest_port);
	en75_foe_entry_set_ipv4_tuple(&entry, true, new_src_addr, new_src_port,
				      new_dest_addr, new_dest_port);
	err = en75_flow_set_output(&entry, odev);
	if (err)
		return err;
	if (pppoe_sid)
		en75_foe_entry_set_pppoe(&entry, pppoe_sid);

	/*
	 * The engine's lookup key uses the opposite word lane order on the
	 * big-endian EN751221 CPU. The RX bind path will replace it with the
	 * hardware-generated key when that key is visible in DRAM.
	 */
	entry.ipv4.orig.src_ip = swab32(entry.ipv4.orig.src_ip);
	entry.ipv4.orig.dest_ip = swab32(entry.ipv4.orig.dest_ip);
	entry.ipv4.orig.ports = swab32(entry.ipv4.orig.ports);

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return -ENOMEM;
	flow->data = entry;
	flow->cookie = cls->cookie;
	flow->src_ip = ntohl(src_addr);
	flow->dest_ip = ntohl(dest_addr);
	flow->src_port = ntohs(src_port);
	flow->dest_port = ntohs(dest_port);
	flow->hash = EN75_PPE_INVALID_HASH;

	spin_lock_bh(&ppe->lock);
	if (en75_ppe_find_flow(ppe, cls->cookie)) {
		spin_unlock_bh(&ppe->lock);
		kfree(flow);
		return -EEXIST;
	}
	list_add_tail(&flow->list, &ppe->flows);
	spin_unlock_bh(&ppe->lock);
	return 0;
}

static int en75_flow_offload_destroy(struct net_device *dev,
				     struct flow_cls_offload *cls)
{
	struct en75_ppe *ppe = en75_ppe_from_netdev(dev);
	struct en75_flow_entry *flow;

	if (!ppe)
		return -EOPNOTSUPP;
	spin_lock_bh(&ppe->lock);
	flow = en75_ppe_find_flow(ppe, cls->cookie);
	if (flow) {
		en75_ppe_invalidate_entry(ppe, flow->hash);
		en75_ppe_cache_clean(ppe);
		list_del(&flow->list);
	}
	spin_unlock_bh(&ppe->lock);
	kfree(flow);
	return 0;
}

static int en75_flow_offload_stats(struct flow_cls_offload *cls)
{
	flow_stats_update(&cls->stats, 0, 0, 0, jiffies,
			  FLOW_ACTION_HW_STATS_DELAYED);
	return 0;
}

static int en75_flow_offload_cmd(struct net_device *dev,
				 struct flow_cls_offload *cls)
{
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return en75_flow_offload_replace(dev, cls);
	case FLOW_CLS_DESTROY:
		return en75_flow_offload_destroy(dev, cls);
	case FLOW_CLS_STATS:
		return en75_flow_offload_stats(cls);
	default:
		return -EOPNOTSUPP;
	}
}

static int en75_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	struct net_device *dev = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;
	return en75_flow_offload_cmd(dev, type_data);
}

static int en75_setup_tc_block(struct net_device *dev,
			       struct flow_block_offload *offload)
{
	struct en75_ppe *ppe = en75_ppe_from_netdev(dev);
	flow_setup_cb_t *cb = en75_setup_tc_block_cb;
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
		err = en75_ppe_engine_arm(ppe);
		if (err)
			return err;
		block_cb = flow_block_cb_alloc(cb, dev, dev, NULL);
		if (IS_ERR(block_cb)) {
			if (list_empty(&ppe->block_cb_list))
				en75_ppe_engine_disarm(ppe);
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
			en75_ppe_engine_disarm(ppe);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int en75_ppe_setup_tc(struct airoha_ppe_dev *ppe_dev,
			     struct net_device *dev,
			      enum tc_setup_type type, void *type_data)
{
	if (en75_ppe_from_netdev(dev) != ppe_dev->priv)
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return en75_setup_tc_block(dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

struct airoha_ppe_dev *
airoha_ppe_econet_init(struct device *dev, struct device_node *np,
		       void __iomem *fe_base, void __iomem *ppe_base)
{
	struct device_node *mem_np;
	struct reserved_mem *rmem;
	struct en75_ppe *ppe;
	int mem_index;
	u32 tb_cfg;

	mem_index = of_property_match_string(np, "memory-region-names",
					     "ppe-foe");
	if (mem_index < 0) {
		/* Keep compatibility with the original single-region binding, but
		 * never mistake named QDMA buffers for the FoE table.
		 */
		if (of_find_property(np, "memory-region-names", NULL)) {
			dev_info(dev, "PPE disabled: no ppe-foe memory-region\n");
			return NULL;
		}
		mem_index = 0;
	}

	mem_np = of_parse_phandle(np, "memory-region", mem_index);
	if (!mem_np) {
		dev_info(dev, "PPE disabled: no memory-region for the FoE table\n");
		return NULL;
	}
	rmem = of_reserved_mem_lookup(mem_np);
	of_node_put(mem_np);
	if (!rmem)
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "invalid PPE memory-region\n"));
	if (rmem->size < EN75_PPE_TABLE_SIZE)
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "PPE FoE region is too small (%pa bytes)\n",
					     &rmem->size));
	if (upper_32_bits(rmem->base))
		return ERR_PTR(dev_err_probe(dev, -ERANGE,
					     "PPE FoE table must be below 4 GiB\n"));

	ppe = devm_kzalloc(dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return ERR_PTR(-ENOMEM);
	ppe->foe_table = devm_ioremap(dev, rmem->base, EN75_PPE_TABLE_SIZE);
	if (!ppe->foe_table)
		return ERR_PTR(dev_err_probe(dev, -ENOMEM,
					     "failed to map PPE FoE table\n"));

	ppe->parent = dev;
	ppe->base = ppe_base;
	ppe->fe_base = fe_base;
	ppe->foe_phys = rmem->base;
	spin_lock_init(&ppe->lock);
	INIT_LIST_HEAD(&ppe->flows);
	INIT_LIST_HEAD(&ppe->block_cb_list);
	memset_io(ppe->foe_table, 0, EN75_PPE_TABLE_SIZE);

	writel(lower_32_bits(ppe->foe_phys), ppe->base + EN75_PPE_TB_BASE);
	tb_cfg = GENMASK(11, 7) |
		 FIELD_PREP(GENMASK(5, 4), 3) |
		 FIELD_PREP(GENMASK(15, 14), 3) |
		 BIT(16) | FIELD_PREP(GENMASK(2, 0), 4) | BIT(3);
	writel(tb_cfg, ppe->base + EN75_PPE_TB_CFG);
	writel(0xffffffff, ppe->base + EN75_PPE_IP_PROTO_CHK);
	writel(BIT(0), ppe->base + EN75_PPE_CACHE_CTL);
	writel(BIT(8) | BIT(9) | BIT(10) | BIT(12) | BIT(13) | BIT(17),
	       ppe->base + EN75_PPE_FLOW_CFG);
	writel((1000 << 16) | 3, ppe->base + EN75_PPE_UNBIND_AGE);
	writel(12 | BIT(16), ppe->base + EN75_PPE_BIND_AGE0);
	writel(7 | BIT(16), ppe->base + EN75_PPE_BIND_AGE1);
	writel(GENMASK(13, 0) | GENMASK(29, 16),
	       ppe->base + EN75_PPE_BIND_LIMIT0);
	writel(GENMASK(13, 0) | BIT(16),
	       ppe->base + EN75_PPE_BIND_LIMIT1);
	writel(30 | BIT(16), ppe->base + EN75_PPE_BIND_RATE);
	writel(0x12345678, ppe->base + EN75_PPE_HASH_SEED);
	writel(0, ppe->base + EN75_PPE_DEFAULT_CPU_PORT);
	writel(readl(ppe->base + EN75_PPE_GLO_CFG) & ~EN75_PPE_GLO_CFG_EN,
	       ppe->base + EN75_PPE_GLO_CFG);

	ppe->dev.priv = ppe;
	ppe->dev.enabled = true;
	ppe->dev.ops.check_skb_reason = en75_ppe_check_skb_reason;
	ppe->dev.ops.setup_tc = en75_ppe_setup_tc;

	dev_info(dev, "PPE FoE table at %pa, %u entries\n",
		 &rmem->base, EN75_PPE_ENTRIES);
	return &ppe->dev;
}
EXPORT_SYMBOL_GPL(airoha_ppe_econet_init);

void airoha_ppe_econet_deinit(struct airoha_ppe_dev *ppe_dev)
{
	struct en75_ppe *ppe;

	if (!ppe_dev)
		return;

	ppe = ppe_dev->priv;
	if (READ_ONCE(ppe->armed))
		en75_ppe_engine_disarm(ppe);
	else
		en75_ppe_flush(ppe);
	ppe_dev->enabled = false;
}
EXPORT_SYMBOL_GPL(airoha_ppe_econet_deinit);

#endif /* CONFIG_NET_ECONET_PPE */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha and EcoNet PPE flow offload");
