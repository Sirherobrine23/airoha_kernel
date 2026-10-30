// SPDX-License-Identifier: GPL-2.0-only
/*
 * AN7583 asymmetric GPON PHY, based on the vendor combo_phy/an7583/pon_pma
 * implementation. The digital GEPON block uses the shared PHY API, while
 * the ANA/PMA setup and RX calibration are specific to this SerDes.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "xpon.h"

#define AN7583_ANA_OFFSET		0xf000
#define AN7583_PMA_OFFSET		0xe000
#define AN7583_SCU_WAN_CONF		0x070
#define AN7583_SCU_WAN_MASK		GENMASK(7, 0)
#define AN7583_SCU_PBUS_CTRL		0x92c
#define AN7583_SCU_PBUS_DISABLE		BIT(2)
#define AN7583_POLL_MS			1000

#define AN7583_TX_PHY_CLOCK		0xf060
#define AN7583_TX_CLOCK			0xe854
#define AN7583_TX_PLL_STATUS		0xe420
#define AN7583_PON_CLOCK		0xe450
#define AN7583_TX_FIR0			0xe778
#define AN7583_TX_FIR1			0xe780
#define AN7583_SETTING			0xe270
#define AN7583_INT0			0xe280
#define AN7583_INT1			0xe284
#define AN7583_BIST			0xe210
#define AN7583_TX_SOURCE		0xe414

struct an7583_xpon_phy {
	/* Protects calibration, LOS recovery, power transitions and TX gating. */
	struct mutex lock;
	bool configured;
	bool calibrated;
	bool parked;
	bool tx_requested;
	bool tx_calibration;
	u32 saved_bist;
	u32 saved_tx_source;
};

/*
 * GPON-only sequences from combo_phy/an7583/pon_pma/src/pon_pma.c.
 * Offsets 0xe000/0xf000 select the PMA/ANA resources. Keep every write
 * and delay in order: reset and calibration fields latch on edges.
 * AIROHA_XPON_PHY_DELAY denotes a delay in microseconds.
 */
static const struct airoha_xpon_phy_step an7583_init_seq[] = {
	/* pon_DIG_reset_hold() */
	{ 0xe460, 0x00000001, 0x00000000 },
	{ 0xe460, 0x00000040, 0x00000000 },
	{ 0xe460, 0x00000020, 0x00000000 },
	{ 0xe460, 0x00000010, 0x00000000 },
	{ 0xe460, 0x00000004, 0x00000000 },
	{ 0xe460, 0x00000002, 0x00000000 },
	{ 0xe460, 0x00000008, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 50 },
	/* pon_CfgPhyType() */
	{ 0xe854, 0x01000000, 0x01000000 },
	{ 0xe854, 0x00010000, 0x00000000 },
	{ 0xe034, 0x00000003, 0x00000001 },
	{ 0xe02c, 0x00000f00, 0x00000500 },
	{ 0xf0c8, 0x00030000, 0x00000000 },
	{ 0xf0d8, 0x01000000, 0x01000000 },
	{ 0xe004, 0xff000000, 0x01000000 },
	{ 0xe004, 0x00ff0000, 0x00100000 },
	{ 0xe004, 0x0000ff00, 0x00000a00 },
	{ 0xe004, 0x00000001, 0x00000001 },
	{ 0xe444, 0xffff0000, 0x01130000 },
	{ 0xe444, 0x0000ffff, 0x000000fa },
	{ 0xe448, 0xffff0000, 0x009b0000 },
	{ 0xe448, 0x0000ffff, 0x00000210 },
	{ 0xe440, 0xffff0000, 0x00040000 },
	{ 0xe440, 0x0000ffff, 0x00000026 },
	{ 0xe468, 0x00ff0000, 0x00020000 },
	{ 0xe028, 0x00000700, 0x00000300 },
	{ 0xe024, 0x01000000, 0x00000000 },
	/* XPON_TXPLL() */
	{ 0xe794, 0x01000000, 0x01000000 },
	{ 0xe034, 0x00000100, 0x00000000 },
	{ 0xe798, 0x7fffffff, 0x31c432ca },
	{ 0xe048, 0x7fffffff, 0x63886594 },
	{ 0xe04c, 0x7fffffff, 0x63886594 },
	/* XPON_TX() */
	{ 0xf0c4, 0x01000000, 0x01000000 },
	{ 0xf0c4, 0x00000001, 0x00000001 },
	{ 0xf000, 0x00000001, 0x00000001 },
	{ 0xf000, 0x00070000, 0x00040000 },
	{ 0xf000, 0x00003f00, 0x00000100 },
	{ 0xf004, 0x00000100, 0x00000000 },
	{ 0xe874, 0x01000000, 0x01000000 },
	{ 0xe874, 0x00010000, 0x00010000 },
	{ 0xe77c, 0x01000000, 0x01000000 },
	{ 0xe77c, 0x000f0000, 0x00020000 },
	{ 0xe784, 0x00000100, 0x00000100 },
	{ 0xe784, 0x00000003, 0x00000000 },
	{ 0xe580, 0x00000003, 0x00000000 },
	{ 0xe468, 0x0000007f, 0x00000004 },
	/* pon_CfgPhyType() */
	{ 0xe08c, 0x00ffff00, 0x00000100 },
	{ 0xe104, 0x0000ffff, 0x00000002 },
	{ 0xe08c, 0x00000007, 0x00000001 },
	{ 0xe090, 0x0000ffff, 0x00000002 },
	{ 0xe090, 0xffff0000, 0x03e80000 },
	{ 0xe09c, 0x0000ffff, 0x00000002 },
	{ 0xe09c, 0xffff0000, 0x03e80000 },
	{ 0xe094, 0x0000ffff, 0x00000002 },
	{ 0xe094, 0xffff0000, 0x03e80000 },
	{ 0xe098, 0x0000ffff, 0x00000002 },
	{ 0xe098, 0xffff0000, 0x03e80000 },
	{ 0xe120, 0x00001f00, 0x00000100 },
	{ 0xe120, 0x00000003, 0x00000003 },
	{ 0xe068, 0x07ff0000, 0x02400000 },
	{ 0xe068, 0x000007ff, 0x000001c0 },
	{ 0xe088, 0x00000100, 0x00000000 },
	/* XPON_RX() */
	{ 0xf0d4, 0x000c0000, 0x00000000 },
	{ 0xf0d4, 0x00700000, 0x00400000 },
	{ 0xf0d4, 0x07000000, 0x04000000 },
	{ 0xf120, 0x0003ff00, 0x0003ff00 },
	{ 0xf0dc, 0x000001ff, 0x00000000 },
	{ 0xf0dc, 0x00000001, 0x00000000 },
	{ 0xe814, 0x01000000, 0x01000000 },
	{ 0xe814, 0x00010000, 0x00010000 },
	{ 0xf0d8, 0x00000300, 0x00000100 },
	{ 0xf0f8, 0x0000007f, 0x00000008 },
	{ 0xf0d4, 0x0000ffff, 0x000018b0 },
	{ 0xf10c, 0x00000004, 0x00000000 },
	{ 0xe294, 0x00000003, 0x00000003 },
	{ 0xe300, 0x00000001, 0x00000000 },
	{ 0xf0cc, 0x01000000, 0x00000000 },
	{ 0xf0cc, 0x00010000, 0x00000000 },
	{ 0xf0d8, 0x000000ff, 0x00000000 },
	{ 0xf0d8, 0x00010000, 0x00000000 },
	{ 0xf0e8, 0x00000003, 0x00000002 },
	{ 0xf0cc, 0x00000100, 0x00000100 },
	{ 0xf0cc, 0x00000001, 0x00000001 },
	{ 0xe374, 0x00000003, 0x00000002 },
	{ 0xe76c, 0x01000000, 0x01000000 },
	{ 0xe76c, 0x00030000, 0x00020000 },
	/* XPON_ANA() */
	{ 0xf0a0, 0x01000000, 0x01000000 },
	{ 0xf084, 0x03000000, 0x01000000 },
	{ 0xf084, 0x00030000, 0x00010000 },
	{ 0xf088, 0x03000000, 0x00000000 },
	{ 0xf088, 0x00010000, 0x00000000 },
	{ 0xf088, 0x00000700, 0x00000000 },
	{ 0xf088, 0x00000001, 0x00000001 },
	{ 0xf0a8, 0x00030000, 0x00010000 },
	{ 0xf0a8, 0x00000100, 0x00000100 },
	{ 0xf064, 0x01000000, 0x01000000 },
	{ 0xf064, 0x00070000, 0x00040000 },
	{ 0xf064, 0x00000001, 0x00000000 },
	{ 0xf064, 0x00000300, 0x00000000 },
	{ 0xf068, 0x03000000, 0x00000000 },
	{ 0xf068, 0x00010000, 0x00010000 },
	{ 0xf068, 0x00000300, 0x00000100 },
	{ 0xf068, 0x00000001, 0x00000000 },
	{ 0xf06c, 0x00010000, 0x00010000 },
	{ 0xf06c, 0x00000100, 0x00000000 },
	{ 0xf06c, 0x00000003, 0x00000003 },
	{ 0xf080, 0xffff0000, 0x00000000 },
	{ 0xf080, 0x0000ffff, 0x00000000 },
	{ 0xf07c, 0x00010000, 0x00000000 },
	{ 0xf07c, 0x00000100, 0x00000000 },
	{ 0xf07c, 0x00000001, 0x00000000 },
	{ 0xf084, 0x0000ffff, 0x00000000 },
	{ 0xf090, 0x01000000, 0x00000000 },
	{ 0xf050, 0x1f000000, 0x1f000000 },
	{ 0xf050, 0x001f0000, 0x00050000 },
	{ 0xf050, 0x00003f00, 0x00000000 },
	{ 0xf050, 0x0000003f, 0x00000031 },
	{ 0xf054, 0x001f0000, 0x00180000 },
	{ 0xf054, 0x00001f00, 0x00000b00 },
	{ 0xf054, 0x0000001f, 0x00000002 },
	{ 0xf074, 0x03000000, 0x03000000 },
	{ 0xf078, 0x00000001, 0x00000001 },
	{ 0xf078, 0x00000700, 0x00000700 },
	{ 0xf078, 0x07000000, 0x04000000 },
	{ 0xf078, 0x38000000, 0x00000000 },
	{ 0xf078, 0x00070000, 0x00040000 },
	{ 0xf058, 0x03000000, 0x03000000 },
	{ 0xf058, 0x00030000, 0x00000000 },
	{ 0xf058, 0x00000700, 0x00000200 },
	{ 0xf05c, 0x00000003, 0x00000001 },
	{ 0xf058, 0x000000ff, 0x000000e4 },
	{ 0xf054, 0x01000000, 0x00000000 },
	{ 0xf094, 0x01000000, 0x00000000 },
	{ 0xe858, 0x00000100, 0x00000100 },
	{ 0xe858, 0x00000001, 0x00000000 },
	{ 0xf05c, 0x00030000, 0x00000000 },
	{ 0xf05c, 0x00000100, 0x00000100 },
	{ 0xf074, 0x00030000, 0x00000000 },
	{ 0xf094, 0x0000001f, 0x0000000f },
	{ 0xf070, 0x00000007, 0x00000003 },
	{ 0xf070, 0x00001f00, 0x00000b00 },
	{ 0xf074, 0x00000700, 0x00000000 },
	{ 0xf074, 0x00000001, 0x00000001 },
	{ 0xf06c, 0x01000000, 0x01000000 },
	{ 0xf0c0, 0x00070000, 0x00020000 },
	{ 0xf0c0, 0x07000000, 0x02000000 },
	{ 0xe778, 0x01000000, 0x01000000 },
	{ 0xe778, 0x00000100, 0x00000100 },
	{ 0xe780, 0x00000100, 0x00000100 },
	{ 0xe780, 0x01000000, 0x01000000 },
	{ 0xe778, 0x001f0000, 0x00000000 },
	{ 0xe778, 0x0000003f, 0x00000000 },
	{ 0xe780, 0x0000001f, 0x00000000 },
	{ 0xe780, 0x00070000, 0x00000000 },
	{ 0xf118, 0x01000000, 0x01000000 },
	{ 0xf118, 0x00010000, 0x00010000 },
	{ 0xf118, 0x00000100, 0x00000100 },
	{ 0xf118, 0x00000001, 0x00000000 },
	{ 0xf11c, 0x00000001, 0x00000001 },
	{ 0xf0e8, 0x07ffff00, 0x00800000 },
	{ 0xf0e0, 0x0007ffff, 0x00078000 },
	{ 0xf100, 0x00010000, 0x00000000 },
	{ 0xf110, 0x00000300, 0x00000000 },
	/* pon_CfgPhyType() */
	{ 0xe088, 0x00000001, 0x00000001 },
	{ 0xe074, 0x00010000, 0x00010000 },
	{ 0xe078, 0x0000ff00, 0x00000400 },
	{ 0xe078, 0x000000ff, 0x00000004 },
	{ 0xe07c, 0x00ff0000, 0x00040000 },
	{ 0xe07c, 0x000007ff, 0x00000004 },
	{ 0xe080, 0xff000000, 0xff000000 },
	{ 0xe080, 0x000003ff, 0x000000d0 },
	{ 0xe118, 0x000000ff, 0x00000001 },
	{ 0xe118, 0xff000000, 0x00000000 },
	{ 0xe118, 0x0007ff00, 0x00000000 },
	{ 0xe11c, 0x00000001, 0x00000001 },
	{ 0xe160, 0x03000000, 0x01000000 },
	{ 0xe160, 0x0000ff00, 0x00002e00 },
	{ 0xe160, 0x00000001, 0x00000001 },
	{ 0xe160, 0x00030000, 0x00010000 },
	{ 0xe164, 0x00030000, 0x00000000 },
	{ 0xe164, 0x00000f00, 0x00000000 },
	{ 0xe164, 0x00000001, 0x00000000 },
	{ 0xe100, 0x0000ffff, 0x00000005 },
	{ 0xe100, 0xffff0000, 0x000a0000 },
	{ 0xe144, 0x000000ff, 0x00000000 },
	{ 0xe144, 0x00000100, 0x00000000 },
	{ 0xe06c, 0x0000007f, 0x00000040 },
	{ 0xe06c, 0x00007f00, 0x00003f00 },
	{ 0xe06c, 0x00010000, 0x00010000 },
	{ 0xe070, 0x000fffff, 0x00000018 },
	{ 0xe48c, 0x0000ff00, 0x00000200 },
	{ 0xe48c, 0x00000002, 0x00000002 },
	{ 0xe48c, 0x00000001, 0x00000000 },
	{ 0xe170, 0x0007ff00, 0x0000a500 },
	{ 0xe170, 0x00000007, 0x00000002 },
	{ 0xe170, 0x03000000, 0x00000000 },
	{ 0xe174, 0x07000000, 0x04000000 },
	{ 0xe174, 0x00070000, 0x00010000 },
	{ 0xe184, 0x000007ff, 0x000001ff },
	{ 0xe184, 0x07ff0000, 0x04000000 },
	{ 0xe178, 0x00000700, 0x00000400 },
	{ 0xe178, 0x00000007, 0x00000003 },
	{ 0xe320, 0x01000000, 0x00000000 },
	{ 0xe200, 0x00000001, 0x00000001 },
	{ 0xe200, 0x00000100, 0x00000000 },
	{ 0xe148, 0x00000001, 0x00000001 },
	{ 0xe148, 0x00000f00, 0x00000a00 },
	{ 0xe148, 0x007f0000, 0x00700000 },
	{ 0xe148, 0x3f800000, 0x08000000 },
	/* XPON_TXPLL_on() */
	{ 0xe854, 0x01000000, 0x01000000 },
	{ 0xe854, 0x00010000, 0x00010000 },
	{ 0xe000, 0x01000000, 0x01000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 6 },
	{ 0xe854, 0x00000100, 0x00000100 },
	{ 0xe854, 0x00000001, 0x00000001 },
	{ 0xf094, 0x00010000, 0x00010000 },
	{ 0xf094, 0x00000100, 0x00000000 },
	{ 0xf060, 0x00000100, 0x00000100 },
	{ 0xf060, 0x00000001, 0x00000001 },
	{ 0xf094, 0x00010000, 0x00000000 },
	{ 0xf04c, 0x01000000, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 500 },
	/* XPON_TX_on() */
	{ 0xe260, 0x00000001, 0x00000001 },
	{ 0xe410, 0x00000100, 0x00000100 },
	{ 0xe410, 0x00000001, 0x00000001 },
	{ 0xe260, 0x00000100, 0x00000100 },
	{ 0xe260, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
};

static const struct airoha_xpon_phy_step an7583_rx_start_seq[] = {
	/* XPON_RX_preset() */
	{ 0xf114, 0x00000300, 0x00000200 },
	{ 0xf114, 0x001f0000, 0x00020000 },
	{ 0xf110, 0x03000000, 0x03000000 },
	{ 0xf10c, 0x00080000, 0x00000000 },
	{ 0xf10c, 0x00070000, 0x00070000 },
	{ 0xe114, 0x00000100, 0x00000000 },
	{ 0xe10c, 0x00000100, 0x00000000 },
	{ 0xe818, 0x00000001, 0x00000000 },
	{ 0xe818, 0x00000100, 0x00000100 },
	{ 0xe768, 0x01000000, 0x01000000 },
	{ 0xe768, 0x000f0000, 0x00000000 },
	{ 0xe88c, 0x00000100, 0x00000100 },
	{ 0xe88c, 0x00000003, 0x00000001 },
	{ 0xe330, 0x00000100, 0x00000100 },
	{ 0xe33c, 0x00000100, 0x00000000 },
	{ 0xe330, 0x00010000, 0x00000000 },
	{ 0xe33c, 0x00010000, 0x00000000 },
	{ 0xe338, 0x00000001, 0x00000000 },
	{ 0xe32c, 0x00000001, 0x00000000 },
	{ 0xe10c, 0x00010000, 0x00010000 },
	{ 0xe114, 0x00010000, 0x00010000 },
	/* XPON_TDC_off() */
	{ 0xe034, 0x00000100, 0x00000100 },
	{ 0xe02c, 0x00000001, 0x00000000 },
	{ 0xe010, 0x00000001, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 1000 },
	/* XPON_RX_on() */
	{ 0xe824, 0x01000000, 0x01000000 },
	{ 0xe824, 0x00010000, 0x00010000 },
	{ 0xe824, 0x00000100, 0x00000100 },
	{ 0xe824, 0x00000001, 0x00000001 },
	{ 0xe81c, 0x01000000, 0x00000000 },
	{ 0xe81c, 0x00010000, 0x00000000 },
	{ 0xe81c, 0x00000100, 0x00000100 },
	{ 0xe81c, 0x00000001, 0x00000001 },
	{ 0xe894, 0x00000100, 0x00000100 },
	{ 0xe894, 0x00000001, 0x00000001 },
	{ 0xe84c, 0x01000000, 0x01000000 },
	{ 0xe84c, 0x00010000, 0x00010000 },
	{ 0xe84c, 0x00000100, 0x00000000 },
	{ 0xe84c, 0x00000001, 0x00000000 },
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
	{ 0xe34c, 0x01000000, 0x01000000 },
	{ 0xe34c, 0x00010000, 0x00010000 },
	{ 0xe34c, 0x00000100, 0x00000100 },
	{ 0xe34c, 0x00000001, 0x00000001 },
	{ 0xe350, 0x00000001, 0x00000001 },
	{ 0xe38c, 0x00000003, 0x00000001 },
	{ 0xf0fc, 0x03000000, 0x00000000 },
	{ 0xf0fc, 0x001f0000, 0x00080000 },
	{ 0xf0fc, 0x00000700, 0x00000500 },
	{ 0xf0fc, 0x00000007, 0x00000005 },
	{ 0xe108, 0x00000100, 0x00000000 },
	{ 0xe108, 0x00010000, 0x00000000 },
	{ 0xe108, 0x01000000, 0x00000000 },
	{ 0xe10c, 0x00000001, 0x00000000 },
	{ 0xe108, 0x00000001, 0x00000000 },
	{ 0xe10c, 0x00010000, 0x00000000 },
	{ 0xf100, 0x00000003, 0x00000000 },
	{ 0xf108, 0x03000000, 0x00000000 },
	{ 0xe460, 0x00000002, 0x00000002 },
	{ 0xe460, 0x00000020, 0x00000020 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe818, 0x00010000, 0x00010000 },
	/* XPON_RX_L2R() */
	{ 0xe818, 0x00000001, 0x00000000 },
	{ 0xe818, 0x00000100, 0x00000100 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
	/* pon_DIG_ref_release() */
	{ 0xe460, 0x00000020, 0x00000020 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
};

static const struct airoha_xpon_phy_step an7583_rx_cal_seq[] = {
	/* XPON_RX_OSCal() */
	{ 0xe33c, 0x00000001, 0x00000000 },
	{ 0xe330, 0x00000001, 0x00000001 },
	{ 0xe83c, 0x01000000, 0x01000000 },
	{ 0xe83c, 0x00010000, 0x00010000 },
	{ 0xe840, 0x01000000, 0x01000000 },
	{ 0xe840, 0x00010000, 0x00010000 },
	{ 0xe840, 0x00000100, 0x00000100 },
	{ 0xe840, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	{ 0xe108, 0x00000001, 0x00000000 },
	{ 0xe10c, 0x00000100, 0x00000000 },
	{ 0xe110, 0x00000001, 0x00000000 },
	{ 0xe114, 0x00000100, 0x00000000 },
	{ 0xe110, 0x00000001, 0x00000001 },
	/* XPON_RX_pical() */
	{ 0xe308, 0x01000000, 0x01000000 },
	{ 0xe15c, 0x00000700, 0x00000400 },
	{ 0xe118, 0x000000ff, 0x00000008 },
	{ 0xe204, 0x00010000, 0x00000000 },
	{ 0xe328, 0x00000100, 0x00000000 },
	{ 0xe334, 0x00000100, 0x00000000 },
	{ 0xe328, 0x00000001, 0x00000000 },
	{ 0xe334, 0x00000001, 0x00000000 },
	{ 0xe31c, 0x00000001, 0x00000000 },
	{ 0xe318, 0x00000100, 0x00000000 },
	{ 0xe324, 0x01000000, 0x00000000 },
	{ 0xe110, 0x00000100, 0x00000000 },
	{ 0xe108, 0x00000100, 0x00000000 },
	{ 0xe30c, 0x00000001, 0x00000000 },
	{ 0xe204, 0x00010000, 0x00010000 },
	{ 0xe328, 0x00000100, 0x00000100 },
	{ 0xe328, 0x00000001, 0x00000001 },
	{ 0xe318, 0x00000100, 0x00000100 },
	{ 0xe110, 0x00000100, 0x00000100 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	{ 0xe110, 0x00000100, 0x00000000 },
	{ 0xe318, 0x00000100, 0x00000000 },
	{ 0xe30c, 0x00000001, 0x00000001 },
	/* XPON_RX_pdos() */
	{ 0xe894, 0x01000000, 0x01000000 },
	{ 0xe894, 0x00010000, 0x00010000 },
	{ 0xe114, 0x00000100, 0x00000000 },
	{ 0xe10c, 0x00000100, 0x00000000 },
	{ 0xe304, 0x00000100, 0x00000100 },
	{ 0xe304, 0x00010000, 0x00010000 },
	{ 0xe304, 0x01000000, 0x01000000 },
	{ 0xe308, 0x00000001, 0x00000001 },
	{ 0xe308, 0x00000100, 0x00000100 },
	{ 0xe32c, 0x00000001, 0x00000000 },
	{ 0xe338, 0x00000001, 0x00000000 },
	{ 0xe084, 0x01000000, 0x00000000 },
	{ 0xe084, 0x00000100, 0x00000000 },
	{ 0xe32c, 0x01000000, 0x00000000 },
	{ 0xe338, 0x01000000, 0x00000000 },
	{ 0xe084, 0x00010000, 0x00000000 },
	{ 0xe084, 0x00000001, 0x00000000 },
	{ 0xe200, 0x00070000, 0x00020000 },
	{ 0xe328, 0x00010000, 0x00000000 },
	{ 0xe334, 0x00010000, 0x00000000 },
	{ 0xe208, 0x00000001, 0x00000000 },
	{ 0xe110, 0x00010000, 0x00000000 },
	{ 0xe108, 0x00010000, 0x00000000 },
	{ 0xe110, 0x00000001, 0x00000000 },
	{ 0xe108, 0x00000001, 0x00000000 },
	{ 0xe328, 0x00010000, 0x00010000 },
	{ 0xe208, 0x00000001, 0x00000001 },
	{ 0xe110, 0x00010000, 0x00010000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	{ 0xe110, 0x00010000, 0x00000000 },
	{ 0xe110, 0x00000001, 0x00000000 },
	{ 0xe084, 0x01000000, 0x00000000 },
	{ 0xe084, 0x00000100, 0x00000100 },
	{ 0xe32c, 0x01000000, 0x00000000 },
	{ 0xe338, 0x01000000, 0x01000000 },
	{ 0xe084, 0x00010000, 0x00000000 },
	{ 0xe084, 0x00000001, 0x00000001 },
	{ 0xe894, 0x01000000, 0x01000000 },
	{ 0xe894, 0x00010000, 0x00000000 },
	/* XPON_RX_feos() */
	{ 0xe114, 0x00000100, 0x00000000 },
	{ 0xe10c, 0x00000100, 0x00000000 },
	{ 0xe308, 0x00010000, 0x00010000 },
	{ 0xe32c, 0x00000001, 0x00000000 },
	{ 0xe338, 0x00000001, 0x00000000 },
	{ 0xe144, 0x000000ff, 0x00000030 },
	{ 0xe32c, 0x00010000, 0x00000000 },
	{ 0xe338, 0x00010000, 0x00000000 },
	{ 0xe204, 0x00000100, 0x00000000 },
	{ 0xe110, 0x01000000, 0x00000000 },
	{ 0xe108, 0x01000000, 0x00000000 },
	{ 0xe110, 0x00000001, 0x00000001 },
	{ 0xe108, 0x00000001, 0x00000000 },
	{ 0xe32c, 0x00010000, 0x00010000 },
	{ 0xe204, 0x00000100, 0x00000100 },
	{ 0xe110, 0x01000000, 0x01000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	{ 0xe110, 0x01000000, 0x00000000 },
	{ 0xe110, 0x00000001, 0x00000000 },
	/* XPON_RX_sdcal() */
	{ 0xe898, 0x00000100, 0x00000100 },
	{ 0xe898, 0x00000001, 0x00000000 },
	{ 0xe840, 0x00000100, 0x00000100 },
	{ 0xe840, 0x00000001, 0x00000001 },
	{ 0xe204, 0x01000000, 0x00000000 },
	{ 0xe32c, 0x00000100, 0x00000000 },
	{ 0xe10c, 0x00000001, 0x00000000 },
	{ 0xe338, 0x00000100, 0x00000000 },
	{ 0xe114, 0x00000001, 0x00000000 },
	{ 0xe204, 0x01000000, 0x01000000 },
	{ 0xe32c, 0x00000100, 0x00000100 },
	{ 0xe114, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	{ 0xe114, 0x00000001, 0x00000000 },
	{ 0xe898, 0x00000100, 0x00000100 },
	{ 0xe898, 0x00000001, 0x00000000 },
	{ 0xe83c, 0x01000000, 0x01000000 },
	{ 0xe83c, 0x00010000, 0x00000000 },
	{ 0xe840, 0x01000000, 0x01000000 },
	{ 0xe840, 0x00010000, 0x00000000 },
	{ 0xe840, 0x00000100, 0x00000100 },
	{ 0xe840, 0x00000001, 0x00000000 },
};

static const struct airoha_xpon_phy_step an7583_rx_finish_seq[] = {
	/* XPON_phy_status() */
	{ 0xe114, 0x00000100, 0x00000100 },
	{ 0xe10c, 0x00000100, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 1 },
	/* XPON_DIG_reset() */
	{ 0xe460, 0x00000001, 0x00000000 },
	{ 0xe460, 0x00000040, 0x00000000 },
	{ 0xe460, 0x00000020, 0x00000000 },
	{ 0xe460, 0x00000010, 0x00000000 },
	{ 0xe460, 0x00000004, 0x00000000 },
	{ 0xe460, 0x00000002, 0x00000000 },
	{ 0xe460, 0x00000008, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 10 },
	{ 0xe460, 0x00000020, 0x00000020 },
	{ 0xe460, 0x00000010, 0x00000010 },
	{ 0xe460, 0x00000008, 0x00000008 },
	{ 0xe460, 0x00000001, 0x00000001 },
	{ 0xe460, 0x00000040, 0x00000040 },
	{ 0xe460, 0x00000004, 0x00000004 },
	{ 0xe460, 0x00000002, 0x00000002 },
	/* XPON_RX_L2D() */
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
	{ 0xe818, 0x00000001, 0x00000001 },
	{ 0xe818, 0x00000100, 0x00000100 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	/* XPON_TDC_on() */
	{ 0xe040, 0x00000300, 0x00000000 },
	{ 0xe028, 0x00000003, 0x00000001 },
	{ 0xe864, 0x00000100, 0x00000000 },
	{ 0xe794, 0x01000000, 0x00000000 },
	{ 0xe024, 0x00010000, 0x00010000 },
	{ 0xe034, 0x00000100, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe03c, 0x00010000, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe02c, 0x00000001, 0x00000001 },
	{ 0xe010, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 500 },
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
	/* XPON_RX_rxrdy() */
	{ 0xe114, 0x01000000, 0x01000000 },
	{ 0xe10c, 0x01000000, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 10 },
	{ 0xe460, 0x00000001, 0x00000000 },
	{ 0xe460, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	/* XPON_BIST_setting() */
	{ 0xe214, 0xffffffff, 0x8ff1fd53 },
	{ 0xe224, 0xffffffff, 0x0ff1fd53 },
	{ 0xe230, 0x0000ffff, 0x00000001 },
	{ 0xe210, 0x0000001f, 0x00000005 },
	{ 0xe220, 0x00010000, 0x00010000 },
	{ 0xe344, 0x00000001, 0x00000000 },
	{ 0xe344, 0x01000000, 0x00000000 },
	/* XPON_R2T_sel() */
	{ 0xe414, 0x00010000, 0x00000000 },
	{ 0xe414, 0x00000100, 0x00000000 },
};

static const struct airoha_xpon_phy_step an7583_rx_disconnect_seq[] = {
	/* XPON_TDC_off() */
	{ 0xe034, 0x00000100, 0x00000100 },
	{ 0xe02c, 0x00000001, 0x00000000 },
	{ 0xe010, 0x00000001, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 1000 },
	/* XPON_RX_L2R() */
	{ 0xe818, 0x00000001, 0x00000000 },
	{ 0xe818, 0x00000100, 0x00000100 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
};

static const struct airoha_xpon_phy_step an7583_rx_connect_seq[] = {
	/* pon_DIG_reset_hold() */
	{ 0xe460, 0x00000001, 0x00000000 },
	{ 0xe460, 0x00000040, 0x00000000 },
	{ 0xe460, 0x00000020, 0x00000000 },
	{ 0xe460, 0x00000010, 0x00000000 },
	{ 0xe460, 0x00000004, 0x00000000 },
	{ 0xe460, 0x00000002, 0x00000000 },
	{ 0xe460, 0x00000008, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 50 },
	/* XPON_RX_L2D() */
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
	{ 0xe818, 0x00000001, 0x00000001 },
	{ 0xe818, 0x00000100, 0x00000100 },
	{ AIROHA_XPON_PHY_DELAY, 0, 200 },
	/* XPON_TDC_on() */
	{ 0xe040, 0x00000300, 0x00000000 },
	{ 0xe028, 0x00000003, 0x00000001 },
	{ 0xe864, 0x00000100, 0x00000000 },
	{ 0xe794, 0x01000000, 0x00000000 },
	{ 0xe024, 0x00010000, 0x00010000 },
	{ 0xe034, 0x00000100, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe03c, 0x00010000, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	{ 0xe02c, 0x00000001, 0x00000001 },
	{ 0xe010, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 500 },
	{ 0xe818, 0x01000000, 0x01000000 },
	{ 0xe818, 0x00010000, 0x00000000 },
	{ 0xe818, 0x00010000, 0x00010000 },
	/* pon_DIG_reset_release() */
	{ 0xe460, 0x00000020, 0x00000020 },
	{ 0xe460, 0x00000004, 0x00000004 },
	{ 0xe460, 0x00000002, 0x00000002 },
	{ 0xe460, 0x00000008, 0x00000008 },
	{ 0xe460, 0x00000001, 0x00000001 },
	{ 0xe460, 0x00000040, 0x00000040 },
	{ 0xe460, 0x00000010, 0x00000010 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	/* XPON_RX_rxrdy() */
	{ 0xe114, 0x01000000, 0x01000000 },
	{ 0xe10c, 0x01000000, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 10 },
	{ 0xe460, 0x00000001, 0x00000000 },
	{ 0xe460, 0x00000001, 0x00000001 },
	{ AIROHA_XPON_PHY_DELAY, 0, 100 },
	/* XPON_phy_status() */
	{ 0xe114, 0x00000100, 0x00000100 },
	{ 0xe10c, 0x00000100, 0x00000000 },
	{ AIROHA_XPON_PHY_DELAY, 0, 1 },
	/* XPON_R2T_sel() */
	{ 0xe414, 0x00010000, 0x00000000 },
	{ 0xe414, 0x00000100, 0x00000000 },
};

#define an7583_run(priv, name) \
	airoha_xpon_phy_run(priv, an7583_##name##_seq, \
			    ARRAY_SIZE(an7583_##name##_seq))

static bool an7583_los(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, XPON_TRANS_STATUS) &
		XPON_TRANS_STATUS_LOS;
}

static bool an7583_ready(struct airoha_xpon_phy *priv)
{
	struct an7583_xpon_phy *an = priv->soc_priv;

	return an->calibrated && !an->parked && !an7583_los(priv) &&
		FIELD_GET(XPON_PHYSTA1_STATE_MASK,
			  airoha_xpon_phy_read(priv, XPON_PHYSTA1)) ==
		XPON_PHYSTA1_READY;
}

static void an7583_update_tx(struct airoha_xpon_phy *priv)
{
	struct an7583_xpon_phy *an = priv->soc_priv;

	airoha_xpon_phy_set_tx_gpio(priv, priv->powered &&
				    an->tx_requested && an7583_ready(priv));
}

static int an7583_get_link_state(struct airoha_xpon_phy *priv,
				 bool *ready, bool *los)
{
	struct an7583_xpon_phy *an = priv->soc_priv;

	mutex_lock(&an->lock);
	*los = !priv->powered || an7583_los(priv);
	*ready = priv->powered && an7583_ready(priv);
	mutex_unlock(&an->lock);
	return 0;
}

static int an7583_set_tx_enable(struct airoha_xpon_phy *priv, bool enable)
{
	struct an7583_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	mutex_lock(&an->lock);
	if (enable && !priv->powered) {
		ret = -EHOSTDOWN;
		goto out;
	}
	an->tx_requested = enable;
	an7583_update_tx(priv);
out:
	mutex_unlock(&an->lock);
	return ret;
}

static void an7583_mask_interrupts(struct airoha_xpon_phy *priv)
{
	/* Link events are polled; never enable an unhandled PHY interrupt. */
	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, AN7583_INT0, 0);
	airoha_xpon_phy_write(priv, AN7583_INT1, 0);
}

static void an7583_park_rx(struct airoha_xpon_phy *priv)
{
	struct an7583_xpon_phy *an = priv->soc_priv;

	airoha_xpon_phy_set_tx_gpio(priv, false);
	an7583_run(priv, rx_disconnect);
	an->parked = true;
}

static void an7583_tx_calibration_pattern(struct airoha_xpon_phy *priv)
{
	/* PRBS23 uses the normal MAC burst slots. */
	airoha_xpon_phy_rmw(priv, AN7583_BIST, GENMASK(4, 0) | BIT(8),
			    4 | BIT(8));
	airoha_xpon_phy_rmw(priv, AN7583_TX_SOURCE, BIT(16) | BIT(8), BIT(16));
}

static void an7583_connect(struct airoha_xpon_phy *priv)
{
	struct an7583_xpon_phy *an = priv->soc_priv;

	if (an->calibrated) {
		an7583_run(priv, rx_connect);
	} else {
		an7583_run(priv, rx_start);
		/* AN7583 always runs OSCal, PICAL, PDOS, FEOS and SDCal. */
		an7583_run(priv, rx_cal);
		/* The vendor EO scan only applies to 10G receive modes. */
		an7583_run(priv, rx_finish);
		an->calibrated = true;
	}
	/* Reconnection restores normal TX routing in the PMA. */
	if (an->tx_calibration)
		an7583_tx_calibration_pattern(priv);
	an->parked = false;
}

static void an7583_ready_work(struct work_struct *work)
{
	struct airoha_xpon_phy *priv = container_of(to_delayed_work(work),
					struct airoha_xpon_phy, ready_work);
	struct an7583_xpon_phy *an = priv->soc_priv;
	bool ready;

	mutex_lock(&an->lock);
	if (!priv->powered)
		goto out;
	if (an7583_los(priv)) {
		if (!an->parked)
			an7583_park_rx(priv);
	} else if (an->parked || !an7583_ready(priv)) {
		/* Also recover LOF without a transition on the optical LOS pin. */
		if (!an->parked)
			an7583_park_rx(priv);
		an7583_connect(priv);
	}
	ready = an7583_ready(priv);
	if (ready != priv->ready_reported) {
		priv->ready_reported = ready;
		dev_info(priv->dev, "GPON PHY %s (LOS=%u)\n",
			 ready ? "ready" : "unsynchronized", an7583_los(priv));
	}
	an7583_update_tx(priv);
	mod_delayed_work(system_wq, &priv->ready_work,
			 msecs_to_jiffies(AN7583_POLL_MS));
out:
	mutex_unlock(&an->lock);
}

static int an7583_power_off(struct airoha_xpon_phy *priv)
{
	struct an7583_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	mutex_lock(&an->lock);
	WRITE_ONCE(priv->powered, false);
	an->tx_requested = false;
	airoha_xpon_phy_set_tx_gpio(priv, false);
	mutex_unlock(&an->lock);
	cancel_delayed_work_sync(&priv->ready_work);

	mutex_lock(&an->lock);
	if (an->configured) {
		an7583_mask_interrupts(priv);
		airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY,
				    XPON_PHYFWREADY_READY, 0);
		airoha_xpon_phy_write(priv, AN7583_PON_CLOCK, 0);
		ret = reset_control_assert(priv->reset);
	}
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	an->configured = false;
	an->calibrated = false;
	an->parked = true;
	an->tx_calibration = false;
	priv->ready_reported = false;
	mutex_unlock(&an->lock);
	return ret;
}

static void an7583_shutdown(struct airoha_xpon_phy *priv)
{
	int ret = an7583_power_off(priv);

	if (ret)
		dev_warn(priv->dev, "failed to assert PHY reset: %d\n", ret);
}

static void an7583_cleanup(void *data)
{
	an7583_shutdown(data);
}

static int an7583_init(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7583_xpon_phy *an = priv->soc_priv;

	mutex_lock(&an->lock);
	priv->initialized = true;
	mutex_unlock(&an->lock);
	return 0;
}

static int an7583_exit(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7583_xpon_phy *an = priv->soc_priv;
	int ret;

	ret = an7583_power_off(priv);
	mutex_lock(&an->lock);
	priv->initialized = false;
	mutex_unlock(&an->lock);
	return ret;
}

static int an7583_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7583_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	if (mode != PHY_MODE_ETHERNET || submode != AIROHA_XPON_PHY_SUBMODE_GPON)
		return -EOPNOTSUPP;
	mutex_lock(&an->lock);
	if (priv->powered)
		ret = -EBUSY;
	else
		priv->submode = submode;
	mutex_unlock(&an->lock);
	return ret;
}

static int an7583_power_on(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7583_xpon_phy *an = priv->soc_priv;
	u32 value;
	int ret;

	mutex_lock(&an->lock);
	if (!priv->initialized) {
		ret = -EINVAL;
		goto out;
	}
	if (priv->powered) {
		ret = 0;
		goto out;
	}
	airoha_xpon_phy_set_tx_gpio(priv, false);
	an->tx_requested = false;
	airoha_xpon_phy_set_vcc_enabled(priv, true);
	ret = regmap_clear_bits(priv->scu, AN7583_SCU_PBUS_CTRL,
				AN7583_SCU_PBUS_DISABLE);
	if (ret)
		goto err_power;

	/* Gate both PHY clock paths before pulsing the top reset. */
	airoha_xpon_phy_write(priv, AN7583_PON_CLOCK, 0);
	airoha_xpon_phy_rmw(priv, AN7583_TX_PHY_CLOCK, BIT(8), 0);
	airoha_xpon_phy_rmw(priv, AN7583_TX_CLOCK, BIT(24) | BIT(16), BIT(24));
	ret = regmap_update_bits(priv->scu, AN7583_SCU_WAN_CONF,
				 AN7583_SCU_WAN_MASK, 0x11);
	if (ret)
		goto err_power;
	ret = reset_control_assert(priv->reset);
	if (ret)
		goto err_power;
	udelay(1);
	ret = reset_control_deassert(priv->reset);
	if (ret)
		goto err_power;
	an->configured = true;
	udelay(1);
	ret = regmap_update_bits(priv->scu, AN7583_SCU_WAN_CONF,
				 AN7583_SCU_WAN_MASK, 0);
	if (ret)
		goto err_power;
	ret = regmap_clear_bits(priv->scu, AN7583_SCU_PBUS_CTRL,
				AN7583_SCU_PBUS_DISABLE);
	if (ret)
		goto err_power;

	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY, 0);
	an7583_mask_interrupts(priv);
	an7583_run(priv, init);
	an7583_mask_interrupts(priv);
	ret = read_poll_timeout(airoha_xpon_phy_read, value, value & BIT(16),
				100, 100000, false, priv, AN7583_TX_PLL_STATUS);
	if (ret) {
		dev_err(priv->dev, "GPON TX PLL lock timeout: %#08x\n", value);
		goto err_power;
	}
	airoha_xpon_phy_rmw(priv, XPON_PHYSET10, XPON_PHYSET10_GPON,
			    XPON_PHYSET10_GPON);
	airoha_xpon_phy_rmw(priv, XPON_SETTING, XPON_SETTING_INV_MASK,
			    priv->trans_invert);
	airoha_xpon_phy_rmw(priv, AN7583_SETTING, BIT(8) | BIT(24),
			    !!(priv->trans_invert & XPON_SETTING_BURST_EN_INV) * BIT(8) |
			    !!(priv->trans_invert & XPON_SETTING_TX_FAULT_INV) * BIT(24));
	airoha_xpon_phy_write(priv, XPON_GPON_DELIMITER_GUARD,
			      XPON_GPON_DELIMITER_DEFAULT);
	airoha_xpon_phy_rmw(priv, XPON_RX_COUNTER_ENABLE,
			    ECONET_XPON_COUNTER_ENABLE_MASK,
			    ECONET_XPON_COUNTER_ENABLE_MASK);
	airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, BIT(1));
	airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, BIT(3));
	airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, BIT(5));
	airoha_xpon_phy_rmw(priv, XPON_GPON_TX_COUNTER_CTRL,
			    BIT(2) | XPON_GPON_TX_COUNTER_ENABLE,
			    BIT(2) | XPON_GPON_TX_COUNTER_ENABLE);
	an->calibrated = false;
	an->parked = true;
	an7583_connect(priv);
	if (an7583_los(priv)) {
		usleep_range(1000, 1100);
		an7583_park_rx(priv);
		/* Repeat FIRST_PLUG_IN when the initial optical signal arrives. */
		an->calibrated = false;
		usleep_range(1000, 1100);
	} else {
		msleep(350);
	}

	/* Shared SDK low-rate FIR default: CN1=0, C0B=8, C1=C2=0. */
	airoha_xpon_phy_rmw(priv, AN7583_TX_FIR0,
			    BIT(24) | BIT(8) | GENMASK(20, 16) | GENMASK(5, 0),
			    BIT(24) | BIT(8) | 8);
	airoha_xpon_phy_rmw(priv, AN7583_TX_FIR1,
			    BIT(24) | BIT(8) | GENMASK(18, 16) | GENMASK(4, 0),
			    BIT(24) | BIT(8));
	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY,
			    XPON_PHYFWREADY_READY);
	WRITE_ONCE(priv->powered, true);
	mod_delayed_work(system_wq, &priv->ready_work,
			 msecs_to_jiffies(AN7583_POLL_MS));
	dev_info(priv->dev, "AN7583 GPON PHY started (LOS=%u)\n",
		 an7583_los(priv));
	ret = 0;
	goto out;

err_power:
	airoha_xpon_phy_set_tx_gpio(priv, false);
	if (an->configured)
		an7583_mask_interrupts(priv);
	reset_control_assert(priv->reset);
	/* Never leave the temporary reset-mode selector in the SCU. */
	regmap_update_bits(priv->scu, AN7583_SCU_WAN_CONF, AN7583_SCU_WAN_MASK, 0);
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	an->configured = false;
	an->calibrated = false;
	an->parked = true;
out:
	mutex_unlock(&an->lock);
	return ret;
}

static int an7583_phy_power_off(struct phy *phy)
{
	return an7583_power_off(phy_get_drvdata(phy));
}

static int an7583_set_tx_calibration_mode(struct airoha_xpon_phy *priv,
					  bool enable)
{
	struct an7583_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	mutex_lock(&an->lock);
	if (!priv->powered) {
		ret = -EHOSTDOWN;
		goto out;
	}
	if (enable == an->tx_calibration)
		goto out;
	if (enable) {
		an->saved_bist = airoha_xpon_phy_read(priv, AN7583_BIST);
		an->saved_tx_source = airoha_xpon_phy_read(priv, AN7583_TX_SOURCE);
		an7583_tx_calibration_pattern(priv);
	} else {
		airoha_xpon_phy_write(priv, AN7583_TX_SOURCE, an->saved_tx_source);
		airoha_xpon_phy_write(priv, AN7583_BIST, an->saved_bist);
	}
	an->tx_calibration = enable;
out:
	mutex_unlock(&an->lock);
	return ret;
}

static const struct phy_ops an7583_phy_ops = {
	.init = an7583_init,
	.exit = an7583_exit,
	.set_mode = an7583_set_mode,
	.power_on = an7583_power_on,
	.power_off = an7583_phy_power_off,
	.owner = THIS_MODULE,
};

static int an7583_probe(struct airoha_xpon_phy *priv, struct platform_device *pdev)
{
	struct an7583_xpon_phy *an;
	int ret;

	an = devm_kzalloc(priv->dev, sizeof(*an), GFP_KERNEL);
	if (!an)
		return -ENOMEM;
	mutex_init(&an->lock);
	an->parked = true;
	priv->soc_priv = an;
	INIT_DELAYED_WORK(&priv->ready_work, an7583_ready_work);
	ret = reset_control_assert(priv->reset);
	if (ret)
		return ret;
	return devm_add_action_or_reset(priv->dev, an7583_cleanup, priv);
}

static const struct airoha_xpon_phy_soc_data airoha_an7583_xpon_phy_data = {
	.name = "AN7583",
	.min_size = SZ_4K,
	.ana_offset = AN7583_ANA_OFFSET,
	.pma_offset = AN7583_PMA_OFFSET,
	.gpon_bit_delay_reg = XPON_PHYSET5,
	.gpon_bit_delay_mask = XPON_PHYSET5_BIT_DELAY_MASK,
	.gpon_bit_delay_enable = XPON_PHYSET5_BIT_DELAY_EN,
	.manages_fw_ready = true,
	.has_gpon_bip = true,
	.ops = &an7583_phy_ops,
	.probe = an7583_probe,
	.shutdown = an7583_shutdown,
	.get_link_state = an7583_get_link_state,
	.set_tx_enable = an7583_set_tx_enable,
	.set_tx_calibration_mode = an7583_set_tx_calibration_mode,
};

static const struct of_device_id an7583_xpon_phy_of_match[] = {
	{ .compatible = "airoha,an7583-xpon-phy", .data = &airoha_an7583_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, an7583_xpon_phy_of_match);

static struct platform_driver an7583_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-an7583-xpon-phy",
		.of_match_table = an7583_xpon_phy_of_match,
	},
};
module_platform_driver(an7583_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha AN7583 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
