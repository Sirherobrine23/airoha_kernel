// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental AB1536U UART transport for the Quantum C6500XK.
 * The stock Gemtek application uses RACE; external HCI availability
 * depends on the firmware running on the Bluetooth module.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"

#define AB1536U_BAUDRATE		115200
#define AB1536U_RACE_PKT		0x05
#define AB1536U_RACE_ALT_PKT	0x15
#define AB1536U_RACE_RESPONSE	0x5b
#define AB1536U_RACE_NOTIFY	0x5d
#define AB1536U_RACE_VERSION	0x1c09
#define AB1536U_RACE_NVKEY_READ	0x0a00
#define AB1536U_RACE_MAX_LEN	0x1010
#define AB1536U_RACE_TIMEOUT	msecs_to_jiffies(2500)
#define AB1536U_DIAG_RX_CHUNKS	8
#define AB1536U_DIAG_TX_PACKETS	4
#define AB1536U_DIAG_DUMP_BYTES	32

static bool ab1536u_vendor_high=1;
module_param_named(vendor_high, ab1536u_vendor_high, bool, 0400);
MODULE_PARM_DESC(vendor_high,
		"Test GPIO26/28 held high instead of the low-ending recovery sequence");

struct ab1536u {
	/* The HCI serdev helpers retrieve this directly from drvdata. */
	struct hci_uart hu;
	struct gpio_descs *controls;
	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
	struct completion version_done;
	int version_status;
	struct completion nvkey_done;
	int nvkey_status;
	bool nvkey_pending;
	bool seen_race;
	atomic_t rx_bytes;
	atomic_t rx_errors;
	atomic_t tx_dequeued;
	u8 rx_log_count;
	u8 tx_log_count;
};

static int ab1536u_open(struct hci_uart *hu)
{
	struct device *dev = &hu->serdev->dev;
	unsigned int speed;
	int err;

	speed = serdev_device_set_baudrate(hu->serdev, AB1536U_BAUDRATE);
	if (speed != AB1536U_BAUDRATE)
		return dev_err_probe(dev, -EINVAL,
				     "Cannot configure UART at 115200 baud (got %u)\n",
				     speed);

	serdev_device_set_flow_control(hu->serdev, false);
	err = serdev_device_set_parity(hu->serdev, SERDEV_PARITY_NONE);
	if (err)
		return dev_err_probe(dev, err, "Cannot configure UART parity\n");

	dev_info(dev, "AB1536U test transport: 115200 baud, 8N1, no RTS/CTS\n");
	return 0;
}

static int ab1536u_flush(struct hci_uart *hu)
{
	struct ab1536u *ab = hu->priv;

	skb_queue_purge(&ab->txq);
	return 0;
}

static int ab1536u_close(struct hci_uart *hu)
{
	struct ab1536u *ab = hu->priv;

	skb_queue_purge(&ab->txq);
	kfree_skb(ab->rx_skb);
	ab->rx_skb = NULL;
	return 0;
}

static int ab1536u_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct ab1536u *ab = hu->priv;

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&ab->txq, skb);
	return 0;
}

static struct sk_buff *ab1536u_dequeue(struct hci_uart *hu)
{
	struct ab1536u *ab = hu->priv;
	struct sk_buff *skb;
	unsigned int len;

	skb = skb_dequeue(&ab->txq);
	if (!skb)
		return NULL;

	atomic_add(skb->len, &ab->tx_dequeued);
	if (ab->tx_log_count < AB1536U_DIAG_TX_PACKETS) {
		ab->tx_log_count++;
		len = min_t(unsigned int, skb->len, AB1536U_DIAG_DUMP_BYTES);
		dev_info(&hu->serdev->dev, "UART TX dequeued %u bytes, first %u: %*ph\n",
			 skb->len, len, len, skb->data);
	}

	return skb;
}

static int ab1536u_recv_race(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_uart *hu = hci_get_drvdata(hdev);
	struct ab1536u *ab = hu->priv;
	const u8 *payload;
	size_t len;
	u16 id;
	u8 type;
	int err;

	/* h4_recv_buf() removes the packet marker, but keeps type/length/ID. */
	if (skb->len < 5 || skb->data[0] < 0x5a || skb->data[0] > 0x5d)
		goto malformed;

	len = get_unaligned_le16(skb->data + 1);
	if (len < 2 || len > AB1536U_RACE_MAX_LEN || skb->len != len + 3)
		goto malformed;

	WRITE_ONCE(ab->seen_race, true);
	type = skb->data[0];
	id = get_unaligned_le16(skb->data + 3);
	payload = skb->data + 5;
	len -= 2;
	bt_dev_dbg(hdev, "RACE type 0x%02x ID 0x%04x payload %zu bytes",
		   type, id, len);

	if (type == AB1536U_RACE_RESPONSE && id == AB1536U_RACE_NVKEY_READ &&
	    READ_ONCE(ab->nvkey_pending)) {
		unsigned int dump_len = min_t(size_t, len, AB1536U_DIAG_DUMP_BYTES);

		/* The SDK describes raw payload; Gemtek status/layout is unconfirmed. */
		bt_dev_info(hdev,
			    "RACE NVKEY 0x3604 reply: type 0x%02x ID 0x%04x payload %zu bytes, first %u: %*ph",
			    type, id, len, dump_len, dump_len, payload);
		WRITE_ONCE(ab->nvkey_status, len ? 0 : -EBADMSG);
		WRITE_ONCE(ab->nvkey_pending, false);
		complete(&ab->nvkey_done);
		goto done;
	}

	if (type != AB1536U_RACE_NOTIFY || id != AB1536U_RACE_VERSION)
		goto done;

	/* Version layout recovered from gtk_ble_daemon 0x00402ac0. */
	if (!len) {
		err = -EBADMSG;
	} else if (payload[0]) {
		bt_dev_warn(hdev, "RACE version status 0x%02x", payload[0]);
		err = -EREMOTEIO;
	} else if (len < 10) {
		err = -EBADMSG;
	} else {
		bt_dev_info(hdev, "AB1536U RACE firmware %u.%u.%u.%u",
			    payload[4], payload[5],
			    get_unaligned_le16(payload + 6),
			    get_unaligned_le16(payload + 8));
		err = 0;
	}

	WRITE_ONCE(ab->version_status, err);
	complete(&ab->version_done);
done:
	kfree_skb(skb);
	return 0;

malformed:
	bt_dev_warn_ratelimited(hdev, "Malformed RACE frame (%u bytes)", skb->len);
	kfree_skb(skb);
	return -EBADMSG;
}

/* RACE occupies 0x05 on this pre-ISO firmware, so do not use H4_RECV_ISO. */
static const struct h4_recv_pkt ab1536u_recv_pkts[] = {
	{ H4_RECV_ACL, .recv = hci_recv_frame },
	{ H4_RECV_SCO, .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
	{
		.type = AB1536U_RACE_PKT,
		.hlen = 3,
		.loff = 1,
		.lsize = 2,
		.maxlen = AB1536U_RACE_MAX_LEN + 3,
		.recv = ab1536u_recv_race,
	},
	{
		.type = AB1536U_RACE_ALT_PKT,
		.hlen = 3,
		.loff = 1,
		.lsize = 2,
		.maxlen = AB1536U_RACE_MAX_LEN + 3,
		.recv = ab1536u_recv_race,
	},
};

static int ab1536u_recv(struct hci_uart *hu, const void *data, int count)
{
	struct ab1536u *ab = hu->priv;
	unsigned int len;
	int err;

	if (count <= 0)
		return count;

	atomic_add(count, &ab->rx_bytes);
	if (ab->rx_log_count < AB1536U_DIAG_RX_CHUNKS) {
		ab->rx_log_count++;
		len = min_t(unsigned int, count, AB1536U_DIAG_DUMP_BYTES);
		dev_info(&hu->serdev->dev, "UART RX chunk %d bytes, first %u: %*ph\n",
			 count, len, len, data);
	}

	if (!hu->hdev)
		return count;

	ab->rx_skb = h4_recv_buf(hu, ab->rx_skb, data, count,
				  ab1536u_recv_pkts, ARRAY_SIZE(ab1536u_recv_pkts));
	if (IS_ERR(ab->rx_skb)) {
		err = PTR_ERR(ab->rx_skb);
		ab->rx_skb = NULL;
		atomic_inc(&ab->rx_errors);
		bt_dev_warn_ratelimited(hu->hdev, "UART frame reassembly failed (%d)", err);
		return err;
	}

	return count;
}

static int ab1536u_stock_start(struct ab1536u *ab)
{
	struct device *dev = &ab->hu.serdev->dev;
	int err;

	if (!ab->controls) {
		dev_info(dev, "Control GPIO sequence omitted: no control GPIOs in DT\n");
		return 0;
	}

	/* These are two observed stock states, not established signal roles.
	 * The high profile reproduces a register snapshot without a pulse.
	 */
	dev_info(dev, "Control profile: %s\n",
		 ab1536u_vendor_high ? "vendor-high" : "recovery-low");
	err = gpiod_direction_output(ab->controls->desc[1], ab1536u_vendor_high);
	if (err)
		return err;

	err = gpiod_direction_output(ab->controls->desc[0], 1);
	if (err)
		return err;

	if (!ab1536u_vendor_high) {
		msleep(200);
		gpiod_set_value_cansleep(ab->controls->desc[0], 0);
	}
	msleep(4000);
	dev_info(dev, "Control GPIO sequence complete: GPIO26=%d GPIO28=%d (raw readback)\n",
		 gpiod_get_raw_value_cansleep(ab->controls->desc[0]),
		 gpiod_get_raw_value_cansleep(ab->controls->desc[1]));
	return 0;
}

static void ab1536u_diag(struct hci_uart *hu, const char *stage)
{
	struct ab1536u *ab = hu->priv;

	/* byte_tx counts bytes accepted by the tty, not bytes on the wire. */
	bt_dev_info(hu->hdev,
		    "%s: TX dequeued=%d tty accepted=%u raw RX=%d reassembly errors=%d queued packets=%u",
		    stage, atomic_read(&ab->tx_dequeued),
		    READ_ONCE(hu->hdev->stat.byte_tx), atomic_read(&ab->rx_bytes),
		    atomic_read(&ab->rx_errors), skb_queue_len(&ab->txq));
}

static int ab1536u_read_controller_nvkey(struct hci_uart *hu)
{
	/* AB1562 command_factory.xml: READFULLKEY, key 0x3604, one byte. */
	static const u8 query_nvkey[] = { 0x05, 0x5a, 0x06, 0x00, 0x00,
					0x0a, 0x04, 0x36, 0x01, 0x00 };
	struct ab1536u *ab = hu->priv;
	struct sk_buff *skb;
	unsigned long completed;

	skb = bt_skb_alloc(sizeof(query_nvkey), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	reinit_completion(&ab->nvkey_done);
	WRITE_ONCE(ab->nvkey_status, -ETIMEDOUT);
	WRITE_ONCE(ab->nvkey_pending, true);
	bt_dev_info(hu->hdev,
		    "RACE NVKEY read-only probe: ID 0x0a00 key 0x3604 length 1 (SDK candidate)");
	skb_put_data(skb, query_nvkey, sizeof(query_nvkey));
	hci_skb_pkt_type(skb) = AB1536U_RACE_PKT;
	skb_queue_tail(&ab->txq, skb);
	hci_uart_tx_wakeup(hu);

	completed = wait_for_completion_timeout(&ab->nvkey_done,
						AB1536U_RACE_TIMEOUT);
	WRITE_ONCE(ab->nvkey_pending, false);
	return completed ? READ_ONCE(ab->nvkey_status) : -ETIMEDOUT;
}

static int ab1536u_setup(struct hci_uart *hu)
{
	/* Wire packet recovered from gtk_ble_daemon 0x00403494. */
	static const u8 query_version[] = { 0x05, 0x5a, 0x04, 0x00,
					  0x09, 0x1c, 0x01, 0xff };
	struct ab1536u *ab = hu->priv;
	struct hci_dev *hdev = hu->hdev;
	struct sk_buff *skb;
	int err;

	WRITE_ONCE(ab->seen_race, false);
	err = ab1536u_stock_start(ab);
	if (err)
		return err;

	reinit_completion(&ab->version_done);
	WRITE_ONCE(ab->version_status, -ETIMEDOUT);
	skb = bt_skb_alloc(sizeof(query_version), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	skb_put_data(skb, query_version, sizeof(query_version));
	hci_skb_pkt_type(skb) = AB1536U_RACE_PKT;
	skb_queue_tail(&ab->txq, skb);
	hci_uart_tx_wakeup(hu);

	if (!wait_for_completion_timeout(&ab->version_done, AB1536U_RACE_TIMEOUT))
		bt_dev_warn(hdev, "RACE version query timed out; checking standard HCI");
	else if (READ_ONCE(ab->version_status))
		bt_dev_warn(hdev, "RACE version query failed (%d); checking standard HCI",
			    READ_ONCE(ab->version_status));
	ab1536u_diag(hu, "After RACE query");

	if (!READ_ONCE(ab->version_status)) {
		err = ab1536u_read_controller_nvkey(hu);
		if (err)
			bt_dev_warn(hdev,
				    "RACE NVKEY 0x3604 query failed (%d); command/key support is unconfirmed",
				    err);
		else
			bt_dev_info(hdev,
				    "RACE NVKEY reply received; key semantics and payload layout require confirmation");
		ab1536u_diag(hu, "After NVKEY read");
	} else {
		bt_dev_info(hdev, "Skipping NVKEY read: no successful RACE version reply");
	}

	/* Do not guess an NVKEY write or claim RACE is a standard HCI event. */
	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	ab1536u_diag(hu, "After HCI Reset");
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "HCI Reset failed (%d), RACE traffic %s; external controller mode is unconfirmed",
			   err, READ_ONCE(ab->seen_race) ? "detected" : "not detected");
		return err;
	}

	kfree_skb(skb);
	bt_dev_info(hdev, "AB1536U standard HCI Reset succeeded");
	return 0;
}

static const struct hci_uart_proto ab1536u_proto = {
	.name = "AB1536U",
	.manufacturer = 0xffff,
	.init_speed = AB1536U_BAUDRATE,
	.open = ab1536u_open,
	.close = ab1536u_close,
	.flush = ab1536u_flush,
	.setup = ab1536u_setup,
	.recv = ab1536u_recv,
	.enqueue = ab1536u_enqueue,
	.dequeue = ab1536u_dequeue,
};

static int ab1536u_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct ab1536u *ab;
	u32 speed;

	if (!device_property_read_u32(dev, "max-speed", &speed) &&
	    speed != AB1536U_BAUDRATE)
		return dev_err_probe(dev, -EINVAL, "Only 115200 baud is supported\n");

	ab = devm_kzalloc(dev, sizeof(*ab), GFP_KERNEL);
	if (!ab)
		return -ENOMEM;

	ab->controls = devm_gpiod_get_array_optional(dev, "gemtek,control", GPIOD_ASIS);
	if (IS_ERR(ab->controls))
		return dev_err_probe(dev, PTR_ERR(ab->controls), "Cannot acquire control GPIOs\n");
	if (ab->controls && ab->controls->ndescs != 2)
		return dev_err_probe(dev, -EINVAL, "Expected two Gemtek control GPIOs\n");
	if (ab->controls &&
	    !of_device_is_compatible(dev->of_node, "gemtek,c6500xk-ab1536u"))
		return dev_err_probe(dev, -EINVAL, "Control sequence is specific to C6500XK\n");
	if (ab->controls &&
	    (gpiod_is_active_low(ab->controls->desc[0]) ||
	     gpiod_is_active_low(ab->controls->desc[1])))
		return dev_err_probe(dev, -EINVAL, "Gemtek control GPIOs must be active high\n");

	ab->hu.serdev = serdev;
	ab->hu.priv = ab;
	atomic_set(&ab->rx_bytes, 0);
	atomic_set(&ab->rx_errors, 0);
	atomic_set(&ab->tx_dequeued, 0);
	skb_queue_head_init(&ab->txq);
	init_completion(&ab->version_done);
	init_completion(&ab->nvkey_done);
	serdev_device_set_drvdata(serdev, &ab->hu);

	return hci_uart_register_device(&ab->hu, &ab1536u_proto);
}

static void ab1536u_remove(struct serdev_device *serdev)
{
	struct hci_uart *hu = serdev_device_get_drvdata(serdev);

	hci_uart_unregister_device(hu);
}

static const struct of_device_id ab1536u_of_match[] = {
	{ .compatible = "gemtek,c6500xk-ab1536u" },
	{ .compatible = "airoha,ab1536u" },
	{ }
};
MODULE_DEVICE_TABLE(of, ab1536u_of_match);

static struct serdev_device_driver ab1536u_driver = {
	.probe = ab1536u_probe,
	.remove = ab1536u_remove,
	.driver = {
		.name = "btab1536u",
		.of_match_table = ab1536u_of_match,
	},
};
module_serdev_device_driver(ab1536u_driver);

MODULE_DESCRIPTION("Experimental Airoha AB1536U UART HCI/RACE transport");
MODULE_LICENSE("GPL");
