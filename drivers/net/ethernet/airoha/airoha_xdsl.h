/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XDSL_H
#define _AIROHA_XDSL_H

#include <linux/io.h>
#include <linux/types.h>

struct device;
struct dma_chan;
struct module;

enum airoha_xdsl_afe {
	AIROHA_XDSL_AFE_A60928,
	AIROHA_XDSL_AFE_A10627,
};

enum airoha_xdsl_line_status {
	AIROHA_XDSL_LINE_DOWN,
	AIROHA_XDSL_LINE_TRAINING,
	AIROHA_XDSL_LINE_SHOWTIME,
	AIROHA_XDSL_LINE_FAILED,
};

enum airoha_xdsl_transport {
	AIROHA_XDSL_TRANSPORT_ATM,
	AIROHA_XDSL_TRANSPORT_PTM,
};

/**
 * struct airoha_xdsl_hw - resources borrowed by a registered line engine
 * @dmt: mapped DMT register window, in native SoC byte order
 * @dmt_phys: physical address of the DMT window
 * @dmt_size: size of the mapped window
 * @afe: SoC-specific analog frontend
 * @qam_dma: optional GDMA channel; NULL on DTs without a DMA specifier
 *
 * Valid only between start() and stop(). The frontend owns the SCU WAN mux,
 * resets, Linux IRQ and PTM MAC. The engine owns AFE/DSP/TPSTC configuration
 * and the DMT interrupt masks. DMA buffers must be mapped against the DMA
 * channel's device, not the frontend device. No legacy KSEG1 addresses may be
 * submitted to DMAEngine. GDMA memcpy alone is not a QAM programming protocol;
 * byte order, device access attributes and aperture sizes need validation.
 */
struct airoha_xdsl_hw {
	void __iomem *dmt;
	phys_addr_t dmt_phys;
	resource_size_t dmt_size;
	enum airoha_xdsl_afe afe;
	struct dma_chan *qam_dma;
};

/**
 * struct airoha_xdsl_line_state - asynchronous line-engine notification
 * @status: current training or showtime state
 * @transport: negotiated ATM or PTM transport
 * @bearer_mask: active line-0 PTM bearers, bits 0 and 1
 * @tx_bearer: bearer used for ordinary, non-preemptive Ethernet transmission
 * @downstream_bps: negotiated downstream rate in bits per second
 * @upstream_bps: negotiated upstream rate in bits per second
 * @error: negative errno for a failed line, otherwise zero
 *
 * Only PTM showtime is supported. ATM and bonding need separate datapaths.
 * There is no userspace operation for manufacturing a showtime notification.
 */
struct airoha_xdsl_line_state {
	enum airoha_xdsl_line_status status;
	enum airoha_xdsl_transport transport;
	u8 bearer_mask;
	u8 tx_bearer;
	u64 downstream_bps;
	u64 upstream_bps;
	int error;
};

/**
 * struct airoha_xdsl_engine_ops - host-side DSL PHY implementation
 * @owner: module owning these callbacks, or NULL for built-in code
 * @start: initialize the PHY and begin training; may sleep and report events
 * @stop: stop all engine work, DMA and event reporters; may sleep
 * @irq: consume read-to-clear DMT status in threaded IRQ context; may sleep
 *
 * The frontend calls stop() even after a failed start(). Callbacks must not
 * take RTNL, unregister the engine, or wait for the frontend's event worker.
 * stop() must join every engine-owned task that can report an event or touch
 * hardware. irq() must be bounded so disable_irq() can drain it. The engine
 * must configure TPSTC for the negotiated transport before reporting showtime.
 */
struct airoha_xdsl_engine_ops {
	struct module *owner;
	int (*start)(void *priv, const struct airoha_xdsl_hw *hw);
	void (*stop)(void *priv);
	void (*irq)(void *priv, u32 status);
};

/* Register/unregister in process context without RTNL held. Registration
 * requires an administratively down netdev; unregister closes it if necessary.
 * A device link enforces engine-before-frontend removal. The caller must hold
 * device references throughout registration.
 */
int airoha_xdsl_register_engine(struct device *frontend, struct device *engine,
				const struct airoha_xdsl_engine_ops *ops,
				void *engine_priv);
void airoha_xdsl_unregister_engine(struct device *frontend, void *engine_priv);

/* IRQ-safe; notifications are applied asynchronously in process context.
 * The engine must stop reporting before unregister_engine() returns.
 */
int airoha_xdsl_report_line(struct device *frontend, void *engine_priv,
			    const struct airoha_xdsl_line_state *state);

#endif /* _AIROHA_XDSL_H */
