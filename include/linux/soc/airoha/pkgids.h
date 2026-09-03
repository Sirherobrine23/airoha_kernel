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
#define AIROHA_NP_SCU_SUBMODEL		0x0f8
#define AIROHA_NP_SCU_SCREG_WR1	0x284

#define AIROHA_NP_SCU_HIR_MASK		GENMASK(31, 16)
#define AIROHA_NP_SCU_PDIDR_MASK	GENMASK(15, 0)
#define AIROHA_NP_SCU_PACKAGE_ID_MASK	GENMASK(3, 0)
#define AIROHA_NP_SCU_PACKAGE_ID_EXT	BIT(7)
#define AIROHA_NP_SCU_SUBMODEL_MASK	GENMASK(1, 0)

/* Before i2c reg: 0x1FBF8100 */

/* Physical addresses used by the legacy MIPS EcoNet identification logic. */
#define AIROHA_EFUSE_VERIFY_DATA0_PHYS	0x1fbf8214
#define AIROHA_EFUSE_VERIFY_DATA1_PHYS	0x1fbf8218
#define AIROHA_CHIP_SCU_PACKAGE_PHYS	0x1fa20174
#define AIROHA_CHIP_SCU_CONFIG_PHYS	0x1fa201ec

/* EN7512/EN7521 and EN7526C/EN7522 eFuse encoding. */
#define AIROHA_EFUSE_PKG_MASK		GENMASK(5, 0)
#define AIROHA_EFUSE_PKG_IGNORE_BIT01	GENMASK(5, 2)
#define AIROHA_EFUSE_REMARK_BIT	BIT(6)
#define AIROHA_EFUSE_PKG_REMARK_SHIFT	7

#define AIROHA_EFUSE_EN7526F		0x00
#define AIROHA_EFUSE_EN7526D		0x01
#define AIROHA_EFUSE_EN7526G		0x02
#define AIROHA_EFUSE_EN7512		0x04
#define AIROHA_EFUSE_EN7513		0x05
#define AIROHA_EFUSE_EN7513G		0x06
#define AIROHA_EFUSE_EN7586		0x0a
#define AIROHA_EFUSE_EN7521F		0x10
#define AIROHA_EFUSE_EN7526FT		0x11
#define AIROHA_EFUSE_EN7521G		0x12
#define AIROHA_EFUSE_EN7521S		0x20

#define AIROHA_EFUSE_DDR3_BIT		BIT(23)
#define AIROHA_EFUSE_DDR3_REMARK_BIT	BIT(24)

/* EN7516/EN7527 eFuse encoding. */
#define AIROHA_EFUSE_7516_PKG_MASK	GENMASK(19, 18)
#define AIROHA_EFUSE_7516_REMARK_BIT	BIT(0)
#define AIROHA_EFUSE_7516_REMARK_SHIFT	2

#define AIROHA_EFUSE_EN7527H		0x00000
#define AIROHA_EFUSE_EN7527G		0x00000
#define AIROHA_EFUSE_EN7516G		0x80000
#define AIROHA_EFUSE_EN7561G		0xc0000

/* Additional package selectors used by the legacy vendor code. */
#define AIROHA_EN751627_QFP_MASK	BIT(15)
#define AIROHA_EN7526C_FP_MASK		BIT(10)
#define AIROHA_EN7526C_FTC_BIT		BIT(9)
#define AIROHA_EN7526C_FTC_REMARK_BIT	BIT(10)

/* MT7510/MT7520 module-selection bits in EFUSE_VERIFY_DATA0. */
#define AIROHA_MT751020_ENP_SEL		BIT(3)
#define AIROHA_MT751020_ENP_LOW		BIT(1)
#define AIROHA_MT751020_ENP_HIGH	BIT(5)
#define AIROHA_MT751020_ENS_LOW		BIT(2)
#define AIROHA_MT751020_ENS_HIGH	BIT(6)

enum airoha_pkg {
	/* AN7583 */
	AN7583_PKG = 0x10,

	/* AN7552 */
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

	/* EN7526C, EN7522 */
	EN7526C_PKG = 0x8,

	/* EN7512, EN7521 */
	EN751221_PKG = 0x7,

	/* MT7505 */
	MT7505_PKG = 0x6,

	/* MT7510, MT7520 */
	MT751020_PKG = 0x5,
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
#define EN7521GEN7528DU	EN7521G_EN7528DU

static const char *const airoha_pkg_id_names[] = {
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
	AIROHA_PKG_ID_NAME(EN7528HU),
	AIROHA_PKG_ID_NAME(EN7528DU),
	AIROHA_PKG_ID_NAME(EN7561DU),
	AIROHA_PKG_ID_NAME(EN7526FH_EN7528DU),
	AIROHA_PKG_ID_NAME(EN7561HU),
	AIROHA_PKG_ID_NAME(EN7521G_EN7528DU),
	AIROHA_PKG_ID_NAME(EN7580GT),
	AIROHA_PKG_ID_NAME(EN7580ST),
	AIROHA_PKG_ID_NAME(EN7580GAT),
	AIROHA_PKG_ID_NAME(EN7565),
	AIROHA_PKG_ID_NAME(EN7528GT_EN7580),
	AIROHA_PKG_ID_NAME(EN7580),
	AIROHA_PKG_ID_NAME(EN7516G),
	AIROHA_PKG_ID_NAME(EN7527H),
	AIROHA_PKG_ID_NAME(EN7527G),
	AIROHA_PKG_ID_NAME(EN7561G),
	AIROHA_PKG_ID_NAME(EN751627),
	AIROHA_PKG_ID_NAME(EN7512),
	AIROHA_PKG_ID_NAME(EN7513),
	AIROHA_PKG_ID_NAME(EN7513G),
	AIROHA_PKG_ID_NAME(EN7521FCUD),
	AIROHA_PKG_ID_NAME(EN7521F),
	AIROHA_PKG_ID_NAME(EN7521S),
	AIROHA_PKG_ID_NAME(EN7521G),
	AIROHA_PKG_ID_NAME(EN7526D),
	AIROHA_PKG_ID_NAME(EN7526F),
	AIROHA_PKG_ID_NAME(EN7526G),
	AIROHA_PKG_ID_NAME(EN7526FT),
	AIROHA_PKG_ID_NAME(EN7526FP),
	AIROHA_PKG_ID_NAME(EN7526FT_C),
	AIROHA_PKG_ID_NAME(EN7586),
	AIROHA_PKG_ID_NAME(EN751221),
	AIROHA_PKG_ID_NAME(MT7510),
	AIROHA_PKG_ID_NAME(MT7511),
	AIROHA_PKG_ID_NAME(MT7520S),
	AIROHA_PKG_ID_NAME(MT7520),
	AIROHA_PKG_ID_NAME(MT7520G),
	AIROHA_PKG_ID_NAME(MT7525),
	AIROHA_PKG_ID_NAME(MT7525G),
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
	AIROHA_PKG_ID_NAME(AN7552CT),
	AIROHA_PKG_ID_NAME(AN7552ST),
	AIROHA_PKG_ID_NAME(AN7552FT),
	AIROHA_PKG_ID_NAME(AN7563CT),
	AIROHA_PKG_ID_NAME(AN7563PT),
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

struct airoha_soc_id_regs {
	u32 hir;
	u32 pdidr;
	u32 pkgid;
	u32 efuse_data0;
	u32 efuse_data1;
	u32 np_scu_submodel;
	u32 chip_scu_package;
	u32 chip_scu_config;
};

static inline const char *airoha_pkg_id_name(enum airoha_pkg_ids id)
{
	if (id == END_PACKAGE_ID ||
	    (unsigned int)id >= ARRAY_SIZE(airoha_pkg_id_names) ||
	    !airoha_pkg_id_names[id])
		return "unknown";

	return airoha_pkg_id_names[id];
}

static inline enum airoha_pkg_ids
airoha_pkg_id_from_range(u32 pkgid, enum airoha_pkg_ids first,
			 enum airoha_pkg_ids last)
{
	u32 id;

	if (pkgid > (u32)(last - first))
		return END_PACKAGE_ID;

	id = first + pkgid;
	if (id >= ARRAY_SIZE(airoha_pkg_id_names) ||
	    !airoha_pkg_id_names[id])
		return END_PACKAGE_ID;

	return id;
}

static inline const char *
airoha_pkg_id_range_name(u32 pkgid, enum airoha_pkg_ids first,
			 enum airoha_pkg_ids last)
{
	enum airoha_pkg_ids id;

	id = airoha_pkg_id_from_range(pkgid, first, last);
	if (id == END_PACKAGE_ID)
		return NULL;

	return airoha_pkg_id_name(id);
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

static inline u32 airoha_efuse_pkg_value(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_EFUSE_REMARK_BIT)
		return (efuse_data0 >> AIROHA_EFUSE_PKG_REMARK_SHIFT) &
		       AIROHA_EFUSE_PKG_MASK;

	return efuse_data0 & AIROHA_EFUSE_PKG_MASK;
}

static inline u32 airoha_efuse_7516_pkg_value(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_EFUSE_7516_REMARK_BIT)
		return (efuse_data0 >> AIROHA_EFUSE_7516_REMARK_SHIFT) &
		       AIROHA_EFUSE_7516_PKG_MASK;

	return efuse_data0 & AIROHA_EFUSE_7516_PKG_MASK;
}

static inline bool airoha_efuse_is_ddr3(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_EFUSE_REMARK_BIT)
		return !!(efuse_data0 & AIROHA_EFUSE_DDR3_REMARK_BIT);

	return !!(efuse_data0 & AIROHA_EFUSE_DDR3_BIT);
}

static inline bool
airoha_en7526c_is_ft_c(u32 efuse_data0, u32 efuse_data1)
{
	if (efuse_data0 & AIROHA_EFUSE_REMARK_BIT)
		return !!(efuse_data1 & AIROHA_EN7526C_FTC_REMARK_BIT);

	return !!(efuse_data1 & AIROHA_EN7526C_FTC_BIT);
}

static inline enum airoha_pkg_ids
airoha_en751221_variant_id(u32 efuse_data0)
{
	switch (airoha_efuse_pkg_value(efuse_data0)) {
	case AIROHA_EFUSE_EN7526F:
		return EN7526F;
	case AIROHA_EFUSE_EN7526D:
		return EN7526D;
	case AIROHA_EFUSE_EN7526G:
		return EN7526G;
	case AIROHA_EFUSE_EN7512:
		return EN7512;
	case AIROHA_EFUSE_EN7513:
		return EN7513;
	case AIROHA_EFUSE_EN7513G:
		return EN7513G;
	case AIROHA_EFUSE_EN7586:
		return EN7586;
	case AIROHA_EFUSE_EN7521F:
		return EN7521F;
	case AIROHA_EFUSE_EN7526FT:
		return EN7526FT;
	case AIROHA_EFUSE_EN7521G:
		return EN7521G;
	case AIROHA_EFUSE_EN7521S:
		return EN7521S;
	default:
		return EN751221;
	}
}

static inline enum airoha_pkg_ids
airoha_en7526c_variant_id(u32 efuse_data0, u32 efuse_data1,
			 u32 chip_scu_config)
{
	u32 pkg = airoha_efuse_pkg_value(efuse_data0) &
		  AIROHA_EFUSE_PKG_IGNORE_BIT01;

	/* The vendor macros make these refinements orthogonal to the base ID. */
	if (airoha_en7526c_is_ft_c(efuse_data0, efuse_data1))
		return EN7526FT_C;

	switch (pkg) {
	case AIROHA_EFUSE_EN7526F:
		if (chip_scu_config & AIROHA_EN7526C_FP_MASK)
			return EN7526FP;
		return EN7526F;
	case AIROHA_EFUSE_EN7521F:
		if (airoha_efuse_is_ddr3(efuse_data0))
			return EN7521FCUD;
		return EN7521F;
	case AIROHA_EFUSE_EN7521S:
		return EN7521S;
	default:
		return END_PACKAGE_ID;
	}
}

static inline enum airoha_pkg_ids
airoha_en751627_variant_id(u32 efuse_data0, u32 chip_scu_package)
{
	switch (airoha_efuse_7516_pkg_value(efuse_data0)) {
	case AIROHA_EFUSE_EN7527G:
		if (chip_scu_package & AIROHA_EN751627_QFP_MASK)
			return EN7527H;
		return EN7527G;
	case AIROHA_EFUSE_EN7516G:
		return EN7516G;
	case AIROHA_EFUSE_EN7561G:
		return EN7561G;
	default:
		return EN751627;
	}
}

static inline bool airoha_mt751020_is_enp(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_MT751020_ENP_SEL)
		return !!(efuse_data0 & AIROHA_MT751020_ENP_HIGH);

	return !!(efuse_data0 & AIROHA_MT751020_ENP_LOW);
}

static inline bool airoha_mt751020_is_ens(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_MT751020_ENP_SEL)
		return !!(efuse_data0 & AIROHA_MT751020_ENS_HIGH);

	return !!(efuse_data0 & AIROHA_MT751020_ENS_LOW);
}

static inline enum airoha_pkg_ids
airoha_mt751020_variant_id(u32 submodel, u32 efuse_data0)
{
	bool enp = airoha_mt751020_is_enp(efuse_data0);
	bool ens = airoha_mt751020_is_ens(efuse_data0);

	switch (FIELD_GET(AIROHA_NP_SCU_SUBMODEL_MASK, submodel)) {
	case 0:
		return enp ? MT7510 : MT7511;
	case 2:
		if (ens)
			return MT7520S;
		return enp ? MT7520 : MT7525;
	case 3:
		return enp ? MT7520G : MT7525G;
	default:
		return END_PACKAGE_ID;
	}
}

static inline enum airoha_pkg_ids
airoha_pkgid_variant_id(enum airoha_pkg pkg, u32 pkgid)
{
	switch (airoha_pkg_from_id(pkg)) {
	case AN7583_PKG:
		return airoha_pkg_id_from_range(pkgid, AN7583GT, AN7583FF);
	case AN7552_PKG:
		return airoha_pkg_id_from_range(pkgid, AN7552CT, AN7563PT);
	case EN7581_PKG:
		return airoha_pkg_id_from_range(pkgid, AN7581GT, AN7581SIT);
	case EN7523_PKG:
		return airoha_pkg_id_from_range(pkgid, EN7529DU, EN7523DTM);
	case EN7528_PKG:
		switch (pkgid) {
		case 0x0:
			return EN7528HU;
		case 0x1:
			return EN7528DU;
		case 0x2:
			return EN7561DU;
		case 0x3:
			return EN7526FH_EN7528DU;
		case 0x4:
			return EN7561HU;
		case 0x7:
			return EN7521G_EN7528DU;
		default:
			return END_PACKAGE_ID;
		}
	case EN7580_PKG:
		switch (pkgid) {
		case 0x0:
			return EN7580GT;
		case 0x1:
			return EN7580ST;
		case 0x2:
			return EN7580GAT;
		case 0x3:
			return EN7565;
		case 0x4:
			return EN7528GT_EN7580;
		default:
			return END_PACKAGE_ID;
		}
	default:
		return END_PACKAGE_ID;
	}
}

static inline enum airoha_pkg_ids
airoha_soc_variant_id(const struct airoha_soc_id_regs *regs)
{
	enum airoha_pkg pkg;

	pkg = airoha_pkg_from_id(regs->hir);
	if (!pkg)
		pkg = airoha_pkg_from_id(regs->pdidr);

	switch (pkg) {
	case EN751221_PKG:
		return airoha_en751221_variant_id(regs->efuse_data0);
	case EN7526C_PKG:
		return airoha_en7526c_variant_id(regs->efuse_data0,
						  regs->efuse_data1,
						  regs->chip_scu_config);
	case EN751627_PKG:
		return airoha_en751627_variant_id(regs->efuse_data0,
						   regs->chip_scu_package);
	case MT751020_PKG:
		return airoha_mt751020_variant_id(regs->np_scu_submodel,
						   regs->efuse_data0);
	default:
		return airoha_pkgid_variant_id(pkg, regs->pkgid);
	}
}

static inline const char *
airoha_soc_variant_name_from_id_regs(const struct airoha_soc_id_regs *regs)
{
	enum airoha_pkg_ids id = airoha_soc_variant_id(regs);

	if (id == END_PACKAGE_ID)
		return NULL;

	return airoha_pkg_id_name(id);
}

static inline const char *
airoha_soc_name_from_id_regs(const struct airoha_soc_id_regs *regs)
{
	enum airoha_pkg pkg;
	const char *name;

	name = airoha_soc_variant_name_from_id_regs(regs);
	if (name)
		return name;

	pkg = airoha_pkg_from_id(regs->hir);
	if (!pkg)
		pkg = airoha_pkg_from_id(regs->pdidr);

	name = airoha_pkg_family_name(pkg);
	if (name)
		return name;

	return "unknown";
}

/*
 * Package-only helper. EN751221, EN7526C, EN751627 and MT751020 require
 * additional eFuse/strap data and intentionally resolve only to a family here.
 */
static inline const char *airoha_soc_variant_name(enum airoha_pkg pkg, u32 pkgid)
{
	enum airoha_pkg_ids id = airoha_pkgid_variant_id(pkg, pkgid);

	if (id == END_PACKAGE_ID)
		return NULL;

	return airoha_pkg_id_name(id);
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

/*
 * Legacy API retained for callers that only have HIR/PKGID/PDIDR. It does not
 * guess an eFuse-selected variant when the required eFuse values are absent.
 */
static inline const char *
airoha_soc_name_from_regs(u32 hir, u32 pkgid, u32 pdidr)
{
	enum airoha_pkg pkg = airoha_pkg_from_id(hir);
	const char *name;

	if (!pkg)
		pkg = airoha_pkg_from_id(pdidr);

	name = airoha_soc_variant_name(pkg, pkgid);
	if (name)
		return name;

	name = airoha_pkg_family_name(pkg);
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

static inline u32 get_submodel_mem(void __iomem *np_scu)
{
	return readl(np_scu + AIROHA_NP_SCU_SUBMODEL);
}

#endif
