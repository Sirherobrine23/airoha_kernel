// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include <linux/devcoredump.h>
#include <linux/devm-helpers.h>
#include <linux/firmware.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/xarray.h>

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
/*
 * Mailbox DMA payload size. Covers sizeof(struct ppe_mbox_data) (28) and
 * WLAN TLV messages (header + payload); largest in-tree WLAN payload today
 * is 16 bytes (INODE_TXRX_REG_ADDR). Keep headroom for future commands.
 */
#define AIROHA_NPU_MBOX_SIZE			256

#define REG_NPU_LOCAL_SRAM		0x0

#define V1_NPU_CLUSTER_BASE		0x308000
#define V2_NPU_CLUSTER_BASE		0x306000
#define V1_CR_PC_BASE_ADDR		(V1_NPU_CLUSTER_BASE + 0x800)
#define V2_CR_PC_BASE_ADDR		0x305000

#define NPU_CLUSTER_BASE(_SOC)		((_SOC)->version == NPU_V1 ? V1_NPU_CLUSTER_BASE : V2_NPU_CLUSTER_BASE)
#define CR_PC_BASE_ADDR(_SOC)		((_SOC)->version == NPU_V1 ? V1_CR_PC_BASE_ADDR : V2_CR_PC_BASE_ADDR)

#define REG_PC_DBG(_SOC, _n)		(CR_PC_BASE_ADDR(_SOC) + ((_n) * 0x100))
#define REG_CR_BOOT_TRIGGER(_SOC)	(NPU_CLUSTER_BASE(_SOC) + 0x000)
#define REG_CR_BOOT_CONFIG(_SOC)	(NPU_CLUSTER_BASE(_SOC) + 0x004)
#define REG_CR_BOOT_BASE(_SOC, _n)	(NPU_CLUSTER_BASE(_SOC) + 0x020 + ((_n) << 2))

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
#define REG_TX_DMA_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x088)
#define REG_TX_CPU_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x08c)

#define REG_RX_BASE(_n)			(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x180)
#define REG_RX_DSCP_NUM(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x184)
#define REG_RX_DMA_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x188)
#define REG_RX_CPU_IDX(_n)		(NPU_WLAN_BASE_ADDR + ((_n) << 4) + 0x18c)

#define NPU_TIMER_BASE_ADDR		0x310100
#define REG_WDT_TIMER_CTRL(_n)		(NPU_TIMER_BASE_ADDR + ((_n) * 0x100))
#define WDT_EN_MASK			BIT(25)
#define WDT_INTR_MASK			BIT(21)

#define AN7552_WDT_TIMER_EN_MASK	0x1e0001ef /* keep enable_bits (4 cntTimers + 4 wdogTimers) */
#define AN7581_WDT_TIMER_EN_MASK	0x2000027  /* keep enable_bits (3 cntTimers + 1 wdogTimer) */
#define EN7523_WDT_TIMER_EN_MASK	0x2000027  /* keep enable_bits (3 cntTimers + 1 wdogTimer) */

#define EN7523_CSR_REG_NUM		12
#define AN7581_CSR_REG_NUM		3
#define AN7552_CSR_REG_NUM		3

#define V1_CSR_PC_BASE_ADDR		0x1ec08800
#define V2_CSR_PC_BASE_ADDR		0x1ec05000

#define EN7523_NPU_CORES		4
#define AN7552_NPU_CORES		2
#define AN7583_NPU_CORES		6
#define AN7581_NPU_CORES		8
#define MAX_NPU_CORES(_SOC)		((_SOC)->version == NPU_V2 ?   AN7581_NPU_CORES : \
					 (_SOC)->version == NPU_V2_S ? AN7552_NPU_CORES : \
					 (_SOC)->version == NPU_V2_P ? AN7583_NPU_CORES : \
								       EN7523_NPU_CORES)

#define STATIC_L2C_SRAM_SIZE(_SOC)	((_SOC)->version == NPU_V2 ? SZ_256K : SZ_128K)

#define VERSION_MAJOR(version)		(((version) >> 24) & 0xff)
#define VERSION_MINOR(version)		(((version) >> 16) & 0xff)
#define VERSION_PATCH(version)		(((version) >> 8)  & 0xff)
#define VERSION_BUILD(version)		((version) & 0xff)

enum func_type {
	NPU_OP_SET = 1,
	NPU_OP_SET_NO_WAIT,
	NPU_OP_GET,
	NPU_OP_GET_NO_WAIT,
};

enum mailbox_callbacks {
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

	u32 mbox_func_mask;
	bool shared_wdt;
	bool have_wdt;
	u16 wlan_mbox_size;

	struct airoha_npu_fw fw_rv32;
	struct airoha_npu_fw fw_data;

	void (*boot_core)(struct airoha_npu *npu, struct reserved_mem *rmem);
};

struct airoha_npu_priv {
	struct airoha_npu npu;
	struct reserved_mem *rmem;
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
	/* AN7552 packs four watchdogs into each timer control register. */
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

enum airoha_npu_debugfs_counter {
	AIROHA_NPU_DBG_MBOX_REQUESTS,
	AIROHA_NPU_DBG_MBOX_SUCCESS,
	AIROHA_NPU_DBG_MBOX_ERRORS,
	AIROHA_NPU_DBG_MBOX_TIMEOUTS,
	AIROHA_NPU_DBG_MBOX_IRQS,
	AIROHA_NPU_DBG_WDT_IRQS,
	AIROHA_NPU_DBG_COUNTER_MAX,
};

#if IS_ENABLED(CONFIG_DEBUG_FS)
struct airoha_npu_debugfs {
	struct airoha_npu *npu;
	struct dentry *dir;
	atomic64_t counters[AIROHA_NPU_DBG_COUNTER_MAX];
	u32 retry_times;
	u32 debug_level;
	u32 fw_version;
	bool fw_version_valid;
};

static DEFINE_XARRAY(airoha_npu_debugfs_ctx);

static void airoha_npu_debugfs_count(struct airoha_npu *npu,
				     enum airoha_npu_debugfs_counter id)
{
	struct airoha_npu_debugfs *dbg;

	rcu_read_lock();
	dbg = xa_load(&airoha_npu_debugfs_ctx, (unsigned long)npu);
	if (dbg)
		atomic64_inc(&dbg->counters[id]);
	rcu_read_unlock();
}

static u32 airoha_npu_debugfs_level(struct airoha_npu *npu)
{
	struct airoha_npu_debugfs *dbg;
	u32 level = 0;

	rcu_read_lock();
	dbg = xa_load(&airoha_npu_debugfs_ctx, (unsigned long)npu);
	if (dbg)
		level = READ_ONCE(dbg->debug_level);
	rcu_read_unlock();

	return level;
}
#else
static inline void
airoha_npu_debugfs_count(struct airoha_npu *npu,
			 enum airoha_npu_debugfs_counter id)
{
}

static inline u32 airoha_npu_debugfs_level(struct airoha_npu *npu)
{
	return 0;
}
#endif

static int airoha_npu_send_msg_locked(struct airoha_npu *npu, int func_id,
				      const void *data, int len, void *reply,
				      u16 reply_len)
{
	struct airoha_npu_core *core = &npu->cores[0]; /* FIXME: core */
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

	/* A mailbox request requires a coherent payload buffer. */
	if (!core->buf)
		return -ENODEV;

	airoha_npu_debugfs_count(npu, AIROHA_NPU_DBG_MBOX_REQUESTS);

	/*
	 * Mailbox payloads are bidirectional (CPU request, NPU response).
	 * On EN7581+MT7996, streaming DMA_BIDIRECTIONAL against the
	 * caller kzalloc() buffer can leave WLAN_FUNC_GET_WAIT_NPU_VERSION
	 * reading as 0.0 despite MBOX success. Reuse a probe-time coherent
	 * bounce buffer under the per-core lock (also used from PPE
	 * foe_commit under atomic context).
	 */
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
	if (ret) {
		if (ret == -ETIMEDOUT)
			airoha_npu_debugfs_count(npu,
					   AIROHA_NPU_DBG_MBOX_TIMEOUTS);
		airoha_npu_debugfs_count(npu, AIROHA_NPU_DBG_MBOX_ERRORS);
	} else if (FIELD_GET(MBOX_MSG_STATUS, val) != NPU_MBOX_SUCCESS) {
		ret = FIELD_GET(MBOX_MSG_STATUS, val);
		switch (ret) {
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
		airoha_npu_debugfs_count(npu, AIROHA_NPU_DBG_MBOX_ERRORS);
	} else {
		airoha_npu_debugfs_count(npu, AIROHA_NPU_DBG_MBOX_SUCCESS);
	}

	/* Copy the trailing reply_len bytes of the response. */
	if (!ret && reply)
		memcpy(reply, core->buf + len - reply_len, reply_len);

	if (airoha_npu_debugfs_level(npu) >= 2)
		dev_dbg(npu->dev, "mbox func=%d size=%d status=%#x ret=%d\n",
			func_id, len, val, ret);

	return ret;
}

static int __airoha_npu_send_msg(struct airoha_npu *npu, int func_id,
				 const void *data, int len, void *reply,
				 u16 reply_len)
{
	struct airoha_npu_core *core = &npu->cores[0];
	int ret;

	spin_lock_bh(&core->lock);
	ret = airoha_npu_send_msg_locked(npu, func_id, data, len, reply, reply_len);
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

	airoha_npu_debugfs_count(npu, AIROHA_NPU_DBG_MBOX_IRQS);

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

	airoha_npu_debugfs_count(npu, AIROHA_NPU_DBG_WDT_IRQS);

	if (val & enable)
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
	struct airoha_npu_core *core = &npu->cores[0];
	struct ppe_mbox_data msg = {};
	int err;

	if (upper_32_bits(foe_addr) || entry_size != 80)
		return -EINVAL;
	msg.func_type = NPU_OP_SET;
	msg.func_id = PPE_FUNC_SET_WAIT_API;
	msg.set_info.data = lower_32_bits(foe_addr);
	msg.set_info.size = entry_size;
	msg.set_info.func_id = ppe2 ? PPE2_SRAM_SET_ENTRY : PPE_SRAM_SET_ENTRY;

	/* The SDK stage/hash pair is one transaction on the shared mailbox. */
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
	enum airoha_npu_wlan_set_cmd cmd;
	u32 val = 0;
	int err;

	cmd = WLAN_FUNC_SET_WAIT_NPU_BAND0_ONCPU;
	err = airoha_npu_wlan_msg_send(npu, 1, cmd, &val, sizeof(val),
				       GFP_KERNEL);
	if (err && err != -EOPNOTSUPP) {
		dev_err(npu->dev, "error on set NPU_BAND0 value: %d\n", err);
		return err;
	}
	dev_info(npu->dev, "NPU_BAND0 value set to %u\n", val);

	cmd = WLAN_FUNC_SET_WAIT_PKT_BUF_ADDR;
	err = airoha_npu_wlan_set_reserved_memory(npu, 0, "pkt", cmd);
	if (err) {
		dev_err(npu->dev, "error on set pkt memory region: %d\n", err);
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
		dev_err(npu->dev, "error on send force_to_cpu command: %d\n", err);

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


#if IS_ENABLED(CONFIG_DEBUG_FS)
#define AIROHA_NPU_MIB_COUNT	32
#define AIROHA_NPU_DEBUG_LEVEL_MAX	3

static void airoha_npu_debugfs_print_reg(struct seq_file *m,
					 struct airoha_npu *npu,
					 const char *name, u32 reg)
{
	u32 val;
	int err;

	err = regmap_read(npu->regmap, reg, &val);
	if (err)
		seq_printf(m, "%-28s [%08x] error=%d\n", name, reg, err);
	else
		seq_printf(m, "%-28s [%08x] %08x\n", name, reg, val);
}

static int airoha_npu_debugfs_status_show(struct seq_file *m, void *private)
{
	struct airoha_npu_debugfs *dbg = m->private;
	struct airoha_npu *npu = dbg->npu;
	u32 boot_config = 0, boot_trigger = 0, irq_status = 0;
	int i;

	regmap_read(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data),
		    &boot_config);
	regmap_read(npu->regmap, REG_CR_BOOT_TRIGGER(npu->soc_data),
		    &boot_trigger);
	regmap_read(npu->regmap, REG_IRQ_STATUS, &irq_status);

	seq_printf(m, "device: %s\n", dev_name(npu->dev));
	seq_printf(m, "npu_interface: v%u\n",
		   npu->soc_data->version == NPU_V1 ? 1 : 2);
	seq_printf(m, "cores: %d\n", MAX_NPU_CORES(npu->soc_data));
	if (!dbg->fw_version_valid)
		seq_puts(m, "firmware: unavailable\n");
	else
		seq_printf(m, "firmware: v%u.%u.%u.%u (0x%08x)\n",
			   VERSION_MAJOR(dbg->fw_version),
			   VERSION_MINOR(dbg->fw_version),
			   VERSION_PATCH(dbg->fw_version),
			   VERSION_BUILD(dbg->fw_version),
			   dbg->fw_version);
	seq_printf(m, "boot_config: 0x%08x\n", boot_config);
	seq_printf(m, "boot_trigger: 0x%08x\n", boot_trigger);
	seq_printf(m, "wlan_irq_status: 0x%08x\n", irq_status);
	seq_printf(m, "retry_times: %u\n", READ_ONCE(dbg->retry_times));
	seq_printf(m, "debug_level: %u\n", READ_ONCE(dbg->debug_level));

	for (i = 0; i < (int)ARRAY_SIZE(npu->irqs); i++)
		seq_printf(m, "wlan_irq%d: %d\n", i, npu->irqs[i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_npu_debugfs_status);

static int airoha_npu_debugfs_rings_show(struct seq_file *m, void *private)
{
	struct airoha_npu_debugfs *dbg = m->private;
	struct airoha_npu *npu = dbg->npu;
	u32 base, count, dma_idx, cpu_idx;
	int i;

	seq_puts(m, "ring direction base       descriptors dma_idx cpu_idx\n");

	for (i = 0; i < 2; i++) {
		regmap_read(npu->regmap, REG_RX_BASE(i), &base);
		regmap_read(npu->regmap, REG_RX_DSCP_NUM(i), &count);
		regmap_read(npu->regmap, REG_RX_DMA_IDX(i), &dma_idx);
		regmap_read(npu->regmap, REG_RX_CPU_IDX(i), &cpu_idx);
		seq_printf(m, "%4d RX        0x%08x %11u %7u %7u\n",
			   i, base, count, dma_idx, cpu_idx);
	}

	for (i = 0; i < 2; i++) {
		int ring = i + 2;

		regmap_read(npu->regmap, REG_TX_BASE(ring), &base);
		regmap_read(npu->regmap, REG_TX_DSCP_NUM(ring), &count);
		regmap_read(npu->regmap, REG_TX_DMA_IDX(ring), &dma_idx);
		regmap_read(npu->regmap, REG_TX_CPU_IDX(ring), &cpu_idx);
		seq_printf(m, "%4d TX        0x%08x %11u %7u %7u\n",
			   ring, base, count, dma_idx, cpu_idx);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_npu_debugfs_rings);

static int airoha_npu_debugfs_registers_show(struct seq_file *m,
					      void *private)
{
	struct airoha_npu_debugfs *dbg = m->private;
	struct airoha_npu *npu = dbg->npu;
	char name[32];
	int i, ring;

	airoha_npu_debugfs_print_reg(m, npu, "MBOX_INT_STATUS",
				      REG_CR_MBOX_INT_STATUS);
	for (i = 0; i < 4; i++) {
		snprintf(name, sizeof(name), "MBOX_INT_MASK%d", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_CR_MBOX_INT_MASK(i));
		snprintf(name, sizeof(name), "MBQ0_CTRL%d", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_CR_MBQ0_CTRL(i));
		snprintf(name, sizeof(name), "MBQ8_CTRL%d", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_CR_MBQ8_CTRL(i));
	}

	airoha_npu_debugfs_print_reg(m, npu, "WLAN_IRQ_STATUS",
				      REG_IRQ_STATUS);
	for (i = 0; i < 2; i++) {
		snprintf(name, sizeof(name), "WLAN_IRQ_RXDONE%d", i);
		airoha_npu_debugfs_print_reg(m, npu, name, REG_IRQ_RXDONE(i));

		snprintf(name, sizeof(name), "RX%d_BASE", i);
		airoha_npu_debugfs_print_reg(m, npu, name, REG_RX_BASE(i));
		snprintf(name, sizeof(name), "RX%d_DSCP_NUM", i);
		airoha_npu_debugfs_print_reg(m, npu, name, REG_RX_DSCP_NUM(i));
		snprintf(name, sizeof(name), "RX%d_DMA_IDX", i);
		airoha_npu_debugfs_print_reg(m, npu, name, REG_RX_DMA_IDX(i));
		snprintf(name, sizeof(name), "RX%d_CPU_IDX", i);
		airoha_npu_debugfs_print_reg(m, npu, name, REG_RX_CPU_IDX(i));

		ring = i + 2;
		snprintf(name, sizeof(name), "TX%d_BASE", ring);
		airoha_npu_debugfs_print_reg(m, npu, name, REG_TX_BASE(ring));
		snprintf(name, sizeof(name), "TX%d_DSCP_NUM", ring);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_TX_DSCP_NUM(ring));
		snprintf(name, sizeof(name), "TX%d_DMA_IDX", ring);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_TX_DMA_IDX(ring));
		snprintf(name, sizeof(name), "TX%d_CPU_IDX", ring);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_TX_CPU_IDX(ring));
	}

	airoha_npu_debugfs_print_reg(m, npu, "BOOT_TRIGGER",
				      REG_CR_BOOT_TRIGGER(npu->soc_data));
	airoha_npu_debugfs_print_reg(m, npu, "BOOT_CONFIG",
				      REG_CR_BOOT_CONFIG(npu->soc_data));
	for (i = 0; i < MAX_NPU_CORES(npu->soc_data); i++) {
		snprintf(name, sizeof(name), "CORE%d_BOOT_BASE", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_CR_BOOT_BASE(npu->soc_data, i));
		snprintf(name, sizeof(name), "CORE%d_PC", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_PC_DBG(npu->soc_data, i));
		snprintf(name, sizeof(name), "CORE%d_SP", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_PC_DBG(npu->soc_data, i) + 4);
		snprintf(name, sizeof(name), "CORE%d_LR", i);
		airoha_npu_debugfs_print_reg(m, npu, name,
					      REG_PC_DBG(npu->soc_data, i) + 8);

		if (npu->soc_data->version >= NPU_V2) {
			snprintf(name, sizeof(name), "CORE%d_WDT_CTRL", i);
			airoha_npu_debugfs_print_reg(m, npu, name,
						     airoha_npu_wdt_ctrl(npu, i));
		}
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_npu_debugfs_registers);

static int airoha_npu_debugfs_mib_show(struct seq_file *m, void *private)
{
	struct airoha_npu_debugfs *dbg = m->private;
	struct airoha_npu *npu = dbg->npu;
	u32 val;
	int i, err;

	for (i = 0; i < AIROHA_NPU_MIB_COUNT; i++) {
		err = regmap_read(npu->regmap, REG_CR_NPU_MIB(i), &val);
		if (err)
			seq_printf(m, "%02d: error=%d\n", i, err);
		else
			seq_printf(m, "%02d: 0x%08x (%u)\n", i, val, val);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_npu_debugfs_mib);

static int airoha_npu_debugfs_counters_show(struct seq_file *m,
					     void *private)
{
	static const char * const names[AIROHA_NPU_DBG_COUNTER_MAX] = {
		[AIROHA_NPU_DBG_MBOX_REQUESTS] = "mbox_requests",
		[AIROHA_NPU_DBG_MBOX_SUCCESS] = "mbox_success",
		[AIROHA_NPU_DBG_MBOX_ERRORS] = "mbox_errors",
		[AIROHA_NPU_DBG_MBOX_TIMEOUTS] = "mbox_timeouts",
		[AIROHA_NPU_DBG_MBOX_IRQS] = "mbox_irqs",
		[AIROHA_NPU_DBG_WDT_IRQS] = "wdt_irqs",
	};
	struct airoha_npu_debugfs *dbg = m->private;
	int i;

	for (i = 0; i < AIROHA_NPU_DBG_COUNTER_MAX; i++)
		seq_printf(m, "%-20s %lld\n", names[i],
			   (long long)atomic64_read(&dbg->counters[i]));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_npu_debugfs_counters);

static void airoha_npu_debugfs_json_string(struct seq_file *m,
					    const char *str)
{
	const unsigned char *p = (const unsigned char *)str;

	seq_putc(m, '"');
	for (; *p; p++) {
		switch (*p) {
		case '"':
			seq_puts(m, "\\\"");
			break;
		case '\\':
			seq_puts(m, "\\\\");
			break;
		case '\b':
			seq_puts(m, "\\b");
			break;
		case '\f':
			seq_puts(m, "\\f");
			break;
		case '\n':
			seq_puts(m, "\\n");
			break;
		case '\r':
			seq_puts(m, "\\r");
			break;
		case '\t':
			seq_puts(m, "\\t");
			break;
		default:
			if (*p < 0x20)
				seq_printf(m, "\\u%04x", *p);
			else
				seq_putc(m, *p);
			break;
		}
	}
	seq_putc(m, '"');
}

static void airoha_npu_debugfs_json_nullable_u32(struct seq_file *m,
						 const char *name,
						 u32 value, int err,
						 bool comma)
{
	seq_printf(m, "\"%s\": ", name);
	if (err)
		seq_puts(m, "null");
	else
		seq_printf(m, "%u", value);

	if (comma)
		seq_puts(m, ", ");
}

static void airoha_npu_debugfs_json_u32(struct seq_file *m,
					const char *name, u32 value,
					int err, bool comma)
{
	airoha_npu_debugfs_json_nullable_u32(m, name, value, err, true);
	seq_printf(m, "\"%s_hex\": ", name);
	if (err)
		seq_puts(m, "null");
	else
		seq_printf(m, "\"0x%08x\"", value);

	if (comma)
		seq_puts(m, ", ");
}

static void airoha_npu_debugfs_json_reg(struct seq_file *m,
					struct airoha_npu *npu,
					const char *name, u32 reg,
					bool *first)
{
	u32 value = 0;
	int err;

	err = regmap_read(npu->regmap, reg, &value);
	if (!*first)
		seq_puts(m, ",\n");
	*first = false;

	seq_puts(m, "      ");
	airoha_npu_debugfs_json_string(m, name);
	seq_printf(m, ": {\"address\": %u, "
		   "\"address_hex\": \"0x%08x\", ", reg, reg);
	if (err)
		seq_printf(m, "\"value\": null, \"value_hex\": null, "
			   "\"error\": %d}", err);
	else
		seq_printf(m, "\"value\": %u, "
			   "\"value_hex\": \"0x%08x\", "
			   "\"error\": 0}", value, value);
}

static void airoha_npu_debugfs_json_ring(struct seq_file *m,
					 struct airoha_npu *npu,
					 bool rx, int ring, bool comma)
{
	u32 base = 0, count = 0, dma_idx = 0, cpu_idx = 0;
	int base_err, count_err, dma_err, cpu_err;

	if (rx) {
		base_err = regmap_read(npu->regmap, REG_RX_BASE(ring), &base);
		count_err = regmap_read(npu->regmap, REG_RX_DSCP_NUM(ring),
					&count);
		dma_err = regmap_read(npu->regmap, REG_RX_DMA_IDX(ring),
				      &dma_idx);
		cpu_err = regmap_read(npu->regmap, REG_RX_CPU_IDX(ring),
				      &cpu_idx);
	} else {
		base_err = regmap_read(npu->regmap, REG_TX_BASE(ring), &base);
		count_err = regmap_read(npu->regmap, REG_TX_DSCP_NUM(ring),
					&count);
		dma_err = regmap_read(npu->regmap, REG_TX_DMA_IDX(ring),
				      &dma_idx);
		cpu_err = regmap_read(npu->regmap, REG_TX_CPU_IDX(ring),
				      &cpu_idx);
	}

	seq_printf(m, "    {\"name\": \"%s%d\", "
		   "\"direction\": \"%s\", \"index\": %d, ",
		   rx ? "rx" : "tx", ring, rx ? "rx" : "tx", ring);
	airoha_npu_debugfs_json_u32(m, "base", base, base_err, true);
	airoha_npu_debugfs_json_nullable_u32(m, "descriptors", count,
						 count_err, true);
	airoha_npu_debugfs_json_nullable_u32(m, "dma_idx", dma_idx,
						 dma_err, true);
	airoha_npu_debugfs_json_nullable_u32(m, "cpu_idx", cpu_idx,
						 cpu_err, true);
	seq_printf(m, "\"errors\": {\"base\": %d, "
		   "\"descriptors\": %d, \"dma_idx\": %d, "
		   "\"cpu_idx\": %d}}%s\n",
		   base_err, count_err, dma_err, cpu_err, comma ? "," : "");
}

static int airoha_npu_debugfs_json_show(struct seq_file *m, void *private)
{
	static const char * const counter_names[AIROHA_NPU_DBG_COUNTER_MAX] = {
		[AIROHA_NPU_DBG_MBOX_REQUESTS] = "mbox_requests",
		[AIROHA_NPU_DBG_MBOX_SUCCESS] = "mbox_success",
		[AIROHA_NPU_DBG_MBOX_ERRORS] = "mbox_errors",
		[AIROHA_NPU_DBG_MBOX_TIMEOUTS] = "mbox_timeouts",
		[AIROHA_NPU_DBG_MBOX_IRQS] = "mbox_irqs",
		[AIROHA_NPU_DBG_WDT_IRQS] = "wdt_irqs",
	};
	struct airoha_npu_debugfs *dbg = m->private;
	struct airoha_npu *npu = dbg->npu;
	u32 boot_config = 0, boot_trigger = 0, irq_status = 0;
	int boot_config_err, boot_trigger_err, irq_status_err;
	bool first = true;
	char name[32];
	int i, ring, err;
	u32 value;

	(void)private;

	boot_config_err = regmap_read(npu->regmap,
				      REG_CR_BOOT_CONFIG(npu->soc_data),
				      &boot_config);
	boot_trigger_err = regmap_read(npu->regmap,
				       REG_CR_BOOT_TRIGGER(npu->soc_data),
				       &boot_trigger);
	irq_status_err = regmap_read(npu->regmap, REG_IRQ_STATUS, &irq_status);

	seq_puts(m, "{\n  \"schema_version\": 1,\n  \"device\": ");
	airoha_npu_debugfs_json_string(m, dev_name(npu->dev));
	seq_printf(m, ",\n  \"interface\": {\"version\": %u, "
		   "\"cores\": %d},\n  \"firmware\": ",
		   npu->soc_data->version, MAX_NPU_CORES(npu->soc_data));
	if (dbg->fw_version_valid)
		seq_printf(m, "{\"major\": %u, \"minor\": %u, \"patch\": %u, \"build\": %u, "
			   "\"raw\": %u, \"raw_hex\": \"0x%08x\"}",
			   VERSION_MAJOR(dbg->fw_version),
			   VERSION_MINOR(dbg->fw_version),
			   VERSION_PATCH(dbg->fw_version),
			   VERSION_BUILD(dbg->fw_version),
			   dbg->fw_version, dbg->fw_version);
	else
		seq_puts(m, "null");

	seq_printf(m, ",\n  \"controls\": {\"retry_times\": %u, "
		   "\"debug_level\": %u},\n  \"boot\": {",
		   READ_ONCE(dbg->retry_times), READ_ONCE(dbg->debug_level));
	airoha_npu_debugfs_json_u32(m, "config", boot_config,
					boot_config_err, true);
	airoha_npu_debugfs_json_u32(m, "trigger", boot_trigger,
					boot_trigger_err, true);
	seq_printf(m, "\"errors\": {\"config\": %d, "
		   "\"trigger\": %d}},\n  \"interrupts\": {",
		   boot_config_err, boot_trigger_err);
	airoha_npu_debugfs_json_u32(m, "wlan_status", irq_status,
					irq_status_err, true);
	seq_printf(m, "\"status_error\": %d, \"wlan_irqs\": [",
		   irq_status_err);
	for (i = 0; i < (int)ARRAY_SIZE(npu->irqs); i++)
		seq_printf(m, "%s%d", i ? ", " : "", npu->irqs[i]);
	seq_puts(m, "]},\n  \"rings\": [\n");
	airoha_npu_debugfs_json_ring(m, npu, true, 0, true);
	airoha_npu_debugfs_json_ring(m, npu, true, 1, true);
	airoha_npu_debugfs_json_ring(m, npu, false, 2, true);
	airoha_npu_debugfs_json_ring(m, npu, false, 3, false);

	seq_puts(m, "  ],\n  \"counters\": {");
	for (i = 0; i < AIROHA_NPU_DBG_COUNTER_MAX; i++)
		seq_printf(m, "%s\"%s\": %lld",
			   i ? ", " : "", counter_names[i],
			   (long long)atomic64_read(&dbg->counters[i]));

	seq_puts(m, "},\n  \"mib\": [\n");
	for (i = 0; i < AIROHA_NPU_MIB_COUNT; i++) {
		value = 0;
		err = regmap_read(npu->regmap, REG_CR_NPU_MIB(i), &value);
		seq_printf(m, "    {\"index\": %d, ", i);
		if (err)
			seq_printf(m, "\"value\": null, "
				   "\"value_hex\": null, \"error\": %d}",
				   err);
		else
			seq_printf(m, "\"value\": %u, "
				   "\"value_hex\": \"0x%08x\", "
				   "\"error\": 0}", value, value);
		seq_printf(m, "%s\n",
			   i + 1 == AIROHA_NPU_MIB_COUNT ? "" : ",");
	}

	seq_puts(m, "  ],\n  \"registers\": {\n");
	airoha_npu_debugfs_json_reg(m, npu, "MBOX_INT_STATUS",
				    REG_CR_MBOX_INT_STATUS, &first);
	for (i = 0; i < 4; i++) {
		snprintf(name, sizeof(name), "MBOX_INT_MASK%d", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_CR_MBOX_INT_MASK(i), &first);
		snprintf(name, sizeof(name), "MBQ0_CTRL%d", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_CR_MBQ0_CTRL(i), &first);
		snprintf(name, sizeof(name), "MBQ8_CTRL%d", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_CR_MBQ8_CTRL(i), &first);
	}

	airoha_npu_debugfs_json_reg(m, npu, "WLAN_IRQ_STATUS",
				    REG_IRQ_STATUS, &first);
	for (i = 0; i < 2; i++) {
		snprintf(name, sizeof(name), "WLAN_IRQ_RXDONE%d", i);
		airoha_npu_debugfs_json_reg(m, npu, name, REG_IRQ_RXDONE(i),
					    &first);

		snprintf(name, sizeof(name), "RX%d_BASE", i);
		airoha_npu_debugfs_json_reg(m, npu, name, REG_RX_BASE(i),
					    &first);
		snprintf(name, sizeof(name), "RX%d_DSCP_NUM", i);
		airoha_npu_debugfs_json_reg(m, npu, name, REG_RX_DSCP_NUM(i),
					    &first);
		snprintf(name, sizeof(name), "RX%d_DMA_IDX", i);
		airoha_npu_debugfs_json_reg(m, npu, name, REG_RX_DMA_IDX(i),
					    &first);
		snprintf(name, sizeof(name), "RX%d_CPU_IDX", i);
		airoha_npu_debugfs_json_reg(m, npu, name, REG_RX_CPU_IDX(i),
					    &first);

		ring = i + 2;
		snprintf(name, sizeof(name), "TX%d_BASE", ring);
		airoha_npu_debugfs_json_reg(m, npu, name, REG_TX_BASE(ring),
					    &first);
		snprintf(name, sizeof(name), "TX%d_DSCP_NUM", ring);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_TX_DSCP_NUM(ring), &first);
		snprintf(name, sizeof(name), "TX%d_DMA_IDX", ring);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_TX_DMA_IDX(ring), &first);
		snprintf(name, sizeof(name), "TX%d_CPU_IDX", ring);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_TX_CPU_IDX(ring), &first);
	}

	airoha_npu_debugfs_json_reg(m, npu, "BOOT_TRIGGER",
				    REG_CR_BOOT_TRIGGER(npu->soc_data), &first);
	airoha_npu_debugfs_json_reg(m, npu, "BOOT_CONFIG",
				    REG_CR_BOOT_CONFIG(npu->soc_data), &first);
	for (i = 0; i < MAX_NPU_CORES(npu->soc_data); i++) {
		snprintf(name, sizeof(name), "CORE%d_BOOT_BASE", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_CR_BOOT_BASE(npu->soc_data, i),
					    &first);
		snprintf(name, sizeof(name), "CORE%d_PC", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_PC_DBG(npu->soc_data, i), &first);
		snprintf(name, sizeof(name), "CORE%d_SP", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_PC_DBG(npu->soc_data, i) + 4,
					    &first);
		snprintf(name, sizeof(name), "CORE%d_LR", i);
		airoha_npu_debugfs_json_reg(m, npu, name,
					    REG_PC_DBG(npu->soc_data, i) + 8,
					    &first);

		if (npu->soc_data->version >= NPU_V2) {
			snprintf(name, sizeof(name), "CORE%d_WDT_CTRL", i);
			airoha_npu_debugfs_json_reg(m, npu, name,
						    airoha_npu_wdt_ctrl(npu, i), &first);
		}
	}

	seq_puts(m, "\n  }\n}\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_npu_debugfs_json);

static int airoha_npu_debugfs_retry_get(void *data, u64 *val)
{
	struct airoha_npu_debugfs *dbg = data;

	*val = READ_ONCE(dbg->retry_times);
	return 0;
}

static int airoha_npu_debugfs_retry_set(void *data, u64 val)
{
	struct airoha_npu_debugfs *dbg = data;
	u32 retry_times;
	int err;

	if (!val || val > U16_MAX)
		return -ERANGE;

	retry_times = val;
	err = airoha_npu_wlan_msg_send(dbg->npu, 0,
				       WLAN_FUNC_SET_WAIT_ERROR_RETRY_TIMES,
				       &retry_times, sizeof(retry_times),
				       GFP_KERNEL);
	if (err)
		return err;

	WRITE_ONCE(dbg->retry_times, retry_times);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(airoha_npu_debugfs_retry_fops,
			 airoha_npu_debugfs_retry_get,
			 airoha_npu_debugfs_retry_set, "%llu\n");

static int airoha_npu_debugfs_level_get(void *data, u64 *val)
{
	struct airoha_npu_debugfs *dbg = data;

	*val = READ_ONCE(dbg->debug_level);
	return 0;
}

static int airoha_npu_debugfs_level_set(void *data, u64 val)
{
	struct airoha_npu_debugfs *dbg = data;

	if (val > AIROHA_NPU_DEBUG_LEVEL_MAX)
		return -ERANGE;

	WRITE_ONCE(dbg->debug_level, val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(airoha_npu_debugfs_level_fops,
			 airoha_npu_debugfs_level_get,
			 airoha_npu_debugfs_level_set, "%llu\n");

static int airoha_npu_debugfs_init(struct airoha_npu *npu, u32 fw_version,
				   bool fw_version_valid)
{
	struct airoha_npu_debugfs *dbg;
	int err;

	dbg = devm_kzalloc(npu->dev, sizeof(*dbg), GFP_KERNEL);
	if (!dbg)
		return -ENOMEM;

	dbg->npu = npu;
	dbg->retry_times = 3;
	dbg->fw_version = fw_version;
	dbg->fw_version_valid = fw_version_valid;

	dbg->dir = debugfs_create_dir("airoha_npu", NULL);
	if (IS_ERR_OR_NULL(dbg->dir))
		return dbg->dir ? PTR_ERR(dbg->dir) : -ENOMEM;

	err = xa_err(xa_store(&airoha_npu_debugfs_ctx,
			      (unsigned long)npu, dbg, GFP_KERNEL));
	if (err) {
		debugfs_remove_recursive(dbg->dir);
		return err;
	}

	debugfs_create_file("status", 0444, dbg->dir, dbg,
			    &airoha_npu_debugfs_status_fops);
	debugfs_create_file("rings", 0444, dbg->dir, dbg,
			    &airoha_npu_debugfs_rings_fops);
	debugfs_create_file("registers", 0444, dbg->dir, dbg,
			    &airoha_npu_debugfs_registers_fops);
	debugfs_create_file("mib", 0444, dbg->dir, dbg,
			    &airoha_npu_debugfs_mib_fops);
	debugfs_create_file("counters", 0444, dbg->dir, dbg,
			    &airoha_npu_debugfs_counters_fops);
	debugfs_create_file("debug.json", 0444, dbg->dir, dbg,
			    &airoha_npu_debugfs_json_fops);
	debugfs_create_file("retry_times", 0600, dbg->dir, dbg,
			    &airoha_npu_debugfs_retry_fops);
	debugfs_create_file("debug_level", 0600, dbg->dir, dbg,
			    &airoha_npu_debugfs_level_fops);

	return 0;
}

static void airoha_npu_debugfs_remove(struct airoha_npu *npu)
{
	struct airoha_npu_debugfs *dbg;

	dbg = xa_erase(&airoha_npu_debugfs_ctx, (unsigned long)npu);
	if (!dbg)
		return;

	synchronize_rcu();
	debugfs_remove_recursive(dbg->dir);
}
#else
static inline int airoha_npu_debugfs_init(struct airoha_npu *npu,
					  u32 fw_version,
					  bool fw_version_valid)
{
	return 0;
}

static inline void airoha_npu_debugfs_remove(struct airoha_npu *npu)
{
}
#endif

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

static void airoha_boot_core_v1(struct airoha_npu *npu,
				struct reserved_mem *rmem)
{
	u32 val = 0x1, boot = 0x1;
	int core;

	/* setting booting address */
	for (core = 0; core < MAX_NPU_CORES(npu->soc_data); core++) {
		regmap_write(npu->regmap, REG_CR_BOOT_BASE(npu->soc_data, core), rmem->base);
		usleep_range(1000, 2000);

		if (core > 0) {
			boot = 0x2;
			regmap_read(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data), &val);
			val &= 0xff;          /* keep old enabled bits */
			val |= (0x1 << core);   /* add new enable bit for CoreX */
			val |= (0x100 << core); /* add reboot bit for CoreX */
		}

		regmap_write(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data), val);
		regmap_write(npu->regmap, REG_CR_BOOT_TRIGGER(npu->soc_data), boot);

		msleep(100);
	}
}

static void airoha_boot_core_v2(struct airoha_npu *npu,
				struct reserved_mem *rmem)
{
	int i;

	/* setting booting address */
	for (i = 0; i < MAX_NPU_CORES(npu->soc_data); i++)
		regmap_write(npu->regmap, REG_CR_BOOT_BASE(npu->soc_data, i), rmem->base);
	usleep_range(1000, 2000);

	/* enable NPU cores */
	regmap_write(npu->regmap, REG_CR_BOOT_CONFIG(npu->soc_data),
		     GENMASK(MAX_NPU_CORES(npu->soc_data) - 1, 0));
	regmap_write(npu->regmap, REG_CR_BOOT_TRIGGER(npu->soc_data), 0x1);
	msleep(100);
}

static bool is_fpga(struct regmap *scuclk)
{
	u32 val;

	regmap_read(scuclk, 0x9c, &val);
	return !(val & BIT(0));
}

static int sram_size_of_l2c(struct regmap *scuclk)
{
	u32 val;

	regmap_read(scuclk, 0x280, &val);
	return val;
}

static const struct regmap_config regmap_config = {
	.name			= "npu",
	.reg_bits		= 32,
	.val_bits		= 32,
	.reg_stride		= 4,
	.disable_locking	= true,
};

static int airoha_npu_probe_cpu_cores(struct airoha_npu *npu)
{
	struct airoha_npu_priv *priv = airoha_npu_to_priv(npu);
	bool npu_version_valid = false;
	__le32 fw_version = 0;
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
			dev_err(npu->dev, "failed to run npu firmware: %d\n", err);
		goto out_unlock;
	}

	/* Set the information consumed by the NPU firmware before booting it. */
	regmap_write(npu->regmap, REG_CR_NPU_MIB(10),
		     (u32)priv->rmem->base + npu->soc_data->fw_rv32.max_size);
	regmap_write(npu->regmap, REG_CR_NPU_MIB(11),
		     sram_size_of_l2c(npu->scu_regmap));
	regmap_write(npu->regmap, REG_CR_NPU_MIB(12),
		     is_fpga(npu->scu_regmap));
	regmap_write(npu->regmap, REG_CR_NPU_MIB(21),
		     !of_property_present(npu->dev->of_node,
					  "airoha,enable_npu_tx_uart"));
	msleep(100);

	/* Firmware and host mailbox buffers are ready before core release. */
	npu->soc_data->boot_core(npu, priv->rmem);

	/* Get NPU firmware version when supported. */
	err = airoha_npu_wlan_msg_get(npu, 0, WLAN_FUNC_GET_WAIT_NPU_VERSION,
				      &fw_version, sizeof(fw_version),
				      GFP_KERNEL);
	if (!err) {
		npu_version = le32_to_cpu(fw_version);
		npu_version_valid = true;
		dev_info(npu->dev, "NPU fw version: v%u.%u.%u.%u\n",
				   VERSION_MAJOR(npu_version),
				   VERSION_MINOR(npu_version),
				   VERSION_PATCH(npu_version),
				   VERSION_BUILD(npu_version));
	} else if (err != -EOPNOTSUPP) {
		dev_err(npu->dev, "cannot get NPU version: %d\n", err);
	}

	err = airoha_npu_debugfs_init(npu, npu_version, npu_version_valid);
	if (err)
		dev_warn(npu->dev, "failed to initialize debugfs: %d\n", err);

	/* Publish after the optional version and debugfs setup is complete. */
	smp_store_release(&priv->started, true);
	dev_info(npu->dev, "NPU firmware loaded, %u cores started\n",
		 MAX_NPU_CORES(npu->soc_data));
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

static int airoha_npu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_npu_priv *priv;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct airoha_npu *npu;
	void __iomem *base;
	int i, irq, err;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
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

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return -ENODEV;
	priv->rmem = rmem;
	priv->fw_addr = devm_memremap(dev, rmem->base, rmem->size,
				      MEMREMAP_WC);
	if (IS_ERR(priv->fw_addr))
		return PTR_ERR(priv->fw_addr);

	np = of_parse_phandle(dev->of_node, "airoha,scu", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "cannot get scuclk\n");

	npu->scu_regmap = syscon_node_to_regmap(np);
	of_node_put(np);
	if (IS_ERR(npu->scu_regmap))
		return PTR_ERR(npu->scu_regmap);

	npu->cores = devm_kcalloc(dev, MAX_NPU_CORES(npu->soc_data),
				  sizeof(*npu->cores), GFP_KERNEL);
	if (!npu->cores)
		return -ENOMEM;

	irq = platform_get_irq_byname(pdev, NPU_MBOX_INTNAME);
	if (irq < 0)
		return dev_err_probe(dev, irq, "error on get mbox irq");

	err = devm_request_irq(dev, irq, airoha_npu_mbox_handler,
			       IRQF_SHARED, "airoha-npu-mbox", npu);
	if (err)
		return err;

	/* WLAN IRQ lines follow the mailbox IRQ on both NPU revisions. */
	for (i = 0; i < ARRAY_SIZE(npu->irqs); i++) {
		char wlan_name[16];
		snprintf(wlan_name, 16, NPU_WLAN_INTNAME, i);
		irq = platform_get_irq_byname(pdev, wlan_name);
		if (irq < 0)
			return dev_err_probe(dev, irq, "error on get irq to " NPU_WLAN_INTNAME, i);

		npu->irqs[i] = irq;
	}

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		return err;

	/*
	 * Prepare the coherent mailbox buffers before the delayed firmware start.
	 * Only NPU v2 exposes per-core watchdog IRQs.
	 */
	for (i = 0; i < MAX_NPU_CORES(npu->soc_data); i++) {
		struct airoha_npu_core *core = &npu->cores[i];

		spin_lock_init(&core->lock);
		core->npu = npu;
		INIT_WORK(&core->wdt_work, airoha_npu_wdt_work);

		core->buf = dmam_alloc_coherent(dev, AIROHA_NPU_MBOX_SIZE,
						&core->addr, GFP_KERNEL);
		if (!core->buf)
			return -ENOMEM;

		if (npu->soc_data->have_wdt) {
			char watchdog_name[16];
			snprintf(watchdog_name, 16, NPU_WATCHDOG_INTNAME, i);
			irq = platform_get_irq_byname(pdev, watchdog_name);
			if (irq < 0)
				return dev_err_probe(dev, irq, "error on get irq to " NPU_WATCHDOG_INTNAME, i);
	
			err = devm_request_irq(dev, irq, airoha_npu_wdt_handler,
					       IRQF_SHARED, "airoha-npu-wdt", core);
			if (err)
				return err;
		}
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

static void airoha_npu_remove(struct platform_device *pdev)
{
	struct airoha_npu *npu = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&airoha_npu_to_priv(npu)->start_work);
	airoha_npu_debugfs_remove(npu);
}

static const struct airoha_npu_soc_data en7523_npu_soc_data = {
	.version = NPU_V1,
	.boot_core = airoha_boot_core_v1,
	.shared_wdt = false,
	.have_wdt = false,
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
	.mbox_func_mask = GENMASK(NPU_FUNC_NOTIFY, NPU_FUNC_WIFI),
	.shared_wdt = true,
	.have_wdt = true,
	.boot_core = airoha_boot_core_v2,
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
	.shared_wdt = false,
	.have_wdt = true,
	.boot_core = airoha_boot_core_v2,
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
	.shared_wdt = false,
	.have_wdt = true,
	.boot_core = airoha_boot_core_v2,
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
	{ .compatible = "airoha,en7581-npu", .data = &en7581_npu_soc_data },
	{ .compatible = "airoha,an7581-npu", .data = &en7581_npu_soc_data },
	{ .compatible = "airoha,an7583-npu", .data = &an7583_npu_soc_data },
	{ .compatible = "airoha,an7552-npu", .data = &an7552_npu_soc_data },
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
