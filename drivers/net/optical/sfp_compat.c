// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtual SFF-8472 compatibility bus for soldered optical frontends.
 *
 * This is deliberately an optional compatibility surface. Consumers should
 * bind to struct optical_frontend directly; the virtual SFP pages exist for
 * existing SFP/phylink and userspace tooling which expects A0/A2 EEPROMs.
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/optical_frontend.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define OPTICAL_SFP_ROM_ADDR	0x50
#define OPTICAL_SFP_DDM_ADDR	0x51
#define OPTICAL_SFP_PAGE_SIZE	256

struct optical_sfp {
	struct optical_frontend *frontend;
	struct i2c_adapter adapter;
	u8 rom[OPTICAL_SFP_PAGE_SIZE];
	u8 ddm[OPTICAL_SFP_PAGE_SIZE];
	u8 rom_ptr;
	u8 ddm_ptr;
	bool registered;
};

static void optical_sfp_put_ascii(u8 *buf, unsigned int off, const char *str,
				  unsigned int len)
{
	unsigned int n = str ? strnlen(str, len) : 0;

	memset(buf + off, ' ', len);
	if (n)
		memcpy(buf + off, str, n);
}

static void optical_sfp_put_be16(u8 *buf, unsigned int off, u16 value)
{
	put_unaligned_be16(value, buf + off);
}

static u8 optical_sfp_checksum(const u8 *buf, unsigned int first,
			       unsigned int last)
{
	u8 sum = 0;
	unsigned int i;

	for (i = first; i <= last; i++)
		sum += buf[i];
	return -sum;
}

static void optical_sfp_build_rom(struct optical_sfp *sfp)
{
	const struct optical_frontend_desc *desc =
		optical_frontend_get_desc(sfp->frontend);
	struct optical_frontend_mode mode = {};
	bool gpon;
	u8 *r = sfp->rom;

	gpon = !optical_frontend_get_mode(sfp->frontend, &mode) &&
	       mode.protocol == OPTICAL_FRONTEND_PROTO_GPON;
	if (mode.protocol == OPTICAL_FRONTEND_PROTO_UNSPEC)
		gpon = desc->protocols & BIT(OPTICAL_FRONTEND_PROTO_GPON);

	memset(r, 0, OPTICAL_SFP_PAGE_SIZE);
	r[0] = 0x03;
	r[1] = 0x04;
	r[2] = 0x01;
	r[11] = 0x03;
	r[12] = gpon ? 0x19 : 0x0d;
	optical_sfp_put_be16(r, 14, 1490);
	optical_sfp_put_ascii(r, 20, desc->vendor_name ?: "Unknown", 16);
	r[37] = desc->vendor_oui[0];
	r[38] = desc->vendor_oui[1];
	r[39] = desc->vendor_oui[2];
	optical_sfp_put_ascii(r, 40, desc->part_number ?: desc->name, 16);
	optical_sfp_put_ascii(r, 56, "0001", 4);
	optical_sfp_put_be16(r, 60, 1490);
	r[63] = optical_sfp_checksum(r, 0, 62);
	r[65] = 0x1a;
	optical_sfp_put_ascii(r, 68, desc->serial ?: "UNKNOWN", 16);
	optical_sfp_put_ascii(r, 84, desc->date_code ?: "000000", 8);
	r[92] = 0x68;
	r[93] = 0xf0;
	r[94] = 0x06;
	r[95] = optical_sfp_checksum(r, 64, 94);
}

static u16 optical_sfp_temp_word(s32 temp_mc)
{
	return (u16)(s16)DIV_ROUND_CLOSEST((s64)temp_mc * 256, 1000);
}

static u16 optical_sfp_u16_scaled(u32 value, u32 divisor)
{
	return min_t(u32, DIV_ROUND_CLOSEST(value, divisor), U16_MAX);
}

static void optical_sfp_build_thresholds(struct optical_sfp *sfp)
{
	const struct optical_frontend_desc *desc =
		optical_frontend_get_desc(sfp->frontend);
	const struct optical_frontend_thresholds *t = desc->thresholds;
	u8 *d = sfp->ddm;

	memset(d, 0, OPTICAL_SFP_PAGE_SIZE);
	if (t && (t->valid & OPTICAL_FRONTEND_THRESHOLD_F_TEMPERATURE)) {
		optical_sfp_put_be16(d, 0, optical_sfp_temp_word(t->temperature_high_mc));
		optical_sfp_put_be16(d, 2, optical_sfp_temp_word(t->temperature_low_mc));
		optical_sfp_put_be16(d, 4, optical_sfp_temp_word(t->temperature_high_mc));
		optical_sfp_put_be16(d, 6, optical_sfp_temp_word(t->temperature_low_mc));
	}
	if (t && (t->valid & OPTICAL_FRONTEND_THRESHOLD_F_VOLTAGE)) {
		u16 high = optical_sfp_u16_scaled(t->voltage_high_uv, 100);
		u16 low = optical_sfp_u16_scaled(t->voltage_low_uv, 100);

		optical_sfp_put_be16(d, 8, high);
		optical_sfp_put_be16(d, 10, low);
		optical_sfp_put_be16(d, 12, high);
		optical_sfp_put_be16(d, 14, low);
	}
	if (t && (t->valid & OPTICAL_FRONTEND_THRESHOLD_F_BIAS)) {
		u16 high = optical_sfp_u16_scaled(t->bias_high_ua, 2);
		u16 low = optical_sfp_u16_scaled(t->bias_low_ua, 2);

		optical_sfp_put_be16(d, 16, high);
		optical_sfp_put_be16(d, 18, low);
		optical_sfp_put_be16(d, 20, high);
		optical_sfp_put_be16(d, 22, low);
	}
	if (t && (t->valid & OPTICAL_FRONTEND_THRESHOLD_F_TX_POWER)) {
		u16 high = optical_sfp_u16_scaled(t->tx_power_high_nw, 100);
		u16 low = optical_sfp_u16_scaled(t->tx_power_low_nw, 100);

		optical_sfp_put_be16(d, 24, high);
		optical_sfp_put_be16(d, 26, low);
		optical_sfp_put_be16(d, 28, high);
		optical_sfp_put_be16(d, 30, low);
	}
	if (t && (t->valid & OPTICAL_FRONTEND_THRESHOLD_F_RX_POWER)) {
		u16 high = optical_sfp_u16_scaled(t->rx_power_high_nw, 100);
		u16 low = optical_sfp_u16_scaled(t->rx_power_low_nw, 100);

		optical_sfp_put_be16(d, 32, high);
		optical_sfp_put_be16(d, 34, low);
		optical_sfp_put_be16(d, 36, high);
		optical_sfp_put_be16(d, 38, low);
	}

	/* Linear calibration coefficients, matching SFF-8472 defaults. */
	optical_sfp_put_be16(d, 56, 1);
	optical_sfp_put_be16(d, 80, 1);
	optical_sfp_put_be16(d, 84, 1);
	optical_sfp_put_be16(d, 88, 1);
	optical_sfp_put_be16(d, 90, 1);
	d[95] = optical_sfp_checksum(d, 0, 94);
}

static void optical_sfp_refresh_ddm(struct optical_sfp *sfp)
{
	struct optical_frontend_telemetry t = {};
	u8 *d = sfp->ddm;
	u32 alarm = 0;

	if (optical_frontend_get_telemetry(sfp->frontend, &t))
		return;

	if (t.valid & OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE)
		optical_sfp_put_be16(d, 96, optical_sfp_temp_word(t.temperature_mc));
	if (t.valid & OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE)
		optical_sfp_put_be16(d, 98, min_t(u32, t.voltage_uv / 100, U16_MAX));
	if (t.valid & OPTICAL_FRONTEND_TELEMETRY_F_BIAS)
		optical_sfp_put_be16(d, 100, min_t(u32, t.bias_ua / 2, U16_MAX));
	if (t.valid & OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER)
		optical_sfp_put_be16(d, 102, min_t(u32, t.tx_power_nw / 100, U16_MAX));
	if (t.valid & OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER)
		optical_sfp_put_be16(d, 104, min_t(u32, t.rx_power_nw / 100, U16_MAX));
	if (t.valid & OPTICAL_FRONTEND_TELEMETRY_F_ALARMS)
		alarm = t.alarms;

	d[110] = 0;
	d[112] = (alarm & OPTICAL_FRONTEND_ALARM_HIGH_TEMP ? 0x80 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_LOW_TEMP ? 0x40 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE ? 0x20 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE ? 0x10 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS ? 0x08 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS ? 0x04 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER ? 0x02 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_TX_LOW_POWER ? 0x01 : 0);
	d[113] = (alarm & OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER ? 0x80 : 0) |
		 (alarm & OPTICAL_FRONTEND_ALARM_RX_LOW_POWER ? 0x40 : 0);
	d[116] = d[112];
	d[117] = d[113];
}

static int optical_sfp_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			    int num)
{
	struct optical_sfp *sfp = i2c_get_adapdata(adap);
	u8 *page, *ptr;
	int i, j;

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];

		if (msg->addr == OPTICAL_SFP_ROM_ADDR) {
			page = sfp->rom;
			ptr = &sfp->rom_ptr;
		} else if (msg->addr == OPTICAL_SFP_DDM_ADDR) {
			page = sfp->ddm;
			ptr = &sfp->ddm_ptr;
		} else {
			return -ENXIO;
		}

		if (msg->flags & I2C_M_RD) {
			if (page == sfp->ddm)
				optical_sfp_refresh_ddm(sfp);
			for (j = 0; j < msg->len; j++)
				msg->buf[j] = page[(*ptr)++];
		} else if (msg->len) {
			*ptr = msg->buf[0];
		}
	}

	return num;
}

static u32 optical_sfp_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm optical_sfp_algo = {
	.master_xfer = optical_sfp_xfer,
	.functionality = optical_sfp_func,
};

static void optical_sfp_unregister(void *data)
{
	struct optical_sfp *sfp = data;

	if (!sfp->registered)
		return;

	i2c_del_adapter(&sfp->adapter);
	of_node_put(sfp->adapter.dev.of_node);
	sfp->registered = false;
}

int devm_optical_frontend_sfp_register(struct optical_frontend *frontend)
{
	struct device *provider = optical_frontend_get_provider(frontend);
	struct device_node *i2c_sfp_node;
	struct optical_sfp *sfp;
	int ret;

	if (!provider)
		return -EINVAL;
	if (!provider->of_node)
		return 0;

	i2c_sfp_node = of_get_child_by_name(provider->of_node, "i2c-sfp");
	if (!i2c_sfp_node)
		return 0;

	sfp = devm_kzalloc(provider, sizeof(*sfp), GFP_KERNEL);
	if (!sfp) {
		of_node_put(i2c_sfp_node);
		return -ENOMEM;
	}

	sfp->frontend = frontend;
	optical_sfp_build_rom(sfp);
	optical_sfp_build_thresholds(sfp);

	sfp->adapter.owner = THIS_MODULE;
	sfp->adapter.algo = &optical_sfp_algo;
	sfp->adapter.dev.parent = provider;
	sfp->adapter.dev.of_node = i2c_sfp_node;
	sfp->adapter.class = 0;
	strscpy(sfp->adapter.name, "optical frontend virtual SFP",
		sizeof(sfp->adapter.name));
	i2c_set_adapdata(&sfp->adapter, sfp);

	ret = i2c_add_adapter(&sfp->adapter);
	if (ret) {
		of_node_put(sfp->adapter.dev.of_node);
		return ret;
	}
	sfp->registered = true;

	ret = devm_add_action_or_reset(provider, optical_sfp_unregister, sfp);
	if (ret)
		return ret;

	dev_info(provider, "virtual SFP bus %d: ROM @0x%02x, DDM @0x%02x\n",
		 sfp->adapter.nr, OPTICAL_SFP_ROM_ADDR, OPTICAL_SFP_DDM_ADDR);
	return 0;
}
EXPORT_SYMBOL_GPL(devm_optical_frontend_sfp_register);

MODULE_DESCRIPTION("Optical frontend virtual SFF-8472 compatibility bus");
MODULE_LICENSE("GPL");
