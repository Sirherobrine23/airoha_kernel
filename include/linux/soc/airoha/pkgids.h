/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __AIROHA_CHIP_ID_H_
#define __AIROHA_CHIP_ID_H_

#include <linux/kconfig.h>

struct device_node;

enum airoha_id_source {
	FROM_SCU,
	FROM_EFUSE,
	ID_SOURCE_UNKNOWN,
};

enum airoha_pkg {
	TC3169_PKG	= 0x00, /* TrendChip 3169 */
	TC3182_PKG	= 0x01, /* TrendChip 3182 */
	RT65168_PKG	= 0x02, /* Ralink 65168 */
	RT63165_PKG	= 0x03, /* Ralink 63165 */
	RT63365_PKG	= 0x04, /* Ralink 63365 */
	MT751020_PKG	= 0x05, /* Mediatek MT7510, MT7520 */
	MT7505_PKG	= 0x06, /* Mediatek MT7505 */
	EN751221_PKG	= 0x07, /* Econet EN7512, EN7521 */
	EN7526C_PKG	= 0x08, /* Econet EN7526C, EN7522 */
	EN751627_PKG	= 0x09, /* Econet EN7516, EN7527 */
	EN7580_PKG	= 0x0a, /* Econet EN7580 */
	EN7528_PKG	= 0x0b, /* Econet EN7528 */
	EN7523_PKG	= 0x0c, /* Airoha EN7523 */
	EN7581_PKG	= 0x0e, /* Airoha EN7581 */
	AN7552_PKG	= 0x0f, /* Airoha AN7552 */
	AN7583_PKG	= 0x10, /* Airoha AN7583 */

	UNKNOWN_PKG,
};

enum airoha_efuse_soc {
	EN7526F_EFUSE	= 0x00,
	EN7526D_EFUSE	= 0x01,
	EN7526G_EFUSE	= 0x02,
	EN7512_EFUSE	= 0x04,
	EN7513_EFUSE	= 0x05,
	EN7513G_EFUSE	= 0x06,
	EN7586_EFUSE	= 0x0a,
	EN7521F_EFUSE	= 0x10,
	EN7526FT_EFUSE	= 0x11,
	EN7521G_EFUSE	= 0x12,
	EN7521S_EFUSE	= 0x20,
};

enum airoha_pkg_ids {
	/* EN7523 */
	EN7529DU,
	EN7529DT,
	EN7529CU,
	EN7562DU,
	EN7562DT,
	EN7562CU,
	EN7523GU,
	EN7523DU,
	EN7529GTH,
	EN7562GTH,
	EN7523SU,
	EN7529GTS,
	EN7562GTS,
	EN7529IT,
	EN7529CT,
	EN7562CT,
	EN7523DT,
	EN7529DTM,
	EN7562DTM,
	EN7529ITM,
	EN7529CTM,
	EN7562CTM,
	EN7523DTM,

	/* EN7528 */
	EN7528HU,
	EN7528DU,
	EN7561DU,
	EN7526FH_EN7528DU,
	EN7561HU,
	EN7521G_EN7528DU,

	/* EN7580 */
	EN7580GT,
	EN7580ST,
	EN7580GAT,
	EN7565,
	EN7528GT_EN7580,
	EN7580,

	/* EN7516 / EN7527 */
	EN7516G,
	EN7527H,
	EN7527G,
	EN7561G,
	EN751627,

	/* EN7512 */
	EN7512,
	EN7513,
	EN7513G,

	/* EN7521 / EN7526 */
	EN7521FCUD,
	EN7521F,
	EN7521S,
	EN7521G,
	EN7526D,
	EN7526F,
	EN7526G,
	EN7526FT,
	EN7526FP,
	EN7526FT_C,
	EN7586,
	EN751221,

	/* MT7510 / MT7520 */
	MT7510,
	MT7511,
	MT7520S,
	MT7520,
	MT7520G,
	MT7525,
	MT7525G,

	/* AN7581 */
	AN7581GT,
	AN7566GT,
	AN7581PT,
	AN7581ST,
	AN7551PT,
	AN7581CT,
	AN7581DT,
	AN7581FG,
	AN7581FP,
	AN7581FD,
	AN7551GT,
	AN7566PT,
	AN7581IT,
	AN7581SIT,

	/* AN7552 */
	AN7552CT,
	AN7552ST,
	AN7552FT,
	AN7563CT,
	AN7563PT,

	/* AN7583 */
	AN7583GT,
	AN7583GIT,
	AN7583CT,
	AN7583DT,
	RESERVED_PKGID_4,
	AN7583ST,
	AN9510GT,
	RESERVED_PKGID_7,
	AN7553GT,
	AN7553CT,
	AN7567GT,
	AN7567CT,
	AN7583ET,
	AN7583EIT,
	RESERVED_PKGID_14,
	RESERVED_PKGID_15,
	AN7583FG,
	RESERVED_PKGID_17,
	AN7583FP,
	AN7583FD,
	RESERVED_PKGID_20,
	AN7583FS,
	AN7583FF,

	END_PACKAGE_ID = 0xffffffff,
};

/* Compatibility aliases for the names used by the previous header. */
#define EN7526FHEN7528DU	EN7526FH_EN7528DU
#define EN7521GEN7528DU		EN7521G_EN7528DU

#if IS_ENABLED(CONFIG_COMMON_CLK_EN7523)
/**
 * airoha_get_pkg_id() - Get the cached SoC package ID
 * @np: SCU clock provider node, or NULL to find an available provider
 *
 * Context: Process context. May sleep while waiting for the provider's probe.
 * Return: The package ID, or END_PACKAGE_ID if identification is unavailable.
 */
enum airoha_pkg_ids airoha_get_pkg_id(struct device_node *np);

/**
 * airoha_pkg_source() - Get the source of the cached SoC package ID
 * @np: SCU clock provider node, or NULL to find an available provider
 *
 * Context: Process context. May sleep while waiting for the provider's probe.
 * Return: FROM_SCU, FROM_EFUSE, or ID_SOURCE_UNKNOWN if unavailable.
 */
enum airoha_id_source airoha_pkg_source(struct device_node *np);
const char *airoha_id_name(enum airoha_pkg_ids id);
#else
static inline enum airoha_pkg_ids airoha_get_pkg_id(struct device_node *np)
{
	return END_PACKAGE_ID;
}

static inline enum airoha_id_source airoha_pkg_source(struct device_node *np)
{
	return ID_SOURCE_UNKNOWN;
}

static inline const char *airoha_id_name(enum airoha_pkg_ids id)
{
	return "unknown";
}
#endif

#endif
