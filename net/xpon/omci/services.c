// SPDX-License-Identifier: GPL-2.0-only
/* Reconcile normalized OMCI services with a hardware provider. */

#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "internal.h"

void omci_ext_vlan_parse_rule(struct omci_extended_vlan_rule *rule,
			      const u8 *raw)
{
	u32 word;

	memcpy(rule->raw, raw, sizeof(rule->raw));
	word = get_unaligned_be32(raw);
	rule->filter_outer_pbit = (word >> 28) & 0xf;
	rule->filter_outer_vid = (word >> 15) & 0x1fff;
	rule->filter_outer_tpid_dei = (word >> 12) & 0x7;

	word = get_unaligned_be32(raw + 4);
	rule->filter_inner_pbit = (word >> 28) & 0xf;
	rule->filter_inner_vid = (word >> 15) & 0x1fff;
	rule->filter_inner_tpid_dei = (word >> 12) & 0x7;
	rule->filter_ethertype = word & 0xf;

	word = get_unaligned_be32(raw + 8);
	rule->tags_to_remove = (word >> 30) & 0x3;
	rule->treat_outer_pbit = (word >> 16) & 0xf;
	rule->treat_outer_vid = (word >> 3) & 0x1fff;
	rule->treat_outer_tpid_dei = word & 0x7;

	word = get_unaligned_be32(raw + 12);
	rule->treat_inner_pbit = (word >> 16) & 0xf;
	rule->treat_inner_vid = (word >> 3) & 0x1fff;
	rule->treat_inner_tpid_dei = word & 0x7;
	rule->delete = !memchr_inv(raw + 8, 0xff, 8);
}

static int omci_vlan_match(const struct omci_extended_vlan *vlan,
			   const struct omci_extended_vlan_rule *rule,
			   struct xpon_vlan_rule *up)
{
	if (rule->filter_inner_vid < VLAN_VID_MASK) {
		up->tci = rule->filter_inner_vid;
		up->tci_mask = VLAN_VID_MASK;
	} else if (rule->filter_inner_vid != 4096) {
		return -EINVAL;
	}
	if (rule->filter_inner_pbit <= 7) {
		up->tci |= rule->filter_inner_pbit << VLAN_PRIO_SHIFT;
		up->tci_mask |= VLAN_PRIO_MASK;
	} else if (rule->filter_inner_pbit != 8 &&
		   rule->filter_inner_pbit != 14) {
		return -EINVAL;
	}

	switch (rule->filter_inner_tpid_dei) {
	case 0:
		break;
	case 4:
		up->tpid = htons(ETH_P_8021Q);
		break;
	case 5:
	case 6:
	case 7:
		if (!vlan->input_tpid)
			return -EINVAL;
		up->tpid = htons(vlan->input_tpid);
		if (rule->filter_inner_tpid_dei >= 6) {
			up->tci_mask |= VLAN_CFI_MASK;
			if (rule->filter_inner_tpid_dei == 7)
				up->tci |= VLAN_CFI_MASK;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int omci_vlan_treatment(const struct omci_extended_vlan *vlan,
			       const struct omci_extended_vlan_rule *rule,
			       struct xpon_vlan_rule *up)
{
	if (rule->treat_inner_vid < VLAN_VID_MASK) {
		up->new_tci = rule->treat_inner_vid;
		up->new_tci_mask = VLAN_VID_MASK;
	} else if (rule->treat_inner_vid == 4097) {
		return -EOPNOTSUPP; /* No outer tag to copy. */
	} else if (rule->treat_inner_vid != 4096) {
		return -EINVAL;
	}
	if (rule->treat_inner_pbit <= 7) {
		up->new_tci |= rule->treat_inner_pbit << VLAN_PRIO_SHIFT;
		up->new_tci_mask |= VLAN_PRIO_MASK;
	} else if (rule->treat_inner_pbit == 9 ||
		   rule->treat_inner_pbit == 10) {
		return -EOPNOTSUPP; /* Outer-tag copy and DSCP mapping. */
	} else if (rule->treat_inner_pbit != 8) {
		return -EINVAL;
	}

	switch (rule->treat_inner_tpid_dei) {
	case 0:
		break; /* Copy TPID and DEI from the existing inner tag. */
	case 2:
	case 6:
	case 7:
		if (!vlan->output_tpid)
			return -EINVAL;
		up->new_tpid = htons(vlan->output_tpid);
		if (rule->treat_inner_tpid_dei >= 6) {
			up->new_tci_mask |= VLAN_CFI_MASK;
			if (rule->treat_inner_tpid_dei == 7)
				up->new_tci |= VLAN_CFI_MASK;
		}
		break;
	case 4:
		up->new_tpid = htons(ETH_P_8021Q);
		up->new_tci_mask |= VLAN_CFI_MASK;
		break;
	case 1:
	case 3:
		return -EOPNOTSUPP; /* No outer tag to copy. */
	default:
		return -EINVAL;
	}
	return 0;
}

int omci_vlan_parse_rewrite(const struct omci_extended_vlan *vlan,
			    const struct omci_extended_vlan_rule *rule,
			    struct xpon_vlan_config *config)
{
	struct xpon_vlan_config parsed = {};
	struct xpon_vlan_rule *up = &parsed.upstream;
	struct xpon_vlan_rule *down = &parsed.downstream;
	struct omci_extended_vlan_rule first;
	int ret;

	if (!vlan || !rule || !config)
		return -EINVAL;
	if (rule->delete || rule->filter_ethertype || vlan->downstream_mode > 1)
		return -EOPNOTSUPP;
	first = *rule;
	if (rule->filter_outer_pbit != 15) {
		struct xpon_vlan_rule inner = {};

		if (rule->filter_inner_pbit == 15)
			return -EOPNOTSUPP;
		up->tag_mode = XPON_VLAN_DOUBLE_TAG;
		ret = omci_vlan_match(vlan, rule, &inner);
		if (ret)
			return ret;
		up->inner_tpid = inner.tpid;
		up->inner_tci = inner.tci;
		up->inner_tci_mask = inner.tci_mask;
		first.filter_inner_pbit = rule->filter_outer_pbit;
		first.filter_inner_vid = rule->filter_outer_vid;
		first.filter_inner_tpid_dei = rule->filter_outer_tpid_dei;
	} else if (rule->filter_inner_pbit == 15) {
		up->tag_mode = XPON_VLAN_UNTAGGED;
	}
	if (up->tag_mode != XPON_VLAN_UNTAGGED) {
		ret = omci_vlan_match(vlan, &first, up);
		if (ret)
			return ret;
	}
	if (rule->filter_inner_pbit == 14 || rule->filter_outer_pbit == 14)
		parsed.priority = BIT(15);
	/* Discard rows are policies, not forwarding services or inverse rules. */
	if (rule->tags_to_remove == 3) {
		up->drop = true;
		*config = parsed;
		return 0;
	}
	/* Replace the first tag; insertion and removal remain unsupported. */
	if (up->tag_mode == XPON_VLAN_UNTAGGED || rule->tags_to_remove != 1 ||
	    rule->treat_outer_pbit != 15 || rule->treat_inner_pbit == 15)
		return -EOPNOTSUPP;
	if (up->tag_mode == XPON_VLAN_DOUBLE_TAG) {
		/* OMCI names the emitted tag "inner", although it replaces outer. */
		switch (first.treat_inner_tpid_dei) {
		case 1: /* Copy the original outer TPID and DEI. */
			first.treat_inner_tpid_dei = 0;
			break;
		case 3: /* Output TPID, preserving the original outer DEI. */
			first.treat_inner_tpid_dei = 2;
			break;
		case 0:
		case 2:
			return -EOPNOTSUPP; /* Would copy DEI from the untouched inner tag. */
		}
		if (first.treat_inner_pbit == 8)
			return -EOPNOTSUPP; /* Would copy priority from the inner tag. */
		if (first.treat_inner_pbit == 9)
			first.treat_inner_pbit = 8;
		if (first.treat_inner_vid == 4096)
			return -EOPNOTSUPP; /* Would copy VID from the inner tag. */
		if (first.treat_inner_vid == 4097)
			first.treat_inner_vid = 4096;
	}
	ret = omci_vlan_treatment(vlan, &first, up);
	if (ret)
		return ret;
	if (vlan->downstream_mode == 0) {
		if (up->new_tpid && !up->tpid)
			return -EOPNOTSUPP;
		/* SDK inverse: match the emitted values, restore exact input bits,
		 * and copy fields which were wildcarded upstream (notably PCP).
		 */
		*down = *up;
		down->tpid = up->new_tpid ?: up->tpid;
		down->new_tpid = up->new_tpid ? up->tpid : 0;
		down->tci_mask = up->tci_mask | up->new_tci_mask;
		down->tci = (up->tci & ~up->new_tci_mask) | up->new_tci;
		down->new_tci_mask = up->new_tci_mask & up->tci_mask;
		down->new_tci = up->tci & down->new_tci_mask;
		parsed.downstream_valid = true;
	}
	*config = parsed;
	return 0;
}

static bool omci_vlan_rule_equal(const struct xpon_vlan_rule *a,
				 const struct xpon_vlan_rule *b)
{
	return a->tpid == b->tpid && a->new_tpid == b->new_tpid &&
	       a->tci == b->tci && a->tci_mask == b->tci_mask &&
	       a->new_tci == b->new_tci && a->new_tci_mask == b->new_tci_mask &&
	       a->inner_tpid == b->inner_tpid && a->inner_tci == b->inner_tci &&
	       a->inner_tci_mask == b->inner_tci_mask && a->tag_mode == b->tag_mode &&
	       a->drop == b->drop;
}

static bool omci_service_equal(const struct omci_service_config *a,
			       const struct omci_service_config *b)
{
	return a->cookie == b->cookie &&
	       a->uni_entity_id == b->uni_entity_id &&
	       a->gem_ctp_entity_id == b->gem_ctp_entity_id &&
	       a->gem_port_id == b->gem_port_id &&
	       a->tcont_entity_id == b->tcont_entity_id &&
	       a->alloc_id == b->alloc_id && a->vlan_id == b->vlan_id &&
	       a->pcp == b->pcp && a->queue == b->queue &&
	       a->direction == b->direction &&
	       !memcmp(a->vlan_treatment, b->vlan_treatment,
		       sizeof(a->vlan_treatment)) &&
	       a->multicast_ani_entity_id == b->multicast_ani_entity_id &&
	       a->vlan_valid == b->vlan_valid && a->pcp_valid == b->pcp_valid &&
	       a->vlan_treatment_valid == b->vlan_treatment_valid &&
	       a->multicast_ani_valid == b->multicast_ani_valid &&
	       a->multicast == b->multicast &&
	       a->default_service == b->default_service &&
	       a->vlan_rewrite == b->vlan_rewrite &&
	       (!a->vlan_rewrite ||
		(a->vlan.cookie == b->vlan.cookie &&
		 a->vlan.port_id == b->vlan.port_id &&
		 a->vlan.wire_id == b->vlan.wire_id &&
		 a->vlan.priority == b->vlan.priority &&
		 a->vlan.downstream_valid == b->vlan.downstream_valid &&
		 omci_vlan_rule_equal(&a->vlan.upstream, &b->vlan.upstream) &&
		 (!a->vlan.downstream_valid ||
		  omci_vlan_rule_equal(&a->vlan.downstream, &b->vlan.downstream))));
}

static bool omci_service_unchanged(struct xarray *services,
				   const struct omci_service_state *state)
{
	struct omci_service_state *other;

	other = xa_load(services, state->config.cookie);
	return other && omci_service_equal(&other->config, &state->config);
}

static int omci_agent_validate_vlans(struct omci_device *odev,
				     struct xarray *desired)
{
	struct xpon_vlan_config *configs;
	struct omci_service_state *state;
	unsigned long index;
	size_t count = 0, i = 0;
	int ret;

	xa_for_each(desired, index, state)
		if (state->config.vlan_rewrite)
			count++;
	configs = kvmalloc_array(count, sizeof(*configs), GFP_KERNEL);
	if (!configs && count)
		return -ENOMEM;
	xa_for_each(desired, index, state)
		if (state->config.vlan_rewrite)
			configs[i++] = state->config.vlan;
	ret = xpon_device_validate_vlans(odev->xpon, configs, count);
	kvfree(configs);
	return ret;
}

static bool omci_service_is_policy(const struct omci_service_config *config)
{
	return config->vlan_rewrite && config->vlan.upstream.drop;
}

static int omci_service_replace(struct omci_device *odev,
				const struct omci_service_config *config)
{
	int ret;

	ret = omci_service_is_policy(config) ? 0 :
		odev->ops->replace_service(odev, config);
	if (ret || !config->vlan_rewrite)
		return ret;
	return xpon_device_replace_vlan(odev->xpon, &config->vlan);
}

int omci_service_delete(struct omci_device *odev,
			const struct omci_service_config *config)
{
	int ret;

	if (config->vlan_rewrite) {
		ret = xpon_device_delete_vlan(odev->xpon, config->cookie);
		if (ret && ret != -ENOENT)
			return ret;
	}
	return omci_service_is_policy(config) ? 0 :
		odev->ops->delete_service(odev, config->cookie);
}

static int omci_agent_validate_services(struct omci_device *odev,
					struct xarray *desired)
{
	struct omci_service_config *configs;
	struct omci_service_state *state;
	unsigned long index;
	size_t count = 0, i = 0;
	int ret;

	if (!odev->ops->validate_services)
		return 0;
	xa_for_each(desired, index, state)
		if (!omci_service_is_policy(&state->config))
			count++;
	configs = kvmalloc_array(count, sizeof(*configs), GFP_KERNEL);
	if (!configs && count)
		return -ENOMEM;
	xa_for_each(desired, index, state)
		if (!omci_service_is_policy(&state->config))
			configs[i++] = state->config;
	ret = odev->ops->validate_services(odev, configs, count);
	kvfree(configs);
	if (ret)
		dev_warn(odev->parent,
			 "OMCI service preflight failed for %zu rules: %d; active services unchanged\n",
			 count, ret);
	return ret;
}

int omci_agent_apply_services_locked(struct omci_device *odev,
				     struct xarray *desired)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_service_state *state;
	struct omci_service_state *old_state;
	unsigned long index;
	int ret, err, rollback_error = 0;

	if ((!xa_empty(desired) || !xa_empty(&agent->services)) &&
	    (!odev->ops->replace_service || !odev->ops->delete_service))
		return -EOPNOTSUPP;

	ret = omci_agent_validate_services(odev, desired);
	if (ret)
		return ret;
	ret = omci_agent_validate_vlans(odev, desired);
	if (ret)
		return ret;

	/* Reserve all new cookies before changing hardware state. */
	xa_for_each(desired, index, state) {
		if (xa_load(&agent->services, index))
			continue;
		ret = xa_reserve(&agent->services, index, GFP_KERNEL);
		if (ret)
			goto release_reservations;
	}

	/* Free obsolete and changed rules so only the final set must fit. */
	xa_for_each(&agent->services, index, old_state) {
		if (omci_service_unchanged(desired, old_state))
			continue;
		ret = omci_service_delete(odev, &old_state->config);
		if (ret && ret != -ENOENT) {
			dev_warn(odev->parent,
				 "OMCI delete failed for service %#x: %d; rolling back\n",
				 old_state->config.cookie, ret);
			goto rollback_hardware;
		}
	}

	/* Reapply unchanged rules too: a GPON restart may have lost them. */
	xa_for_each(desired, index, state) {
		ret = omci_service_replace(odev, &state->config);
		if (ret) {
			dev_warn(odev->parent,
				 "OMCI install failed for service %#x (UNI %#x GEM %u T-CONT %#x queue %u): %d; rolling back\n",
				 state->config.cookie, state->config.uni_entity_id,
				 state->config.gem_port_id,
				 state->config.tcont_entity_id,
				 state->config.queue, ret);
			goto rollback_hardware;
		}
	}

	/* Keep desired keys and reservations alive throughout the commit. */
	xa_for_each(&agent->services, index, old_state) {
		if (xa_load(desired, index))
			continue;
		xa_erase(&agent->services, index);
		kfree(old_state);
	}
	xa_for_each(desired, index, state) {
		void *entry;

		xa_erase(desired, index);
		entry = xa_store(&agent->services, index, state, GFP_NOWAIT);
		if (WARN_ON(xa_is_err(entry))) {
			kfree(state);
			continue;
		}
		kfree(entry);
	}
	return 0;

rollback_hardware:
	/* Remove attempted replacements before restoring the previous set. */
	if (odev->ops->delete_service)
		xa_for_each(desired, index, state) {
			if (omci_service_unchanged(&agent->services, state))
				continue;
			err = omci_service_delete(odev, &state->config);
			if (err && err != -ENOENT) {
				rollback_error = err;
				dev_err(odev->parent,
					"OMCI rollback delete failed for service %#x: %d\n",
					state->config.cookie, err);
			}
		}
	if (odev->ops->replace_service)
		xa_for_each(&agent->services, index, old_state) {
			err = omci_service_replace(odev, &old_state->config);
			if (err) {
				rollback_error = err;
				dev_err(odev->parent,
					"OMCI rollback restore failed for service %#x GEM %u: %d\n",
					old_state->config.cookie,
					old_state->config.gem_port_id, err);
			}
		}
	if (rollback_error) {
		dev_err(odev->parent,
			"OMCI service rollback incomplete after error %d\n", ret);
		ret = -EIO;
	}

release_reservations:
	xa_for_each(desired, index, state)
		if (!xa_load(&agent->services, index))
			xa_release(&agent->services, index);
	return ret;
}
