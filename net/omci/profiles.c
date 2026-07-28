// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI OLT interoperability profiles
 *
 * Profiles describe policy deviations required by selected OLT families.
 * Automatic detection is the default and uses OLT-G identification attributes
 * only as a hint. Until OLT-G is available, the effective profile is generic.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "internal.h"

struct omci_profile_desc {
	u8 profile;
	u32 quirks;
	const char *name;
};

static const struct omci_profile_desc omci_profiles[] = {
	{
		.profile = OMCI_OLT_PROFILE_GENERIC,
		.name = "generic",
	}, {
		.profile = OMCI_OLT_PROFILE_AUTO,
		.name = "auto",
	}, {
		.profile = OMCI_OLT_PROFILE_NOKIA_ALCL,
		.quirks = OMCI_OLT_QUIRK_SANITIZE_VERSION |
			  OMCI_OLT_QUIRK_VENDOR_SPECIFIC_MES,
		.name = "nokia-alcl",
	}, {
		.profile = OMCI_OLT_PROFILE_DASAN,
		.quirks = OMCI_OLT_QUIRK_ALLOW_SET_CREATE |
			  OMCI_OLT_QUIRK_FAKE_UNSUPPORTED_SUCCESS |
			  OMCI_OLT_QUIRK_IGNORE_UNSUPPORTED_UNI |
			  OMCI_OLT_QUIRK_DASAN_MULTICAST_ANI,
		.name = "dasan",
	}, {
		.profile = OMCI_OLT_PROFILE_HUAWEI,
		.quirks = OMCI_OLT_QUIRK_VENDOR_SPECIFIC_MES,
		.name = "huawei",
	}, {
		.profile = OMCI_OLT_PROFILE_FIBERHOME,
		.quirks = OMCI_OLT_QUIRK_ALLOW_SET_CREATE |
			  OMCI_OLT_QUIRK_FULL_UNI_ENTITY_ID,
		.name = "fiberhome",
	}, {
		.profile = OMCI_OLT_PROFILE_ZTE,
		.quirks = OMCI_OLT_QUIRK_ALLOW_SET_CREATE |
			  OMCI_OLT_QUIRK_ZTE_VLAN_TAG_MODE,
		.name = "zte",
	},
};

static const struct omci_profile_desc *omci_profile_find(u8 profile)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_profiles); i++)
		if (omci_profiles[i].profile == profile)
			return &omci_profiles[i];

	return NULL;
}

bool omci_profile_valid(u8 profile)
{
	return !!omci_profile_find(profile);
}

bool omci_profile_forceable(u8 profile)
{
	return profile == OMCI_OLT_PROFILE_UNSPEC ||
	       (profile != OMCI_OLT_PROFILE_AUTO && omci_profile_valid(profile));
}

u32 omci_profile_quirks(u8 profile)
{
	const struct omci_profile_desc *desc = omci_profile_find(profile);

	return desc ? desc->quirks : 0;
}

const char *omci_olt_profile_name(u8 profile)
{
	const struct omci_profile_desc *desc = omci_profile_find(profile);

	return desc ? desc->name : "unspecified";
}
EXPORT_SYMBOL_GPL(omci_olt_profile_name);

static bool omci_profile_vendor_is(const struct omci_olt_g *olt,
				   const char vendor[OMCI_OLT_VENDOR_ID_LEN])
{
	return olt && olt->vendor_id_valid &&
	       !strncasecmp(olt->vendor_id, vendor, OMCI_OLT_VENDOR_ID_LEN);
}

static bool omci_profile_field_contains(const char *field, size_t len,
					const char *token)
{
	size_t token_len = strlen(token);
	size_t i;

	if (!token_len || token_len > len)
		return false;

	for (i = 0; i + token_len <= len && field[i]; i++)
		if (!strncasecmp(field + i, token, token_len))
			return true;

	return false;
}

u8 omci_profile_detect(const struct omci_olt_g *olt)
{
	if (!olt || !olt->valid)
		return OMCI_OLT_PROFILE_GENERIC;

	if (omci_profile_vendor_is(olt, "ALCL") ||
	    omci_profile_vendor_is(olt, "NOKI"))
		return OMCI_OLT_PROFILE_NOKIA_ALCL;
	if (omci_profile_vendor_is(olt, "DSNW") ||
	    omci_profile_vendor_is(olt, "DASA"))
		return OMCI_OLT_PROFILE_DASAN;
	if (omci_profile_vendor_is(olt, "HWTC") ||
	    omci_profile_vendor_is(olt, "HWTI"))
		return OMCI_OLT_PROFILE_HUAWEI;
	if (omci_profile_vendor_is(olt, "FHTT") ||
	    omci_profile_vendor_is(olt, "FHTC"))
		return OMCI_OLT_PROFILE_FIBERHOME;
	if (omci_profile_vendor_is(olt, "ZTEG") ||
	    omci_profile_vendor_is(olt, "ZTEC"))
		return OMCI_OLT_PROFILE_ZTE;

	if (olt->equipment_id_valid) {
		if (omci_profile_field_contains(olt->equipment_id,
						OMCI_OLT_EQUIPMENT_ID_LEN,
						"NOKIA") ||
		    omci_profile_field_contains(olt->equipment_id,
						OMCI_OLT_EQUIPMENT_ID_LEN,
						"ALCATEL"))
			return OMCI_OLT_PROFILE_NOKIA_ALCL;
		if (omci_profile_field_contains(olt->equipment_id,
						OMCI_OLT_EQUIPMENT_ID_LEN,
						"DASAN"))
			return OMCI_OLT_PROFILE_DASAN;
		if (omci_profile_field_contains(olt->equipment_id,
						OMCI_OLT_EQUIPMENT_ID_LEN,
						"HUAWEI"))
			return OMCI_OLT_PROFILE_HUAWEI;
		if (omci_profile_field_contains(olt->equipment_id,
						OMCI_OLT_EQUIPMENT_ID_LEN,
						"FIBERHOME"))
			return OMCI_OLT_PROFILE_FIBERHOME;
		if (omci_profile_field_contains(olt->equipment_id,
						OMCI_OLT_EQUIPMENT_ID_LEN,
						"ZTE"))
			return OMCI_OLT_PROFILE_ZTE;
	}

	return OMCI_OLT_PROFILE_GENERIC;
}

void omci_profile_sanitize_olt_g(struct omci_olt_g *olt, u32 quirks)
{
	unsigned int i;

	if (!olt || !(quirks & OMCI_OLT_QUIRK_SANITIZE_VERSION) ||
	    !olt->version_valid)
		return;

	for (i = 0; i < OMCI_OLT_VERSION_LEN && olt->version[i]; i++)
		if (olt->version[i] < ' ' || olt->version[i] > '~')
			olt->version[i] = ' ';
}

u16 omci_profile_normalize_uni_entity(u8 profile, u16 entity_id)
{
	if (omci_profile_quirks(profile) & OMCI_OLT_QUIRK_FULL_UNI_ENTITY_ID)
		return entity_id;

	return entity_id & 0xff;
}

void omci_profile_normalize_vlan_rule(u8 profile,
				      struct omci_extended_vlan_rule *rule)
{
	if (!rule || !(omci_profile_quirks(profile) &
		       OMCI_OLT_QUIRK_ZTE_VLAN_TAG_MODE))
		return;

	/*
	 * The vendor SDK forces a TPID and DEI when a ZTE rule produces an
	 * untagged service. Preserve the wire rule, but normalize the treatment
	 * consumed by the hardware-independent service resolver.
	 */
	if (!rule->tags_to_remove && rule->treat_outer_vid >= 4096 &&
	    rule->treat_inner_vid >= 4096) {
		rule->treat_inner_tpid_dei = 1;
		rule->treat_inner_pbit = min_t(u8, rule->treat_inner_pbit, 8);
	}
}

int omci_profile_resolve_multicast_ani(u8 profile,
				       u16 bridge_port_entity_id,
				       u16 *ani_entity_id)
{
	if (!ani_entity_id)
		return -EINVAL;
	if (!(omci_profile_quirks(profile) &
	      OMCI_OLT_QUIRK_DASAN_MULTICAST_ANI))
		return -EOPNOTSUPP;

	/*
	 * The legacy SDK associates a multicast GEM with the ANI allocated to
	 * its WAN MAC bridge port. Keep that stable semantic identifier in the
	 * normalized service; the hardware backend owns index allocation.
	 */
	*ani_entity_id = bridge_port_entity_id;
	return 0;
}
