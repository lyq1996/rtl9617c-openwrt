// SPDX-License-Identifier: GPL-2.0
#include <linux/arm-smccc.h>
#include <linux/dma-map-ops.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define TAURUS_OTP_SIZE		(1 << 10)
#define TAURUS_SVC_OTP_GPHY_PATCH_READ	0x82000a0c

struct taurus_otp_priv {
	u8 *buf;
	size_t size;
};

static int taurus_otp_reg_read(void *context, unsigned int reg, void *_val,
			       size_t bytes)
{
	struct taurus_otp_priv *priv = context;
	u8 *val = _val;

	if (reg + bytes > priv->size)
		return -EINVAL;

	memcpy(val, priv->buf + reg, bytes);
	return 0;
}

static int taurus_otp_reg_write(void *context, unsigned int reg, void *_val,
				size_t bytes)
{
	return -EOPNOTSUPP;
}

static const struct of_device_id taurus_otp_of_match[] = {
	{ .compatible = "realtek,taurus-otp" },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_otp_of_match);

static int taurus_otp_probe(struct platform_device *pdev)
{
	struct taurus_otp_priv *priv;
	struct nvmem_device *nvmem;
	struct resource *res;
	struct nvmem_config econfig = {
		.name = "taurus-otp",
		.owner = THIS_MODULE,
		.read_only = true,
		.stride = 1,
		.word_size = 1,
		.reg_read = taurus_otp_reg_read,
		.reg_write = taurus_otp_reg_write,
	};
	struct arm_smccc_res smc_res;
	u8 *otp_buf;
	u32 otp_offset;
	phys_addr_t phyaddr;
	void *smc_buf;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	otp_offset = res->start & (TAURUS_OTP_SIZE - 1);
	if (resource_size(res) > TAURUS_OTP_SIZE ||
	    otp_offset > TAURUS_OTP_SIZE - resource_size(res)) {
		dev_err(&pdev->dev, "OTP region out of bounds (off=0x%x size=%pa)\n",
			otp_offset, &res->end);
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	smc_buf = kzalloc(TAURUS_OTP_SIZE, GFP_KERNEL);
	if (!smc_buf)
		return -ENOMEM;

	phyaddr = virt_to_phys(smc_buf);
	arch_sync_dma_for_device(phyaddr, TAURUS_OTP_SIZE, DMA_FROM_DEVICE);
	arm_smccc_smc(TAURUS_SVC_OTP_GPHY_PATCH_READ, 0, 0, phyaddr, 0, 0, 0, 0,
		      &smc_res);
	arch_sync_dma_for_cpu(phyaddr, TAURUS_OTP_SIZE, DMA_FROM_DEVICE);
	if (smc_res.a0) {
		dev_err(&pdev->dev, "OTP read via SMC failed: 0x%lx\n",
			smc_res.a0);
		kfree(smc_buf);
		return -EIO;
	}

	otp_buf = devm_kmalloc(&pdev->dev, resource_size(res), GFP_KERNEL);
	if (!otp_buf) {
		kfree(smc_buf);
		return -ENOMEM;
	}

	memcpy(otp_buf, smc_buf + otp_offset, resource_size(res));
	kfree(smc_buf);

	priv->buf = otp_buf;
	priv->size = resource_size(res);

	econfig.size = priv->size;
	econfig.dev = &pdev->dev;
	econfig.priv = priv;

	nvmem = devm_nvmem_register(&pdev->dev, &econfig);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	platform_set_drvdata(pdev, nvmem);

	return 0;
}

static struct platform_driver taurus_otp_driver = {
	.probe = taurus_otp_probe,
	.driver = {
		.name = "taurus-otp",
		.of_match_table = taurus_otp_of_match,
	},
};
module_platform_driver(taurus_otp_driver);

MODULE_DESCRIPTION("Realtek Taurus OTP nvmem driver");
MODULE_LICENSE("GPL");
