#!/usr/bin/env python3
"""Pixel 4a (sunfish) additions to a mounted iOS root (after sweet-root-tweaks.py).

    sudo python3 -I tools/sunfish/sunfish-root-tweaks.py /mnt/iosroot

CoreUI software rendering. Boot D6c: SpringBoard crash loop, SIGSEGV pc 0
  -[UIImage _imageWithStylePresets:...] -> -[CUICatalog imageByStylingImage:...]
  -> +[CUIShapeEffectStack sharedCIContext] -> +[CIContext contextWithOptions:]
  -> CI::GLContext::init() + 204 (a NULL OpenGL ES entry point: no GPU driver).
CoreUI builds that context with kCIContextUseSoftwareRenderer =
!_CUIDebugAllowHardwareRendering(), which is
  [NSUserDefaults boolForKey:@"CUIAllowDebugPrefs"] &&
  [[NSUserDefaults objectForKey:@"CUIAllowHardwareRendering"] boolValue] == NO -> software.
Both keys go into the global domain of mobile and root, so every process that
draws styled CoreUI images uses CoreImage's CPU renderer.
"""
import os, plistlib, sys

root = sys.argv[1]
PREFS = {'CUIAllowDebugPrefs': True, 'CUIAllowHardwareRendering': False}
TARGETS = [  # (path, uid, gid)
    ('private/var/mobile/Library/Preferences/.GlobalPreferences.plist', 501, 501),
    ('private/var/root/Library/Preferences/.GlobalPreferences.plist', 0, 0),
]

for rel, uid, gid in TARGETS:
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = {}
    if os.path.exists(path):
        with open(path, 'rb') as f:
            data = plistlib.load(f)
    data.update(PREFS)
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, uid, gid)
    os.chmod(path, 0o600)
    print(f'{rel}: {sorted(PREFS)} set ({len(data)} keys)')
