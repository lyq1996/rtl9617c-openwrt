// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fixed-size event ring: keeps "what happened, in which order" available after
 * the fact on a customer unit, without a rebuild or a reflash.
 *
 * The recording path never formats a string and never allocates; it stores one
 * 24-byte record under a spinlock. Export happens only when somebody reads
 * /sys/kernel/debug/rtl9617c-pon/events.
 *
 * The ring is module-global because this SoC carries exactly one PON block;
 * each record still carries its own mode so EPON and ITU events can share it.
 */
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include "pon/pon.h"
#include "pon/pon_debug.h"
#define PON_EVENT_COUNT 256

static const char *const pon_event_names[PON_EV_COUNT] = {
	[PON_EV_MPCP_CONFIG] = "mpcp_config",
	[PON_EV_MPCP_STOP] = "mpcp_stop",
	[PON_EV_MPCP_REGISTER] = "mpcp_register",
	[PON_EV_MPCP_NACK] = "mpcp_nack",
	[PON_EV_MPCP_SYNC] = "mpcp_sync",
	[PON_EV_OAM_TX] = "oam_tx",
	[PON_EV_OAM_RX] = "oam_rx",
	[PON_EV_OMCI_TX] = "omci_tx",
	[PON_EV_OMCI_RX] = "omci_rx",
	[PON_EV_PDC_CONFIG] = "pdc_config",
	[PON_EV_PUC_CONFIG] = "puc_config",
	[PON_EV_PLOAM_TX] = "ploam_tx",
	[PON_EV_PLOAM_RX] = "ploam_rx",
	[PON_EV_GPON_STATE] = "gpon_state",
	[PON_EV_XGPON_STATE] = "xgpon_state",
	[PON_EV_ITU_PATHS] = "itu_paths",
	[PON_EV_IRQ] = "irq",
	[PON_EV_ERROR] = "error",
};

static struct {
	spinlock_t lock;
	unsigned int head;
	unsigned int count;
	u64 seq;
	struct pon_event ring[PON_EVENT_COUNT];
} pon_events = {
	.lock = __SPIN_LOCK_UNLOCKED(pon_events.lock),
};

static const char *pon_event_name(unsigned int code)
{
	if (code >= PON_EV_COUNT || !pon_event_names[code])
		return "unknown";
	return pon_event_names[code];
}

void pon_event_at(struct rtl9617c_pon *pon, unsigned int code, u8 phase,
		  u32 a, u32 b, u32 c)
{
	unsigned long flags;
	struct pon_event *ev;
	unsigned int index;

	if (!pon)
		return;
	spin_lock_irqsave(&pon_events.lock, flags);
	index = pon_events.head;
	ev = &pon_events.ring[index];
	ev->ns = ktime_get_mono_fast_ns();
	ev->code = code;
	ev->mode = READ_ONCE(pon->mode);
	ev->phase = phase;
	ev->a = a;
	ev->b = b;
	ev->c = c;
	pon_events.head = (index + 1) % PON_EVENT_COUNT;
	pon_events.seq++;
	if (pon_events.count < PON_EVENT_COUNT)
		pon_events.count++;
	spin_unlock_irqrestore(&pon_events.lock, flags);
}

void pon_event(struct rtl9617c_pon *pon, unsigned int code, u32 a, u32 b, u32 c)
{
	pon_event_at(pon, code, 0, a, b, c);
}

bool pon_event_last(unsigned int code, struct pon_event *out)
{
	unsigned long flags;
	unsigned int i, index;
	bool found = false;

	spin_lock_irqsave(&pon_events.lock, flags);
	for (i = 0; i < pon_events.count; i++) {
		index = (pon_events.head + PON_EVENT_COUNT - 1 - i) % PON_EVENT_COUNT;
		if (pon_events.ring[index].code != code)
			continue;
		*out = pon_events.ring[index];
		found = true;
		break;
	}
	spin_unlock_irqrestore(&pon_events.lock, flags);
	return found;
}

static int pon_events_show(struct seq_file *m, void *unused)
{
	unsigned long flags;
	unsigned int i, index;
	u64 seq;
	struct pon_event ev;

	seq_printf(m, "ring=%u order=newest-first t_ms=monotonic\n", PON_EVENT_COUNT);
	spin_lock_irqsave(&pon_events.lock, flags);
	seq = pon_events.seq;
	for (i = 0; i < pon_events.count; i++) {
		index = (pon_events.head + PON_EVENT_COUNT - 1 - i) % PON_EVENT_COUNT;
		ev = pon_events.ring[index];
		seq--;
		seq_printf(m, "seq=%08llu t_ms=%10llu mode=%-6s ph=%u ev=%-14s a=%08x b=%08x c=%08x\n",
			   seq, div_u64(ev.ns, NSEC_PER_MSEC),
			   ev.mode <= RTL_PON_EPON_BI10G ? "epon" :
			   ev.mode == RTL_PON_GPON ? "gpon" :
			   ev.mode == RTL_PON_XGPON ? "xgpon" : "xgspon",
			   ev.phase, pon_event_name(ev.code), ev.a, ev.b, ev.c);
	}
	spin_unlock_irqrestore(&pon_events.lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_events);

static int pon_events_clear_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t pon_events_clear_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	unsigned long flags;

	spin_lock_irqsave(&pon_events.lock, flags);
	pon_events.head = 0;
	pon_events.count = 0;
	pon_events.seq = 0;
	memset(pon_events.ring, 0, sizeof(pon_events.ring));
	spin_unlock_irqrestore(&pon_events.lock, flags);
	return count;
}

static const struct file_operations pon_events_clear_fops = {
	.owner = THIS_MODULE,
	.open = pon_events_clear_open,
	.write = pon_events_clear_write,
};

int pon_debug_events_init(struct dentry *dir)
{
	debugfs_create_file("events", 0400, dir, NULL, &pon_events_fops);
	debugfs_create_file("events_clear", 0200, dir, NULL, &pon_events_clear_fops);
	return 0;
}
