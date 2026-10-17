// SPDX-License-Identifier: GPL-2.0-only
/* Reconcile normalized OMCI services with a hardware provider. */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "internal.h"

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
	       a->default_service == b->default_service;
}

static bool omci_service_unchanged(struct xarray *services,
				   const struct omci_service_state *state)
{
	struct omci_service_state *other;

	other = xa_load(services, state->config.cookie);
	return other && omci_service_equal(&other->config, &state->config);
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
		count++;
	configs = kvmalloc_array(count, sizeof(*configs), GFP_KERNEL);
	if (!configs && count)
		return -ENOMEM;
	xa_for_each(desired, index, state)
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
		ret = odev->ops->delete_service(odev, old_state->config.cookie);
		if (ret && ret != -ENOENT) {
			dev_warn(odev->parent,
				 "OMCI delete failed for service %#x: %d; rolling back\n",
				 old_state->config.cookie, ret);
			goto rollback_hardware;
		}
	}

	/* Reapply unchanged rules too: a GPON restart may have lost them. */
	xa_for_each(desired, index, state) {
		ret = odev->ops->replace_service(odev, &state->config);
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
			err = odev->ops->delete_service(odev, state->config.cookie);
			if (err && err != -ENOENT) {
				rollback_error = err;
				dev_err(odev->parent,
					"OMCI rollback delete failed for service %#x: %d\n",
					state->config.cookie, err);
			}
		}
	if (odev->ops->replace_service)
		xa_for_each(&agent->services, index, old_state) {
			err = odev->ops->replace_service(odev, &old_state->config);
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
