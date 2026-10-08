// SPDX-License-Identifier: GPL-2.0-only
/* EN7580 XGS-PON MAC preparation. Activation is added separately. */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mfd/syscon.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <net/xpon.h>

#include "airoha_xgspon.h"

#define EN7580_SCU_WAN_CONF		0x070
#define EN7580_SCU_WAN_MASK		GENMASK(7, 0)
#define EN7580_SCU_WAN_XGSPON		0x0a
#define EN7580_XGSPON_TCONTS		32
#define EN7580_XGSPON_GEM_IDS		65536
#define EN7580_XGSPON_RSP_TIME_DEFAULT	0x1600
#define EN7580_XGSPON_IDLE_DEFAULT	0x120
#define EN7580_XGSPON_CMD_TIMEOUT_US	3000
#define EN7580_XGSPON_TABLE_TIMEOUT_US	100000
#define EN7580_XGSPON_GEM_TIMEOUT_MS	10000

struct airoha_xgspon {
	struct device *dev;
	void __iomem *mac;
	struct reset_control *reset;
	struct net_device *netdev;
	struct xpon_device *xpon;
	struct dentry *debugfs;
	bool accessible;
};

static u32 airoha_xgspon_read(struct airoha_xgspon *pon, u32 reg)
{
	return readl(pon->mac + reg);
}

static void airoha_xgspon_write(struct airoha_xgspon *pon, u32 reg, u32 value)
{
	writel(value, pon->mac + reg);
}

static void airoha_xgspon_quiesce(struct airoha_xgspon *pon)
{
	airoha_xgspon_write(pon, EN7580_XGSPON_INT_ENABLE, 0);
	/* Disable automatic serial-number and registration responses. */
	airoha_xgspon_write(pon, EN7580_XGSPON_PLOAMU_CTRL,
			    EN7580_XGSPON_PLOAMU_SW_CTRL);
	airoha_xgspon_write(pon, EN7580_XGSPON_DYING_GASP_CTRL, 0);
	airoha_xgspon_write(pon, EN7580_XGSPON_US_PROF_VLD, 0);
	airoha_xgspon_write(pon, EN7580_XGSPON_ONU_ID,
			    EN7580_XGSPON_ONU_UNASSIGNED);
	airoha_xgspon_write(pon, EN7580_XGSPON_ACTIVATION_ST,
			    EN7580_XGSPON_STATE_O1);
	airoha_xgspon_write(pon, EN7580_XGSPON_MBI_MPI_STOP,
			    EN7580_XGSPON_STOP_MASK);
	/* Flush the posted writes before reset or resource teardown. */
	airoha_xgspon_read(pon, EN7580_XGSPON_MBI_MPI_STOP);
}

static void airoha_xgspon_reset(void *data)
{
	struct airoha_xgspon *pon = data;
	int ret;

	if (pon->accessible) {
		airoha_xgspon_quiesce(pon);
		airoha_xgspon_write(pon, EN7580_XGSPON_SW_RST, 0);
		airoha_xgspon_read(pon, EN7580_XGSPON_SW_RST);
	}
	ret = reset_control_assert(pon->reset);
	if (ret)
		dev_warn(pon->dev, "failed to hold XGS-PON MAC reset: %d\n", ret);
}

static int airoha_xgspon_wait(struct airoha_xgspon *pon, u32 reg,
			      u32 mask, unsigned int timeout_us)
{
	u32 value;

	return readl_poll_timeout_atomic(pon->mac + reg, value,
					(value & mask) == mask, 1, timeout_us);
}

static int airoha_xgspon_init_tables(struct airoha_xgspon *pon)
{
	unsigned long deadline;
	u32 command;
	int i, ret;

	/* All 32 T-CONTs use the indirect command interface on EN7580. */
	for (i = 0; i < EN7580_XGSPON_TCONTS; i++) {
		command = EN7580_XGSPON_TCONT_WRITE |
			  FIELD_PREP(EN7580_XGSPON_TCONT_INDEX, i) |
			  EN7580_XGSPON_ONU_UNASSIGNED;
		airoha_xgspon_write(pon, EN7580_XGSPON_TCONT_ID_CFG, command);
		ret = airoha_xgspon_wait(pon, EN7580_XGSPON_TCONT_ID_STS,
					 EN7580_XGSPON_CMD_DONE,
					EN7580_XGSPON_CMD_TIMEOUT_US);
		if (ret)
			return dev_err_probe(pon->dev, ret,
					     "T-CONT %d clear timed out\n", i);
	}

	/* Follow gponDevResetGemInfo(), rather than the unused GEM_TBL_INIT. */
	deadline = jiffies + msecs_to_jiffies(EN7580_XGSPON_GEM_TIMEOUT_MS);
	for (i = 0; i < EN7580_XGSPON_GEM_IDS; i++) {
		command = EN7580_XGSPON_GEM_WRITE |
			  EN7580_XGSPON_GEM_UNICAST | i;
		airoha_xgspon_write(pon, EN7580_XGSPON_GEM_PORT_CFG, command);
		ret = airoha_xgspon_wait(pon, EN7580_XGSPON_GEM_PORT_STS,
					 EN7580_XGSPON_CMD_DONE,
					EN7580_XGSPON_CMD_TIMEOUT_US);
		if (ret || time_after(jiffies, deadline))
			return dev_err_probe(pon->dev, -ETIMEDOUT,
					     "XGEM %d clear timed out\n", i);
		if (!(i & 0xff))
			cond_resched();
	}

	/* Start both tables together, as in gponDevGemMibTablesInit(). */
	airoha_xgspon_write(pon, EN7580_XGSPON_MIB_TBL_CONFIG,
			    EN7580_XGSPON_TABLE_START);
	airoha_xgspon_write(pon, EN7580_XGSPON_GPIDX_TBL_INIT,
			    EN7580_XGSPON_TABLE_START);
	ret = airoha_xgspon_wait(pon, EN7580_XGSPON_MIB_TBL_CONFIG,
				 EN7580_XGSPON_TABLE_DONE,
				EN7580_XGSPON_TABLE_TIMEOUT_US);
	if (ret)
		return dev_err_probe(pon->dev, ret, "MIB table init timed out\n");
	ret = airoha_xgspon_wait(pon, EN7580_XGSPON_GPIDX_TBL_INIT,
				 EN7580_XGSPON_TABLE_DONE,
				EN7580_XGSPON_TABLE_TIMEOUT_US);
	if (ret)
		return dev_err_probe(pon->dev, ret, "GPIDX table init timed out\n");

	return 0;
}

static int airoha_xgspon_prepare(struct airoha_xgspon *pon)
{
	u32 value;
	int ret;

	airoha_xgspon_write(pon, EN7580_XGSPON_SW_RST, 0);
	airoha_xgspon_read(pon, EN7580_XGSPON_SW_RST);
	udelay(1);
	airoha_xgspon_write(pon, EN7580_XGSPON_SW_RST, EN7580_XGSPON_RST_N);
	ret = airoha_xgspon_wait(pon, EN7580_XGSPON_SW_RST,
				 EN7580_XGSPON_RST_N,
				EN7580_XGSPON_CMD_TIMEOUT_US);
	if (ret)
		return dev_err_probe(pon->dev, ret, "MAC reset release timed out\n");

	airoha_xgspon_quiesce(pon);
	/* Reject an inaccessible or incorrectly mapped MAC before table commands. */
	if (airoha_xgspon_read(pon, EN7580_XGSPON_INT_ENABLE) ||
	    airoha_xgspon_read(pon, EN7580_XGSPON_ONU_ID) !=
	    EN7580_XGSPON_ONU_UNASSIGNED ||
	    (airoha_xgspon_read(pon, EN7580_XGSPON_MBI_MPI_STOP) &
	     EN7580_XGSPON_STOP_MASK) != EN7580_XGSPON_STOP_MASK ||
	    !(airoha_xgspon_read(pon, EN7580_XGSPON_PLOAMU_CTRL) &
	      EN7580_XGSPON_PLOAMU_SW_CTRL))
		return dev_err_probe(pon->dev, -EIO, "MAC quiesce readback failed\n");
	airoha_xgspon_write(pon, EN7580_XGSPON_INT_STATUS, U32_MAX);
	airoha_xgspon_write(pon, EN7580_XGSPON_US_AES_KEY_CTRL, 0);
	airoha_xgspon_write(pon, EN7580_XGSPON_DS_AES_KEY_VLD, 0);
	airoha_xgspon_write(pon, EN7580_XGSPON_RSP_TIME,
			    EN7580_XGSPON_RSP_TIME_DEFAULT);
	value = airoha_xgspon_read(pon, EN7580_XGSPON_IDLE_GEM_CTRL);
	value &= ~EN7580_XGSPON_IDLE_THRESHOLD;
	value |= FIELD_PREP(EN7580_XGSPON_IDLE_THRESHOLD,
			    EN7580_XGSPON_IDLE_DEFAULT);
	airoha_xgspon_write(pon, EN7580_XGSPON_IDLE_GEM_CTRL, value);

	return airoha_xgspon_init_tables(pon);
}

static int airoha_xgspon_status_show(struct seq_file *s, void *unused)
{
	struct airoha_xgspon *pon = s->private;

	seq_puts(s, "stage: MAC prepared; PHY and activation unavailable\n");
	seq_printf(s, "reset: %#x\nonu-id: %#x\nstate: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_SW_RST),
		   airoha_xgspon_read(pon, EN7580_XGSPON_ONU_ID),
		   airoha_xgspon_read(pon, EN7580_XGSPON_ACTIVATION_ST));
	seq_printf(s, "interrupt-enable: %#x\ninterrupt-status: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_INT_ENABLE),
		   airoha_xgspon_read(pon, EN7580_XGSPON_INT_STATUS));
	seq_printf(s, "mbi-mpi-stop: %#x\nploamu-control: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_MBI_MPI_STOP),
		   airoha_xgspon_read(pon, EN7580_XGSPON_PLOAMU_CTRL));
	seq_printf(s, "response-time: %#x\nidle-gem-control: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_RSP_TIME),
		   airoha_xgspon_read(pon, EN7580_XGSPON_IDLE_GEM_CTRL));
	seq_printf(s, "mib-init: %#x\ngpidx-init: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_MIB_TBL_CONFIG),
		   airoha_xgspon_read(pon, EN7580_XGSPON_GPIDX_TBL_INIT));
	seq_printf(s, "fifo-errors: %#x\ntx-errors: %#x\nrx-errors: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_FIFO_ERR_STS),
		   airoha_xgspon_read(pon, EN7580_XGSPON_TX_ERR_STS),
		   airoha_xgspon_read(pon, EN7580_XGSPON_RX_ERR_STS));
	/* Read FIFO occupancy only; do not consume PLOAM or expose keys. */
	seq_printf(s, "ploamu-fifo: %#x\nploamd-fifo: %#x\n",
		   airoha_xgspon_read(pon, EN7580_XGSPON_PLOAMU_FIFO_STS),
		   airoha_xgspon_read(pon, EN7580_XGSPON_PLOAMD_FIFO_STS));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_xgspon_status);

static void airoha_xgspon_unregister(void *data)
{
	struct airoha_xgspon *pon = data;

	debugfs_remove_recursive(pon->debugfs);
	xpon_device_unregister(pon->xpon);
	dev_put(pon->netdev);
}

int airoha_xgspon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xpon_device_desc desc = {};
	struct device_node *eth_node;
	struct airoha_xgspon *pon;
	struct resource *res;
	struct regmap *scu;
	const char *mode;
	char *name;
	void __iomem *base;
	int ret;

	if (device_property_present(dev, "airoha,pon-mode")) {
		ret = device_property_read_string(dev, "airoha,pon-mode", &mode);
		if (ret)
			return ret;
		if (strcmp(mode, "xgspon"))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "EN7580 currently requires xgspon mode\n");
	}

	pon = devm_kzalloc(dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;
	pon->dev = dev;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	if (!res || resource_size(res) < EN7580_XGSPON_OFFSET + EN7580_XGSPON_SIZE)
		return dev_err_probe(dev, -EINVAL, "missing or short XGS-PON region\n");
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);
	pon->mac = base + EN7580_XGSPON_OFFSET;
	pon->reset = devm_reset_control_get_exclusive(dev, "mac");
	if (IS_ERR(pon->reset))
		return dev_err_probe(dev, PTR_ERR(pon->reset), "missing MAC reset\n");
	scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(scu))
		return dev_err_probe(dev, PTR_ERR(scu), "missing SCU regmap\n");

	/* Resolve the explicit GDM2 phandle; EN7580 has no FE xpon_ops yet. */
	eth_node = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!eth_node)
		return dev_err_probe(dev, -EINVAL, "missing GDM2 phandle\n");
	pon->netdev = of_find_net_device_by_node(eth_node);
	of_node_put(eth_node);
	if (!pon->netdev)
		return dev_err_probe(dev, -EPROBE_DEFER, "GDM2 is not registered yet\n");
	/* Install cleanup before modifying any hardware state. */
	ret = devm_add_action_or_reset(dev, airoha_xgspon_unregister, pon);
	if (ret)
		return ret;
	ret = reset_control_assert(pon->reset);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, airoha_xgspon_reset, pon);
	if (ret)
		return ret;
	/* Select the 10G engine while the MAC is held in reset. */
	ret = regmap_update_bits(scu, EN7580_SCU_WAN_CONF,
				 EN7580_SCU_WAN_MASK, EN7580_SCU_WAN_XGSPON);
	if (ret)
		return ret;
	ret = reset_control_deassert(pon->reset);
	if (ret)
		return ret;
	pon->accessible = true;
	ret = airoha_xgspon_prepare(pon);
	if (ret)
		return ret;

	desc.netdev = pon->netdev;
	desc.modes = XPON_MODE_CAP(XPON_MODE_XGSPON);
	desc.mode = XPON_MODE_XGSPON;
	desc.priv = pon;
	pon->xpon = xpon_device_register(dev, &desc);
	if (IS_ERR(pon->xpon)) {
		ret = PTR_ERR(pon->xpon);
		pon->xpon = NULL;
		return ret;
	}

	name = devm_kasprintf(dev, GFP_KERNEL, "airoha-xgspon-%s", dev_name(dev));
	if (name) {
		pon->debugfs = debugfs_create_dir(name, NULL);
		if (!IS_ERR_OR_NULL(pon->debugfs))
			debugfs_create_file("status", 0444, pon->debugfs, pon,
					    &airoha_xgspon_status_fops);
	}
	platform_set_drvdata(pdev, pon);
	dev_info(dev,
		 "EN7580 XGS-PON MAC prepared in O1; PHY, activation and datapath are not enabled\n");
	return 0;
}

void airoha_xgspon_remove(struct platform_device *pdev)
{
	/* Stop diagnostic readers before the devres MAC reset action runs. */
	struct airoha_xgspon *pon = platform_get_drvdata(pdev);

	debugfs_remove_recursive(pon->debugfs);
	pon->debugfs = NULL;
}
