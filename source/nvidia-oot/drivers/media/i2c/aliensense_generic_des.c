// SPDX-License-Identifier: GPL-2.0-only
/*
 * aliensense_generic_des.c - probe-only GMSL deserializer driver
 *
 * Copyright (C) 2025 RidgeRun, LLC (http://www.ridgerun.com)
 * Copyright (C) 2026 Aliensense
 */

#include <nvidia/conftest.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "../platform/tegra/camera/camera_gpio.h"

struct universal_des {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	struct mutex lock;
	int reset_gpio;
	struct regulator *vdd_cam_1v2;
};

static int universal_des_parse_dt(struct universal_des *priv,
				  struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *des_node = dev->of_node;
	struct device_node *i2c_mux_ch_node;
	int err;

	if (!des_node)
		return -EINVAL;

	i2c_mux_ch_node = of_get_parent(des_node);
	if (!i2c_mux_ch_node) {
		dev_err(dev, "i2c mux channel node not found in dt\n");
		return -EFAULT;
	}

	priv->reset_gpio = of_get_named_gpio(i2c_mux_ch_node, "reset-gpios", 0);
	if (priv->reset_gpio >= 0) {
		err = cam_gpio_register(dev, priv->reset_gpio);
		if (err) {
			dev_err(dev, "reset gpio registration failed: %d\n", err);
			return err;
		}
	} else {
		priv->reset_gpio = 0;
	}

	if (of_get_property(des_node, "vdd_cam_1v2-supply", NULL)) {
		priv->vdd_cam_1v2 = regulator_get(dev, "vdd_cam_1v2");
		if (IS_ERR(priv->vdd_cam_1v2)) {
			err = PTR_ERR(priv->vdd_cam_1v2);
			dev_err(dev, "vdd_cam_1v2 regulator get failed: %d\n", err);
			priv->vdd_cam_1v2 = NULL;
			if (priv->reset_gpio)
				cam_gpio_deregister(dev, priv->reset_gpio);
			return err;
		}
	} else {
		priv->vdd_cam_1v2 = NULL;
	}

	return 0;
}

static struct regmap_config universal_des_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static const struct of_device_id universal_des_of_match[] = {
	{ .compatible = "aliensense,genericdes", },
	{ },
};
MODULE_DEVICE_TABLE(of, universal_des_of_match);

static const struct i2c_device_id universal_des_id[] = {
	{ "universal_des", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, universal_des_id);

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int universal_des_probe(struct i2c_client *client)
#else
static int universal_des_probe(struct i2c_client *client,
			       const struct i2c_device_id *id)
#endif
{
	struct universal_des *priv;
	int err;

	dev_info(&client->dev, "[universal_des]: probing dummy deserializer\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i2c_client = client;
	priv->regmap = devm_regmap_init_i2c(client, &universal_des_regmap_config);
	if (IS_ERR(priv->regmap)) {
		err = PTR_ERR(priv->regmap);
		dev_err(&client->dev, "regmap init failed: %d\n", err);
		return err;
	}

	mutex_init(&priv->lock);

	err = universal_des_parse_dt(priv, client);
	if (err) {
		mutex_destroy(&priv->lock);
		return err;
	}

	dev_set_drvdata(&client->dev, priv);
	dev_info(&client->dev, "%s: probe complete for dummy deserializer\n",
		 __func__);

	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int universal_des_remove(struct i2c_client *client)
#else
static void universal_des_remove(struct i2c_client *client)
#endif
{
	struct universal_des *priv = dev_get_drvdata(&client->dev);

	if (priv) {
		if (priv->vdd_cam_1v2)
			regulator_put(priv->vdd_cam_1v2);
		if (priv->reset_gpio)
			cam_gpio_deregister(&client->dev, priv->reset_gpio);
		mutex_destroy(&priv->lock);
	}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
	return 0;
#endif
}

static struct i2c_driver universal_des_i2c_driver = {
	.driver = {
		.name = "universal_des",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(universal_des_of_match),
	},
	.probe = universal_des_probe,
	.remove = universal_des_remove,
	.id_table = universal_des_id,
};

static int __init universal_des_init(void)
{
	return i2c_add_driver(&universal_des_i2c_driver);
}

static void __exit universal_des_exit(void)
{
	i2c_del_driver(&universal_des_i2c_driver);
}

module_init(universal_des_init);
module_exit(universal_des_exit);

MODULE_DESCRIPTION("Generic probe-only deserializer driver");
MODULE_AUTHOR("Aliensense");
MODULE_LICENSE("GPL v2");
