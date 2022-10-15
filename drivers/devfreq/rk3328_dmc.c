// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd.
 * Author: Lin Huang <hl@rock-chips.com>
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include <linux/devfreq_cooling.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/rwsem.h>
#include <linux/suspend.h>

#include <soc/rockchip/rockchip_sip.h>

#define DTS_PAR_OFFSET		(4096)

struct share_params {
	u32 hz;
	u32 lcdc_type;
	u32 vop;
	u32 vop_dclk_mode;
	u32 sr_idle_en;
	u32 addr_mcu_el3;
	/*
	 * 1: need to wait flag1
	 * 0: never wait flag1
	 */
	u32 wait_flag1;
	/*
	 * 1: need to wait flag1
	 * 0: never wait flag1
	 */
	u32 wait_flag0;
	u32 complt_hwirq;
	/* if need, add parameter after */
};

static struct share_params *ddr_psci_param;

/* hope this define can adapt all future platform */
static const char * const rk3328_dts_timing[] = {
	"ddr3_speed_bin",
	"ddr4_speed_bin",
	"pd_idle",
	"sr_idle",
	"sr_mc_gate_idle",
	"srpd_lite_idle",
	"standby_idle",

	"auto_pd_dis_freq",
	"auto_sr_dis_freq",
	"ddr3_dll_dis_freq",
	"ddr4_dll_dis_freq",
	"phy_dll_dis_freq",

	"ddr3_odt_dis_freq",
	"phy_ddr3_odt_dis_freq",
	"ddr3_drv",
	"ddr3_odt",
	"phy_ddr3_ca_drv",
	"phy_ddr3_ck_drv",
	"phy_ddr3_dq_drv",
	"phy_ddr3_odt",

	"lpddr3_odt_dis_freq",
	"phy_lpddr3_odt_dis_freq",
	"lpddr3_drv",
	"lpddr3_odt",
	"phy_lpddr3_ca_drv",
	"phy_lpddr3_ck_drv",
	"phy_lpddr3_dq_drv",
	"phy_lpddr3_odt",

	"lpddr4_odt_dis_freq",
	"phy_lpddr4_odt_dis_freq",
	"lpddr4_drv",
	"lpddr4_dq_odt",
	"lpddr4_ca_odt",
	"phy_lpddr4_ca_drv",
	"phy_lpddr4_ck_cs_drv",
	"phy_lpddr4_dq_drv",
	"phy_lpddr4_odt",

	"ddr4_odt_dis_freq",
	"phy_ddr4_odt_dis_freq",
	"ddr4_drv",
	"ddr4_odt",
	"phy_ddr4_ca_drv",
	"phy_ddr4_ck_drv",
	"phy_ddr4_dq_drv",
	"phy_ddr4_odt",
};

static const char * const rk3328_dts_ca_timing[] = {
	"ddr3a1_ddr4a9_de-skew",
	"ddr3a0_ddr4a10_de-skew",
	"ddr3a3_ddr4a6_de-skew",
	"ddr3a2_ddr4a4_de-skew",
	"ddr3a5_ddr4a8_de-skew",
	"ddr3a4_ddr4a5_de-skew",
	"ddr3a7_ddr4a11_de-skew",
	"ddr3a6_ddr4a7_de-skew",
	"ddr3a9_ddr4a0_de-skew",
	"ddr3a8_ddr4a13_de-skew",
	"ddr3a11_ddr4a3_de-skew",
	"ddr3a10_ddr4cs0_de-skew",
	"ddr3a13_ddr4a2_de-skew",
	"ddr3a12_ddr4ba1_de-skew",
	"ddr3a15_ddr4odt0_de-skew",
	"ddr3a14_ddr4a1_de-skew",
	"ddr3ba1_ddr4a15_de-skew",
	"ddr3ba0_ddr4bg0_de-skew",
	"ddr3ras_ddr4cke_de-skew",
	"ddr3ba2_ddr4ba0_de-skew",
	"ddr3we_ddr4bg1_de-skew",
	"ddr3cas_ddr4a12_de-skew",
	"ddr3ckn_ddr4ckn_de-skew",
	"ddr3ckp_ddr4ckp_de-skew",
	"ddr3cke_ddr4a16_de-skew",
	"ddr3odt0_ddr4a14_de-skew",
	"ddr3cs0_ddr4act_de-skew",
	"ddr3reset_ddr4reset_de-skew",
	"ddr3cs1_ddr4cs1_de-skew",
	"ddr3odt1_ddr4odt1_de-skew",
};

static const char * const rk3328_dts_cs0_timing[] = {
	"cs0_dm0_rx_de-skew",
	"cs0_dm0_tx_de-skew",
	"cs0_dq0_rx_de-skew",
	"cs0_dq0_tx_de-skew",
	"cs0_dq1_rx_de-skew",
	"cs0_dq1_tx_de-skew",
	"cs0_dq2_rx_de-skew",
	"cs0_dq2_tx_de-skew",
	"cs0_dq3_rx_de-skew",
	"cs0_dq3_tx_de-skew",
	"cs0_dq4_rx_de-skew",
	"cs0_dq4_tx_de-skew",
	"cs0_dq5_rx_de-skew",
	"cs0_dq5_tx_de-skew",
	"cs0_dq6_rx_de-skew",
	"cs0_dq6_tx_de-skew",
	"cs0_dq7_rx_de-skew",
	"cs0_dq7_tx_de-skew",
	"cs0_dqs0_rx_de-skew",
	"cs0_dqs0p_tx_de-skew",
	"cs0_dqs0n_tx_de-skew",

	"cs0_dm1_rx_de-skew",
	"cs0_dm1_tx_de-skew",
	"cs0_dq8_rx_de-skew",
	"cs0_dq8_tx_de-skew",
	"cs0_dq9_rx_de-skew",
	"cs0_dq9_tx_de-skew",
	"cs0_dq10_rx_de-skew",
	"cs0_dq10_tx_de-skew",
	"cs0_dq11_rx_de-skew",
	"cs0_dq11_tx_de-skew",
	"cs0_dq12_rx_de-skew",
	"cs0_dq12_tx_de-skew",
	"cs0_dq13_rx_de-skew",
	"cs0_dq13_tx_de-skew",
	"cs0_dq14_rx_de-skew",
	"cs0_dq14_tx_de-skew",
	"cs0_dq15_rx_de-skew",
	"cs0_dq15_tx_de-skew",
	"cs0_dqs1_rx_de-skew",
	"cs0_dqs1p_tx_de-skew",
	"cs0_dqs1n_tx_de-skew",

	"cs0_dm2_rx_de-skew",
	"cs0_dm2_tx_de-skew",
	"cs0_dq16_rx_de-skew",
	"cs0_dq16_tx_de-skew",
	"cs0_dq17_rx_de-skew",
	"cs0_dq17_tx_de-skew",
	"cs0_dq18_rx_de-skew",
	"cs0_dq18_tx_de-skew",
	"cs0_dq19_rx_de-skew",
	"cs0_dq19_tx_de-skew",
	"cs0_dq20_rx_de-skew",
	"cs0_dq20_tx_de-skew",
	"cs0_dq21_rx_de-skew",
	"cs0_dq21_tx_de-skew",
	"cs0_dq22_rx_de-skew",
	"cs0_dq22_tx_de-skew",
	"cs0_dq23_rx_de-skew",
	"cs0_dq23_tx_de-skew",
	"cs0_dqs2_rx_de-skew",
	"cs0_dqs2p_tx_de-skew",
	"cs0_dqs2n_tx_de-skew",

	"cs0_dm3_rx_de-skew",
	"cs0_dm3_tx_de-skew",
	"cs0_dq24_rx_de-skew",
	"cs0_dq24_tx_de-skew",
	"cs0_dq25_rx_de-skew",
	"cs0_dq25_tx_de-skew",
	"cs0_dq26_rx_de-skew",
	"cs0_dq26_tx_de-skew",
	"cs0_dq27_rx_de-skew",
	"cs0_dq27_tx_de-skew",
	"cs0_dq28_rx_de-skew",
	"cs0_dq28_tx_de-skew",
	"cs0_dq29_rx_de-skew",
	"cs0_dq29_tx_de-skew",
	"cs0_dq30_rx_de-skew",
	"cs0_dq30_tx_de-skew",
	"cs0_dq31_rx_de-skew",
	"cs0_dq31_tx_de-skew",
	"cs0_dqs3_rx_de-skew",
	"cs0_dqs3p_tx_de-skew",
	"cs0_dqs3n_tx_de-skew",
};

static const char * const rk3328_dts_cs1_timing[] = {
	"cs1_dm0_rx_de-skew",
	"cs1_dm0_tx_de-skew",
	"cs1_dq0_rx_de-skew",
	"cs1_dq0_tx_de-skew",
	"cs1_dq1_rx_de-skew",
	"cs1_dq1_tx_de-skew",
	"cs1_dq2_rx_de-skew",
	"cs1_dq2_tx_de-skew",
	"cs1_dq3_rx_de-skew",
	"cs1_dq3_tx_de-skew",
	"cs1_dq4_rx_de-skew",
	"cs1_dq4_tx_de-skew",
	"cs1_dq5_rx_de-skew",
	"cs1_dq5_tx_de-skew",
	"cs1_dq6_rx_de-skew",
	"cs1_dq6_tx_de-skew",
	"cs1_dq7_rx_de-skew",
	"cs1_dq7_tx_de-skew",
	"cs1_dqs0_rx_de-skew",
	"cs1_dqs0p_tx_de-skew",
	"cs1_dqs0n_tx_de-skew",

	"cs1_dm1_rx_de-skew",
	"cs1_dm1_tx_de-skew",
	"cs1_dq8_rx_de-skew",
	"cs1_dq8_tx_de-skew",
	"cs1_dq9_rx_de-skew",
	"cs1_dq9_tx_de-skew",
	"cs1_dq10_rx_de-skew",
	"cs1_dq10_tx_de-skew",
	"cs1_dq11_rx_de-skew",
	"cs1_dq11_tx_de-skew",
	"cs1_dq12_rx_de-skew",
	"cs1_dq12_tx_de-skew",
	"cs1_dq13_rx_de-skew",
	"cs1_dq13_tx_de-skew",
	"cs1_dq14_rx_de-skew",
	"cs1_dq14_tx_de-skew",
	"cs1_dq15_rx_de-skew",
	"cs1_dq15_tx_de-skew",
	"cs1_dqs1_rx_de-skew",
	"cs1_dqs1p_tx_de-skew",
	"cs1_dqs1n_tx_de-skew",

	"cs1_dm2_rx_de-skew",
	"cs1_dm2_tx_de-skew",
	"cs1_dq16_rx_de-skew",
	"cs1_dq16_tx_de-skew",
	"cs1_dq17_rx_de-skew",
	"cs1_dq17_tx_de-skew",
	"cs1_dq18_rx_de-skew",
	"cs1_dq18_tx_de-skew",
	"cs1_dq19_rx_de-skew",
	"cs1_dq19_tx_de-skew",
	"cs1_dq20_rx_de-skew",
	"cs1_dq20_tx_de-skew",
	"cs1_dq21_rx_de-skew",
	"cs1_dq21_tx_de-skew",
	"cs1_dq22_rx_de-skew",
	"cs1_dq22_tx_de-skew",
	"cs1_dq23_rx_de-skew",
	"cs1_dq23_tx_de-skew",
	"cs1_dqs2_rx_de-skew",
	"cs1_dqs2p_tx_de-skew",
	"cs1_dqs2n_tx_de-skew",

	"cs1_dm3_rx_de-skew",
	"cs1_dm3_tx_de-skew",
	"cs1_dq24_rx_de-skew",
	"cs1_dq24_tx_de-skew",
	"cs1_dq25_rx_de-skew",
	"cs1_dq25_tx_de-skew",
	"cs1_dq26_rx_de-skew",
	"cs1_dq26_tx_de-skew",
	"cs1_dq27_rx_de-skew",
	"cs1_dq27_tx_de-skew",
	"cs1_dq28_rx_de-skew",
	"cs1_dq28_tx_de-skew",
	"cs1_dq29_rx_de-skew",
	"cs1_dq29_tx_de-skew",
	"cs1_dq30_rx_de-skew",
	"cs1_dq30_tx_de-skew",
	"cs1_dq31_rx_de-skew",
	"cs1_dq31_tx_de-skew",
	"cs1_dqs3_rx_de-skew",
	"cs1_dqs3p_tx_de-skew",
	"cs1_dqs3n_tx_de-skew",
};

struct rk3328_ddr_dts_config_timing {
	unsigned int ddr3_speed_bin;
	unsigned int ddr4_speed_bin;
	unsigned int pd_idle;
	unsigned int sr_idle;
	unsigned int sr_mc_gate_idle;
	unsigned int srpd_lite_idle;
	unsigned int standby_idle;

	unsigned int auto_pd_dis_freq;
	unsigned int auto_sr_dis_freq;
	/* for ddr3 only */
	unsigned int ddr3_dll_dis_freq;
	/* for ddr4 only */
	unsigned int ddr4_dll_dis_freq;
	unsigned int phy_dll_dis_freq;

	unsigned int ddr3_odt_dis_freq;
	unsigned int phy_ddr3_odt_dis_freq;
	unsigned int ddr3_drv;
	unsigned int ddr3_odt;
	unsigned int phy_ddr3_ca_drv;
	unsigned int phy_ddr3_ck_drv;
	unsigned int phy_ddr3_dq_drv;
	unsigned int phy_ddr3_odt;

	unsigned int lpddr3_odt_dis_freq;
	unsigned int phy_lpddr3_odt_dis_freq;
	unsigned int lpddr3_drv;
	unsigned int lpddr3_odt;
	unsigned int phy_lpddr3_ca_drv;
	unsigned int phy_lpddr3_ck_drv;
	unsigned int phy_lpddr3_dq_drv;
	unsigned int phy_lpddr3_odt;

	unsigned int lpddr4_odt_dis_freq;
	unsigned int phy_lpddr4_odt_dis_freq;
	unsigned int lpddr4_drv;
	unsigned int lpddr4_dq_odt;
	unsigned int lpddr4_ca_odt;
	unsigned int phy_lpddr4_ca_drv;
	unsigned int phy_lpddr4_ck_cs_drv;
	unsigned int phy_lpddr4_dq_drv;
	unsigned int phy_lpddr4_odt;

	unsigned int ddr4_odt_dis_freq;
	unsigned int phy_ddr4_odt_dis_freq;
	unsigned int ddr4_drv;
	unsigned int ddr4_odt;
	unsigned int phy_ddr4_ca_drv;
	unsigned int phy_ddr4_ck_drv;
	unsigned int phy_ddr4_dq_drv;
	unsigned int phy_ddr4_odt;

	unsigned int ca_skew[15];
	unsigned int cs0_skew[44];
	unsigned int cs1_skew[44];

	unsigned int available;
};

struct rk3328_ddr_de_skew_setting {
	unsigned int ca_de_skew[30];
	unsigned int cs0_de_skew[84];
	unsigned int cs1_de_skew[84];
};

struct rk3328_devfreq {
	struct devfreq *devfreq;
	struct opp_table *clkname_opp_table;
	struct opp_table *regulators_opp_table;
	struct thermal_cooling_device *cooling;
	bool opp_of_table_added;
};

struct rk3328_dmcfreq {
	struct device *dev;
	//struct devfreq *devfreq;
	struct devfreq_simple_ondemand_data ondemand_data;
	struct clk *dmc_clk;
	struct devfreq_event_dev *edev;
	struct mutex lock;
	struct regulator *vdd_center;
	struct rk3328_devfreq devfreq;
	unsigned long rate, target_rate;
	unsigned long volt, target_volt;

	int (*set_auto_self_refresh)(u32 en);
};

static void
rk3328_de_skew_setting_2_register(struct rk3328_ddr_de_skew_setting *de_skew,
				  struct rk3328_ddr_dts_config_timing *tim)
{
	u32 n;
	u32 offset;
	u32 shift;

	memset_io(tim->ca_skew, 0, sizeof(tim->ca_skew));
	memset_io(tim->cs0_skew, 0, sizeof(tim->cs0_skew));
	memset_io(tim->cs1_skew, 0, sizeof(tim->cs1_skew));

	/* CA de-skew */
	for (n = 0; n < ARRAY_SIZE(de_skew->ca_de_skew); n++) {
		offset = n / 2;
		shift = n % 2;
		/* 0 => 4; 1 => 0 */
		shift = (shift == 0) ? 4 : 0;
		tim->ca_skew[offset] &= ~(0xf << shift);
		tim->ca_skew[offset] |= (de_skew->ca_de_skew[n] << shift);
	}

	/* CS0 data de-skew */
	for (n = 0; n < ARRAY_SIZE(de_skew->cs0_de_skew); n++) {
		offset = ((n / 21) * 11) + ((n % 21) / 2);
		shift = ((n % 21) % 2);
		if ((n % 21) == 20)
			shift = 0;
		else
			/* 0 => 4; 1 => 0 */
			shift = (shift == 0) ? 4 : 0;
		tim->cs0_skew[offset] &= ~(0xf << shift);
		tim->cs0_skew[offset] |= (de_skew->cs0_de_skew[n] << shift);
	}

	/* CS1 data de-skew */
	for (n = 0; n < ARRAY_SIZE(de_skew->cs1_de_skew); n++) {
		offset = ((n / 21) * 11) + ((n % 21) / 2);
		shift = ((n % 21) % 2);
		if ((n % 21) == 20)
			shift = 0;
		else
			/* 0 => 4; 1 => 0 */
			shift = (shift == 0) ? 4 : 0;
		tim->cs1_skew[offset] &= ~(0xf << shift);
		tim->cs1_skew[offset] |= (de_skew->cs1_de_skew[n] << shift);
	}
}

static void of_get_rk3328_timings(struct device *dev,
				  struct device_node *np, uint32_t *timing)
{
	struct device_node *np_tim;
	u32 *p;
	struct rk3328_ddr_dts_config_timing *dts_timing;
	struct rk3328_ddr_de_skew_setting *de_skew;
	int ret = 0;
	u32 i;

	dts_timing =
		(struct rk3328_ddr_dts_config_timing *)(timing +
							DTS_PAR_OFFSET / 4);

	np_tim = of_parse_phandle(np, "ddr_timing", 0);
	if (!np_tim) {
		ret = -EINVAL;
		goto end;
	}
	de_skew = kmalloc(sizeof(*de_skew), GFP_KERNEL);
	if (!de_skew) {
		ret = -ENOMEM;
		goto end;
	}

	p = (u32 *)dts_timing;
	for (i = 0; i < ARRAY_SIZE(rk3328_dts_timing); i++) {
		ret |= of_property_read_u32(np_tim, rk3328_dts_timing[i],
					p + i);
	}
	p = (u32 *)de_skew->ca_de_skew;
	for (i = 0; i < ARRAY_SIZE(rk3328_dts_ca_timing); i++) {
		ret |= of_property_read_u32(np_tim, rk3328_dts_ca_timing[i],
					p + i);
	}
	p = (u32 *)de_skew->cs0_de_skew;
	for (i = 0; i < ARRAY_SIZE(rk3328_dts_cs0_timing); i++) {
		ret |= of_property_read_u32(np_tim, rk3328_dts_cs0_timing[i],
					p + i);
	}
	p = (u32 *)de_skew->cs1_de_skew;
	for (i = 0; i < ARRAY_SIZE(rk3328_dts_cs1_timing); i++) {
		ret |= of_property_read_u32(np_tim, rk3328_dts_cs1_timing[i],
					p + i);
	}
	if (!ret)
		rk3328_de_skew_setting_2_register(de_skew, dts_timing);

	kfree(de_skew);
end:
	if (!ret) {
		dts_timing->available = 1;
	} else {
		dts_timing->available = 0;
		dev_err(dev, "of_get_ddr_timings: fail\n");
	}

	of_node_put(np_tim);
}

static int rockchip_ddr_set_auto_self_refresh(uint32_t en)
{
	struct arm_smccc_res res;

	ddr_psci_param->sr_idle_en = en;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ,
		      SHARE_PAGE_TYPE_DDR, 0, ROCKCHIP_SIP_CONFIG_DRAM_SET_AT_SR,
		      0, 0, 0, 0, &res);

	return res.a0;
}

static int rk3328_dmc_init(struct platform_device *pdev,
			   struct rk3328_dmcfreq *dmcfreq)
{
	struct arm_smccc_res res;
	u32 size, page_num;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ,
		      0, 0, ROCKCHIP_SIP_CONFIG_DRAM_GET_VERSION,
		      0, 0, 0, 0, &res);
	if (res.a0 || (res.a1 < 0x101)) {
		dev_err(&pdev->dev,
			"trusted firmware need to update or is invalid\n");
		return -ENXIO;
	}

	dev_notice(&pdev->dev, "current ATF version 0x%lx\n", res.a1);

	/*
	 * first 4KB is used for interface parameters
	 * after 4KB * N is dts parameters
	 */
	size = sizeof(struct rk3328_ddr_dts_config_timing);
	page_num = DIV_ROUND_UP(size, 4096) + 1;

	arm_smccc_smc(ROCKCHIP_SIP_SHARE_MEM,
		      page_num, SHARE_PAGE_TYPE_DDR, 0,
		      0, 0, 0, 0, &res);
	if (res.a0 != 0) {
		dev_err(&pdev->dev, "no ATF memory for init\n");
		return -ENOMEM;
	}

	ddr_psci_param = ioremap(res.a1, page_num << 12);
	of_get_rk3328_timings(&pdev->dev, pdev->dev.of_node,
			      (uint32_t *)ddr_psci_param);

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ,
		      SHARE_PAGE_TYPE_DDR, 0, ROCKCHIP_SIP_CONFIG_DRAM_INIT,
		      0, 0, 0, 0, &res);
	if (res.a0) {
		dev_err(&pdev->dev, "Rockchip dram init error %lx\n", res.a0);
		return -ENOMEM;
	}

	dmcfreq->set_auto_self_refresh = rockchip_ddr_set_auto_self_refresh;

	return 0;
}

static int rk3328_dmcfreq_target(struct device *dev, unsigned long *freq,
				 u32 flags)
{
	struct rk3328_dmcfreq *rdev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	err = dev_pm_opp_set_rate(dev, *freq);
	if (err)
		return err;

	rdev->rate = *freq;

	return 0;
}

static int rk3328_dmcfreq_get_dev_status(struct device *dev,
					 struct devfreq_dev_status *stat)
{
	struct rk3328_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	struct devfreq_event_data edata;
	int ret = 0;

	ret = devfreq_event_get_event(dmcfreq->edev, &edata);
	if (ret < 0)
		return ret;

	stat->current_frequency = dmcfreq->rate;
	stat->busy_time = edata.load_count;
	stat->total_time = edata.total_count;

	return ret;
}

static int rk3328_dmcfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct rk3328_dmcfreq *dmcfreq = dev_get_drvdata(dev);

	*freq = dmcfreq->rate;

	return 0;
}

static struct devfreq_dev_profile rk3328_devfreq_dmc_profile = {
	.polling_ms	= 50,
	.target		= rk3328_dmcfreq_target,
	.get_dev_status	= rk3328_dmcfreq_get_dev_status,
	.get_cur_freq	= rk3328_dmcfreq_get_cur_freq,
};

static __maybe_unused int rk3328_dmcfreq_suspend(struct device *dev)
{
	struct rk3328_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	int ret = 0;

	ret = devfreq_event_disable_edev(dmcfreq->edev);
	if (ret < 0) {
		dev_err(dev, "failed to disable the devfreq-event devices\n");
		return ret;
	}

	ret = devfreq_suspend_device(dmcfreq->devfreq.devfreq);
	if (ret < 0) {
		dev_err(dev, "failed to suspend the devfreq devices\n");
		return ret;
	}

	return 0;
}

static __maybe_unused int rk3328_dmcfreq_resume(struct device *dev)
{
	struct rk3328_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	int ret = 0;

	ret = devfreq_event_enable_edev(dmcfreq->edev);
	if (ret < 0) {
		dev_err(dev, "failed to enable the devfreq-event devices\n");
		return ret;
	}

	ret = devfreq_resume_device(dmcfreq->devfreq.devfreq);
	if (ret < 0) {
		dev_err(dev, "failed to resume the devfreq devices\n");
		return ret;
	}
	return ret;
}

static SIMPLE_DEV_PM_OPS(rk3328_dmcfreq_pm, rk3328_dmcfreq_suspend,
			 rk3328_dmcfreq_resume);

void rk3328_devfreq_fini(struct rk3328_dmcfreq *rdev)
{
	struct rk3328_devfreq *devfreq = &rdev->devfreq;

	if (devfreq->cooling) {
		devfreq_cooling_unregister(devfreq->cooling);
		devfreq->cooling = NULL;
	}

	if (devfreq->devfreq) {
		devm_devfreq_remove_device(rdev->dev, devfreq->devfreq);
		devfreq->devfreq = NULL;
	}

	if (devfreq->opp_of_table_added) {
		dev_pm_opp_of_remove_table(rdev->dev);
		devfreq->opp_of_table_added = false;
	}

	if (devfreq->regulators_opp_table) {
		dev_pm_opp_put_regulators(devfreq->regulators_opp_table);
		devfreq->regulators_opp_table = NULL;
	}

	if (devfreq->clkname_opp_table) {
		dev_pm_opp_put_clkname(devfreq->clkname_opp_table);
		devfreq->clkname_opp_table = NULL;
	}
}

int rk3328_devfreq_init(struct rk3328_dmcfreq *rdev)
{
	struct thermal_cooling_device *cooling;
	struct device *dev = rdev->dev;
	struct opp_table *opp_table;
	struct devfreq *devfreq;
	struct rk3328_devfreq *rdevfreq = &rdev->devfreq;

	struct dev_pm_opp *opp;
	unsigned long cur_freq;
	int ret;
	const char *reg_name[] = { "center" }; 

	if (!device_property_present(dev, "operating-points-v2"))
		/* Optional, continue without devfreq */
		return 0;

	opp_table = dev_pm_opp_set_clkname(dev, "dmc_clk");
	if (IS_ERR(opp_table)) {
		ret = PTR_ERR(opp_table);
		goto err_fini;
	}

	rdevfreq->clkname_opp_table = opp_table;

	opp_table = dev_pm_opp_set_regulators(dev, &reg_name[0]);
	if (IS_ERR(opp_table)) {
		ret = PTR_ERR(opp_table);

		/* Continue if the optional regulator is missing */
		if (ret != -ENODEV)
			goto err_fini;
	} else {
		rdevfreq->regulators_opp_table = opp_table;
	}

	ret = dev_pm_opp_of_add_table(dev);
	if (ret)
		goto err_fini;
	rdevfreq->opp_of_table_added = true;

	cur_freq = 0;

	opp = devfreq_recommended_opp(dev, &cur_freq, 0);
	if (IS_ERR(opp)) {
		ret = PTR_ERR(opp);
		goto err_fini;
	}

	rk3328_devfreq_dmc_profile.initial_freq = cur_freq;
	dev_pm_opp_put(opp);

	rdev->ondemand_data.upthreshold = 15;
	rdev->ondemand_data.downdifferential = 5;

	devfreq = devm_devfreq_add_device(dev, &rk3328_devfreq_dmc_profile,
					  DEVFREQ_GOV_SIMPLE_ONDEMAND, &rdev->ondemand_data);
	if (IS_ERR(devfreq)) {
		dev_err(dev, "Couldn't initialize rk3328-dmc devfreq\n");
		ret = PTR_ERR(devfreq);
		goto err_fini;
	}

	rdevfreq->devfreq = devfreq;

	cooling = of_devfreq_cooling_register(dev->of_node, devfreq);
	if (IS_ERR(cooling))
		dev_warn(dev, "Failed to register cooling device\n");
	else
		rdevfreq->cooling = cooling;

	return 0;

err_fini:
	rk3328_devfreq_fini(rdev);
	return ret;
}

static int rk3328_dmcfreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk3328_dmcfreq *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(struct rk3328_dmcfreq), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);

	data->dev = dev;

	data->dmc_clk = devm_clk_get(dev, "dmc_clk");
	if (IS_ERR(data->dmc_clk)) {
		if (PTR_ERR(data->dmc_clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_err(dev, "Cannot get the clk dmc_clk\n");
		return PTR_ERR(data->dmc_clk);
	}

	data->edev = devfreq_event_get_edev_by_phandle(dev, "devfreq-events", 0);
	if (IS_ERR(data->edev))
		return -EPROBE_DEFER;

	ret = devfreq_event_enable_edev(data->edev);
	if (ret < 0) {
		dev_err(dev, "failed to enable devfreq-event devices\n");
		return ret;
	}

	ret = rk3328_dmc_init(pdev, data);
	if (ret)
		return ret;

	ret = rk3328_devfreq_init(data);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, data);

	return 0;

}

static int rk3328_dmcfreq_remove(struct platform_device *pdev)
{
	struct rk3328_dmcfreq *dmcfreq = dev_get_drvdata(&pdev->dev);

	/*
	 * Before remove the opp table we need to unregister the opp notifier.
	 */
	rk3328_devfreq_fini(dmcfreq);

	return 0;
}

static const struct of_device_id rk3328dmc_devfreq_of_match[] = {
	{ .compatible = "rockchip,rk3328-dmc" },
	{ },
};
MODULE_DEVICE_TABLE(of, rk3328dmc_devfreq_of_match);

static struct platform_driver rk3328_dmcfreq_driver = {
	.probe	= rk3328_dmcfreq_probe,
	.remove = rk3328_dmcfreq_remove,
	.driver = {
		.name	= "rk3328-dmc",
		.pm	= &rk3328_dmcfreq_pm,
		.of_match_table = rk3328dmc_devfreq_of_match,
	},
};
module_platform_driver(rk3328_dmcfreq_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lin Huang <hl@rock-chips.com>");
MODULE_DESCRIPTION("RK3328 dmcfreq driver with devfreq framework");
