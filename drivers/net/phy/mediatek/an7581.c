// SPDX-License-Identifier: GPL-2.0

#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include "mtk-ge-soc.h"

#define ARIOHA_SCU_PDIDR	0x5c

/*
 * 2 chip revision
 * TXMR or discrete
 * 2 MDI type
 * TX AMP test/TX AMP 1G/TX AMP 100M/TX AMP 10M/R50
 * 4 PHY
 */
static int an7581_tx_amp_compensation_tbl[2][TRANSFORMER_TYPE_MAX][MDI_TYPE_MAX][CALIB_CONST_TYPE_MAX][4] = {
	{ /* IC version 1 */
		[TXMR] = {
			{ },
			{
				[TX_AMP_TEST_A] = { -12, -11, -9, -6 },
				[TX_AMP_TEST_B] = { -9, -13, -9, -5 },
				[TX_AMP_TEST_C] = { -5, -4, -3, 1 },
				[TX_AMP_TEST_D] = { -9, -8, -7, -5 },
				[TX_AMP_1G_A] = { -12, -11, -9, -6 },
				[TX_AMP_1G_B] = { -9, -13, -9, -5 },
				[TX_AMP_1G_C] = { -5, -4, -3, 1 },
				[TX_AMP_1G_D] = { -9, -8, -7, -5 },
				[TX_AMP_100M_A] = { -2, -2, 0, 2 },
				[TX_AMP_100M_B] = { -2, -2, 1, 1 },
				[TX_AMP_10M_A] = { 0, 0, 0, 0 },
				[TX_AMP_10M_B] = { 0, 0, 0, 0 },
				[R50_A] = { 15, 15, 15, 15 },
				[R50_B] = { 15, 15, 18, 15 },
				[R50_C] = { -8, -8, -8, -8 },
				[R50_D] = { -8, -8, -8, -8 },
			},
		},
		[DISCRETE] = {
			{ },
			{
				[TX_AMP_TEST_A] = { -9, -7, -7, -5 },
				[TX_AMP_TEST_B] = { -9, -9, -7, -4 },
				[TX_AMP_TEST_C] = { -9, -7, -9, -6 },
				[TX_AMP_TEST_D] = { -11, -9, -10, -8 },
				[TX_AMP_1G_A] = { -9, -7, -7, -5 },
				[TX_AMP_1G_B] = { -9, -8, -7, -4 },
				[TX_AMP_1G_C] = { -9, -7, -9, -6 },
				[TX_AMP_1G_D] = { -11, -8, -10, -8 },
				[TX_AMP_100M_A] = { 4, 3, 4, 4 },
				[TX_AMP_100M_B] = { 4, 4, 5, 4 },
				[TX_AMP_10M_A] = { 0, 0, 0, 0 },
				[TX_AMP_10M_B] = { 0, 0, 0, 0 },
				[R50_A] = { 0, 0, 0, 0 },
				[R50_B] = { 0, 0, 0, 0 },
				[R50_C] = { 0, 0, 0, 0 },
				[R50_D] = { 0, 0, 0, 0 },
			},
		},
	},
	{ /* IC version 2 */
		[TXMR] = {
			{ },
			{
				[TX_AMP_TEST_A] = { -10, -12, -12, -11 },
				[TX_AMP_TEST_B] = { -10, -11, -11, -10 },
				[TX_AMP_TEST_C] = { -5, -7, -9, -3 },
				[TX_AMP_TEST_D] = { -8, -10, -7, -8 },
				[TX_AMP_1G_A] = { -12, -6, -6, 4 },
				[TX_AMP_1G_B] = { -10, -5, -5, 1 },
				[TX_AMP_1G_C] = { -5, -3, -4, 11 },
				[TX_AMP_1G_D] = { -8, -3, -3, 4 },
				[TX_AMP_100M_A] = { 2, 1, 0, 3 },
				[TX_AMP_100M_B] = { 0, 0, 0, 3 },
				[TX_AMP_10M_A] = { 0, 0, 0, 0 },
				[TX_AMP_10M_B] = { 0, 0, 0, 0 },
				[R50_A] = { 15, 15, 15, 15 },
				[R50_B] = { 15, 15, 18, 15 },
				[R50_C] = { 4, 4, 4, 4 },
				[R50_D] = { 4, 4, 4, 4 },
			},
		},
		[DISCRETE] = {
			{ },
			{
				[TX_AMP_TEST_A] = { -11, 11, -12, -7 },
				[TX_AMP_TEST_B] = { -9, -10, -9, -8 },
				[TX_AMP_TEST_C] = { -6, -3, -7, -4 },
				[TX_AMP_TEST_D] = { -7, -8, -10, -7 },
				[TX_AMP_1G_A] = { -11, -11, -12, -7 },
				[TX_AMP_1G_B] = { -9, -10, -9, -8 },
				[TX_AMP_1G_C] = { -6, -3, -7, -4 },
				[TX_AMP_1G_D] = { -7, -8, -10, -7 },
				[TX_AMP_100M_A] = { 2, 2, 1, 2 },
				[TX_AMP_100M_B] = { 3, 3, 3, 3 },
				[TX_AMP_10M_A] = { 0, 0, 0, 0 },
				[TX_AMP_10M_B] = { 0, 0, 0, 0 },
				[R50_A] = { 12, 12, 12, 12 },
				[R50_B] = { 7, 7, 7, 7 },
				[R50_C] = { 2, 2, 0, 0 },
				[R50_D] = { 0, 0, 2, 2 },
			},
		},
	},
};

static u8 an7581_zcal_to_r45ohm[64] = {
	127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
	127, 127, 127, 127, 127, 127, 127, 127, 123, 119, 115, 112, 108, 104, 100,  96,
	 94,  92,  88,  85,  82,  80,  76,  74,  72,  68,  66,  64,  62,  60,  56,  55,
	 52,  50,  48,  46,  44,  42,  40,  39,  36,  35,  32,  32,  30,  28,  27,  25
};

int an7581_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phydev->shared->priv;
	struct device_node *np = phydev->mdio.dev.of_node;
	struct phy_device *phydev_p0;
	struct regmap *scu_map;
	u32 soc_pdidr;
	u8 phy_offset;
	int ret;

	phydev_p0 = shared->phydev_p0;
	phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;

	scu_map = syscon_regmap_lookup_by_phandle_optional(np, "airoha,scu");
	if (IS_ERR(scu_map))
		return PTR_ERR(scu_map);

	/* Read SoC PDIDR if available or default to 1 */
	if (scu_map) {
		ret = regmap_read(scu_map, ARIOHA_SCU_PDIDR, &soc_pdidr);
		if (ret)
			return ret;
	} else {
		soc_pdidr = 1;
	}

	shared->mdi_resister_type = MDI_5R;
	if (soc_pdidr == 1)
		shared->tx_amp_compensation_tbl = &an7581_tx_amp_compensation_tbl[0];
	else
		shared->tx_amp_compensation_tbl = &an7581_tx_amp_compensation_tbl[1];
	shared->r50_cal_tbl = an7581_zcal_to_r45ohm;

	ret = airoha_phy_calib(phydev);
	if (ret)
		return ret;

	ret = airoha_phy_auto_select_transformer(phydev);
	if (ret)
		return ret;

	ret = airoha_phy_tx_amp_compensation(phydev);
	if (ret)
		return ret;

	/* RX setting for 5R_TXMR before AN setting */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe6, 0x1111);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe7, 0x5555);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe9, 0x0001);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xfe, 0x0000);

	/* Long Loop Reach setting */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x11, 0xff00);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x202, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x203, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x204, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x205, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x206, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x207, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x208, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x209, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x20a, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x20b, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x20e, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x20f, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x210, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x211, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x212, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x213, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x214, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x215, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x216, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x217, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x21a, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x21b, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x21c, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x21d, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x21e, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x21f, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x220, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x221, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x222, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x223, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x226, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x227, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x228, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x229, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x22a, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x22b, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x22c, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x22d, 0x0025);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x22e, 0x2310);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x22f, 0x0025);

	/* RX Setting */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3c, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3d, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);

	/* EEE setting */
	phy_write_mmd(phydev, MDIO_MMD_AN, 0x3c, 0x0000);

	/* RX */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x41, 0x3333);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x44, 0x00c0);

	/* 10M settings */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x1a3, 0x00d2);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x1a4, 0x010e);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x27b, 0x1177);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x27c, 0x0808);

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_1);
	__phy_write(phydev, 0x14, 0x3a18);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	phy_write(phydev, 0x9, 0x1e00);

	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x269, 0x2114);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x26a, 0x1113);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x26f, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x271, 0x2c63);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x272, 0x0c2b);

	/* EEE set because power down can't set tokenring */
	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_52B5);
	__mtk_tr_write(phydev, 0x0, 0x7, 0x15, 0x0055a0);
	__mtk_tr_write(phydev, 0x0, 0x7, 0x17, 0x07ff3f);
	__mtk_tr_write(phydev, 0x1, 0xf, 0x0, 0x00001e);
	__mtk_tr_write(phydev, 0x1, 0xf, 0x1, 0x6fb90a);
	__mtk_tr_write(phydev, 0x1, 0xf, 0x18, 0x0e2f00);
	__mtk_tr_write(phydev, 0x1, 0xd, 0x26, 0x444444);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x3, 0x000004);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x6, 0x2ebaef);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x8, 0x00000b);
	__mtk_tr_write(phydev, 0x2, 0xd, 0xc, 0x00504d);
	__mtk_tr_write(phydev, 0x2, 0xd, 0xd, 0x02314f);
	__mtk_tr_write(phydev, 0x2, 0xd, 0xf, 0x003028);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x10, 0x00000a);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x11, 0x040001);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x14, 0x00024a);
	__mtk_tr_write(phydev, 0x2, 0xd, 0x1c, 0x003210);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x120, 0x8014);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x122, 0xffff);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x144, 0x0200);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x14a, 0xee20);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x19b, 0x0111);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x234, 0x1181);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x238, 0x0120);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x239, 0x0117);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x2d1, 0x0733);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x323, 0x0011);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x324, 0x013f);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x326, 0x0037);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x268, 0x07f4);

	if (shared->transformer_type[phy_offset] == TXMR) {
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00, 0x0187);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x01, 0x01cc);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x02, 0x01c2);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03, 0x0109);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x04, 0x020b);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0202);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x06, 0x0387);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x07, 0x03c5);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x08, 0x03c2);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x09, 0x0309);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0a, 0x000e);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0b, 0x0002);

		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x23, 0x0880);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x24, 0x0880);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x25, 0x0880);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x26, 0x0880);
	} else {
		if (soc_pdidr == 2) {
			phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x269, 0x1114);
			phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x26a, 0x1113);
			phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x26f, 0x2000);
			phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x271, 0x4c88);
			phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x272, 0x0c2b);

			/* 100M sharp */
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00, 0x0190);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x01, 0x01c8);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x02, 0x01c0);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03, 0x0030);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x04, 0x0208);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0000);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x06, 0x0318);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x07, 0x03c8);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x08, 0x03c0);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x09, 0x0230);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0a, 0x0008);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0b, 0x0200);

			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x23, 0x0885);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x24, 0x0885);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x25, 0x0885);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x26, 0x0885);
		} else {
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00, 0x0190);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x01, 0x01cf);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x02, 0x01c0);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03, 0x0030);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x04, 0x020f);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0000);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x06, 0x0318);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x07, 0x03cf);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x08, 0x03c0);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x09, 0x0230);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0a, 0x000f);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0b, 0x0200);

			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x23, 0x0003);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x24, 0x0003);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x25, 0x0003);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x26, 0x0003);
		}
	}

	return 0;
}