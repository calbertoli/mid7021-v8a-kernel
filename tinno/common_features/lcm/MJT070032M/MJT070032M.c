// SPDX-License-Identifier: GPL-2.0
/*
 * Genuine MID7021 MJT070032M 1024x600 MIPI-DSI panel driver.
 *
 * Stock ARMv7 evidence:
 *   lcm_init          0xc080eb98 -> table 0xc1ac44a4, 60 records
 *   lcm_suspend       0xc080ebd0 -> table 0xc1ac6c04, 5 records
 *   lcm_suspend_power 0xc080ec54
 *   lcm_resume_power  0xc080ed54
 */
#ifndef BUILD_LK
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#else
#include <platform/mt_gpio.h>
#include <string.h>
#endif

#include "lcm_drv.h"

#define FRAME_WIDTH                 1024
#define FRAME_HEIGHT                600
#define LCM_PHYSICAL_WIDTH_UM       154000
#define LCM_PHYSICAL_HEIGHT_UM      90000
#define LCM_DENSITY                 170

/* These are the exact stock table sentinels decoded from push_table(). */
#define REGFLAG_DELAY               0xFE
#define REGFLAG_END_OF_TABLE        0xFD
#define LCM_ARRAY_SIZE(a)           (sizeof(a) / sizeof((a)[0]))

struct LCM_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[64];
};

static struct LCM_UTIL_FUNCS lcm_util;

#define MDELAY(n) lcm_util.mdelay(n)
#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
	lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define read_reg_v2(cmd, buffer, size) \
	lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, size)

static struct LCM_setting_table lcm_compare_id_setting[] = {
	{0x30, 1, {0x00}},
	{0xF7, 4, {0x49, 0x61, 0x02, 0x00}},
	{0x30, 1, {0x01}},
};

static struct LCM_setting_table lcm_initialization_setting[] = {
	{0x30, 1, {0x00}},
	{0xF7, 4, {0x49, 0x61, 0x02, 0x00}},
	{0x30, 1, {0x01}},
	{0x08, 1, {0x08}},
	{0x04, 1, {0x0E}},
	{0x0B, 1, {0x10}},
	{0x1F, 1, {0x03}},
	{0x23, 1, {0x38}},
	{0x28, 1, {0x18}},
	{0x29, 1, {0x29}},
	{0x2A, 1, {0x01}},
	{0x2B, 1, {0x29}},
	{0x2C, 1, {0x01}},
	{0x30, 1, {0x02}},
	{0x00, 1, {0x05}},
	{0x01, 1, {0x22}},
	{0x02, 1, {0x08}},
	{0x03, 1, {0x12}},
	{0x04, 1, {0x16}},
	{0x05, 1, {0x64}},
	{0x06, 1, {0x00}},
	{0x07, 1, {0x00}},
	{0x08, 1, {0x78}},
	{0x09, 1, {0x00}},
	{0x0A, 1, {0x04}},
	{0x0B, 11, {0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09}},
	{0x0C, 11, {0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}},
	{0x0D, 11, {0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08}},
	{0x0E, 11, {0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}},
	{0x0F, 11, {0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F}},
	{0x10, 11, {0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}},
	{0x11, 11, {0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E}},
	{0x12, 11, {0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}},
	{0x13, 4, {0x00, 0x00, 0x00, 0x00}},
	{0x14, 4, {0x00, 0x00, 0x41, 0x41}},
	{0x15, 4, {0x00, 0x00, 0x00, 0x00}},
	{0x17, 1, {0x00}},
	{0x18, 1, {0x85}},
	{0x19, 2, {0x06, 0x09}},
	{0x1A, 2, {0x05, 0x08}},
	{0x1B, 2, {0x0A, 0x04}},
	{0x26, 1, {0x00}},
	{0x27, 1, {0x00}},
	{0x30, 1, {0x06}},
	{0x12, 14, {0x3F, 0x27, 0x28, 0x35, 0x1B, 0x17, 0x16, 0x13, 0x10, 0x01, 0x23, 0x1B, 0x10, 0x30}},
	{0x13, 14, {0x3F, 0x27, 0x28, 0x35, 0x1D, 0x18, 0x16, 0x13, 0x10, 0x02, 0x24, 0x1B, 0x10, 0x30}},
	{0x30, 1, {0x0A}},
	{0x02, 1, {0x4F}},
	{0x0B, 1, {0x40}},
	{0x30, 1, {0x0D}},
	{0x10, 1, {0x05}},
	{0x11, 1, {0x0C}},
	{0x12, 1, {0x05}},
	{0x13, 1, {0x0C}},
	{0x30, 1, {0x00}},
	{0x11, 1, {0x00}},
	{REGFLAG_DELAY, 120, {}},
	{0x29, 1, {0x00}},
	{REGFLAG_DELAY, 20, {}},
	{REGFLAG_END_OF_TABLE, 0, {}},
};

static struct LCM_setting_table lcm_deep_sleep_mode_in_setting[] = {
	{0x28, 0, {}},
	{REGFLAG_DELAY, 20, {}},
	{0x10, 0, {}},
	{REGFLAG_DELAY, 120, {}},
	{REGFLAG_END_OF_TABLE, 0, {}},
};

static void push_table(struct LCM_setting_table *table,
		unsigned int count, unsigned char force_update)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		switch (table[i].cmd) {
		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			return;
		default:
			dsi_set_cmdq_V2(table[i].cmd, table[i].count,
				table[i].para_list, force_update);
			break;
		}
	}
}

static void lcm_set_util_funcs(const struct LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(lcm_util));
}

static void lcm_get_params(struct LCM_PARAMS *params)
{
	memset(params, 0, sizeof(*params));
	params->type = LCM_TYPE_DSI;
	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_UM / 1000;
	params->physical_height = LCM_PHYSICAL_HEIGHT_UM / 1000;
	params->density = LCM_DENSITY;
	params->dbi.te_mode = LCM_DBI_TE_MODE_DISABLED;
	params->dsi.mode = BURST_VDO_MODE;
	params->dsi.LANE_NUM = LCM_FOUR_LANE;
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;
	params->dsi.packet_size = 256;
	params->dsi.intermediat_buffer_num = 2;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.vertical_sync_active = 2;
	params->dsi.vertical_backporch = 21;
	params->dsi.vertical_frontporch = 12;
	params->dsi.vertical_active_line = FRAME_HEIGHT;
	params->dsi.horizontal_sync_active = 24;
	params->dsi.horizontal_backporch = 136;
	params->dsi.horizontal_frontporch = 160;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;
	params->dsi.compatibility_for_nvk = 0;
	params->dsi.ssc_disable = 1;
	params->dsi.ssc_range = 2;
	params->dsi.PLL_CLOCK = 146;
	params->dsi.cont_clock = 1;
	params->dsi.clk_lp_per_line_enable = 1;
	params->dsi.HS_PRPR = 5;
	params->dsi.word_count = 3072;
#ifndef BUILD_LK
	params->dsi.esd_check_enable = 0;
	params->dsi.customization_esd_check_enable = 0;
#endif
}

#ifndef BUILD_LK
static int gpio_lcd_pwr = -EINVAL;
static int gpio_lcd_rst = -EINVAL;
static struct regulator *lcm_vddio;

static void set_panel_gpio(int gpio, int value)
{
	if (gpio_is_valid(gpio))
		gpio_set_value(gpio, value);
}

static void lcm_init_power(void)
{
	/* LK hands the kernel an already powered and initialized panel. */
}

static void lcm_suspend_power(void)
{
	int ret;

	/* The caller has already completed 0x28, 20 ms, 0x10, 120 ms. */
	set_panel_gpio(gpio_lcd_pwr, 0);
	MDELAY(5);
	if (!IS_ERR_OR_NULL(lcm_vddio) && regulator_is_enabled(lcm_vddio) > 0) {
		ret = regulator_disable(lcm_vddio);
		if (ret)
			pr_err("MJT070032M: lcm_vddio disable failed: %d\n", ret);
	}
	MDELAY(5);
	set_panel_gpio(gpio_lcd_rst, 0);
}

static void lcm_resume_power(void)
{
	int ret;

	if (!IS_ERR_OR_NULL(lcm_vddio) && regulator_is_enabled(lcm_vddio) <= 0) {
		ret = regulator_enable(lcm_vddio);
		if (ret) {
			pr_err("MJT070032M: lcm_vddio enable failed: %d\n", ret);
			return;
		}
	}
	MDELAY(10);
	set_panel_gpio(gpio_lcd_rst, 1);
	MDELAY(10);
	set_panel_gpio(gpio_lcd_rst, 0);
	MDELAY(10);
	set_panel_gpio(gpio_lcd_rst, 1);
	MDELAY(40);
	set_panel_gpio(gpio_lcd_pwr, 1);
	MDELAY(10);
}

static int mjt070032m_platform_probe(struct platform_device *pdev)
{
	int ret;

	gpio_lcd_pwr = of_get_named_gpio(pdev->dev.of_node, "gpio_lcd_pwr", 0);
	gpio_lcd_rst = of_get_named_gpio(pdev->dev.of_node, "gpio_lcd_rst", 0);
	if (!gpio_is_valid(gpio_lcd_pwr) || !gpio_is_valid(gpio_lcd_rst))
		return -EINVAL;
	ret = devm_gpio_request_one(&pdev->dev, gpio_lcd_pwr,
		GPIOF_OUT_INIT_HIGH, "mjt070032m-pwr");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(&pdev->dev, gpio_lcd_rst,
		GPIOF_OUT_INIT_HIGH, "mjt070032m-rst");
	if (ret)
		return ret;
	lcm_vddio = devm_regulator_get(&pdev->dev, "lcm_vddio");
	if (IS_ERR(lcm_vddio))
		return PTR_ERR(lcm_vddio);
	ret = regulator_set_voltage(lcm_vddio, 1800000, 1800000);
	if (ret)
		return ret;
	if (regulator_is_enabled(lcm_vddio) <= 0) {
		ret = regulator_enable(lcm_vddio);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct of_device_id mjt070032m_of_match[] = {
	{ .compatible = "MJT070032M" },
	{ }
};
MODULE_DEVICE_TABLE(of, mjt070032m_of_match);

static struct platform_driver mjt070032m_platform_driver = {
	.probe = mjt070032m_platform_probe,
	.driver = {
		.name = "MJT070032M",
		.of_match_table = mjt070032m_of_match,
	},
};
module_platform_driver(mjt070032m_platform_driver);
#else
static void lcm_init_power(void) { }
static void lcm_suspend_power(void) { }
static void lcm_resume_power(void) { }
#endif

static void lcm_init(void)
{
	push_table(lcm_initialization_setting,
		LCM_ARRAY_SIZE(lcm_initialization_setting), 1);
}

static void lcm_suspend(void)
{
	push_table(lcm_deep_sleep_mode_in_setting,
		LCM_ARRAY_SIZE(lcm_deep_sleep_mode_in_setting), 1);
}

static void lcm_resume(void)
{
	push_table(lcm_initialization_setting,
		LCM_ARRAY_SIZE(lcm_initialization_setting), 1);
}

static unsigned int lcm_compare_id(void)
{
	unsigned char id = 0;

	push_table(lcm_compare_id_setting, LCM_ARRAY_SIZE(lcm_compare_id_setting), 1);
	read_reg_v2(0x18, &id, 1);
	return id == 0x65 || id == 0x77;
}

struct LCM_DRIVER MJT070032M_lcm_drv = {
	.name = "MJT070032M",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
	.compare_id = lcm_compare_id,
	.init_power = lcm_init_power,
	.suspend_power = lcm_suspend_power,
	.resume_power = lcm_resume_power,
};
