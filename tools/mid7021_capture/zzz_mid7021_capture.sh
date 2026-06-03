#!/system/bin/sh
# MID7021 arm64 _b boot capture (Magisk post-fs-data.d). Guard on KERNEL VERSION,
# not arch: our arm64 build is "4.19.191-gfef14d8ce382-dirty"; stock _a is plain
# "4.19.191" (and BOTH report uname -m = armv7l, which is why the old guard
# failed). Skip stock; on our build, continuously snapshot kernel log + props +
# logcat to eMMC so a hung/looping arm64 boot's log survives.
case "$(uname -r)" in
  4.19.191) exit 0 ;;   # stock _a -> skip (don't overwrite the capture)
esac
(
  N=0
  while [ "$N" -lt 900 ]; do
    dmesg > /data/local/tmp/arm64_kmsg.txt 2>/dev/null
    getprop > /data/local/tmp/arm64_props.txt 2>/dev/null
    logcat -d > /data/local/tmp/arm64_logcat.txt 2>/dev/null
    sync
    N=$((N + 1))
    sleep 1
  done
) &
