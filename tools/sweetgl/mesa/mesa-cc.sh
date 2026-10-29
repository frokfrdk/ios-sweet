#!/bin/bash
# Print a compile command for code built against the Mesa build from build.sh:
# meson's command for one of Mesa's own sources (defines, generated include
# dirs, target flags) minus that source and its outputs, plus the winsys and
# driver dirs for freedreno_drm_public.h and driinfo_freedreno.h. The printed
# line takes "$1" (source) and "$2" (object), and any further flags after them.
# Usage: mesa-cc.sh <mesa source> <mesa build dir> <mesa source path suffix> > cc.sh
set -euo pipefail

SRC=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
python3 - "$B" "$SRC" "$3" <<'EOF'
import json, shlex, sys
b, mesa, ref = sys.argv[1:]
for e in json.load(open(b + "/compile_commands.json")):
    if e["file"].endswith(ref):
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        break
else:
    sys.exit(ref + " is not in compile_commands.json")
out, skip = [], False
for a in args:
    if skip:
        skip = False
    elif a in ("-o", "-MF", "-MQ", "-MT"):
        skip = True
    elif a not in ("-c", "-MD") and not a.endswith(ref):
        out.append(a)
extra = ["-I" + mesa + "/src/gallium/winsys", "-I" + mesa + "/src/gallium/drivers", "-Wno-error"]
print("cd " + shlex.quote(e["directory"]) + " && " + shlex.join(out + extra) + ' -c "$1" -o "$2" "${@:3}"')
EOF
