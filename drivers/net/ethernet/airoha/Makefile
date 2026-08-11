# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha and EcoNet frame-engine Ethernet drivers
#

# Shared GDM/phylink helpers and the common PPE frontend/backends.
obj-$(CONFIG_NET_AIROHA_COMMON) += airoha-common.o
airoha-common-y := airoha_common.o airoha_ppe.o
airoha-common-$(CONFIG_NET_AIROHA_PPE_DEBUGFS) += airoha_ppe_debugfs.o

# Shared Ethernet host with generation-specific FE backends.
obj-$(CONFIG_NET_AIROHA_ETH_CORE) += airoha-eth.o
airoha-eth-y := airoha_eth.o airoha_qdma.o airoha_eth_gen1.o airoha_eth_gen2.o
airoha-eth-$(CONFIG_NET_AIROHA_SOC_WED) += airoha_wed.o airoha_wed_debugfs.o

obj-$(CONFIG_NET_AIROHA_NPU) += airoha_npu.o

# Airoha/EcoNet shared GPON + EPON MAC complex.
obj-$(CONFIG_AIROHA_XPON_V1) += airoha-xpon.o
airoha-xpon-y := airoha_xpon.o airoha_ploam.o airoha_gpon_omci.o
