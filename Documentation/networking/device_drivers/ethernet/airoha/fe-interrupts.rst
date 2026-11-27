.. SPDX-License-Identifier: GPL-2.0-only

Airoha Frame Engine interrupts
==============================

The Ethernet driver requests the optional interrupt named ``fe-error`` on
EN7523, AN7552, EN7581, and AN7583. The IRQ must be named in the device tree;
there is no positional fallback. Missing names leave monitoring disabled
without preventing Ethernet operation.

The FE register map defines status at ``FE + 0x08`` and enable at ``FE + 0x0c``.
Per-SoC masks select PSE free-queue exhaustion (bit 8) and GDM2 MBI/GEM/FIFO
events (bits 16-18 and 20-23). Normal PSE flow-control notifications (bits 0-5)
and flow-control drops (bit 9) remain masked.

When PDMA TSO v2 is active, FE TSO alerts (bits 12-15) are also enabled. These
are separate from the PDMA TX_DONE interrupts at ``FE + 0xa20``.

Only AN7552 enables the PSE exception event (bit 28). The handler acknowledges
the exception and masks that source until driver reprobe instead of resetting
an active core without coordinated DMA/PPE recovery. Other event sources remain
enabled.

Initialization keeps FE interrupts masked, requests the IRQ, clears stale
status, and then unmasks supported events. The handler reads enabled status,
returns ``IRQ_NONE`` for unrelated interrupts, and acknowledges only observed
events using write-one-to-clear. Remove and probe-error paths synchronize the
handler and mask events before releasing the IRQ and DMA resources.

Diagnostics
-----------

``/proc/interrupts`` lists the handler as ``airoha-fe-error``. Event counters
are available through ethtool on any GDM interface, for example::

  ethtool -S eth0 | grep fe_irq_

These are shared FE counters across all GDM interfaces. They count observed
status assertions rather than individual packets. MBI auto-aging notifications
are counted without warning messages; other alerts produce rate-limited
diagnostics.
