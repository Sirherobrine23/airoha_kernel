.. SPDX-License-Identifier: GPL-2.0-only

Airoha PDMA TSO v2 transmit segmentation
========================================

``CONFIG_NET_AIROHA_TSO`` enables the PDMA TSO v2 and scatter-gather offload
engine on Airoha Frame Engine SoCs (EN7523, AN7552, EN7581, and AN7583) when
the dedicated ``tdma`` reset control is provided in the device tree.

The PDMA engine exposes two TX rings inside the FE register space (ring 0 for
LAN QDMA0 traffic and ring 1 for WAN QDMA1 traffic). Nonlinear packets,
including ordinary scatter-gather packets without GSO, are submitted through
PDMA; linear single-buffer packets continue using QDMA.

PDMA TSO v2 requires the Ethernet reset named ``tdma``. On EN7523, the reset
named ``pdma`` resets QDMA0/LAN rather than the TSO engine and must not be
used to flush TSO during interface shutdown. When the ``tdma`` reset is not
present in the device tree, PDMA TSO v2 stays disabled and existing SoCs
continue using QDMA TX unchanged; EN7523 GDM2 falls back to linear TX.

TSO v2 accepts plain TCP over IPv4 or IPv6 up to 65535 bytes per skb. IP
options, IPv6 extension headers, tunnels, and other GSO types are segmented in
software via ``ndo_features_check``.

PDMA TX completion and interrupts
---------------------------------

When the optional named ``pdma`` interrupt is present in the device tree, the
driver requests it and enables ``TX0_DONE``/``TX1_DONE`` at ``FE + 0xa28``.
Missing IRQ names or request failures fall back to 100 us timer-driven TX NAPI
polling.

In IRQ mode, the handler acknowledges observed ``TX_DONE`` bits using
write-one-to-clear, masks both sources, and schedules TX NAPI. NAPI recycles
completed descriptors (requiring both ``DMA_IDX`` and ``DONE`` to advance) and
rearms the IRQ while maintaining a 1 ms guard timer whenever descriptors remain
in flight.

Shared PDMA counters are exposed through ``ethtool -S`` on every GDM interface:

* ``pdma_irq_mode``: 1 if the PDMA interrupt was requested, 0 in polling mode.
* ``pdma_irq_events``, ``pdma_irq_tx0``, ``pdma_irq_tx1``: observed IRQ events.
* ``pdma_timer_fires``: guard/polling timer wakeups with pending descriptors.
* ``pdma_irq_reclaimed`` / ``pdma_poll_reclaimed``: descriptors reclaimed by
  IRQ-scheduled vs timer/recheck NAPI runs.
* ``pdma_tx_packets`` / ``pdma_gso_packets``: skbs submitted to PDMA.
