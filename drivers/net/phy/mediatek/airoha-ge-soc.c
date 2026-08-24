// SPDX-License-Identifier: GPL-2.0+
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/pinctrl/consumer.h>
#include <linux/phy.h>

#include "../phylib.h"
#include "mtk.h"
#include "mtk-ge-soc.h"

#define AIROHA_DEFAULT_PORT0_ADDR		0x9
#define AIROHA_PHY_MAX_LEDS			2

enum airoha_mdi_resister_type {
	MDI_0R,
	MDI_5R,

	MDI_TYPE_MAX,
};

enum airoha_transformer_type {
	TXMR,
	DISCRETE,

	TRANSFORMER_TYPE_MAX,
};

enum airoha_calib_const_type {
	TX_AMP_TEST_A,
	TX_AMP_TEST_B,
	TX_AMP_TEST_C,
	TX_AMP_TEST_D,
	TX_AMP_1G_A,
	TX_AMP_1G_B,
	TX_AMP_1G_C,
	TX_AMP_1G_D,
	TX_AMP_100M_A,
	TX_AMP_100M_B,
	TX_AMP_10M_A,
	TX_AMP_10M_B,
	R50_A,
	R50_B,
	R50_C,
	R50_D,

	CALIB_CONST_TYPE_MAX,
};

enum airoha_9491_variant {
	AIROHA_9491_EN7528,
	AIROHA_9491_EN7580,
};

struct airoha_socphy_shared {
	struct phy_device *phydev_p0;
	enum airoha_9491_variant variant;
	enum airoha_transformer_type transformer_type[4];
	enum airoha_mdi_resister_type mdi_resister_type;
	bool rext_sw_calib_done;
	int (*tx_amp_compensation_tbl)[TRANSFORMER_TYPE_MAX][MDI_TYPE_MAX][CALIB_CONST_TYPE_MAX][4];
	const u8 *r50_cal_tbl;
	u8 r50_cal_offset;
};

struct airoha_mmd_reg {
	int devad;
	u16 reg;
	u16 val;
};

struct airoha_tr_reg {
	u8 ch_addr;
	u8 node_addr;
	u8 data_addr;
	u32 val;
};


static int en7523_tx_amp_compensation_tbl[TRANSFORMER_TYPE_MAX][MDI_TYPE_MAX][CALIB_CONST_TYPE_MAX][4];

static u8 en7523_zcal_to_r45ohm[64] = {
	127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
	127, 127, 127, 127, 127, 123, 118, 114, 110, 106, 102,  98,  96,  92,  88,  85,
	 82,  80,  76,  72,  70,  67,  64,  62,  60,  56,  54,  52,  49,  48,  45,  43,
	 40,  39,  36,  34,  32,  32,  30,  28,  25,  24,  22,  20,  18,  16,  16,  14,
};

static const struct airoha_mmd_reg en7523_ge_init_regs[] = {
	/* Global tuning from the EN7523 SDK tcphy tables. */
	{ MDIO_MMD_VEND2, 0x273, 0x1000 },
	{ MDIO_MMD_VEND2, 0x272, 0x0c6b },
	{ MDIO_MMD_VEND2, 0x268, 0x07f4 },
	{ MDIO_MMD_VEND2, 0x269, 0x1114 },
	{ MDIO_MMD_VEND2, 0x271, 0x2ca5 },
	{ MDIO_MMD_VEND2, 0x044, 0x00a0 },
	{ MDIO_MMD_VEND2, 0x27c, 0x0808 },
	{ MDIO_MMD_VEND2, 0x27b, 0x1177 },
	{ MDIO_MMD_VEND2, 0x417, 0x7775 },

	/* Local EN7523 5R/TXMR defaults. */
	{ MDIO_MMD_VEND1, 0x000, 0x0187 },
	{ MDIO_MMD_VEND1, 0x001, 0x01c0 },
	{ MDIO_MMD_VEND1, 0x002, 0x01c2 },
	{ MDIO_MMD_VEND1, 0x003, 0x0109 },
	{ MDIO_MMD_VEND1, 0x004, 0x0200 },
	{ MDIO_MMD_VEND1, 0x005, 0x0202 },
	{ MDIO_MMD_VEND1, 0x006, 0x0387 },
	{ MDIO_MMD_VEND1, 0x007, 0x03c0 },
	{ MDIO_MMD_VEND1, 0x008, 0x03c2 },
	{ MDIO_MMD_VEND1, 0x009, 0x0309 },
	{ MDIO_MMD_VEND1, 0x00a, 0x0000 },
	{ MDIO_MMD_VEND1, 0x00b, 0x0002 },
	{ MDIO_MMD_VEND1, 0x011, 0xff00 },
	{ MDIO_MMD_VEND1, 0x013, 0x0000 },
	{ MDIO_MMD_VEND1, 0x014, 0x0000 },
	{ MDIO_MMD_VEND1, 0x044, 0x0000 },
	{ MDIO_MMD_VEND1, 0x176, 0x5500 },
	{ MDIO_MMD_VEND1, 0x177, 0x0055 },
	{ MDIO_MMD_VEND1, 0x041, 0x3333 },
	{ MDIO_MMD_VEND1, 0x040, 0x0000 },
	{ MDIO_MMD_VEND1, 0x201, 0x4000 },
	{ MDIO_MMD_VEND1, 0x03d, 0x0000 },
	{ MDIO_MMD_VEND1, 0x198, 0x0001 },
	{ MDIO_MMD_VEND1, 0x03e, 0xc000 },
	{ MDIO_MMD_VEND1, 0x23c, 0x0a20 },
	{ MDIO_MMD_VEND1, 0x1a3, 0x00d2 },
	{ MDIO_MMD_VEND1, 0x1a4, 0x010e },
	{ MDIO_MMD_VEND1, 0x189, 0x0110 },
	{ MDIO_MMD_VEND1, 0x122, 0xffff },
	{ MDIO_MMD_VEND1, 0x123, 0xffff },
	{ MDIO_MMD_VEND1, 0x234, 0x1180 },
	{ MDIO_MMD_VEND1, 0x238, 0x0120 },
	{ MDIO_MMD_VEND1, 0x120, 0x8014 },
	{ MDIO_MMD_VEND1, 0x239, 0x0117 },
	{ MDIO_MMD_VEND1, 0x14a, 0xee20 },
	{ MDIO_MMD_VEND1, 0x19b, 0x0111 },
	{ MDIO_MMD_VEND1, 0x147, 0x0000 },
	{ MDIO_MMD_VEND1, 0x2d1, 0x0733 },
	{ MDIO_MMD_VEND1, 0x236, 0x0020 },
	{ MDIO_MMD_VEND1, 0x144, 0x0200 },
	{ MDIO_MMD_VEND1, 0x323, 0x0011 },
	{ MDIO_MMD_VEND1, 0x324, 0x013f },
	{ MDIO_MMD_VEND1, 0x326, 0x0037 },
	{ MDIO_MMD_VEND1, 0x190, 0x0110 },
	{ MDIO_MMD_VEND1, 0x191, 0x4444 },
	{ MDIO_MMD_VEND1, 0x0a6, 0x0350 },
	{ MDIO_MMD_AN,    0x03c, 0x0006 },
};

static const struct airoha_tr_reg en7523_ge_tr_regs[] = {
	{ 0x1, 0xd, 0x26, 0x444444 },
	{ 0x1, 0xf, 0x00, 0x00001e },
	{ 0x1, 0xf, 0x01, 0x6fb90a },
	{ 0x1, 0xf, 0x17, 0x060671 },
	{ 0x1, 0xf, 0x18, 0x0e2f00 },
	{ 0x0, 0x7, 0x15, 0x0055a0 },
	{ 0x0, 0x7, 0x17, 0x07ff3f },
	{ 0x2, 0xd, 0x06, 0x2ebaef },
	{ 0x2, 0xd, 0x08, 0x00000b },
	{ 0x2, 0xd, 0x10, 0x005010 },
	{ 0x2, 0xd, 0x11, 0x040001 },
	{ 0x2, 0xd, 0x03, 0x000004 },
	{ 0x2, 0xd, 0x13, 0x018670 },
	{ 0x2, 0xd, 0x1b, 0x000072 },
	{ 0x2, 0xd, 0x1c, 0x003210 },
	{ 0x2, 0xd, 0x14, 0x00024a },
	{ 0x2, 0xd, 0x0d, 0x02314f },
	{ 0x2, 0xd, 0x0c, 0x00504d },
	{ 0x2, 0xd, 0x0f, 0x003028 },
	{ 0x1, 0xf, 0x03, 0x082422 },
};

/* EN7528 integrated switch GPHY initialization from tcetherphy_7528.c. */
static const u8 en7528_zcal_to_r45ohm[64] = {
	127, 127, 127, 127, 127, 127, 126, 123, 120, 117, 114, 112, 110, 107, 105, 103,
	101,  99,  97,  79,  77,  75,  74,  72,  70,  69,  67,  66,  65,  47,  46,  45,
	 43,  42,  41,  40,  39,  38,  37,  36,  34,  34,  33,  32,  15,  14,  13,  12,
	 11,  10,  10,   9,   8,   7,   7,   6,   5,   4,   4,   3,   2,   2,   1,   1,
};

static const struct airoha_mmd_reg en7528_ge_init_regs[] = {
	/* Global VEND2 tuning. */
	{ MDIO_MMD_VEND2, 0x273, 0x1000 },
	{ MDIO_MMD_VEND2, 0x272, 0x7fff },
	{ MDIO_MMD_VEND2, 0x268, 0x0728 },
	{ MDIO_MMD_VEND2, 0x269, 0x111f },
	{ MDIO_MMD_VEND2, 0x271, 0x7ca8 },
	{ MDIO_MMD_VEND2, 0x272, 0x14ff },
	{ MDIO_MMD_VEND2, 0x044, 0x0030 },
	{ MDIO_MMD_VEND2, 0x27c, 0x0808 },
	{ MDIO_MMD_VEND2, 0x27b, 0x0177 },
	{ MDIO_MMD_VEND2, 0x417, 0x7775 },
	{ MDIO_MMD_VEND2, 0x024, 0xc007 },
	{ MDIO_MMD_VEND2, 0x025, 0x003f },
	{ MDIO_MMD_VEND2, 0x026, 0xc007 },
	{ MDIO_MMD_VEND2, 0x027, 0x003f },
	{ MDIO_MMD_VEND2, 0x021, 0x800a },

	/* Per-PHY VEND1/AN tuning. */
	{ MDIO_MMD_VEND1, 0x000, 0x01b0 },
	{ MDIO_MMD_VEND1, 0x001, 0x01be },
	{ MDIO_MMD_VEND1, 0x002, 0x01be },
	{ MDIO_MMD_VEND1, 0x003, 0x0090 },
	{ MDIO_MMD_VEND1, 0x004, 0x0205 },
	{ MDIO_MMD_VEND1, 0x005, 0x0204 },
	{ MDIO_MMD_VEND1, 0x006, 0x03a7 },
	{ MDIO_MMD_VEND1, 0x007, 0x03c7 },
	{ MDIO_MMD_VEND1, 0x008, 0x03bc },
	{ MDIO_MMD_VEND1, 0x009, 0x0290 },
	{ MDIO_MMD_VEND1, 0x00a, 0x0005 },
	{ MDIO_MMD_VEND1, 0x00b, 0x0006 },
	{ MDIO_MMD_VEND1, 0x013, 0x0000 },
	{ MDIO_MMD_VEND1, 0x014, 0x0000 },
	{ MDIO_MMD_VEND1, 0x03e, 0x0000 },
	{ MDIO_MMD_VEND1, 0x044, 0x0060 },
	{ MDIO_MMD_VEND1, 0x176, 0x5500 },
	{ MDIO_MMD_VEND1, 0x177, 0x0000 },
	{ MDIO_MMD_VEND1, 0x041, 0x3333 },
	{ MDIO_MMD_VEND1, 0x040, 0x0000 },
	{ MDIO_MMD_VEND1, 0x201, 0x4000 },
	{ MDIO_MMD_VEND1, 0x190, 0x0110 },
	{ MDIO_MMD_VEND1, 0x0a6, 0x0350 },
	{ MDIO_MMD_VEND1, 0x0b6, 0x7777 },
	{ MDIO_MMD_AN,    0x03c, 0x0000 },
};

static const struct airoha_tr_reg en7528_ge_tr_regs[] = {
	{ 0x1, 0xf, 0x00, 0x00002b },
	{ 0x1, 0xf, 0x17, 0x060671 },
	{ 0x1, 0xf, 0x18, 0x0e2e00 },
	{ 0x1, 0xf, 0x12, 0x3c4d2a },
	{ 0x1, 0xf, 0x03, 0x082422 },
};

/* EN7580 integrated GPHY initialization from tcetherphy_7580.c. */
static const u8 en7580_zcal_to_r44ohm[64] = {
	127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
	127, 127, 124, 121, 119, 116, 114, 104, 102,  99,  97,  88,  85,  83,  81,  72,
	 70,  68,  66,  64,  55,  53,  52,  50,  49,  48,  38,  37,  36,  34,  33,  32,
	 23,  22,  20,  19,  18,  17,  16,   7,   6,   5,   4,   3,   2,   1,   0,   0,
};

static const struct airoha_mmd_reg en7580_ge_init_regs[] = {
	{ MDIO_MMD_VEND2, 0x273, 0x1000 },
	{ MDIO_MMD_VEND2, 0x272, 0x7cff },
	{ MDIO_MMD_VEND2, 0x268, 0x0728 },
	{ MDIO_MMD_VEND2, 0x269, 0x144f },
	{ MDIO_MMD_VEND2, 0x271, 0x7e0e },
	{ MDIO_MMD_VEND2, 0x044, 0x0030 },
	{ MDIO_MMD_VEND2, 0x27c, 0x0808 },
	{ MDIO_MMD_VEND2, 0x27b, 0x1177 },
	{ MDIO_MMD_VEND2, 0x417, 0x7775 },
	{ MDIO_MMD_VEND2, 0x024, 0xc007 },
	{ MDIO_MMD_VEND2, 0x025, 0x003f },
	{ MDIO_MMD_VEND2, 0x026, 0xc007 },
	{ MDIO_MMD_VEND2, 0x027, 0x003f },
	{ MDIO_MMD_VEND2, 0x021, 0x800a },

	{ MDIO_MMD_VEND1, 0x013, 0x0000 },
	{ MDIO_MMD_VEND1, 0x014, 0x0000 },
	{ MDIO_MMD_VEND1, 0x044, 0x0060 },
	{ MDIO_MMD_VEND1, 0x176, 0x5500 },
	{ MDIO_MMD_VEND1, 0x177, 0x0055 },
	{ MDIO_MMD_VEND1, 0x041, 0x3333 },
	{ MDIO_MMD_VEND1, 0x040, 0x0000 },
	{ MDIO_MMD_VEND1, 0x201, 0x4000 },
	{ MDIO_MMD_VEND1, 0x03d, 0x0c00 },
	{ MDIO_MMD_VEND1, 0x198, 0x0001 },
	{ MDIO_MMD_VEND1, 0x03e, 0xc000 },
	{ MDIO_MMD_VEND1, 0x23c, 0x0a20 },
	{ MDIO_MMD_VEND1, 0x1a3, 0x00d2 },
	{ MDIO_MMD_VEND1, 0x1a4, 0x010e },
	{ MDIO_MMD_VEND1, 0x190, 0x0110 },
	{ MDIO_MMD_VEND1, 0x191, 0x4444 },
	{ MDIO_MMD_VEND1, 0x0a6, 0x0350 },
	{ MDIO_MMD_AN,    0x03c, 0x0000 },
};

static const struct airoha_tr_reg en7580_ge_tr_regs[] = {
	{ 0x1, 0xd, 0x26, 0x444444 },
	{ 0x1, 0xf, 0x00, 0x00002b },
	{ 0x1, 0xf, 0x17, 0x060671 },
	{ 0x1, 0xf, 0x18, 0x0e2e00 },
	{ 0x1, 0xf, 0x03, 0x082422 },
};

struct en7523_rx_setting {
	u16 e6;
	u16 e7;
	u16 fe;
};

static const struct en7523_rx_setting en7523_rx_setting_tbl[TRANSFORMER_TYPE_MAX][MDI_TYPE_MAX] = {
	[TXMR] = {
		[MDI_0R] = { 0x0000, 0x1111, 0x0002 },
		[MDI_5R] = { 0x1111, 0x0000, 0x0002 },
	},
	[DISCRETE] = {
		[MDI_0R] = { 0x0000, 0x4444, 0x0002 },
		[MDI_5R] = { 0x1111, 0x0000, 0x0002 },
	},
};

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

/*
 * 2 chip revision
 * TXMR or discrete
 * 2 MDI type
 * TX AMP test/TX AMP 1G/TX AMP 100M/TX AMP 10M/R50
 * 4 PHY
 */
static int an7583_tx_amp_compensation_tbl[TRANSFORMER_TYPE_MAX][MDI_TYPE_MAX][CALIB_CONST_TYPE_MAX][4] = {
	[TXMR] = {
		[MDI_0R] = {
			[TX_AMP_TEST_A] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_B] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_C] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_D] = { -3, -3, -3, -3 },
			[TX_AMP_1G_A] = { 0, 0, 0, 0 },
			[TX_AMP_1G_B] = { 0, 0, 0, 0 },
			[TX_AMP_1G_C] = { 0, 0, 0, 0 },
			[TX_AMP_1G_D] = { 0, 0, 0, 0 },
			[TX_AMP_100M_A] = { 4, 4, 4, 4 },
			[TX_AMP_100M_B] = { 4, 4, 4, 4 },
			[TX_AMP_10M_A] = { 0, 0, 0, 0 },
			[TX_AMP_10M_B] = { 0, 0, 0, 0 },
			[R50_A] = { 4, 4, 0, 0 },
			[R50_B] = { 4, 4, 0, 0 },
			[R50_C] = { 5, 5, 3, 4 },
			[R50_D] = { 4, 4, 4, 0 },
		},
		[MDI_5R] = {
			[TX_AMP_TEST_A] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_B] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_C] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_D] = { -3, -3, -3, -3 },
			[TX_AMP_1G_A] = { 0, 0, 0, 0 },
			[TX_AMP_1G_B] = { 0, 0, 0, 0 },
			[TX_AMP_1G_C] = { 0, 0, 0, 0 },
			[TX_AMP_1G_D] = { 0, 0, 0, 0 },
			[TX_AMP_100M_A] = { 4, 4, 4, 4 },
			[TX_AMP_100M_B] = { 4, 4, 4, 4 },
			[TX_AMP_10M_A] = { 0, 0, 0, 0 },
			[TX_AMP_10M_B] = { 0, 0, 0, 0 },
			[R50_A] = { -4, -4, -4, -4 },
			[R50_B] = { -4, -4, -4, -4 },
			[R50_C] = { -2, -2, -2, -2 },
			[R50_D] = { -3, -3, -2, -2 },
		},
	},
	[DISCRETE] = {
		[MDI_0R] = {
			[TX_AMP_TEST_A] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_B] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_C] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_D] = { -3, -3, -3, -3 },
			[TX_AMP_1G_A] = { 0, 0, 0, 0 },
			[TX_AMP_1G_B] = { 0, 0, 0, 0 },
			[TX_AMP_1G_C] = { 0, 0, 0, 0 },
			[TX_AMP_1G_D] = { 0, 0, 0, 0 },
			[TX_AMP_100M_A] = { 4, 4, 4, 4 },
			[TX_AMP_100M_B] = { 4, 4, 4, 4 },
			[TX_AMP_10M_A] = { 0, 0, 0, 0 },
			[TX_AMP_10M_B] = { 0, 0, 0, 0 },
			[R50_A] = { 4, 4, 4, 4 },
			[R50_B] = { 4, 4, 4, 4 },
			[R50_C] = { 6, 6, 6, 6 },
			[R50_D] = { 4, 4, 4, 4 },
		},
		[MDI_5R] = {
			[TX_AMP_TEST_A] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_B] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_C] = { -3, -3, -3, -3 },
			[TX_AMP_TEST_D] = { -3, -3, -3, -3 },
			[TX_AMP_1G_A] = { 0, 0, 0, 0 },
			[TX_AMP_1G_B] = { 0, 0, 0, 0 },
			[TX_AMP_1G_C] = { 0, 0, 0, 0 },
			[TX_AMP_1G_D] = { 0, 0, 0, 0 },
			[TX_AMP_100M_A] = { 4, 4, 4, 4 },
			[TX_AMP_100M_B] = { 4, 4, 4, 4 },
			[TX_AMP_10M_A] = { 0, 0, 0, 0 },
			[TX_AMP_10M_B] = { 0, 0, 0, 0 },
			[R50_A] = { 0, 0, 0, 0 },
			[R50_B] = { 0, 0, 0, 0 },
			[R50_C] = { 0, 0, 0, 0 },
			[R50_D] = { 0, 0, 0, 0 },
		},
	},
};

static u8 an7583_zcal_to_r50ohm_0R[64] = {
	127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
	127, 127, 127, 122, 118, 114, 110, 106, 102,  98,  96,  92,  88,  85,  82,  80,
	 76,  72,  70,  68,  64,  62,  60,  57,  55,  52,  50,  48,  46,  44,  41,  40,
	 38,  36,  33,  32,  30,  28,  26,  24,  24,  22,  20,  18,  16,  16,  14,  12,
};

static u8 an7583_zcal_to_r50ohm_5R[64] = {
	127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
	127, 127, 127, 127, 127, 127, 127, 127, 127, 124, 120, 116, 112, 110, 106, 102,
	 99,  96,  93,  90,  88,  84,  81,  79,  76,  73,  71,  68,  66,  64,  61,  59,
	 56,  54,  52,  50,  48,  46,  44,  42,  40,  39,  37,  36,  33,  32,  31,  29,
};

static int airoha_cal_cycle_wait(struct phy_device *phydev)
{
	int ret;

	/*
	 * The EN7523 SDK flow does not poll here: it raises DA_CALIN,
	 * waits 20 us for the analog comparator, reads AD_CAL_CLK/COMP,
	 * then lowers DA_CALIN.  Using the generic MTK helper can sample the
	 * comparator too early on EN7523 and drive TX AMP to the rail.
	 */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_AD_CALIN,
			       MTK_PHY_DA_CALIN_FLAG);
	if (ret)
		return ret;

	udelay(20);

	ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_AD_CAL_CLK);
	if (ret < 0)
		goto out;

	if (!(ret & MTK_PHY_DA_CAL_CLK)) {
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_AD_CAL_COMP);
	if (ret < 0)
		goto out;

	ret = FIELD_GET(MTK_PHY_AD_CAL_COMP_OUT_MASK, ret);

out:
	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_AD_CALIN,
			   MTK_PHY_DA_CALIN_FLAG);

	return ret;
}

static bool airoha_is_en7580(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);

	return phydev->drv->phy_id == AIROHA_GPHY_ID_EN7528 &&
	       shared->variant == AIROHA_9491_EN7580;
}

static bool airoha_is_en7528(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);

	return phydev->drv->phy_id == AIROHA_GPHY_ID_EN7528 &&
	       shared->variant == AIROHA_9491_EN7528;
}

static bool airoha_is_9491(struct phy_device *phydev)
{
	return phydev->drv->phy_id == AIROHA_GPHY_ID_EN7528;
}

static int airoha_cal_cycle(struct phy_device *phydev, int devad,
			    u32 regnum, u16 mask, u16 cal_val)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	struct phy_device *phydev_p0;
	int ret;

	phydev_p0 = shared->phydev_p0;

	phy_modify_mmd(phydev, devad, regnum, mask, cal_val);

	ret = airoha_cal_cycle_wait(phydev_p0);
	phydev_dbg(phydev, "cal_val: 0x%x, ret: %d\n", cal_val, ret);

	return ret;
}

static int airoha_rext_cal_sw(struct phy_device *phydev)
{
	struct regmap *chip_scu;
	int calibration_polarity;
	u8 zcal_ctrl = 32;
	int first_calib;
	int ret;

	/* BG voltage output */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0xc000);

	if (!airoha_is_9491(phydev)) {
		/* tst_mode2 */
		phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xff, 0x2);
		phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, 0xff,
				   GENMASK(15, 4) | GENMASK(1, 0));
	}

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

	if (airoha_is_9491(phydev)) {
		const char *compat = airoha_is_en7580(phydev) ?
			"econet,en7580-chip-scu" : "econet,en7528-chip-scu";

		chip_scu = syscon_regmap_lookup_by_compatible(compat);
		if (!IS_ERR(chip_scu))
			regmap_update_bits(chip_scu, 0x16c, GENMASK(15, 13),
					   FIELD_PREP(GENMASK(15, 13),
						      zcal_ctrl >> 3));
		else
			phydev_dbg(phydev, "CHIP SCU unavailable for REXT mirror: %ld\n",
				   PTR_ERR(chip_scu));
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0, 0);

	return 0;
}

static int airoha_tx_offset_cal_sw(struct phy_device *phydev, u8 txg_calen_x)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
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
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0xc000);

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

	if (airoha_is_9491(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0000);
	if (airoha_is_en7528(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0100);

	/* 1e_96[15]:bypass_tx_offset_cal, Hw bypass, Fw cal */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x96,
		      0x8000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e,
		      0xf808);

	switch (txg_calen_x) {
	case PAIR_A:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(12));
		if (airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_ANA_CALEN | MTK_PHY_RG_ZCALEN_A);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_TXVOS_CALEN);
		}
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_A;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_A;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_A_B;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_A_MASK;
		break;
	case PAIR_B:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(8));
		if (airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_ANA_CALEN);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_TXVOS_CALEN | MTK_PHY_RG_ZCALEN_B);
		}
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_B;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_B;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_A_B;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_B_MASK;
		break;
	case PAIR_C:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(4));
		if (airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_ANA_CALEN);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_TXVOS_CALEN | MTK_PHY_RG_ZCALEN_C);
		}
		reg_dac1 = MTK_PHY_RG_DASN_DAC_IN0_C;
		reg_dac2 = MTK_PHY_RG_DASN_DAC_IN1_C;
		reg = MTK_PHY_RG_CR_TX_AMP_OFFSET_C_D;
		mask = MTK_PHY_CR_TX_AMP_OFFSET_C_MASK;
		break;
	case PAIR_D:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(0));
		if (airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_ANA_CALEN);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_TXVOS_CALEN | MTK_PHY_RG_ZCALEN_D);
		}
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

	if (airoha_is_9491(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0001);
	if (airoha_is_en7528(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0000);

	return 0;
}

static u16 airoha_tx_amp_limit(int val)
{
	if (val < 0)
		return 0;
	if (val > 0x3f)
		return 0x3f;

	return val;
}

static u16 airoha_field_prep(u16 mask, u16 val)
{
	return (val << __ffs(mask)) & mask;
}

static int airoha_tx_amp_cal_sw(struct phy_device *phydev, u8 txg_calen_x)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	u16 mask_gbe, mask_tbt, mask_tst, mask_hbt;
	u16 reg, reg_100, reg_dac1, reg_dac2;
	struct phy_device *phydev_p0;
	int calibration_polarity;
	int dev1e_145_tmp;
	int bmcr_tmp;
	bool saturation;
	u8 zcal_ctrl = 32;
	int first_calib;
	int ret = 0;

	phydev_p0 = shared->phydev_p0;

	bmcr_tmp = phy_read(phydev, MII_BMCR);
	if (bmcr_tmp < 0)
		return bmcr_tmp;

	dev1e_145_tmp = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x145);
	if (dev1e_145_tmp < 0)
		return dev1e_145_tmp;

	/* BG voltage output */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0xc000);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x145, 0x1010);

	if (airoha_is_9491(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0000);
	if (airoha_is_en7528(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0100);

	switch (phydev->drv->phy_id) {
	case AIROHA_GPHY_ID_EN7528:
	case AIROHA_GPHY_ID_EN7523:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
			      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
			      MTK_PHY_RG_TXVOS_CALEN);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0);
		phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0);
		break;
	case AIROHA_GPHY_ID_AN7581:
	case AIROHA_GPHY_ID_AN7583:
	default:
		/* select 1V */
		phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0x10);
		break;
	}

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1,
		      MTK_PHY_RG_TXVOS_CALEN);

	/* Enable Tx VLD. */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xf808);

	/* Force 1G full duplex for calibration */
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);

	switch (txg_calen_x) {
	case PAIR_A:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, BIT(12));
		if (phydev->drv->phy_id == AIROHA_GPHY_ID_EN7523 ||
		    airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_CAL_CKINV |
				      MTK_PHY_RG_ANA_CALEN |
				      MTK_PHY_RG_ZCALEN_A);
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_TXVOS_CALEN);
		}
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
		if (phydev->drv->phy_id == AIROHA_GPHY_ID_EN7523 ||
		    airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_CAL_CKINV |
				      MTK_PHY_RG_ANA_CALEN);
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_ZCALEN_B |
				      MTK_PHY_RG_TXVOS_CALEN);
		}
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
		if (phydev->drv->phy_id == AIROHA_GPHY_ID_EN7523 ||
		    airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_CAL_CKINV |
				      MTK_PHY_RG_ANA_CALEN);
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_ZCALEN_C |
				      MTK_PHY_RG_TXVOS_CALEN);
		}
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
		if (phydev->drv->phy_id == AIROHA_GPHY_ID_EN7523 ||
		    airoha_is_en7580(phydev)) {
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG0,
				      MTK_PHY_RG_CAL_CKINV |
				      MTK_PHY_RG_ANA_CALEN);
			phy_write_mmd(phydev, MDIO_MMD_VEND1,
				      MTK_PHY_RG_ANA_CAL_RG1,
				      MTK_PHY_RG_ZCALEN_D |
				      MTK_PHY_RG_TXVOS_CALEN);
		}
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
	if (first_calib < 0) {
		phydev_err(phydev, "TX AMP calibration failed.\n");
		ret = -EINVAL;
		goto restore;
	}

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
		ret = -EINVAL;
		goto restore;
	}

	saturation = zcal_ctrl == 0 ||
		     zcal_ctrl == FIELD_MAX(MTK_PHY_RG_REXT_ZCAL_CTRL_MASK);
	if (saturation)
		phydev_dbg(phydev,
			   "TX AMP SW calibration saturation on pair %u (%d), using vendor fallback values\n",
			   txg_calen_x, zcal_ctrl);

	switch (phydev->drv->phy_id) {
	case AIROHA_GPHY_ID_EN7523: {
		u16 val_gbe, val_tbt, val_hbt, val_tst;

		/*
		 * Match the EN7523 R45 SDK flow. The calibrated value is the
		 * 1G/GbE DAC value. 100M/HBT, 10M/TBT and test-mode/TST use
		 * per-pair offsets and special fallback codes when the analog
		 * comparator saturates.
		 */
		if (saturation) {
			val_gbe = 0x20;
			val_tbt = 0x10;

			switch (txg_calen_x) {
			case PAIR_A:
				val_hbt = 0x18;
				val_tst = 0x10;
				break;
			case PAIR_B:
				val_hbt = phydev->mdio.addr == AIROHA_DEFAULT_PORT0_ADDR ?
					  0x24 : 0x28;
				val_tst = 0x20;
				break;
			case PAIR_C:
			case PAIR_D:
			default:
				val_hbt = 0x04;
				val_tst = 0x20;
				break;
			}
		} else {
			val_gbe = zcal_ctrl;
			val_tbt = zcal_ctrl;
			val_hbt = txg_calen_x == PAIR_A ?
				  airoha_tx_amp_limit(zcal_ctrl + 3) : zcal_ctrl;
			val_tst = txg_calen_x == PAIR_A ?
				  airoha_tx_amp_limit(zcal_ctrl - 1) :
				  airoha_tx_amp_limit(zcal_ctrl - 3);
		}

		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg,
			       mask_gbe | mask_tbt,
			       airoha_field_prep(mask_gbe, val_gbe) |
			       airoha_field_prep(mask_tbt, val_tbt));
		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg_100,
			       mask_hbt | mask_tst,
			       airoha_field_prep(mask_hbt, val_hbt) |
			       airoha_field_prep(mask_tst, val_tst));
		break;
	}
	case AIROHA_GPHY_ID_EN7528: {
		u16 val_gbe, val_tbt, val_hbt, val_tst;

		if (airoha_is_en7580(phydev)) {
			static const s8 tst_offset[4][4] = {
				{ 5, 7, 9, 7 },
				{ 4, 5, 10, 5 },
				{ 4, 6, 9, 6 },
				{ 3, 7, 9, 6 },
			};
			u8 port = phydev->mdio.addr - 9;

			if (port >= ARRAY_SIZE(tst_offset))
				port = 0;

			if (saturation) {
				val_gbe = txg_calen_x == PAIR_A || txg_calen_x == PAIR_C ?
					  0x10 : 0x20;
				val_tbt = 0x10;
				if (txg_calen_x == PAIR_A)
					val_hbt = 0x18;
				else if (txg_calen_x == PAIR_B)
					val_hbt = phydev->mdio.addr == 9 ? 0x24 : 0x28;
				else
					val_hbt = 0x04;
				val_tst = txg_calen_x == PAIR_B || txg_calen_x == PAIR_D ?
					  0x20 : 0x10;
			} else {
				val_tst = airoha_tx_amp_limit(zcal_ctrl -
							 tst_offset[port][txg_calen_x]);
				val_gbe = val_tst;
				val_tbt = airoha_tx_amp_limit(zcal_ctrl + 13);
				if (txg_calen_x == PAIR_B && phydev->mdio.addr == 9)
					val_hbt = zcal_ctrl;
				else if (txg_calen_x == PAIR_B && phydev->mdio.addr == 10)
					val_hbt = airoha_tx_amp_limit(zcal_ctrl + 1);
				else
					val_hbt = airoha_tx_amp_limit(zcal_ctrl + 2);
			}
		} else if (saturation) {
			val_gbe = 0x20;
			val_tbt = 0x10;
			val_hbt = 0x04;
			val_tst = 0x10;
		} else {
			val_gbe = airoha_tx_amp_limit(zcal_ctrl + 14);
			val_tst = airoha_tx_amp_limit(zcal_ctrl + 14);
			val_hbt = airoha_tx_amp_limit(zcal_ctrl +
				((phydev->mdio.addr == 9 || phydev->mdio.addr == 10) &&
				 (txg_calen_x == PAIR_A || txg_calen_x == PAIR_B) ? 3 : 2));
			if (phydev->mdio.addr == 12 && txg_calen_x == PAIR_A)
				val_tbt = airoha_tx_amp_limit(zcal_ctrl - 5);
			else
				val_tbt = airoha_tx_amp_limit(zcal_ctrl + 3);
		}

		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg,
			       mask_gbe | mask_tbt,
			       airoha_field_prep(mask_gbe, val_gbe) |
			       airoha_field_prep(mask_tbt, val_tbt));
		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg_100,
			       mask_hbt | mask_tst,
			       airoha_field_prep(mask_hbt, val_hbt) |
			       airoha_field_prep(mask_tst, val_tst));
		break;
	}
	case AIROHA_GPHY_ID_AN7581:
	case AIROHA_GPHY_ID_AN7583:
	default:
		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg,
			       mask_gbe | mask_tbt,
			       (zcal_ctrl << __ffs(mask_gbe)) |
			       (zcal_ctrl << __ffs(mask_tbt)));

		phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg_100,
			       mask_hbt | mask_tst,
			       (zcal_ctrl << __ffs(mask_hbt)) |
			       (zcal_ctrl << __ffs(mask_tst)));
		break;
	}

	ret = 0;

restore:
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
	switch (phydev->drv->phy_id) {
	case AIROHA_GPHY_ID_EN7528:
	case AIROHA_GPHY_ID_EN7523:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0);
		phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6, 0);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);
		break;
	case AIROHA_GPHY_ID_AN7581:
	case AIROHA_GPHY_ID_AN7583:
	default:
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);
		break;
	}
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, 0);

	/* Restore BMCR */
	phy_write(phydev, MII_BMCR, bmcr_tmp);

	/* Restore MDI */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x145, dev1e_145_tmp);

	if (airoha_is_9491(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0001);
	if (airoha_is_en7528(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0000);

	return ret;
}

static int airoha_tx_r50_cal_sw(struct phy_device *phydev, u8 txg_calen_x)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	struct phy_device *phydev_p0;
	u16 dev1e_145_tmp, bmcr_tmp;
	int calibration_polarity;
	u8 zcal_ctrl = 32;
	int first_calib;
	u16 reg;
	int ret;

	phydev_p0 = shared->phydev_p0;

	/* BG voltage output */
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0xc000);

	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev_p0, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6,
		      airoha_is_9491(phydev) ? 0 : 0x10);

	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG0,
		      MTK_PHY_RG_CAL_CKINV | MTK_PHY_RG_ANA_CALEN);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG1, 0);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RG_ANA_CAL_RG6,
		      airoha_is_9491(phydev) ? 0 : 0x10);

	/* Force 1G full duplex for calibration */
	bmcr_tmp = phy_read(phydev, MII_BMCR);
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);

	/* Force MDI */
	dev1e_145_tmp = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x0145);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0145, 0x1010);

	/* disable tx slew control */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0185, 0x0000);
	if (airoha_is_en7528(phydev))
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

	if (shared->r50_cal_offset) {
		if (zcal_ctrl > shared->r50_cal_offset)
			zcal_ctrl = shared->r50_cal_tbl[zcal_ctrl - shared->r50_cal_offset];
	} else {
		zcal_ctrl = shared->r50_cal_tbl[zcal_ctrl];
	}

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
	if (airoha_is_en7528(phydev))
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0000);

	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0x0);

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

	if (phydev->drv->phy_id == AIROHA_GPHY_ID_AN7581)
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

static int airoha_phy_calib(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
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

static int airoha_9491_phy_calib(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	int ret;

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

	phy_write_mmd(shared->phydev_p0, MDIO_MMD_VEND2, 0x100, 0x0000);

	return 0;
}

static int airoha_phy_auto_select_transformer(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
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

static int airoha_phy_tx_amp_compensation(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);

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


static void airoha_phy_write_mmd_regs(struct phy_device *phydev,
				      const struct airoha_mmd_reg *regs,
				      size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		phy_write_mmd(phydev, regs[i].devad, regs[i].reg, regs[i].val);
}

static void airoha_phy_write_tr_regs(struct phy_device *phydev,
				     const struct airoha_tr_reg *regs,
				     size_t count)
{
	size_t i;

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_52B5);
	for (i = 0; i < count; i++)
		__mtk_tr_write(phydev, regs[i].ch_addr, regs[i].node_addr,
			       regs[i].data_addr, regs[i].val);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);
}

/* EN7512/EN7521 standalone GPHY initialization from the vendor SDK. */
static const u8 en7512_zcal_to_r50ohm_100[64] = {
	127, 127, 127, 127, 127, 127, 126, 123, 120, 117, 114, 112, 110, 107, 105, 103,
	101,  99,  97,  79,  77,  75,  74,  72,  70,  69,  67,  66,  65,  47,  46,  45,
	 43,  42,  41,  40,  39,  38,  37,  36,  34,  34,  33,  32,  15,  14,  13,  12,
	 11,  10,  10,   9,   8,   7,   7,   6,   5,   4,   4,   3,   2,   2,   1,   1,
};

static const struct airoha_mmd_reg en7512_ge_init_regs[] = {
	/* Global MMD (devad 0x1f) settings. */
	{ MDIO_MMD_VEND2, 0x273, 0x1000 },
	{ MDIO_MMD_VEND2, 0x272, 0x3cff },
	{ MDIO_MMD_VEND2, 0x269, 0x4344 },
	{ MDIO_MMD_VEND2, 0x044, 0x0030 },
	{ MDIO_MMD_VEND2, 0x27c, 0x0808 },
	{ MDIO_MMD_VEND2, 0x27b, 0x1177 },
	{ MDIO_MMD_VEND2, 0x417, 0x7775 },
	{ MDIO_MMD_VEND2, 0x024, 0xc007 },
	{ MDIO_MMD_VEND2, 0x025, 0x003f },
	{ MDIO_MMD_VEND2, 0x021, 0x800a },

	/* Local MMD (devad 0x1e) settings. */
	{ MDIO_MMD_VEND1, 0x000, 0x01b7 },
	{ MDIO_MMD_VEND1, 0x001, 0x01c0 },
	{ MDIO_MMD_VEND1, 0x002, 0x01c0 },
	{ MDIO_MMD_VEND1, 0x003, 0x0090 },
	{ MDIO_MMD_VEND1, 0x004, 0x0205 },
	{ MDIO_MMD_VEND1, 0x005, 0x0205 },
	{ MDIO_MMD_VEND1, 0x006, 0x039c },
	{ MDIO_MMD_VEND1, 0x007, 0x03c0 },
	{ MDIO_MMD_VEND1, 0x008, 0x03c7 },
	{ MDIO_MMD_VEND1, 0x009, 0x0297 },
	{ MDIO_MMD_VEND1, 0x00a, 0x0005 },
	{ MDIO_MMD_VEND1, 0x00b, 0x0007 },
	{ MDIO_MMD_VEND1, 0x014, 0x0040 },
	{ MDIO_MMD_VEND1, 0x044, 0x0060 },
	{ MDIO_MMD_VEND1, 0x176, 0x5500 },
	{ MDIO_MMD_VEND1, 0x177, 0x0055 },
	{ MDIO_MMD_VEND1, 0x012, 0x2c10 },
	{ MDIO_MMD_VEND1, 0x017, 0x0410 },
	{ MDIO_MMD_VEND1, 0x0a6, 0x0350 },
	{ MDIO_MMD_AN,    0x03c, 0x0000 },
};

static const struct airoha_tr_reg en7512_ge_tr_regs[] = {
	{ 0x1, 0xf, 0x03, 0x082422 },
};

static int en7512_cal_wait(struct phy_device *phydev, unsigned int delay_us)
{
	int ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x17c, 0x0001);
	if (ret)
		return ret;

	udelay(delay_us);

	ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x17b);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x17c, 0x0000);
	if (ret < 0)
		return ret;

	return ret & BIT(0);
}

static int en7512_cal_comp(struct phy_device *phydev)
{
	int ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x17a);

	if (ret < 0)
		return ret;

	return !!(ret & BIT(8));
}

static int en7512_rext_cal(struct phy_device *phydev)
{
	struct regmap *chip_scu;
	int first, comp, ret, polarity;
	u8 zcal = 0x20;
	int i;

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x1110);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe1, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe0, zcal);

	first = en7512_cal_wait(phydev, 100);
	if (first <= 0) {
		phydev_warn(phydev, "REXT calibration did not start, using default\n");
		zcal = 0x20;
		goto finish;
	}

	comp = en7512_cal_comp(phydev);
	if (comp < 0)
		return comp;
	polarity = comp ? -1 : 1;

	for (i = 0; i < 63; i++) {
		if ((polarity < 0 && zcal == 0) ||
		    (polarity > 0 && zcal == 0x3f)) {
			phydev_warn(phydev, "REXT calibration saturated\n");
			zcal = 0x20;
			break;
		}

		zcal += polarity;
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe0, zcal);
		ret = en7512_cal_wait(phydev, 100);
		if (ret <= 0) {
			phydev_warn(phydev, "REXT calibration timeout, using default\n");
			zcal = 0x20;
			break;
		}

		ret = en7512_cal_comp(phydev);
		if (ret < 0)
			return ret;
		if (ret != comp)
			break;
	}

finish:
	/* The SDK mirrors the REXT code in both bytes of 1e:e0. */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe0, (zcal << 8) | zcal);
	phy_modify_mmd(phydev, MDIO_MMD_VEND2, 0x115, GENMASK(2, 0),
		       FIELD_PREP(GENMASK(2, 0), zcal >> 3));

	/* EN7512 also mirrors RG_BG_RASEL into CHIP SCU register 0x16c. */
	chip_scu = syscon_regmap_lookup_by_compatible("airoha,chip-scu");
	if (!IS_ERR(chip_scu))
		regmap_update_bits(chip_scu, 0x16c, GENMASK(15, 13),
				   FIELD_PREP(GENMASK(15, 13), zcal >> 3));
	else
		phydev_dbg(phydev, "CHIP SCU unavailable for REXT mirror: %ld\n",
			   PTR_ERR(chip_scu));

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x0000);
	phydev_info(phydev, "EN7512 REXT calibration: 0x%02x\n", zcal);

	return 0;
}

static void en7512_r50_store(struct phy_device *phydev, int pair, u8 zcal)
{
	int reg = pair < PAIR_C ? 0x174 : 0x175;
	u16 mask = (pair == PAIR_A || pair == PAIR_C) ? GENMASK(14, 8) : GENMASK(6, 0);
	u16 enable = (pair == PAIR_A || pair == PAIR_C) ? BIT(15) : BIT(7);

	phy_modify_mmd(phydev, MDIO_MMD_VEND1, reg, mask | enable,
		       enable | airoha_field_prep(mask, zcal));
}

static int en7512_r50_cal(struct phy_device *phydev)
{
	static const u16 db_val[] = { 0x1101, 0x1100, 0x1100, 0x1100 };
	static const u16 dc_val[] = { 0x0000, 0x1000, 0x0100, 0x0010 };
	unsigned int idx;

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x1100);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, 0x0000);

	for (idx = 0; idx < ARRAY_SIZE(db_val); idx++) {
		int first, comp, ret, polarity, base_e0, i;
		int pair = PAIR_A + idx;
		u8 zcal = 0x20;

		base_e0 = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0xe0);
		if (base_e0 < 0)
			return base_e0;
		base_e0 &= ~GENMASK(5, 0);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe0, base_e0 | zcal);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, db_val[idx]);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, dc_val[idx]);

		first = en7512_cal_wait(phydev, 20);
		if (first <= 0) {
			phydev_warn(phydev, "R50 pair %d calibration did not start\n", pair);
			zcal = 0x20;
			goto store;
		}

		comp = en7512_cal_comp(phydev);
		if (comp < 0)
			return comp;
		polarity = comp ? -1 : 1;

		for (i = 0; i < 63; i++) {
			if ((polarity < 0 && zcal == 0) ||
			    (polarity > 0 && zcal == 0x3f)) {
				phydev_warn(phydev, "R50 pair %d calibration saturated\n", pair);
				break;
			}

			zcal += polarity;
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe0,
				      base_e0 | zcal);
			ret = en7512_cal_wait(phydev, 20);
			if (ret <= 0) {
				phydev_warn(phydev, "R50 pair %d calibration timeout\n", pair);
				zcal = 0x20;
				break;
			}

			ret = en7512_cal_comp(phydev);
			if (ret < 0)
				return ret;
			if (ret != comp)
				break;
		}

		/* SDK uses the 100-ohm conversion table and subtracts two. */
		zcal = en7512_zcal_to_r50ohm_100[zcal];
		zcal = zcal > 2 ? zcal - 2 : 0;
store:
		en7512_r50_store(phydev, pair, zcal);
		phydev_dbg(phydev, "R50 pair %d: 0x%02x\n", pair, zcal);
	}

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x0000);
	return 0;
}

static int en7512_tx_offset_cal(struct phy_device *phydev)
{
	static const u16 cal_en[] = { BIT(12), BIT(8), BIT(4), BIT(0) };
	static const u16 dac0[] = { 0x17d, 0x17e, 0x17f, 0x180 };
	static const u16 dac1[] = { 0x181, 0x182, 0x183, 0x184 };
	static const u16 reg[] = { 0x172, 0x172, 0x173, 0x173 };
	static const u16 mask[] = { GENMASK(13, 8), GENMASK(5, 0),
				    GENMASK(13, 8), GENMASK(5, 0) };
	unsigned int idx;
	int pair;

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x0100);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, 0x0001);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x096, 0x8000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03e, 0xf808);

	for (idx = 0; idx < ARRAY_SIZE(cal_en); idx++) {
		int first, comp, ret, polarity, offset = 0, i;
		int pair = PAIR_A + idx;
		int base;
		u16 val = 0;

		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, cal_en[idx]);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac0[idx], 0x8000);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac1[idx], 0x8000);

		base = phy_read_mmd(phydev, MDIO_MMD_VEND1, reg[idx]);
		if (base < 0)
			return base;
		base &= ~mask[idx];
		phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx], base);

		first = en7512_cal_wait(phydev, 20);
		if (first <= 0) {
			phydev_warn(phydev, "TX offset pair %d calibration did not start\n", pair);
			continue;
		}

		comp = en7512_cal_comp(phydev);
		if (comp < 0)
			return comp;
		polarity = comp ? -1 : 1;

		for (i = 0; i < 63; i++) {
			if ((polarity < 0 && offset <= -31) ||
			    (polarity > 0 && offset >= 31)) {
				phydev_warn(phydev,
					    "TX offset pair %d calibration saturated\n",
					    pair);
				break;
			}

			offset += polarity;
			val = offset >= 0 ? offset : BIT(5) | abs(offset);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx],
				      base | airoha_field_prep(mask[idx], val));
			ret = en7512_cal_wait(phydev, 20);
			if (ret <= 0) {
				phydev_warn(phydev,
					    "TX offset pair %d calibration timeout\n",
					    pair);
				phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx], base);
				break;
			}

			ret = en7512_cal_comp(phydev);
			if (ret < 0)
				return ret;
			if (ret != comp)
				break;
		}
		phydev_dbg(phydev, "TX offset pair %d: %d\n", pair, offset);
	}

	for (pair = 0; pair < 4; pair++) {
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac0[pair], 0x0000);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac1[pair], 0x0000);
	}
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03e, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, 0x0000);

	return 0;
}

static int en7512_tx_amp_cal(struct phy_device *phydev)
{
	static const u16 cal_en[] = { BIT(12), BIT(8), BIT(4), BIT(0) };
	static const u16 dac0[] = { 0x17d, 0x17e, 0x17f, 0x180 };
	static const u16 dac1[] = { 0x181, 0x182, 0x183, 0x184 };
	static const u16 reg[] = { 0x012, 0x017, 0x019, 0x021 };
	static const u16 reg100[] = { 0x016, 0x018, 0x020, 0x022 };
	static const u16 mask[] = { GENMASK(15, 10), GENMASK(13, 8),
				    GENMASK(13, 8), GENMASK(13, 8) };
	unsigned int idx;
	int pair;

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x1100);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, 0x0001);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe1, 0x0010);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03e, 0xf808);

	for (idx = 0; idx < ARRAY_SIZE(cal_en); idx++) {
		int first, comp, ret, polarity, amp = 0x20, i;
		bool saturated = false, finished = false;
		int pair = PAIR_A + idx;
		int base, base100;
		u16 low100, high100;

		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, cal_en[idx]);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac0[idx], 0x8000 | 0x0f0);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac1[idx], 0x8000 | 0x0f0);

		base = phy_read_mmd(phydev, MDIO_MMD_VEND1, reg[idx]);
		if (base < 0)
			return base;
		base &= ~mask[idx];
		phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx],
			      base | airoha_field_prep(mask[idx], amp));

		first = en7512_cal_wait(phydev, 20);
		if (first <= 0) {
			phydev_warn(phydev,
				    "TX amplitude pair %d calibration did not start\n",
				    pair);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx],
				      base | airoha_field_prep(mask[idx], 0x20));
			continue;
		}

		comp = en7512_cal_comp(phydev);
		if (comp < 0)
			return comp;
		polarity = comp ? -1 : 1;

		for (i = 0; i < 63; i++) {
			if ((polarity < 0 && amp == 0) ||
			    (polarity > 0 && amp == 0x3f)) {
				saturated = true;
				phydev_warn(phydev,
					    "TX amplitude pair %d calibration saturated\n",
					    pair);
				break;
			}

			amp += polarity;
			phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx],
				      base | airoha_field_prep(mask[idx], amp));
			ret = en7512_cal_wait(phydev, 100);
			if (ret <= 0) {
				phydev_warn(phydev,
					    "TX amplitude pair %d calibration timeout\n",
					    pair);
				amp = 0x20;
				phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx],
					      base | airoha_field_prep(mask[idx], amp));
				break;
			}

			ret = en7512_cal_comp(phydev);
			if (ret < 0)
				return ret;
			if (ret != comp) {
				finished = true;
				break;
			}
		}

		/* Vendor adds two codes only after a successful comparator edge. */
		if (finished)
			phy_write_mmd(phydev, MDIO_MMD_VEND1, reg[idx],
				      base | airoha_field_prep(mask[idx],
							min(amp + 2, 0x3f)));

		base100 = phy_read_mmd(phydev, MDIO_MMD_VEND1, reg100[idx]);
		if (base100 < 0)
			return base100;
		low100 = saturated || !amp ? 0x10 : min(amp + 14, 0x3f);
		high100 = saturated || !amp ? 0x04 : min(amp + 9, 0x3f);
		base100 &= ~(GENMASK(5, 0) | mask[idx]);
		base100 |= FIELD_PREP(GENMASK(5, 0), low100);
		base100 |= airoha_field_prep(mask[idx], high100);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, reg100[idx], base100);
		phydev_dbg(phydev, "TX amplitude pair %d: 0x%02x\n", pair, amp);
	}

	for (pair = 0; pair < 4; pair++) {
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac0[pair], 0x0000);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, dac1[pair], 0x0000);
	}
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdb, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdc, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03e, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xdd, 0x0000);

	return 0;
}

static void en7512_rx_offset_cal(struct phy_device *phydev)
{
	int val;

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x096, 0x8000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x037, 0x0033);
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, 0x039,
		       BIT(14) | BIT(11), 0);
	phy_modify_mmd(phydev, MDIO_MMD_VEND2, 0x107, BIT(12), 0);
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, 0x171,
		       GENMASK(8, 7), GENMASK(8, 7));

	val = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x039);
	if (val >= 0) {
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x039, val | BIT(13));
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x039, val & ~BIT(13));
	}

	mdelay(10);
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, 0x171, GENMASK(8, 7), 0);
}

static int en7512_phy_calib(struct phy_device *phydev)
{
	int saved_145, saved_bmcr;
	int ret;

	saved_bmcr = phy_read(phydev, MII_BMCR);
	if (saved_bmcr < 0)
		return saved_bmcr;
	saved_145 = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x145);
	if (saved_145 < 0)
		return saved_145;

	/* Match doGePhyALLAnalogCal(): force 1000/full, MDI and BG output. */
	phy_write(phydev, MII_BMCR, BMCR_FULLDPLX | BMCR_SPEED1000);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x145, 0x1010);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x185, 0x0000);

	ret = en7512_rext_cal(phydev);
	if (ret)
		goto restore;
	ret = en7512_r50_cal(phydev);
	if (ret)
		goto restore;
	ret = en7512_tx_offset_cal(phydev);
	if (ret)
		goto restore;
	ret = en7512_tx_amp_cal(phydev);
	if (ret)
		goto restore;
	en7512_rx_offset_cal(phydev);

restore:
	phy_write(phydev, MII_BMCR, saved_bmcr);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x100, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x145, saved_145);

	if (!ret)
		phydev_info(phydev, "EN7512/EN7521 analog calibration complete\n");
	return ret;
}

static int en7512_phy_probe(struct phy_device *phydev)
{
	/* EN7512 has one standalone GPHY at MDIO address 12. */
	return en7512_phy_calib(phydev);
}

static int en7512_phy_config_init(struct phy_device *phydev)
{
	int ret;

	airoha_phy_write_mmd_regs(phydev, en7512_ge_init_regs,
				  ARRAY_SIZE(en7512_ge_init_regs));
	airoha_phy_write_tr_regs(phydev, en7512_ge_tr_regs,
				 ARRAY_SIZE(en7512_ge_tr_regs));

	/* Vendor mt7512Ge_cfg local Clause 22 settings. */
	ret = phy_write(phydev, MII_CTRL1000, 0x1e00);
	if (ret)
		return ret;

	ret = phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_1);
	if (ret < 0)
		return ret;
	__phy_write(phydev, 0x14, 0x3a14);

	return phy_restore_page(phydev, ret, 0);
}

static void en7523_phy_apply_rx_setting(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	const struct en7523_rx_setting *rx_setting;
	struct phy_device *phydev_p0;
	u8 phy_offset;

	phydev_p0 = shared->phydev_p0;
	phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;
	rx_setting = &en7523_rx_setting_tbl[shared->transformer_type[phy_offset]]
					  [shared->mdi_resister_type];

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe6, rx_setting->e6);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe7, rx_setting->e7);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xfe, rx_setting->fe);
}

struct en7523_sdk_final_reg {
	u16 reg;
	u16 val[4];
};

/*
 * Temporary EN7523 R45 final profile captured from the vendor SDK after
 * PHY calibration on the MitraStar GPT-2742GX4X5v6. This keeps runtime
 * behavior close to the SDK while the proper compensation tables and
 * board-type detection are still being derived.
 */
static const struct en7523_sdk_final_reg en7523_sdk_final_1e_regs[] = {
	{ 0x012, { 0x9826, 0x9c27, 0x9425, 0x9826 } },
	{ 0x016, { 0x941e, 0x981f, 0x901d, 0x941e } },
	{ 0x017, { 0x3434, 0x2b2b, 0x2727, 0x2727 } },
	{ 0x018, { 0x342a, 0x2b21, 0x271d, 0x271d } },
	{ 0x019, { 0x3232, 0x2e2e, 0x2525, 0x1e1e } },
	{ 0x020, { 0x3228, 0x2e24, 0x251b, 0x1e14 } },
	{ 0x021, { 0x2f2f, 0x2828, 0x2424, 0x2a2a } },
	{ 0x022, { 0x2f25, 0x281e, 0x241a, 0x2a20 } },
	{ 0x172, { 0x2524, 0x2325, 0x2726, 0x2324 } },
	{ 0x173, { 0x2822, 0x2124, 0x2424, 0x2323 } },
	{ 0x174, { 0xd8de, 0xdbe0, 0xdee3, 0xdee3 } },
	{ 0x175, { 0xe0e0, 0xe0e0, 0xe3e3, 0xe3e3 } },
};

static void en7523_phy_apply_sdk_final_profile(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	struct phy_device *phydev_p0 = shared->phydev_p0;
	u8 phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;
	int i;

	if (phy_offset >= 4)
		return;

	for (i = 0; i < ARRAY_SIZE(en7523_sdk_final_1e_regs); i++)
		phy_write_mmd(phydev, MDIO_MMD_VEND1,
			      en7523_sdk_final_1e_regs[i].reg,
			      en7523_sdk_final_1e_regs[i].val[phy_offset]);
}

static void en7523_phy_apply_normal_init(struct phy_device *phydev)
{
	static const u16 lre_regs[] = {
		0x202, 0x204, 0x206, 0x208, 0x20a, 0x20e, 0x210, 0x212,
		0x214, 0x216, 0x21a, 0x21c, 0x21e, 0x220, 0x222, 0x226,
		0x228, 0x22a, 0x22c, 0x22e,
	};
	size_t i;

	/* Long Loop Reach setting. */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x11, 0xff00);
	for (i = 0; i < ARRAY_SIZE(lre_regs); i++) {
		phy_write_mmd(phydev, MDIO_MMD_VEND2, lre_regs[i], 0x2219);
		phy_write_mmd(phydev, MDIO_MMD_VEND2, lre_regs[i] + 1, 0x0023);
	}

	/* RX setting. */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3c, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3d, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_AN, 0x3c, 0x0000);
	en7523_phy_apply_rx_setting(phydev);

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_1);
	__phy_write(phydev, 0x14, 0x3a18);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	phy_write(phydev, 0x9, 0x1e00);
}

static void en7580_phy_set_calibration_gating(struct phy_device *phydev, bool enable)
{
	int addr;

	for (addr = 9; addr <= 12; addr++) {
		struct phy_device *port = mdiobus_get_phy(phydev->mdio.bus, addr);

		if (port)
			phy_write_mmd(port, MDIO_MMD_VEND1, 0x0015, enable ? 0x0004 : 0);
	}
}

static int en7580_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	int ret;

	if (!shared->phydev_p0)
		return -ENODEV;

	ret = genphy_soft_reset(phydev);
	if (ret)
		return ret;

	shared->mdi_resister_type = MDI_5R;
	shared->r50_cal_tbl = en7580_zcal_to_r44ohm;
	shared->r50_cal_offset = 0;

	airoha_phy_write_mmd_regs(phydev, en7580_ge_init_regs,
				  ARRAY_SIZE(en7580_ge_init_regs));

	phy_write(phydev, MII_CTRL1000, 0x0600);
	phy_write(phydev, 0x17, 0x00b0);
	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_1);
	__phy_write(phydev, 0x14, 0x3a14);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	airoha_phy_write_tr_regs(phydev, en7580_ge_tr_regs,
				 ARRAY_SIZE(en7580_ge_tr_regs));

	en7580_phy_set_calibration_gating(phydev, true);
	ret = airoha_9491_phy_calib(phydev);
	en7580_phy_set_calibration_gating(phydev, false);
	if (ret)
		return ret;

	phy_write(phydev, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

	return 0;
}

static int en7528_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	int ret;

	if (!shared->phydev_p0)
		return -ENODEV;

	ret = genphy_soft_reset(phydev);
	if (ret)
		return ret;

	shared->mdi_resister_type = MDI_5R;
	shared->r50_cal_tbl = en7528_zcal_to_r45ohm;
	shared->r50_cal_offset = 0;

	airoha_phy_write_mmd_regs(phydev, en7528_ge_init_regs,
				  ARRAY_SIZE(en7528_ge_init_regs));

	/* Clause 22 local settings from the EN7528 SDK. */
	phy_write(phydev, MII_CTRL1000, 0x0600);
	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_1);
	__phy_write(phydev, 0x14, 0x3a14);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	airoha_phy_write_tr_regs(phydev, en7528_ge_tr_regs,
				 ARRAY_SIZE(en7528_ge_tr_regs));

	/* The EN7528 R45 flow uses LDO setting 1e:fb[9:8] = 01. */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0100);

	ret = airoha_9491_phy_calib(phydev);
	if (ret)
		return ret;

	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00fb, 0x0000);
	phy_write(phydev, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

	return 0;
}

static int en7528_en7580_phy_config_init(struct phy_device *phydev)
{
	if (airoha_is_en7580(phydev))
		return en7580_phy_config_init(phydev);

	return en7528_phy_config_init(phydev);
}

static int en7523_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	int ret;

	/* FIXME: Read SoC PDIDR if available or default to MDI_5R */
	shared->mdi_resister_type = MDI_5R;

	shared->tx_amp_compensation_tbl = &en7523_tx_amp_compensation_tbl;
	shared->r50_cal_tbl = en7523_zcal_to_r45ohm;
	shared->r50_cal_offset = 11;

	/* Initial register load before analog calibration. */
	airoha_phy_write_mmd_regs(phydev, en7523_ge_init_regs,
				 ARRAY_SIZE(en7523_ge_init_regs));
	airoha_phy_write_tr_regs(phydev, en7523_ge_tr_regs,
				ARRAY_SIZE(en7523_ge_tr_regs));

	ret = airoha_phy_calib(phydev);
	if (ret)
		return ret;

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_52B5);
	__mtk_tr_write(phydev, 0x1, 0xf, 0x12, 0x5e4d2a);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	ret = airoha_phy_auto_select_transformer(phydev);
	if (ret)
		return ret;

	ret = airoha_phy_tx_amp_compensation(phydev);
	if (ret)
		return ret;

	en7523_phy_apply_normal_init(phydev);
	en7523_phy_apply_sdk_final_profile(phydev);

	return 0;
}

static int an7581_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	struct phy_device *phydev_p0;
	u32 soc_pdidr;
	u8 phy_offset;
	int ret;

	phydev_p0 = shared->phydev_p0;
	phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;

	/* FIXME: Read SoC PDIDR if available or default to 1 */
	soc_pdidr = 1;

	shared->mdi_resister_type = MDI_5R;
	if (soc_pdidr == 1)
		shared->tx_amp_compensation_tbl = &an7581_tx_amp_compensation_tbl[0];
	else
		shared->tx_amp_compensation_tbl = &an7581_tx_amp_compensation_tbl[1];
	shared->r50_cal_tbl = an7581_zcal_to_r45ohm;
	shared->r50_cal_offset = 0;

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

static int an7581_phy_probe(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared;
	struct mtk_socphy_priv *priv;
	struct pinctrl *pinctrl;
	int ret;

	/* Toggle pinctrl to enable PHY LED */
	pinctrl = devm_pinctrl_get_select(&phydev->mdio.dev, "gbe-led");
	if (IS_ERR(pinctrl))
		dev_err(&phydev->mdio.bus->dev,
			"Failed to setup PHY LED pinctrl\n");

	ret = devm_phy_package_join(&phydev->mdio.dev, phydev, 0,
				    sizeof(struct airoha_socphy_shared));
	if (ret)
		return ret;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	shared = phy_package_get_priv(phydev);
	if (phydev->drv->phy_id == AIROHA_GPHY_ID_EN7528 &&
	    of_machine_is_compatible("econet,en7580"))
		shared->variant = AIROHA_9491_EN7580;
	if (phydev->mdio.addr == AIROHA_DEFAULT_PORT0_ADDR)
		shared->phydev_p0 = phydev;

	phydev->priv = priv;

	return 0;
}

static int an7581_phy_led_polarity_set(struct phy_device *phydev, int index,
				       unsigned long modes)
{
	u16 val = 0;
	u32 mode;

	if (index >= AIROHA_PHY_MAX_LEDS)
		return -EINVAL;

	for_each_set_bit(mode, &modes, __PHY_LED_MODES_NUM) {
		switch (mode) {
		case PHY_LED_ACTIVE_LOW:
			val = MTK_PHY_LED_ON_POLARITY;
			break;
		case PHY_LED_ACTIVE_HIGH:
			break;
		default:
			return -EINVAL;
		}
	}

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2, index ?
			      MTK_PHY_LED1_ON_CTRL : MTK_PHY_LED0_ON_CTRL,
			      MTK_PHY_LED_ON_POLARITY, val);
}

static int an7583_phy_config_init(struct phy_device *phydev)
{
	struct airoha_socphy_shared *shared = phy_package_get_priv(phydev);
	struct phy_device *phydev_p0;
	u8 phy_offset;
	int ret;

	/* BMCR_PDOWN is enabled by default */
	phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);

	phydev_p0 = shared->phydev_p0;
	phy_offset = phydev->mdio.addr - phydev_p0->mdio.addr;

	/* FIXME: Read SoC MDI Resister Type if available or default to 5R */
	shared->mdi_resister_type = MDI_5R;

	shared->mdi_resister_type = MDI_5R;
	if (shared->mdi_resister_type == MDI_0R)
		shared->r50_cal_tbl = an7583_zcal_to_r50ohm_0R;
	if (shared->mdi_resister_type == MDI_5R)
		shared->r50_cal_tbl = an7583_zcal_to_r50ohm_5R;
	shared->r50_cal_offset = 0;
	shared->tx_amp_compensation_tbl = &an7583_tx_amp_compensation_tbl;

	ret = airoha_phy_calib(phydev);
	if (ret)
		return ret;

	ret = airoha_phy_auto_select_transformer(phydev);
	if (ret)
		return ret;

	ret = airoha_phy_tx_amp_compensation(phydev);
	if (ret)
		return ret;

	/* Enable Idle Mode Power Setting */
	if (phy_offset == 0) {
		phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x268, 0x07F1);
		phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x703, 0x3111);
		phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x271, 0x3C24);
		phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x701, 0x1023);
	}

	if (shared->transformer_type[phy_offset] == TXMR) {
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x23, 0x0881);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x24, 0x0881);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x25, 0x0881);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x26, 0x0881);

		if (shared->mdi_resister_type == MDI_0R)
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0205);
		else
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0200);
	} else {
		if (shared->mdi_resister_type == MDI_0R) {
			/* RX setting for 5R_TXMR before AN setting */
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe7, 0x6666);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe9, 0x0003);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xfe, 0x0006);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xe6, 0x1111);
			/* 100M sharp */
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x23, 0x0c86);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x24, 0x0c86);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x25, 0x0c86);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x26, 0x0c86);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x01, 0x01cb);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x02, 0x01c2);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03, 0x0108);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x04, 0x0211);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0205);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x06, 0x0387);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x07, 0x03ce);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x08, 0x03c8);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0b, 0x0005);
		} else {
			/* 100M sharp */
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x23, 0x0886);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x24, 0x0886);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x25, 0x0886);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x26, 0x0886);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00, 0x0195);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x01, 0x01cb);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x02, 0x01c2);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x03, 0x0108);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x04, 0x0211);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x05, 0x0205);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x06, 0x0387);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x07, 0x03ce);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x08, 0x03c3);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0a, 0x0010);
			phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0b, 0x0005);
		}
	}

	/* RX Setting */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3c, 0xc000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3d, 0x0000);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x3e, 0xc000);

	/* EEE setting */
	phy_write_mmd(phydev, MDIO_MMD_AN, 0x3c, 0x0000);

	/* 10M settings */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x1a3, 0x00d2);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x1a4, 0x010e);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x27b, 0x1177);
	phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x27c, 0x0808);

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_3);
	__phy_write(phydev, 0x14, 0x190);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_1);
	__phy_write(phydev, 0x14, 0x3a18);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	phy_write(phydev, 0x9, 0x0600);

	/* EEE keep only Pair A ON */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x630, 0x006E);

	return 0;
}

static struct phy_driver mtk_socphy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_EN7512),
		.name		= "EcoNet EN7512/EN7521 GPHY",
		.config_init	= en7512_phy_config_init,
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.probe		= en7512_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_EN7528),
		.name		= "Airoha EN7528/EN7580 PHY",
		.config_init	= en7528_en7580_phy_config_init,
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.probe		= an7581_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_EN7523),
		.name		= "Airoha EN7523 PHY",
		.config_init	= en7523_phy_config_init,
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.probe		= an7581_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7581),
		.name		= "Airoha AN7581 PHY",
		.config_init	= an7581_phy_config_init,
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.probe		= an7581_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7583),
		.name		= "Airoha AN7583 PHY",
		.config_init	= an7583_phy_config_init,
		.probe		= an7581_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
};

module_phy_driver(mtk_socphy_driver);

static const struct mdio_device_id __maybe_unused airoha_socphy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_EN7512) },
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_EN7528) },
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_EN7523) },
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7581) },
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7583) },
	{ }
};

MODULE_DESCRIPTION("Airoha SoC Gigabit Ethernet PHY driver");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(mdio, airoha_socphy_tbl);
