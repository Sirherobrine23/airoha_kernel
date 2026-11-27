# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha for the Mediatek SoCs built-in ethernet macs
#

obj-$(CONFIG_NET_AIROHA) += airoha-eth.o
airoha-eth-y := airoha_eth.o airoha_ppe.o
airoha-eth-$(CONFIG_DEBUG_FS) += airoha_ppe_debugfs.o
airoha-eth-$(CONFIG_NET_AIROHA_TSO) += airoha_tso.o
airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed.o
ifdef CONFIG_DEBUG_FS
airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed_debugfs.o
endif
obj-$(CONFIG_NET_AIROHA_NPU) += airoha_npu.o
