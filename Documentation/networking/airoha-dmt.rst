.. SPDX-License-Identifier: GPL-2.0-only

EcoNet DMT and AFE reconstruction
================================

``airoha_xdsl.c`` implements a common platform driver for EN751221/A60928
and EN751627/A10627. Its OF match data selects the expected family, register
counts and data filename. The driver also caches HIR, PDIDR and the package
eFuse shadow and uses ``include/linux/soc/airoha/pkgids.h`` to decode the
actual variant. A conflicting family, an unreadable identity register or a
package outside EN7512/EN7513/EN7513G/EN7516G fails probe before DMT writes.
These packages are reconstruction targets, not hardware-certified devices.

The DT provides NP SCU, CHIP SCU and the single EFUSE_VERIFY_DATA0 shadow
word through separate phandles. The identity shadow at 0x1fbf8214 must not
be confused with the AFE calibration bank or added to the DMT MMIO window.

This revision reconstructs AFE initial defaults and register access. The
G.hs/ADSL/VDSL host state machines, DSP programs and scheduling, modem
calibration and the separate DSP GDMA are still absent. Carrier stays off,
DMT interrupts stay masked and OMCI reports UNAVAILABLE without RETRAIN.
See :doc:`omci-access` for the management and PTM/SAR boundaries.

Generating and loading data
---------------------------

Install Python's ``pyelftools`` to run ``tools/net/airoha/extract_dmt_data.py``.
The extractor accepts only the two audited ELF SHA-256 fingerprints listed
in that script. A different filename does not affect selection; a different
module build is rejected and must be audited before adding it.

For the audited EN7512 kernel-3.18 module (DMT 5.10.3.0)::

  python3 tools/net/airoha/extract_dmt_data.py \
      tc3162_dmt_en7512_kernel_3_18.ko firmware/airoha/en7512-afe-v1.bin

For the audited EN7516 DMT 5.12.10.0 module::

  python3 tools/net/airoha/extract_dmt_data.py \
      tc3162_dmt-5.12.10.0.ko firmware/airoha/en7516-afe-v1.bin

Output files must not already exist. A JSON manifest records the source
hash, image hash, version, initializer spans and runtime register sources.
Install the resulting files at these firmware search paths::

  /lib/firmware/airoha/en7512-afe-v1.bin
  /lib/firmware/airoha/en7516-afe-v1.bin

``request_firmware()`` runs during probe. A missing image, wrong target,
invalid length/map or CRC failure prevents registration. Keep the files in
the initramfs when the driver is built in and probes before the rootfs is
available, or include them through the kernel's built-in firmware mechanism.
The script's license does not change the provenance or redistribution terms
of data extracted from a supplied module.

Data format, version 1
----------------------

All multibyte fields are little-endian. The header is exactly 32 bytes:

==========  ====  =========================================================
Offset      Size  Field
==========  ====  =========================================================
0           8     Magic ``AIRODMT`` followed by NUL
8           2     Format version: 1
10          2     Target: 0x7512 or 0x7516
12          2     Digital register count: 34 or 44
14          2     Analog register count: 52
16          2     Digital map count: 208 or 262
18          2     Analog map count: 201
20          4     Payload size
24          4     Payload CRC-32 (IEEE, same value as Python zlib.crc32)
28          4     Reserved, zero
==========  ====  =========================================================

The payload contains, in order, the digital defaults as u32 words, analog
defaults as bytes, the digital map and the analog map. Each map entry is
six bytes: u16 logical ID, u8 physical register, u8 field width, u8 shift
and a zero reserved byte. Exact sizes are 2674 and 3038 bytes respectively.
No ELF headers, relocations, pointers, CPU instructions or MMIO command
lists are loaded. CRC checks accidental corruption; it is not a signature.

The original EN7512 maps have five-byte entries and EN7516 maps have
six-byte big-endian entries. The extractor converts only the allowlisted
objects, rejecting any overlapping relocation. A limited static constant
propagator recovers the initial defaults from the audited MIPS initializer
spans, including delay-slot instructions; it records calls without executing
them. Unsupported instructions, loads, call relocations and missing defaults
are errors. This is not an export of the whole module's writable data.

The three EN7556 map entries 149..151 repeat logical ID 148 in both inputs.
Their bytes are preserved, but the field accessor refuses to use them.
Full-register identity mappings, field bounds and reserved bytes are checked
before the image is exposed to the driver. Field writes reject overflow and
handle a width of 32 without an undefined shift.

At each start, EN7516 digital registers 34 and 35 are populated from the
local CHIP_SCU words at offsets 0x1c0 and 0x1c4. The image contains explicit
zero placeholders for them. The meaning of all bits in those words still
requires hardware confirmation. The analog shadow's uninitialized slots
15, 46 and 48 are zero in the image; slot 46 (0x2e) is replaced with the
device's current value before any analog batch is issued.

Experimental AFE bring-up
-------------------------

Normal startup validates data and prepares software shadows without
programming the AFE. An explicit read-only module parameter enables the
experimental MMIO stage::

  modprobe airoha_xdsl afe_bringup=1 afe_annex_b=0 afe_edges=1

The filename above is the module name when built as a module. For a built-in
driver, use the corresponding ``airoha_xdsl.`` boot-parameter prefix. Network
interface open triggers startup; loading the module alone does not issue
the AFE sequence. Use the GDM2 interface selected by the board's DT.

``afe_annex_b=0`` applies the recovered Annex A software-default branch;
``1`` selects Annex B. Other annex combinations and automatic switching
are not implemented. ``afe_edges`` is a three-bit edge setting, range 0..7;
1 corresponds to the legacy default. These are bring-up controls rather
than a negotiated line profile or a complete board calibration interface.

The sequence applies the recovered DMT system bytes and edge settings,
selects the EN7556 control mux, reads analog register 0x2e, submits thirteen
four-register analog batches and writes the target's digital defaults.
Digital registers 7, 8, 17, 25 and 26 are skipped as in the vendor sequence.
The control bus is returned to digital mode after every command. Each
completion wait is bounded to 100 ms after an initial 250..500 us wait.
Failures propagate to interface open, assert resets and restore the saved
AFE/WAN mux fields; stopping also restores these fields.

There are deliberate differences from the legacy initialization: 0x2e is
preserved throughout the first batch containing it, the final analog batch
is not submitted twice, and CPU-dependent busy loops/retries are replaced
with bounded waits. Completion clearing, command latching, edge polarity,
clock/reset prerequisites and timing remain unverified on hardware. A
timeout or success status must be checked against register traces during
bring-up. The defaults do not guarantee correct line-driver power, impedance
or PSD for a board; AFE programming should first be tested on a disconnected
lab device. Resetting DMT does not prove the external analog chip is powered
down. No full analog rollback or calibration is claimed.

Validation
-----------

``CONFIG_NET_AIROHA_DMT_KUNIT_TEST`` exercises both format specifications,
truncation/trailing data, header and CRC errors, register bounds, full-width
field writes, the vendor duplicate IDs and package decoding. It is standalone
and can run under UML together with the OMCI and PTM tests. It does not
exercise physical AFE MMIO or analog behavior.

``tools/net/airoha/test_dmt_data.py`` takes the two audited module paths and
checks golden images/defaults, dynamic placeholders, source fingerprints,
relocation rejection and unsupported/truncated MIPS initialization.
Neither these tests nor cross-compilation establish DSL synchronization.
