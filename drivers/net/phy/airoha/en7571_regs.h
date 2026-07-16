/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register map and calibration constants for the Airoha/Econet EN7571
 * xPON LDDLA controller (laser-driver dual-closed-loop power control,
 * burst-timing generation, shared SVADC + dedicated PWRADC, APD bias and
 * loss-of-signal detection).
 *
 * Offsets are byte addresses into the device's flat register file
 * (0x0000-0x0300).  Registers are 32-bit and word aligned, but the I2C
 * pointer is a 16-bit byte sub-address so an individual byte lane of a 32-bit
 * register is addressed as <reg> + <lane>.  Data on the wire is little-endian:
 * lane 0 is the least-significant byte of the 32-bit word.
 */
#ifndef _EN7571_REGS_H
#define _EN7571_REGS_H

/* --- Control / analog block (0x0000-0x0044) --- */
#define EN7571_TIAMUX			0x0000	/* TIA mux (PWRADC path, TxSD) */
#define EN7571_MPDH			0x0004	/* MPDH set-point (referenced by name) */
#define EN7571_T1DELAY			0x0008	/* T-GEN timing, ERC enable, T0/T1 delay */
#define EN7571_TIASD			0x000c	/* TIA signal-detect threshold */
#define EN7571_T0C			0x0010	/* T0C/T1C burst counters (read-only) */
#define EN7571_LA_PWD			0x0014	/* RSSI cal/gain, Rx high-Z */
#define EN7571_LA_RSV2			0x0018	/* LA reserved #2 */
#define EN7571_BGCKEN			0x001c	/* Bandgap clock enable */
#define EN7571_PI_TGEN			0x0020	/* PI/TGEN control */
#define EN7571_SVADC_PD			0x0024	/* SVADC mux/power, LOS ADC-rev bits */
#define EN7571_SVADC_REV3		0x0028	/* SVADC revision-3 enables */
#define EN7571_I2C_SCL_SMT		0x002c	/* I2C SCL slew-rate / Schmitt */
#define EN7571_APD_DAC_CODE		0x0030	/* APD DAC code, soft-start, control enable */
#define EN7571_RG_ANA_CTRL1		0x0038	/* analog control 1 (RG_PWRK1 for PWRADC) */
#define EN7571_RG_EFUSE_TEMP		0x0044	/* eFuse temperature read */

/* --- Safety / LOS block (0x0100-0x0170) --- */
#define EN7571_SAFE_PROTECT		0x0100	/* safe-circuit / Tx-fault latch */
#define EN7571_LOS_CTRL1		0x011c	/* LOS/SD thresholds + cal trigger */
#define EN7571_LOS_CTRL2		0x0120	/* LOS confidence / count */
#define EN7571_LOS_CAL_TIMER		0x0124	/* LOS calibration timer */
#define EN7571_LOS_CAL_TIMEOUT_CNT	0x0128	/* LOS calibration timeout counter */
#define EN7571_LOS_CAL_TIMEOUT		0x012c	/* LOS calibration timeout threshold */
#define EN7571_LOS_DBG_RG		0x0130	/* LOS status */
#define EN7571_P0_PWR_CTRL_CS1		0x0134	/* phase-0 (Ibias) coarse #1 */
#define EN7571_P0_PWR_CTRL_CS2		0x0138	/* phase-0 coarse #2 */
#define EN7571_P0_PWR_CTRL_CS3		0x013c	/* phase-0 readback (Ibias now, bytes 2-3) */
#define EN7571_P0_PWR_CTRL_LCH		0x0140	/* phase-0 latch */
#define EN7571_P1_PWR_CTRL_CS1		0x0144	/* phase-1 (Imod) coarse #1 */
#define EN7571_P1_PWR_CTRL_CS2		0x0148	/* phase-1 IMOD DAC (fast Imod path) */
#define EN7571_P1_PWR_CTRL_CS3		0x014c	/* phase-1 readback (Imod now, bytes 2-3) */
#define EN7571_P1_PWR_CTRL_LCH		0x0150	/* phase-1 latch */
#define EN7571_ADC_PROBE_STATUS		0x0154	/* 16-bit SVADC sample (little-endian) */
#define EN7571_PROBE_CONTROL		0x0158	/* SVADC latch (byte 1 bit 4) */
#define EN7571_DUMMY			0x015c	/* HW-reset, burst-ctrl, revision id */
#define EN7571_OVFL_DBG_CLR		0x0160	/* overflow debug clear */
#define EN7571_APD_OVP_LATCH		0x0164	/* APD over-voltage-protection latch */
#define EN7571_ROGUE_ONU_DET_CTRL	0x0168	/* rogue-ONU detect (clear at +1), Tx SD */
#define EN7571_ERC_FILTER_CTRL		0x016c	/* ERC digital-loop filter */
#define EN7571_FT_ADC_CLK_CLR		0x0170	/* detection id1 */

/* --- Power-control / ADC block (0x0200-0x0300) --- */
#define EN7571_RG_ADLCH_BEN_CTRL	0x019c
#define EN7571_RG_ADLCH_CTRL		0x0200	/* RSSI accumulate/latch/valid */
#define EN7571_RG_PWRADC_DATA		0x0204	/* PWRADC data (TxSD) */
#define EN7571_RG_PWRADC_DATA2		0x0208	/* PWRADC count/trig/valid/data */
#define EN7571_PWR_LIMITER_0		0x0220	/* Ibias minimum */
#define EN7571_PWR_LIMITER_2		0x0224	/* Imod minimum */
#define EN7571_PWR_CTRL_0		0x0228	/* enable, DCL reset-b */
#define EN7571_PWR_CTRL_1		0x022c	/* MPDH step / averaging */
#define EN7571_PWR_CTRL_2		0x0230	/* KT Imod-adjust select / coeff */
#define EN7571_PWR_CTRL_3		0x0234	/* stepmu / stepsize 0-2 */
#define EN7571_PWR_CTRL_4		0x0238	/* stepsize 3-5 */
#define EN7571_PWR_CTRL_5		0x023c	/* phase-1 timer */
#define EN7571_PWR_CTRL_6		0x0240	/* Imax */
#define EN7571_PWR_CTRL_7		0x0244	/* Imax */
#define EN7571_PWR_CTRL_8		0x0248	/* Iav/Imod variation, MPDH updatePd */
#define EN7571_PWR_CTRL_9		0x024c	/* Iav/Imod command (loop set-point) */
#define EN7571_PWR_CTRL_A		0x0250	/* Imod/Iav scale (TC disable) */
#define EN7571_PWR_CTRL_B		0x0254	/* adjust intervals */
#define EN7571_PWR_CTRL_C		0x0258	/* Pav/P1 force mode */
#define EN7571_PWR_CTRL_D		0x025c	/* Pav/P1/MPDH set-point */
#define EN7571_PWR_CTRL_E		0x0260	/* PWRADC averaging */
#define EN7571_PWR_CTRL_FLASH_1		0x027c	/* Iav/Imod flash-seed mirror */
#define EN7571_PWR_CTRL_FLASH_2		0x0280	/* Pav/P1 flash-seed mirror */
#define EN7571_PWR_CTRL_FLASH_3		0x0284
#define EN7571_LINK_ADJ_0		0x0288
#define EN7571_RO_PWR_CTRL_0		0x02a4	/* read-only: Pav now (b0-1), P1 now (b2-3) */
#define EN7571_RO_PWR_CTRL_3		0x02b0	/* read-only: Iav now (b0-1) */
#define EN7571_RG_PWR_CTRL_BEN_0	0x02b4	/* PWRADC block enable */
#define EN7571_SW_RESET			0x0300	/* synchronous software reset */

/* IMOD DAC alias used by the rev-2 fast cross-Imod path (= P1_PWR_CTRL_CS2). */
#define EN7571_IMOD_DAC			0x0148

/* --- Detection --- */
#define EN7571_ID1			0x03	/* FT_ADC_CLK_CLR byte 0 */
#define EN7571_ID2_MIN			0x03	/* DUMMY byte 0, minimum */
#define EN7571_ID2_REV2			0x03	/* DUMMY byte 0 == 0x03 -> silicon rev 2 */

/* --- SW / HW reset --- */
#define EN7571_SW_RESET_MASK		0xf8	/* SW_RESET byte 0: keep upper bits */
#define EN7571_SW_RESET_SET		0x01
#define EN7571_HWRESET_MASK		0xfe	/* DUMMY byte 1: clear reset bit */
#define EN7571_HWRESET_SET		0x01

/* DUMMY burst-ctrl (byte 1). */
#define EN7571_BURST_CTRL_MASK		0xfb
#define EN7571_BURST_CTRL_OFF		0x04

/* --- 7571-mode enable / DCL (PWR_CTRL_0) --- */
#define EN7571_PWR_CTRL_EN_MASK		0xfe	/* byte 0 */
#define EN7571_PWR_CTRL_EN		0x01
#define EN7571_DCL_RST_B_MASK		0xfe	/* byte 1 */
#define EN7571_DCL_RST_B		0x01

/* --- Safe-circuit reset (SAFE_PROTECT byte 1) --- */
#define EN7571_SAFE_CIRCUIT_MASK	0xbf	/* SAFE_PROTECT byte 1 keep-mask */
#define EN7571_SAFE_CIRCUIT_RESET	0x40

/* --- reg_init power-loop settings --- */
#define EN7571_DA_IBIAS_MIN		0x080	/* PWR_LIMITER_0 bytes 2-3 */
#define EN7571_DA_IMOD_MIN		0x032	/* PWR_LIMITER_2 bytes 2-3 */
#define EN7571_IMAX			0xfff	/* PWR_CTRL_6 bytes 2-3 */
#define EN7571_CMD_IAV_SETTING		0x300	/* PWR_CTRL_9 bytes 0-1 */
#define EN7571_CMD_IMOD_SETTING		0x200	/* PWR_CTRL_9 bytes 2-3 */
#define EN7571_STEPMU_SEL_MASK		0xfc	/* PWR_CTRL_3 byte 0 */
#define EN7571_STEPMU_SEL		0x02
#define EN7571_STEPSIZE_MASK		0xc0	/* keep mask when freezing a step */
#define EN7571_STEPSIZE0		0x3f	/* Pav step #0 (PWR_CTRL_3 byte 1) */
#define EN7571_STEPSIZE1		0x25	/* MPDH step #0 (PWR_CTRL_3 byte 2) */
#define EN7571_STEPSIZE2		0x3f	/* Pav step #1 (PWR_CTRL_3 byte 3) */
#define EN7571_STEPSIZE3		0x25	/* MPDH step #1 (PWR_CTRL_4 byte 0) */
#define EN7571_STEPSIZE4		0x3f	/* Pav step #2 (PWR_CTRL_4 byte 1) */
#define EN7571_STEPSIZE5		0x25	/* MPDH step #2 (PWR_CTRL_4 byte 2) */
#define EN7571_TIMER_PHZ1_NUM		0x007	/* PWR_CTRL_5 bytes 0-1 */
#define EN7571_P1_STEPSIZE_MASK		0xc0	/* PWR_CTRL_1 byte 3 */
#define EN7571_P1_STEPSIZE		0x02
#define EN7571_MPDX_SHTBIT_MASK		0xf8	/* PWR_CTRL_1 byte 0 */
#define EN7571_MPDX_SHTBIT		0x07
#define EN7571_PAVG_SHTBIT_MASK		0xf8	/* PWR_CTRL_E byte 3 */
#define EN7571_PAVG_SHTBIT_64		0x03
#define EN7571_MPDH_UPDATEPD_MASK	0xf8	/* PWR_CTRL_8 byte 3 */
#define EN7571_MPDH_UPDATEPD		0x01
#define EN7571_DELTA_IMOD_MAX		0x02	/* PWR_CTRL_8 byte 1 */
#define EN7571_DELTA_IAV_MAX		0x04	/* PWR_CTRL_8 byte 0 */
#define EN7571_IMOD_IAVSCALE_MASK	0x80	/* PWR_CTRL_A byte 2 (disable TC) */

/* PWR_CTRL_C byte 3: Pav/P1 force vs auto-lock. */
#define EN7571_PAV_P1_FORCE_MASK	0xfc
#define EN7571_PAV_P1_FORCE		0x03

/* P0/P1_PWR_CTRL_CS3 byte 0: open-loop mode (ERC). */
#define EN7571_ERC_OPEN_LOOP_MASK	0xfc
#define EN7571_ERC_OPEN_LOOP		0x02

/* --- en7571_info() loop-readback selectors --- */
#define EN7571_SELECT_PAV_CAL		1	/* PWR_CTRL_D b0-1 & 0xfff */
#define EN7571_SELECT_IAV_NOW		2	/* RO_PWR_CTRL_3 b0-1 & 0xfff */
#define EN7571_SELECT_IBIAS_NOW		3	/* P0_PWR_CTRL_CS3 b2-3 & 0xfff */
#define EN7571_SELECT_IMOD_NOW		4	/* P1_PWR_CTRL_CS3 b2-3 & 0xfff */
#define EN7571_SELECT_P1_CAL		6	/* PWR_CTRL_D b2-3 & 0x3ff */
#define EN7571_SELECT_PAV_NOW		7	/* RO_PWR_CTRL_0 b0-1 & 0xfff */
#define EN7571_SELECT_P1_NOW		8	/* RO_PWR_CTRL_0 b2-3 & 0x3ff */

/* --- SVADC mux selects / masks --- */
#define EN7571_ADC_SELECT_MASK		0xe1	/* byte 0: preserves non-channel bits */
#define EN7571_ADC_SELECT_VOLTAGE	0x00	/* channel field cleared -> VCC */
#define EN7571_ADC_SELECT_TEMPERATURE	0x08
#define EN7571_ADC_SELECT_BG_1V76	0x06
#define EN7571_ADC_RSSI_ENABLE		0x02
#define EN7571_ADC_BG0V875_MASK		0xb3	/* byte 1 */
#define EN7571_ADC_SELECT_BG_0V875	0x4c
#define EN7571_ADC_LATCH_MASK		0xef	/* PROBE_CONTROL+1 byte 0 */
#define EN7571_ADC_LATCH		0x10
#define EN7571_ADC_0V5			0xb7	/* code for ~0.5 V (precision floor) */
#define EN7571_ADC_RSSI_DEFEND_NOISE	0x32	/* margin below Vref that breaks sweep */

/* LOS ADC-revision enables (SVADC_PD bytes 2/3). */
#define EN7571_LOS_ADCREV1_MASK		0xbf	/* byte 2 */
#define EN7571_LOS_ADCREV1_ENABLE	0x40
#define EN7571_LOS_ADCREV2_MASK		0xfb	/* byte 3 */
#define EN7571_LOS_ADCREV2_ENABLE	0x04

/* --- TIA mux --- */
#define EN7571_TIA_MUX_MASK		0xf1
#define EN7571_TIA_MUX_TIASD		0x02
#define EN7571_TIA_MUX_TIAFLT		0x08
#define EN7571_TIASD_UPPER_MASK		0xfe	/* TIASD byte 1: keep upper, set bit 8 */

/* --- LA_PWD: RSSI cal/gain, Rx high-Z --- */
#define EN7571_LA_RX_HIGHZ_MASK		0xef	/* byte 0 */
#define EN7571_LA_RX_HIGHZ_ENABLE	0x10
#define EN7571_RSSI_CAL_MASK		0xef	/* byte 1 */
#define EN7571_RSSI_CAL_EN		0x10
#define EN7571_RSSI_VMODE_MASK		0xbf	/* byte 1 bit 6 */
#define EN7571_RSSI_VMODE_EN		0x40
#define EN7571_RSSI_GAIN_MASK		0xf8	/* byte 2 [2:0] */
#define EN7571_RSSI_GAIN_DEFAULT	0x05

/* --- RSSI accumulate (RG_ADLCH_CTRL byte 3) --- */
#define EN7571_ADLCH_COUNT_MASK		0x8f
#define EN7571_ADLCH_COUNT_1024		0x70
#define EN7571_ADLCH_TRIG		0x80	/* byte 3 bit 7 */
#define EN7571_ADLCH_SUM_MASK		0xfffff	/* 20-bit accumulated sum */

/* --- PWRADC (RG_ANA_CTRL1, RG_PWRADC_DATA2, RG_PWR_CTRL_BEN_0) --- */
#define EN7571_RG_PWRK1_MASK		0xef	/* RG_ANA_CTRL1 byte 3 */
#define EN7571_RG_PWRK1_1		0x10
#define EN7571_PWRADC_COUNT_MASK	0xf8	/* RG_PWRADC_DATA2 byte 3 */
#define EN7571_PWRADC_COUNT_1024	0x07
#define EN7571_PWRADC_TRIG		0x80	/* byte 3 bit 7 */
#define EN7571_PWRADC_VALID		0x10	/* byte 2 bit 4 */
#define EN7571_PWRADC_SUM_MASK		0xfffff
#define EN7571_PWR_CTRL_BEN_MASK	0xfc	/* RG_PWR_CTRL_BEN_0 byte 0 */
#define EN7571_PWR_CTRL_BEN_SET		0x01

/* --- KT (PWR_CTRL_2) --- */
#define EN7571_IMOD_ADJ_SEL_MASK	0xfe	/* byte 0 */
#define EN7571_IMOD_ADJ_SEL_KT		0x01

/* --- ERC / TGEN (T1DELAY) --- */
#define EN7571_ERC_ENABLE_MASK		0xf7	/* T1DELAY byte 3 bit 3 */
#define EN7571_ERC_ENABLE		0x08
#define EN7571_T1_T0_DELAY_SETTING1	0x66
#define EN7571_T1_T0_DELAY_GPON		0xaa
#define EN7571_T1_T0_DELAY_EPON		0x77
#define EN7571_T1_DELAY_MASK		0xf0	/* T1DELAY byte 0: keep T1 nibble */
#define EN7571_T1_DELAY_NORMAL		0x00	/* clear the T0 delay nibble */
#define EN7571_TIMER_RESET_VALUE	0x7f	/* T1/T0 timer reset */
#define EN7571_TGEN_RESET_MASK		0xdf	/* T1DELAY byte 3 bit 5 */
#define EN7571_TGEN_RESET_T1T0		0x20
#define EN7571_TGEN_METHOD2_MASK	0xfe	/* T1DELAY byte 3 bit 0 */
#define EN7571_TGEN_METHOD2_ENABLE	0x01

/* --- LOS --- */
#define EN7571_LOS_CAL_TRIG_MASK	0xfe	/* LOS_CTRL1 byte 0 */
#define EN7571_LOS_CAL_TRIG		0x01
#define EN7571_LOS_AIN_STABLE_MASK	0xe0	/* LOS_CTRL1 byte 1 */
#define EN7571_LOS_AIN_STABLE_SET	0x1f
#define EN7571_LOS_COMP_THLD_MASK	0x80	/* LOS_CTRL1 bytes 2/3: keep bit 7 */
#define EN7571_LOS_COMP_THLD_H_DEF	0x30	/* Rx SD default (byte 2) */
#define EN7571_LOS_COMP_THLD_L_DEF	0x20	/* Rx LOS default (byte 3) */
#define EN7571_LOS_CONFIDENCE_MASK	0xe0	/* LOS_CTRL2 byte 1 */
#define EN7571_LOS_CONFIDENCE_SET	0x1f
#define EN7571_LOS_CNT_MASK		0x80	/* LOS_CTRL2 byte 0 */
#define EN7571_LOS_CNT_SET		0x05

/* --- Rogue ONU (ROGUE_ONU_DET_CTRL+1 byte 0) --- */
#define EN7571_ROGUE_ONU_MASK		0xfe
#define EN7571_ROGUE_ONU_CLEAR		0x01

/* --- APD (APD_DAC_CODE) --- */
#define EN7571_APD_SOFTSTART_ENABLE	0x20	/* byte 2 bit 5 */
#define EN7571_APD_CONTROL_ENABLE	0x01	/* byte 1 bit 0 */

/*
 * Flash (calibration NVM) byte offsets.  The blob is 100 little-endian
 * 32-bit words; index = offset >> 2.  A word equal to 0xffffffff is "erased"
 * and selects the corresponding compiled-in default.
 */
#define EN7571_FLASH_WORDS		100
#define EN7571_FLASH_DEFAULT_WORDS	40
#define EN7571_FLASH_ERASED		0xffffffff

#define EN7571_FL_IAV_IMOD		0x000	/* [27:16]=Iav, [11:0]=Imod */
#define EN7571_FL_PAV_P1		0x004	/* [27:16]=APC/Pav, [9:0]=ERC/P1 */
#define EN7571_FL_OPEN_LOOP_SEED	0x008	/* open-loop: [27:16]=Ibias, [11:0]=Imod */
#define EN7571_FL_T0CT1C		0x00c	/* [7:0]=delay, [23:16]=T0C, [31:24]=T1C */
#define EN7571_FL_APD_SLOPE_UP		0x010	/* V/degC x100, above knee */
#define EN7571_FL_APD_SLOPE_DN		0x014	/* V/degC x100, below knee */
#define EN7571_FL_APD_CHANGE_POINT	0x018	/* APD voltage at 25 degC, V x100 */
#define EN7571_FL_T_APD			0x01c	/* APD update period (s, min 10) */
#define EN7571_FL_LOS_THLD		0x020	/* [6:0]=Rx LOS, [22:16]=Rx SD */
#define EN7571_FL_TEMP_OFFSET		0x024	/* [31:16]=Env off x10, [7:0]=BOSA off degC */
#define EN7571_FL_EXTCAL_TEMP		0x028	/* [31:16]=offset degC, [15:0]=slope x256 */
#define EN7571_FL_INTERNAL_DDMI		0x02c	/* 0=off, 1=on, 2=on+fast */
#define EN7571_FL_APD_VOLTAGE_1		0x030	/* [31:16]=V@0x00 x10, [15:0]=V@0x40 x10 */
#define EN7571_FL_APD_VOLTAGE_2		0x034	/* [31:16]=V@0x80 x10, [15:0]=V@0xC0 x10 */
#define EN7571_FL_EXTCAL_BIAS		0x038	/* [31:16]=offset 2uA, [15:0]=slope x256 */
#define EN7571_FL_EXTCAL_MOD		0x03c	/* [31:16]=offset 2uA, [15:0]=slope x256 */
#define EN7571_FL_DDMI_TX_P1		0x040	/* [31:16]=power mWx100, [15:0]=PWRADC */
#define EN7571_FL_DDMI_TX_P2		0x044	/* reserved */
#define EN7571_FL_TIA_SETTING		0x048	/* [31:16]=TIA gain, [15:0]=TIA bandwidth */
#define EN7571_FL_DDMI_RX_P1		0x050	/* [31:16]=power uWx100, [15:0]=RSSI */
#define EN7571_FL_DDMI_RX_P2		0x054
#define EN7571_FL_DDMI_RX_P3		0x058
#define EN7571_FL_DDMI_RX_ALG_SEL	0x05c	/* 0xfffffff1 = conic Rx fit (unused) */
#define EN7571_FL_MBI_DELAY		0x060	/* LDD/MBI parameter (cross-Imod udelay) */
#define EN7571_FL_KT			0x064	/* [15:0]=factor_HT, [31:16]=factor_LT */
#define EN7571_FL_KT_SWITCH		0x068	/* 2=inverse, 3=enhanced */
#define EN7571_FL_CRS			0x06c	/* 0=disable cross-Imod MBI path */
#define EN7571_FL_IBIAS_LIMITER		0x070	/* [27:16]=Ibias_min, [11:0]=Ibias_max */
#define EN7571_FL_IMOD_LIMITER		0x074	/* [27:16]=Imod_min, [11:0]=Imod_max */
#define EN7571_FL_ENV_OFFSET		0x08c	/* calibrated environment temperature degC x10 */
#define EN7571_FL_CL_SWITCH		0x090	/* LOOP_SEL: 1=SCL, 2=open, 3=APC/ERC LUT */
#define EN7571_FL_MAGIC			0x094	/* PON-mode magic number */
#define EN7571_FL_LUT_BASE		0x0a0	/* 16 per-temperature Ibias/Imod anchors */
#define EN7571_FL_LUT_END		0x0dc

/* Loop-select values (flash 0x090). */
#define EN7571_LOOP_SCL			1
#define EN7571_LOOP_OPEN		2
#define EN7571_LOOP_LUT			3

/* Field masks within flash words. */
#define EN7571_FL_IAV_MASK		0x0fff0000
#define EN7571_FL_IMOD_MASK		0x00000fff
#define EN7571_FL_PAV_MASK		0x0fff0000
#define EN7571_FL_P1_MASK		0x000003ff
#define EN7571_FL_RX_LOS_MASK		0x0000007f
#define EN7571_FL_RX_SD_MASK		0x007f0000
#define EN7571_FL_KT_DEFAULT		0x00060020

/* PON-mode magic numbers (flash offset 0x094). */
#define EN7571_MAGIC_GPON		0x07050701
#define EN7571_MAGIC_EPON		0xe7050701
#define EN7571_MAGIC_XPON		0xa7050701	/* adapter mode, runs as GPON */

/* xPON mode enumeration (struct en7571_priv .pon_mode). */
#define EN7571_PON_UNKNOWN		(-1)
#define EN7571_PON_EPON			0
#define EN7571_PON_GPON			1

/* KT modes (struct en7571_priv .kt). */
#define EN7571_KT_OFF			0
#define EN7571_KT_NORMAL		1
#define EN7571_KT_INVERSE		2
#define EN7571_KT_ENHANCED		3

/* DDMI refresh modes (flash offset 0x02c). */
#define EN7571_DDMI_OFF			0
#define EN7571_DDMI_ON			1
#define EN7571_DDMI_FAST		2

/*
 * Calibration scale constants for fixed-point use, in the units documented
 * next to each constant.
 */
#define EN7571_TEMP_OFFSET_X10_DEF	4958	/* ADC code at 0 degC, x10 (495.8) */
#define EN7571_TEMP_SLOPE_X10_DEF	3275	/* degC/V scale, x10 (327.5) */
#define EN7571_EFUSE_FIXED_OFFSET_MC	3440	/* fixed eFuse correction (3.44 degC) */
#define EN7571_EFUSE_STEP_MC		300	/* eFuse code step (0.3 degC) */
#define EN7571_EFUSE_SIGN_THRESHOLD	128	/* two's-complement boundary (8-bit) */
#define EN7571_BOSA_TEMP_OFFSET_MC	5000	/* IC->BOSA delta (5 degC) */
#define EN7571_APD_KNEE_TEMP_MC		25000	/* APD knee temperature (25 degC) */

/* Two-point bandgap reference voltages in nanovolts. */
#define EN7571_BG_1V76_NV		1760000000LL
#define EN7571_BG_0V875_NV		875000000LL
#define EN7571_ADC_BANDGAP_MIN_DELTA	0	/* slope valid when spread > 0 */
#define EN7571_DEFAULT_SLOPE_NV		5474000	/* default ADC slope, 0.005474 V/code */

#define EN7571_VOLTAGE_8472_NV		100000	/* SFF-8472 supply-voltage LSB = 100 uV */

/* Current scaling: register code -> microamps. */
#define EN7571_BIAS_UA_PER_CODE_X100	2442	/* 0.02442 mA/code, x100 */
#define EN7571_MOD_UA_PER_CODE_X100	2198	/* 0.02198 mA/code, x100 */
#define EN7571_BIAS_8472_UA_PER_LSB	2	/* DDMI bias word LSB = 2 uA */

/* APD dual-slope defaults. */
#define EN7571_APD_KNEE_MV_DEF		35000	/* voltage at 25 degC, 35.0 V */
#define EN7571_APD_SLOPE_UP_UV_DEF	100000	/* 0.10 V/degC above knee, uV/degC */
#define EN7571_APD_SLOPE_DN_UV_DEF	70000	/* 0.07 V/degC below knee, uV/degC */
#define EN7571_APD_V00_MV_DEF		30000	/* breakpoint @ code 0x00, 30.0 V */
#define EN7571_APD_V40_MV_DEF		36000	/* breakpoint @ code 0x40, 36.0 V */
#define EN7571_APD_V80_MV_DEF		42000	/* breakpoint @ code 0x80, 42.0 V */
#define EN7571_APD_VC0_MV_DEF		45600	/* breakpoint @ code 0xc0, 45.6 V */
#define EN7571_T_APD_DEFAULT		60	/* APD update interval (s) */
#define EN7571_T_APD_MIN		10

/* RSSI gain table (LA_PWD byte 2 [2:0] -> multiplicative factor). */
#define EN7571_RSSI_IDEAL_IR_X100	35	/* RSSI ideal IR 0.35, x100 */

/* SW temperature-compensation loop. */
#define EN7571_SWKT_IMOD_EPISODE	0x02e	/* Imod dead-band */
#define EN7571_SWKT_PERIOD		10	/* KT loop period (s) */

/* Alarm thresholds (SFF-8472 word units). */
#define EN7571_TX_PWR_LOW_THLD		0x2710	/* 0.0 dBm   (0.1 uW units) */
#define EN7571_TX_PWR_HIGH_THLD		0x8a99	/* +5.5 dBm */
#define EN7571_TX_BIAS_LOW_THLD		0x01f4	/* 1 mA      (2 uA units) */
#define EN7571_TX_BIAS_HIGH_THLD	0xc350	/* 100 mA */
#define EN7571_RX_PWR_LOW_THLD		0x000a	/* -30 dBm   (0.1 uW units) */
#define EN7571_RX_PWR_HIGH_THLD		0x09cf	/* -6 dBm */
#define EN7571_VOLT_LOW_THLD		0x7148	/* 2.9 V     (100 uV units) */
#define EN7571_VOLT_HIGH_THLD		0x9088	/* 3.7 V */
#define EN7571_TEMP_LOW_THLD		0xfb00	/* -5 degC   (1/256 degC) */
#define EN7571_TEMP_HIGH_THLD		0x5500	/* +85 degC */

#endif /* _EN7571_REGS_H */
