// SPDX-License-Identifier: GPL-2.0
/*
 * EN7572 / AN8901 SFF-8472 diagnostics readback and alarm evaluation.
 *
 * The MD32 firmware maintains the SFF-8472 diagnostic block on the A2 page
 * (big-endian words at 0x60-0x6B) already in MSA units, so temperature,
 * voltage and current are read straight through.  Optical power is reported by
 * the firmware in 0.1 uW; the dBm view used by the diagnostics dump is the one
 * conversion still computed on the host, via an integer log10.
 *
 * Locking: callers hold lddla->lock.
 */
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>

#include "en7572.h"

/* Read a big-endian SFF-8472 diagnostic word from the A2 page. */
static u16 en7572_ddmi_word(struct en7572_priv *priv, u16 reg)
{
	u8 b[2] = { 0 };

	en7572_rd(priv, EN7572_DEV_A2, reg, b, 2);
	return (b[0] << 8) | b[1];
}

/* ------------------------------------------------------------------ */
/* Integer log10 (for the dBm diagnostics view)			      */
/* ------------------------------------------------------------------ */

static const u16 en7572_logtable[256] = {
	0x0000, 0x0171, 0x02e0, 0x044e, 0x05ba, 0x0725, 0x088e, 0x09f7,
	0x0b5d, 0x0cc3, 0x0e27, 0x0f8a, 0x10eb, 0x124b, 0x13aa, 0x1508,
	0x1664, 0x17bf, 0x1919, 0x1a71, 0x1bc8, 0x1d1e, 0x1e73, 0x1fc6,
	0x2119, 0x226a, 0x23ba, 0x2508, 0x2656, 0x27a2, 0x28ed, 0x2a37,
	0x2b80, 0x2cc8, 0x2e0f, 0x2f54, 0x3098, 0x31dc, 0x331e, 0x345f,
	0x359f, 0x36de, 0x381b, 0x3958, 0x3a94, 0x3bce, 0x3d08, 0x3e41,
	0x3f78, 0x40af, 0x41e4, 0x4319, 0x444c, 0x457f, 0x46b0, 0x47e1,
	0x4910, 0x4a3f, 0x4b6c, 0x4c99, 0x4dc5, 0x4eef, 0x5019, 0x5142,
	0x526a, 0x5391, 0x54b7, 0x55dc, 0x5700, 0x5824, 0x5946, 0x5a68,
	0x5b89, 0x5ca8, 0x5dc7, 0x5ee5, 0x6003, 0x611f, 0x623a, 0x6355,
	0x646f, 0x6588, 0x66a0, 0x67b7, 0x68ce, 0x69e4, 0x6af8, 0x6c0c,
	0x6d20, 0x6e32, 0x6f44, 0x7055, 0x7165, 0x7274, 0x7383, 0x7490,
	0x759d, 0x76aa, 0x77b5, 0x78c0, 0x79ca, 0x7ad3, 0x7bdb, 0x7ce3,
	0x7dea, 0x7ef0, 0x7ff6, 0x80fb, 0x81ff, 0x8302, 0x8405, 0x8507,
	0x8608, 0x8709, 0x8809, 0x8908, 0x8a06, 0x8b04, 0x8c01, 0x8cfe,
	0x8dfa, 0x8ef5, 0x8fef, 0x90e9, 0x91e2, 0x92db, 0x93d2, 0x94ca,
	0x95c0, 0x96b6, 0x97ab, 0x98a0, 0x9994, 0x9a87, 0x9b7a, 0x9c6c,
	0x9d5e, 0x9e4f, 0x9f3f, 0xa02e, 0xa11e, 0xa20c, 0xa2fa, 0xa3e7,
	0xa4d4, 0xa5c0, 0xa6ab, 0xa796, 0xa881, 0xa96a, 0xaa53, 0xab3c,
	0xac24, 0xad0c, 0xadf2, 0xaed9, 0xafbe, 0xb0a4, 0xb188, 0xb26c,
	0xb350, 0xb433, 0xb515, 0xb5f7, 0xb6d9, 0xb7ba, 0xb89a, 0xb97a,
	0xba59, 0xbb38, 0xbc16, 0xbcf4, 0xbdd1, 0xbead, 0xbf8a, 0xc065,
	0xc140, 0xc21b, 0xc2f5, 0xc3cf, 0xc4a8, 0xc580, 0xc658, 0xc730,
	0xc807, 0xc8de, 0xc9b4, 0xca8a, 0xcb5f, 0xcc34, 0xcd08, 0xcddc,
	0xceaf, 0xcf82, 0xd054, 0xd126, 0xd1f7, 0xd2c8, 0xd399, 0xd469,
	0xd538, 0xd607, 0xd6d6, 0xd7a4, 0xd872, 0xd93f, 0xda0c, 0xdad9,
	0xdba5, 0xdc70, 0xdd3b, 0xde06, 0xded0, 0xdf9a, 0xe063, 0xe12c,
	0xe1f5, 0xe2bd, 0xe385, 0xe44c, 0xe513, 0xe5d9, 0xe69f, 0xe765,
	0xe82a, 0xe8ef, 0xe9b3, 0xea77, 0xeb3b, 0xebfe, 0xecc1, 0xed83,
	0xee45, 0xef06, 0xefc8, 0xf088, 0xf149, 0xf209, 0xf2c8, 0xf387,
	0xf446, 0xf505, 0xf5c3, 0xf680, 0xf73e, 0xf7fb, 0xf8b7, 0xf973,
	0xfa2f, 0xfaea, 0xfba5, 0xfc60, 0xfd1a, 0xfdd4, 0xfe8e, 0xff47,
};

/* Returns log2(value) * 2^24 (0 for value 0, which is undefined). */
static u32 en7572_intlog2(u32 value)
{
	u32 msb, logentry, significand, interpolation;

	if (value == 0)
		return 0;

	msb = fls(value) - 1;
	significand = value << (31 - msb);
	logentry = (significand >> 23) & 0xff;
	interpolation = ((significand & 0x7fffff) *
			 ((en7572_logtable[(logentry + 1) & 0xff] -
			   en7572_logtable[logentry]) & 0xffff)) >> 15;

	return (msb << 24) + (en7572_logtable[logentry] << 8) + interpolation;
}

/* Returns log10(value) * 2^24. */
u32 en7572_intlog10(u32 value)
{
	u64 log;

	if (value == 0)
		return 0;
	log = en7572_intlog2(value);
	return (log * 646456993) >> 31;	/* log10(2) * 2^31 */
}

/**
 * en7572_power_dbm_x100() - convert an SFF-8472 optical-power word to 0.01 dBm.
 * @word_0p1uw: optical power in 0.1 uW units (SFF-8472)
 *
 * Return: power in hundredths of a dBm (e.g. 123 == 1.23 dBm).
 */
s32 en7572_power_dbm_x100(u16 word_0p1uw)
{
	s32 t = en7572_intlog10(word_0p1uw) >> 14;

	t -= (4 << 10);		/* reference 0.1 uW units to mW */
	return (t * 1000) >> 10;
}

/* ------------------------------------------------------------------ */
/* SFF-8472 refreshers (cache the MSA words for hwmon / virtual SFP)   */
/* ------------------------------------------------------------------ */

/* Refresh the temperature word; return the IC temperature in m degC. */
s32 en7572_temp_refresh(struct airoha_lddla *lddla)
{
	struct en7572_priv *priv = container_of(lddla, struct en7572_priv, lddla);

	lddla->ddmi_temperature = en7572_ddmi_word(priv, EN7572_A2_DDMI_TEMP);
	return (s16)lddla->ddmi_temperature * 1000 / 256;
}

u16 en7572_vcc_refresh(struct airoha_lddla *lddla)
{
	struct en7572_priv *priv = container_of(lddla, struct en7572_priv, lddla);

	lddla->ddmi_voltage = en7572_ddmi_word(priv, EN7572_A2_DDMI_VCC);
	return lddla->ddmi_voltage;
}

u16 en7572_bias_refresh(struct airoha_lddla *lddla)
{
	struct en7572_priv *priv = container_of(lddla, struct en7572_priv, lddla);

	lddla->ddmi_current = en7572_ddmi_word(priv, EN7572_A2_DDMI_BIAS);
	return lddla->ddmi_current;
}

u16 en7572_tx_power_refresh(struct airoha_lddla *lddla)
{
	struct en7572_priv *priv = container_of(lddla, struct en7572_priv, lddla);

	lddla->ddmi_tx_power = en7572_ddmi_word(priv, EN7572_A2_DDMI_TX_POWER);
	return lddla->ddmi_tx_power;
}

u16 en7572_rx_power_refresh(struct airoha_lddla *lddla)
{
	struct en7572_priv *priv = container_of(lddla, struct en7572_priv, lddla);

	lddla->ddmi_rx_power = en7572_ddmi_word(priv, EN7572_A2_DDMI_RX_POWER);
	return lddla->ddmi_rx_power;
}

/* Re-evaluate the host-visible alarm bitmap from the cached DDMI words. */
void en7572_alarms(struct en7572_priv *priv)
{
	struct airoha_lddla *lddla = &priv->lddla;
	u32 a = 0;

	if (lddla->ddmi_tx_power < AIROHA_TX_PWR_LOW_THLD)
		a |= AIROHA_ALARM_TX_LOW_POWER;
	if (lddla->ddmi_tx_power > AIROHA_TX_PWR_HIGH_THLD)
		a |= AIROHA_ALARM_TX_HIGH_POWER;
	if (lddla->ddmi_current < AIROHA_TX_BIAS_LOW_THLD)
		a |= AIROHA_ALARM_TX_LOW_BIAS;
	if (lddla->ddmi_current > AIROHA_TX_BIAS_HIGH_THLD)
		a |= AIROHA_ALARM_TX_HIGH_BIAS;
	if (lddla->ddmi_rx_power < AIROHA_RX_PWR_LOW_THLD)
		a |= AIROHA_ALARM_RX_LOW_POWER;
	if (lddla->ddmi_rx_power > AIROHA_RX_PWR_HIGH_THLD)
		a |= AIROHA_ALARM_RX_HIGH_POWER;
	if (lddla->ddmi_voltage < AIROHA_VOLT_LOW_THLD)
		a |= AIROHA_ALARM_LOW_VOLT;
	if (lddla->ddmi_voltage > AIROHA_VOLT_HIGH_THLD)
		a |= AIROHA_ALARM_HIGH_VOLT;
	if ((s16)lddla->ddmi_temperature < (s16)AIROHA_TEMP_LOW_THLD)
		a |= AIROHA_ALARM_LOW_TEMP;
	if ((s16)lddla->ddmi_temperature > (s16)AIROHA_TEMP_HIGH_THLD)
		a |= AIROHA_ALARM_HIGH_TEMP;

	lddla->alarm = a;
}

/* ------------------------------------------------------------------ */
/* Debugfs diagnostics dump					      */
/* ------------------------------------------------------------------ */

void en7572_diag_show(struct airoha_lddla *lddla, struct seq_file *s)
{
	struct en7572_priv *priv = container_of(lddla, struct en7572_priv, lddla);
	s32 tx = en7572_power_dbm_x100(lddla->ddmi_tx_power);
	s32 rx = en7572_power_dbm_x100(lddla->ddmi_rx_power);

	seq_printf(s, "fw ver:      %u\n", en7572_a2_byte(priv, EN7572_A2_FW_VER));
	seq_printf(s, "mcu ready:   %d\n", priv->mcu_ready);
	seq_printf(s, "los:         %u\n", en7572_a2_byte(priv, EN7572_A2_LOS_STA));
	seq_printf(s, "ben:         %u\n", en7572_bit_rd(priv, EN7572_RG_BEN_STATUS, 0, 0));
	seq_printf(s, "bosa temp:   %d degC\n",
		   (s8)en7572_a2_byte(priv, EN7572_A2_BOSA_TEMP));
	seq_printf(s, "vapd:        %u.%u V\n",
		   en7572_a2_word(priv, EN7572_A2_VAPD) >> 3,
		   (en7572_a2_word(priv, EN7572_A2_VAPD) * 10 / 8) % 10);
	seq_printf(s, "rssi cur:    %u uA\n",
		   en7572_a2_word(priv, EN7572_A2_RSSI_CURRENT) >> 5);
	seq_printf(s, "tx power:    %d.%02d dBm\n", tx / 100, abs(tx % 100));
	seq_printf(s, "rx power:    %d.%02d dBm\n", rx / 100, abs(rx % 100));
}
