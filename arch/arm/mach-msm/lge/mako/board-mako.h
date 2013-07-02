/* Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
 * Copyright (c) 2012 LGE Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ARCH_ARM_MACH_MSM_BOARD_APQ8064_MAKO_H
#define __ARCH_ARM_MACH_MSM_BOARD_APQ8064_MAKO_H

#include <linux/regulator/msm-gpio-regulator.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/mfd/pm8xxx/pm8821.h>
#include <mach/msm_memtypes.h>
#include <mach/irqs.h>
#include <mach/rpm-regulator.h>
#include <mach/msm_rtb.h>
#include <mach/msm_cache_dump.h>

/* Macros assume PMIC GPIOs and MPPs start at 1 */
#define PM8921_GPIO_BASE		NR_GPIO_IRQS
#define PM8921_GPIO_PM_TO_SYS(pm_gpio)	(pm_gpio - 1 + PM8921_GPIO_BASE)
#define PM8921_MPP_BASE			(PM8921_GPIO_BASE + PM8921_NR_GPIOS)
#define PM8921_MPP_PM_TO_SYS(pm_mpp)	(pm_mpp - 1 + PM8921_MPP_BASE)
#define PM8921_IRQ_BASE			(NR_MSM_IRQS + NR_GPIO_IRQS)

#define PM8821_MPP_BASE			(PM8921_MPP_BASE + PM8921_NR_MPPS)
#define PM8821_MPP_PM_TO_SYS(pm_mpp)	(pm_mpp - 1 + PM8821_MPP_BASE)
#define PM8821_IRQ_BASE			(PM8921_IRQ_BASE + PM8921_NR_IRQS)

#define TABLA_INTERRUPT_BASE		(PM8821_IRQ_BASE + PM8821_NR_IRQS)

extern struct pm8xxx_regulator_platform_data
	msm8064_pm8921_regulator_pdata[] __devinitdata;

extern int msm8064_pm8921_regulator_pdata_len __devinitdata;

#define GPIO_VREG_ID_EXT_5V		0
#define GPIO_VREG_ID_EXT_3P3V		1
#define GPIO_VREG_ID_EXT_TS_SW		2
#define GPIO_VREG_ID_EXT_MPP8		3

#define GPIO_VREG_ID_AVC_1P2V		0
#define GPIO_VREG_ID_AVC_1P8V		1
#define GPIO_VREG_ID_AVC_2P2V		2
#define GPIO_VREG_ID_AVC_5V		3
#define GPIO_VREG_ID_AVC_3P3V		4

#define GPIO_VREG_ID_EXT_DSV_LOAD	0

#define APQ8064_EXT_3P3V_REG_EN_GPIO	77
#define APQ8064_EXT_DSV_LOAD_EN_GPIO	86

extern struct gpio_regulator_platform_data
	apq8064_gpio_regulator_pdata[] __devinitdata;

extern struct rpm_regulator_platform_data
	apq8064_rpm_regulator_pdata __devinitdata;

extern struct regulator_init_data msm8064_saw_regulator_pdata_8921_s5;
extern struct regulator_init_data msm8064_saw_regulator_pdata_8921_s6;
extern struct regulator_init_data msm8064_saw_regulator_pdata_8821_s0;
extern struct regulator_init_data msm8064_saw_regulator_pdata_8821_s1;

struct mmc_platform_data;
int __init apq8064_add_sdcc(unsigned int controller,
		struct mmc_platform_data *plat);
extern void __init lge_add_sound_devices(void);
extern void __init lge_add_backlight_devices(void);
void __init lge_add_bcm2079x_device(void);
void apq8064_init_mmc(void);
void apq8064_init_gpiomux(void);
void apq8064_init_pmic(void);

#ifdef CONFIG_WIRELESS_CHARGER
extern struct platform_device wireless_charger;
#endif
extern struct platform_device batt_temp_ctrl;

extern struct msm_camera_board_info apq8064_camera_board_info;
/* Enabling flash LED for camera */
extern struct msm_camera_board_info apq8064_lge_camera_board_info;

void apq8064_init_cam(void);

#define APQ_8064_GSBI1_QUP_I2C_BUS_ID 0
#define APQ_8064_GSBI2_QUP_I2C_BUS_ID 2
#define APQ_8064_GSBI3_QUP_I2C_BUS_ID 3
#define APQ_8064_GSBI4_QUP_I2C_BUS_ID 4
#define APQ_8064_GSBI5_QUP_I2C_BUS_ID 5

/* Camera GPIO Settings */
#define GPIO_CAM_MCLK0          (5)

#define GPIO_CAM_MCLK2          (2)
#define GPIO_CAM_FLASH_EN       (7)

#define GPIO_CAM_I2C_SDA        (12)
#define GPIO_CAM_I2C_SCL        (13)
#define GPIO_CAM1_RST_N         (32)
#define GPIO_CAM2_RST_N         (34)

#define GPIO_CAM_FLASH_I2C_SDA  (20)
#define GPIO_CAM_FLASH_I2C_SCL  (21)

#define I2C_SLAVE_ADDR_IMX111				(0x0D)
#define I2C_SLAVE_ADDR_SEKONIX_LENS_ACT		(0x18)
#define I2C_SLAVE_ADDR_IMX091				(0x0D)
#define I2C_SLAVE_ADDR_IMX091_ACT			(0x18)
#define I2C_SLAVE_ADDR_IMX119				(0x6E)
#define I2C_SLAVE_ADDR_FLASH				(0xA6 >> 1)

void apq8064_init_fb(void);
void apq8064_allocate_fb_region(void);
void apq8064_mdp_writeback(struct memtype_reserve *reserve_table);
void __init apq8064_set_display_params(char *prim_panel, char *ext_panel);

void apq8064_init_gpu(void);
void apq8064_pm8xxx_gpio_mpp_init(void);

extern struct msm_rtb_platform_data apq8064_rtb_pdata;
extern struct msm_cache_dump_platform_data apq8064_cache_dump_pdata;

void apq8064_init_input(void);
void __init apq8064_init_misc(void);

#define I2C_SURF 1
#define I2C_FFA  (1 << 1)
#define I2C_RUMI (1 << 2)
#define I2C_SIM  (1 << 3)
#define I2C_LIQUID (1 << 4)
#define I2C_MPQ_CDP BIT(5)
#define I2C_MPQ_HRD BIT(6)
#define I2C_MPQ_DTV BIT(7)

struct i2c_registry {
	u8                     machs;
	int                    bus;
	struct i2c_board_info *info;
	int                    len;
};

#endif
