// SPDX-License-Identifier: GPL-2.0
/*
 * Hynitron CST226SE capacitive touch driver for MediaTek TPD framework.
 *
 * MID7021 (onn 100135924 / Lightcomm) arm64 bring-up.
 * Reconstructed from:
 *   - stock "Hynitron V2.10 20241111" hyn_ts driver (of_match "hyn,226se"),
 *   - the published CST226SE register protocol (lewisxhe/SensorLib),
 *   - the in-tree MTK FocalTech TPD sub-driver (framework glue).
 *
 * Stock DT (from dtbo_a.bin fragment@6):
 *   hynitron@5A { compatible="hyn,226se"; reg=<0x5a>;
 *     interrupt-parent=<&pio>; interrupts=<0 2>;        // EINT, falling
 *     int-gpio=<&pio 0 0>;    // INT  = GPIO0
 *     rst-gpio=<&pio 0xae 0>; // RST  = GPIO174
 *     pos-swap=<1>; posx-reverse=<0>; posy-reverse=<1>; }
 *   resolution 1024x600 (landscape), max 5 touch points.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include "tpd.h"

#define CST_DEV_NAME		"hyn_ts"

/* CST226SE identity (SensorLib) */
#define CST226SE_I2C_ADDR	0x5a
#define CST226SE_CHIPTYPE	0xa8
#define CST_MAX_POINTS		5
#define CST_REPORT_LEN		28	/* status buffer size */

/* default logical resolution (panel is 1024x600 landscape) */
#define CST_DEF_X_MAX		1024
#define CST_DEF_Y_MAX		600

struct cst_ts_data {
	struct i2c_client	*client;
	int			irq;
	int			int_gpio;
	int			rst_gpio;
	int			x_max;
	int			y_max;
	bool			pos_swap;
	bool			posx_reverse;
	bool			posy_reverse;
};

static struct cst_ts_data *g_cst;

/* ---- i2c helpers ---- */

static int cst_read(struct i2c_client *client, u8 *cmd, int clen,
		    u8 *buf, int blen)
{
	struct i2c_msg msgs[2];
	int ret;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = clen;
	msgs[0].buf = cmd;
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = blen;
	msgs[1].buf = buf;

	ret = i2c_transfer(client->adapter, msgs, 2);
	return (ret == 2) ? 0 : -EIO;
}

static int cst_write(struct i2c_client *client, u8 *buf, int len)
{
	int ret = i2c_master_send(client, buf, len);

	return (ret == len) ? 0 : -EIO;
}

/* ---- chip identity (CST226SE command mode, SensorLib) ---- */

static int cst_read_chip_type(struct cst_ts_data *ts, u16 *chip_type)
{
	u8 enter[2] = { 0xd1, 0x01 };
	u8 exit[2]  = { 0xd1, 0x09 };
	u8 cmd[2]   = { 0xd2, 0x04 };
	u8 buf[4]   = { 0 };
	int ret;

	cst_write(ts->client, enter, 2);
	msleep(2);
	ret = cst_read(ts->client, cmd, 2, buf, 4);
	cst_write(ts->client, exit, 2);
	if (ret)
		return ret;

	/* chip type in bytes 2..3 */
	*chip_type = ((u16)buf[3] << 8) | buf[2];
	return 0;
}

/* ---- reset / power ---- */

static void cst_reset(struct cst_ts_data *ts)
{
	if (!gpio_is_valid(ts->rst_gpio))
		return;
	gpio_direction_output(ts->rst_gpio, 0);
	msleep(10);
	gpio_direction_output(ts->rst_gpio, 1);
	msleep(50);
}

/* ---- coordinate transform (stock pos-swap / posx/posy-reverse) ---- */

static void cst_transform(struct cst_ts_data *ts, int *px, int *py)
{
	int x = *px, y = *py, tmp;

	if (ts->pos_swap) {
		tmp = x;
		x = y;
		y = tmp;
	}
	if (ts->posx_reverse)
		x = ts->x_max - 1 - x;
	if (ts->posy_reverse)
		y = ts->y_max - 1 - y;

	if (x < 0)
		x = 0;
	if (x > ts->x_max - 1)
		x = ts->x_max - 1;
	if (y < 0)
		y = 0;
	if (y > ts->y_max - 1)
		y = ts->y_max - 1;
	*px = x;
	*py = y;
}

/* ---- report packet parse (SensorLib getTouchPoints) ---- */

static irqreturn_t cst_irq_thread(int irq, void *dev_id)
{
	struct cst_ts_data *ts = dev_id;
	u8 reg = 0x00;
	u8 buf[CST_REPORT_LEN] = { 0 };
	u8 ack[3] = { 0x00, 0xab, 0x00 };
	int num, i, idx;
	bool any = false;

	if (cst_read(ts->client, &reg, 1, buf, CST_REPORT_LEN))
		goto out;

	num = buf[5] & 0x7f;
	if (num == 0 || num > CST_MAX_POINTS) {
		/* all up */
		input_report_key(tpd->dev, BTN_TOUCH, 0);
		input_mt_sync(tpd->dev);
		input_sync(tpd->dev);
		goto ack;
	}

	idx = 0;
	for (i = 0; i < num; i++) {
		int id = (buf[idx] & 0xf0) >> 4;
		int x  = (buf[idx + 1] << 4) | ((buf[idx + 3] >> 4) & 0x0f);
		int y  = (buf[idx + 2] << 4) | (buf[idx + 3] & 0x0f);
		int p  = buf[idx + 4];

		cst_transform(ts, &x, &y);

		input_report_key(tpd->dev, BTN_TOUCH, 1);
		input_report_abs(tpd->dev, ABS_MT_TRACKING_ID, id);
		input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, p ? p : 1);
		input_report_abs(tpd->dev, ABS_MT_POSITION_X, x);
		input_report_abs(tpd->dev, ABS_MT_POSITION_Y, y);
		input_mt_sync(tpd->dev);
		any = true;

		idx += (i == 0) ? 7 : 5;
		if (idx + 4 >= CST_REPORT_LEN)
			break;
	}
	if (any)
		input_sync(tpd->dev);

ack:
	/* CST acknowledge: clear status so the next IRQ fires */
	cst_write(ts->client, ack, 3);
out:
	return IRQ_HANDLED;
}

/* ---- DT parse ---- */

static int cst_parse_dt(struct cst_ts_data *ts, struct device_node *np)
{
	u32 res[2];
	u32 v;

	ts->int_gpio = of_get_named_gpio(np, "int-gpio", 0);
	ts->rst_gpio = of_get_named_gpio(np, "rst-gpio", 0);

	ts->pos_swap     = (of_property_read_u32(np, "pos-swap", &v) == 0) && v;
	ts->posx_reverse = (of_property_read_u32(np, "posx-reverse", &v) == 0) && v;
	ts->posy_reverse = (of_property_read_u32(np, "posy-reverse", &v) == 0) && v;

	ts->x_max = CST_DEF_X_MAX;
	ts->y_max = CST_DEF_Y_MAX;
	if (of_property_read_u32_array(np, "tpd-resolution", res, 2) == 0) {
		ts->x_max = res[0];
		ts->y_max = res[1];
	}
	return 0;
}

/* ---- i2c probe ---- */

static int cst_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct cst_ts_data *ts;
	u16 chip_type = 0;
	int ret;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	if (client->addr != CST226SE_I2C_ADDR)
		client->addr = CST226SE_I2C_ADDR;

	cst_parse_dt(ts, client->dev.of_node);

	if (gpio_is_valid(ts->rst_gpio)) {
		ret = devm_gpio_request(&client->dev, ts->rst_gpio, "hyn_rst");
		if (ret)
			dev_warn(&client->dev, "rst gpio req failed %d\n", ret);
	}
	if (gpio_is_valid(ts->int_gpio)) {
		ret = devm_gpio_request(&client->dev, ts->int_gpio, "hyn_int");
		if (ret)
			dev_warn(&client->dev, "int gpio req failed %d\n", ret);
		else
			gpio_direction_input(ts->int_gpio);
	}

	cst_reset(ts);
	msleep(50);

	if (cst_read_chip_type(ts, &chip_type) == 0) {
		dev_info(&client->dev, "hyn chip_type=0x%04x (expect 0x%02x)\n",
			 chip_type, CST226SE_CHIPTYPE);
	} else {
		dev_warn(&client->dev, "hyn chip id read failed; continuing\n");
	}

	/* tpd->dev is allocated by mtk_tpd; declare our capabilities */
	input_set_capability(tpd->dev, EV_KEY, BTN_TOUCH);
	set_bit(EV_ABS, tpd->dev->evbit);
	set_bit(INPUT_PROP_DIRECT, tpd->dev->propbit);
	input_set_abs_params(tpd->dev, ABS_MT_POSITION_X, 0, ts->x_max - 1, 0, 0);
	input_set_abs_params(tpd->dev, ABS_MT_POSITION_Y, 0, ts->y_max - 1, 0, 0);
	input_set_abs_params(tpd->dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(tpd->dev, ABS_MT_TRACKING_ID, 0, CST_MAX_POINTS - 1, 0, 0);

	ts->irq = client->irq;
	if (ts->irq <= 0 && gpio_is_valid(ts->int_gpio))
		ts->irq = gpio_to_irq(ts->int_gpio);
	if (ts->irq <= 0) {
		dev_err(&client->dev, "no valid irq\n");
		return -EINVAL;
	}

	ret = devm_request_threaded_irq(&client->dev, ts->irq, NULL,
					cst_irq_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					CST_DEV_NAME, ts);
	if (ret) {
		dev_err(&client->dev, "request_irq failed %d\n", ret);
		return ret;
	}

	g_cst = ts;
	tpd_load_status = 1;
	dev_info(&client->dev, "hyn CST226SE probed: int=%d rst=%d res=%dx%d\n",
		 ts->int_gpio, ts->rst_gpio, ts->x_max, ts->y_max);
	return 0;
}

static int cst_i2c_remove(struct i2c_client *client)
{
	g_cst = NULL;
	return 0;
}

static const struct i2c_device_id cst_i2c_id[] = {
	{ CST_DEV_NAME, 0 },
	{ }
};

static const struct of_device_id cst_of_match[] = {
	{ .compatible = "hyn,226se" },
	{ }
};
MODULE_DEVICE_TABLE(of, cst_of_match);

static struct i2c_driver cst_i2c_driver = {
	.probe		= cst_i2c_probe,
	.remove		= cst_i2c_remove,
	.id_table	= cst_i2c_id,
	.driver = {
		.name		= CST_DEV_NAME,
		.of_match_table	= cst_of_match,
	},
};

/* ---- MTK TPD sub-driver glue ---- */

static int cst_local_init(void)
{
	tpd->reg = regulator_get(tpd->tpd_dev, "vtouch");
	if (!IS_ERR_OR_NULL(tpd->reg)) {
		regulator_set_voltage(tpd->reg, 3300000, 3300000);
		regulator_enable(tpd->reg);
	}
	if (i2c_add_driver(&cst_i2c_driver) != 0) {
		pr_err("[hyn] unable to add i2c driver\n");
		return -1;
	}
	if (tpd_load_status == 0) {
		pr_err("[hyn] add i2c ok but probe did not load\n");
		i2c_del_driver(&cst_i2c_driver);
		return -1;
	}
	tpd_type_cap = 1;
	return 0;
}

static void cst_suspend(struct device *h)
{
	if (g_cst)
		disable_irq(g_cst->irq);
}

static void cst_resume(struct device *h)
{
	if (g_cst) {
		cst_reset(g_cst);
		enable_irq(g_cst->irq);
	}
}

static struct tpd_driver_t cst_tpd_driver = {
	.tpd_device_name = "hyn_cst226se",
	.tpd_local_init  = cst_local_init,
	.suspend         = cst_suspend,
	.resume          = cst_resume,
};

static int __init cst_driver_init(void)
{
	pr_info("[hyn] CST226SE MTK TPD driver init\n");
	tpd_get_dts_info();
	if (tpd_driver_add(&cst_tpd_driver) < 0)
		pr_err("[hyn] add tpd driver failed\n");
	return 0;
}

static void __exit cst_driver_exit(void)
{
	tpd_driver_remove(&cst_tpd_driver);
}

module_init(cst_driver_init);
module_exit(cst_driver_exit);

MODULE_AUTHOR("MID7021 arm64 bring-up");
MODULE_DESCRIPTION("Hynitron CST226SE touch driver (MTK TPD)");
MODULE_LICENSE("GPL");
