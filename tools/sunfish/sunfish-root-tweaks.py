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

--wallpaper FILE.cpbitmap [--home-wallpaper FILE.cpbitmap]: legacy wallpaper for lock and home screen
(the home screen gets the second file if given). PaperBoardUI's
directory data store reads ~mobile/Library/SpringBoard/{Lock,Home}Background
.cpbitmap with AppSupport's CPBitmapCreateImagesFromData, which needs a real
CPBitmap (trailer magic; there is no ImageIO fallback, a PNG stays black):
convert with tools/sunfish/png2cpbitmap.py. Needed because the stock default (WallpaperKit 7400 "WWDC
2022") only ships 375w/393w/430w @3x variants, none for the 414w-896h@2x
screen class of the 828xr ADT, and a failed default restore leaves the
SBLockScreenWallpapers/SBHomeScreenWallpapers entries empty (black).
--wallpaper also resets what SpringBoard derived from the old image: it empties
~mobile/Library/Caches/MappedImageCache/Wallpaper (blurred/tinted variants, keyed by the home entry's
SBWallpaperImageHashDataKey, or "#" for the lock screen) and WallpaperColorBoxesCache, rewrites
OriginalHomeBackground.cpbitmap, sets a new image hash and SBWallpaperLastChangeUserDefaultKey.
--wallpaper-thumbs LOCK.jpg HOME.jpg replaces {Lock,Home}BackgroundThumbnail.jpg (206x446).

PaperBoardUI static wallpaper, two 8-byte patches in dyld_shared_cache_arm64.15 (20H392). With the
CPBitmap alone the lock and home screen were full white:
- -[PBUIStaticWallpaperView _setupWallpaperImageFromConfiguration:options:] replaces the raw image
  with a "treated" one (luminance, dimming, the 1/16 sample) rendered by
  com.apple.PaperBoardUI.RendererService through a Metal CARenderer. workaround_alwaysForceNoTreatment
  (file offset 0x115c3bc) -> YES (mov w0, #1; ret) skips the treatment.
- The result is kept in PBUIWallpaperCache, a disk-backed BSUIMappedImageCache
  (~mobile/Library/Caches/MappedImageCache/Wallpaper, ASTC-compressed CPBitmap entries), and a cache hit
  never runs the generator, so the first patch alone still showed white. +[PBUIStaticWallpaperView
  _canCacheImages] (0x115ba34) -> NO (mov w0, #0; ret) leaves no cache key: the image is generated
  directly every time (PBUIScrollableStaticWallpaperView already returns NO).
The kernel accepts the changed cache pages (no AMFI, so no CS_KILL). Already-patched bytes are left alone.

SplashBoard snapshots, one 4-byte patch in dyld_shared_cache_arm64.31 (0x5bfbedc). The app switcher card
showed only its left part: snapshots are saved as JPEG when the device "supports ASTC", and a JPEG
decodes to a 420f IOSurface whose BGRA conversion needs the missing AppleM2ScalerCSCDriver (a strong
inference from the format and the drawn part, not a proven cause). The store of the cached
supportsASTC answer becomes strb wzr: NO, so snapshots are PNG. Snapshots already on disk stay JPEG
until the app is snapshotted again.

--sweetwall PKGDIR DYLIB: the layered iOS 16 wallpaper (Stage 7.7). PKGDIR holds
{Background,Foreground,Floating}.ca from tools/sunfish/sweetwall-pkg.py (the system's 7400 packages
with PNG assets), installed to /Library/Wallpaper/SweetLayered; DYLIB is libsweetwall.dylib (sweetwall
CI), installed as /usr/lib/sweetwall.dylib. Tried on G7b (2026-10-31): the owner saw it flash between the
static wallpaper and the layered one, and dropped it for static 7400 images (--wallpaper / --home-wallpaper). SpringBoard loads it through the boot-arg
sweetenv=...;SpringBoard:DYLD_INSERT_LIBRARIES=/usr/lib/sweetwall.dylib (the short name keeps the
G7a cmdline + this at 605 of the 607 bytes). Keep the --wallpaper CPBitmap: it stays underneath.

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
import datetime, hashlib, os, plistlib, shutil, struct, sys

root = sys.argv[1]
migrate_once = '--migrate-once' in sys.argv[2:]
wallpaper = sys.argv[sys.argv.index('--wallpaper') + 1] if '--wallpaper' in sys.argv else None
home_wallpaper = sys.argv[sys.argv.index('--home-wallpaper') + 1] if '--home-wallpaper' in sys.argv else wallpaper
wallpaper_thumbs = sys.argv[sys.argv.index('--wallpaper-thumbs') + 1:][:2] if '--wallpaper-thumbs' in sys.argv else None
sweetwall = sys.argv[sys.argv.index('--sweetwall') + 1:][:2] if '--sweetwall' in sys.argv else None

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

DSC_PATCHES = [  # (cache file, file offset, original, patched, what)
    ('dyld_shared_cache_arm64.15', 0x115c3bc, 'f44fbea9fd7b01a9', '20008052c0035fd6',
     'PBUIStaticWallpaperView workaround_alwaysForceNoTreatment -> YES'),
    ('dyld_shared_cache_arm64.15', 0x115ba34, '20008052c0035fd6', '00008052c0035fd6',
     'PBUIStaticWallpaperView +_canCacheImages -> NO'),
    ('dyld_shared_cache_arm64.31', 0x5bfbedc, '0041093920b70df0', '1f41093920b70df0',
     'SplashBoard supportsASTC -> NO (PNG snapshots)'),
]
for name, at, old, new, what in DSC_PATCHES:
    rel = 'System/Library/Caches/com.apple.dyld/' + name
    with open(os.path.join(root, rel), 'r+b') as f:
        f.seek(at)
        cur = f.read(len(old) // 2).hex()
        assert cur in (old, new), f'{rel} +{at:#x}: {cur} is neither {old} nor {new} (not 20H392?)'
        if cur == old:
            f.seek(at)
            f.write(bytes.fromhex(new))
    print(f'{rel} +{at:#x}: {what} ({"patched" if cur == old else "already patched"})')

if wallpaper:
    sbdir = os.path.join(root, 'private/var/mobile/Library/SpringBoard')
    for name, src in (('LockBackground.cpbitmap', wallpaper), ('HomeBackground.cpbitmap', home_wallpaper)):
        with open(src, 'rb') as f:
            data = f.read()
        assert struct.unpack('<I', data[-4:])[0] in (0xDCB543A2, 0xCBA43291), \
            f'{src}: not a CPBitmap (convert it with png2cpbitmap.py)'
        path = os.path.join(sbdir, name)
        with open(path, 'wb') as f:
            f.write(data)
        os.chown(path, 501, 501)
        os.chmod(path, 0o644)
        print(f'wallpaper: {src} -> {sbdir}/{name} ({len(data)} bytes)')
        if name == 'HomeBackground.cpbitmap':
            home_data = data
    # Swapping the files behind SpringBoard's back left the old wallpaper in its derived copies (G7c:
    # old image in the lock screen and in Search's blur). Reset them all.
    path = os.path.join(sbdir, 'OriginalHomeBackground.cpbitmap')
    with open(path, 'wb') as f:
        f.write(home_data)
    os.chown(path, 501, 501)
    os.chmod(path, 0o600)
    if wallpaper_thumbs:
        for name, src in zip(('LockBackgroundThumbnail.jpg', 'HomeBackgroundThumbnail.jpg'), wallpaper_thumbs):
            path = os.path.join(sbdir, name)
            shutil.copyfile(src, path)
            os.chown(path, 501, 501)
            os.chmod(path, 0o644)
            print(f'wallpaper: {src} -> {sbdir}/{name}')
    removed = []
    for rel in ('private/var/mobile/Library/Caches/MappedImageCache/Wallpaper',
                'private/var/mobile/Library/Caches/com.apple.springboard/WallpaperColorBoxesCache'):
        d = os.path.join(root, rel)
        for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            p = os.path.join(d, n)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
            removed.append(n)
    print(f'wallpaper: {len(removed)} cached variants removed {removed}')
    # The blur cache is keyed by the home entry's SBWallpaperImageHashDataKey (base64 in the file name),
    # so a new value also misses any variant made from the old image.
    path = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.springboard.plist')
    with open(path, 'rb') as f:
        sb = plistlib.load(f)
    entry = sb.get('SBHomeScreenWallpapers', {}).get('none')
    if entry is not None:
        entry['SBWallpaperImageHashDataKey'] = hashlib.sha256(home_data).digest()[:8]
    sb['SBWallpaperLastChangeUserDefaultKey'] = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    st = os.stat(path)
    with open(path, 'wb') as f:
        plistlib.dump(sb, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'com.apple.springboard.plist: home image hash '
          f'{entry["SBWallpaperImageHashDataKey"].hex() if entry else "(no home entry)"}, last change now')

if sweetwall:
    pkgdir, dylib = sweetwall
    dst = os.path.join(root, 'Library/Wallpaper/SweetLayered')
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for name in ('Background', 'Foreground', 'Floating'):
        src = os.path.join(pkgdir, name + '.ca')
        assert os.path.exists(os.path.join(src, 'main.caml')), f'{src}: no main.caml (make it with sweetwall-pkg.py)'
        shutil.copytree(src, os.path.join(dst, name + '.ca'))
    for d, dirs, files in os.walk(dst):
        for n in dirs + files:
            p = os.path.join(d, n)
            os.chown(p, 0, 0)
            os.chmod(p, 0o755 if n in dirs else 0o644)
    os.chown(dst, 0, 0)
    os.chmod(dst, 0o755)
    path = os.path.join(root, 'usr/lib/sweetwall.dylib')
    shutil.copyfile(dylib, path)
    os.chown(path, 0, 0)
    os.chmod(path, 0o755)
    print(f'sweetwall: {pkgdir} -> /Library/Wallpaper/SweetLayered, {dylib} -> /usr/lib/sweetwall.dylib')

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
