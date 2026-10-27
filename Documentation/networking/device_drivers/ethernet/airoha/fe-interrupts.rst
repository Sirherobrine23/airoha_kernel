.. SPDX-License-Identifier: GPL-2.0-only

Airoha/EcoNet Frame Engine interrupts
=====================================

The Ethernet driver requests the optional interrupt named ``fe-error`` on
EN7580, EN7523, EN7581, AN7552 and AN7583. The IRQ must be named in the DT;
there is no positional fallback. Missing names leave monitoring disabled
without preventing Ethernet operation. EN751221, EN751627 and EN7528 keep
monitoring disabled until their event maps have been verified.

The shared SDK FE register map defines status at FE + 0x08 and enable at
FE + 0x0c. Per-SoC masks select PSE free-queue exhaustion (bit 8) and GDM2
MBI/GEM/FIFO events (bits 16-18 and 20-23). This is the default event set
in the SDK EN7580/EN7523 registration path. The newer SoCs use the same
status definitions. Board validation is still required for each target.
Normal PSE flow-control notifications (bits 0-5) and flow-control drops
(bit 9) remain masked, matching the SDK default.

When TSO v2 is active, FE TSO alerts (bits 12-15) are also enabled. The SDK
labels these collectively as TSO alerts without documenting their individual
meanings; counters retain their bit numbers. These are not the PDMA TX_DONE
interrupts at FE + 0xa20. This change does not replace TSO completion polling.

Only AN7552 enables the PSE exception event (bit 28), matching the SDK's
``SUPPORT_DETECT_PSE_EXCEPTION`` capability. The handler acknowledges the
exception and masks that source until driver reprobe. It does not copy the
SDK's PSE core reset: resetting an active core requires coordinated DMA/PPE
recovery. This reports an exception, but does not recover a stalled datapath.
The other event sources remain enabled.

Initialization keeps FE interrupts masked, requests the IRQ, clears stale
status and then unmasks supported events. The handler reads enabled status,
returns IRQ_NONE for an unrelated interrupt, and acknowledges only observed
events using write-one-to-clear. Remove and probe-error paths synchronize
the handler and mask events before releasing the IRQ and DMA resources.

Diagnostics
-----------

``/proc/interrupts`` lists the handler as ``airoha-fe-error``. Event counters
are available through ethtool on any GDM interface, for example::

  ethtool -S eth0 | grep fe_irq_
  ethtool -S pon0 | grep fe_irq_

These are shared FE counters: eth0 and pon0 show the same values. They count
observed status assertions, not packets or a lossless total of hardware
faults. Multiple events can coalesce in a single status bit. Stale status
cleared during initialization is not counted. Sources outside the configured
monitoring mask are omitted; the PSE exception counter remains visible after
masking that source. MBI auto-aging notifications are counted without warning
messages. Other alerts produce rate-limited diagnostics.

During board validation, exercise bidirectional traffic, optical channel
retirement and interface stop/start with TSO enabled and disabled. Check the
FE counters together with PDMA indices, BQL inflight bytes and kernel logs.
Do not infer successful recovery or TSO segmentation from IRQ counts alone.
