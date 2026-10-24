#!/usr/bin/env python3
"""Pixel 4a (sunfish) additions to a mounted iOS root (after sweet-root-tweaks.py).

    sudo python3 -I tools/sunfish/sunfish-root-tweaks.py /mnt/iosroot [--migrate-once]

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

--migrate-once: let DataMigration run on the next boot. The staged system apps
(/var/staged_system_apps: Calculator, Books, ...; "Waiting..." placeholders)
are installed by SystemAppMigrator.migrator, which only runs during migration;
sweet-root-tweaks.py pins LastSystemVersion to the current build so migration
never runs. Here the recorded build is set one lower (an upgrade), and the
CoreLocation migrator, which waited on locationd for 170+ minutes (no GPS or
baseband) and held the whole boot, is disabled. App staging also needs the
kernel's HFS+ clonefile copy fallback (xnu patch 0091).
"""
import os, plistlib, sys

root = sys.argv[1]
migrate_once = '--migrate-once' in sys.argv[2:]

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

if migrate_once:
    rel = 'private/var/mobile/Library/Preferences/com.apple.migration.plist'
    path = os.path.join(root, rel)
    with open(path, 'rb') as f:
        mig = plistlib.load(f)
    build = mig.get('LastSystemVersion', '20H392')
    older = build[:-1] + chr(ord(build[-1]) - 1)      # 20H392 -> 20H391: an upgrade
    mig['LastSystemVersion'] = older
    mig.setdefault('DMUserDataDisposition', {})['buildVersion'] = older
    st = os.stat(path)
    with open(path, 'wb') as f:
        plistlib.dump(mig, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'{rel}: LastSystemVersion {build} -> {older}')

    mdir = os.path.join(root, 'System/Library/DataClassMigrators')
    src = os.path.join(mdir, 'CoreLocationMigrator.migrator')
    if os.path.isdir(src):
        os.rename(src, src + '.disabled')
        print('DataClassMigrators/CoreLocationMigrator.migrator disabled')
