# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha for the Mediatek SoCs built-in ethernet macs
#

obj-$(CONFIG_NET_AIROHA) += airoha-eth.o
airoha-eth-y := airoha_eth.o airoha_ppe.o
airoha-eth-$(CONFIG_DEBUG_FS) += airoha_ppe_debugfs.o
obj-$(CONFIG_NET_AIROHA_NPU) += airoha_npu.o

airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed.o airoha_wed_debugfs.o

# Airoha EN7521/EN7526/EN751221 GPON + EPON MAC drivers (attach to a DT sfp)
obj-$(CONFIG_AIROHA_EPON) += airoha_epon.o
obj-$(CONFIG_AIROHA_GPON) += airoha-gpon.o
airoha-gpon-y := airoha_gpon.o airoha_ploam.o