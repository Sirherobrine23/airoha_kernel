.. SPDX-License-Identifier: GPL-2.0-only

OMCI access line management
===========================

The ``omci`` Generic Netlink family provides a local access-line interface in
addition to its existing OMCC transport and ONU managed-entity interface.
The local interface describes GPON, XG-PON, XGS-PON and xDSL frontends. A
copper frontend registers a line without creating an ONU, GEM port, T-CONT,
optical identity, or a wire OMCI agent.

This distinction also allows a future PON ONU with xDSL subscriber ports to
use the line API as the backend of standard xDSL managed entities. Such
entity provisioning and OLT-to-DSL attribute mappings are not implemented by
this change; catalog entries alone do not imply hardware support.

Userspace ABI
-------------

Version 15 appends commands and attributes; existing numeric values are
unchanged. All operations use the existing ``omci`` family in the initial
network namespace.

``OMCI_CMD_LINE_GET`` with ``NLM_F_DUMP`` enumerates registered lines. Without
``NLM_F_DUMP``, it requires a nonzero ``OMCI_ATTR_LINE_ID``. Replies include:

* LINE_ID: u32, independent of the existing OMCI DEV_ID namespace;
* IFINDEX: u32, current data netdevice index;
* NAME: parent device name;
* ACCESS_TYPE: u8, GPON, XGPON, XGSPON, XDSL, or UNSPEC;
* LINE_STATE: u8, UNKNOWN, UNAVAILABLE, DOWN, TRAINING, or UP;
* LINE_FLAGS: u32, STARTED, CARRIER, and RATES_VALID;
* LINE_CAPABILITIES: u32, OMCC and optional RETRAIN;
* DOWNSTREAM_BPS and UPSTREAM_BPS: u64, present only with RATES_VALID.

IDs identify live registrations and may be reused after removal. STARTED
means the provider's frontend or transport has been started; it does not
imply carrier, DSL training, or an operational OMCI MIB. PON line capacity
is not reported as a measured subscriber rate. Unsupported PON modes are
reported as UNSPEC/UNAVAILABLE instead of being mislabeled GPON.

``OMCI_CMD_LINE_RETRAIN`` requires CAP_NET_ADMIN and LINE_ID. Only an xDSL
provider with a real retrain callback accepts it. Otherwise it returns
EOPNOTSUPP; provider errors such as ENETDOWN and EIO are preserved. A
successful call accepts the request, not a promise of synchronization.

Existing ``OMCI_CMD_GET`` replies include LINE_ID to associate a PON agent
with its line. Existing transport and MIB commands still take DEV_ID, not
LINE_ID. A local DSL line is not inserted in that registry. No optical
authentication or GEM service is fabricated for DSL.

Provider lifetime
-----------------

``omci_line_register()`` pins the parent and data netdevice and rejects a
duplicate registration on that netdevice. Providers must keep their ops and
private storage alive until ``omci_line_unregister()`` returns. Unregister
waits for running status/retrain callbacks. Callbacks may sleep but must not
re-enter the registry, and the provider must not unregister while holding a
lock needed by those callbacks. Netlink dispatch holds RTNL before the registry
lock; callbacks must not acquire RTNL themselves. Providers must unregister
before unregistering the data netdevice. Lines moved out of init_net are not
exposed by this ABI.

GPON/XG-PON/XGS-PON agents acquire a line in ``omci_device_register()`` and
release it in ``omci_device_unregister()``. The line snapshot reads the live
xPON mode and registration state. The common layer does not implement a
new optical MAC, PHY, PLOAM activation engine or message integrity algorithm;
those remain responsibilities of the selected PON provider.

EcoNet xDSL
-----------

``airoha_xdsl.c`` remains the only driver source for both supported matches:

* ``econet,en751221-xdsl``: EN7512, A60928 analog frontend;
* ``econet,en751627-xdsl``: EN7516, A10627 analog frontend.

Both register a local management line after claiming GDM2 and remove that
line before dismantling the datapath. The common layer does not bypass the
existing exclusive xPON/xDSL ownership of GDM2 or the shared WAN mux.

The existing driver prepares DMT/PTM but has no training/DSP implementation.
It therefore reports UNAVAILABLE, leaves carrier off, and does not advertise
RETRAIN. The EN7512 5.10.x and EN7516 5.12.x decompilations are evidence for
reconstruction, not buildable engines or interchangeable AFE implementations.
No external ``tc3162`` module or second xDSL driver file is introduced.

The data-only firmware format, silicon identification and opt-in AFE
initialization stage are documented in :doc:`airoha-dmt`. Completing that
stage does not change the line's UNAVAILABLE state.

When the engine is reconstructed inside ``airoha_xdsl.c``, the management
callback should return a coherent cached state/rate snapshot, and a retrain
callback can be added to the same ops table. The recovered TCIf convention is
DOWN=0x00, WAIT_INIT=0x08, INIT=0x10, UP=0x20; its notification adapter emits
events 2, 3, 3 and 1 respectively. Recheck the current state after deferred
notification processing, so an old showtime event cannot re-enable PTM after
stop or loss of signal. Only showtime with a negotiated PTM bearer may enable
the Ethernet datapath; ADSL/ATM requires a separate bearer implementation.

The engine must share the existing DT-managed DMT IRQ and MMIO ownership.
The DMT status register is read-to-clear: read it once and pass the snapshot
to the engine instead of calling a legacy ISR that rereads it. Preserve
timing-critical work until its execution context is understood. Stop must
mask interrupts, drain IRQ/work, and reset hardware without holding locks
needed by the draining work.

The legacy GDMA base 0xbfb30000 is a separate DMA engine, not GDM2. Its
replacement requires DMA resource/channel ownership. It must not be removed
on the assumption that Ethernet GDM2 handles those DSP transfers. Likewise,
the soft-float helper imports require reconstruction of the numerical
contract; blindly changing them to fixed-point arithmetic changes results.
AFE calibration, data tables, TCIf layouts and negotiated rate units require
validation separately for EN751221 and EN751627.

PTM and SAR integration boundary
-------------------------------

The SDK's ``private_econet/mt7510_ptm/7510ptm.c`` and ``7510ptm.h`` describe
the QDMA/GDM2 datapath for these SoCs. The EN7512 conditional layout also
serves EN7516. The older ``private_econet/tc3262/tc3262ptm.*`` instead uses
its own rings at physical 0x1fb68000, with 16-byte TX and 32-byte RX
descriptors. Those rings must not replace the current QDMA implementation.

For managed xDSL, the shared Ethernet driver now treats the QDMA channel as
``(line << 2) | (bearer << 1) | preemption``, not a flow/hash queue. Initial
TX is restricted to line 0, bearer 0, non-preemptive path 0 and QoS queue 0.
Linux queue accounting is unchanged. PTM frames use GDM2, are padded to
ETH_ZLEN and are not classified as MediaTek switch special tags. Carrier
must be on before a managed PTM frame can be submitted.

On direct GDM2 ingress through QDMA1, PTM RX word 0 has CRC/runt/long flags
in bits 0..2, path in bits 3..10, OAM in bit 11 and BACP in bit 12. Error
frames, unsupported control frames, additional lines/bearers, short Ethernet
headers and frames received while the interface/carrier is down are
dropped before normal Ethernet processing. Only paths 0 and 1 are accepted.
The old SDK's unconditional ``PTM_RX_W0_ERR_BITS`` mask predates this
layout; use the EN7512 structures, not that legacy mask. This is software
descriptor preparation/validation, not a completed or hardware-tested
PTM datapath. Channel enables/retirement, FE limits and CRC policy, hardware
flow offload, bonding and multi-bearer provisioning remain to be integrated
with the line engine.

The Ethernet OF table maps ``econet,en751627-eth`` to
``en7528_soc_data``. The numeric EN751627 enum alone does not describe
runtime dispatch. This change does not alter optical SoC validation.

``airoha_xdsl.c`` stops PTM TX before RX, with readbacks to flush the
writes. It no longer forces TPSTC into PTM before negotiation or clears
PTM_CTRL bit 17 independently of DMT. The vendor fast path pairs that
change with DMT setting 0x3002 (redundant/dummy U-cell mode). A future
engine must configure both ends consistently, then enable RX before TX
and publish carrier only after the negotiated bearer is ready.

``7510ptm_L2encap.c`` copies a preformed frame from
``ptm_L2encap_gen_hook_func`` into an skb and transmits on bearer 0. This
is the missing reference for the recovered G.vector feedback call chain;
it is not an OMCC/GEM transmit path. No unsafe global hook is added here.

The SDK's ``private_econet/mtk_sar`` also uses QDMA/GDM2, but word 0
carries VC, PTI, CLP, UU and CPI rather than a PTM path. Word 1 additionally
has XOA/CC/raw flags, and RX word 2 contains SAR/AAL5 error information.
ATM OAM cells are not OMCI messages. Implementing ATM requires a SAR MMIO
resource, VPI/VCI allocation, AAL5/VCC lifetime, encapsulation, QoS and
completion/error handling. The existing DMT/PTM binding has no SAR
resource; this patch does not invent one or expose a nonfunctional ATM
device. The ATM backend can live in the same ``airoha_xdsl.c``, sharing
the Ethernet driver's QDMA service rather than allocating competing rings.

OMCI remains management, not packet segmentation or DMA ownership. A
future xDSL managed-entity backend must distinguish the physical DSL line
from the negotiated ATM/PTM bearer and its services. ATM VC identifiers,
PTM path numbers and PON GEM/T-CONT identifiers are not interchangeable.

Testing
-------

Enable ``CONFIG_KUNIT=y``, ``CONFIG_XPON=y``, ``CONFIG_XPON_OMCI=y`` and
``CONFIG_XPON_OMCI_ACCESS_KUNIT_TEST=y``, or use::

  ./tools/testing/kunit/kunit.py run --kunitconfig=net/xpon/omci

The ``omci-access`` suite exercises
duplicate registration, removal, provider errors, 64-bit rates, missing
engines, PON technology/state mapping, retrain dispatch and unregister waiting
for an in-flight callback. These tests do not validate DSL synchronization,
optical activation or packet forwarding on physical hardware.

The independent ``airoha-ptm-metadata`` suite checks all 256 PTM path
values, individual MAC error flags, OAM/BACP rejection and reserved bits::

  ./tools/testing/kunit/kunit.py run --kunitconfig=drivers/net/ethernet/airoha

It does not exercise the MMIO stop sequence, NAPI dispatch or TX DMA.
