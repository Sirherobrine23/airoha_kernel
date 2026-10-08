.. SPDX-License-Identifier: GPL-2.0-only

EcoNet EN7512/EN7516 xDSL integration
===================================

Functional status
-----------------

``CONFIG_AIROHA_XDSL`` implements the Linux resource and PTM packet-transport
boundary. It is **not a working ADSL/VDSL modem**. There is no line engine in
this tree; opening its GDM2 netdev returns ``-EOPNOTSUPP`` without a registered
engine. The driver does not assert carrier, unmask DMT sources, or manufacture
showtime to conceal the missing PHY implementation.

The legacy EN7516 module contains 3,313 defined host-side functions, including
G.hs, ADSL1/2/2+, VDSL2, bit loading, DSP and AFE control. The inspected SDK
does not provide a separate firmware image with a documented loader ABI that
can replace those algorithms. Porting a Linux-3.18 MIPS module by changing its
vermagic is not supported. Decompiled output with inferred types is not
buildable, verified source. A new host-side PHY implementation or a supported
vendor firmware/API is still required, followed by tests against actual DSLAMs.

ATM/SAR, bonding, vectoring data transport, PTM hardware flow offload,
preemptive TX classification and live-PHY system suspend are not implemented.
The two line-0 PTM bearers are represented in the API; ordinary Ethernet TX
uses the selected bearer's non-preemptive channel. RX descriptor format,
error/OAM handling and byte order still require per-SoC hardware validation.
The integration code has not been validated on either SoC in hardware.

Resources and DMA
-----------------

The physical DMT window starts at ``0x1f900000``; the vendor's ``0xbf900000``
is its MIPS KSEG1 alias. ``0x1faa0000`` belongs to eFuse calibration, not DMT.
PTM-TC is at ``0x1fb62000``. EN7512 uses the A60928 AFE, EN7516 the A10627.

GDMA at ``0x1fb30000`` is a separate DMAEngine provider. The vendor PHY uses
channel 0 for QAM table copies to DMT apertures at ``0x1f904000`` and
``0x1f908000``. It is not the Ethernet packet DMA: PTM packets use GDM2/QDMA.
HSDMA on newer SoCs is also independent and is not required by these targets.
The frontend optionally reserves the DT ``qam`` channel and exposes it to a
line engine; it does not issue speculative transfers.

The existing GDMA driver implements memory copies, not a validated DMT QAM
protocol. Its CTRL1 value differs from the legacy QAM sequence (the latter
sets bit 28), as does the burst setting. Those access attributes, payload
endianness and aperture bounds must be established before using it to program
the DSP. Coherent/streaming buffers must be allocated/mapped against
``dmaengine_get_dma_device(hw->qam_dma)``. Physical device addresses must not
be confused with CPU pointers, KSEG1 aliases or arbitrary DMA bus addresses.
The frontend terminates its channel after the engine has stopped.

GDMA reset is owned by the DMA provider. The EN751221 Ethernet node no longer
claims that reset, preventing Ethernet initialization from resetting a PHY
table transfer. No register/IRQ assignments from HSDMA are reused for DMT.

Device tree and configuration
-----------------------------

Enable ``CONFIG_NET_AIROHA``, ``CONFIG_MFD_SYSCON``,
``CONFIG_RESET_CONTROLLER``, ``CONFIG_DMADEVICES``, ``CONFIG_AIROHA_GDMA``
and ``CONFIG_AIROHA_XDSL``. ``CONFIG_DEBUG_FS`` enables read-only diagnostics.
The SoC xDSL nodes remain disabled by default. An appropriate board DT must
enable GDMA, Ethernet, GDM2 and xDSL and disable optical WAN users of GDM2::

    &gdma { status = "okay"; };
    &eth { status = "okay"; };
    &gdm2 { status = "okay"; };
    &xdsl { status = "okay"; };
    &xpon { status = "disabled"; };
    &xpon_phy { status = "disabled"; };

These overrides are necessary resource configuration, not a way to obtain
DSL synchronization without a PHY. EN751627 uses its own compatible strings;
no EN7528 fallback compatible is required. Shared internal Ethernet register
layout data does not establish compatibility of unrelated PHY algorithms.

Line-engine contract
--------------------

``drivers/net/ethernet/airoha/airoha_xdsl.h`` defines the kernel-internal API,
not a stable userspace ABI. A separately implemented engine registers its
device, private cookie and operations using ``airoha_xdsl_register_engine()``
while the netdev is down. A device link orders its removal before the frontend.
The frontend also links itself to the Ethernet supplier. Registration and
unregistration serialize against RTNL/netdev lifecycle; reports are IRQ-safe.

On open, the frontend acquires the callback module, selects PTM in the shared
SCU WAN mux, disables packet channels, resets the PHY/MAC and invokes
``start()``. The engine initializes AFE/DSP/TPSTC, trains the line and owns DMT
interrupt masking. The frontend supplies raw read-to-clear IRQ status to
``irq()`` in a threaded handler. It cannot infer PHY state from IRQ bits alone.

An engine reports down, training, failed, or genuine negotiated showtime using
``airoha_xdsl_report_line()``. Showtime requires PTM transport, nonzero line
rates, valid line-0 bearer bits and an active TX bearer. Invalid notifications
return an error and schedule a failed/down datapath, rather than retaining
an old carrier. ATM showtime returns ``-EOPNOTSUPP``. Down notifications clear
carrier immediately; hardware changes run in a serialized workqueue.

PTM path numbers follow ``line * 4 + bearer * 2 + preemption``. The frontend
restricts line-0 paths to 0..3; Ethernet TX chooses channel 0 or 2, with one of
the eight QDMA QoS queues. It never interprets a PTM frame as a LAN DSA tag.
Short Ethernet frames are padded to 60 bytes. Packet channels are enabled
before RX/TX MAC enable and carrier publication. Link loss blocks new CPU TX,
disables forwarding, drains/retire channels and disables the MAC. Failed
retirement leaves TX blocked and preserves the channel set for a later retry.
PPE output-flow preparation refuses PTM, preserving the CPU TX checks.

On close/removal, IRQ sources are masked and the IRQ thread is joined before
calling ``stop()``. The engine must join all its event reporters, timers,
workers and DMA users before returning, including after a failed ``start()``.
It must not acquire RTNL or wait for the frontend worker inside a callback.
The frontend joins its own worker outside its state mutex, terminates DMA and
asserts resets only once PTM reports TX/RX idle. Idle timeout is reported; it
does not force a reset known to risk bus timeout. The previous SCU mux mode is
restored after safe shutdown. A live engine prevents system suspend with
``-EBUSY`` until an explicit PHY power-management protocol exists.

Diagnostics and software tests
------------------------------

With debugfs mounted, ``<platform-device-name>/status`` exposes SoC/AFE,
engine availability, line state/rates, bearer selection, last error, cached
DMT version and raw IRQ-bit counters. Reading it does not touch reset-held
registers or acknowledge the read-to-clear IRQ register. There are no
writable training/showtime test switches.

``CONFIG_AIROHA_XDSL_KUNIT_TEST`` tests event validation, ATM rejection,
rate constraints, duplicate detection and line-0 bearer/path mapping. It
requires ``CONFIG_KUNIT`` and a supported platform or ``CONFIG_COMPILE_TEST``;
the test does not require the frontend or any PHY hardware. These tests and
cross-compilation do not prove synchronization, packet traffic, AFE calibration
or QAM DMA correctness. Before enabling the driver on a production board,
validate reset/idle timing, IRQ masks, descriptor layout, DMA attributes,
showtime/retrain/LOS, both bearer paths and removal under load on hardware.
