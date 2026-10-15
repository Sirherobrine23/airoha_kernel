// SPDX-License-Identifier: GPL-2.0
/* Airoha EN7580 XSI PCS and PMA. */
/* EN7580 PMA sequences from the C6500XK xsi_phy.ko and XGZ030 GPL.
 * Preserve the order, masks and delays, including repeated writes.
 * Mode columns: XFI 10G, XFI 5G, HSGMII 2.5G, HSGMII 1G.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pcs/pcs-provider.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#include "pcs-airoha.h"

#define EN7580_SCU_RESET			0x830
#define EN7580_SCU_XSI_PHY_RESET		BIT(8)
#define EN7580_SCU_XSI_CLK			0x860
#define EN7580_SCU_XSI_CLK_PD			BIT(10)
#define EN7580_SCU_XSI_CTRL			0x92c
#define EN7580_SCU_XSI_DISABLE			BIT(2)
#define EN7580_CHIP_SCU_XSI_CTRL		0x104
#define EN7580_CHIP_SCU_XSI_ENABLE		(BIT(15) | BIT(0))

/* Offsets relative to the PCS resource at 0x1faf7900. */
#define EN7580_XFI_CTRL				0x0
#define EN7580_XFI_STATUS			0x30
#define EN7580_XFI_READY			(BIT(12) | BIT(0))
#define EN7580_HSGMII_CTRL			0x808
#define EN7580_HSGMII_STATUS			0x91c
#define EN7580_HSGMII_STATE			GENMASK(3, 0)
#define EN7580_HSGMII_READY			0xa

#define EN7580_MAC_STOP				GENMASK(3, 0)
#define EN7580_MAC_TX_MASK			BIT(9)
#define EN7580_MAC_INT_STATUS			0x4
#define EN7580_SEQ_DELAY			0xffff
#define EN7580_SEQ_SCU				BIT(15)

#define EN7580_PMA_RX_EQ			0x630
#define EN7580_RX_EQ_GAIN			GENMASK(1, 0)
#define EN7580_RX_EQ_PEAKING			GENMASK(10, 8)
#define EN7580_PMA_RX_EQ_CTRL			0x63c
#define EN7580_RX_EQ_GAIN_AUTO			BIT(0)
#define EN7580_RX_EQ_PEAKING_AUTO		BIT(8)
#define EN7580_PMA_EYE_LATCH			0x49c
#define EN7580_EYE_LATCH			BIT(24)
#define EN7580_PMA_EYE_DONE			0x548
#define EN7580_EYE_DONE				(BIT(16) | BIT(24))
#define EN7580_PMA_EYE_READY			0x538
#define EN7580_EYE_HREADY			BIT(16)
#define EN7580_EYE_VREADY			BIT(24)
#define EN7580_PMA_EYE_WINDOW			0x54c
#define EN7580_EYE_LEFT				GENMASK(26, 16)
#define EN7580_EYE_RIGHT			GENMASK(10, 0)

#define EN7580_SEQ(r, m, v)			{ (r), (m), { (v), (v), (v), (v) } }
#define EN7580_MODE_SEQ(r, m, a, b, c, d)	{ (r), (m), { (a), (b), (c), (d) } }

struct en7580_pcs_step {
	u16 reg;
	u32 mask;
	u32 value[4];
};

static const struct en7580_pcs_step en7580_pcs_init_seq[] = {
	/* Setting_mode_XFI_DIG */
	EN7580_MODE_SEQ(0x8094, 0x00006000, 0x00002000, 0x00002000, 0x00004000, 0x00004000),
	/* Setting_mode_XFI_TXPLL */
	EN7580_MODE_SEQ(0x0230, 0xffffffff, 0x07400000, 0x07400000, 0x09000000, 0x07000000),
	EN7580_MODE_SEQ(0x0248, 0xffffffff, 0x21000000, 0x21000000, 0x28000000, 0x20000000),
	EN7580_MODE_SEQ(0x024c, 0xffffffff, 0x21000000, 0x21000000, 0x28000000, 0x20000000),
	/* Setting_mode_XFI_TX */
	EN7580_MODE_SEQ(0x0070, 0x00001c00, 0x00001400, 0x00001000, 0x00001000, 0x00000800),
	EN7580_MODE_SEQ(0x0070, 0x00000030, 0x00000020, 0x00000020, 0x00000010, 0x00000010),
	EN7580_SEQ(0x0754, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0758, 0x00000001, 0x00000000),
	/* Setting_mode_XFI_RX */
	EN7580_MODE_SEQ(0x0030, 0x00006000, 0x00000000, 0x00002000, 0x00002000, 0x00006000),
	EN7580_MODE_SEQ(0x0714, 0x00000003, 0x00000002, 0x00000002, 0x00000001, 0x00000001),
	EN7580_MODE_SEQ(0x0754, 0x00000007, 0x00000000, 0x00000001, 0x00000004, 0x00000005),
	EN7580_MODE_SEQ(0x0754, 0x00000700, 0x00000000, 0x00000100, 0x00000400, 0x00000500),
	/* Setting_XFI_DIG */
	EN7580_SEQ(0x0868, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0868, 0x00000002, 0x00000002),
	EN7580_SEQ(0x0868, 0x00000008, 0x00000008),
	EN7580_SEQ(0x0868, 0x00000010, 0x00000010),
	EN7580_SEQ(0x0868, 0x00000020, 0x00000020),
	EN7580_SEQ(0x0204, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0204, 0x0000ff00, 0x00000d00),
	EN7580_SEQ(0x0204, 0x00ff0000, 0x00140000),
	EN7580_SEQ(0x0804, 0x0000ffff, 0x000013ba),
	EN7580_SEQ(0x0804, 0x00ff0000, 0x00140000),
	EN7580_SEQ(0x090c, 0x0000ffff, 0x00000fa0),
	EN7580_SEQ(0x090c, 0xffff0000, 0x00000000),
	EN7580_SEQ(0x0910, 0x0000ffff, 0x0000109a),
	EN7580_SEQ(0x0910, 0xffff0000, 0x00000000),
	EN7580_SEQ(0x0888, 0x0000ffff, 0x000000fa),
	EN7580_SEQ(0x0888, 0xffff0000, 0x01130000),
	EN7580_SEQ(0x088c, 0x0000ffff, 0x00000210),
	EN7580_SEQ(0x088c, 0xffff0000, 0x009b0000),
	EN7580_SEQ(0x0884, 0x0000ffff, 0x00000026),
	EN7580_SEQ(0x0884, 0xffff0000, 0x00040000),
	EN7580_SEQ(0x0234, 0x00000003, 0x00000002),
	EN7580_SEQ(0x022c, 0x00000f00, 0x00000700),
	EN7580_SEQ(0x0888, 0xffff0000, 0x01130000),
	EN7580_SEQ(0x0888, 0x0000ffff, 0x000000fa),
	EN7580_SEQ(0x088c, 0xffff0000, 0x009b0000),
	EN7580_SEQ(0x088c, 0x0000ffff, 0x00000210),
	EN7580_SEQ(0x0884, 0xffff0000, 0x00040000),
	EN7580_SEQ(0x0884, 0x0000ffff, 0x00000026),
	EN7580_SEQ(0x0228, 0x00000700, 0x00000300),
	EN7580_SEQ(0x0224, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0204, 0xff000000, 0x01000000),
	EN7580_SEQ(0x0204, 0x00ff0000, 0x00010000),
	EN7580_SEQ(0x0204, 0x0000ff00, 0x00000a00),
	EN7580_SEQ(0x0204, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0344, 0x0000ffff, 0x00000002),
	EN7580_SEQ(0x0344, 0xffff0000, 0x03e80000),
	EN7580_SEQ(0x0350, 0x0000ffff, 0x00000002),
	EN7580_SEQ(0x0350, 0xffff0000, 0x03e80000),
	EN7580_SEQ(0x0348, 0x0000ffff, 0x00000002),
	EN7580_SEQ(0x0348, 0xffff0000, 0x03e80000),
	EN7580_SEQ(0x034c, 0x0000ffff, 0x00000002),
	EN7580_SEQ(0x034c, 0xffff0000, 0x03e80000),
	EN7580_SEQ(0x0340, 0x00ffff00, 0x00000100),
	EN7580_SEQ(0x0340, 0x00000007, 0x00000001),
	EN7580_SEQ(0x0358, 0x0000ffff, 0x00000002),
	EN7580_SEQ(0x0378, 0x00000003, 0x00000003),
	EN7580_SEQ(0x0378, 0x00001f00, 0x00000500),
	EN7580_SEQ(0x0300, 0x07ff0000, 0x02400000),
	EN7580_SEQ(0x0300, 0x000007ff, 0x000001c0),
	EN7580_SEQ(0x0338, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0338, 0x00000100, 0x00000000),
	EN7580_SEQ(0x030c, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0320, 0x0000ff00, 0x00000400),
	EN7580_SEQ(0x0320, 0x000000ff, 0x00000004),
	EN7580_SEQ(0x0324, 0x00ff0000, 0x00040000),
	EN7580_SEQ(0x0324, 0x000007ff, 0x00000004),
	EN7580_SEQ(0x0330, 0xff000000, 0xff000000),
	EN7580_SEQ(0x0330, 0x000003ff, 0x000000d0),
	EN7580_SEQ(0x0370, 0x000000ff, 0x00000080),
	EN7580_SEQ(0x0370, 0x0007ff00, 0x00000000),
	EN7580_SEQ(0x0370, 0xff000000, 0x00000000),
	EN7580_SEQ(0x0374, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0490, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0490, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0440, 0x03000000, 0x01000000),
	EN7580_SEQ(0x0440, 0x0000ff00, 0x00002e00),
	EN7580_SEQ(0x0440, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0440, 0x00030000, 0x00010000),
	EN7580_SEQ(0x0444, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0444, 0x00000f00, 0x00000000),
	EN7580_SEQ(0x0444, 0x00030000, 0x00010000),
	EN7580_SEQ(0x0354, 0x0000ffff, 0x00000004),
	EN7580_SEQ(0x0354, 0xffff0000, 0x00c80000),
	EN7580_SEQ(0x0400, 0x000000ff, 0x00000000),
	EN7580_SEQ(0x0400, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0410, 0x00000f00, 0x00000a00),
	EN7580_SEQ(0x0410, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0410, 0x007f0000, 0x00100000),
	EN7580_SEQ(0x0410, 0x3f800000, 0x38000000),
	EN7580_SEQ(0x0304, 0x0000007f, 0x00000040),
	EN7580_SEQ(0x0304, 0x00007f00, 0x00003f00),
	EN7580_SEQ(0x0304, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0308, 0x000fffff, 0x00000018),
	EN7580_SEQ(0x0930, 0x0000ff00, 0x00000200),
	EN7580_SEQ(0x0930, 0x00000002, 0x00000002),
	EN7580_SEQ(0x0930, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0430, 0x00000700, 0x00000100),
	EN7580_SEQ(0x0430, 0x00030000, 0x00000000),
	EN7580_SEQ(0x0460, 0x0007ff00, 0x0000a500),
	EN7580_SEQ(0x0460, 0x00000007, 0x00000003),
	EN7580_SEQ(0x0460, 0x03000000, 0x00000000),
	EN7580_SEQ(0x0464, 0x07000000, 0x04000000),
	EN7580_SEQ(0x0464, 0x00070000, 0x00010000),
	EN7580_SEQ(0x0474, 0x000007ff, 0x000001ff),
	EN7580_SEQ(0x0474, 0x07ff0000, 0x04000000),
	EN7580_SEQ(0x0468, 0x00000700, 0x00000400),
	EN7580_SEQ(0x0468, 0x00000007, 0x00000003),
	EN7580_SEQ(0x065c, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0490, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0490, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0410, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0410, 0x00000f00, 0x00000a00),
	EN7580_SEQ(0x0410, 0x007f0000, 0x00100000),
	EN7580_SEQ(0x0410, 0x3f800000, 0x38000000),
	EN7580_SEQ(0x0400, 0x00000100, 0x00000000),
	/* Setting_XFI_ANA */
	EN7580_SEQ(0x0004, 0x0f000000, 0x00000000),
	EN7580_SEQ(0x0004, 0x00006000, 0x00006000),
	EN7580_SEQ(0x0004, 0x00000400, 0x00000400),
	EN7580_SEQ(0x0004, 0x00000200, 0x00000200),
	EN7580_SEQ(0x0004, 0x00000020, 0x00000020),
	EN7580_SEQ(0x0008, 0x00060000, 0x00060000),
	EN7580_SEQ(0x0008, 0x00018000, 0x00018000),
	EN7580_SEQ(0x0008, 0x00006000, 0x00000000),
	EN7580_SEQ(0x000c, 0xc0000000, 0xc0000000),
	EN7580_SEQ(0x000c, 0x00010000, 0x00010000),
	EN7580_SEQ(0x000c, 0x00001fe0, 0x00000000),
	EN7580_SEQ(0x000c, 0x00000008, 0x00000008),
	EN7580_SEQ(0x000c, 0x00000004, 0x00000004),
	EN7580_SEQ(0x0060, 0x10000000, 0x10000000),
	EN7580_MODE_SEQ(0x003c, 0x7f000000, 0x1c000000, 0x1c000000, 0x08000000, 0x1c000000),
	EN7580_MODE_SEQ(0x003c, 0x0000007c, 0x00000070, 0x00000070, 0x00000020, 0x00000070),
	EN7580_SEQ(0x002c, 0x00200000, 0x00200000),
	EN7580_SEQ(0x0044, 0x00000080, 0x00000080),
	EN7580_MODE_SEQ(0x002c, 0xff000000, 0x42000000, 0x42000000, 0x0a000000, 0x10000000),
	EN7580_MODE_SEQ(0x002c, 0x00c00000, 0x00800000, 0x00800000, 0x00000000, 0x00000000),
	EN7580_SEQ(0x0018, 0x01c00000, 0x01000000),
	EN7580_SEQ(0x0018, 0x80000000, 0x80000000),
	EN7580_SEQ(0x0018, 0x7e000000, 0x02000000),
	EN7580_SEQ(0x001c, 0x80000000, 0x80000000),
	EN7580_SEQ(0x001c, 0x40000000, 0x40000000),
	EN7580_SEQ(0x0750, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0750, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0060, 0x00080000, 0x00080000),
	EN7580_SEQ(0x0060, 0x00020000, 0x00000000),
	EN7580_SEQ(0x0060, 0x00040000, 0x00000000),
	EN7580_SEQ(0x0008, 0xe0000000, 0x80000000),
	EN7580_SEQ(0x0008, 0x1c000000, 0x10000000),
	EN7580_SEQ(0x0008, 0x03c00000, 0x02000000),
	EN7580_SEQ(0x0008, 0x00200000, 0x00200000),
	EN7580_SEQ(0x0008, 0x00060000, 0x00060000),
	EN7580_SEQ(0x0008, 0x00018000, 0x00018000),
	EN7580_SEQ(0x0008, 0x00000010, 0x00000010),
	EN7580_SEQ(0x0008, 0x00001fe0, 0x00001fe0),
	EN7580_SEQ(0x000c, 0xc0000000, 0xc0000000),
	EN7580_SEQ(0x000c, 0x00010000, 0x00010000),
	EN7580_SEQ(0x000c, 0x00001fe0, 0x00001000),
	EN7580_SEQ(0x0010, 0xff000000, 0x00000000),
	EN7580_SEQ(0x0010, 0x00ff0000, 0x00ff0000),
	EN7580_SEQ(0x0004, 0xf0000000, 0x60000000),
	EN7580_SEQ(0x0004, 0x0f000000, 0x03000000),
	EN7580_SEQ(0x0004, 0x001c0000, 0x00000000),
	EN7580_SEQ(0x0004, 0x00e00000, 0x00200000),
	EN7580_SEQ(0x0004, 0x00038000, 0x00028000),
	EN7580_SEQ(0x0004, 0x00000400, 0x00000400),
	EN7580_SEQ(0x0004, 0x00000200, 0x00000200),
	EN7580_SEQ(0x0004, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0004, 0x00000020, 0x00000020),
	EN7580_SEQ(0x0004, 0x00000010, 0x00000010),
	EN7580_SEQ(0x0004, 0x0000000e, 0x00000008),
	EN7580_SEQ(0x090c, 0x0000ffff, 0x00000c80),
	EN7580_SEQ(0x0910, 0x0000ffff, 0x00000d48),
	EN7580_SEQ(0x0068, 0x80000000, 0x80000000),
	EN7580_SEQ(0x0068, 0x60000000, 0x40000000),
	EN7580_SEQ(0x0068, 0x00000020, 0x00000020),
	EN7580_SEQ(0x006c, 0x80000000, 0x00000000),
	EN7580_SEQ(0x006c, 0x40000000, 0x00000000),
	EN7580_SEQ(0x006c, 0x00020000, 0x00000000),
	EN7580_SEQ(0x0070, 0x00000300, 0x00000200),
	EN7580_SEQ(0x0070, 0x000000c0, 0x00000080),
	EN7580_SEQ(0x006c, 0x00fc0000, 0x00000000),
	EN7580_SEQ(0x006c, 0x02000000, 0x00000000),
	EN7580_SEQ(0x006c, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0068, 0x00000400, 0x00000400),
	EN7580_SEQ(0x006c, 0x20000000, 0x20000000),
	EN7580_SEQ(0x006c, 0x10000000, 0x00000000),
	EN7580_SEQ(0x006c, 0x08000000, 0x08000000),
	EN7580_SEQ(0x006c, 0x04000000, 0x04000000),
	EN7580_MODE_SEQ(0x0068, 0x0f000000, 0x02000000, 0x00000000, 0x00000000, 0x00000000),
	EN7580_MODE_SEQ(0x0068, 0x00f80000, 0x00080000, 0x00000000, 0x00000000, 0x00000000),
	EN7580_MODE_SEQ(0x0068, 0x0007c000, 0x00030000, 0x00000000, 0x00000000, 0x00000000),
	EN7580_MODE_SEQ(0x0068, 0x00003800, 0x00002000, 0x00000000, 0x00000000, 0x00000000),
	EN7580_SEQ(0x0068, 0x000003c0, 0x00000000),
	EN7580_SEQ(0x0048, 0x00002000, 0x00002000),
	EN7580_SEQ(0x0048, 0x00004000, 0x00004000),
	EN7580_SEQ(0x0048, 0x00008000, 0x00008000),
	EN7580_SEQ(0x0048, 0x00000e00, 0x00000800),
	EN7580_SEQ(0x0048, 0x00001000, 0x00001000),
	EN7580_SEQ(0x0640, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0640, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0640, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0644, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0644, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0644, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0644, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0634, 0x00007f00, 0x00000000),
	EN7580_SEQ(0x0634, 0x007f0000, 0x00000000),
	EN7580_SEQ(0x0634, 0x7f000000, 0x00000000),
	EN7580_SEQ(0x0638, 0x0000007f, 0x00000000),
	EN7580_SEQ(0x0638, 0x00007f00, 0x00000000),
	EN7580_SEQ(0x0638, 0x003f0000, 0x00000000),
	EN7580_SEQ(0x0638, 0x7f000000, 0x00000000),
	EN7580_SEQ(0x035c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x035c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x035c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x035c, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0360, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0034, 0xffffe000, 0x10000000),
	EN7580_SEQ(0x002c, 0x0007ffff, 0x00078000),
	/* Setting_XFI_DIG_reset */
	EN7580_SEQ(0x0900, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0900, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0900, 0x00000002, 0x00000000),
	EN7580_SEQ(0x0904, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0904, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0900, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0900, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0900, 0x00000002, 0x00000002),
	EN7580_SEQ(0x0904, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0904, 0x00000001, 0x00000001),
	/* seq_JCPLL_force_on */
	EN7580_SEQ(0x0204, 0x00000001, 0x00000001),
	EN7580_SEQ(0xffff, 0x00000000, 0x00001388),
	EN7580_SEQ(0x0800, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0800, 0x00000001, 0x00000001),
	EN7580_SEQ(0xffff, 0x00000000, 0x00001388),
};

static const struct en7580_pcs_step en7580_pcs_start_seq[] = {
	/* seq_XFI_TX_on_normal */
	EN7580_SEQ(0x0550, 0x00000001, 0x00000001),
	EN7580_SEQ(0xffff, 0x00000000, 0x000003e8),
	EN7580_SEQ(0x0750, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0750, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0550, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0550, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0554, 0x03000000, 0x02000000),
	EN7580_SEQ(0x0554, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0554, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0558, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0558, 0x00000001, 0x00000000),
	EN7580_SEQ(0x004c, 0x00300000, 0x00100000),
	EN7580_MODE_SEQ(0x004c, 0x0003fc00, 0x00008400, 0x00008400, 0x00005000, 0x00008000),
	EN7580_SEQ(0x004c, 0x000c0000, 0x00000000),
	EN7580_SEQ(0x004c, 0x000003c0, 0x00000380),
	EN7580_SEQ(0x004c, 0x0000003c, 0x00000000),
	EN7580_SEQ(0x0050, 0xf0000000, 0x50000000),
	EN7580_MODE_SEQ(0x0050, 0x0f800000, 0x0f800000, 0x0f800000, 0x09800000, 0x0f800000),
	EN7580_MODE_SEQ(0x0054, 0xfc000000, 0xa0000000, 0xa0000000, 0x54000000, 0xa0000000),
	EN7580_SEQ(0x0054, 0x00ff0000, 0x00000000),
	EN7580_SEQ(0x0834, 0x00080000, 0x00000000),
	EN7580_SEQ(0x0834, 0x00010000, 0x00000000),
	EN7580_SEQ(0x083c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0838, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0838, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0060, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0210, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0218, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0210, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0830, 0x00080000, 0x00000000),
	EN7580_SEQ(0x0830, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0830, 0x00040000, 0x00000000),
	EN7580_SEQ(0x0200, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0200, 0x00000001, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x0000000a),
	EN7580_SEQ(0x0200, 0x00000001, 0x00000001),
	EN7580_SEQ(0xffff, 0x00000000, 0x000001f4),
	/* seq_XFI_RX_preset */
	EN7580_SEQ(0x0368, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0360, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0368, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0360, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0630, 0x00000003, 0x00000001),
	EN7580_SEQ(0x063c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0630, 0x00000700, 0x00000300),
	EN7580_SEQ(0x063c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0630, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0630, 0x01000000, 0x01000000),
	EN7580_SEQ(0x063c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x063c, 0x01000000, 0x00000000),
	EN7580_SEQ(0x066c, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0678, 0x00000100, 0x00000000),
	EN7580_SEQ(0x066c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0678, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0494, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0674, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0668, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0360, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0368, 0x00010000, 0x00000000),
	/* aux_XFI_TDC_off */
	EN7580_SEQ(0x0234, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0210, 0x00000001, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x000003e8),
	/* seq_XFI_RX_on */
	EN7580_SEQ(0x068c, 0x00000100, 0x00000100),
	EN7580_SEQ(0x068c, 0x00000001, 0x00000001),
	EN7580_SEQ(0x068c, 0x01000000, 0x01000000),
	EN7580_SEQ(0x068c, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0690, 0x00000001, 0x00000001),
	EN7580_SEQ(0x068c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x068c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x068c, 0x01000000, 0x00000000),
	EN7580_SEQ(0x068c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0690, 0x00000001, 0x00000000),
	EN7580_SEQ(0x068c, 0x00000100, 0x00000100),
	EN7580_SEQ(0x068c, 0x00000001, 0x00000001),
	EN7580_SEQ(0x068c, 0x01000000, 0x01000000),
	EN7580_SEQ(0x068c, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0690, 0x00000001, 0x00000001),
	EN7580_SEQ(0xffff, 0x00000000, 0x000001f4),
	/* seq_XFI_RX_L2R */
	EN7580_SEQ(0x0630, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0630, 0x01000000, 0x01000000),
	EN7580_SEQ(0x063c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x063c, 0x01000000, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x00000064),
	/* seq_XFI_RX_pical */
	EN7580_SEQ(0x0644, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0370, 0x000000ff, 0x00000080),
	EN7580_SEQ(0x0430, 0x00000700, 0x00000400),
	EN7580_SEQ(0x0494, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0664, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0670, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0664, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0670, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0654, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0660, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0364, 0x00000100, 0x00000000),
	EN7580_SEQ(0x035c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0494, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0664, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0664, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0654, 0x00000100, 0x00000100),
	EN7580_SEQ(0xffff, 0x00000000, 0x000000c8),
	EN7580_SEQ(0x0654, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0658, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0648, 0x00000001, 0x00000001),
	/* seq_XFI_RX_pdos */
	EN7580_SEQ(0x0368, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0360, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0640, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0640, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0640, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0644, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0644, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0668, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0674, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0334, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0334, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0668, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0674, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0334, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0334, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0490, 0x00070000, 0x00020000),
	EN7580_SEQ(0x0664, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0670, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0498, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0364, 0x00010000, 0x00000000),
	EN7580_SEQ(0x035c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0364, 0x00000001, 0x00000001),
	EN7580_SEQ(0x035c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0664, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0498, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0364, 0x00010000, 0x00010000),
	EN7580_SEQ(0xffff, 0x00000000, 0x000000c8),
	EN7580_SEQ(0x0364, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0364, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0334, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0334, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0668, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0674, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0334, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0334, 0x00000001, 0x00000001),
	EN7580_SEQ(0xffff, 0x00000000, 0x000000c8),
	/* seq_XFI_RX_feos */
	EN7580_SEQ(0x0368, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0360, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0644, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0668, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0674, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0400, 0x000000ff, 0x00000030),
	EN7580_SEQ(0x0668, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0674, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0494, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0364, 0x01000000, 0x00000000),
	EN7580_SEQ(0x035c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0364, 0x00000001, 0x00000001),
	EN7580_SEQ(0x035c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0668, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0494, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0364, 0x01000000, 0x01000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x000000c8),
	EN7580_SEQ(0x0364, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0364, 0x00000001, 0x00000000),
	/* aux_XFI_phy_status */
	EN7580_SEQ(0x0368, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0360, 0x00000100, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x0000000a),
	/* seq_XFI_RX_L2D */
	EN7580_SEQ(0x063c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0630, 0x00010000, 0x00010000),
	EN7580_SEQ(0xffff, 0x00000000, 0x000000c8),
	EN7580_SEQ(0x0368, 0x01000000, 0x01000000),
};

static const struct en7580_pcs_step en7580_pcs_ready_seq[] = {
	/* seq_XFI_RX_rxrdy */
	EN7580_SEQ(0x0368, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0360, 0x01000000, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x0000000a),
	EN7580_SEQ(0x0900, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0048, 0x00060000, 0x00020000),
	/* aux_XFI_phy_status */
	EN7580_SEQ(0x0368, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0360, 0x00000100, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x0000000a),
	/* aux_XFI_R2T_sel */
	EN7580_SEQ(0x0754, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0758, 0x00000001, 0x00000000),
};

/* XGZ030 GPL: eye_setting(), eye_Cal() and eye_EO(). */
static const struct en7580_pcs_step en7580_pcs_eye_setup_seq[] = {
	EN7580_MODE_SEQ(0x0030, 0x00006000, 0x00000000, 0x00002000, 0x00002000, 0x00006000),
	EN7580_MODE_SEQ(0x0330, 0xff000000, 0xff000000, 0xff000000, 0xff000000, 0x22000000),
	EN7580_MODE_SEQ(0x0300, 0x000007ff, 0x000001c0, 0x00000180, 0x00000140, 0x00000000),
	EN7580_MODE_SEQ(0x0300, 0x07ff0000, 0x02400000, 0x02800000, 0x02c00000, 0x04000000),
	EN7580_SEQ(0x0330, 0x000003ff, 0x000000f8),
	EN7580_SEQ(0x0330, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0338, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0304, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0308, 0x000fffff, 0x0000fff8),
	EN7580_SEQ(0x030c, 0x00000001, 0x00000000),
	EN7580_SEQ(0x030c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x030c, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0320, 0x000000ff, 0x00000004),
	EN7580_SEQ(0x0320, 0x0000ff00, 0x00000400),
	EN7580_SEQ(0x0324, 0x000007ff, 0x00000004),
	EN7580_SEQ(0x0324, 0x00ff0000, 0x00040000),
	EN7580_SEQ(0x0374, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0374, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0374, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0378, 0x00000003, 0x00000001),
	EN7580_SEQ(0x0374, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0370, 0x000000ff, 0x00000080),
	EN7580_SEQ(0x0430, 0x00000700, 0x00000400),
};

static const struct en7580_pcs_step en7580_pcs_eye_cal_seq[] = {
	EN7580_SEQ(0x0644, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0370, 0x000000ff, 0x00000080),
	EN7580_SEQ(0x0430, 0x00000700, 0x00000400),
	EN7580_SEQ(0x0494, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0670, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0664, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0670, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0664, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0660, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0654, 0x00000100, 0x00000000),
	EN7580_SEQ(0x035c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0364, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0494, 0x00010000, 0x00010000),
	EN7580_SEQ(0x0664, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0664, 0x00000001, 0x00000001),
	EN7580_SEQ(0x0654, 0x00000100, 0x00000100),
	EN7580_SEQ(0xffff, 0x00000000, 0x000003e8),
	EN7580_SEQ(0x0654, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0658, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0648, 0x00000001, 0x00000001),
};

static const struct en7580_pcs_step en7580_pcs_eye_measure_seq[] = {
	EN7580_SEQ(0x0330, 0x000003ff, 0x0000000a),
	EN7580_MODE_SEQ(0x0308, 0x000fffff, 0x0000044c, 0x0000044c, 0x00000898, 0x0000044c),
	EN7580_SEQ(0x0678, 0x00000100, 0x00000000),
	EN7580_SEQ(0x066c, 0x00000100, 0x00000100),
	EN7580_SEQ(0x066c, 0x00000100, 0x00000000),
	EN7580_SEQ(0x0678, 0x00010000, 0x00000000),
	EN7580_SEQ(0x066c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x066c, 0x00010000, 0x00010000),
	EN7580_MODE_SEQ(0xffff, 0x00000000, 0x00002710, 0x00002710, 0x0001adb0, 0x0000d6d8),
	EN7580_SEQ(0x0644, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0644, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0334, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0674, 0x01000000, 0x01000000),
	EN7580_SEQ(0x0334, 0x00000001, 0x00000001),
};

static const struct en7580_pcs_step en7580_pcs_eye_stop_seq[] = {
	EN7580_SEQ(0x066c, 0x00000100, 0x00000100),
	EN7580_SEQ(0x0678, 0x00010000, 0x00000000),
	EN7580_SEQ(0x066c, 0x00010000, 0x00000000),
	EN7580_SEQ(0x0658, 0x00000001, 0x00000000),
	EN7580_SEQ(0x0648, 0x00000001, 0x00000000),
};

/* readout_EO(): restart CDR tracking when HEO is not ready. */
static const struct en7580_pcs_step en7580_pcs_eye_cdr_seq[] = {
	EN7580_SEQ(0x063c, 0x01000000, 0x00000000),
	EN7580_SEQ(0x0630, 0x01000000, 0x00000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x000001f4),
	EN7580_SEQ(0x0630, 0x01000000, 0x01000000),
	EN7580_SEQ(0xffff, 0x00000000, 0x000001f4),
};

struct en7580_pcs {
	struct fwnode_pcs_provider *provider;
	struct phylink_pcs pcs;
	struct device *dev;
	struct regmap *mac;
	struct regmap *pma;
	struct regmap *digital;
	struct regmap *scu;
	struct regmap *chip_scu;
	phy_interface_t interface;
	bool configured;
	/* Serialize PMA configuration against diagnostic register snapshots. */
	struct mutex lock;
	int last_config_error;
};

static struct en7580_pcs *to_en7580_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct en7580_pcs, pcs);
}

static int en7580_pcs_mode(phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_10GBASER:
		return 0;
	case PHY_INTERFACE_MODE_5GBASER:
		return 1;
	case PHY_INTERFACE_MODE_2500BASEX:
		return 2;
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		return 3;
	default:
		return -EINVAL;
	}
}

static int en7580_pcs_sequence(struct en7580_pcs *priv,
			       const struct en7580_pcs_step *seq,
			       unsigned int count, unsigned int mode)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct regmap *map = priv->pma;
		u32 value = seq[i].value[mode];
		u16 reg = seq[i].reg;

		if (reg == EN7580_SEQ_DELAY) {
			if (value < 10)
				udelay(value);
			else
				usleep_range(value, value + max(1U, value / 10));
			continue;
		}

		if (reg & EN7580_SEQ_SCU) {
			map = priv->scu;
			reg &= ~EN7580_SEQ_SCU;
		}

		/* Calibration and reset strobes must always reach hardware. */
		ret = regmap_write_bits(map, reg, seq[i].mask, value);
		if (ret)
			return ret;
	}

	return 0;
}

static int en7580_pcs_digital_reset(struct en7580_pcs *priv, int mode,
				    bool release)
{
	u32 ctrl = mode < 2 ? EN7580_XFI_CTRL : EN7580_HSGMII_CTRL;
	u32 first = mode < 2 ? BIT(15) : BIT(27);
	u32 second = mode < 2 ? BIT(16) : BIT(31);
	int ret;

	/* The vendor changes RX and TX reset separately, in this order. */
	ret = regmap_write_bits(priv->digital, ctrl, first, release ? first : 0);
	if (ret)
		return ret;
	return regmap_write_bits(priv->digital, ctrl, second, release ? second : 0);
}

static int en7580_pcs_eye_score(struct en7580_pcs *priv, unsigned int *score)
{
	u32 done, ready, window;
	int ret;

	*score = 0;

	ret = regmap_write_bits(priv->pma, EN7580_PMA_EYE_LATCH,
				EN7580_EYE_LATCH, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(priv->pma, EN7580_PMA_EYE_LATCH,
				EN7580_EYE_LATCH, EN7580_EYE_LATCH);
	if (ret)
		return ret;

	ret = regmap_read(priv->pma, EN7580_PMA_EYE_DONE, &done);
	if (ret)
		return ret;

	ret = regmap_read(priv->pma, EN7580_PMA_EYE_READY, &ready);
	if (ret)
		return ret;

	/* readout_EO() restarts CDR tracking when the horizontal eye is not ready. */
	if (!(ready & EN7580_EYE_HREADY))
		return en7580_pcs_sequence(priv, en7580_pcs_eye_cdr_seq,
					   ARRAY_SIZE(en7580_pcs_eye_cdr_seq), 0);

	/* Do not select an equalizer setting from stale or incomplete results. */
	if ((done & EN7580_EYE_DONE) != EN7580_EYE_DONE ||
	    !(ready & EN7580_EYE_VREADY))
		return 0;

	ret = regmap_read(priv->pma, EN7580_PMA_EYE_WINDOW, &window);
	if (ret)
		return ret;
	*score = abs((int)FIELD_GET(EN7580_EYE_RIGHT, window) -
		     (int)FIELD_GET(EN7580_EYE_LEFT, window));
	return 0;
}

static int en7580_pcs_eye_scan(struct en7580_pcs *priv, unsigned int mode)
{
	u32 original, best = 0, score, peaking;
	u32 best_score = 0;
	int ret, err;

	ret = regmap_read(priv->pma, EN7580_PMA_RX_EQ, &original);
	if (ret)
		return ret;

	ret = en7580_pcs_sequence(priv, en7580_pcs_eye_setup_seq,
				  ARRAY_SIZE(en7580_pcs_eye_setup_seq), mode);
	if (ret)
		return ret;

	ret = regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ_CTRL,
				EN7580_RX_EQ_GAIN_AUTO, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ,
				EN7580_RX_EQ_GAIN, FIELD_PREP(EN7580_RX_EQ_GAIN, 1));
	if (ret)
		return ret;

	/* XFI_EO_Scan(): sweep peaking 0..7 with LEQ gain fixed at 1. */
	for (peaking = 0; peaking <= FIELD_MAX(EN7580_RX_EQ_PEAKING); peaking++) {
		ret = regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ_CTRL,
					EN7580_RX_EQ_PEAKING_AUTO, 0);
		if (ret)
			return ret;

		ret = regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ,
					EN7580_RX_EQ_PEAKING,
					FIELD_PREP(EN7580_RX_EQ_PEAKING, peaking));
		if (ret)
			return ret;

		ret = en7580_pcs_sequence(priv, en7580_pcs_eye_cal_seq,
					  ARRAY_SIZE(en7580_pcs_eye_cal_seq), mode);
		if (ret)
			return ret;

		ret = en7580_pcs_sequence(priv, en7580_pcs_eye_measure_seq,
					  ARRAY_SIZE(en7580_pcs_eye_measure_seq), mode);
		if (!ret)
			ret = en7580_pcs_eye_score(priv, &score);

		/* Stop measurement even if a status read failed. */
		err = en7580_pcs_sequence(priv, en7580_pcs_eye_stop_seq,
					  ARRAY_SIZE(en7580_pcs_eye_stop_seq), mode);
		if (ret || err)
			return ret ?: err;

		if (score > best_score) {
			best_score = score;
			best = peaking;
		}
	}

	ret = regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ_CTRL,
				EN7580_RX_EQ_GAIN_AUTO, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ_CTRL,
				EN7580_RX_EQ_PEAKING_AUTO, 0);
	if (ret)
		return ret;

	/* A disconnected lane need not produce a usable eye during initial setup. */
	if (!best_score)
		return regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ,
					 EN7580_RX_EQ_GAIN | EN7580_RX_EQ_PEAKING,
					 original);

	dev_dbg(priv->dev, "RX eye peaking %u, horizontal opening %u\n",
		best, best_score);
	return regmap_write_bits(priv->pma, EN7580_PMA_RX_EQ,
				 EN7580_RX_EQ_PEAKING,
				 FIELD_PREP(EN7580_RX_EQ_PEAKING, best));
}

static unsigned int en7580_pcs_inband_caps(struct phylink_pcs *pcs,
					 phy_interface_t interface)
{
	/* The recovered path uses the external PHY for negotiation. */
	return LINK_INBAND_DISABLE;
}

static void en7580_pcs_link_down(struct phylink_pcs *pcs)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);

	regmap_set_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			EN7580_MAC_STOP | EN7580_MAC_TX_MASK);
}

static int en7580_pcs_config_locked(struct phylink_pcs *pcs,
				    unsigned int neg_mode,
				    phy_interface_t interface)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	int mode, ret;

	mode = en7580_pcs_mode(interface);
	if (mode < 0 || neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		return -EINVAL;

	if (priv->configured && priv->interface == interface)
		return 0;

	priv->configured = false;
	en7580_pcs_link_down(pcs);

	ret = regmap_set_bits(priv->scu, EN7580_SCU_RESET,
			      EN7580_SCU_XSI_PHY_RESET);
	if (ret)
		return ret;
	udelay(1);
	ret = regmap_clear_bits(priv->scu, EN7580_SCU_RESET,
				EN7580_SCU_XSI_PHY_RESET);
	if (ret)
		return ret;

	ret = en7580_pcs_sequence(priv, en7580_pcs_init_seq,
				 ARRAY_SIZE(en7580_pcs_init_seq), mode);
	if (ret)
		return ret;

	ret = en7580_pcs_digital_reset(priv, mode, false);
	if (ret)
		return ret;

	ret = en7580_pcs_sequence(priv, en7580_pcs_start_seq,
				 ARRAY_SIZE(en7580_pcs_start_seq), mode);
	if (ret)
		return ret;

	ret = en7580_pcs_eye_scan(priv, mode);
	if (ret)
		return ret;
	ret = en7580_pcs_sequence(priv, en7580_pcs_ready_seq,
				  ARRAY_SIZE(en7580_pcs_ready_seq), mode);
	if (ret)
		return ret;

	ret = en7580_pcs_digital_reset(priv, mode, true);
	if (ret)
		return ret;

	/* Reproduce xsi_mac_itf_reset while keeping traffic stopped. */
	ret = regmap_set_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_CNT_CLR,
			      AIROHA_PCS_XFI_GLB_CNT_CLR);
	if (ret)
		return ret;
	ret = regmap_write(priv->mac, EN7580_MAC_INT_STATUS, U32_MAX);
	if (ret)
		return ret;

	priv->interface = interface;
	priv->configured = true;
	dev_dbg(priv->dev, "XSI PCS configured for %s\n", phy_modes(interface));
	return 0;
}

static int en7580_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			     phy_interface_t interface,
			     const unsigned long *advertising,
			     bool permit_pause_to_mac)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	int ret;

	mutex_lock(&priv->lock);
	ret = en7580_pcs_config_locked(pcs, neg_mode, interface);
	priv->last_config_error = ret;
	mutex_unlock(&priv->lock);
	return ret;
}

static void en7580_pcs_get_state(struct phylink_pcs *pcs, unsigned int neg_mode,
				 struct phylink_link_state *state)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	u32 val;
	int mode = en7580_pcs_mode(priv->interface);
	static const int speeds[] = { SPEED_10000, SPEED_5000,
				      SPEED_2500, SPEED_1000 };

	state->link = false;
	if (!priv->configured || mode < 0)
		return;

	if (mode < 2) {
		if (regmap_read(priv->digital, EN7580_XFI_STATUS, &val))
			return;
		state->link = (val & EN7580_XFI_READY) == EN7580_XFI_READY;
	} else {
		if (regmap_read(priv->digital, EN7580_HSGMII_STATUS, &val))
			return;
		state->link = FIELD_GET(EN7580_HSGMII_STATE, val) ==
			      EN7580_HSGMII_READY;
	}
	state->speed = speeds[mode];
	state->duplex = DUPLEX_FULL;
}

static void en7580_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			      phy_interface_t interface, int speed, int duplex)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	u32 pause = AIROHA_PCS_XFI_TX_FC_EN | AIROHA_PCS_XFI_RX_FC_EN;

	if (!priv->configured)
		return;

	regmap_update_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			   AIROHA_PCS_XFI_TX_FC_EN | AIROHA_PCS_XFI_RX_FC_EN,
			   pause);
	regmap_clear_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			  EN7580_MAC_STOP | EN7580_MAC_TX_MASK);
}

static const struct phylink_pcs_ops en7580_pcs_ops = {
	.pcs_inband_caps = en7580_pcs_inband_caps,
	.pcs_config = en7580_pcs_config,
	.pcs_get_state = en7580_pcs_get_state,
	.pcs_link_up = en7580_pcs_link_up,
	.pcs_link_down = en7580_pcs_link_down,
	.pcs_disable = en7580_pcs_link_down,
};

static struct phylink_pcs *en7580_pcs_get(struct fwnode_reference_args *args,
					void *data)
{
	struct en7580_pcs *priv = data;

	if (args->nargs)
		return ERR_PTR(-EINVAL);
	return &priv->pcs;
}

static struct regmap *en7580_pcs_map(struct platform_device *pdev,
				   const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							 name);
	const struct regmap_config config = {
		.name = name,
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = res ? resource_size(res) - 4 : 0,
	};
	void __iomem *base;

	if (!res)
		return ERR_PTR(-EINVAL);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return ERR_CAST(base);
	return devm_regmap_init_mmio(&pdev->dev, base, &config);
}

static int en7580_pcs_status_show(struct seq_file *s, void *unused)
{
	struct en7580_pcs *priv = s->private;
	unsigned int i;
	u32 value;
	int ret;

	static const struct {
		u16 reg;
		const char *name;
	} registers[] = {
		{ 0x0004, "jcpll-ic" },
		{ 0x0008, "jcpll-bias" },
		{ 0x000c, "jcpll-sdm-order" },
		{ 0x0010, "jcpll-reserve" },
		{ 0x0200, "xfi-pll-power0" },
		{ 0x0204, "xfi-pll-power1" },
		{ 0x0760, "xfi-pll-status" },
		{ 0x0800, "jcpll-power0" },
		{ 0x0804, "jcpll-power1" },
		{ 0x0868, "jcpll-da-rg-control" },
		{ 0x0884, "xfi-tx-counter0" },
		{ 0x0888, "xfi-tx-counter1" },
		{ 0x088c, "xfi-tx-counter2" },
		{ 0x090c, "pll-stable-counter" },
		{ 0x0910, "pll-stop-counter" },
		{ 0x0974, "rx-frequency-status" },
		{ 0x0978, "pll-frequency-status" },
		{ 0x0980, "jcpll-frequency-status" },
	};

	mutex_lock(&priv->lock);
	seq_printf(s, "configured: %u\ninterface: %s\nlast-config-error: %d\n",
		   priv->configured, phy_modes(priv->interface),
		   priv->last_config_error);

	/* The register window is only sampled after PMA startup completed. */
	if (!priv->configured)
		goto out;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = regmap_read(priv->pma, registers[i].reg, &value);
		if (ret)
			seq_printf(s, "%04x %-22s error %d\n", registers[i].reg,
				   registers[i].name, ret);
		else
			seq_printf(s, "%04x %-22s %#010x\n", registers[i].reg,
				   registers[i].name, value);
	}

	ret = regmap_read(priv->digital, EN7580_XFI_STATUS, &value);
	if (!ret)
		seq_printf(s, "pcs-xfi-status: %#010x\n", value);

	ret = regmap_read(priv->digital, EN7580_HSGMII_STATUS, &value);
	if (!ret)
		seq_printf(s, "pcs-hsgmii-status: %#010x\n", value);
out:
	mutex_unlock(&priv->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en7580_pcs_status);

static void en7580_pcs_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static void en7580_pcs_debugfs_init(struct en7580_pcs *priv)
{
	struct dentry *dir;
	char *name;

	name = devm_kasprintf(priv->dev, GFP_KERNEL, "airoha-xsi-pcs-%s",
			      dev_name(priv->dev));
	if (!name)
		return;

	dir = debugfs_create_dir(name, NULL);
	if (IS_ERR_OR_NULL(dir))
		return;

	debugfs_create_file("status", 0444, dir, priv, &en7580_pcs_status_fops);
	if (devm_add_action_or_reset(priv->dev, en7580_pcs_debugfs_remove, dir))
		dev_warn(priv->dev, "failed to retain PCS debugfs status\n");
}

static int en7580_pcs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct en7580_pcs *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->lock);

	priv->mac = en7580_pcs_map(pdev, "mac");
	if (IS_ERR(priv->mac))
		return dev_err_probe(dev, PTR_ERR(priv->mac), "failed to map MAC\n");

	priv->pma = en7580_pcs_map(pdev, "pma");
	if (IS_ERR(priv->pma))
		return dev_err_probe(dev, PTR_ERR(priv->pma), "failed to map PMA\n");

	priv->digital = en7580_pcs_map(pdev, "pcs");
	if (IS_ERR(priv->digital))
		return dev_err_probe(dev, PTR_ERR(priv->digital), "failed to map PCS\n");

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu), "failed to get NP SCU\n");

	priv->chip_scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "airoha,chip-scu");
	if (IS_ERR(priv->chip_scu))
		return dev_err_probe(dev, PTR_ERR(priv->chip_scu), "failed to get chip SCU\n");

	/* en7580_xfi_phy_dev_init: enable XSI and its clock. */
	ret = regmap_set_bits(priv->chip_scu, EN7580_CHIP_SCU_XSI_CTRL,
			      EN7580_CHIP_SCU_XSI_ENABLE);
	if (ret)
		return ret;

	ret = regmap_clear_bits(priv->scu, EN7580_SCU_XSI_CTRL,
				EN7580_SCU_XSI_DISABLE);
	if (ret)
		return ret;

	ret = regmap_clear_bits(priv->scu, EN7580_SCU_XSI_CLK,
				EN7580_SCU_XSI_CLK_PD);
	if (ret)
		return ret;

	priv->pcs.ops = &en7580_pcs_ops;
	priv->pcs.poll = true;
	__set_bit(PHY_INTERFACE_MODE_10GBASER, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_5GBASER, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_SGMII, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_1000BASEX, priv->pcs.supported_interfaces);

	platform_set_drvdata(pdev, priv);

	priv->provider = fwnode_pcs_add_provider(dev_fwnode(dev), en7580_pcs_get, priv);
	if (IS_ERR(priv->provider))
		return dev_err_probe(dev, PTR_ERR(priv->provider),
				     "failed to register PCS provider\n");

	en7580_pcs_debugfs_init(priv);
	return 0;
}

static void en7580_pcs_remove(struct platform_device *pdev)
{
	struct en7580_pcs *priv = platform_get_drvdata(pdev);

	fwnode_pcs_del_provider(priv->provider);
}

static const struct of_device_id en7580_pcs_of_match[] = {
	{ .compatible = "airoha,en7580-pcs" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7580_pcs_of_match);

static struct platform_driver en7580_pcs_driver = {
	.probe = en7580_pcs_probe,
	.remove = en7580_pcs_remove,
	.driver = {
		.name = "en7580-pcs",
		.of_match_table = en7580_pcs_of_match,
	},
};
module_platform_driver(en7580_pcs_driver);

MODULE_DESCRIPTION("Airoha EN7580 PCS driver");
MODULE_LICENSE("GPL");
