// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 *
 * MID7021 first-pass sensor list:
 * - keep only the currently buildable GC02M1 path for arm64 bring-up
 */

#include "kd_imgsensor.h"
#include "imgsensor_sensor_list.h"

struct IMGSENSOR_INIT_FUNC_LIST kdSensorList[MAX_NUM_OF_SUPPORT_SENSOR] = {
#if defined(GC02M1_TSP_P410AE_MIPI_RAW)
	{GC02M1_TSP_P410AE_SENSOR_ID,
	SENSOR_DRVNAME_GC02M1_TSP_P410AE_MIPI_RAW,
	GC02M1_TSP_P410AE_MIPI_RAW_SensorInit},
#endif
#if defined(MID7021_STAGE_C2599_HOOKS)
	{C2599_MAIN_CXT_SENSOR_ID,
	SENSOR_DRVNAME_C2599_MAIN_CXT_MIPI_RAW,
	C2599_MAIN_CXT_MIPI_RAW_SensorInit},
	{C2599_SUB_CXT_SENSOR_ID,
	SENSOR_DRVNAME_C2599_SUB_CXT_MIPI_RAW,
	C2599_SUB_CXT_MIPI_RAW_SensorInit},
#endif
	{0, {0}, NULL}, /* end of list */
};
