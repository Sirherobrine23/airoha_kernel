// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/devcoredump.h>
#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include "airoha_eth.h"

#define NPU_EN7523_FIRMWARE_DATA		"airoha/en7523_npu_data.bin"
#define NPU_EN7523_FIRMWARE_RV32		"airoha/en7523_npu_rv32.bin"
#define NPU_EN7581_FIRMWARE_DATA		"airoha/en7581_npu_data.bin"
#define NPU_EN7581_FIRMWARE_RV32		"airoha/en7581_npu_rv32.bin"
#define NPU_EN7581_7996_FIRMWARE_DATA		"airoha/en7581_MT7996_npu_data.bin"
#define NPU_EN7581_7996_FIRMWARE_RV32		"airoha/en7581_MT7996_npu_rv32.bin"
#define NPU_AN7583_FIRMWARE_DATA		"airoha/an7583_npu_data.bin"
#define NPU_AN7583_FIRMWARE_RV32		"airoha/an7583_npu_rv32.bin"
#define NPU_EN7581_FIRMWARE_RV32_MAX_SIZE	0x200000
#define NPU_EN7581_FIRMWARE_DATA_MAX_SIZE	0x10000
#define NPU_DUMP_SIZE				512

#define REG_NPU_LOCAL_SRAM		0x0

#define REG_PC_DBG(_soc, _n)		((_soc->pc_base_addr) + ((_n) * 0x100))
#define REG_CR_BOOT_TRIGGER(_soc)	((_soc->cluster_base_addr) + 0x000)
#define REG_CR_BOOT_CONFIG(_soc)	((_soc->cluster_base_addr) + 0x004)
#define REG_CR_BOOT_BASE(_soc, _n)	((_soc->cluster_base_addr) + 0x020 + ((_n) << 2))

#define NPU_MBOX_BASE_ADDR		0x30c000

#define REG_CR_MBOX_INT_STATUS		(NPU_MBOX_BASE_ADDR + 0x000)
#define MBOX_INT_STATUS_MASK		BIT(8)

#define REG_CR_MBOX_INT_MASK(_n)	(NPU_MBOX_BASE_ADDR + 0x004 + ((_n) << 2))
#define REG_CR_MBQ0_CTRL(_n)		(NPU_MBOX_BASE_ADDR + 0x030 + ((_n) << 2))
#define REG_CR_MBQ8_CTRL(_n)		(NPU_MBOX_BASE_ADDR + 0x0b0 + ((_n) << 2))
#define REG_CR_NPU_MIB(_n)		(NPU_MBOX_BASE_ADDR + 0x140 + ((_n) << 2))

#define NPU_WLAN_BASE_ADDR		0x30d000

#define REG_IRQ_STATUS			(NPU_WLAN_BASE_ADDR + 0x030)
#define REG_IRQ_RXDONE(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 2) + 0x034)
#define NPU_IRQ_RX_MASK(_n)		((_n) == 1 ? BIT(17) : BIT(16))

#define REG_TX_BASE(_n)			(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x080)
#define REG_TX_DSCP_NUM(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x084)
#define REG_TX_CPU_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x088)
#define REG_TX_DMA_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x08c)

#define REG_RX_BASE(_n)			(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x180)
#define REG_RX_DSCP_NUM(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x184)
#define REG_RX_CPU_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x188)
#define REG_RX_DMA_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x18c)

#define NPU_TIMER_BASE_ADDR		0x310100
#define REG_WDT_TIMER_CTRL(_n)		(NPU_TIMER_BASE_ADDR + ((_n) * 0x100))
#define WDT_EN_MASK			BIT(25)
#define WDT_INTR_MASK			BIT(21)

enum {
	NPU_OP_SET = 1,
	NPU_OP_SET_NO_WAIT,
	NPU_OP_GET,
	NPU_OP_GET_NO_WAIT,
};

enum {
	NPU_FUNC_WIFI,
	NPU_FUNC_TUNNEL,
	NPU_FUNC_NOTIFY,
	NPU_FUNC_DBA,
	NPU_FUNC_TR471,
	NPU_FUNC_PPE,
};

enum {
	NPU_MBOX_ERROR,
	NPU_MBOX_SUCCESS,
};

enum {
	PPE_FUNC_SET_WAIT,
	PPE_FUNC_SET_WAIT_HWNAT_INIT,
	PPE_FUNC_SET_WAIT_HWNAT_DEINIT,
	PPE_FUNC_SET_WAIT_API,
	PPE_FUNC_SET_WAIT_FLOW_STATS_SETUP,
};

enum {
	PPE2_SRAM_SET_ENTRY,
	PPE_SRAM_SET_ENTRY,
	PPE_SRAM_SET_VAL,
	PPE_SRAM_RESET_VAL,
};

enum {
	QDMA_WAN_ETHER = 1,
	QDMA_WAN_PON_XDSL,
};

struct airoha_npu_fw {
	const char *name;
	int max_size;
};

struct airoha_npu_soc_data {
	enum airoha_npu_version version;
	int max_cores;
	int cluster_base_addr;
	int pc_base_addr;

	struct airoha_npu_fw fw_rv32;
	struct airoha_npu_fw fw_data;

	void (*boot_core)(struct airoha_npu *npu, struct reserved_mem *rmem);

	const u32 wlan_func_set[WLAN_FUNC_SET_WAIT_MAX];
	const u32 wlan_func_get[WLAN_FUNC_GET_WAIT_MAX];
};

#define MBOX_MSG_FUNC_ID	GENMASK(14, 11)
#define MBOX_MSG_STATIC_BUF	BIT(5)
#define MBOX_MSG_STATUS		GENMASK(4, 2)
#define MBOX_MSG_DONE		BIT(1)
#define MBOX_MSG_WAIT_RSP	BIT(0)

#define PPE_TYPE_L2B_IPV4	2
#define PPE_TYPE_L2B_IPV4_IPV6	3

struct ppe_mbox_data {
	u32 func_type;
	u32 func_id;
	union {
		struct {
			u8 cds;
			u8 xpon_hal_api;
			u8 wan_xsi;
			u8 ct_joyme4;
			u8 max_packet;
			u8 rsv[3];
			u32 ppe_type;
			u32 wan_mode;
			u32 wan_sel;
		} init_info;
		struct {
			u32 func_id;
			u32 size;
			u32 data;
		} set_info;
		struct {
			u32 npu_stats_addr;
			u32 foe_stats_addr;
		} stats_info;
	};
};

struct wlan_mbox_data {
	u32 ifindex:4;
	u32 func_type:4;
	u32 func_id;
	DECLARE_FLEX_ARRAY(u8, d);
};

static int airoha_npu_send_msg(struct airoha_npu *npu, int func_id,
			       void *p, int size)
{
	u16 core = 0; /* FIXME */
	u32 val, offset = core << 4;
	dma_addr_t dma_addr;
	int ret;

	if (npu->soc_data->version == NPU_V1 && func_id > NPU_FUNC_TR471)
		return -EOPNOTSUPP;

	dma_addr = dma_map_single(npu->dev, p, size, DMA_TO_DEVICE);
	ret = dma_mapping_error(npu->dev, dma_addr);
	if (ret)
		return ret;

	spin_lock_bh(&npu->cores[core].lock);

	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(0) + offset, dma_addr);
	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(1) + offset, size);
	regmap_read(npu->regmap, REG_CR_MBQ0_CTRL(2) + offset, &val);
	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(2) + offset, val + 1);
	val = FIELD_PREP(MBOX_MSG_FUNC_ID, func_id) | MBOX_MSG_WAIT_RSP;
	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(3) + offset, val);

	ret = regmap_read_poll_timeout_atomic(npu->regmap,
					      REG_CR_MBQ0_CTRL(3) + offset,
					      val, (val & MBOX_MSG_DONE),
					      100, 100 * MSEC_PER_SEC);
	if (!ret && FIELD_GET(MBOX_MSG_STATUS, val) != NPU_MBOX_SUCCESS)
		ret = -EINVAL;

	spin_unlock_bh(&npu->cores[core].lock);

	dma_unmap_single(npu->dev, dma_addr, size, DMA_TO_DEVICE);

	return ret;
}

static int airoha_npu_load_firmware(struct device *dev, void __iomem *addr,
				    const char *fw_name, int fw_max_size)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware_direct(&fw, fw_name, dev);
	if (ret)
		return ret == -ENOENT ? -EPROBE_DEFER : ret;

	if (fw->size > fw_max_size) {
		dev_err(dev, "%s: fw size too overlimit (%zu)\n",
			fw_name, fw->size);
		ret = -E2BIG;
		goto out;
	}

	memcpy_toio(addr, fw->data, fw->size);
out:
	release_firmware(fw);

	return ret;
}

static int
airoha_npu_load_firmware_from_dts(struct device *dev, void __iomem *addr,
				  void __iomem *base)
{
	const char *fw_names[2];
	int ret;

	ret = of_property_read_string_array(dev->of_node, "firmware-name",
					    fw_names, ARRAY_SIZE(fw_names));
	if (ret != ARRAY_SIZE(fw_names))
		return -EINVAL;

	ret = airoha_npu_load_firmware(dev, addr, fw_names[0],
				       NPU_EN7581_FIRMWARE_RV32_MAX_SIZE);
	if (ret)
		return ret;

	return airoha_npu_load_firmware(dev, base + REG_NPU_LOCAL_SRAM,
					fw_names[1],
					NPU_EN7581_FIRMWARE_DATA_MAX_SIZE);
}

static int airoha_npu_run_firmware(struct device *dev, void __iomem *base,
				   struct reserved_mem *rmem)
{
	const struct airoha_npu_soc_data *soc;
	void __iomem *addr;
	int ret;

	soc = of_device_get_match_data(dev);
	if (!soc)
		return -EINVAL;

	addr = devm_memremap(dev, rmem->base, rmem->size, MEMREMAP_WC);
	if (IS_ERR(addr))
		return PTR_ERR(addr);

	/* Try to load firmware images using the firmware names provided via
	 * dts if available.
	 */
	if (of_find_property(dev->of_node, "firmware-name", NULL))
		return airoha_npu_load_firmware_from_dts(dev, addr, base);

	/* Load rv32 npu firmware */
	ret = airoha_npu_load_firmware(dev, addr, soc->fw_rv32.name,
				       soc->fw_rv32.max_size);
	if (ret)
		return ret;

	/* Load data npu firmware */
	return airoha_npu_load_firmware(dev, base + REG_NPU_LOCAL_SRAM,
					soc->fw_data.name,
					soc->fw_data.max_size);
}

static irqreturn_t airoha_npu_mbox_handler(int irq, void *npu_instance)
{
	struct airoha_npu *npu = npu_instance;

	/* clear mbox interrupt status */
	regmap_write(npu->regmap, REG_CR_MBOX_INT_STATUS,
		     MBOX_INT_STATUS_MASK);

	/* acknowledge npu */
	regmap_update_bits(npu->regmap, REG_CR_MBQ8_CTRL(3),
			   MBOX_MSG_STATUS | MBOX_MSG_DONE, MBOX_MSG_DONE);

	return IRQ_HANDLED;
}

static void airoha_npu_wdt_work(struct work_struct *work)
{
	struct airoha_npu_core *core;
	struct airoha_npu *npu;
	void *dump;
	u32 val[3];
	int c;

	core = container_of(work, struct airoha_npu_core, wdt_work);
	npu = core->npu;

	dump = vzalloc(NPU_DUMP_SIZE);
	if (!dump)
		return;

	c = core - &npu->cores[0];
	regmap_bulk_read(npu->regmap, REG_PC_DBG(npu->soc_data, c), val, ARRAY_SIZE(val));
	snprintf(dump, NPU_DUMP_SIZE, "PC: %08x SP: %08x LR: %08x\n",
		 val[0], val[1], val[2]);

	dev_coredumpv(npu->dev, dump, NPU_DUMP_SIZE, GFP_KERNEL);
}

static irqreturn_t airoha_npu_wdt_handler(int irq, void *core_instance)
{
	struct airoha_npu_core *core = core_instance;
	struct airoha_npu *npu = core->npu;
	int c = core - &npu->cores[0];
	u32 val;

	regmap_set_bits(npu->regmap, REG_WDT_TIMER_CTRL(c), WDT_INTR_MASK);
	if (!regmap_read(npu->regmap, REG_WDT_TIMER_CTRL(c), &val) &&
	    FIELD_GET(WDT_EN_MASK, val))
		schedule_work(&core->wdt_work);

	return IRQ_HANDLED;
}

static int airoha_npu_ppe_init(struct airoha_npu *npu)
{
	struct ppe_mbox_data *ppe_data;
	int err;

	ppe_data = kzalloc(sizeof(*ppe_data), GFP_KERNEL);
	if (!ppe_data)
		return -ENOMEM;

	ppe_data->func_type = NPU_OP_SET;
	ppe_data->func_id = PPE_FUNC_SET_WAIT_HWNAT_INIT;
	ppe_data->init_info.ppe_type = PPE_TYPE_L2B_IPV4_IPV6;
	ppe_data->init_info.wan_mode = QDMA_WAN_ETHER;

	err = airoha_npu_send_msg(npu, NPU_FUNC_PPE, ppe_data,
				  sizeof(*ppe_data));
	kfree(ppe_data);

	return err;
}

static int airoha_npu_ppe_deinit(struct airoha_npu *npu)
{
	struct ppe_mbox_data *ppe_data;
	int err;

	ppe_data = kzalloc(sizeof(*ppe_data), GFP_KERNEL);
	if (!ppe_data)
		return -ENOMEM;

	ppe_data->func_type = NPU_OP_SET;
	ppe_data->func_id = PPE_FUNC_SET_WAIT_HWNAT_DEINIT;

	err = airoha_npu_send_msg(npu, NPU_FUNC_PPE, ppe_data,
				  sizeof(*ppe_data));
	kfree(ppe_data);

	return err;
}

static int airoha_npu_ppe_flush_sram_entries(struct airoha_npu *npu,
					     dma_addr_t foe_addr,
					     int sram_num_entries)
{
	struct ppe_mbox_data *ppe_data;
	int err;

	ppe_data = kzalloc(sizeof(*ppe_data), GFP_KERNEL);
	if (!ppe_data)
		return -ENOMEM;

	ppe_data->func_type = NPU_OP_SET;
	ppe_data->func_id = PPE_FUNC_SET_WAIT_API;
	ppe_data->set_info.func_id = PPE_SRAM_RESET_VAL;
	ppe_data->set_info.data = foe_addr;
	ppe_data->set_info.size = sram_num_entries;

	err = airoha_npu_send_msg(npu, NPU_FUNC_PPE, ppe_data,
				  sizeof(*ppe_data));
	kfree(ppe_data);

	return err;
}

static int airoha_npu_foe_commit_entry(struct airoha_npu *npu,
				       dma_addr_t foe_addr,
				       u32 entry_size, u32 hash, bool ppe2)
{
	struct ppe_mbox_data *ppe_data;
	int err;

	ppe_data = kzalloc(sizeof(*ppe_data), GFP_ATOMIC);
	if (!ppe_data)
		return -ENOMEM;

	ppe_data->func_type = NPU_OP_SET;
	ppe_data->func_id = PPE_FUNC_SET_WAIT_API;
	ppe_data->set_info.data = foe_addr;
	ppe_data->set_info.size = entry_size;
	ppe_data->set_info.func_id = ppe2 ? PPE2_SRAM_SET_ENTRY
					  : PPE_SRAM_SET_ENTRY;

	err = airoha_npu_send_msg(npu, NPU_FUNC_PPE, ppe_data,
				  sizeof(*ppe_data));
	if (err)
		goto out;

	ppe_data->set_info.func_id = PPE_SRAM_SET_VAL;
	ppe_data->set_info.data = hash;
	ppe_data->set_info.size = sizeof(u32);

	err = airoha_npu_send_msg(npu, NPU_FUNC_PPE, ppe_data,
				  sizeof(*ppe_data));
out:
	kfree(ppe_data);

	return err;
}

static int airoha_npu_ppe_stats_setup(struct airoha_npu *npu,
				      dma_addr_t foe_stats_addr,
				      u32 num_stats_entries)
{
	int err, size = num_stats_entries * sizeof(*npu->stats);
	struct ppe_mbox_data *ppe_data;

	ppe_data = kzalloc(sizeof(*ppe_data), GFP_ATOMIC);
	if (!ppe_data)
		return -ENOMEM;

	ppe_data->func_type = NPU_OP_SET;
	ppe_data->func_id = PPE_FUNC_SET_WAIT_FLOW_STATS_SETUP;
	ppe_data->stats_info.foe_stats_addr = foe_stats_addr;

	err = airoha_npu_send_msg(npu, NPU_FUNC_PPE, ppe_data,
				  sizeof(*ppe_data));
	if (err)
		goto out;

	npu->stats = devm_ioremap(npu->dev,
				  ppe_data->stats_info.npu_stats_addr,
				  size);
	if (!npu->stats)
		err = -ENOMEM;
out:
	kfree(ppe_data);

	return err;
}

static int airoha_npu_wlan_msg_send(struct airoha_npu *npu, int ifindex,
				    enum airoha_npu_wlan_set_cmd func_id,
				    void *data, int data_len, gfp_t gfp)
{
	struct wlan_mbox_data *wlan_data;
	int err, len;

	if (npu->soc_data->wlan_func_set[func_id] == WLAN_FUNC_SET_WAIT_MAX)
		return -EOPNOTSUPP;
	func_id = npu->soc_data->wlan_func_set[func_id];

	len = sizeof(*wlan_data) + data_len;
	wlan_data = kzalloc(len, gfp);
	if (!wlan_data)
		return -ENOMEM;

	wlan_data->ifindex = ifindex;
	wlan_data->func_type = NPU_OP_SET;
	wlan_data->func_id = func_id;
	memcpy(wlan_data->d, data, data_len);

	err = airoha_npu_send_msg(npu, NPU_FUNC_WIFI, wlan_data, len);
	kfree(wlan_data);

	return err;
}

static int airoha_npu_wlan_msg_get(struct airoha_npu *npu, int ifindex,
				   enum airoha_npu_wlan_get_cmd func_id,
				   void *data, int data_len, gfp_t gfp)
{
	struct wlan_mbox_data *wlan_data;
	int err, len;

	if (npu->soc_data->wlan_func_get[func_id] == WLAN_FUNC_GET_WAIT_MAX)
		return -EOPNOTSUPP;
	func_id = npu->soc_data->wlan_func_get[func_id];

	len = sizeof(*wlan_data) + data_len;
	wlan_data = kzalloc(len, gfp);
	if (!wlan_data)
		return -ENOMEM;

	wlan_data->ifindex = ifindex;
	wlan_data->func_type = NPU_OP_GET;
	wlan_data->func_id = func_id;

	err = airoha_npu_send_msg(npu, NPU_FUNC_WIFI, wlan_data, len);
	if (!err)
		memcpy(data, wlan_data->d, data_len);
	kfree(wlan_data);

	return err;
}

static int
airoha_npu_wlan_set_reserved_memory(struct airoha_npu *npu,
				    int ifindex, const char *name,
				    enum airoha_npu_wlan_set_cmd func_id)
{
	struct device *dev = npu->dev;
	struct resource res;
	int err;
	u32 val;

	err = of_reserved_mem_region_to_resource_byname(dev->of_node, name,
							&res);
	if (err)
		return err;

	val = res.start;
	return airoha_npu_wlan_msg_send(npu, ifindex, func_id, &val,
					sizeof(val), GFP_KERNEL);
}

static int airoha_npu_wlan_init_memory(struct airoha_npu *npu)
{
	enum airoha_npu_wlan_set_cmd cmd = WLAN_FUNC_SET_WAIT_NPU_BAND0_ONCPU;
	u32 val = 0;
	int err;

	err = airoha_npu_wlan_msg_send(npu, 1, cmd, &val, sizeof(val),
				       GFP_KERNEL);
	if (err && err != -EOPNOTSUPP) {
		dev_err(npu->dev, "error on get NPU_BAND0 value: %d", err);
		return err;
	}

	cmd = WLAN_FUNC_SET_WAIT_PKT_BUF_ADDR;
	err = airoha_npu_wlan_set_reserved_memory(npu, 0, "pkt", cmd);
	if (err) {
		dev_err(npu->dev, "error on set pkt memory region: %d", err);
		return err;
	}

	if (of_property_match_string(npu->dev->of_node, "memory-region-names",
				     "tx-pkt") >= 0) {
		cmd = WLAN_FUNC_SET_WAIT_TX_PKT_BUF_ADDR;
		err = airoha_npu_wlan_set_reserved_memory(npu, 0, "tx-pkt", cmd);
		if (err)
			return err;
	}

	if (of_property_match_string(npu->dev->of_node, "memory-region-names",
				     "ba") >= 0) {
		cmd = WLAN_FUNC_SET_WAIT_DRAM_BA_NODE_ADDR;
		err = airoha_npu_wlan_set_reserved_memory(npu, 0, "ba", cmd);
		if (err)
			return err;
	}

	cmd = WLAN_FUNC_SET_WAIT_IS_FORCE_TO_CPU;
	err = airoha_npu_wlan_msg_send(npu, 0, cmd, &val, sizeof(val),
					GFP_KERNEL);
	if (err)
		dev_err(npu->dev, "error on send force_to_cpu command: %d", err);

	return err;
}

static u32 airoha_npu_wlan_queue_addr_get(struct airoha_npu *npu, int qid,
					  bool xmit)
{
	if (xmit)
		return REG_TX_BASE(qid + 2);

	return REG_RX_BASE(qid);
}

static void airoha_npu_wlan_irq_status_set(struct airoha_npu *npu, u32 val)
{
	regmap_write(npu->regmap, REG_IRQ_STATUS, val);
}

static u32 airoha_npu_wlan_irq_status_get(struct airoha_npu *npu, int q)
{
	u32 val;

	regmap_read(npu->regmap, REG_IRQ_STATUS, &val);
	return val;
}

static void airoha_npu_wlan_irq_enable(struct airoha_npu *npu, int q)
{
	regmap_set_bits(npu->regmap, REG_IRQ_RXDONE(q), NPU_IRQ_RX_MASK(q));
}

static void airoha_npu_wlan_irq_disable(struct airoha_npu *npu, int q)
{
	regmap_clear_bits(npu->regmap, REG_IRQ_RXDONE(q), NPU_IRQ_RX_MASK(q));
}

struct airoha_npu *airoha_npu_get(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct airoha_npu *npu;

	np = of_parse_phandle(dev->of_node, "airoha,npu", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		dev_err(dev, "cannot find device node %s\n", np->name);
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}
	of_node_put(np);

	if (!try_module_get(THIS_MODULE)) {
		dev_err(dev, "failed to get the device driver module\n");
		npu = ERR_PTR(-ENODEV);
		goto error_pdev_put;
	}

	npu = platform_get_drvdata(pdev);
	if (!npu) {
		npu = ERR_PTR(-ENODEV);
		goto error_module_put;
	}

	if (!device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_SUPPLIER)) {
		dev_err(&pdev->dev,
			"failed to create device link to consumer %s\n",
			dev_name(dev));
		npu = ERR_PTR(-EINVAL);
		goto error_module_put;
	}

	return npu;

error_module_put:
	module_put(THIS_MODULE);
error_pdev_put:
	platform_device_put(pdev);

	return npu;
}
EXPORT_SYMBOL_GPL(airoha_npu_get);

void airoha_npu_put(struct airoha_npu *npu)
{
	module_put(THIS_MODULE);
	put_device(npu->dev);
}
EXPORT_SYMBOL_GPL(airoha_npu_put);

static void airoha_boot_core_v1(struct airoha_npu *npu, struct reserved_mem *rmem) {
	u32 val = 0x1, boot = 0x1;

	/* setting booting address */
	for (int core = 0; core < npu->soc_data->max_cores; core++) {
		regmap_write(npu->regmap, REG_CR_BOOT_BASE(npu->soc_data, core), rmem->base);
		usleep_range(1000, 2000);

		if (core > 0) {
			boot = 0x2;
			regmap_read(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data), &val);
			val &= 0xff;          // keep old enabled bits
			val |= (0x1<<core);   // add new enable bit for CoreX
			val |= (0x100<<core); // add reboot bit for CoreX
		}

		regmap_write(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data), val);
		regmap_write(npu->regmap, REG_CR_BOOT_TRIGGER(npu->soc_data), boot);

		msleep(100);
	}
}

static void airoha_boot_core_v2(struct airoha_npu *npu, struct reserved_mem *rmem) {
	/* setting booting address */
	for (int i = 0; i < npu->soc_data->max_cores; i++)
		regmap_write(npu->regmap, REG_CR_BOOT_BASE(npu->soc_data, i), rmem->base);
	usleep_range(1000, 2000);

	/* enable NPU cores */
	regmap_write(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data), 0xff);
	regmap_write(npu->regmap, REG_CR_BOOT_TRIGGER(npu->soc_data), 0x1);
	msleep(100);
}

static bool is_fpga(struct regmap *scuclk)
{
	u32 isFPGA;
	regmap_read(scuclk, 0x9c, &isFPGA);
	return (isFPGA & 0x1) == 0;
}

static int sram_size_of_l2c(struct regmap *scuclk)
{
	u32 l2c_sram_size;
	regmap_read(scuclk, 0x280, &l2c_sram_size);
	return l2c_sram_size;
}

static const struct regmap_config regmap_config = {
	.name			= "npu",
	.reg_bits		= 32,
	.val_bits		= 32,
	.reg_stride		= 4,
	.disable_locking	= true,
};

static int airoha_npu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct airoha_npu *npu;
	struct device_node *np;
	void __iomem *base;
	int i, irq, err;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	npu = devm_kzalloc(dev, sizeof(*npu), GFP_KERNEL);
	if (!npu)
		return -ENOMEM;

	npu->soc_data = of_device_get_match_data(dev);
	if (!npu->soc_data)
		return -ENODEV;

	npu->dev = dev;
	npu->ops.ppe_init = airoha_npu_ppe_init;
	npu->ops.ppe_deinit = airoha_npu_ppe_deinit;
	npu->ops.ppe_init_stats = airoha_npu_ppe_stats_setup;
	npu->ops.ppe_flush_sram_entries = airoha_npu_ppe_flush_sram_entries;
	npu->ops.ppe_foe_commit_entry = airoha_npu_foe_commit_entry;
	npu->ops.wlan_init_reserved_memory = airoha_npu_wlan_init_memory;
	npu->ops.wlan_send_msg = airoha_npu_wlan_msg_send;
	npu->ops.wlan_get_msg = airoha_npu_wlan_msg_get;
	npu->ops.wlan_get_queue_addr = airoha_npu_wlan_queue_addr_get;
	npu->ops.wlan_set_irq_status = airoha_npu_wlan_irq_status_set;
	npu->ops.wlan_get_irq_status = airoha_npu_wlan_irq_status_get;
	npu->ops.wlan_enable_irq = airoha_npu_wlan_irq_enable;
	npu->ops.wlan_disable_irq = airoha_npu_wlan_irq_disable;

	npu->regmap = devm_regmap_init_mmio(dev, base, &regmap_config);
	if (IS_ERR(npu->regmap))
		return PTR_ERR(npu->regmap);

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);

	if (!rmem)
		return -ENODEV;

	np = of_parse_phandle(dev->of_node, "airoha,scu", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "cannot get scuclk");

	npu->scu_regmap = syscon_node_to_regmap(np);
	of_node_put(np);
	if (IS_ERR(npu->scu_regmap))
		return PTR_ERR(npu->scu_regmap);

	npu->cores = devm_kzalloc(&pdev->dev,
				npu->soc_data->max_cores * sizeof(*npu->cores),
				GFP_KERNEL);
	if (!npu->cores)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	// NPU mbox host irq
	err = devm_request_irq(dev, irq, airoha_npu_mbox_handler,
			       IRQF_SHARED, "airoha-npu-mbox", npu);
	if (err)
		return err;

	/* wlan IRQ lines */
	for (i = 0; i < ARRAY_SIZE(npu->irqs); i++) {
		irq = platform_get_irq(pdev, i + 1);
		if (irq < 0)
			return irq;

		npu->irqs[i] = irq;
	}

	if (npu->soc_data->version >= NPU_V2) {
		err = dma_set_coherent_mask(dev, 0xbfffffff);
		if (err)
			return dev_err_probe(dev, err, "No usable coherent DMA configuration");

		for (i = 0; i < npu->soc_data->max_cores; i++) {
			struct airoha_npu_core *core = &npu->cores[i];

			spin_lock_init(&core->lock);
			core->npu = npu;

			irq = platform_get_irq(pdev, i + npu->soc_data->max_cores + 1);
			if (irq < 0)
				return irq;

			err = devm_request_irq(dev, irq, airoha_npu_wdt_handler,
					       IRQF_SHARED, "airoha-npu-wdt", core);
			if (err)
				return err;

			INIT_WORK(&core->wdt_work, airoha_npu_wdt_work);
		}
	}

	err = airoha_npu_run_firmware(dev, base, rmem);
	if (err)
		return dev_err_probe(dev, err, "failed to run npu firmware\n");

	// set npu info needed
	regmap_write(npu->regmap, REG_CR_NPU_MIB(10),
		     ((unsigned int)rmem->base) + npu->soc_data->fw_rv32.max_size);
	regmap_write(npu->regmap, REG_CR_NPU_MIB(11),
		     sram_size_of_l2c(npu->scu_regmap));
	regmap_write(npu->regmap, REG_CR_NPU_MIB(12),
		     is_fpga(npu->scu_regmap));
	regmap_write(npu->regmap, REG_CR_NPU_MIB(21),
		     !of_property_present(dev->of_node, "airoha,enable_npu_tx_uart"));
	msleep(100);

	/* setting booting address and enable NPU cores */
	npu->soc_data->boot_core(npu, rmem);
	platform_set_drvdata(pdev, npu);

	// Get NPU Version
	int npu_version;
	err = airoha_npu_wlan_msg_get(npu, 0, WLAN_FUNC_GET_WAIT_NPU_VERSION, &npu_version, sizeof(npu_version), GFP_KERNEL);
	if (!err)
		dev_info(npu->dev, "Airoha NPU fw version: v%0d.%0d (0x%x)\n", (npu_version >> 16) & 0xffff, npu_version & 0xffff, npu_version);
	else if (err != -EOPNOTSUPP)
		dev_err(npu->dev, "cannot get NPU Version, error %d", err);

	return 0;
}

static void airoha_npu_remove(struct platform_device *pdev)
{
	struct airoha_npu *npu = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < npu->soc_data->max_cores; i++)
		cancel_work_sync(&npu->cores[i].wdt_work);
}

static const struct airoha_npu_soc_data en7523_npu_soc_data = {
	.version = NPU_V1,
	.max_cores = 4,
	.cluster_base_addr = 0x308000,
	.pc_base_addr = 0x308800,
	.boot_core = airoha_boot_core_v1,
	.fw_rv32 = {
		.name = NPU_EN7523_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_EN7523_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
	.wlan_func_get = {
		[WLAN_FUNC_GET_WAIT_NPU_INFO] = 0,
		[WLAN_FUNC_GET_WAIT_LAST_RATE] = 1,
		[WLAN_FUNC_GET_WAIT_COUNTER] = 2,
		[WLAN_FUNC_GET_WAIT_DBG_COUNTER] = 3,
		[WLAN_FUNC_GET_WAIT_RXDESC_BASE] = 4,
		[WLAN_FUNC_GET_WAIT_WCID_DBG_COUNTER] = 5,

		/* Old npu drive (v1) not support new features from v2 */
		[WLAN_FUNC_GET_WAIT_DMA_ADDR] = WLAN_FUNC_GET_WAIT_MAX,
		[WLAN_FUNC_GET_WAIT_RING_SIZE] = WLAN_FUNC_GET_WAIT_MAX,
		[WLAN_FUNC_GET_WAIT_NPU_SUPPORT_MAP] = WLAN_FUNC_GET_WAIT_MAX,
		[WLAN_FUNC_GET_WAIT_MDC_LOCK_ADDRESS] = WLAN_FUNC_GET_WAIT_MAX,
		[WLAN_FUNC_GET_WAIT_NPU_VERSION] = WLAN_FUNC_GET_WAIT_MAX,
	},
	.wlan_func_set = {
		[WLAN_FUNC_SET_WAIT_PCIE_ADDR] = 0,
		[WLAN_FUNC_SET_WAIT_DESC] = 1,
		[WLAN_FUNC_SET_WAIT_NPU_INIT_DONE] = 2,
		[WLAN_FUNC_SET_WAIT_TRAN_TO_CPU] = 3,
		[WLAN_FUNC_SET_WAIT_BA_WIN_SIZE] = 4,
		[WLAN_FUNC_SET_WAIT_DRIVER_MODEL] = 5,
		[WLAN_FUNC_SET_WAIT_DEL_STA] = 6,
		[WLAN_FUNC_SET_WAIT_DRAM_BA_NODE_ADDR] = 7,
		[WLAN_FUNC_SET_WAIT_PKT_BUF_ADDR] = 8,
		[WLAN_FUNC_SET_WAIT_IS_TEST_NOBA] = 9,
		[WLAN_FUNC_SET_WAIT_FLUSHONE_TIMEOUT] = 10,
		[WLAN_FUNC_SET_WAIT_FLUSHALL_TIMEOUT] = 11,
		[WLAN_FUNC_SET_WAIT_IS_FORCE_TO_CPU] = 12,
		[WLAN_FUNC_SET_WAIT_NPU_BAND0_ONCPU] = 12,
		[WLAN_FUNC_SET_WAIT_PCIE_STATE] = 13,
		[WLAN_FUNC_SET_WAIT_PCIE_PORT_TYPE] = 14,
		[WLAN_FUNC_SET_WAIT_ERROR_RETRY_TIMES] = 15,
		[WLAN_FUNC_SET_WAIT_BAR_INFO] = 16,
		[WLAN_FUNC_SET_WAIT_FAST_FLAG] = 17,

		/* Old npu drive (v1) not support new features from v2 */
		[WLAN_FUNC_SET_WAIT_TX_RING_PCIE_ADDR] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_TX_DESC_HW_BASE] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_TX_BUF_SPACE_HW_BASE] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_RX_RING_FOR_TXDONE_HW_BASE] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_TX_PKT_BUF_ADDR] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_INODE_TXRX_REG_ADDR] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_INODE_DEBUG_FLAG] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_INODE_HW_CFG_INFO] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_INODE_STOP_ACTION] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_INODE_PCIE_SWAP] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_RATELIMIT_CTRL] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_HWNAT_INIT] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_ARHT_CHIP_INFO] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_TX_BUF_CHECK_ADDR] = WLAN_FUNC_SET_WAIT_MAX,
		[WLAN_FUNC_SET_WAIT_TOKEN_ID_SIZE] = WLAN_FUNC_SET_WAIT_MAX,
	},
};

static const struct airoha_npu_soc_data en7581_npu_soc_data = {
	.version = NPU_V2,
	.max_cores = 8,
	.cluster_base_addr = 0x306000,
	.pc_base_addr = 0x305000,
	.boot_core = airoha_boot_core_v2,
	.fw_rv32 = {
		.name = NPU_EN7581_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_EN7581_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
	.wlan_func_get = {
		[WLAN_FUNC_GET_WAIT_NPU_INFO] = 0,
		[WLAN_FUNC_GET_WAIT_LAST_RATE] = 1,
		[WLAN_FUNC_GET_WAIT_COUNTER] = 2,
		[WLAN_FUNC_GET_WAIT_DBG_COUNTER] = 3,
		[WLAN_FUNC_GET_WAIT_RXDESC_BASE] = 4,
		[WLAN_FUNC_GET_WAIT_WCID_DBG_COUNTER] = 5,
		[WLAN_FUNC_GET_WAIT_DMA_ADDR] = 6,
		[WLAN_FUNC_GET_WAIT_RING_SIZE] = 7,
		[WLAN_FUNC_GET_WAIT_NPU_SUPPORT_MAP] = 8,
		[WLAN_FUNC_GET_WAIT_MDC_LOCK_ADDRESS] = 9,
		[WLAN_FUNC_GET_WAIT_NPU_VERSION] = 10,
	},
	.wlan_func_set = {
		[WLAN_FUNC_SET_WAIT_PCIE_ADDR] = 0,
		[WLAN_FUNC_SET_WAIT_DESC] = 1,
		[WLAN_FUNC_SET_WAIT_NPU_INIT_DONE] = 2,
		[WLAN_FUNC_SET_WAIT_TRAN_TO_CPU] = 3,
		[WLAN_FUNC_SET_WAIT_BA_WIN_SIZE] = 4,
		[WLAN_FUNC_SET_WAIT_DRIVER_MODEL] = 5,
		[WLAN_FUNC_SET_WAIT_DEL_STA] = 6,
		[WLAN_FUNC_SET_WAIT_DRAM_BA_NODE_ADDR] = 7,
		[WLAN_FUNC_SET_WAIT_PKT_BUF_ADDR] = 8,
		[WLAN_FUNC_SET_WAIT_IS_TEST_NOBA] = 9,
		[WLAN_FUNC_SET_WAIT_FLUSHONE_TIMEOUT] = 10,
		[WLAN_FUNC_SET_WAIT_FLUSHALL_TIMEOUT] = 11,
		[WLAN_FUNC_SET_WAIT_IS_FORCE_TO_CPU] = 12,
		[WLAN_FUNC_SET_WAIT_PCIE_STATE] = 13,
		[WLAN_FUNC_SET_WAIT_PCIE_PORT_TYPE] = 14,
		[WLAN_FUNC_SET_WAIT_ERROR_RETRY_TIMES] = 15,
		[WLAN_FUNC_SET_WAIT_BAR_INFO] = 16,
		[WLAN_FUNC_SET_WAIT_FAST_FLAG] = 17,
		[WLAN_FUNC_SET_WAIT_NPU_BAND0_ONCPU] = 18,
		[WLAN_FUNC_SET_WAIT_TX_RING_PCIE_ADDR] = 19,
		[WLAN_FUNC_SET_WAIT_TX_DESC_HW_BASE] = 20,
		[WLAN_FUNC_SET_WAIT_TX_BUF_SPACE_HW_BASE] = 21,
		[WLAN_FUNC_SET_WAIT_RX_RING_FOR_TXDONE_HW_BASE] = 22,
		[WLAN_FUNC_SET_WAIT_TX_PKT_BUF_ADDR] = 23,
		[WLAN_FUNC_SET_WAIT_INODE_TXRX_REG_ADDR] = 24,
		[WLAN_FUNC_SET_WAIT_INODE_DEBUG_FLAG] = 25,
		[WLAN_FUNC_SET_WAIT_INODE_HW_CFG_INFO] = 26,
		[WLAN_FUNC_SET_WAIT_INODE_STOP_ACTION] = 27,
		[WLAN_FUNC_SET_WAIT_INODE_PCIE_SWAP] = 28,
		[WLAN_FUNC_SET_WAIT_RATELIMIT_CTRL] = 29,
		[WLAN_FUNC_SET_WAIT_HWNAT_INIT] = 30,
		[WLAN_FUNC_SET_WAIT_ARHT_CHIP_INFO] = 31,
		[WLAN_FUNC_SET_WAIT_TX_BUF_CHECK_ADDR] = 32,
		[WLAN_FUNC_SET_WAIT_TOKEN_ID_SIZE] = 33,
	},
};

static const struct airoha_npu_soc_data an7583_npu_soc_data = {
	.version = NPU_V2,
	.max_cores = 8,
	.cluster_base_addr = 0x306000,
	.pc_base_addr = 0x305000,
	.boot_core = airoha_boot_core_v2,
	.fw_rv32 = {
		.name = NPU_AN7583_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_AN7583_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
	.wlan_func_get = {
		[WLAN_FUNC_GET_WAIT_NPU_INFO] = 0,
		[WLAN_FUNC_GET_WAIT_LAST_RATE] = 1,
		[WLAN_FUNC_GET_WAIT_COUNTER] = 2,
		[WLAN_FUNC_GET_WAIT_DBG_COUNTER] = 3,
		[WLAN_FUNC_GET_WAIT_RXDESC_BASE] = 4,
		[WLAN_FUNC_GET_WAIT_WCID_DBG_COUNTER] = 5,
		[WLAN_FUNC_GET_WAIT_DMA_ADDR] = 6,
		[WLAN_FUNC_GET_WAIT_RING_SIZE] = 7,
		[WLAN_FUNC_GET_WAIT_NPU_SUPPORT_MAP] = 8,
		[WLAN_FUNC_GET_WAIT_MDC_LOCK_ADDRESS] = 9,
		[WLAN_FUNC_GET_WAIT_NPU_VERSION] = 10,
	},
	.wlan_func_set = {
		[WLAN_FUNC_SET_WAIT_PCIE_ADDR] = 0,
		[WLAN_FUNC_SET_WAIT_DESC] = 1,
		[WLAN_FUNC_SET_WAIT_NPU_INIT_DONE] = 2,
		[WLAN_FUNC_SET_WAIT_TRAN_TO_CPU] = 3,
		[WLAN_FUNC_SET_WAIT_BA_WIN_SIZE] = 4,
		[WLAN_FUNC_SET_WAIT_DRIVER_MODEL] = 5,
		[WLAN_FUNC_SET_WAIT_DEL_STA] = 6,
		[WLAN_FUNC_SET_WAIT_DRAM_BA_NODE_ADDR] = 7,
		[WLAN_FUNC_SET_WAIT_PKT_BUF_ADDR] = 8,
		[WLAN_FUNC_SET_WAIT_IS_TEST_NOBA] = 9,
		[WLAN_FUNC_SET_WAIT_FLUSHONE_TIMEOUT] = 10,
		[WLAN_FUNC_SET_WAIT_FLUSHALL_TIMEOUT] = 11,
		[WLAN_FUNC_SET_WAIT_IS_FORCE_TO_CPU] = 12,
		[WLAN_FUNC_SET_WAIT_PCIE_STATE] = 13,
		[WLAN_FUNC_SET_WAIT_PCIE_PORT_TYPE] = 14,
		[WLAN_FUNC_SET_WAIT_ERROR_RETRY_TIMES] = 15,
		[WLAN_FUNC_SET_WAIT_BAR_INFO] = 16,
		[WLAN_FUNC_SET_WAIT_FAST_FLAG] = 17,
		[WLAN_FUNC_SET_WAIT_NPU_BAND0_ONCPU] = 18,
		[WLAN_FUNC_SET_WAIT_TX_RING_PCIE_ADDR] = 19,
		[WLAN_FUNC_SET_WAIT_TX_DESC_HW_BASE] = 20,
		[WLAN_FUNC_SET_WAIT_TX_BUF_SPACE_HW_BASE] = 21,
		[WLAN_FUNC_SET_WAIT_RX_RING_FOR_TXDONE_HW_BASE] = 22,
		[WLAN_FUNC_SET_WAIT_TX_PKT_BUF_ADDR] = 23,
		[WLAN_FUNC_SET_WAIT_INODE_TXRX_REG_ADDR] = 24,
		[WLAN_FUNC_SET_WAIT_INODE_DEBUG_FLAG] = 25,
		[WLAN_FUNC_SET_WAIT_INODE_HW_CFG_INFO] = 26,
		[WLAN_FUNC_SET_WAIT_INODE_STOP_ACTION] = 27,
		[WLAN_FUNC_SET_WAIT_INODE_PCIE_SWAP] = 28,
		[WLAN_FUNC_SET_WAIT_RATELIMIT_CTRL] = 29,
		[WLAN_FUNC_SET_WAIT_HWNAT_INIT] = 30,
		[WLAN_FUNC_SET_WAIT_ARHT_CHIP_INFO] = 31,
		[WLAN_FUNC_SET_WAIT_TX_BUF_CHECK_ADDR] = 32,
		[WLAN_FUNC_SET_WAIT_TOKEN_ID_SIZE] = 33,
	},
};

static const struct of_device_id of_airoha_npu_match[] = {
	{ .compatible = "airoha,en7523-npu", .data = &en7523_npu_soc_data },
	{ .compatible = "airoha,en7581-npu", .data = &en7581_npu_soc_data },
	{ .compatible = "airoha,an7583-npu", .data = &an7583_npu_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_airoha_npu_match);

static struct platform_driver airoha_npu_driver = {
	.probe = airoha_npu_probe,
	.remove = airoha_npu_remove,
	.driver = {
		.name = "airoha-npu",
		.of_match_table = of_airoha_npu_match,
	},
};
module_platform_driver(airoha_npu_driver);

MODULE_FIRMWARE(NPU_EN7523_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_EN7523_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_EN7581_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_EN7581_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_EN7581_7996_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_EN7581_7996_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_AN7583_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_AN7583_FIRMWARE_RV32);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lorenzo Bianconi <lorenzo@kernel.org>");
MODULE_DESCRIPTION("Airoha Network Processor Unit driver");
