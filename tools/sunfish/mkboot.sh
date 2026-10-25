#!/bin/bash
# Pack a sunfish XNU boot image: [CPUS=n] mkboot.sh KERNEL_DIR NAME ["extra boot-args"]
# XNU takes the first occurrence of a boot-arg, so cpus= is set only here (default 1).
# TOUCH= overrides the touch boot-args. Default: the sunfish FTM5 driver with its
# power switch (kernel 0084+); TOUCH=sweettouch=0 turns touch off.
# SWEETLOG=1 turns on the persistent console log (needs ufsrw=1 in the extra args;
# it overwrites the last 4 MiB + 4 KiB of userdata). GPUFW=1 adds the Adreno 618
# firmware from $GPUFW_DIR (default ~/ios-pixel/gpu-fw/fw) as payload blobs for
# sweet_gpu.c (kernel 0094+); such images contain vendor firmware: keep them out of git.
# It also adds GPU-GMU-MEM, 1 MiB of zeros that sweet_gmu.c (kernel 0100+) maps for the
# GMU (page tables, HFI queues, cache backing); no vendor data.
set -euo pipefail
R="/mnt/c/Users/frokfrdk/Documents/ios 16 on Google Pixel 4a/ios-sweet-pixel"
export PATH=~/.local/bin:$PATH
cd "$R"
K="$1/kernel.development.sweet"; NAME=$2; EXTRA=${3:-}
(cd "$1" && sha256sum -c --quiet SHA256SUMS) && echo "kernel checksum ok"
# Read-only first boot: no ufsrw/rootrw, no persistent log ring on userdata (SWEETLOG=0).
B="debug=0xc serial=3 cpus=${CPUS:-1} keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 ${TOUCH:-sweetfts=1 sweetftspower=1 sweettouchdelay=20} sweetlog=${SWEETLOG:-0} $EXTRA"
BLOBS=()
if [ "${GPUFW:-0}" = 1 ]; then
  FW=${GPUFW_DIR:-$HOME/ios-pixel/gpu-fw/fw}
  for b in GPU-SQE=a630_sqe.fw GPU-GMU=a618_gmu.bin GPU-ZAP-MDT=a615_zap.mdt GPU-ZAP-B02=a615_zap.b02; do
    BLOBS+=(--blob "${b%%=*}=$FW/${b#*=}")
  done
  head -c 1048576 /dev/zero > shim/out/gmu-mem.bin
  BLOBS+=(--blob GPU-GMU-MEM=shim/out/gmu-mem.bin)
fi
# ADT= picks another device tree (e.g. shim/out/sunfish-hi-scale2.adt from mkadt.py --scale2).
python3 -I tools/mkpayload.py "$K" "${ADT:-shim/out/sunfish-hi.adt}" shim/out/payload-$NAME.bin --cmdline "$B" "${BLOBS[@]}"
rm -f shim/out/shim-boot.img
# build.sh cd's into shim/: hand it an absolute payload path.
BOARD=sunfish bash shim/build.sh "$R/shim/out/payload-$NAME.bin" > /tmp/shim-build.log 2>&1 || { tail -5 /tmp/shim-build.log; exit 1; }
cp shim/out/shim-boot.img shim/out/shim-boot-$NAME.img
python3 -I tools/unpack_bootv2.py shim/out/shim-boot-$NAME.img shim/out/check-$NAME >/dev/null
cmp <(head -c $(stat -c %s shim/out/payload-$NAME.bin) shim/out/check-$NAME/ramdisk) shim/out/payload-$NAME.bin && echo "payload round-trip OK"
ls -la shim/out/payload-$NAME.bin shim/out/shim-boot-$NAME.img
sha256sum shim/out/shim-boot-$NAME.img
echo "boot-args: $B"
