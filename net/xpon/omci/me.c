// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI managed entity catalogue and attribute descriptors
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <net/xpon/omci.h>

#include "internal.h"
#include "me.h"

struct omci_class_name {
	u16 class_id;
	const char *name;
};

static const struct omci_class_name omci_classes[] = {
	{ 1, "ONT" },
	{ 2, "ONU data" },
	{ 3, "PON IF line cardholder" },
	{ 4, "PON IF line card" },
	{ 5, "Cardholder" },
	{ 6, "Circuit pack" },
	{ 7, "Software image" },
	{ 8, "UNI" },
	{ 9, "TC adapter" },
	{ 10, "Physical path termination point ATM UNI" },
	{ 11, "Physical path termination point Ethernet UNI" },
	{ 12, "Physical path termination point CES UNI" },
	{ 13, "Logical N x 64 kbit/s sub-port connection termination point (CTP)" },
	{ 14, "Interworking VCC termination point" },
	{ 15, "AAL 1 profile" },
	{ 16, "AAL 5 profile" },
	{ 17, "AAL 1 protocol monitoring history data" },
	{ 18, "AAL 5 performance monitoring history data" },
	{ 19, "AAL 2 profile" },
	{ 20, "Intentionally left blank" },
	{ 21, "CES service profile" },
	{ 22, "Reserved" },
	{ 23, "CES physical interface performance monitoring history data" },
	{ 24, "Ethernet performance monitoring history data" },
	{ 25, "VP network CTP" },
	{ 26, "ATM VP cross-connection" },
	{ 27, "Priority queue" },
	{ 28, "DBR/CBR traffic descriptor" },
	{ 29, "UBR traffic descriptor" },
	{ 30, "SBR1/VBR1 traffic descriptor" },
	{ 31, "SBR2/VBR2 traffic descriptor" },
	{ 32, "SBR3/VBR3 traffic descriptor" },
	{ 33, "ABR traffic descriptor" },
	{ 34, "GFR traffic descriptor" },
	{ 35, "ABT/DT/IT traffic descriptor" },
	{ 36, "UPC disagreement monitoring history data" },
	{ 37, "Intentionally left blank" },
	{ 38, "ANI" },
	{ 39, "PON TC adapter" },
	{ 40, "PON physical path termination point" },
	{ 41, "TC adapter protocol monitoring history data" },
	{ 42, "Threshold data" },
	{ 43, "Operator specific" },
	{ 44, "Vendor specific" },
	{ 45, "MAC bridge service profile" },
	{ 46, "MAC bridge configuration data" },
	{ 47, "MAC bridge port configuration data" },
	{ 48, "MAC bridge port designation data" },
	{ 49, "MAC bridge port filter table data" },
	{ 50, "MAC bridge port bridge table data" },
	{ 51, "MAC bridge performance monitoring history data" },
	{ 52, "MAC bridge port performance monitoring history data" },
	{ 53, "Physical path termination point POTS UNI" },
	{ 54, "Voice CTP" },
	{ 55, "Voice PM history data" },
	{ 56, "AAL2 PVC profile" },
	{ 57, "AAL2 CPS protocol monitoring history data" },
	{ 58, "Voice service profile" },
	{ 59, "LES service profile" },
	{ 60, "AAL2 SSCS parameter profile 1" },
	{ 61, "AAL2 SSCS parameter profile 2" },
	{ 62, "VP performance monitoring history data" },
	{ 63, "Traffic scheduler" },
	{ 64, "T-CONT buffer" },
	{ 65, "UBR+ traffic descriptor" },
	{ 66, "AAL2 SSCS protocol monitoring history data" },
	{ 67, "IP port configuration data" },
	{ 68, "IP router service profile" },
	{ 69, "IP router configuration data" },
	{ 70, "IP router performance monitoring history data 1" },
	{ 71, "IP router performance monitoring history data 2" },
	{ 72, "ICMP performance monitoring history data 1" },
	{ 73, "ICMP performance monitoring history data 2" },
	{ 74, "IP route table" },
	{ 75, "IP static routes" },
	{ 76, "ARP service profile" },
	{ 77, "ARP configuration data" },
	{ 78, "VLAN tagging operation configuration data" },
	{ 79, "MAC bridge port filter pre-assign table" },
	{ 80, "Physical path termination point ISDN UNI" },
	{ 81, "Reserved" },
	{ 82, "Physical path termination point video UNI" },
	{ 83, "Physical path termination point LCT UNI" },
	{ 84, "VLAN tagging filter data" },
	{ 85, "ONU" },
	{ 86, "ATM VC cross-connection" },
	{ 87, "VC network CTP" },
	{ 88, "VC PM history data" },
	{ 89, "Ethernet performance monitoring history data 2" },
	{ 90, "Physical path termination point video ANI" },
	{ 91, "Physical path termination point IEEE 802.11 UNI" },
	{ 92, "IEEE 802.11 station management data 1" },
	{ 93, "IEEE 802.11 station management data 2" },
	{ 94, "IEEE 802.11 general purpose object" },
	{ 95, "IEEE 802.11 MAC and PHY operation and antenna data" },
	{ 96, "IEEE 802.11 performance monitoring history data" },
	{ 97, "IEEE 802.11 PHY FHSS DSSS IR tables" },
	{ 98, "Physical path termination point xDSL UNI part 1" },
	{ 99, "Physical path termination point xDSL UNI part 2" },
	{ 100, "xDSL line inventory and status data part 1" },
	{ 101, "xDSL line inventory and status data part 2" },
	{ 102, "xDSL channel downstream status data" },
	{ 103, "xDSL channel upstream status data" },
	{ 104, "xDSL line configuration profile part 1" },
	{ 105, "xDSL line configuration profile part 2" },
	{ 106, "xDSL line configuration profile part 3" },
	{ 107, "xDSL channel configuration profile" },
	{ 108, "xDSL subcarrier masking downstream profile" },
	{ 109, "xDSL subcarrier masking upstream profile" },
	{ 110, "xDSL PSD mask profile" },
	{ 111, "xDSL downstream radio frequency interference (RFI) bands profile" },
	{ 112, "xDSL xTU-C performance monitoring history data" },
	{ 113, "xDSL xTU-R performance monitoring history data" },
	{ 114, "xDSL xTU-C channel performance monitoring history data" },
	{ 115, "xDSL xTU-R channel performance monitoring history data" },
	{ 116, "TC adaptor performance monitoring history data xDSL" },
	{ 117, "Physical path termination point VDSL UNI (ITU-T G.993.1 VDSL1)" },
	{ 118, "VDSL VTU-O physical data" },
	{ 119, "VDSL VTU-R physical data" },
	{ 120, "VDSL channel data" },
	{ 121, "VDSL line configuration profile" },
	{ 122, "VDSL channel configuration profile" },
	{ 123, "VDSL band plan configuration profile" },
	{ 124, "VDSL VTU-O physical interface monitoring history data" },
	{ 125, "VDSL VTU-R physical interface monitoring history data" },
	{ 126, "VDSL VTU-O channel performance monitoring history data" },
	{ 127, "VDSL VTU-R channel performance monitoring history data" },
	{ 128, "Video return path service profile" },
	{ 129, "Video return path performance monitoring history data" },
	{ 130, "IEEE 802.1p mapper service profile" },
	{ 131, "OLT-G" },
	{ 132, "Multicast interworking VCC termination point" },
	{ 133, "ONU power shedding" },
	{ 134, "IP host config data" },
	{ 135, "IP host performance monitoring history data" },
	{ 136, "TCP/UDP config data" },
	{ 137, "Network address" },
	{ 138, "VoIP config data" },
	{ 139, "VoIP voice CTP" },
	{ 140, "Call control performance monitoring history data" },
	{ 141, "VoIP line status" },
	{ 142, "VoIP media profile" },
	{ 143, "RTP profile data" },
	{ 144, "RTP performance monitoring history data" },
	{ 145, "Network dial plan table" },
	{ 146, "VoIP application service profile" },
	{ 147, "VoIP feature access codes" },
	{ 148, "Authentication security method" },
	{ 149, "SIP config portal" },
	{ 150, "SIP agent config data" },
	{ 151, "SIP agent performance monitoring history data" },
	{ 152, "SIP call initiation performance monitoring history data" },
	{ 153, "SIP user data" },
	{ 154, "MGC config portal" },
	{ 155, "MGC config data" },
	{ 156, "MGC performance monitoring history data" },
	{ 157, "Large string" },
	{ 158, "ONU remote debug" },
	{ 159, "Equipment protection profile" },
	{ 160, "Equipment extension package" },
	{ 161, "Port-mapping package (legacy B-PON)" },
	{ 162, "Physical path termination point MoCA UNI" },
	{ 163, "MoCA Ethernet performance monitoring history data" },
	{ 164, "MoCA interface performance monitoring history data" },
	{ 165, "VDSL2 line configuration extensions" },
	{ 166, "xDSL line inventory and status data part 3" },
	{ 167, "xDSL line inventory and status data part 4" },
	{ 168, "VDSL2 line inventory and status data part 1" },
	{ 169, "VDSL2 line inventory and status data part 2" },
	{ 170, "VDSL2 line inventory and status data part 3" },
	{ 171, "Extended VLAN tagging operation configuration data" },
	{ 256, "ONU-G" },
	{ 257, "ONU2-G" },
	{ 258, "ONU-G (deprecated)" },
	{ 259, "ONU2-G (deprecated)" },
	{ 260, "PON IF line card-G" },
	{ 261, "PON TC adapter-G" },
	{ 262, "T-CONT" },
	{ 263, "ANI-G" },
	{ 264, "UNI-G" },
	{ 265, "ATM interworking VCC termination point" },
	{ 266, "GEM interworking termination point" },
	{ 267, "GEM port performance monitoring history data (obsolete)" },
	{ 268, "GEM port network CTP" },
	{ 269, "VP network CTP" },
	{ 270, "VC network CTP-G" },
	{ 271, "GAL TDM profile (deprecated)" },
	{ 272, "GAL Ethernet profile" },
	{ 273, "Threshold data 1" },
	{ 274, "Threshold data 2" },
	{ 275, "GAL TDM performance monitoring history data (deprecated)" },
	{ 276, "GAL Ethernet performance monitoring history data" },
	{ 277, "Priority queue" },
	{ 278, "Traffic scheduler" },
	{ 279, "Protection data" },
	{ 280, "Traffic descriptor" },
	{ 281, "Multicast GEM interworking termination point" },
	{ 282, "Pseudowire termination point" },
	{ 283, "RTP pseudowire parameters" },
	{ 284, "Pseudowire maintenance profile" },
	{ 285, "Pseudowire performance monitoring history data" },
	{ 286, "Ethernet flow termination point" },
	{ 287, "OMCI" },
	{ 288, "Managed entity" },
	{ 289, "Attribute" },
	{ 290, "Dot1X port extension package" },
	{ 291, "Dot1X configuration profile" },
	{ 292, "Dot1X performance monitoring history data" },
	{ 293, "Radius performance monitoring history data" },
	{ 294, "TU CTP" },
	{ 295, "TU performance monitoring history data" },
	{ 296, "Ethernet performance monitoring history data 3" },
	{ 297, "Port-mapping package" },
	{ 298, "Dot1 rate limiter" },
	{ 299, "Dot1ag maintenance domain" },
	{ 300, "Dot1ag maintenance association" },
	{ 301, "Dot1ag default MD level" },
	{ 302, "Dot1ag MEP" },
	{ 303, "Dot1ag MEP status" },
	{ 304, "Dot1ag MEP CCM database" },
	{ 305, "Dot1ag CFM stack" },
	{ 306, "Dot1ag chassis - management info" },
	{ 307, "Octet string" },
	{ 308, "General purpose buffer" },
	{ 309, "Multicast operations profile" },
	{ 310, "Multicast subscriber config info" },
	{ 311, "Multicast subscriber monitor" },
	{ 312, "FEC performance monitoring history data" },
	{ 313, "RE ANI-G" },
	{ 314, "Physical path termination point RE UNI" },
	{ 315, "RE upstream amplifier" },
	{ 316, "RE downstream amplifier" },
	{ 317, "RE config portal" },
	{ 318, "File transfer controller" },
	{ 319, "CES physical interface performance monitoring history data 2" },
	{ 320, "CES physical interface performance monitoring history data 3" },
	{ 321, "Ethernet frame performance monitoring history data downstream" },
	{ 322, "Ethernet frame performance monitoring history data upstream" },
	{ 323, "VDSL2 line configuration extensions 2" },
	{ 324, "xDSL impulse noise monitor performance monitoring history data" },
	{ 325, "xDSL line inventory and status data part 5" },
	{ 326, "xDSL line inventory and status data part 6" },
	{ 327, "xDSL line inventory and status data part 7" },
	{ 328, "RE common amplifier parameters" },
	{ 329, "Virtual Ethernet interface point" },
	{ 330, "Generic status portal" },
	{ 331, "ONU-E" },
	{ 332, "Enhanced security control" },
	{ 333, "MPLS pseudowire termination point" },
	{ 334, "Ethernet frame extended PM" },
	{ 335, "Simple network management protocol (SNMP) configuration data" },
	{ 336, "ONU dynamic power management control" },
	{ 337, "PW ATM configuration data" },
	{ 338, "PW ATM performance monitoring history data" },
	{ 339, "PW Ethernet configuration data" },
	{ 340, "BBF TR-069 management server" },
	{ 341, "GEM port network CTP performance monitoring history data" },
	{ 342, "TCP/UDP performance monitoring history data" },
	{ 343, "Energy consumption performance monitoring history data" },
	{ 344, "XG-PON TC performance monitoring history data" },
	{ 345, "XG-PON downstream management performance monitoring history data" },
	{ 346, "XG-PON upstream management performance monitoring history data" },
	{ 347, "IPv6 host config data" },
	{ 348, "MAC bridge port ICMPv6 process pre-assign table" },
	{ 349, "Power over Ethernet (PoE) control" },
	{ 400, "Ethernet pseudowire parameters" },
	{ 401, "Physical path termination point RS232/RS485 UNI" },
	{ 402, "RS232/RS485 port operation configuration data" },
	{ 403, "RS232/RS485 performance monitoring history data" },
	{ 404, "L2 multicast GEM interworking termination point" },
	{ 405, "ANI-E" },
	{ 406, "EPON downstream performance monitoring configuration" },
	{ 407, "SIP agent config data 2" },
	{ 408, "xDSL xTU-C performance monitoring history data part 2" },
	{ 409, "PTM performance monitoring history data xDSL" },
	{ 410, "VDSL2 line configuration extensions 3" },
	{ 411, "Vectoring line configuration extensions" },
	{ 412, "xDSL channel configuration profile part 2" },
	{ 413, "xTU data gathering configuration" },
	{ 414, "xDSL line inventory and status data part 8" },
	{ 415, "VDSL2 line inventory and status data part 4" },
	{ 416, "Vectoring line inventory and status data" },
	{ 417, "Data gathering line test, diagnostic and status" },
	{ 419, "EFM bonding group" },
	{ 420, "EFM bonding link" },
	{ 421, "EFM bonding group performance monitoring history data" },
	{ 422, "EFM bonding group performance monitoring history data part 2" },
	{ 423, "EFM bonding link performance monitoring history data" },
	{ 424, "EFM bonding port performance monitoring history data" },
	{ 425, "EFM bonding port performance monitoring history data part 2" },
	{ 426, "Ethernet frame extended PM 64 bit" },
	{ 427, "Physical path termination point xDSL UNI part 3" },
	{ 428, "FAST line configuration profile part 1" },
	{ 429, "FAST line configuration profile part 2" },
	{ 430, "FAST line configuration profile part 3" },
	{ 431, "FAST line configuration profile part 4" },
	{ 432, "FAST channel configuration profile" },
	{ 433, "FAST data path configuration profile" },
	{ 434, "FAST vectoring line configuration extensions" },
	{ 435, "FAST line inventory and status data" },
	{ 436, "FAST line inventory and status data part 2" },
	{ 437, "FAST xTU-C performance monitoring history data" },
	{ 438, "FAST xTU-R performance monitoring history data" },
	{ 439, "OpenFlow config data" },
	{ 440, "Time Status Message" },
	{ 441, "ONU3-G" },
	{ 442, "TWDM System Profile managed entity" },
	{ 443, "TWDM channel managed entity" },
	{ 444, "TWDM channel PHY/LODS performance monitoring history data" },
	{ 445, "TWDM channel XGEM performance monitoring history data" },
	{ 446, "TWDM channel PLOAM performance monitoring history data part 1" },
	{ 447, "TWDM channel PLOAM performance monitoring history data part 2" },
	{ 448, "TWDM channel PLOAM performance monitoring history data part 3" },
	{ 449, "TWDM channel tuning performance monitoring history data part 1" },
	{ 450, "TWDM channel tuning performance monitoring history data part 2" },
	{ 451, "TWDM channel tuning performance monitoring history data part 3" },
	{ 452, "TWDM channel OMCI performance monitoring history data" },
};

#define OMCI_ATTR(_bit, _offset, _len, _access) \
	{ .mask = BIT(_bit), .offset = (_offset), .len = (_len), \
	  .access = (_access) }

#define OMCI_RW_SC	(OMCI_ATTR_ACCESS_READ | OMCI_ATTR_ACCESS_WRITE | \
			 OMCI_ATTR_ACCESS_SET_CREATE)
#define OMCI_RW		(OMCI_ATTR_ACCESS_READ | OMCI_ATTR_ACCESS_WRITE)
#define OMCI_R		OMCI_ATTR_ACCESS_READ

static const struct omci_attr_desc omci_software_image_attrs[] = {
	OMCI_ATTR(15, 0, OMCI_SOFTWARE_VERSION_LEN, OMCI_R),
	OMCI_ATTR(14, 14, 1, OMCI_R),
	OMCI_ATTR(13, 15, 1, OMCI_R),
	OMCI_ATTR(12, 16, 1, OMCI_R),
};

static const struct omci_attr_desc omci_cardholder_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 20, OMCI_RW),
	OMCI_ATTR(11, 23, 20, OMCI_RW),
	OMCI_ATTR(10, 43, 2, OMCI_R),
};

static const struct omci_attr_desc omci_circuit_pack_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 8, OMCI_R),
	OMCI_ATTR(12, 10, 14, OMCI_R),
	OMCI_ATTR(11, 24, 4, OMCI_R),
	OMCI_ATTR(10, 28, 1, OMCI_RW),
	OMCI_ATTR(9, 29, 1, OMCI_R),
	OMCI_ATTR(8, 30, 1, OMCI_RW),
	OMCI_ATTR(7, 31, 20, OMCI_R),
	OMCI_ATTR(6, 51, 1, OMCI_RW),
	OMCI_ATTR(5, 52, 1, OMCI_R),
	OMCI_ATTR(4, 53, 1, OMCI_R),
	OMCI_ATTR(3, 54, 1, OMCI_R),
	OMCI_ATTR(2, 55, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_pptp_ethernet_uni_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 1, OMCI_RW),
	OMCI_ATTR(10, 5, 1, OMCI_R),
	OMCI_ATTR(9, 6, 1, OMCI_R),
	OMCI_ATTR(8, 7, 2, OMCI_RW),
	OMCI_ATTR(7, 9, 1, OMCI_RW),
	OMCI_ATTR(6, 10, 2, OMCI_RW),
	OMCI_ATTR(5, 12, 1, OMCI_RW),
	OMCI_ATTR(4, 13, 1, OMCI_RW),
	OMCI_ATTR(3, 14, 1, OMCI_RW),
	OMCI_ATTR(2, 15, 1, OMCI_RW),
	OMCI_ATTR(1, 16, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_bridge_port_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 3, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 6, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 8, 2, OMCI_RW_SC),
	OMCI_ATTR(9, 10, 1, OMCI_RW_SC),
	OMCI_ATTR(8, 11, 1, OMCI_RW_SC),
	OMCI_ATTR(7, 12, 1, OMCI_RW_SC),
	OMCI_ATTR(6, 13, 6, OMCI_R),
	OMCI_ATTR(5, 19, 2, OMCI_RW),
	OMCI_ATTR(4, 21, 2, OMCI_RW),
	OMCI_ATTR(3, 23, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_vlan_filter_attrs[] = {
	OMCI_ATTR(15, 0, 24, OMCI_RW_SC),
	OMCI_ATTR(14, 24, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 25, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_8021p_mapper_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 6, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 8, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 10, 2, OMCI_RW_SC),
	OMCI_ATTR(9, 12, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 14, 2, OMCI_RW_SC),
	OMCI_ATTR(7, 16, 2, OMCI_RW_SC),
	OMCI_ATTR(6, 18, 1, OMCI_RW_SC),
	OMCI_ATTR(5, 19, 24, OMCI_RW),
	OMCI_ATTR(4, 43, 1, OMCI_RW_SC),
	OMCI_ATTR(3, 44, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_extended_vlan_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 2, OMCI_R),
	OMCI_ATTR(13, 3, 2, OMCI_RW),
	OMCI_ATTR(12, 5, 2, OMCI_RW),
	OMCI_ATTR(11, 7, 1, OMCI_RW),
	OMCI_ATTR(10, 8, 16, OMCI_RW),
	OMCI_ATTR(9, 24, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 26, 24, OMCI_RW),
};

static const struct omci_attr_desc omci_tcont_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 1, OMCI_R),
	OMCI_ATTR(13, 3, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_uni_g_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 1, OMCI_RW),
	OMCI_ATTR(13, 3, 1, OMCI_R),
	OMCI_ATTR(12, 4, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_gem_iwtp_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 3, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 5, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 7, 1, OMCI_R),
	OMCI_ATTR(10, 8, 1, OMCI_R),
	OMCI_ATTR(9, 9, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 11, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_gem_port_ctp_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 4, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 5, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 7, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 9, 1, OMCI_R),
	OMCI_ATTR(9, 10, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 12, 1, OMCI_R),
	OMCI_ATTR(7, 13, 2, OMCI_RW_SC),
	OMCI_ATTR(6, 15, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_veip_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 25, OMCI_RW),
	OMCI_ATTR(12, 27, 2, OMCI_RW),
	OMCI_ATTR(11, 29, 2, OMCI_R),
};

static const struct omci_attr_desc omci_huawei_350_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 2, OMCI_R),
	OMCI_ATTR(10, 6, 4, OMCI_RW),
	OMCI_ATTR(9, 10, 16, OMCI_R),
	OMCI_ATTR(8, 26, 4, OMCI_R),
	OMCI_ATTR(7, 30, 6, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_352_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 6, 1, OMCI_RW_SC),
	OMCI_ATTR(11, 7, 4, OMCI_RW_SC),
	OMCI_ATTR(10, 11, 4, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_huawei_353_attrs[] = {
	OMCI_ATTR(15, 0, 14, OMCI_R),
	OMCI_ATTR(14, 14, 1, OMCI_R),
	OMCI_ATTR(13, 15, 1, OMCI_R),
	OMCI_ATTR(12, 16, 1, OMCI_R),
	OMCI_ATTR(11, 17, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_multicast_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 2, OMCI_RW),
	OMCI_ATTR(13, 3, 2, OMCI_RW),
	OMCI_ATTR(12, 5, 2, OMCI_RW),
	OMCI_ATTR(11, 7, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_nokia_65295_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 2, OMCI_R),
	OMCI_ATTR(13, 3, 1, OMCI_R),
	OMCI_ATTR(12, 4, 2, OMCI_R),
	OMCI_ATTR(11, 6, 1, OMCI_R),
	OMCI_ATTR(10, 7, 2, OMCI_R),
	OMCI_ATTR(9, 9, 1, OMCI_R),
	OMCI_ATTR(8, 10, 2, OMCI_R),
	OMCI_ATTR(7, 12, 1, OMCI_R),
	OMCI_ATTR(6, 13, 2, OMCI_R),
};

static const struct omci_attr_desc omci_nokia_65296_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 4, OMCI_R),
	OMCI_ATTR(12, 6, 1, OMCI_RW),
	OMCI_ATTR(11, 7, 1, OMCI_RW),
	OMCI_ATTR(10, 8, 1, OMCI_RW),
	OMCI_ATTR(9, 9, 4, OMCI_R),
	OMCI_ATTR(8, 13, 1, OMCI_R),
	OMCI_ATTR(7, 14, 1, OMCI_RW),
	OMCI_ATTR(6, 15, 12, OMCI_R),
	OMCI_ATTR(5, 27, 4, OMCI_R),
	OMCI_ATTR(4, 31, 25, OMCI_R),
};

static const struct omci_attr_desc omci_nokia_65297_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 2, OMCI_RW),
	OMCI_ATTR(12, 4, 1, OMCI_RW),
	OMCI_ATTR(11, 5, 1, OMCI_RW),
	OMCI_ATTR(10, 6, 1, OMCI_RW),
	OMCI_ATTR(9, 7, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_nokia_65305_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 6, OMCI_RW),
};

static const struct omci_attr_desc omci_nokia_65306_attrs[] = {
	OMCI_ATTR(15, 0, 19, OMCI_RW),
};

static const struct omci_attr_desc omci_omci_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_R),
	OMCI_ATTR(14, 4, 4, OMCI_R),
};

static const struct omci_attr_desc omci_managed_entity_attrs[] = {
	OMCI_ATTR(15, 0, 25, OMCI_R),
	OMCI_ATTR(14, 25, 4, OMCI_R),
	OMCI_ATTR(13, 29, 1, OMCI_R),
	OMCI_ATTR(12, 30, 4, OMCI_R),
	OMCI_ATTR(11, 34, 4, OMCI_R),
	OMCI_ATTR(10, 38, 4, OMCI_R),
	OMCI_ATTR(9, 42, 4, OMCI_R),
	OMCI_ATTR(8, 46, 1, OMCI_R),
};

static const struct omci_attr_desc omci_attribute_attrs[] = {
	OMCI_ATTR(15, 0, 25, OMCI_R),
	OMCI_ATTR(14, 25, 2, OMCI_R),
	OMCI_ATTR(13, 27, 1, OMCI_R),
	OMCI_ATTR(12, 28, 1, OMCI_R),
	OMCI_ATTR(11, 29, 4, OMCI_R),
	OMCI_ATTR(10, 33, 4, OMCI_R),
	OMCI_ATTR(9, 37, 4, OMCI_R),
	OMCI_ATTR(8, 41, 4, OMCI_R),
	OMCI_ATTR(7, 45, 1, OMCI_R),
};

#define STANDARD_ACTIONS (OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET)
#define OLT_CREATED_ACTIONS (OMCI_ME_ACTION_CREATE | OMCI_ME_ACTION_DELETE | \
			     OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET)
#define STANDARD_DESC(_class, _name, _actions, _flags, _mask, _upload, \
		      _len, _category, _support, _attrs) \
	{ .class_id = (_class), .name = (_name), \
	  .profile_mask = OMCI_PROFILE_STANDARD_MASK, .actions = (_actions), \
	  .flags = (_flags), .valid_attr_mask = (_mask), \
	  .mib_upload_mask = (_upload), .data_len = (_len), \
	  .category = (_category), .support = (_support), \
	  .attrs = (_attrs), .num_attrs = ARRAY_SIZE(_attrs) }
#define VENDOR_DESC(_class, _name, _profile, _actions, _flags, _mask, \
		    _upload, _len, _category, _support, _attrs) \
	{ .class_id = (_class), .name = (_name), \
	  .profile_mask = OMCI_PROFILE_BIT(_profile), .actions = (_actions), \
	  .flags = (_flags) | OMCI_ME_F_VENDOR, .valid_attr_mask = (_mask), \
	  .mib_upload_mask = (_upload), .data_len = (_len), \
	  .category = (_category), .support = (_support), \
	  .attrs = (_attrs), .num_attrs = ARRAY_SIZE(_attrs) }

static const struct omci_me_desc omci_me_descs[] = {
	STANDARD_DESC(OMCI_CLASS_SOFTWARE_IMAGE, "Software image",
		      OMCI_ME_ACTION_GET, OMCI_ME_F_ONU_CREATED,
		      GENMASK(15, 12), GENMASK(15, 12), 17,
		      OMCI_CLASS_CATEGORY_EQUIPMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_software_image_attrs),
	STANDARD_DESC(OMCI_CLASS_CARDHOLDER, "Cardholder", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED, GENMASK(15, 10), GENMASK(15, 10),
		      45, OMCI_CLASS_CATEGORY_EQUIPMENT,
		      OMCI_CLASS_SUPPORT_NATIVE, omci_cardholder_attrs),
	STANDARD_DESC(OMCI_CLASS_CIRCUIT_PACK, "Circuit pack", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED, GENMASK(15, 2), GENMASK(15, 2),
		      59, OMCI_CLASS_CATEGORY_EQUIPMENT,
		      OMCI_CLASS_SUPPORT_NATIVE, omci_circuit_pack_attrs),
	STANDARD_DESC(OMCI_CLASS_PPTP_ETHERNET_UNI,
		      "Physical path termination point Ethernet UNI",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		      GENMASK(15, 1), GENMASK(15, 1), 17,
		      OMCI_CLASS_CATEGORY_UNI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_pptp_ethernet_uni_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA,
		      "MAC bridge port configuration data", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, GENMASK(15, 3), GENMASK(15, 3), 24,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_bridge_port_attrs),
	STANDARD_DESC(OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
		      "VLAN tagging filter data", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, GENMASK(15, 13), GENMASK(15, 13), 26,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_vlan_filter_attrs),
	STANDARD_DESC(OMCI_CLASS_8021P_MAPPER,
		      "802.1p mapper service profile", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, GENMASK(15, 3), GENMASK(15, 3), 45,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_8021p_mapper_attrs),
	STANDARD_DESC(OMCI_CLASS_EXTENDED_VLAN,
		      "Extended VLAN tagging operation configuration data",
		      OLT_CREATED_ACTIONS | OMCI_ME_ACTION_SET_TABLE,
		      OMCI_ME_F_DATAPATH, GENMASK(15, 8), GENMASK(15, 8), 50,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_extended_vlan_attrs),
	STANDARD_DESC(OMCI_CLASS_TCONT, "T-CONT", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		      GENMASK(15, 13), GENMASK(15, 13), 4,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_tcont_attrs),
	STANDARD_DESC(OMCI_CLASS_UNI_G, "UNI-G", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED, GENMASK(15, 12), GENMASK(15, 12),
		      6, OMCI_CLASS_CATEGORY_UNI, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_uni_g_attrs),
	STANDARD_DESC(OMCI_CLASS_GEM_IWTP,
		      "GEM interworking termination point", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, GENMASK(15, 8), GENMASK(15, 8), 12,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_gem_iwtp_attrs),
	STANDARD_DESC(OMCI_CLASS_GEM_PORT_CTP, "GEM port network CTP",
		      OLT_CREATED_ACTIONS, OMCI_ME_F_DATAPATH,
		      GENMASK(15, 6), GENMASK(15, 6), 17,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_gem_port_ctp_attrs),
	STANDARD_DESC(OMCI_CLASS_OMCI, "OMCI",
		      OMCI_ME_ACTION_GET | OMCI_ME_ACTION_GET_NEXT,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_NO_MIB_UPLOAD,
		      GENMASK(15, 14), 0, 8,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_omci_attrs),
	STANDARD_DESC(OMCI_CLASS_MANAGED_ENTITY, "Managed entity",
		      OMCI_ME_ACTION_GET | OMCI_ME_ACTION_GET_NEXT,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_NO_MIB_UPLOAD,
		      GENMASK(15, 8), 0, 47,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_managed_entity_attrs),
	STANDARD_DESC(OMCI_CLASS_ATTRIBUTE, "Attribute",
		      OMCI_ME_ACTION_GET | OMCI_ME_ACTION_GET_NEXT,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_NO_MIB_UPLOAD,
		      GENMASK(15, 7), 0, 46,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_attribute_attrs),
	STANDARD_DESC(OMCI_CLASS_VEIP, "Virtual Ethernet interface point",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		      GENMASK(15, 11), GENMASK(15, 11), 31,
		      OMCI_CLASS_CATEGORY_UNI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_veip_attrs),

	VENDOR_DESC(OMCI_CLASS_HUAWEI_FLOW_MAPPING,
		    "Huawei flow mapping configuration", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    GENMASK(15, 7), GENMASK(15, 7), 36,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_huawei_350_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_VLAN_MAPPING,
		    "Huawei VLAN mapping configuration", OMCI_OLT_PROFILE_HUAWEI,
		    OLT_CREATED_ACTIONS, OMCI_ME_F_NO_MIB_UPLOAD | OMCI_ME_F_DATAPATH,
		    GENMASK(15, 10), 0, 15, OMCI_CLASS_CATEGORY_VENDOR,
		    OMCI_CLASS_SUPPORT_SHADOW, omci_huawei_352_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_SW_IMAGE_EXT,
		    "Huawei software image extension", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    GENMASK(15, 11), GENMASK(15, 11), 21,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_huawei_353_attrs),
#define HUAWEI_MULTICAST_DESC(_class, _name) \
	VENDOR_DESC((_class), (_name), OMCI_OLT_PROFILE_HUAWEI, STANDARD_ACTIONS, \
		    OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH, GENMASK(15, 11), \
		    GENMASK(15, 11), 11, \
		    OMCI_CLASS_CATEGORY_MULTICAST, OMCI_CLASS_SUPPORT_SHADOW, \
		    omci_huawei_multicast_attrs)
	HUAWEI_MULTICAST_DESC(OMCI_CLASS_HUAWEI_MULTICAST_367,
			      "Huawei multicast configuration 367"),
	HUAWEI_MULTICAST_DESC(OMCI_CLASS_HUAWEI_MULTICAST_370,
			      "Huawei multicast configuration 370"),
	HUAWEI_MULTICAST_DESC(OMCI_CLASS_HUAWEI_MULTICAST_373,
			      "Huawei multicast configuration 373"),
	HUAWEI_MULTICAST_DESC(OMCI_CLASS_HUAWEI_MULTICAST_65408,
			      "Huawei multicast configuration 65408"),
	HUAWEI_MULTICAST_DESC(OMCI_CLASS_HUAWEI_MULTICAST_65414,
			      "Huawei multicast configuration 65414"),
	HUAWEI_MULTICAST_DESC(OMCI_CLASS_HUAWEI_MULTICAST_65425,
			      "Huawei multicast configuration 65425"),
#undef HUAWEI_MULTICAST_DESC

	VENDOR_DESC(OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION,
		    "Nokia ONT optical supervision status",
		    OMCI_OLT_PROFILE_NOKIA_ALCL, OMCI_ME_ACTION_GET,
		    OMCI_ME_F_ONU_CREATED, GENMASK(15, 6), GENMASK(15, 6), 15,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_nokia_65295_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_ONT_GENERIC_V2,
		    "Nokia ONT generic V2", OMCI_OLT_PROFILE_NOKIA_ALCL,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    GENMASK(15, 4), GENMASK(15, 4), 56,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_nokia_65296_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_UNI_SUPPLEMENTAL_V2,
		    "Nokia UNI supplemental 1 V2", OMCI_OLT_PROFILE_NOKIA_ALCL,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    GENMASK(15, 9), GENMASK(15, 9), 8,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_nokia_65297_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_VLAN_DS_SUPPLEMENTAL,
		    "Nokia VLAN downstream supplemental",
		    OMCI_OLT_PROFILE_NOKIA_ALCL, OLT_CREATED_ACTIONS,
		    OMCI_ME_F_DATAPATH, GENMASK(15, 14), GENMASK(15, 14), 7,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_nokia_65305_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_VLAN_US_POLICER,
		    "Nokia upstream VLAN policer", OMCI_OLT_PROFILE_NOKIA_ALCL,
		    OLT_CREATED_ACTIONS, OMCI_ME_F_DATAPATH,
		    BIT(15), BIT(15), 19, OMCI_CLASS_CATEGORY_VENDOR,
		    OMCI_CLASS_SUPPORT_SHADOW, omci_nokia_65306_attrs),
};

static bool omci_class_name_contains(const char *name, const char *needle)
{
	return strnstr(name, needle, strlen(name));
}

static u8 omci_class_category(u16 class_id, const char *name)
{
	if ((class_id >= 240 && class_id <= 255) ||
	    (class_id >= 350 && class_id <= 399) || class_id >= 65280)
		return OMCI_CLASS_CATEGORY_VENDOR;
	if ((class_id >= 172 && class_id <= 239) ||
	    (class_id >= 453 && class_id <= 65279) ||
	    omci_class_name_contains(name, "Reserved") ||
	    omci_class_name_contains(name, "Intentionally left blank"))
		return OMCI_CLASS_CATEGORY_RESERVED;
	if (omci_class_name_contains(name, "performance monitoring") ||
	    omci_class_name_contains(name, "PM history") ||
	    omci_class_name_contains(name, "monitoring history"))
		return OMCI_CLASS_CATEGORY_PERFORMANCE;
	if (omci_class_name_contains(name, "xDSL") ||
	    omci_class_name_contains(name, "VDSL") ||
	    omci_class_name_contains(name, "FAST") ||
	    omci_class_name_contains(name, "EFM bonding"))
		return OMCI_CLASS_CATEGORY_XDSL;
	if (omci_class_name_contains(name, "VoIP") ||
	    omci_class_name_contains(name, "SIP") ||
	    omci_class_name_contains(name, "MGC") ||
	    omci_class_name_contains(name, "RTP") ||
	    omci_class_name_contains(name, "Voice") ||
	    omci_class_name_contains(name, "POTS"))
		return OMCI_CLASS_CATEGORY_VOICE;
	if (omci_class_name_contains(name, "Multicast"))
		return OMCI_CLASS_CATEGORY_MULTICAST;
	if (omci_class_name_contains(name, "security") ||
	    omci_class_name_contains(name, "Authentication") ||
	    omci_class_name_contains(name, "Dot1X") ||
	    omci_class_name_contains(name, "Radius"))
		return OMCI_CLASS_CATEGORY_SECURITY;
	if (omci_class_name_contains(name, "IP ") ||
	    omci_class_name_contains(name, "TCP/UDP") ||
	    omci_class_name_contains(name, "ARP") ||
	    omci_class_name_contains(name, "ICMP") ||
	    omci_class_name_contains(name, "TR-069") ||
	    omci_class_name_contains(name, "SNMP") ||
	    omci_class_name_contains(name, "Network address"))
		return OMCI_CLASS_CATEGORY_LAYER3;
	if (omci_class_name_contains(name, "VLAN") ||
	    omci_class_name_contains(name, "MAC bridge") ||
	    omci_class_name_contains(name, "Ethernet") ||
	    omci_class_name_contains(name, "802.1p") ||
	    omci_class_name_contains(name, "Dot1ag") ||
	    omci_class_name_contains(name, "OpenFlow"))
		return OMCI_CLASS_CATEGORY_LAYER2;
	if (omci_class_name_contains(name, "UNI"))
		return OMCI_CLASS_CATEGORY_UNI;
	if (omci_class_name_contains(name, "ANI") ||
	    omci_class_name_contains(name, "GEM") ||
	    omci_class_name_contains(name, "T-CONT") ||
	    omci_class_name_contains(name, "PON") ||
	    omci_class_name_contains(name, "Traffic scheduler") ||
	    omci_class_name_contains(name, "Priority queue"))
		return OMCI_CLASS_CATEGORY_ANI;
	if (class_id <= 7 || class_id == 133 || class_id == 159 ||
	    class_id == 160 || class_id == 256 || class_id == 257 ||
	    class_id == 297 || class_id == 331 || class_id == 336 ||
	    class_id == 441)
		return OMCI_CLASS_CATEGORY_EQUIPMENT;
	if (class_id == 137 || class_id == 157 || class_id == 158 ||
	    class_id == 287 || class_id == 288 || class_id == 289 ||
	    class_id == 307 || class_id == 308 || class_id == 318 ||
	    class_id == 330 || class_id == 440)
		return OMCI_CLASS_CATEGORY_MANAGEMENT;
	if (class_id < 256)
		return OMCI_CLASS_CATEGORY_LEGACY;

	return OMCI_CLASS_CATEGORY_OTHER;
}

static u32 omci_class_flags(u16 class_id, const char *name)
{
	u32 flags = OMCI_CLASS_F_STANDARD;
	u8 category = omci_class_category(class_id, name);

	if (omci_class_name_contains(name, "deprecated") ||
	    omci_class_name_contains(name, "obsolete"))
		flags |= OMCI_CLASS_F_DEPRECATED;
	if (category == OMCI_CLASS_CATEGORY_RESERVED)
		flags |= OMCI_CLASS_F_RESERVED;
	if (category == OMCI_CLASS_CATEGORY_VENDOR)
		flags |= OMCI_CLASS_F_VENDOR_SPECIFIC;
	if (category == OMCI_CLASS_CATEGORY_PERFORMANCE)
		flags |= OMCI_CLASS_F_PERFORMANCE;
	if (omci_class_name_contains(name, "table") || class_id == 84 ||
	    class_id == 145 || class_id == 171)
		flags |= OMCI_CLASS_F_TABLE;

	return flags;
}

const struct omci_me_desc *omci_me_lookup_profile(u8 profile, u16 class_id)
{
	unsigned int i;

	if (profile == OMCI_OLT_PROFILE_UNSPEC ||
	    profile == OMCI_OLT_PROFILE_AUTO)
		profile = OMCI_OLT_PROFILE_GENERIC;

	for (i = 0; i < ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];

		if (desc->class_id == class_id &&
		    (desc->profile_mask & OMCI_PROFILE_BIT(profile)))
			return desc;
	}

	return NULL;
}

const struct omci_me_desc *omci_me_lookup(const struct omci_agent *agent,
					  u16 class_id)
{
	return omci_me_lookup_profile(agent->profile_effective, class_id);
}

bool omci_me_active(const struct omci_agent *agent, u16 class_id)
{
	const struct omci_me_desc *desc;

	desc = omci_me_lookup(agent, class_id);
	if (desc)
		return true;

	/* Standard catalogue entries remain available as shadow objects. */
	return class_id < 240 || (class_id >= 256 && class_id < 350) ||
	       (class_id >= 400 && class_id <= 452);
}

bool omci_me_action_allowed(const struct omci_agent *agent, u16 class_id,
			    u8 message_type)
{
	const struct omci_me_desc *desc = omci_me_lookup(agent, class_id);

	if (!desc)
		return omci_me_active(agent, class_id);
	if (message_type >= 32)
		return false;

	return !!(desc->actions & BIT(message_type));
}

static const struct omci_attr_desc *
omci_me_find_attr(const struct omci_me_desc *desc, u16 mask)
{
	unsigned int i;

	for (i = 0; i < desc->num_attrs; i++)
		if (desc->attrs[i].mask == mask)
			return &desc->attrs[i];

	return NULL;
}

int omci_me_decode_create(const struct omci_me_desc *desc,
			  struct omci_mib_object *object,
			  const u8 *data, size_t len)
{
	unsigned int i;
	u16 mask = 0;

	if (!desc)
		return 0;
	for (i = 0; i < desc->num_attrs; i++) {
		const struct omci_attr_desc *attr = &desc->attrs[i];

		if (!(attr->access & OMCI_ATTR_ACCESS_SET_CREATE))
			continue;
		if (attr->offset + attr->len > sizeof(object->data) ||
		    len < attr->len)
			return -EINVAL;
		memcpy(object->data + attr->offset, data, attr->len);
		data += attr->len;
		len -= attr->len;
		mask |= attr->mask;
	}
	object->attr_mask |= mask;

	return 0;
}

int omci_me_decode_set(const struct omci_me_desc *desc,
			       struct omci_mib_object *object, u16 mask,
			       const u8 *data, size_t len)
{
	unsigned int bit;

	if (!desc)
		return 0;
	if (mask & ~desc->valid_attr_mask)
		return -EINVAL;

	for (bit = 0; bit < 16; bit++) {
		const struct omci_attr_desc *attr;
		u16 attr_mask = BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		attr = omci_me_find_attr(desc, attr_mask);
		if (!attr || !(attr->access & OMCI_ATTR_ACCESS_WRITE))
			return -EACCES;
		if (attr->offset + attr->len > sizeof(object->data) ||
		    len < attr->len)
			return -EINVAL;
		memcpy(object->data + attr->offset, data, attr->len);
		data += attr->len;
		len -= attr->len;
	}
	object->attr_mask |= mask;

	return 0;
}

int omci_me_encode_attributes(const struct omci_me_desc *desc,
			      const struct omci_mib_object *object,
			      u16 mask, u8 *data, size_t len,
			      u16 *encoded_mask, size_t *encoded_len)
{
	size_t used = 0;
	unsigned int bit;
	u16 encoded = 0;

	if (!desc)
		return -ENOENT;
	mask &= desc->valid_attr_mask;
	for (bit = 0; bit < 16; bit++) {
		const struct omci_attr_desc *attr;
		u16 attr_mask = BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		attr = omci_me_find_attr(desc, attr_mask);
		if (!attr || !(attr->access & OMCI_ATTR_ACCESS_READ))
			continue;
		if (attr->offset + attr->len > sizeof(object->data))
			return -EINVAL;
		if (len - used < attr->len)
			break;
		memcpy(data + used, object->data + attr->offset, attr->len);
		used += attr->len;
		encoded |= attr_mask;
	}
	if (encoded_mask)
		*encoded_mask = encoded;
	if (encoded_len)
		*encoded_len = used;

	return encoded ? 0 : -ENOSPC;
}

unsigned int omci_me_attr_count(const struct omci_agent *agent)
{
	unsigned int count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];

		if (omci_me_lookup(agent, desc->class_id) != desc)
			continue;
		count += desc->num_attrs;
	}

	return count;
}

int omci_me_attr_id(const struct omci_agent *agent, u16 class_id,
		    unsigned int attr_index, u16 *attr_id)
{
	unsigned int next_id = 1;
	unsigned int i;

	if (!agent || !attr_id)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];

		if (omci_me_lookup(agent, desc->class_id) != desc)
			continue;
		if (desc->class_id == class_id) {
			if (attr_index >= desc->num_attrs)
				return -ENOENT;
			if (next_id + attr_index > U16_MAX)
				return -EOVERFLOW;
			*attr_id = next_id + attr_index;
			return 0;
		}
		if (next_id + desc->num_attrs > U16_MAX)
			return -EOVERFLOW;
		next_id += desc->num_attrs;
	}

	return -ENOENT;
}

int omci_me_attr_get(const struct omci_agent *agent, u16 attr_id,
		     u16 *class_id, unsigned int *attr_index,
		     const struct omci_me_desc **me_desc,
		     const struct omci_attr_desc **me_attr)
{
	unsigned int next_id = 1;
	unsigned int i;

	if (!agent || !attr_id)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];
		unsigned int index;

		if (omci_me_lookup(agent, desc->class_id) != desc)
			continue;
		if (attr_id < next_id || attr_id >= next_id + desc->num_attrs) {
			next_id += desc->num_attrs;
			continue;
		}

		index = attr_id - next_id;
		if (class_id)
			*class_id = desc->class_id;
		if (attr_index)
			*attr_index = index;
		if (me_desc)
			*me_desc = desc;
		if (me_attr)
			*me_attr = &desc->attrs[index];
		return 0;
	}

	return -ENOENT;
}

const char *omci_me_class_name(u16 class_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_me_descs); i++)
		if (omci_me_descs[i].class_id == class_id)
			return omci_me_descs[i].name;
	for (i = 0; i < ARRAY_SIZE(omci_classes); i++)
		if (omci_classes[i].class_id == class_id)
			return omci_classes[i].name;

	if (class_id >= 172 && class_id <= 239)
		return "Reserved for future managed entities";
	if (class_id >= 240 && class_id <= 255)
		return "Reserved for vendor-specific managed entities";
	if (class_id >= 350 && class_id <= 399)
		return "Vendor-specific managed entity";
	if (class_id >= 453 && class_id <= 65279)
		return "Reserved for future standardization";
	if (class_id >= 65280)
		return "Vendor-specific managed entity";

	return "Unassigned managed entity";
}

int omci_me_class_get(struct omci_device *odev, u16 class_id,
		      struct omci_me_class *class)
{
	const struct omci_me_desc *desc;
	const char *name;

	if (!odev || !class)
		return -EINVAL;
	if (!omci_me_active(&odev->agent, class_id))
		return -ENOENT;

	desc = omci_me_lookup(&odev->agent, class_id);
	name = omci_me_class_name(class_id);
	class->class_id = class_id;
	class->category = desc ? desc->category :
			  omci_class_category(class_id, name);
	class->support = desc ? desc->support : OMCI_CLASS_SUPPORT_SHADOW;
	class->flags = omci_class_flags(class_id, name);
	if (desc) {
		if (desc->flags & OMCI_ME_F_VENDOR)
			class->flags |= OMCI_CLASS_F_VENDOR_SPECIFIC;
		if (desc->flags & OMCI_ME_F_DATAPATH)
			class->flags |= OMCI_CLASS_F_DATAPATH;
	}
	class->name = name;

	return 0;
}

int omci_me_class_next(struct omci_device *odev, u32 index,
		       struct omci_me_class *class, u32 *next_index)
{
	u32 position = 0;
	unsigned int i;

	if (!odev || !class || !next_index)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(omci_classes); i++) {
		if (!omci_me_active(&odev->agent, omci_classes[i].class_id))
			continue;
		if (position++ < index)
			continue;
		omci_me_class_get(odev, omci_classes[i].class_id, class);
		*next_index = index + 1;
		return 0;
	}
	for (i = 0; i < ARRAY_SIZE(omci_me_descs); i++) {
		u16 class_id = omci_me_descs[i].class_id;
		unsigned int j;
		bool catalogued = false;

		for (j = 0; j < ARRAY_SIZE(omci_classes); j++)
			if (omci_classes[j].class_id == class_id) {
				catalogued = true;
				break;
			}
		if (catalogued || !omci_me_lookup(&odev->agent, class_id))
			continue;
		if (position++ < index)
			continue;
		omci_me_class_get(odev, class_id, class);
		*next_index = index + 1;
		return 0;
	}

	return -ENOENT;
}
