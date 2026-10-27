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

PDMA completion uses timer-driven TX NAPI, following the polling mode
selected by the 32-bit SDK. It does not require a new interrupt assignment
or a device-tree node. Both DONE and the DMA index must indicate completion
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
