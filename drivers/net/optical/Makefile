# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_OPTICAL_FRONTEND) += optical_frontend.o
optical_frontend-y := core.o
optical_frontend-$(CONFIG_OPTICAL_FRONTEND_HWMON) += hwmon.o
optical_frontend-$(CONFIG_OPTICAL_FRONTEND_SFP_COMPAT) += sfp_compat.o

obj-y += semtech/
