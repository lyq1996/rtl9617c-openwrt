// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek Taurus (RTL9617C) hardware crypto accelerator driver
 *
 * Supports AES-CBC (128/192/256-bit keys) and AES-GCM / RFC4106(AES-GCM)
 * using the IPsec DMA engine at 0x90500000.
 */

#include <crypto/aead.h>
#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/gcm.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/skcipher.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define RTK_CRYPTO_DRV_NAME		"rtk-taurus-crypto"

/* Register offsets */
#define RTK_REG_SRC_WORD0		0x004
#define RTK_REG_SRC_ADDR		0x008
#define RTK_REG_CTRL_STAT		0x010
#define RTK_REG_IRQ_CTRL		0x014
#define RTK_REG_DMA_BURST		0x018
#define RTK_REG_ERR_STAT		0x01c
#define RTK_REG_MODE_CTRL		0x030
#define RTK_REG_KEY_SLOT(s)		(0x500 + (s) * 32)
#define RTK_REG_REV			0x888
#define RTK_REG_DST_WORD0		0x1004
#define RTK_REG_DST_ADDR		0x1008

/* Protect control register bit enabling crypto MMIO/DMA */
#define RTK_PROTECT_CRYPTO_EN		BIT(4)

/* CTRL_STAT bits */
#define RTK_STAT_SRST			(BIT(31) | BIT(0))
#define RTK_STAT_DONE			BIT(4)
#define RTK_STAT_CNT_MODE		BIT(7)

/* IRQ_CTRL mask values (active-low enable: 0 = unmasked, 0xffff = masked) */
#define RTK_IRQ_UNMASK_ALL		0x00000000U
#define RTK_IRQ_MASK_BIT0		0x00000001U
#define RTK_IRQ_MASK_ALL		0x0000ffffU

/* Mode & DMA defaults */
#define RTK_MODE_CTRL_DEFAULT		0x0000061eU
#define RTK_DMA_BURST_DEFAULT		0x01010000U

/* Descriptor word0 encodings */
#define RTK_SRC_LAST			BIT(28)
#define RTK_SRC_MORE			0x40000000U
#define RTK_SRC_END			0x50000000U
#define RTK_SRC_CMD_INLINE		0x63000000U
#define RTK_SRC_KEY_INLINE		0x60000000U
#define RTK_SRC_CMD_SLOT_BASE		0x67008000U
#define RTK_SRC_IV_SLOT_BASE		0x64008000U

#define RTK_DST_MORE			0x68000000U
#define RTK_DST_END			0x78000000U
#define RTK_DST_ICV_MORE		0x40000000U
#define RTK_DST_ICV_END			0x50000000U

/* Command word0 templates (bits [9:8] select AES key size: 0=128,1=192,2=256) */
#define RTK_CMD0_AES_CBC_DEC		0x40000001U
#define RTK_CMD0_AES_CBC_ENC		0x40000081U
#define RTK_CMD0_AES_GCM_DEC		0x40000008U
#define RTK_CMD0_AES_GCM_ENC		0x40000088U

/* Hardware descriptor limits */
#define RTK_MAX_HW_SEG			16368U
#define RTK_MAX_HW_AAD			31U
#define RTK_QUEUE_LEN			256
#define RTK_TIMEOUT_MS			500
#define RTK_DMA_BUF_SZ			SZ_64K

/* Sub-offsets inside coherent DMA pool */
#define RTK_OFF_CMD			0x0000
#define RTK_OFF_KEY			0x0040
#define RTK_OFF_IV			0x0080
#define RTK_OFF_AAD			0x00c0
#define RTK_OFF_ICV			0x0100
#define RTK_OFF_ZERO			0x0140
#define RTK_OFF_SRC			0x1000
#define RTK_OFF_DST			0x6000

static int prio = 150;
module_param(prio, int, 0444);
MODULE_PARM_DESC(prio, "Crypto algorithm registration priority (default 150)");

static bool use_irq = true;
module_param(use_irq, bool, 0444);
MODULE_PARM_DESC(use_irq, "Use SPI interrupt completion instead of polling (default true)");

struct rtk_crypto_dev;

struct rtk_skcipher_alg {
	struct skcipher_alg alg;
	struct rtk_crypto_dev *rdev;
};

struct rtk_aead_alg {
	struct aead_alg alg;
	struct rtk_crypto_dev *rdev;
	bool is_rfc4106;
};

struct rtk_cipher_ctx {
	struct rtk_crypto_dev *rdev;
	u8 key[AES_MAX_KEY_SIZE];
	unsigned int keylen;
};

struct rtk_aead_ctx {
	struct rtk_crypto_dev *rdev;
	struct crypto_aead *fallback;
	u8 key[AES_MAX_KEY_SIZE];
	u8 salt[4];
	unsigned int keylen;
	unsigned int authsize;
	bool is_rfc4106;
};

struct rtk_skcipher_reqctx {
	bool enc;
};

struct rtk_aead_reqctx {
	bool enc;
	/* Must be last for fallback AEAD request context */
	struct aead_request fb_req;
};

struct rtk_crypto_dev {
	struct device *dev;
	void __iomem *base;
	void __iomem *protect;
	struct clk *clk;
	int irq;

	spinlock_t q_lock; /* protects queue */
	struct crypto_queue queue;
	struct workqueue_struct *wq;
	struct work_struct work;

	struct completion done;
	u32 irq_err;

	void *dma_cpu;
	dma_addr_t dma_bus;
	u32 *cmd;
	dma_addr_t cmd_dma;
	u8 *key;
	dma_addr_t key_dma;
	u8 *iv;
	dma_addr_t iv_dma;
	u8 *aad;
	dma_addr_t aad_dma;
	u8 *icv;
	dma_addr_t icv_dma;
	u8 *zero;
	dma_addr_t zero_dma;
	u8 *src;
	dma_addr_t src_dma;
	u8 *dst;
	dma_addr_t dst_dma;

	u8 active_key[AES_MAX_KEY_SIZE];
	unsigned int active_keylen;

	struct rtk_skcipher_alg skcipher;
	struct rtk_aead_alg aead[2];
};

static inline void rtk_push_src(struct rtk_crypto_dev *rdev, u32 word0,
				dma_addr_t addr)
{
	wmb(); /* Ensure descriptor payload reaches DMA RAM before MMIO kick */
	writel(word0, rdev->base + RTK_REG_SRC_WORD0);
	writel(lower_32_bits(addr), rdev->base + RTK_REG_SRC_ADDR);
}

static inline void rtk_push_dst(struct rtk_crypto_dev *rdev, u32 word0,
				dma_addr_t addr)
{
	wmb(); /* Ensure DST descriptor is ordered before SRC kick */
	writel(word0, rdev->base + RTK_REG_DST_WORD0);
	writel(lower_32_bits(addr), rdev->base + RTK_REG_DST_ADDR);
}

static void rtk_hw_init(struct rtk_crypto_dev *rdev)
{
	u32 val;

	if (rdev->protect) {
		val = readl(rdev->protect);
		writel(val | RTK_PROTECT_CRYPTO_EN, rdev->protect);
	}

	val = readl(rdev->base + RTK_REG_CTRL_STAT);
	writel(val | RTK_STAT_SRST, rdev->base + RTK_REG_CTRL_STAT);
	udelay(10);

	val = readl(rdev->base + RTK_REG_CTRL_STAT);
	val &= ~RTK_STAT_CNT_MODE;
	writel(val, rdev->base + RTK_REG_CTRL_STAT);

	val = readl(rdev->base + RTK_REG_MODE_CTRL);
	writel(val | RTK_MODE_CTRL_DEFAULT, rdev->base + RTK_REG_MODE_CTRL);

	val = readl(rdev->base + RTK_REG_DMA_BURST);
	writel(val | RTK_DMA_BURST_DEFAULT, rdev->base + RTK_REG_DMA_BURST);

	writel(RTK_IRQ_MASK_ALL, rdev->base + RTK_REG_ERR_STAT);
	writel(RTK_STAT_DONE, rdev->base + RTK_REG_CTRL_STAT);
	writel(use_irq && rdev->irq > 0 ?
	       RTK_IRQ_UNMASK_ALL : RTK_IRQ_MASK_ALL,
	       rdev->base + RTK_REG_IRQ_CTRL);

	memzero_explicit(rdev->active_key, sizeof(rdev->active_key));
	rdev->active_keylen = 0;
}

static irqreturn_t rtk_crypto_isr(int irq, void *data)
{
	struct rtk_crypto_dev *rdev = data;
	u32 stat, err;

	writel(RTK_IRQ_MASK_BIT0, rdev->base + RTK_REG_IRQ_CTRL);
	stat = readl(rdev->base + RTK_REG_CTRL_STAT);
	err = readl(rdev->base + RTK_REG_ERR_STAT);

	if (stat & RTK_STAT_DONE)
		writel(RTK_STAT_DONE, rdev->base + RTK_REG_CTRL_STAT);
	if (err) {
		rdev->irq_err = err;
		writel(err, rdev->base + RTK_REG_ERR_STAT);
	} else {
		rdev->irq_err = 0;
	}

	writel(RTK_IRQ_UNMASK_ALL, rdev->base + RTK_REG_IRQ_CTRL);
	complete(&rdev->done);
	return IRQ_HANDLED;
}

static int rtk_wait_done(struct rtk_crypto_dev *rdev)
{
	int loops = 250000;
	u32 stat, err;

	if (use_irq && rdev->irq > 0) {
		if (!wait_for_completion_timeout(&rdev->done,
						 msecs_to_jiffies(RTK_TIMEOUT_MS))) {
			dev_err_ratelimited(rdev->dev,
					    "crypto timeout stat=0x%08x err=0x%08x\n",
					    readl(rdev->base + RTK_REG_CTRL_STAT),
					    readl(rdev->base + RTK_REG_ERR_STAT));
			disable_irq(rdev->irq);
			rtk_hw_init(rdev);
			enable_irq(rdev->irq);
			return -ETIMEDOUT;
		}
		if (rdev->irq_err) {
			dev_err_ratelimited(rdev->dev,
					    "crypto irq err=0x%08x\n",
					    rdev->irq_err);
			disable_irq(rdev->irq);
			rtk_hw_init(rdev);
			enable_irq(rdev->irq);
			return -EIO;
		}
		return 0;
	}

	while (loops--) {
		err = readl(rdev->base + RTK_REG_ERR_STAT);
		if (unlikely(err)) {
			writel(err, rdev->base + RTK_REG_ERR_STAT);
			rtk_hw_init(rdev);
			return -EIO;
		}
		stat = readl(rdev->base + RTK_REG_CTRL_STAT);
		if (likely(stat & RTK_STAT_DONE)) {
			writel(RTK_STAT_DONE, rdev->base + RTK_REG_CTRL_STAT);
			return 0;
		}
		cpu_relax();
	}

	rtk_hw_init(rdev);
	return -ETIMEDOUT;
}

static u32 rtk_aes_kl_bits(unsigned int keylen)
{
	if (keylen == AES_KEYSIZE_256)
		return 2;
	if (keylen == AES_KEYSIZE_192)
		return 1;
	return 0;
}

static void rtk_write_hw_key(struct rtk_crypto_dev *rdev, const u8 *key,
			     unsigned int keylen)
{
	u32 words[8] = { };
	int i;

	if (rdev->active_keylen == keylen &&
	    !crypto_memneq(rdev->active_key, key, keylen))
		return;

	memcpy(words, key, min_t(unsigned int, keylen, AES_MAX_KEY_SIZE));
	for (i = 0; i < ARRAY_SIZE(words); i++)
		writel(cpu_to_be32(words[i]),
		       rdev->base + RTK_REG_KEY_SLOT(0) + i * 4);
	memzero_explicit(words, sizeof(words));

	memcpy(rdev->active_key, key, keylen);
	rdev->active_keylen = keylen;
}

static int rtk_hw_aes_cbc(struct rtk_crypto_dev *rdev, bool enc,
			  const u8 *key, unsigned int keylen,
			  const u8 *iv, unsigned int len)
{
	u32 kl = rtk_aes_kl_bits(keylen);
	int ret;

	memset(rdev->cmd, 0, 28);
	rdev->cmd[0] = (enc ? RTK_CMD0_AES_CBC_ENC : RTK_CMD0_AES_CBC_DEC) |
		       (kl << 8);
	rdev->cmd[1] = 0x00020000U | ((len & 0xf) << 4);
	rdev->cmd[2] = ((len + 15) >> 4) << 16;

	memcpy(rdev->key, key, keylen);
	memcpy(rdev->iv, iv, AES_BLOCK_SIZE);

	if (use_irq && rdev->irq > 0)
		reinit_completion(&rdev->done);

	rtk_push_dst(rdev, RTK_DST_END | (len & 0xffffffU), rdev->dst_dma);
	rtk_push_src(rdev, RTK_SRC_CMD_INLINE, rdev->cmd_dma);
	rtk_push_src(rdev, RTK_SRC_KEY_INLINE | ((keylen / 4) & 0xf),
		     rdev->key_dma);
	rtk_push_src(rdev, RTK_SRC_KEY_INLINE | ((AES_BLOCK_SIZE / 4) << 4),
		     rdev->iv_dma);
	rtk_push_src(rdev, RTK_SRC_END | ((len & 0x3fffU) << 14),
		     rdev->src_dma);

	ret = rtk_wait_done(rdev);
	if (!ret)
		rmb(); /* Order DMA completion before reading output buffer */

	return ret;
}

static int rtk_hw_aes_gcm(struct rtk_crypto_dev *rdev, bool enc,
			  const u8 *key, unsigned int keylen,
			  const u8 *iv12, unsigned int aad_len,
			  unsigned int pt_len)
{
	u32 kl = rtk_aes_kl_bits(keylen);
	unsigned int aad_pad = (16 - (aad_len & 0xf)) & 0xf;
	unsigned int data_pad = (16 - (pt_len & 0xf)) & 0xf;
	int ret;

	rtk_write_hw_key(rdev, key, keylen);

	memset(rdev->cmd, 0, 28);
	rdev->cmd[0] = (enc ? RTK_CMD0_AES_GCM_ENC : RTK_CMD0_AES_GCM_DEC) |
		       (kl << 8);
	rdev->cmd[1] = 0x00020000U | ((pt_len & 0xf) << 4) | (aad_len & 0xf);
	rdev->cmd[2] = ((((aad_len + 15) >> 4) & 0x3f) << 8) |
		       (((pt_len + 15) >> 4) << 16);

	memcpy(rdev->iv, iv12, GCM_AES_IV_SIZE);
	*(__be32 *)(rdev->iv + GCM_AES_IV_SIZE) = cpu_to_be32(1);

	if (use_irq && rdev->irq > 0)
		reinit_completion(&rdev->done);

	/*
	 * Output plaintext/ciphertext to DMA dst and the computed 16-byte
	 * GHASH tag to DMA icv; decrypt verifies the tag safely in CPU.
	 */
	rtk_push_dst(rdev, RTK_DST_MORE | (pt_len & 0xffffffU), rdev->dst_dma);
	rtk_push_dst(rdev, RTK_DST_ICV_END | AES_BLOCK_SIZE, rdev->icv_dma);

	rtk_push_src(rdev, RTK_SRC_CMD_SLOT_BASE, rdev->cmd_dma);
	rtk_push_src(rdev, RTK_SRC_IV_SLOT_BASE | (4 << 4), rdev->iv_dma);
	if (aad_len) {
		rtk_push_src(rdev, RTK_SRC_MORE | ((aad_len & 0x1fU) << 8),
			     rdev->aad_dma);
		if (aad_pad)
			rtk_push_src(rdev, RTK_SRC_MORE | ((aad_pad & 0xfU) << 8),
				     rdev->zero_dma);
	}
	rtk_push_src(rdev,
		     (data_pad ? RTK_SRC_MORE : RTK_SRC_END) |
		     ((pt_len & 0x3fffU) << 14),
		     rdev->src_dma);
	if (data_pad)
		rtk_push_src(rdev, RTK_SRC_END | ((data_pad & 0x3fffU) << 14),
			     rdev->zero_dma);

	ret = rtk_wait_done(rdev);
	if (!ret)
		rmb(); /* Order DMA completion before reading dst and icv */

	return ret;
}

static int rtk_process_skcipher(struct rtk_crypto_dev *rdev,
				struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct rtk_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct rtk_skcipher_reqctx *rctx = skcipher_request_ctx(req);
	u8 cur_iv[AES_BLOCK_SIZE];
	u8 next_iv[AES_BLOCK_SIZE];
	unsigned int off = 0;
	int err = 0;

	if (!req->cryptlen)
		return 0;

	memcpy(cur_iv, req->iv, AES_BLOCK_SIZE);

	while (off < req->cryptlen) {
		unsigned int chunk = min_t(unsigned int, req->cryptlen - off,
					   RTK_MAX_HW_SEG);

		scatterwalk_map_and_copy(rdev->src, req->src, off, chunk, 0);
		if (!rctx->enc)
			memcpy(next_iv, rdev->src + chunk - AES_BLOCK_SIZE,
			       AES_BLOCK_SIZE);

		err = rtk_hw_aes_cbc(rdev, rctx->enc, ctx->key, ctx->keylen,
				     cur_iv, chunk);
		if (err)
			break;

		scatterwalk_map_and_copy(rdev->dst, req->dst, off, chunk, 1);
		if (rctx->enc)
			memcpy(cur_iv, rdev->dst + chunk - AES_BLOCK_SIZE,
			       AES_BLOCK_SIZE);
		else
			memcpy(cur_iv, next_iv, AES_BLOCK_SIZE);

		off += chunk;
	}

	if (!err)
		memcpy(req->iv, cur_iv, AES_BLOCK_SIZE);

	memzero_explicit(rdev->key, AES_MAX_KEY_SIZE);
	memzero_explicit(cur_iv, sizeof(cur_iv));
	memzero_explicit(next_iv, sizeof(next_iv));
	return err;
}

static int rtk_process_aead(struct rtk_crypto_dev *rdev,
			    struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct rtk_aead_ctx *ctx = crypto_aead_ctx(tfm);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	unsigned int authsize = crypto_aead_authsize(tfm);
	unsigned int aad_len = ctx->is_rfc4106 ? req->assoclen - 8 : req->assoclen;
	unsigned int pt_len = rctx->enc ? req->cryptlen : req->cryptlen - authsize;
	u8 iv12[GCM_AES_IV_SIZE];
	u8 expected_tag[AES_BLOCK_SIZE];
	int err;

	if (ctx->is_rfc4106) {
		memcpy(iv12, ctx->salt, 4);
		memcpy(iv12 + 4, req->iv, 8);
	} else {
		memcpy(iv12, req->iv, GCM_AES_IV_SIZE);
	}

	if (aad_len)
		scatterwalk_map_and_copy(rdev->aad, req->src, 0, aad_len, 0);

	scatterwalk_map_and_copy(rdev->src, req->src, req->assoclen, pt_len, 0);
	if (!rctx->enc)
		scatterwalk_map_and_copy(expected_tag, req->src,
					 req->assoclen + pt_len, authsize, 0);

	err = rtk_hw_aes_gcm(rdev, rctx->enc, ctx->key, ctx->keylen, iv12,
			     aad_len, pt_len);
	if (!err) {
		if (!rctx->enc && crypto_memneq(rdev->icv, expected_tag, authsize)) {
			err = -EBADMSG;
		} else {
			scatterwalk_map_and_copy(rdev->dst, req->dst,
						 req->assoclen, pt_len, 1);
			if (rctx->enc)
				scatterwalk_map_and_copy(rdev->icv, req->dst,
							 req->assoclen + pt_len,
							 authsize, 1);
		}
	}

	memzero_explicit(iv12, sizeof(iv12));
	memzero_explicit(expected_tag, sizeof(expected_tag));
	return err;
}

static void rtk_crypto_work_handler(struct work_struct *work)
{
	struct rtk_crypto_dev *rdev =
		container_of(work, struct rtk_crypto_dev, work);

	for (;;) {
		struct crypto_async_request *areq, *backlog;
		unsigned long flags;
		int err;

		spin_lock_irqsave(&rdev->q_lock, flags);
		backlog = crypto_get_backlog(&rdev->queue);
		areq = crypto_dequeue_request(&rdev->queue);
		spin_unlock_irqrestore(&rdev->q_lock, flags);

		if (!areq)
			break;
		if (backlog) {
			local_bh_disable();
			crypto_request_complete(backlog, -EINPROGRESS);
			local_bh_enable();
		}

		if (crypto_tfm_alg_type(areq->tfm) == CRYPTO_ALG_TYPE_SKCIPHER) {
			struct skcipher_request *sreq =
				skcipher_request_cast(areq);

			err = rtk_process_skcipher(rdev, sreq);
		} else {
			struct aead_request *areq_aead =
				container_of(areq, struct aead_request, base);

			err = rtk_process_aead(rdev, areq_aead);
		}

		local_bh_disable();
		crypto_request_complete(areq, err);
		local_bh_enable();
	}
}

static int rtk_enqueue_req(struct rtk_crypto_dev *rdev,
			   struct crypto_async_request *areq)
{
	unsigned long flags;
	int err;

	spin_lock_irqsave(&rdev->q_lock, flags);
	err = crypto_enqueue_request(&rdev->queue, areq);
	spin_unlock_irqrestore(&rdev->q_lock, flags);

	if (err == -ENOSPC)
		return err;

	queue_work(rdev->wq, &rdev->work);
	return err;
}

static int rtk_cbc_setkey(struct crypto_skcipher *tfm, const u8 *key,
			  unsigned int keylen)
{
	struct rtk_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	int err;

	err = aes_check_keylen(keylen);
	if (err)
		return err;

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;
	return 0;
}

static int rtk_cbc_crypt(struct skcipher_request *req, bool enc)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct rtk_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct rtk_skcipher_reqctx *rctx = skcipher_request_ctx(req);

	if (!req->cryptlen)
		return 0;
	if (!IS_ALIGNED(req->cryptlen, AES_BLOCK_SIZE))
		return -EINVAL;

	rctx->enc = enc;
	return rtk_enqueue_req(ctx->rdev, &req->base);
}

static int rtk_cbc_encrypt(struct skcipher_request *req)
{
	return rtk_cbc_crypt(req, true);
}

static int rtk_cbc_decrypt(struct skcipher_request *req)
{
	return rtk_cbc_crypt(req, false);
}

static int rtk_cbc_init_tfm(struct crypto_skcipher *tfm)
{
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct rtk_skcipher_alg *ralg =
		container_of(alg, struct rtk_skcipher_alg, alg);
	struct rtk_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->rdev = ralg->rdev;
	crypto_skcipher_set_reqsize(tfm, sizeof(struct rtk_skcipher_reqctx));
	return 0;
}

static void rtk_cbc_exit_tfm(struct crypto_skcipher *tfm)
{
	struct rtk_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);

	memzero_explicit(ctx->key, sizeof(ctx->key));
}

static int rtk_aead_setkey(struct crypto_aead *tfm, const u8 *key,
			   unsigned int keylen)
{
	struct rtk_aead_ctx *ctx = crypto_aead_ctx(tfm);
	unsigned int aes_keylen = keylen;
	int err;

	if (ctx->is_rfc4106) {
		if (keylen < 4)
			return -EINVAL;
		aes_keylen = keylen - 4;
	}

	err = aes_check_keylen(aes_keylen);
	if (err)
		return err;

	memcpy(ctx->key, key, aes_keylen);
	if (ctx->is_rfc4106)
		memcpy(ctx->salt, key + aes_keylen, 4);
	ctx->keylen = aes_keylen;

	if (ctx->fallback)
		return crypto_aead_setkey(ctx->fallback, key, keylen);
	return 0;
}

static int rtk_aead_setauthsize(struct crypto_aead *tfm, unsigned int authsize)
{
	struct rtk_aead_ctx *ctx = crypto_aead_ctx(tfm);
	int err;

	err = ctx->is_rfc4106 ? crypto_rfc4106_check_authsize(authsize) :
				crypto_gcm_check_authsize(authsize);
	if (err)
		return err;

	ctx->authsize = authsize;
	if (ctx->fallback)
		return crypto_aead_setauthsize(ctx->fallback, authsize);
	return 0;
}

static int rtk_aead_fallback(struct aead_request *req,
			     struct rtk_aead_ctx *ctx, bool enc)
{
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);

	aead_request_set_tfm(&rctx->fb_req, ctx->fallback);
	aead_request_set_callback(&rctx->fb_req, req->base.flags,
				  req->base.complete, req->base.data);
	aead_request_set_crypt(&rctx->fb_req, req->src, req->dst,
			       req->cryptlen, req->iv);
	aead_request_set_ad(&rctx->fb_req, req->assoclen);
	return enc ? crypto_aead_encrypt(&rctx->fb_req) :
		     crypto_aead_decrypt(&rctx->fb_req);
}

static int rtk_aead_crypt(struct aead_request *req, bool enc)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct rtk_aead_ctx *ctx = crypto_aead_ctx(tfm);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	unsigned int authsize = crypto_aead_authsize(tfm);
	unsigned int aad_len, pt_len;

	if (ctx->is_rfc4106) {
		int err = crypto_ipsec_check_assoclen(req->assoclen);

		if (err)
			return err;
		aad_len = req->assoclen - 8;
	} else {
		aad_len = req->assoclen;
	}

	if (!enc && req->cryptlen < authsize)
		return -EINVAL;

	pt_len = enc ? req->cryptlen : req->cryptlen - authsize;
	if (!pt_len || pt_len > RTK_MAX_HW_SEG || aad_len > RTK_MAX_HW_AAD)
		return rtk_aead_fallback(req, ctx, enc);

	rctx->enc = enc;
	return rtk_enqueue_req(ctx->rdev, &req->base);
}

static int rtk_aead_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, true);
}

static int rtk_aead_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, false);
}

static int rtk_aead_init_tfm(struct crypto_aead *tfm)
{
	struct aead_alg *alg = crypto_aead_alg(tfm);
	struct rtk_aead_alg *ralg =
		container_of(alg, struct rtk_aead_alg, alg);
	struct rtk_aead_ctx *ctx = crypto_aead_ctx(tfm);
	const char *fb_name = ralg->is_rfc4106 ? "rfc4106(gcm(aes))" :
						 "gcm(aes)";
	struct crypto_aead *fb;

	fb = crypto_alloc_aead(fb_name, 0, CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(fb))
		return PTR_ERR(fb);

	ctx->rdev = ralg->rdev;
	ctx->is_rfc4106 = ralg->is_rfc4106;
	ctx->fallback = fb;
	crypto_aead_set_reqsize(tfm, sizeof(struct rtk_aead_reqctx) +
				     crypto_aead_reqsize(fb));
	return 0;
}

static void rtk_aead_exit_tfm(struct crypto_aead *tfm)
{
	struct rtk_aead_ctx *ctx = crypto_aead_ctx(tfm);

	crypto_free_aead(ctx->fallback);
	memzero_explicit(ctx->key, sizeof(ctx->key));
	memzero_explicit(ctx->salt, sizeof(ctx->salt));
}

static void rtk_init_algs(struct rtk_crypto_dev *rdev)
{
	struct skcipher_alg *sk = &rdev->skcipher.alg;
	struct aead_alg *gcm = &rdev->aead[0].alg;
	struct aead_alg *rfc = &rdev->aead[1].alg;

	rdev->skcipher.rdev = rdev;
	strscpy(sk->base.cra_name, "cbc(aes)", sizeof(sk->base.cra_name));
	strscpy(sk->base.cra_driver_name, "rtk-cbc-aes",
		sizeof(sk->base.cra_driver_name));
	sk->base.cra_priority = prio;
	sk->base.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_KERN_DRIVER_ONLY;
	sk->base.cra_blocksize = AES_BLOCK_SIZE;
	sk->base.cra_ctxsize = sizeof(struct rtk_cipher_ctx);
	sk->base.cra_module = THIS_MODULE;
	sk->min_keysize = AES_MIN_KEY_SIZE;
	sk->max_keysize = AES_MAX_KEY_SIZE;
	sk->ivsize = AES_BLOCK_SIZE;
	sk->setkey = rtk_cbc_setkey;
	sk->encrypt = rtk_cbc_encrypt;
	sk->decrypt = rtk_cbc_decrypt;
	sk->init = rtk_cbc_init_tfm;
	sk->exit = rtk_cbc_exit_tfm;

	rdev->aead[0].rdev = rdev;
	rdev->aead[0].is_rfc4106 = false;
	strscpy(gcm->base.cra_name, "gcm(aes)", sizeof(gcm->base.cra_name));
	strscpy(gcm->base.cra_driver_name, "rtk-gcm-aes",
		sizeof(gcm->base.cra_driver_name));
	gcm->base.cra_priority = prio;
	gcm->base.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK |
			      CRYPTO_ALG_KERN_DRIVER_ONLY;
	gcm->base.cra_blocksize = 1;
	gcm->base.cra_ctxsize = sizeof(struct rtk_aead_ctx);
	gcm->base.cra_module = THIS_MODULE;
	gcm->ivsize = GCM_AES_IV_SIZE;
	gcm->maxauthsize = AES_BLOCK_SIZE;
	gcm->setkey = rtk_aead_setkey;
	gcm->setauthsize = rtk_aead_setauthsize;
	gcm->encrypt = rtk_aead_encrypt;
	gcm->decrypt = rtk_aead_decrypt;
	gcm->init = rtk_aead_init_tfm;
	gcm->exit = rtk_aead_exit_tfm;

	rdev->aead[1].rdev = rdev;
	rdev->aead[1].is_rfc4106 = true;
	strscpy(rfc->base.cra_name, "rfc4106(gcm(aes))",
		sizeof(rfc->base.cra_name));
	strscpy(rfc->base.cra_driver_name, "rtk-rfc4106-gcm-aes",
		sizeof(rfc->base.cra_driver_name));
	rfc->base.cra_priority = prio;
	rfc->base.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK |
			      CRYPTO_ALG_KERN_DRIVER_ONLY;
	rfc->base.cra_blocksize = 1;
	rfc->base.cra_ctxsize = sizeof(struct rtk_aead_ctx);
	rfc->base.cra_module = THIS_MODULE;
	rfc->ivsize = 8;
	rfc->maxauthsize = AES_BLOCK_SIZE;
	rfc->setkey = rtk_aead_setkey;
	rfc->setauthsize = rtk_aead_setauthsize;
	rfc->encrypt = rtk_aead_encrypt;
	rfc->decrypt = rtk_aead_decrypt;
	rfc->init = rtk_aead_init_tfm;
	rfc->exit = rtk_aead_exit_tfm;
}

static int rtk_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtk_crypto_dev *rdev;
	struct resource *res;
	u32 rev;
	int ret;

	rdev = devm_kzalloc(dev, sizeof(*rdev), GFP_KERNEL);
	if (!rdev)
		return -ENOMEM;

	rdev->dev = dev;
	platform_set_drvdata(pdev, rdev);
	spin_lock_init(&rdev->q_lock);
	init_completion(&rdev->done);
	crypto_init_queue(&rdev->queue, RTK_QUEUE_LEN);
	INIT_WORK(&rdev->work, rtk_crypto_work_handler);

	rdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rdev->base))
		return PTR_ERR(rdev->base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		rdev->protect = devm_ioremap_resource(dev, res);
		if (IS_ERR(rdev->protect))
			return PTR_ERR(rdev->protect);
	}

	rdev->clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(rdev->clk))
		return PTR_ERR(rdev->clk);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	rdev->dma_cpu = dmam_alloc_coherent(dev, RTK_DMA_BUF_SZ, &rdev->dma_bus,
					    GFP_KERNEL);
	if (!rdev->dma_cpu)
		return -ENOMEM;

	rdev->cmd = rdev->dma_cpu + RTK_OFF_CMD;
	rdev->cmd_dma = rdev->dma_bus + RTK_OFF_CMD;
	rdev->key = rdev->dma_cpu + RTK_OFF_KEY;
	rdev->key_dma = rdev->dma_bus + RTK_OFF_KEY;
	rdev->iv = rdev->dma_cpu + RTK_OFF_IV;
	rdev->iv_dma = rdev->dma_bus + RTK_OFF_IV;
	rdev->aad = rdev->dma_cpu + RTK_OFF_AAD;
	rdev->aad_dma = rdev->dma_bus + RTK_OFF_AAD;
	rdev->icv = rdev->dma_cpu + RTK_OFF_ICV;
	rdev->icv_dma = rdev->dma_bus + RTK_OFF_ICV;
	rdev->zero = rdev->dma_cpu + RTK_OFF_ZERO;
	rdev->zero_dma = rdev->dma_bus + RTK_OFF_ZERO;
	rdev->src = rdev->dma_cpu + RTK_OFF_SRC;
	rdev->src_dma = rdev->dma_bus + RTK_OFF_SRC;
	rdev->dst = rdev->dma_cpu + RTK_OFF_DST;
	rdev->dst_dma = rdev->dma_bus + RTK_OFF_DST;

	rdev->irq = 0;
	rtk_hw_init(rdev);
	rev = readl(rdev->base + RTK_REG_REV);
	if (!rev || rev == 0xffffffffU)
		return -ENODEV;

	rdev->irq = platform_get_irq_optional(pdev, 0);
	if (rdev->irq > 0) {
		ret = devm_request_irq(dev, rdev->irq, rtk_crypto_isr, 0,
				       dev_name(dev), rdev);
		if (ret)
			return ret;
		if (use_irq)
			writel(RTK_IRQ_UNMASK_ALL,
			       rdev->base + RTK_REG_IRQ_CTRL);
	}

	rdev->wq = alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM,
					   dev_name(dev));
	if (!rdev->wq)
		return -ENOMEM;

	rtk_init_algs(rdev);

	ret = crypto_register_skcipher(&rdev->skcipher.alg);
	if (ret)
		goto err_wq;

	ret = crypto_register_aead(&rdev->aead[0].alg);
	if (ret)
		goto err_sk;

	ret = crypto_register_aead(&rdev->aead[1].alg);
	if (ret)
		goto err_gcm;

	dev_info(dev, "Realtek Taurus crypto engine rev 0x%08x (irq=%d, prio=%d)\n",
		 rev, rdev->irq, prio);
	return 0;

err_gcm:
	crypto_unregister_aead(&rdev->aead[0].alg);
err_sk:
	crypto_unregister_skcipher(&rdev->skcipher.alg);
err_wq:
	writel(RTK_IRQ_MASK_ALL, rdev->base + RTK_REG_IRQ_CTRL);
	if (rdev->irq > 0)
		devm_free_irq(dev, rdev->irq, rdev);
	destroy_workqueue(rdev->wq);
	return ret;
}

static void rtk_crypto_remove(struct platform_device *pdev)
{
	struct rtk_crypto_dev *rdev = platform_get_drvdata(pdev);
	int i;

	crypto_unregister_aead(&rdev->aead[1].alg);
	crypto_unregister_aead(&rdev->aead[0].alg);
	crypto_unregister_skcipher(&rdev->skcipher.alg);

	flush_work(&rdev->work);
	destroy_workqueue(rdev->wq);

	writel(RTK_IRQ_MASK_ALL, rdev->base + RTK_REG_IRQ_CTRL);
	if (rdev->irq > 0)
		devm_free_irq(rdev->dev, rdev->irq, rdev);

	for (i = 0; i < 8; i++)
		writel(0, rdev->base + RTK_REG_KEY_SLOT(0) + i * 4);

	memzero_explicit(rdev->active_key, sizeof(rdev->active_key));
	memzero_explicit(rdev->dma_cpu, RTK_DMA_BUF_SZ);
}

static const struct of_device_id rtk_crypto_of_match[] = {
	{ .compatible = "realtek,taurus-crypto" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtk_crypto_of_match);

static const struct platform_device_id rtk_crypto_id_table[] = {
	{ RTK_CRYPTO_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, rtk_crypto_id_table);

static struct platform_driver rtk_crypto_driver = {
	.probe = rtk_crypto_probe,
	.remove = rtk_crypto_remove,
	.id_table = rtk_crypto_id_table,
	.driver = {
		.name = RTK_CRYPTO_DRV_NAME,
		.of_match_table = rtk_crypto_of_match,
	},
};
module_platform_driver(rtk_crypto_driver);

MODULE_DESCRIPTION("Realtek Taurus hardware crypto engine driver");
MODULE_AUTHOR("PT1500 OpenWrt Contributors");
MODULE_LICENSE("GPL");
