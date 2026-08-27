/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_WIRE_H
#define _NET_OMCI_WIRE_H

#include <linux/types.h>

struct omci_wire_request {
	u16 transaction_id;
	u16 class_id;
	u16 entity_id;
	u16 payload_len;
	u8 message_type;
	u8 device_id;
	const u8 *payload;
};

int omci_wire_decode(const void *data, size_t len,
		     struct omci_wire_request *request);
int omci_wire_encode_response(const struct omci_wire_request *request,
			      u8 action, const void *payload,
			      size_t payload_len, void *data,
			      size_t capacity, size_t *encoded_len);

#endif /* _NET_OMCI_WIRE_H */
