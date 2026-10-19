# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_XPON_OMCI) += omci.o
omci-y := core.o access.o agent.o services.o me.o wire.o identity.o profiles.o sysfs.o
omci-$(CONFIG_XPON_OMCI_KUNIT_TEST) += services_test.o
omci-$(CONFIG_XPON_OMCI_ACCESS_KUNIT_TEST) += access_test.o
