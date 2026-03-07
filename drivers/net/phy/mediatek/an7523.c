// SPDX-License-Identifier: GPL-2.0

#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/bitfield.h>
#include <linux/mfd/syscon.h>
#include "mtk-ge-soc.h"

static int an7523_rext_cal_sw(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct phy_device *phydev_p0 = shared->phydev_p0;
	int calibration_polarity;
	u8 zcal_ctrl = 0x20;
	int first_calib;
	int ret;

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0xc000);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN |
		      MTK_PHY_RG_REXT_CALEN);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0);

	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
		       MTK_PHY_RG_REXT_ZCAL_CTRL_MASK, zcal_ctrl);

	first_calib = mtk_cal_cycle_wait(phydev_p0);
	if (first_calib < 0) {
		phydev_err(phydev, "REXT calibration failed.");
		return -EINVAL;
	}

	if (first_calib == 1)
		calibration_polarity = -1;
	else
		calibration_polarity = 1;

	while (zcal_ctrl > 0 &&
	       zcal_ctrl < FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK)) {
		zcal_ctrl += calibration_polarity;

		phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
			       MTK_PHY_RG_REXT_ZCAL_CTRL_MASK, zcal_ctrl);

		ret = mtk_cal_cycle_wait(phydev_p0);
		if (ret != first_calib)
			break;
	}

	if (ret < 0) {
		phydev_err(phydev, "REXT calibration failed during loop.");
		return -EINVAL;
	}

	if (zcal_ctrl == 0 ||
	    zcal_ctrl == FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK)) {
		zcal_ctrl = 32;
		phydev_warn(phydev, "DE REXT calibration saturation. Returning to the default %x.",
			   zcal_ctrl);
	}

	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
		       MTK_PHY_RG_REXT_TRIM_MASK,
		       FIELD_PREP(MTK_PHY_RG_REXT_TRIM_MASK, zcal_ctrl));
	phy_modify_mmd(phydev, MDIO_MMD_VEND2, MTK_PHY_RG_BG_RASEL,
		       MTK_PHY_RG_BG_RASEL_MASK,
		       FIELD_PREP(MTK_PHY_RG_BG_RASEL_MASK, zcal_ctrl >> 3));

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);

	return 0;
}

int an7523_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	int ret;

	if (!shared->rext_sw_calib_done) {
		ret = an7523_rext_cal_sw(phydev);
		if (ret)
			return ret;

		shared->rext_sw_calib_done = true;
	}

	return 0;
}