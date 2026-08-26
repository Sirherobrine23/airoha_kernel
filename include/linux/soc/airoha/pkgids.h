// SPDX-License-Identifier: GPL-2.0

#ifndef __AIROHA_CHIP_ID_H_
#define __AIROHA_CHIP_ID_H_

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define AIROHA_PKG_ID_NAME(_id) [(_id)] = #_id

#define AIROHA_NP_SCU_PDIDR		0x05c
#define AIROHA_NP_SCU_HIR		0x064
#define AIROHA_NP_SCU_SCREG_WR1	0x284

#define AIROHA_NP_SCU_HIR_MASK		GENMASK(31, 16)
#define AIROHA_NP_SCU_PDIDR_MASK	GENMASK(15, 0)
#define AIROHA_NP_SCU_PACKAGE_ID_MASK	GENMASK(3, 0)
#define AIROHA_NP_SCU_PACKAGE_ID_EXT	BIT(7)

enum airoha_pkg {
	/* AN7583 */
	AN7583_PKG = 0x10,

	/* AM7552 */
	AN7552_PKG = 0xf,

	/* EN7581 */
	EN7581_PKG = 0xe,

	/* EN7523 */
	EN7523_PKG = 0xc,

	/* EN7528 */
	EN7528_PKG = 0xb,

	/* EN7580 */
	EN7580_PKG = 0xa,

	/* EN7516, EN7527 */
	EN751627_PKG = 0x9,

	/* EN7526c, EN7522 */
	EN7526C_PKG = 0x8,

	/* EN7512, EN7521 */
	EN751221_PKG = 0x7,

	/* MT7505 */
	MT7505_PKG = 0x6,

	/* MT7510, MT7520 */
	MT751020_PKG = 0x5,
};

enum airoha_pkg_ids {
	/*EN7523*/
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

	/*EN7528*/
	EN7528HU,
	EN7528DU,
	EN7561DU,
	EN7526FHEN7528DU,
	EN7521GEN7528DU,

	/* EN7580 */
	EN7580GT,
	EN7580ST,
	EN7580GAT,
	EN7565,
	EN7580,

	/* EN7516 */
	EN7516G,

	/* EN7527 */
	EN7527G,
	EN7561G,
	EN751627,

	/* EN7512 */
	EN7512,
	EN7513,
	EN7513G,

	/* EN7521, EN7521FC */
	EN7521FCUD,
	EN7521F,
	EN7521S,
	EN7526D,
	EN7526F,
	EN7526G,
	EN7526FT,
	EN7526FP,
	EN7526FT_C,
	EN751221,

	/* MT7520 */
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

	END_PACKAGE_ID = 0xFFFFFFFF,
};

static const char *const airoha_pkg_id_names[] = {
	/* EN7523 */
	AIROHA_PKG_ID_NAME(EN7529DU),
	AIROHA_PKG_ID_NAME(EN7529DT),
	AIROHA_PKG_ID_NAME(EN7529CU),
	AIROHA_PKG_ID_NAME(EN7562DU),
	AIROHA_PKG_ID_NAME(EN7562DT),
	AIROHA_PKG_ID_NAME(EN7562CU),
	AIROHA_PKG_ID_NAME(EN7523GU),
	AIROHA_PKG_ID_NAME(EN7523DU),
	AIROHA_PKG_ID_NAME(EN7529GTH),
	AIROHA_PKG_ID_NAME(EN7562GTH),
	AIROHA_PKG_ID_NAME(EN7523SU),
	AIROHA_PKG_ID_NAME(EN7529GTS),
	AIROHA_PKG_ID_NAME(EN7562GTS),
	AIROHA_PKG_ID_NAME(EN7529IT),
	AIROHA_PKG_ID_NAME(EN7529CT),
	AIROHA_PKG_ID_NAME(EN7562CT),
	AIROHA_PKG_ID_NAME(EN7523DT),
	AIROHA_PKG_ID_NAME(EN7529DTM),
	AIROHA_PKG_ID_NAME(EN7562DTM),
	AIROHA_PKG_ID_NAME(EN7529ITM),
	AIROHA_PKG_ID_NAME(EN7529CTM),
	AIROHA_PKG_ID_NAME(EN7562CTM),
	AIROHA_PKG_ID_NAME(EN7523DTM),

	/* EN7528 */
	AIROHA_PKG_ID_NAME(EN7528HU),
	AIROHA_PKG_ID_NAME(EN7528DU),
	AIROHA_PKG_ID_NAME(EN7561DU),
	AIROHA_PKG_ID_NAME(EN7526FHEN7528DU),
	AIROHA_PKG_ID_NAME(EN7521GEN7528DU),

	/* EN7580 */
	AIROHA_PKG_ID_NAME(EN7580GT),
	AIROHA_PKG_ID_NAME(EN7580ST),
	AIROHA_PKG_ID_NAME(EN7580GAT),
	AIROHA_PKG_ID_NAME(EN7565),
	AIROHA_PKG_ID_NAME(EN7580),

	/* EN7516 */
	AIROHA_PKG_ID_NAME(EN7516G),

	/* EN7527 */
	AIROHA_PKG_ID_NAME(EN7527G),
	AIROHA_PKG_ID_NAME(EN7561G),
	AIROHA_PKG_ID_NAME(EN751627),

	/* EN7512 */
	AIROHA_PKG_ID_NAME(EN7512),
	AIROHA_PKG_ID_NAME(EN7513),
	AIROHA_PKG_ID_NAME(EN7513G),

	/* EN7521 / EN7526 */
	AIROHA_PKG_ID_NAME(EN7521FCUD),
	AIROHA_PKG_ID_NAME(EN7521F),
	AIROHA_PKG_ID_NAME(EN7521S),
	AIROHA_PKG_ID_NAME(EN7526D),
	AIROHA_PKG_ID_NAME(EN7526F),
	AIROHA_PKG_ID_NAME(EN7526G),
	AIROHA_PKG_ID_NAME(EN7526FT),
	AIROHA_PKG_ID_NAME(EN7526FP),
	AIROHA_PKG_ID_NAME(EN7526FT_C),
	AIROHA_PKG_ID_NAME(EN751221),

	/* MT7520 */
	AIROHA_PKG_ID_NAME(MT7520S),
	AIROHA_PKG_ID_NAME(MT7520),
	AIROHA_PKG_ID_NAME(MT7520G),
	AIROHA_PKG_ID_NAME(MT7525),
	AIROHA_PKG_ID_NAME(MT7525G),

	/* AN7581 */
	AIROHA_PKG_ID_NAME(AN7581GT),
	AIROHA_PKG_ID_NAME(AN7566GT),
	AIROHA_PKG_ID_NAME(AN7581PT),
	AIROHA_PKG_ID_NAME(AN7581ST),
	AIROHA_PKG_ID_NAME(AN7551PT),
	AIROHA_PKG_ID_NAME(AN7581CT),
	AIROHA_PKG_ID_NAME(AN7581DT),
	AIROHA_PKG_ID_NAME(AN7581FG),
	AIROHA_PKG_ID_NAME(AN7581FP),
	AIROHA_PKG_ID_NAME(AN7581FD),
	AIROHA_PKG_ID_NAME(AN7551GT),
	AIROHA_PKG_ID_NAME(AN7566PT),
	AIROHA_PKG_ID_NAME(AN7581IT),
	AIROHA_PKG_ID_NAME(AN7581SIT),

	/* AN7552 */
	AIROHA_PKG_ID_NAME(AN7552CT),
	AIROHA_PKG_ID_NAME(AN7552ST),
	AIROHA_PKG_ID_NAME(AN7552FT),
	AIROHA_PKG_ID_NAME(AN7563CT),
	AIROHA_PKG_ID_NAME(AN7563PT),

	/* AN7583 */
	AIROHA_PKG_ID_NAME(AN7583GT),
	AIROHA_PKG_ID_NAME(AN7583GIT),
	AIROHA_PKG_ID_NAME(AN7583CT),
	AIROHA_PKG_ID_NAME(AN7583DT),
	AIROHA_PKG_ID_NAME(AN7583ST),
	AIROHA_PKG_ID_NAME(AN9510GT),
	AIROHA_PKG_ID_NAME(AN7553GT),
	AIROHA_PKG_ID_NAME(AN7553CT),
	AIROHA_PKG_ID_NAME(AN7567GT),
	AIROHA_PKG_ID_NAME(AN7567CT),
	AIROHA_PKG_ID_NAME(AN7583ET),
	AIROHA_PKG_ID_NAME(AN7583EIT),
	AIROHA_PKG_ID_NAME(AN7583FG),
	AIROHA_PKG_ID_NAME(AN7583FP),
	AIROHA_PKG_ID_NAME(AN7583FD),
	AIROHA_PKG_ID_NAME(AN7583FS),
	AIROHA_PKG_ID_NAME(AN7583FF),
};

static inline const char *airoha_pkg_id_name(enum airoha_pkg_ids id)
{
	if (id >= END_PACKAGE_ID ||
	    (unsigned int)id >= ARRAY_SIZE(airoha_pkg_id_names) ||
	    !airoha_pkg_id_names[id])
		return "unknown";
	return airoha_pkg_id_names[id];
}

static inline const char *
airoha_pkg_id_range_name(u32 pkgid, enum airoha_pkg_ids first,
			 enum airoha_pkg_ids last)
{
	u32 id;

	if (pkgid > (u32)(last - first))
		return NULL;

	id = first + pkgid;
	if (id >= ARRAY_SIZE(airoha_pkg_id_names) ||
	    !airoha_pkg_id_names[id])
		return NULL;

	return airoha_pkg_id_names[id];
}

static inline enum airoha_pkg airoha_pkg_from_id(u32 id)
{
	switch (id) {
	case AN7583_PKG:
	case 0x7583:
	case 0x9510:
	case 0x7553:
	case 0x7567:
		return AN7583_PKG;
	case AN7552_PKG:
	case 0x7552:
	case 0x7563:
		return AN7552_PKG;
	case EN7581_PKG:
	case 0x7581:
	case 0x7566:
	case 0x7551:
		return EN7581_PKG;
	case EN7523_PKG:
	case 0x7523:
	case 0x7529:
	case 0x7562:
		return EN7523_PKG;
	case EN7528_PKG:
	case 0x7528:
	case 0x7561:
		return EN7528_PKG;
	case EN7580_PKG:
	case 0x7580:
	case 0x7565:
		return EN7580_PKG;
	case EN751627_PKG:
	case 0x7516:
	case 0x7527:
		return EN751627_PKG;
	case EN7526C_PKG:
	case 0x7522:
		return EN7526C_PKG;
	case EN751221_PKG:
	case 0x7512:
	case 0x7513:
	case 0x7521:
	case 0x7526:
		return EN751221_PKG;
	case MT751020_PKG:
	case 0x7510:
	case 0x7520:
	case 0x7525:
		return MT751020_PKG;
	default:
		return 0;
	}
}

static inline const char *airoha_pkg_family_name(enum airoha_pkg pkg)
{
	switch (airoha_pkg_from_id(pkg)) {
	case AN7583_PKG:
		return "AN7583";
	case AN7552_PKG:
		return "AN7552";
	case EN7581_PKG:
		return "EN7581";
	case EN7523_PKG:
		return "EN7523";
	case EN7528_PKG:
		return "EN7528";
	case EN7580_PKG:
		return "EN7580";
	case EN751627_PKG:
		return "EN7516/EN7527";
	case EN7526C_PKG:
		return "EN7526C/EN7522";
	case EN751221_PKG:
		return "EN7512/EN7521";
	case MT751020_PKG:
		return "MT7510/MT7520";
	default:
		return NULL;
	}
}

static inline const char *
airoha_soc_variant_name(enum airoha_pkg pkg, u32 pkgid)
{
	switch (airoha_pkg_from_id(pkg)) {
	case AN7583_PKG:
		return airoha_pkg_id_range_name(pkgid, AN7583GT, AN7583FF);
	case AN7552_PKG:
		return airoha_pkg_id_range_name(pkgid, AN7552CT, AN7563PT);
	case EN7581_PKG:
		return airoha_pkg_id_range_name(pkgid, AN7581GT, AN7581SIT);
	case EN7523_PKG:
		return airoha_pkg_id_range_name(pkgid, EN7529DU, EN7523DTM);
	case EN7528_PKG:
		return airoha_pkg_id_range_name(pkgid, EN7528HU,
						EN7521GEN7528DU);
	case EN7580_PKG:
		return airoha_pkg_id_range_name(pkgid, EN7580GT, EN7580);
	case EN751627_PKG:
		return airoha_pkg_id_range_name(pkgid, EN7516G, EN751627);
	case EN7526C_PKG:
		return airoha_pkg_id_range_name(pkgid, EN7521FCUD, EN751221);
	case EN751221_PKG:
		return airoha_pkg_id_range_name(pkgid, EN7512, EN751221);
	case MT751020_PKG:
		return airoha_pkg_id_range_name(pkgid, MT7520S, MT7525G);
	default:
		return NULL;
	}
}

static inline const char *airoha_soc_name(enum airoha_pkg pkg, u32 pkgid)
{
	const char *name;

	name = airoha_soc_variant_name(pkg, pkgid);
	if (name)
		return name;

	name = airoha_pkg_family_name(pkg);
	if (name)
		return name;

	return "unknown";
}

static inline const char *
airoha_soc_name_from_regs(u32 hir, u32 pkgid, u32 pdidr)
{
	enum airoha_pkg hir_pkg = airoha_pkg_from_id(hir);
	enum airoha_pkg pdidr_pkg = airoha_pkg_from_id(pdidr);
	const char *name;

	name = airoha_soc_variant_name(hir_pkg, pkgid);
	if (name)
		return name;

	name = airoha_soc_variant_name(pdidr_pkg, pkgid);
	if (name)
		return name;

	name = airoha_pkg_family_name(hir_pkg);
	if (name)
		return name;

	name = airoha_pkg_family_name(pdidr_pkg);
	if (name)
		return name;

	return "unknown";
}

static inline u32 airoha_pkgid_from_screg(u32 value)
{
	u32 pkgid = FIELD_GET(AIROHA_NP_SCU_PACKAGE_ID_MASK, value);

	if (value & AIROHA_NP_SCU_PACKAGE_ID_EXT)
		pkgid |= BIT(4);

	return pkgid;
}

/*
 * Package IDs are family-relative values stored by the bootloader in the
 * NP-SCU watchdog-reset scratch register 1. A value of zero is valid.
 */
static inline u32 get_pkgid(struct regmap *np_scu)
{
	u32 value;
	int err;

	err = regmap_read(np_scu, AIROHA_NP_SCU_SCREG_WR1, &value);
	if (err)
		return END_PACKAGE_ID;

	return airoha_pkgid_from_screg(value);
}

static inline u32 get_pkgid_mem(void __iomem *np_scu)
{
	return airoha_pkgid_from_screg(readl(np_scu +
					     AIROHA_NP_SCU_SCREG_WR1));
}

/* HIR identifies the SoC family, for example EN7523_PKG (0x0c). */
static inline enum airoha_pkg get_pkg(struct regmap *np_scu)
{
	u32 value;
	int err;

	err = regmap_read(np_scu, AIROHA_NP_SCU_HIR, &value);
	if (err)
		return 0;

	return FIELD_GET(AIROHA_NP_SCU_HIR_MASK, value);
}

static inline enum airoha_pkg get_pkg_mem(void __iomem *np_scu)
{
	return FIELD_GET(AIROHA_NP_SCU_HIR_MASK,
			 readl(np_scu + AIROHA_NP_SCU_HIR));
}

static inline u32 get_pdidr(struct regmap *np_scu)
{
	u32 value;
	int err;

	err = regmap_read(np_scu, AIROHA_NP_SCU_PDIDR, &value);
	if (err)
		return 0;

	return FIELD_GET(AIROHA_NP_SCU_PDIDR_MASK, value);
}

static inline u32 get_pdidr_mem(void __iomem *np_scu)
{
	return FIELD_GET(AIROHA_NP_SCU_PDIDR_MASK,
			 readl(np_scu + AIROHA_NP_SCU_PDIDR));
}

#endif
