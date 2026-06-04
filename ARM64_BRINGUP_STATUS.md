# MID7021 (Onn 100135924) v7a→v8a arm64 Kernel Bring-up — Status

**Device:** Onn 100135924 / Lightcomm MID7021 (reskin of **100071481**). MediaTek **MT6765**
(Cortex-A53, PowerVR GE8320), kernel **4.19.191**, Android 14 **UP1A.231005.007**.
**Goal:** boot a 64-bit (arm64/v8a) kernel under the shipped 32-bit (v7a) AOSP+MTK userspace.
**Branch:** `mid7021-arm64-bringup`. **Build box:** poca@192.168.4.79.

## Status

| Stage | Status | Notes |
|---|---|---|
| arm64 kernel builds & loads | ✅ | config validated vs working UMIDIGI A14 ikconfig (watchdog/connsys/TEE byte-identical) |
| Silent resets | ✅ solved | LK-armed **TOPRGU hardware watchdog** killing a healthy-but-stalled boot; disarmed via `early_initcall` in `drivers/watchdog/mtk_wdt.c` (diagnostic only) |
| Boots to **zygote + SurfaceFlinger + HAL stack** | ✅ | v5 = WDT-neutered kernel + permissive-SELinux capture ramdisk; 447 procs; furthest ever |
| WiFi / BT / GPS (connectivity) | ⛔ | vendor `.ko` are **32-bit ARMv7** → `Exec format error` on aarch64. Non-fatal to boot |
| **GPU / reach `boot_completed`** | ⛔ | PowerVR bridge **err 311** (`PVRSRV_ERROR_BRIDGE_EINVAL`): 32-bit `/vendor` GL/gralloc call **MM bridge fn 42/44**, our DDK has only 0–36. SurfaceFlinger wedged |

## Root cause of the two remaining walls
Both are **closed 32-bit `/vendor` binaries built against a 32-bit-kernel ABI**, and the matching
kernel-side source is **MTK-proprietary** (the original 100071481 BSP):
- **Connectivity:** `wmt_drv.ko`, `wlan_drv_gen4m.ko`, `bt_drv_connac1x.ko`, `gps_drv.ko`, `connfem.ko`
  ship prebuilt; source absent from every public tree (only in-tree `build_in_adapter` shims).
- **GPU:** `/vendor` GL blobs (`libGLESv2_mtk.so`/`libsrv_um.so` = `1.13@5776728`) call MTK-custom
  PowerVR **MM bridge functions 42/44**. No public DDK source has them — our tree, the Rabbit R1
  tree, and all `m1.13`/`m1.15` variants cap MM at function 36 (`PVRSRV_BRIDGE_MM_CMD_LAST = +36`).

## What was ruled out (so we don't re-chase)
- **Config:** validated vs a working arm64 A14 MT6765 reference (UMIDIGI G9C) — watchdog, connsys,
  TEE all byte-identical. Not a config problem.
- **TEE:** device genuinely uses Trustonic (mobicore/keymaster-4-1-trustonic) — our build is correct.
- **`vendor.all.modules.ready` gate:** pre-set to 1 on non-GKI; not a hard block.
- **Rabbit R1 source** (`rabbit-hmi-oss/android_kernel_rabbit_mt6765`): same `k65v1_64_bsp` base,
  same DDK (≤36 MM fns), same connectivity shims — confirms our kernel side, supplies neither missing piece.

## Paths forward
1. **Proprietary BSP (real fix, both walls):** obtain the MTK A14 MT6765 BSP (via the 100071481 OEM)
   → rebuild connectivity `.ko` for arm64 (`make -C $KDIR M=<conn-src> ARCH=arm64 modules`, match
   vermagic) and build the MTK-patched pvrsrvkm with MM fn 42/44.
2. **Userspace graphics bypass (boot without GPU accel):** force software EGL (`ro.hardware.egl=
   swiftshader`) + replace the PVR-coupled MTK gralloc/hwcomposer with a generic dma-buf-heap stack.
   No proprietary source needed, but real integration work; may hit further MTK coupling.

## Key proven facts / observability
- Only working log channel on arm64: Magisk overlay `/metadata` capture (dmesg/logcat/ps/getprop),
  since USB→KPOC, UART→GZ-EL2 blocked, AEE broken, pstore DRAM-wiped on cold reset.
- expdb (eMMC) holds LK/GZ + WDT status across power-cycles: `wdt_status 0x2, exp_type 0x0` = HW
  watchdog, no SW exception.
- Flash arm64 to `boot_a` (mmcblk0p22) via root `dd` while v7a is up; battery-boot (no USB) to avoid KPOC.

## Blocking-hardware manifest (the "hunt" list)
Both walls are **MT6765 on-die silicon → one vendor: MediaTek**. No discrete 3rd-party chips.

**WALL 1 — Connectivity (WiFi/BT/GPS), all integrated CONNAC1x in MT6765:**
- combo: `connsys.chipid=0x6765`, `bt.platform=connac1x`, codename CERVINO/"soc1", fw `WIFI_RAM_CODE_soc1_0_1_1.bin`, patch `250307100601000`
- WiFi `wlan_drv_gen4m.ko` · BT `bt_drv_connac1x.ko` · GPS `gps_drv.ko` · FM **MT6631** (`fm_chipid=mt6631`) · `connfem.ko` (no discrete FEM DT node)
- NEED: MediaTek MT6765 connectivity kernel-module source (wmt/connac/gen4m) → build arm64 `.ko`

**WALL 2 — GPU:**
- **Imagination PowerVR GE8320 (Rogue)**, integrated MT6765; DT `13000000.mfg_doma`; DDK `1.13@5776728`; fw `rgx.fw.22.87.104.18`; needs MTK-custom MM bridge fns **42/44** (vanilla IMG DDK caps at 36)
- NEED: MTK-patched PowerVR DDK source matching 1.13@5776728

**Source location for BOTH:** MediaTek **MT6765 Android-14 vendor BSP** (`vendor/mediatek/.../kernel_modules/connectivity` + the patched `gpu_rgx` DDK). Match criteria when hunting a public/leaked BSP: connectivity fw tag `soc1_0` + PVR DDK `1.13@5776728`. Obtainable via the 100071481 OEM, or any matching public A14 MT6765 BSP.
