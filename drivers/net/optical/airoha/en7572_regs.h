/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register / mailbox map for the Airoha EN7572 / AN8901 xPON LDDLA controller.
 *
 * Unlike the EN7570/EN7571 (whose host directly drives the analog ADC and
 * power-control registers), the EN7572/AN8901 carries an embedded MD32
 * microcontroller.  The host loads the MD32 firmware (program + data memory)
 * and a calibration (BOB) table over I2C, releases the MCU, and from then on
 * the firmware runs all of the analog loops itself.  The host only:
 *
 *   - loads firmware/BOB into the MD32 (device 0x50, the "A0" page),
 *   - reads the SFF-8472 digital diagnostics and a firmware mailbox from the
 *     "A2" page (device 0x51),
 *   - pokes a handful of hardware CSRs (also on device 0x51) for the runtime
 *     control loops.
 *
 * Every access is an I2C transaction carrying a 16-bit big-endian register
 * pointer; mailbox/CSR data is little-endian, while the SFF-8472 diagnostic
 * block is big-endian (per the MSA).
 */
#ifndef _EN7572_REGS_H
#define _EN7572_REGS_H

/* I2C device addresses (7-bit). */
#define EN7572_DEV_A0			0x50	/* MD32 memory + SFF-8472 serial ID */
#define EN7572_DEV_A2			0x51	/* mailbox, CSRs, SFF-8472 diagnostics */

/* MD32 program/data-memory load ports (CSR space, device 0x51). */
#define EN7572_MD32_PM_CFG		0x3000
#define EN7572_MD32_PM_ADDR		0x3004
#define EN7572_MD32_PM_DATA		0x3008	/* data port lives on device 0x50 */
#define EN7572_MD32_DM_CFG		0x300c
#define EN7572_MD32_DM_ADDR		0x3010
#define EN7572_MD32_DM_DATA		0x3014	/* data port lives on device 0x50 */
#define EN7572_MD32_EN_CFG		0x3018

#define EN7572_MD32_BOB_DM_OFFSET	0x600	/* BOB table base inside MD32 DM */

/* Firmware image sizes (bytes). */
#define EN7572_PM_SIZE			(16 << 10)	/* program memory: 16 KiB */
#define EN7572_DM_SIZE			(4 << 10)	/* data memory: 4 KiB */
#define EN7572_BOB_SIZE			512		/* calibration table */

/* Hardware CSRs used by reset and the runtime control loops (device 0x51). */
#define EN7572_RG_TX_CROSSING		0x00000100	/* BEN gate (bits 2-3) */
#define EN7572_RG_TXOUTPK_LOAD		0x00000108
#define EN7572_RG_APC_ANA_VMON_SEL	0x00000124	/* APC DAC (bits 8-15) */
#define EN7572_RG_IMPD_SINK		0x00000128	/* ERC CDAC/DAC */
#define EN7572_RG_RESERVE_TIA		0x00000130	/* TIA cur/gain/bw */
#define EN7572_RG_REP_PH_CAP_SEL	0x0000013c	/* PGA gain/cap */
#define EN7572_RG_APD_DAC_CODE		0x0000015c	/* APD enable (bit 8) */
#define EN7572_RG_OCP_CTRL		0x00000160	/* OCP enable (bit 30) */
#define EN7572_RG_SYS_RESET		0x00000200	/* core reset (bits 30-31) */
#define EN7572_RG_LOOP_EN		0x00000208	/* rg_loop_en (bit 0) */
#define EN7572_DCL_CTRL_2		0x00000210	/* forced Iav/Imod */
#define EN7572_RG_IMOD_MAX		0x00000248	/* Imod max tune (bits 0-11) */
#define EN7572_RG_IAV_LIMIT		0x00000214	/* Iav limit (bits 16-28) */
#define EN7572_CSR_IAV			0x000003c4	/* live Iav code (bits 0-12) */
#define EN7572_CSR_IBIAS_IMOD		0x000003c8	/* live Imod code (bits 16-27) */
#define EN7572_RG_TX_DIS		0x000003e0	/* TX_DIS / status */
#define EN7572_RG_OCP_STATUS		0x000003e4	/* OCP detected (bit 8) */
#define EN7572_CSR_CHIP_ID		0x00000408	/* identity word == 0x1388 */
#define EN7572_RG_LOS_DAC		0x0000043c	/* LOS/SD DAC */
#define EN7572_RG_BEN_STATUS		0x00000488	/* burst-enable (bit 0) */

#define EN7572_CHIP_ID			0x1388		/* WordReadA2(0x408) */

/* A2 firmware mailbox (device 0x51, byte offsets 0x80-0xFF). */
#define EN7572_A2_FW_VER		0x82
#define EN7572_A2_MCU_IDLE		0x83
#define EN7572_A2_APC_CAL		0x8a
#define EN7572_A2_GPL			0x9c	/* Pav step: 0x11 up, 0x1f down */
#define EN7572_A2_SLOPE_DN		0xa0
#define EN7572_A2_SLOPE_UP		0xa1
#define EN7572_A2_VAPD_25		0xa2
#define EN7572_A2_BOSA_TEMP_OFFSET	0xac
#define EN7572_A2_VAPD			0xae	/* current APD voltage */
#define EN7572_A2_BOSA_TEMP		0xcd	/* signed degC */
#define EN7572_A2_IAV_MAX		0xe4
#define EN7572_A2_IMOD_MAX		0xe5
#define EN7572_A2_SLOPE_TIME		0xe6	/* loop cadence (s) */
#define EN7572_A2_APC_RED_LMT		0xe7	/* high-temp / APC reduction limit */
#define EN7572_A2_CUSTOM_FUNC		0xe8	/* bit0 reduce-Imod, bit1 adaptive-Pav */
#define EN7572_A2_RSSI_CURRENT		0xf6	/* 1/32 uA */
#define EN7572_A2_LOS_STA		0xfa

/* SFF-8472 diagnostics block on the A2 page (big-endian words). */
#define EN7572_A2_DDMI_TEMP		96	/* 0x60: 1/256 degC */
#define EN7572_A2_DDMI_VCC		98	/* 0x62: 100 uV */
#define EN7572_A2_DDMI_BIAS		100	/* 0x64: 2 uA */
#define EN7572_A2_DDMI_TX_POWER		102	/* 0x66: 0.1 uW */
#define EN7572_A2_DDMI_RX_POWER		104	/* 0x68: 0.1 uW */
#define EN7572_A2_DDMI_IMOD		106	/* 0x6a: 2 uA */

#define EN7572_A2_ALARM_FLAGS		112	/* 0x70: SFF-8472 alarm flags */
#define EN7572_A2_WARN_FLAGS		116	/* 0x74: SFF-8472 warning flags */

/* BOB calibration-table field offsets (within the per-eye A0/A2 halves). */
#define EN7572_BOB_IBIAS_CAL		0x84
#define EN7572_BOB_IMOD_CAL		0x86
#define EN7572_BOB_IAV_CAL		0x88
#define EN7572_BOB_APC_CAL		0x8a
#define EN7572_BOB_TIA_CUR		0x8c
#define EN7572_BOB_ERC_CDAC		0x8d
#define EN7572_BOB_ERC_DAC		0x8e
#define EN7572_BOB_TIA_GAIN		0x90
#define EN7572_BOB_TIA_BW		0x91
#define EN7572_BOB_PGA_GAIN		0x92
#define EN7572_BOB_PGA_CAP		0x93
#define EN7572_BOB_TSSI_CAL_1		0xb4	/* 2nd-eye Tx DDMI cal block */

#define EN7572_BOB_A0(off)		((off))		/* eye-1 (A0) half */
#define EN7572_BOB_A2(off)		((off) + 256)	/* eye-0 (A2) half */

/* Chip variants (driver_data / of_device_id data). */
#define EN7572_VARIANT_EN7572		0
#define EN7572_VARIANT_AN8901		1

#endif /* _EN7572_REGS_H */
