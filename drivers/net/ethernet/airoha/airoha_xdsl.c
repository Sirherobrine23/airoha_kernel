// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN7512/EN7516 xDSL/PTM frontend
 *
 * This driver owns the Linux-visible DMT and PTM resources and connects
 * their lifecycle to the Airoha GDM2 netdev. The legacy tc3162_dmt module also
 * embeds the complete G.hs/ADSL/VDSL training and DSP implementation. That
 * line engine is not implemented here yet. Data-only AFE defaults/maps are
 * loaded separately; experimental AFE initialization is opt-in. Neither this
 * stage nor preparing PTM constitutes DSL training or showtime.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/airoha/pkgids.h>
#include <net/xpon/omci.h>

#include "airoha_eth.h"
#include "airoha_dmt_data.h"

/* Legacy 0xbf900000 is the KSEG1 alias; 0x1faa0000 is an eFuse bank. */
#define AIROHA_XDSL_DMT_PHYS_BASE	0x1f900000
#define AIROHA_XDSL_PTM_PHYS_BASE	0x1fb62000

#define AIROHA_XDSL_DMT_MIN_SIZE		0x2000
#define AIROHA_XDSL_PTM_MIN_SIZE		0x200

/* Shared WAN mux in the system-control block. */
#define SCU_WAN_CONF			0x0070
#define SCU_WAN_MODE_MASK		GENMASK(1, 0)
#define SCU_WAN_MODE_PTM		2

#define SCU_AFE_MUX			0x0860
#define SCU_AFE_MUX_MASK			GENMASK(25, 24)
#define SCU_AFE_MUX_EN7556		BIT(24)
#define CHIP_SCU_AFE_TRIM0		0x01c0
#define CHIP_SCU_AFE_TRIM1		0x01c4

#define DMT_AFE_COMMAND			0x0812
#define DMT_AFE_COUNT			0x0813
#define DMT_AFE_DONE			0x0814
#define DMT_AFE_READ_DATA			0x0818
#define DMT_AFE_SLOT_DATA(n)		(0x081c + (n) * 8)
#define DMT_AFE_SLOT_ADDR(n)		(0x0820 + (n) * 8)
#define DMT_AFE_BUS_MODE			0x0842
#define DMT_AFE_BUS_ID			0x0843
#define DMT_AFE_DIGITAL(n)		(0x1400 + (n) * 0x10)
#define DMT_AFE_READ_DONE			BIT(0)
#define DMT_AFE_WRITE_DONE		BIT(8)

/* DMT system block, relative to the common DMT window. */
#define DMT_VERSION			0x0000
#define DMT_INT_STATUS0			0x0004 /* read-to-clear */
#define DMT_INT_MASK3			0x000c
#define DMT_INT_MASK2			0x000d
#define DMT_INT_MASK1			0x000e
#define DMT_INT_MASK0			0x000f
#define DMT_INT_MASK4			0x0013
#define DMT_INT_MASKED			1

/* PTM-TC MAC block. */
#define PTM_CTRL			0x0004
#define PTM_CTRL_TX_EN			BIT(0)
#define PTM_CTRL_RX_EN			BIT(8)

#define AIROHA_XDSL_NUM_DMT_IRQS	6

/* Lab bring-up only. These do not implement a line configuration API. */
static bool afe_bringup;
module_param(afe_bringup, bool, 0400);
MODULE_PARM_DESC(afe_bringup, "Experimental AFE initialization, no DSL training");

static bool afe_annex_b;
module_param(afe_annex_b, bool, 0400);
MODULE_PARM_DESC(afe_annex_b, "Use Annex B instead of A for experimental AFE init");

static uint afe_edges = 1;
module_param(afe_edges, uint, 0400);
MODULE_PARM_DESC(afe_edges, "Experimental AFE edge bits (0..7), vendor default 1");

enum airoha_xdsl_afe {
	AIROHA_XDSL_AFE_A60928,
	AIROHA_XDSL_AFE_A10627,
};

struct airoha_xdsl_soc_data {
	const char *name;
	const char *firmware;
	struct airoha_dmt_data_spec data_spec;
	enum airoha_pkg family;
	enum airoha_xdsl_afe afe;
};

struct airoha_xdsl {
	struct device *dev;
	const struct airoha_xdsl_soc_data *soc;
	void __iomem *dmt_base;
	void __iomem *ptm_base;
	struct regmap *scu;
	struct regmap *chip_scu;
	struct airoha_soc_id_regs id_regs;
	enum airoha_pkg_ids variant;
	const struct firmware *firmware;
	struct airoha_dmt_data data;
	u32 digital[AIROHA_DMT_MAX_DIGITAL_REGS];
	u32 analog[AIROHA_DMT_ANALOG_REGS];
	u32 saved_wan_mux;
	u32 saved_afe_mux;
	bool wan_mux_saved;
	bool afe_mux_saved;
	bool afe_initialized;
	struct reset_control *dmt_reset;
	struct reset_control *ptm_reset;
	struct net_device *gdm_dev;
	struct omci_line *management;
	/* Serializes hardware state changes from netdev open/close and remove. */
	struct mutex state_lock;
	atomic64_t irq_count[AIROHA_XDSL_NUM_DMT_IRQS];
	u32 dmt_version;
	int dmt_irq;
	bool irq_enabled;
	bool running;
	bool registered;
};

static int airoha_xdsl_identify(struct airoha_xdsl *priv)
{
	struct airoha_soc_id_regs *regs = &priv->id_regs;
	struct device *dev = priv->dev;
	struct regmap *efuse;
	enum airoha_pkg family, pdidr_family;
	u32 value;
	int ret;

	priv->chip_scu = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "airoha,chip-scu");
	if (IS_ERR(priv->chip_scu))
		return PTR_ERR(priv->chip_scu);
	efuse = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,efuse-id");
	if (IS_ERR(efuse))
		return PTR_ERR(efuse);
	ret = regmap_read(priv->scu, AIROHA_NP_SCU_HIR, &value);
	if (ret)
		return ret;
	regs->hir = FIELD_GET(AIROHA_NP_SCU_HIR_MASK, value);
	ret = regmap_read(priv->scu, AIROHA_NP_SCU_PDIDR, &value);
	if (ret)
		return ret;
	regs->pdidr = FIELD_GET(AIROHA_NP_SCU_PDIDR_MASK, value);
	family = airoha_pkg_from_id(regs->hir);
	pdidr_family = airoha_pkg_from_id(regs->pdidr);
	if (family && pdidr_family && family != pdidr_family)
		return -ENODEV;
	if (!family)
		family = pdidr_family;
	if (family != priv->soc->family)
		return -ENODEV;
	/* The phandle covers only EFUSE_VERIFY_DATA0, not the calibration bank. */
	ret = regmap_read(efuse, 0, &regs->efuse_data0);
	if (ret)
		return ret;
	if (family == EN751627_PKG) {
		ret = regmap_read(priv->chip_scu, AIROHA_CHIP_SCU_PACKAGE,
				  &regs->chip_scu_package);
		if (ret)
			return ret;
	}
	priv->variant = airoha_soc_variant_id(regs);
	/* A matching family also contains fiber-only/unaudited packages. */
	switch (priv->variant) {
	case EN7512:
	case EN7513:
	case EN7513G:
	case EN7516G:
		break;
	default:
		return -ENODEV;
	}
	dev_info(dev, "DMT silicon %s (HIR %#x, PDIDR %#x)\n",
		 airoha_pkg_id_name(priv->variant), regs->hir, regs->pdidr);
	return 0;
}

static void airoha_xdsl_release_firmware(void *data)
{
	release_firmware(data);
}

static int airoha_xdsl_load_data(struct airoha_xdsl *priv)
{
	int ret;

	ret = request_firmware(&priv->firmware, priv->soc->firmware, priv->dev);
	if (ret)
		return ret;
	ret = airoha_dmt_data_parse(priv->firmware->data, priv->firmware->size,
				    &priv->soc->data_spec, &priv->data);
	if (ret) {
		release_firmware(priv->firmware);
		priv->firmware = NULL;
		return ret;
	}
	return devm_add_action_or_reset(priv->dev, airoha_xdsl_release_firmware,
				       (void *)priv->firmware);
}

static int airoha_xdsl_prepare_afe(struct airoha_xdsl *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < priv->soc->data_spec.digital_regs; i++)
		priv->digital[i] = get_unaligned_le32(priv->data.digital + i * 4);
	for (i = 0; i < AIROHA_DMT_ANALOG_REGS; i++)
		priv->analog[i] = priv->data.analog[i];
	/* Software-init's Annex A/B branches, before any hardware write. */
	ret = airoha_dmt_field_set(priv->data.analog_map, AIROHA_DMT_ANALOG_MAPS,
				   0x57, afe_annex_b ? 3 : 2, priv->analog);
	if (ret)
		return ret;
	ret = airoha_dmt_field_set(priv->data.analog_map, AIROHA_DMT_ANALOG_MAPS,
				   0x76, afe_annex_b ? 7 : 3, priv->analog);
	if (ret)
		return ret;
	if (priv->soc->afe != AIROHA_XDSL_AFE_A10627)
		return 0;
	/* Never freeze another device's factory values into the data file. */
	ret = regmap_read(priv->chip_scu, CHIP_SCU_AFE_TRIM0, &priv->digital[34]);
	if (ret)
		return ret;
	return regmap_read(priv->chip_scu, CHIP_SCU_AFE_TRIM1, &priv->digital[35]);
}

static void airoha_xdsl_afe_select(struct airoha_xdsl *priv, bool analog)
{
	writeb(analog ? 0x5d : 0x10, priv->dmt_base + DMT_AFE_BUS_ID);
	writeb(analog ? 0 : 1, priv->dmt_base + DMT_AFE_BUS_MODE);
}

static int airoha_xdsl_afe_command(struct airoha_xdsl *priv, bool read,
				   unsigned int count, u32 *value)
{
	u32 status, done = read ? DMT_AFE_READ_DONE : DMT_AFE_WRITE_DONE;
	int ret;

	if (!count || count > 4 || (read && !value))
		return -EINVAL;
	airoha_xdsl_afe_select(priv, true);
	writeb(count - 1, priv->dmt_base + DMT_AFE_COUNT);
	writeb(read ? 1 : 0, priv->dmt_base + DMT_AFE_COMMAND);
	/* Conservative wall-clock wait, not the vendor CPU-dependent busy loop.
	 * Do not invent a W1C write to DONE or retry an unacknowledged command.
	 * Completion-reset semantics still require validation on hardware.
	 */
	usleep_range(250, 500);
	ret = readl_poll_timeout(priv->dmt_base + DMT_AFE_DONE, status,
				 status & done, 250, 100000);
	if (!ret && read)
		*value = readl(priv->dmt_base + DMT_AFE_READ_DATA) & 0xff;
	airoha_xdsl_afe_select(priv, false);
	return ret;
}

static int airoha_xdsl_afe_init(struct airoha_xdsl *priv)
{
	u32 preserved;
	unsigned int i, slot;
	int ret;

	ret = regmap_read(priv->scu, SCU_AFE_MUX, &priv->saved_afe_mux);
	if (ret)
		return ret;
	priv->afe_mux_saved = true;
	ret = regmap_update_bits(priv->scu, SCU_AFE_MUX, SCU_AFE_MUX_MASK,
				 SCU_AFE_MUX_EN7556);
	if (ret)
		return ret;
	/* HwSysReset's system bytes and HwAfeSetEdgeReset's configurable edges. */
	writeb(3, priv->dmt_base + 0x31);
	writeb(0, priv->dmt_base + 0x30);
	writeb(0, priv->dmt_base + 0x37);
	writeb(3, priv->dmt_base + 0x36);
	writeb(afe_edges & 1, priv->dmt_base + 0x803);
	writeb((afe_edges >> 1) & 1, priv->dmt_base + 0x802);
	writeb((afe_edges >> 2) & 1, priv->dmt_base + 0x806);
	writel(0x2e, priv->dmt_base + DMT_AFE_SLOT_ADDR(0));
	ret = airoha_xdsl_afe_command(priv, true, 1, &preserved);
	if (ret)
		return ret;
	/* Preserve from the first batch, rather than temporarily zeroing 0x2e. */
	priv->analog[0x2e] = preserved;
	for (i = 0; i < AIROHA_DMT_ANALOG_REGS; i += 4) {
		for (slot = 0; slot < 4; slot++) {
			writel(priv->analog[i + slot],
			       priv->dmt_base + DMT_AFE_SLOT_DATA(slot));
			writel(i + slot, priv->dmt_base + DMT_AFE_SLOT_ADDR(slot));
		}
		ret = airoha_xdsl_afe_command(priv, false, 4, NULL);
		if (ret)
			return ret;
	}
	for (i = 0; i < priv->soc->data_spec.digital_regs; i++) {
		/* Registers omitted by both vendor initializers. */
		if (i == 7 || i == 8 || i == 17 || i == 25 || i == 26)
			continue;
		writel(priv->digital[i], priv->dmt_base + DMT_AFE_DIGITAL(i));
	}
	readl(priv->dmt_base + DMT_AFE_DIGITAL(0));
	priv->afe_initialized = true;
	return 0;
}

static void airoha_xdsl_restore_muxes(struct airoha_xdsl *priv)
{
	int ret;

	if (priv->afe_mux_saved) {
		ret = regmap_update_bits(priv->scu, SCU_AFE_MUX,
					 SCU_AFE_MUX_MASK, priv->saved_afe_mux);
		if (ret)
			dev_warn(priv->dev, "AFE mux restore failed: %d\n", ret);
		priv->afe_mux_saved = false;
	}
	if (priv->wan_mux_saved) {
		ret = regmap_update_bits(priv->scu, SCU_WAN_CONF,
					 SCU_WAN_MODE_MASK, priv->saved_wan_mux);
		if (ret)
			dev_warn(priv->dev, "WAN mux restore failed: %d\n", ret);
		priv->wan_mux_saved = false;
	}
	priv->afe_initialized = false;
}

/*
 * EN751221 (A60928) and EN751627 (A10627) share this frontend, not the
 * calibration tables or DSP state layouts of their legacy DMT modules.
 * Keep line-engine status distinct from the Linux/PTM lifecycle: preparing
 * the hardware does not mean training has started or showtime was reached.
 */
static int airoha_xdsl_get_line_status(void *data,
				       struct omci_line_status *status)
{
	struct airoha_xdsl *priv = data;

	mutex_lock(&priv->state_lock);
	*status = (struct omci_line_status) {
		.access = OMCI_ACCESS_XDSL,
		.state = OMCI_LINE_UNAVAILABLE,
		.flags = priv->running ? OMCI_LINE_F_STARTED : 0,
	};
	mutex_unlock(&priv->state_lock);
	return 0;
}

static const struct omci_line_ops airoha_xdsl_management_ops = {
	.get_status = airoha_xdsl_get_line_status,
	/* Add retrain only when a real, SoC-specific DMT engine is present. */
};

static void airoha_xdsl_dmt_mask_all(struct airoha_xdsl *priv)
{
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK0);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK1);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK2);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK3);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK4);
}

static void airoha_xdsl_publish_link(struct airoha_xdsl *priv, bool link,
				     u64 downstream, u64 upstream)
{
	struct airoha_xdsl_link_state state = {
		.valid = true,
		.link = link,
		.speed = link ? div_u64(downstream, 1000000) : SPEED_UNKNOWN,
		.duplex = link ? DUPLEX_FULL : DUPLEX_UNKNOWN,
		.autoneg = AUTONEG_DISABLE,
		.port = PORT_OTHER,
		.rx_line_rate_bps = downstream,
		.tx_line_rate_bps = upstream,
	};

	airoha_eth_xdsl_update_link(priv->gdm_dev, &state);
}

static void airoha_xdsl_ptm_stop(struct airoha_xdsl *priv)
{
	u32 val;

	/* Vendor ptm_close(): stop TX before RX. Flush the TX write before
	 * stopping RX; do not change the dummy U-cell mode independently of
	 * the DMT engine's VDSL2_SET_REDUNDANT_MODE_SET (0x3002) setting.
	 */
	val = readl(priv->ptm_base + PTM_CTRL);
	val &= ~PTM_CTRL_TX_EN;
	writel(val, priv->ptm_base + PTM_CTRL);
	val = readl(priv->ptm_base + PTM_CTRL);
	val &= ~PTM_CTRL_RX_EN;
	writel(val, priv->ptm_base + PTM_CTRL);
	readl(priv->ptm_base + PTM_CTRL);
}

static int airoha_xdsl_reset_deassert(struct reset_control *reset)
{
	int ret;

	ret = reset_control_assert(reset);
	if (ret)
		return ret;

	msleep(20);
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;

	msleep(20);
	return 0;
}

static int airoha_xdsl_hw_start(struct airoha_xdsl *priv)
{
	int ret;

	mutex_lock(&priv->state_lock);
	if (priv->running) {
		ret = 0;
		goto out_unlock;
	}

	ret = airoha_xdsl_prepare_afe(priv);
	if (ret)
		goto out_unlock;
	ret = regmap_read(priv->scu, SCU_WAN_CONF, &priv->saved_wan_mux);
	if (ret)
		goto out_unlock;
	priv->wan_mux_saved = true;
	ret = regmap_update_bits(priv->scu, SCU_WAN_CONF,
				 SCU_WAN_MODE_MASK, SCU_WAN_MODE_PTM);
	if (ret)
		goto err_restore_muxes;

	ret = airoha_xdsl_reset_deassert(priv->dmt_reset);
	if (ret)
		goto err_assert_dmt;

	airoha_xdsl_dmt_mask_all(priv);
	/* DMT_INT_STATUS0 is read-to-clear. */
	readl(priv->dmt_base + DMT_INT_STATUS0);
	priv->dmt_version = readl(priv->dmt_base + DMT_VERSION);

	ret = airoha_xdsl_reset_deassert(priv->ptm_reset);
	if (ret)
		goto err_assert_ptm;

	/* The line engine must select the negotiated ATM/PTM TPSTC mode.
	 * Preparing the frontend is not permission to force PTM or enable TX.
	 */
	airoha_xdsl_ptm_stop(priv);

	if (afe_bringup) {
		ret = airoha_xdsl_afe_init(priv);
		if (ret) {
			dev_err(priv->dev, "experimental AFE initialization failed: %d\n",
				ret);
			goto err_assert_ptm;
		}
	}

	/* The line engine owns unmasking; keep the periodic DMT IRQ quiescent. */
	enable_irq(priv->dmt_irq);
	priv->irq_enabled = true;
	priv->running = true;
	airoha_xdsl_publish_link(priv, false, 0, 0);

	dev_info(priv->dev,
		 "%s DMT %#08x: data ready, AFE %s; no line engine\n",
		 priv->soc->name, priv->dmt_version,
		 priv->afe_initialized ? "initialized (experimental)" : "not programmed");
	mutex_unlock(&priv->state_lock);
	return 0;

err_assert_ptm:
	reset_control_assert(priv->ptm_reset);
err_assert_dmt:
	reset_control_assert(priv->dmt_reset);
err_restore_muxes:
	airoha_xdsl_restore_muxes(priv);
out_unlock:
	mutex_unlock(&priv->state_lock);
	return ret;
}

static void airoha_xdsl_hw_stop(struct airoha_xdsl *priv)
{
	mutex_lock(&priv->state_lock);
	if (!priv->running)
		goto out_unlock;

	airoha_xdsl_publish_link(priv, false, 0, 0);
	airoha_xdsl_ptm_stop(priv);
	airoha_xdsl_dmt_mask_all(priv);
	if (priv->irq_enabled) {
		disable_irq(priv->dmt_irq);
		priv->irq_enabled = false;
	}
	reset_control_assert(priv->ptm_reset);
	reset_control_assert(priv->dmt_reset);
	priv->running = false;
	airoha_xdsl_restore_muxes(priv);

out_unlock:
	mutex_unlock(&priv->state_lock);
}

static int airoha_xdsl_netdev_start(void *data)
{
	return airoha_xdsl_hw_start(data);
}

static void airoha_xdsl_netdev_stop(void *data)
{
	airoha_xdsl_hw_stop(data);
}

static const struct airoha_xdsl_link_ops airoha_xdsl_link_ops = {
	.start = airoha_xdsl_netdev_start,
	.stop = airoha_xdsl_netdev_stop,
};

static irqreturn_t airoha_xdsl_irq(int irq, void *data)
{
	struct airoha_xdsl *priv = data;
	u32 status;
	int i;

	status = readl(priv->dmt_base + DMT_INT_STATUS0);
	if (!status)
		return IRQ_NONE;

	for (i = 0; i < AIROHA_XDSL_NUM_DMT_IRQS; i++)
		if (status & BIT(i))
			atomic64_inc(&priv->irq_count[i]);

	/* A future line engine consumes the decoded events before unmasking. */
	airoha_xdsl_dmt_mask_all(priv);
	dev_dbg_ratelimited(priv->dev, "masked unsupported DMT IRQ status %#x\n",
			    status);

	return IRQ_HANDLED;
}

static int airoha_xdsl_resolve_datapath(struct airoha_xdsl *priv)
{
	struct device_node *eth_node;

	eth_node = of_parse_phandle(priv->dev->of_node, "ethernet", 0);
	if (!eth_node)
		return dev_err_probe(priv->dev, -EINVAL,
				     "missing GDM2 ethernet phandle\n");

	priv->gdm_dev = of_find_net_device_by_node(eth_node);
	of_node_put(eth_node);
	if (!priv->gdm_dev)
		return dev_err_probe(priv->dev, -EPROBE_DEFER,
				     "GDM2 netdev is not registered yet\n");

	return 0;
}

static int airoha_xdsl_map_resource(struct platform_device *pdev,
				    const char *name, resource_size_t start,
				    resource_size_t min_size, void __iomem **base)
{
	struct device *dev = &pdev->dev;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "missing %s resource\n", name);
	if (res->start != start)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected %s base %pa\n", name, &res->start);
	if (resource_size(res) < min_size)
		return dev_err_probe(dev, -EINVAL,
				     "%s resource is too small\n", name);

	*base = devm_ioremap_resource(dev, res);
	return PTR_ERR_OR_ZERO(*base);
}

static int airoha_xdsl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_xdsl *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->soc = device_get_match_data(dev);
	if (!priv->soc)
		return -EINVAL;
	if (afe_edges > 7)
		return dev_err_probe(dev, -EINVAL, "afe_edges must be between 0 and 7\n");

	mutex_init(&priv->state_lock);

	ret = airoha_xdsl_map_resource(pdev, "dmt",
				       AIROHA_XDSL_DMT_PHYS_BASE,
				       AIROHA_XDSL_DMT_MIN_SIZE,
				       &priv->dmt_base);
	if (ret)
		return ret;

	ret = airoha_xdsl_map_resource(pdev, "ptm",
				       AIROHA_XDSL_PTM_PHYS_BASE,
				       AIROHA_XDSL_PTM_MIN_SIZE,
				       &priv->ptm_base);
	if (ret)
		return ret;

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu),
				     "failed to get SCU regmap\n");
	ret = airoha_xdsl_identify(priv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot verify supported DSL silicon\n");
	ret = airoha_xdsl_load_data(priv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot load/validate %s\n",
				     priv->soc->firmware);
	if (afe_bringup)
		dev_warn(dev, "experimental AFE bring-up enabled; no DSL training support\n");

	priv->dmt_irq = platform_get_irq_byname(pdev, "dmt");
	if (priv->dmt_irq < 0)
		return dev_err_probe(dev, priv->dmt_irq,
				     "failed to get DMT interrupt\n");

	priv->dmt_reset = devm_reset_control_get_exclusive(dev, "dmt");
	if (IS_ERR(priv->dmt_reset))
		return dev_err_probe(dev, PTR_ERR(priv->dmt_reset),
				     "failed to get DMT reset\n");

	priv->ptm_reset = devm_reset_control_get_exclusive(dev, "ptm");
	if (IS_ERR(priv->ptm_reset))
		return dev_err_probe(dev, PTR_ERR(priv->ptm_reset),
				     "failed to get PTM reset\n");

	ret = devm_request_irq(dev, priv->dmt_irq, airoha_xdsl_irq,
			       IRQF_NO_AUTOEN, dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request DMT interrupt\n");

	ret = airoha_xdsl_resolve_datapath(priv);
	if (ret)
		return ret;

	ret = airoha_eth_register_xdsl(priv->gdm_dev, &airoha_xdsl_link_ops,
				       priv);
	if (ret) {
		put_device(&priv->gdm_dev->dev);
		priv->gdm_dev = NULL;
		return dev_err_probe(dev, ret,
				     "failed to register GDM2 xDSL frontend\n");
	}
	priv->registered = true;
	priv->management = omci_line_register(dev, priv->gdm_dev,
					      &airoha_xdsl_management_ops, priv);
	if (IS_ERR(priv->management)) {
		ret = PTR_ERR(priv->management);
		priv->management = NULL;
		airoha_eth_unregister_xdsl(priv->gdm_dev,
					   &airoha_xdsl_link_ops, priv);
		priv->registered = false;
		airoha_xdsl_hw_stop(priv);
		put_device(&priv->gdm_dev->dev);
		return dev_err_probe(dev, ret,
				     "failed to register xDSL line management\n");
	}

	platform_set_drvdata(pdev, priv);
	airoha_xdsl_publish_link(priv, false, 0, 0);

	dev_info(dev, "%s xDSL/PTM frontend, AFE %s, IRQ %d\n",
		 priv->soc->name,
		 priv->soc->afe == AIROHA_XDSL_AFE_A60928 ? "A60928" : "A10627",
		 priv->dmt_irq);

	return 0;
}

static void airoha_xdsl_remove(struct platform_device *pdev)
{
	struct airoha_xdsl *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	/* Drain management readers before taking lifecycle locks or freeing priv. */
	omci_line_unregister(priv->management);
	priv->management = NULL;
	if (priv->registered)
		airoha_eth_unregister_xdsl(priv->gdm_dev,
					   &airoha_xdsl_link_ops, priv);
	airoha_xdsl_hw_stop(priv);
	if (priv->gdm_dev)
		put_device(&priv->gdm_dev->dev);
}

static const struct airoha_xdsl_soc_data en751221_xdsl_data = {
	.name = "EN7512",
	.firmware = "airoha/en7512-afe-v1.bin",
	.data_spec = { .target = 0x7512, .digital_regs = 34, .digital_maps = 208 },
	.family = EN751221_PKG,
	.afe = AIROHA_XDSL_AFE_A60928,
};

static const struct airoha_xdsl_soc_data en751627_xdsl_data = {
	.name = "EN7516",
	.firmware = "airoha/en7516-afe-v1.bin",
	.data_spec = { .target = 0x7516, .digital_regs = 44, .digital_maps = 262 },
	.family = EN751627_PKG,
	.afe = AIROHA_XDSL_AFE_A10627,
};

static const struct of_device_id airoha_xdsl_of_match[] = {
	{ .compatible = "econet,en751221-xdsl", .data = &en751221_xdsl_data },
	{ .compatible = "econet,en751627-xdsl", .data = &en751627_xdsl_data },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_xdsl_of_match);

static struct platform_driver airoha_xdsl_driver = {
	.probe = airoha_xdsl_probe,
	.remove = airoha_xdsl_remove,
	.driver = {
		.name = "airoha-xdsl",
		.of_match_table = airoha_xdsl_of_match,
	},
};
module_platform_driver(airoha_xdsl_driver);

MODULE_DESCRIPTION("EcoNet EN7512/EN7516 xDSL/PTM frontend");
MODULE_FIRMWARE("airoha/en7512-afe-v1.bin");
MODULE_FIRMWARE("airoha/en7516-afe-v1.bin");
MODULE_LICENSE("GPL");
