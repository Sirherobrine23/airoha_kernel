.. SPDX-License-Identifier: GPL-2.0-only

EN7580 XGS-PON port
===================

Current scope
-------------

The ``airoha-xpon`` module retains the existing GPON and EPON backends.
Only ``airoha,en7580-xpon`` selects the staged XGS-PON engine inside
``airoha_xpon.c``. All three engines share ``xpon_priv``, resource lookup
and registration with the generic XPON core; register access and protocol
initialization remain specific to each engine.
It prepares the EN7580 10G MAC, clears stale table entries and registers
an XGS-PON object with the generic XPON core. Registration stays DOWN and
carrier stays false in that object. It does not register a GDM2 link
provider, an SFP upstream, an optical frontend or an OMCI transport.

This is a MAC preparation stage, not a working optical link. It does not
initialize the 10G PHY/PMA, receive or authenticate PLOAM, enter O2/3 or O5,
program registration credentials, or configure the GDM2/QDMA datapath.
Opening the Ethernet interface does not start an XGS-PON session.
AN7581 and AN7583 are outside the scope of this backend.

Register status is available with ``CONFIG_DEBUG_FS`` under
``/sys/kernel/debug/airoha-xgspon-<platform-device-name>/status``.
This diagnostic reads status and FIFO occupancy without consuming PLOAM
words or reading registration credentials and security keys.

Mode representation
-------------------

The values in ``enum xpon_mode`` are individual bits, and capability masks
combine those values directly. A current mode must contain exactly one
known bit. The provider advertises only implemented modes: GPON and EPON
for the legacy engines, and XGS-PON alone for this EN7580 preparation stage.
The hardware capability bitmap does not enable unimplemented engines.

Generic Netlink family ``xpon`` uses version 2. ``XPON_ATTR_MODE`` and
``XPON_ATTR_AVAILABLE_MODES`` both carry native-endian ``u32`` values;
the former is a single mode bit and the latter a capability bitmap.
Requests must use version 2. Version 1 clients must update both the mode
encoding and attribute width; an 8-bit value cannot represent 50G-PON,
25G-EPON or 50G-EPON. The sysfs mode names remain unchanged.

SDK evidence
------------

The supplied SDK separates the old XPON stack from the following modules:

* ``private/xpon_bsp/modules/xpon_10g``: 10G MAC, PLOAM, activation,
  authentication, encryption and WAN integration.
* ``private/xpon_bsp/modules/xpon_phy_10g``: digital PHY and PMA, with
  EN7580-specific register definitions and bring-up sequences.

The MAC layout comes from
``xpon_10g/inc/gpon/xgpon_mac_reg_c_header.h``. Its leading 0x5000 bytes
are reserved, so the SDK's mapping of 0x1fb60000 places ``SW_RST`` at
0x1fb65000. The new backend maps the complete shared DT resource and uses
offsets relative to 0x1fb65000. It needs at least 0x6000 bytes of shared
MMIO space; the existing EN7580 DT reserves 0x10000 bytes.

Important differences from the current GPON backend:

====================== ==================== ===========================
Property               Existing GPON        EN7580 10G MAC
====================== ==================== ===========================
MAC offset             0x4000               0x5000
ONU-ID width           8 bits               10 bits, invalid ID 0x3ff
T-CONT access          Direct + indirect    All 32 entries indirect
Alloc-ID width         12 bits              14 bits
GEM ID space           4096                 65536
Active GEM capacity    Legacy backend       256 in SDK software
GEM valid bit          16                   18
GEM encryption bit     17                   16, upstream encryption
PLOAM wire message     12 bytes             48 bytes including MIC
PLOAM RX FIFO record   3 words              13 words including metadata
PLOAM TX FIFO record   3 words              11 words with hardware MIC
Registration data      10-byte password     36-byte Registration_ID
Activation             O1/O2/O3/O4/O5       O1/O2_3/O4/O5 and extensions
Nominal DS / US rate   2.48832 / 1.24416 Gb  9.95328 / 9.95328 Gb
====================== ==================== ===========================

``xpon_phy_10g/inc/phy_reg.h`` assigns WAN_SEL 10 (0x0a) to XGS-PON and
11 (0x0b) to NGPON2. ``src/phy_init.c`` actually writes 0x0a, and
``src/en7580_pma.c`` does the same in its synchronous XGS-PON mode.
An older comment in ``src/en7580.c`` incorrectly calls 0x0b XGS-PON;
the runtime code and constants take precedence over that comment.

``xpon_10g/inc/gpon/gpon_dev.h`` defines the XGS-PON response time as
0x1600. ``src/gpon/gpon_init.c`` chooses idle XGEM threshold 0x120 for
ASIC XGS-PON, distinct from both the FPGA value and asymmetric XG-PON.
These are MAC defaults, not substitutes for PHY delay calibration.

``gponDevResetGemInfo()`` in ``src/gpon/gpon_dev.c`` invalidates all
65536 GEM IDs with individual commands. The port uses that path rather
than assuming the unused GEM_TBL_INIT register has suitable semantics.
It checks every command and imposes a deadline on the complete sweep.
``gponDevGemMibTablesInit()`` starts the MIB and GPIDX tables together;
the port follows that ordering and checks both completion bits.

Preparation and teardown
------------------------

Probe requires a named MAC region, an exclusive MAC reset, an SCU syscon
and an explicit GDM2 phandle. Match data supplies the initial XGS-PON
mode; mode control belongs to the generic XPON core rather than device
tree. All fallible resource lookups precede hardware configuration.

The MAC is held in reset while WAN_SEL is changed to 0x0a. After reset
release, probe pulses the local active-low MAC reset, masks interrupts,
disables automatic O2/3 and O4 replies and dying-gasp transmission, clears
upstream profile validity and ONU-ID validity, selects O1 and stops both
MBI and MPI directions. It invalidates AES keys, programs the ASIC MAC
timing defaults and clears T-CONT, GEM, MIB and GPIDX state.

Neither this stage nor its diagnostic enables optical TX. The optical
PHY and frontend remain unconfigured; physical laser pin state is not
verified by MAC register preparation. MAC stops and O1 prevent this
backend from starting an upstream session. Every post-reset failure
quiesces the MAC and holds it in reset. Teardown stops diagnostic readers
before resetting and releasing the resources.

The EN7580 DT node remains disabled by default. MAC preparation can be
checked on the bench by enabling only the MAC node while leaving the
unimplemented PHY disabled. No generic PHY provider is requested at this
stage. A board with an enabled but unbound PHY supplier may be held back
by firmware device-link dependencies; keep that node disabled for the
MAC-only check.

A successful preparation log should accompany these register values:

* ``interrupt-enable`` is zero.
* ``onu-id`` is 0x3ff with validity bit 15 clear.
* ``state`` is 1 (O1).
* ``ploamu-control`` has bit 0 set, selecting software replies.
* ``mbi-mpi-stop`` has bits 0, 8, 16 and 24 set.
* ``response-time`` is 0x1600 and the idle threshold is 0x120.
* ``mib-init`` and ``gpidx-init`` have completion bit 8 set.

Reading these values is a hardware check. Successful compilation alone
does not prove clocks, reset release, table command completion or optical
behavior on an EN7580 board.

Next implementation stages
--------------------------

1. Connect the EN7580 generic PHY backend to the staged MAC activation
   lifecycle. The provider in ``drivers/phy/airoha/phy-airoha-xpon.c`` now
   implements XGS-PON calibration, PCS synchronization and polled LOS
   recovery. Its 0x4000-byte resource includes the PON PMA at offset 0x3000
   and excludes the XFI PMA. RX FEC is enabled during initialization;
   upstream burst profiles and TX FEC selection remain MAC activation work.
   TX_DISABLE remains asserted until the consumer explicitly requests TX,
   and is reasserted whenever synchronization is lost. Power cycling clears
   the request. The three optional NVMEM trims correspond to eFuse bits
   90-95; absent or unprogrammed values use impedance level 2.

   The SDK also touches the XFI/JCPLL reference-clock domain. Those writes
   are excluded from the PON sequences because that domain currently belongs
   to the XSI PCS driver. A bounded TX PLL lock check rejects power-on if
   the required clock is unavailable. Standalone PON cold boot still needs
   shared reference-clock ownership between the two drivers, rather than
   depending on prior Ethernet initialization. Do not enable the board PHY
   node until that clock lifecycle and the optical sequence are validated.
2. Add G.9807.1 PLOAM framing, FIFO conversion and MIC handling. The
   downstream FIFO includes an extra word carrying the hardware MIC
   result. Preserve that result through the IRQ-to-worker handoff.
3. Implement O1/O2_3/O4/O5 transitions, burst profiles, calibrated EqD,
   registration, key derivation and timeout/LOS recovery. The legacy
   ``airoha_ploam`` structures and GPON callbacks cannot carry this wire
   format or the 10-bit ONU-ID.
4. Add EN7580 FE/QDMA XGS-PON operations, 14-bit Alloc-ID and 16-bit GEM
   mapping, channel retirement, authenticated OMCI and service setup.
   Reuse the generic XPON/OMCI interfaces where their data models apply,
   while keeping the existing GPON transport intact.
5. Validate cold boot, OLT activation, registration, service traffic,
   LOS recovery and unload on the EN7580 bench before enabling the board
   node by default.
