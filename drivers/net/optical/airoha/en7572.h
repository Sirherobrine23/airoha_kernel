/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Airoha EN7572 / AN8901 xPON LDDLA controller driver.
 *
 * Per-device state and internal API.  The shared transport-agnostic
 * scaffolding (hwmon, optical frontend, debugfs, module entry) lives in the
 * airoha_lddla core; this holds the EN7572-specific MD32 firmware loader, the
 * A0/A2 I2C transport, the SFF-8472 readback and the runtime control loops.
 *
 * The MD32 firmware presents the digital diagnostics already in SFF-8472 word
 * units, so the readback here is a direct mailbox read rather than the
 * fixed-point ADC arithmetic the EN7570/EN7571 perform.  Optical power is the
 * one conversion still done on the host (log10 for the hwmon/diag dBm view).
 */
#ifndef _EN7572_H
#define _EN7572_H

#include <linux/types.h>
#include <linux/workqueue.h>

#include "airoha_lddla.h"
#include "en7572_regs.h"

/* Driver version reported in diagnostics. */
#define EN7572_VERSION			1

/*
 * Per-device state for one EN7572 / AN8901 LDDLA controller.  The shared
 * airoha_lddla object (hwmon / SFP / debugfs) must be the first member.  The
 * A0 (0x50) and A2 (0x51) pages are both reached over the embedded client's
 * I2C adapter using explicit slave addresses.
 */
struct en7572_priv {
	struct airoha_lddla lddla;

	/* 1 Hz periodic worker driving the DDMI refresh and control loops. */
	struct delayed_work tick_work;

	int variant;			/* EN7572_VARIANT_* */
	const char *fw_pm;		/* MD32 program-memory image */
	const char *fw_dm;		/* MD32 data-memory image */

	bool mcu_ready;			/* firmware loaded and MD32 released */

	/* reduce-Imod high-temperature loop state (en7572_loop.c). */
	bool ri_assert;
	bool ri_first_boot;
	s16 ri_temp_org;
	u16 ri_imod_max_new;
	u8 ri_cnt1;
	u8 ri_cnt2;
	u32 ri_wait_s;			/* seconds until the next evaluation */

	/* adaptive-Pav loop state (en7572_loop.c). */
	bool ap_assert;
	u8 ap_inc_cnt;
	u32 ap_wait_s;

	u32 cnt;			/* free-running 1 Hz tick counter */
};

/* --- Transport (en7572_main.c) --- */
int en7572_rd(struct en7572_priv *priv, u8 dev, u16 reg, u8 *buf, int len);
int en7572_wr(struct en7572_priv *priv, u8 dev, u16 reg, const u8 *buf, int len);
u8 en7572_byte_rd(struct en7572_priv *priv, u8 dev, u16 reg);
u16 en7572_word_rd(struct en7572_priv *priv, u8 dev, u16 reg);
void en7572_byte_wr(struct en7572_priv *priv, u8 dev, u16 reg, u8 val);
void en7572_word_wr(struct en7572_priv *priv, u8 dev, u16 reg, u16 val);
u32 en7572_bit_rd(struct en7572_priv *priv, u16 reg, int start, int end);
void en7572_bit_wr(struct en7572_priv *priv, u16 reg, int start, int end, u32 val);

/* A2-page (mailbox/CSR) convenience wrappers. */
static inline u8 en7572_a2_byte(struct en7572_priv *priv, u16 reg)
{
	return en7572_byte_rd(priv, EN7572_DEV_A2, reg);
}

static inline u16 en7572_a2_word(struct en7572_priv *priv, u16 reg)
{
	return en7572_word_rd(priv, EN7572_DEV_A2, reg);
}

/* --- Init / reset / detect (en7572_main.c) --- */
int en7572_detect(struct en7572_priv *priv);
int en7572_init(struct en7572_priv *priv);
void en7572_tick(struct en7572_priv *priv);

/* --- DDMI / diagnostics (en7572_ddmi.c) --- */
u32 en7572_intlog10(u32 value);
s32 en7572_power_dbm_x100(u16 word_0p1uw);
s32 en7572_temp_refresh(struct airoha_lddla *lddla);
u16 en7572_vcc_refresh(struct airoha_lddla *lddla);
u16 en7572_bias_refresh(struct airoha_lddla *lddla);
u16 en7572_tx_power_refresh(struct airoha_lddla *lddla);
u16 en7572_rx_power_refresh(struct airoha_lddla *lddla);
void en7572_alarms(struct en7572_priv *priv);
void en7572_diag_show(struct airoha_lddla *lddla, struct seq_file *s);

/* --- Runtime control loops (en7572_loop.c) --- */
bool en7572_adaptive_pon(struct en7572_priv *priv, int mode);
void en7572_reduce_imod(struct en7572_priv *priv);
void en7572_adaptive_pav(struct en7572_priv *priv);

#endif /* _EN7572_H */
