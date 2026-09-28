// SPDX-License-Identifier: GPL-2.0-only
/*
 * universal_aliensense_mode_tbls.h - mode tables for universal_aliensense
 *
 * Copyright (C) 2025 RidgeRun, LLC
 *
 */

#ifndef __UNIVERSAL_ALIENSENSE_MODE_TBLS_H__
#define __UNIVERSAL_ALIENSENSE_MODE_TBLS_H__

static const int universal_aliensense_30fps[] = {
    30,
};

static const int universal_aliensense_60fps[] = {
    60,
};

static const int universal_aliensense_120fps[] = {
    120,
};

static const int universal_aliensense_125fps[] = {
    125,
};

static const int universal_aliensense_172fps[] = {
    172,
};

static const int universal_aliensense_230fps[] = {
    230,
};

static const int universal_aliensense_397fps[] = {
    397,
};

static const int universal_aliensense_656fps[] = {
    656,
};

/* New modes will need to be added here, we've added a comprenhensive list, this is neccesary for v4l2-ctl but not for nvarguscamerasrc */
static const struct camera_common_frmfmt universal_aliensense_frmfmt[] = {
    { {2616, 1964}, universal_aliensense_60fps, 1, 0, 0 },
    { {1920, 1080}, universal_aliensense_120fps, 1, 0, 1 },
    { {1320, 984}, universal_aliensense_60fps, 1, 0, 2 },
    { {2064, 1552}, universal_aliensense_125fps, 1, 0, 3 },
    { {1920, 1080}, universal_aliensense_172fps, 1, 0, 4 },
    { {1032, 776}, universal_aliensense_230fps, 1, 0, 5 },
    { {2064, 154}, universal_aliensense_656fps, 1, 0, 6 },
    { {1024, 720}, universal_aliensense_397fps, 1, 0, 7 },
};

#endif /* __UNIVERSAL_ALIENSENSE_MODE_TBLS_H__ */
