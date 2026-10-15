.. SPDX-License-Identifier: GPL-2.0-only

xPON logical endpoints and Ethernet UNIs
=======================================

Scope
-----

The xPON core exposes a port, a firmware UNI inventory, local EPON link
identities and optional Ethernet endpoints. Hardware providers translate
protocol identifiers into their own channels and queues. This interface
is experimental and does not modify DSA or require an Ethernet switch.

The Airoha v1 backend selects GPON or EPON with ``airoha,pon-mode`` at
probe. It advertises only the selected mode: runtime engine switching is
not implemented. This backend covers the existing 1G MAC implementation;
it does not add an EN7580/EN7581 10G MAC implementation or XGS-PON support.

Existing HGU aggregate forwarding is retained unless an endpoint is
explicitly created. SFU GEM services create Ethernet endpoints through a
coalescing work item. The protocol callback does not take RTNL, and the
netdev appears after the provider installs its service. Endpoint updates
are reported through the xPON events multicast group. Creation errors are
logged; SFU RX never falls back to the aggregate interface when the
endpoint is absent. Netdevice creation is not a guarantee of hardware
forwarding offload.

SFU inventory
-------------

A PON node can describe its physical subscriber interfaces::

    omci-type = "sfu";
    omci-ethernets = <&gsw_port1>, <&gsw_port2>,
                    <&gsw_port3>, <&gsw_port4>;

Each reference is an Ethernet netdev node or a DSA user-port node. CPU
ports, switch interconnects, disabled nodes and duplicate references are
rejected. The phandle order defines UNI IDs starting at one. Interface
names, Linux ifindices, switch port numbers and OMCI entity IDs are separate
identifiers. Interfaces are resolved when queried or configured, so probe
order and Ethernet driver reprobe do not invalidate the inventory.

SFU requires a nonempty inventory of up to sixteen UNIs. OMCI defaults use
that inventory before the first MIB upload. Physical UNI entities use the
existing Ethernet slot 1 mapping, ``0x0100 | UNI ID``. SFU does not seed the
HGU VEIP defaults. Administrative lock/unlock is applied through the
normal Ethernet netdevice API. DSA continues to own its own hardware.
Other Ethernet operations, bridge membership, VLANs and QoS remain the
responsibility of the service manager using rtnetlink, ethtool and tc.
This patch does not configure bridges automatically or integrate netifd
ownership. An SFU manager must coordinate the selected ports with netifd.

Generic Netlink
---------------

Family ``xpon``, version 1, keeps the existing command and attribute numbers.
New attributes and commands are appended. The implementation currently
operates in the initial network namespace. Every request identifies the
PON datapath using ``XPON_ATTR_IFINDEX``. Mutations require CAP_NET_ADMIN.
The ``oam`` multicast group also requires CAP_NET_ADMIN.

``GET_CAPS`` reports provider limits, available operations, initial ONU
personality and UNI count. Missing optical state attributes mean that the
state is unavailable; they must not be interpreted as a measured zero.
FEC or encryption operations are not advertised merely because a vendor
header declares a function.

``UNI_GET``, ``LINK_GET`` and ``ENDPOINT_GET`` support individual GETs and
multipart dumps. Object IDs start at one. LINK_GET exposes administrative
state, MPCP registration state, the assigned LLID and OAM frame counters.
These counters are lifetime software transport counters, not hardware
MPCP, traffic or FEC counters.

``LINK_SET`` enables or locally deregisters a registration instance. The
OLT assigns its LLID. The v1 backend enables the first instance by default.
Other instances are enabled explicitly. Registration changes are safe from
hard IRQ context. A pending channel retirement prevents publication of a
new operational binding until the previous channel is drained. A retirement
failure leaves the logical endpoint inactive and is logged.

``ENDPOINT_NEW``, ``ENDPOINT_SET`` and ``ENDPOINT_DEL`` manage netdevs.
A GEM endpoint uses GEM Port-ID and Alloc-ID; an LLID endpoint uses a local
link ID and obtains the current wire LLID from the backend. Channel numbers
are never part of this UAPI. The queue is a provider-supported upstream
queue. Endpoint creation does not create a GEM, assign an LLID or provision
a T-CONT; the protocol resource must already exist. The Airoha GEM backend also
requires an installed service rule for the requested GEM.

Updating a binding preserves the interface and ifindex. A registered LLID
endpoint survives an MPCP outage and is rebound to its newly assigned LLID.
Duplicate receive identities are rejected. Deletion removes the receive
mapping before unregistering the interface and waits for network readers.
The physical PON device persists. Network namespace migration is disabled
for endpoint netdevs. They do not assume checksum or GSO offload.

SFU automatic GEM endpoints use ``0x80000000 | (GEM Port-ID + 1)`` as their
local object ID. Multiple service rules referring to the same GEM share
one endpoint; its default queue comes from the most recent service update.
Per-priority queue policy should be configured by the service manager.

``REPORT_GET`` and ``REPORT_SET`` address a local link, queue and threshold
set. Thresholds are in MPCP time quanta (16 ns), never native SDK structs or
implicit byte counts. ``MAX_REPORT_SETS`` advertises the number of threshold
sets; ``REPORT_SETS`` is the configured number. The v1 backend supports three
additional threshold sets and eight queues. The total-occupancy queue set
is separate from those additional thresholds. Updates are individual table
operations, not an atomic multi-queue transaction. The MAC must be running.

``OAM_TX`` and ``OAM_RX`` carry complete Ethernet frames without FCS or a
private LLID/channel prefix. Frames use EtherType 0x8809 and subtype 3.
The transport validates those fields and frame length, associates the
frame with a registered local link, and pads short transmissions. It does
not parse or acknowledge organization-specific messages. Discovery,
keepalive, rate limits, authentication and CTC/CUC, DPoE or SIEPON service
profiles belong to the userspace OAM daemon. The old incomplete in-kernel
Information-only parser is replaced with this transport.

Command-line examples
---------------------

``tools/net/xpon/xponctl.py`` uses only the Python standard library. Numeric
netlink attributes use native CPU byte order; Ethernet frames remain in
network byte order. Example interface and protocol IDs must be replaced
with values actually provisioned by the OLT::

    python3 tools/net/xpon/xponctl.py pon0 caps
    python3 tools/net/xpon/xponctl.py pon0 unis
    python3 tools/net/xpon/xponctl.py pon0 links
    python3 tools/net/xpon/xponctl.py pon0 endpoints
    python3 tools/net/xpon/xponctl.py pon0 monitor
    python3 tools/net/xpon/xponctl.py pon0 oam-monitor

    python3 tools/net/xpon/xponctl.py pon0 link-set --id 2 --enabled 1
    python3 tools/net/xpon/xponctl.py pon0 endpoint-new --id 1 \
        --type llid --link-id 2 --queue 0
    python3 tools/net/xpon/xponctl.py pon0 endpoint-del --id 1

    python3 tools/net/xpon/xponctl.py pon0 endpoint-new --id 1 \
        --type gem --wire-id 247 --scheduler-id 307 --queue 0
    python3 tools/net/xpon/xponctl.py pon0 report-get --id 1 --queue 0 --set 0
    python3 tools/net/xpon/xponctl.py pon0 report-set --id 1 --queue 0 \
        --set 0 --sets 1 --threshold-tq 1000

Endpoint netdevs can then be configured using iproute2 and attached to a
service bridge containing the selected UNI. Keep management traffic
independent from subscriber bridge forwarding. The OAM daemon must use
its dedicated transport while subscriber interfaces are administratively
locked.

Validation
----------

Enable CONFIG_XPON_KUNIT_TEST with built-in xPON and KUnit to exercise the
provider-independent object lifecycle. The tests use a hardware-free
provider and do not prove MAC, QDMA, PHY or OLT interoperability. Cross-build
net/xpon and the Airoha Ethernet objects on ARM and MIPS BE/LE. Hardware
validation should cover registration loss, a changed assigned LLID, repeated
service creation/deletion, UNI lock/unlock and OAM while data is locked.
