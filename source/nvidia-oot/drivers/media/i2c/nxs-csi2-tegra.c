// SPDX-License-Identifier: GPL-2.0-only
/*
 * nxs-csi2-tegra.c - the NXS camera map as a tegracam sensor
 *
 * Copyright (C) 2026 Aliensense
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

#include <linux/i2c.h>
#include <linux/kstrtox.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/videodev2.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>
#include <media/camera_common.h>

#include "nxs-csi2-core.h"

/*
 * The driver knows no sensor. A pod's NXS unit, or a smart camera, serves
 * the camera map, and nxs-csi2-core reads it, commits its rows and offers
 * the rows the framework does not map as custom controls. This adapter is
 * tegracam's: the gain, exposure and frame-rate callbacks commit the rows
 * in the factor units of the device tree's modes, a group hold gathers one
 * commit per frame, and set_mode runs the map's mode of the session's mode
 * node. The host tool owns the graph: it brings the port up, starts and
 * stops the stream and converts the trigger, so this driver commits no
 * stream gate. The map answers only once the hub's link is up, so the
 * frmfmt table is the tree's mode nodes, and the first session the map
 * answers checks its mode table against them: the overlay generator writes
 * both from one personality. The driver's sequence is
 * docs/bring-up/nxs-csi2-mainline.md in the redbrain tree.
 *
 * A node with the property aliensense,passive is a camera whose stream
 * reaches this host while another host owns the bus the map answers on. The
 * driver then transfers nothing: the modes are the tree's, no control
 * commits, and a session changes nothing on the device.
 */

/* Mode nodes a tree carries at most, mode0 onward. */
#define NXS_TEGRA_MODES_MAX	64

static const struct of_device_id nxs_csi2_tegra_of_match[] = {
	{ .compatible = "aliensense,nxs-csi2", },
	{ },
};

MODULE_DEVICE_TABLE(of, nxs_csi2_tegra_of_match);

/* Group hold is not listed: the framework registers it and calls .set_group_hold. */
static const u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

struct nxs_csi2_tegra {
	struct i2c_client *client;
	struct tegracam_device *tc_dev;
	struct camera_common_data *s_data;
	struct regmap_config regmap_config;
	/* The sensor ops of this node: the frmfmt table is the node's mode list. */
	struct camera_common_sensor_ops ops;
	struct camera_common_frmfmt *frmfmt;
	int *framerates;
	unsigned int nr_frmfmt;

	struct nxs_csi2_core core;
	/* A passive node: the modes are the tree's, and the bus is another host's. */
	bool passive;
	/* The map's modes agree with the tree's, checked when the map first answered. */
	bool checked;
	/* The core's controls stand, created when the map first answered. */
	bool controls;
	/* The map read READY or FAULT at this session's start: the controls commit. */
	bool configured;
	/* The rate in effect, in milli-frames per second, which a request that rounds to it keeps. */
	u32 mode_rate_mfps;
	/* The commit a group hold gathers. */
	bool held;
	u64 pending;
};

static const struct regmap_config nxs_map_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

/* ---- the tree's modes ------------------------------------------------ */

/* A tegracam mode property: a decimal number as a string. */
static int nxs_mode_u32(struct device_node *node, const char *name, u32 *value)
{
	const char *str;
	int err;

	err = of_property_read_string(node, name, &str);
	if (err)
		return err;

	return kstrtou32(str, 10, value);
}

/*
 * The frmfmt table from the tree's mode nodes: each mode's size, and its
 * fps ceiling from max_framerate over framerate_factor. The map answers
 * only once the hub's link is up, so the table cannot be its.
 */
static int nxs_csi2_tegra_parse_modes(struct nxs_csi2_tegra *priv)
{
	struct device *dev = &priv->client->dev;
	struct device_node *node;
	char name[16];
	unsigned int n;
	unsigned int i;
	int err;

	for (n = 0; n < NXS_TEGRA_MODES_MAX; n++) {
		snprintf(name, sizeof(name), "mode%u", n);
		node = of_get_child_by_name(dev->of_node, name);
		if (!node)
			break;
		of_node_put(node);
	}
	if (!n) {
		dev_err(dev, "no mode node\n");
		return -EINVAL;
	}
	priv->frmfmt = devm_kcalloc(dev, n, sizeof(*priv->frmfmt), GFP_KERNEL);
	priv->framerates = devm_kcalloc(dev, n, sizeof(*priv->framerates), GFP_KERNEL);
	if (!priv->frmfmt || !priv->framerates)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		struct camera_common_frmfmt *fmt = &priv->frmfmt[i];
		u32 width, height, max_framerate, factor;

		snprintf(name, sizeof(name), "mode%u", i);
		node = of_get_child_by_name(dev->of_node, name);
		err = nxs_mode_u32(node, "active_w", &width);
		if (!err)
			err = nxs_mode_u32(node, "active_h", &height);
		if (!err)
			err = nxs_mode_u32(node, "max_framerate", &max_framerate);
		if (!err)
			err = nxs_mode_u32(node, "framerate_factor", &factor);
		of_node_put(node);
		if (err || !factor) {
			dev_err(dev, "%s: active_w, active_h, max_framerate and framerate_factor are needed\n",
				name);
			return err ? err : -EINVAL;
		}
		priv->framerates[i] = max_framerate / factor;
		fmt->size.width = width;
		fmt->size.height = height;
		fmt->framerates = &priv->framerates[i];
		fmt->num_framerates = 1;
		fmt->hdr_en = false;
		fmt->mode = i;
	}
	priv->nr_frmfmt = n;

	return 0;
}

/* The selected mode's properties: the factors the control values carry. */
static const struct sensor_mode_properties *nxs_mode_props(struct nxs_csi2_tegra *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	int idx = s_data->mode_prop_idx;

	if (idx < 0 || idx >= s_data->sensor_props.num_modes)
		return NULL;

	return &s_data->sensor_props.sensor_modes[idx];
}

/* The bits a pixel carries in a raw Bayer format, 0 for another format. */
static u8 nxs_bit_depth(u32 pixel_format)
{
	switch (pixel_format) {
	case V4L2_PIX_FMT_SRGGB8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SBGGR8:
		return 8;
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SBGGR10:
		return 10;
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SBGGR12:
		return 12;
	default:
		return 0;
	}
}

/* The map's mode of a tree mode: the row of its width, height and bit depth. */
static const struct nxs_csi2_mode *nxs_map_mode(struct nxs_csi2_tegra *priv,
						const struct sensor_mode_properties *props)
{
	const struct sensor_image_properties *image = &props->image_properties;

	return nxs_csi2_core_find_geometry(&priv->core, image->width, image->height,
					   nxs_bit_depth(image->pixel_format));
}

/*
 * The tree's modes against the map's: every mode node names a row of the
 * map, since the overlay generator writes both from one personality. A
 * mode the map does not serve refuses the session with the line that says
 * what to regenerate; a rate past the map's, and a count that differs, are
 * logged.
 */
static int nxs_csi2_tegra_check_modes(struct nxs_csi2_tegra *priv)
{
	const struct sensor_properties *props = &priv->s_data->sensor_props;
	struct device *dev = &priv->client->dev;
	unsigned int i;

	for (i = 0; i < props->num_modes; i++) {
		const struct sensor_mode_properties *m = &props->sensor_modes[i];
		const struct nxs_csi2_mode *mode = nxs_map_mode(priv, m);
		u64 factor = m->control_properties.framerate_factor ?: 1;
		u64 fps_milli = div64_u64((u64)m->control_properties.max_framerate * 1000, factor);

		if (!mode) {
			dev_err(dev, "mode%u: %ux%u at %u bits is in the device tree and not in the camera map: regenerate the overlay (nxs switch)\n",
				i, m->image_properties.width, m->image_properties.height,
				nxs_bit_depth(m->image_properties.pixel_format));
			return -EINVAL;
		}
		if (mode->fps_max_milli && fps_milli > mode->fps_max_milli)
			dev_warn(dev, "mode%u: %llu.%03llu fps in the device tree, the camera map tops at %u.%03u\n",
				 i, fps_milli / 1000, fps_milli % 1000, mode->fps_max_milli / 1000,
				 mode->fps_max_milli % 1000);
	}
	if (props->num_modes != priv->core.mode_count)
		dev_warn(dev, "the device tree names %u modes, the camera map serves %u\n",
			 (unsigned int)props->num_modes, priv->core.mode_count);

	return 0;
}

/* ---- the map at a session -------------------------------------------- */

/*
 * The first time the map answers: its modes against the tree's, and the
 * core's controls on the framework's handler.
 */
static int nxs_csi2_tegra_first_answer(struct nxs_csi2_tegra *priv)
{
	struct device *dev = &priv->client->dev;
	int err;

	if (!priv->checked) {
		err = nxs_csi2_tegra_check_modes(priv);
		if (err)
			return err;
		priv->checked = true;
	}
	if (!priv->controls) {
		err = nxs_csi2_core_init_controls(&priv->core, priv->s_data->ctrl_handler,
						  NXS_CSI2_CID_BASE);
		if (err) {
			dev_err(dev, "the camera map's controls failed: %d\n", err);
			return err;
		}
		priv->controls = true;
	}

	return 0;
}

/*
 * The map at a session's start. A map silent at probe is read now, and one
 * read before is checked for the rows it had, which the controls stand on.
 * Returns 1 when the map does not answer, so the session runs with the
 * controls inert, on the stream the host tool started.
 */
static int nxs_csi2_tegra_reach(struct nxs_csi2_tegra *priv, struct nxs_csi2_status *status)
{
	struct nxs_csi2_core *core = &priv->core;
	struct device *dev = &priv->client->dev;
	int err;

	if (!core->read) {
		err = nxs_csi2_core_init(core, priv->client);
		if (err == -ENODEV)
			return err;
		if (err) {
			dev_dbg(dev, "camera map not reachable (%d): the controls stay inert\n",
				err);
			return 1;
		}
	}
	err = nxs_csi2_tegra_first_answer(priv);
	if (err)
		return err;
	err = nxs_csi2_core_read_status(core, status);
	if (err) {
		dev_dbg(dev, "camera map not reachable (%d): the controls stay inert\n", err);
		return 1;
	}
	if (status->param_count != core->param_count || status->mode_count != core->mode_count) {
		dev_err(dev, "the camera map changed since its rows were read (%u rows, %u modes): reload the module\n",
			status->param_count, status->mode_count);
		return -ENODEV;
	}

	return 0;
}

/* ---- the commits ----------------------------------------------------- */

/* The rows gathered so far, committed: one commit per kind, by the core. */
static int nxs_csi2_tegra_flush(struct nxs_csi2_tegra *priv, const char *what)
{
	u64 mask = priv->pending;
	int err;

	priv->pending = 0;
	err = nxs_csi2_core_commit(&priv->core, mask, what);
	if (err == NXS_CSI2_INERT) {
		dev_dbg(&priv->client->dev, "%s: the camera map is INERT, the values wait for the next session\n",
			what);
		return 0;
	}

	return err;
}

/*
 * A row staged under a hold waits for the hold's end; outside one, or for
 * a kind a hold does not gather, it commits. A row the map does not serve
 * stages nothing.
 */
static int nxs_csi2_tegra_stage(struct nxs_csi2_tegra *priv, unsigned int id, u32 value,
				const char *what)
{
	int row = nxs_csi2_core_row(&priv->core, id);

	if (row < 0)
		return 0;
	nxs_csi2_core_stage(&priv->core, row, value);
	priv->pending |= BIT_ULL(row);
	if (priv->held && priv->core.rows[row].kind == NXS_PARAM_KIND_APPLY)
		return 0;

	return nxs_csi2_tegra_flush(priv, what);
}

/* ---- tegracam control callbacks ------------------------------------ */

static int nxs_csi2_tegra_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	struct nxs_csi2_tegra *priv = tegracam_get_privdata(tc_dev);

	if (!priv || priv->passive)
		return 0;
	priv->held = val;
	if (val || !priv->pending)
		return 0;

	return nxs_csi2_tegra_flush(priv, "group hold");
}

/* The gain in the mode's factor (tenths of a decibel at gain_factor 10), as millidecibels. */
static int nxs_csi2_tegra_set_gain(struct tegracam_device *tc_dev, s64 val)
{
	struct nxs_csi2_tegra *priv = tegracam_get_privdata(tc_dev);
	const struct sensor_mode_properties *mode;
	u32 factor;

	if (!priv || !priv->configured)
		return 0;
	mode = nxs_mode_props(priv);
	if (!mode || val < 0)
		return -EINVAL;
	factor = mode->control_properties.gain_factor ? mode->control_properties.gain_factor : 1;

	return nxs_csi2_tegra_stage(priv, NXS_PARAM_ID_GAIN_MDB,
				    (u32)div64_u64((u64)val * 1000, factor), "gain");
}

/*
 * The rate as the row's milli-frames per second, from the control's frames
 * per second times the mode's framerate_factor. Under a trigger the pulse
 * period is the frame period, so the rate is left alone. A request that
 * rounds to the session's own rate keeps the frame it runs, since the
 * capture stack asks in whole frames per second.
 */
static int nxs_csi2_tegra_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
	struct nxs_csi2_tegra *priv = tegracam_get_privdata(tc_dev);
	const struct sensor_mode_properties *mode;
	u64 factor;
	u64 own;
	u32 mfps;

	if (!priv || !priv->configured)
		return 0;
	if (priv->core.trigger_live != NXS_TRIGGER_CODE_FREERUN) {
		dev_dbg(tc_dev->dev, "frame rate left to the trigger pulse\n");
		return 0;
	}
	mode = nxs_mode_props(priv);
	if (!mode || val <= 0)
		return -EINVAL;
	factor = mode->control_properties.framerate_factor ?
		 mode->control_properties.framerate_factor : 1;
	mfps = (u32)div64_u64((u64)val * 1000, factor);
	if (priv->mode_rate_mfps) {
		own = div64_u64((u64)priv->mode_rate_mfps * factor, 1000);
		if ((own > (u64)val ? own - (u64)val : (u64)val - own) < factor)
			mfps = priv->mode_rate_mfps;
	}

	return nxs_csi2_tegra_stage(priv, NXS_PARAM_ID_FRAME_RATE_MFPS, mfps, "frame rate");
}

/* The exposure row says the fast trigger's pulse width sets it. */
static bool nxs_exposure_set_by_trigger(struct nxs_csi2_tegra *priv)
{
	int row = nxs_csi2_core_row(&priv->core, NXS_PARAM_ID_EXPOSURE_US);

	return row >= 0 && (priv->core.rows[row].flags & NXS_ROW_FLAG_SET_BY_TRIGGER);
}

/*
 * The exposure in microseconds. A row the trigger pulse sets takes no
 * value under the fast trigger, whose pulse width is the exposure; a
 * sequential trigger exposes at the row's value. The device clamps a value
 * past the frame.
 */
static int nxs_csi2_tegra_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
	struct nxs_csi2_tegra *priv = tegracam_get_privdata(tc_dev);
	const struct sensor_mode_properties *mode;
	u64 factor;

	if (!priv || !priv->configured)
		return 0;
	if (nxs_exposure_set_by_trigger(priv) &&
	    priv->core.trigger_live == NXS_TRIGGER_CODE_FAST) {
		dev_dbg(tc_dev->dev, "exposure left to the trigger pulse\n");
		return 0;
	}
	mode = nxs_mode_props(priv);
	if (!mode || val < 0)
		return -EINVAL;
	factor = mode->control_properties.exposure_factor ?
		 mode->control_properties.exposure_factor : 1;

	return nxs_csi2_tegra_stage(priv, NXS_PARAM_ID_EXPOSURE_US,
				    (u32)div64_u64((u64)val * 1000000ULL, factor), "exposure");
}

static struct tegracam_ctrl_ops nxs_csi2_tegra_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	.set_gain = nxs_csi2_tegra_set_gain,
	.set_exposure = nxs_csi2_tegra_set_exposure,
	.set_frame_rate = nxs_csi2_tegra_set_frame_rate,
	.set_group_hold = nxs_csi2_tegra_set_group_hold,
};

/* ---- power, dt, mode, streaming ------------------------------------ */

static int nxs_csi2_tegra_power_on(struct camera_common_data *s_data)
{
	if (s_data && s_data->power)
		s_data->power->state = SWITCH_ON;

	return 0;
}

static int nxs_csi2_tegra_power_off(struct camera_common_data *s_data)
{
	if (s_data && s_data->power)
		s_data->power->state = SWITCH_OFF;

	return 0;
}

static int nxs_csi2_tegra_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;

	return 0;
}

static int nxs_csi2_tegra_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	const char *mclk_name;
	const char *parentclk_name;
	struct clk *parent;

	if (!pdata) {
		dev_err(dev, "pdata missing\n");
		return -EFAULT;
	}
	mclk_name = pdata->mclk_name ? pdata->mclk_name : "extperiph1";
	pw->mclk = devm_clk_get(dev, mclk_name);
	if (IS_ERR(pw->mclk)) {
		dev_err(dev, "unable to get clock %s\n", mclk_name);
		return PTR_ERR(pw->mclk);
	}
	parentclk_name = pdata->parentclk_name;
	if (parentclk_name) {
		parent = devm_clk_get(dev, parentclk_name);
		if (IS_ERR(parent))
			dev_err(dev, "unable to get parent clock %s", parentclk_name);
		else
			clk_set_parent(pw->mclk, parent);
	}
	pw->state = SWITCH_OFF;

	return 0;
}

static struct camera_common_pdata *nxs_csi2_tegra_parse_dt(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_pdata *board_priv_pdata;
	int err;

	if (!dev->of_node)
		return NULL;
	if (!of_match_device(nxs_csi2_tegra_of_match, dev)) {
		dev_err(dev, "Failed to find matching dt id\n");
		return NULL;
	}
	board_priv_pdata = devm_kzalloc(dev, sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;
	err = camera_common_parse_clocks(dev, board_priv_pdata);
	if (err) {
		dev_err(dev, "Failed to find clocks\n");
		devm_kfree(dev, board_priv_pdata);
		return NULL;
	}

	return board_priv_pdata;
}

/*
 * Each session reads the map. A mode node whose row the map does not run
 * is a RUN commit of that row, in standby: a streaming sensor parks first
 * and starts again after the run. The rows staged while the map was INERT
 * commit, and the values in effect are read. A map that does not answer,
 * or has not configured its sensor, leaves the controls inert and the
 * session to the stream the host tool started. The core's controls follow
 * the values in effect under the handler's lock, which no caller of
 * set_mode holds.
 */
static int nxs_csi2_tegra_set_mode(struct tegracam_device *tc_dev)
{
	struct nxs_csi2_tegra *priv = tegracam_get_privdata(tc_dev);
	struct nxs_csi2_core *core = &priv->core;
	struct device *dev = tc_dev->dev;
	const struct sensor_mode_properties *props;
	const struct nxs_csi2_mode *mode;
	struct nxs_csi2_status status;
	bool streamed;
	int frame;
	int err;

	if (!priv || priv->passive)
		return 0;
	priv->configured = false;
	priv->pending = 0;
	err = nxs_csi2_tegra_reach(priv, &status);
	if (err < 0)
		return err;
	if (err > 0)
		return 0;
	if (status.state == NXS_MAP_STATE_INERT) {
		dev_dbg(dev, "camera map INERT: the controls stay inert\n");
		return 0;
	}
	priv->configured = true;
	props = nxs_mode_props(priv);
	mode = props ? nxs_map_mode(priv, props) : NULL;
	if (!mode) {
		dev_err(dev, "the session's mode is not in the camera map\n");
		return -EINVAL;
	}
	if (nxs_csi2_core_row(core, NXS_PARAM_ID_MODE) >= 0 && mode->value != status.current_mode) {
		dev_info(dev, "mode %u asked, the camera map runs %u: switching in standby\n",
			 mode->value, status.current_mode);
		streamed = status.stream && nxs_csi2_core_row(core, NXS_PARAM_ID_STREAM) >= 0;
		err = nxs_csi2_core_run_mode(core, mode, false);
		if (!err && streamed)
			err = nxs_csi2_core_gate(core, true);
		if (err)
			return err > 0 ? 0 : err;
	}
	if (core->held) {
		err = nxs_csi2_core_commit_held(core);
		if (err < 0)
			return err;
	}
	mutex_lock(priv->s_data->ctrl_handler->lock);
	err = nxs_csi2_core_refresh(core);
	mutex_unlock(priv->s_data->ctrl_handler->lock);
	if (err)
		return err;
	frame = nxs_csi2_core_row(core, NXS_PARAM_ID_FRAME_RATE_MFPS);
	priv->mode_rate_mfps = frame >= 0 ? core->rows[frame].live : 0;

	return 0;
}

/* The host tool owns the stream gate: `nxs <port> on` starts it, `off` parks it. */
static int nxs_csi2_tegra_start_streaming(struct tegracam_device *tc_dev)
{
	return 0;
}

static int nxs_csi2_tegra_stop_streaming(struct tegracam_device *tc_dev)
{
	return 0;
}

static int nxs_csi2_tegra_read_reg(struct camera_common_data *s_data, u16 addr, u8 *val)
{
	unsigned int v = 0;
	int err = regmap_read(s_data->regmap, addr, &v);

	*val = v & 0xff;

	return err;
}

static int nxs_csi2_tegra_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
	return regmap_write(s_data->regmap, addr, val);
}

/* The frmfmt table and its count are the node's, set at probe from the tree's modes. */
static const struct camera_common_sensor_ops nxs_csi2_tegra_sensor_ops = {
	.power_on = nxs_csi2_tegra_power_on,
	.power_off = nxs_csi2_tegra_power_off,
	.write_reg = nxs_csi2_tegra_write_reg,
	.read_reg = nxs_csi2_tegra_read_reg,
	.parse_dt = nxs_csi2_tegra_parse_dt,
	.power_get = nxs_csi2_tegra_power_get,
	.power_put = nxs_csi2_tegra_power_put,
	.set_mode = nxs_csi2_tegra_set_mode,
	.start_streaming = nxs_csi2_tegra_start_streaming,
	.stop_streaming = nxs_csi2_tegra_stop_streaming,
};

static int nxs_csi2_tegra_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static const struct v4l2_subdev_internal_ops nxs_csi2_tegra_subdev_internal_ops = {
	.open = nxs_csi2_tegra_open,
};

static int nxs_csi2_tegra_power_on_probe(struct nxs_csi2_tegra *priv)
{
	struct device *dev = &priv->client->dev;
	struct camera_common_power_rail *pw;
	int err;

	if (!priv->s_data || !priv->s_data->power)
		return -EINVAL;
	pw = priv->s_data->power;
	err = clk_prepare_enable(pw->mclk);
	if (err) {
		dev_err(dev, "%s: failed to enable mclk\n", __func__);
		return err;
	}
	pw->state = SWITCH_ON;

	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int nxs_csi2_tegra_probe(struct i2c_client *client)
#else
static int nxs_csi2_tegra_probe(struct i2c_client *client, const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct nxs_csi2_tegra *priv;
	int err;

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	tc_dev = devm_kzalloc(dev, sizeof(*tc_dev), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	priv->client = client;
	priv->passive = of_property_read_bool(dev->of_node, "aliensense,passive");
	err = nxs_csi2_tegra_parse_modes(priv);
	if (err)
		return err;
	priv->ops = nxs_csi2_tegra_sensor_ops;
	priv->ops.frmfmt_table = priv->frmfmt;
	priv->ops.numfrmfmts = priv->nr_frmfmt;
	priv->regmap_config = nxs_map_regmap_config;
	tc_dev->client = client;
	tc_dev->dev = dev;
	strscpy(tc_dev->name, "nxs-csi2", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &priv->regmap_config;
	tc_dev->sensor_ops = &priv->ops;
	tc_dev->v4l2sd_internal_ops = &nxs_csi2_tegra_subdev_internal_ops;
	tc_dev->tcctrl_ops = &nxs_csi2_tegra_ctrl_ops;

	err = tegracam_device_register(tc_dev);
	if (err) {
		dev_err(dev, "tegra camera driver registration failed\n");
		return err;
	}
	priv->tc_dev = tc_dev;
	priv->s_data = tc_dev->s_data;
	tegracam_set_privdata(tc_dev, (void *)priv);

	/*
	 * The pod answers only once the hub's link is up, which the host tool
	 * does after boot: a map silent at probe is read at the first session,
	 * and one that answers is checked now. A passive node is never read.
	 */
	if (priv->passive) {
		dev_info(dev, "passive: %u modes from the device tree, no transfer\n",
			 priv->nr_frmfmt);
	} else {
		err = nxs_csi2_core_init(&priv->core, client);
		if (err == -ENODEV) {
			tegracam_device_unregister(tc_dev);
			return err;
		}
		if (err)
			dev_info(dev, "camera map not reachable at probe, read at the first session\n");
	}

	err = tegracam_v4l2subdev_register(tc_dev, true);
	if (err) {
		dev_err(dev, "tegra camera subdev registration failed\n");
		tegracam_device_unregister(tc_dev);
		return err;
	}
	/* The modes against the tree's and the core's controls, now or at the first session. */
	if (priv->core.read)
		nxs_csi2_tegra_first_answer(priv);
	err = nxs_csi2_tegra_power_on_probe(priv);
	if (err) {
		tegracam_v4l2subdev_unregister(tc_dev);
		tegracam_device_unregister(tc_dev);
		return err;
	}

	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int nxs_csi2_tegra_remove(struct i2c_client *client)
#else
static void nxs_csi2_tegra_remove(struct i2c_client *client)
#endif
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct nxs_csi2_tegra *priv = (struct nxs_csi2_tegra *)s_data->priv;

	tegracam_v4l2subdev_unregister(priv->tc_dev);
	tegracam_device_unregister(priv->tc_dev);

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
	return 0;
#endif
}

static struct i2c_driver nxs_csi2_tegra_i2c_driver = {
	.driver = {
		.name = "nxs-csi2",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nxs_csi2_tegra_of_match),
	},
	.probe = nxs_csi2_tegra_probe,
	.remove = nxs_csi2_tegra_remove,
};

module_i2c_driver(nxs_csi2_tegra_i2c_driver);

MODULE_DESCRIPTION("V4L2 driver for the NXS camera map on tegracam");
MODULE_AUTHOR("Aliensense");
MODULE_LICENSE("GPL v2");
