/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 *
 * MID7021 first-pass header:
 * - keeps only declarations needed for the current GC02M1 bring-up path
 */

#ifndef __KD_SENSORLIST_H__
#define __KD_SENSORLIST_H__

#include "kd_camera_typedef.h"
#include "imgsensor_sensor.h"

struct IMGSENSOR_INIT_FUNC_LIST {
	MUINT32 id;
	MUINT8 name[32];
	MUINT32 (*init)(struct SENSOR_FUNCTION_STRUCT **pfFunc);
};

UINT32 GC02M1_TSP_P410AE_MIPI_RAW_SensorInit(
	struct SENSOR_FUNCTION_STRUCT **pfFunc);

/*
 * Staged C2599 hooks:
 * - keep disabled until real c2599 driver sources are imported.
 */
#if defined(MID7021_STAGE_C2599_HOOKS)
#define C2599_MAIN_CXT_SENSOR_ID 0x2599
#define C2599_SUB_CXT_SENSOR_ID 0x25A0
#define SENSOR_DRVNAME_C2599_MAIN_CXT_MIPI_RAW "c2599_main_cxt_mipi_raw"
#define SENSOR_DRVNAME_C2599_SUB_CXT_MIPI_RAW "c2599_sub_cxt_mipi_raw"
UINT32 C2599_MAIN_CXT_MIPI_RAW_SensorInit(
	struct SENSOR_FUNCTION_STRUCT **pfFunc);
UINT32 C2599_SUB_CXT_MIPI_RAW_SensorInit(
	struct SENSOR_FUNCTION_STRUCT **pfFunc);
#endif

extern struct IMGSENSOR_INIT_FUNC_LIST kdSensorList[];

#endif
