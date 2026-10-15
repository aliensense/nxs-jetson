// SPDX-License-Identifier: GPL-2.0-only
/*
 * nxs_csi2_tegra.c - the NXS camera map as a tegracam sensor
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

#include <linux/delay.h>
#include <linux/i2c.h>
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

#include "universal_aliensense_mode_tbls.h"

/*
 * The driver knows no sensor. A pod's NXS unit, or a smart camera, serves
 * the camera map: a flat map behind a 16-bit register pointer, high byte
 * first. It holds a bootstrap block, the value and the range in effect of
 * each parameter row, the staged cells, the mode table and the rows. The
 * driver maps the tegracam controls to the rows by their standard id and
 * commits a frame's values in one write. The map is the chapter "Camera
 * map" of the NXS Interface Description; the driver's sequence is
 * docs/bring-up/nxs-csi2-mainline.md in the redbrain tree. The host tool
 * owns the graph: it brings the port up, starts and stops the stream and
 * converts the trigger, so this driver commits no stream gate.
 */

/* The bootstrap block. */
#define NXS_MAP_ID              0x0000
#define NXS_MAP_STATE           0x0002
#define NXS_MAP_ERROR           0x0003
#define NXS_MAP_PARAM_COUNT     0x0006
#define NXS_MAP_MODE_COUNT      0x0007
#define NXS_MAP_CURRENT_MODE    0x0008
#define NXS_MAP_STREAM          0x0009
#define NXS_MAP_MODE_RUN_MS     0x0010
#define NXS_MAP_GATE_RUN_MS     0x0012
#define NXS_MAP_TRIGGER_RUN_MS  0x0014
#define NXS_MAP_BOOTSTRAP_LEN   0x16
/* The value in effect of row i at LIVE + 4 i, little-endian. */
#define NXS_MAP_LIVE            0x0100
/* The staged cells run downward and end at MASK: row i's at MASK - 4 (i + 1). */
#define NXS_MAP_MASK            0x0600
#define NXS_MAP_MODE_TABLE      0x0700
#define NXS_MAP_ROWS            0x0800

#define NXS_MAP_ID_VALUE        0xC4
#define NXS_MAP_VERSION_VALUE   1
#define NXS_MAP_MAX_ROWS        64
#define NXS_MAP_MAX_MODES       16
#define NXS_MAP_ROW_SIZE        64
#define NXS_MAP_MODE_SIZE       16
/* The longest commit: every staged cell, MASK and COMMIT. */
#define NXS_MAP_COMMIT_MAX      (4 * NXS_MAP_MAX_ROWS + 9)

/* A row's fields. */
#define NXS_ROW_ID              22
#define NXS_ROW_TYPE            24
#define NXS_ROW_KIND            25
#define NXS_ROW_FLAGS           26
#define NXS_ROW_COUNT           27
#define NXS_ROW_VALUES          32
#define NXS_ROW_TYPE_RANGE      BIT(0)
#define NXS_ROW_TYPE_WIDE       BIT(2)
#define NXS_ROW_FLAG_SET_BY_TRIGGER BIT(1)

/* A mode-table row's fields. */
#define NXS_MODE_WIDTH          0
#define NXS_MODE_HEIGHT         2
#define NXS_MODE_BIT_DEPTH      4
#define NXS_MODE_VALUE          14

/* The standard parameter ids the driver serves (the ParamId registry). */
enum nxs_param_id {
    NXS_ID_EXPOSURE_US = 1,
    NXS_ID_FRAME_PERIOD_US = 2,
    NXS_ID_GAIN_MDB = 3,
    NXS_ID_BLACK_LEVEL_DN = 4,
    NXS_ID_TEST_PATTERN = 6,
    NXS_ID_TRIGGER = 7,
    NXS_ID_STREAM = 8,
    NXS_ID_MODE = 9,
    NXS_ID_COUNT = 14,
};

/* How a commit of a row runs. */
enum nxs_kind {
    NXS_KIND_APPLY = 2,
    NXS_KIND_RUN = 3,
    NXS_KIND_GATE = 4,
    NXS_KIND_TRIGGER = 5,
};

enum nxs_state {
    NXS_STATE_INERT = 0,
    NXS_STATE_READY = 1,
    NXS_STATE_BUSY = 2,
    NXS_STATE_FAULT = 3,
};

/* The trigger row's values (the TriggerCode registry). */
enum nxs_trigger_code {
    NXS_TRIGGER_FREERUN = 0,
    NXS_TRIGGER_FAST = 1,
    NXS_TRIGGER_SEQUENTIAL = 2,
};

/* A NAK is the device inside one of its own transfers: repeat. */
#define NXS_BUSY_RETRY_US       1000
#define NXS_BUSY_RETRIES        8
/* The apply window: wait, then poll STATE. */
#define NXS_APPLY_WAIT_US       1500
#define NXS_APPLY_POLL_US       500
#define NXS_APPLY_POLL_MAX_MS   20
/* A run's window: the painted run time first, then poll STATE. */
#define NXS_RUN_POLL_MS         10
#define NXS_RUN_TIMEOUT_MS      6000

#define NXS_CID_TEST_PATTERN   (TEGRA_CAMERA_CID_BASE + 121)
#define NXS_CID_OPERATION_MODE (TEGRA_CAMERA_CID_BASE + 122)
#define NXS_CID_BLACK_LEVEL    (TEGRA_CAMERA_CID_BASE + 125)
#define NXS_CID_SHUTTER_MODE   (TEGRA_CAMERA_CID_BASE + 126)

/* The shutter-mode menu's entries, in the menu's order. */
enum nxs_shutter_mode {
    NXS_SHUTTER_NORMAL = 0,
    NXS_SHUTTER_SEQUENTIAL = 1,
    NXS_SHUTTER_FAST = 2,
};

static const struct of_device_id nxs_csi2_of_match[] = {
    { .compatible = "aliensense,nxs-csi2", },
    { },
};

MODULE_DEVICE_TABLE(of, nxs_csi2_of_match);

/* Group hold is not listed: the framework registers it and calls
 * .set_group_hold, which brackets one commit per frame. */
static const u32 ctrl_cid_list[] = {
    TEGRA_CAMERA_CID_GAIN,
    TEGRA_CAMERA_CID_EXPOSURE,
    TEGRA_CAMERA_CID_FRAME_RATE,
    TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

struct nxs_csi2 {
    struct i2c_client *i2c_client;
    struct v4l2_subdev *subdev;
    struct camera_common_data *s_data;
    struct tegracam_device *tc_dev;
    struct regmap_config regmap_config;
    /* The rows, read at each session: a standard id's row index, -1 for none. */
    s8 row_of[NXS_ID_COUNT];
    u8 kind_of[NXS_ID_COUNT];
    u8 param_count;
    u8 mode_count;
    bool exposure_set_by_trigger;
    u32 trigger_codes;
    /* The cells in effect at the session's start. */
    bool configured;
    u8 current_mode;
    u8 stream;
    u32 mode_period_us;
    u32 trigger_live;
    /* The commit a group hold gathers: one kind, row i's value in staged[i]. */
    u32 staged[NXS_MAP_MAX_ROWS];
    u64 mask;
    u8 kind;
    bool held;
    u32 shutter_mode;
    u32 operation_mode;
    bool probing;
    bool announced;
};

static const struct regmap_config nxs_map_regmap_config = {
    .reg_bits = 16,
    .val_bits = 8,
    .cache_type = REGCACHE_NONE,
};

/* ---- the map, with the busy retry ------------------------------------ */

static bool nxs_map_busy(int err)
{
    return err == -ENXIO || err == -EREMOTEIO || err == -EIO || err == -EAGAIN;
}

/*
 * One transfer: the 2-byte register pointer and the bytes written, or the
 * pointer and a read under a repeated start. `locked` is a transfer under
 * the caller's bus lock (a commit's window); otherwise the adapter locks.
 */
static int nxs_map_xfer(struct i2c_client *client, bool locked, u16 reg, const u8 *wbuf,
                        size_t wlen, u8 *rbuf, size_t rlen)
{
    u8 out[2 + NXS_MAP_COMMIT_MAX];
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 2 + wlen, .buf = out },
        { .addr = client->addr, .flags = I2C_M_RD, .len = rlen, .buf = rbuf },
    };
    int n = rlen ? 2 : 1;
    int err = 0;
    int i;

    if (wlen > NXS_MAP_COMMIT_MAX)
        return -EINVAL;
    out[0] = reg >> 8;
    out[1] = reg & 0xFF;
    if (wlen)
        memcpy(out + 2, wbuf, wlen);
    for (i = 0; i < NXS_BUSY_RETRIES; i++) {
        err = locked ? __i2c_transfer(client->adapter, msgs, n)
                     : i2c_transfer(client->adapter, msgs, n);
        if (err == n)
            return 0;
        if (err >= 0)
            err = -EIO;
        if (!nxs_map_busy(err))
            return err;
        usleep_range(NXS_BUSY_RETRY_US, NXS_BUSY_RETRY_US + 500);
    }

    return err;
}

static u32 nxs_le32(const u8 *p)
{
    return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 nxs_le16(const u8 *p)
{
    return p[0] | ((u16)p[1] << 8);
}

static void nxs_put_le32(u8 *p, u32 v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

/*
 * A bus failure during probe (the framework writes every control's default
 * while the pod may not be up) is logged and ignored so the node registers;
 * afterwards it is the caller's.
 */
static int nxs_bus_result(struct nxs_csi2 *priv, int err, const char *what)
{
    struct device *dev = priv->tc_dev->dev;

    if (!err)
        return 0;
    if (priv->probing) {
        dev_dbg(dev, "%s: camera map not reachable during probe (%d), ignored\n", what, err);
        return 0;
    }
    dev_warn_ratelimited(dev, "%s: bus error %d\n", what, err);

    return err;
}

/*
 * STATE inside a window: a value outside the four, or a terminal state the
 * next read does not repeat, is a read the device's own transaction tore,
 * so it counts as busy.
 */
static int nxs_map_state(struct i2c_client *client, u8 *state)
{
    u8 first = 0;
    u8 second = 0;
    int err;

    err = nxs_map_xfer(client, true, NXS_MAP_STATE, NULL, 0, &first, 1);
    if (err)
        return err;
    if (first > NXS_STATE_FAULT || first == NXS_STATE_BUSY) {
        *state = NXS_STATE_BUSY;
        return 0;
    }
    err = nxs_map_xfer(client, true, NXS_MAP_STATE, NULL, 0, &second, 1);
    if (err)
        return err;
    *state = first == second ? first : NXS_STATE_BUSY;

    return 0;
}

static const char *nxs_state_name(u8 state)
{
    switch (state) {
    case NXS_STATE_INERT:
        return "INERT";
    case NXS_STATE_READY:
        return "READY";
    case NXS_STATE_BUSY:
        return "BUSY";
    case NXS_STATE_FAULT:
        return "FAULT";
    default:
        return "unknown";
    }
}

/*
 * Before a COMMIT, which the map ignores while BUSY or INERT: STATE must
 * read READY or FAULT. A run of the host tool's in flight is waited out
 * with the bus released, so the tool's own reads go on. Returns 0 with the
 * bus held from that read, 1 with it released when the map is INERT.
 */
static int nxs_map_idle(struct i2c_client *client, bool run)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(run ? NXS_RUN_TIMEOUT_MS
                                                            : NXS_APPLY_POLL_MAX_MS);
    u8 state = NXS_STATE_BUSY;
    int err;

    for (;;) {
        i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);
        err = nxs_map_state(client, &state);
        if (!err && state != NXS_STATE_BUSY) {
            if (state != NXS_STATE_INERT)
                return 0;
            i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
            return 1;
        }
        i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
        if (err && !nxs_map_busy(err))
            return err;
        if (!time_before(jiffies, deadline))
            return err ? err : -EBUSY;
        if (run)
            msleep(NXS_RUN_POLL_MS);
        else
            usleep_range(NXS_APPLY_POLL_US, NXS_APPLY_POLL_US + 100);
    }
}

/*
 * The device's errno values that differ from the kernel's, from the
 * Interface Description's §12 table. Its values below 35 agree.
 */
enum nxs_dev_errno {
    NXS_DEV_ETIMEDOUT = 116,
    NXS_DEV_ESTALE = 133,
    NXS_DEV_ENOTSUP = 134,
    NXS_DEV_EILSEQ = 138,
    NXS_DEV_ECANCELED = 140,
};

/* A FAULT's ERROR cell in the kernel's numbering; a value the table lacks reads EIO. */
static int nxs_map_errno(u8 error)
{
    switch (error) {
    case NXS_DEV_ETIMEDOUT:
        return -ETIMEDOUT;
    case NXS_DEV_ESTALE:
        return -ESTALE;
    case NXS_DEV_ENOTSUP:
        return -EOPNOTSUPP;
    case NXS_DEV_EILSEQ:
        return -EILSEQ;
    case NXS_DEV_ECANCELED:
        return -ECANCELED;
    case ENODATA:
    case EPROTO:
        return -(int)error;
    default:
        return error && error < 35 ? -(int)error : -EIO;
    }
}

/*
 * One commit: the staged cells from the highest row staged down to row 0,
 * MASK and COMMIT in one write, so a transfer the bus cuts never applies.
 * The host bus stays held from the READY read before COMMIT until STATE
 * leaves BUSY: an apply polls after the apply wait, and a run first waits
 * the time the map painted for its kind, reading a NAK as busy. An INERT
 * map takes nothing: the tool's bring-up has not run its sensor yet.
 */
static int nxs_commit(struct nxs_csi2 *priv, const char *what)
{
    struct i2c_client *client = priv->i2c_client;
    struct device *dev = priv->tc_dev->dev;
    u8 buf[NXS_MAP_COMMIT_MAX];
    u8 state = NXS_STATE_BUSY;
    u8 error = 0;
    u8 ms[2] = { 0, 0 };
    bool run = priv->kind != NXS_KIND_APPLY;
    unsigned long deadline;
    unsigned int hi;
    unsigned int len;
    unsigned int i;
    int err;

    if (!priv->mask)
        return 0;
    hi = fls64(priv->mask) - 1;
    for (i = 0; i <= hi; i++)
        nxs_put_le32(buf + 4 * (hi - i), (priv->mask & BIT_ULL(i)) ? priv->staged[i] : 0);
    len = 4 * (hi + 1);
    for (i = 0; i < 8; i++)
        buf[len + i] = (priv->mask >> (8 * i)) & 0xFF;
    buf[len + 8] = 1;
    len += 9;
    if (run) {
        u16 cell = priv->kind == NXS_KIND_RUN ? NXS_MAP_MODE_RUN_MS
                 : priv->kind == NXS_KIND_GATE ? NXS_MAP_GATE_RUN_MS : NXS_MAP_TRIGGER_RUN_MS;

        if (nxs_map_xfer(client, false, cell, NULL, 0, ms, sizeof(ms)))
            ms[0] = ms[1] = 0;
    }
    err = nxs_map_idle(client, run);
    if (err) {
        priv->mask = 0;
        if (err == 1) {
            dev_dbg(dev, "%s: the camera map is INERT, nothing committed\n", what);
            return 0;
        }
        if (err == -EBUSY) {
            dev_warn_ratelimited(dev, "%s: the camera map stayed busy\n", what);
            return err;
        }
        return nxs_bus_result(priv, err, what);
    }
    err = nxs_map_xfer(client, true, NXS_MAP_MASK - 4 * (hi + 1), buf, len, NULL, 0);
    if (!err && run) {
        if (nxs_le16(ms))
            msleep(nxs_le16(ms));
        deadline = jiffies + msecs_to_jiffies(NXS_RUN_TIMEOUT_MS);
        for (;;) {
            err = nxs_map_state(client, &state);
            if (err && nxs_map_busy(err) && time_before(jiffies, deadline)) {
                err = 0;
                state = NXS_STATE_BUSY;
            }
            if (err || state != NXS_STATE_BUSY || !time_before(jiffies, deadline))
                break;
            msleep(NXS_RUN_POLL_MS);
        }
    } else if (!err) {
        usleep_range(NXS_APPLY_WAIT_US, NXS_APPLY_WAIT_US + 200);
        for (i = 0; i < NXS_APPLY_POLL_MAX_MS * 1000 / NXS_APPLY_POLL_US; i++) {
            err = nxs_map_state(client, &state);
            if (err || state != NXS_STATE_BUSY)
                break;
            usleep_range(NXS_APPLY_POLL_US, NXS_APPLY_POLL_US + 100);
        }
    }
    if (!err && state == NXS_STATE_FAULT)
        err = nxs_map_xfer(client, true, NXS_MAP_ERROR, NULL, 0, &error, 1);
    i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
    priv->mask = 0;
    if (err)
        return nxs_bus_result(priv, err, what);
    if (state == NXS_STATE_FAULT) {
        dev_warn_ratelimited(dev, "%s: the camera map refused the commit (error %u)\n", what, error);
        return nxs_map_errno(error);
    }
    if (state == NXS_STATE_BUSY) {
        dev_warn_ratelimited(dev, "%s: the camera map stayed busy\n", what);
        return -EBUSY;
    }

    return 0;
}

/*
 * A row staged under a hold waits for the hold's end; outside one, or for
 * a kind a hold does not gather, it commits. A commit carries one kind, so
 * a row of another kind commits what is staged first. A row the map does
 * not serve stages nothing.
 */
static int nxs_stage(struct nxs_csi2 *priv, unsigned int id, u32 value, const char *what)
{
    int row = priv->row_of[id];
    u8 kind = priv->kind_of[id];
    int err;

    if (row < 0)
        return 0;
    if (priv->mask && priv->kind != kind) {
        err = nxs_commit(priv, what);
        if (err)
            return err;
    }
    priv->staged[row] = value;
    priv->mask |= BIT_ULL(row);
    priv->kind = kind;
    if (priv->held && kind == NXS_KIND_APPLY)
        return 0;

    return nxs_commit(priv, what);
}

/*
 * The bootstrap block, the rows and the cells in effect, read at probe and
 * at every session: what the controls convert against. A map that has not
 * configured its sensor yet (INERT) leaves the controls inert.
 */
static int nxs_read_map(struct nxs_csi2 *priv)
{
    struct i2c_client *client = priv->i2c_client;
    struct device *dev = priv->tc_dev->dev;
    u8 boot[NXS_MAP_BOOTSTRAP_LEN];
    u8 row[NXS_MAP_ROW_SIZE];
    u8 live[4 * NXS_MAP_MAX_ROWS];
    unsigned int i;
    unsigned int v;
    int err;

    err = nxs_map_xfer(client, false, NXS_MAP_ID, NULL, 0, boot, sizeof(boot));
    if (err)
        return err;
    if (boot[0] != NXS_MAP_ID_VALUE || boot[1] != NXS_MAP_VERSION_VALUE) {
        dev_err(dev, "no camera map at 0x%02x: id 0x%02x version %u\n", client->addr,
                boot[0], boot[1]);
        return -ENODEV;
    }
    priv->param_count = min_t(u8, boot[NXS_MAP_PARAM_COUNT], NXS_MAP_MAX_ROWS);
    priv->mode_count = min_t(u8, boot[NXS_MAP_MODE_COUNT], NXS_MAP_MAX_MODES);
    priv->current_mode = boot[NXS_MAP_CURRENT_MODE];
    priv->stream = boot[NXS_MAP_STREAM];
    priv->configured = boot[NXS_MAP_STATE] != NXS_STATE_INERT;
    memset(priv->row_of, -1, sizeof(priv->row_of));
    priv->exposure_set_by_trigger = false;
    priv->trigger_codes = 0;
    for (i = 0; i < priv->param_count; i++) {
        unsigned int id;

        err = nxs_map_xfer(client, false, NXS_MAP_ROWS + i * NXS_MAP_ROW_SIZE, NULL, 0,
                           row, sizeof(row));
        if (err)
            return err;
        id = nxs_le16(row + NXS_ROW_ID);
        if (!id || id >= NXS_ID_COUNT || priv->row_of[id] >= 0)
            continue;
        priv->row_of[id] = i;
        priv->kind_of[id] = row[NXS_ROW_KIND];
        if (id == NXS_ID_EXPOSURE_US)
            priv->exposure_set_by_trigger = row[NXS_ROW_FLAGS] & NXS_ROW_FLAG_SET_BY_TRIGGER;
        if (id == NXS_ID_TRIGGER && !(row[NXS_ROW_TYPE] & NXS_ROW_TYPE_RANGE)) {
            for (v = 0; v < row[NXS_ROW_COUNT]; v++) {
                u32 code = (row[NXS_ROW_TYPE] & NXS_ROW_TYPE_WIDE)
                           ? nxs_le32(row + NXS_ROW_VALUES + 4 * v)
                           : nxs_le16(row + NXS_ROW_VALUES + 2 * v);

                if (code < 32)
                    priv->trigger_codes |= BIT(code);
            }
        }
    }
    priv->mode_period_us = 0;
    priv->trigger_live = NXS_TRIGGER_FREERUN;
    if (priv->param_count) {
        err = nxs_map_xfer(client, false, NXS_MAP_LIVE, NULL, 0, live, 4 * priv->param_count);
        if (err)
            return err;
        if (priv->row_of[NXS_ID_FRAME_PERIOD_US] >= 0)
            priv->mode_period_us = nxs_le32(live + 4 * priv->row_of[NXS_ID_FRAME_PERIOD_US]);
        if (priv->row_of[NXS_ID_TRIGGER] >= 0)
            priv->trigger_live = nxs_le32(live + 4 * priv->row_of[NXS_ID_TRIGGER]);
    }
    if (!priv->announced) {
        dev_info(dev, "camera map v%u: %u rows, %u modes, %s\n", boot[1],
                 priv->param_count, priv->mode_count, nxs_state_name(boot[NXS_MAP_STATE]));
        priv->announced = true;
    }

    return 0;
}

/* The selected mode's properties: the factors the control values carry. */
static const struct sensor_mode_properties *nxs_mode_props(struct nxs_csi2 *priv)
{
    struct camera_common_data *s_data = priv->s_data;
    int idx = s_data->mode_prop_idx;

    if (idx < 0 || idx >= s_data->sensor_props.num_modes)
        return NULL;

    return &s_data->sensor_props.sensor_modes[idx];
}

/* The bits a pixel carries in a raw Bayer format, 0 for another format. */
static unsigned int nxs_bit_depth(u32 pixel_format)
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

/*
 * The map's mode-table value of the session's mode: the row whose width,
 * height and bit depth the device-tree mode carries. -ENOENT for none.
 */
static int nxs_mode_value(struct nxs_csi2 *priv, u8 *value)
{
    const struct sensor_mode_properties *mode = nxs_mode_props(priv);
    u8 table[NXS_MAP_MAX_MODES * NXS_MAP_MODE_SIZE];
    unsigned int depth;
    unsigned int i;
    int err;

    if (!mode || !priv->mode_count)
        return -ENOENT;
    depth = nxs_bit_depth(mode->image_properties.pixel_format);
    err = nxs_map_xfer(priv->i2c_client, false, NXS_MAP_MODE_TABLE, NULL, 0, table,
                       priv->mode_count * NXS_MAP_MODE_SIZE);
    if (err)
        return err;
    for (i = 0; i < priv->mode_count; i++) {
        const u8 *row = table + i * NXS_MAP_MODE_SIZE;

        if (nxs_le16(row + NXS_MODE_WIDTH) == mode->image_properties.width &&
            nxs_le16(row + NXS_MODE_HEIGHT) == mode->image_properties.height &&
            (!depth || row[NXS_MODE_BIT_DEPTH] == depth)) {
            *value = row[NXS_MODE_VALUE];
            return 0;
        }
    }

    return -ENOENT;
}

/* ---- tegracam control callbacks ------------------------------------ */

static int nxs_csi2_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
    struct nxs_csi2 *priv = tegracam_get_privdata(tc_dev);

    if (!priv)
        return 0;
    priv->held = val;
    if (val)
        return 0;

    return nxs_commit(priv, "group hold");
}

/* The gain in the mode's factor (tenths of a decibel at gain_factor 10), as millidecibels. */
static int nxs_csi2_set_gain(struct tegracam_device *tc_dev, s64 val)
{
    struct nxs_csi2 *priv = tegracam_get_privdata(tc_dev);
    const struct sensor_mode_properties *mode;
    u32 factor;

    if (!priv || !priv->configured)
        return 0;
    mode = nxs_mode_props(priv);
    if (!mode || val < 0)
        return -EINVAL;
    factor = mode->control_properties.gain_factor ? mode->control_properties.gain_factor : 1;

    return nxs_stage(priv, NXS_ID_GAIN_MDB, (u32)div64_u64((u64)val * 1000, factor), "gain");
}

/*
 * The rate as the frame period row's microseconds. Under a trigger the
 * pulse period is the frame period, so the rate is left alone. A request
 * that rounds to the session's own rate keeps the frame it runs, since
 * the capture stack asks in whole frames per second.
 */
static int nxs_csi2_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
    struct nxs_csi2 *priv = tegracam_get_privdata(tc_dev);
    const struct sensor_mode_properties *mode;
    u64 factor;
    u64 own;
    u32 period;

    if (!priv || !priv->configured)
        return 0;
    if (priv->trigger_live != NXS_TRIGGER_FREERUN) {
        dev_dbg(tc_dev->dev, "frame rate left to the trigger pulse\n");
        return 0;
    }
    mode = nxs_mode_props(priv);
    if (!mode || val <= 0)
        return -EINVAL;
    factor = mode->control_properties.framerate_factor ? mode->control_properties.framerate_factor : 1;
    period = (u32)div64_u64(1000000ULL * factor, (u64)val);
    if (priv->mode_period_us) {
        own = div64_u64(1000000ULL * factor, priv->mode_period_us);
        if ((own > (u64)val ? own - (u64)val : (u64)val - own) < factor)
            period = priv->mode_period_us;
    }

    return nxs_stage(priv, NXS_ID_FRAME_PERIOD_US, period, "frame period");
}

/*
 * The exposure in microseconds. A row the trigger pulse sets takes no
 * value under the fast trigger, whose pulse width is the exposure; a
 * sequential trigger exposes at the row's value. The device clamps a value
 * past the frame.
 */
static int nxs_csi2_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
    struct nxs_csi2 *priv = tegracam_get_privdata(tc_dev);
    const struct sensor_mode_properties *mode;
    u64 factor;

    if (!priv || !priv->configured)
        return 0;
    if (priv->exposure_set_by_trigger && priv->trigger_live == NXS_TRIGGER_FAST) {
        dev_dbg(tc_dev->dev, "exposure left to the trigger pulse\n");
        return 0;
    }
    mode = nxs_mode_props(priv);
    if (!mode || val < 0)
        return -EINVAL;
    factor = mode->control_properties.exposure_factor ? mode->control_properties.exposure_factor : 1;

    return nxs_stage(priv, NXS_ID_EXPOSURE_US, (u32)div64_u64((u64)val * 1000000ULL, factor),
                     "exposure");
}

static struct tegracam_ctrl_ops nxs_csi2_ctrl_ops = {
    .numctrls = ARRAY_SIZE(ctrl_cid_list),
    .ctrl_cid_list = ctrl_cid_list,
    .set_gain = nxs_csi2_set_gain,
    .set_exposure = nxs_csi2_set_exposure,
    .set_frame_rate = nxs_csi2_set_frame_rate,
    .set_group_hold = nxs_csi2_set_group_hold,
};

/* ---- private controls ---------------------------------------------- */

/* The trigger code a shutter-mode menu entry selects. */
static u32 nxs_trigger_code(u32 shutter_mode)
{
    switch (shutter_mode) {
    case NXS_SHUTTER_SEQUENTIAL:
        return NXS_TRIGGER_SEQUENTIAL;
    case NXS_SHUTTER_FAST:
        return NXS_TRIGGER_FAST;
    default:
        return NXS_TRIGGER_FREERUN;
    }
}

static int nxs_priv_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct nxs_csi2 *priv = ctrl->priv;
    struct device *dev = priv->tc_dev->dev;
    u32 code;
    int err;

    switch (ctrl->id) {
    case NXS_CID_OPERATION_MODE:
        /* The personality's timing program sets the sync role. */
        priv->operation_mode = ctrl->val;
        return 0;
    case NXS_CID_SHUTTER_MODE:
        priv->shutter_mode = ctrl->val;
        if (!priv->configured)
            return 0;
        code = nxs_trigger_code(ctrl->val);
        if (priv->row_of[NXS_ID_TRIGGER] < 0 || !(priv->trigger_codes & BIT(code))) {
            dev_warn(dev, "shutter mode %u: the camera map serves no trigger code %u\n",
                     ctrl->val, code);
            return -EINVAL;
        }
        err = nxs_stage(priv, NXS_ID_TRIGGER, code, "shutter mode");
        if (!err)
            priv->trigger_live = code;
        return err;
    case NXS_CID_TEST_PATTERN:
        if (!priv->configured)
            return 0;
        if (priv->row_of[NXS_ID_TEST_PATTERN] < 0)
            return -EINVAL;
        return nxs_stage(priv, NXS_ID_TEST_PATTERN, ctrl->val, "test pattern");
    case NXS_CID_BLACK_LEVEL:
        if (!priv->configured)
            return 0;
        if (priv->row_of[NXS_ID_BLACK_LEVEL_DN] < 0)
            return -EINVAL;
        return nxs_stage(priv, NXS_ID_BLACK_LEVEL_DN, ctrl->val, "black level");
    default:
        return -EINVAL;
    }
}

static const struct v4l2_ctrl_ops nxs_priv_ctrl_ops = {
    .s_ctrl = nxs_priv_s_ctrl,
};

static const char * const nxs_operation_mode_menu[] = {
    "Master Mode",
    "Slave Mode",
};

static const char * const nxs_shutter_mode_menu[] = {
    "Normal",
    "Sequential Trigger",
    "Fast Trigger",
};

static const struct v4l2_ctrl_config nxs_priv_ctrl_cfg[] = {
    {
        .ops = &nxs_priv_ctrl_ops,
        .id = NXS_CID_OPERATION_MODE,
        .flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
        .name = "operation_mode",
        .type = V4L2_CTRL_TYPE_MENU,
        .min = 0,
        .max = ARRAY_SIZE(nxs_operation_mode_menu) - 1,
        .def = 0,
        .qmenu = nxs_operation_mode_menu,
    },
    {
        .ops = &nxs_priv_ctrl_ops,
        .id = NXS_CID_SHUTTER_MODE,
        .flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
        .name = "shutter_mode",
        .type = V4L2_CTRL_TYPE_MENU,
        .min = 0,
        .max = ARRAY_SIZE(nxs_shutter_mode_menu) - 1,
        .def = 0,
        .qmenu = nxs_shutter_mode_menu,
    },
    {
        .ops = &nxs_priv_ctrl_ops,
        .id = NXS_CID_TEST_PATTERN,
        .flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
        .name = "test_pattern",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .min = 0,
        .max = 255,
        .step = 1,
        .def = 0,
    },
    {
        .ops = &nxs_priv_ctrl_ops,
        .id = NXS_CID_BLACK_LEVEL,
        .name = "black_level",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .min = 0,
        .max = 0xFFFF,
        .step = 1,
        .def = 0,
    },
};

static int nxs_add_priv_ctrls(struct nxs_csi2 *priv)
{
    struct v4l2_ctrl_handler *hdl = priv->s_data->ctrl_handler;
    int i;

    if (!hdl)
        return -ENODEV;
    for (i = 0; i < ARRAY_SIZE(nxs_priv_ctrl_cfg); i++) {
        if (!v4l2_ctrl_new_custom(hdl, &nxs_priv_ctrl_cfg[i], priv))
            return hdl->error ? hdl->error : -EINVAL;
    }

    return 0;
}

/* ---- power, dt, mode, streaming ------------------------------------ */

static int nxs_csi2_power_on(struct camera_common_data *s_data)
{
    if (s_data && s_data->power)
        s_data->power->state = SWITCH_ON;

    return 0;
}

static int nxs_csi2_power_off(struct camera_common_data *s_data)
{
    if (s_data && s_data->power)
        s_data->power->state = SWITCH_OFF;

    return 0;
}

static int nxs_csi2_power_put(struct tegracam_device *tc_dev)
{
    struct camera_common_data *s_data = tc_dev->s_data;
    struct camera_common_power_rail *pw = s_data->power;

    if (unlikely(!pw))
        return -EFAULT;

    return 0;
}

static int nxs_csi2_power_get(struct tegracam_device *tc_dev)
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

static struct camera_common_pdata *nxs_csi2_parse_dt(struct tegracam_device *tc_dev)
{
    struct device *dev = tc_dev->dev;
    struct camera_common_pdata *board_priv_pdata;
    int err;

    if (!dev->of_node)
        return NULL;
    if (!of_match_device(nxs_csi2_of_match, dev)) {
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
 * Each session reads the map. A device-tree mode whose row the map does
 * not run is a RUN commit of that row, in standby: a streaming sensor parks
 * first and starts again after the run, and the map is read again. A map
 * that does not answer, or has not configured its sensor, leaves the
 * controls inert and the session to the stream the host tool started.
 */
static int nxs_csi2_set_mode(struct tegracam_device *tc_dev)
{
    struct nxs_csi2 *priv = tegracam_get_privdata(tc_dev);
    bool streaming;
    u8 value;
    int err;

    if (!priv)
        return 0;
    err = nxs_read_map(priv);
    if (err == -ENODEV)
        return err;
    if (err || !priv->configured) {
        priv->configured = false;
        dev_dbg(tc_dev->dev, "camera map not configured (%d), controls inert\n", err);
        return 0;
    }
    if (priv->row_of[NXS_ID_MODE] < 0 || nxs_mode_value(priv, &value) || value == priv->current_mode)
        return 0;
    streaming = priv->stream && priv->row_of[NXS_ID_STREAM] >= 0;
    dev_info(tc_dev->dev, "mode row %u asked, the camera map runs %u: switching in standby\n",
             value, priv->current_mode);
    if (streaming) {
        err = nxs_stage(priv, NXS_ID_STREAM, 0, "stream off");
        if (err)
            return err;
    }
    err = nxs_stage(priv, NXS_ID_MODE, value, "mode");
    if (!err && streaming)
        err = nxs_stage(priv, NXS_ID_STREAM, 1, "stream on");
    if (err)
        return err;

    return nxs_read_map(priv);
}

/* The host tool owns the stream gate: `nxs <port> on` starts it, `off` parks it. */
static int nxs_csi2_start_streaming(struct tegracam_device *tc_dev)
{
    return 0;
}

static int nxs_csi2_stop_streaming(struct tegracam_device *tc_dev)
{
    return 0;
}

static int nxs_csi2_read_reg(struct camera_common_data *s_data, u16 addr, u8 *val)
{
    unsigned int v = 0;
    int err = regmap_read(s_data->regmap, addr, &v);

    *val = v & 0xFF;

    return err;
}

static int nxs_csi2_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
    return regmap_write(s_data->regmap, addr, val);
}

static struct camera_common_sensor_ops nxs_csi2_common_ops = {
    .numfrmfmts = ARRAY_SIZE(universal_aliensense_frmfmt),
    .frmfmt_table = universal_aliensense_frmfmt,
    .power_on = nxs_csi2_power_on,
    .power_off = nxs_csi2_power_off,
    .write_reg = nxs_csi2_write_reg,
    .read_reg = nxs_csi2_read_reg,
    .parse_dt = nxs_csi2_parse_dt,
    .power_get = nxs_csi2_power_get,
    .power_put = nxs_csi2_power_put,
    .set_mode = nxs_csi2_set_mode,
    .start_streaming = nxs_csi2_start_streaming,
    .stop_streaming = nxs_csi2_stop_streaming,
};

static int nxs_csi2_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);

    dev_dbg(&client->dev, "%s", __func__);

    return 0;
}

static const struct v4l2_subdev_internal_ops nxs_csi2_subdev_internal_ops = {
    .open = nxs_csi2_open,
};

static int nxs_csi2_power_on_probe(struct nxs_csi2 *priv)
{
    struct device *dev = &priv->i2c_client->dev;
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
static int nxs_csi2_probe(struct i2c_client *client)
#else
static int nxs_csi2_probe(struct i2c_client *client, const struct i2c_device_id *id)
#endif
{
    struct device *dev = &client->dev;
    struct tegracam_device *tc_dev;
    struct nxs_csi2 *priv;
    int err;

    if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
        return -EINVAL;
    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    tc_dev = devm_kzalloc(dev, sizeof(*tc_dev), GFP_KERNEL);
    if (!tc_dev)
        return -ENOMEM;

    priv->i2c_client = tc_dev->client = client;
    priv->probing = true;
    priv->regmap_config = nxs_map_regmap_config;
    tc_dev->dev = dev;
    strncpy(tc_dev->name, "nxs-csi2", sizeof(tc_dev->name));
    tc_dev->dev_regmap_config = &priv->regmap_config;
    tc_dev->sensor_ops = &nxs_csi2_common_ops;
    tc_dev->v4l2sd_internal_ops = &nxs_csi2_subdev_internal_ops;
    tc_dev->tcctrl_ops = &nxs_csi2_ctrl_ops;

    err = tegracam_device_register(tc_dev);
    if (err) {
        dev_err(dev, "tegra camera driver registration failed\n");
        return err;
    }
    priv->tc_dev = tc_dev;
    priv->s_data = tc_dev->s_data;
    priv->subdev = &tc_dev->s_data->subdev;
    tegracam_set_privdata(tc_dev, (void *)priv);

    /*
     * The pod answers only once the hub's link is up, which the host tool
     * does after boot: a map silent at probe is read at the first session,
     * and one that answers is checked now.
     */
    err = nxs_read_map(priv);
    if (err == -ENODEV) {
        tegracam_device_unregister(tc_dev);
        return err;
    }
    if (err)
        dev_info(dev, "camera map not reachable at probe, read at the first session\n");

    err = tegracam_v4l2subdev_register(tc_dev, true);
    if (err) {
        dev_err(dev, "tegra camera subdev registration failed\n");
        tegracam_device_unregister(tc_dev);
        return err;
    }
    err = nxs_add_priv_ctrls(priv);
    if (err) {
        dev_err(dev, "private controls failed: %d\n", err);
        tegracam_v4l2subdev_unregister(tc_dev);
        tegracam_device_unregister(tc_dev);
        return err;
    }
    err = nxs_csi2_power_on_probe(priv);
    if (err) {
        tegracam_v4l2subdev_unregister(tc_dev);
        tegracam_device_unregister(tc_dev);
        return err;
    }
    priv->probing = false;

    return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int nxs_csi2_remove(struct i2c_client *client)
#else
static void nxs_csi2_remove(struct i2c_client *client)
#endif
{
    struct camera_common_data *s_data = to_camera_common_data(&client->dev);
    struct nxs_csi2 *priv = (struct nxs_csi2 *)s_data->priv;

    tegracam_v4l2subdev_unregister(priv->tc_dev);
    tegracam_device_unregister(priv->tc_dev);

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
    return 0;
#endif
}

static struct i2c_driver nxs_csi2_i2c_driver = {
    .driver = {
           .name = "nxs-csi2",
           .owner = THIS_MODULE,
           .of_match_table = of_match_ptr(nxs_csi2_of_match),
            },
    .probe = nxs_csi2_probe,
    .remove = nxs_csi2_remove,
};

module_i2c_driver(nxs_csi2_i2c_driver);

MODULE_DESCRIPTION("V4L2 driver for the NXS camera map");
MODULE_AUTHOR("Aliensense");
MODULE_LICENSE("GPL v2");
