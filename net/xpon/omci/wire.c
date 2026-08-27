// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "internal.h"
#include "wire.h"

int omci_wire_decode(const void *data, size_t len,
		     struct omci_wire_request *request)
{
	const u8 *pdu = data;
	u16 payload_len;
	size_t pdu_len;

	if (!data || !request || len < 8)
		return -EINVAL;

	memset(request, 0, sizeof(*request));
	request->transaction_id = get_unaligned_be16(pdu);
	request->message_type = pdu[2];
	request->device_id = pdu[3];
	request->class_id = get_unaligned_be16(pdu + 4);
	request->entity_id = get_unaligned_be16(pdu + 6);

	switch (request->device_id) {
	case OMCI_BASELINE_DEV_ID:
		if (len != OMCI_BASELINE_LEN_NO_MIC && len != OMCI_BASELINE_LEN)
			return -EMSGSIZE;
		request->payload = pdu + 8;
		request->payload_len = 32;
		return 0;
	case OMCI_EXTENDED_DEV_ID:
		if (len < OMCI_EXTENDED_HEADER_LEN)
			return -EMSGSIZE;
		payload_len = get_unaligned_be16(pdu + 8);
		pdu_len = OMCI_EXTENDED_HEADER_LEN + payload_len;
		if (pdu_len > OMCI_MAX_PDU_LEN || len < pdu_len)
			return -EMSGSIZE;
		if (len != pdu_len && len != pdu_len + OMCI_MIC_LEN)
			return -EMSGSIZE;
		request->payload = pdu + OMCI_EXTENDED_HEADER_LEN;
		request->payload_len = payload_len;
		return 0;
	default:
		return -EPROTO;
	}
}

int omci_wire_encode_response(const struct omci_wire_request *request,
			      u8 action, const void *payload,
			      size_t payload_len, void *data,
			      size_t capacity, size_t *encoded_len)
{
	u8 *pdu = data;
	size_t len;

	if (!request || !data || !encoded_len || (!payload && payload_len))
		return -EINVAL;

	switch (request->device_id) {
	case OMCI_BASELINE_DEV_ID:
		if (payload_len > 32 || capacity < OMCI_BASELINE_LEN_NO_MIC)
			return -EMSGSIZE;
		len = OMCI_BASELINE_LEN_NO_MIC;
		memset(pdu, 0, len);
		put_unaligned_be16(request->transaction_id, pdu);
		pdu[2] = (request->message_type & 0x80) | 0x20 | action;
		pdu[3] = OMCI_BASELINE_DEV_ID;
		put_unaligned_be16(request->class_id, pdu + 4);
		put_unaligned_be16(request->entity_id, pdu + 6);
		if (payload_len)
			memcpy(pdu + 8, payload, payload_len);
		put_unaligned_be32(40, pdu + 40);
		break;
	case OMCI_EXTENDED_DEV_ID:
		if (payload_len > U16_MAX)
			return -EMSGSIZE;
		len = OMCI_EXTENDED_HEADER_LEN + payload_len;
		if (len > OMCI_MAX_PDU_LEN || capacity < len)
			return -EMSGSIZE;
		put_unaligned_be16(request->transaction_id, pdu);
		pdu[2] = (request->message_type & 0x80) | 0x20 | action;
		pdu[3] = OMCI_EXTENDED_DEV_ID;
		put_unaligned_be16(request->class_id, pdu + 4);
		put_unaligned_be16(request->entity_id, pdu + 6);
		put_unaligned_be16(payload_len, pdu + 8);
		if (payload_len)
			memcpy(pdu + OMCI_EXTENDED_HEADER_LEN, payload,
			       payload_len);
		break;
	default:
		return -EPROTO;
	}

	*encoded_len = len;
	return 0;
}
