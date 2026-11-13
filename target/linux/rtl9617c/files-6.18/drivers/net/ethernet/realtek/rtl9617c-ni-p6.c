/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel_read_file.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>

#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"

static bool rtl_p6_next_ul(const char **line, unsigned long *value)
{
	const char *token = skip_spaces(*line);
	size_t length = strcspn(token, " \t");
	char number[24];

	if (!token || *token == '#' || *token == '\0' || !length ||
	    length >= sizeof(number))
		return false;
	memcpy(number, token, length);
	number[length] = '\0';
	if (kstrtoul(number, 0, value))
		return false;
	*line = token + length;

	return true;
}

static int rtl_p6_read(struct rtl9617c_ni *priv, struct rtl_p6_profile *profile)
{
	size_t size = 0;
	ssize_t ret;

	void *buf = NULL, *end_buf;
	char *line, *next;
	unsigned long offset, value;

	if (profile->status)
		return profile->status > 0 ? 0 : profile->status;
	ret = kernel_read_file_from_path(profile->path, 0, &buf, 0x10000, &size,
					 READING_UNKNOWN);
	if (ret < 0) {
		profile->status = ret;
		return ret;
	}
	if (!size || size > 0x10000) {
		vfree(buf);
		profile->status = -EINVAL;
		return -EINVAL;
	}
	end_buf = buf + size;
	for (line = buf; line < (char *)end_buf; line = next) {
		const char *cursor;

		next = memchr(line, '\n', (char *)end_buf - line);
		if (next)
			*next++ = '\0';
		else
			next = end_buf;
		cursor = line;
		if (!rtl_p6_next_ul(&cursor, &offset))
			continue;
		if (!rtl_p6_next_ul(&cursor, &value))
			continue;
		if (offset < RTL_P6_QX_BASE || offset >= RTL_P6_QX_BASE + 0x1000)
			continue;
		profile->word[profile->count][0] = offset - RTL_P6_QX_BASE;
		profile->word[profile->count][1] = value;
		if (++profile->count == RTL_P6_MAX_WRITES)
			break;
	}
	vfree(buf);
	profile->status = 1;
	return 0;
}

static void rtl_p6_apply(struct rtl9617c_ni *priv, struct rtl_p6_profile *profile)
{
	unsigned int i;

	for (i = 0; i < profile->count; i++) {
		writel(profile->word[i][1], priv->p6_serdes + profile->word[i][0]);
		udelay(RTL_P6_PROFILE_STEP_US);
	}
}

#define RTL_P6_REG_DATAIN	0x50
#define RTL_P6_REG_ACCESS	0x4c
#define RTL_P6_REG_DATAOUT	0x54
#define RTL_P6_REG_STATUS	0x58
#define RTL_P6_SDS_WE		BIT(30)
#define RTL_P6_SDS_ACCESS	BIT(31)

static void rtl_p6_ind_write(struct rtl9617c_ni *priv, u32 addr, u16 data)
{
	writel(data, priv->p6_serdes + RTL_P6_REG_DATAIN);
	writel(RTL_P6_SDS_ACCESS | RTL_P6_SDS_WE | (addr & RTL_NI_P6_SDS_ADDR),
	       priv->p6_serdes + RTL_P6_REG_ACCESS);

	usleep_range(1000, 2000);
}

#define RTL_P6_LANE_STRIDE	4
#define RTL_P6_LANE_COUNT	4

static void rtl_p6_mode_word(struct rtl9617c_ni *priv, u32 word)
{
	unsigned int lane;

	for (lane = 0; lane < RTL_P6_LANE_COUNT; lane++) {
		void __iomem *reg = priv->p6_serdes + 0x8 +
				    lane * RTL_P6_LANE_STRIDE;

		writel((readl(reg) & ~RTL_NI_P6_MODE_WORD) | (word & RTL_NI_P6_MODE_WORD), reg);
	}
}

static void rtl_p6_mac_iface_select(struct rtl9617c_ni *priv);

static void rtl_p6_mac_config(struct rtl9617c_ni *priv)
{
	struct regmap *ni = priv->regs;
	u32 es, qm, value;

	es = rtl_ni_read(ni, RTL_NI_L2TM_ES_CTRL);
	rtl_ni_write(ni, RTL_NI_L2TM_ES_CTRL, es & ~BIT(6));
	qm = rtl_ni_read(ni, RTL_NI_QM_ES_CTRL);
	regmap_write(ni, RTL_NI_QM_ES_CTRL, (qm & 0xffff0000U) | (qm & 0xffU) | ((qm >> 8) & 0xbfU) << 8);
	rtl_ni_write(ni, RTL_NI_NITX_MISC_CFG, rtl_ni_read(ni, RTL_NI_NITX_MISC_CFG) | BIT(20));

	value = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) & 0xcfffffffU;
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, value | 0x80000000U);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, value | 0xb0000000U);

	msleep(10);

	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_TX, rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_TX) & ~0x3U);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_TX, rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_TX) | 1);
	value = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_TX_2);
	regmap_write(ni, RTL_NI_XGE_MAC_CFG_TX_2, (value & 0xffffe000U) | (value & 0xe0U) | 0xa00U | 10);

	value = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_COM);
	value = (value | 0x00008000U | 0x00004000U) & 0xffffdfffU & 0xffffff8fU;
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_COM, value);
	msleep(10);
	value = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_COM);
	value &= 0xffff7fffU & 0xffffbfffU & 0xffffdfffU & 0xffffff8fU;
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_COM, value);

	value = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG);
	value = (value & 0x7fffffffU) & 0xcfffffffU;
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, value);
	value &= 0x7fffffffU;
	value = (value & ~(3U << 28)) | (2U << 28);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, value);
	rtl_ni_write(ni, RTL_NI_NITX_MISC_CFG, rtl_ni_read(ni, RTL_NI_NITX_MISC_CFG) & 0xffefffffU);

	rtl_ni_write(ni, RTL_NI_L2TM_ES_CTRL, rtl_ni_read(ni, RTL_NI_L2TM_ES_CTRL) | BIT(6));
	regmap_write(ni, RTL_NI_QM_ES_CTRL, (qm & 0xffff0000U) | (qm & 0xffU) | (((qm >> 8) & 0xffU) | 0x40) << 8);
}

static bool rtl_p6_serdes_init(struct rtl9617c_ni *priv, u32 word, u32 iface,
			      struct rtl_p6_profile *profile)
{
	u32 value;
	int ret;

	if (priv->qm.block_reset) {
		value = readl(priv->qm.block_reset);
		writel(value | BIT(31), priv->qm.block_reset);
		udelay(1);
		writel(readl(priv->qm.block_reset) & ~BIT(31),
		       priv->qm.block_reset);
	}

	writel(readl(priv->p6_serdes) | BIT(0), priv->p6_serdes);
	writel(readl(priv->p6_serdes + 0x4) | 0x1f, priv->p6_serdes + 0x4);
	usleep_range(1000, 2000);
	writel((readl(priv->p6_serdes + 0x4) & ~0x1fU) | iface,
	       priv->p6_serdes + 0x4);
	rtl_p6_mode_word(priv, word);

	rtl_p6_ind_write(priv, RTL_P6_PMA_CTRL, 0x009f);

	if (rtl_p6_read(priv, profile) == 0)
		rtl_p6_apply(priv, profile);
	else
		dev_warn(priv->dev, "serdes load %s failed\n",
			 profile->path);

	writel(readl(priv->p6_serdes) & ~BIT(0), priv->p6_serdes);

	ret = readl_poll_timeout(priv->p6_serdes + RTL_P6_REG_STATUS, value,
				 value & RTL_P6_CLOCK_BIT,
				 RTL_P6_CLOCK_STEP_MS * USEC_PER_MSEC,
				 RTL_P6_CLOCK_TIMEOUT_MS * USEC_PER_MSEC);
	if (ret) {
		dev_warn(priv->dev, "P6: SerDes clock not ready\n");
		return false;
	}
	msleep(RTL_P6_SETTLE_MS);
	return true;
}

void rtl_p6_bringup(struct rtl9617c_ni *priv)
{
	bool ready = true;
	u32 value;

	if (!priv->p6_serdes || priv->p6_started)
		return;

	if (!priv->sgmii) {
		priv->sgmii = devm_phy_optional_get(priv->dev,
						    "sgmii");
		if (IS_ERR(priv->sgmii)) {
			int lret = PTR_ERR(priv->sgmii);

			priv->sgmii = NULL;
			if (lret != -EPROBE_DEFER)
				dev_warn_once(priv->dev,
					      "P6: no sgmii lane (%d); continuing without it\n",
					      lret);
			ready = false;
		}
	}

	if (priv->sgmii && !priv->sgmii_on) {
		int lret = phy_power_on(priv->sgmii);

		if (lret) {
			dev_warn(priv->dev,
				 "P6: S3 lane power-on failed: %d\n", lret);
			ready = false;
		} else {
			priv->sgmii_on = true;
		}
	}

	if (priv->qm.block_reset) {
		value = readl(priv->qm.block_reset);
		writel(value | 0x102001c0U, priv->qm.block_reset);

		value = readl(priv->qm.block_reset);
		writel(value | BIT(31), priv->qm.block_reset);
		msleep(50);

		value = readl(priv->qm.block_reset);
		writel(value & ~BIT(31), priv->qm.block_reset);
		usleep_range(1000, 2000);
	}

	if (!rtl_p6_serdes_init(priv, RTL_P6_MODE_SGMII_2G5,
			       RTL_P6_IFACE_SGMII_2G5, &priv->p6_profile[0]))
		ready = false;

	if (!priv->sgmii) {
		priv->sgmii_speed = SPEED_2500;
	} else if (priv->sgmii_speed != SPEED_2500) {
		int sret = phy_set_speed(priv->sgmii, SPEED_2500);

		if (sret) {
			dev_warn(priv->dev,
				 "P6: S3 lane 2.5G programming failed: %d\n", sret);
			ready = false;
		} else {
			priv->sgmii_speed = SPEED_2500;
		}
	}

	rtl_p6_mac_config(priv);

	rtl_update(priv, RTL_NI_XGE_MAC_CFG_1, BIT(1) | BIT(4) | BIT(5) | BIT(9),
		   0x0bf3);
	rtl_update(priv, RTL_NI_XGE_MAC_CFG_TX, BIT(4) | BIT(5) | 0x31, 0x31);

	rtl_ni_write(priv->regs, RTL_NI_GLB_INTF_RST_CONFIG, 0);

	rtl_p6_mac_iface_select(priv);
	priv->p6_started = ready;
}

static void rtl_p6_mac_iface_select(struct rtl9617c_ni *priv)
{
	struct regmap *ni = priv->regs;
	u32 value;

	value = rtl_ni_read(ni, RTL_NI_NITX_MISC_CFG);
	rtl_ni_write(ni, RTL_NI_NITX_MISC_CFG, value | BIT(20));

	value = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, value & ~(BIT(31) | (3U << 28)));
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG,
		     (value & ~(BIT(31) | (3U << 28))) | BIT(31) | BIT(28));
	msleep(10);

	value = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_COM);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_COM, value | BIT(13));
	msleep(10);
	value = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_COM);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_COM, value & ~BIT(13));

	value = rtl_ni_read(ni, RTL_NI_NITX_MISC_CFG);
	rtl_ni_write(ni, RTL_NI_NITX_MISC_CFG, value & ~BIT(20));
}

void rtl_p6_set_speed(struct rtl9617c_ni *priv, unsigned int speed)
{
	bool two_g5 = speed == SPEED_2500;
	struct rtl_p6_profile *profile = &priv->p6_profile[two_g5 ? 0 : 1];
	u32 word = two_g5 ? RTL_P6_MODE_SGMII_2G5 : RTL_P6_MODE_SGMII_1G;
	u32 iface = two_g5 ? RTL_P6_IFACE_SGMII_2G5 : RTL_P6_IFACE_SGMII;

	if (!priv->p6_serdes)
		return;

	if (priv->sgmii_speed == speed)
		return;

	if (!rtl_p6_serdes_init(priv, word, iface, profile))
		return;

	if (priv->sgmii) {
		int sret = phy_set_speed(priv->sgmii, speed);

		if (sret) {
			dev_warn(priv->dev,
				 "P6: S3 lane %uM programming failed: %d\n",
				 speed, sret);
			return;
		}
	}
	priv->sgmii_speed = speed;

	rtl_p6_mac_config(priv);

	rtl_p6_mac_iface_select(priv);
}
