# MID7021 Camera Project Scaffold (First Pass)

This directory is a first-pass scaffold for `CONFIG_ARCH_MTK_PROJECT="mid7021_mq_32"`.

Current scope:
- `v1/imgsensor_sensor_list.c` only registers `gc02m1_tsp_p410ae_mipi_raw`
- `v1/imgsensor_sensor_list.h` only declares the GC02M1 init symbol
- `camera_hw/` is copied from `p410ae` to provide required MT6765 camera HW glue

Why this is minimal:
- MID7021 defconfig sensor IDs (`gc02m1_*_cxt/kyt`, `c2599_*`) do not currently have matching source directories in this kernel tree.
- This scaffold gives a buildable baseline for arm64 bring-up while missing sensor sources are imported later.

Next planned imports:
- `c2599_main_cxt_mipi_raw`
- `c2599_sub_cxt_mipi_raw`
- then optional `gc02m2_*`, `ov02b10_*`, `bf2253l_*`
