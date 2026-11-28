// SPDX-License-Identifier: GPL-2.0
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/soc/realtek/taurus-soc.h>
#include <linux/spinlock.h>

#include "pcie-designware.h"

#define TAURUS_PCIE_SERDES_CFG_VER_UNKNOWN	"unknown"

#define MAX_INTX_HOST_IRQS		4
#define MAX_BER_POLL_COUNT		50
#define MAX_LANE_NUM			2
#define MAX_NAME_LEN			16
#define SERDES_LANE_STRIDE		0x1000

#define to_taurus_pcie(x)		dev_get_drvdata((x)->dev)

struct serdes_cfg {
	u32 addr;
	u32 val;
};

struct taurus_pcie {
	struct dw_pcie *pci;
	void __iomem *reg_base;
	void __iomem *serdes_base;
	phys_addr_t dbi_start;
	phys_addr_t dbi_end;

	phys_addr_t iatu_unroll_start;
	void __iomem *iatu_unroll_base;
	void __iomem *coherent_atu;
	u32 coherent_hi_addr;
	bool dma_coherent;

	struct reset_control *core_reset;
	struct reset_control *phy_reset;
	struct reset_control *device_reset;
	struct reset_control *device_power;
	struct gpio_desc *perst;

	struct phy *phy[MAX_LANE_NUM];
	resource_size_t serdes_size;
	const char *serdes_cfg_ver;
	struct serdes_cfg *cfg[MAX_LANE_NUM];
	int cfg_cnt[MAX_LANE_NUM];

	u32 idx;
	u8 lanes;
	bool rev_a;
	bool phy_on;
	int irq_affinity;

	raw_spinlock_t intr_lock;
	struct irq_domain *intx_domain;
};

#define PCIE_GLBL_INTERRUPT_0					0x00
#define INT_RADM_INTA_ASSERTED					0x00000001
#define INT_RADM_INTB_ASSERTED					0x00000004
#define INT_RADM_INTC_ASSERTED					0x00000010
#define INT_RADM_INTD_ASSERTED					0x00000040
#define INT_MSI_CTR_INT							0x00000100
#define INT_HP_INT								0x00000400
#define INT_RADM_CORRECTABLE_ERR				0x00000800
#define INT_RADM_NONFATAL_ERR					0x00001000
#define INT_RADM_FATAL_ERR						0x00002000
#define INT_RADM_PM_TO_ACK						0x00004000
#define INT_RADM_PM_PME							0x00008000
#define INT_RADM_QOVERFLOW						0x00010000
#define INT_LINK_DOWN							0x00400000
#define INT_CFG_AER_RC_ERR_MSI					0x00800000
#define INT_CFG_PME_MSI							0x01000000
#define INT_HP_PME								0x02000000
#define INT_HP_MSI								0x04000000
#define INT_CFG_UR_RESP							0x08000000
#define PCIE_GLBL_INTERRUPT_ENABLE_0			0x04
#define PCIE_GLBL_CORE_CONFIG_REG					0x18
#define PCIE_LTSSM_ENABLE							BIT(0)
#define PCIE_LINK_DOWN_RST							BIT(6)
#define PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS	0x1C
#define PWR_STATUS_SMLH_LTSSM_STATE_MASK			0x000003F0
#define PWR_STATUS_RDLH_LINK_UP						BIT(18)
#define PCIE_GLBL_PCIE_CONTR_CFG_START_ADDR			0x40
#define PCIE_GLBL_PCIE_CONTR_CFG_END_ADDR			0x44
#define PCIE_GLBL_PCIE_CONTR_IATU_BASE_ADDR			0x48
#define RC_IATU_BASE_ADDR_MASK					0xFFF80000
#define PCIE_MSI_INTERRUPT_ENABLE_0				0x1a4

#define TAURUS_ATU_CTRL						0x00
#define TAURUS_ATU_CTRL_IDX_MASK				GENMASK(29, 24)
#define TAURUS_ATU_CTRL_R					BIT(30)
#define TAURUS_ATU_CTRL_W					BIT(31)
#define TAURUS_ATU_INPUT					0x04
#define TAURUS_ATU_MASK						0x08
#define TAURUS_ATU_OUTPUT					0x0c
#define TAURUS_ATU_ACE_CFG					0x10
#define TAURUS_ATU_AXI_ARUSER					0x14
#define TAURUS_ATU_AXI_AWUSER					0x18

#define TAURUS_ATU_ACE_ARPROT_NS				FIELD_PREP(GENMASK(2, 0), 2)
#define TAURUS_ATU_ACE_AWPROT_NS				FIELD_PREP(GENMASK(6, 4), 2)
#define TAURUS_ATU_ACE_ARCACHE_WB_RWA				FIELD_PREP(GENMASK(19, 16), 0xf)
#define TAURUS_ATU_ACE_AWCACHE_WB_RWA				FIELD_PREP(GENMASK(23, 20), 0xf)
#define TAURUS_ATU_ACE_USER_EN					BIT(24)
#define TAURUS_ATU_ACE_PROT_EN					BIT(25)
#define TAURUS_ATU_ACE_CACHE_EN					BIT(26)
#define TAURUS_ATU_ACE_COHERENT_CFG				(TAURUS_ATU_ACE_ARPROT_NS | \
								 TAURUS_ATU_ACE_AWPROT_NS | \
								 TAURUS_ATU_ACE_ARCACHE_WB_RWA | \
								 TAURUS_ATU_ACE_AWCACHE_WB_RWA | \
								 TAURUS_ATU_ACE_USER_EN | \
								 TAURUS_ATU_ACE_PROT_EN | \
								 TAURUS_ATU_ACE_CACHE_EN)
#define TAURUS_ATU_AXUSER_OUTER_SHAREABLE			FIELD_PREP(GENMASK(7, 6), 2)

#define TAURUS_ATU_TLB_COUNT					8
#define TAURUS_ATU_DDR_LOW_INPUT				0x00000
#define TAURUS_ATU_DDR_HIGH_INPUT				0x18000
#define TAURUS_ATU_DDR_WINDOW_PAGES				(SZ_2G / SZ_64K)
#define TAURUS_ATU_DEFAULT_HI_ADDR				0x4

static const char * const taurus_pcie_ltssm_names[] = {
	"DETECT_QUIET",
	"DETECT_ACT",
	"POLL_ACTIVE",
	"POLL_COMPLIANCE",
	"POLL_CONFIG",
	"PRE_DETECT_QUIET",
	"DETECT_WAIT",
	"CFG_LINKWD_START",
	"CFG_LINKWD_ACEPT",
	"CFG_LANENUM_WAI",
	"CFG_LANENUM_ACEPT",
	"CFG_COMPLETE",
	"CFG_IDLE",
	"RCVRY_LOCK",
	"RCVRY_SPEED",
	"RCVRY_RCVRCFG",
	"RCVRY_IDLE",
	"L0",
	"L0S",
	"L123_SEND_EIDLE",
	"L1_IDLE",
	"L2_IDLE",
	"L2_WAKE",
	"DISABLED_ENTRY",
	"DISABLED_IDLE",
	"DISABLED",
	"LPBK_ENTRY",
	"LPBK_ACTIVE",
	"LPBK_EXIT",
	"LPBK_EXIT_TIMEOUT",
	"HOT_RESET_ENTRY",
	"HOT_RESET",
	"RCVRY_EQ0",
	"RCVRY_EQ1",
	"RCVRY_EQ2",
	"RCVRY_EQ3"
};

static const char *taurus_pcie_ltssm_name(unsigned int state)
{
	if (state < ARRAY_SIZE(taurus_pcie_ltssm_names))
		return taurus_pcie_ltssm_names[state];

	return "UNKNOWN";
}

static u32 taurus_pcie_readl(struct taurus_pcie *pcie, u32 reg);
static void taurus_pcie_writel(struct taurus_pcie *pcie, u32 val, u32 reg);

static int taurus_pcie_device_reset(struct taurus_pcie *pcie);
static unsigned int taurus_pcie_ltssm(struct taurus_pcie *pcie);

static ssize_t taurus_pcie_device_reset_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct taurus_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	ret = taurus_pcie_device_reset(pcie);
	if (ret)
		return ret;

	return count;
}

static void taurus_pcie_serdes_switch_oobs(void __iomem *phy_base)
{
	writel(0xf75a, phy_base + 0x0034);
	usleep_range(10, 20);
	writel(0xf75a, phy_base + 0x0134);
	usleep_range(10, 20);
	writel(0x4500, phy_base + 0x00bc);
	usleep_range(10, 20);
	writel(0x3d00, phy_base + 0x01bc);
	usleep_range(10, 20);
	writel(0x0011, phy_base + 0x006c);
	usleep_range(10, 20);
	writel(0x0011, phy_base + 0x016c);
	usleep_range(10, 20);
	writel(0x01a4, phy_base + 0x0040);
	usleep_range(10, 20);
	writel(0x01a4, phy_base + 0x0140);
	usleep_range(10, 20);
}

static DEVICE_ATTR(device_reset, 0200, NULL,
		   taurus_pcie_device_reset_store);

static ssize_t taurus_pcie_serdes_phy_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct taurus_pcie *pcie = dev_get_drvdata(dev);
	ssize_t len = 0;
	void __iomem *phy_base;
	unsigned int lane, addr;
	u32 val;

	len += sysfs_emit_at(buf, len, "PCIe PHY ver: %s\n",
			     pcie->serdes_cfg_ver);

	for (lane = 0; lane < pcie->lanes; lane++) {
		len += sysfs_emit_at(buf, len, "lane [%u]\n", lane);
		len += sysfs_emit_at(buf, len, "Gen 1:  \tGen 2:\n");

		phy_base = pcie->serdes_base + lane * SERDES_LANE_STRIDE;
		taurus_pcie_serdes_switch_oobs(phy_base);
		len += sysfs_emit_at(buf, len, "Page 0  \tPage 0\n");
		for (addr = 0; addr <= 0x1f; addr++) {
			val = readl(phy_base + addr * 4) & 0xffff;
			len += sysfs_emit_at(buf, len, "%02x: %04x\t", addr,
					     val);
			val = readl(phy_base + (addr + 0x40) * 4) & 0xffff;
			len += sysfs_emit_at(buf, len, "%02x: %04x\n", addr,
					     val);
		}
		len += sysfs_emit_at(buf, len, "Page 1  \tPage 1\n");
		for (addr = 0x20; addr <= 0x34; addr++) {
			val = readl(phy_base + addr * 4) & 0xffff;
			len += sysfs_emit_at(buf, len, "%02x: %04x\t",
					     addr - 0x20, val);
			val = readl(phy_base + (addr + 0x40) * 4) & 0xffff;
			len += sysfs_emit_at(buf, len, "%02x: %04x\n",
					     addr - 0x20, val);
		}
		len += sysfs_emit_at(buf, len, "\n");
	}

	return len;
}

static ssize_t taurus_pcie_serdes_phy_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	dev_info(dev, "Use \"devmem 0xf433[serdes phy id]000 + addr*0x4 32 [value]\" to write PHY parameters\n");
	dev_info(dev, "Example: devmem 0xf4333004 32 0x1852\n");
	return count;
}

static DEVICE_ATTR(serdes_phy, 0600, taurus_pcie_serdes_phy_show,
		   taurus_pcie_serdes_phy_store);

static struct attribute *taurus_pcie_attrs[] = {
	&dev_attr_device_reset.attr,
	&dev_attr_serdes_phy.attr,
	NULL,
};

static const struct attribute_group taurus_pcie_attr_group = {
	.attrs = taurus_pcie_attrs,
};

static void taurus_pcie_intr_update(struct taurus_pcie *pcie, u32 clr, u32 set)
{
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pcie->intr_lock, flags);
	val = taurus_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_ENABLE_0);
	val &= ~clr;
	val |= set;
	taurus_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_ENABLE_0);
	raw_spin_unlock_irqrestore(&pcie->intr_lock, flags);
}

static void taurus_pcie_intx_mask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct taurus_pcie *pcie = to_taurus_pcie(to_dw_pcie_from_pp(pp));

	taurus_pcie_intr_update(pcie, BIT(2 * d->hwirq), 0);
}

static void taurus_pcie_intx_unmask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct taurus_pcie *pcie = to_taurus_pcie(to_dw_pcie_from_pp(pp));

	taurus_pcie_intr_update(pcie, 0, BIT(2 * d->hwirq));
}

static struct irq_chip taurus_pcie_intx_chip = {
	.name = "PCI-INTx",
	.irq_mask = taurus_pcie_intx_mask,
	.irq_unmask = taurus_pcie_intx_unmask,
};

static int taurus_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &taurus_pcie_intx_chip,
				 handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops taurus_pcie_intx_domain_ops = {
	.map = taurus_pcie_intx_map,
	.xlate = irq_domain_xlate_onecell,
};

static void taurus_pcie_serdes_load_cfg(struct taurus_pcie *pcie)
{
	struct device *dev = pcie->pci->dev;
	unsigned int lane;
	int i;

	dev_info(dev, "SerDes configuration %s\n", pcie->serdes_cfg_ver);

	for (lane = 0; lane < pcie->lanes; lane++) {
		if (pcie->cfg_cnt[lane] <= 0 || !pcie->cfg[lane]) {
			dev_warn(dev, "lane %u has no SerDes configuration\n",
				 lane);
			continue;
		}

		for (i = 0; i < pcie->cfg_cnt[lane]; i++) {
			u32 off = pcie->cfg[lane][i].addr;

			if (off + sizeof(u32) <= pcie->serdes_size &&
			    IS_ALIGNED(off, sizeof(u32)))
				writel(pcie->cfg[lane][i].val,
				       pcie->serdes_base + off);
		}
	}
}

static bool taurus_pcie_serdes_wait_ber(struct taurus_pcie *pcie)
{
	unsigned int cnt;
	u32 val;

	for (cnt = 0; cnt < MAX_BER_POLL_COUNT; cnt++) {
		val = readl(pcie->serdes_base + 0x007c) &
		      readl(pcie->serdes_base + 0x017c);
		if (pcie->lanes == 2)
			val &= readl(pcie->serdes_base + 0x107c) &
			       readl(pcie->serdes_base + 0x117c);
		if (val & BIT(0))
			return true;
		usleep_range(1000, 2000);
	}

	return false;
}

static u32 taurus_pcie_readl(struct taurus_pcie *pcie, u32 reg)
{
	return readl(pcie->reg_base + reg);
}

static void taurus_pcie_writel(struct taurus_pcie *pcie, u32 val, u32 reg)
{
	writel(val, pcie->reg_base + reg);
}

static int taurus_pcie_device_reset(struct taurus_pcie *pcie)
{
	if (pcie->device_reset)
		return reset_control_reset(pcie->device_reset);

	if (pcie->perst) {
		gpiod_set_value_cansleep(pcie->perst, 1);
		msleep(PCIE_T_PVPERL_MS);
		gpiod_set_value_cansleep(pcie->perst, 0);
		return 0;
	}

	return -ENODEV;
}

static void taurus_pcie_serdes_toggle(struct taurus_pcie *pcie)
{
	writel(0x500c, pcie->serdes_base + 0x0024);
	usleep_range(10, 20);
	writel(0x520c, pcie->serdes_base + 0x0024);
	usleep_range(10, 20);
	writel(0x500c, pcie->serdes_base + 0x0124);
	usleep_range(10, 20);
	writel(0x520c, pcie->serdes_base + 0x0124);
	usleep_range(10, 20);

	if (pcie->lanes == 2) {
		writel(0x500c, pcie->serdes_base + 0x1024);
		usleep_range(10, 20);
		writel(0x520c, pcie->serdes_base + 0x1024);
		usleep_range(10, 20);
		writel(0x500c, pcie->serdes_base + 0x1124);
		usleep_range(10, 20);
		writel(0x520c, pcie->serdes_base + 0x1124);
		usleep_range(10, 20);
	}
}

static int taurus_pcie_phy_power_on(struct taurus_pcie *pcie)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pcie->lanes; i++) {
		ret = phy_power_on(pcie->phy[i]);
		if (ret) {
			while (i--)
				phy_power_off(pcie->phy[i]);
			return ret;
		}
	}

	pcie->phy_on = true;

	return 0;
}

static void taurus_pcie_phy_power_off(struct taurus_pcie *pcie)
{
	unsigned int i;

	if (!pcie->phy_on)
		return;

	for (i = 0; i < pcie->lanes; i++)
		phy_power_off(pcie->phy[i]);

	pcie->phy_on = false;
}

static int taurus_pcie_serdes_wait_cal(struct taurus_pcie *pcie)
{
	u32 val;

	return readl_poll_timeout(pcie->serdes_base + 0x007c, val,
				  val & BIT(4), 10, 100000);
}

static int taurus_pcie_serdes_calibrate(struct taurus_pcie *pcie)
{
	int ret;
	u32 val, reg;
	unsigned int lane, addr;

	if (pcie->idx != 0)
		return 0;

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		writel(0x500c, pcie->serdes_base + addr + 0x0024);
		usleep_range(10, 20);
		writel(0x520c, pcie->serdes_base + addr + 0x0024);
		usleep_range(10, 20);
		writel(0x500c, pcie->serdes_base + addr + 0x0124);
		usleep_range(10, 20);
		writel(0x520c, pcie->serdes_base + addr + 0x0124);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x0034);
		val |= BIT(6);
		writel(val, pcie->serdes_base + addr + 0x0034);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x0134);
		val |= BIT(6);
		writel(val, pcie->serdes_base + addr + 0x0134);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x0008);
		val |= BIT(9);
		writel(val, pcie->serdes_base + addr + 0x0008);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x0108);
		val |= BIT(9);
		writel(val, pcie->serdes_base + addr + 0x0108);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		writel(0x194, pcie->serdes_base + addr + 0x0040);
		usleep_range(10, 20);
		writel(0x194, pcie->serdes_base + addr + 0x0140);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x00bc);
		val |= BIT(10);
		writel(val, pcie->serdes_base + addr + 0x00bc);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x01bc);
		val |= BIT(10);
		writel(val, pcie->serdes_base + addr + 0x01bc);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x006c);
		val &= ~GENMASK(6, 5);
		writel(val, pcie->serdes_base + addr + 0x006c);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x016c);
		val &= ~GENMASK(6, 5);
		writel(val, pcie->serdes_base + addr + 0x016c);
		usleep_range(10, 20);
	}

	ret = taurus_pcie_serdes_wait_cal(pcie);
	if (ret)
		return ret;

	if (pcie->lanes == 2) {
		for (lane = 0; lane < pcie->lanes; lane++) {
			addr = lane * SERDES_LANE_STRIDE;
			val = readl(pcie->serdes_base + addr + 0x006c);
			val &= ~GENMASK(6, 5);
			val |= BIT(5);
			writel(val, pcie->serdes_base + addr + 0x006c);
			usleep_range(10, 20);
			val = readl(pcie->serdes_base + addr + 0x016c);
			val &= ~GENMASK(6, 5);
			val |= BIT(5);
			writel(val, pcie->serdes_base + addr + 0x016c);
			usleep_range(10, 20);
		}

		ret = taurus_pcie_serdes_wait_cal(pcie);
		if (ret)
			return ret;
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x0008);
		val &= ~BIT(9);
		writel(val, pcie->serdes_base + addr + 0x0008);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x0108);
		val &= ~BIT(9);
		writel(val, pcie->serdes_base + addr + 0x0108);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x006c);
		val &= ~GENMASK(6, 5);
		writel(val, pcie->serdes_base + addr + 0x006c);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x016c);
		val &= ~GENMASK(6, 5);
		writel(val, pcie->serdes_base + addr + 0x016c);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		writel(0x1a4, pcie->serdes_base + addr + 0x0040);
		usleep_range(10, 20);
		writel(0x1a4, pcie->serdes_base + addr + 0x0140);
		usleep_range(10, 20);
	}

	reg = readl(pcie->serdes_base + 0x007c);
	val = FIELD_GET(GENMASK(4, 0), reg);
	reg = readl(pcie->serdes_base + 0x0c);
	reg &= ~GENMASK(5, 1);
	reg |= FIELD_PREP(GENMASK(5, 1), val);

	writel(reg, pcie->serdes_base + 0x0c);
	usleep_range(10, 20);
	reg = readl(pcie->serdes_base + 0x10c);
	reg &= ~GENMASK(5, 1);
	reg |= FIELD_PREP(GENMASK(5, 1), val);
	writel(reg, pcie->serdes_base + 0x10c);
	usleep_range(10, 20);

	if (pcie->lanes == 2) {
		for (lane = 0; lane < pcie->lanes; lane++) {
			addr = lane * SERDES_LANE_STRIDE;
			val = readl(pcie->serdes_base + addr + 0x006c);
			val &= ~GENMASK(6, 5);
			val |= BIT(5);
			writel(val, pcie->serdes_base + addr + 0x006c);
			usleep_range(10, 20);
			val = readl(pcie->serdes_base + addr + 0x016c);
			val &= ~GENMASK(6, 5);
			val |= BIT(5);
			writel(val, pcie->serdes_base + addr + 0x016c);
			usleep_range(10, 20);
		}

		reg = readl(pcie->serdes_base + 0x007c);
		val = FIELD_GET(GENMASK(4, 0), reg);
		reg = readl(pcie->serdes_base + 0x100c);
		reg &= ~GENMASK(5, 1);
		reg |= FIELD_PREP(GENMASK(5, 1), val);

		writel(reg, pcie->serdes_base + 0x100c);
		usleep_range(10, 20);
		reg = readl(pcie->serdes_base +  0x110c);
		reg &= ~GENMASK(5, 1);
		reg |= FIELD_PREP(GENMASK(5, 1), val);
		writel(reg, pcie->serdes_base + 0x110c);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		writel(0x501c, pcie->serdes_base + addr + 0x0024);
		usleep_range(10, 20);
		writel(0x521c, pcie->serdes_base + addr + 0x0024);
		usleep_range(10, 20);
		writel(0x501c, pcie->serdes_base + addr + 0x0124);
		usleep_range(10, 20);
		writel(0x521c, pcie->serdes_base + addr + 0x0124);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x0034);
		val &= ~BIT(6);
		writel(val, pcie->serdes_base + addr + 0x0034);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x0134);
		val &= ~BIT(6);
		writel(val, pcie->serdes_base + addr + 0x0134);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x00bc);
		val &= ~BIT(10);
		writel(val, pcie->serdes_base + addr + 0x00bc);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x01bc);
		val &= ~BIT(10);
		writel(val, pcie->serdes_base + addr + 0x01bc);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x006c);
		val &= ~GENMASK(6, 5);
		writel(val, pcie->serdes_base + addr + 0x006c);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x016c);
		val &= ~GENMASK(6, 5);
		writel(val, pcie->serdes_base + addr + 0x016c);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		writel(0xc, pcie->serdes_base + addr + 0x0040);
		usleep_range(10, 20);
		writel(0xc, pcie->serdes_base + addr + 0x0140);
		usleep_range(10, 20);
	}

	reg = readl(pcie->serdes_base + 0x007c);
	val = FIELD_GET(GENMASK(4, 1), reg);

	reg = readl(pcie->serdes_base + 0x2c);
	reg &= ~GENMASK(8, 5);
	reg |= FIELD_PREP(GENMASK(8, 5), val);
	writel(reg, pcie->serdes_base + 0x2c);
	usleep_range(10, 20);
	reg = readl(pcie->serdes_base + 0x12c);
	reg &= ~GENMASK(8, 5);
	reg |= FIELD_PREP(GENMASK(8, 5), val);
	writel(reg, pcie->serdes_base + 0x12c);
	usleep_range(10, 20);

	if (pcie->lanes == 2) {
		reg = readl(pcie->serdes_base + 0x107c);
		val = FIELD_GET(GENMASK(4, 1), reg);

		reg = readl(pcie->serdes_base + 0x102c);
		reg &= ~GENMASK(8, 5);
		reg |= FIELD_PREP(GENMASK(8, 5), val);
		writel(reg, pcie->serdes_base + 0x102c);
		usleep_range(10, 20);
		reg = readl(pcie->serdes_base + 0x112c);
		reg &= ~GENMASK(8, 5);
		reg |= FIELD_PREP(GENMASK(8, 5), val);
		writel(reg, pcie->serdes_base + 0x112c);
		usleep_range(10, 20);
	}

	for (lane = 0; lane < pcie->lanes; lane++) {
		addr = lane * SERDES_LANE_STRIDE;
		val = readl(pcie->serdes_base + addr + 0x0034);
		val &= ~BIT(13);
		writel(val, pcie->serdes_base + addr + 0x0034);
		usleep_range(10, 20);
		val = readl(pcie->serdes_base + addr + 0x0134);
		val &= ~BIT(13);
		writel(val, pcie->serdes_base + addr + 0x0134);
		usleep_range(10, 20);
	}

	taurus_pcie_phy_power_off(pcie);
	usleep_range(1000, 2000);

	reset_control_assert(pcie->phy_reset);
	usleep_range(1000, 2000);

	reset_control_assert(pcie->core_reset);
	usleep_range(1000, 2000);

	ret = taurus_pcie_phy_power_on(pcie);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	reset_control_deassert(pcie->phy_reset);
	usleep_range(1000, 2000);

	reset_control_deassert(pcie->core_reset);
	usleep_range(1000, 2000);

	return 0;
}

static void taurus_pcie_perst_assert(struct taurus_pcie *pcie)
{
	if (pcie->device_reset)
		reset_control_assert(pcie->device_reset);
	if (pcie->perst)
		gpiod_set_value_cansleep(pcie->perst, 1);
}

static void taurus_pcie_perst_deassert(struct taurus_pcie *pcie)
{
	if (pcie->device_reset)
		reset_control_deassert(pcie->device_reset);
	if (pcie->perst)
		gpiod_set_value_cansleep(pcie->perst, 0);
}

static int taurus_pcie_host_reset(struct taurus_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	int ret;

	taurus_pcie_perst_assert(pcie);

	reset_control_assert(pcie->core_reset);
	usleep_range(1000, 2000);

	reset_control_assert(pcie->phy_reset);
	usleep_range(1000, 2000);

	taurus_pcie_serdes_load_cfg(pcie);

	ret = taurus_pcie_phy_power_on(pcie);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	reset_control_deassert(pcie->phy_reset);
	usleep_range(1000, 2000);

	reset_control_deassert(pcie->core_reset);
	usleep_range(1000, 2000);

	if (pcie->rev_a) {
		taurus_pcie_serdes_toggle(pcie);
	} else {
		ret = taurus_pcie_serdes_calibrate(pcie);
		if (ret)
			return ret;
	}

	if (!taurus_pcie_serdes_wait_ber(pcie))
		dev_warn(pci->dev, "SerDes BER notification timed out\n");

	msleep(PCIE_T_PVPERL_MS);
	taurus_pcie_perst_deassert(pcie);

	return 0;
}

static int taurus_pcie_atu_write_ctrl(struct taurus_pcie *pcie, u32 idx,
				      bool enable)
{
	u32 ctrl, val;

	ctrl = readl(pcie->coherent_atu + TAURUS_ATU_CTRL);
	ctrl &= ~(TAURUS_ATU_CTRL_IDX_MASK | TAURUS_ATU_CTRL_R);
	if (enable)
		ctrl |= BIT(idx);
	else
		ctrl &= ~BIT(idx);
	ctrl |= FIELD_PREP(TAURUS_ATU_CTRL_IDX_MASK, idx) | TAURUS_ATU_CTRL_W;
	writel(ctrl, pcie->coherent_atu + TAURUS_ATU_CTRL);

	return readl_poll_timeout(pcie->coherent_atu + TAURUS_ATU_CTRL, val,
				  !(val & TAURUS_ATU_CTRL_W), 1, 100000);
}

static int taurus_pcie_atu_clear(struct taurus_pcie *pcie)
{
	u32 idx;
	int ret;

	if (!pcie->coherent_atu)
		return 0;

	for (idx = 0; idx < TAURUS_ATU_TLB_COUNT; idx++) {
		writel(0, pcie->coherent_atu + TAURUS_ATU_INPUT);
		writel(0, pcie->coherent_atu + TAURUS_ATU_MASK);
		writel(0, pcie->coherent_atu + TAURUS_ATU_OUTPUT);
		writel(0, pcie->coherent_atu + TAURUS_ATU_ACE_CFG);
		writel(0, pcie->coherent_atu + TAURUS_ATU_AXI_ARUSER);
		writel(0, pcie->coherent_atu + TAURUS_ATU_AXI_AWUSER);

		ret = taurus_pcie_atu_write_ctrl(pcie, idx, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int taurus_pcie_atu_set_entry(struct taurus_pcie *pcie, u32 idx,
				     u32 input, u32 pages, u32 output)
{
	writel(input, pcie->coherent_atu + TAURUS_ATU_INPUT);
	writel(~(pages - 1), pcie->coherent_atu + TAURUS_ATU_MASK);
	writel(output, pcie->coherent_atu + TAURUS_ATU_OUTPUT);
	writel(TAURUS_ATU_ACE_COHERENT_CFG,
	       pcie->coherent_atu + TAURUS_ATU_ACE_CFG);
	writel(TAURUS_ATU_AXUSER_OUTER_SHAREABLE,
	       pcie->coherent_atu + TAURUS_ATU_AXI_ARUSER);
	writel(TAURUS_ATU_AXUSER_OUTER_SHAREABLE,
	       pcie->coherent_atu + TAURUS_ATU_AXI_AWUSER);

	return taurus_pcie_atu_write_ctrl(pcie, idx, true);
}

static int taurus_pcie_atu_init(struct taurus_pcie *pcie)
{
	u32 hi_offset;
	int ret;

	if (!pcie->coherent_atu)
		return 0;

	ret = taurus_pcie_atu_clear(pcie);
	if (ret)
		return ret;

	if (!pcie->dma_coherent)
		return 0;

	hi_offset = pcie->coherent_hi_addr << 16;

	ret = taurus_pcie_atu_set_entry(pcie, 0, TAURUS_ATU_DDR_LOW_INPUT,
					TAURUS_ATU_DDR_WINDOW_PAGES, hi_offset);
	if (ret)
		return ret;

	ret = taurus_pcie_atu_set_entry(pcie, 1, TAURUS_ATU_DDR_HIGH_INPUT,
					TAURUS_ATU_DDR_WINDOW_PAGES,
					TAURUS_ATU_DDR_HIGH_INPUT + hi_offset);
	if (ret)
		return ret;

	dev_info(pcie->pci->dev, "coherent ATU enabled (hi_addr 0x%x)\n",
		 pcie->coherent_hi_addr);

	return 0;
}

static void taurus_pcie_host_setup(struct taurus_pcie *pcie)
{
	u32 base_addr = (u32)pcie->iatu_unroll_start & RC_IATU_BASE_ADDR_MASK;

	taurus_pcie_writel(pcie, pcie->dbi_start,
			    PCIE_GLBL_PCIE_CONTR_CFG_START_ADDR);
	taurus_pcie_writel(pcie, pcie->dbi_end,
			    PCIE_GLBL_PCIE_CONTR_CFG_END_ADDR);
	taurus_pcie_writel(pcie, base_addr,
			    PCIE_GLBL_PCIE_CONTR_IATU_BASE_ADDR);
}

static void taurus_pcie_stop_link(struct dw_pcie *pci)
{
	struct taurus_pcie *pcie = to_taurus_pcie(pci);

	taurus_pcie_writel(pcie, PCIE_LINK_DOWN_RST,
			    PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(100, 200);
	taurus_pcie_writel(pcie, 0, PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(100, 200);
}

static int taurus_pcie_start_link(struct dw_pcie *pci)
{
	struct taurus_pcie *pcie = to_taurus_pcie(pci);

	if (dw_pcie_link_up(pci))
		return 0;

	taurus_pcie_writel(pcie, PCIE_LTSSM_ENABLE,
			    PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(1000, 2000);

	return 0;
}

static void taurus_pcie_misc_enable(struct taurus_pcie *pcie)
{
	taurus_pcie_intr_update(pcie, 0,
				INT_HP_INT | INT_RADM_CORRECTABLE_ERR |
				INT_RADM_NONFATAL_ERR | INT_RADM_FATAL_ERR |
				INT_RADM_PM_TO_ACK | INT_RADM_PM_PME |
				INT_RADM_QOVERFLOW | INT_LINK_DOWN |
				INT_CFG_AER_RC_ERR_MSI | INT_CFG_PME_MSI |
				INT_HP_PME | INT_HP_MSI | INT_CFG_UR_RESP);
}

static void taurus_pcie_msi_enable(struct taurus_pcie *pcie)
{
	if (!pci_msi_enabled())
		return;

	taurus_pcie_writel(pcie, 0xffffffff, PCIE_MSI_INTERRUPT_ENABLE_0);
	taurus_pcie_intr_update(pcie, 0, INT_MSI_CTR_INT);
}

static void taurus_pcie_enable_interrupts(struct taurus_pcie *pcie)
{
	taurus_pcie_misc_enable(pcie);
	taurus_pcie_msi_enable(pcie);
}

static irqreturn_t taurus_pcie_irq_handler(int irq, void *arg)
{
	struct taurus_pcie *pcie = arg;
	struct dw_pcie_rp *pp = &pcie->pci->pp;
	u32 val;

	val = taurus_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_0);
	if (!val)
		return IRQ_NONE;

	if ((val & INT_MSI_CTR_INT) && pp->irq_domain)
		dw_handle_msi_irq(pp);

	if (pcie->intx_domain) {
		if (val & INT_RADM_INTA_ASSERTED)
			generic_handle_domain_irq(pcie->intx_domain, 0);
		if (val & INT_RADM_INTB_ASSERTED)
			generic_handle_domain_irq(pcie->intx_domain, 1);
		if (val & INT_RADM_INTC_ASSERTED)
			generic_handle_domain_irq(pcie->intx_domain, 2);
		if (val & INT_RADM_INTD_ASSERTED)
			generic_handle_domain_irq(pcie->intx_domain, 3);
	}
	if (val & INT_LINK_DOWN) {
		unsigned int ltssm = taurus_pcie_ltssm(pcie);

		dev_err(pcie->pci->dev, "link down, LTSSM %s\n",
			taurus_pcie_ltssm_name(ltssm));
		taurus_pcie_writel(pcie, PCIE_LINK_DOWN_RST,
				    PCIE_GLBL_CORE_CONFIG_REG);
	}

	taurus_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_0);

	return IRQ_HANDLED;
}

static unsigned int taurus_pcie_ltssm(struct taurus_pcie *pcie)
{
	u32 val;

	val = taurus_pcie_readl(pcie, PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS);

	return FIELD_GET(PWR_STATUS_SMLH_LTSSM_STATE_MASK, val);
}

static void taurus_pcie_link_status(struct dw_pcie *pci, u8 *speed, u8 *lanes)
{
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u16 status = dw_pcie_readw_dbi(pci, offset + PCI_EXP_LNKSTA);

	*speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status);
	*lanes = FIELD_GET(PCI_EXP_LNKSTA_NLW, status);
}

static int taurus_pcie_intx_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct taurus_pcie *pcie = to_taurus_pcie(pci);
	struct device *dev = pci->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node;

	pcie_intc_node = of_get_child_by_name(node, "interrupt-controller");
	if (!pcie_intc_node)
		return dev_err_probe(dev, -ENODEV,
				     "missing interrupt-controller node\n");

	pcie->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node),
						     MAX_INTX_HOST_IRQS,
						     &taurus_pcie_intx_domain_ops,
						     pp);
	of_node_put(pcie_intc_node);
	if (!pcie->intx_domain)
		return dev_err_probe(dev, -ENOMEM, "failed to create INTx domain\n");

	return 0;
}

static int taurus_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct taurus_pcie *pcie = to_taurus_pcie(pci);
	int ret;

	taurus_pcie_writel(pcie, 0, PCIE_GLBL_INTERRUPT_ENABLE_0);
	taurus_pcie_writel(pcie,
			   taurus_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_0),
			   PCIE_GLBL_INTERRUPT_0);

	ret = taurus_pcie_host_reset(pcie);
	if (ret)
		return ret;

	ret = taurus_pcie_atu_init(pcie);
	if (ret)
		return ret;

	taurus_pcie_host_setup(pcie);

	return taurus_pcie_intx_host_init(pp);
}

static void taurus_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct taurus_pcie *pcie = to_taurus_pcie(pci);

	if (dw_pcie_link_up(pci)) {
		u8 speed, lanes;

		taurus_pcie_link_status(pci, &speed, &lanes);
		dev_info(pci->dev, "link up: Gen%u, x%u lanes\n", speed, lanes);
	} else {
		dev_err(pci->dev, "link down, LTSSM %s\n",
			taurus_pcie_ltssm_name(taurus_pcie_ltssm(pcie)));
	}

	taurus_pcie_enable_interrupts(pcie);
}

static void taurus_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct taurus_pcie *pcie = to_taurus_pcie(pci);

	taurus_pcie_writel(pcie, 0, PCIE_GLBL_INTERRUPT_ENABLE_0);
	if (pp->irq > 0)
		devm_free_irq(pci->dev, pp->irq, pcie);
	taurus_pcie_perst_assert(pcie);
	taurus_pcie_atu_clear(pcie);
	reset_control_assert(pcie->core_reset);
	reset_control_assert(pcie->phy_reset);
	taurus_pcie_phy_power_off(pcie);

	if (pcie->intx_domain) {
		irq_domain_remove(pcie->intx_domain);
		pcie->intx_domain = NULL;
	}
}

static const struct dw_pcie_host_ops taurus_pcie_host_ops = {
	.init = taurus_pcie_host_init,
	.post_init = taurus_pcie_host_post_init,
	.deinit = taurus_pcie_host_deinit,
};

static bool taurus_pcie_link_up(struct dw_pcie *pci)
{
	struct taurus_pcie *pcie = to_taurus_pcie(pci);
	u32 val;

	val = taurus_pcie_readl(pcie, PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS);

	return !!(val & PWR_STATUS_RDLH_LINK_UP);
}

static const struct dw_pcie_ops taurus_pcie_ops = {
	.link_up = taurus_pcie_link_up,
	.start_link = taurus_pcie_start_link,
	.stop_link = taurus_pcie_stop_link,
};

static int taurus_pcie_add_port(struct taurus_pcie *pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = pci->dev;
	struct resource *res;
	int ret;

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0)
		return pp->irq;

	taurus_pcie_writel(pcie, 0, PCIE_GLBL_INTERRUPT_ENABLE_0);

	ret = devm_request_irq(dev, pp->irq, taurus_pcie_irq_handler,
			       IRQF_NO_THREAD, dev_name(dev), pcie);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbi");
	if (!res)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	if (!res)
		return dev_err_probe(dev, -ENODEV, "missing DBI resource\n");

	pci->dbi_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);
	pci->dbi_phys_addr = res->start;

	dev_dbg(dev, "DBI %pR\n", res);

	pcie->dbi_start = res->start;
	pcie->dbi_end = res->end;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iatu");
	if (!res)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "atu");
	if (!res)
		return dev_err_probe(dev, -ENODEV, "missing iATU resource\n");

	pcie->iatu_unroll_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->iatu_unroll_base))
		return PTR_ERR(pcie->iatu_unroll_base);

	pci->atu_base = pcie->iatu_unroll_base;
	pci->atu_size = resource_size(res);
	pci->atu_phys_addr = res->start;
	pcie->iatu_unroll_start = res->start;

	pp->ops = &taurus_pcie_host_ops;
	pp->msi_irq[0] = -ENODEV;

	ret = dw_pcie_host_init(pp);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize host\n");

	if (pcie->irq_affinity >= 0 &&
	    irq_set_affinity(pp->irq, cpumask_of(pcie->irq_affinity)))
		dev_warn(dev, "failed to set IRQ affinity to CPU %d\n",
			 pcie->irq_affinity);

	return 0;
}

enum taurus_pcie_rev {
	TAURUS_PCIE_REV_A,
	TAURUS_PCIE_REV_B,
};

static const char * const serdes_cfg_ver_prop[] = {
	"serdes-cfg-verA",
	"serdes-cfg-verB",
};

static const char * const serdes_cfg_data_prop[] = {
	"serdes-cfg-dataA",
	"serdes-cfg-dataB",
};

static int taurus_pcie_serdes_parse_lane(struct device *dev,
					  struct device_node *np,
					  struct taurus_pcie *pcie,
					  const char *prop,
					  unsigned int lane,
					  bool per_lane_prop)
{
	struct serdes_cfg *cfg;
	int ncells, ncfg, i, ret;

	ncells = of_property_count_u32_elems(np, prop);
	if (ncells <= 0)
		return 0;
	if (ncells % 2)
		return dev_err_probe(dev, -EINVAL,
				     "%s: odd number of cells\n", prop);

	ncfg = ncells / 2;
	cfg = devm_kcalloc(dev, ncfg, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	for (i = 0; i < ncfg; i++) {
		ret = of_property_read_u32_index(np, prop, 2 * i,
						 &cfg[i].addr);
		if (ret)
			return ret;
		ret = of_property_read_u32_index(np, prop, 2 * i + 1,
						 &cfg[i].val);
		if (ret)
			return ret;

		if (!per_lane_prop)
			cfg[i].addr += lane * SERDES_LANE_STRIDE;
	}

	pcie->cfg[lane] = cfg;
	pcie->cfg_cnt[lane] = ncfg;

	return 0;
}

static int taurus_pcie_serdes_parse_rev(struct device *dev,
					 struct device_node *np,
					 struct taurus_pcie *pcie)
{
	enum taurus_pcie_rev rev = pcie->rev_a ? TAURUS_PCIE_REV_A :
						  TAURUS_PCIE_REV_B;
	unsigned int lane;
	int ret;

	ret = of_property_read_string(np, serdes_cfg_ver_prop[rev],
				      &pcie->serdes_cfg_ver);
	if (ret)
		pcie->serdes_cfg_ver = TAURUS_PCIE_SERDES_CFG_VER_UNKNOWN;

	dev_info(dev, "Chip revision %c, SerDes configuration %s\n",
		 pcie->rev_a ? 'A' : 'B', pcie->serdes_cfg_ver);

	for (lane = 0; lane < pcie->lanes; lane++) {
		ret = taurus_pcie_serdes_parse_lane(dev, np, pcie,
						     serdes_cfg_data_prop[rev],
						     lane, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int taurus_pcie_serdes_parse(struct device *dev,
				     struct device_node *np,
				     struct taurus_pcie *pcie)
{
	const char *ver;
	unsigned int lane;
	char name[MAX_NAME_LEN];
	int ret;

	if (of_property_read_string(np, "serdes-cfg-ver", &ver))
		return taurus_pcie_serdes_parse_rev(dev, np, pcie);

	pcie->serdes_cfg_ver = ver;

	for (lane = 0; lane < pcie->lanes; lane++) {
		snprintf(name, sizeof(name), "serdes-cfg%u", lane);
		ret = taurus_pcie_serdes_parse_lane(dev, np, pcie, name,
						     lane, true);
		if (ret)
			return ret;
	}

	return 0;
}

static void taurus_pcie_get_affinity(struct device *dev,
				      struct taurus_pcie *pcie)
{
	u32 cpu;

	if (of_property_read_u32(dev->of_node, "irq-affinity", &cpu) ||
	    cpu >= nr_cpu_ids) {
		pcie->irq_affinity = -1;
		return;
	}

	pcie->irq_affinity = cpu;
	dev_info(dev, "IRQ affinity set to CPU %u\n", cpu);
}

static int taurus_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct taurus_soc_data soc;
	struct taurus_pcie *pcie;
	struct dw_pcie *pci;
	struct resource *res;
	struct clk *clk;
	unsigned int i;
	int lanes, ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &taurus_pcie_ops;
	pcie->pci = pci;
	raw_spin_lock_init(&pcie->intr_lock);
	platform_set_drvdata(pdev, pcie);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "failed to enable bus clock\n");

	ret = taurus_soc_data_get(&soc);
	if (ret)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "SoC data not available\n");

	pcie->rev_a = soc.chip_revision == 'A';

	ret = of_property_read_u32(np, "id", &pcie->idx);
	if (ret)
		return dev_err_probe(dev, ret, "missing id property\n");

	ret = of_property_read_u32(np, "num-lanes", &lanes);
	if (ret || lanes < 1 || lanes > MAX_LANE_NUM)
		pcie->lanes = 1;
	else
		pcie->lanes = lanes;

	pcie->reg_base = devm_platform_ioremap_resource_byname(pdev, "glbl_regs");
	if (IS_ERR(pcie->reg_base))
		return PTR_ERR(pcie->reg_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "serdes_phy");
	if (!res)
		return dev_err_probe(dev, -ENODEV, "missing SerDes resource\n");

	pcie->serdes_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->serdes_base))
		return PTR_ERR(pcie->serdes_base);
	pcie->serdes_size = resource_size(res);

	pcie->dma_coherent = of_dma_is_coherent(np);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "coherent-atu");
	if (res) {
		pcie->coherent_atu = devm_ioremap_resource(dev, res);
		if (IS_ERR(pcie->coherent_atu))
			return PTR_ERR(pcie->coherent_atu);
	} else if (pcie->dma_coherent) {
		return dev_err_probe(dev, -EINVAL,
				     "dma-coherent requires coherent-atu resource\n");
	}

	if (of_property_read_u32(np, "realtek,coherent-hi-addr",
				 &pcie->coherent_hi_addr))
		pcie->coherent_hi_addr = TAURUS_ATU_DEFAULT_HI_ADDR;

	ret = taurus_pcie_serdes_parse(dev, np, pcie);
	if (ret)
		return ret;

	pcie->core_reset = devm_reset_control_get_exclusive(dev, "core_reset");
	if (IS_ERR(pcie->core_reset))
		return dev_err_probe(dev, PTR_ERR(pcie->core_reset),
				     "failed to get core_reset\n");

	pcie->phy_reset = devm_reset_control_get_exclusive(dev, "phy_reset");
	if (IS_ERR(pcie->phy_reset))
		return dev_err_probe(dev, PTR_ERR(pcie->phy_reset),
				     "failed to get phy_reset\n");

	pcie->device_reset = devm_reset_control_get_optional_exclusive(dev,
								       "device_reset");
	if (IS_ERR(pcie->device_reset))
		return dev_err_probe(dev, PTR_ERR(pcie->device_reset),
				     "failed to get device_reset\n");

	pcie->device_power = devm_reset_control_get_optional_exclusive(dev,
								       "device_power");
	if (IS_ERR(pcie->device_power))
		return dev_err_probe(dev, PTR_ERR(pcie->device_power),
				     "failed to get device_power\n");
	if (pcie->device_power)
		reset_control_reset(pcie->device_power);

	pcie->perst = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pcie->perst))
		return dev_err_probe(dev, PTR_ERR(pcie->perst),
				     "failed to get PERST GPIO\n");

	for (i = 0; i < pcie->lanes; i++) {
		char name[MAX_NAME_LEN];

		snprintf(name, sizeof(name), "pcie-phy%u", i);
		pcie->phy[i] = devm_phy_get(dev, name);
		if (IS_ERR(pcie->phy[i]))
			return dev_err_probe(dev, PTR_ERR(pcie->phy[i]),
					     "failed to get %s\n", name);
	}

	taurus_pcie_get_affinity(dev, pcie);

	if (of_property_read_bool(np, "forced-gen1"))
		pci->max_link_speed = 1;

	ret = devm_device_add_group(dev, &taurus_pcie_attr_group);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create sysfs group\n");

	ret = taurus_pcie_add_port(pcie, pdev);
	if (ret) {
		taurus_pcie_phy_power_off(pcie);
		return ret;
	}

	return 0;
}

static void taurus_pcie_remove(struct platform_device *pdev)
{
	struct taurus_pcie *pcie = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&pcie->pci->pp);
}

static const struct of_device_id taurus_pcie_of_match[] = {
	{ .compatible = "realtek,taurus-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_pcie_of_match);

static struct platform_driver taurus_pcie_driver = {
	.probe		= taurus_pcie_probe,
	.remove		= taurus_pcie_remove,
	.driver = {
		.name = "taurus-pcie",
		.of_match_table = taurus_pcie_of_match,
	},
};

static int __init taurus_pcie_init(void)
{
	return platform_driver_register(&taurus_pcie_driver);
}
late_initcall(taurus_pcie_init);

static void __exit taurus_pcie_exit(void)
{
	platform_driver_unregister(&taurus_pcie_driver);
}
module_exit(taurus_pcie_exit);

MODULE_DESCRIPTION("Realtek Taurus PCIe host controller driver");
MODULE_LICENSE("GPL");
