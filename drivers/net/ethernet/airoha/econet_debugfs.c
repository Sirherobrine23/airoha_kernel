// SPDX-License-Identifier: GPL-2.0-only
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "econet_eth.h"
#include "econet_qdma_desc.h"

struct en75_qdma_chain_debug {
	struct dentry *descs;
	struct dentry *chan_ctrs;
	struct en75_debug_qdma_chain_conf *config;
	struct en75_qdma_debug *qdma;
	int chain_n;
};

struct en75_qdma_debug {
	struct dentry *dir;
	struct en75_debug_qdma_conf *config;
	struct en75_qdma_chain_debug chains[QDMA_NUM_CHAINS];
	struct dentry *fwdescs;
	struct dentry *regs;
	struct dentry *interrupts;
};

struct en75_debug {
	struct dentry *dir;
	struct en75_qdma_debug qdma[EN75_NUM_QDMA];
	struct en75_debug_conf config;
};

static void print_erx(struct seq_file *m, struct qdma_desc_erx *erx)
{
	seq_printf(m, "crsn=%d sport=%d ppe=%d"
		"%s%s%s%s%s%s%s",
		get_erx_crsn(erx), get_erx_sport(erx), get_erx_ppe_entry(erx),

		is_erx_ip6(erx) ? " IP6" : "",
		is_erx_ip4(erx) ? " IP4" : "",
		is_erx_ip4f(erx) ? " IP4F" : "",
		is_erx_tack(erx) ? " TACK" : "",
		is_erx_l2vld(erx) ? " L2VLD" : "",
		is_erx_l4f(erx) ? " L4F" : "",
		is_erx_untag(erx) ? " UNTAG" : ""
	);

	if (get_erx_sp_tag(erx)) {
		seq_printf(m, " sp_tag=%.4x", get_erx_sp_tag(erx));
	}
	if (get_erx_tci(erx)) {
		seq_printf(m, " tci=%.4x", get_erx_tci(erx));
	}
	if (erx->unknown0) {
		seq_printf(m, " unknown0=%.8x", erx->unknown0);
	}
	if (get_erx_unknown1(erx)) {
		seq_printf(m, " unknown1=%.2x", get_erx_unknown1(erx));
	}
	if (get_erx_unknown2(erx)) {
		seq_printf(m, " unknown2=%.8x", get_erx_unknown2(erx));
	}
}

static void print_etx(struct seq_file *m, struct etx *etx)
{
	seq_printf(m, "fport=%d"
		"%s%s%s%s%s",
		get_etx_fport(etx),

		is_etx_oam(etx) ? " OAM" : "",
		is_etx_ico(etx) ? " ICO" : "",
		is_etx_sco(etx) ? " SCO" : "",
		is_etx_tco(etx) ? " TCO" : "",
		is_etx_uco(etx) ? " UCO" : ""
	);

	if (get_etx_channel(etx)) {
		seq_printf(m, " channel=%d", get_etx_channel(etx));
	}
	if (get_etx_queue(etx)) {
		seq_printf(m, " queue=%d", get_etx_queue(etx));
	}
	if (get_etx_sp_tag(etx)) {
		seq_printf(m, " sp_tag=%.4x", get_etx_sp_tag(etx));
	}
	if (get_etx_udf_pmap(etx)) {
		seq_printf(m, " udf_pmap=%.2x", get_etx_udf_pmap(etx));
	}
	if (is_etx_vlan_en(etx)) {
		seq_printf(m, " vlan_type=%.2x", get_etx_vlan_type(etx));
	}
	if (get_etx_vlan_tag(etx)) {
		seq_printf(m, " vlan_tag=%.4x", get_etx_vlan_tag(etx));
	}
}

static void print_desc(struct seq_file *m, struct desc *desc)
{
	seq_printf(m, "len=%d\taddr=%.8x next=%d%s%s%s",
		get_desc_info_pkt_len(&desc->info),
		desc->pkt_addr,
		desc->next_idx,
		is_desc_info_done(&desc->info) ? " DONE" : "",
		is_desc_info_dropped(&desc->info) ? " DROPPED" : "",
		is_desc_info_nls(&desc->info) ? " NLS" : ""
	);
	if (desc->unknown0) {
		seq_printf(m, " unknown0=%.8x", desc->unknown0);
	}
	if (get_desc_info_unknown1(&desc->info)) {
		seq_printf(m, " unknown1=%.4x", get_desc_info_unknown1(&desc->info));
	}
}

static void print_fwdesc(struct seq_file *m, struct fwdesc *desc)
{
	seq_printf(m, "len=%d\taddr=%.8x%s",
		get_fwdesc_info_pkt_len(&desc->info),
		desc->pkt_addr,
		!is_fwdesc_info_ctx(&desc->info) ? " FWD" : ""
	);
	if (is_fwdesc_info_ctx(&desc->info)) {
		seq_printf(m, " RING=%d IDX=%d",
			   is_fwdesc_info_ctx_ring(&desc->info),
			   get_fwdesc_info_ctx_idx(&desc->info));
	}
}

static int en75_qdma_hwf(struct seq_file *m, void *v)
{
	struct en75_qdma_debug *debug = m->private;
	struct qregs __iomem *regs;
	u32 free_desc_n;
	int i;

	regs = debug->config->regs;

	struct qregs_hwf_cfg1 cfg1;

	cfg1 = en75_rreg(&regs->hwf_cfg1);
	free_desc_n = en75_rreg(&regs->debug.hwf_desc_free);

	seq_printf(m, "QDMA FWD Descriptors used=%d total=%d\n",
		get_qregs_hwf_cfg1_fwd_desc_n(&cfg1) - free_desc_n,
		get_qregs_hwf_cfg1_fwd_desc_n(&cfg1));

	for (i = 0; i < get_qregs_hwf_cfg1_fwd_desc_n(&cfg1); i++) {
		struct fwdesc *desc = &debug->config->hwf_desc[i];
		seq_printf(m, "  %d ", i);
		print_fwdesc(m, desc);
		seq_puts(m, " ");
		print_etx(m, &desc->msg.etx);
		seq_puts(m, "\n");
	}

	return 0;
}

static int en75_hwf_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_qdma_hwf, inode->i_private);
}

static const struct file_operations en75_hwf_fops = {
	.owner   = THIS_MODULE,
	.open    = en75_hwf_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void en75_qdma_qchain(struct seq_file *m, struct qchain_regs __iomem *regs)
{
	seq_printf(m, "\ttxbase=0x%llx\n", (unsigned long long)regs->txbase);
	seq_printf(m, "\trxbase=0x%llx\n", (unsigned long long)regs->rxbase);
	seq_printf(m, "\ttx_cpui=%u\n", regs->tx_cpui);
	seq_printf(m, "\ttx_hwi=%u\n", regs->tx_hwi);
	seq_printf(m, "\trx_cpui=%u\n", regs->rx_cpui);
	seq_printf(m, "\trx_hwi=%u\n", regs->rx_hwi);
}


static int en75_qdma_regs(struct seq_file *m, void *v)
{
	struct en75_qdma_debug *debug = m->private;
	struct qregs __iomem *regs;

	regs = debug->config->regs;

	seq_printf(m, "version=%.08x\n", en75_rreg(&regs->version));

	{
		struct qregs_qcfg cfg = en75_rreg(&regs->qdma_cfg);

		seq_printf(m,
			"qdma_cfg=%.08x"
			" dma_pref=%u burst_size=%u"
			"%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
			en75_word(cfg),

			/* numeric fields */
			get_qregs_qcfg_dma_pref(&cfg),
			get_qregs_qcfg_burst_size(&cfg),

			/* flags: printed only if set */
			is_qregs_qcfg_rx_2b_offset(&cfg)        ? " +rx_2b_offset"        : "",
			is_qregs_qcfg_msg_word_swap(&cfg)       ? " +msg_word_swap"       : "",
			is_qregs_qcfg_dscp_byte_swap(&cfg)      ? " +dscp_byte_swap"      : "",
			is_qregs_qcfg_payload_byte_sw(&cfg)     ? " +payload_byte_sw"     : "",
			is_qregs_qcfg_vchnl_map_en(&cfg)        ? " +vchnl_map_en"        : "",
			is_qregs_qcfg_vchnl_map_mode(&cfg)      ? " +vchnl_map_mode"      : "",
			is_qregs_qcfg_qdma_lpbk_rxq_sel(&cfg)   ? " +qdma_lpbk_rxq_sel"   : "",
			is_qregs_qcfg_slm_release_en(&cfg)      ? " +slm_release_en"      : "",
			is_qregs_qcfg_tx_immediate_done(&cfg)   ? " +tx_immediate_done"   : "",
			is_qregs_qcfg_irq_en(&cfg)              ? " +irq_en"              : "",
			is_qregs_qcfg_gdm_loopback(&cfg)        ? " +gdm_loopback"        : "",
			is_qregs_qcfg_qdma_loopback(&cfg)       ? " +qdma_loopback"       : "",
			is_qregs_qcfg_check_done(&cfg)           ? " +check_done"          : "",
			is_qregs_qcfg_tx_wb_done(&cfg)           ? " +tx_wb_done"          : "",
			is_qregs_qcfg_rx_dma_busy(&cfg)          ? " +rx_dma_busy"         : "",
			is_qregs_qcfg_rx_dma_en(&cfg)            ? " +rx_dma_en"           : "",
			is_qregs_qcfg_tx_dma_busy(&cfg)          ? " +tx_dma_busy"         : "",
			is_qregs_qcfg_tx_dma_en(&cfg)            ? " +tx_dma_en"           : ""
		);
	}

	seq_puts(m, "qchain0:\n");
	en75_qdma_qchain(m, &regs->qchain0);

	seq_printf(m, "hwf_desc_addr=%.08x\n", en75_rreg(&regs->hwf_desc_addr));
	seq_printf(m, "hwf_data_addr=%.08x\n", en75_rreg(&regs->hwf_data_addr));

	{
		struct hwf_cfg cfg = en75_rreg(&regs->hwf_cfg);

		seq_printf(m,
			"hwf_cfg=%.08x pkt_sz=%u low_th=%u\n",
			en75_word(cfg),
			get_qregs_hwf_cfg_pkt_sz(&cfg),
			get_qregs_hwf_cfg_low_th(&cfg)
		);
	}

	{
		struct qregs_hwf_cfg1 cfg = en75_rreg(&regs->hwf_cfg1);

		seq_printf(m,
			"hwf_cfg1=%.08x fwd_desc_n=%u overhead=%u%s%s\n",
			cfg.word,
			get_qregs_hwf_cfg1_fwd_desc_n(&cfg),
			get_qregs_hwf_cfg1_overhead(&cfg),
			is_qregs_hwf_cfg1_start(&cfg)       ? " +start"        : "",
			is_qregs_hwf_cfg1_overhead_en(&cfg) ? " +overhead_en" : ""
		);
	}

	seq_printf(m, "channel_retire=%.08x\n", en75_rreg(&regs->channel_retire));
	seq_printf(m, "int_status=%.08x\n", en75_rreg(&regs->int_status));
	seq_printf(m, "int_enable=%.08x\n", en75_rreg(&regs->int_enable));
	seq_printf(m, "tx_int_delay=%.08x\n", en75_rreg(&regs->tx_int_delay));
	seq_printf(m, "rx_int_delay=%.08x\n", en75_rreg(&regs->rx_int_delay));

	seq_puts(m, "done_queue:\n");
	seq_printf(m, "\taddress=%.08x\n", en75_rreg(&regs->done_queue.address));
	{
		struct qregs_doneq_cfg cfg = en75_rreg(&regs->done_queue.config);

		seq_printf(m,
			"\tconfig=%.08x size=%u int_threshold=%u\n",
			cfg.word,
			get_qregs_doneq_cfg_size(&cfg),
			get_qregs_doneq_cfg_int_threshold(&cfg)
		);
	}
	seq_printf(m, "\tpop_back=%.08x\n", en75_rreg(&regs->done_queue.pop_back));
	{
		struct qregs_doneq_state st = en75_rreg(&regs->done_queue.state);

		seq_printf(m,
			"\tstate=%.08x len=%u head=%u\n",
			st.word,
			get_qregs_doneq_state_length(&st),
			get_qregs_doneq_state_head_index(&st)
		);
	}
	seq_printf(m, "\twait_time=%.08x\n", en75_rreg(&regs->done_queue.wait_time));

	{
		struct wrr_mode cfg = en75_rreg(&regs->wrr_mode);

		seq_printf(m,
			"wrr_mode=%.08x%s%s\n",
			en75_word(cfg),
			is_qregs_wrr_mode_use_16b(&cfg) ? " +use_16b" : "",
			is_qregs_wrr_mode_by_byte(&cfg) ? " +by_byte" : ""
		);
	}

	/* wrr_weight needs writing to read as anything meaningful */
	seq_printf(m, "wrr_weight=%.08x\n",      en75_word(en75_rreg(&regs->wrr_weight)));
	seq_printf(m, "buf_usage_cfg=%.08x\n",   en75_rreg(&regs->buf_usage_cfg));
	seq_printf(m, "tx_meter_cfg=%.08x\n",    en75_rreg(&regs->tx_meter_cfg));
	seq_printf(m, "tx_limit_cfg=%.08x\n",    en75_rreg(&regs->tx_limit_cfg));
	seq_printf(m, "tx_limit_param=%.08x\n",  en75_rreg(&regs->tx_limit_param));

	{
		struct qregs_tx_congest_cfg cfg = en75_rreg(&regs->tx_congest_cfg);

		seq_printf(m,
			"tx_congest_cfg=%.08x dyncong_tick=%u margin=%u dei_scale=%u\n"
			"%s%s%s%s%s%s%s%s%s%s\n",
			en75_word(cfg),
			cfg.dyncong_tick,
			get_qregs_tx_congest_cfg_dyncong_margin(&cfg),
			get_qregs_tx_congest_cfg_dyncong_dei_scale(&cfg),

			/* drop / enable modes */
			is_qregs_tx_congest_cfg_tail_drop_en(&cfg)    ? " +tail_drop"      : "",
			is_qregs_tx_congest_cfg_dei_drop_en(&cfg)     ? " +dei_drop"       : "",
			is_qregs_tx_congest_cfg_dyncong_en(&cfg)      ? " +dyncong"        : "",

			/* threshold blocking */
			is_qregs_tx_congest_cfg_max_thr_blk_tx1(&cfg) ? " +max_thr_tx1"    : "",
			is_qregs_tx_congest_cfg_min_thr_blk_tx1(&cfg) ? " +min_thr_tx1"    : "",
			is_qregs_tx_congest_cfg_max_thr_blk_tx0(&cfg) ? " +max_thr_tx0"    : "",
			is_qregs_tx_congest_cfg_min_thr_blk_tx0(&cfg) ? " +min_thr_tx0"    : "",

			/* dynamic congestion update sources */
			is_qregs_tx_congest_cfg_dyncong_upd_wrr(&cfg)   ? " +upd_wrr"   : "",
			is_qregs_tx_congest_cfg_dyncong_upd_txrx(&cfg) ? " +upd_txrx"  : "",
			is_qregs_tx_congest_cfg_dyncong_upd_tick(&cfg) ? " +upd_tick"  : ""
		);
	}

	{
		struct qregs_tx_congest_thr cfg = en75_rreg(&regs->tx_congest_thr);

		seq_printf(m,
			"tx_congest_thr=%.08x min=%u max=%u\n",
			en75_word(cfg),
			cfg.min,
			cfg.max
		);
	}

	seq_printf(m, "tx_per_ch_dthr=%.08x\n", en75_rreg(&regs->tx_per_ch_dthr));
	seq_printf(m, "tx_per_q_dthr=%.08x\n", en75_rreg(&regs->tx_per_q_dthr));

	seq_puts(m, "debug:\n");
	{
		struct mem_ctl cfg = en75_rreg(&regs->debug.mem_ctl);

		seq_printf(m,
			"\tmem_ctl=%.08x%s\n",
			en75_word(cfg),
			is_qregs_debug_mem_ctl_done(&cfg) ? " +done" : ""
		);
	}
	seq_printf(m, "\tmem_lo=%.08x\n",       en75_rreg(&regs->debug.mem_lo));
	seq_printf(m, "\tmem_hi=%.08x\n",       en75_rreg(&regs->debug.mem_hi));
	seq_printf(m, "\tunused_0=%.08x\n",     en75_rreg(&regs->debug.unused_0));
	seq_printf(m, "\thwf_desc_free=%.08x\n", en75_rreg(&regs->debug.hwf_desc_free));
	seq_printf(m, "\thwd_buf_used=%.08x\n", en75_rreg(&regs->debug.hwd_buf_used));
	seq_printf(m, "\tprobe_lo=%.08x\n",     en75_rreg(&regs->debug.probe_lo));
	seq_printf(m, "\tprobe_hi=%.08x\n",     en75_rreg(&regs->debug.probe_hi));

	{
		struct qregs_rxring_size cfg = en75_rreg(&regs->rxring_size);

		seq_printf(m,
			"rxring_size=%.08x rxring0=%u rxring1=%u\n",
			cfg.word,
			get_qregs_rxring_size_ring0(&cfg),
			get_qregs_rxring_size_ring1(&cfg)
		);
	}

	{
		struct qregs_rxring_low cfg = en75_rreg(&regs->rxring_low);

		seq_printf(m,
			"rxring_low=%.08x rxring0_low=%u rxring1_low=%u\n",
			cfg.word,
			get_qregs_rxring_low_ring0(&cfg),
			get_qregs_rxring_low_ring1(&cfg)
		);
	}

	seq_puts(m, "qchain1:\n");
	en75_qdma_qchain(m, &regs->qchain1);

	seq_printf(m, "cpu_rx_limit=%.08x\n",     en75_rreg(&regs->cpu_rx_limit));
	seq_printf(m, "cpu_rx_limit_val=%.08x\n", en75_rreg(&regs->cpu_rx_limit_val));
	seq_printf(m, "vch_wrr=%.08x\n",         en75_rreg(&regs->vch_wrr));
	seq_printf(m, "vch_qmode=%.08x\n",       en75_rreg(&regs->vch_qmode));
	seq_printf(m, "ch_lim_en=%.08x\n",       en75_rreg(&regs->ch_lim_en));


	return 0;
}

static int en75_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_qdma_regs, inode->i_private);
}

static const struct file_operations en75_regs_fops = {
	.owner   = THIS_MODULE,
	.open    = en75_regs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int en75_qdma_interrupts(struct seq_file *m, void *v)
{
	struct en75_qdma_debug *debug = m->private;
	int i;

	seq_puts(m, "count\tirq\treg\tbit\torigin\tchain\tmeaning\n");
	for (i = 0; i < ARRAY_SIZE(debug->config->irqs); i++) {
		struct en75_debug_qdma_irq_conf *irq = &debug->config->irqs[i];
		int j;

		for (j = 0; j < ARRAY_SIZE(irq->desc); j++) {
			/* Unused bit */
			if (!irq->desc[j].word)
				continue;

			seq_printf(m, "%u\t%d\t%d\t%d\t%s\t%d\t%s\n",
				   irq->counters[j], i, j / 32, j % 32,
				   en75_irq_purpose_source_str(irq->desc[j].source),
				   irq->desc[j].chain,
				   en75_irq_purpose_type_str(irq->desc[j].type));
		}
	}
	return 0;
}

static int en75_interrupts_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_qdma_interrupts, inode->i_private);
}

static const struct file_operations en75_interrupts_fops = {
	.owner   = THIS_MODULE,
	.open    = en75_interrupts_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int en75_qdma_descs(struct seq_file *m, void *v)
{
	struct en75_qdma_chain_debug *chain = m->private;
	struct qchain_regs __iomem *qchain_reg;
	int i;

	qchain_reg = chain->chain_n == 1 ?
		&chain->qdma->config->regs->qchain1 :
		&chain->qdma->config->regs->qchain0;

	seq_printf(m, "QDMA RX Descriptors driver_idx=%d hardware_idx=%d\n",
		readl(&qchain_reg->rx_cpui), readl(&qchain_reg->rx_hwi));
	for (i = 0; i < chain->config->rx_count; i++) {
		struct desc *desc = &chain->config->rx_descs[i];
		seq_printf(m, "  %d ", i);
		print_desc(m, desc);
		seq_puts(m, " ");
		print_erx(m, &desc->msg.erx);
		seq_puts(m, "\n");
	}

	seq_printf(m, "QDMA TX Descriptors driver_idx=%d hardware_idx=%d\n",
		readl(&qchain_reg->tx_cpui), readl(&qchain_reg->tx_hwi));
	for (i = 0; i < chain->config->tx_count; i++) {
		struct desc *desc = &chain->config->tx_descs[i];
		seq_printf(m, "  %d ", i);
		print_desc(m, desc);
		seq_puts(m, " ");
		print_etx(m, &desc->msg.etx);
		seq_puts(m, "\n");
	}

	return 0;
}

static int en75_descs_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_qdma_descs, inode->i_private);
}

static const struct file_operations en75_descs_fops = {
	.owner   = THIS_MODULE,
	.open    = en75_descs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int en75_chan_ctrs(struct seq_file *m, void *v)
{
	struct en75_qdma_chain_debug *chain = m->private;
	u32 *ctrs = chain->config->tx_per_ch_q;
	int i;

	seq_puts(m, "chan\tqueue0\tqueue1\tqueue2\tqueue3\tqueue4\tqueue5\tqueue6\tqueue7\n");
	for (i = 0; i < EN75_NUM_CHANNELS * EN75_NUM_QUEUES; i += EN75_NUM_QUEUES) {
		seq_printf(m, "%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
			   i / EN75_NUM_QUEUES, ctrs[i], ctrs[i + 1], ctrs[i + 2], ctrs[i + 3],
			   ctrs[i + 4], ctrs[i + 5], ctrs[i + 6], ctrs[i + 7]);
	}

	return 0;
}

static int en75_chan_ctrs_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_chan_ctrs, inode->i_private);
}

static const struct file_operations en75_chan_ctrs_fops = {
	.owner   = THIS_MODULE,
	.open    = en75_chan_ctrs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int en75_init_qdma(struct en75_qdma_debug *qdma_debug)
{
	struct en75_debug_qdma_conf *config = qdma_debug->config;

	BUILD_BUG_ON(ARRAY_SIZE(qdma_debug->chains) != ARRAY_SIZE(config->chains));
	for (int i = 0; i < ARRAY_SIZE(qdma_debug->chains); i++) {
		char filename[16] = {0};

		if (config->chains[i].rx_descs == NULL ||
		    config->chains[i].tx_descs == NULL)
			continue;

		snprintf(filename, sizeof(filename) - 1, "descs%d", i);
		qdma_debug->chains[i].descs =
			debugfs_create_file(filename, 0444,
					    qdma_debug->dir,
					    &qdma_debug->chains[i],
					    &en75_descs_fops);

		snprintf(filename, sizeof(filename) - 1, "chan_ctrs%d", i);
		qdma_debug->chains[i].chan_ctrs =
			debugfs_create_file(filename, 0444, qdma_debug->dir,
					    &qdma_debug->chains[i],
					    &en75_chan_ctrs_fops);

		if (!qdma_debug->chains[i].descs)
			return -ENOMEM;

		qdma_debug->chains[i].config = &config->chains[i];
		qdma_debug->chains[i].qdma = qdma_debug;
		qdma_debug->chains[i].chain_n = i;
	}

	qdma_debug->fwdescs = debugfs_create_file("hwf_descs", 0444,
						  qdma_debug->dir, qdma_debug,
						  &en75_hwf_fops);
	qdma_debug->regs = debugfs_create_file("regs", 0444,
						  qdma_debug->dir, qdma_debug,
						  &en75_regs_fops);

	qdma_debug->interrupts = debugfs_create_file("interrupts", 0444,
						     qdma_debug->dir, qdma_debug,
						     &en75_interrupts_fops);
	return 0;
}

struct en75_debug *en75_debugfs_init(struct en75_debug_conf *config)
{
	struct en75_debug *debug;
	int ret = -EINVAL;

	debug = kzalloc(sizeof(*debug), GFP_KERNEL);
	if (!debug)
		return ERR_PTR(-ENOMEM);

	memcpy(&debug->config, config, sizeof(debug->config));
	config = &debug->config;

	debug->dir = debugfs_create_dir("econet_eth", NULL);
	if (!debug->dir) {
		ret = -ENOMEM;
		goto err_dir;
	}

	BUILD_BUG_ON(ARRAY_SIZE(debug->qdma) != ARRAY_SIZE(config->qdma));
	for (int i = 0; i < ARRAY_SIZE(debug->qdma); i++) {
		char qdma_dirname[8] = {0};

		if (config->qdma[i].regs == NULL)
			continue;

		snprintf(qdma_dirname, sizeof(qdma_dirname) - 1, "qdma%d", i);
		debug->qdma[i].dir =
			debugfs_create_dir(qdma_dirname, debug->dir);
		if (!debug->qdma[i].dir) {
			ret = -ENOMEM;
			goto err_file;
		}

		debug->qdma[i].config = &config->qdma[i];
		ret = en75_init_qdma(&debug->qdma[i]);
		if (ret)
			goto err_file;
	}

	return debug;

err_file:
	debugfs_remove_recursive(debug->dir);
err_dir:
	kfree(debug);
	return ERR_PTR(ret);
}

void en75_debugfs_exit(struct en75_debug *debug)
{
	if (!debug)
		return;
	debugfs_remove_recursive(debug->dir);
	kfree(debug);
}
