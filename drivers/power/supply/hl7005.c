/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ETA6937 charger-class driver, retained as hl7005.c for existing
 * MediaTek build integration.
 *
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/alarmtimer.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

#include "charger_class.h"
#include "mtk_charger.h"
#include "hl7005.h"

#define ETA6937_DEVICE_ID_MASK		0xf8
#define ETA6937_DEVICE_ID		0x50

#define ETA6937_SAFETY_VALUE		0x8a

#define ETA6937_DEFAULT_CV_UV		4400000U
#define ETA6937_DEFAULT_ICHG_UA		2000000U
#define ETA6937_DEFAULT_IINLIM_UA	800000U

#define ETA6937_MIN_ICHG_UA		550000U
#define ETA6937_MAX_ICHG_UA		3050000U
#define ETA6937_ICHG_STEP_UA		100000U

#define ETA6937_MIN_CV_UV		3500000U
#define ETA6937_MAX_CV_UV		4440000U
#define ETA6937_CV_STEP_UV		20000U

#define ETA6937_DUMP_LAST_REG		0x0b

static const u32 eta6937_iinlim1_ua[] = {
	100000,
	500000,
	800000,
};

static const u32 eta6937_iinlim2_ua[] = {
	300000,
	500000,
	800000,
	1200000,
	1500000,
	2000000,
	3000000,
	5000000,
};

struct hl7005_info {
	struct charger_device *chg_dev;
	struct charger_properties chg_props;
	struct device *dev;

	struct alarm otg_timer;
	struct work_struct otg_kick_work;
	unsigned int polling_interval;
	bool polling_enabled;
	bool charge_enabled;

	const char *chg_dev_name;
	int cd_gpio;
};

static struct hl7005_info *g_info;
static struct i2c_client *new_client;

static DEFINE_MUTEX(hl7005_i2c_access);
static DEFINE_MUTEX(hl7005_probe_lock);

static void enable_boost_polling(bool enable);

static int hl7005_read_byte(u8 reg, u8 *val)
{
	int ret;

	if (!new_client)
		return -ENODEV;

	ret = i2c_smbus_read_byte_data(new_client, reg);
	if (ret < 0)
		return ret;

	*val = (u8)ret;
	return 0;
}

/*
 * This function writes exactly one register byte.
 *
 * The old port allocated wr_len bytes and then copied wr_len bytes after
 * the register-address byte, overflowing the allocation by one byte.
 */
static int hl7005_write_byte(u8 reg, u8 val)
{
	if (!new_client)
		return -ENODEV;

	return i2c_smbus_write_byte_data(new_client, reg, val);
}

static int hl7005_update_bits(u8 reg, u8 mask, u8 value)
{
	u8 old_value;
	u8 new_value;
	int ret;

	mutex_lock(&hl7005_i2c_access);

	ret = hl7005_read_byte(reg, &old_value);
	if (ret < 0)
		goto out;

	new_value = (old_value & ~mask) | (value & mask);

	/*
	 * REG01[7:6] = 3 disables input-current regulation, so never write
	 * that encoding even while modifying an unrelated REG01 field.
	 */
	if (reg == HL7005_CON1 &&
	    (new_value & (CON1_LIN_LIMIT_MASK << CON1_LIN_LIMIT_SHIFT)) ==
	    (CON1_LIN_LIMIT_MASK << CON1_LIN_LIMIT_SHIFT)) {
		new_value &= ~(CON1_LIN_LIMIT_MASK <<
			       CON1_LIN_LIMIT_SHIFT);
		new_value |= 2U << CON1_LIN_LIMIT_SHIFT;
	}

	/* Never accidentally assert the write-only charger-reset bit. */
	if (reg == HL7005_CON4)
		new_value &= ~BIT(CON4_RESET_SHIFT);

	if (new_value != old_value)
		ret = hl7005_write_byte(reg, new_value);
	else
		ret = 0;

out:
	mutex_unlock(&hl7005_i2c_access);
	return ret;
}

static int eta6937_read_identity(u8 *identity)
{
	int ret;

	mutex_lock(&hl7005_i2c_access);
	ret = hl7005_read_byte(HL7005_CON3, identity);
	mutex_unlock(&hl7005_i2c_access);

	return ret;
}

static int eta6937_program_safety_register(struct device *dev)
{
	u8 value;
	int ret;

	/*
	 * REG06 is write-once after reset and must be the first register write.
	 *
	 * IMCHG[3:0] = 1000b:
	 *   550mA + (8 * 200mA) = 2150mA, safely above the 2A policy.
	 *
	 * VMREG[3:0] = 1010b:
	 *   4.20V + (10 * 20mV) = 4.40V.
	 *
	 * Therefore REG06 = (0x8 << 4) | 0xA = 0x8A.
	 */
	mutex_lock(&hl7005_i2c_access);

	ret = hl7005_write_byte(HL7005_CON6, ETA6937_SAFETY_VALUE);
	if (ret < 0)
		goto out;

	ret = hl7005_read_byte(HL7005_CON6, &value);

out:
	mutex_unlock(&hl7005_i2c_access);

	if (ret < 0)
		return ret;

	/*
	 * A bootloader may already have consumed the write-once register, so
	 * accept another latched value only if it cannot clamp 2A or 4.4V.
	 */
	if ((value >> 4) < 8 || (value & 0x0f) < 0x0a) {
		dev_err(dev,
			"unsafe locked safety register: REG06=0x%02x\n",
			value);
		return -EPERM;
	}

	if (value != ETA6937_SAFETY_VALUE)
		dev_warn(dev,
			 "REG06 already latched at safe value 0x%02x\n",
			 value);

	return 0;
}

static int eta6937_set_cv_uv(u32 cv_uv)
{
	u8 code;

	if (cv_uv < ETA6937_MIN_CV_UV)
		cv_uv = ETA6937_MIN_CV_UV;
	else if (cv_uv > ETA6937_MAX_CV_UV)
		cv_uv = ETA6937_MAX_CV_UV;

	/* Round downward so the programmed CV never exceeds the request. */
	code = (cv_uv - ETA6937_MIN_CV_UV) / ETA6937_CV_STEP_UV;

	return hl7005_update_bits(
		HL7005_CON2,
		CON2_OREG_MASK << CON2_OREG_SHIFT,
		code << CON2_OREG_SHIFT);
}

static u32 eta6937_decode_ichg_ua(u8 reg4, u8 reg5)
{
	u8 code;
	u32 current_ua;

	if (reg5 & BIT(CON5_LOW_CHG_SHIFT))
		return ETA6937_MIN_ICHG_UA;

	code = (((reg5 >> CON5_I_CHR_HI_SHIFT) &
		 CON5_I_CHR_HI_MASK) << 3) |
	       ((reg4 >> CON4_I_CHR_SHIFT) & CON4_I_CHR_MASK);

	/*
	 * Codes 0..24 increase by 100mA from 550mA, while codes 25..31
	 * saturate at 3050mA before applying ICHRG_OFFSET.
	 */
	if (code >= 25)
		current_ua = ETA6937_MAX_ICHG_UA;
	else
		current_ua = ETA6937_MIN_ICHG_UA +
			     code * ETA6937_ICHG_STEP_UA;

	if (reg4 & BIT(CON4_I_CHR_OFFSET_SHIFT))
		current_ua += ETA6937_ICHG_STEP_UA;

	return current_ua;
}

static int eta6937_get_charge_current_ua(u32 *current_ua)
{
	u8 reg4;
	u8 reg5;
	int ret;

	mutex_lock(&hl7005_i2c_access);

	ret = hl7005_read_byte(HL7005_CON5, &reg5);
	if (ret < 0)
		goto out;

	ret = hl7005_read_byte(HL7005_CON4, &reg4);
	if (ret < 0)
		goto out;

	*current_ua = eta6937_decode_ichg_ua(reg4, reg5);

out:
	mutex_unlock(&hl7005_i2c_access);
	return ret;
}

static int eta6937_set_charge_current_ua(u32 requested_ua)
{
	u8 reg4;
	u8 reg5;
	u8 safe_reg5;
	u8 new_reg4;
	u8 new_reg5;
	u8 code;
	int ret;

	if (requested_ua <= ETA6937_MIN_ICHG_UA) {
		code = 0;
	} else if (requested_ua >= ETA6937_MAX_ICHG_UA) {
		code = 25;
	} else {
		/*
		 * Round downward because ETA6937 has no exact 2.000A code:
		 * 2.000A therefore becomes code 14, which is 1.950A.
		 */
		code = (requested_ua - ETA6937_MIN_ICHG_UA) /
		       ETA6937_ICHG_STEP_UA;
	}

	mutex_lock(&hl7005_i2c_access);

	ret = hl7005_read_byte(HL7005_CON5, &reg5);
	if (ret < 0)
		goto out;

	ret = hl7005_read_byte(HL7005_CON4, &reg4);
	if (ret < 0)
		goto out;

	/*
	 * Temporarily force 550mA while changing the split current code so
	 * no intermediate REG04/REG05 combination can create an overcurrent.
	 */
	safe_reg5 = reg5 | BIT(CON5_LOW_CHG_SHIFT);

	if (safe_reg5 != reg5) {
		ret = hl7005_write_byte(HL7005_CON5, safe_reg5);
		if (ret < 0)
			goto out;
	}

	/*
	 * Preserve ITERM[2:0], clear RESET, select offset zero, and replace
	 * only ICHG[2:0].
	 */
	new_reg4 = reg4 & ((CON4_I_TERM_MASK << CON4_I_TERM_SHIFT));
	new_reg4 |= (code & CON4_I_CHR_MASK) << CON4_I_CHR_SHIFT;

	ret = hl7005_write_byte(HL7005_CON4, new_reg4);
	if (ret < 0)
		goto out;

	/*
	 * Preserve DPM, CD status, and VINDPM fields while replacing only
	 * ICHG[4:3] and clearing LOW_CHG.
	 */
	new_reg5 = reg5;
	new_reg5 &= ~((CON5_I_CHR_HI_MASK << CON5_I_CHR_HI_SHIFT) |
		      BIT(CON5_LOW_CHG_SHIFT));
	new_reg5 |= ((code >> 3) & CON5_I_CHR_HI_MASK) <<
		    CON5_I_CHR_HI_SHIFT;

	ret = hl7005_write_byte(HL7005_CON5, new_reg5);

out:
	mutex_unlock(&hl7005_i2c_access);
	return ret;
}

static int eta6937_get_input_current_ua(u32 *current_ua)
{
	u8 reg1;
	u8 reg7;
	u8 code;
	int ret;

	mutex_lock(&hl7005_i2c_access);

	ret = hl7005_read_byte(HL7005_CON7, &reg7);
	if (ret < 0)
		goto out;

	if (reg7 & BIT(CON7_EN_ILIM2_SHIFT)) {
		code = (reg7 >> CON7_IIN_LIMIT2_SHIFT) &
		       CON7_IIN_LIMIT2_MASK;
		*current_ua = eta6937_iinlim2_ua[code];
		goto out;
	}

	ret = hl7005_read_byte(HL7005_CON1, &reg1);
	if (ret < 0)
		goto out;

	code = (reg1 >> CON1_LIN_LIMIT_SHIFT) & CON1_LIN_LIMIT_MASK;

	if (code == 3) {
		*current_ua = 0;
		ret = -ERANGE;
		goto out;
	}

	*current_ua = eta6937_iinlim1_ua[code];

out:
	mutex_unlock(&hl7005_i2c_access);
	return ret;
}

static int eta6937_set_input_current_ua(u32 requested_ua)
{
	u8 reg1;
	u8 reg7;
	u8 new_reg1;
	u8 new_reg7;
	u8 code;
	int i;
	int ret;

	mutex_lock(&hl7005_i2c_access);

	if (requested_ua >= 1200000) {
		code = 3;

		for (i = ARRAY_SIZE(eta6937_iinlim2_ua) - 1;
		     i >= 3; i--) {
			if (eta6937_iinlim2_ua[i] <= requested_ua) {
				code = i;
				break;
			}
		}

		ret = hl7005_read_byte(HL7005_CON7, &reg7);
		if (ret < 0)
			goto out;

		/*
		 * Preserve VINDPM[6:3] and replace only EN_ILIM2 plus
		 * IIN_LIMIT_2[2:0].
		 */
		new_reg7 = reg7 &
			(CON7_VINDPM_HI_MASK << CON7_VINDPM_HI_SHIFT);
		new_reg7 |= BIT(CON7_EN_ILIM2_SHIFT);
		new_reg7 |= code << CON7_IIN_LIMIT2_SHIFT;

		if (new_reg7 != reg7)
			ret = hl7005_write_byte(HL7005_CON7, new_reg7);
		else
			ret = 0;

		goto out;
	}

	if (requested_ua >= 800000)
		code = 2;
	else if (requested_ua >= 500000)
		code = 1;
	else
		code = 0;

	ret = hl7005_read_byte(HL7005_CON1, &reg1);
	if (ret < 0)
		goto out;

	ret = hl7005_read_byte(HL7005_CON7, &reg7);
	if (ret < 0)
		goto out;

	/*
	 * Program a regulated REG01 value before selecting it; code 3 is
	 * never emitted.
	 */
	new_reg1 = reg1 &
		~(CON1_LIN_LIMIT_MASK << CON1_LIN_LIMIT_SHIFT);
	new_reg1 |= code << CON1_LIN_LIMIT_SHIFT;

	if (new_reg1 != reg1) {
		ret = hl7005_write_byte(HL7005_CON1, new_reg1);
		if (ret < 0)
			goto out;
	}

	/* Preserve every VINDPM field while selecting REG01. */
	new_reg7 = reg7 & ~BIT(CON7_EN_ILIM2_SHIFT);

	if (new_reg7 != reg7)
		ret = hl7005_write_byte(HL7005_CON7, new_reg7);
	else
		ret = 0;

out:
	mutex_unlock(&hl7005_i2c_access);
	return ret;
}

static int eta6937_kick_watchdog(void)
{
	u8 value;
	int ret;

	mutex_lock(&hl7005_i2c_access);

	ret = hl7005_read_byte(HL7005_CON0, &value);
	if (ret < 0)
		goto out;

	/*
	 * REG00 bit 7 reads OTG_STAT but writes TMR_RST, so always perform
	 * the write even if the read value already contains bit 7.
	 */
	ret = hl7005_write_byte(HL7005_CON0,
			       value | BIT(CON0_TMR_RST_SHIFT));

out:
	mutex_unlock(&hl7005_i2c_access);
	return ret;
}

static int eta6937_hw_init(struct hl7005_info *info)
{
	u8 control_mask;
	u8 control_value;
	int ret;

	/* This must remain the first register write after identity checking. */
	ret = eta6937_program_safety_register(info->dev);
	if (ret < 0)
		return ret;

	ret = eta6937_set_cv_uv(ETA6937_DEFAULT_CV_UV);
	if (ret < 0)
		return ret;

	ret = eta6937_set_charge_current_ua(ETA6937_DEFAULT_ICHG_UA);
	if (ret < 0)
		return ret;

	ret = eta6937_set_input_current_ua(ETA6937_DEFAULT_IINLIM_UA);
	if (ret < 0)
		return ret;

	control_mask = BIT(CON1_TE_SHIFT) |
		       BIT(CON1_CE_SHIFT) |
		       BIT(CON1_HZ_MODE_SHIFT) |
		       BIT(CON1_OPA_MODE_SHIFT);
	control_value = BIT(CON1_TE_SHIFT);

	ret = hl7005_update_bits(HL7005_CON1,
				 control_mask, control_value);
	if (ret < 0)
		return ret;

	return eta6937_kick_watchdog();
}

static int hl7005_dump_register(struct charger_device *chg_dev)
{
	struct hl7005_info *info = g_info;
	int first_error = 0;
	int ret;
	int reg;
	u8 value;

	if (!info)
		return -ENODEV;

	mutex_lock(&hl7005_i2c_access);

	for (reg = 0; reg <= ETA6937_DUMP_LAST_REG; reg++) {
		ret = hl7005_read_byte((u8)reg, &value);
		if (ret < 0) {
			dev_info(info->dev,
				 "REG[0x%02x] read error: %d\n",
				 reg, ret);
			if (!first_error)
				first_error = ret;
			continue;
		}

		dev_info(info->dev, "REG[0x%02x] = 0x%02x\n",
			 reg, value);
	}

	mutex_unlock(&hl7005_i2c_access);
	return first_error;
}

static int hl7005_parse_dt(struct hl7005_info *info)
{
	struct device_node *np = info->dev->of_node;
	int gpio;

	if (!np)
		return -ENODEV;

	/*
	 * mtk_charger resolves this exact global name, so it is intentionally
	 * not configurable.
	 */
	info->chg_dev_name = "primary_chg";

	if (of_property_read_string(np, "alias_name",
				    &info->chg_props.alias_name))
		info->chg_props.alias_name = "eta6937";

	gpio = of_get_named_gpio(np, "hl7005,cd_pin", 0);
	if (gpio == -EPROBE_DEFER)
		return gpio;

	if (!gpio_is_valid(gpio)) {
		dev_err(info->dev,
			"missing or invalid hl7005,cd_pin: %d\n", gpio);
		return gpio < 0 ? gpio : -EINVAL;
	}

	info->cd_gpio = gpio;
	return 0;
}

static int hl7005_do_event(struct charger_device *chg_dev,
			   unsigned int event, unsigned int args)
{
	if (!chg_dev)
		return -EINVAL;

	switch (event) {
	case EVENT_FULL:
		charger_dev_notify(chg_dev, CHARGER_DEV_NOTIFY_EOC);
		break;
	case EVENT_RECHARGE:
		charger_dev_notify(chg_dev, CHARGER_DEV_NOTIFY_RECHG);
		break;
	default:
		break;
	}

	return 0;
}

static int hl7005_enable_charging(struct charger_device *chg_dev, bool enable)
{
	struct hl7005_info *info = g_info;
	u8 mask;
	int ret;

	if (!info)
		return -ENODEV;

	if (!enable) {
		/*
		 * CD is active-high: assert the physical disable first so a
		 * failed I2C transaction still leaves charging disabled.
		 */
		gpio_set_value_cansleep(info->cd_gpio, 1);
		info->charge_enabled = false;

		return hl7005_update_bits(HL7005_CON1,
					  BIT(CON1_CE_SHIFT),
					  BIT(CON1_CE_SHIFT));
	}

	mask = BIT(CON1_CE_SHIFT) |
	       BIT(CON1_HZ_MODE_SHIFT) |
	       BIT(CON1_OPA_MODE_SHIFT);

	ret = hl7005_update_bits(HL7005_CON1, mask, 0);
	if (ret < 0)
		return ret;

	/* CD=0 enables the physical charging path. */
	gpio_set_value_cansleep(info->cd_gpio, 0);
	info->charge_enabled = true;

	return 0;
}

static int hl7005_set_cv_voltage(struct charger_device *chg_dev, u32 cv_uv)
{
	return eta6937_set_cv_uv(cv_uv);
}

static int hl7005_get_current(struct charger_device *chg_dev, u32 *ichg_ua)
{
	if (!ichg_ua)
		return -EINVAL;

	return eta6937_get_charge_current_ua(ichg_ua);
}

static int hl7005_set_current(struct charger_device *chg_dev, u32 ichg_ua)
{
	return eta6937_set_charge_current_ua(ichg_ua);
}

static int hl7005_get_input_current(struct charger_device *chg_dev,
				    u32 *aicr_ua)
{
	if (!aicr_ua)
		return -EINVAL;

	return eta6937_get_input_current_ua(aicr_ua);
}

static int hl7005_set_input_current(struct charger_device *chg_dev,
				    u32 aicr_ua)
{
	return eta6937_set_input_current_ua(aicr_ua);
}

static int hl7005_get_charging_status(struct charger_device *chg_dev,
				      bool *is_done)
{
	u8 value;
	int ret;

	if (!is_done)
		return -EINVAL;

	mutex_lock(&hl7005_i2c_access);
	ret = hl7005_read_byte(HL7005_CON0, &value);
	mutex_unlock(&hl7005_i2c_access);

	if (ret < 0)
		return ret;

	*is_done = (((value >> CON0_STAT_SHIFT) & CON0_STAT_MASK) == 2);
	return 0;
}

static int hl7005_reset_watch_dog_timer(struct charger_device *chg_dev)
{
	return eta6937_kick_watchdog();
}

static void eta6937_start_otg_timer(struct hl7005_info *info)
{
	struct timespec now;
	struct timespec interval;
	struct timespec expires;

	get_monotonic_boottime(&now);
	interval.tv_sec = info->polling_interval;
	interval.tv_nsec = 0;
	expires = timespec_add(now, interval);

	alarm_start(&info->otg_timer,
		    ktime_set(expires.tv_sec, expires.tv_nsec));
}

static void enable_boost_polling(bool enable)
{
	struct hl7005_info *info = g_info;

	if (!info)
		return;

	if (enable) {
		info->polling_enabled = true;
		eta6937_start_otg_timer(info);
	} else {
		info->polling_enabled = false;
		alarm_cancel(&info->otg_timer);
	}
}

static void usbotg_boost_kick_work(struct work_struct *work)
{
	struct hl7005_info *info =
		container_of(work, struct hl7005_info, otg_kick_work);

	eta6937_kick_watchdog();

	if (info->polling_enabled)
		eta6937_start_otg_timer(info);
}

static enum alarmtimer_restart usbotg_timer_func(struct alarm *alarm,
						  ktime_t now)
{
	struct hl7005_info *info =
		container_of(alarm, struct hl7005_info, otg_timer);

	schedule_work(&info->otg_kick_work);
	return ALARMTIMER_NORESTART;
}

static int hl7005_charger_enable_otg(struct charger_device *chg_dev,
				     bool enable)
{
	struct hl7005_info *info = g_info;
	u8 mask;
	u8 value;
	int ret;

	if (!info)
		return -ENODEV;

	mask = BIT(CON1_HZ_MODE_SHIFT) |
	       BIT(CON1_OPA_MODE_SHIFT);
	value = enable ? BIT(CON1_OPA_MODE_SHIFT) : 0;

	ret = hl7005_update_bits(HL7005_CON1, mask, value);
	if (ret < 0)
		return ret;

	if (enable) {
		/* CD must be low for the chip to leave hardware disable. */
		gpio_set_value_cansleep(info->cd_gpio, 0);
		enable_boost_polling(true);
	} else {
		enable_boost_polling(false);
		gpio_set_value_cansleep(info->cd_gpio,
				       info->charge_enabled ? 0 : 1);
	}

	return 0;
}

static struct charger_ops hl7005_chg_ops = {
	.dump_registers = hl7005_dump_register,
	.enable = hl7005_enable_charging,
	.get_charging_current = hl7005_get_current,
	.set_charging_current = hl7005_set_current,
	.get_input_current = hl7005_get_input_current,
	.set_input_current = hl7005_set_input_current,
	.set_constant_voltage = hl7005_set_cv_voltage,
	.kick_wdt = hl7005_reset_watch_dog_timer,
	.is_charging_done = hl7005_get_charging_status,
	.enable_otg = hl7005_charger_enable_otg,
	.event = hl7005_do_event,
};

static void hl7005_release_client(struct i2c_client *client)
{
	mutex_lock(&hl7005_probe_lock);

	if (new_client == client)
		new_client = NULL;

	mutex_unlock(&hl7005_probe_lock);
}

static int hl7005_driver_probe(struct i2c_client *client,
			       const struct i2c_device_id *id)
{
	struct hl7005_info *info;
	u8 identity;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;

	info = devm_kzalloc(&client->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &client->dev;

	ret = hl7005_parse_dt(info);
	if (ret < 0)
		return ret;

	/*
	 * The implementation is deliberately single-instance because
	 * mtk_charger expects exactly one charger named primary_chg.
	 */
	mutex_lock(&hl7005_probe_lock);

	if (new_client) {
		mutex_unlock(&hl7005_probe_lock);
		dev_err(&client->dev,
			"another primary_chg instance is already active\n");
		return -EBUSY;
	}

	new_client = client;
	mutex_unlock(&hl7005_probe_lock);

	/*
	 * This read-only identity check occurs before any register write and
	 * before charger_device_register().
	 */
	ret = eta6937_read_identity(&identity);
	if (ret < 0) {
		dev_err(&client->dev,
			"failed to read ETA6937 identity: %d\n", ret);
		goto err_release_client;
	}

	if ((identity & ETA6937_DEVICE_ID_MASK) != ETA6937_DEVICE_ID) {
		dev_err(&client->dev,
			"identity mismatch: REG03=0x%02x expected 0x50/0xf8\n",
			identity);
		ret = -ENODEV;
		goto err_release_client;
	}

	/*
	 * Request CD initially high so the physical charger remains disabled
	 * until all safety, CV, current, and input-limit fields are valid.
	 */
	ret = devm_gpio_request_one(&client->dev, info->cd_gpio,
				    GPIOF_OUT_INIT_HIGH, "eta6937-cd");
	if (ret < 0) {
		dev_err(&client->dev,
			"failed to request CD GPIO %d: %d\n",
			info->cd_gpio, ret);
		goto err_release_client;
	}

	ret = eta6937_hw_init(info);
	if (ret < 0) {
		dev_err(&client->dev,
			"ETA6937 hardware initialization failed: %d\n",
			ret);
		goto err_disable;
	}

	alarm_init(&info->otg_timer, ALARM_BOOTTIME,
		   usbotg_timer_func);
	INIT_WORK(&info->otg_kick_work, usbotg_boost_kick_work);
	info->polling_interval = 20;
	info->polling_enabled = false;
	info->charge_enabled = true;

	/* All safety-critical registers are valid before CD is deasserted. */
	gpio_set_value_cansleep(info->cd_gpio, 0);

	/*
	 * Registration is intentionally last so every preceding failure
	 * exits without leaking a charger-class device.
	 */
	info->chg_dev = charger_device_register(
		"primary_chg", &client->dev, info,
		&hl7005_chg_ops, &info->chg_props);

	if (IS_ERR_OR_NULL(info->chg_dev)) {
		ret = info->chg_dev ? PTR_ERR(info->chg_dev) : -EINVAL;
		dev_err(&client->dev,
			"charger_device_register failed: %d\n", ret);
		goto err_disable;
	}

	i2c_set_clientdata(client, info);
	g_info = info;

	dev_info(&client->dev,
		 "ETA6937 detected: REG03=0x%02x, registered primary_chg\n",
		 identity);

	/* Dump failures are diagnostic and do not invalidate the probe. */
	hl7005_dump_register(info->chg_dev);

	return 0;

err_disable:
	gpio_set_value_cansleep(info->cd_gpio, 1);
	info->charge_enabled = false;

err_release_client:
	hl7005_release_client(client);
	return ret;
}

static int hl7005_driver_remove(struct i2c_client *client)
{
	struct hl7005_info *info = i2c_get_clientdata(client);

	if (!info)
		return 0;

	/* Hardware disable is asserted before unregistering software state. */
	gpio_set_value_cansleep(info->cd_gpio, 1);
	info->charge_enabled = false;
	info->polling_enabled = false;

	alarm_cancel(&info->otg_timer);
	cancel_work_sync(&info->otg_kick_work);

	if (info->chg_dev)
		charger_device_unregister(info->chg_dev);

	mutex_lock(&hl7005_probe_lock);

	if (g_info == info)
		g_info = NULL;

	if (new_client == client)
		new_client = NULL;

	mutex_unlock(&hl7005_probe_lock);

	i2c_set_clientdata(client, NULL);
	return 0;
}

static const struct i2c_device_id hl7005_i2c_id[] = {
	{ "hl7005", 0 },
	{ "eta6937", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, hl7005_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id hl7005_of_match[] = {
	{ .compatible = "mediatek,hl7005_chg_driver" },
	{ .compatible = "halo,hl7005" },
	{ .compatible = "hcn,eta6937" },
	{ }
};
MODULE_DEVICE_TABLE(of, hl7005_of_match);
#endif

static struct i2c_driver hl7005_driver = {
	.driver = {
		.name = "hl7005",
		.of_match_table = of_match_ptr(hl7005_of_match),
	},
	.probe = hl7005_driver_probe,
	.remove = hl7005_driver_remove,
	.id_table = hl7005_i2c_id,
};

static int __init hl7005_init(void)
{
	return i2c_add_driver(&hl7005_driver);
}

static void __exit hl7005_exit(void)
{
	i2c_del_driver(&hl7005_driver);
}

module_init(hl7005_init);
module_exit(hl7005_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ETA6937 charger-class driver");
MODULE_AUTHOR("MediaTek Inc.; ETA6937 corrections");
