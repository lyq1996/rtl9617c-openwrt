/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_IO_H
#define RTL9617C_IO_H

struct rtl_dq_io {
	void *context;
	unsigned int (*read)(void *context, unsigned int reg);
	void (*write)(void *context, unsigned int reg, unsigned int value);
	int (*indirect)(void *context, unsigned int reg, unsigned int value);
};

#endif
