// SPDX-License-Identifier: GPL-2.0-only
/* AN7583 requires a dedicated PHY sequence; now is clone from an7581 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "xpon.h"

#define AN7581_ANA_OFFSET		0xa000
#define AN7581_PMA_OFFSET		0xb000
#define AN7581_SCU_REVISION		0x05c
#define AN7581_SCU_WAN_CONF		0x070
#define AN7581_SCU_WAN_MASK		GENMASK(7, 0)
#define AN7581_SCU_PBUS_CTRL		0x92c
#define AN7581_SCU_PBUS_DISABLE		BIT(2)
#define AN7581_POLL_MS			1000

#define AN7581_TX_PHY_CLOCK		0xa060
#define AN7581_RX_IMPEDANCE		0xa114
#define AN7581_CDR_INJECTION		0xa0f4
#define AN7581_CDR_LPF			0xb820
#define AN7581_CDR_IDAC			0xb794
#define AN7581_CDR_POWER		0xb824
#define AN7581_TX_CLOCK			0xb854
#define AN7581_RX_FREQ			0xb530
#define AN7581_RX_FREQ_CTRL		0xb158
#define AN7581_RX_FLL_IDAC		0xb174
#define AN7581_RX_FLL_CTRL		0xb19c
#define AN7581_TX_PLL_STATUS		0xb420
#define AN7581_PON_CLOCK		0xb450
#define AN7581_RESET			0xb460
#define AN7581_TX_FIR0			0xb778
#define AN7581_TX_FIR1			0xb780
#define AN7581_TX_CALIB0		0xb264
#define AN7581_TX_CALIB1		0xb268
#define AN7581_SETTING			0xb270
#define AN7581_MEM_CLOCK		0xb60c
#define AN7581_TX_DELAY			0xb468
#define AN7581_INT0			0xb280
#define AN7581_INT1			0xb284
#define AN7581_BIST			0xb210
#define AN7581_TX_SOURCE		0xb414

struct an7581_xpon_phy {
	/* Protects calibration, LOS recovery, power transitions and TX gating. */
	struct mutex lock;
	u16 revision;
	bool configured;
	bool calibrated;
	bool parked;
	bool tx_requested;
	bool tx_calibration;
	u32 saved_bist;
	u32 saved_tx_source;
};

/*
 * GPON-only register sequences from en7581_pma.c: pon_CfgPhyType(),
 * pon_LinkControl() and fiber_plug_reset(). Offsets 0xa000/0xb000 select
 * the separately mapped ANA/PMA resources; 0xffff denotes a delay in us.
 * Preserve individual writes: several fields latch on a rising edge.
 */
static const struct airoha_xpon_phy_step an7581_init_seq[] = {
	{ 0xb460, 0x00000040, 0x00000000 },
	{ 0xb460, 0x00000020, 0x00000000 },
	{ 0xb460, 0x00000010, 0x00000000 },
	{ 0xb460, 0x00000008, 0x00000000 },
	{ 0xb460, 0x00000004, 0x00000000 },
	{ 0xb460, 0x00000002, 0x00000000 },
	{ 0xb460, 0x00000001, 0x00000000 },
	{ 0xb828, 0x01000000, 0x01000000 },
	{ 0xb828, 0x00010000, 0x00000000 },
	{ 0xb79c, 0x00010000, 0x00010000 },
	{ 0xb800, 0xffffffff, 0x25800000 },
	{ 0xb854, 0x01000000, 0x01000000 },
	{ 0xb854, 0x00010000, 0x00000000 },
	{ 0xb034, 0x00000003, 0x00000001 },
	{ 0xb02c, 0x00000f00, 0x00000500 },
	{ 0xa0c8, 0x00030000, 0x00000000 },
	{ 0xa0d8, 0x01000000, 0x01000000 },
	{ 0xb004, 0xff000000, 0x01000000 },
	{ 0xb004, 0x00ff0000, 0x00100000 },
	{ 0xb004, 0x0000ff00, 0x00000a00 },
	{ 0xb004, 0x00000001, 0x00000001 },
	{ 0xb444, 0xffff0000, 0x01130000 },
	{ 0xb444, 0x0000ffff, 0x000000fa },
	{ 0xb448, 0xffff0000, 0x009b0000 },
	{ 0xb448, 0x0000ffff, 0x00000210 },
	{ 0xb440, 0xffff0000, 0x00040000 },
	{ 0xb440, 0x0000ffff, 0x00000026 },
	{ 0xb468, 0x00ff0000, 0x00020000 },
	{ 0xb028, 0x00000700, 0x00000300 },
	{ 0xb024, 0x01000000, 0x00000000 },
	{ 0xb794, 0x01000000, 0x01000000 },
	{ 0xb034, 0x00000100, 0x00000100 },
	{ 0xb798, 0x7fffffff, 0x07f66e86 },
	{ 0xb048, 0x7fffffff, 0x0fecdd0c },
	{ 0xb04c, 0x7fffffff, 0x0fecdd0c },
	{ 0xa0c4, 0x01000000, 0x01000000 },
	{ 0xa0c4, 0x00000001, 0x00000001 },
	{ 0xa000, 0x00000001, 0x00000001 },
	{ 0xa000, 0x00070000, 0x00040000 },
	{ 0xa000, 0x00003f00, 0x00000100 },
	{ 0xb874, 0x01000000, 0x01000000 },
	{ 0xb874, 0x00010000, 0x00010000 },
	{ 0xb77c, 0x01000000, 0x01000000 },
	{ 0xb77c, 0x000f0000, 0x00020000 },
	{ 0xb784, 0x00000100, 0x00000100 },
	{ 0xb784, 0x00000003, 0x00000000 },
	{ 0xb580, 0x00000003, 0x00000000 },
	{ 0xb468, 0x0000007f, 0x00000004 },
	{ 0xb08c, 0x00ffff00, 0x00000100 },
	{ 0xb104, 0x0000ffff, 0x00000002 },
	{ 0xb08c, 0x00000007, 0x00000001 },
	{ 0xb090, 0x0000ffff, 0x00000002 },
	{ 0xb090, 0xffff0000, 0x03e80000 },
	{ 0xb09c, 0x0000ffff, 0x00000002 },
	{ 0xb09c, 0xffff0000, 0x03e80000 },
	{ 0xb094, 0x0000ffff, 0x00000002 },
	{ 0xb094, 0xffff0000, 0x03e80000 },
	{ 0xb098, 0x0000ffff, 0x00000002 },
	{ 0xb098, 0xffff0000, 0x03e80000 },
	{ 0xb120, 0x00001f00, 0x00000100 },
	{ 0xb120, 0x00000003, 0x00000003 },
	{ 0xb068, 0x07ff0000, 0x02400000 },
	{ 0xb068, 0x000007ff, 0x000001c0 },
	{ 0xb088, 0x00000100, 0x00000000 },
	{ 0xb088, 0x00000001, 0x00000001 },
	{ 0xa0d4, 0xffff0000, 0xcccb0000 },
	{ 0xa144, 0x01000000, 0x01000000 },
	{ 0xa148, 0x00000001, 0x00000001 },
	{ 0xa148, 0x00000100, 0x00000100 },
	{ 0xa148, 0x00010000, 0x00010000 },
	{ 0xa148, 0x01000000, 0x01000000 },
	{ 0xa13c, 0x000fff00, 0x00020000 },
	{ 0xa120, 0x0003ff00, 0x0003ff00 },
	{ 0xa0dc, 0x000001ff, 0x00000000 },
	{ 0xa0dc, 0x00000001, 0x00000000 },
	{ 0xb80c, 0x01000000, 0x01000000 },
	{ 0xb80c, 0x00010000, 0x00000000 },
	{ 0xb814, 0x01000000, 0x01000000 },
	{ 0xb814, 0x00010000, 0x00010000 },
	{ 0xa10c, 0x1f000000, 0x00000000 },
	{ 0xa0d8, 0x00000300, 0x00000100 },
	{ 0xb8b8, 0x00000001, 0x00000001 },
	{ 0xb958, 0x10000000, 0x10000000 },
	{ 0xb958, 0x20000000, 0x20000000 },
	{ 0xb958, 0x40000000, 0x40000000 },
	{ 0xb958, 0x80000000, 0x80000000 },
	{ 0xa10c, 0x00000004, 0x00000000 },
	{ 0xb360, 0xffffffff, 0x00000300 },
	{ 0xa0cc, 0x01000000, 0x00000000 },
	{ 0xa0cc, 0x00010000, 0x00000000 },
	{ 0xa0d8, 0x000000ff, 0x00000000 },
	{ 0xa0d8, 0x00010000, 0x00000000 },
	{ 0xa0e8, 0x00000003, 0x00000002 },
	{ 0xa0cc, 0x00000100, 0x00000100 },
	{ 0xa0cc, 0x00000001, 0x00000001 },
	{ 0xb76c, 0x01000000, 0x01000000 },
	{ 0xb76c, 0x00030000, 0x00020000 },
	{ 0xb374, 0x00000003, 0x00000000 },
	{ 0xa048, 0x0000ff00, 0x00002000 },
	{ 0xa048, 0x000000ff, 0x000000ff },
	{ 0xa01c, 0x00000007, 0x00000004 },
	{ 0xa01c, 0x00000100, 0x00000100 },
	{ 0xa01c, 0x00010000, 0x00000000 },
	{ 0xa01c, 0x03000000, 0x00000000 },
	{ 0xa020, 0x00000001, 0x00000000 },
	{ 0xa020, 0x00000300, 0x00000000 },
	{ 0xa020, 0x00030000, 0x00030000 },
	{ 0xa020, 0x01000000, 0x00000000 },
	{ 0xa024, 0x00000001, 0x00000000 },
	{ 0xa038, 0x0000ffff, 0x00000000 },
	{ 0xa038, 0xffff0000, 0x00000000 },
	{ 0xa034, 0x00000001, 0x00000000 },
	{ 0xa034, 0x00ffff00, 0x00000000 },
	{ 0xa030, 0x00020000, 0x00000000 },
	{ 0xa030, 0x00000700, 0x00000000 },
	{ 0xa030, 0x00010000, 0x00000000 },
	{ 0xa004, 0x003f0000, 0x00180000 },
	{ 0xa004, 0x3f000000, 0x00000000 },
	{ 0xa004, 0x00000100, 0x00000000 },
	{ 0xa008, 0x1f000000, 0x00000000 },
	{ 0xa008, 0x001f0000, 0x00100000 },
	{ 0xa008, 0x00001f00, 0x00001f00 },
	{ 0xa008, 0x0000001f, 0x0000000a },
	{ 0xa00c, 0x0000001f, 0x00000000 },
	{ 0xa02c, 0x00000300, 0x00000100 },
	{ 0xa02c, 0x07000000, 0x04000000 },
	{ 0xa02c, 0x00010000, 0x00010000 },
	{ 0xa030, 0x00000038, 0x00000018 },
	{ 0xa030, 0x00000700, 0x00000000 },
	{ 0xa030, 0x00000007, 0x00000003 },
	{ 0xa014, 0x00000003, 0x00000000 },
	{ 0xa014, 0x01000000, 0x00000000 },
	{ 0xa02c, 0x00000003, 0x00000000 },
	{ 0xa010, 0x00000003, 0x00000000 },
	{ 0xa010, 0x00000300, 0x00000300 },
	{ 0xa010, 0x00030000, 0x00000000 },
	{ 0xa00c, 0x07000000, 0x02000000 },
	{ 0xa00c, 0x00ff0000, 0x00e40000 },
	{ 0xa00c, 0x00000100, 0x00000000 },
	{ 0xa04c, 0x00000100, 0x00000000 },
	{ 0xb884, 0x00000100, 0x00000100 },
	{ 0xb884, 0x00000001, 0x00000000 },
	{ 0xa048, 0x001f0000, 0x00100000 },
	{ 0xa024, 0x00070000, 0x00010000 },
	{ 0xa024, 0x1f000000, 0x05000000 },
	{ 0xa024, 0x00000100, 0x00000100 },
	{ 0xa028, 0x07000000, 0x01000000 },
	{ 0xa028, 0x00010000, 0x00010000 },
	{ 0xa028, 0x00000700, 0x00000000 },
	{ 0xa028, 0x00000001, 0x00000001 },
	{ 0xa0a0, 0x01000000, 0x01000000 },
	{ 0xa0a4, 0x00010000, 0x00000000 },
	{ 0xa0a4, 0x01000000, 0x00000000 },
	{ 0xa0a8, 0x00000001, 0x00000000 },
	{ 0xa084, 0x03000000, 0x01000000 },
	{ 0xa084, 0x00030000, 0x00010000 },
	{ 0xa088, 0x03000000, 0x00000000 },
	{ 0xa088, 0x00010000, 0x00000000 },
	{ 0xa088, 0x00000700, 0x00000000 },
	{ 0xa088, 0x00000001, 0x00000001 },
	{ 0xa064, 0x01000000, 0x01000000 },
	{ 0xa064, 0x00070000, 0x00040000 },
	{ 0xa064, 0x00000001, 0x00000001 },
	{ 0xa064, 0x00000300, 0x00000000 },
	{ 0xa068, 0x03000000, 0x00000000 },
	{ 0xa068, 0x00010000, 0x00010000 },
	{ 0xa068, 0x00000300, 0x00000000 },
	{ 0xa068, 0x00000001, 0x00000000 },
	{ 0xa06c, 0x00010000, 0x00010000 },
	{ 0xa06c, 0x00000100, 0x00000000 },
	{ 0xa06c, 0x00000003, 0x00000003 },
	{ 0xa080, 0xffff0000, 0x00000000 },
	{ 0xa080, 0x0000ffff, 0x00000000 },
	{ 0xa07c, 0x00010000, 0x00000000 },
	{ 0xa07c, 0x00000100, 0x00000000 },
	{ 0xa07c, 0x00000001, 0x00000000 },
	{ 0xa084, 0x0000ffff, 0x00000000 },
	{ 0xa050, 0x1f000000, 0x1f000000 },
	{ 0xa050, 0x001f0000, 0x00050000 },
	{ 0xa050, 0x00003f00, 0x00000000 },
	{ 0xa050, 0x0000003f, 0x0000000c },
	{ 0xa054, 0x001f0000, 0x00180000 },
	{ 0xa054, 0x00001f00, 0x00000b00 },
	{ 0xa054, 0x0000001f, 0x00000002 },
	{ 0xa04c, 0x02000000, 0x00000000 },
	{ 0xa074, 0x03000000, 0x03000000 },
	{ 0xa078, 0x00000001, 0x00000001 },
	{ 0xa078, 0x00000700, 0x00000700 },
	{ 0xa078, 0x07000000, 0x04000000 },
	{ 0xa078, 0x38000000, 0x00000000 },
	{ 0xa078, 0x00070000, 0x00040000 },
	{ 0xa058, 0x03000000, 0x03000000 },
	{ 0xa058, 0x00030000, 0x00000000 },
	{ 0xa05c, 0x00000003, 0x00000001 },
	{ 0xa058, 0x00000700, 0x00000400 },
	{ 0xa058, 0x000000ff, 0x000000e4 },
	{ 0xa054, 0x01000000, 0x00000000 },
	{ 0xa094, 0x01000000, 0x00000000 },
	{ 0xb858, 0x00000100, 0x00000100 },
	{ 0xb858, 0x00000001, 0x00000000 },
	{ 0xa05c, 0x00030000, 0x00000000 },
	{ 0xa05c, 0x00000100, 0x00000100 },
	{ 0xa074, 0x00030000, 0x00000000 },
	{ 0xa094, 0x0000001f, 0x0000000f },
	{ 0xa070, 0x00000007, 0x00000003 },
	{ 0xa070, 0x00001f00, 0x00000b00 },
	{ 0xa074, 0x00000700, 0x00000000 },
	{ 0xa074, 0x00000001, 0x00000001 },
	{ 0xa06c, 0x01000000, 0x01000000 },
	{ 0xa0b4, 0x00000003, 0x00000002 },
	{ 0xa0c0, 0x00000001, 0x00000000 },
	{ 0xa0c0, 0x00070000, 0x00020000 },
	{ 0xa0c0, 0x07000000, 0x02000000 },
	{ 0xb778, 0x01000000, 0x01000000 },
	{ 0xb778, 0x00000100, 0x00000100 },
	{ 0xb780, 0x00000100, 0x00000100 },
	{ 0xb780, 0x01000000, 0x01000000 },
	{ 0xb778, 0x001f0000, 0x00000000 },
	{ 0xb778, 0x0000003f, 0x00000000 },
	{ 0xb780, 0x0000001f, 0x00000000 },
	{ 0xb780, 0x00070000, 0x00000000 },
	{ 0xa118, 0x01000000, 0x01000000 },
	{ 0xa118, 0x00010000, 0x00010000 },
	{ 0xa118, 0x00000100, 0x00000100 },
	{ 0xa11c, 0x00000700, 0x00000400 },
	{ 0xa118, 0x00000001, 0x00000000 },
	{ 0xa11c, 0x00000001, 0x00000001 },
	{ 0xa0e8, 0x07ffff00, 0x00800000 },
	{ 0xa0e0, 0x0007ffff, 0x00078000 },
	{ 0xa100, 0x00010000, 0x00000000 },
	{ 0xa110, 0x00000300, 0x00000000 },
	{ 0xb074, 0x00010000, 0x00010000 },
	{ 0xb078, 0x0000ff00, 0x00000400 },
	{ 0xb078, 0x000000ff, 0x00000004 },
	{ 0xb07c, 0x00ff0000, 0x00040000 },
	{ 0xb07c, 0x000007ff, 0x00000004 },
	{ 0xb080, 0xff000000, 0xff000000 },
	{ 0xb080, 0x000003ff, 0x000000d0 },
	{ 0xb118, 0x000000ff, 0x00000001 },
	{ 0xb118, 0x0007ff00, 0x00000000 },
	{ 0xb118, 0xff000000, 0x00000000 },
	{ 0xb11c, 0x00000001, 0x00000001 },
	{ 0xb160, 0x03000000, 0x01000000 },
	{ 0xb160, 0x0000ff00, 0x00002e00 },
	{ 0xb160, 0x00000001, 0x00000001 },
	{ 0xb160, 0x00030000, 0x00010000 },
	{ 0xb164, 0x00000001, 0x00000000 },
	{ 0xb164, 0x00000f00, 0x00000000 },
	{ 0xb164, 0x00030000, 0x00000000 },
	{ 0xb100, 0x0000ffff, 0x00000005 },
	{ 0xb100, 0xffff0000, 0x000a0000 },
	{ 0xb144, 0x000000ff, 0x00000000 },
	{ 0xb144, 0x00000100, 0x00000000 },
	{ 0xb06c, 0x0000007f, 0x00000040 },
	{ 0xb06c, 0x00007f00, 0x00003f00 },
	{ 0xb06c, 0x00010000, 0x00010000 },
	{ 0xb070, 0x000fffff, 0x00000018 },
	{ 0xb48c, 0x0000ff00, 0x00000200 },
	{ 0xb48c, 0x00000002, 0x00000002 },
	{ 0xb48c, 0x00000001, 0x00000000 },
	{ 0xb170, 0x0007ff00, 0x0000a500 },
	{ 0xb170, 0x00000007, 0x00000003 },
	{ 0xb170, 0x03000000, 0x00000000 },
	{ 0xb174, 0x07000000, 0x04000000 },
	{ 0xb174, 0x00070000, 0x00010000 },
	{ 0xb184, 0x000007ff, 0x000001ff },
	{ 0xb184, 0x07ff0000, 0x04000000 },
	{ 0xb178, 0x00000700, 0x00000400 },
	{ 0xb178, 0x00000007, 0x00000003 },
	{ 0xb320, 0x01000000, 0x00000000 },
	{ 0xb200, 0x00000001, 0x00000001 },
	{ 0xb200, 0x00000100, 0x00000000 },
	{ 0xb148, 0x00000001, 0x00000001 },
	{ 0xb148, 0x00000f00, 0x00000a00 },
	{ 0xb148, 0x007f0000, 0x00700000 },
	{ 0xb148, 0x3f800000, 0x08000000 },
	{ 0xb828, 0x01000000, 0x01000000 },
	{ 0xb828, 0x00010000, 0x00010000 },
	{ 0xb828, 0x00000100, 0x00000100 },
	{ 0xb828, 0x00000001, 0x00000001 },
	{ 0xffff, 0x00000000, 0x000001f4 },
	{ 0xb854, 0x01000000, 0x01000000 },
	{ 0xb854, 0x00010000, 0x00010000 },
	{ 0xb000, 0x01000000, 0x01000000 },
	{ 0xffff, 0x00000000, 0x00000006 },
	{ 0xa04c, 0x00000001, 0x00000001 },
	{ 0xb854, 0x00000100, 0x00000100 },
	{ 0xb854, 0x00000001, 0x00000001 },
	{ 0xa094, 0x00010000, 0x00010000 },
	{ 0xa094, 0x00000100, 0x00000000 },
	{ 0xa060, 0x00000100, 0x00000100 },
	{ 0xa060, 0x00000001, 0x00000001 },
	{ 0xa04c, 0x00000001, 0x00000000 },
	{ 0xa04c, 0x01000000, 0x00000000 },
	{ 0xa04c, 0x00010000, 0x00000000 },
	{ 0xffff, 0x00000000, 0x000001f4 },
	{ 0xb14c, 0xffff0000, 0x7fff0000 },
	{ 0xb14c, 0x0000ffff, 0x00007fff },
	{ 0xb150, 0xffff0000, 0xa4ff0000 },
	{ 0xb150, 0x0000ffff, 0x0000a436 },
	{ 0xb154, 0xffff0000, 0xa4ff0000 },
	{ 0xb154, 0x0000ffff, 0x0000a436 },
	{ 0xb260, 0x00000001, 0x00000001 },
	{ 0xb410, 0x00000100, 0x00000100 },
	{ 0xb410, 0x00000001, 0x00000001 },
	{ 0xb260, 0x00000100, 0x00000100 },
	{ 0xb260, 0x00000001, 0x00000001 },
	{ 0xffff, 0x00000000, 0x00000064 },
};

static const struct airoha_xpon_phy_step an7581_rx_start_seq[] = {
	{ 0xa114, 0x00000300, 0x00000200 },
	{ 0xa114, 0x001f0000, 0x00020000 },
	{ 0xa110, 0x03000000, 0x03000000 },
	{ 0xa10c, 0x00080000, 0x00000000 },
	{ 0xa10c, 0x00070000, 0x00070000 },
	{ 0xb114, 0x00000100, 0x00000000 },
	{ 0xb10c, 0x00000100, 0x00000000 },
	{ 0xb818, 0x00000001, 0x00000000 },
	{ 0xb818, 0x00000100, 0x00000000 },
	{ 0xb768, 0x01000000, 0x01000000 },
	{ 0xb768, 0x000f0000, 0x00040000 },
	{ 0xb330, 0x00000100, 0x00000100 },
	{ 0xb33c, 0x00000100, 0x00000000 },
	{ 0xb330, 0x00010000, 0x00000000 },
	{ 0xb33c, 0x00010000, 0x00000000 },
	{ 0xb338, 0x00000001, 0x00000000 },
	{ 0xb32c, 0x00000001, 0x00000000 },
	{ 0xb10c, 0x00010000, 0x00010000 },
	{ 0xb114, 0x00010000, 0x00010000 },
	{ 0xb794, 0x01000000, 0x01000000 },
	{ 0xb034, 0x00000100, 0x00000100 },
	{ 0xb864, 0x00000100, 0x00000100 },
	{ 0xb864, 0x00000001, 0x00000000 },
	{ 0xb864, 0x00000001, 0x00000001 },
	{ 0xb02c, 0x00000001, 0x00000000 },
	{ 0xb010, 0x00000001, 0x00000000 },
	{ 0xb88c, 0x00000100, 0x00000100 },
	{ 0xb88c, 0x00000003, 0x00000003 },
	{ 0xa0d4, 0x0000ffff, 0x000018b0 },
	{ 0xb824, 0x01000000, 0x01000000 },
	{ 0xb824, 0x00010000, 0x00010000 },
	{ 0xb824, 0x00000100, 0x00000100 },
	{ 0xb824, 0x00000001, 0x00000001 },
	{ 0xb81c, 0x01000000, 0x00000000 },
	{ 0xb81c, 0x00010000, 0x00000000 },
	{ 0xb81c, 0x00000100, 0x00000100 },
	{ 0xb81c, 0x00000001, 0x00000001 },
	{ 0xb894, 0x00000100, 0x00000100 },
	{ 0xb894, 0x00000001, 0x00000001 },
	{ 0xb84c, 0x01000000, 0x01000000 },
	{ 0xb84c, 0x00010000, 0x00010000 },
	{ 0xb84c, 0x00000100, 0x00000000 },
	{ 0xb84c, 0x00000001, 0x00000000 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xb34c, 0x01000000, 0x01000000 },
	{ 0xb34c, 0x00010000, 0x00010000 },
	{ 0xb34c, 0x00000100, 0x00000100 },
	{ 0xb34c, 0x00000001, 0x00000001 },
	{ 0xb350, 0x00000001, 0x00000001 },
	{ 0xb38c, 0x00000003, 0x00000001 },
	{ 0xa0fc, 0x03000000, 0x00000000 },
	{ 0xa0fc, 0x001f0000, 0x00080000 },
	{ 0xa0fc, 0x00000700, 0x00000500 },
	{ 0xa0fc, 0x00000007, 0x00000005 },
	{ 0xb108, 0x00000100, 0x00000000 },
	{ 0xb108, 0x00010000, 0x00000000 },
	{ 0xb108, 0x01000000, 0x00000000 },
	{ 0xb10c, 0x00000001, 0x00000000 },
	{ 0xb108, 0x00000001, 0x00000000 },
	{ 0xb10c, 0x00010000, 0x00000000 },
	{ 0xb808, 0x00000100, 0x00000100 },
	{ 0xb808, 0x00000001, 0x00000000 },
	{ 0xa100, 0x00000003, 0x00000000 },
	{ 0xa108, 0x03000000, 0x00000000 },
	{ 0xb460, 0x00000002, 0x00000002 },
	{ 0xb460, 0x00000020, 0x00000020 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb818, 0x00010000, 0x00010000 },
};

static const struct airoha_xpon_phy_step an7581_rx_cal_seq[] = {
	{ 0xb818, 0x00000001, 0x00000000 },
	{ 0xb818, 0x00000100, 0x00000100 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xb460, 0x00000020, 0x00000020 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb33c, 0x00000001, 0x00000000 },
	{ 0xb330, 0x00000001, 0x00000001 },
	{ 0xb83c, 0x01000000, 0x01000000 },
	{ 0xb83c, 0x00010000, 0x00010000 },
	{ 0xb840, 0x01000000, 0x01000000 },
	{ 0xb840, 0x00010000, 0x00010000 },
	{ 0xb840, 0x00000100, 0x00000100 },
	{ 0xb840, 0x00000001, 0x00000001 },
	{ 0xffff, 0x00000000, 0x000000c8 },
	{ 0xb108, 0x00000001, 0x00000000 },
	{ 0xb10c, 0x00000100, 0x00000000 },
	{ 0xb110, 0x00000001, 0x00000000 },
	{ 0xb114, 0x00000100, 0x00000000 },
	{ 0xb110, 0x00000001, 0x00000001 },
	{ 0xb308, 0x01000000, 0x01000000 },
	{ 0xb15c, 0x00000700, 0x00000400 },
	{ 0xb118, 0x000000ff, 0x00000008 },
	{ 0xb204, 0x00010000, 0x00000000 },
	{ 0xb328, 0x00000100, 0x00000000 },
	{ 0xb334, 0x00000100, 0x00000000 },
	{ 0xb328, 0x00000001, 0x00000000 },
	{ 0xb334, 0x00000001, 0x00000000 },
	{ 0xb31c, 0x00000001, 0x00000000 },
	{ 0xb318, 0x00000100, 0x00000000 },
	{ 0xb324, 0x01000000, 0x00000000 },
	{ 0xb110, 0x00000100, 0x00000000 },
	{ 0xb108, 0x00000100, 0x00000000 },
	{ 0xb30c, 0x00000001, 0x00000000 },
	{ 0xb204, 0x00010000, 0x00010000 },
	{ 0xb328, 0x00000100, 0x00000100 },
	{ 0xb328, 0x00000001, 0x00000001 },
	{ 0xb318, 0x00000100, 0x00000100 },
	{ 0xb110, 0x00000100, 0x00000100 },
	{ 0xffff, 0x00000000, 0x000000c8 },
	{ 0xb110, 0x00000100, 0x00000000 },
	{ 0xb318, 0x00000100, 0x00000000 },
	{ 0xb30c, 0x00000001, 0x00000001 },
	{ 0xb894, 0x01000000, 0x01000000 },
	{ 0xb894, 0x00010000, 0x00010000 },
	{ 0xb114, 0x00000100, 0x00000000 },
	{ 0xb10c, 0x00000100, 0x00000000 },
	{ 0xb304, 0x00000100, 0x00000100 },
	{ 0xb304, 0x00010000, 0x00010000 },
	{ 0xb304, 0x01000000, 0x01000000 },
	{ 0xb308, 0x00000001, 0x00000001 },
	{ 0xb308, 0x00000100, 0x00000100 },
	{ 0xb32c, 0x00000001, 0x00000000 },
	{ 0xb338, 0x00000001, 0x00000000 },
	{ 0xb084, 0x01000000, 0x00000000 },
	{ 0xb084, 0x00000100, 0x00000000 },
	{ 0xb32c, 0x01000000, 0x00000000 },
	{ 0xb338, 0x01000000, 0x00000000 },
	{ 0xb084, 0x00010000, 0x00000000 },
	{ 0xb084, 0x00000001, 0x00000000 },
	{ 0xb200, 0x00070000, 0x00020000 },
	{ 0xb328, 0x00010000, 0x00000000 },
	{ 0xb334, 0x00010000, 0x00000000 },
	{ 0xb208, 0x00000001, 0x00000000 },
	{ 0xb110, 0x00010000, 0x00000000 },
	{ 0xb108, 0x00010000, 0x00000000 },
	{ 0xb110, 0x00000001, 0x00000000 },
	{ 0xb108, 0x00000001, 0x00000000 },
	{ 0xb328, 0x00010000, 0x00010000 },
	{ 0xb208, 0x00000001, 0x00000001 },
	{ 0xb110, 0x00010000, 0x00010000 },
	{ 0xffff, 0x00000000, 0x000000c8 },
	{ 0xb110, 0x00010000, 0x00000000 },
	{ 0xb110, 0x00000001, 0x00000000 },
	{ 0xb084, 0x01000000, 0x00000000 },
	{ 0xb084, 0x00000100, 0x00000100 },
	{ 0xb32c, 0x01000000, 0x00000000 },
	{ 0xb338, 0x01000000, 0x01000000 },
	{ 0xb084, 0x00010000, 0x00000000 },
	{ 0xb084, 0x00000001, 0x00000001 },
	{ 0xb894, 0x01000000, 0x01000000 },
	{ 0xb894, 0x00010000, 0x00000000 },
};

static const struct airoha_xpon_phy_step an7581_rx_feos_seq[] = {
	{ 0xb114, 0x00000100, 0x00000000 },
	{ 0xb10c, 0x00000100, 0x00000000 },
	{ 0xb308, 0x00010000, 0x00010000 },
	{ 0xb32c, 0x00000001, 0x00000000 },
	{ 0xb338, 0x00000001, 0x00000000 },
	{ 0xb144, 0x000000ff, 0x00000030 },
	{ 0xb32c, 0x00010000, 0x00000000 },
	{ 0xb338, 0x00010000, 0x00000000 },
	{ 0xb204, 0x00000100, 0x00000000 },
	{ 0xb110, 0x01000000, 0x00000000 },
	{ 0xb108, 0x01000000, 0x00000000 },
	{ 0xb110, 0x00000001, 0x00000001 },
	{ 0xb108, 0x00000001, 0x00000000 },
	{ 0xb32c, 0x00010000, 0x00010000 },
	{ 0xb204, 0x00000100, 0x00000100 },
	{ 0xb110, 0x01000000, 0x01000000 },
	{ 0xffff, 0x00000000, 0x000003e8 },
	{ 0xb110, 0x01000000, 0x00000000 },
	{ 0xb110, 0x00000001, 0x00000000 },
};

static const struct airoha_xpon_phy_step an7581_rx_finish_seq[] = {
	{ 0xb898, 0x00000100, 0x00000100 },
	{ 0xb898, 0x00000001, 0x00000000 },
	{ 0xb840, 0x00000100, 0x00000100 },
	{ 0xb840, 0x00000001, 0x00000001 },
	{ 0xb204, 0x01000000, 0x00000000 },
	{ 0xb32c, 0x00000100, 0x00000000 },
	{ 0xb10c, 0x00000001, 0x00000000 },
	{ 0xb338, 0x00000100, 0x00000000 },
	{ 0xb114, 0x00000001, 0x00000000 },
	{ 0xb204, 0x01000000, 0x01000000 },
	{ 0xb32c, 0x00000100, 0x00000100 },
	{ 0xb114, 0x00000001, 0x00000001 },
	{ 0xffff, 0x00000000, 0x000000c8 },
	{ 0xb114, 0x00000001, 0x00000000 },
	{ 0xb898, 0x00000100, 0x00000100 },
	{ 0xb898, 0x00000001, 0x00000000 },
	{ 0xb83c, 0x01000000, 0x01000000 },
	{ 0xb83c, 0x00010000, 0x00000000 },
	{ 0xb840, 0x01000000, 0x01000000 },
	{ 0xb840, 0x00010000, 0x00000000 },
	{ 0xb840, 0x00000100, 0x00000100 },
	{ 0xb840, 0x00000001, 0x00000000 },
	{ 0xb114, 0x00000100, 0x00000100 },
	{ 0xb10c, 0x00000100, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000001 },
	{ 0xb460, 0x00000020, 0x00000020 },
	{ 0xb460, 0x00000002, 0x00000002 },
	{ 0xb460, 0x00000004, 0x00000004 },
	{ 0xb460, 0x00000008, 0x00000008 },
	{ 0xb460, 0x00000001, 0x00000001 },
	{ 0xb460, 0x00000040, 0x00000040 },
	{ 0xb460, 0x00000010, 0x00000010 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xffff, 0x00000000, 0x000000c8 },
	{ 0xb818, 0x00000001, 0x00000001 },
	{ 0xb818, 0x00000100, 0x00000100 },
	{ 0xb040, 0x00000300, 0x00000000 },
	{ 0xb028, 0x00000003, 0x00000001 },
	{ 0xb864, 0x00000100, 0x00000000 },
	{ 0xb794, 0x01000000, 0x00000000 },
	{ 0xb024, 0x00010000, 0x00010000 },
	{ 0xb034, 0x00000100, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb03c, 0x00010000, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb02c, 0x00000001, 0x00000001 },
	{ 0xb010, 0x00000001, 0x00000001 },
	{ 0xffff, 0x00000000, 0x000001f4 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xb114, 0x01000000, 0x01000000 },
	{ 0xb10c, 0x01000000, 0x00000000 },
	{ 0xb460, 0x00000001, 0x00000000 },
	{ 0xb460, 0x00000001, 0x00000001 },
	{ 0xb214, 0xffffffff, 0x8ff1fd53 },
	{ 0xb224, 0xffffffff, 0x0ff1fd53 },
	{ 0xb230, 0x0000ffff, 0x00000001 },
	{ 0xb210, 0x0000001f, 0x00000005 },
	{ 0xb220, 0x00010000, 0x00010000 },
	{ 0xb344, 0x00000001, 0x00000000 },
	{ 0xb344, 0x01000000, 0x00000000 },
	{ 0xb414, 0x00010000, 0x00000000 },
	{ 0xb414, 0x00000100, 0x00000000 },
};

static const struct airoha_xpon_phy_step an7581_rx_connect_seq[] = {
	{ 0xb460, 0x00000040, 0x00000000 },
	{ 0xb460, 0x00000020, 0x00000000 },
	{ 0xb460, 0x00000010, 0x00000000 },
	{ 0xb460, 0x00000008, 0x00000000 },
	{ 0xb460, 0x00000004, 0x00000000 },
	{ 0xb460, 0x00000002, 0x00000000 },
	{ 0xb460, 0x00000001, 0x00000000 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xffff, 0x00000000, 0x000000c8 },
	{ 0xb818, 0x00000001, 0x00000001 },
	{ 0xb818, 0x00000100, 0x00000100 },
	{ 0xb040, 0x00000300, 0x00000000 },
	{ 0xb028, 0x00000003, 0x00000001 },
	{ 0xb864, 0x00000100, 0x00000000 },
	{ 0xb794, 0x01000000, 0x00000000 },
	{ 0xb024, 0x00010000, 0x00010000 },
	{ 0xb034, 0x00000100, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb03c, 0x00010000, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb02c, 0x00000001, 0x00000001 },
	{ 0xb010, 0x00000001, 0x00000001 },
	{ 0xffff, 0x00000000, 0x000001f4 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xb460, 0x00000020, 0x00000020 },
	{ 0xb460, 0x00000002, 0x00000002 },
	{ 0xb460, 0x00000004, 0x00000004 },
	{ 0xb460, 0x00000008, 0x00000008 },
	{ 0xb460, 0x00000001, 0x00000001 },
	{ 0xb460, 0x00000040, 0x00000040 },
	{ 0xb460, 0x00000010, 0x00000010 },
	{ 0xb114, 0x01000000, 0x01000000 },
	{ 0xb10c, 0x01000000, 0x00000000 },
	{ 0xb460, 0x00000001, 0x00000000 },
	{ 0xb460, 0x00000001, 0x00000001 },
	{ 0xb114, 0x00000100, 0x00000100 },
	{ 0xb10c, 0x00000100, 0x00000000 },
	{ 0xffff, 0x00000000, 0x00000001 },
	{ 0xb414, 0x00010000, 0x00000000 },
	{ 0xb414, 0x00000100, 0x00000000 },
};

static const struct airoha_xpon_phy_step an7581_rx_disconnect_seq[] = {
	{ 0xb794, 0x01000000, 0x01000000 },
	{ 0xb034, 0x00000100, 0x00000100 },
	{ 0xb864, 0x00000100, 0x00000100 },
	{ 0xb864, 0x00000001, 0x00000000 },
	{ 0xb864, 0x00000001, 0x00000001 },
	{ 0xb02c, 0x00000001, 0x00000000 },
	{ 0xb010, 0x00000001, 0x00000000 },
	{ 0xb818, 0x00000001, 0x00000000 },
	{ 0xb818, 0x00000100, 0x00000100 },
	{ 0xffff, 0x00000000, 0x00000064 },
	{ 0xb818, 0x01000000, 0x01000000 },
	{ 0xb818, 0x00010000, 0x00000000 },
	{ 0xb818, 0x00010000, 0x00010000 },
	{ 0xb17c, 0x00000001, 0x00000000 },
	{ 0xffff, 0x00000000, 0x000001f4 },
	{ 0xb17c, 0x00000001, 0x00000001 },
	{ 0xb17c, 0x00000100, 0x00000000 },
	{ 0xffff, 0x00000000, 0x000001f4 },
	{ 0xb17c, 0x00000100, 0x00000100 },
};

#define an7581_run(priv, name) \
	airoha_xpon_phy_run(priv, an7581_##name##_seq, \
			    ARRAY_SIZE(an7581_##name##_seq))

static bool an7581_los(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, XPON_TRANS_STATUS) &
		XPON_TRANS_STATUS_LOS;
}

static bool an7581_ready(struct airoha_xpon_phy *priv)
{
	struct an7581_xpon_phy *an = priv->soc_priv;

	return an->calibrated && !an->parked && !an7581_los(priv) &&
		FIELD_GET(XPON_PHYSTA1_STATE_MASK,
			  airoha_xpon_phy_read(priv, XPON_PHYSTA1)) ==
		XPON_PHYSTA1_READY;
}

static void an7581_update_tx(struct airoha_xpon_phy *priv)
{
	struct an7581_xpon_phy *an = priv->soc_priv;

	airoha_xpon_phy_set_tx_gpio(priv, priv->powered &&
				    an->tx_requested && an7581_ready(priv));
}

static int an7581_get_link_state(struct airoha_xpon_phy *priv,
				 bool *ready, bool *los)
{
	struct an7581_xpon_phy *an = priv->soc_priv;

	mutex_lock(&an->lock);
	*los = !priv->powered || an7581_los(priv);
	*ready = priv->powered && an7581_ready(priv);
	mutex_unlock(&an->lock);
	return 0;
}

static int an7581_set_tx_enable(struct airoha_xpon_phy *priv, bool enable)
{
	struct an7581_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	mutex_lock(&an->lock);
	if (enable && !priv->powered) {
		ret = -EHOSTDOWN;
		goto out;
	}
	an->tx_requested = enable;
	an7581_update_tx(priv);
out:
	mutex_unlock(&an->lock);
	return ret;
}

static void an7581_mask_interrupts(struct airoha_xpon_phy *priv)
{
	/* Link events are polled; never enable an unhandled PHY interrupt. */
	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, AN7581_INT0, 0);
	airoha_xpon_phy_write(priv, AN7581_INT1, 0);
}

static void an7581_park_rx(struct airoha_xpon_phy *priv)
{
	struct an7581_xpon_phy *an = priv->soc_priv;

	airoha_xpon_phy_set_tx_gpio(priv, false);
	an7581_run(priv, rx_disconnect);
	an->parked = true;
}

static u32 an7581_cdr_measure(struct airoha_xpon_phy *priv, u32 idac)
{
	airoha_xpon_phy_rmw(priv, AN7581_CDR_IDAC, GENMASK(10, 0), idac);
	airoha_xpon_phy_rmw(priv, AN7581_RX_FREQ_CTRL, GENMASK(2, 0), 3);
	usleep_range(5000, 5500);
	return airoha_xpon_phy_read(priv, AN7581_RX_FREQ) >> 16;
}

static int an7581_calibrate_cdr(struct airoha_xpon_phy *priv)
{
	u32 idac = 0, candidate, frequency;
	int coarse, bit;
	bool locked;

	/* E1 XPON_PrCal_WK(): GPON's frequency-detector target is 0xa49a. */
	airoha_xpon_phy_rmw(priv, AN7581_CDR_INJECTION, BIT(24), BIT(24));
	airoha_xpon_phy_rmw(priv, AN7581_CDR_LPF,
			    BIT(24) | BIT(16) | BIT(8) | BIT(0),
			    BIT(24) | BIT(16) | BIT(8) | BIT(0));
	airoha_xpon_phy_rmw(priv, AN7581_CDR_IDAC, BIT(16), BIT(16));
	airoha_xpon_phy_rmw(priv, AN7581_CDR_POWER, BIT(24), BIT(24));
	airoha_xpon_phy_rmw(priv, AN7581_CDR_POWER, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, AN7581_CDR_POWER, BIT(16), BIT(16));

	for (coarse = 1; coarse < 8; coarse++) {
		candidate = coarse << 8;
		frequency = an7581_cdr_measure(priv, candidate);
		if (frequency > 0xa49a)
			idac = candidate;
	}
	for (bit = 7; bit >= 0; bit--) {
		candidate = idac | BIT(bit);
		frequency = an7581_cdr_measure(priv, candidate);
		idac = frequency < 0xa49a ? candidate & ~BIT(bit) : candidate;
	}
	airoha_xpon_phy_rmw(priv, AN7581_CDR_IDAC, GENMASK(10, 0), idac);
	airoha_xpon_phy_rmw(priv, AN7581_RX_FREQ_CTRL, GENMASK(2, 0), 3);
	locked = airoha_xpon_phy_read(priv, AN7581_RX_FREQ) & BIT(0);

	/* Always release the calibration overrides, including on failure. */
	airoha_xpon_phy_rmw(priv, AN7581_CDR_INJECTION, BIT(24), 0);
	airoha_xpon_phy_rmw(priv, AN7581_CDR_LPF,
			    BIT(24) | BIT(16) | BIT(8) | BIT(0),
			    BIT(24) | BIT(16));
	airoha_xpon_phy_rmw(priv, AN7581_CDR_IDAC, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, AN7581_RX_FLL_CTRL, BIT(0), BIT(0));
	airoha_xpon_phy_rmw(priv, AN7581_RX_FLL_IDAC, GENMASK(10, 0), idac);
	airoha_xpon_phy_rmw(priv, AN7581_CDR_POWER, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, AN7581_CDR_POWER, BIT(16), BIT(16));
	airoha_xpon_phy_rmw(priv, AN7581_CDR_POWER, BIT(24), BIT(24));

	return locked ? 0 : -ETIMEDOUT;
}

static int an7581_connect(struct airoha_xpon_phy *priv)
{
	struct an7581_xpon_phy *an = priv->soc_priv;
	int ret;

	if (an->calibrated) {
		an7581_run(priv, rx_connect);
		an->parked = false;
		return 0;
	}

	an7581_run(priv, rx_start);
	if (an->revision == 1) {
		ret = an7581_calibrate_cdr(priv);
		if (ret)
			ret = an7581_calibrate_cdr(priv);
		if (ret)
			return ret;
	}
	an7581_run(priv, rx_cal);
	/* The GPON E1 path skips FEOS; later revisions execute it. */
	if (an->revision != 1)
		an7581_run(priv, rx_feos);
	/* EO_Scan() only scans the 10G modes, so GPON proceeds directly. */
	an7581_run(priv, rx_finish);
	an->calibrated = true;
	an->parked = false;
	return 0;
}

static void an7581_ready_work(struct work_struct *work)
{
	struct airoha_xpon_phy *priv = container_of(to_delayed_work(work),
					struct airoha_xpon_phy, ready_work);
	struct an7581_xpon_phy *an = priv->soc_priv;
	bool ready;
	int ret;

	mutex_lock(&an->lock);
	if (!priv->powered)
		goto out;
	if (an7581_los(priv)) {
		if (!an->parked)
			an7581_park_rx(priv);
	} else if (an->parked || !an7581_ready(priv)) {
		/* Also recover LOF without a transition on the optical LOS pin. */
		if (!an->parked)
			an7581_park_rx(priv);
		ret = an7581_connect(priv);
		if (ret) {
			an7581_park_rx(priv);
			dev_warn_ratelimited(priv->dev,
					     "GPON RX calibration failed: %d\n", ret);
		}
	}
	ready = an7581_ready(priv);
	if (ready != priv->ready_reported) {
		priv->ready_reported = ready;
		dev_info(priv->dev, "GPON PHY %s (LOS=%u)\n",
			 ready ? "ready" : "unsynchronized", an7581_los(priv));
	}
	an7581_update_tx(priv);
	mod_delayed_work(system_wq, &priv->ready_work,
			 msecs_to_jiffies(AN7581_POLL_MS));
out:
	mutex_unlock(&an->lock);
}

static int an7581_power_off(struct airoha_xpon_phy *priv)
{
	struct an7581_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	mutex_lock(&an->lock);
	WRITE_ONCE(priv->powered, false);
	an->tx_requested = false;
	airoha_xpon_phy_set_tx_gpio(priv, false);
	mutex_unlock(&an->lock);
	cancel_delayed_work_sync(&priv->ready_work);

	mutex_lock(&an->lock);
	if (an->configured) {
		an7581_mask_interrupts(priv);
		airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY,
				    XPON_PHYFWREADY_READY, 0);
		airoha_xpon_phy_write(priv, AN7581_PON_CLOCK, 0);
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

static void an7581_shutdown(struct airoha_xpon_phy *priv)
{
	int ret = an7581_power_off(priv);

	if (ret)
		dev_warn(priv->dev, "failed to assert PHY reset: %d\n", ret);
}

static void an7581_cleanup(void *data)
{
	an7581_shutdown(data);
}

static int an7581_init(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7581_xpon_phy *an = priv->soc_priv;

	mutex_lock(&an->lock);
	priv->initialized = true;
	mutex_unlock(&an->lock);
	return 0;
}

static int an7581_exit(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7581_xpon_phy *an = priv->soc_priv;
	int ret;

	ret = an7581_power_off(priv);
	mutex_lock(&an->lock);
	priv->initialized = false;
	mutex_unlock(&an->lock);
	return ret;
}

static int an7581_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7581_xpon_phy *an = priv->soc_priv;
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

static int an7581_power_on(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	struct an7581_xpon_phy *an = priv->soc_priv;
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
	ret = regmap_clear_bits(priv->scu, AN7581_SCU_PBUS_CTRL,
				AN7581_SCU_PBUS_DISABLE);
	if (ret)
		goto err_power;

	/* Gate both PHY clock paths before pulsing the top reset. */
	airoha_xpon_phy_write(priv, AN7581_PON_CLOCK, 0);
	airoha_xpon_phy_rmw(priv, AN7581_TX_PHY_CLOCK, BIT(8), 0);
	airoha_xpon_phy_rmw(priv, AN7581_TX_CLOCK, BIT(24) | BIT(16), BIT(24));
	ret = regmap_update_bits(priv->scu, AN7581_SCU_WAN_CONF,
				 AN7581_SCU_WAN_MASK, 0x11);
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
	ret = regmap_update_bits(priv->scu, AN7581_SCU_WAN_CONF,
				 AN7581_SCU_WAN_MASK, 0);
	if (ret)
		goto err_power;
	ret = regmap_clear_bits(priv->scu, AN7581_SCU_PBUS_CTRL,
				AN7581_SCU_PBUS_DISABLE);
	if (ret)
		goto err_power;

	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY, 0);
	an7581_mask_interrupts(priv);
	an7581_run(priv, init);
	an7581_mask_interrupts(priv);
	ret = read_poll_timeout(airoha_xpon_phy_read, value, value & BIT(16),
				100, 100000, false, priv, AN7581_TX_PLL_STATUS);
	if (ret) {
		dev_err(priv->dev, "GPON TX PLL lock timeout: %#08x\n", value);
		goto err_power;
	}
	airoha_xpon_phy_rmw(priv, XPON_PHYSET10, XPON_PHYSET10_GPON,
			    XPON_PHYSET10_GPON);
	airoha_xpon_phy_rmw(priv, XPON_SETTING, XPON_SETTING_INV_MASK,
			    priv->trans_invert);
	airoha_xpon_phy_rmw(priv, AN7581_SETTING, BIT(8) | BIT(24),
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
	ret = an7581_connect(priv);
	if (ret && !an7581_los(priv))
		goto err_power;
	if (an7581_los(priv) || ret) {
		usleep_range(1000, 1100);
		an7581_park_rx(priv);
		usleep_range(1000, 1100);
	} else {
		msleep(350);
	}

	/* xpon_pma_param_opt(): GPON FIR and the SDK's non-eFuse trims. */
	airoha_xpon_phy_rmw(priv, AN7581_TX_FIR0,
			    BIT(24) | BIT(8) | GENMASK(20, 16) | GENMASK(5, 0),
			    BIT(24) | BIT(8) | 8);
	airoha_xpon_phy_rmw(priv, AN7581_TX_FIR1,
			    BIT(24) | BIT(8) | GENMASK(18, 16) | GENMASK(4, 0),
			    BIT(24) | BIT(8));
	airoha_xpon_phy_rmw(priv, AN7581_MEM_CLOCK, GENMASK(1, 0), 3);
	airoha_xpon_phy_rmw(priv, AN7581_TX_DELAY, GENMASK(30, 28), BIT(28));
	airoha_xpon_phy_rmw(priv, AN7581_RX_IMPEDANCE, GENMASK(25, 24), BIT(25));
	airoha_xpon_phy_rmw(priv, AN7581_TX_CALIB0,
			    GENMASK(25, 24) | BIT(16), BIT(24) | BIT(16));
	airoha_xpon_phy_rmw(priv, AN7581_TX_CALIB1,
			    GENMASK(25, 24) | BIT(16), BIT(24) | BIT(16));
	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY,
			    XPON_PHYFWREADY_READY);
	WRITE_ONCE(priv->powered, true);
	mod_delayed_work(system_wq, &priv->ready_work,
			 msecs_to_jiffies(AN7581_POLL_MS));
	dev_info(priv->dev, "AN7581 GPON PHY started (revision %u, LOS=%u)\n",
		 an->revision, an7581_los(priv));
	ret = 0;
	goto out;

err_power:
	airoha_xpon_phy_set_tx_gpio(priv, false);
	if (an->configured)
		an7581_mask_interrupts(priv);
	reset_control_assert(priv->reset);
	/* Never leave the temporary reset-mode selector in the SCU. */
	regmap_update_bits(priv->scu, AN7581_SCU_WAN_CONF, AN7581_SCU_WAN_MASK, 0);
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	an->configured = false;
	an->calibrated = false;
	an->parked = true;
out:
	mutex_unlock(&an->lock);
	return ret;
}

static int an7581_phy_power_off(struct phy *phy)
{
	return an7581_power_off(phy_get_drvdata(phy));
}

static int an7581_set_tx_calibration_mode(struct airoha_xpon_phy *priv,
					  bool enable)
{
	struct an7581_xpon_phy *an = priv->soc_priv;
	int ret = 0;

	mutex_lock(&an->lock);
	if (!priv->powered) {
		ret = -EHOSTDOWN;
		goto out;
	}
	if (enable == an->tx_calibration)
		goto out;
	if (enable) {
		an->saved_bist = airoha_xpon_phy_read(priv, AN7581_BIST);
		an->saved_tx_source = airoha_xpon_phy_read(priv, AN7581_TX_SOURCE);
		/* The AN7581 PMA supplies PRBS23 within the normal burst slots. */
		airoha_xpon_phy_rmw(priv, AN7581_BIST, GENMASK(4, 0) | BIT(8),
				    4 | BIT(8));
		airoha_xpon_phy_rmw(priv, AN7581_TX_SOURCE, BIT(16) | BIT(8), BIT(16));
	} else {
		airoha_xpon_phy_write(priv, AN7581_TX_SOURCE, an->saved_tx_source);
		airoha_xpon_phy_write(priv, AN7581_BIST, an->saved_bist);
	}
	an->tx_calibration = enable;
out:
	mutex_unlock(&an->lock);
	return ret;
}

static const struct phy_ops an7581_phy_ops = {
	.init = an7581_init,
	.exit = an7581_exit,
	.set_mode = an7581_set_mode,
	.power_on = an7581_power_on,
	.power_off = an7581_phy_power_off,
	.owner = THIS_MODULE,
};

static int an7581_probe(struct airoha_xpon_phy *priv, struct platform_device *pdev)
{
	struct an7581_xpon_phy *an;
	u32 revision;
	int ret;

	an = devm_kzalloc(priv->dev, sizeof(*an), GFP_KERNEL);
	if (!an)
		return -ENOMEM;
	mutex_init(&an->lock);
	an->parked = true;
	priv->soc_priv = an;
	ret = regmap_read(priv->scu, AN7581_SCU_REVISION, &revision);
	if (ret)
		return dev_err_probe(priv->dev, ret, "failed to read chip revision\n");
	an->revision = revision & 0xffff;
	INIT_DELAYED_WORK(&priv->ready_work, an7581_ready_work);
	ret = reset_control_assert(priv->reset);
	if (ret)
		return ret;
	return devm_add_action_or_reset(priv->dev, an7581_cleanup, priv);
}

static const struct airoha_xpon_phy_soc_data airoha_an7581_xpon_phy_data = {
	.name = "AN7581",
	.min_size = SZ_4K,
	.ana_offset = AN7581_ANA_OFFSET,
	.pma_offset = AN7581_PMA_OFFSET,
	.gpon_bit_delay_reg = XPON_PHYSET5,
	.gpon_bit_delay_mask = XPON_PHYSET5_BIT_DELAY_MASK,
	.gpon_bit_delay_enable = XPON_PHYSET5_BIT_DELAY_EN,
	.manages_fw_ready = true,
	.has_gpon_bip = true,
	.ops = &an7581_phy_ops,
	.probe = an7581_probe,
	.shutdown = an7581_shutdown,
	.get_link_state = an7581_get_link_state,
	.set_tx_enable = an7581_set_tx_enable,
	.set_tx_calibration_mode = an7581_set_tx_calibration_mode,
};

static const struct of_device_id an7581_xpon_phy_of_match[] = {
	{ .compatible = "airoha,an7583-xpon-phy", .data = &airoha_an7581_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_xpon_phy_of_match);

static struct platform_driver an7581_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-an7583-xpon-phy",
		.of_match_table = an7581_xpon_phy_of_match,
	},
};
module_platform_driver(an7581_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha AN7583 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
