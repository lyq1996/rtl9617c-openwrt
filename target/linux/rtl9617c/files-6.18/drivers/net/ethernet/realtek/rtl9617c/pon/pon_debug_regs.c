// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Read-only register window snapshot plus the gated poke channel.
 *
 * The window table is the single place that decides which offsets are readable
 * without side effects. Windows are mode-gated: an EPON unit never reads an
 * XGTC block that was never mapped/powered for that mode.
 *
 * The reads are plain readl(). Write-1-to-clear interrupt status registers are
 * not touched here, and the PDC/PUC access registers are only *read* (their
 * busy bit is a status bit; the write that starts an indirect cycle is not
 * issued from this file).
 */
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include "pon/pon.h"
#include "pon/pon_debug.h"
#define PON_REG_MAX_OFFSET 0xc000

/* Allowed modes per window; 0 means every mode. */
#define PON_REG_MODES_EPON	(BIT(RTL_PON_EPON_1G) | BIT(RTL_PON_EPON_D10G) | \
				 BIT(RTL_PON_EPON_BI10G))
#define PON_REG_MODES_GPON	BIT(RTL_PON_GPON)
#define PON_REG_MODES_XG	(BIT(RTL_PON_XGPON) | BIT(RTL_PON_XGSPON))

enum pon_reg_block {
	PON_REG_MAC,
	PON_REG_CONTROL,
	PON_REG_EPON_CONTROL,
	PON_REG_TOP,
	PON_REG_OPTICAL_STATUS,
	PON_REG_OPTICAL_CONTROL,
};

struct pon_reg_window {
	const char *name;
	u8 block;
	u16 first;
	u16 last;	/* inclusive, step 4 */
	u8 modes;
};

static const struct pon_reg_window pon_reg_windows[] = {
	{ "epon_status",	PON_REG_MAC, 0x0000, 0x001c, PON_REG_MODES_EPON },
	{ "epon_llid",		PON_REG_MAC, 0x0064, 0x0170, PON_REG_MODES_EPON },
	{ "epon_mib",		PON_REG_MAC, 0x0600, 0x0604, PON_REG_MODES_EPON },
	{ "epon_fec",		PON_REG_MAC, 0x0800, 0x0814, PON_REG_MODES_EPON },
	{ "mpcp",		PON_REG_MAC, 0x2000, 0x204c, PON_REG_MODES_EPON },
	{ "puc_epon",		PON_REG_MAC, 0x8000, 0x808c, PON_REG_MODES_EPON },
	{ "pdc",		PON_REG_MAC, 0x9000, 0x9028, 0 },
	{ "serdes",		PON_REG_MAC, 0xa02c, 0xa06c, 0 },
	{ "gpon_ctrl",		PON_REG_MAC, 0x6000, 0x6024, PON_REG_MODES_GPON },
	{ "gpon_ploam",		PON_REG_MAC, 0x60e0, 0x6118, PON_REG_MODES_GPON },
	{ "xgpon_ctrl",		PON_REG_MAC, 0x4000, 0x4004, PON_REG_MODES_XG },
	{ "xgpon_state",	PON_REG_MAC, 0x4d30, 0x4d34, PON_REG_MODES_XG },
	{ "pon_control",	PON_REG_CONTROL, 0x0000, 0x0000, 0 },
	{ "epon_control",	PON_REG_EPON_CONTROL, 0x0000, 0x0000, PON_REG_MODES_EPON },
	{ "top_irq",		PON_REG_TOP, 0x0000, 0x0004, 0 },
	{ "optical_status",	PON_REG_OPTICAL_STATUS, 0x0000, 0x0000, 0 },
	{ "optical_control",	PON_REG_OPTICAL_CONTROL, 0x0000, 0x0000, 0 },
};

static void __iomem *pon_reg_base(struct rtl9617c_pon *pon, unsigned int block)
{
	switch (block) {
	case PON_REG_MAC:
		return pon->mac;
	case PON_REG_CONTROL:
		return pon->control;
	case PON_REG_EPON_CONTROL:
		return pon->epon_control;
	case PON_REG_TOP:
		return pon->top_irq;
	case PON_REG_OPTICAL_STATUS:
		return pon->optical_status;
	case PON_REG_OPTICAL_CONTROL:
		return pon->optical_control;
	default:
		return NULL;
	}
}

static int pon_regs_show(struct seq_file *m, void *unused)
{
	struct rtl9617c_pon *pon = pon_debug_pon();
	const struct pon_reg_window *w;
	unsigned int i, offset, mask;
	void __iomem *base;

	if (!pon) {
		seq_puts(m, "device gone\n");
		return 0;
	}
	mask = BIT(READ_ONCE(pon->mode));
	seq_printf(m, "mode=%s windows=%zu\n", rtl_pon_mode_name(pon->mode),
		   ARRAY_SIZE(pon_reg_windows));
	for (i = 0; i < ARRAY_SIZE(pon_reg_windows); i++) {
		w = &pon_reg_windows[i];
		base = pon_reg_base(pon, w->block);
		if (!base)
			continue;
		if (!w->modes)
			seq_printf(m, "%s:", w->name);
		else if (w->modes & mask)
			seq_printf(m, "%s:", w->name);
		else
			continue;
		for (offset = w->first; offset <= w->last; offset += 4)
			seq_printf(m, " %04x=%08x", offset, readl(base + offset));
		seq_puts(m, "\n");
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_regs);

static ssize_t pon_reg_write_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct rtl9617c_pon *pon = pon_debug_pon();
	char buf[64];
	unsigned int offset, value;

	if (!pon_debug_poke)
		return -EPERM;
	if (!pon)
		return -ENODEV;
	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (sscanf(buf, "%x %x", &offset, &value) != 2)
		return -EINVAL;
	if ((offset & 3) || offset >= PON_REG_MAX_OFFSET)
		return -EINVAL;
	/*
	 * Experimental channel only: no locking, no ownership, no filtering
	 * beyond alignment. Never reachable unless debug_poke=1 was set at load
	 * time, so it cannot fire in a production boot.
	 */
	writel(value, pon->mac + offset);
	pon_event(pon, PON_EV_ERROR, 1, offset, value);
	return count;
}

static const struct file_operations pon_reg_write_fops = {
	.owner = THIS_MODULE,
	.write = pon_reg_write_write,
};

int pon_debug_regs_init(struct dentry *dir)
{
	debugfs_create_file("regs", 0400, dir, NULL, &pon_regs_fops);
	debugfs_create_file("reg_write", 0200, dir, NULL, &pon_reg_write_fops);
	return 0;
}
