// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include "airoha_eth.h"
#include "airoha_regs.h"

static void airoha_debugfs_ppe_print_tuple(struct seq_file *m,
					   void *src_addr, void *dest_addr,
					   u16 *src_port, u16 *dest_port,
					   bool ipv6)
{
	__be32 n_addr[IPV6_ADDR_WORDS];

	if (ipv6) {
		ipv6_addr_cpu_to_be32(n_addr, src_addr);
		seq_printf(m, "%pI6", n_addr);
	} else {
		seq_printf(m, "%pI4h", src_addr);
	}
	if (src_port)
		seq_printf(m, ":%d", *src_port);

	seq_puts(m, "->");

	if (ipv6) {
		ipv6_addr_cpu_to_be32(n_addr, dest_addr);
		seq_printf(m, "%pI6", n_addr);
	} else {
		seq_printf(m, "%pI4h", dest_addr);
	}
	if (dest_port)
		seq_printf(m, ":%d", *dest_port);
}

static void airoha_debugfs_ppe_print_tuple_json(struct seq_file *m,
					   const char *prefix,
					   void *src_addr, void *dest_addr,
					   u16 *src_port, u16 *dest_port,
					   bool ipv6)
{
	__be32 n_addr[IPV6_ADDR_WORDS];

	if (ipv6) {
		ipv6_addr_cpu_to_be32(n_addr, src_addr);
		seq_printf(m, "\"%s_src\":\"%pI6\",", prefix, n_addr);
		ipv6_addr_cpu_to_be32(n_addr, dest_addr);
		seq_printf(m, "\"%s_dest\":\"%pI6\"", prefix, n_addr);
	} else {
		seq_printf(m, "\"%s_src\":\"%pI4h\",", prefix, src_addr);
		seq_printf(m, "\"%s_dest\":\"%pI4h\"", prefix, dest_addr);
	}

	if (src_port)
		seq_printf(m, ",\"%s_src_port\":%d", prefix, *src_port);
	if (dest_port)
		seq_printf(m, ",\"%s_dest_port\":%d", prefix, *dest_port);
}

static int airoha_ppe_debugfs_foe_json_show(struct seq_file *m, void *private)
{
	static const char *const ppe_type_str[] = {
		[PPE_PKT_TYPE_IPV4_HNAPT] = "IPv4 5T",
		[PPE_PKT_TYPE_IPV4_ROUTE] = "IPv4 3T",
		[PPE_PKT_TYPE_BRIDGE] = "L2B",
		[PPE_PKT_TYPE_IPV4_DSLITE] = "DS-LITE",
		[PPE_PKT_TYPE_IPV6_ROUTE_3T] = "IPv6 3T",
		[PPE_PKT_TYPE_IPV6_ROUTE_5T] = "IPv6 5T",
		[PPE_PKT_TYPE_IPV6_6RD] = "6RD",
	};
	static const char *const ppe_state_str[] = {
		[AIROHA_FOE_STATE_INVALID] = "Invalid",
		[AIROHA_FOE_STATE_UNBIND] = "Unbind",
		[AIROHA_FOE_STATE_BIND] = "Bind",
		[AIROHA_FOE_STATE_FIN] = "FIN",
	};
	struct airoha_ppe *ppe = m->private;
	u32 ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);
	bool first_entry = true;
	int i;

	seq_puts(m, "[\n");

	for (i = 0; i < ppe_num_entries; i++) {
		const char *state_str, *type_str = "UNKNOWN";
		void *src_addr = NULL, *dest_addr = NULL;
		u16 *src_port = NULL, *dest_port = NULL;
		struct airoha_foe_mac_info_common *l2;
		unsigned char h_source[ETH_ALEN] = {};
		struct airoha_foe_stats64 stats = {};
		unsigned char h_dest[ETH_ALEN];
		struct airoha_foe_entry *hwe;
		u32 type, state, ib2, data;
		bool ipv6 = false;

		hwe = airoha_ppe_foe_get_entry(ppe, i);
		if (!hwe)
			continue;

		state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, hwe->ib1);
		if (!state)
			continue;

		state_str = ppe_state_str[state % ARRAY_SIZE(ppe_state_str)];
		type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe->ib1);
		if (type < ARRAY_SIZE(ppe_type_str) && ppe_type_str[type])
			type_str = ppe_type_str[type];

		if (!first_entry)
			seq_puts(m, ",\n");
		first_entry = false;

		seq_printf(m, "  {\"index\":%d,\"state\":\"%s\",\"type\":\"%s\"",
			   i, state_str, type_str);

		switch (type) {
		case PPE_PKT_TYPE_IPV4_HNAPT:
		case PPE_PKT_TYPE_IPV4_DSLITE:
			src_port = &hwe->ipv4.orig_tuple.src_port;
			dest_port = &hwe->ipv4.orig_tuple.dest_port;
			fallthrough;
		case PPE_PKT_TYPE_IPV4_ROUTE:
			src_addr = &hwe->ipv4.orig_tuple.src_ip;
			dest_addr = &hwe->ipv4.orig_tuple.dest_ip;
			break;
		case PPE_PKT_TYPE_IPV6_ROUTE_5T:
			src_port = &hwe->ipv6.src_port;
			dest_port = &hwe->ipv6.dest_port;
			fallthrough;
		case PPE_PKT_TYPE_IPV6_ROUTE_3T:
		case PPE_PKT_TYPE_IPV6_6RD:
			src_addr = &hwe->ipv6.src_ip;
			dest_addr = &hwe->ipv6.dest_ip;
			ipv6 = true;
			break;
		default:
			break;
		}

		if (src_addr && dest_addr) {
			seq_puts(m, ",");
			airoha_debugfs_ppe_print_tuple_json(m, "orig", src_addr, dest_addr,
							    src_port, dest_port, ipv6);
		}

		switch (type) {
		case PPE_PKT_TYPE_IPV4_HNAPT:
		case PPE_PKT_TYPE_IPV4_DSLITE:
			src_port = &hwe->ipv4.new_tuple.src_port;
			dest_port = &hwe->ipv4.new_tuple.dest_port;
			fallthrough;
		case PPE_PKT_TYPE_IPV4_ROUTE:
			src_addr = &hwe->ipv4.new_tuple.src_ip;
			dest_addr = &hwe->ipv4.new_tuple.dest_ip;
			seq_puts(m, ",");
			airoha_debugfs_ppe_print_tuple_json(m, "new", src_addr, dest_addr,
							    src_port, dest_port, ipv6);
			break;
		default:
			break;
		}

		if (type == PPE_PKT_TYPE_BRIDGE) {
			data = hwe->bridge.data;
			ib2 = hwe->bridge.ib2;
			l2 = &hwe->bridge.l2.common;
			*((__be16 *)&h_source[4]) = cpu_to_be16(hwe->bridge.l2.src_mac_lo);
		} else if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
			data = hwe->ipv6.data;
			ib2 = hwe->ipv6.ib2;
			l2 = &hwe->ipv6.l2;
			*((__be16 *)&h_source[4]) = 0;
		} else {
			data = hwe->ipv4.data;
			ib2 = hwe->ipv4.ib2;
			l2 = &hwe->ipv4.l2.common;
			*((__be16 *)&h_source[4]) = cpu_to_be16(hwe->ipv4.l2.src_mac_lo);
		}

		airoha_ppe_foe_entry_get_stats(ppe, i, &stats);

		*((__be32 *)h_dest) = cpu_to_be32(l2->dest_mac_hi);
		*((__be16 *)&h_dest[4]) = cpu_to_be16(l2->dest_mac_lo);
		*((__be32 *)h_source) = cpu_to_be32(l2->src_mac_hi);

		seq_printf(m, ",\"eth_src\":\"%pM\",\"eth_dest\":\"%pM\",\"etype\":\"0x%04x\",\"data\":\"0x%08x\","
			      "\"vlan1\":%d,\"vlan2\":%d,\"ib1\":\"0x%08x\",\"ib2\":\"0x%08x\","
			      "\"packets\":%llu,\"bytes\":%llu}",
			   h_source, h_dest, l2->etype, data,
			   l2->vlan1, l2->vlan2, hwe->ib1, ib2,
			   stats.packets, stats.bytes);
	}

	seq_puts(m, "\n]\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_ppe_debugfs_foe_json);

static int airoha_ppe_debugfs_foe_show(struct seq_file *m, void *private,
				       bool bind)
{
	static const char *const ppe_type_str[] = {
		[PPE_PKT_TYPE_IPV4_HNAPT] = "IPv4 5T",
		[PPE_PKT_TYPE_IPV4_ROUTE] = "IPv4 3T",
		[PPE_PKT_TYPE_BRIDGE] = "L2B",
		[PPE_PKT_TYPE_IPV4_DSLITE] = "DS-LITE",
		[PPE_PKT_TYPE_IPV6_ROUTE_3T] = "IPv6 3T",
		[PPE_PKT_TYPE_IPV6_ROUTE_5T] = "IPv6 5T",
		[PPE_PKT_TYPE_IPV6_6RD] = "6RD",
	};
	static const char *const ppe_state_str[] = {
		[AIROHA_FOE_STATE_INVALID] = "INV",
		[AIROHA_FOE_STATE_UNBIND] = "UNB",
		[AIROHA_FOE_STATE_BIND] = "BND",
		[AIROHA_FOE_STATE_FIN] = "FIN",
	};
	struct airoha_ppe *ppe = m->private;
	u32 ppe_num_entries = airoha_ppe_get_total_num_entries(ppe);
	int i;

	for (i = 0; i < ppe_num_entries; i++) {
		const char *state_str, *type_str = "UNKNOWN";
		void *src_addr = NULL, *dest_addr = NULL;
		u16 *src_port = NULL, *dest_port = NULL;
		struct airoha_foe_mac_info_common *l2;
		unsigned char h_source[ETH_ALEN] = {};
		struct airoha_foe_stats64 stats = {};
		unsigned char h_dest[ETH_ALEN];
		struct airoha_foe_entry *hwe;
		u32 type, state, ib2, data;
		bool ipv6 = false;

		hwe = airoha_ppe_foe_get_entry(ppe, i);
		if (!hwe)
			continue;

		state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, hwe->ib1);
		if (!state)
			continue;

		if (bind && state != AIROHA_FOE_STATE_BIND)
			continue;

		state_str = ppe_state_str[state % ARRAY_SIZE(ppe_state_str)];
		type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, hwe->ib1);
		if (type < ARRAY_SIZE(ppe_type_str) && ppe_type_str[type])
			type_str = ppe_type_str[type];

		seq_printf(m, "%05x %s %7s", i, state_str, type_str);

		switch (type) {
		case PPE_PKT_TYPE_IPV4_HNAPT:
		case PPE_PKT_TYPE_IPV4_DSLITE:
			src_port = &hwe->ipv4.orig_tuple.src_port;
			dest_port = &hwe->ipv4.orig_tuple.dest_port;
			fallthrough;
		case PPE_PKT_TYPE_IPV4_ROUTE:
			src_addr = &hwe->ipv4.orig_tuple.src_ip;
			dest_addr = &hwe->ipv4.orig_tuple.dest_ip;
			break;
		case PPE_PKT_TYPE_IPV6_ROUTE_5T:
			src_port = &hwe->ipv6.src_port;
			dest_port = &hwe->ipv6.dest_port;
			fallthrough;
		case PPE_PKT_TYPE_IPV6_ROUTE_3T:
		case PPE_PKT_TYPE_IPV6_6RD:
			src_addr = &hwe->ipv6.src_ip;
			dest_addr = &hwe->ipv6.dest_ip;
			ipv6 = true;
			break;
		default:
			break;
		}

		if (src_addr && dest_addr) {
			seq_puts(m, " orig=");
			airoha_debugfs_ppe_print_tuple(m, src_addr, dest_addr,
						       src_port, dest_port, ipv6);
		}

		switch (type) {
		case PPE_PKT_TYPE_IPV4_HNAPT:
		case PPE_PKT_TYPE_IPV4_DSLITE:
			src_port = &hwe->ipv4.new_tuple.src_port;
			dest_port = &hwe->ipv4.new_tuple.dest_port;
			src_addr = &hwe->ipv4.new_tuple.src_ip;
			dest_addr = &hwe->ipv4.new_tuple.dest_ip;
			seq_puts(m, " new=");
			airoha_debugfs_ppe_print_tuple(m, src_addr, dest_addr,
						       src_port, dest_port,
						       ipv6);
			break;
		default:
			break;
		}

		if (type == PPE_PKT_TYPE_BRIDGE) {
			data = hwe->bridge.data;
			ib2 = hwe->bridge.ib2;
			l2 = &hwe->bridge.l2.common;
			*((__be16 *)&h_source[4]) = cpu_to_be16(hwe->bridge.l2.src_mac_lo);
		} else if (type >= PPE_PKT_TYPE_IPV6_ROUTE_3T) {
			data = hwe->ipv6.data;
			ib2 = hwe->ipv6.ib2;
			l2 = &hwe->ipv6.l2;
			*((__be16 *)&h_source[4]) = 0;
		} else {
			data = hwe->ipv4.data;
			ib2 = hwe->ipv4.ib2;
			l2 = &hwe->ipv4.l2.common;
			*((__be16 *)&h_source[4]) = cpu_to_be16(hwe->ipv4.l2.src_mac_lo);
		}

		airoha_ppe_foe_entry_get_stats(ppe, i, &stats);

		*((__be32 *)h_dest) = cpu_to_be32(l2->dest_mac_hi);
		*((__be16 *)&h_dest[4]) = cpu_to_be16(l2->dest_mac_lo);
		*((__be32 *)h_source) = cpu_to_be32(l2->src_mac_hi);

		seq_printf(m, " eth=%pM->%pM etype=%04x data=%08x"
			      " vlan=%d,%d ib1=%08x ib2=%08x"
			      " packets=%llu bytes=%llu\n",
			   h_source, h_dest, l2->etype, data,
			   l2->vlan1, l2->vlan2, hwe->ib1, ib2,
			   stats.packets, stats.bytes);
	}

	return 0;
}

static int airoha_ppe_debugfs_foe_all_show(struct seq_file *m, void *private)
{
	return airoha_ppe_debugfs_foe_show(m, private, false);
}
DEFINE_SHOW_ATTRIBUTE(airoha_ppe_debugfs_foe_all);

static int airoha_ppe_debugfs_foe_bind_show(struct seq_file *m, void *private)
{
	return airoha_ppe_debugfs_foe_show(m, private, true);
}
DEFINE_SHOW_ATTRIBUTE(airoha_ppe_debugfs_foe_bind);


static const char *econet_ppe_debugfs_state_name(u32 state)
{
	switch (state) {
	case AIROHA_FOE_STATE_INVALID:
		return "INV";
	case AIROHA_FOE_STATE_UNBIND:
		return "UNB";
	case AIROHA_FOE_STATE_BIND:
		return "BND";
	case AIROHA_FOE_STATE_FIN:
		return "FIN";
	default:
		return "???";
	}
}

static const char *econet_ppe_debugfs_type_name(u32 type)
{
	switch (type) {
	case PPE_PKT_TYPE_IPV4_HNAPT:
		return "IPv4 5T";
	case PPE_PKT_TYPE_IPV4_ROUTE:
		return "IPv4 3T";
	case PPE_PKT_TYPE_BRIDGE:
		return "L2B";
	case PPE_PKT_TYPE_IPV4_DSLITE:
		return "DS-LITE";
	case PPE_PKT_TYPE_IPV6_ROUTE_3T:
		return "IPv6 3T";
	case PPE_PKT_TYPE_IPV6_ROUTE_5T:
		return "IPv6 5T";
	case PPE_PKT_TYPE_IPV6_6RD:
		return "6RD";
	default:
		return "UNKNOWN";
	}
}

/*
 * econet_ppe_commit_entry() stores EN751221 FoE words in the byte/halfword
 * layout consumed by the PPE. Convert a CPU snapshot of that DMA memory back
 * into the logical struct layout before decoding it in debugfs.
 */
static void
econet_ppe_debugfs_decode_entry(const struct econet_foe_entry *raw,
				struct econet_foe_entry *entry)
{
	int i;

	memset(entry, 0, sizeof(*entry));
	entry->ib1 = swab32(raw->ib1);

	for (i = 0; i < ARRAY_SIZE(entry->data); i++) {
		u32 val = swab32(raw->data[i]);

		/* Mirror the 16-bit lane rotation used by commit_entry(). */
		if (i == 6 || i == 10 || i == 12 || i == 14)
			val = rol32(val, 16);

		entry->data[i] = val;
	}

}

static void econet_ppe_debugfs_mac(const struct econet_foe_mac_info *l2,
				   u8 *src, u8 *dest)
{
	*(__be32 *)&dest[0] = cpu_to_be32(l2->dest_mac_hi);
	*(__be16 *)&dest[4] = cpu_to_be16(l2->dest_mac_lo);
	*(__be32 *)&src[0] = cpu_to_be32(l2->src_mac_hi);
	*(__be16 *)&src[4] = cpu_to_be16(l2->src_mac_lo);
}

static void econet_ppe_debugfs_print_ipv4(struct seq_file *m,
					  const struct econet_foe_entry *entry)
{
	const struct econet_foe_ipv4 *ipv4 = &entry->ipv4;
	u8 src[ETH_ALEN], dest[ETH_ALEN];

	econet_ppe_debugfs_mac(&ipv4->l2, src, dest);
	seq_printf(m,
		   " orig=%pI4h:%u->%pI4h:%u new=%pI4h:%u->%pI4h:%u eth=%pM->%pM etype=%04x vlan=%u,%u pppoe=%u ib1=%08x ib2=%08x",
		   &ipv4->orig.src_ip, ipv4->orig.src_port,
		   &ipv4->orig.dest_ip, ipv4->orig.dest_port,
		   &ipv4->new.src_ip, ipv4->new.src_port,
		   &ipv4->new.dest_ip, ipv4->new.dest_port,
		   src, dest, ipv4->l2.etype, ipv4->l2.vlan1, ipv4->l2.vlan2,
		   ipv4->l2.pppoe_id, entry->ib1, ipv4->ib2);
}

static int econet_ppe_debugfs_foe_show(struct seq_file *m, bool bind_only)
{
	struct econet_ppe *ppe = m->private;
	u32 entries = ppe->eth->soc->ppe_dram_entries;
	u32 i;

	for (i = 0; i < entries; i++) {
		struct econet_foe_entry raw, entry;
		u32 state, type;

		spin_lock_bh(&ppe->lock);
		econet_ppe_read_entry(ppe, i, &raw);
		spin_unlock_bh(&ppe->lock);

		econet_ppe_debugfs_decode_entry(&raw, &entry);
		state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, entry.ib1);
		if (state == AIROHA_FOE_STATE_INVALID)
			continue;
		if (bind_only && state != AIROHA_FOE_STATE_BIND)
			continue;

		type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, entry.ib1);
		seq_printf(m, "%05x %s %7s", i,
			   econet_ppe_debugfs_state_name(state),
			   econet_ppe_debugfs_type_name(type));

		if (type == PPE_PKT_TYPE_IPV4_HNAPT ||
		    type == PPE_PKT_TYPE_IPV4_ROUTE)
			econet_ppe_debugfs_print_ipv4(m, &entry);
		else
			seq_printf(m, " ib1=%08x", entry.ib1);

		seq_printf(m, " hw_ib1=%08x\n", raw.ib1);
	}

	return 0;
}

static int econet_ppe_debugfs_foe_all_show(struct seq_file *m, void *private)
{
	return econet_ppe_debugfs_foe_show(m, false);
}
DEFINE_SHOW_ATTRIBUTE(econet_ppe_debugfs_foe_all);

static int econet_ppe_debugfs_foe_bind_show(struct seq_file *m, void *private)
{
	return econet_ppe_debugfs_foe_show(m, true);
}
DEFINE_SHOW_ATTRIBUTE(econet_ppe_debugfs_foe_bind);

static int econet_ppe_debugfs_foe_raw_show(struct seq_file *m, void *private)
{
	struct econet_ppe *ppe = m->private;
	u32 entries = ppe->eth->soc->ppe_dram_entries;
	u32 i;

	for (i = 0; i < entries; i++) {
		struct econet_foe_entry raw;
		u32 *words = (u32 *)&raw;
		bool nonzero = false;
		size_t j;

		spin_lock_bh(&ppe->lock);
		econet_ppe_read_entry(ppe, i, &raw);
		spin_unlock_bh(&ppe->lock);

		for (j = 0; j < sizeof(raw) / sizeof(*words); j++) {
			if (words[j]) {
				nonzero = true;
				break;
			}
		}
		if (!nonzero)
			continue;

		seq_printf(m, "%05x:", i);
		for (j = 0; j < sizeof(raw) / sizeof(*words); j++)
			seq_printf(m, " %08x", words[j]);
		seq_putc(m, '\n');
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(econet_ppe_debugfs_foe_raw);

static int econet_ppe_debugfs_flows_show(struct seq_file *m, void *private)
{
	struct econet_ppe *ppe = m->private;
	struct econet_flow_entry *flow, *snapshot;
	u32 count = 0, n = 0, i;

	spin_lock_bh(&ppe->lock);
	list_for_each_entry(flow, &ppe->flows, list)
		count++;
	spin_unlock_bh(&ppe->lock);

	if (!count)
		return 0;

	snapshot = kcalloc(count, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	spin_lock_bh(&ppe->lock);
	list_for_each_entry(flow, &ppe->flows, list) {
		if (n == count)
			break;
		snapshot[n].data = flow->data;
		snapshot[n].cookie = flow->cookie;
		snapshot[n].hash = flow->hash;
		n++;
	}
	spin_unlock_bh(&ppe->lock);

	for (i = 0; i < n; i++) {
		struct econet_foe_entry entry = snapshot[i].data;
		
		/* flow->data keeps the pre-swapped EN751221 lookup key. */
		entry.ipv4.orig.src_ip = swab32(entry.ipv4.orig.src_ip);
		entry.ipv4.orig.dest_ip = swab32(entry.ipv4.orig.dest_ip);
		entry.ipv4.orig.ports = swab32(entry.ipv4.orig.ports);

		seq_printf(m, "cookie=%lx hash=%04x", snapshot[i].cookie,
			   snapshot[i].hash);
		econet_ppe_debugfs_print_ipv4(m, &entry);
		seq_putc(m, '\n');
	}

	kfree(snapshot);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(econet_ppe_debugfs_flows);

static int econet_ppe_debugfs_regs_show(struct seq_file *m, void *private)
{
	struct econet_ppe *ppe = m->private;
	struct airoha_eth *eth = ppe->eth;
	struct econet_flow_entry *flow;
	u32 flows = 0;

	spin_lock_bh(&ppe->lock);
	list_for_each_entry(flow, &ppe->flows, list)
		flows++;
	spin_unlock_bh(&ppe->lock);

	seq_printf(m, "armed=%u\n", READ_ONCE(ppe->armed));
	seq_printf(m, "flows=%u\n", flows);
	seq_printf(m, "foe_cpu=%px\n", ppe->foe_table);
	seq_printf(m, "foe_dma=%pad\n", &ppe->foe_dma);
	seq_printf(m, "foe_entries=%u\n", eth->soc->ppe_dram_entries);
	seq_printf(m, "GLO_CFG=%08x\n", airoha_fe_rr(eth, REG_PPE_GLO_CFG(0)));
	seq_printf(m, "FLOW_CFG=%08x\n", airoha_fe_rr(eth, REG_PPE_PPE_FLOW_CFG(0)));
	seq_printf(m, "IP_PROTO_CHK=%08x\n", airoha_fe_rr(eth, REG_PPE_IP_PROTO_CHK(0)));
	seq_printf(m, "TB_CFG=%08x\n", airoha_fe_rr(eth, REG_PPE_TB_CFG(0)));
	seq_printf(m, "TB_BASE=%08x\n", airoha_fe_rr(eth, REG_PPE_TB_BASE(0)));
	seq_printf(m, "TB_USED=%08x\n", airoha_fe_rr(eth, REG_PPE_TB_USED(0)));
	seq_printf(m, "BIND_RATE=%08x\n", airoha_fe_rr(eth, REG_PPE_BIND_RATE(0)));
	seq_printf(m, "BIND_LIMIT0=%08x\n", airoha_fe_rr(eth, REG_PPE_BIND_LIMIT0(0)));
	seq_printf(m, "BIND_LIMIT1=%08x\n", airoha_fe_rr(eth, REG_PPE_BIND_LIMIT1(0)));
	seq_printf(m, "KEEPALIVE=%08x\n", airoha_fe_rr(eth, REG_PPE_KEEPALIVE(0)));
	seq_printf(m, "UNBIND_AGE=%08x\n", airoha_fe_rr(eth, REG_PPE_UNBIND_AGE(0)));
	seq_printf(m, "BND_AGE0=%08x\n", airoha_fe_rr(eth, REG_PPE_BND_AGE0(0)));
	seq_printf(m, "BND_AGE1=%08x\n", airoha_fe_rr(eth, REG_PPE_BND_AGE1(0)));
	seq_printf(m, "HASH_SEED=%08x\n", airoha_fe_rr(eth, REG_PPE_HASH_SEED(0)));
	seq_printf(m, "DFT_CPORT=%08x\n", airoha_fe_rr(eth, REG_PPE_DFT_CPORT_BASE(0)));
	seq_printf(m, "VPM_TPID=%08x\n", airoha_fe_rr(eth, REG_PPE_VPM_TPID(0)));
	seq_printf(m, "CACHE_CTL=%08x\n", airoha_fe_rr(eth, REG_EN751221_PPE_CACHE_CTL));
	seq_printf(m, "CACHE_GATE=%08x\n", airoha_fe_rr(eth, REG_EN751221_PPE_CAH_GATE));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(econet_ppe_debugfs_regs);

static int econet_ppe_debugfs_foe_json_show(struct seq_file *m, void *private)
{
	struct econet_ppe *ppe = m->private;
	u32 entries = ppe->eth->soc->ppe_dram_entries;
	bool first = true;
	u32 i;

	seq_puts(m, "[\n");
	for (i = 0; i < entries; i++) {
		struct econet_foe_entry raw, entry;
		const struct econet_foe_ipv4 *ipv4;
		u32 state, type;

		spin_lock_bh(&ppe->lock);
		econet_ppe_read_entry(ppe, i, &raw);
		spin_unlock_bh(&ppe->lock);

		econet_ppe_debugfs_decode_entry(&raw, &entry);
		state = FIELD_GET(AIROHA_FOE_IB1_BIND_STATE, entry.ib1);
		if (state == AIROHA_FOE_STATE_INVALID)
			continue;

		type = FIELD_GET(AIROHA_FOE_IB1_BIND_PACKET_TYPE, entry.ib1);
		if (!first)
			seq_puts(m, ",\n");
		first = false;

		seq_printf(m, "  {\"index\":%u,\"state\":\"%s\",\"type\":\"%s\",\"ib1\":\"0x%08x\",\"hw_ib1\":\"0x%08x\"",
			   i, econet_ppe_debugfs_state_name(state),
			   econet_ppe_debugfs_type_name(type), entry.ib1, raw.ib1);

		if (type == PPE_PKT_TYPE_IPV4_HNAPT ||
		    type == PPE_PKT_TYPE_IPV4_ROUTE) {
			ipv4 = &entry.ipv4;
			seq_printf(m, ",\"orig_src\":\"%pI4h\",\"orig_src_port\":%u,\"orig_dest\":\"%pI4h\",\"orig_dest_port\":%u,\"new_src\":\"%pI4h\",\"new_src_port\":%u,\"new_dest\":\"%pI4h\",\"new_dest_port\":%u,\"ib2\":\"0x%08x\"",
				   &ipv4->orig.src_ip, ipv4->orig.src_port,
				   &ipv4->orig.dest_ip, ipv4->orig.dest_port,
				   &ipv4->new.src_ip, ipv4->new.src_port,
				   &ipv4->new.dest_ip, ipv4->new.dest_port,
				   ipv4->ib2);
		}

		seq_putc(m, '}');
	}
	seq_puts(m, "\n]\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(econet_ppe_debugfs_foe_json);

int econet_ppe_debugfs_init(struct econet_ppe *ppe)
{
	struct dentry *dir;

	dir = debugfs_create_dir("ppe", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	ppe->debugfs_dir = dir;
	debugfs_create_file("entries", 0444, dir, ppe,
			    &econet_ppe_debugfs_foe_all_fops);
	debugfs_create_file("entries.json", 0444, dir, ppe,
			    &econet_ppe_debugfs_foe_json_fops);
	debugfs_create_file("bind", 0444, dir, ppe,
			    &econet_ppe_debugfs_foe_bind_fops);
	debugfs_create_file("raw", 0444, dir, ppe,
			    &econet_ppe_debugfs_foe_raw_fops);
	debugfs_create_file("flows", 0444, dir, ppe,
			    &econet_ppe_debugfs_flows_fops);
	debugfs_create_file("regs", 0444, dir, ppe,
			    &econet_ppe_debugfs_regs_fops);

	return 0;
}

int airoha_ppe_debugfs_init(struct airoha_ppe *ppe)
{
	ppe->debugfs_dir = debugfs_create_dir("ppe", NULL);
	debugfs_create_file("entries", 0444, ppe->debugfs_dir, ppe,
			    &airoha_ppe_debugfs_foe_all_fops);
	debugfs_create_file("entries.json", 0444, ppe->debugfs_dir, ppe,
			    &airoha_ppe_debugfs_foe_json_fops);
	debugfs_create_file("bind", 0444, ppe->debugfs_dir, ppe,
			    &airoha_ppe_debugfs_foe_bind_fops);

	return 0;
}