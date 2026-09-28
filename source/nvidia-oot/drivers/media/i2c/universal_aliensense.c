// SPDX-License-Identifier: GPL-2.0-only
/*
 * universal_aliensense.c - universal_aliensense sensor driver
 *
 * Copyright (C) 2025 RidgeRun, LLC (http://www.ridgerun.com)
 * Copyright (C) 2026 Aliensense
 * Portions derived from the FRAMOS Technologies Jetson sensor drivers
 * (fr_imx900.c, fr_imx335.c and their common framework), GPL-2.0.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <nvidia/conftest.h>

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/math64.h>
#include <linux/string.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>
#include <media/camera_common.h>
#include "../platform/tegra/camera/camera_gpio.h"

#include "universal_aliensense_mode_tbls.h"

/*
 * The driver knows no sensor by name. The device tree carries one control
 * table per sensor the node may serve (see docs/dt-bindings/aliensense,universal.md);
 * the mode's aliensense_sensors_pool tag selects the table, and the tegracam
 * control callbacks are implemented as formulas over that table.
 */

#define UA_MAX_TABLES        4
#define UA_SENSOR_NAME_LEN   32
#define UA_CTRL_CELLS        8
#define UA_CTRL_NODE         "aliensense-ctrl"
#define UA_POOL_PROP         "aliensense_sensors_pool"

/* Private controls, named after the FRAMOS Jetson drivers. */
#define UA_CID_TEST_PATTERN   (TEGRA_CAMERA_CID_BASE + 121)
#define UA_CID_OPERATION_MODE (TEGRA_CAMERA_CID_BASE + 122)
#define UA_CID_BLACK_LEVEL    (TEGRA_CAMERA_CID_BASE + 125)
#define UA_CID_SHUTTER_MODE   (TEGRA_CAMERA_CID_BASE + 126)

enum ua_shutter_mode {
    UA_SHUTTER_NORMAL = 0,
    UA_SHUTTER_SEQUENTIAL = 1,
    UA_SHUTTER_FAST = 2,
};

enum ua_form {
    UA_FORM_RAW = 0,
    UA_FORM_LAW = 1,
};

struct ua_ctrl {
    bool present;
    u16 addr;
    u8 width;
    u8 order;   /* 0 little-endian, 1 big-endian */
    u8 form;
    u32 p[4];
};

struct ua_table {
    char sensor[UA_SENSOR_NAME_LEN];
    u32 inck_hz;
    struct ua_ctrl group_hold;
    struct ua_ctrl gain;
    struct ua_ctrl exposure;
    struct ua_ctrl frame_length;
    struct ua_ctrl line_length;
    struct ua_ctrl standby;
    struct ua_ctrl start;
    struct ua_ctrl trigger;
    struct ua_ctrl vint_en;
    struct ua_ctrl sync_sel;
    struct ua_ctrl test_pattern;
    struct ua_ctrl black_level;
};

static const struct of_device_id universal_aliensense_of_match[] = {
    { .compatible = "aliensense,universal", },
    { },
};

MODULE_DEVICE_TABLE(of, universal_aliensense_of_match);

/* Group hold is not listed: the framework registers that control itself
 * and refuses a driver that lists it; the .set_group_hold op serves it. */
static const u32 ctrl_cid_list[] = {
    TEGRA_CAMERA_CID_GAIN,
    TEGRA_CAMERA_CID_EXPOSURE,
    TEGRA_CAMERA_CID_FRAME_RATE,
    TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

struct universal_aliensense {
    struct i2c_client *i2c_client;
    struct v4l2_subdev *subdev;
    struct camera_common_data *s_data;
    struct tegracam_device *tc_dev;
    struct regmap_config regmap_config;
    struct ua_table tables[UA_MAX_TABLES];
    int ntables;
    const struct ua_table *cur;
    u32 hmax;
    u32 vmax;
    u32 mode_vmax;
    u32 shutter_mode;
    u32 operation_mode;
    bool probing;
};

static const struct regmap_config sensor_regmap_config = {
    .reg_bits = 16,
    .val_bits = 8,
    .cache_type = REGCACHE_NONE,
#if KERNEL_VERSION(5, 4, 0) > LINUX_VERSION_CODE
    .use_single_rw = true,
#else
    .use_single_read = true,
    .use_single_write = true,
#endif
};

/* ---- register access ----------------------------------------------- */

static inline int universal_aliensense_read_reg(struct camera_common_data *s_data,
                    u16 addr, u8 *val)
{
    unsigned int v = 0;
    int err = regmap_read(s_data->regmap, addr, &v);

    *val = v & 0xFF;

    return err;
}

static int universal_aliensense_write_reg(struct camera_common_data *s_data, u16 addr,
                  u8 val)
{
    return regmap_write(s_data->regmap, addr, val);
}

/*
 * A bus failure during probe (the handler setup writes every control's
 * default while the pod is not brought up yet) is logged and ignored so the
 * node registers; afterwards it is the caller's.
 */
static int ua_bus_result(struct universal_aliensense *priv, int err, const char *what)
{
    struct device *dev = priv->tc_dev->dev;

    if (!err)
        return 0;
    if (priv->probing) {
        dev_dbg(dev, "%s: sensor not reachable during probe (%d), ignored\n", what, err);
        return 0;
    }
    dev_warn_ratelimited(dev, "%s: bus error %d\n", what, err);

    return err;
}

/*
 * A control's `width` counts bytes; the register map's value width decides
 * how many registers those bytes span (one per byte on an 8-bit-value part,
 * one per pair on a 16-bit-value part). `order` places the slots.
 */
static unsigned int ua_slot_bytes(const struct universal_aliensense *priv)
{
    return priv->regmap_config.val_bits / 8;
}

static unsigned int ua_slots(const struct universal_aliensense *priv, const struct ua_ctrl *c)
{
    unsigned int sb = ua_slot_bytes(priv);

    return (c->width + sb - 1) / sb;
}

static u16 ua_slot_addr(const struct universal_aliensense *priv, const struct ua_ctrl *c,
                        unsigned int i)
{
    unsigned int n = ua_slots(priv, c);

    return c->order ? c->addr + (n - 1 - i) : c->addr + i;
}

static int ua_write_multi(struct universal_aliensense *priv, const struct ua_ctrl *c, u32 value)
{
    unsigned int sb = ua_slot_bytes(priv);
    unsigned int mask = sb == 1 ? 0xFF : 0xFFFF;
    unsigned int i;
    int err;

    for (i = 0; i < ua_slots(priv, c); i++) {
        err = regmap_write(priv->s_data->regmap, ua_slot_addr(priv, c, i),
                           (value >> (8 * sb * i)) & mask);
        if (err)
            return err;
    }

    return 0;
}

static int ua_read_multi(struct universal_aliensense *priv, const struct ua_ctrl *c, u32 *value)
{
    unsigned int sb = ua_slot_bytes(priv);
    unsigned int mask = sb == 1 ? 0xFF : 0xFFFF;
    unsigned int slot = 0;
    u32 acc = 0;
    unsigned int i;
    int err;

    for (i = 0; i < ua_slots(priv, c); i++) {
        err = regmap_read(priv->s_data->regmap, ua_slot_addr(priv, c, i), &slot);
        if (err)
            return err;
        acc |= (slot & mask) << (8 * sb * i);
    }
    *value = acc;

    return 0;
}

/* ---- the device-tree control tables -------------------------------- */

static void ua_parse_ctrl(struct device_node *node, const char *prop, struct ua_ctrl *c)
{
    u32 cells[UA_CTRL_CELLS];

    c->present = false;
    if (of_property_read_u32_array(node, prop, cells, UA_CTRL_CELLS))
        return;
    if (cells[1] < 1 || cells[1] > 4)
        return;
    c->addr = cells[0];
    c->width = cells[1];
    c->order = cells[2] ? 1 : 0;
    c->form = cells[3];
    c->p[0] = cells[4];
    c->p[1] = cells[5];
    c->p[2] = cells[6];
    c->p[3] = cells[7];
    c->present = true;
}

static int ua_parse_tables(struct universal_aliensense *priv)
{
    struct device *dev = priv->tc_dev->dev;
    struct device_node *ctrl_node;
    struct device_node *child;
    const char *sensor;

    priv->ntables = 0;
    ctrl_node = of_get_child_by_name(dev->of_node, UA_CTRL_NODE);
    if (!ctrl_node) {
        dev_info(dev, "no %s node: controls stay inert\n", UA_CTRL_NODE);
        return 0;
    }

    for_each_child_of_node(ctrl_node, child) {
        struct ua_table *t;

        if (priv->ntables >= UA_MAX_TABLES) {
            dev_warn(dev, "%s: more than %d sensors, the rest ignored\n",
                     UA_CTRL_NODE, UA_MAX_TABLES);
            of_node_put(child);
            break;
        }
        t = &priv->tables[priv->ntables];
        if (of_property_read_string(child, "aliensense,sensor", &sensor)) {
            dev_warn(dev, "%s: a child without aliensense,sensor, skipped\n", UA_CTRL_NODE);
            continue;
        }
        strscpy(t->sensor, sensor, sizeof(t->sensor));
        if (of_property_read_u32(child, "aliensense,inck-hz", &t->inck_hz))
            t->inck_hz = 0;
        ua_parse_ctrl(child, "aliensense,ctrl-group-hold", &t->group_hold);
        ua_parse_ctrl(child, "aliensense,ctrl-gain", &t->gain);
        ua_parse_ctrl(child, "aliensense,ctrl-exposure", &t->exposure);
        ua_parse_ctrl(child, "aliensense,ctrl-frame-length", &t->frame_length);
        ua_parse_ctrl(child, "aliensense,ctrl-line-length", &t->line_length);
        ua_parse_ctrl(child, "aliensense,ctrl-standby", &t->standby);
        ua_parse_ctrl(child, "aliensense,ctrl-start", &t->start);
        ua_parse_ctrl(child, "aliensense,ctrl-trigger", &t->trigger);
        ua_parse_ctrl(child, "aliensense,ctrl-vint-en", &t->vint_en);
        ua_parse_ctrl(child, "aliensense,ctrl-sync-sel", &t->sync_sel);
        ua_parse_ctrl(child, "aliensense,ctrl-test-pattern", &t->test_pattern);
        ua_parse_ctrl(child, "aliensense,ctrl-black-level", &t->black_level);
        dev_info(dev, "control table for %s (inck %u Hz)\n", t->sensor, t->inck_hz);
        priv->ntables++;
    }
    of_node_put(ctrl_node);

    return 0;
}

/* The table for a mode: its pool tag names the sensor. */
static const struct ua_table *ua_table_for_mode(struct universal_aliensense *priv, int mode_idx)
{
    struct device *dev = priv->tc_dev->dev;
    struct device_node *mode_node;
    const char *pool = NULL;
    char name[16];
    size_t len;
    int i;

    snprintf(name, sizeof(name), "mode%d", mode_idx);
    mode_node = of_get_child_by_name(dev->of_node, name);
    if (!mode_node)
        return NULL;
    if (of_property_read_string(mode_node, UA_POOL_PROP, &pool))
        pool = NULL;
    of_node_put(mode_node);
    if (!pool)
        return NULL;

    len = strlen(pool);
    if (len && pool[len - 1] == ';')
        len--;
    for (i = 0; i < priv->ntables; i++) {
        const struct ua_table *t = &priv->tables[i];

        if (strlen(t->sensor) == len && !strncmp(t->sensor, pool, len))
            return t;
    }

    return NULL;
}

static const struct sensor_mode_properties *ua_mode_props(struct universal_aliensense *priv)
{
    struct camera_common_data *s_data = priv->s_data;
    int idx = s_data->mode_prop_idx;

    if (idx < 0 || idx >= s_data->sensor_props.num_modes)
        return NULL;

    return &s_data->sensor_props.sensor_modes[idx];
}

/* ---- tegracam control callbacks ------------------------------------ */

static int universal_aliensense_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
    struct universal_aliensense *priv = tegracam_get_privdata(tc_dev);
    const struct ua_table *t = priv ? priv->cur : NULL;

    if (!t || !t->group_hold.present)
        return 0;

    return ua_bus_result(priv, ua_write_multi(priv, &t->group_hold,
                                              val ? t->group_hold.p[0] : t->group_hold.p[1]),
                         "group hold");
}

static int universal_aliensense_set_gain(struct tegracam_device *tc_dev, s64 val)
{
    struct universal_aliensense *priv = tegracam_get_privdata(tc_dev);
    const struct ua_table *t = priv ? priv->cur : NULL;
    u64 reg;

    if (!t || !t->gain.present)
        return 0;
    if (val < 0)
        val = 0;
    /* reg = val * num / den, clamped to the register's range */
    reg = div64_u64((u64)val * (t->gain.p[0] ? t->gain.p[0] : 1),
                    t->gain.p[1] ? t->gain.p[1] : 1);
    if (t->gain.p[2] && reg > t->gain.p[2])
        reg = t->gain.p[2];

    return ua_bus_result(priv, ua_write_multi(priv, &t->gain, (u32)reg), "gain");
}

static int universal_aliensense_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
    struct universal_aliensense *priv = tegracam_get_privdata(tc_dev);
    const struct ua_table *t = priv ? priv->cur : NULL;
    const struct sensor_mode_properties *mode;
    u64 vmax;
    u64 own;
    u32 factor;
    u32 floor;
    int err;

    if (!t || !t->frame_length.present)
        return 0;
    if (priv->shutter_mode != UA_SHUTTER_NORMAL) {
        /* Under a trigger the pulse period is the frame period. */
        dev_dbg(tc_dev->dev, "frame rate ignored under trigger\n");
        return 0;
    }
    mode = ua_mode_props(priv);
    if (!mode || !priv->hmax || !t->inck_hz || val <= 0)
        return -EINVAL;

    /* VMAX = inck * factor / (HMAX * fps*factor) */
    factor = mode->control_properties.framerate_factor;
    vmax = div64_u64((u64)t->inck_hz * factor, (u64)priv->hmax * (u64)val);
    floor = mode->image_properties.height + t->frame_length.p[0];
    if (priv->mode_vmax) {
        /*
         * The capture stack asks in whole frames per second. A request that
         * rounds to the mode's own rate keeps the frame length the mode was
         * programmed with, and the floor never exceeds it.
         */
        own = div64_u64((u64)t->inck_hz * factor,
                        (u64)priv->hmax * (u64)priv->mode_vmax);
        if ((own > (u64)val ? own - (u64)val : (u64)val - own) < factor)
            vmax = priv->mode_vmax;
        if (floor > priv->mode_vmax)
            floor = priv->mode_vmax;
    }
    if (vmax < floor)
        vmax = floor;
    if (vmax > 0xFFFFFF)
        vmax = 0xFFFFFF;

    err = ua_write_multi(priv, &t->frame_length, (u32)vmax);
    if (!err)
        priv->vmax = (u32)vmax;

    return ua_bus_result(priv, err, "frame length");
}

static int universal_aliensense_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
    struct universal_aliensense *priv = tegracam_get_privdata(tc_dev);
    const struct ua_table *t = priv ? priv->cur : NULL;
    const struct sensor_mode_properties *mode;
    u64 t_ns;
    u64 line_ns;
    u64 lines;
    u64 shs;
    u32 ceiling;

    if (!t || !t->exposure.present)
        return 0;
    if (priv->shutter_mode == UA_SHUTTER_FAST) {
        /* The XTRIG pulse width is the exposure; the shutter register is inert. */
        dev_dbg(tc_dev->dev, "exposure ignored under fast trigger\n");
        return 0;
    }
    mode = ua_mode_props(priv);
    if (!mode || !priv->hmax || !priv->vmax || !t->inck_hz || val < 0)
        return -EINVAL;

    t_ns = div64_u64((u64)val * 1000000000ULL,
                     mode->control_properties.exposure_factor ?
                         mode->control_properties.exposure_factor : 1);
    line_ns = div64_u64((u64)priv->hmax * 1000000000ULL, t->inck_hz);
    if (!line_ns)
        return -EINVAL;
    lines = t_ns > t->exposure.p[0] ? div64_u64(t_ns - t->exposure.p[0], line_ns) : 0;

    /* Sony inverted shutter: SHS = VMAX - lines, within [floor, VMAX - min_lines] */
    ceiling = priv->vmax > t->exposure.p[2] ? priv->vmax - t->exposure.p[2] : 0;
    shs = lines < priv->vmax ? priv->vmax - lines : 0;
    if (shs < t->exposure.p[1])
        shs = t->exposure.p[1];
    if (shs > ceiling)
        shs = ceiling;

    return ua_bus_result(priv, ua_write_multi(priv, &t->exposure, (u32)shs), "exposure");
}

static struct tegracam_ctrl_ops universal_aliensense_ctrl_ops = {
    .numctrls = ARRAY_SIZE(ctrl_cid_list),
    .ctrl_cid_list = ctrl_cid_list,
    .set_gain = universal_aliensense_set_gain,
    .set_exposure = universal_aliensense_set_exposure,
    .set_frame_rate = universal_aliensense_set_frame_rate,
    .set_group_hold = universal_aliensense_set_group_hold,
};

/* ---- private controls ---------------------------------------------- */

static int ua_priv_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct universal_aliensense *priv = ctrl->priv;
    const struct ua_table *t = priv->cur;
    int err;

    switch (ctrl->id) {
    case UA_CID_OPERATION_MODE:
        priv->operation_mode = ctrl->val;
        if (!t || !t->sync_sel.present)
            return 0;
        return ua_bus_result(priv, ua_write_multi(priv, &t->sync_sel,
                                                  t->sync_sel.p[ctrl->val ? 1 : 0]),
                             "operation mode");
    case UA_CID_SHUTTER_MODE:
        /* Recorded only: the host tool programs the conversion. */
        priv->shutter_mode = ctrl->val;
        return 0;
    case UA_CID_TEST_PATTERN:
        if (!t || !t->test_pattern.present)
            return 0;
        if (ctrl->val == 0)
            return ua_bus_result(priv, ua_write_multi(priv, &t->test_pattern,
                                                      t->test_pattern.p[0]),
                                 "test pattern off");
        err = regmap_write(priv->s_data->regmap, t->test_pattern.p[2] & 0xFFFF,
                           ctrl->val & 0xFF);
        if (!err)
            err = ua_write_multi(priv, &t->test_pattern, t->test_pattern.p[1]);
        return ua_bus_result(priv, err, "test pattern");
    case UA_CID_BLACK_LEVEL:
        if (!t || !t->black_level.present)
            return 0;
        return ua_bus_result(priv, ua_write_multi(priv, &t->black_level, ctrl->val),
                             "black level");
    default:
        return -EINVAL;
    }
}

static const struct v4l2_ctrl_ops ua_priv_ctrl_ops = {
    .s_ctrl = ua_priv_s_ctrl,
};

static const char * const ua_operation_mode_menu[] = {
    "Master Mode",
    "Slave Mode",
};

static const char * const ua_shutter_mode_menu[] = {
    "Normal",
    "Sequential Trigger",
    "Fast Trigger",
};

static const struct v4l2_ctrl_config ua_priv_ctrl_cfg[] = {
    {
        .ops = &ua_priv_ctrl_ops,
        .id = UA_CID_OPERATION_MODE,
        .name = "operation_mode",
        .type = V4L2_CTRL_TYPE_MENU,
        .min = 0,
        .max = ARRAY_SIZE(ua_operation_mode_menu) - 1,
        .def = 0,
        .qmenu = ua_operation_mode_menu,
    },
    {
        .ops = &ua_priv_ctrl_ops,
        .id = UA_CID_SHUTTER_MODE,
        .name = "shutter_mode",
        .type = V4L2_CTRL_TYPE_MENU,
        .min = 0,
        .max = ARRAY_SIZE(ua_shutter_mode_menu) - 1,
        .def = 0,
        .qmenu = ua_shutter_mode_menu,
    },
    {
        .ops = &ua_priv_ctrl_ops,
        .id = UA_CID_TEST_PATTERN,
        .name = "test_pattern",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .min = 0,
        .max = 255,
        .step = 1,
        .def = 0,
    },
    {
        .ops = &ua_priv_ctrl_ops,
        .id = UA_CID_BLACK_LEVEL,
        .name = "black_level",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .min = 0,
        .max = 0xFFFF,
        .step = 1,
        .def = 0,
    },
};

static int ua_add_priv_ctrls(struct universal_aliensense *priv)
{
    struct v4l2_ctrl_handler *hdl = priv->s_data->ctrl_handler;
    int i;

    if (!hdl)
        return -ENODEV;
    for (i = 0; i < ARRAY_SIZE(ua_priv_ctrl_cfg); i++) {
        if (!v4l2_ctrl_new_custom(hdl, &ua_priv_ctrl_cfg[i], priv))
            return hdl->error ? hdl->error : -EINVAL;
    }

    return 0;
}

/* ---- power, dt, mode, streaming ------------------------------------ */

static int universal_aliensense_power_on(struct camera_common_data *s_data)
{
    if (s_data && s_data->power)
        s_data->power->state = SWITCH_ON;

    return 0;
}

static int universal_aliensense_power_off(struct camera_common_data *s_data)
{
    if (s_data && s_data->power)
        s_data->power->state = SWITCH_OFF;

    return 0;
}

static int universal_aliensense_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;

	return 0;
}

static int universal_aliensense_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	const char *mclk_name;
	const char *parentclk_name;
	struct clk *parent;
	int err = 0;

	if (!pdata) {
		dev_err(dev, "pdata missing\n");
		return -EFAULT;
	}

	mclk_name = pdata->mclk_name ?
		pdata->mclk_name : "extperiph1";
	pw->mclk = devm_clk_get(dev, mclk_name);
	if (IS_ERR(pw->mclk)) {
		dev_err(dev, "unable to get clock %s\n", mclk_name);
		return PTR_ERR(pw->mclk);
	}

	parentclk_name = pdata->parentclk_name;
	if (parentclk_name) {
		parent = devm_clk_get(dev, parentclk_name);
		if (IS_ERR(parent)) {
			dev_err(dev, "unable to get parent clock %s",
							parentclk_name);
		} else
			clk_set_parent(pw->mclk, parent);
	}

	pw->state = SWITCH_OFF;

	return err;
}

static struct camera_common_pdata *universal_aliensense_parse_dt(
                             struct tegracam_device *tc_dev)
{
    struct device *dev = tc_dev->dev;
    struct device_node *sensor_node = dev->of_node;
    struct device_node *i2c_mux_ch_node;
    struct device_node *gmsl_node;
    struct camera_common_pdata *board_priv_pdata;
    const struct of_device_id *match;
    struct camera_common_pdata *ret = NULL;
    int err;

    if (!sensor_node)
        return NULL;

    match = of_match_device(universal_aliensense_of_match, dev);
    if (!match) {
        dev_err(dev, "Failed to find matching dt id\n");
        return NULL;
    }

    board_priv_pdata = devm_kzalloc(dev,
                    sizeof(*board_priv_pdata), GFP_KERNEL);
    if (!board_priv_pdata)
        return NULL;

    err = camera_common_parse_clocks(dev, board_priv_pdata);
    if (err) {
        dev_err(dev, "Failed to find clocks\n");
        goto error;
    }

    gmsl_node = of_get_child_by_name(sensor_node, "gmsl-link");
    if (gmsl_node == NULL) {
        dev_warn(dev, "initializing mipi...\n");
        dev_dbg(dev, "no gmsl-link property found in dt...\n");
    } else {
        dev_warn(dev, "initializing GMSL...\n");
        dev_dbg(dev, "gmsl-link property found in dt...\n");
    }

    i2c_mux_ch_node = of_get_parent(sensor_node);
    if (!i2c_mux_ch_node) {
        dev_err(dev, "i2c mux channel node not found in dt\n");
        goto error;
    }

    return board_priv_pdata;

error:
    devm_kfree(dev, board_priv_pdata);
    return ret;
}

/*
 * A mode selects its sensor's control table and refreshes the timing the
 * laws divide by. The registers may not answer before the host tool brings
 * the pod up; the previous values then stand.
 */
static int universal_aliensense_set_mode(struct tegracam_device *tc_dev)
{
    struct universal_aliensense *priv = tegracam_get_privdata(tc_dev);
    struct camera_common_data *s_data = tc_dev->s_data;
    u32 value;

    if (!priv)
        return 0;
    priv->cur = ua_table_for_mode(priv, s_data->mode_prop_idx);
    if (!priv->cur) {
        dev_dbg(tc_dev->dev, "mode %d: no control table, controls inert\n",
                s_data->mode_prop_idx);
        return 0;
    }
    if (priv->cur->line_length.present &&
        !ua_read_multi(priv, &priv->cur->line_length, &value) && value)
        priv->hmax = value;
    if (priv->cur->frame_length.present &&
        !ua_read_multi(priv, &priv->cur->frame_length, &value) && value)
        priv->vmax = priv->mode_vmax = value;

    dev_dbg(tc_dev->dev, "mode %d: %s, line length %u, frame length %u\n",
            s_data->mode_prop_idx, priv->cur->sensor, priv->hmax, priv->vmax);

    return 0;
}

static int universal_aliensense_start_streaming(struct tegracam_device *tc_dev)
{
    return 0;
}

static int universal_aliensense_stop_streaming(struct tegracam_device *tc_dev)
{
    return 0;
}

static struct camera_common_sensor_ops universal_aliensense_common_ops = {
    .numfrmfmts = ARRAY_SIZE(universal_aliensense_frmfmt),
    .frmfmt_table = universal_aliensense_frmfmt,
    .power_on = universal_aliensense_power_on,
    .power_off = universal_aliensense_power_off,
    .write_reg = universal_aliensense_write_reg,
    .read_reg = universal_aliensense_read_reg,
    .parse_dt = universal_aliensense_parse_dt,
    .power_get = universal_aliensense_power_get,
    .power_put = universal_aliensense_power_put,
    .set_mode = universal_aliensense_set_mode,
    .start_streaming = universal_aliensense_start_streaming,
    .stop_streaming = universal_aliensense_stop_streaming,
};

static int universal_aliensense_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);

    dev_dbg(&client->dev, "%s", __func__);

    return 0;
}

static const struct v4l2_subdev_internal_ops universal_aliensense_subdev_internal_ops = {
    .open = universal_aliensense_open,
};

static int universal_aliensense_power_on_probe(struct universal_aliensense *priv)
{
    struct device *dev;
    struct camera_common_power_rail *pw;
    int err;

    if (!priv || !priv->s_data || !priv->s_data->power)
        return -EINVAL;

    dev = &priv->i2c_client->dev;
    pw = priv->s_data->power;

    dev_info(dev, "Enabling mclk\n");
    err = clk_prepare_enable(pw->mclk);
    if (err) {
        dev_err(dev, "%s: failed to enable mclk\n", __func__);
        return err;
    }

    pw->state = SWITCH_ON;
    dev_info(dev, "Powered on in probe\n");

    return 0;
}

/*
 * The register map's widths are the sensor's, declared on the node by the
 * overlay generator from the personality's I2C profile; the defaults are
 * the Sony CCI shape (16-bit registers, 8-bit values).
 */
static int ua_regmap_config_from_dt(struct universal_aliensense *priv, struct device *dev)
{
    u32 reg_bits = 16;
    u32 val_bits = 8;

    priv->regmap_config = sensor_regmap_config;
    of_property_read_u32(dev->of_node, "aliensense,reg-bits", &reg_bits);
    of_property_read_u32(dev->of_node, "aliensense,val-bits", &val_bits);
    if ((reg_bits != 8 && reg_bits != 16) || (val_bits != 8 && val_bits != 16)) {
        dev_err(dev, "unsupported register map %u/%u (reg/val bits)\n", reg_bits, val_bits);
        return -EINVAL;
    }
    priv->regmap_config.reg_bits = reg_bits;
    priv->regmap_config.val_bits = val_bits;
    dev_info(dev, "register map %u-bit registers, %u-bit values\n", reg_bits, val_bits);

    return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int universal_aliensense_probe(struct i2c_client *client)
#else
static int universal_aliensense_probe(struct i2c_client *client,
              const struct i2c_device_id *id)
#endif
{
    struct device *dev = &client->dev;
    struct tegracam_device *tc_dev = NULL;
    struct universal_aliensense *priv = NULL;
    int err = 0;

    dev_info(dev, "probing v4l2 sensor assumed at addr 0x%0x\n", client->addr);

    if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
        return -EINVAL;

    priv = devm_kzalloc(dev, sizeof(struct universal_aliensense), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    tc_dev = devm_kzalloc(dev, sizeof(struct tegracam_device), GFP_KERNEL);
    if (!tc_dev)
        return -ENOMEM;

    priv->i2c_client = tc_dev->client = client;
    priv->probing = true;
    tc_dev->dev = dev;
    strncpy(tc_dev->name, "universal", sizeof(tc_dev->name));
    err = ua_regmap_config_from_dt(priv, dev);
    if (err)
        return err;
    tc_dev->dev_regmap_config = &priv->regmap_config;
    tc_dev->sensor_ops = &universal_aliensense_common_ops;
    tc_dev->v4l2sd_internal_ops = &universal_aliensense_subdev_internal_ops;
    tc_dev->tcctrl_ops = &universal_aliensense_ctrl_ops;

    err = tegracam_device_register(tc_dev);
    if (err) {
        dev_err(dev, "tegra camera driver registration failed\n");
        return err;
    }

    priv->tc_dev = tc_dev;
    priv->s_data = tc_dev->s_data;
    priv->subdev = &tc_dev->s_data->subdev;
    tegracam_set_privdata(tc_dev, (void *)priv);

    err = ua_parse_tables(priv);
    if (err) {
        tegracam_device_unregister(tc_dev);
        return err;
    }

    err = tegracam_v4l2subdev_register(tc_dev, true);
    if (err) {
        dev_err(dev, "tegra camera subdev registration failed\n");
        tegracam_device_unregister(tc_dev);
        return err;
    }

    err = ua_add_priv_ctrls(priv);
    if (err) {
        dev_err(dev, "private controls failed: %d\n", err);
        tegracam_v4l2subdev_unregister(tc_dev);
        tegracam_device_unregister(tc_dev);
        return err;
    }

    err = universal_aliensense_power_on_probe(priv);
    if (err) {
        tegracam_v4l2subdev_unregister(tc_dev);
        tegracam_device_unregister(tc_dev);
        return err;
    }

    priv->probing = false;
    dev_info(dev, "universal_aliensense sensor driver probed (%d control tables)\n",
             priv->ntables);

    return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int universal_aliensense_remove(struct i2c_client *client)
#else
static void universal_aliensense_remove(struct i2c_client *client)
#endif
{
    struct camera_common_data *s_data = to_camera_common_data(&client->dev);
    struct universal_aliensense *priv = (struct universal_aliensense *)s_data->priv;

    tegracam_v4l2subdev_unregister(priv->tc_dev);
    tegracam_device_unregister(priv->tc_dev);

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
    return 0;
#endif
}

static struct i2c_driver universal_aliensense_i2c_driver = {
    .driver = {
           .name = "universal",
           .owner = THIS_MODULE,
           .of_match_table = of_match_ptr(universal_aliensense_of_match),
            },
    .probe = universal_aliensense_probe,
    .remove = universal_aliensense_remove,
};

module_i2c_driver(universal_aliensense_i2c_driver);

MODULE_DESCRIPTION("V4L2 driver for universal_aliensense sensor");
MODULE_AUTHOR("RidgeRun");
MODULE_LICENSE("GPL v2");
