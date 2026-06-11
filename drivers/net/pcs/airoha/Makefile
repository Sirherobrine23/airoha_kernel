# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_PCS_AIROHA_COMMON)	+= pcs-airoha.o
pcs-airoha-y			:= pcs-airoha-common.o
ifdef CONFIG_PCS_AIROHA_AN7581
pcs-airoha-y			+= pcs-an7581.o
endif
ifdef CONFIG_PCS_AIROHA_AN7583
pcs-airoha-y			+= pcs-an7583.o
endif
obj-$(CONFIG_PCS_AIROHA_EN7523)	+= pcs-en7523.o