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
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/irq.h>
#include <linux/mfd/core.h>
#include <linux/mfd/wcd9xxx/core.h>
#include <linux/mfd/wcd9xxx/wcd9xxx_registers.h>
#include <linux/mfd/wcd9xxx/wcd9310_registers.h>
#include <linux/interrupt.h>

#include <mach/cpuidle.h>

#define BYTE_BIT_MASK(nr)		(1UL << ((nr) % BITS_PER_BYTE))
#define BIT_BYTE(nr)			((nr) / BITS_PER_BYTE)

#define WCD9XXX_SYSTEM_RESUME_TIMEOUT_MS 100

struct wcd9xxx_irq {
	bool level;
};

static struct wcd9xxx_irq wcd9xxx_irqs[TABLA_NUM_IRQS] = {
	[0] = { .level = 1},
/* All other wcd9xxx interrupts are edge triggered */
};

static inline int irq_to_wcd9xxx_irq(struct wcd9xxx *wcd9xxx, int irq)
{
	return irq - wcd9xxx->irq_base;
}

static void wcd9xxx_irq_lock(struct irq_data *data)
{
	struct wcd9xxx *wcd9xxx = irq_data_get_irq_chip_data(data);
	mutex_lock(&wcd9xxx->irq_lock);
}

static void wcd9xxx_irq_sync_unlock(struct irq_data *data)
{
	struct wcd9xxx *wcd9xxx = irq_data_get_irq_chip_data(data);
	int i;

	for (i = 0; i < ARRAY_SIZE(wcd9xxx->irq_masks_cur); i++) {
		/* If there's been a change in the mask write it back
		 * to the hardware.
		 */
		if (wcd9xxx->irq_masks_cur[i] != wcd9xxx->irq_masks_cache[i]) {
			wcd9xxx->irq_masks_cache[i] = wcd9xxx->irq_masks_cur[i];
			wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_MASK0+i,
				wcd9xxx->irq_masks_cur[i]);
		}
	}

	mutex_unlock(&wcd9xxx->irq_lock);
}

static void wcd9xxx_irq_enable(struct irq_data *data)
{
	struct wcd9xxx *wcd9xxx = irq_data_get_irq_chip_data(data);
	int wcd9xxx_irq = irq_to_wcd9xxx_irq(wcd9xxx, data->irq);
	wcd9xxx->irq_masks_cur[BIT_BYTE(wcd9xxx_irq)] &=
		~(BYTE_BIT_MASK(wcd9xxx_irq));
}

static void wcd9xxx_irq_disable(struct irq_data *data)
{
	struct wcd9xxx *wcd9xxx = irq_data_get_irq_chip_data(data);
	int wcd9xxx_irq = irq_to_wcd9xxx_irq(wcd9xxx, data->irq);
	wcd9xxx->irq_masks_cur[BIT_BYTE(wcd9xxx_irq)]
			|= BYTE_BIT_MASK(wcd9xxx_irq);
}

static struct irq_chip wcd9xxx_irq_chip = {
	.name = "wcd9xxx",
	.irq_bus_lock = wcd9xxx_irq_lock,
	.irq_bus_sync_unlock = wcd9xxx_irq_sync_unlock,
	.irq_disable = wcd9xxx_irq_disable,
	.irq_enable = wcd9xxx_irq_enable,
};

enum wcd9xxx_pm_state wcd9xxx_pm_cmpxchg(struct wcd9xxx *wcd9xxx,
		enum wcd9xxx_pm_state o,
		enum wcd9xxx_pm_state n)
{
	enum wcd9xxx_pm_state old;
	mutex_lock(&wcd9xxx->pm_lock);
	old = wcd9xxx->pm_state;
	if (old == o)
		wcd9xxx->pm_state = n;
	mutex_unlock(&wcd9xxx->pm_lock);
	return old;
}
EXPORT_SYMBOL_GPL(wcd9xxx_pm_cmpxchg);

bool wcd9xxx_lock_sleep(struct wcd9xxx *wcd9xxx)
{
	enum wcd9xxx_pm_state os;

	/*
	 * wcd9xxx_{lock/unlock}_sleep will be called by wcd9xxx_irq_thread
	 * and its subroutines only motly.
	 * but btn0_lpress_fn is not wcd9xxx_irq_thread's subroutine and
	 * It can race with wcd9xxx_irq_thread.
	 * So need to embrace wlock_holders with mutex.
	 *
	 * If system didn't resume, we can simply return false so codec driver's
	 * IRQ handler can return without handling IRQ.
	 * As interrupt line is still active, codec will have another IRQ to
	 * retry shortly.
	 */
	mutex_lock(&wcd9xxx->pm_lock);
	if (wcd9xxx->wlock_holders++ == 0) {
		pr_debug("%s: holding wake lock\n", __func__);
		pm_qos_update_request(&wcd9xxx->pm_qos_req,
				      msm_cpuidle_get_deep_idle_latency());
	}
	mutex_unlock(&wcd9xxx->pm_lock);
	if (!wait_event_timeout(wcd9xxx->pm_wq,
			((os = wcd9xxx_pm_cmpxchg(wcd9xxx, WCD9XXX_PM_SLEEPABLE,
						WCD9XXX_PM_AWAKE)) ==
						    WCD9XXX_PM_SLEEPABLE ||
			 (os == WCD9XXX_PM_AWAKE)),
			msecs_to_jiffies(WCD9XXX_SYSTEM_RESUME_TIMEOUT_MS))) {
		pr_warn("%s: system didn't resume within %dms, s %d, w %d\n",
			__func__,
			WCD9XXX_SYSTEM_RESUME_TIMEOUT_MS, wcd9xxx->pm_state,
			wcd9xxx->wlock_holders);
		wcd9xxx_unlock_sleep(wcd9xxx);
		return false;
	}
	wake_up_all(&wcd9xxx->pm_wq);
	return true;
}
EXPORT_SYMBOL_GPL(wcd9xxx_lock_sleep);

void wcd9xxx_unlock_sleep(struct wcd9xxx *wcd9xxx)
{
	mutex_lock(&wcd9xxx->pm_lock);
	if (--wcd9xxx->wlock_holders == 0) {
		pr_debug("%s: releasing wake lock pm_state %d -> %d\n",
			 __func__, wcd9xxx->pm_state, WCD9XXX_PM_SLEEPABLE);
		/*
		 * if wcd9xxx_lock_sleep failed, pm_state would be still
		 * WCD9XXX_PM_ASLEEP, don't overwrite
		 */
		if (likely(wcd9xxx->pm_state == WCD9XXX_PM_AWAKE))
			wcd9xxx->pm_state = WCD9XXX_PM_SLEEPABLE;
		pm_qos_update_request(&wcd9xxx->pm_qos_req,
				PM_QOS_DEFAULT_VALUE);
	}
	mutex_unlock(&wcd9xxx->pm_lock);
	wake_up_all(&wcd9xxx->pm_wq);
}
EXPORT_SYMBOL_GPL(wcd9xxx_unlock_sleep);

void wcd9xxx_nested_irq_lock(struct wcd9xxx *wcd9xxx)
{
	mutex_lock(&wcd9xxx->nested_irq_lock);
}

void wcd9xxx_nested_irq_unlock(struct wcd9xxx *wcd9xxx)
{
	mutex_unlock(&wcd9xxx->nested_irq_lock);
}

static void wcd9xxx_irq_dispatch(struct wcd9xxx *wcd9xxx, int irqbit)
{
	if ((irqbit <= TABLA_IRQ_MBHC_INSERTION) &&
	    (irqbit >= TABLA_IRQ_MBHC_REMOVAL)) {
		wcd9xxx_nested_irq_lock(wcd9xxx);
		wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_CLEAR0 +
					   BIT_BYTE(irqbit),
				  BYTE_BIT_MASK(irqbit));
		if (wcd9xxx_get_intf_type() == WCD9XXX_INTERFACE_TYPE_I2C)
			wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_MODE, 0x02);
		handle_nested_irq(wcd9xxx->irq_base + irqbit);
		wcd9xxx_nested_irq_unlock(wcd9xxx);
	} else {
		wcd9xxx_nested_irq_lock(wcd9xxx);
		handle_nested_irq(wcd9xxx->irq_base + irqbit);
		wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_CLEAR0 +
					   BIT_BYTE(irqbit),
				  BYTE_BIT_MASK(irqbit));
		if (wcd9xxx_get_intf_type() == WCD9XXX_INTERFACE_TYPE_I2C)
			wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_MODE, 0x02);
		wcd9xxx_nested_irq_unlock(wcd9xxx);
	}
}

static irqreturn_t wcd9xxx_irq_thread(int irq, void *data)
{
	int ret;
	struct wcd9xxx *wcd9xxx = data;
	u8 status[WCD9XXX_NUM_IRQ_REGS];
	int i;

	if (unlikely(wcd9xxx_lock_sleep(wcd9xxx) == false)) {
		dev_err(wcd9xxx->dev, "Failed to hold suspend\n");
		return IRQ_NONE;
	}
	ret = wcd9xxx_bulk_read(wcd9xxx, TABLA_A_INTR_STATUS0,
			       WCD9XXX_NUM_IRQ_REGS, status);
	if (ret < 0) {
		dev_err(wcd9xxx->dev, "Failed to read interrupt status: %d\n",
			ret);
		wcd9xxx_unlock_sleep(wcd9xxx);
		return IRQ_NONE;
	}
	/* Apply masking */
	for (i = 0; i < WCD9XXX_NUM_IRQ_REGS; i++)
		status[i] &= ~wcd9xxx->irq_masks_cur[i];

	/* Find out which interrupt was triggered and call that interrupt's
	 * handler function
	 */
	if (status[BIT_BYTE(TABLA_IRQ_SLIMBUS)] &
	    BYTE_BIT_MASK(TABLA_IRQ_SLIMBUS))
		wcd9xxx_irq_dispatch(wcd9xxx, TABLA_IRQ_SLIMBUS);

	/* Since codec has only one hardware irq line which is shared by
	 * codec's different internal interrupts, so it's possible master irq
	 * handler dispatches multiple nested irq handlers after breaking
	 * order.  Dispatch MBHC interrupts order to follow MBHC state
	 * machine's order */
	for (i = TABLA_IRQ_MBHC_INSERTION; i >= TABLA_IRQ_MBHC_REMOVAL; i--) {
		if (status[BIT_BYTE(i)] & BYTE_BIT_MASK(i))
			wcd9xxx_irq_dispatch(wcd9xxx, i);
	}
	for (i = TABLA_IRQ_BG_PRECHARGE; i < TABLA_NUM_IRQS; i++) {
		if (status[BIT_BYTE(i)] & BYTE_BIT_MASK(i))
			wcd9xxx_irq_dispatch(wcd9xxx, i);
	}
	wcd9xxx_unlock_sleep(wcd9xxx);

	return IRQ_HANDLED;
}

int wcd9xxx_irq_init(struct wcd9xxx *wcd9xxx)
{
	int ret;
	unsigned int i, cur_irq;

	mutex_init(&wcd9xxx->irq_lock);
	mutex_init(&wcd9xxx->nested_irq_lock);

	if (!wcd9xxx->irq) {
		dev_warn(wcd9xxx->dev,
			 "No interrupt specified, no interrupts\n");
		wcd9xxx->irq_base = 0;
		mutex_destroy(&wcd9xxx->irq_lock);
		mutex_destroy(&wcd9xxx->nested_irq_lock);
		return 0;
	}

	if (!wcd9xxx->irq_base) {
		dev_err(wcd9xxx->dev,
			"No interrupt base specified, no interrupts\n");
		mutex_destroy(&wcd9xxx->irq_lock);
		mutex_destroy(&wcd9xxx->nested_irq_lock);
		return 0;
	}
	/* Mask the individual interrupt sources */
	for (i = 0, cur_irq = wcd9xxx->irq_base; i < TABLA_NUM_IRQS; i++,
		cur_irq++) {

		irq_set_chip_data(cur_irq, wcd9xxx);

		if (wcd9xxx_irqs[i].level)
			irq_set_chip_and_handler(cur_irq, &wcd9xxx_irq_chip,
					 handle_level_irq);
		else
			irq_set_chip_and_handler(cur_irq, &wcd9xxx_irq_chip,
					 handle_edge_irq);

		irq_set_nested_thread(cur_irq, 1);

		/* ARM needs us to explicitly flag the IRQ as valid
		 * and will set them noprobe when we do so. */
#ifdef CONFIG_ARM
		set_irq_flags(cur_irq, IRQF_VALID);
#else
		set_irq_noprobe(cur_irq);
#endif

		wcd9xxx->irq_masks_cur[BIT_BYTE(i)] |= BYTE_BIT_MASK(i);
		wcd9xxx->irq_masks_cache[BIT_BYTE(i)] |= BYTE_BIT_MASK(i);
		wcd9xxx->irq_level[BIT_BYTE(i)] |= wcd9xxx_irqs[i].level <<
			(i % BITS_PER_BYTE);
	}
	for (i = 0; i < WCD9XXX_NUM_IRQ_REGS; i++) {
		/* Initialize interrupt mask and level registers */
		wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_LEVEL0 + i,
			wcd9xxx->irq_level[i]);
		wcd9xxx_reg_write(wcd9xxx, TABLA_A_INTR_MASK0 + i,
			wcd9xxx->irq_masks_cur[i]);
	}

	ret = request_threaded_irq(wcd9xxx->irq, NULL, wcd9xxx_irq_thread,
				   IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
				   "wcd9xxx", wcd9xxx);
	if (ret != 0)
		dev_err(wcd9xxx->dev, "Failed to request IRQ %d: %d\n",
			wcd9xxx->irq, ret);
	else {
		ret = enable_irq_wake(wcd9xxx->irq);
		if (ret == 0) {
			ret = device_init_wakeup(wcd9xxx->dev, 1);
			if (ret) {
				dev_err(wcd9xxx->dev, "Failed to init device"
					"wakeup : %d\n", ret);
				disable_irq_wake(wcd9xxx->irq);
			}
		} else
			dev_err(wcd9xxx->dev, "Failed to set wake interrupt on"
				" IRQ %d: %d\n", wcd9xxx->irq, ret);
		if (ret)
			free_irq(wcd9xxx->irq, wcd9xxx);
	}

	if (ret) {
		mutex_destroy(&wcd9xxx->irq_lock);
		mutex_destroy(&wcd9xxx->nested_irq_lock);
	}

	return ret;
}

void wcd9xxx_irq_exit(struct wcd9xxx *wcd9xxx)
{
	if (wcd9xxx->irq) {
		disable_irq_wake(wcd9xxx->irq);
		free_irq(wcd9xxx->irq, wcd9xxx);
		device_init_wakeup(wcd9xxx->dev, 0);
	}
	mutex_destroy(&wcd9xxx->irq_lock);
	mutex_destroy(&wcd9xxx->nested_irq_lock);
}
