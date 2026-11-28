// SPDX-License-Identifier: GPL-2.0
#include <linux/arm-smccc.h>
#include <linux/dma-map-ops.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>

#include <linux/soc/realtek/taurus-soc.h>

#define TAURUS_GLOBAL_BASE		0xf4320000
#define TAURUS_GLOBAL_SIZE		0x1000

#define TAURUS_REG_STRAP		0x0ac
#define TAURUS_REG_GLOBAL_CONFIG	0x0c0
#define TAURUS_REG_GLOBAL_SOFTWARE	0x194
#define TAURUS_REG_CPLLDIV		0x200
#define TAURUS_REG_EPLLDIV		0x208
#define TAURUS_REG_EPLLDIV2		0x20c
#define TAURUS_REG_PEDIV		0x210
#define TAURUS_REG_CPLL0		0x230
#define TAURUS_REG_EPLL0		0x238
#define TAURUS_REG_FPLL0		0x240

#define TAURUS_CHIP_ID_CA8277C		0x6706
#define TAURUS_CHIP_ID_GET(v)		(((v) >> 16) & 0xffff)
#define TAURUS_VENDOR_ID_GET(v)		((v) & 0xfff)

#define TAURUS_STRAP_SPEED_SHIFT	1
#define TAURUS_STRAP_SPEED_WIDTH	3

#define TAURUS_SVC_OTP_CHIP_MODE_READ	0x82000a0b
#define TAURUS_SVC_ROM_VERSION		0x82001106

#define MHZ	(1000000)

struct m_id {
	int id;
	const char *name;
};

static const struct m_id taurus_m_id_table[] = {
	{ 0x000, "CA8277C" },
	{ 0x030, "RTL9615C" },
	{ 0x020, "RTL9617C" },
	{ 0x010, "RTL9617C-VW" },
	{ 0x040, "RTL9607DQ" },
	{ 0x080, "RTL8198XB" },
	{ 0x100, "RTL9619" },
	{ 0x110, "RTL9619C" },
	{ 0x210, "RTL9607DM" },
	{ 0x220, "RTL9607DS" },
	{ 0x240, "RTL9617CM" },
	{ 0x280, "RTL9607DM" }
};

static const char *taurus_id_to_name(int id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(taurus_m_id_table); i++) {
		if (id == taurus_m_id_table[i].id)
			return taurus_m_id_table[i].name;
	}
	return "<Unknown>";
}

struct taurus_soc_priv {
	struct taurus_soc_data soc;
	u32 cpll;
	u32 epll;
	u32 fpll;
	u32 cpu_clk;
	u32 cci_clk;
	u32 lsaxi_clk;
	u32 hsaxi_clk;
	u32 core_clk;
	u32 pe_clk;
	u32 peaxi_clk;
};

static struct taurus_soc_priv *glb_soc;

static int taurus_smc_buff_read(u32 smc_id, void *buf, size_t len)
{
	size_t alloc_len = max_t(size_t, len, ARCH_DMA_MINALIGN);
	void *tmp;
	phys_addr_t phyaddr;

	tmp = kzalloc(alloc_len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	phyaddr = virt_to_phys(tmp);
	arch_sync_dma_for_device(phyaddr, alloc_len, DMA_FROM_DEVICE);
	arm_smccc_smc(smc_id, 0, 0, phyaddr, 0, 0, 0, 0,
		      &(struct arm_smccc_res){0});
	arch_sync_dma_for_cpu(phyaddr, alloc_len, DMA_FROM_DEVICE);
	memcpy(buf, tmp, len);
	kfree(tmp);
	return 0;
}

static void taurus_clock_probe(struct taurus_soc_priv *soc, void __iomem *base)
{
	u32 ref_clk = 25 * MHZ;
	u32 strap, cpll0, epll0, fpll0, cplldiv, eplldiv, eplldiv2, pediv;

	strap = readl(base + TAURUS_REG_STRAP);
	cpll0 = readl(base + TAURUS_REG_CPLL0);
	epll0 = readl(base + TAURUS_REG_EPLL0);
	fpll0 = readl(base + TAURUS_REG_FPLL0);
	cplldiv = readl(base + TAURUS_REG_CPLLDIV);
	eplldiv = readl(base + TAURUS_REG_EPLLDIV);
	eplldiv2 = readl(base + TAURUS_REG_EPLLDIV2);
	pediv = readl(base + TAURUS_REG_PEDIV);

	if (cpll0 & BIT(14))
		soc->cpll = ref_clk * ((cpll0 & 0xff) + 3);
	else
		soc->cpll = ref_clk / (((cpll0 >> 12) & 0x3) + 2) *
			    ((cpll0 & 0xff) + 3);

	if (epll0 & BIT(14))
		soc->epll = ref_clk * ((epll0 & 0xff) + 3);
	else
		soc->epll = ref_clk / (((epll0 >> 12) & 0x3) + 2) *
			    ((epll0 & 0xff) + 3);

	if (fpll0 & BIT(14))
		soc->fpll = ref_clk * ((fpll0 & 0xff) + 3);
	else
		soc->fpll = ref_clk / (((fpll0 >> 12) & 0x3) + 2) *
			    ((fpll0 & 0xff) + 3);

	if (cplldiv & BIT(6)) {
		if (cplldiv & BIT(11)) {
			if ((cplldiv >> 7) & 0xf)
				soc->cpu_clk = soc->fpll * 2 / ((cplldiv >> 7) & 0xf);
			else
				soc->cpu_clk = soc->fpll;
		} else {
			if ((cplldiv & 0x3f))
				soc->cpu_clk = soc->cpll * 2 / (cplldiv & 0x3f);
			else
				soc->cpu_clk = soc->cpll;
		}
	} else {
		switch ((strap >> TAURUS_STRAP_SPEED_SHIFT) &
			((1 << TAURUS_STRAP_SPEED_WIDTH) - 1)) {
		case 0: soc->cpu_clk = 400 * MHZ; break;
		case 1: soc->cpu_clk = 625 * MHZ; break;
		case 2: soc->cpu_clk = 700 * MHZ; break;
		case 3: soc->cpu_clk = 800 * MHZ; break;
		case 4: soc->cpu_clk = 900 * MHZ; break;
		case 5: soc->cpu_clk = 1000 * MHZ; break;
		case 6: soc->cpu_clk = 1100 * MHZ; break;
		case 7: soc->cpu_clk = 1200 * MHZ; break;
		}
	}

	{
		u32 cci_div = ((eplldiv2 >> 8) & 0x7f) / 2;

		soc->cci_clk = cci_div ? soc->epll / cci_div : soc->epll;
	}

	if ((eplldiv >> 24) & 0x7f)
		soc->lsaxi_clk = soc->epll * 2 / ((eplldiv >> 24) & 0x7f);
	else
		soc->lsaxi_clk = soc->epll;
	soc->hsaxi_clk = soc->lsaxi_clk << 1;

	if ((eplldiv >> 8) & 0x7f)
		soc->core_clk = soc->epll * 2 / ((eplldiv >> 8) & 0x7f);
	else
		soc->core_clk = soc->epll;

	if (pediv & BIT(6)) {
		if ((pediv >> 8) & 0x3f)
			soc->pe_clk = soc->cpll * 2 / ((pediv >> 8) & 0x3f);
		else
			soc->pe_clk = soc->cpll;
	} else {
		if (pediv & 0x3f)
			soc->pe_clk = soc->fpll * 2 / (pediv & 0x3f);
		else
			soc->pe_clk = soc->fpll;
	}

	if (pediv & BIT(22))
		soc->peaxi_clk = soc->pe_clk;
	else
		soc->peaxi_clk = soc->pe_clk / 2;
}

static void taurus_soc_register(struct taurus_soc_priv *soc)
{
	struct soc_device_attribute *attr;
	u16 id;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return;

	id = soc->soc.chipmode & 0xfff0;
	attr->family = "Taurus";
	attr->soc_id = kasprintf(GFP_KERNEL, "0x%x", id);
	attr->machine = taurus_id_to_name(id);
	attr->revision = kasprintf(GFP_KERNEL, "%c", soc->soc.chip_revision);

	soc_device_register(attr);
}

static int taurus_soc_probe(struct platform_device *pdev)
{
	struct taurus_soc_priv *soc;
	struct resource *mem;
	struct arm_smccc_res res;
	void __iomem *base;
	u32 v;
	int rev;

	soc = devm_kzalloc(&pdev->dev, sizeof(*soc), GFP_KERNEL);
	if (!soc)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -EINVAL;

	base = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (!base)
		return -ENOMEM;

	v = readl(base);
	if (TAURUS_CHIP_ID_GET(v) != TAURUS_CHIP_ID_CA8277C)
		return -ENODEV;

	soc->soc.chip_id = TAURUS_CHIP_ID_GET(v);
	soc->soc.vendor_id = TAURUS_VENDOR_ID_GET(v);

	taurus_smc_buff_read(TAURUS_SVC_OTP_CHIP_MODE_READ, &soc->soc.chipmode,
			     sizeof(soc->soc.chipmode));

	arm_smccc_smc(TAURUS_SVC_ROM_VERSION, 0x12345678, 0, 0, 0, 0, 0, 0,
		      &res);
	rev = (int)res.a0;
	if (rev < 'A' || rev == -1)
		rev = 'A';
	soc->soc.chip_revision = (u8)rev;

	taurus_clock_probe(soc, base);

	glb_soc = soc;
	platform_set_drvdata(pdev, soc);

	dev_info(&pdev->dev,
		 "vendor 0x%03x chip 0x%04x rev %c mode 0x%02x cpll %u.%02u epll %u.%02u fpll %u.%02u cpu %u.%02u core %u.%02u\n",
		 soc->soc.vendor_id, soc->soc.chip_id, soc->soc.chip_revision,
		 soc->soc.chipmode,
		 soc->cpll / MHZ, (soc->cpll / 10000) % 100,
		 soc->epll / MHZ, (soc->epll / 10000) % 100,
		 soc->fpll / MHZ, (soc->fpll / 10000) % 100,
		 soc->cpu_clk / MHZ, (soc->cpu_clk / 10000) % 100,
		 soc->core_clk / MHZ, (soc->core_clk / 10000) % 100);

	taurus_soc_register(soc);

	return 0;
}

int taurus_soc_data_get(struct taurus_soc_data *data)
{
	if (!glb_soc || !data)
		return -EINVAL;

	memcpy(data, &glb_soc->soc, sizeof(*data));
	return 0;
}
EXPORT_SYMBOL(taurus_soc_data_get);

unsigned int taurus_soc_refclk_khz(void)
{
	return 25000;
}
EXPORT_SYMBOL(taurus_soc_refclk_khz);

static const struct of_device_id taurus_soc_of_match[] = {
	{ .compatible = "realtek,taurus-soc" },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_soc_of_match);

static struct platform_driver taurus_soc_driver = {
	.probe = taurus_soc_probe,
	.driver = {
		.name = "taurus-soc",
		.of_match_table = taurus_soc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(taurus_soc_driver);

MODULE_DESCRIPTION("Realtek Taurus SoC identification driver");
MODULE_LICENSE("GPL");
