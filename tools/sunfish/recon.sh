#!/bin/bash
# Rebuild the files the xnu patch series *creates* (not in Apple's tree) into DIR
# (default ~/ios-pixel/recon), so new patches can be written against their final
# state without an xnu checkout. Handles both "new file mode" and "--- /dev/null".
#   tools/sunfish/recon.sh [DIR]
set -uo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
O=${1:-$HOME/ios-pixel/recon}
rm -rf "$O" && mkdir -p "$O" && cd "$O" && git init -q .
newfiles=$(cd "$R/xnu/patches" && awk '
	/^--- \/dev\/null/ { dn = 1; next }
	/^\+\+\+ b\// && dn { sub(/^\+\+\+ b\//, ""); sub(/\t.*/, ""); print; dn = 0; next }
	{ dn = 0 }
	/^diff --git a\// { f = $3; sub(/^a\//, "", f) }
	/^new file mode/ { print f }
' *.patch | sort -u)
echo "$(echo "$newfiles" | wc -l) files created by the series"
inc=""; for f in $newfiles; do inc="$inc --include=$f"; done
fail=0
for p in "$R"/xnu/patches/*.patch; do
	git apply $inc "$p" 2>/tmp/applyerr || { echo "FAIL $(basename "$p"): $(head -1 /tmp/applyerr)"; fail=1; }
done
echo "apply failures: $fail"
