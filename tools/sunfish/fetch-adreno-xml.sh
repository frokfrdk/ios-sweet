#!/bin/bash
# Fetch the freedreno register database (MIT) that gen-adreno-regs.py reads, at a pinned
# Linux commit, and check it: fetch-adreno-xml.sh [DIR] (default ~/ios-pixel/ref/adreno-xml).
# Uses the authenticated GitHub contents API (gh), since raw.githubusercontent.com
# rate-limits. The XML stays out of git; the generated headers carry its licence.
set -euo pipefail
D=${1:-$HOME/ios-pixel/ref/adreno-xml}
SHA=5b0764a53235d3c77e1ed674e836e1b3fe1a4b1f
P=drivers/gpu/drm/msm/registers
mkdir -p "$D" && cd "$D"
fetch() { gh api -H "Accept: application/vnd.github.raw" "repos/torvalds/linux/contents/$1?ref=$SHA" > "$(basename "$1")"; }
for f in a6xx.xml a6xx_enums.xml a6xx_gmu.xml adreno_pm4.xml adreno_common.xml; do
	fetch $P/adreno/$f
done
fetch $P/freedreno_copyright.xml
echo "torvalds/linux $SHA $P/adreno" > SOURCE.txt
sha256sum -c - <<'EOF'
6e20d0347b06f26220f6b87d8600e94790331fec45363a167c5e63ea6ad8f9c2  a6xx.xml
08d56c3ee45ebaa151a8adc7adf7c372ecf89ca72fabc09da1d64645c31f06d4  a6xx_enums.xml
f13f07aa3c975c1d671c1a7f5ba62ddaaf525aedb7a0e481186cbfffe461f88b  a6xx_gmu.xml
f898e4eee3de5f92d25597f003e5550a24fe50552d25555d6916dcd1b922d6f6  adreno_common.xml
155df8614c56bd49464e7d33daf764f8c97ecf9942d6242c7fbcb3eaccd1acb5  adreno_pm4.xml
16e6fda4963d01a6d2fa6c938a26082d6f0afd0988db7f953c9e4dea38739f2f  freedreno_copyright.xml
EOF
