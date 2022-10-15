/* SPDX-License-Identifier: GPL-2.0 */

#include <crypto/aes.h>
#include <crypto/engine.h>
#include <crypto/internal/des.h>
#include <crypto/internal/hash.h>
#include <crypto/internal/skcipher.h>
#include <crypto/algapi.h>
#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/sm3.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>

#define RK_CRYPTO_CLK_CTL	0x0000
#define RK_CRYPTO_RST_CTL	0x0004

#define RK_CRYPTO_DMA_INT_EN	0x0008
/* values for RK_CRYPTO_DMA_INT_EN */
#define RK_CRYPTO_DMA_INT_LISTDONE	BIT(0)

#define RK_CRYPTO_DMA_INT_ST	0x000C
/* values in RK_CRYPTO_DMA_INT_ST are the same than in RK_CRYPTO_DMA_INT_EN */

#define RK_CRYPTO_DMA_CTL	0x0010
#define RK_CRYPTO_DMA_CTL_START	BIT(0)

#define RK_CRYPTO_DMA_LLI_ADDR	0x0014

#define RK_CRYPTO_FIFO_CTL	0x0040

#define RK_CRYPTO_BC_CTL	0x0044
#define RK_CRYPTO_AES		(0 << 8)
#define RK_CRYPTO_MODE_ECB	(0 << 4)
#define RK_CRYPTO_MODE_CBC	(1 << 4)

#define RK_CRYPTO_HASH_CTL	0x0048
#define RK_CRYPTO_HW_PAD	BIT(2)
#define RK_CRYPTO_SHA1		(0 << 4)
#define RK_CRYPTO_MD5		(1 << 4)
#define RK_CRYPTO_SHA224	(3 << 4)
#define RK_CRYPTO_SHA256	(2 << 4)
#define RK_CRYPTO_SHA384	(9 << 4)
#define RK_CRYPTO_SHA512	(8 << 4)
#define RK_CRYPTO_SM3		(4 << 4)

#define RK_CRYPTO_AES_ECB_MODE		(RK_CRYPTO_AES | RK_CRYPTO_MODE_ECB)
#define RK_CRYPTO_AES_CBC_MODE		(RK_CRYPTO_AES | RK_CRYPTO_MODE_CBC)
#define RK_CRYPTO_AES_CTR_MODE		3
#define RK_CRYPTO_AES_128BIT_key	(0 << 2)
#define RK_CRYPTO_AES_192BIT_key	(1 << 2)
#define RK_CRYPTO_AES_256BIT_key	(2 << 2)

#define RK_CRYPTO_DEC			BIT(1)
#define RK_CRYPTO_ENABLE		BIT(0)

#define RK_CRYPTO_CH0_IV_0		0x0100

#define RK_CRYPTO_KEY0			0x0180
#define RK_CRYPTO_KEY1			0x0184
#define RK_CRYPTO_KEY2			0x0188
#define RK_CRYPTO_KEY3			0x018C
#define RK_CRYPTO_KEY4			0x0190
#define RK_CRYPTO_KEY5			0x0194
#define RK_CRYPTO_KEY6			0x0198
#define RK_CRYPTO_KEY7			0x019C

#define RK_CRYPTO_CH0_PC_LEN_0		0x0280

#define RK_CRYPTO_CH0_IV_LEN		0x0300

#define RK_CRYPTO_HASH_DOUT_0	0x03A0
#define RK_CRYPTO_HASH_VALID	0x03E4

#define CRYPTO_AES_VERSION	0x0680
#define CRYPTO_DES_VERSION	0x0684
#define CRYPTO_SM4_VERSION	0x0688
#define CRYPTO_HASH_VERSION	0x068C
#define CRYPTO_HMAC_VERSION	0x0690
#define CRYPTO_RNG_VERSION	0x0694
#define CRYPTO_PKA_VERSION	0x0698
#define CRYPTO_CRYPTO_VERSION	0x06F0

#define RK_LLI_DMA_CTRL_SRC_INT		BIT(10)
#define RK_LLI_DMA_CTRL_DST_INT		BIT(9)
#define RK_LLI_DMA_CTRL_LIST_INT	BIT(8)
#define RK_LLI_DMA_CTRL_LAST		BIT(0)

#define RK_LLI_STRING_LAST		BIT(2)
#define RK_LLI_STRING_FIRST		BIT(1)
#define RK_LLI_CIPHER_START		BIT(0)

#define RK_MAX_CLKS 4

/* there are no hw limit, but we need to choose a maximum of descriptor to allocate */
#define MAX_LLI 20

struct rk_crypto_lli {
	__le32 src_addr;
	__le32 src_len;
	__le32 dst_addr;
	__le32 dst_len;
	__le32 user;
	__le32 iv;
	__le32 dma_ctrl;
	__le32 next;
};

/*
 * struct rockchip_ip - struct for managing a list of RK crypto instance
 * @dev_list:		Used for doing a list of rk_crypto_dev
 * @lock:		Control access to dev_list
 * @dbgfs_dir:		Debugfs dentry for statistic directory
 * @dbgfs_stats:	Debugfs dentry for statistic counters
 */
struct rockchip_ip {
	struct list_head	dev_list;
	spinlock_t		lock; /* Control access to dev_list */
	struct dentry		*dbgfs_dir;
	struct dentry		*dbgfs_stats;
};

struct rk_clks {
	const char *name;
	unsigned long max;
};

struct rk_variant {
	int num_clks;
	struct rk_clks rkclks[RK_MAX_CLKS];
};

struct rk_crypto_dev {
	struct list_head		list;
	struct device			*dev;
	struct clk_bulk_data		*clks;
	int				num_clks;
	struct reset_control		*rst;
	void __iomem			*reg;
	int				irq;
	const struct rk_variant *variant;
	unsigned long nreq;
	struct crypto_engine *engine;
	struct completion complete;
	int status;
	struct rk_crypto_lli *tl;
	dma_addr_t t_phy;
};

/* the private variable of hash */
struct rk_ahash_ctx {
	struct crypto_engine_ctx enginectx;
	/* for fallback */
	struct crypto_ahash		*fallback_tfm;
};

/* the private variable of hash for fallback */
struct rk_ahash_rctx {
	struct rk_crypto_dev		*dev;
	struct ahash_request		fallback_req;
	u32				mode;
	int nrsgs;
};

/* the private variable of cipher */
struct rk_cipher_ctx {
	struct crypto_engine_ctx enginectx;
	unsigned int			keylen;
	u8				key[AES_MAX_KEY_SIZE];
	u8				iv[AES_BLOCK_SIZE];
	struct crypto_skcipher *fallback_tfm;
};

struct rk_cipher_rctx {
	struct rk_crypto_dev		*dev;
	u8 backup_iv[AES_BLOCK_SIZE];
	u32				mode;
	struct skcipher_request fallback_req;   // keep at the end
};

struct rk_crypto_template {
	u32 type;
	u32 rk_mode;
	struct rk_crypto_dev           *dev;
	union {
		struct skcipher_alg	skcipher;
		struct ahash_alg	hash;
	} alg;
	unsigned long stat_req;
	unsigned long stat_fb;
	unsigned long stat_fb_len;
	unsigned long stat_fb_sglen;
	unsigned long stat_fb_align;
	unsigned long stat_fb_sgdiff;
};

struct rk_crypto_dev *get_rk_crypto(void);

int rk_cipher_tfm_init(struct crypto_skcipher *tfm);
void rk_cipher_tfm_exit(struct crypto_skcipher *tfm);
int rk_aes_setkey(struct crypto_skcipher *cipher, const u8 *key,
		  unsigned int keylen);
int rk_aes_ecb_encrypt(struct skcipher_request *req);
int rk_aes_ecb_decrypt(struct skcipher_request *req);
int rk_aes_cbc_encrypt(struct skcipher_request *req);
int rk_aes_cbc_decrypt(struct skcipher_request *req);

int rk_ahash_init(struct ahash_request *req);
int rk_ahash_update(struct ahash_request *req);
int rk_ahash_final(struct ahash_request *req);
int rk_ahash_finup(struct ahash_request *req);
int rk_ahash_import(struct ahash_request *req, const void *in);
int rk_ahash_export(struct ahash_request *req, void *out);
int rk_ahash_digest(struct ahash_request *req);
int rk_cra_hash_init(struct crypto_tfm *tfm);
void rk_cra_hash_exit(struct crypto_tfm *tfm);
