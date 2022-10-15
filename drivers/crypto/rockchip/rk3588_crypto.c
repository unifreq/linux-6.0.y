// SPDX-License-Identifier: GPL-2.0
/*
 * hardware cryptographic offloader for rk3568/rk3588 SoC
 *
 * Copyright (c) 2022, Corentin Labbe <clabbe@baylibre.com>
 */

#include "rk3588_crypto.h"
#include <linux/clk.h>
#include <linux/crypto.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/reset.h>

static struct rockchip_ip rocklist = {
	.dev_list = LIST_HEAD_INIT(rocklist.dev_list),
	.lock = __SPIN_LOCK_UNLOCKED(rocklist.lock),
};

struct rk_crypto_dev *get_rk_crypto(void)
{
	struct rk_crypto_dev *first;

	spin_lock(&rocklist.lock);
	first = list_first_entry_or_null(&rocklist.dev_list,
					 struct rk_crypto_dev, list);
	list_rotate_left(&rocklist.dev_list);
	spin_unlock(&rocklist.lock);
	return first;
}

static const struct rk_variant rk3568_variant = {
	.num_clks = 4,
};

static const struct rk_variant rk3588_variant = {
	.num_clks = 4,
};

static int rk_crypto_get_clks(struct rk_crypto_dev *dev)
{
	int i, j, err;
	unsigned long cr;

	dev->num_clks = devm_clk_bulk_get_all(dev->dev, &dev->clks);
	if (dev->num_clks < dev->variant->num_clks) {
		dev_err(dev->dev, "Missing clocks, got %d instead of %d\n",
			dev->num_clks, dev->variant->num_clks);
		return -EINVAL;
	}

	for (i = 0; i < dev->num_clks; i++) {
		cr = clk_get_rate(dev->clks[i].clk);
		for (j = 0; j < ARRAY_SIZE(dev->variant->rkclks); j++) {
			if (dev->variant->rkclks[j].max == 0)
				continue;
			if (strcmp(dev->variant->rkclks[j].name, dev->clks[i].id))
				continue;
			if (cr > dev->variant->rkclks[j].max) {
				err = clk_set_rate(dev->clks[i].clk,
						   dev->variant->rkclks[j].max);
				if (err)
					dev_err(dev->dev, "Fail downclocking %s from %lu to %lu\n",
						dev->variant->rkclks[j].name, cr,
						dev->variant->rkclks[j].max);
				else
					dev_info(dev->dev, "Downclocking %s from %lu to %lu\n",
						 dev->variant->rkclks[j].name, cr,
						 dev->variant->rkclks[j].max);
			}
		}
	}
	return 0;
}

static int rk_crypto_enable_clk(struct rk_crypto_dev *dev)
{
	int err;

	err = clk_bulk_prepare_enable(dev->num_clks, dev->clks);
	if (err)
		dev_err(dev->dev, "Could not enable clock clks\n");

	return err;
}

static void rk_crypto_disable_clk(struct rk_crypto_dev *dev)
{
	clk_bulk_disable_unprepare(dev->num_clks, dev->clks);
}

/*
 * Power management strategy: The device is suspended until a request
 * is handled. For avoiding suspend/resume yoyo, the autosuspend is set to 2s.
 */
static int rk_crypto_pm_suspend(struct device *dev)
{
	struct rk_crypto_dev *rkdev = dev_get_drvdata(dev);

	rk_crypto_disable_clk(rkdev);
	reset_control_assert(rkdev->rst);

	return 0;
}

static int rk_crypto_pm_resume(struct device *dev)
{
	struct rk_crypto_dev *rkdev = dev_get_drvdata(dev);
	int ret;

	ret = rk_crypto_enable_clk(rkdev);
	if (ret)
		return ret;

	reset_control_deassert(rkdev->rst);
	return 0;
}

static const struct dev_pm_ops rk_crypto_pm_ops = {
	SET_RUNTIME_PM_OPS(rk_crypto_pm_suspend, rk_crypto_pm_resume, NULL)
};

static int rk_crypto_pm_init(struct rk_crypto_dev *rkdev)
{
	int err;

	pm_runtime_use_autosuspend(rkdev->dev);
	pm_runtime_set_autosuspend_delay(rkdev->dev, 2000);

	err = pm_runtime_set_suspended(rkdev->dev);
	if (err)
		return err;
	pm_runtime_enable(rkdev->dev);
	return err;
}

static void rk_crypto_pm_exit(struct rk_crypto_dev *rkdev)
{
	pm_runtime_disable(rkdev->dev);
}

static irqreturn_t rk_crypto_irq_handle(int irq, void *dev_id)
{
	struct rk_crypto_dev *rkc  = platform_get_drvdata(dev_id);
	u32 v;

	v = readl(rkc->reg + RK_CRYPTO_DMA_INT_ST);
	writel(v, rkc->reg + RK_CRYPTO_DMA_INT_ST);

	rkc->status = 1;
	if (v & 0xF8) {
		dev_warn(rkc->dev, "DMA Error\n");
		rkc->status = 0;
	}
	complete(&rkc->complete);

	return IRQ_HANDLED;
}

static struct rk_crypto_template rk_cipher_algs[] = {
	{
		.type = CRYPTO_ALG_TYPE_SKCIPHER,
		.alg.skcipher = {
			.base.cra_name		= "ecb(aes)",
			.base.cra_driver_name	= "ecb-aes-rk2",
			.base.cra_priority	= 300,
			.base.cra_flags		= CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize	= AES_BLOCK_SIZE,
			.base.cra_ctxsize	= sizeof(struct rk_cipher_ctx),
			.base.cra_alignmask	= 0x0f,
			.base.cra_module	= THIS_MODULE,

			.init			= rk_cipher_tfm_init,
			.exit			= rk_cipher_tfm_exit,
			.min_keysize		= AES_MIN_KEY_SIZE,
			.max_keysize		= AES_MAX_KEY_SIZE,
			.setkey			= rk_aes_setkey,
			.encrypt		= rk_aes_ecb_encrypt,
			.decrypt		= rk_aes_ecb_decrypt,
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_SKCIPHER,
		.alg.skcipher = {
			.base.cra_name		= "cbc(aes)",
			.base.cra_driver_name	= "cbc-aes-rk2",
			.base.cra_priority	= 300,
			.base.cra_flags		= CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize	= AES_BLOCK_SIZE,
			.base.cra_ctxsize	= sizeof(struct rk_cipher_ctx),
			.base.cra_alignmask	= 0x0f,
			.base.cra_module	= THIS_MODULE,

			.init			= rk_cipher_tfm_init,
			.exit			= rk_cipher_tfm_exit,
			.min_keysize		= AES_MIN_KEY_SIZE,
			.max_keysize		= AES_MAX_KEY_SIZE,
			.ivsize			= AES_BLOCK_SIZE,
			.setkey			= rk_aes_setkey,
			.encrypt		= rk_aes_cbc_encrypt,
			.decrypt		= rk_aes_cbc_decrypt,
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_AHASH,
		.rk_mode = RK_CRYPTO_MD5,
		.alg.hash = {
			.init = rk_ahash_init,
			.update = rk_ahash_update,
			.final = rk_ahash_final,
			.finup = rk_ahash_finup,
			.export = rk_ahash_export,
			.import = rk_ahash_import,
			.digest = rk_ahash_digest,
			.halg = {
				.digestsize = MD5_DIGEST_SIZE,
				.statesize = sizeof(struct md5_state),
				.base = {
					.cra_name = "md5",
					.cra_driver_name = "rk2-md5",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA1_BLOCK_SIZE,
					.cra_ctxsize = sizeof(struct rk_ahash_ctx),
					.cra_alignmask = 3,
					.cra_init = rk_cra_hash_init,
					.cra_exit = rk_cra_hash_exit,
					.cra_module = THIS_MODULE,
				}
			}
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_AHASH,
		.rk_mode = RK_CRYPTO_SHA1,
		.alg.hash = {
			.init = rk_ahash_init,
			.update = rk_ahash_update,
			.final = rk_ahash_final,
			.finup = rk_ahash_finup,
			.export = rk_ahash_export,
			.import = rk_ahash_import,
			.digest = rk_ahash_digest,
			.halg = {
				.digestsize = SHA1_DIGEST_SIZE,
				.statesize = sizeof(struct sha1_state),
				.base = {
					.cra_name = "sha1",
					.cra_driver_name = "rk2-sha1",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA1_BLOCK_SIZE,
					.cra_ctxsize = sizeof(struct rk_ahash_ctx),
					.cra_alignmask = 3,
					.cra_init = rk_cra_hash_init,
					.cra_exit = rk_cra_hash_exit,
					.cra_module = THIS_MODULE,
				}
			}
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_AHASH,
		.rk_mode = RK_CRYPTO_SHA256,
		.alg.hash = {
			.init = rk_ahash_init,
			.update = rk_ahash_update,
			.final = rk_ahash_final,
			.finup = rk_ahash_finup,
			.export = rk_ahash_export,
			.import = rk_ahash_import,
			.digest = rk_ahash_digest,
			.halg = {
				.digestsize = SHA256_DIGEST_SIZE,
				.statesize = sizeof(struct sha256_state),
				.base = {
					.cra_name = "sha256",
					.cra_driver_name = "rk2-sha256",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA256_BLOCK_SIZE,
					.cra_ctxsize = sizeof(struct rk_ahash_ctx),
					.cra_alignmask = 3,
					.cra_init = rk_cra_hash_init,
					.cra_exit = rk_cra_hash_exit,
					.cra_module = THIS_MODULE,
				}
			}
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_AHASH,
		.rk_mode = RK_CRYPTO_SHA384,
		.alg.hash = {
			.init = rk_ahash_init,
			.update = rk_ahash_update,
			.final = rk_ahash_final,
			.finup = rk_ahash_finup,
			.export = rk_ahash_export,
			.import = rk_ahash_import,
			.digest = rk_ahash_digest,
			.halg = {
				.digestsize = SHA384_DIGEST_SIZE,
				.statesize = sizeof(struct sha512_state),
				.base = {
					.cra_name = "sha384",
					.cra_driver_name = "rk2-sha384",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA384_BLOCK_SIZE,
					.cra_ctxsize = sizeof(struct rk_ahash_ctx),
					.cra_alignmask = 3,
					.cra_init = rk_cra_hash_init,
					.cra_exit = rk_cra_hash_exit,
					.cra_module = THIS_MODULE,
				}
			}
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_AHASH,
		.rk_mode = RK_CRYPTO_SHA512,
		.alg.hash = {
			.init = rk_ahash_init,
			.update = rk_ahash_update,
			.final = rk_ahash_final,
			.finup = rk_ahash_finup,
			.export = rk_ahash_export,
			.import = rk_ahash_import,
			.digest = rk_ahash_digest,
			.halg = {
				.digestsize = SHA512_DIGEST_SIZE,
				.statesize = sizeof(struct sha512_state),
				.base = {
					.cra_name = "sha512",
					.cra_driver_name = "rk2-sha512",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA512_BLOCK_SIZE,
					.cra_ctxsize = sizeof(struct rk_ahash_ctx),
					.cra_alignmask = 3,
					.cra_init = rk_cra_hash_init,
					.cra_exit = rk_cra_hash_exit,
					.cra_module = THIS_MODULE,
				}
			}
		}
	},
	{
		.type = CRYPTO_ALG_TYPE_AHASH,
		.rk_mode = RK_CRYPTO_SM3,
		.alg.hash = {
			.init = rk_ahash_init,
			.update = rk_ahash_update,
			.final = rk_ahash_final,
			.finup = rk_ahash_finup,
			.export = rk_ahash_export,
			.import = rk_ahash_import,
			.digest = rk_ahash_digest,
			.halg = {
				.digestsize = SM3_DIGEST_SIZE,
				.statesize = sizeof(struct sm3_state),
				.base = {
					.cra_name = "sm3",
					.cra_driver_name = "rk2-sm3",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SM3_BLOCK_SIZE,
					.cra_ctxsize = sizeof(struct rk_ahash_ctx),
					.cra_alignmask = 3,
					.cra_init = rk_cra_hash_init,
					.cra_exit = rk_cra_hash_exit,
					.cra_module = THIS_MODULE,
				}
			}
		}
	},
};

#ifdef CONFIG_CRYPTO_DEV_ROCKCHIP2_DEBUG
static int rk_crypto_debugfs_show(struct seq_file *seq, void *v)
{
	struct rk_crypto_dev *dd;
	unsigned int i;

	spin_lock(&rocklist.lock);
	list_for_each_entry(dd, &rocklist.dev_list, list) {
		seq_printf(seq, "%s %s requests: %lu\n",
			   dev_driver_string(dd->dev), dev_name(dd->dev),
			   dd->nreq);
	}
	spin_unlock(&rocklist.lock);

	for (i = 0; i < ARRAY_SIZE(rk_cipher_algs); i++) {
		if (!rk_cipher_algs[i].dev)
			continue;
		switch (rk_cipher_algs[i].type) {
		case CRYPTO_ALG_TYPE_SKCIPHER:
			seq_printf(seq, "%s %s reqs=%lu fallback=%lu\n",
				   rk_cipher_algs[i].alg.skcipher.base.cra_driver_name,
				   rk_cipher_algs[i].alg.skcipher.base.cra_name,
				   rk_cipher_algs[i].stat_req, rk_cipher_algs[i].stat_fb);
			seq_printf(seq, "\tfallback due to length: %lu\n",
				   rk_cipher_algs[i].stat_fb_len);
			seq_printf(seq, "\tfallback due to alignment: %lu\n",
				   rk_cipher_algs[i].stat_fb_align);
			seq_printf(seq, "\tfallback due to SGs: %lu\n",
				   rk_cipher_algs[i].stat_fb_sgdiff);
			break;
		case CRYPTO_ALG_TYPE_AHASH:
			seq_printf(seq, "%s %s reqs=%lu fallback=%lu\n",
				   rk_cipher_algs[i].alg.hash.halg.base.cra_driver_name,
				   rk_cipher_algs[i].alg.hash.halg.base.cra_name,
				   rk_cipher_algs[i].stat_req, rk_cipher_algs[i].stat_fb);
			break;
		}
	}
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(rk_crypto_debugfs);
#endif

static void register_debugfs(struct rk_crypto_dev *crypto_dev)
{
#ifdef CONFIG_CRYPTO_DEV_ROCKCHIP2_DEBUG
	/* Ignore error of debugfs */
	rocklist.dbgfs_dir = debugfs_create_dir("rk3588_crypto", NULL);
	rocklist.dbgfs_stats = debugfs_create_file("stats", 0444,
						   rocklist.dbgfs_dir,
						   &rocklist,
						   &rk_crypto_debugfs_fops);
#endif
}

static int rk_crypto_register(struct rk_crypto_dev *rkc)
{
	unsigned int i, k;
	int err = 0;

	for (i = 0; i < ARRAY_SIZE(rk_cipher_algs); i++) {
		rk_cipher_algs[i].dev = rkc;
		switch (rk_cipher_algs[i].type) {
		case CRYPTO_ALG_TYPE_SKCIPHER:
			dev_info(rkc->dev, "Register %s as %s\n",
				 rk_cipher_algs[i].alg.skcipher.base.cra_name,
				 rk_cipher_algs[i].alg.skcipher.base.cra_driver_name);
			err = crypto_register_skcipher(&rk_cipher_algs[i].alg.skcipher);
			break;
		case CRYPTO_ALG_TYPE_AHASH:
			dev_info(rkc->dev, "Register %s as %s\n",
				 rk_cipher_algs[i].alg.hash.halg.base.cra_name,
				 rk_cipher_algs[i].alg.hash.halg.base.cra_driver_name);
			err = crypto_register_ahash(&rk_cipher_algs[i].alg.hash);
			break;
		default:
			dev_err(rkc->dev, "unknown algorithm\n");
		}
		if (err)
			goto err_cipher_algs;
	}
	return 0;

err_cipher_algs:
	for (k = 0; k < i; k++) {
		if (rk_cipher_algs[i].type == CRYPTO_ALG_TYPE_SKCIPHER)
			crypto_unregister_skcipher(&rk_cipher_algs[k].alg.skcipher);
		else
			crypto_unregister_ahash(&rk_cipher_algs[i].alg.hash);
	}
	return err;
}

static void rk_crypto_unregister(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rk_cipher_algs); i++) {
		if (rk_cipher_algs[i].type == CRYPTO_ALG_TYPE_SKCIPHER)
			crypto_unregister_skcipher(&rk_cipher_algs[i].alg.skcipher);
		else
			crypto_unregister_ahash(&rk_cipher_algs[i].alg.hash);
	}
}

static const struct of_device_id crypto_of_id_table[] = {
	{ .compatible = "rockchip,rk3568-crypto",
	  .data = &rk3568_variant,
	},
	{ .compatible = "rockchip,rk3588-crypto",
	  .data = &rk3588_variant,
	},
	{}
};
MODULE_DEVICE_TABLE(of, crypto_of_id_table);

static int rk_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk_crypto_dev *rkc, *first;
	int err = 0;

	rkc = devm_kzalloc(&pdev->dev, sizeof(*rkc), GFP_KERNEL);
	if (!rkc) {
		err = -ENOMEM;
		goto err_crypto;
	}

	rkc->dev = &pdev->dev;
	platform_set_drvdata(pdev, rkc);

	rkc->variant = of_device_get_match_data(&pdev->dev);
	if (!rkc->variant) {
		dev_err(&pdev->dev, "Missing variant\n");
		return -EINVAL;
	}

	rkc->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rkc->rst)) {
		err = PTR_ERR(rkc->rst);
		goto err_crypto;
	}

	rkc->tl = dma_alloc_coherent(rkc->dev,
				     sizeof(struct rk_crypto_lli) * MAX_LLI,
				     &rkc->t_phy, GFP_KERNEL);
	if (!rkc->tl) {
		dev_err(rkc->dev, "Cannot get DMA memory for task\n");
		err = -ENOMEM;
		goto err_crypto;
	}

	reset_control_assert(rkc->rst);
	usleep_range(10, 20);
	reset_control_deassert(rkc->rst);

	rkc->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkc->reg)) {
		err = PTR_ERR(rkc->reg);
		goto err_crypto;
	}

	err = rk_crypto_get_clks(rkc);
	if (err)
		goto err_crypto;

	rkc->irq = platform_get_irq(pdev, 0);
	if (rkc->irq < 0) {
		dev_err(&pdev->dev, "control Interrupt is not available.\n");
		err = rkc->irq;
		goto err_crypto;
	}

	err = devm_request_irq(&pdev->dev, rkc->irq,
			       rk_crypto_irq_handle, IRQF_SHARED,
			       "rk-crypto", pdev);

	if (err) {
		dev_err(&pdev->dev, "irq request failed.\n");
		goto err_crypto;
	}

	rkc->engine = crypto_engine_alloc_init(&pdev->dev, true);
	crypto_engine_start(rkc->engine);
	init_completion(&rkc->complete);

	err = rk_crypto_pm_init(rkc);
	if (err)
		goto err_pm;

	err = pm_runtime_resume_and_get(&pdev->dev);

	spin_lock(&rocklist.lock);
	first = list_first_entry_or_null(&rocklist.dev_list,
					 struct rk_crypto_dev, list);
	list_add_tail(&rkc->list, &rocklist.dev_list);
	spin_unlock(&rocklist.lock);

	if (!first) {
		dev_info(dev, "Registers crypto algos\n");
		err = rk_crypto_register(rkc);
		if (err) {
			dev_err(dev, "Fail to register crypto algorithms");
			goto err_register_alg;
		}

		register_debugfs(rkc);
	}

	return 0;

err_register_alg:
	rk_crypto_pm_exit(rkc);
err_pm:
	crypto_engine_exit(rkc->engine);
err_crypto:
	dev_err(dev, "Crypto Accelerator not successfully registered\n");
	return err;
}

static int rk_crypto_remove(struct platform_device *pdev)
{
	struct rk_crypto_dev *crypto_tmp = platform_get_drvdata(pdev);
	struct rk_crypto_dev *first;

	spin_lock_bh(&rocklist.lock);
	list_del(&crypto_tmp->list);
	first = list_first_entry_or_null(&rocklist.dev_list,
					 struct rk_crypto_dev, list);
	spin_unlock_bh(&rocklist.lock);

	if (!first) {
#ifdef CONFIG_CRYPTO_DEV_ROCKCHIP2_DEBUG
		debugfs_remove_recursive(rocklist.dbgfs_dir);
#endif
		rk_crypto_unregister();
	}
	rk_crypto_pm_exit(crypto_tmp);
	crypto_engine_exit(crypto_tmp->engine);
	return 0;
}

static struct platform_driver crypto_driver = {
	.probe		= rk_crypto_probe,
	.remove		= rk_crypto_remove,
	.driver		= {
		.name	= "rk3588-crypto",
		.pm		= &rk_crypto_pm_ops,
		.of_match_table	= crypto_of_id_table,
	},
};

module_platform_driver(crypto_driver);

MODULE_DESCRIPTION("Rockchip Crypto Engine cryptographic offloader");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Corentin Labbe <clabbe@baylibre.com>");
