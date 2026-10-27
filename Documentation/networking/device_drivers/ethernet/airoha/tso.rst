.. SPDX-License-Identifier: GPL-2.0-only

Airoha/EcoNet transmit segmentation
==================================

``CONFIG_NET_AIROHA_TSO`` enables the native TSO and scatter-gather paths
for the following SoCs. The option is disabled by default pending board
validation.

.. list-table::
   :header-rows: 1

   * - SoC
     - Version
     - Datapath
   * - EN7528
     - v1
     - Linked QDMA descriptors; TSO in TX word 1 bit 28
   * - EN7580
     - v1
     - Linked QDMA descriptors; TSO in TX word 0 bit 10
   * - EN7523
     - v2
     - Two PDMA TX rings in the existing FE resource

The SDK EN7528_LE and EN7580 kernel 4.4 profiles select
``TCSUPPORT_TSO_ENABLE``; EN7523 profiles additionally select
``TCSUPPORT_TSO_V2_ENABLE``. The v1 transmit routine has explicit EN7580
handling. The EN7580 kernel 5.4 demo profile leaves TSO disabled. The two
generations share MSS encoding in the TCP checksum field but do not share
their TX engine.

The EN7523 path sends nonlinear packets, including ordinary SG packets
without GSO, through PDMA. Linear packets and optical control frames keep
using QDMA. Setting only the TSO bit on a fragmented QDMA chain does not
implement v2. When the option is disabled, EN7523 GDM2 retains software
segmentation and linear TX.

PDMA completion normally uses timer-driven TX NAPI, following the polling
mode selected by the 32-bit SDK. The EN7523 IRQ path described below
uses the existing named ``pdma`` interrupt. Both DONE and the DMA index must
indicate completion
before a buffer is unmapped. Polling continues while descriptors remain,
including after the final packet of a connection. Interface shutdown and
PON channel retirement drain PDMA and reset it before releasing buffers
which have not completed.

EN7523 TSO v2 requires the Ethernet reset named ``tdma``, connected to
``EN7523_TDMA_RST``. Build and boot the updated device tree together with
the driver. The older reset named ``pdma`` actually resets QDMA0/LAN on
this SoC: using it during an interface stop destroys the live LAN queue
configuration. It must never be used to flush the TSO engine.

Native TSO accepts plain TCP over IPv4 or IPv6, up to 65535 bytes per skb.
IP options, IPv6 extension headers, tunnels and other GSO types are
segmented in software. VLAN devices inherit the corresponding TX features.

Board validation
----------------

Compilation and simulated DMA ownership tests do not establish hardware
correctness. Test each board and each output path before enabling this
option in a production configuration:

* Start with checksum offload and SG enabled, with TSO disabled. Exercise
  both directions, VLAN traffic and concurrent queues.
* Enable TSO and exercise TCP over IPv4 and IPv6, including local TCP
  output and forwarding of packets aggregated by GRO.
* Check the final packet of short connections as well as sustained load.
  BQL inflight bytes must return to zero after the traffic drains.
* Repeat interface stop/start and PON session reactivation under load.
  Validate with and without PPE offload; its conntrack flags alone do not
  establish that the CPU TX segmentation path was exercised.

On EN7523, disabling only TSO still leaves the SG reassembly path in use.
To return an enabled build to linear software TX during diagnosis, disable
SG and TSO together, for example::

  ethtool -K pon0 tso off gso off sg off

EN7523 PDMA TX interrupts
------------------------

PDMA TX interrupts are enabled by default on EN7523 with TSO v2 enabled.
The driver requests the Ethernet interrupt named ``pdma`` (SPI 64
in the EN7523 DTS) and enables TX0_DONE/TX1_DONE at FE + 0xa28. Other SoCs
keep the existing polling path. Missing IRQ names or request failures also
fall back to polling. A deferred IRQ provider defers probe.

The hard IRQ acknowledges observed TX_DONE bits using W1C, masks both TX
interrupt sources, and schedules TX NAPI. NAPI recycles buffers and rearms
the IRQ after completing its work, checking for events latched while the
interrupt was masked. A 1 ms timer remains armed while buffers are pending,
so missing IRQ delivery or delayed descriptor writeback cannot strand the
last transmission. The polling-only path retains its 100 us interval.
Shutdown masks and synchronizes the IRQ before disabling NAPI or resetting
the dedicated TDMA engine. Restart reenables the IRQ after NAPI is ready.

After boot, check registration and record counters before and after traffic::

  dmesg | grep 'PDMA TX IRQ'
  grep airoha-pdma /proc/interrupts
  ethtool -S eth0 | grep pdma_

Run sustained and short TCP tests in both directions, then check that PDMA
CPU/DMA indices converge and BQL inflight bytes return to zero. Repeat
interface stop/start and optical channel retirement while traffic is active.

The ethtool counters are shared by all GDM interfaces:

* ``pdma_irq_mode`` is 1 when the IRQ was successfully requested. It does
  not establish IRQ delivery; ``pdma_irq_events`` counts handled IRQs.
* ``pdma_irq_tx0`` and ``pdma_irq_tx1`` count observed TX_DONE bits in the
  hard IRQ handler. Coalesced events are not individual packet counts.
* ``pdma_timer_fires`` counts timer callbacks which found pending buffers.
* ``pdma_irq_reclaimed`` counts descriptors reclaimed by NAPI scheduled by
  the hard IRQ. ``pdma_poll_reclaimed`` counts descriptors reclaimed by
  polling or the software status recheck after rearming. Attribution follows
  the NAPI scheduling source until completion; it is not per-packet timing.
  Synchronous shutdown draining is excluded from both counters.
* ``pdma_tx_packets`` counts skbs submitted to PDMA, including ordinary SG.
  ``pdma_gso_packets`` counts the subset submitted with GSO metadata.

IRQ delivery is established when ``pdma_irq_events`` and the corresponding
``/proc/interrupts`` count increase. Positive ``pdma_irq_reclaimed`` confirms
that IRQ-scheduled NAPI recycled descriptors. If these stay zero while
``pdma_poll_reclaimed`` grows, the guard is doing the recycling; successful
traffic alone does not validate the IRQ. Some polling reclamation can still
occur with a working IRQ due to delayed writeback or the status recheck.

The read-only module parameter ``pdma_irq=0`` restores polling-only
operation at the next driver load. For an OpenWrt module, append that option
to the airoha-eth entry in the module loader configuration and reboot. For
a built-in driver, use ``airoha_eth.pdma_irq=0`` on the kernel command
line. The parameter cannot be switched while the driver is running.
