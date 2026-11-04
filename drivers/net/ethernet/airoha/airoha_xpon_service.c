// SPDX-License-Identifier: GPL-2.0-only
/* Shared software classifier table for Airoha and EcoNet xPON. */

#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "airoha_eth.h"

static bool airoha_xpon_vlan_overlap(const struct xpon_vlan_rule *a,
				     const struct xpon_vlan_rule *b)
{
	if (a->tag_mode != b->tag_mode)
		return false;

	if (a->tag_mode == XPON_VLAN_UNTAGGED)
		return true;

	return (!a->tpid || !b->tpid || a->tpid == b->tpid) &&
	       !((a->tci ^ b->tci) & a->tci_mask & b->tci_mask) &&
	       (a->tag_mode != XPON_VLAN_DOUBLE_TAG ||
		((!a->inner_tpid || !b->inner_tpid || a->inner_tpid == b->inner_tpid) &&
		 !((a->inner_tci ^ b->inner_tci) & a->inner_tci_mask & b->inner_tci_mask)));
}

static bool airoha_xpon_vlan_action_equal(const struct xpon_vlan_rule *a,
					  const struct xpon_vlan_rule *b)
{
	return a->drop == b->drop && a->new_tpid == b->new_tpid &&
	       a->new_tci == b->new_tci && a->new_tci_mask == b->new_tci_mask;
}

static int airoha_xpon_vlan_check_rule(const struct xpon_vlan_rule *rule)
{
	if (rule->tag_mode > XPON_VLAN_UNTAGGED ||
	    (rule->tag_mode == XPON_VLAN_UNTAGGED && !rule->drop))
		return -EOPNOTSUPP;

	if ((rule->inner_tpid && !eth_type_vlan(rule->inner_tpid)) ||
	    (rule->inner_tci & ~rule->inner_tci_mask))
		return -EINVAL;

	if ((rule->tpid && !eth_type_vlan(rule->tpid)) ||
	    (rule->new_tpid && !eth_type_vlan(rule->new_tpid)))
		return -EOPNOTSUPP;

	if ((rule->tci & ~rule->tci_mask) ||
	    (rule->new_tci & ~rule->new_tci_mask))
		return -EINVAL;

	return 0;
}

static struct xpon_vlan_rule
airoha_xpon_vlan_downstream(const struct xpon_vlan_config *config)
{
	const struct xpon_vlan_rule *up = &config->upstream;

	if (config->downstream_valid)
		return config->downstream;

	/* Even transparent downstream must not be captured by another inverse. */
	return (struct xpon_vlan_rule) {
		.tag_mode = up->tag_mode,
		.inner_tpid = up->inner_tpid,
		.inner_tci = up->inner_tci,
		.inner_tci_mask = up->inner_tci_mask,
		.tpid = up->new_tpid ?: up->tpid,
		.tci = (up->tci & ~up->new_tci_mask) | up->new_tci,
		.tci_mask = up->tci_mask | up->new_tci_mask,
	};
}

int airoha_xpon_vlan_validate(const struct xpon_vlan_config *configs, size_t count)
{
	size_t i, j;
	int ret;

	if (count > AIROHA_XPON_MAX_SERVICES)
		return -ENOSPC;

	if (count && !configs)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		const struct xpon_vlan_config *a = &configs[i];

		if (!a->cookie || a->wire_id >= 4096)
			return -EINVAL;

		ret = airoha_xpon_vlan_check_rule(&a->upstream);
		if (ret)
			return ret;

		if (a->downstream_valid) {
			ret = airoha_xpon_vlan_check_rule(&a->downstream);
			if (ret)
				return ret;
		}

		for (j = 0; j < i; j++) {
			const struct xpon_vlan_config *b = &configs[j];
			struct xpon_vlan_rule down_a, down_b;

			if (a->cookie == b->cookie)
				return -EEXIST;
		
			/* pon0 has no ingress UNI metadata: never guess between UNIs. */
			if (airoha_xpon_vlan_overlap(&a->upstream, &b->upstream) &&
			    (a->port_id != b->port_id || a->priority == b->priority) &&
			    (a->wire_id != b->wire_id ||
			     !airoha_xpon_vlan_action_equal(&a->upstream, &b->upstream)))
				return -EOPNOTSUPP;
		
			if (a->upstream.drop || b->upstream.drop)
				continue;
		
			down_a = airoha_xpon_vlan_downstream(a);
			down_b = airoha_xpon_vlan_downstream(b);
			if (a->wire_id == b->wire_id &&
			    airoha_xpon_vlan_overlap(&down_a, &down_b) &&
			    !airoha_xpon_vlan_action_equal(&down_a, &down_b))
				return -EOPNOTSUPP;
		}
	}

	return 0;
}

/* skb->data points at Ethernet on both TX and the common xPON RX hook. */
static int airoha_xpon_vlan_read(struct sk_buff *skb,
				 struct airoha_xpon_vlan_packet *packet)
{
	struct ethhdr eth_buffer;
	struct vlan_hdr vlan_buffer;
	const struct vlan_hdr *vh;
	const struct ethhdr *eh;
	unsigned int offset = ETH_HLEN;
	__be16 proto;

	memset(packet, 0, sizeof(*packet));
	eh = skb_header_pointer(skb, 0, ETH_HLEN, &eth_buffer);
	if (!eh)
		return -EINVAL;

	proto = eh->h_proto;
	if (skb_vlan_tag_present(skb)) {
		if (!eth_type_vlan(skb->vlan_proto))
			return -EINVAL;

		packet->tpid[0] = skb->vlan_proto;
		packet->tci[0] = skb_vlan_tag_get(skb);
		packet->tags = 1;
	}
	while (eth_type_vlan(proto)) {
		if (packet->tags == ARRAY_SIZE(packet->tpid))
			return -EOPNOTSUPP;

		vh = skb_header_pointer(skb, offset, sizeof(vlan_buffer), &vlan_buffer);
		if (!vh)
			return -EINVAL;

		packet->tpid[packet->tags] = proto;
		packet->tci[packet->tags++] = ntohs(vh->h_vlan_TCI);
		proto = vh->h_vlan_encapsulated_proto;
		offset += VLAN_HLEN;
	}

	return 0;
}

static bool airoha_xpon_vlan_matches(const struct xpon_vlan_rule *rule,
				     const struct airoha_xpon_vlan_packet *packet)
{
	unsigned int tags = rule->tag_mode == XPON_VLAN_UNTAGGED ? 0 :
			    rule->tag_mode == XPON_VLAN_DOUBLE_TAG ? 2 : 1;

	if (packet->tags != tags)
		return false;

	if (!tags)
		return true;

	if ((rule->tpid && packet->tpid[0] != rule->tpid) ||
	    ((packet->tci[0] ^ rule->tci) & rule->tci_mask))
		return false;

	return tags == 1 ||
	       ((!rule->inner_tpid || packet->tpid[1] == rule->inner_tpid) &&
		!((packet->tci[1] ^ rule->inner_tci) & rule->inner_tci_mask));
}

int airoha_xpon_vlan_lookup_tags(const struct airoha_xpon_vlan_table *table,
				 const struct airoha_xpon_vlan_packet *packet,
				 u16 wire_id, bool downstream,
				 struct xpon_vlan_config *config)
{
	const struct xpon_vlan_config *best = NULL;
	int i;

	for (i = 0; i < table->count; i++) {
		const struct xpon_vlan_config *candidate = &table->rules[i];
		const struct xpon_vlan_rule *rule;

		if ((wire_id != U16_MAX && wire_id != candidate->wire_id) ||
		    (downstream && !candidate->downstream_valid))
			continue;

		rule = downstream ? &candidate->downstream : &candidate->upstream;
		if (!airoha_xpon_vlan_matches(rule, packet))
			continue;

		if (!best || candidate->priority < best->priority)
			best = candidate;
	}

	if (!best)
		return 0;

	*config = *best;

	return 1;
}

int airoha_xpon_vlan_lookup(const struct airoha_xpon_vlan_table *table,
			    __be16 tpid, u16 tci, u16 wire_id, bool downstream,
			    struct xpon_vlan_config *config)
{
	struct airoha_xpon_vlan_packet packet = {
		.tpid[0] = tpid, .tci[0] = tci, .tags = 1,
	};

	return airoha_xpon_vlan_lookup_tags(table, &packet, wire_id, downstream, config);
}

int airoha_xpon_vlan_select(const struct airoha_xpon_vlan_table *table,
			    struct sk_buff *skb, u16 wire_id, bool downstream,
			    struct xpon_vlan_config *config)
{
	struct airoha_xpon_vlan_packet packet;
	int ret;

	if (!table->count)
		return 0;

	ret = airoha_xpon_vlan_read(skb, &packet);
	if (ret)
		return ret;

	return airoha_xpon_vlan_lookup_tags(table, &packet, wire_id, downstream, config);
}

int airoha_xpon_vlan_apply(struct sk_buff *skb, const struct xpon_vlan_rule *rule,
			   struct airoha_xpon_vlan_undo *undo)
{
	struct airoha_xpon_vlan_packet packet;
	__be16 tpid, new_tpid;
	u16 tci, new_tci;
	int ret;

	ret = airoha_xpon_vlan_read(skb, &packet);
	if (ret)
		return ret;

	if (rule->drop)
		return -EACCES;

	if (!packet.tags || packet.tags != (rule->tag_mode == XPON_VLAN_DOUBLE_TAG ? 2 : 1))
		return -EINVAL;

	tpid = packet.tpid[0];
	tci = packet.tci[0];
	new_tpid = rule->new_tpid ?: tpid;
	new_tci = (tci & ~rule->new_tci_mask) | rule->new_tci;
	if (!skb_vlan_tag_present(skb) && skb_ensure_writable(skb, VLAN_ETH_HLEN))
		return -ENOMEM;

	if (undo) {
		*undo = (struct airoha_xpon_vlan_undo) {
			.rule.tag_mode = rule->tag_mode,
			.rule.new_tpid = tpid,
			.rule.new_tci = tci,
			.rule.new_tci_mask = U16_MAX,
			.protocol = skb->protocol,
			.csum = skb->csum,
			.ip_summed = skb->ip_summed,
			.accelerated = skb_vlan_tag_present(skb),
		};
	}

	if (skb_vlan_tag_present(skb)) {
		__vlan_hwaccel_put_tag(skb, new_tpid, new_tci);
	} else {
		struct vlan_ethhdr *vh = (struct vlan_ethhdr *)skb->data;

		vh->h_vlan_proto = new_tpid;
		vh->h_vlan_TCI = htons(new_tci);
		if (skb->protocol == tpid)
			skb->protocol = new_tpid;
		/* RX may have pulled Ethernet without including it in skb->csum. */
		if (skb->ip_summed == CHECKSUM_COMPLETE)
			skb->ip_summed = CHECKSUM_NONE;
	}

	return 0;
}

int airoha_xpon_vlan_restore(struct sk_buff *skb,
			     const struct airoha_xpon_vlan_undo *undo)
{
	int ret;

	if (undo->accelerated && !skb_vlan_tag_present(skb)) {
		u16 tci;

		/* Remove only the materialized outer tag; keep inner in the packet. */
		ret = __skb_vlan_pop(skb, &tci);
		if (!ret)
			__vlan_hwaccel_put_tag(skb, undo->rule.new_tpid,
					       undo->rule.new_tci);
	} else {
		ret = airoha_xpon_vlan_apply(skb, &undo->rule, NULL);
	}
	if (ret)
		return ret;

	skb->protocol = undo->protocol;
	skb->ip_summed = undo->ip_summed;
	skb->csum = undo->csum;

	return 0;
}

/* Same rule from the classifier's point of view?  The cookie is not part of it. */
static bool airoha_xpon_rule_eq(const struct airoha_xpon_service_cfg *a,
				const struct airoha_xpon_service_cfg *b)
{
	return a->gem_port_id == b->gem_port_id && a->tcont == b->tcont &&
	       a->queue == b->queue &&
	       a->vlan_valid == b->vlan_valid &&
	       (!a->vlan_valid || a->vlan_id == b->vlan_id) &&
	       a->pcp_valid == b->pcp_valid &&
	       (!a->pcp_valid || a->pcp == b->pcp) &&
	       a->default_service == b->default_service &&
	       a->tag_untagged == b->tag_untagged &&
	       (!a->tag_untagged || (a->tag_tci == b->tag_tci &&
				     a->strip_downstream == b->strip_downstream));
}

/* Caller serializes access to the table. */
int airoha_xpon_find_cookie(const struct airoha_xpon_service_table *table,
			    u32 cookie)
{
	int i, j;

	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++) {
		if (!table->rules[i].valid)
			continue;
	
		for (j = 0; j < table->ncookies[i]; j++)
			if (table->cookies[i][j] == cookie)
				return i;
	}

	return -1;
}

void airoha_xpon_forget_cookie(struct airoha_xpon_service_table *table,
			       u32 cookie)
{
	int i, j;

	i = airoha_xpon_find_cookie(table, cookie);
	if (i < 0)
		return;

	for (j = 0; j < table->ncookies[i]; j++) {
		if (table->cookies[i][j] != cookie)
			continue;
	
		table->ncookies[i]--;
		table->cookies[i][j] = table->cookies[i][table->ncookies[i]];
		table->cookies[i][table->ncookies[i]] = 0;
		break;
	}

	if (!table->ncookies[i])
		memset(&table->rules[i], 0, sizeof(table->rules[i]));
}

/* Reclaim buckets fragmented by deleting cookies from shared rules. */
static bool airoha_xpon_compact_services(struct airoha_xpon_service_table *table)
{
	bool reclaimed = false;
	int i, j;

	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++) {
		if (!table->rules[i].valid)
			continue;
	
		for (j = i + 1; j < AIROHA_XPON_MAX_SERVICES; j++) {
			if (table->ncookies[i] == AIROHA_XPON_SVC_MAX_COOKIES)
				break;
	
			if (!table->rules[j].valid ||
			    !airoha_xpon_rule_eq(&table->rules[i], &table->rules[j]))
				continue;
	
			while (table->ncookies[j] &&
			       table->ncookies[i] < AIROHA_XPON_SVC_MAX_COOKIES) {
				u8 last = --table->ncookies[j];

				table->cookies[i][table->ncookies[i]++] =
					table->cookies[j][last];
				table->cookies[j][last] = 0;
			}
	
			if (!table->ncookies[j]) {
				memset(&table->rules[j], 0, sizeof(table->rules[j]));
				reclaimed = true;
			}
		}
	}

	return reclaimed;
}

/*
 * Choose the destination before detaching an existing cookie. A failed
 * replacement must leave both its previous rule and the default intact.
 * Full cookie buckets spill into another rule slot, as on the EcoNet path.
 */
int airoha_xpon_store_service(struct airoha_xpon_service_table *table,
			      const struct airoha_xpon_service_cfg *cfg)
{
	int old, empty, target, i;

retry:
	old = airoha_xpon_find_cookie(table, cfg->cookie);
	empty = -1;
	target = -1;

	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++) {
		if (empty < 0 &&
		    (!table->rules[i].valid || (i == old && table->ncookies[i] == 1)))
			empty = i;
	
		if (table->rules[i].valid &&
		    airoha_xpon_rule_eq(&table->rules[i], cfg) &&
		    (i == old || table->ncookies[i] < AIROHA_XPON_SVC_MAX_COOKIES)) {
			target = i;
			break;
		}
	}
	if (target < 0)
		target = empty;

	if (target < 0) {
		if (airoha_xpon_compact_services(table))
			goto retry;
	
		return -ENOSPC;
	}

	if (target != old || !airoha_xpon_rule_eq(&table->rules[target], cfg)) {
		airoha_xpon_forget_cookie(table, cfg->cookie);
		if (!table->rules[target].valid) {
			table->rules[target] = *cfg;
			table->rules[target].valid = true;
		}
		table->cookies[target][table->ncookies[target]++] = cfg->cookie;
	}

	if (cfg->default_service)
		for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++)
			if (i != target)
				table->rules[i].default_service = false;

	return 0;
}
