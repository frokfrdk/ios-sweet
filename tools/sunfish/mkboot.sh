#!/bin/bash
# Pack a sunfish XNU boot image: mkboot.sh KERNEL_DIR NAME ["extra boot-args"]
set -euo pipefail
R="/mnt/c/Users/frokfrdk/Documents/ios 16 on Google Pixel 4a/ios-sweet-pixel"
export PATH=~/.local/bin:$PATH
cd "$R"
K="$1/kernel.development.sweet"; NAME=$2; EXTRA=${3:-}
(cd "$1" && sha256sum -c --quiet SHA256SUMS) && echo "kernel checksum ok"
# Read-only first boot: no ufsrw/rootrw, no persistent log ring on userdata.
B="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweettouch=0 sweetlog=0 $EXTRA"
python3 -I tools/mkpayload.py "$K" shim/out/sunfish-hi.adt shim/out/payload-$NAME.bin --cmdline "$B"
rm -f shim/out/shim-boot.img
# build.sh cd's into shim/: hand it an absolute payload path.
BOARD=sunfish bash shim/build.sh "$R/shim/out/payload-$NAME.bin" > /tmp/shim-build.log 2>&1 || { tail -5 /tmp/shim-build.log; exit 1; }
cp shim/out/shim-boot.img shim/out/shim-boot-$NAME.img
python3 -I tools/unpack_bootv2.py shim/out/shim-boot-$NAME.img shim/out/check-$NAME >/dev/null
cmp <(head -c $(stat -c %s shim/out/payload-$NAME.bin) shim/out/check-$NAME/ramdisk) shim/out/payload-$NAME.bin && echo "payload round-trip OK"
ls -la shim/out/payload-$NAME.bin shim/out/shim-boot-$NAME.img
sha256sum shim/out/shim-boot-$NAME.img
echo "boot-args: $B"
