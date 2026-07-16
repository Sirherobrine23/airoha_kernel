/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_OMCI_H
#define _UAPI_LINUX_OMCI_H

#define OMCI_GENL_NAME		"omci"
#define OMCI_GENL_VERSION	1
#define OMCI_MAX_PDU_LEN	1980

/**
 * enum omci_cmd - Generic Netlink commands
 * @OMCI_CMD_UNSPEC: unspecified command
 * @OMCI_CMD_GET: return channel state and counters
 * @OMCI_CMD_BIND: claim the userspace OMCI endpoint
 * @OMCI_CMD_UNBIND: release the userspace OMCI endpoint
 * @OMCI_CMD_TX: transmit an OMCI PDU through the OMCC
 * @OMCI_CMD_RX: kernel-to-userspace received OMCI PDU
 * @OMCI_CMD_EVENT: kernel-to-userspace channel event
 */
enum omci_cmd {
	OMCI_CMD_UNSPEC,
	OMCI_CMD_GET,
	OMCI_CMD_BIND,
	OMCI_CMD_UNBIND,
	OMCI_CMD_TX,
	OMCI_CMD_RX,
	OMCI_CMD_EVENT,

	__OMCI_CMD_MAX,
};

#define OMCI_CMD_MAX (__OMCI_CMD_MAX - 1)

/**
 * enum omci_attr - Generic Netlink attributes
 */
enum omci_attr {
	OMCI_ATTR_UNSPEC,
	OMCI_ATTR_DEV_ID,
	OMCI_ATTR_IFINDEX,
	OMCI_ATTR_ONU_ID,
	OMCI_ATTR_GEM_PORT_ID,
	OMCI_ATTR_STATE,
	OMCI_ATTR_FLAGS,
	OMCI_ATTR_CAPABILITIES,
	OMCI_ATTR_SEQUENCE,
	OMCI_ATTR_PDU,
	OMCI_ATTR_EVENT,
	OMCI_ATTR_OWNER_PORTID,
	OMCI_ATTR_RX_PACKETS,
	OMCI_ATTR_RX_BYTES,
	OMCI_ATTR_RX_DROPPED,
	OMCI_ATTR_TX_PACKETS,
	OMCI_ATTR_TX_BYTES,
	OMCI_ATTR_TX_ERRORS,
	OMCI_ATTR_PAD,

	__OMCI_ATTR_MAX,
};

#define OMCI_ATTR_MAX (__OMCI_ATTR_MAX - 1)

enum omci_event {
	OMCI_EVENT_UNSPEC,
	OMCI_EVENT_CHANNEL_UP,
	OMCI_EVENT_CHANNEL_DOWN,
	OMCI_EVENT_STATE_CHANGE,
};

#define OMCI_F_MIC_PRESENT	(1U << 0)
#define OMCI_F_MIC_VALID	(1U << 1)
#define OMCI_F_CRC_ERROR	(1U << 2)
#define OMCI_F_CHANNEL_UP	(1U << 3)

#define OMCI_CAP_HW_MIC		(1U << 0)

#endif /* _UAPI_LINUX_OMCI_H */
