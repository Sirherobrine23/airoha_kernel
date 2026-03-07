// SPDX-License-Identifier: GPL-2.0
#include <linux/bitfield.h>
#include <linux/phy.h>
#include "mtk-ge-soc.h"

static int airoha_cal_cycle(struct phy_device *phydev, int devad,
			    u32 regnum, u16 mask, u16 cal_val)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct phy_device *phydev_p0;
	int ret;

	phydev_p0 = shared->phydev_p0;

	phy_modify_mmd(phydev, devad, regnum, mask, cal_val);

	ret = mtk_cal_cycle_wait(phydev_p0);
	phydev_dbg(phydev, "cal_val: 0x%x, ret: %d\n", cal_val, ret);

	return ret;
}

static int airoha_rext_cal_sw(struct phy_device *phydev)
{
	int calibration_polarity;
	u8 zcal_ctrl = 32;
	int first_calib;
	int ret;

	/* BG voltage output */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0xc000);
	/* tst_mode2 */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xff, 0x2);

	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, 0xff,
			   GENMASK(15, 4) | GENMASK(1, 0));

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN |
		      MTK_PHY_RG_REXT_CALEN);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0);

	phydev_dbg(phydev, "Start REXT SW cal.\n");
	first_calib = airoha_cal_cycle(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
				       MTK_PHY_RG_REXT_ZCAL_CTRL_MASK, zcal_ctrl);

	if (first_calib < 0) {
		phydev_err(phydev, "REXT SW calibration failed.\n");
		return -EINVAL;
	}

	/* If REXT calibration failed:
	 * - increase dB until calibration succeed.
	 * If REXT calibration succeeded:
	 * - decrease dB until calibration fail to fine tune it.
	 */
	if (first_calib == 1)
		calibration_polarity = -1;
	else
		calibration_polarity = 1;

	while (zcal_ctrl > 0 &&
	       zcal_ctrl < FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK)) {
		zcal_ctrl += calibration_polarity;

		ret = airoha_cal_cycle(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
				       MTK_PHY_RG_REXT_ZCAL_CTRL_MASK, zcal_ctrl);
		/* Exit if we either failed or succeeded compared to the
		 * first calibration result. (aka we finished fine tuning or
		 * we succeeded with calibration)
		 */
		if (ret != first_calib)
			break;
	}

	if (ret < 0) {
		phydev_err(phydev, "REXT SW calibration failed.\n");
		return -EINVAL;
	}

	if (zcal_ctrl == 0 ||
	    zcal_ctrl == FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK)) {
		zcal_ctrl = 32;
		phydev_err(phydev, "REXT SW calibration saturation. Defaulting to %x.\n",
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

static int airoha_tx_offset_cal_sw(struct phy_device *phydev, u8 txg_calen_x)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct phy_device *phydev_p0;
	u16 dev1e_145_tmp, bmcr_tmp;
	int calibration_polarity;
	u16 reg_dac1, reg_dac2;
	int zcal_ctrl = 0;
	int first_calib;
	u16 reg, mask;
	int ret;

	phydev_p0 = shared->phydev_p0;

	/* BG voltage output */
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0xc000);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
		      MTK_PHY_RG_TXVOS_CALEN);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
		      MTK_PHY_RG_TXVOS_CALEN);

	/* Force 1G full duplex for calibration */
	bmcr_tmp = phy_read(phydev, MII_BMCR);
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);

	/* Force MDI */
	dev1e_145_tmp = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x0145);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0145, 0x1010);

	/* 1e_96[15]:bypass_tx_offset_cal, Hw bypass, Fw cal */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x96,
		      0x8000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e,
		      0xf808);

	switch (txg_calen_x) {
	case PAIR_A:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(12));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_A;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_A;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_A_B;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_A_MASK;
		break;
	case PAIR_B:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(8));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_B;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_B;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_A_B;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_B_MASK;
		break;
	case PAIR_C:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(4));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_C;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_C;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_C_D;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_C_MASK;
		break;
	case PAIR_D:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(0));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_D;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_D;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_C_D;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_D_MASK;
		break;
	default:
		return -EINVAL;
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND1, reg_dac1,
		      0x8000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, reg_dac2,
		      0x8000);

	phydev_dbg(phydev, "Start TX Offset SW cal.\n");

	first_calib = airoha_cal_cycle(phydev, MDIO_MMD_VEND1, reg, mask,
				       zcal_ctrl << __ffs(mask));

	/* If TX Offset calibration failed:
	 * - increase dB until calibration succeed.
	 * If TX Offset calibration succeeded:
	 * - decrease dB until calibration fail to fine tune it.
	 */
	if (first_calib == 1)
		calibration_polarity = -1;
	else
		calibration_polarity = 1;

	while (zcal_ctrl > -32 && zcal_ctrl < 32) {
		u32 val;

		zcal_ctrl += calibration_polarity;
		if (zcal_ctrl >= 0)
			val = zcal_ctrl;
		else
			/* BIT(5) signal negative number for TX Offset */
			val = BIT(5) | abs(zcal_ctrl);

		ret = airoha_cal_cycle(phydev, MDIO_MMD_VEND1, reg, mask,
				       val << __ffs(mask));
		/* Exit if we either failed or succeeded compared to the
		 * first calibration result. (aka we finished fine tuning or
		 * we succeeded with calibration)
		 */
		if (ret != first_calib)
			break;
	}

	if (ret < 0) {
		phydev_err(phydev, "TX Offset calibration failed.\n");
		return -EINVAL;
	}

	if (zcal_ctrl == -32 ||
	    zcal_ctrl == 32) {
		zcal_ctrl = 0;
		phydev_err(phydev, "TX Offset SW calibration saturation. Defaulting to %x.\n",
			   zcal_ctrl);

		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg, mask,
			       zcal_ctrl << __ffs(mask));
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0x0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_A, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_B, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_C, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_D, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_A, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_B, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_C, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_D, 0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x96, 0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, 0);

	/* Restore BMCR */
	phy_write(phydev, MII_BMCR, bmcr_tmp);

	/* Restore MDI */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0145, dev1e_145_tmp);

	return 0;
}

static int airoha_tx_amp_cal_sw(struct phy_device *phydev, u8 txg_calen_x)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	u16 mask_gbe, mask_tbt, mask_tst, mask_hbt;
	u16 reg, reg_100, reg_dac1, reg_dac2;
	struct phy_device *phydev_p0;
	int calibration_polarity;
	u8 zcal_ctrl = 32;
	int first_calib;
	int ret;

	phydev_p0 = shared->phydev_p0;

	/* BG voltage output */
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0xc000);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x145, 0x1010);

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
		      MTK_PHY_RG_TXVOS_CALEN);

	/* select 1V */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0x10);

	/* enable Tx VLD*/
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xf808);

	/* Force 1G full duplex for calibration */
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);

	switch (txg_calen_x) {
	case PAIR_A:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(12));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_A;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_A;
		reg = MTK_PHY_TXVLD_DA_RG;
		mask_gbe = MTK_PHY_DA_TX_I2MPB_A_GBE_MASK;
		mask_tbt = MTK_PHY_DA_TX_I2MPB_A_TBT_MASK;
		reg_100 = MTK_PHY_TX_I2MPB_TEST_MODE_A2;
		mask_hbt = MTK_PHY_DA_TX_I2MPB_A_HBT_MASK;
		mask_tst = MTK_PHY_DA_TX_I2MPB_A_TST_MASK;
		break;
	case PAIR_B:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(8));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_B;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_B;
		reg = MTK_PHY_TX_I2MPB_TEST_MODE_B1;
		mask_gbe = MTK_PHY_DA_TX_I2MPB_B_GBE_MASK;
		mask_tbt = MTK_PHY_DA_TX_I2MPB_B_TBT_MASK;
		reg_100 = MTK_PHY_TX_I2MPB_TEST_MODE_B2;
		mask_hbt = MTK_PHY_DA_TX_I2MPB_B_HBT_MASK;
		mask_tst = MTK_PHY_DA_TX_I2MPB_B_TST_MASK;
		break;
	case PAIR_C:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(4));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_C;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_C;
		reg = MTK_PHY_TX_I2MPB_TEST_MODE_C1;
		mask_gbe = MTK_PHY_DA_TX_I2MPB_C_GBE_MASK;
		mask_tbt = MTK_PHY_DA_TX_I2MPB_C_TBT_MASK;
		reg_100 = MTK_PHY_TX_I2MPB_TEST_MODE_C2;
		mask_hbt = MTK_PHY_DA_TX_I2MPB_C_HBT_MASK;
		mask_tst = MTK_PHY_DA_TX_I2MPB_C_TST_MASK;
		break;
	case PAIR_D:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(0));
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_D;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_D;
		reg = MTK_PHY_TX_I2MPB_TEST_MODE_D1;
		mask_gbe = MTK_PHY_DA_TX_I2MPB_D_GBE_MASK;
		mask_tbt = MTK_PHY_DA_TX_I2MPB_D_TBT_MASK;
		reg_100 = MTK_PHY_TX_I2MPB_TEST_MODE_D2;
		mask_hbt = MTK_PHY_DA_TX_I2MPB_D_HBT_MASK;
		mask_tst = MTK_PHY_DA_TX_I2MPB_D_TST_MASK;
		break;
	default:
		return -EINVAL;
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND1, reg_dac1,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, reg_dac2,
		      0x8000 | 0xf0);

	phydev_dbg(phydev, "Start TX Amp SW cal.\n");
	first_calib = airoha_cal_cycle(phydev, MDIO_MMD_VEND1, reg,
				       mask_gbe, zcal_ctrl << __ffs(mask_gbe));

	/* If TX Amp calibration failed:
	 * - increase dB until calibration succeed.
	 * If TX Amp calibration succeeded:
	 * - decrease dB until calibration fail to fine tune it.
	 */
	if (first_calib == 1)
		calibration_polarity = -1;
	else
		calibration_polarity = 1;

	while (zcal_ctrl > 0 &&
	       zcal_ctrl < FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK)) {
		zcal_ctrl += calibration_polarity;

		ret = airoha_cal_cycle(phydev, MDIO_MMD_VEND1, reg,
				       mask_gbe, zcal_ctrl << __ffs(mask_gbe));
		/* Exit if we either failed or succeeded compared to the
		 * first calibration result. (aka we finished fine tuning or
		 * we succeeded with calibration)
		 */
		if (ret != first_calib)
			break;
	}

	if (ret < 0) {
		phydev_err(phydev, "TX AMP calibration failed.\n");
		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg,
			       mask_gbe | mask_tbt,
			       (zcal_ctrl << __ffs(mask_gbe)) |
			       (zcal_ctrl << __ffs(mask_tbt)));

		return -EINVAL;
	}

	if (zcal_ctrl == 0 ||
	    zcal_ctrl == FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK))
		phydev_warn(phydev, "TX AMP SW calibration saturation.\n");

	phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg,
		       mask_gbe | mask_tbt,
		       (zcal_ctrl << __ffs(mask_gbe)) |
		       (zcal_ctrl << __ffs(mask_tbt)));

	phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg_100,
		       mask_hbt | mask_tst,
		       (zcal_ctrl << __ffs(mask_hbt)) |
		       (zcal_ctrl << __ffs(mask_tst)));

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0x0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_A, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_B, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_C, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_D, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_A, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_B, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_C, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_D, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, 0x145, 0x1000);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, 0);

	/* Restore AN enable */
	phy_write(phydev, MII_BMCR, BMCR_ANENABLE);

	return 0;
}

static int airoha_tx_r50_cal_sw(struct phy_device *phydev, u8 txg_calen_x)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct phy_device *phydev_p0;
	u16 dev1e_145_tmp, bmcr_tmp;
	int calibration_polarity;
	u8 zcal_ctrl = 32;
	int first_calib;
	u16 reg;
	int ret;

	phydev_p0 = shared->phydev_p0;

	/* BG voltage output */
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0xc000);

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	/* select 1V */
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0x10);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	/* select 1V */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0x10);

	/* Force 1G full duplex for calibration */
	bmcr_tmp = phy_read(phydev, MII_BMCR);
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);

	/* Force MDI */
	dev1e_145_tmp = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x0145);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0145, 0x1010);

	/* disable tx slew control */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0100);

	switch (txg_calen_x) {
	case PAIR_A:
		phy_set_bits_mmd(phydev, MDIO_MMD_VEND1,
				 MTK_PHY_RG_ANA_CAL_RG0,
				 MTK_PHY_RG_ZCALEN_A);
		break;
	case PAIR_B:
		phy_set_bits_mmd(phydev, MDIO_MMD_VEND1,
				 MTK_PHY_RG_ANA_CAL_RG1,
				 MTK_PHY_RG_ZCALEN_B);
		break;
	case PAIR_C:
		phy_set_bits_mmd(phydev, MDIO_MMD_VEND1,
				 MTK_PHY_RG_ANA_CAL_RG1,
				 MTK_PHY_RG_ZCALEN_C);
		break;
	case PAIR_D:
		phy_set_bits_mmd(phydev, MDIO_MMD_VEND1,
				 MTK_PHY_RG_ANA_CAL_RG1,
				 MTK_PHY_RG_ZCALEN_D);
		break;
	default:
		return -EINVAL;
	}

	phydev_dbg(phydev, "Start TX r50 SW cal.\n");
	first_calib = airoha_cal_cycle(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
				       MTK_PHY_RG_REXT_ZCAL_CTRL_MASK, zcal_ctrl);

	if (first_calib < 0) {
		phydev_err(phydev, "TX r50 SW calibration failed.\n");
		return -EINVAL;
	}

	/* If TX r50 calibration failed:
	 * - increase dB until calibration succeed.
	 * If TX r50 calibration succeeded:
	 * - decrease dB until calibration fail to fine tune it.
	 */
	if (first_calib == 1)
		calibration_polarity = -1;
	else
		calibration_polarity = 1;

	while (zcal_ctrl > 0 &&
	       zcal_ctrl < FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK)) {
		zcal_ctrl += calibration_polarity;

		ret = airoha_cal_cycle(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG5,
				       MTK_PHY_RG_REXT_ZCAL_CTRL_MASK, zcal_ctrl);
		/* Exit if we either failed or succeeded compared to the
		 * first calibration result. (aka we finished fine tuning or
		 * we succeeded with calibration)
		 */
		if (ret != first_calib)
			break;
	}

	if (ret < 0) {
		phydev_err(phydev, "TX r50 SW calibration failed.\n");
		return -EINVAL;
	}

	if (zcal_ctrl == 0 ||
	    zcal_ctrl == FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK))
		phydev_warn(phydev, "TX r50 SW calibration saturation.\n");

	zcal_ctrl = shared->r50_cal_tbl[zcal_ctrl];

	switch (txg_calen_x) {
	case PAIR_A:
		reg = BIT(7) | FIELD_PREP(MTK_PHY_CR_TX_R50_OFFSET_A_MASK, zcal_ctrl);
		phy_modify_mmd(phydev, MDIO_MMD_VEND1,
			       MTK_PHY_RG_CR_TX_R50_OFFSET_A_B,
			       BIT(7) | MTK_PHY_CR_TX_R50_OFFSET_A_MASK, reg);
		break;
	case PAIR_B:
		reg = BIT(7) | FIELD_PREP(MTK_PHY_CR_TX_R50_OFFSET_B_MASK, zcal_ctrl);
		phy_modify_mmd(phydev, MDIO_MMD_VEND1,
			       MTK_PHY_RG_CR_TX_R50_OFFSET_A_B,
			       BIT(7) | MTK_PHY_CR_TX_R50_OFFSET_B_MASK, reg);
		break;
	case PAIR_C:
		reg = BIT(7) | FIELD_PREP(MTK_PHY_CR_TX_R50_OFFSET_C_MASK, zcal_ctrl);
		phy_modify_mmd(phydev, MDIO_MMD_VEND1,
			       MTK_PHY_RG_CR_TX_R50_OFFSET_C_D,
			       BIT(7) | MTK_PHY_CR_TX_R50_OFFSET_C_MASK, reg);
		break;
	case PAIR_D:
		reg = BIT(7) | FIELD_PREP(MTK_PHY_CR_TX_R50_OFFSET_D_MASK, zcal_ctrl);
		phy_modify_mmd(phydev, MDIO_MMD_VEND1,
			       MTK_PHY_RG_CR_TX_R50_OFFSET_C_D,
			       BIT(7) | MTK_PHY_CR_TX_R50_OFFSET_D_MASK, reg);
		break;
	default:
		return -EINVAL;
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);

	/* Enable tx slew control */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0001);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0000);

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0x0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0x0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0x0);

	/* Restore BMCR */
	phy_write(phydev, MII_BMCR, bmcr_tmp);

	/* Restore MDI */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0145, dev1e_145_tmp);

	return 0;
}

static int airoha_rx_offset_cal_sw(struct phy_device *phydev)
{
	/* Hw bypass, Fw cal */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x96, 0x8000);
	/* tx/rx_cal_criteria_value */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x37, 0x0033);

	/* rx offset cal by Hw setup */
	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND1, 0x39,
			   BIT(14) | BIT(11));

	/* disable rtune calibration */
	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, 0x107,
			   BIT(12));

	/* bypass tx/rx dc offset cancellation process */
	phy_set_bits_mmd(phydev, MDIO_MMD_VEND1, 0x171,
			 BIT(8) | BIT(7));

	/* rx offset calibration start */
	phy_set_bits_mmd(phydev, MDIO_MMD_VEND1, 0x39,
			 BIT(13));

	/* rx offset calibration stop */
	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND1, 0x39,
			   BIT(13));

	mdelay(10);

	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND1, 0x171,
			   BIT(8) | BIT(7));

	if (phydev->drv->phy_id == MTK_GPHY_ID_AN7581)
		phy_write_mmd(phydev, MDIO_MMD_VEND1,
			      MTK_PHY_RG_ANA_CAL_RG6, 0x1);

	return 0;
}

static int airoha_cal_sw(struct phy_device *phydev, enum CAL_ITEM cal_item,
			 u8 start_pair, u8 end_pair)
{
	u8 pair_n;
	int ret;

	for (pair_n = start_pair; pair_n <= end_pair; pair_n++) {
		switch (cal_item) {
		case REXT:
			ret = airoha_rext_cal_sw(phydev);
			break;
		case TX_OFFSET:
			ret = airoha_tx_offset_cal_sw(phydev, pair_n);
			break;
		case TX_AMP:
			ret = airoha_tx_amp_cal_sw(phydev, pair_n);
			break;
		case TX_R50:
			ret = airoha_tx_r50_cal_sw(phydev, pair_n);
			break;
		case RX_OFFSET:
			ret = airoha_rx_offset_cal_sw(phydev);
			break;
		default:
			return -EINVAL;
		}
		if (ret)
			return ret;
	}
	return 0;
}

static int airoha_start_cal(struct phy_device *phydev, enum CAL_ITEM cal_item,
			    enum CAL_MODE cal_mode, u8 start_pair,
			    u8 end_pair, u32 *buf)
{
	int ret;

	switch (cal_mode) {
	case SW_M:
		ret = airoha_cal_sw(phydev, cal_item, start_pair, end_pair);
		break;
	default:
		return -EINVAL;
	}

	if (ret) {
		phydev_err(phydev, "cal %d failed\n", cal_item);
		return -EIO;
	}

	return 0;
}

int airoha_phy_calib(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct phy_device *phydev_p0;
	int ret;

	phydev_p0 = shared->phydev_p0;

	/* PreCalibrate Set */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x5c, 0x6666);

	if (!shared->rext_sw_calib_done) {
		ret = airoha_start_cal(phydev, REXT, SW_M, NO_PAIR, NO_PAIR, NULL);
		if (ret)
			return ret;

		shared->rext_sw_calib_done = true;
	}

	ret = airoha_start_cal(phydev, TX_R50, SW_M, PAIR_A, PAIR_D, NULL);
	if (ret)
		return ret;

	ret = airoha_start_cal(phydev, TX_OFFSET, SW_M, PAIR_A, PAIR_D, NULL);
	if (ret)
		return ret;

	ret = airoha_start_cal(phydev, TX_AMP, SW_M, PAIR_A, PAIR_D, NULL);
	if (ret)
		return ret;

	ret = airoha_start_cal(phydev, RX_OFFSET, SW_M, NO_PAIR, NO_PAIR, NULL);
	if (ret)
		return ret;

	/* Gating, short with other pair */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x15, 0x0);

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND2, 0x100, 0x0);

	return 0;
}

int airoha_phy_auto_select_transformer(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct phy_device *phydev_p0;
	u8 phy_offset;
	u16 bmcr_tmp;
	u16 val;
	int i;

	phydev_p0 = shared->phydev_p0;
	phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;

	/* Force 1G full duplex for calibration */
	bmcr_tmp = phy_read(phydev, MII_BMCR);
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);

	phy_modify_mmd(phydev, MDIO_MMD_VEND2, 0x271, GENMASK(4, 0),
		       FIELD_PREP(GENMASK(4, 0), 0x3));

	phy_modify_mmd(phydev, MDIO_MMD_VEND2, 0x269, GENMASK(15, 12),
		       FIELD_PREP(GENMASK(15, 12), 0x2));

	phy_modify_mmd(phydev, MDIO_MMD_VEND2, 0x26f, GENMASK(14, 12),
		       FIELD_PREP(GENMASK(14, 12), 0x0));

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(12));

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_A,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_A,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_B,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_B,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_C,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_C,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_D,
		      0x8000 | 0xf0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_D,
		      0x8000 | 0xf0);

	/* Force pGA gain */
	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_52B5);
	__mtk_tr_modify(phydev, 0x1, 0xf, 0x10,
			GENMASK(20, 15),
			FIELD_PREP(GENMASK(20, 15), 0x1f));

	__mtk_tr_modify(phydev, 0x1, 0xf, 0x11,
			GENMASK(23, 18) | GENMASK(15, 10) | GENMASK(7, 2),
			FIELD_PREP(GENMASK(23, 18), 0x1f) |
			FIELD_PREP(GENMASK(15, 10), 0x1f) |
			FIELD_PREP(GENMASK(7, 2), 0x1f));
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xc9, 0xffff);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x151, 0x12);

	switch (phy_offset) {
	case 0:
		val = 0x1;
		break;
	case 1:
		val = 0x3;
		break;
	case 2:
		val = 0x5;
		break;
	case 3:
		val = 0x7;
		break;
	default:
		return -EINVAL;
	}
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x15,
		      FIELD_PREP(GENMASK(14, 12), val) | 0x161);
	mdelay(500);

	for (i = 0; i < 3; i++)
		val = phy_read_mmd(phydev, MDIO_MMD_VEND2, 0x1a);

	/* Check the value and report transformer type for later */
	shared->transformer_type[phy_offset] = val < 0xe0;

	/* Switch to Default */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x15, 0x1000);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, 0x0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_A,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_A,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_B,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_B,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_C,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_C,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN0_D,
		      0x0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_DASN_DAC_IN1_D,
		      0x0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xc9, 0xfff);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x151, 0x0);

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_52B5);
	__mtk_tr_modify(phydev, 0x1, 0xf, 0x10,
			GENMASK(20, 15),
			FIELD_PREP(GENMASK(20, 15), 0x4));

	__mtk_tr_modify(phydev, 0x1, 0xf, 0x11,
			GENMASK(23, 18) | GENMASK(15, 10) | GENMASK(7, 2),
			FIELD_PREP(GENMASK(23, 18), 0x0) |
			FIELD_PREP(GENMASK(15, 10), 0x0) |
			FIELD_PREP(GENMASK(7, 2), 0x0));
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	/* Restore BMCR */
	phy_write(phydev, MII_BMCR, bmcr_tmp);

	return 0;
}

int airoha_phy_tx_amp_compensation(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;

	u16 reg_1e_12_read;
	u16 reg_1e_16_read;
	u16 reg_1e_17_read;
	u16 reg_1e_18_read;
	u16 reg_1e_19_read;
	u16 reg_1e_20_read;
	u16 reg_1e_21_read;
	u16 reg_1e_22_read;
	u16 reg_1e_174_read;
	u16 reg_1e_175_read;

	int reg_1e_12_header;
	int reg_1e_16_header;
	int reg_1e_17_header;
	int reg_1e_18_header;
	int reg_1e_19_header;
	int reg_1e_21_header;
	int reg_1e_174_header;
	int reg_1e_175_header;

	int reg_1e_12_end;
	int reg_1e_16_end;
	int reg_1e_17_end;
	int reg_1e_18_end;
	int reg_1e_20_end;
	int reg_1e_22_end;
	int reg_1e_174_end;
	int reg_1e_175_end;

	struct phy_device *phydev_p0;
	u8 phy_offset;

	int txamp_low_limit = -3;
	int txamp_high_limit = 0x42;

	int r50_low_limit = -8;
	int r50_high_limit = 0x88;

	bool overflow;
	int (*tx_amp_table)[CALIB_CONST_TYPE_MAX][4];
	int transformer_type, mdi_resister_type;

	phydev_p0 = shared->phydev_p0;
	phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;

	transformer_type = shared->transformer_type[phy_offset];
	mdi_resister_type = shared->mdi_resister_type;
	tx_amp_table = shared->tx_amp_compensation_tbl[transformer_type][mdi_resister_type];

	reg_1e_12_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x12);
	reg_1e_16_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x16);
	reg_1e_17_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x17);
	reg_1e_18_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x18);
	reg_1e_19_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x19);
	reg_1e_20_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x20);
	reg_1e_21_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x21);
	reg_1e_22_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x22);
	reg_1e_174_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x174);
	reg_1e_175_read = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x175);

	reg_1e_12_header = ((reg_1e_12_read & 0xfc00) / 1024) + *tx_amp_table[TX_AMP_1G_A][phy_offset];
	reg_1e_17_header = ((reg_1e_17_read & 0x3f00) / 256) + *tx_amp_table[TX_AMP_1G_B][phy_offset];
	reg_1e_19_header = ((reg_1e_19_read & 0x3f00) / 256) + *tx_amp_table[TX_AMP_1G_C][phy_offset];
	reg_1e_21_header = ((reg_1e_21_read & 0x3f00) / 256) + *tx_amp_table[TX_AMP_1G_D][phy_offset];

	overflow = false;
	if (reg_1e_12_header < txamp_low_limit ||
	    reg_1e_12_header > txamp_high_limit)
		overflow = true;
	if (reg_1e_17_header < txamp_low_limit ||
	    reg_1e_17_header > txamp_high_limit)
		overflow = true;
	if (reg_1e_19_header < txamp_low_limit ||
	    reg_1e_19_header > txamp_high_limit)
		overflow = true;
	if (reg_1e_21_header < txamp_low_limit ||
	    reg_1e_21_header > txamp_high_limit)
		overflow = true;

	if (overflow) {
		reg_1e_12_header = 0x20;
		reg_1e_17_header = 0x20;
		reg_1e_19_header = 0x20;
		reg_1e_21_header = 0x20;
	} else {
		if (reg_1e_12_header < 0)
			reg_1e_12_header = 0;
		else if (reg_1e_12_header > 0x3f)
			reg_1e_12_header = 0x3f;
		if (reg_1e_17_header < 0)
			reg_1e_17_header = 0;
		else if (reg_1e_17_header > 0x3f)
			reg_1e_17_header = 0x3f;
		if (reg_1e_19_header < 0)
			reg_1e_19_header = 0;
		else if (reg_1e_19_header > 0x3f)
			reg_1e_19_header = 0x3f;
		if (reg_1e_21_header < 0)
			reg_1e_21_header = 0;
		else if (reg_1e_21_header > 0x3f)
			reg_1e_21_header = 0x3f;
	}

	reg_1e_16_end = (reg_1e_16_read & 0x003f) + *tx_amp_table[TX_AMP_TEST_A][phy_offset];
	reg_1e_18_end = (reg_1e_18_read & 0x003f) + *tx_amp_table[TX_AMP_TEST_B][phy_offset];
	reg_1e_20_end = (reg_1e_20_read & 0x003f) + *tx_amp_table[TX_AMP_TEST_C][phy_offset];
	reg_1e_22_end = (reg_1e_22_read & 0x003f) + *tx_amp_table[TX_AMP_TEST_D][phy_offset];

	overflow = false;
	if (reg_1e_16_end < txamp_low_limit ||
	    reg_1e_16_end > txamp_high_limit)
		overflow = true;
	if (reg_1e_18_end < txamp_low_limit ||
	    reg_1e_18_end > txamp_high_limit)
		overflow = true;
	if (reg_1e_20_end < txamp_low_limit ||
	    reg_1e_20_end > txamp_high_limit)
		overflow = true;
	if (reg_1e_22_end < txamp_low_limit ||
	    reg_1e_22_end > txamp_high_limit)
		overflow = true;

	if (overflow) {
		reg_1e_16_end = 0x20;
		reg_1e_18_end = 0x20;
		reg_1e_20_end = 0x20;
		reg_1e_22_end = 0x20;
	} else {
		if (reg_1e_16_end < 0)
			reg_1e_16_end = 0;
		else if (reg_1e_16_end > 0x3f)
			reg_1e_16_end = 0x3f;
		if (reg_1e_18_end < 0)
			reg_1e_18_end = 0;
		else if (reg_1e_18_end > 0x3f)
			reg_1e_18_end = 0x3f;
		if (reg_1e_20_end < 0)
			reg_1e_20_end = 0;
		else if (reg_1e_20_end > 0x3f)
			reg_1e_20_end = 0x3f;
		if (reg_1e_22_end < 0)
			reg_1e_22_end = 0;
		else if (reg_1e_22_end > 0x3f)
			reg_1e_22_end = 0x3f;
	}

	reg_1e_16_header = ((reg_1e_16_read & 0xfc00) / 1024) + *tx_amp_table[TX_AMP_100M_A][phy_offset];
	reg_1e_18_header = ((reg_1e_18_read & 0x3f00) / 256) + *tx_amp_table[TX_AMP_100M_B][phy_offset];

	overflow = false;
	if (reg_1e_16_header < txamp_low_limit ||
	    reg_1e_16_header > txamp_high_limit)
		overflow = true;
	if (reg_1e_18_header < txamp_low_limit ||
	    reg_1e_18_header > txamp_high_limit)
		overflow = true;

	if (overflow) {
		reg_1e_16_header = 0x20;
		reg_1e_18_header = 0x20;
	} else {
		if (reg_1e_16_header < 0)
			reg_1e_16_header = 0;
		else if (reg_1e_16_header > 0x3f)
			reg_1e_16_header = 0x3f;
		if (reg_1e_18_header < 0)
			reg_1e_18_header = 0;
		else if (reg_1e_18_header > 0x3f)
			reg_1e_18_header = 0x3f;
	}

	reg_1e_12_end = (reg_1e_12_read & 0x003f) + *tx_amp_table[TX_AMP_10M_A][phy_offset];
	reg_1e_17_end = (reg_1e_17_read & 0x003f) + *tx_amp_table[TX_AMP_10M_B][phy_offset];

	overflow = false;
	if (reg_1e_12_end < txamp_low_limit ||
	    reg_1e_12_end > txamp_high_limit)
		overflow = true;
	if (reg_1e_17_end < txamp_low_limit ||
	    reg_1e_17_end > txamp_high_limit)
		overflow = true;

	if (overflow) {
		reg_1e_12_end = 0x20;
		reg_1e_17_end = 0x20;
	} else {
		if (reg_1e_12_end < 0)
			reg_1e_12_end = 0;
		else if (reg_1e_12_end > 0x3f)
			reg_1e_12_end = 0x3f;
		if (reg_1e_17_end < 0)
			reg_1e_17_end = 0;
		else if (reg_1e_17_end > 0x3f)
			reg_1e_17_end = 0x3f;
	}

	reg_1e_174_header = ((reg_1e_174_read & 0x7f00) / 256) + *tx_amp_table[R50_A][phy_offset];
	reg_1e_174_end = (reg_1e_174_read & 0x007f) + *tx_amp_table[R50_B][phy_offset];
	reg_1e_175_header = ((reg_1e_175_read & 0x7f00) / 256) + *tx_amp_table[R50_C][phy_offset];
	reg_1e_175_end = (reg_1e_175_read & 0x007f) + *tx_amp_table[R50_D][phy_offset];

	overflow = false;
	if (reg_1e_174_header < r50_low_limit ||
	    reg_1e_174_header > r50_high_limit)
		overflow = true;
	if (reg_1e_174_end < r50_low_limit ||
	    reg_1e_174_end > r50_high_limit)
		overflow = true;
	if (reg_1e_175_header < r50_low_limit ||
	    reg_1e_175_header > r50_high_limit)
		overflow = true;
	if (reg_1e_175_end < r50_low_limit ||
	    reg_1e_175_end > r50_high_limit)
		overflow = true;

	if (overflow) {
		reg_1e_174_header = 0x34;
		reg_1e_174_end = 0x34;
		reg_1e_175_header = 0x34;
		reg_1e_175_end = 0x34;
	} else {
		if (reg_1e_174_header < 0)
			reg_1e_174_header = 0;
		else if (reg_1e_174_header > 0x7f)
			reg_1e_174_header = 0x7f;
		if (reg_1e_174_end < 0)
			reg_1e_174_end = 0;
		else if (reg_1e_174_end > 0x7f)
			reg_1e_174_end = 0x7f;
		if (reg_1e_175_header < 0)
			reg_1e_175_header = 0;
		else if (reg_1e_175_header > 0x7f)
			reg_1e_175_header = 0x7f;
		if (reg_1e_175_end < 0)
			reg_1e_175_end = 0;
		else if (reg_1e_175_end > 0x7f)
			reg_1e_175_end = 0x7f;
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x12,
		      (reg_1e_12_header * 1024) + reg_1e_12_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x16,
		      (reg_1e_16_header * 1024) + reg_1e_16_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x17,
		      (reg_1e_17_header * 256) + reg_1e_17_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x18,
		      (reg_1e_18_header * 256) + reg_1e_18_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x19,
		      (reg_1e_19_read & 0x00ff) + (reg_1e_19_header * 256));
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x20,
		      (reg_1e_20_read & 0xff00) + reg_1e_20_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x21,
		      (reg_1e_21_read & 0x00ff) + (reg_1e_21_header * 256));
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x22,
		      (reg_1e_22_read & 0xff00) + reg_1e_22_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x174,
		      0x8080 + (reg_1e_174_header * 256) + reg_1e_174_end);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x175,
		      0x8080 + (reg_1e_175_header * 256) + reg_1e_175_end);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x176, 0x4400);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x177, 0x0044);

	return 0;
}