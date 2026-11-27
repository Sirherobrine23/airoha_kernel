// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/devcoredump.h>
#include <linux/devm-helpers.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/mfd/syscon.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

#include "airoha_eth.h"

#define NPU_EN7523_FIRMWARE_DATA		"airoha/en7523_npu_data.bin"
#define NPU_EN7523_FIRMWARE_RV32		"airoha/en7523_npu_rv32.bin"
#define NPU_AN7552_FIRMWARE_DATA		"airoha/an7552_npu_data.bin"
#define NPU_AN7552_FIRMWARE_RV32		"airoha/an7552_npu_rv32.bin"
#define NPU_EN7581_FIRMWARE_DATA		"airoha/en7581_npu_data.bin"
#define NPU_EN7581_FIRMWARE_RV32		"airoha/en7581_npu_rv32.bin"
#define NPU_EN7581_7996_FIRMWARE_DATA		"airoha/en7581_MT7996_npu_data.bin"
#define NPU_EN7581_7996_FIRMWARE_RV32		"airoha/en7581_MT7996_npu_rv32.bin"
#define NPU_AN7583_FIRMWARE_DATA		"airoha/an7583_npu_data.bin"
#define NPU_AN7583_FIRMWARE_RV32		"airoha/an7583_npu_rv32.bin"
#define NPU_MBOX_INTNAME			"mbox"
#define NPU_WATCHDOG_INTNAME			"wdt%d"
#define NPU_WLAN_INTNAME			"wlan%d"
#define NPU_EN7581_FIRMWARE_RV32_MAX_SIZE	0x200000
#define NPU_EN7581_FIRMWARE_DATA_MAX_SIZE	0x10000
#define NPU_DUMP_SIZE				512
#define NPU_FIRMWARE_RETRY_MS			1000
#define NPU_V1_BOOT_READY			0xcccccccc
#define NPU_V1_BOOT_POLL_US			10000
#define NPU_V1_BOOT_TIMEOUT_US			(40 * USEC_PER_SEC)
/*
 * Mailbox DMA payload size. Covers sizeof(struct ppe_mbox_data) (28) and
 * WLAN TLV messages (header + payload). Keep headroom for future commands.
 */
#define AIROHA_NPU_MBOX_SIZE			256

#define REG_NPU_LOCAL_SRAM		0x0

#define V1_NPU_CLUSTER_BASE		0x308000
#define V2_NPU_CLUSTER_BASE		0x306000
#define V1_CR_PC_BASE_ADDR		(V1_NPU_CLUSTER_BASE + 0x800)
#define V2_CR_PC_BASE_ADDR		0x305000

#define NPU_CLUSTER_BASE(_soc)		((_soc)->version == NPU_V1 ? \
					 V1_NPU_CLUSTER_BASE : \
					 V2_NPU_CLUSTER_BASE)
#define CR_PC_BASE_ADDR(_soc)		((_soc)->version == NPU_V1 ? \
					 V1_CR_PC_BASE_ADDR : \
					 V2_CR_PC_BASE_ADDR)

#define REG_PC_DBG(_soc, _n)		(CR_PC_BASE_ADDR(_soc) + ((_n) * 0x100))
#define REG_CR_BOOT_TRIGGER(_soc)	(NPU_CLUSTER_BASE(_soc) + 0x000)
#define REG_CR_BOOT_CONFIG(_soc)	(NPU_CLUSTER_BASE(_soc) + 0x004)
#define REG_CR_BOOT_BASE(_soc, _n)	(NPU_CLUSTER_BASE(_soc) + 0x020 + ((_n) << 2))

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

#define AN7552_WDT_TIMER_EN_MASK	0x1e0001ef

#define EN7523_NPU_CORES		4
#define AN7552_NPU_CORES		2
#define AN7581_NPU_CORES		8
#define AN7583_NPU_CORES		8

#define STATIC_L2C_SRAM_SIZE(_soc)	((_soc)->version == NPU_V2 ? \
					 SZ_256K : SZ_128K)

#define VERSION_MAJOR(_ver)		(((_ver) >> 22) & 0xff)
#define VERSION_MINOR(_ver)		(((_ver) >> 12) & 0xff)
#define VERSION_PATCH(_ver)		((_ver) & 0xff)

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
	NPU_MBOX_EINVAL,
	NPU_MBOX_EOPNOTSUPP,
	NPU_MBOX_ENODEV,
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
	unsigned int num_cores;
	u32 mbox_func_mask;
	bool shared_wdt;
	bool have_wdt;
	bool require_scu;

	struct airoha_npu_fw fw_rv32;
	struct airoha_npu_fw fw_data;

	int (*boot_core)(struct airoha_npu *npu, struct resource *res);
};

struct airoha_npu_priv {
	struct airoha_npu npu;
	struct resource fw_res;
	void __iomem *base;
	void __iomem *fw_addr;
	/* Serialize the one-time firmware load and core startup. */
	struct mutex start_lock;
	struct delayed_work start_work;
	bool started;
};

static struct airoha_npu_priv *airoha_npu_to_priv(struct airoha_npu *npu)
{
	return container_of(npu, struct airoha_npu_priv, npu);
}

static u32 airoha_npu_wdt_ctrl(struct airoha_npu *npu, int core)
{
	return REG_WDT_TIMER_CTRL(npu->soc_data->shared_wdt ? core / 4 : core);
}

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
	__le32 op;
	__le32 func_id;
	DECLARE_FLEX_ARRAY(u8, d);
};

static int airoha_npu_send_msg_locked(struct airoha_npu *npu, int func_id,
				      const void *data, int len, void *reply,
				      u16 reply_len)
{
	struct airoha_npu_core *core = &npu->cores[0]; /* FIXME */
	u32 val, offset = 0;
	int ret;

	if (!data || len <= 0 || len > AIROHA_NPU_MBOX_SIZE ||
	    func_id < 0 || func_id > FIELD_MAX(MBOX_MSG_FUNC_ID))
		return -EINVAL;

	if (reply && reply_len > len)
		return -EINVAL;

	if (npu->soc_data->mbox_func_mask &&
	    !(npu->soc_data->mbox_func_mask & BIT(func_id)))
		return -EOPNOTSUPP;

	if (!core->buf)
		return -ENODEV;

	lockdep_assert_held(&core->lock);

	memcpy(core->buf, data, len);
	dma_wmb();

	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(0) + offset, core->addr);
	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(1) + offset, len);
	val = FIELD_PREP(MBOX_MSG_FUNC_ID, func_id) | MBOX_MSG_WAIT_RSP;
	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(3) + offset, val);
	/* CTRL2 raises the IRQ after the complete request is visible. */
	wmb();
	regmap_read(npu->regmap, REG_CR_MBQ0_CTRL(2) + offset, &val);
	regmap_write(npu->regmap, REG_CR_MBQ0_CTRL(2) + offset, val + 1);

	ret = regmap_read_poll_timeout_atomic(npu->regmap,
					      REG_CR_MBQ0_CTRL(3) + offset,
					      val, (val & MBOX_MSG_DONE),
					      100, 100 * MSEC_PER_SEC);
	if (!ret && FIELD_GET(MBOX_MSG_STATUS, val) != NPU_MBOX_SUCCESS) {
		switch (FIELD_GET(MBOX_MSG_STATUS, val)) {
		case NPU_MBOX_EOPNOTSUPP:
			ret = -EOPNOTSUPP;
			break;
		case NPU_MBOX_ENODEV:
			ret = -ENODEV;
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	if (!ret && reply)
		memcpy(reply, core->buf + len - reply_len, reply_len);

	return ret;
}

static int __airoha_npu_send_msg(struct airoha_npu *npu, int func_id,
				 const void *data, int len, void *reply,
				 u16 reply_len)
{
	struct airoha_npu_core *core = &npu->cores[0];
	int ret;

	spin_lock_bh(&core->lock);
	ret = airoha_npu_send_msg_locked(npu, func_id, data, len,
					 reply, reply_len);
	spin_unlock_bh(&core->lock);

	return ret;
}

static int airoha_npu_send_msg(struct airoha_npu *npu, int func_id,
			       void *data, int len)
{
	return __airoha_npu_send_msg(npu, func_id, data, len, data, len);
}

static int airoha_npu_load_firmware(struct device *dev, void __iomem *addr,
				    const char *fw_name, int fw_max_size)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware_direct(&fw, fw_name, dev);
	if (ret)
		return dev_err_probe(dev, ret == -ENOENT ? -EPROBE_DEFER : ret,
				     "failed to load %s\n", fw_name);

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
		return dev_err_probe(dev, -EINVAL,
				     "invalid firmware-name property\n");

	ret = airoha_npu_load_firmware(dev, addr, fw_names[0],
				       NPU_EN7581_FIRMWARE_RV32_MAX_SIZE);
	if (ret)
		return ret;

	return airoha_npu_load_firmware(dev, base + REG_NPU_LOCAL_SRAM,
					fw_names[1],
					NPU_EN7581_FIRMWARE_DATA_MAX_SIZE);
}

static int airoha_npu_run_firmware(struct device *dev, void __iomem *base,
				   void __iomem *fw_addr)
{
	const struct airoha_npu_soc_data *soc;
	int ret;

	soc = of_device_get_match_data(dev);
	if (!soc)
		return -EINVAL;

	/* Try to load firmware images using the firmware names provided via
	 * dts if available.
	 */
	if (of_find_property(dev->of_node, "firmware-name", NULL))
		return airoha_npu_load_firmware_from_dts(dev, fw_addr, base);

	/* Load rv32 npu firmware */
	ret = airoha_npu_load_firmware(dev, fw_addr, soc->fw_rv32.name,
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
	regmap_bulk_read(npu->regmap, REG_PC_DBG(npu->soc_data, c),
			 val, ARRAY_SIZE(val));
	snprintf(dump, NPU_DUMP_SIZE, "PC: %08x SP: %08x LR: %08x\n",
		 val[0], val[1], val[2]);

	dev_coredumpv(npu->dev, dump, NPU_DUMP_SIZE, GFP_KERNEL);
}

static irqreturn_t airoha_npu_wdt_handler(int irq, void *core_instance)
{
	struct airoha_npu_core *core = core_instance;
	struct airoha_npu *npu = core->npu;
	int c = core - &npu->cores[0];
	u32 ctrl = airoha_npu_wdt_ctrl(npu, c);
	u32 intr = WDT_INTR_MASK, enable = WDT_EN_MASK;
	u32 val;

	if (npu->soc_data->shared_wdt) {
		intr <<= c % 4;
		enable <<= c % 4;
		if (regmap_read(npu->regmap, ctrl, &val) || !(val & intr))
			return IRQ_NONE;

		/* Status bits are W1C: acknowledge only this core and retain
		 * the timer enable bits, including the other watchdogs.
		 */
		regmap_write(npu->regmap, ctrl,
			     (val & AN7552_WDT_TIMER_EN_MASK) | intr);
	} else {
		regmap_set_bits(npu->regmap, ctrl, intr);
		if (regmap_read(npu->regmap, ctrl, &val))
			return IRQ_HANDLED;
	}

	if (val & enable)
		schedule_work(&core->wdt_work);

	return IRQ_HANDLED;
}

static int airoha_npu_ppe_init(struct airoha_npu *npu)
{
	struct ppe_mbox_data *ppe_data;
	int err;

	ppe_data = kzalloc_obj(*ppe_data);
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

	ppe_data = kzalloc_obj(*ppe_data);
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

	ppe_data = kzalloc_obj(*ppe_data);
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
	struct airoha_npu_core *core = &npu->cores[0];
	struct ppe_mbox_data msg = {};
	int err;

	if (upper_32_bits(foe_addr) || entry_size != PPE_ENTRY_SIZE)
		return -EINVAL;

	msg.func_type = NPU_OP_SET;
	msg.func_id = PPE_FUNC_SET_WAIT_API;
	msg.set_info.data = lower_32_bits(foe_addr);
	msg.set_info.size = entry_size;
	msg.set_info.func_id = ppe2 ? PPE2_SRAM_SET_ENTRY : PPE_SRAM_SET_ENTRY;

	/* Keep stage and hash commit atomic on the shared mailbox. */
	spin_lock_bh(&core->lock);
	err = airoha_npu_send_msg_locked(npu, NPU_FUNC_PPE, &msg,
					 sizeof(msg), NULL, 0);
	if (err)
		goto out;

	msg.set_info.func_id = PPE_SRAM_SET_VAL;
	msg.set_info.data = hash;
	msg.set_info.size = sizeof(u32);

	err = airoha_npu_send_msg_locked(npu, NPU_FUNC_PPE, &msg,
					 sizeof(msg), NULL, 0);
out:
	spin_unlock_bh(&core->lock);

	return err;
}

static int airoha_npu_ppe_stats_setup(struct airoha_npu *npu,
				      dma_addr_t foe_stats_addr,
				      u32 num_stats_entries)
{
	int err, size = num_stats_entries * sizeof(*npu->stats);
	struct ppe_mbox_data *ppe_data;

	ppe_data = kzalloc_obj(*ppe_data, GFP_ATOMIC);
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

	if ((unsigned int)func_id >= WLAN_FUNC_SET_WAIT_MAX ||
	    ifindex < 0 || ifindex > 0xf || !data ||
	    data_len < sizeof(u32) ||
	    data_len > AIROHA_NPU_MBOX_SIZE - sizeof(*wlan_data))
		return -EINVAL;

	len = sizeof(*wlan_data) + data_len;
	wlan_data = kzalloc(len, gfp);
	if (!wlan_data)
		return -ENOMEM;

	wlan_data->op = cpu_to_le32(ifindex | (NPU_OP_SET << 4));
	wlan_data->func_id = cpu_to_le32(func_id);
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

	if ((unsigned int)func_id >= WLAN_FUNC_GET_WAIT_MAX ||
	    ifindex < 0 || ifindex > 0xf || !data ||
	    data_len < sizeof(u32) ||
	    data_len > AIROHA_NPU_MBOX_SIZE - sizeof(*wlan_data))
		return -EINVAL;

	len = sizeof(*wlan_data) + data_len;
	wlan_data = kzalloc(len, gfp);
	if (!wlan_data)
		return -ENOMEM;

	wlan_data->op = cpu_to_le32(ifindex | (NPU_OP_GET << 4));
	wlan_data->func_id = cpu_to_le32(func_id);

	err = __airoha_npu_send_msg(npu, NPU_FUNC_WIFI, wlan_data, len,
				    data, data_len);
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
	if (err && err != -EOPNOTSUPP)
		return err;

	if (of_property_match_string(npu->dev->of_node, "memory-region-names",
				     "tx-bufid") >= 0) {
		cmd = WLAN_FUNC_SET_WAIT_TX_BUF_CHECK_ADDR;
		err = airoha_npu_wlan_set_reserved_memory(npu, 0, "tx-bufid",
							  cmd);
		if (err)
			return err;
	}

	cmd = WLAN_FUNC_SET_WAIT_PKT_BUF_ADDR;
	err = airoha_npu_wlan_set_reserved_memory(npu, 0, "pkt", cmd);
	if (err)
		return err;

	if (of_property_match_string(npu->dev->of_node, "memory-region-names",
				     "tx-pkt") >= 0) {
		cmd = WLAN_FUNC_SET_WAIT_TX_PKT_BUF_ADDR;
		err = airoha_npu_wlan_set_reserved_memory(npu, 0, "tx-pkt",
							  cmd);
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
	return airoha_npu_wlan_msg_send(npu, 0, cmd, &val, sizeof(val),
					GFP_KERNEL);
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

	if (!of_device_is_available(np)) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

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

	device_lock(&pdev->dev);
	npu = platform_get_drvdata(pdev);
	if (!npu) {
		npu = ERR_PTR(-EPROBE_DEFER);
		goto error_module_put;
	}

	/* Pairs with the release after the NPU cores have been booted. */
	if (!smp_load_acquire(&airoha_npu_to_priv(npu)->started)) {
		npu = ERR_PTR(-EPROBE_DEFER);
		goto error_module_put;
	}

	if (!device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_SUPPLIER)) {
		dev_err(&pdev->dev,
			"failed to create device link to consumer %s\n",
			dev_name(dev));
		npu = ERR_PTR(-EINVAL);
		goto error_module_put;
	}

	device_unlock(&pdev->dev);
	return npu;

error_module_put:
	device_unlock(&pdev->dev);
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

static int airoha_npu_boot_core_v1(struct airoha_npu *npu,
				   struct resource *res)
{
	u32 val = 0x1, boot = 0x1, ready = 0;
	int core, ret;

	/* Do not accept a ready marker left by a previous firmware instance. */
	ret = regmap_write(npu->regmap, REG_CR_NPU_MIB(31), 0);
	if (ret)
		return ret;

	for (core = 0; core < npu->soc_data->num_cores; core++) {
		ret = regmap_write(npu->regmap,
				   REG_CR_BOOT_BASE(npu->soc_data, core),
				   res->start);
		if (ret)
			return ret;
		usleep_range(1000, 2000);

		if (core > 0) {
			boot = 0x2;
			ret = regmap_read(npu->regmap,
					  REG_CR_BOOT_CONFIG(npu->soc_data),
					  &val);
			if (ret)
				return ret;
			val &= 0xff;
			val |= BIT(core);
			val |= BIT(core + 8);
		}

		ret = regmap_write(npu->regmap,
				   REG_CR_BOOT_CONFIG(npu->soc_data), val);
		if (ret)
			return ret;
		ret = regmap_write(npu->regmap,
				   REG_CR_BOOT_TRIGGER(npu->soc_data), boot);
		if (ret)
			return ret;

		msleep(100);
	}

	/* MIB31 is a global firmware-init marker, not per-core ack. */
	ret = regmap_read_poll_timeout(npu->regmap, REG_CR_NPU_MIB(31), ready,
				       ready == NPU_V1_BOOT_READY,
				       NPU_V1_BOOT_POLL_US,
				       NPU_V1_BOOT_TIMEOUT_US);
	if (ret) {
		if (ret == -ETIMEDOUT)
			dev_err(npu->dev,
				"NPU initialization timed out (MIB31=%#010x)\n",
				ready);
		else
			dev_err(npu->dev, "failed to read NPU ready marker: %d\n",
				ret);
		return ret;
	}

	return 0;
}

static int airoha_npu_boot_core_v2(struct airoha_npu *npu,
				   struct resource *res)
{
	int i;

	/* setting booting address */
	for (i = 0; i < npu->soc_data->num_cores; i++)
		regmap_write(npu->regmap, REG_CR_BOOT_BASE(npu->soc_data, i),
			     res->start);
	usleep_range(1000, 2000);

	/* enable NPU cores */
	regmap_write(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data),
		     GENMASK(npu->soc_data->num_cores - 1, 0));
	regmap_write(npu->regmap, REG_CR_BOOT_TRIGGER(npu->soc_data), 0x1);
	msleep(100);

	return 0;
}

static bool airoha_npu_is_fpga(struct airoha_npu *npu)
{
	u32 val;

	if (!npu->scu_regmap)
		return false;

	regmap_read(npu->scu_regmap, 0x9c, &val);
	return !(val & BIT(0));
}

static u32 airoha_npu_l2c_sram_size(struct airoha_npu *npu)
{
	u32 val;

	if (!npu->scu_regmap)
		return STATIC_L2C_SRAM_SIZE(npu->soc_data);

	regmap_read(npu->scu_regmap, 0x280, &val);
	return val;
}

static int airoha_npu_get_irq(struct platform_device *pdev, const char *name,
			      int fallback_idx)
{
	int irq;

	irq = platform_get_irq_byname_optional(pdev, name);
	if (irq != -ENXIO)
		return irq;

	return platform_get_irq(pdev, fallback_idx);
}

static int airoha_npu_probe_cpu_cores(struct airoha_npu *npu)
{
	struct airoha_npu_priv *priv = airoha_npu_to_priv(npu);
	u32 npu_version = 0;
	int err;

	mutex_lock(&priv->start_lock);
	if (priv->started) {
		err = 0;
		goto out_unlock;
	}

	err = airoha_npu_run_firmware(npu->dev, priv->base, priv->fw_addr);
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(npu->dev, "failed to run npu firmware: %d\n",
				err);
		goto out_unlock;
	}

	regmap_write(npu->regmap, REG_CR_NPU_MIB(10),
		     (u32)priv->fw_res.start + npu->soc_data->fw_rv32.max_size);
	regmap_write(npu->regmap, REG_CR_NPU_MIB(11),
		     airoha_npu_l2c_sram_size(npu));
	regmap_write(npu->regmap, REG_CR_NPU_MIB(12),
		     airoha_npu_is_fpga(npu));
	regmap_write(npu->regmap, REG_CR_NPU_MIB(21),
		     !of_property_present(npu->dev->of_node,
					  "airoha,enable_npu_tx_uart"));
	msleep(100);

	err = npu->soc_data->boot_core(npu, &priv->fw_res);
	if (err) {
		dev_err(npu->dev, "failed to boot NPU cores: %d\n", err);
		goto out_unlock;
	}

	err = airoha_npu_wlan_msg_get(npu, 0, WLAN_FUNC_GET_WAIT_NPU_VERSION,
				      &npu_version, sizeof(npu_version),
				      GFP_KERNEL);
	if (!err)
		dev_info(npu->dev, "NPU fw version: v%u.%u.%u\n",
			 VERSION_MAJOR(npu_version),
			 VERSION_MINOR(npu_version),
			 VERSION_PATCH(npu_version));
	else if (err != -EOPNOTSUPP)
		dev_err(npu->dev, "cannot get NPU version: %d\n", err);

	/* Pairs with smp_load_acquire() in airoha_npu_get(). */
	smp_store_release(&priv->started, true);
	err = 0;

out_unlock:
	mutex_unlock(&priv->start_lock);

	return err;
}

static void airoha_npu_firmware_work(struct work_struct *work)
{
	struct airoha_npu_priv *priv;
	int err;

	priv = container_of(to_delayed_work(work), struct airoha_npu_priv,
			    start_work);
	err = airoha_npu_probe_cpu_cores(&priv->npu);
	if (err == -EPROBE_DEFER) {
		mod_delayed_work(system_wq, &priv->start_work,
				 msecs_to_jiffies(NPU_FIRMWARE_RETRY_MS));
		return;
	}

	if (err)
		dev_err(priv->npu.dev, "failed to start NPU: %d\n", err);
}

static const struct airoha_npu_soc_data en7523_npu_soc_data = {
	.version = NPU_V1,
	.num_cores = EN7523_NPU_CORES,
	.have_wdt = false,
	.require_scu = true,
	.boot_core = airoha_npu_boot_core_v1,
	.fw_rv32 = {
		.name = NPU_EN7523_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_EN7523_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
};

static const struct airoha_npu_soc_data an7552_npu_soc_data = {
	.version = NPU_V2_S,
	.num_cores = AN7552_NPU_CORES,
	.mbox_func_mask = GENMASK(NPU_FUNC_NOTIFY, NPU_FUNC_WIFI),
	.shared_wdt = true,
	.have_wdt = true,
	.require_scu = true,
	.boot_core = airoha_npu_boot_core_v2,
	.fw_rv32 = {
		.name = NPU_AN7552_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_AN7552_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
};

static const struct airoha_npu_soc_data en7581_npu_soc_data = {
	.version = NPU_V2,
	.num_cores = AN7581_NPU_CORES,
	.have_wdt = true,
	.boot_core = airoha_npu_boot_core_v2,
	.fw_rv32 = {
		.name = NPU_EN7581_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_EN7581_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
};

static const struct airoha_npu_soc_data an7583_npu_soc_data = {
	.version = NPU_V2_P,
	.num_cores = AN7583_NPU_CORES,
	.have_wdt = true,
	.boot_core = airoha_npu_boot_core_v2,
	.fw_rv32 = {
		.name = NPU_AN7583_FIRMWARE_RV32,
		.max_size = NPU_EN7581_FIRMWARE_RV32_MAX_SIZE,
	},
	.fw_data = {
		.name = NPU_AN7583_FIRMWARE_DATA,
		.max_size = NPU_EN7581_FIRMWARE_DATA_MAX_SIZE,
	},
};

static const struct of_device_id of_airoha_npu_match[] = {
	{ .compatible = "airoha,en7523-npu", .data = &en7523_npu_soc_data },
	{ .compatible = "airoha,an7552-npu", .data = &an7552_npu_soc_data },
	{ .compatible = "airoha,en7581-npu", .data = &en7581_npu_soc_data },
	{ .compatible = "airoha,an7583-npu", .data = &an7583_npu_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_airoha_npu_match);

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
	struct airoha_npu_priv *priv;
	struct device_node *np;
	struct airoha_npu *npu;
	void __iomem *base;
	int i, irq, err;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv = devm_kzalloc_obj(dev, *priv);
	if (!priv)
		return -ENOMEM;

	npu = &priv->npu;
	priv->base = base;
	mutex_init(&priv->start_lock);

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

	err = of_reserved_mem_region_to_resource(dev->of_node, 0,
						 &priv->fw_res);
	if (err)
		return err;

	priv->fw_addr = devm_ioremap_resource(dev, &priv->fw_res);
	if (IS_ERR(priv->fw_addr))
		return PTR_ERR(priv->fw_addr);

	np = of_parse_phandle(dev->of_node, "airoha,scu", 0);
	if (np) {
		npu->scu_regmap = syscon_node_to_regmap(np);
		of_node_put(np);
		if (IS_ERR(npu->scu_regmap))
			return PTR_ERR(npu->scu_regmap);
	} else if (npu->soc_data->require_scu) {
		return dev_err_probe(dev, -EINVAL, "missing airoha,scu\n");
	}

	npu->cores = devm_kzalloc_objs(dev, *npu->cores,
				       npu->soc_data->num_cores);
	if (!npu->cores)
		return -ENOMEM;

	irq = airoha_npu_get_irq(pdev, NPU_MBOX_INTNAME, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get mbox irq\n");

	err = devm_request_irq(dev, irq, airoha_npu_mbox_handler,
			       IRQF_SHARED, "airoha-npu-mbox", npu);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		return err;

	for (i = 0; i < npu->soc_data->num_cores; i++) {
		struct airoha_npu_core *core = &npu->cores[i];

		spin_lock_init(&core->lock);
		core->npu = npu;

		core->buf = dmam_alloc_coherent(dev, AIROHA_NPU_MBOX_SIZE,
						&core->addr, GFP_KERNEL);
		if (!core->buf)
			return -ENOMEM;

		err = devm_work_autocancel(dev, &core->wdt_work,
					   airoha_npu_wdt_work);
		if (err)
			return err;

		if (npu->soc_data->have_wdt) {
			char watchdog_name[16];

			snprintf(watchdog_name, sizeof(watchdog_name),
				 NPU_WATCHDOG_INTNAME, i);
			irq = airoha_npu_get_irq(pdev, watchdog_name, i + 1);
			if (irq < 0)
				return dev_err_probe(dev, irq,
						     "failed to get %s irq\n",
						     watchdog_name);

			err = devm_request_irq(dev, irq, airoha_npu_wdt_handler,
					       IRQF_SHARED, "airoha-npu-wdt",
					       core);
			if (err)
				return err;
		}
	}

	/* wlan IRQ lines */
	for (i = 0; i < ARRAY_SIZE(npu->irqs); i++) {
		char wlan_name[16];
		int fallback_idx = i + 1 + (npu->soc_data->have_wdt ?
					    npu->soc_data->num_cores : 0);

		snprintf(wlan_name, sizeof(wlan_name), NPU_WLAN_INTNAME, i);
		irq = airoha_npu_get_irq(pdev, wlan_name, fallback_idx);
		if (irq < 0)
			return dev_err_probe(dev, irq, "failed to get %s irq\n",
					     wlan_name);

		npu->irqs[i] = irq;
	}

	err = devm_delayed_work_autocancel(dev, &priv->start_work,
					   airoha_npu_firmware_work);
	if (err)
		return err;

	platform_set_drvdata(pdev, npu);
	err = airoha_npu_probe_cpu_cores(npu);
	if (err == -EPROBE_DEFER) {
		mod_delayed_work(system_wq, &priv->start_work,
				 msecs_to_jiffies(NPU_FIRMWARE_RETRY_MS));
		dev_info(dev,
			 "firmware unavailable; NPU startup will be retried\n");
		return 0;
	}

	return err;
}

static struct platform_driver airoha_npu_driver = {
	.probe = airoha_npu_probe,
	.driver = {
		.name = "airoha-npu",
		.of_match_table = of_airoha_npu_match,
	},
};
module_platform_driver(airoha_npu_driver);

MODULE_FIRMWARE(NPU_EN7523_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_EN7523_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_AN7552_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_AN7552_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_EN7581_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_EN7581_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_EN7581_7996_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_EN7581_7996_FIRMWARE_RV32);
MODULE_FIRMWARE(NPU_AN7583_FIRMWARE_DATA);
MODULE_FIRMWARE(NPU_AN7583_FIRMWARE_RV32);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lorenzo Bianconi <lorenzo@kernel.org>");
MODULE_DESCRIPTION("Airoha Network Processor Unit driver");
