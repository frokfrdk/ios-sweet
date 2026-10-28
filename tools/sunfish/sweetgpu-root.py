#!/usr/bin/env python3
"""Install (or remove) the GPU stage 6U test on a mounted iOS root.

    sudo python3 -I tools/sunfish/sweetgpu-root.py /mnt/iosroot BIN      # install
    sudo python3 -I tools/sunfish/sweetgpu-root.py /mnt/iosroot --remove

BIN is out/sweetgpu-test from the sweetgpu-test (or xnu) workflow. It goes to
/usr/libexec/sweetgpu-test (root:wheel 0755). launchd on this root reads its
jobs from /System/Library/xpc/launchd.plist (boot-arg launchd_unsecure_cache=1),
so the job com.sweet.gpu-test goes there: run once at load, as root, stdout and
stderr to /dev/console, which the persistent log (sweetlog=1) records. The tool
does nothing unless the boot-args hold sweetgputest=1 (docs/GPU_A618_PLAN.md 6U).
"""
import os, plistlib, shutil, sys

DEST = 'usr/libexec/sweetgpu-test'
LABEL = 'com.sweet.gpu-test'
KEY = '/System/Library/LaunchDaemons/com.sweet.gpu-test.plist'
JOB = {
    'Label': LABEL,
    'ProgramArguments': ['/' + DEST],
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'StandardOutPath': '/dev/console',
    'StandardErrorPath': '/dev/console',
}


def launchd(root, add):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    if add:
        jobs[KEY] = JOB
    else:
        jobs.pop(KEY, None)
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {LABEL} {"added" if add else "removed"}, {len(jobs)} jobs')


def main():
    if len(sys.argv) != 3 or not os.path.isdir(os.path.join(sys.argv[1], 'System/Library/xpc')):
        sys.exit(__doc__)
    root, arg = sys.argv[1], sys.argv[2]
    dest = os.path.join(root, DEST)
    if arg == '--remove':
        if os.path.exists(dest):
            os.unlink(dest)
            print('removed', '/' + DEST)
        launchd(root, False)
        return
    shutil.copyfile(arg, dest)
    os.chown(dest, 0, 0)
    os.chmod(dest, 0o755)
    print('installed', '/' + DEST, os.path.getsize(dest), 'bytes')
    launchd(root, True)


if __name__ == '__main__':
    main()
