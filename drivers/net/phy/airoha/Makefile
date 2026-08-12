obj-$(CONFIG_AIROHA_LDDLA_PHY) += airoha_lddla.o
obj-$(CONFIG_EN7570_PHY) += airoha_en7570.o
obj-$(CONFIG_EN7571_PHY) += airoha_en7571.o
obj-$(CONFIG_EN7572_PHY) += airoha_en7572.o
obj-$(CONFIG_GN25L95_PHY) += semtech_gn25l95.o
airoha_lddla-y += airoha_lddla.o

airoha_en7570-$(CONFIG_EN7570_PHY)	+= en7570_adcloop.o
airoha_en7570-$(CONFIG_EN7570_PHY)	+= en7570_ddmi.o
airoha_en7570-$(CONFIG_EN7570_PHY)	+= en7570_main.o
airoha_en7570-$(CONFIG_EN7570_PHY)	+= en7570_txrx.o

airoha_en7571-$(CONFIG_EN7571_PHY)	+= en7571_adcloop.o
airoha_en7571-$(CONFIG_EN7571_PHY)	+= en7571_ddmi.o
airoha_en7571-$(CONFIG_EN7571_PHY)	+= en7571_main.o
airoha_en7571-$(CONFIG_EN7571_PHY)	+= en7571_txrx.o

airoha_en7572-$(CONFIG_EN7572_PHY)	+= en7572_ddmi.o
airoha_en7572-$(CONFIG_EN7572_PHY)	+= en7572_loop.o
airoha_en7572-$(CONFIG_EN7572_PHY)	+= en7572_main.o

semtech_gn25l95-$(CONFIG_GN25L95_PHY) += gn25l95.o
