# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha for the Mediatek SoCs built-in ethernet macs
#

obj-$(CONFIG_NET_AIROHA_NPU) += airoha_npu.o
obj-$(CONFIG_NET_AIROHA_COMMON) += airoha-eth.o
airoha-eth-y := airoha_ppe.o airoha-common.o
airoha-eth-$(CONFIG_NET_AIROHA) += airoha_eth.o
airoha-eth-$(CONFIG_NET_ECONET) += econet_eth.o
airoha-eth-$(CONFIG_AIROHA_XPON_V1) := airoha_xpon.o airoha_ploam.o airoha_gpon_omci.o
airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed.o
airoha-eth-$(CONFIG_DEBUG_FS) += airoha_ppe_debugfs.o airoha_wed_debugfs.o

