# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha for the Mediatek SoCs built-in ethernet macs
#

obj-$(CONFIG_NET_AIROHA_COMMON) += airoha-common.o
airoha-common-y := airoha_common.o

obj-$(CONFIG_NET_AIROHA) += airoha-eth.o
airoha-eth-y := airoha_eth.o airoha_ppe.o
airoha-eth-$(CONFIG_DEBUG_FS) += airoha_ppe_debugfs.o
obj-$(CONFIG_NET_AIROHA_NPU) += airoha_npu.o

obj-$(CONFIG_NET_ECONET_EN751221) += econet-eth.o
econet-eth-y := econet_eth.o

airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed.o airoha_wed_debugfs.o

# Airoha/EcoNet shared GPON + EPON MAC complex
obj-$(CONFIG_AIROHA_XPON) += airoha-xpon.o
airoha-xpon-y := airoha_xpon.o airoha_ploam.o airoha_gpon_omci.o
