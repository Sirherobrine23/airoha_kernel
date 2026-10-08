/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#ifndef _NET_XPON_PLOAM_H
#define _NET_XPON_PLOAM_H

#include <linux/bits.h>
#include <linux/types.h>

/**
 * DOC: PLOAM protocol vocabulary
 *
 * GPON (G.984.3) and XGS-PON (G.9807.1) have different message bodies,
 * identifiers and activation rules. Keep their constants in separate
 * namespaces. Register layouts and recovery policy belong to the driver.
 * These definitions are kernel internal and do not change the xPON UAPI.
 */

/* GPON vocabulary, preserving the existing G.984.3 provider's values. */
#define GPON_PLOAM_CONTENT_LEN		10
#define GPON_PLOAM_ONU_BCAST		0xff
#define GPON_PLOAM_ONU_UNASSIGNED	0xff

/* Downstream PLOAM message type IDs (from ITU-T G.984.3 / vendor ref) */
#define GPON_PLOAM_DOWN_UPSTREAM_OVERHEAD	0x01
#define GPON_PLOAM_DOWN_ASSIGN_ONU_ID		0x03
#define GPON_PLOAM_DOWN_RANGING_TIME		0x04
#define GPON_PLOAM_DOWN_DEACTIVATE_ONU_ID	0x05
#define GPON_PLOAM_DOWN_DISABLE_SN		0x06
#define GPON_PLOAM_DOWN_ENCRYPTED_PORT_ID	0x08
#define GPON_PLOAM_DOWN_REQUEST_PASSWORD	0x09
#define GPON_PLOAM_DOWN_ASSIGN_ALLOC_ID		0x0A
#define GPON_PLOAM_DOWN_POPUP			0x0C
#define GPON_PLOAM_DOWN_REQUEST_KEY		0x0D
#define GPON_PLOAM_DOWN_CONFIGURE_PORT_ID	0x0E
#define GPON_PLOAM_DOWN_PEE			0x0F
#define GPON_PLOAM_DOWN_CHANGE_POWER_LEVEL	0x10
#define GPON_PLOAM_DOWN_PST			0x11
#define GPON_PLOAM_DOWN_BER_INTERVAL		0x12
#define GPON_PLOAM_DOWN_KEY_SWITCHING_TIME	0x13
#define GPON_PLOAM_DOWN_EXTENDED_BURST_LEN	0x14
#define GPON_PLOAM_DOWN_PON_ID			0x15
#define GPON_PLOAM_DOWN_SWIFT_POPUP		0x16
#define GPON_PLOAM_DOWN_RANGING_ADJUSTMENT	0x17
#define GPON_PLOAM_DOWN_SLEEP_ALLOW		0x18
#define GPON_PLOAM_DOWN_MAX_TYPE		0x19

/* Disable_SN mode byte values */
#define GPON_PLOAM_DISABLE_DENIED		0xFF	/* unicast SN → go to O7  */
#define GPON_PLOAM_DISABLE_DENIED_ALL		0xF0	/* all ONUs → O7          */
#define GPON_PLOAM_DISABLE_PARTICIPATE		0x00	/* unicast SN → O2        */
#define GPON_PLOAM_DISABLE_PARTICIPATE_ALL	0x0F	/* all O7 ONUs → O2       */

/* Upstream PLOAM message type IDs */
#define GPON_PLOAM_UP_SERIAL_NUMBER_ONU		0x01
#define GPON_PLOAM_UP_PASSWORD			0x02
#define GPON_PLOAM_UP_DYING_GASP		0x03
#define GPON_PLOAM_UP_NO_MESSAGE		0x04
#define GPON_PLOAM_UP_ENCRYPT_KEY		0x05
#define GPON_PLOAM_UP_PEE			0x06
#define GPON_PLOAM_UP_PST			0x07
#define GPON_PLOAM_UP_REI			0x08
#define GPON_PLOAM_UP_ACK			0x09
#define GPON_PLOAM_UP_SLEEP_REQUEST		0x0A

/**
 * enum gpon_state - ONU activation states per ITU-T G.984.3
 * @GPON_O1_INITIAL: initial state
 * @GPON_O2_STANDBY: standby, waiting for upstream overhead
 * @GPON_O3_SERIAL_NUMBER: serial-number exchange
 * @GPON_O4_RANGING: ranging and equalization delay assignment
 * @GPON_O5_OPERATION: operational state
 * @GPON_O6_POPUP: recovery after loss of signal
 * @GPON_O7_EMERGENCY_STOP: upstream access disabled by the OLT
 */
enum gpon_state {
	GPON_O1_INITIAL		= 1,
	GPON_O2_STANDBY,
	GPON_O3_SERIAL_NUMBER,
	GPON_O4_RANGING,
	GPON_O5_OPERATION,
	GPON_O6_POPUP,
	GPON_O7_EMERGENCY_STOP,
};

/**
 * enum xgspon_ploam_down_id - downstream PLOAM message identifiers
 * @XGSPON_PLOAM_DOWN_BURST_PROFILE: Burst_Profile
 * @XGSPON_PLOAM_DOWN_ASSIGN_ONU_ID: Assign_ONU-ID
 * @XGSPON_PLOAM_DOWN_RANGING_TIME: Ranging_Time
 * @XGSPON_PLOAM_DOWN_DEACTIVATE: Deactivate_ONU-ID
 * @XGSPON_PLOAM_DOWN_DISABLE_SN: Disable_Serial_Number
 * @XGSPON_PLOAM_DOWN_REQUEST_REG: Request_Registration
 * @XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID: Assign_Alloc-ID
 * @XGSPON_PLOAM_DOWN_KEY_CONTROL: Key_Control
 * @XGSPON_PLOAM_DOWN_SLEEP_ALLOW: Sleep_Allow
 * @XGSPON_PLOAM_DOWN_REBOOT_ONU: Reboot_ONU
 * @XGSPON_PLOAM_DOWN_MAX: one above the highest identifier listed here, the
 *	size of an array indexed by the identifier, not a wire value
 *
 * ITU-T G.9807.1 Table C.11.2.
 */
enum xgspon_ploam_down_id {
	XGSPON_PLOAM_DOWN_BURST_PROFILE		= 0x01,
	XGSPON_PLOAM_DOWN_ASSIGN_ONU_ID		= 0x03,
	XGSPON_PLOAM_DOWN_RANGING_TIME		= 0x04,
	XGSPON_PLOAM_DOWN_DEACTIVATE		= 0x05,
	XGSPON_PLOAM_DOWN_DISABLE_SN		= 0x06,
	XGSPON_PLOAM_DOWN_REQUEST_REG		= 0x09,
	XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID	= 0x0a,
	XGSPON_PLOAM_DOWN_KEY_CONTROL		= 0x0d,
	XGSPON_PLOAM_DOWN_SLEEP_ALLOW		= 0x12,
	
	

	XGSPON_PLOAM_DOWN_REBOOT_ONU		= 0x1d,
	XGSPON_PLOAM_DOWN_MAX			= 0x1e,
};

/**
 * enum xgspon_ploam_up_id - upstream PLOAM message identifiers
 * @XGSPON_PLOAM_UP_SERIAL_NUMBER: Serial_Number_ONU
 * @XGSPON_PLOAM_UP_REGISTRATION: Registration
 * @XGSPON_PLOAM_UP_KEY_REPORT: Key_Report
 * @XGSPON_PLOAM_UP_ACKNOWLEDGE: Acknowledgment
 * @XGSPON_PLOAM_UP_SLEEP_REQUEST: Sleep_Request
 * @XGSPON_PLOAM_UP_MAX: one above the highest identifier listed here, the size
 *	of an array indexed by the identifier, not a wire value
 *
 * ITU-T G.9807.1 Table C.11.3.
 */
enum xgspon_ploam_up_id {
	XGSPON_PLOAM_UP_SERIAL_NUMBER	= 0x01,
	XGSPON_PLOAM_UP_REGISTRATION	= 0x02,
	XGSPON_PLOAM_UP_KEY_REPORT	= 0x05,
	XGSPON_PLOAM_UP_ACKNOWLEDGE	= 0x09,
	XGSPON_PLOAM_UP_SLEEP_REQUEST	= 0x10,
	XGSPON_PLOAM_UP_MAX		= 0x11,
};

/**
 * enum xgspon_ploam_ack - completion codes of the upstream Acknowledgment message
 * @XGSPON_PLOAM_ACK_OK: OK
 * @XGSPON_PLOAM_ACK_NO_MESSAGE: no message to send
 * @XGSPON_PLOAM_ACK_BUSY: busy, preparing a response
 * @XGSPON_PLOAM_ACK_UNKNOWN_TYPE: unknown message type
 * @XGSPON_PLOAM_ACK_PARAM_ERR: parameter error
 * @XGSPON_PLOAM_ACK_PROCESS_ERR: processing error
 *
 * ITU-T G.9807.1 Table C.11.27, the Completion_code field.
 */
enum xgspon_ploam_ack {
	XGSPON_PLOAM_ACK_OK		= 0,
	XGSPON_PLOAM_ACK_NO_MESSAGE	= 1,
	XGSPON_PLOAM_ACK_BUSY		= 2,
	XGSPON_PLOAM_ACK_UNKNOWN_TYPE	= 3,
	XGSPON_PLOAM_ACK_PARAM_ERR	= 4,
	XGSPON_PLOAM_ACK_PROCESS_ERR	= 5,
};

/**
 * enum xgspon_ploam_disable_mode - the Mode field of Disable_Serial_Number
 * @XGSPON_PLOAM_DISABLE_ALLOW_ONE: the ONU with this serial number is allowed
 *	upstream access
 * @XGSPON_PLOAM_DISABLE_DENY_ALL: all ONUs are denied upstream access. The
 *	serial number is ignored
 * @XGSPON_PLOAM_DISABLE_ALLOW_ALL: all ONUs are allowed upstream access
 * @XGSPON_PLOAM_DISABLE_DENY_ONE: the ONU with this serial number is denied
 *	upstream access
 *
 * ITU-T G.9807.1 Table C.11.9, the Disable/enable field.
 */
enum xgspon_ploam_disable_mode {
	XGSPON_PLOAM_DISABLE_ALLOW_ONE	= 0x00,
	XGSPON_PLOAM_DISABLE_DENY_ALL	= 0x0f,
	XGSPON_PLOAM_DISABLE_ALLOW_ALL	= 0xf0,
	XGSPON_PLOAM_DISABLE_DENY_ONE	= 0xff,
};

/* The equalization delay encoding of Ranging_Time, G.9807.1 Table C.11.7. */
#define XGSPON_PLOAM_EQD_RELATIVE		0
#define XGSPON_PLOAM_EQD_ABSOLUTE		1
#define XGSPON_PLOAM_EQD_POSITIVE		0

/* The Alloc-ID type field of Assign_Alloc-ID, G.9807.1 Table C.11.11. */
#define XGSPON_PLOAM_ALLOC_ASSIGN		0x01
#define XGSPON_PLOAM_ALLOC_DEALLOCATE	0xff

/* Alloc-ID values, G.9807.1 Table C.6.5. The default alloc-id equals the
 * ONU-ID, the three above the default range are serial number grants that
 * are never assigned to an ONU and the rest are assignable. 1022 is the
 * serial number grant for the 9.95328 Gbit/s upstream rate, 1023 the one
 * for 2.48832 Gbit/s.
 */
#define XGSPON_PLOAM_ALLOC_ID_DEFAULT_MAX	1020
#define XGSPON_PLOAM_ALLOC_ID_SN_GRANT_MIN	1021
#define XGSPON_PLOAM_ALLOC_ID_SN_GRANT_10G	1022
#define XGSPON_PLOAM_ALLOC_ID_SN_GRANT_2G5	1023
#define XGSPON_PLOAM_ALLOC_ID_MAX		16383

/* XGEM Port-ID values, G.9807.1 Table C.6.6. 0 to 1020 is the default
 * Port-ID, which equals the ONU-ID and carries only the OMCC. The OLT
 * assigns every other GEM port of the ONU over the OMCC from 1021 to 65534
 * and 65535 is the idle Port-ID.
 */
#define XGSPON_GEM_PORT_ID_ASSIGNABLE_MIN	1021
#define XGSPON_GEM_PORT_ID_ASSIGNABLE_MAX	65534

/* The Reboot_ONU fields, G.9807.1 Table C.11.23A. */
#define XGSPON_PLOAM_REBOOT_DEPTH_MAX		3
#define XGSPON_PLOAM_REBOOT_IMAGE_MAX		1
#define XGSPON_PLOAM_REBOOT_STATE_INACTIVE_ONLY	1
#define XGSPON_PLOAM_REBOOT_CALLS_MASK		0x3

/* The Key_Length a Key_Control carries for the AES-128 cipher,
 * G.9807.1 Table C.11.12.
 */
#define XGSPON_PLOAM_KEY_LEN_AES128	16

/* The Key index of Key_Control and Key_Report, the two low bits of the
 * field. Values 00 and 11 are not defined. G.9807.1 Table C.11.12 and
 * Table C.11.26.
 */
#define XGSPON_PLOAM_KEY_INDEX_FIRST	1
#define XGSPON_PLOAM_KEY_INDEX_SECOND	2

/* The Control flag of Key_Control (G.9807.1 Table C.11.12) and the Report
 * type of Key_Report (Table C.11.26).
 */
#define XGSPON_PLOAM_KEY_CONTROL_GENERATE	0
#define XGSPON_PLOAM_KEY_CONTROL_CONFIRM	1
#define XGSPON_PLOAM_KEY_REPORT_TYPE_NEW	0
#define XGSPON_PLOAM_KEY_REPORT_TYPE_EXISTING	1

/* The Key_Name of a Key_Report on an existing key is AES-CMAC(KEK,
 * encryption_key | 0x33313431353932363533353839373933, 128), G.9807.1
 * Table C.11.26. The constant is the ASCII string below, without its NUL.
 * The key fragment number of a single fragment report is 0.
 */
#define XGSPON_PLOAM_KEY_NAME_CONSTANT		"3141592653589793"
#define XGSPON_PLOAM_KEY_NAME_CONSTANT_LEN	16
#define XGSPON_PLOAM_KEY_FRAGMENT_FIRST		0

/* The default PLOAM_IK is this byte repeated sixteen times, G.9807.1
 * clause C.15.3.3. The MSK of the well-known default Registration_ID
 * (thirty-six zero bytes, Table C.11.25) is formula C.15-2 under that key,
 * clause C.15.3.2. XGSPON_PLOAM_DEFAULT_MSK holds it as an initializer. A MAC
 * that derives an MSK only from the Registration_ID programmed into it
 * needs the value as a constant.
 */
#define XGSPON_PLOAM_DEFAULT_PLOAM_IK_BYTE		0x55
#define XGSPON_PLOAM_DEFAULT_MSK			{\
	0x24, 0x37, 0xbe, 0x54, 0xe9, 0x5e, 0x6e, 0xe3,	\
	0x53, 0x8b, 0xb1, 0xb4, 0xb5, 0xd4, 0x32, 0xeb,	\
}

/* The delimiter and preamble lengths of the burst profile, in octets,
 * G.9807.1 Table C.11.4.
 */
#define XGSPON_PLOAM_BURST_DELIMITER_LEN_MAX	8
#define XGSPON_PLOAM_BURST_PREAMBLE_LEN_MIN	1
#define XGSPON_PLOAM_BURST_PREAMBLE_LEN_MAX	8

/* The upstream line rate bit: R of the burst profile (G.9807.1
 * Table C.11.4) and U of Assign_ONU-ID (Table C.11.6). Then the widths of the
 * preamble repeat count of the burst profile, Table C.11.4.
 */
#define XGSPON_PLOAM_LINE_RATE_XGPON		0
#define XGSPON_PLOAM_LINE_RATE_XGSPON		1
#define XGSPON_PLOAM_PREAMBLE_MASK_XGPON	0x1f
#define XGSPON_PLOAM_PREAMBLE_MASK_XGSPON	0xff

/* The upstream line rate capability of Serial_Number_ONU, a bitmap of the
 * form 0000 00HL, G.9807.1 Table C.11.24. H set: the ONU supports the
 * 9.95328 Gbit/s upstream rate. L set: the ONU does not support the
 * 2.48832 Gbit/s upstream rate.
 */
#define XGSPON_PLOAM_SN_RATE_10G	BIT(1)
#define XGSPON_PLOAM_SN_RATE_NO_2G5	BIT(0)

/* The ONU-ID is ten bits, G.9807.1 clause C.11.2.1. The OLT assigns 0 to
 * 1020, Table C.6.4. 0x3ff is both the broadcast destination and the value
 * an ONU carries before the OLT assigns it one. 0x3fe appears only in a
 * Burst_Profile, as the broadcast destination of a profile for the
 * 9.95328 Gbit/s upstream rate.
 */
#define XGSPON_PLOAM_ONU_ID_MASK		GENMASK(9, 0)
#define XGSPON_PLOAM_ONU_ID_MAX			1020
#define XGSPON_PLOAM_ONU_ID_BROADCAST		0x3ff
#define XGSPON_PLOAM_ONU_ID_UNASSIGNED		0x3ff
#define XGSPON_PLOAM_ONU_ID_PROFILE_BCAST_10G	0x3fe

/* Fields of the standard message, in bytes. The serial number is the
 * Vendor_ID and the VSSN, G.9807.1 clauses C.11.2.6.1 and C.11.2.6.2. The
 * Registration_ID is Table C.11.25 and the key fragment Table C.11.26.
 */
#define XGSPON_PLOAM_SN_LEN		8
#define XGSPON_PLOAM_REG_ID_LEN		36
#define XGSPON_PLOAM_KEY_FRAGMENT_LEN	32

/* The recommended initial value of TO1, in milliseconds, Table C.12.2. */
#define XGSPON_PLOAM_TO1_MS		10000

/* The key exchange timers, in milliseconds, G.9807.1 clause C.15.5.3.3. */
#define XGSPON_PLOAM_TK4_MS		100
#define XGSPON_PLOAM_TK5_MS		20

/**
 * xgspon_ploam_alloc_is_assignable() - whether the OLT can assign an Alloc-ID
 * @alloc_id: the fourteen bit Alloc-ID
 *
 * ITU-T G.9807.1 Table C.6.5. An XGS-PON MAC driver
 * needs it to refuse an Assign_Alloc-ID outside that range.
 *
 * Return: true for the values above XGSPON_PLOAM_ALLOC_ID_SN_GRANT_2G5 up to
 * XGSPON_PLOAM_ALLOC_ID_MAX, 1024 to 16383.
 */
static inline bool xgspon_ploam_alloc_is_assignable(u16 alloc_id)
{
	return alloc_id > XGSPON_PLOAM_ALLOC_ID_SN_GRANT_2G5 &&
	       alloc_id <= XGSPON_PLOAM_ALLOC_ID_MAX;
}

#endif /* _NET_XPON_PLOAM_H */
