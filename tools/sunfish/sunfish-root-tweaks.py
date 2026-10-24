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

ExtensionKit feature flag enforce_xpc_cache_code_signing -> off. installd's
appex preflight looks up every extension point in EXExtensionPointCatalog,
which loads /System/Library/xpc/extensions.plist only if its detached .sig
validates (trust cache via AMFI, absent here). Otherwise only the 6
ExtensionKit/ExtensionPoints/*.appexpt are known and every app with a widget,
intents or notification appex fails: "Extension cache does not have an entry
for com.apple.widgetkit-extension" (MIInstaller Code=59, Preflight stage failed).
-[_EXDefaults enforceXPCCacheCodeSigning] = os_feature_enabled(ExtensionKit,
enforce_xpc_cache_code_signing) && !(amfi bootarg state & 0x86).

SpringBoard feature flags Autobahn* -> off (the iOS 16 poster lock screen).
Posters need Metal: the device's default poster, AegirPoster (Astronomy),
aborts in -[NUNIAegirRenderer getOrCreateUniformBufferforFrameBufferIndex:]
(nil MTLBuffer), and PosterBoard's home screen snapshots render through
+[CARenderer rendererWithMTLTexture:], so lock and home screen stay black.
With Autobahn off SpringBoard uses the PaperBoardUI legacy wallpaper path,
which draws plain images with CoreAnimation.

--wallpaper IMAGE: legacy wallpaper for lock and home screen. PaperBoardUI's
directory data store reads ~mobile/Library/SpringBoard/{Lock,Home}Background
.cpbitmap and falls back to ImageIO when the data is not a CPBitmap, so a PNG
or JPEG works. Needed because the stock default (WallpaperKit 7400 "WWDC
2022") only ships 375w/393w/430w @3x variants, none for the 414w-896h@2x
screen class of the 828xr ADT, and a failed default restore leaves the
SBLockScreenWallpapers/SBHomeScreenWallpapers entries empty (black).

--migrate-once: let DataMigration run on the next boot. The staged system apps
(/var/staged_system_apps: Calculator, Books, ...; "Waiting..." placeholders)
are installed by SystemAppMigrator.migrator, which only runs during migration;
sweet-root-tweaks.py pins LastSystemVersion to the current build so migration
never runs. Here the recorded build is set one lower (an upgrade), and every
migrator except LaunchServices and SystemAppMigrator is disabled: upstream runs
with none of them, and they wait on missing hardware and daemons
(CoreLocationMigrator 170+ min on locationd, AirTrafficMigrator 40+ min on atc),
holding the whole boot. App staging also needs the kernel's HFS+ clonefile copy
fallback (xnu patch 0091).
"""
import os, plistlib, sys

root = sys.argv[1]
migrate_once = '--migrate-once' in sys.argv[2:]
wallpaper = sys.argv[sys.argv.index('--wallpaper') + 1] if '--wallpaper' in sys.argv else None

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

rel = 'System/Library/FeatureFlags/Domain/ExtensionKit.plist'
path = os.path.join(root, rel)
with open(path, 'rb') as f:
    flags = plistlib.load(f)
flags.setdefault('enforce_xpc_cache_code_signing', {})['Enabled'] = False
st = os.stat(path)
with open(path, 'wb') as f:
    plistlib.dump(flags, f)
os.chown(path, st.st_uid, st.st_gid)
os.chmod(path, st.st_mode & 0o7777)
print(f'{rel}: enforce_xpc_cache_code_signing disabled')

rel = 'System/Library/FeatureFlags/Domain/SpringBoard.plist'
path = os.path.join(root, rel)
with open(path, 'rb') as f:
    flags = plistlib.load(f)
off = [k for k in flags if k.startswith('Autobahn')]
for k in off:
    flags[k]['Enabled'] = False
st = os.stat(path)
with open(path, 'wb') as f:
    plistlib.dump(flags, f)
os.chown(path, st.st_uid, st.st_gid)
os.chmod(path, st.st_mode & 0o7777)
print(f'{rel}: {off} disabled')

if wallpaper:
    sbdir = os.path.join(root, 'private/var/mobile/Library/SpringBoard')
    with open(wallpaper, 'rb') as f:
        data = f.read()
    for name in ('LockBackground.cpbitmap', 'HomeBackground.cpbitmap'):
        path = os.path.join(sbdir, name)
        with open(path, 'wb') as f:
            f.write(data)
        os.chown(path, 501, 501)
        os.chmod(path, 0o644)
    print(f'wallpaper: {wallpaper} -> {sbdir}/{{Lock,Home}}Background.cpbitmap ({len(data)} bytes)')

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

    KEEP = {'00LaunchServicesMigrator.migrator', 'SystemAppMigrator.migrator'}
    mdir = os.path.join(root, 'System/Library/DataClassMigrators')
    disabled = []
    for name in sorted(os.listdir(mdir)):
        if name.endswith('.migrator') and name not in KEEP:
            os.rename(os.path.join(mdir, name), os.path.join(mdir, name + '.disabled'))
            disabled.append(name)
    print(f'DataClassMigrators: {len(disabled)} disabled, kept {sorted(KEEP)}')
