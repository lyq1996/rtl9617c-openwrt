/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_FE_PIPELINE_H
#define RTL9617C_FE_PIPELINE_H
#include "rtl9617c-fe-table.h"

struct rtl_fe_cam_protocol {
	unsigned int saved[5];
	unsigned char bank,index;
	unsigned char created;
};

#endif

