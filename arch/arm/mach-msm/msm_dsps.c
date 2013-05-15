/* Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
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

/*
 * msm_dsps - control DSPS clocks, gpios and vregs.
 *
 */

#include <asm/atomic.h>

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/msm_dsps.h>

#include <mach/irqs.h>
#include <mach/peripheral-loader.h>
#include <mach/msm_iomap.h>
#include <mach/msm_smsm.h>
#include <mach/msm_dsps.h>
#include <mach/subsystem_restart.h>
#include <mach/subsystem_notif.h>

#include "ramdump.h"
#include "timer.h"

#define DRV_NAME	"msm_dsps"
#define DRV_VERSION	"4.03"


#define PPSS_TIMER0_32KHZ_REG	0x1004
#define PPSS_TIMER0_20MHZ_REG	0x0804

/**
 *  Driver Context
 *
 *  @dev_class - device class.
 *  @dev_num - device major & minor number.
 *  @dev - the device.
 *  @cdev - character device for user interface.
 *  @pdata - platform data.
 *  @pil - handle to DSPS Firmware loader.
 *  @dspsfw_ramdump_dev - handle to ramdump device for DSPS
 *  @dspsfw_ramdump_segments - Ramdump segment information for DSPS
 *  @smem_ramdump_dev - handle to ramdump device for smem
 *  @smem_ramdump_segments - Ramdump segment information for smem
 *  @is_on - DSPS is on.
 *  @ref_count - open/close reference count.
 *  @wdog_irq - DSPS Watchdog IRQ
 *  @crash_in_progress - 1 if crash recovery is in progress
 *  @ppss_base - ppss registers virtual base address.
 */
struct dsps_drv {

	struct class *dev_class;
	dev_t dev_num;
	struct device *dev;
	struct cdev *cdev;

	struct msm_dsps_platform_data *pdata;

	void *pil;

	void *dspsfw_ramdump_dev;
	struct ramdump_segment dspsfw_ramdump_segments[4];

	void *smem_ramdump_dev;
	struct ramdump_segment smem_ramdump_segments[1];

	int is_on;
	int ref_count;
	int wdog_irq;

	atomic_t crash_in_progress;
	void __iomem *ppss_base;
};

/**
 * Driver context.
 */
static struct dsps_drv *drv;

/**
 * self-initiated shutdown flag
 */
static int dsps_crash_shutdown_g;

static void dsps_restart_handler(void);

/**
 *  Load DSPS Firmware.
 */
static int dsps_load(const char *name)
{
	pr_debug("%s.\n", __func__);

	drv->pil = pil_get(name);

	if (IS_ERR(drv->pil)) {
		pr_err("%s: fail to load DSPS firmware %s.\n", __func__, name);
		return -ENODEV;
	}
	msleep(20);
	return 0;
}

/**
 *  Unload DSPS Firmware.
 */
static void dsps_unload(void)
{
	pr_debug("%s.\n", __func__);

	pil_put(drv->pil);
}

/**
 *  Suspend DSPS CPU.
 *
 * Only call if dsps_pwr_ctl_en is false.
 * If dsps_pwr_ctl_en is true, then DSPS will control its own power state.
 */
static void dsps_suspend(void)
{
	pr_debug("%s.\n", __func__);

	writel_relaxed(1, drv->ppss_base + drv->pdata->ppss_pause_reg);
	mb(); /* Make sure write commited before ioctl returns. */
}

/**
 *  Resume DSPS CPU.
 *
 * Only call if dsps_pwr_ctl_en is false.
 * If dsps_pwr_ctl_en is true, then DSPS will control its own power state.
 */
static void dsps_resume(void)
{
	pr_debug("%s.\n", __func__);

	writel_relaxed(0, drv->ppss_base + drv->pdata->ppss_pause_reg);
	mb(); /* Make sure write commited before ioctl returns. */
}

/**
 * Read DSPS slow timer.
 */
static u32 dsps_read_slow_timer(void)
{
	u32 val;

	/* Read the timer value from the MSM sclk. The MSM slow clock & DSPS
	 * timers are in sync, so these are the same value */
	val = msm_timer_get_sclk_ticks();
	pr_debug("%s.count=%d.\n", __func__, val);

	return val;
}

/**
 * Read DSPS fast timer.
 */
static u32 dsps_read_fast_timer(void)
{
	u32 val;

	val = readl_relaxed(drv->ppss_base + PPSS_TIMER0_20MHZ_REG);
	rmb(); /* order reads from the user output buffer */

	pr_debug("%s.count=%d.\n", __func__, val);

	return val;
}

/**
 *  Power on request.
 *
 *  Set clocks to ON.
 *  Set sensors chip-select GPIO to non-reset (on) value.
 *
 */
static int dsps_power_on_handler(void)
{
	int ret = 0;
	int i, ci, gi, ri;

	pr_debug("%s.\n", __func__);

	if (drv->is_on) {
		pr_debug("%s: already ON.\n",  __func__);
		return 0;
	}

	for (ci = 0; ci < drv->pdata->clks_num; ci++) {
		const char *name = drv->pdata->clks[ci].name;
		u32 rate = drv->pdata->clks[ci].rate;
		struct clk *clock = drv->pdata->clks[ci].clock;

		if (clock == NULL)
			continue;

		if (rate > 0) {
			ret = clk_set_rate(clock, rate);
			pr_debug("%s: clk %s set rate %d.",
				__func__, name, rate);
			if (ret) {
				pr_err("%s: clk %s set rate %d. err=%d.",
					__func__, name, rate, ret);
				goto clk_err;
			}

		}

		ret = clk_prepare_enable(clock);
		if (ret) {
			pr_err("%s: enable clk %s err %d.",
			       __func__, name, ret);
			goto clk_err;
		}
	}

	for (gi = 0; gi < drv->pdata->gpios_num; gi++) {
		const char *name = drv->pdata->gpios[gi].name;
		int num = drv->pdata->gpios[gi].num;
		int val = drv->pdata->gpios[gi].on_val;
		int is_owner = drv->pdata->gpios[gi].is_owner;

		if (!is_owner)
			continue;

		ret = gpio_direction_output(num, val);
		if (ret) {
			pr_err("%s: set GPIO %s num %d to %d err %d.",
			       __func__, name, num, val, ret);
			goto gpio_err;
		}
	}

	for (ri = 0; ri < drv->pdata->regs_num; ri++) {
		const char *name = drv->pdata->regs[ri].name;
		struct regulator *reg = drv->pdata->regs[ri].reg;
		int volt = drv->pdata->regs[ri].volt;

		if (reg == NULL)
			continue;

		pr_debug("%s: set regulator %s.", __func__, name);

		ret = regulator_set_voltage(reg, volt, volt);

		if (ret) {
			pr_err("%s: set regulator %s voltage %d err = %d.\n",
				__func__, name, volt, ret);
			goto reg_err;
		}

		ret = regulator_enable(reg);
		if (ret) {
			pr_err("%s: enable regulator %s err = %d.\n",
				__func__, name, ret);
			goto reg_err;
		}
	}

	drv->is_on = true;

	return 0;

	/*
	 * If failling to set ANY clock/gpio/regulator to ON then we set
	 * them back to OFF to avoid consuming power for unused
	 * clocks/gpios/regulators.
	 */
reg_err:
	for (i = 0; i < ri; i++) {
		struct regulator *reg = drv->pdata->regs[ri].reg;

		if (reg == NULL)
			continue;

		regulator_disable(reg);
	}

gpio_err:
	for (i = 0; i < gi; i++) {
		int num = drv->pdata->gpios[i].num;
		int val = drv->pdata->gpios[i].off_val;
		int is_owner = drv->pdata->gpios[i].is_owner;

		if (!is_owner)
			continue;

		ret = gpio_direction_output(num, val);
	}

clk_err:
	for (i = 0; i < ci; i++) {
		struct clk *clock = drv->pdata->clks[i].clock;

		if (clock == NULL)
			continue;

		clk_disable_unprepare(clock);
	}

	return -ENODEV;
}

/**
 *  Power off request.
 *
 *  Set clocks to OFF.
 *  Set sensors chip-select GPIO to reset (off) value.
 *
 */
static int dsps_power_off_handler(void)
{
	int ret;
	int i;

	pr_debug("%s.\n", __func__);

	if (!drv->is_on) {
		pr_debug("%s: already OFF.\n", __func__);
		return 0;
	}

	for (i = 0; i < drv->pdata->clks_num; i++)
		if (drv->pdata->clks[i].clock) {
			const char *name = drv->pdata->clks[i].name;

			pr_debug("%s: set clk %s off.", __func__, name);
			clk_disable_unprepare(drv->pdata->clks[i].clock);
		}

	for (i = 0; i < drv->pdata->regs_num; i++)
		if (drv->pdata->regs[i].reg) {
			const char *name = drv->pdata->regs[i].name;

			pr_debug("%s: set regulator %s off.", __func__, name);
			regulator_disable(drv->pdata->regs[i].reg);
		}

	/* Clocks on/off has reference count but GPIOs don't. */
	drv->is_on = false;

	for (i = 0; i < drv->pdata->gpios_num; i++) {
		const char *name = drv->pdata->gpios[i].name;
		int num = drv->pdata->gpios[i].num;
		int val = drv->pdata->gpios[i].off_val;

		pr_debug("%s: set gpio %s off.", __func__, name);

		ret = gpio_direction_output(num, val);
		if (ret) {
			pr_err("%s: set GPIO %s err %d.", __func__, name, ret);
			return ret;
		}
	}

	return 0;
}

/**
 *
 * Log subsystem restart failure reason
 */
static void dsps_log_sfr(void)
{
	const char dflt_reason[] = "Died too early due to unknown reason";
	char *smem_reset_reason;
	unsigned smem_reset_size;

	smem_reset_reason = smem_get_entry(SMEM_SSR_REASON_DSPS0,
		&smem_reset_size);
	if (smem_reset_reason != NULL && smem_reset_reason[0] != 0) {
		smem_reset_reason[smem_reset_size-1] = 0;
		pr_err("%s: DSPS failure: %s\nResetting DSPS\n",
			__func__, smem_reset_reason);
		memset(smem_reset_reason, 0, smem_reset_size);
		wmb();
	} else
		pr_err("%s: DSPS failure: %s\nResetting DSPS\n",
			__func__, dflt_reason);
}

/**
 *  Watchdog interrupt handler
 *
 */
static irqreturn_t dsps_wdog_bite_irq(int irq, void *dev_id)
{
	pr_err("%s\n", __func__);
	dsps_log_sfr();
	dsps_restart_handler();
	return IRQ_HANDLED;
}

/**
 * IO Control - handle commands from client.
 *
 */
static long dsps_ioctl(struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	u32 val = 0;

	pr_debug("%s.\n", __func__);

	switch (cmd) {
	case DSPS_IOCTL_ON:
		if (!drv->pdata->dsps_pwr_ctl_en) {
			ret = dsps_power_on_handler();
			dsps_resume();
		}
		break;
	case DSPS_IOCTL_OFF:
		if (!drv->pdata->dsps_pwr_ctl_en) {
			dsps_suspend();
			ret = dsps_power_off_handler();
		}
		break;
	case DSPS_IOCTL_READ_SLOW_TIMER:
		val = dsps_read_slow_timer();
		ret = put_user(val, (u32 __user *) arg);
		break;
	case DSPS_IOCTL_READ_FAST_TIMER:
		val = dsps_read_fast_timer();
		ret = put_user(val, (u32 __user *) arg);
		break;
	case DSPS_IOCTL_RESET:
		pr_err("%s: User-initiated DSPS reset.\nResetting DSPS\n",
		       __func__);
		dsps_restart_handler();
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/**
 * allocate resources.
 * @pdev - pointer to platform device.
 */
static int dsps_alloc_resources(struct platform_device *pdev)
{
	int ret = -ENODEV;
	struct resource *ppss_res;
	struct resource *ppss_wdog;
	int i;

	pr_debug("%s.\n", __func__);

	if ((drv->pdata->signature != DSPS_SIGNATURE)) {
		pr_err("%s: invalid signature for pdata.", __func__);
		return -EINVAL;
	}

	ppss_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"ppss_reg");
	if (!ppss_res) {
		pr_err("%s: failed to get ppss_reg resource.\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < drv->pdata->clks_num; i++) {
		const char *name = drv->pdata->clks[i].name;
		struct clk *clock;

		drv->pdata->clks[i].clock = NULL;

		pr_debug("%s: get clk %s.", __func__, name);

		clock = clk_get(drv->dev, name);
		if (IS_ERR(clock)) {
			pr_err("%s: can't get clk %s.", __func__, name);
			goto clk_err;
		}
		drv->pdata->clks[i].clock = clock;
	}

	for (i = 0; i < drv->pdata->gpios_num; i++) {
		const char *name = drv->pdata->gpios[i].name;
		int num = drv->pdata->gpios[i].num;

		drv->pdata->gpios[i].is_owner = false;

		pr_debug("%s: get gpio %s.", __func__, name);

		ret = gpio_request(num, name);
		if (ret) {
			pr_err("%s: request GPIO %s err %d.",
			       __func__, name, ret);
			goto gpio_err;
		}

		drv->pdata->gpios[i].is_owner = true;

	}

	for (i = 0; i < drv->pdata->regs_num; i++) {
		const char *name = drv->pdata->regs[i].name;

		drv->pdata->regs[i].reg = NULL;

		pr_debug("%s: get regulator %s.", __func__, name);

		drv->pdata->regs[i].reg = regulator_get(drv->dev, name);
		if (IS_ERR(drv->pdata->regs[i].reg)) {
			pr_err("%s: get regulator %s failed.",
			       __func__, name);
			goto reg_err;
		}
	}

	drv->ppss_base = ioremap(ppss_res->start,
				 resource_size(ppss_res));

	ppss_wdog = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						"ppss_wdog");
	if (ppss_wdog) {
		drv->wdog_irq = ppss_wdog->start;
		ret = request_irq(drv->wdog_irq, dsps_wdog_bite_irq,
				  IRQF_TRIGGER_RISING, "dsps_wdog", NULL);
		if (ret) {
			pr_err("%s: request_irq fail %d\n", __func__, ret);
			goto request_irq_err;
		}
	} else {
		drv->wdog_irq = -1;
		pr_debug("%s: ppss_wdog not supported.\n", __func__);
	}

	drv->dspsfw_ramdump_segments[0].address = drv->pdata->tcm_code_start;
	drv->dspsfw_ramdump_segments[0].size =  drv->pdata->tcm_code_size;
	drv->dspsfw_ramdump_segments[1].address = drv->pdata->tcm_buf_start;
	drv->dspsfw_ramdump_segments[1].size =  drv->pdata->tcm_buf_size;
	drv->dspsfw_ramdump_segments[2].address = drv->pdata->pipe_start;
	drv->dspsfw_ramdump_segments[2].size =  drv->pdata->pipe_size;
	drv->dspsfw_ramdump_segments[3].address = drv->pdata->ddr_start;
	drv->dspsfw_ramdump_segments[3].size =  drv->pdata->ddr_size;

	drv->dspsfw_ramdump_dev = create_ramdump_device("dsps");
	if (!drv->dspsfw_ramdump_dev) {
		pr_err("%s: create_ramdump_device(\"dsps\") fail\n",
			      __func__);
		goto create_ramdump_err;
	}

	drv->smem_ramdump_segments[0].address = drv->pdata->smem_start;
	drv->smem_ramdump_segments[0].size =  drv->pdata->smem_size;
	drv->smem_ramdump_dev = create_ramdump_device("smem-dsps");
	if (!drv->smem_ramdump_dev) {
		pr_err("%s: create_ramdump_device(\"smem\") fail\n",
		       __func__);
		goto create_ramdump_err;
	}

	if (drv->pdata->init)
		drv->pdata->init(drv->pdata);

	return 0;

create_ramdump_err:
	disable_irq_nosync(drv->wdog_irq);
	free_irq(drv->wdog_irq, NULL);

request_irq_err:
	iounmap(drv->ppss_base);

reg_err:
	for (i = 0; i < drv->pdata->regs_num; i++) {
		if (drv->pdata->regs[i].reg) {
			regulator_put(drv->pdata->regs[i].reg);
			drv->pdata->regs[i].reg = NULL;
		}
	}

gpio_err:
	for (i = 0; i < drv->pdata->gpios_num; i++)
		if (drv->pdata->gpios[i].is_owner) {
			gpio_free(drv->pdata->gpios[i].num);
			drv->pdata->gpios[i].is_owner = false;
		}
clk_err:
	for (i = 0; i < drv->pdata->clks_num; i++)
		if (drv->pdata->clks[i].clock) {
			clk_put(drv->pdata->clks[i].clock);
			drv->pdata->clks[i].clock = NULL;
		}

	return ret;
}

/**
 * Open File.
 *
 */
static int dsps_open(struct inode *ip, struct file *fp)
{
	int ret = 0;

	pr_debug("%s.\n", __func__);

	if (drv->ref_count == 0) {

		/* clocks must be ON before loading.*/
		ret = dsps_power_on_handler();
		if (ret)
			return ret;

		ret = dsps_load(drv->pdata->pil_name);

		if (ret) {
			dsps_power_off_handler();
			return ret;
		}

		if (!drv->pdata->dsps_pwr_ctl_en)
			dsps_resume();
	}
	drv->ref_count++;

	return ret;
}

/**
 * free resources.
 *
 */
static void dsps_free_resources(void)
{
	int i;

	pr_debug("%s.\n", __func__);

	for (i = 0; i < drv->pdata->clks_num; i++)
		if (drv->pdata->clks[i].clock) {
			clk_put(drv->pdata->clks[i].clock);
			drv->pdata->clks[i].clock = NULL;
		}

	for (i = 0; i < drv->pdata->gpios_num; i++)
		if (drv->pdata->gpios[i].is_owner) {
			gpio_free(drv->pdata->gpios[i].num);
			drv->pdata->gpios[i].is_owner = false;
		}

	for (i = 0; i < drv->pdata->regs_num; i++) {
		if (drv->pdata->regs[i].reg) {
			regulator_put(drv->pdata->regs[i].reg);
			drv->pdata->regs[i].reg = NULL;
		}
	}

	free_irq(drv->wdog_irq, NULL);

	iounmap(drv->ppss_base);
}

/**
 * Close File.
 *
 * The client shall close and re-open the file for re-loading the DSPS
 * firmware.
 * The file system will close the file if the user space app has crashed.
 *
 * If the DSPS is running, then we must reset DSPS CPU & HW before
 * setting the clocks off.
 * The DSPS reset should be done as part of the pil_put().
 * The DSPS reset should be used for error recovery if the DSPS firmware
 * has crashed and re-loading the firmware is required.
 */
static int dsps_release(struct inode *inode, struct file *file)
{
	pr_debug("%s.\n", __func__);

	drv->ref_count--;

	if (drv->ref_count == 0) {
		if (!drv->pdata->dsps_pwr_ctl_en) {
			dsps_suspend();

			dsps_unload();

			dsps_power_off_handler();
		}
	}

	return 0;
}

const struct file_operations dsps_fops = {
	.owner = THIS_MODULE,
	.open = dsps_open,
	.release = dsps_release,
	.unlocked_ioctl = dsps_ioctl,
};

static struct subsys_device *dsps_dev;

/**
 *  Fatal error handler
 *  Resets DSPS.
 */
static void dsps_restart_handler(void)
{
	pr_debug("%s: Restart lvl %d\n",
		__func__, get_restart_level());

	if (atomic_add_return(1, &drv->crash_in_progress) > 1) {
		pr_err("%s: DSPS already resetting. Count %d\n", __func__,
		       atomic_read(&drv->crash_in_progress));
	} else {
		subsystem_restart_dev(dsps_dev);
	}
}


/**
 *  SMSM state change callback
 *
 */
static void dsps_smsm_state_cb(void *data, uint32_t old_state,
			       uint32_t new_state)
{
	pr_debug("%s\n", __func__);
	if (dsps_crash_shutdown_g == 1) {
		pr_debug("%s: SMSM_RESET state change ignored\n",
			 __func__);
		dsps_crash_shutdown_g = 0;
		return;
	}
	if (new_state & SMSM_RESET) {
		dsps_log_sfr();
		dsps_restart_handler();
	}
}

/**
 *  Shutdown function
 * called by the restart notifier
 *
 */
static int dsps_shutdown(const struct subsys_desc *subsys)
{
	pr_debug("%s\n", __func__);
	disable_irq_nosync(drv->wdog_irq);
	if (drv->pdata->ppss_wdog_unmasked_int_en_reg) {
		writel_relaxed(0, (drv->ppss_base+
			drv->pdata->ppss_wdog_unmasked_int_en_reg));
		mb(); /* Make sure wdog is disabled before shutting down */
	}
	pil_force_shutdown(drv->pdata->pil_name);
	dsps_power_off_handler();
	return 0;
}

/**
 *  Powerup function
 * called by the restart notifier
 *
 */
static int dsps_powerup(const struct subsys_desc *subsys)
{
	pr_debug("%s\n", __func__);
	dsps_power_on_handler();
	pil_force_boot(drv->pdata->pil_name);
	atomic_set(&drv->crash_in_progress, 0);
	enable_irq(drv->wdog_irq);
	return 0;
}

/**
 *  Crash shutdown function
 * called by the restart notifier
 *
 */
static void dsps_crash_shutdown(const struct subsys_desc *subsys)
{
	pr_debug("%s\n", __func__);
	disable_irq_nosync(drv->wdog_irq);
	dsps_crash_shutdown_g = 1;
	smsm_change_state(SMSM_DSPS_STATE, SMSM_RESET, SMSM_RESET);
}

/**
 *  Ramdump function
 * called by the restart notifier
 *
 */
static int dsps_ramdump(int enable, const struct subsys_desc *subsys)
{
	int ret = 0;
	pr_debug("%s\n", __func__);

	if (enable) {
		if (drv->dspsfw_ramdump_dev != NULL) {
			ret = do_ramdump(drv->dspsfw_ramdump_dev,
				drv->dspsfw_ramdump_segments,
				ARRAY_SIZE(drv->dspsfw_ramdump_segments));
			if (ret < 0) {
				pr_err("%s: Unable to dump DSPS memory (rc = %d).\n",
				       __func__, ret);
				goto dsps_ramdump_out;
			}
		}
		if (drv->smem_ramdump_dev != NULL) {
			ret = do_ramdump(drv->smem_ramdump_dev,
				drv->smem_ramdump_segments,
				ARRAY_SIZE(drv->smem_ramdump_segments));
			if (ret < 0) {
				pr_err("%s: Unable to dump smem memory (rc = %d).\n",
				       __func__, ret);
				goto dsps_ramdump_out;
			}
		}
	}

dsps_ramdump_out:
	return ret;
}

static struct subsys_desc dsps_ssrops = {
	.name = "dsps",
	.shutdown = dsps_shutdown,
	.powerup = dsps_powerup,
	.ramdump = dsps_ramdump,
	.crash_shutdown = dsps_crash_shutdown
};

/**
 * platform driver
 *
 */
static int __devinit dsps_probe(struct platform_device *pdev)
{
	int ret;

	pr_debug("%s.\n", __func__);

	if (pdev->dev.platform_data == NULL) {
		pr_err("%s: platform data is NULL.\n", __func__);
		return -ENODEV;
	}

	drv = kzalloc(sizeof(*drv), GFP_KERNEL);
	if (drv == NULL) {
		pr_err("%s: kzalloc fail.\n", __func__);
		goto alloc_err;
	}
	atomic_set(&drv->crash_in_progress, 0);

	drv->pdata = pdev->dev.platform_data;

	drv->dev_class = class_create(THIS_MODULE, DRV_NAME);
	if (drv->dev_class == NULL) {
		pr_err("%s: class_create fail.\n", __func__);
		goto res_err;
	}

	ret = alloc_chrdev_region(&drv->dev_num, 0, 1, DRV_NAME);
	if (ret) {
		pr_err("%s: alloc_chrdev_region fail.\n", __func__);
		goto alloc_chrdev_region_err;
	}

	drv->dev = device_create(drv->dev_class, NULL,
				     drv->dev_num,
				     drv, DRV_NAME);
	if (IS_ERR(drv->dev)) {
		pr_err("%s: device_create fail.\n", __func__);
		goto device_create_err;
	}

	drv->cdev = cdev_alloc();
	if (drv->cdev == NULL) {
		pr_err("%s: cdev_alloc fail.\n", __func__);
		goto cdev_alloc_err;
	}
	cdev_init(drv->cdev, &dsps_fops);
	drv->cdev->owner = THIS_MODULE;

	ret = cdev_add(drv->cdev, drv->dev_num, 1);
	if (ret) {
		pr_err("%s: cdev_add fail.\n", __func__);
		goto cdev_add_err;
	}

	ret = dsps_alloc_resources(pdev);
	if (ret) {
		pr_err("%s: failed to allocate dsps resources.\n", __func__);
		goto cdev_add_err;
	}

	ret =
	    smsm_state_cb_register(SMSM_DSPS_STATE, SMSM_RESET,
				   dsps_smsm_state_cb, 0);
	if (ret) {
		pr_err("%s: smsm_state_cb_register fail %d\n", __func__,
		       ret);
		goto smsm_register_err;
	}

	dsps_dev = subsys_register(&dsps_ssrops);
	if (IS_ERR(dsps_dev)) {
		ret = PTR_ERR(dsps_dev);
		pr_err("%s: subsys_register fail %d\n", __func__,
		       ret);
		goto ssr_register_err;
	}

	return 0;

ssr_register_err:
	smsm_state_cb_deregister(SMSM_DSPS_STATE, SMSM_RESET,
				 dsps_smsm_state_cb,
				 0);
smsm_register_err:
	cdev_del(drv->cdev);
cdev_add_err:
	kfree(drv->cdev);
cdev_alloc_err:
	device_destroy(drv->dev_class, drv->dev_num);
device_create_err:
	unregister_chrdev_region(drv->dev_num, 1);
alloc_chrdev_region_err:
	class_destroy(drv->dev_class);
res_err:
	kfree(drv);
	drv = NULL;
alloc_err:
	return -ENODEV;
}

static int __devexit dsps_remove(struct platform_device *pdev)
{
	pr_debug("%s.\n", __func__);

	subsys_unregister(dsps_dev);
	dsps_power_off_handler();
	dsps_free_resources();

	cdev_del(drv->cdev);
	kfree(drv->cdev);
	drv->cdev = NULL;
	device_destroy(drv->dev_class, drv->dev_num);
	unregister_chrdev_region(drv->dev_num, 1);
	class_destroy(drv->dev_class);
	kfree(drv);
	drv = NULL;

	return 0;
}

static struct platform_driver dsps_driver = {
	.probe          = dsps_probe,
	.remove         = __exit_p(dsps_remove),
	.driver         = {
		.name   = "msm_dsps",
	},
};

/**
 * Module Init.
 */
static int __init dsps_init(void)
{
	int ret;

	pr_info("%s driver version %s.\n", DRV_NAME, DRV_VERSION);

	ret = platform_driver_register(&dsps_driver);

	if (ret)
		pr_err("dsps_init.err=%d.\n", ret);

	return ret;
}

/**
 * Module Exit.
 */
static void __exit dsps_exit(void)
{
	pr_debug("%s.\n", __func__);

	platform_driver_unregister(&dsps_driver);
}

module_init(dsps_init);
module_exit(dsps_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Dedicated Sensors Processor Subsystem (DSPS) driver");
MODULE_AUTHOR("Amir Samuelov <amirs@codeaurora.org>");

