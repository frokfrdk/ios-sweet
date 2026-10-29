#!/usr/bin/env python3
"""Install (or remove) a GPU test tool on a mounted iOS root.

    sudo python3 -I tools/sunfish/sweetgpu-root.py /mnt/iosroot BIN                  # install
    sudo python3 -I tools/sunfish/sweetgpu-root.py /mnt/iosroot --remove [TOOL]

BIN is out/sweetgpu-test from the sweetgpu-test (or xnu) workflow, or
out/bin/sweetgl-tri from the sweetgl workflow; its file name picks the tool.
TOOL defaults to sweetgpu-test. The tool goes to /usr/libexec/<tool>
(root:wheel 0755). launchd on this root reads its jobs from
/System/Library/xpc/launchd.plist (boot-arg launchd_unsecure_cache=1), so the
job goes there: run once at load, as root, stdout and stderr to /dev/console.
Each tool does nothing unless the boot-args ask for it: sweetgputest=1
(docs/GPU_A618_PLAN.md 6U) or sweetgltri=1 (Stage 7.2).
"""
import os, plistlib, shutil, sys

TOOLS = {
    'sweetgpu-test': 'com.sweet.gpu-test',
    'sweetgl-tri': 'com.sweet.gl-tri',
}


def job(tool):
    return {
        'Label': TOOLS[tool],
        'ProgramArguments': ['/usr/libexec/' + tool],
        'RunAtLoad': True,
        'KeepAlive': False,
        'UserName': 'root',
        'StandardOutPath': '/dev/console',
        'StandardErrorPath': '/dev/console',
    }


def launchd(root, tool, add):
    key = f'/System/Library/LaunchDaemons/{TOOLS[tool]}.plist'
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    if add:
        jobs[key] = job(tool)
    else:
        jobs.pop(key, None)
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {TOOLS[tool]} {"added" if add else "removed"}, {len(jobs)} jobs')


def main():
    if len(sys.argv) not in (3, 4) or not os.path.isdir(os.path.join(sys.argv[1], 'System/Library/xpc')):
        sys.exit(__doc__)
    root, arg = sys.argv[1], sys.argv[2]
    if arg == '--remove':
        tool = sys.argv[3] if len(sys.argv) == 4 else 'sweetgpu-test'
        if tool not in TOOLS:
            sys.exit(f'unknown tool {tool}; one of {", ".join(TOOLS)}')
        dest = os.path.join(root, 'usr/libexec', tool)
        if os.path.exists(dest):
            os.unlink(dest)
            print('removed', '/usr/libexec/' + tool)
        launchd(root, tool, False)
        return
    tool = os.path.basename(arg)
    if len(sys.argv) != 3 or tool not in TOOLS:
        sys.exit(f'BIN must be named one of {", ".join(TOOLS)}')
    dest = os.path.join(root, 'usr/libexec', tool)
    shutil.copyfile(arg, dest)
    os.chown(dest, 0, 0)
    os.chmod(dest, 0o755)
    print('installed', '/usr/libexec/' + tool, os.path.getsize(dest), 'bytes')
    launchd(root, tool, True)


if __name__ == '__main__':
    main()
