.. SPDX-License-Identifier: GPL-2.0-only

xPON PLOAM codecs
=================

``include/net/xpon/ploam.h`` contains protocol vocabulary and
``include/net/xpon/ploam_msg.h`` defines decoded messages. The byte codecs in
``net/xpon/ploam_msg.c`` do not allocate memory, access hardware, change ONU
state, schedule timers, or decide whether a message needs acknowledgment.
The XGS-PON vocabulary and codec are adapted from John Crispin's PON RFC.

GPON and XGS-PON
---------------

The two formats have separate namespaces and entry points:

================== ======================== ===========================
Property           GPON                     XGS-PON
================== ======================== ===========================
Recommendation     G.984.3                  G.9807.1
Body length        12 bytes                 40 bytes
Content length     10 bytes                 36 bytes
ONU-ID             8 bits                   10 bits, big-endian field
Sequence number    None                     One octet
Integrity trailer  One BIP-8 octet          Eight MIC octets
API prefix         ``gpon_ploam_``          ``xgspon_ploam_``
Constant prefix    ``GPON_PLOAM_``          ``XGSPON_PLOAM_``
================== ======================== ===========================

Do not infer a wire layout from the size of a decoded C structure. In
particular, the XGS-PON message unions contain native integers and padding.
Use the byte codec and its body-length constant.

The GPON codec preserves every message identifier and all ten content
octets. The existing Airoha GPON provider continues to interpret that content
and implement O1 through O7, key exchange, duplicate filtering and vendor
recovery policy. Its Disable_SN mode values are retained independently of
the XGS-PON values.

The XGS-PON builder currently supports Serial_Number_ONU, Registration,
Key_Report and Acknowledgment. Serial_Number_ONU advertises only the
9.95328 Gbit/s upstream rate, uses ONU-ID 0x3ff and sequence number zero.
The parser supports Burst_Profile, Assign_ONU-ID, Ranging_Time, Deactivate,
Disable_Serial_Number, Request_Registration, Assign_Alloc-ID, Key_Control
and Reboot_ONU. Sleep messages have vocabulary definitions but no codec
implementation yet.

Driver boundary
---------------

The MAC driver owns FIFO word order, vendor metadata, integrity checking and
integrity generation. The XGS-PON codec handles only the 40-byte standard
body, excluding the MIC. It must receive an authenticated message before
the provider applies any hardware effect.

For the Airoha GPON FIFO, a numeric register word holds the first wire octet
in bits 31:24 on both big- and little-endian CPUs. The private driver adapter
uses ``put_unaligned_be32()`` before parsing and ``get_unaligned_be32()``
after building. Applying ``be32_to_cpu()`` directly to the register value
would swap valid messages on little-endian platforms.

For EN7580 XGS-PON, the SDK uses thirteen RX words, including MAC metadata
and the MIC, and eleven TX words, with the MIC supplied by hardware. These
are MAC layouts, not body lengths. FIFO adapters and activation are still
needed before the EN7580 provider can use the XGS-PON codec operationally.

Validation and errors
---------------------

The caller supplies valid message and buffer pointers. Short buffers return
``-ENOSPC`` without changing the destination. Builders return the number of
body bytes written; parsers return zero on success.

The XGS-PON parser masks reserved bits and validates burst delimiter and
preamble lengths. It leaves state-dependent validation to the provider:
destination, serial number, assignable ONU-ID and Alloc-ID, key index and
length, line rate, sequence/replay handling and reboot policy. The range
helpers describe protocol limits, not a MAC's table capacity.

After ``-EINVAL`` or ``-EOPNOTSUPP``, the XGS-PON downstream header is valid
and can be used to choose an acknowledgment code. The content must not be
applied to hardware. After ``-ENOSPC``, no header is available.

``CONFIG_XPON_PLOAM_KUNIT_TEST`` exercises wire offsets, reserved fields,
range checks, short buffers and unaligned buffers without optical hardware.
It does not establish activation, MIC or FIFO correctness on a real ONU.
