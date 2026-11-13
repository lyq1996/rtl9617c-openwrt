# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -I$(src)

obj-$(CONFIG_RTL9617C_NI) += rtl9617c-ni.o
rtl9617c-ni-y := ni/ni_main.o ni/ni_port.o ni/ni_dma.o ni/ni_qm.o \
		 ni/ni_p6.o ni/ni_mib.o ni/ni_api.o
rtl9617c-ni-$(CONFIG_RTL9617C_FE) += fe/fe_main.o fe/fe_hw.o fe/fe_itu.o \
				     fe/fe_bridge.o

obj-$(CONFIG_RTL9617C_BOSA) += rtl9617c-bosa.o
rtl9617c-bosa-y := pon/rtl9617c-bosa.o

obj-$(CONFIG_RTL9617C_PON) += rtl9617c-pon.o
rtl9617c-pon-y := pon/rtl9617c-pon.o pon/rtl9617c-pon-serdes.o \
		  pon/rtl9617c-pon-epon.o pon/rtl9617c-pon-mpcp.o \
		  pon/rtl9617c-pon-pdc.o pon/rtl9617c-pon-puc.o \
		  pon/rtl9617c-pon-oam.o pon/rtl9617c-pon-itu.o \
		  pon/rtl9617c-pon-gpon.o pon/rtl9617c-pon-omci.o \
		  pon/rtl9617c-pon-xgpon.o pon/rtl9617c-pon-path.o
