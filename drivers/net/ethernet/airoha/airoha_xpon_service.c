// SPDX-License-Identifier: GPL-2.0-only
/* Shared software classifier table for Airoha and EcoNet xPON. */

#include <linux/errno.h>
#include <linux/string.h>

#include "airoha_eth.h"

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
	       a->default_service == b->default_service;
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
