# MID7021 arm64 boot capture tooling

The arm64 boot reaches userspace but hangs; expdb/pstore/UART cannot see it.
The WORKING capture = a Magisk overlay.d init service dumping dmesg to /metadata
(early, pre-/data, survives the hang + cold-cycle).

## Recipe (v29)
1. Build the arm64 kernel; package a boot.img (cmdline MUST be `bootopt=64S3,32N2,64N2`
   + optional `androidboot.selinux=permissive`; kernel_offset 0x40080000).
2. Magisk-patch it via the Magisk app on the device (Install -> Select and Patch a File).
3. Unpack the magisk-patched ramdisk, drop `init.mid7021cap.rc` into `overlay.d/`, repack,
   mkbootimg with the same params -> arm64_boot_vXX.img.
4. Flash boot_a, boot. Service `mid7021cap` runs `on init`, dumps dmesg+getprop to
   /metadata/mid7021_kmsg.txt every 1s. (TODO v30: add `logcat -d` to also capture userspace.)
5. BROM-restore the magisk _a boot, then `adb shell su -c cat /metadata/mid7021_kmsg.txt`.

## Files
- init.mid7021cap.rc      : overlay.d init service (WORKS).
- zzz_mid7021_capture.sh  : magisk post-fs-data.d script (does NOT run here -
                            magisk "environment incomplete, abort"; kept for reference).
- uvstub_*                : Magisk module to stub /system/bin/update_verifier
                            (did NOT load - magisk modules disabled by env-incomplete;
                            update_verifier completes on its own anyway).

Donor for true v8a (64-bit vendor): GitLab Android-Dumps/samsung/a04e (mt6765, A14,
UP1A.231005.007 - same build as MID7021, but 64-bit).
