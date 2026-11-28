// SPDX-License-Identifier: GPL-2.0-or-later
/* Native GPON/XGTC control IO, verified against stock ca-ne.ko. */
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include "pon/pon.h"
#include "pon/pon_priv.h"
#include "pon/pon_debug.h"
#include "pon/itu/gpon_hw.h"
#include "pon/itu/xgpon_hw.h"

static unsigned int rtl_itu_read(void *context, unsigned int reg)
{
	struct rtl9617c_pon *pon = context;

	return readl(pon->mac + reg);
}

static void rtl_itu_write(void *context, unsigned int reg, unsigned int value)
{
	struct rtl9617c_pon *pon = context;

	writel(value, pon->mac + reg);
}

ssize_t rtl_pon_itu_state(struct rtl9617c_pon *pon, char *buf)
{
	static const char *const gpon_states[] = {"O1", "O2", "O3", "O4", "O5", "O6", "O7", "reserved"};
	static const char *const xgpon_states[] = {"O1", "O23", "O4", "O5", "O6", "O7", "O8", "O9"};
	u32 word, state;

	lockdep_assert_held(&pon->lock);
	if (pon->mode == RTL_PON_GPON) {
		word = readl(pon->mac + RTL_GPON_60BC);
		state = FIELD_GET(GENMASK(18, 16), word);
		return sysfs_emit(buf,
			"prepared=%u error=%d protocol=gpon activation=%s hardware_state=%u onu_id=%u onu=%08x control=%08x config=%08x rx=%llu tx=%llu enabled=%u omcc=%u irq_count=%llu key_switches=%llu\n",
			pon->mac_prepared, pon->mac_error, gpon_states[state], state,
			word & 255, word, readl(pon->mac + RTL_GPON_6114), readl(pon->mac + RTL_GPON_6118),
			pon->ploam_rx, pon->ploam_tx, pon->gpon_enabled, pon->gpon_omcc_ready,
			pon->gpon_irq_count, pon->gpon_key_switches);
	}
	word = readl(pon->mac + RTL_XGTC_4D30);
	state = word & 7;
	return sysfs_emit(buf,
		"prepared=%u error=%d protocol=%s activation=%s hardware_state=%u sub_state=%u previous_state=%u actp=%08x rx0=%08x rx1=%08x rx=%llu tx=%llu mic_errors=%llu control=%08x upstream_rate=%u\n",
		pon->mac_prepared, pon->mac_error, rtl_pon_mode_name(pon->mode),
		xgpon_states[state], state, !!(word & BIT(4)), (u32)FIELD_GET(GENMASK(10, 8), word), word,
		readl(pon->mac + RTL_XGTC_4B28), readl(pon->mac + RTL_XGTC_4B2C),
		pon->ploam_rx, pon->ploam_tx, pon->ploam_mic_errors, readl(pon->mac + RTL_XGTC_4004),
		(u32)FIELD_GET(GENMASK(7, 6), readl(pon->mac + RTL_XGTC_4004)));
}

ssize_t rtl_pon_ploam_receive(struct rtl9617c_pon *pon, char *buf, size_t count)
{
	const struct rtl_dq_io io = {pon, rtl_itu_read, rtl_itu_write, NULL};
	unsigned char order[2];
	u32 status[2], received_status;
	unsigned int ready;
	ssize_t ret;

	pon_event_at(pon, PON_EV_PLOAM_RX, 0, 0, (u32)count, 0);
	pon_dbg("ploam rx: window=%u\n", (u32)count);
	mutex_lock(&pon->control_lock);
	mutex_lock(&pon->lock);
	if (rtl_pon_mode_is_epon(pon->mode)) {
		ret = -EOPNOTSUPP;
	} else if (pon->mode == RTL_PON_GPON) {
		if (count < RTL_GPON_PLOAM_BYTES) {
			ret = -EMSGSIZE;
		} else {
			rtl_gpon_ploam_receive(&io, (u8 *)buf);
			ret = buf[1] ? RTL_GPON_PLOAM_BYTES : -EAGAIN;
		}
	} else if (count < RTL_XGPON_PLOAM_BYTES) {
		ret = -EMSGSIZE;
	} else if (pon->xgpon_pending_valid) {
		memcpy(buf, pon->xgpon_pending_ploam, RTL_XGPON_PLOAM_BYTES);
		pon->xgpon_pending_valid = false;
		ret = RTL_XGPON_PLOAM_BYTES;
		if (pon->xgpon_pending_status & BIT(15)) {
			pon->ploam_mic_errors++;
			ret = -EBADMSG;
		}
	} else {
		status[0] = readl(pon->mac + RTL_XGTC_4B28);
		status[1] = readl(pon->mac + RTL_XGTC_4B2C);
		ready = rtl_xgpon_ploam_order(status[0], status[1], order);
		if (!ready) {
			ret = -EAGAIN;
		} else {
			rtl_xgpon_ploam_receive(&io, order[0], (u8 *)buf);
			received_status = status[order[0]];
			if (ready == 2) {
				/* Snapshot both old packets before accepting a refill. */
				rtl_xgpon_ploam_receive(&io, order[1], pon->xgpon_pending_ploam);
				pon->xgpon_pending_status = status[order[1]];
				pon->xgpon_pending_valid = true;
			}
			if (received_status & BIT(15)) {
				pon->ploam_mic_errors++;
				ret = -EBADMSG;
			} else {
				ret = RTL_XGPON_PLOAM_BYTES;
			}
		}
	}
	if (ret > 0) {
		pon->ploam_rx++;
		/* byte1/byte2 are the PLOAM type and sequence on the GPON path. */
		pon_event_at(pon, PON_EV_PLOAM_RX, 1, (u32)(u8)buf[1],
			     (u32)(u8)buf[2], (u32)ret);
	} else {
		pon_event_at(pon, PON_EV_PLOAM_RX, 2, 0, 0, (u32)ret);
	}
	mutex_unlock(&pon->lock);
	mutex_unlock(&pon->control_lock);
	return ret;
}

ssize_t rtl_pon_ploam_transmit(struct rtl9617c_pon *pon, const char *buf, size_t count,
			     bool default_key)
{
	const struct rtl_dq_io io = {pon, rtl_itu_read, rtl_itu_write, NULL};
	u32 msg_type = count >= 2 ? (u8)buf[1] : 0;
	u32 value;
	ssize_t ret;

	pon_event_at(pon, PON_EV_PLOAM_TX, 0, msg_type, (u32)count, 0);
	pon_dbg("ploam tx: type=%u len=%u\n", msg_type, (u32)count);
	mutex_lock(&pon->control_lock);
	mutex_lock(&pon->lock);
	if (rtl_pon_mode_is_epon(pon->mode) || (pon->mode == RTL_PON_GPON && default_key)) {
		ret = -EOPNOTSUPP;
	} else if (!pon->mac_prepared || (pon->mode == RTL_PON_GPON && !(readl(pon->mac + RTL_GPON_6114) & BIT(1)))) {
		ret = -EAGAIN;
	} else if (pon->mode == RTL_PON_GPON) {
		if (count != 12 || (unsigned char)buf[0] != (readl(pon->mac + RTL_GPON_60BC) & 255)) {
			ret = -EINVAL;
		} else {
			ret = readl_poll_timeout(pon->mac + RTL_GPON_60E0, value, !(value & BIT(0)), 1, 10000);
			if (!ret) {
				rtl_gpon_ploam_transmit(&io, buf[1], (const u8 *)buf + 2);
				ret = readl_poll_timeout(pon->mac + RTL_GPON_60E0, value, !(value & BIT(0)), 1, 10000);
			}
		}
	} else if (count != RTL_XGPON_PLOAM_TX_BYTES) {
		ret = -EMSGSIZE;
	} else {
		ret = readl_poll_timeout(pon->mac + RTL_XGTC_4AFC, value, !(value & BIT(31)), 1, 10000);
		if (!ret) {
			rtl_xgpon_ploam_transmit(&io, (const u8 *)buf, default_key);
			ret = readl_poll_timeout(pon->mac + RTL_XGTC_4AFC, value, !(value & BIT(31)), 1, 10000);
		}
	}
	if (!ret) {
		pon->ploam_tx++;
		ret = count;
	}
	mutex_unlock(&pon->lock);
	mutex_unlock(&pon->control_lock);
	pon_event_at(pon, PON_EV_PLOAM_TX, 1, msg_type, (u32)count, (u32)ret);
	return ret;
}


static ssize_t ploam_read(struct file *file, struct kobject *kobj,
			 const struct bin_attribute *attr, char *buf, loff_t offset, size_t count)
{
	if (offset)
		return 0;
	return rtl_pon_ploam_receive(dev_get_drvdata(kobj_to_dev(kobj)), buf, count);
}

static ssize_t ploam_write(struct file *file, struct kobject *kobj,
			  const struct bin_attribute *attr, char *buf, loff_t offset, size_t count)
{
	if (offset)
		return -EINVAL;
	return rtl_pon_ploam_transmit(dev_get_drvdata(kobj_to_dev(kobj)), buf, count, false);
}
BIN_ATTR_ADMIN_RW(ploam, 0);

static ssize_t ploam_default_write(struct file *file, struct kobject *kobj,
				   const struct bin_attribute *attr, char *buf,
				   loff_t offset, size_t count)
{
	if (offset)
		return -EINVAL;
	return rtl_pon_ploam_transmit(dev_get_drvdata(kobj_to_dev(kobj)), buf, count, true);
}
BIN_ATTR(ploam_default, 0200, NULL, ploam_default_write, 0);
