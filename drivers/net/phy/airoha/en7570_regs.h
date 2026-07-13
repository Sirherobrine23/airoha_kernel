/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register map and calibration constants for the Airoha/Econet EN7570
 * xPON LDDLA controller (burst-mode laser driver + limiting post-amplifier
 * + digital diagnostic monitoring).
 *
 * Offsets are byte addresses into the device's flat register file
 * (0x0000-0x0300).  Registers are word aligned but the I2C pointer is a
 * 16-bit byte address, so individual byte lanes of a 32-bit register are
 * addressed as <reg> + <lane>.  Data on the wire is little-endian: lane 0 is
 * the least-significant byte of the 32-bit word.
 */
#ifndef _EN7570_REGS_H
#define _EN7570_REGS_H

/* --- Block 0x0000-0x0030: TIA, LA, bandgap, APD --- */
#define EN7570_TIAMUX			0x0000	/* TIA mux control */
#define EN7570_MPDH			0x0004	/* MPD targets: byte0=MPDL, byte2=MPDH */
#define EN7570_T1DELAY			0x0008	/* T1/T0 burst delay & ERC enable */
#define EN7570_TIASD			0x000c	/* TIA automatic signal-detect threshold */
#define EN7570_T0C			0x0010	/* T0 counter measurement (read-only) */
#define EN7570_LA_PWD			0x0014	/* Limiting-amp power-down + RSSI control */
#define EN7570_LA_RSV2			0x0018	/* LA reserved #2 */
#define EN7570_BGCKEN			0x001c	/* Bandgap clock enable */
#define EN7570_PI_TGEN			0x0020	/* PI/TGEN control (ERC bits) */
#define EN7570_SVADC_PD			0x0024	/* Supply-voltage / ADC mux & power-down */
#define EN7570_SVADC_REV3		0x0028	/* ADC revision-3 enables */
#define EN7570_I2C_SCL_SMT		0x002c	/* I2C SCL slew-rate / Schmitt */
#define EN7570_APD_DAC_CODE		0x0030	/* APD DAC code, soft-start, control enable */

/* --- Block 0x0100-0x0130: safety, impedance cal, LOS --- */
#define EN7570_SAFE_PROTECT		0x0100	/* Safe-circuit protection latch */
#define EN7570_LOS_CTRL1		0x011c	/* LOS control 1 (thresholds) */
#define EN7570_LOS_CTRL2		0x0120	/* LOS control 2 (confidence) */
#define EN7570_LOS_CAL_TIMER		0x0124	/* LOS calibration timer */
#define EN7570_LOS_CAL_TIMEOUT_CNT	0x0128	/* LOS calibration timeout counter */
#define EN7570_LOS_CAL_TIMEOUT		0x012c	/* LOS calibration timeout threshold */
#define EN7570_LOS_DBG_RG		0x0130	/* LOS debug register */

/* --- Block 0x0134-0x0170: Tx power loops and ADC probe --- */
#define EN7570_P0_PWR_CTRL_CS1		0x0134	/* P0 coarse setting #1 (cal only) */
#define EN7570_P0_PWR_CTRL_CS2		0x0138	/* P0 / Ibias coarse #2 (10-bit code) */
#define EN7570_P0_PWR_CTRL_CS3		0x013c	/* P0 ERC control */
#define EN7570_P0_PWR_CTRL_LCH		0x0140	/* P0 latch / readback */
#define EN7570_P1_PWR_CTRL_CS1		0x0144	/* P1 coarse setting #1 (cal only) */
#define EN7570_P1_PWR_CTRL_CS2		0x0148	/* P1 / Imod coarse #2 (10-bit code) */
#define EN7570_P1_PWR_CTRL_CS3		0x014c	/* P1 ERC control */
#define EN7570_P1_PWR_CTRL_LCH		0x0150	/* P1 latch / readback */
#define EN7570_ADC_PROBE_STATUS		0x0154	/* 16-bit ADC sample (little-endian) */
#define EN7570_PROBE_CONTROL		0x0158	/* ADC probe control (latch bit) */
#define EN7570_DUMMY			0x015c	/* Silicon-variant dummy register */
#define EN7570_OVFL_DBG_CLR		0x0160	/* Overflow debug clear */
#define EN7570_APD_OVP_LATCH		0x0164	/* APD over-voltage-protection latch */
#define EN7570_ROGUE_ONU_DET_CTRL	0x0168	/* Rogue-ONU detect control */
#define EN7570_ERC_FILTER_CTRL		0x016c	/* ERC digital-loop filter coefficients */
#define EN7570_FT_ADC_CLK_CLR		0x0170	/* Fast-traffic ADC clock clear / EN7570 ID */

/* --- Reset --- */
#define EN7570_SW_RESET			0x0300	/* Synchronous software reset */

/*
 * ADC channel select: SVADC_PD byte-0 field values OR-ed in under mask 0xe1.
 * The 0.875 V bandgap is the exception: it is selected in SVADC_PD byte 1
 * under mask 0xb3.
 */
#define EN7570_ADC_CH_TEMPERATURE	0x08	/* ADC_select_temperature */
#define EN7570_ADC_CH_VOLTAGE		0x00	/* channel field cleared */
#define EN7570_ADC_CH_RSSI		0x02	/* ADC_RSSI_enable */
#define EN7570_ADC_CH_MPD		0x04	/* ADC_TxPW_enable (LDD/MPD probe) */
#define EN7570_ADC_CH_BG1V76		0x06	/* 1.76 V bandgap (byte 0) */
#define EN7570_ADC_CH_BG0V875		0x4c	/* 0.875 V bandgap (byte 1, mask 0xb3) */

/* Selectors used only by en7570_info() to pick a current-readback register. */
#define EN7570_INFO_IBIAS		0x03
#define EN7570_INFO_IMOD		0x04
#define EN7570_INFO_P0			0x05	/* MPDL readback (reg 0x0004 bytes 2-3) */
#define EN7570_INFO_P1			0x06	/* MPDH readback (reg 0x0004 bytes 0-1) */

/* SVADC_PD bit fields (byte 0). */
#define EN7570_ADC_SELECT_MASK		0xe1	/* preserves non-channel bits */
#define EN7570_ADC_SELECT_SD		0x10	/* route SD signal to ADC */
#define EN7570_ADC_RSSI_ENABLE		0x02	/* bit 1 */
#define EN7570_ADC_TXPW_ENABLE		0x04	/* bit 2 */
#define EN7570_ADC_INMUX_MASK		0xe1
#define EN7570_ADC_BG0V875_MASK		0xb3	/* field mask when selecting 0.875 V */

/*
 * LOS ADC-revision enables, programmed in SVADC_PD bytes 2 (REV1, mask 0xbf,
 * set 0x40) and 3 (REV2, mask 0xfb, set 0x04).
 */

/* PROBE_CONTROL: ADC_latch lives in byte 0x01 bit 4. */
#define EN7570_ADC_LATCH_MASK		0xef
#define EN7570_ADC_LATCH		0x10

/* SVADC_REV3 enables (LOS calibration). */
#define EN7570_LOS_ADCREV1_MASK		0xbf
#define EN7570_LOS_ADCREV1_ENABLE	0x40
#define EN7570_LOS_ADCREV2_MASK		0xfb
#define EN7570_LOS_ADCREV2_ENABLE	0x04

/* TIAMUX. */
#define EN7570_TIA_MUX_MASK		0xf1	/* preserves gain bits when changing route */
#define EN7570_TIA_MUX_SELECT_SD	0x02	/* tia_mux_select for the SD path */
#define EN7570_TIA_MUX_DEFAULT		0x08	/* normal receive path */
#define EN7570_TIA_GAIN_SHIFT		6	/* flash TIA gain -> TIAMUX byte1 [7:6] */

/* TIASD threshold. */
#define EN7570_TIASD_MASK		0x7f
#define EN7570_TIASD_UPPER_MASK		0xfe	/* TIASD byte1: clears bit 0 */

/* LA_PWD: RSSI gain field [2:0] in byte 2, RSSI cal-enable bit 4. */
#define EN7570_RSSI_GAIN_MASK		0xf8
#define EN7570_RSSI_CAL_EN_MASK		0xef
#define EN7570_RSSI_CAL_EN		0x10
#define EN7570_LA_RX_HIGHZ_MASK		0xef	/* clears Rx high-Z (byte 0 bit 4) */
#define EN7570_LA_RX_HIGHZ		0x10

/* PI_TGEN ERC bits (byte 0). */
#define EN7570_ERC_ENABLE_MASK		0xf7	/* bit 3 */
#define EN7570_ERC_ENABLE		0x08
#define EN7570_ERC_START_MASK		0xfe	/* bit 0 */
#define EN7570_ERC_START		0x01
#define EN7570_ERC_OPEN_LOOP_MASK	0xfc	/* bits 1:0 */
#define EN7570_ERC_OPEN_LOOP		0x02

/* T1DELAY / TGEN. */
#define EN7570_TGEN_RESET_MASK		0xdf
#define EN7570_TGEN_RESET_T1T0		0x20
#define EN7570_TGEN_METHOD2_MASK	0xfe
#define EN7570_TGEN_METHOD2_ENABLE	0x01
#define EN7570_T1_T0_DELAY_GPON		0x9a
#define EN7570_T1_T0_DELAY_EPON		0x47
#define EN7570_T1_T0_SETTING1		0x66	/* initial sweep value */
#define EN7570_TIMER_RESET_VALUE	0x7f

/* SAFE_PROTECT: safe-circuit reset (byte 1 bit 6). */
#define EN7570_SAFE_CIRCUIT_MASK	0xbf
#define EN7570_SAFE_CIRCUIT_RESET	0x40

/* ROGUE_ONU_DET_CTRL: clear bit (byte 1 bit 0). */
#define EN7570_ROGUE_ONU_MASK		0xfe
#define EN7570_ROGUE_ONU_CLEAR		0x01

/* APD_DAC_CODE control bits. */
#define EN7570_APD_CONTROL_ENABLE	0x01	/* byte 1 bit 0 */
#define EN7570_APD_SOFTSTART_ENABLE	0x20	/* byte 2 bit 5 */

/* LOS_CTRL1 / LOS_CTRL2. */
#define EN7570_LOS_CAL_TRIG_MASK	0xfe
#define EN7570_LOS_CAL_TRIG		0x01
#define EN7570_LOS_THRESH_MASK		0x7f
#define EN7570_LOS_AIN_STABLE_MASK	0xe0
#define EN7570_LOS_AIN_STABLE_SET	0x1f
#define EN7570_LOS_CONFIDENCE_MASK	0xe0
#define EN7570_LOS_CONFIDENCE_SET	0x1f
#define EN7570_LOS_CNT_MASK		0x80
#define EN7570_LOS_CNT_SET		0x05
#define EN7570_LOS_COMP_THLD_H_DEF	0x30
#define EN7570_LOS_COMP_THLD_L_DEF	0x20

/* SW_RESET. */
#define EN7570_SW_RESET_TRIG		0x01	/* byte 0 bit 0 */

/* EN7570 silicon identification (read at FT_ADC_CLK_CLR byte 0). */
#define EN7570_EN7570_ID		0x03

/*
 * Flash (calibration NVM) byte offsets.  The blob is 100 little-endian
 * 32-bit words; index = offset >> 2.  A word equal to 0xffffffff is
 * "erased" and selects the corresponding default constant.
 */
#define EN7570_FLASH_WORDS		100
#define EN7570_FLASH_DEFAULT_WORDS	40	/* set_flash_register_default fills these */
#define EN7570_FLASH_ERASED		0xffffffff

#define EN7570_FL_IBIAS_INIT		0x000	/* initial laser bias (10-bit) */
#define EN7570_FL_IMOD_INIT		0x004	/* initial laser modulation (10-bit) */
#define EN7570_FL_P0_TARGET		0x008	/* P0 (high-power) MPD target (10-bit) */
#define EN7570_FL_P1_TARGET		0x00c	/* P1 (low-power) MPD target (10-bit) */
#define EN7570_FL_APD_SLOPE1		0x010	/* APD slope below knee (V/degC x100) */
#define EN7570_FL_APD_SLOPE2		0x014	/* APD slope above knee (V/degC x100) */
#define EN7570_FL_APD_CHANGE_POINT	0x018	/* APD knee voltage (V x100) */
#define EN7570_FL_T_APD			0x01c	/* APD update interval (seconds) */
#define EN7570_FL_LOS_HIGH_THLD		0x020	/* LOS high comparator threshold (7-bit) */
#define EN7570_FL_LOS_LOW_THLD		0x024	/* LOS low comparator threshold (7-bit) */
#define EN7570_FL_TIAGAIN		0x028	/* Rx TIA gain code (2-bit) */
#define EN7570_FL_INTERNAL_DDMI		0x02c	/* 0=off, 1=on, 2=fast */
#define EN7570_FL_APD_VOLTAGE_1		0x030	/* APD anchor voltages 0x00/0x40 (4-seg) */
#define EN7570_FL_APD_VOLTAGE_2		0x034	/* APD anchor voltages 0x80/0xc0 (4-seg) */
#define EN7570_FL_VOLTAGE_SLOPE		0x038	/* supply-voltage ADC slope correction */
#define EN7570_FL_VOLTAGE_OFFSET	0x03c	/* supply-voltage ADC offset correction */
#define EN7570_FL_TX_K1			0x040	/* Tx K-point #1 (tx_power<<16 | mpd_K) */
#define EN7570_FL_TX_K2			0x044	/* Tx K-point #2 */
#define EN7570_FL_RX_K1			0x050	/* Rx K-point #1 (rx_power<<16 | rssi_K) */
#define EN7570_FL_RX_K2			0x054	/* Rx K-point #2 */
#define EN7570_FL_RX_K3			0x058	/* Rx K-point #3 */
#define EN7570_FL_RX_K4			0x05c	/* Rx K-point #4 (low nibble = mode) */
#define EN7570_FL_RESERVED_ETC1		0x060	/* ETC norm_temp (x10) + cal ibias high */
#define EN7570_FL_ETC_HI_LO_DELTA	0x064	/* ETC P0/P1 compensation bytes */
#define EN7570_FL_RESERVED1		0x070	/* loop-mode flag */
#define EN7570_FL_IBIAS_SLOPE		0x074	/* SOL ibias slope (4 segments) */
#define EN7570_FL_P1_SLOPE		0x078	/* SOL modulation slope set */
#define EN7570_FL_ETC			0x07c	/* ETC mode (0..3) */
#define EN7570_FL_TEMP_K_SLOPE_OFFSET	0x080	/* temp slope(x10)<<16 | offset(x10) */
#define EN7570_FL_TEMPERATURE_OFFSET	0x084	/* (Env_off x10)<<16 | BOSA_off(degC) */
#define EN7570_FL_MPD_POINT_UP		0x088	/* MPD up-point reference (dual-slope) */
#define EN7570_FL_TEC			0x08c	/* TEC / BOSA threshold current (uA) */
#define EN7570_FL_MAGIC			0x094	/* PON-mode magic number */
#define EN7570_FL_T0T1_DELAY		0x098	/* calibrated TGEN T0/T1 burst delay */
#define EN7570_FL_T0CT1C		0x09c	/* TGEN T0C/T1C measured limits */
#define EN7570_FL_LUT_BASE		0x0a0	/* per-temperature LUT mirror start */
#define EN7570_FL_LUT_END		0x0dc	/* per-temperature LUT mirror end (16 pts) */

/* Loop-mode flag values (flash offset 0x070). */
#define EN7570_LOOP_SCL			0x80000000	/* single-closed-loop */
#define EN7570_LOOP_DOL			0xc0000000	/* dual-open-loop */

/* PON-mode magic numbers (flash offset 0x094). */
#define EN7570_MAGIC_GPON		0x07050700
#define EN7570_MAGIC_EPON		0xe7050700

/* xPON mode enumeration (struct en7570_priv .pon_mode). */
#define EN7570_PON_UNKNOWN		(-1)
#define EN7570_PON_EPON			0
#define EN7570_PON_GPON			1

/* ERC digital-loop filter taps (programmed once at init). */
#define EN7570_ERC_FILTER_B0		0xff
#define EN7570_ERC_FILTER_B1		0xa7
#define EN7570_ERC_FILTER_B2		0x58
#define EN7570_ERC_FILTER_B3		0x01

/*
 * Calibration scale constants for fixed-point use, in the units documented
 * next to each constant.
 */
#define EN7570_TEMP_OFFSET_X10_DEF	4958	/* ADC code at 0 degC, x10 (495.8) */
#define EN7570_TEMP_SLOPE_X10_DEF	3275	/* ADC codes per degC, x10 (327.5) */
#define EN7570_BOSA_TEMP_OFFSET_MC	5000	/* IC->BOSA temperature delta (5 degC) */

/* Two-point bandgap reference voltages in nanovolts. */
#define EN7570_BG_1V76_NV		1760000000LL
#define EN7570_BG_0V875_NV		875000000LL
#define EN7570_ADC_BANDGAP_MIN_DELTA	0x60	/* min code spread for a valid cal */
#define EN7570_DEFAULT_SLOPE_NV		5474000	/* default ADC slope, 0.005474 V/code */

#define EN7570_VOLTAGE_8472_NV		100000	/* SFF-8472 supply-voltage LSB = 100 uV */

/* Current scaling: register code -> microamps. */
#define EN7570_BIAS_UA_PER_CODE_X100	2442	/* 0.02442 mA/code = 24.42 uA, x100 */
#define EN7570_MOD_UA_PER_CODE_X100	2198	/* 0.02198 mA/code = 21.98 uA, x100 */
#define EN7570_BIAS_8472_UA_PER_LSB	2	/* DDMI bias word LSB = 2 uA */

/* APD dual-slope defaults. */
#define EN7570_APD_KNEE_MV_DEF		35000	/* knee at 35.0 V */
#define EN7570_APD_SLOPE_UP_UV_DEF	100000	/* 0.10 V/degC above knee, in uV/degC */
#define EN7570_APD_SLOPE_DN_UV_DEF	70000	/* 0.07 V/degC below knee, in uV/degC */
#define EN7570_APD_KNEE_TEMP_MC		25000	/* knee temperature 25 degC */
#define EN7570_APD_ZERO_CODE_MV_DEF	30000	/* legacy: 30.0 V at DAC code 0 */
#define EN7570_APD_STEP_UV_DEF		93750	/* legacy: 0.09375 V/code, in uV */
#define EN7570_T_APD_DEFAULT		600	/* APD update interval (s) */
#define EN7570_T_APD_MIN		10

/* RSSI gain table (LA_PWD byte2 [2:0] -> multiplicative factor). */
#define EN7570_RSSI_GAIN_DEFAULT	5	/* largest gain (factor 256) */
#define EN7570_RSSI_DEFEND_NOISE_THRESH	0x32	/* margin below Vref that breaks sweep */

/* BOSA threshold current default for TEC eye correction (uA). */
#define EN7570_BOSA_LTH_UA_DEF		3000

/* Alarm thresholds (DDMI word units). */
#define EN7570_TX_PWR_LOW_THLD		0x2710	/* 0.0 dBm   (0.1 uW units) */
#define EN7570_TX_PWR_HIGH_THLD		0x8a99	/* +5.5 dBm */
#define EN7570_TX_BIAS_LOW_THLD		0x01f4	/* 1 mA      (2 uA units) */
#define EN7570_TX_BIAS_HIGH_THLD	0xc350	/* 100 mA */
#define EN7570_RX_PWR_LOW_THLD		0x000a	/* -30 dBm   (0.1 uW units) */
#define EN7570_RX_PWR_HIGH_THLD		0x09cf	/* -6 dBm */
#define EN7570_VOLT_LOW_THLD		0x7148	/* 2.9 V     (100 uV units) */
#define EN7570_VOLT_HIGH_THLD		0x9088	/* 3.7 V */
#define EN7570_TEMP_LOW_THLD		0xfb00	/* -5 degC   (1/256 degC) */
#define EN7570_TEMP_HIGH_THLD		0x5500	/* +85 degC */

#endif /* _EN7570_REGS_H */
