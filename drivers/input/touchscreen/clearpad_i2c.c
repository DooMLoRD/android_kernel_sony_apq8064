/* linux/drivers/input/touchscreen/clearpad_i2c.c
 *
 * Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 * Copyright (c) 2011 Synaptics Incorporated
 * Copyright (c) 2011 Unixphere
 * Copyright (C) 2012 Sony Mobile Communications AB.
 *
 * Author: Yusuke Yoshimura <Yusuke.Yoshimura@sonyericsson.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/clearpad.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/wait.h>

#define CLEARPAD_PAGE_SELECT_REGISTER 0xff
#define CLEARPAD_PAGE(addr) (((addr) >> 8) & 0xff)

struct clearpad_i2c {
	struct platform_device *pdev;
	unsigned int page;
	struct mutex page_mutex;

	wait_queue_head_t wq;
	atomic_t suspended;
};

static int clearpad_i2c_read(struct device *dev, u8 reg, u8 *buf, u8 len)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	s32 rc = 0;
	int rsize = I2C_SMBUS_BLOCK_MAX;
	int off;

	/* If i2c is still suspended, wait until we are resumed */
	wait_event(this->wq, (0 == atomic_cmpxchg(&this->suspended, 0, 1)));
	dev_dbg(dev, "%s: i2c no longer suspended\n", __func__);

	for (off = 0; off < len; off += rsize) {
		if (len < off + I2C_SMBUS_BLOCK_MAX)
			rsize = len - off;
		rc = i2c_smbus_read_i2c_block_data(to_i2c_client(dev),
				reg + off, rsize, &buf[off]);
		if (rc != rsize) {
			dev_err(dev, "%s: rc = %d\n", __func__, rc);
			goto err_return;
		}
	}
	rc = 0;
err_return:
	atomic_set(&this->suspended, 0);
	wake_up(&this->wq);
	return rc;
}

static int clearpad_i2c_write(struct device *dev, u8 reg, const u8 *buf, u8 len)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	int rc = 0;
	u8 i;

	/* If i2c is still suspended, wait until we are resumed */
	wait_event(this->wq, (0 == atomic_cmpxchg(&this->suspended, 0, 1)));
	dev_dbg(dev, "%s: i2c no longer suspended\n", __func__);
	for (i = 0; i < len; i++) {
		rc = i2c_smbus_write_byte_data(to_i2c_client(dev),
				reg + i, buf[i]);
		if (rc)
			break;
	}
	atomic_set(&this->suspended, 0);
	wake_up(&this->wq);
	return rc;
}

static int clearpad_i2c_set_page(struct device *dev, unsigned int page)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	char txbuf[2] = {CLEARPAD_PAGE_SELECT_REGISTER, page};
	int rc = 0;

	rc = i2c_master_send(to_i2c_client(dev), txbuf, sizeof(txbuf));
	if (rc != sizeof(txbuf)) {
		dev_err(dev,
			"%s: set page failed: %d.", __func__, rc);
		rc = (rc < 0) ? rc : -EIO;
		goto exit;
	}
	this->page = page;
	rc = 0;
exit:
	return rc;
}

static int clearpad_i2c_read_block(struct device *dev, u16 addr, u8 *buf,
		int len)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	u8 txbuf[1] = {addr & 0xff};
	int rc = 0;

	mutex_lock(&this->page_mutex);

	if (CLEARPAD_PAGE(addr) != this->page) {
		rc = clearpad_i2c_set_page(dev, CLEARPAD_PAGE(addr));
		if (rc < 0)
			goto exit;
	}

	rc = i2c_master_send(to_i2c_client(dev), txbuf, sizeof(txbuf));
	if (rc != sizeof(txbuf)) {
		rc = (rc < 0) ? rc : -EIO;
		goto exit;
	}

	rc = i2c_master_recv(to_i2c_client(dev), buf, len);
	if (rc < 0)
		dev_err(dev, "%s: rc = %d\n", __func__, rc);
exit:
	mutex_unlock(&this->page_mutex);
	return rc;
}

static int clearpad_i2c_write_block(struct device *dev, u16 addr, const u8 *buf,
		int len)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	u8 txbuf[len + 1];
	int rc = 0;

	txbuf[0] = addr & 0xff;
	memcpy(txbuf + 1, buf, len);

	mutex_lock(&this->page_mutex);

	if (CLEARPAD_PAGE(addr) != this->page) {
		rc = clearpad_i2c_set_page(dev, CLEARPAD_PAGE(addr));
		if (rc < 0)
			goto exit;
	}

	rc = i2c_master_send(to_i2c_client(dev), txbuf, sizeof(txbuf));
	if (rc < 0)
		dev_err(dev, "%s: rc = %d\n", __func__, rc);
exit:
	mutex_unlock(&this->page_mutex);
	return rc;
}

static struct clearpad_bus_data clearpad_i2c_bus_data = {
	.bustype	= BUS_I2C,
	.read		= clearpad_i2c_read,
	.write		= clearpad_i2c_write,
	.read_block	= clearpad_i2c_read_block,
	.write_block	= clearpad_i2c_write_block,
};

static int __devinit clearpad_i2c_probe(struct i2c_client *client,
				      const struct i2c_device_id *id)
{
	struct clearpad_data clearpad_data = {
		.pdata = client->dev.platform_data,
		.bdata = &clearpad_i2c_bus_data,
	};
	struct clearpad_i2c *this;
	int rc;

	this = kzalloc(sizeof(struct clearpad_i2c), GFP_KERNEL);
	if (!this) {
		rc = -ENOMEM;
		goto exit;
	}

	init_waitqueue_head(&this->wq);

	dev_set_drvdata(&client->dev, this);

	mutex_init(&this->page_mutex);

	this->pdev = platform_device_alloc(CLEARPAD_NAME, -1);
	if (!this->pdev) {
		rc = -ENOMEM;
		goto err_free;
	}
	clearpad_data.bdata->dev = &client->dev;
	this->pdev->dev.parent = &client->dev;
	rc = platform_device_add_data(this->pdev,
			&clearpad_data, sizeof(clearpad_data));
	if (rc)
		goto err_device_put;

	rc = platform_device_add(this->pdev);
	if (rc)
		goto err_device_put;

	if (!this->pdev->dev.driver) {
		rc = -ENODEV;
		goto err_device_del;
	}
	atomic_set(&this->suspended, 0);
	dev_info(&client->dev, "%s: sucess\n", __func__);
	goto exit;

err_device_del:
	platform_device_del(this->pdev);
err_device_put:
	platform_device_put(this->pdev);
err_free:
	dev_set_drvdata(&client->dev, NULL);
	kfree(this);
exit:
	return rc;
}

static int __devexit clearpad_i2c_remove(struct i2c_client *client)
{
	struct clearpad_i2c *this = dev_get_drvdata(&client->dev);
	platform_device_unregister(this->pdev);
	dev_set_drvdata(&client->dev, NULL);
	kfree(this);
	return 0;
}

static int clearpad_i2c_suspend(struct device *dev)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	dev_dbg(dev, "%s: suspend\n", __func__);
	wait_event(this->wq, atomic_cmpxchg(&this->suspended, 0, 1));
	dev_vdbg(dev, "%s: i2c suspended\n", __func__);
	return 0;
}

static int clearpad_i2c_resume(struct device *dev)
{
	struct clearpad_i2c *this = dev_get_drvdata(dev);
	dev_dbg(dev, "%s: resume\n", __func__);
	atomic_set(&this->suspended, 0);
	wake_up(&this->wq);
	dev_vdbg(dev, "%s: i2c resumed\n", __func__);
	return 0;
}

static const struct dev_pm_ops clearpad_i2c_pm_ops = {
	.suspend		= clearpad_i2c_suspend,
	.resume			= clearpad_i2c_resume,
};

static const struct i2c_device_id clearpad_id[] = {
	{ CLEARPADI2C_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, clearpad_id);

static struct i2c_driver clearpad_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= CLEARPADI2C_NAME,
		.pm	= &clearpad_i2c_pm_ops,
	},
	.id_table	= clearpad_id,
	.probe		= clearpad_i2c_probe,
	.remove		= __devexit_p(clearpad_i2c_remove),
};

static int __init clearpad_i2c_init(void)
{
	return i2c_add_driver(&clearpad_i2c_driver);
}

static void __exit clearpad_i2c_exit(void)
{
	i2c_del_driver(&clearpad_i2c_driver);
}

MODULE_DESCRIPTION(CLEARPADI2C_NAME "ClearPad I2C Driver");
MODULE_LICENSE("GPL v2");

module_init(clearpad_i2c_init);
module_exit(clearpad_i2c_exit);
