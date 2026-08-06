// SPDX-License-Identifier: GPL-2.0-only
#ifndef QDMA_REGS_H
#define QDMA_REGS_H

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/types.h>

#ifndef FIELD_SET
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))
#endif

/** qchain_regs: QDMA Chain Registers */
struct qchain_regs {
	/** qchain_regs_txbase: TX descriptor array address */
	u32 txbase;

	/** qchain_regs_rxbase: RX descriptor array address */
	u32 rxbase;

	/** qchain_regs_tx_cpui: TX ring CPU (driver) index */
	u32 tx_cpui;

	/** qchain_regs_tx_hwi: TX ring hardware index */
	u32 tx_hwi;

	/** qchain_regs_rx_cpui: RX ring CPU (driver) index */
	u32 rx_cpui;

	/** qchain_regs_rx_hwi: TX ring hardware index */
	u32 rx_hwi;

};

/** qregs: QDMA Registers */
struct qregs {
	/** qregs_version:  */
	u32 version;

	/** qregs_qcfg:  */
	struct qregs_qcfg {
		/**
		 * See accessors:
		 * is_qregs_qcfg_rx_2b_offset()
		 * set_qregs_qcfg_rx_2b_offset()
		 * get_qregs_qcfg_dma_pref()
		 * set_qregs_qcfg_dma_pref()
		 * is_qregs_qcfg_msg_word_swap()
		 * set_qregs_qcfg_msg_word_swap()
		 * is_qregs_qcfg_dscp_byte_swap()
		 * set_qregs_qcfg_dscp_byte_swap()
		 * is_qregs_qcfg_payload_byte_sw()
		 * set_qregs_qcfg_payload_byte_sw()
		 * is_qregs_qcfg_vchnl_map_en()
		 * set_qregs_qcfg_vchnl_map_en()
		 * is_qregs_qcfg_vchnl_map_mode()
		 * set_qregs_qcfg_vchnl_map_mode()
		 * is_qregs_qcfg_qdma_lpbk_rxq_sel()
		 * set_qregs_qcfg_qdma_lpbk_rxq_sel()
		 * is_qregs_qcfg_slm_release_en()
		 * set_qregs_qcfg_slm_release_en()
		 * is_qregs_qcfg_tx_immediate_done()
		 * set_qregs_qcfg_tx_immediate_done()
		 * is_qregs_qcfg_irq_en()
		 * set_qregs_qcfg_irq_en()
		 * is_qregs_qcfg_gdm_loopback()
		 * set_qregs_qcfg_gdm_loopback()
		 * is_qregs_qcfg_qdma_loopback()
		 * set_qregs_qcfg_qdma_loopback()
		 * is_qregs_qcfg_check_done()
		 * set_qregs_qcfg_check_done()
		 * is_qregs_qcfg_tx_wb_done()
		 * set_qregs_qcfg_tx_wb_done()
		 * get_qregs_qcfg_burst_size()
		 * set_qregs_qcfg_burst_size()
		 * is_qregs_qcfg_rx_dma_busy()
		 * set_qregs_qcfg_rx_dma_busy()
		 * is_qregs_qcfg_rx_dma_en()
		 * set_qregs_qcfg_rx_dma_en()
		 * is_qregs_qcfg_tx_dma_busy()
		 * set_qregs_qcfg_tx_dma_busy()
		 * is_qregs_qcfg_tx_dma_en()
		 * set_qregs_qcfg_tx_dma_en()
		 */
		u32 bitfield_0;

	} qdma_cfg;

	/** qregs_qchain0:  */
	struct qchain_regs qchain0;

	/**
	 * qregs_hwf_desc_addr: Hardware forwarding descriptor table address. This
	 * memory must be forward-descriptor-size (16 bytes) * fwd_desc_n in size.
	 */
	u32 hwf_desc_addr;

	/**
	 * qregs_hwf_data_addr: Hardware forwarding packet content address. This memory
	 * must be pkt_sz *  fwd_desc_n in size.
	 */
	u32 hwf_data_addr;

	/**
	 * See accessors:
	 * get_qregs_hwf_cfg_pkt_sz()
	 * set_qregs_hwf_cfg_pkt_sz()
	 * get_qregs_hwf_cfg_low_th()
	 * set_qregs_hwf_cfg_low_th()
	 */
	struct hwf_cfg { u32 word; } hwf_cfg;

	u32 unused_0;

	/**
	 * qregs_hwf_cfg1: Hardware forwarding configuration (also called LMGR)
	 * See accessors:
	 * is_qregs_hwf_cfg1_start()
	 * set_qregs_hwf_cfg1_start()
	 * is_qregs_hwf_cfg1_overhead_en()
	 * set_qregs_hwf_cfg1_overhead_en()
	 * get_qregs_hwf_cfg1_overhead()
	 * set_qregs_hwf_cfg1_overhead()
	 * get_qregs_hwf_cfg1_fwd_desc_n()
	 * set_qregs_hwf_cfg1_fwd_desc_n()
	 */
	struct qregs_hwf_cfg1 { u32 word; } hwf_cfg1;

	u8 unused_1[12];

	/** qregs_channel_retire: Referred to as QDMA_CSR_LMGR_CHNL_RETIRE */
	u32 channel_retire;

	u8 unused_2[12];

	/**
	 * qregs_int_status: When an interrupt is triggered, these are the pending
	 * events
	 */
	u32 int_status;

	/** qregs_int_enable: Enabled interrupts */
	u32 int_enable;

	/** qregs_tx_int_delay: Interrupt delay for reducing interrupt load */
	u32 tx_int_delay;

	/** qregs_rx_int_delay: Interrupt delay for reducing interrupt load */
	u32 rx_int_delay;

	/** qregs_doneq:  */
	struct qregs_doneq {
		/** qregs_doneq_addr:  */
		u32 address;

		/**
		 * qregs_doneq_cfg:
		 * See accessors:
		 * get_qregs_doneq_cfg_int_threshold()
		 * set_qregs_doneq_cfg_int_threshold()
		 * get_qregs_doneq_cfg_size()
		 * set_qregs_doneq_cfg_size()
		 */
		struct qregs_doneq_cfg { u32 word; } config;

		/**
		 * qregs_doneq_pop_back: Pop this number of items from the back of the the
		 * done queue, max 255
		 */
		u32 pop_back;

		/**
		 * qregs_doneq_state:
		 * See accessors:
		 * get_qregs_doneq_state_length()
		 * get_qregs_doneq_state_head_index()
		 */
		struct qregs_doneq_state { u32 word; } state;

		/**
		 * qregs_doneq_wait_time: If there is anything in the queue, fire an interrupt
		 * after this number of units of time, unit is 20 microseconds.
		 */
		u32 wait_time;

	} done_queue;

	u8 unused_3[12];

	/**
	 * See accessors:
	 * is_qregs_wrr_mode_use_16b()
	 * set_qregs_wrr_mode_use_16b()
	 * is_qregs_wrr_mode_by_byte()
	 * set_qregs_wrr_mode_by_byte()
	 */
	struct wrr_mode { u32 word; } wrr_mode;

	u32 unused_4;

	/**
	 * qregs_wrr_weight: Read and write a WRR weight for a channel+queue pair.
	 * Called QDMA_CSR_TXWRR_WEIGHT_CFG.
	 */
	struct qregs_wrr_weight {
		/**
		 * See accessors:
		 * set_qregs_wrr_weight_write()
		 * is_qregs_wrr_weight_done()
		 * set_qregs_wrr_weight_channel()
		 * set_qregs_wrr_weight_queue()
		 */
		u16 bitfield_0;

		u8 unused_0;

		/**
		 * qregs_wrr_weight_value: The WRR weight to read or write for the configured
		 * channel+queue.
		 */
		u8 value;

	} wrr_weight;

	u32 unused_5;

	/**
	 * qregs_buf_usage_cfg: Called QDMA_CSR_PSE_BUF_USAGE_CFG, a bitfield, TODO
	 * document
	 */
	u32 buf_usage_cfg;

	/**
	 * qregs_tx_meter_cfg: Called QDMA_CSR_EGRESS_RATEMETER_CFG, a bitfield, TODO
	 * document
	 */
	u32 tx_meter_cfg;

	/**
	 * qregs_tx_limit_cfg: Called QDMA_CSR_EGRESS_RATELIMIT_CFG, a bitfield, TODO
	 * document
	 */
	u32 tx_limit_cfg;

	/**
	 * qregs_tx_limit_param: Called QDMA_CSR_RATELIMIT_PARAMETER_CFG, a bitfield,
	 * per-channel tx rate limit, TODO document
	 */
	u32 tx_limit_param;

	/** qregs_tx_congest_cfg:  */
	struct qregs_tx_congest_cfg {
		/**
		 * See accessors:
		 * is_qregs_tx_congest_cfg_tail_drop_en()
		 * set_qregs_tx_congest_cfg_tail_drop_en()
		 * is_qregs_tx_congest_cfg_dei_drop_en()
		 * set_qregs_tx_congest_cfg_dei_drop_en()
		 * is_qregs_tx_congest_cfg_dyncong_en()
		 * set_qregs_tx_congest_cfg_dyncong_en()
		 * is_qregs_tx_congest_cfg_max_thr_blk_tx1()
		 * set_qregs_tx_congest_cfg_max_thr_blk_tx1()
		 * is_qregs_tx_congest_cfg_min_thr_blk_tx1()
		 * set_qregs_tx_congest_cfg_min_thr_blk_tx1()
		 * is_qregs_tx_congest_cfg_max_thr_blk_tx0()
		 * set_qregs_tx_congest_cfg_max_thr_blk_tx0()
		 * is_qregs_tx_congest_cfg_min_thr_blk_tx0()
		 * set_qregs_tx_congest_cfg_min_thr_blk_tx0()
		 * get_qregs_tx_congest_cfg_dyncong_margin()
		 * set_qregs_tx_congest_cfg_dyncong_margin()
		 * get_qregs_tx_congest_cfg_dyncong_dei_scale()
		 * set_qregs_tx_congest_cfg_dyncong_dei_scale()
		 * is_qregs_tx_congest_cfg_dyncong_upd_wrr()
		 * set_qregs_tx_congest_cfg_dyncong_upd_wrr()
		 * is_qregs_tx_congest_cfg_dyncong_upd_txrx()
		 * set_qregs_tx_congest_cfg_dyncong_upd_txrx()
		 * is_qregs_tx_congest_cfg_dyncong_upd_tick()
		 * set_qregs_tx_congest_cfg_dyncong_upd_tick()
		 */
		u16 bitfield_0;

		/**
		 * qregs_tx_congest_cfg_dyncong_tick: Dynamic congestion ticker rate in
		 * microseconds
		 */
		u16 dyncong_tick;

	} tx_congest_cfg;

	/** qregs_tx_congest_thr:  */
	struct qregs_tx_congest_thr {
		/**
		 * qregs_tx_congest_thr_max: When total buffer usage exceeds this, drop all
		 * packets except VIP
		 */
		u16 max;

		/**
		 * qregs_tx_congest_thr_min: When total buffer usage exceeds this, dynamic
		 * congestion control will start
		 */
		u16 min;

	} tx_congest_thr;

	/**
	 * qregs_tx_per_ch_dthr: Called QDMA_CSR_TXQ_DYN_CHNLTHR_CFG, tx per-channel
	 * dynamic threshold max/min threshold, TODO document
	 */
	u32 tx_per_ch_dthr;

	/**
	 * qregs_tx_per_q_dthr: Called QDMA_CSR_TXQ_DYN_QUEUETHR_CFG, tx per-queue
	 * dynamic threshold max/min threshold, TODO document
	 */
	u32 tx_per_q_dthr;

	/**
	 * qregs_tx_per_q_sthr: Called QDMA_CSR_STATIC_QUEUE_THR(0..7), tx per-queue
	 * static thresholds, if dynamic thresholds are disabled, TODO document
	 */
	u8 unused_6[32];

	u8 unused_7[16];

	/** qregs_debug:  */
	struct qregs_debug {
		/**
		 * See accessors:
		 * set_qregs_debug_mem_ctl_write()
		 * is_qregs_debug_mem_ctl_done()
		 * set_qregs_debug_mem_ctl_dataset()
		 * set_qregs_debug_mem_ctl_word_of_elem()
		 * set_qregs_debug_mem_ctl_elem()
		 */
		struct mem_ctl { u32 word; } mem_ctl;

		/**
		 * qregs_debug_mem_lo: Read or write the low bits of a memory element (use
		 * with mem_ctl) Called QDMA_CSR_DBG_MEM_XS_DATA_LO.
		 */
		u32 mem_lo;

		/**
		 * qregs_debug_mem_hi: Read or write the high bits of a memory element (use
		 * with mem_ctl) Called QDMA_CSR_DBG_MEM_XS_DATA_HI. Not used in practice.
		 */
		u32 mem_hi;

		u32 unused_0;

		/**
		 * qregs_debug_hwf_desc_free: Called QDMA_CSR_DBG_LMGR_STATUS, number of free
		 * hardware forwarding descriptors
		 */
		u32 hwf_desc_free;

		/**
		 * qregs_debug_hwd_buf_used: Number of bytes of buffer used for hardwre
		 * forwarding
		 */
		u32 hwd_buf_used;

		/**
		 * qregs_debug_probe_lo: Called QDMA_CSR_DBG_QDMA_PROBE_LO, unknown usage,
		 * TODO document
		 */
		u32 probe_lo;

		/**
		 * qregs_debug_probe_hi: Called QDMA_CSR_DBG_QDMA_PROBE_HI, unknown usage,
		 * TODO document
		 */
		u32 probe_hi;

	} debug;

	/**
	 * qregs_rxring_size:
	 * See accessors:
	 * get_qregs_rxring_size_ring0()
	 * set_qregs_rxring_size_ring0()
	 * get_qregs_rxring_size_ring1()
	 * set_qregs_rxring_size_ring1()
	 */
	struct qregs_rxring_size { u32 word; } rxring_size;

	/**
	 * qregs_rxring_low:
	 * See accessors:
	 * get_qregs_rxring_low_ring0()
	 * set_qregs_rxring_low_ring0()
	 * get_qregs_rxring_low_ring1()
	 * set_qregs_rxring_low_ring1()
	 */
	struct qregs_rxring_low { u32 word; } rxring_low;

	/** qregs_qchain1:  */
	struct qchain_regs qchain1;

	/** qregs_cpu_rx_limit: CPU protection RX limit, TODO document */
	u32 cpu_rx_limit;

	/** qregs_cpu_rx_limit_val: CPU protection RX limit, TODO document */
	u32 cpu_rx_limit_val;

	u8 unused_8[20];

	/** qregs_vch_wrr: Virtual channel WRR weighting, TODO document */
	u32 vch_wrr;

	/** qregs_vch_qmode: Virtual channel QoS mode (WRR / SP), TODO document */
	u32 vch_qmode;

	u8 unused_9[28];

	/**
	 * qregs_ch_lim_en: Per-channel rate-limit enable, each bit corriponds to one
	 * channel
	 */
	u32 ch_lim_en;

	u8 unused_10[28];

	/**
	 * qregs_ch_qmode: Per-channel queue prioritization mode (WRR / SP), TODO
	 * document
	 */
	u8 unused_11[16];

	u8 unused_12[112];

	/**
	 * qregs_ch_tx_rate: Channel data rate, each word corrisponds to 2 channels,
	 * upper 16 bits is the odd channel number, lower 16 is the even.
	 */
	u8 unused_13[64];

	u8 unused_14[64];

	/**
	 * qregs_ch_drop: Drop counter for normal packets, each byte corrisponds to one
	 * of the 32 channels.
	 */
	u8 unused_15[32];

	u8 unused_16[32];

	/**
	 * qregs_ch_dei_drop: Drop counter for Drop-Elligable (DEI) packets, each byte
	 * corrisponds to one of the 32 channels.
	 */
	u8 unused_17[32];

	u8 unused_18[32];

	/**
	 * qregs_pkt_ctrs: Every other word is a counter config and a counter value,
	 * called QDMA_CSR_DBG_CNTR_CFG / QDMA_CSR_DBG_CNTR_VAR. Definitely 40
	 * counters, possible 64. TODO document
	 */
	u8 unused_19[512];

	u8 unused_20[2816];

};

/**
 * Bitfield accessors for: qregs_qcfg bitfield_0
 */

enum qregs_qcfg_dma_pref {
	QREGS_QCFG_DMA_PREF_ROUND_ROBIN			= 0,
	QREGS_QCFG_DMA_PREF_FRX_TX1_TX0			= 1,
	QREGS_QCFG_DMA_PREF_TX1_FRX_TX0			= 2,
	QREGS_QCFG_DMA_PREF_TX1_TX0_FRX			= 3,
};
enum qregs_qcfg_burst_size {
	QREGS_QCFG_BURST_SIZE_16_BYTES			= 0,
	QREGS_QCFG_BURST_SIZE_32_BYTES			= 1,
	QREGS_QCFG_BURST_SIZE_64_BYTES			= 2,
	QREGS_QCFG_BURST_SIZE_128_BYTES			= 3,
};

#define QREGS_QCFG_RX_2B_OFFSET				BIT(31)
#define QREGS_QCFG_DMA_PREF_MASK			GENMASK(30, 29)
#define QREGS_QCFG_MSG_WORD_SWAP			BIT(28)
#define QREGS_QCFG_DSCP_BYTE_SWAP			BIT(27)
#define QREGS_QCFG_PAYLOAD_BYTE_SW			BIT(26)
#define QREGS_QCFG_VCHNL_MAP_EN				BIT(25)
#define QREGS_QCFG_VCHNL_MAP_MODE			BIT(24)
#define QREGS_QCFG_QDMA_LPBK_RXQ_SEL			BIT(22)
#define QREGS_QCFG_SLM_RELEASE_EN			BIT(21)
#define QREGS_QCFG_TX_IMMEDIATE_DONE			BIT(20)
#define QREGS_QCFG_IRQ_EN				BIT(19)
#define QREGS_QCFG_GDM_LOOPBACK				BIT(17)
#define QREGS_QCFG_QDMA_LOOPBACK			BIT(16)
#define QREGS_QCFG_CHECK_DONE				BIT(7)
#define QREGS_QCFG_TX_WB_DONE				BIT(6)
#define QREGS_QCFG_BURST_SIZE_MASK			GENMASK(5, 4)
#define QREGS_QCFG_RX_DMA_BUSY				BIT(3)
#define QREGS_QCFG_RX_DMA_EN				BIT(2)
#define QREGS_QCFG_TX_DMA_BUSY				BIT(1)
#define QREGS_QCFG_TX_DMA_EN				BIT(0)


/** If enabled, use (dscp_pkt_ptr + 2) as starting address for rx payload */
static inline bool is_qregs_qcfg_rx_2b_offset(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_RX_2B_OFFSET, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_2b_offset(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_2B_OFFSET, v);
}

/** DMA channel scheduling preference, FRX means "Forwarding and RX" */
static inline enum qregs_qcfg_dma_pref get_qregs_qcfg_dma_pref(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_DMA_PREF_MASK, x->bitfield_0);
}
static inline void set_qregs_qcfg_dma_pref(struct qregs_qcfg *x, enum qregs_qcfg_dma_pref v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_DMA_PREF_MASK, v);
}

/**
 * Enable message word swap, don't know what this does but every implementation
 * sets it on Big Endian.
 */
static inline bool is_qregs_qcfg_msg_word_swap(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_MSG_WORD_SWAP, x->bitfield_0);
}
static inline void set_qregs_qcfg_msg_word_swap(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_MSG_WORD_SWAP, v);
}

/**
 * Endian-swap packet descriptors (?), drivers always set this on Big Endian
 * machines.
 */
static inline bool is_qregs_qcfg_dscp_byte_swap(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_DSCP_BYTE_SWAP, x->bitfield_0);
}
static inline void set_qregs_qcfg_dscp_byte_swap(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_DSCP_BYTE_SWAP, v);
}

/**
 * Endian-swap payload bytes, drivers always set this on Big Endian machines.
 */
static inline bool is_qregs_qcfg_payload_byte_sw(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_PAYLOAD_BYTE_SW, x->bitfield_0);
}
static inline void set_qregs_qcfg_payload_byte_sw(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_PAYLOAD_BYTE_SW, v);
}

/** Enable virtual mapping to group queues per physical channel */
static inline bool is_qregs_qcfg_vchnl_map_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_VCHNL_MAP_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_vchnl_map_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_VCHNL_MAP_EN, v);
}

/** Map of 4 virtual channels per physical channel, 0 = map 2 */
static inline bool is_qregs_qcfg_vchnl_map_mode(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_VCHNL_MAP_MODE, x->bitfield_0);
}
static inline void set_qregs_qcfg_vchnl_map_mode(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_VCHNL_MAP_MODE, v);
}

/**
 * If enabled, qdma loopback goes to queue 1, otherwise it goes to queue zero
 */
static inline bool is_qregs_qcfg_qdma_lpbk_rxq_sel(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_QDMA_LPBK_RXQ_SEL, x->bitfield_0);
}
static inline void set_qregs_qcfg_qdma_lpbk_rxq_sel(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_QDMA_LPBK_RXQ_SEL, v);
}

/** Enable qdma fwd path release slm_block */
static inline bool is_qregs_qcfg_slm_release_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_SLM_RELEASE_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_slm_release_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_SLM_RELEASE_EN, v);
}

/** QDMA generate pkt_done itself instead of using pse pkt_done */
static inline bool is_qregs_qcfg_tx_immediate_done(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_IMMEDIATE_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_immediate_done(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_IMMEDIATE_DONE, v);
}

/** Enable "interrupt queue" (i.e. Done List) for tx dma done */
static inline bool is_qregs_qcfg_irq_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_IRQ_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_irq_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_IRQ_EN, v);
}

/** Enable gdm loopback tx packet to rx path */
static inline bool is_qregs_qcfg_gdm_loopback(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_GDM_LOOPBACK, x->bitfield_0);
}
static inline void set_qregs_qcfg_gdm_loopback(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_GDM_LOOPBACK, v);
}

/** Enable hw qdma loopback tx packet to rx path */
static inline bool is_qregs_qcfg_qdma_loopback(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_QDMA_LOOPBACK, x->bitfield_0);
}
static inline void set_qregs_qcfg_qdma_loopback(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_QDMA_LOOPBACK, v);
}

/**
 * Check the done bit of descriptor and don't use descriptors which are marked
 * done. If disabled, the QDMA engine will determine if a descriptor is usable
 * based only on the ring pointers.
 */
static inline bool is_qregs_qcfg_check_done(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_CHECK_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_check_done(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_CHECK_DONE, v);
}

/**
 * Set the "done" bit in tx descriptor after sending. If disabled then the
 * engine will skip setting the done bit and rely on the driver to check the
 * Done List (i.e. `irq_en`).
 */
static inline bool is_qregs_qcfg_tx_wb_done(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_WB_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_wb_done(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_WB_DONE, v);
}

/** Number of bytes per DMA burst */
static inline enum qregs_qcfg_burst_size get_qregs_qcfg_burst_size(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_BURST_SIZE_MASK, x->bitfield_0);
}
static inline void set_qregs_qcfg_burst_size(struct qregs_qcfg *x, enum qregs_qcfg_burst_size v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_BURST_SIZE_MASK, v);
}

/** RX DMA engine currently busy */
static inline bool is_qregs_qcfg_rx_dma_busy(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_RX_DMA_BUSY, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_dma_busy(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_DMA_BUSY, v);
}

/** Enable RX DMA */
static inline bool is_qregs_qcfg_rx_dma_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_RX_DMA_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_dma_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_DMA_EN, v);
}

/** TX DMA engine currently busy */
static inline bool is_qregs_qcfg_tx_dma_busy(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_DMA_BUSY, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_dma_busy(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_DMA_BUSY, v);
}

/** Enable TX DMA */
static inline bool is_qregs_qcfg_tx_dma_en(struct qregs_qcfg *x)
{
	return FIELD_GET(QREGS_QCFG_TX_DMA_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_dma_en(struct qregs_qcfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_DMA_EN, v);
}

/**
 * Bitfield accessors for: struct hwf_cfg
 * Hardware forwarding configuration
 */

enum qregs_hwf_cfg_pkt_sz {
	QREGS_HWF_CFG_PKT_SZ_2048			= 0,
	QREGS_HWF_CFG_PKT_SZ_4096			= 1,
	QREGS_HWF_CFG_PKT_SZ_8192			= 2,
	QREGS_HWF_CFG_PKT_SZ_16384			= 3,
};

#define QREGS_HWF_CFG_PKT_SZ_MASK			GENMASK(29, 28)
#define QREGS_HWF_CFG_LOW_TH_MASK			GENMASK(12, 0)


/**
 * The size of the packet buffers in hwf_data_addr (and therefore the maximum
 * effective MTU).
 */
static inline enum qregs_hwf_cfg_pkt_sz get_qregs_hwf_cfg_pkt_sz(struct hwf_cfg *x)
{
	return FIELD_GET(QREGS_HWF_CFG_PKT_SZ_MASK, x->word);
}
static inline void set_qregs_hwf_cfg_pkt_sz(struct hwf_cfg *x, enum qregs_hwf_cfg_pkt_sz v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG_PKT_SZ_MASK, v);
}

/**
 * When number of available (not busy) hardware descriptors is below this,
 * generate an interrupt and pause hardware forwarding.
 */
static inline u16 get_qregs_hwf_cfg_low_th(struct hwf_cfg *x)
{
	return FIELD_GET(QREGS_HWF_CFG_LOW_TH_MASK, x->word);
}
static inline void set_qregs_hwf_cfg_low_th(struct hwf_cfg *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG_LOW_TH_MASK, v);
}

/**
 * Bitfield accessors for: qregs_hwf_cfg1
 * Register layout (32-bit word):
 * - Bit 31: START
 * - Bit 24: OVERHEAD_EN
 * - Bits 23-16: OVERHEAD (8 bits)
 * - Bits 15-0: FWD_DESC_N (16 bits)
 */

#define QREGS_HWF_CFG1_START				BIT(31)
#define QREGS_HWF_CFG1_OVERHEAD_EN			BIT(24)
#define QREGS_HWF_CFG1_OVERHEAD_MASK			GENMASK(23, 16)
#define QREGS_HWF_CFG1_FWD_DESC_N_MASK			GENMASK(15, 0)


/** Start up the hardware forwarding subsystem */
static inline bool is_qregs_hwf_cfg1_start(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_START, x->word);
}
static inline void set_qregs_hwf_cfg1_start(struct qregs_hwf_cfg1 *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_START, v);
}

/** When set, add overhead to packet size for accounting purposes */
static inline bool is_qregs_hwf_cfg1_overhead_en(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_OVERHEAD_EN, x->word);
}
static inline void set_qregs_hwf_cfg1_overhead_en(struct qregs_hwf_cfg1 *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_OVERHEAD_EN, v);
}

/** Amount of overhead to add to packet size for accounting */
static inline u8 get_qregs_hwf_cfg1_overhead(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_OVERHEAD_MASK, x->word);
}
static inline void set_qregs_hwf_cfg1_overhead(struct qregs_hwf_cfg1 *x, u8 v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_OVERHEAD_MASK, v);
}

/** Number of forward descriptors to use */
static inline u16 get_qregs_hwf_cfg1_fwd_desc_n(struct qregs_hwf_cfg1 *x)
{
	return FIELD_GET(QREGS_HWF_CFG1_FWD_DESC_N_MASK, x->word);
}
static inline void set_qregs_hwf_cfg1_fwd_desc_n(struct qregs_hwf_cfg1 *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG1_FWD_DESC_N_MASK, v);
}

/**
 * Bitfield accessors for: struct wrr_mode
 * WRR control register, called QDMA_CSR_TXWRR_MODE_CFG.
 */

#define QREGS_WRR_MODE_USE_16B				BIT(31)
#define QREGS_WRR_MODE_BY_BYTE				BIT(3)


/** If enabled, weighting is based on 16 byte units, otherwise 64 byte. */
static inline bool is_qregs_wrr_mode_use_16b(struct wrr_mode *x)
{
	return FIELD_GET(QREGS_WRR_MODE_USE_16B, x->word);
}
static inline void set_qregs_wrr_mode_use_16b(struct wrr_mode *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_WRR_MODE_USE_16B, v);
}

/** If enabled, weighting is by byte, otherwise by packet. */
static inline bool is_qregs_wrr_mode_by_byte(struct wrr_mode *x)
{
	return FIELD_GET(QREGS_WRR_MODE_BY_BYTE, x->word);
}
static inline void set_qregs_wrr_mode_by_byte(struct wrr_mode *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_WRR_MODE_BY_BYTE, v);
}

/**
 * Bitfield accessors for: qregs_wrr_weight bitfield_0
 */

#define QREGS_WRR_WEIGHT_WRITE				BIT(15)
#define QREGS_WRR_WEIGHT_DONE				BIT(14)
#define QREGS_WRR_WEIGHT_CHANNEL_MASK			GENMASK(7, 3)
#define QREGS_WRR_WEIGHT_QUEUE_MASK			GENMASK(2, 0)

static inline void set_qregs_wrr_weight_write(struct qregs_wrr_weight *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_WRR_WEIGHT_WRITE, v);
}

/**
 * After performing an operation, this bit asserts from the hardware to indicate
 * the operation is done.
 */
static inline bool is_qregs_wrr_weight_done(struct qregs_wrr_weight *x)
{
	return FIELD_GET(QREGS_WRR_WEIGHT_DONE, x->bitfield_0);
}
static inline void set_qregs_wrr_weight_channel(struct qregs_wrr_weight *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_WRR_WEIGHT_CHANNEL_MASK, v);
}
static inline void set_qregs_wrr_weight_queue(struct qregs_wrr_weight *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_WRR_WEIGHT_QUEUE_MASK, v);
}

/**
 * Bitfield accessors for: qregs_tx_congest_cfg bitfield_0
 * Configuration for TX congestion dropping
 */

enum qregs_tx_congest_cfg_dyncong_margin {
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_0_PCT	= 0,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_25_PCT	= 1,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_50_PCT	= 2,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_100_PCT	= 3,
};
enum qregs_tx_congest_cfg_dyncong_dei_scale {
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_HALF	= 0,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_QUARTER	= 1,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_EIGHTH	= 2,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_SIXTEENTH = 3,
};

#define QREGS_TX_CONGEST_CFG_TAIL_DROP_EN		BIT(15)
#define QREGS_TX_CONGEST_CFG_DEI_DROP_EN		BIT(14)
#define QREGS_TX_CONGEST_CFG_DYNCONG_EN			BIT(13)
#define QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1		BIT(11)
#define QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1		BIT(10)
#define QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0		BIT(9)
#define QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0		BIT(8)
#define QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK	GENMASK(7, 6)
#define QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK	GENMASK(5, 4)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR		BIT(2)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX		BIT(1)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK		BIT(0)

static inline bool is_qregs_tx_congest_cfg_tail_drop_en(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_TAIL_DROP_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_tail_drop_en(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_TAIL_DROP_EN, v);
}

/** Support 802.1ad DEI packet dropping */
static inline bool is_qregs_tx_congest_cfg_dei_drop_en(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DEI_DROP_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dei_drop_en(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DEI_DROP_EN, v);
}

/** Enable dynamic congestion algorithm */
static inline bool is_qregs_tx_congest_cfg_dyncong_en(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_en(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_EN, v);
}

/**
 * Block TX Ring1 when TX buffer usage exceeds max threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_max_thr_blk_tx1(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_max_thr_blk_tx1(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1, v);
}

/**
 * Block TX Ring1 when TX buffer usage exceeds min threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_min_thr_blk_tx1(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_min_thr_blk_tx1(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1, v);
}

/**
 * Block TX Ring0 when TX buffer usage exceeds max threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_max_thr_blk_tx0(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_max_thr_blk_tx0(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0, v);
}

/**
 * Block TX Ring0 when TX buffer usage exceeds min threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_min_thr_blk_tx0(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_min_thr_blk_tx0(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0, v);
}
static inline enum qregs_tx_congest_cfg_dyncong_margin get_qregs_tx_congest_cfg_dyncong_margin(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_margin(struct qregs_tx_congest_cfg *x, enum qregs_tx_congest_cfg_dyncong_margin v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK, v);
}
static inline enum qregs_tx_congest_cfg_dyncong_dei_scale get_qregs_tx_congest_cfg_dyncong_dei_scale(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_dei_scale(struct qregs_tx_congest_cfg *x, enum qregs_tx_congest_cfg_dyncong_dei_scale v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK, v);
}

/** Update dyanmic congestion after each update to WRR weights. */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_wrr(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_wrr(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR, v);
}

/** Update dyanmic congestion after each TX or RX */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_txrx(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_txrx(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX, v);
}

/** Update dyanmic congestion after each tick */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_tick(struct qregs_tx_congest_cfg *x)
{
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_tick(struct qregs_tx_congest_cfg *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK, v);
}

/**
 * Bitfield accessors for: struct mem_ctl
 * Called QDMA_CSR_DBG_MEM_XS_CFG, used for debugging access to memory spaces.
 */

enum qregs_debug_mem_ctl_dataset {
	QREGS_DEBUG_MEM_CTL_DATASET_DESC		= 0,
	QREGS_DEBUG_MEM_CTL_DATASET_QUEUE		= 1,
	QREGS_DEBUG_MEM_CTL_DATASET_QOS_WEIGHT_CTR	= 2,
	QREGS_DEBUG_MEM_CTL_DATASET_DMA_IDX		= 3,
	QREGS_DEBUG_MEM_CTL_DATASET_BUF_MON		= 4,
	QREGS_DEBUG_MEM_CTL_DATASET_RL_PARAM		= 5,
	QREGS_DEBUG_MEM_CTL_DATASET_VCH_WEIGHT		= 6,
};

#define QREGS_DEBUG_MEM_CTL_WRITE			BIT(31)
#define QREGS_DEBUG_MEM_CTL_DONE			BIT(30)
#define QREGS_DEBUG_MEM_CTL_DATASET_MASK		GENMASK(26, 24)
#define QREGS_DEBUG_MEM_CTL_WORD_OF_ELEM_MASK		GENMASK(20, 16)
#define QREGS_DEBUG_MEM_CTL_ELEM_MASK			GENMASK(15, 0)

static inline void set_qregs_debug_mem_ctl_write(struct mem_ctl *x, bool v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_WRITE, v);
}

/** Becomes set when the command is completed */
static inline bool is_qregs_debug_mem_ctl_done(struct mem_ctl *x)
{
	return FIELD_GET(QREGS_DEBUG_MEM_CTL_DONE, x->word);
}
static inline void set_qregs_debug_mem_ctl_dataset(struct mem_ctl *x, enum qregs_debug_mem_ctl_dataset v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_DATASET_MASK, v);
}
static inline void set_qregs_debug_mem_ctl_word_of_elem(struct mem_ctl *x, u8 v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_WORD_OF_ELEM_MASK, v);
}
static inline void set_qregs_debug_mem_ctl_elem(struct mem_ctl *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_DEBUG_MEM_CTL_ELEM_MASK, v);
}

/**
 * Bitfield accessors for: qregs_doneq_cfg
 * Register layout (32-bit word):
 * - Bits 31-16: INT_THRESHOLD (when done queue is this full, fire interrupt)
 * - Bits 15-0: SIZE (size of done queue buffer in 4-byte units)
 */
#define QREGS_DONEQ_CFG_INT_THRESHOLD_MASK		GENMASK(31, 16)
#define QREGS_DONEQ_CFG_SIZE_MASK			GENMASK(15, 0)

static inline u16 get_qregs_doneq_cfg_int_threshold(struct qregs_doneq_cfg *x)
{
	return FIELD_GET(QREGS_DONEQ_CFG_INT_THRESHOLD_MASK, x->word);
}
static inline void set_qregs_doneq_cfg_int_threshold(struct qregs_doneq_cfg *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_DONEQ_CFG_INT_THRESHOLD_MASK, v);
}
static inline u16 get_qregs_doneq_cfg_size(struct qregs_doneq_cfg *x)
{
	return FIELD_GET(QREGS_DONEQ_CFG_SIZE_MASK, x->word);
}
static inline void set_qregs_doneq_cfg_size(struct qregs_doneq_cfg *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_DONEQ_CFG_SIZE_MASK, v);
}

/**
 * Bitfield accessors for: qregs_doneq_state
 * Register layout (32-bit word):
 * - Bits 31-16: LENGTH (number of items waiting in the queue)
 * - Bits 15-0: HEAD_INDEX (index of the first item in the list)
 */
#define QREGS_DONEQ_STATE_LENGTH_MASK			GENMASK(31, 16)
#define QREGS_DONEQ_STATE_HEAD_INDEX_MASK		GENMASK(15, 0)

static inline u16 get_qregs_doneq_state_length(struct qregs_doneq_state *x)
{
	return FIELD_GET(QREGS_DONEQ_STATE_LENGTH_MASK, x->word);
}
static inline u16 get_qregs_doneq_state_head_index(struct qregs_doneq_state *x)
{
	return FIELD_GET(QREGS_DONEQ_STATE_HEAD_INDEX_MASK, x->word);
}

/**
 * Bitfield accessors for: qregs_rxring_size
 * Register layout (32-bit word):
 * - Bits 31-16: RING0_SIZE (size of RX ring 0, max 4095)
 * - Bits 15-0: RING1_SIZE (size of RX ring 1, max 4095)
 */
#define QREGS_RXRING_SIZE_RING0_MASK			GENMASK(31, 16)
#define QREGS_RXRING_SIZE_RING1_MASK			GENMASK(15, 0)

static inline u16 get_qregs_rxring_size_ring0(struct qregs_rxring_size *x)
{
	return FIELD_GET(QREGS_RXRING_SIZE_RING0_MASK, x->word);
}
static inline void set_qregs_rxring_size_ring0(struct qregs_rxring_size *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_SIZE_RING0_MASK, v);
}
static inline u16 get_qregs_rxring_size_ring1(struct qregs_rxring_size *x)
{
	return FIELD_GET(QREGS_RXRING_SIZE_RING1_MASK, x->word);
}
static inline void set_qregs_rxring_size_ring1(struct qregs_rxring_size *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_SIZE_RING1_MASK, v);
}

/**
 * Bitfield accessors for: qregs_rxring_low
 * Register layout (32-bit word):
 * - Bits 31-16: RING0_LOW (interrupt threshold for ring 0, max 4095)
 * - Bits 15-0: RING1_LOW (interrupt threshold for ring 1, max 4095)
 */
#define QREGS_RXRING_LOW_RING0_MASK			GENMASK(31, 16)
#define QREGS_RXRING_LOW_RING1_MASK			GENMASK(15, 0)

static inline u16 get_qregs_rxring_low_ring0(struct qregs_rxring_low *x)
{
	return FIELD_GET(QREGS_RXRING_LOW_RING0_MASK, x->word);
}
static inline void set_qregs_rxring_low_ring0(struct qregs_rxring_low *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_LOW_RING0_MASK, v);
}
static inline u16 get_qregs_rxring_low_ring1(struct qregs_rxring_low *x)
{
	return FIELD_GET(QREGS_RXRING_LOW_RING1_MASK, x->word);
}
static inline void set_qregs_rxring_low_ring1(struct qregs_rxring_low *x, u16 v)
{
	x->word = FIELD_SET(x->word, QREGS_RXRING_LOW_RING1_MASK, v);
}

#endif
