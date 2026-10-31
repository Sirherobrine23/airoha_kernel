// SPDX-License-Identifier: GPL-2.0-only
/* EN7580 XGS-PON xPON PHY backend. */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "xpon.h"

/* EN7580 PCS and PON PMA; offsets are relative to 0x1faf0000. */
#define EN7580_XPON_PHY_PCS_MIN_SIZE		0x1000
#define EN7580_XPON_PHY_PMA_MIN_SIZE		0x1000
#define EN7580_XPON_PHY_PMA_OFFSET		0x3000
#define EN7580_PHY_RX_CTRL			0x0a04
#define EN7580_PHY_PCS_RESET			0x0a0c
#define EN7580_PHY_PCS_INT_ENABLE		0x0a14
#define EN7580_PHY_PCS_DEBUG			0x0a84
#define EN7580_PHY_PCS_SYNC			0x0b1c
#define EN7580_PHY_PHYA_INT_ENABLE		0x0b44
#define EN7580_PHY_SFP_LEVEL			0x0b48
#define EN7580_PHY_SFP_STATUS			0x0b4c
#define EN7580_PHY_PHYA_READY			0x0b54
#define EN7580_PHY_RX_IMPEDANCE			0x312c
#define EN7580_PHY_TX_FIR			0x3148
#define EN7580_PHY_EYE_INDEX2			0x3308
#define EN7580_PHY_EYE_COUNT0			0x3330
#define EN7580_PHY_EYE_COUNT1			0x3334
#define EN7580_PHY_EQ_CTRL0			0x3370
#define EN7580_PHY_PI_CAL			0x3430
#define EN7580_PHY_RX_DEBUG			0x349c
#define EN7580_PHY_EYE_READY			0x3538
#define EN7580_PHY_EYE_DONE			0x3548
#define EN7580_PHY_EYE_WIDTH			0x354c
#define EN7580_PHY_TX_CALIB0			0x3554
#define EN7580_PHY_TX_CALIB1			0x3558
#define EN7580_PHY_PMA_SETTING0			0x3600
#define EN7580_PHY_PMA_INT_ENABLE0		0x3610
#define EN7580_PHY_PMA_INT_ENABLE1		0x3614
#define EN7580_PHY_RX_FORCE0			0x3630
#define EN7580_PHY_RX_DISB0			0x363c
#define EN7580_PHY_RX_DISB2			0x3644
#define EN7580_PHY_RX_FORCE3			0x3648
#define EN7580_PHY_RX_DISB3			0x3658
#define EN7580_PHY_RX_FORCE9			0x366c
#define EN7580_PHY_RX_DISB7			0x3674
#define EN7580_PHY_RX_DISB8			0x3678
#define EN7580_PHY_PMA_MODE			0x3754
#define EN7580_PHY_TX_PLL_STATUS		0x3760
#define EN7580_PHY_PMA_RESET			0x37b0
#define EN7580_PHY_TX_DELAY			0x37b8
#define EN7580_PHY_RX_FREQ_STATUS		0x3820
#define EN7580_PHY_MEM_CLK			0x38a0
#define EN7580_PHY_DELAY			0xffff
#define EN7580_PHY_EYE_VALID			(BIT(16) | BIT(24))

struct en7580_xpon_phy {
	/* Serializes the EN7580 calibration, LOS recovery and TX requests. */
	struct mutex xgspon_lock;
	bool xgspon_configured;
	bool xgspon_calibrated;
	bool xgspon_rx_parked;
	bool xgspon_tx_requested;
	u8 rx_impedance;
	u8 txp_impedance;
	u8 txn_impedance;
	u32 last_tx_pll_status;
	int last_power_on_error;
	bool last_tx_pll_valid;
	int last_rx_error;
	u32 last_eye_done;
	u32 last_eye_ready;
	u8 last_eye_peaking;
	bool last_eye_valid;
};

struct en7580_phy_step {
	u16 reg;
	u32 mask;
	u32 value;
};

static const struct en7580_phy_step en7580_init_seq[] = {
	/* setting_mode_XPON_TXPLL(9) */
	{ 0x3230, 0xffffffff, 0x07f67620 },
	{ 0x3248, 0xffffffff, 0x1fd9d880 },
	{ 0x324c, 0xffffffff, 0x1fd9d880 },
	{ 0x3234, 0x00000100, 0x00000100 },
	/* setting_mode_XPON_TX(TX_Sync_XGSPON) */
	{ 0x3150, 0x00001c00, 0x00001400 },
	{ 0x3874, 0x00000003, 0x00000002 },
	{ 0x3150, 0x00000030, 0x00000020 },
	{ 0x3754, 0x00010000, 0x00000000 },
	{ 0x3754, 0x00000100, 0x00000000 },
	/* setting_mode_XPON_RX(9) */
	{ 0x3118, 0x03000000, 0x00000000 },
	{ 0x3714, 0x00000003, 0x00000002 },
	{ 0x3754, 0x00000007, 0x00000000 },
	/* setting_XPON_DIG() */
	{ 0x3234, 0x00000003, 0x00000002 },
	{ 0x322c, 0x00000f00, 0x00000500 },
	{ 0x3794, 0xffff0000, 0x01130000 },
	{ 0x3794, 0x0000ffff, 0x000000fa },
	{ 0x3798, 0xffff0000, 0x009b0000 },
	{ 0x3798, 0x0000ffff, 0x00000210 },
	{ 0x3790, 0xffff0000, 0x00040000 },
	{ 0x3790, 0x0000ffff, 0x00000026 },
	{ 0x37b8, 0x00ff0000, 0x00020000 },
	{ 0x3228, 0x00000700, 0x00000300 },
	{ 0x3224, 0x01000000, 0x00000000 },
	{ 0x3204, 0xff000000, 0x01000000 },
	{ 0x3204, 0x00ff0000, 0x00010000 },
	{ 0x3204, 0x0000ff00, 0x00000a00 },
	{ 0x3204, 0x00000001, 0x00000001 },
	{ 0x3344, 0x0000ffff, 0x00000002 },
	{ 0x3344, 0xffff0000, 0x03e80000 },
	{ 0x3350, 0x0000ffff, 0x00000002 },
	{ 0x3350, 0xffff0000, 0x03e80000 },
	{ 0x3348, 0x0000ffff, 0x00000002 },
	{ 0x3348, 0xffff0000, 0x03e80000 },
	{ 0x334c, 0x0000ffff, 0x00000002 },
	{ 0x334c, 0xffff0000, 0x03e80000 },
	{ 0x3340, 0x00ffff00, 0x00000100 },
	{ 0x3340, 0x00000007, 0x00000001 },
	{ 0x3358, 0x0000ffff, 0x00000002 },
	{ 0x3378, 0x00000003, 0x00000003 },
	{ 0x3378, 0x00001f00, 0x00000500 },
	{ 0x3300, 0x07ff0000, 0x02400000 },
	{ 0x3300, 0x000007ff, 0x000001c0 },
	{ 0x3338, 0x00000001, 0x00000001 },
	{ 0x3338, 0x00000100, 0x00000000 },
	{ 0x330c, 0x00010000, 0x00010000 },
	{ 0x3320, 0x0000ff00, 0x00000400 },
	{ 0x3320, 0x000000ff, 0x00000004 },
	{ 0x3324, 0x00ff0000, 0x00040000 },
	{ 0x3324, 0x000007ff, 0x00000004 },
	{ 0x3330, 0xff000000, 0xff000000 },
	{ 0x3330, 0x000003ff, 0x000000d0 },
	{ 0x3370, 0x000000ff, 0x00000080 },
	{ 0x3370, 0x0007ff00, 0x00000000 },
	{ 0x3370, 0xff000000, 0x00000000 },
	{ 0x3374, 0x00000001, 0x00000001 },
	{ 0x3490, 0x00000100, 0x00000000 },
	{ 0x3490, 0x00000001, 0x00000001 },
	{ 0x3440, 0x03000000, 0x01000000 },
	{ 0x3440, 0x0000ff00, 0x00002e00 },
	{ 0x3440, 0x00000001, 0x00000001 },
	{ 0x3440, 0x00030000, 0x00010000 },
	{ 0x3444, 0x00000001, 0x00000000 },
	{ 0x3444, 0x00000f00, 0x00000000 },
	{ 0x3444, 0x00030000, 0x00010000 },
	{ 0x3354, 0x0000ffff, 0x00000004 },
	{ 0x3354, 0xffff0000, 0x00c80000 },
	{ 0x3400, 0x000000ff, 0x00000000 },
	{ 0x3400, 0x00000100, 0x00000000 },
	{ 0x3410, 0x00000f00, 0x00000a00 },
	{ 0x3410, 0x00000001, 0x00000001 },
	{ 0x3410, 0x007f0000, 0x00100000 },
	{ 0x3410, 0x3f800000, 0x38000000 },
	{ 0x3304, 0x0000007f, 0x00000040 },
	{ 0x3304, 0x00007f00, 0x00003f00 },
	{ 0x3304, 0x00010000, 0x00010000 },
	{ 0x3308, 0x000fffff, 0x00000018 },
	{ 0x37dc, 0x0000ff00, 0x00000200 },
	{ 0x37dc, 0x00000002, 0x00000002 },
	{ 0x37dc, 0x00000001, 0x00000001 },
	{ 0x3430, 0x00000700, 0x00000100 },
	{ 0x3430, 0x00030000, 0x00000000 },
	{ 0x3460, 0x0007ff00, 0x0000a500 },
	{ 0x3460, 0x00000007, 0x00000003 },
	{ 0x3460, 0x03000000, 0x00000000 },
	{ 0x3464, 0x07000000, 0x04000000 },
	{ 0x3464, 0x00070000, 0x00010000 },
	{ 0x3474, 0x000007ff, 0x000001ff },
	{ 0x3474, 0x07ff0000, 0x04000000 },
	{ 0x3468, 0x00000700, 0x00000400 },
	{ 0x3468, 0x00000007, 0x00000003 },
	{ 0x365c, 0x01000000, 0x01000000 },
	{ 0x3490, 0x00000001, 0x00000001 },
	{ 0x3490, 0x00000100, 0x00000000 },
	{ 0x3410, 0x00000001, 0x00000000 },
	{ 0x3410, 0x00000f00, 0x00000a00 },
	{ 0x3410, 0x007f0000, 0x00100000 },
	{ 0x3410, 0x3f800000, 0x38000000 },
	{ 0x3400, 0x00000100, 0x00000000 },
	/* setting_XPON_ANA(9) */
	{ 0x3100, 0x01c00000, 0x01000000 },
	{ 0x3100, 0x80000000, 0x80000000 },
	{ 0x3100, 0x7e000000, 0x02000000 },
	{ 0x3144, 0x00400000, 0x00400000 },
	{ 0x3144, 0x7f800000, 0x10800000 },
	{ 0x3144, 0x00200000, 0x00000000 },
	{ 0x3144, 0x80000000, 0x00000000 },
	{ 0x3144, 0x00100000, 0x00100000 },
	{ 0x3144, 0x00080000, 0x00000000 },
	{ 0x3144, 0x00040000, 0x00040000 },
	{ 0x3144, 0x00020000, 0x00020000 },
	{ 0x3144, 0x00010000, 0x00000000 },
	{ 0x3134, 0xe0000000, 0x80000000 },
	{ 0x3134, 0x1c000000, 0x10000000 },
	{ 0x3134, 0x03c00000, 0x02000000 },
	{ 0x3134, 0x00200000, 0x00200000 },
	{ 0x3134, 0x00060000, 0x00060000 },
	{ 0x3134, 0x00018000, 0x00018000 },
	{ 0x3134, 0x00000010, 0x00000010 },
	{ 0x3134, 0x00001fe0, 0x00001fe0 },
	{ 0x3138, 0xc0000000, 0xc0000000 },
	{ 0x3138, 0x00010000, 0x00010000 },
	{ 0x3138, 0x00001fe0, 0x00001000 },
	{ 0x313c, 0x7f800000, 0x00000000 },
	{ 0x313c, 0x007f8000, 0x007f8000 },
	{ 0x3130, 0xf0000000, 0x60000000 },
	{ 0x3130, 0x0f000000, 0x03000000 },
	{ 0x3130, 0x001c0000, 0x00000000 },
	{ 0x3130, 0x00e00000, 0x00200000 },
	{ 0x3130, 0x00038000, 0x00028000 },
	{ 0x3130, 0x00000400, 0x00000400 },
	{ 0x3130, 0x00000200, 0x00000200 },
	{ 0x3130, 0x00000100, 0x00000100 },
	{ 0x3130, 0x00000020, 0x00000020 },
	{ 0x3130, 0x00000010, 0x00000010 },
	{ 0x3130, 0x0000000e, 0x00000008 },
	{ 0x37a8, 0x0000ffff, 0x00000c80 },
	{ 0x37ac, 0x0000ffff, 0x00000d48 },
	{ 0x3104, 0x00000800, 0x00000800 },
	{ 0x3104, 0x0000f000, 0x00007000 },
	{ 0x3104, 0x3f000000, 0x1f000000 },
	{ 0x3148, 0x80000000, 0x80000000 },
	{ 0x3148, 0x60000000, 0x40000000 },
	{ 0x3148, 0x00000020, 0x00000020 },
	{ 0x314c, 0x80000000, 0x00000000 },
	{ 0x314c, 0x40000000, 0x00000000 },
	{ 0x314c, 0x00020000, 0x00000000 },
	{ 0x3150, 0x00000300, 0x00000200 },
	{ 0x3150, 0x000000c0, 0x00000080 },
	{ 0x314c, 0x00fc0000, 0x00000000 },
	{ 0x314c, 0x02000000, 0x00000000 },
	{ 0x314c, 0x01000000, 0x00000000 },
	{ 0x3148, 0x00000400, 0x00000400 },
	{ 0x314c, 0x20000000, 0x00000000 },
	{ 0x314c, 0x10000000, 0x00000000 },
	{ 0x314c, 0x08000000, 0x08000000 },
	{ 0x314c, 0x04000000, 0x04000000 },
	{ 0x3148, 0x0f000000, 0x00000000 },
	{ 0x3148, 0x00f80000, 0x00000000 },
	{ 0x3148, 0x0007c000, 0x00030000 },
	{ 0x3148, 0x00003800, 0x00002000 },
	{ 0x312c, 0x00200000, 0x00200000 },
	{ 0x312c, 0x00400000, 0x00400000 },
	{ 0x312c, 0x00800000, 0x00800000 },
	{ 0x312c, 0x000e0000, 0x00080000 },
	{ 0x312c, 0x00100000, 0x00100000 },
	{ 0x3118, 0x00ffffe0, 0x00100000 },
	{ 0x3114, 0x3ffff800, 0x3c000000 },
	{ 0x3124, 0x08000000, 0x00000000 },
	{ 0x310c, 0x00010000, 0x00000000 },
	/* seq_XPON_DIG_reset() */
	{ 0x37b0, 0x0000001f, 0x00000000 },
	{ 0x37a4, 0x00000002, 0x00000000 },
	{ 0x37b0, 0x00000002, 0x00000002 },
	{ 0x37b0, 0x00000010, 0x00000010 },
	{ 0x37b0, 0x00000008, 0x00000008 },
	{ 0x37b0, 0x00000004, 0x00000004 },
	{ 0x37b0, 0x00000001, 0x00000001 },
	{ 0x37a4, 0x00000002, 0x00000002 },
	/* seq_JCPLL_on() */
	{ 0x3204, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 5000 },
	/* seq_XPON_DIG_fm_on(fm_Sync_XGSPON) */
	{ 0x3730, 0xffff0000, 0x01000000 },
	{ 0x3730, 0x0000ffff, 0x00000100 },
	{ 0x3734, 0xffff0000, 0x00a50000 },
	{ 0x3734, 0x0000ffff, 0x000000a4 },
	{ 0x373c, 0x0000f000, 0x00008000 },
	{ 0x373c, 0x00000f00, 0x00000300 },
	{ 0x373c, 0x00000007, 0x00000007 },
	{ 0x3738, 0xffff0000, 0x00a50000 },
	{ 0x3738, 0x0000ffff, 0x000000a4 },
	{ 0x3420, 0xffff0000, 0x02000000 },
	{ 0x3420, 0x0000ffff, 0x00000200 },
	{ 0x3424, 0xffff0000, 0x02930000 },
	{ 0x3424, 0x0000ffff, 0x00000292 },
	{ 0x3428, 0xffff0000, 0x02930000 },
	{ 0x3428, 0x0000ffff, 0x00000292 },
	{ 0x342c, 0x0000f000, 0x00008000 },
	{ 0x342c, 0x00000f00, 0x00000100 },
	{ 0x342c, 0x00000007, 0x00000007 },
	/* seq_XPON_TXPLL_on() */
	{ 0x3200, 0x01000000, 0x01000000 },
	{ 0x3200, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 5000 },
	/* seq_XPON_TX_on() */
	{ 0x3550, 0x00000001, 0x00000001 },
	{ 0x3750, 0x00000100, 0x00000100 },
	{ 0x3750, 0x00000001, 0x00000001 },
	{ 0x3550, 0x00000100, 0x00000100 },
	{ 0x3550, 0x00000001, 0x00000001 },
	{ 0x3554, 0x03000000, 0x02000000 },
	{ 0x3558, 0x03000000, 0x02000000 },
	{ 0x3554, 0x00010000, 0x00010000 },
	{ 0x3558, 0x00010000, 0x00010000 },
};

static const struct en7580_phy_step en7580_rx_cal_seq[] = {
	/* seq_XPON_RX_preset() */
	{ 0x3368, 0x00000100, 0x00000000 },
	{ 0x3360, 0x00000100, 0x00000000 },
	{ 0x3368, 0x01000000, 0x00000000 },
	{ 0x3360, 0x01000000, 0x00000000 },
	{ 0x3630, 0x00010000, 0x00000000 },
	{ 0x3630, 0x01000000, 0x00000000 },
	{ 0x363c, 0x00010000, 0x00000000 },
	{ 0x363c, 0x01000000, 0x00000000 },
	{ 0x3630, 0x00000003, 0x00000001 },
	{ 0x3630, 0x00000700, 0x00000300 },
	{ 0x363c, 0x00000001, 0x00000000 },
	{ 0x363c, 0x00000100, 0x00000000 },
	{ 0x366c, 0x00000100, 0x00000100 },
	{ 0x3678, 0x00000100, 0x00000000 },
	{ 0x366c, 0x00010000, 0x00000000 },
	{ 0x3678, 0x00010000, 0x00000000 },
	{ 0x3494, 0x00000001, 0x00000000 },
	{ 0x3674, 0x00000001, 0x00000000 },
	{ 0x3668, 0x00000001, 0x00000000 },
	{ 0x3360, 0x00010000, 0x00000000 },
	{ 0x3368, 0x00010000, 0x00000000 },
	/* aux_XPON_TDC_off() */
	{ 0x3234, 0x00000100, 0x00000100 },
	{ 0x3210, 0x00000001, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 1000 },
	/* seq_XPON_RX_on() */
	{ 0x368c, 0x00000100, 0x00000100 },
	{ 0x368c, 0x00000001, 0x00000001 },
	{ 0x368c, 0x01000000, 0x01000000 },
	{ 0x368c, 0x00010000, 0x00010000 },
	{ 0x3690, 0x00000001, 0x00000001 },
	{ 0x368c, 0x00000100, 0x00000000 },
	{ 0x368c, 0x00000001, 0x00000000 },
	{ 0x368c, 0x01000000, 0x00000000 },
	{ 0x368c, 0x00010000, 0x00000000 },
	{ 0x3690, 0x00000001, 0x00000000 },
	{ 0x368c, 0x00000100, 0x00000100 },
	{ 0x368c, 0x00000001, 0x00000001 },
	{ 0x368c, 0x01000000, 0x01000000 },
	{ 0x368c, 0x00010000, 0x00010000 },
	{ 0x3690, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 500 },
	/* seq_XPON_RX_L2R() */
	{ 0x3630, 0x00010000, 0x00000000 },
	{ 0x3630, 0x01000000, 0x00000000 },
	{ 0x363c, 0x00010000, 0x00000000 },
	{ 0x363c, 0x01000000, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	/* seq_XPON_RX_pical() */
	{ 0x3644, 0x01000000, 0x01000000 },
	{ 0x3370, 0x000000ff, 0x00000008 },
	{ 0x3430, 0x00000700, 0x00000400 },
	{ 0x3494, 0x00010000, 0x00000000 },
	{ 0x3664, 0x00000100, 0x00000000 },
	{ 0x3670, 0x00000100, 0x00000000 },
	{ 0x3664, 0x00000001, 0x00000000 },
	{ 0x3670, 0x00000001, 0x00000000 },
	{ 0x3654, 0x00000100, 0x00000000 },
	{ 0x3660, 0x01000000, 0x00000000 },
	{ 0x3364, 0x00000100, 0x00000000 },
	{ 0x335c, 0x00000100, 0x00000000 },
	{ 0x3494, 0x00010000, 0x00010000 },
	{ 0x3664, 0x00000100, 0x00000100 },
	{ 0x3664, 0x00000001, 0x00000001 },
	{ 0x3654, 0x00000100, 0x00000100 },
	{ EN7580_PHY_DELAY, 0, 200 },
	{ 0x3654, 0x00000100, 0x00000000 },
	{ 0x3658, 0x00000001, 0x00000000 },
	{ 0x3648, 0x00000001, 0x00000001 },
	/* seq_XPON_RX_pdos() */
	{ 0x3368, 0x00000100, 0x00000000 },
	{ 0x3360, 0x00000100, 0x00000000 },
	{ 0x3640, 0x00000100, 0x00000100 },
	{ 0x3640, 0x00010000, 0x00010000 },
	{ 0x3640, 0x01000000, 0x01000000 },
	{ 0x3644, 0x00000001, 0x00000001 },
	{ 0x3644, 0x00000100, 0x00000100 },
	{ 0x3668, 0x00000001, 0x00000000 },
	{ 0x3674, 0x00000001, 0x00000000 },
	{ 0x3334, 0x01000000, 0x00000000 },
	{ 0x3334, 0x00000100, 0x00000000 },
	{ 0x3668, 0x01000000, 0x00000000 },
	{ 0x3674, 0x01000000, 0x00000000 },
	{ 0x3334, 0x00010000, 0x00000000 },
	{ 0x3334, 0x00000001, 0x00000000 },
	{ 0x3490, 0x00070000, 0x00020000 },
	{ 0x3664, 0x00010000, 0x00000000 },
	{ 0x3670, 0x00010000, 0x00000000 },
	{ 0x3498, 0x00000001, 0x00000000 },
	{ 0x3364, 0x00010000, 0x00000000 },
	{ 0x335c, 0x00010000, 0x00000000 },
	{ 0x3364, 0x00000001, 0x00000001 },
	{ 0x335c, 0x00000001, 0x00000000 },
	{ 0x3664, 0x00010000, 0x00010000 },
	{ 0x3498, 0x00000001, 0x00000001 },
	{ 0x3364, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 200 },
	{ 0x3364, 0x00010000, 0x00000000 },
	{ 0x3364, 0x00000001, 0x00000000 },
	{ 0x3334, 0x01000000, 0x00000000 },
	{ 0x3334, 0x00000100, 0x00000100 },
	{ 0x3668, 0x01000000, 0x00000000 },
	{ 0x3674, 0x01000000, 0x01000000 },
	{ 0x3334, 0x00010000, 0x00000000 },
	{ 0x3334, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 200 },
	/* seq_XPON_RX_feos() */
	{ 0x3368, 0x00000100, 0x00000000 },
	{ 0x3360, 0x00000100, 0x00000000 },
	{ 0x3644, 0x00010000, 0x00010000 },
	{ 0x3668, 0x00000001, 0x00000000 },
	{ 0x3674, 0x00000001, 0x00000000 },
	{ 0x3400, 0x000000ff, 0x00000030 },
	{ 0x3668, 0x00010000, 0x00000000 },
	{ 0x3674, 0x00010000, 0x00000000 },
	{ 0x3494, 0x00000100, 0x00000000 },
	{ 0x3364, 0x01000000, 0x00000000 },
	{ 0x335c, 0x01000000, 0x00000000 },
	{ 0x3364, 0x00000001, 0x00000001 },
	{ 0x335c, 0x00000001, 0x00000000 },
	{ 0x3668, 0x00010000, 0x00010000 },
	{ 0x3494, 0x00000100, 0x00000100 },
	{ 0x3364, 0x01000000, 0x01000000 },
	{ EN7580_PHY_DELAY, 0, 200 },
	{ 0x3364, 0x01000000, 0x00000000 },
	{ 0x3364, 0x00000001, 0x00000000 },
	/* seq_XPON_RX_sdcal() */
	{ 0x3640, 0x00000001, 0x00000000 },
	{ 0x3634, 0x0000001f, 0x00000000 },
};

static const struct en7580_phy_step en7580_eye_setup_seq[] = {
	/* XPON_eye_setting(10) */
	{ 0x3118, 0x03000000, 0x00000000 },
	{ 0x3330, 0xff000000, 0xff000000 },
	{ 0x3300, 0x000007ff, 0x000001c0 },
	{ 0x3300, 0x07ff0000, 0x02400000 },
	{ 0x3330, 0x000003ff, 0x000000f8 },
	{ 0x3330, 0x00010000, 0x00000000 },
	{ 0x3338, 0x00000100, 0x00000000 },
	{ 0x3304, 0x00010000, 0x00000000 },
	{ 0x3308, 0x000fffff, 0x0000fff8 },
	{ 0x330c, 0x00000001, 0x00000000 },
	{ 0x330c, 0x00000100, 0x00000000 },
	{ 0x330c, 0x00010000, 0x00010000 },
	{ 0x3320, 0x000000ff, 0x00000004 },
	{ 0x3320, 0x0000ff00, 0x00000400 },
	{ 0x3324, 0x000007ff, 0x00000004 },
	{ 0x3324, 0x00ff0000, 0x00040000 },
	{ 0x3374, 0x00010000, 0x00000000 },
	{ 0x3374, 0x00000001, 0x00000000 },
	{ 0x3374, 0x00000100, 0x00000000 },
	{ 0x3378, 0x00000003, 0x00000001 },
	{ 0x3374, 0x01000000, 0x00000000 },
};

static const struct en7580_phy_step en7580_eye_cal_seq[] = {
	/* XPON_eye_Cal() */
	{ 0x3644, 0x01000000, 0x01000000 },
	{ 0x3370, 0x000000ff, 0x00000080 },
	{ 0x3430, 0x00000700, 0x00000100 },
	{ 0x3494, 0x00010000, 0x00000000 },
	{ 0x3670, 0x00000100, 0x00000000 },
	{ 0x3664, 0x00000100, 0x00000000 },
	{ 0x3670, 0x00000001, 0x00000000 },
	{ 0x3664, 0x00000001, 0x00000000 },
	{ 0x3660, 0x01000000, 0x00000000 },
	{ 0x3654, 0x00000100, 0x00000000 },
	{ 0x335c, 0x00000100, 0x00000000 },
	{ 0x3364, 0x00000100, 0x00000000 },
	{ 0x3494, 0x00010000, 0x00010000 },
	{ 0x3664, 0x00000100, 0x00000100 },
	{ 0x3664, 0x00000001, 0x00000001 },
	{ 0x3654, 0x00000100, 0x00000100 },
	{ EN7580_PHY_DELAY, 0, 1000 },
	{ 0x3654, 0x00000100, 0x00000000 },
	{ 0x3658, 0x00000001, 0x00000000 },
	{ 0x3648, 0x00000001, 0x00000001 },
};

static const struct en7580_phy_step en7580_rx_connect_seq[] = {
	/* seq_XPON_DIG_reset() */
	{ 0x37b0, 0x0000001f, 0x00000000 },
	{ 0x37a4, 0x00000002, 0x00000000 },
	{ 0x37b0, 0x00000002, 0x00000002 },
	{ 0x37b0, 0x00000010, 0x00000010 },
	{ 0x37b0, 0x00000008, 0x00000008 },
	{ 0x37b0, 0x00000004, 0x00000004 },
	{ 0x37b0, 0x00000001, 0x00000001 },
	{ 0x37a4, 0x00000002, 0x00000002 },
	/* sw_pma_rst_hold() */
	{ 0x37b0, 0x00000008, 0x00000000 },
	/* seq_XPON_RX_L2D() */
	{ 0x3630, 0x01000000, 0x01000000 },
	{ 0x3630, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 200 },
	/* aux_XPON_TDC_on() */
	{ 0x3234, 0x00000100, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3210, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000100, 0x00000100 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000100, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000001, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3224, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 5000 },
	/* sw_pma_rst_release() */
	{ 0x37b0, 0x00000008, 0x00000008 },
};

static const struct en7580_phy_step en7580_rx_finish_seq[] = {
	/* seq_XPON_RX_blwc_on() */
	{ 0x3494, 0x00000001, 0x00000001 },
	{ 0x3668, 0x00000001, 0x00000001 },
	{ 0x3368, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 50 },
	/* aux_XPON_TDC_on() */
	{ 0x3234, 0x00000100, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3210, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000001, 0x00000001 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000100, 0x00000100 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000100, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00000001, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3220, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	{ 0x3224, 0x00010000, 0x00010000 },
	{ EN7580_PHY_DELAY, 0, 5000 },
	/* seq_XPON_RX_rxrdy() */
	{ 0x3368, 0x01000000, 0x01000000 },
	{ 0x3360, 0x01000000, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 10 },
	{ 0x37b0, 0x00000001, 0x00000000 },
	{ 0x37b0, 0x00000001, 0x00000001 },
	{ 0x312c, 0x06000000, 0x04000000 },
};

static const struct en7580_phy_step en7580_rx_disconnect_seq[] = {
	/* aux_XPON_TDC_off() */
	{ 0x3234, 0x00000100, 0x00000100 },
	{ 0x3210, 0x00000001, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 1000 },
	/* seq_XPON_RX_L2R() */
	{ 0x3630, 0x00010000, 0x00000000 },
	{ 0x3630, 0x01000000, 0x00000000 },
	{ 0x363c, 0x00010000, 0x00000000 },
	{ 0x363c, 0x01000000, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 100 },
	/* seq_XPON_FLL_Reset() */
	{ 0x346c, 0x00000001, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 500 },
	{ 0x346c, 0x00000001, 0x00000001 },
	{ 0x346c, 0x00000100, 0x00000000 },
	{ EN7580_PHY_DELAY, 0, 500 },
	{ 0x346c, 0x00000100, 0x00000100 },
};

static bool en7580_phy_ready(struct airoha_xpon_phy *priv)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;

	return soc->xgspon_calibrated && !soc->xgspon_rx_parked &&
		(airoha_xpon_phy_read(priv, EN7580_PHY_PHYA_READY) & BIT(0)) &&
		(airoha_xpon_phy_read(priv, EN7580_PHY_RX_FREQ_STATUS) & BIT(0)) &&
		(airoha_xpon_phy_read(priv, EN7580_PHY_TX_PLL_STATUS) & BIT(16)) &&
		(airoha_xpon_phy_read(priv, EN7580_PHY_PCS_SYNC) & BIT(1));
}

static bool en7580_phy_los(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, EN7580_PHY_SFP_STATUS) & BIT(0);
}

static int en7580_phy_get_link_state(struct airoha_xpon_phy *priv,
				     bool *ready, bool *los)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;

	mutex_lock(&soc->xgspon_lock);
	if (!priv->powered) {
		*ready = false;
		*los = true;
	} else {
		*los = en7580_phy_los(priv);
		*ready = !*los && en7580_phy_ready(priv);
	}
	mutex_unlock(&soc->xgspon_lock);
	return 0;
}

static int en7580_phy_set_tx_enable(struct airoha_xpon_phy *priv, bool enable)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;

	mutex_lock(&soc->xgspon_lock);
	if (enable && !priv->powered) {
		mutex_unlock(&soc->xgspon_lock);
		return -EAGAIN;
	}
	/* A request survives LOS, but only a synchronized RX permits TX. */
	soc->xgspon_tx_requested = enable;
	enable = enable && priv->powered && !en7580_phy_los(priv) &&
		en7580_phy_ready(priv);
	airoha_xpon_phy_set_tx_gpio(priv, enable);
	mutex_unlock(&soc->xgspon_lock);
	return 0;
}

/* EN7580 XGS-PON uses its own PCS/PMA lifecycle and never the 1G registers. */
static void en7580_phy_sequence(struct airoha_xpon_phy *priv,
				const struct en7580_phy_step *seq, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (seq[i].reg == EN7580_PHY_DELAY) {
			if (seq[i].value < 10)
				udelay(seq[i].value);
			else
				usleep_range(seq[i].value,
					     seq[i].value + max(1U, seq[i].value / 10));
			continue;
		}
		airoha_xpon_phy_rmw(priv, seq[i].reg, seq[i].mask, seq[i].value);
	}
	/* Complete all posted calibration/reset writes before proceeding. */
	airoha_xpon_phy_read(priv, EN7580_PHY_PMA_RESET);
}

#define en7580_phy_run(priv, name) \
	en7580_phy_sequence(priv, en7580_##name##_seq, \
			    ARRAY_SIZE(en7580_##name##_seq))

static void en7580_phy_pcs_reset(struct airoha_xpon_phy *priv)
{
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 0);
	airoha_xpon_phy_read(priv, EN7580_PHY_PCS_RESET);
	usleep_range(1000, 1100);
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 3);
	airoha_xpon_phy_read(priv, EN7580_PHY_PCS_RESET);
	usleep_range(1000, 1100);
}

static void en7580_phy_mask_interrupts(struct airoha_xpon_phy *priv)
{
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PHYA_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PMA_INT_ENABLE0, 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PMA_INT_ENABLE1, 0);
}

static void en7580_phy_quiesce(struct airoha_xpon_phy *priv)
{
	en7580_phy_mask_interrupts(priv);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PMA_RESET, GENMASK(4, 0), 0);
	airoha_xpon_phy_read(priv, EN7580_PHY_PMA_RESET);
}

static u32 en7580_phy_eye_status(struct airoha_xpon_phy *priv)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;
	/* Debug results are latched: refresh the latch on every poll. */
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DEBUG, BIT(24), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DEBUG, BIT(24), BIT(24));
	soc->last_eye_done = airoha_xpon_phy_read(priv, EN7580_PHY_EYE_DONE);
	soc->last_eye_ready = airoha_xpon_phy_read(priv, EN7580_PHY_EYE_READY);
	soc->last_eye_valid = true;
	return soc->last_eye_done & soc->last_eye_ready;
}

static void en7580_phy_eye_stop(struct airoha_xpon_phy *priv)
{
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(8), BIT(8));
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB8, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB3, BIT(0), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE3, BIT(0), 0);
}

static int en7580_phy_eye_scan(struct airoha_xpon_phy *priv)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;
	u32 value, left, right, score, best_score = 0;
	unsigned int peaking, best_peaking = 0;
	int ret;

	soc->last_eye_valid = false;
	en7580_phy_run(priv, eye_setup);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_EQ_CTRL0, GENMASK(7, 0), 0x80);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PI_CAL, GENMASK(10, 8), 4 << 8);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB0, BIT(0), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0, GENMASK(1, 0), 1);

	/* SDK EO_Scan(0, 10, 0, 7, 0): choose the widest horizontal eye. */
	for (peaking = 0; peaking < 8; peaking++) {
		if (en7580_phy_los(priv)) {
			ret = -ENOLINK;
			goto err_stop;
		}
		soc->last_eye_peaking = peaking;
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB0, BIT(8), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0, GENMASK(10, 8),
				    peaking << 8);
		en7580_phy_run(priv, eye_cal);

		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_COUNT0,
				    GENMASK(9, 0), 0xa);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_INDEX2,
				    GENMASK(19, 0), 0x44c);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB8, BIT(8), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(8), BIT(8));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(8), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB8, BIT(16), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(16), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(16), BIT(16));
		usleep_range(5500, 6000);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB2,
				    BIT(8) | BIT(24), BIT(8) | BIT(24));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_COUNT1, BIT(8), BIT(8));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB7, BIT(24), BIT(24));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_COUNT1, BIT(0), BIT(0));
		ret = read_poll_timeout(en7580_phy_eye_status, value,
					(value & EN7580_PHY_EYE_VALID) == EN7580_PHY_EYE_VALID,
					100, 10000, false, priv);
		if (en7580_phy_los(priv)) {
			ret = -ENOLINK;
			goto err_stop;
		}

		if (ret)
			goto err_stop;

		value = airoha_xpon_phy_read(priv, EN7580_PHY_EYE_WIDTH);
		left = FIELD_GET(GENMASK(26, 16), value);
		right = FIELD_GET(GENMASK(10, 0), value);
		score = left > right ? left - right : right - left;
		if (score > best_score) {
			best_score = score;
			best_peaking = peaking;
		}
		en7580_phy_eye_stop(priv);
	}

	if (!best_score) {
		ret = -EIO;
		goto err_stop;
	}

	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB0, BIT(0) | BIT(8), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0,
			    GENMASK(1, 0) | GENMASK(10, 8),
			     1 | (best_peaking << 8));
	dev_dbg(priv->dev, "XGS-PON eye width=%u peaking=%u\n",
		best_score, best_peaking);
	return 0;

err_stop:
	en7580_phy_eye_stop(priv);
	return ret;
}

static int en7580_phy_connect(struct airoha_xpon_phy *priv)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;
	int ret;

	airoha_xpon_phy_set_tx_gpio(priv, false);
	if (!soc->xgspon_calibrated) {
		/* Re-run the complete RX calibration after an interrupted eye scan. */
		en7580_phy_run(priv, rx_cal);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0,
				    BIT(24) | BIT(16), BIT(24) | BIT(16));
		usleep_range(200, 250);
		ret = en7580_phy_eye_scan(priv);
		if (ret)
			return ret;
		en7580_phy_run(priv, rx_finish);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_IMPEDANCE,
				    GENMASK(26, 25), soc->rx_impedance << 25);
		soc->xgspon_calibrated = true;
	} else {
		en7580_phy_run(priv, rx_connect);
	}
	/* Normal PMA data, with loopback and BIST paths disabled. */
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PMA_MODE, BIT(16) | BIT(8), 0);
	en7580_phy_pcs_reset(priv);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), BIT(16));
	usleep_range(8000, 9000);
	return 0;
}

static void en7580_phy_ready_work(struct work_struct *work)
{
	struct airoha_xpon_phy *priv =
		container_of(to_delayed_work(work), struct airoha_xpon_phy, ready_work);
	struct en7580_xpon_phy *soc = priv->soc_priv;
	bool ready, los;
	int ret;

	mutex_lock(&soc->xgspon_lock);
	if (!priv->powered)
		goto out;
	los = en7580_phy_los(priv);
	ready = !los && en7580_phy_ready(priv);
	if (los) {
		airoha_xpon_phy_set_tx_gpio(priv, false);
		if (!soc->xgspon_rx_parked) {
			en7580_phy_run(priv, rx_disconnect);
			airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
			soc->xgspon_rx_parked = true;
		}
	} else if (soc->xgspon_rx_parked || !ready) {
		ret = en7580_phy_connect(priv);
		soc->last_rx_error = ret;
		if (ret) {
			dev_warn_ratelimited(priv->dev,
					     "XGS-PON RX recovery failed: %d\n", ret);
			en7580_phy_run(priv, rx_disconnect);
			airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
			soc->xgspon_rx_parked = true;
		} else {
			soc->xgspon_rx_parked = false;
		}
	}
	ready = !en7580_phy_los(priv) && en7580_phy_ready(priv);
	if (ready != priv->ready_reported) {
		priv->ready_reported = ready;
		dev_info(priv->dev, "XGS-PON receiver %s\n", ready ? "ready" : "not ready");
	}
	airoha_xpon_phy_set_tx_gpio(priv, ready && soc->xgspon_tx_requested);
	mod_delayed_work(system_wq, &priv->ready_work, msecs_to_jiffies(1000));
out:
	mutex_unlock(&soc->xgspon_lock);
}

static int en7580_phy_power_off_priv(struct airoha_xpon_phy *priv)
{
	struct en7580_xpon_phy *soc = priv->soc_priv;
	int ret;

	mutex_lock(&soc->xgspon_lock);
	WRITE_ONCE(priv->powered, false);
	soc->xgspon_tx_requested = false;
	airoha_xpon_phy_set_tx_gpio(priv, false);
	mutex_unlock(&soc->xgspon_lock);
	cancel_delayed_work_sync(&priv->ready_work);
	mutex_lock(&soc->xgspon_lock);
	if (soc->xgspon_configured)
		en7580_phy_quiesce(priv);
	ret = reset_control_assert(priv->reset);
	soc->xgspon_configured = false;
	soc->xgspon_calibrated = false;
	priv->ready_reported = false;
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	mutex_unlock(&soc->xgspon_lock);
	return ret;
}

static void en7580_phy_shutdown(void *data)
{
	struct airoha_xpon_phy *priv = data;
	int ret = en7580_phy_power_off_priv(priv);

	if (ret)
		dev_warn(priv->dev, "failed to hold XGS-PON PHY reset: %d\n", ret);
}

static int en7580_phy_init(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct en7580_xpon_phy *soc = priv->soc_priv;

	/* Configuration and reset release are deferred to phy_power_on(). */
	mutex_lock(&soc->xgspon_lock);
	priv->initialized = true;
	mutex_unlock(&soc->xgspon_lock);
	return 0;
}

static int en7580_phy_exit(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct en7580_xpon_phy *soc = priv->soc_priv;
	int ret;

	ret = en7580_phy_power_off_priv(priv);
	mutex_lock(&soc->xgspon_lock);
	priv->initialized = false;
	mutex_unlock(&soc->xgspon_lock);
	return ret;
}

static int en7580_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);

	if (mode != PHY_MODE_ETHERNET || submode != AIROHA_XPON_PHY_SUBMODE_XGSPON)
		return -EOPNOTSUPP;
	/* Live changes are owned by the MAC/core and require a stopped PHY. */
	if (priv->powered)
		return -EBUSY;
	priv->submode = submode;
	return 0;
}

static int en7580_phy_power_on(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct en7580_xpon_phy *soc = priv->soc_priv;
	u32 value;
	int ret;

	if (!priv->initialized)
		return -EINVAL;
	mutex_lock(&soc->xgspon_lock);
	soc->last_power_on_error = 0;
	soc->last_tx_pll_valid = false;
	soc->last_rx_error = 0;
	soc->last_eye_valid = false;
	airoha_xpon_phy_set_tx_gpio(priv, false);
	soc->xgspon_tx_requested = false;
	airoha_xpon_phy_set_vcc_enabled(priv, true);
	ret = reset_control_assert(priv->reset);
	if (ret)
		goto err_power;
	ret = regmap_update_bits(priv->scu, EN7523_SCU_WAN_CONF,
				 EN7523_SCU_WAN_MODE_MASK, 0x0a);
	if (ret)
		goto err_power;
	ret = reset_control_deassert(priv->reset);
	if (ret)
		goto err_power;
	usleep_range(1000, 1100);
	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL1,
				ECONET_SCU_PHY_CTRL1_DIS);
	if (ret)
		goto err_power;
	en7580_phy_run(priv, init);
	soc->xgspon_configured = true;
	en7580_phy_mask_interrupts(priv);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_TX_CALIB0,
			    GENMASK(25, 24) | BIT(16),
			     (soc->txp_impedance << 24) | BIT(16));
	airoha_xpon_phy_rmw(priv, EN7580_PHY_TX_CALIB1,
			    GENMASK(25, 24) | BIT(16),
			     (soc->txn_impedance << 24) | BIT(16));
	/*
	 * The vendor also programs neighboring JCPLL/XFI fields. Those remain
	 * outside this PHY's resource; their electrical clock relationship is
	 * not established by the register overlap alone.
	 */
	ret = read_poll_timeout(airoha_xpon_phy_read, value, value & BIT(16),
				100, 100000, false, priv, EN7580_PHY_TX_PLL_STATUS);
	soc->last_tx_pll_status = value;
	soc->last_tx_pll_valid = true;
	if (ret) {
		dev_err(priv->dev,
			"XGS-PON TX PLL did not lock (status %#08x)\n", value);
		goto err_power;
	}
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PCS_DEBUG, BIT(0) | BIT(1), 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_TX_FIR, 0xc11a5c20);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_MEM_CLK, GENMASK(1, 0), 3);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_TX_DELAY, GENMASK(30, 28), BIT(28));
	/* Map the existing board polarity properties onto the 10G status inputs. */
	value = !(priv->trans_invert & XPON_SETTING_RX_SD_INV) * BIT(0) |
		!!(priv->trans_invert & XPON_SETTING_TX_SD_INV) * BIT(1) |
		!!(priv->trans_invert & XPON_SETTING_TX_FAULT_INV) * BIT(2);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_SFP_LEVEL, GENMASK(2, 0), value);
	value = !!(priv->trans_invert & XPON_SETTING_BURST_EN_INV) * BIT(8) |
		!!(priv->trans_invert & XPON_SETTING_TX_FAULT_INV) * BIT(24);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PMA_SETTING0, BIT(8) | BIT(24), value);

	soc->xgspon_calibrated = false;
	soc->xgspon_rx_parked = true;
	en7580_phy_run(priv, rx_disconnect);
	/*
	 * RX calibration depends on the optical signal, not PHY power validity.
	 * Let the worker retry it while keeping local clocks on and TX disabled.
	 */
	WRITE_ONCE(priv->powered, true);
	priv->ready_reported = false;
	mod_delayed_work(system_wq, &priv->ready_work, msecs_to_jiffies(1000));
	mutex_unlock(&soc->xgspon_lock);
	dev_info(priv->dev, "EN7580 XGS-PON PHY powered; optical TX remains disabled\n");
	return 0;

err_power:
	soc->last_power_on_error = ret;
	if (soc->xgspon_configured)
		en7580_phy_quiesce(priv);
	reset_control_assert(priv->reset);
	soc->xgspon_configured = false;
	soc->xgspon_calibrated = false;
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	mutex_unlock(&soc->xgspon_lock);
	return ret;
}

static int en7580_phy_power_off(struct phy *phy)
{
	return en7580_phy_power_off_priv(phy_get_drvdata(phy));
}

static int en7580_phy_status_show(struct seq_file *s, void *unused)
{
	struct airoha_xpon_phy *priv = s->private;
	struct en7580_xpon_phy *soc = priv->soc_priv;
	static const struct {
		u16 reg;
		const char *name;
	} registers[] = {
		{ 0x312c, "rx-impedance" },
		{ 0x3554, "txp-calibration" },
		{ 0x3558, "txn-calibration" },
		{ 0x3200, "tx-pll-power0" },
		{ 0x3204, "tx-pll-power1" },
		{ 0x3600, "pma-setting0" },
		{ 0x3604, "pma-setting1" },
		{ 0x3754, "pma-mode" },
		{ 0x3760, "tx-pll-status" },
		{ 0x37a0, "clock-setting" },
		{ 0x37b0, "pma-reset" },
		{ 0x3820, "rx-frequency-status" },
		{ 0x38a0, "memory-clock" },
		{ 0x0a04, "rx-sync-control" },
		{ 0x0a0c, "pcs-reset" },
		{ 0x0b1c, "rx-sync-state" },
		{ 0x0b48, "signal-polarity" },
		{ 0x0b4c, "signal-status" },
		{ 0x0b54, "phya-ready" },
	};
	unsigned int i;

	mutex_lock(&soc->xgspon_lock);
	seq_printf(s, "initialized: %u\npowered: %u\nconfigured: %u\n",
		   priv->initialized, priv->powered, soc->xgspon_configured);
	seq_printf(s, "rx-calibrated: %u\nrx-parked: %u\ntx-requested: %u\n",
		   soc->xgspon_calibrated, soc->xgspon_rx_parked,
		   soc->xgspon_tx_requested);
	seq_printf(s, "trim-rx: %u\ntrim-txp: %u\ntrim-txn: %u\n",
		   soc->rx_impedance, soc->txp_impedance, soc->txn_impedance);
	seq_printf(s, "last-power-on-error: %d\n", soc->last_power_on_error);
	seq_printf(s, "last-rx-error: %d\n", soc->last_rx_error);
	if (soc->last_tx_pll_valid)
		seq_printf(s, "last-tx-pll-status: %#010x\n",
			   soc->last_tx_pll_status);
	if (soc->last_eye_valid)
		seq_printf(s, "last-eye-peaking: %u\nlast-eye-done: 0x%08x\nlast-eye-ready: 0x%08x\n",
			   soc->last_eye_peaking, soc->last_eye_done, soc->last_eye_ready);

	/* Never read the optical register window while held in reset. */
	if (priv->powered)
		for (i = 0; i < ARRAY_SIZE(registers); i++)
			seq_printf(s, "%04x %-22s %#010x\n", registers[i].reg,
				   registers[i].name,
				   airoha_xpon_phy_read(priv, registers[i].reg));
	mutex_unlock(&soc->xgspon_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en7580_phy_status);

static void en7580_phy_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static void en7580_phy_debugfs_init(struct airoha_xpon_phy *priv)
{
	struct dentry *dir;
	char *name;

	name = devm_kasprintf(priv->dev, GFP_KERNEL, "airoha-xpon-phy-%s",
			      dev_name(priv->dev));
	if (!name)
		return;
	dir = debugfs_create_dir(name, NULL);
	if (IS_ERR_OR_NULL(dir))
		return;
	debugfs_create_file("status", 0444, dir, priv, &en7580_phy_status_fops);
	if (devm_add_action_or_reset(priv->dev, en7580_phy_debugfs_remove, dir))
		dev_warn(priv->dev, "failed to retain PHY debugfs status\n");
}

static const struct phy_ops en7580_xgspon_phy_ops = {
	.init = en7580_phy_init,
	.exit = en7580_phy_exit,
	.set_mode = en7580_phy_set_mode,
	.power_on = en7580_phy_power_on,
	.power_off = en7580_phy_power_off,
	.owner = THIS_MODULE,
};

static int en7580_phy_read_impedance(struct device *dev, const char *name, u8 *val)
{
	struct nvmem_cell *cell;
	u8 *data;
	size_t len;

	/* Missing/unprogrammed trims use the SDK's impedance level 2. */
	*val = 2;
	if (device_property_match_string(dev, "nvmem-cell-names", name) < 0)
		return 0;
	cell = devm_nvmem_cell_get(dev, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	data = nvmem_cell_read(cell, &len);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (len != 1) {
		kfree(data);
		return -EINVAL;
	}
	if (*data > 0 && *data <= 3)
		*val = *data;
	kfree(data);
	return 0;
}

static void en7580_xpon_phy_shutdown(struct airoha_xpon_phy *priv)
{
	en7580_phy_shutdown(priv);
}

static int en7580_xpon_phy_probe(struct airoha_xpon_phy *priv,
				 struct platform_device *pdev)
{
	struct device *dev = priv->dev;
	struct en7580_xpon_phy *soc;
	int ret;

	soc = devm_kzalloc(priv->dev, sizeof(*soc), GFP_KERNEL);
	if (!soc)
		return -ENOMEM;
	priv->soc_priv = soc;
	mutex_init(&soc->xgspon_lock);

	INIT_DELAYED_WORK(&priv->ready_work, en7580_phy_ready_work);
	ret = en7580_phy_read_impedance(dev, "rx-impedance", &soc->rx_impedance);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read RX impedance trim\n");
	ret = en7580_phy_read_impedance(dev, "txp-impedance", &soc->txp_impedance);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read TXP impedance trim\n");
	ret = en7580_phy_read_impedance(dev, "txn-impedance", &soc->txn_impedance);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read TXN impedance trim\n");
	ret = reset_control_assert(priv->reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to hold PHY reset\n");
	ret = devm_add_action_or_reset(dev, en7580_phy_shutdown, priv);
	if (ret)
		return ret;
	en7580_phy_debugfs_init(priv);
	return 0;
}

static const struct airoha_xpon_phy_soc_data airoha_en7580_xpon_phy_data = {
	.name = "EN7580",
	.pma_offset = EN7580_XPON_PHY_PMA_OFFSET,
	.min_size = EN7580_XPON_PHY_PCS_MIN_SIZE,
	.default_submode = AIROHA_XPON_PHY_SUBMODE_XGSPON,
	.probe = en7580_xpon_phy_probe,
	.shutdown = en7580_xpon_phy_shutdown,
	.get_link_state = en7580_phy_get_link_state,
	.set_tx_enable = en7580_phy_set_tx_enable,
	.ops = &en7580_xgspon_phy_ops,
};

static const struct of_device_id en7580_xpon_phy_of_match[] = {
	{ .compatible = "airoha,en7580-xpon-phy", .data = &airoha_en7580_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, en7580_xpon_phy_of_match);

static struct platform_driver en7580_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-en7580-xpon-phy",
		.of_match_table = en7580_xpon_phy_of_match,
	},
};
module_platform_driver(en7580_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha EN7580 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
