# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_XPON) += xpon.o
xpon-y := core.o sysfs.o leds.o genl.o uni.o endpoint.o

obj-$(CONFIG_XPON_OMCI) += omci/
obj-$(CONFIG_XPON_OAM) += oam/
xpon-$(CONFIG_XPON_KUNIT_TEST) += tests.o
