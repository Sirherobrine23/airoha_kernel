# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha and EcoNet frame-engine Ethernet drivers
#

obj-$(CONFIG_NET_AIROHA) += airoha-eth.o
airoha-eth-y := airoha_eth.o airoha_ppe.o
airoha-eth-$(CONFIG_NET_AIROHA_PPE_DEBUGFS) += airoha_ppe_debugfs.o
airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed.o airoha_wed_debugfs.o
airoha-eth-$(CONFIG_NET_AIROHA_WHNAT) += airoha_whnat.o

obj-$(CONFIG_NET_AIROHA_NPU) += airoha_npu.o

# Airoha/EcoNet shared GPON, EPON and XGS-PON MAC complex.
obj-$(CONFIG_AIROHA_XPON_V1) += airoha-xpon.o
airoha-xpon-y := airoha_xpon.o airoha_ploam.o airoha_gpon_omci.o

# EcoNet EN7512/EN7516 DMT/PTM frontend. The line engine is added separately.
obj-$(CONFIG_AIROHA_XDSL) += airoha_xdsl.o
